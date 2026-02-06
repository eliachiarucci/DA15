// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * Persistent Settings Storage
 *
 * Uses the last flash page (2KB at 0x0801F800) for sequential record writing.
 * Each record is 8 bytes: [magic, volume, muted, bass, treble, brightness, timeout, checksum].
 * Records are appended sequentially; when the page is full it is erased.
 * On load, the last valid record is used.
 *
 * STM32F072CB: 2KB pages, half-word (16-bit) programming.
 */

#include "settings.h"
#include "stm32f0xx_hal.h"

#define SETTINGS_PAGE_ADDR  0x0801F800U
#define SETTINGS_PAGE_SIZE  2048U
#define RECORD_SIZE         8U
#define MAX_RECORDS         (SETTINGS_PAGE_SIZE / RECORD_SIZE)
#define RECORD_MAGIC        0xA6U
#define ERASED_BYTE         0xFFU

static uint8_t compute_checksum(const uint8_t *rec, uint8_t len) {
    uint8_t cksum = 0;
    for (uint8_t i = 0; i < len; i++)
        cksum ^= rec[i];
    return cksum;
}

bool settings_load(settings_t *out) {
    const uint8_t *base = (const uint8_t *)SETTINGS_PAGE_ADDR;

    // Scan backwards to find last valid record
    for (int i = (int)MAX_RECORDS - 1; i >= 0; i--) {
        const uint8_t *rec = base + (i * RECORD_SIZE);
        if (rec[0] != RECORD_MAGIC) continue;

        // Checksum covers bytes 0-6, stored in byte 7
        uint8_t cksum = compute_checksum(rec, 7);
        if (cksum != rec[7]) continue;

        out->local_volume    = rec[1];
        out->local_muted     = rec[2];
        out->bass            = (int8_t)rec[3];
        out->treble          = (int8_t)rec[4];
        out->brightness      = rec[5];
        out->display_timeout = rec[6];
        return true;
    }

    return false;
}

static int find_next_free_slot(void) {
    const uint8_t *base = (const uint8_t *)SETTINGS_PAGE_ADDR;
    for (int i = 0; i < (int)MAX_RECORDS; i++) {
        const uint8_t *rec = base + (i * RECORD_SIZE);
        uint8_t all_erased = 1;
        for (uint8_t j = 0; j < RECORD_SIZE; j++) {
            if (rec[j] != ERASED_BYTE) { all_erased = 0; break; }
        }
        if (all_erased) return i;
    }
    return -1;
}

static bool erase_settings_page(void) {
    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef erase = {
        .TypeErase   = FLASH_TYPEERASE_PAGES,
        .PageAddress = SETTINGS_PAGE_ADDR,
        .NbPages     = 1,
    };
    uint32_t page_error = 0;
    HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&erase, &page_error);

    HAL_FLASH_Lock();
    return status == HAL_OK;
}

bool settings_save(const settings_t *s) {
    int slot = find_next_free_slot();

    if (slot < 0) {
        if (!erase_settings_page()) {
            return false;
        }
        slot = 0;
    }

    uint32_t addr = SETTINGS_PAGE_ADDR + (uint32_t)slot * RECORD_SIZE;

    // Build record: [magic, volume, muted, bass, treble, brightness, timeout, checksum]
    uint8_t rec[RECORD_SIZE];
    rec[0] = RECORD_MAGIC;
    rec[1] = s->local_volume;
    rec[2] = s->local_muted;
    rec[3] = (uint8_t)s->bass;
    rec[4] = (uint8_t)s->treble;
    rec[5] = s->brightness;
    rec[6] = s->display_timeout;
    rec[7] = compute_checksum(rec, 7);

    // STM32F0 programs in half-words (16 bits): 8 bytes = 4 half-words
    HAL_FLASH_Unlock();

    for (uint8_t i = 0; i < RECORD_SIZE; i += 2) {
        uint16_t hw = ((uint16_t)rec[i + 1] << 8) | rec[i];
        HAL_StatusTypeDef status = HAL_FLASH_Program(
            FLASH_TYPEPROGRAM_HALFWORD, addr + i, hw);
        if (status != HAL_OK) {
            HAL_FLASH_Lock();
            return false;
        }
    }

    HAL_FLASH_Lock();
    return true;
}
