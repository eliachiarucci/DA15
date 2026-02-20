// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * Persistent Settings Storage
 *
 * Uses the last flash sector (8KB at 0x0801E000) for sequential record writing.
 * Each record is 16 bytes (quad-word aligned):
 *   [magic, volume, muted, bass, treble, brightness, timeout, checksum, 0xFF x8]
 * Records are appended sequentially; when the sector is full it is erased.
 * On load, the last valid record is used.
 *
 * STM32H503: 8KB sectors, quad-word (128-bit / 16-byte) programming.
 *
 * ECC recovery: if power is lost during a quad-word flash write, the partially
 * programmed word will have invalid ECC. Reading it triggers an NMI. The NMI
 * handler sets settings_ecc_error, and settings_load() erases the sector and
 * falls back to defaults.
 */

#include "settings.h"
#include "SEGGER_RTT.h"
#include "stm32h5xx_hal.h"

#define SETTINGS_BANK        FLASH_BANK_2 // Bank 2 (0x08010000–0x0801FFFF)
#define SETTINGS_SECTOR      7U           // Last 8KB sector of bank 2
#define SETTINGS_PAGE_ADDR   0x0801E000U  // Bank 2, Sector 7 base address
#define SETTINGS_PAGE_SIZE   8192U        // 8KB sector
#define RECORD_SIZE          16U          // Quad-word aligned (16 bytes)
#define MAX_RECORDS          (SETTINGS_PAGE_SIZE / RECORD_SIZE)
#define RECORD_MAGIC         0xA6U
#define ERASED_BYTE          0xFFU

// Set by NMI handler on flash ECC double-detection error
volatile uint8_t settings_ecc_error = 0;

static uint8_t compute_checksum(const uint8_t *rec, uint8_t len) {
    uint8_t cksum = 0;
    for (uint8_t i = 0; i < len; i++)
        cksum ^= rec[i];
    return cksum;
}

static bool erase_settings_page(void) {
    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef erase = {
        .TypeErase = FLASH_TYPEERASE_SECTORS,
        .Banks     = SETTINGS_BANK,
        .Sector    = SETTINGS_SECTOR,
        .NbSectors = 1,
    };
    uint32_t sector_error = 0;
    HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&erase, &sector_error);

    HAL_FLASH_Lock();

    // Invalidate ICACHE so subsequent flash reads don't return stale data
    HAL_ICACHE_Invalidate();

    return status == HAL_OK;
}

static int find_next_free_slot(void) {
    const uint8_t *base = (const uint8_t *)SETTINGS_PAGE_ADDR;
    settings_ecc_error = 0;

    for (int i = 0; i < (int)MAX_RECORDS; i++) {
        const uint8_t *rec = base + (i * RECORD_SIZE);
        uint8_t all_erased = 1;
        for (uint8_t j = 0; j < RECORD_SIZE; j++) {
            volatile uint8_t b = rec[j]; // volatile: read may trigger NMI
            if (settings_ecc_error) {
                erase_settings_page();
                settings_ecc_error = 0;
                return 0;
            }
            if (b != ERASED_BYTE) { all_erased = 0; break; }
        }
        if (all_erased) return i;
    }
    return -1;
}

bool settings_load(settings_t *out) {
    const uint8_t *base = (const uint8_t *)SETTINGS_PAGE_ADDR;

    // Clear any pending ECC error state
    settings_ecc_error = 0;

    // Scan backwards to find last valid record
    for (int i = (int)MAX_RECORDS - 1; i >= 0; i--) {
        const uint8_t *rec = base + (i * RECORD_SIZE);
        volatile uint8_t magic = rec[0]; // volatile: read may trigger NMI on ECC error

        // If reading flash triggered an ECC error (NMI handler sets flag),
        // the sector has a partially-written quad-word from a power loss.
        // Erase it and return defaults.
        if (settings_ecc_error) {
            SEGGER_RTT_printf(0, "[settings] ECC error at record %d, erasing sector\n", i);
            erase_settings_page();
            settings_ecc_error = 0;
            return false;
        }

        if (magic != RECORD_MAGIC) continue;

        // Checksum covers bytes 0-6, stored in byte 7
        uint8_t cksum = compute_checksum(rec, 7);
        if (settings_ecc_error) {
            SEGGER_RTT_printf(0, "[settings] ECC error at record %d, erasing sector\n", i);
            erase_settings_page();
            settings_ecc_error = 0;
            return false;
        }
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

bool settings_save(const settings_t *s) {
    int slot = find_next_free_slot();

    if (slot < 0) {
        if (!erase_settings_page()) {
            return false;
        }
        slot = 0;
    }

    uint32_t addr = SETTINGS_PAGE_ADDR + (uint32_t)slot * RECORD_SIZE;

    // Build 16-byte quad-word aligned record
    // [magic, volume, muted, bass, treble, brightness, timeout, checksum, pad x8]
    uint8_t rec[RECORD_SIZE];
    rec[0] = RECORD_MAGIC;
    rec[1] = s->local_volume;
    rec[2] = s->local_muted;
    rec[3] = (uint8_t)s->bass;
    rec[4] = (uint8_t)s->treble;
    rec[5] = s->brightness;
    rec[6] = s->display_timeout;
    rec[7] = compute_checksum(rec, 7);
    // Pad remaining bytes with 0xFF (erased state)
    for (uint8_t i = 8; i < RECORD_SIZE; i++)
        rec[i] = ERASED_BYTE;

    // STM32H5 programs in quad-words (128 bits = 16 bytes)
    HAL_FLASH_Unlock();

    HAL_StatusTypeDef status = HAL_FLASH_Program(
        FLASH_TYPEPROGRAM_QUADWORD, addr, (uint32_t)rec);
    if (status != HAL_OK) {
        HAL_FLASH_Lock();
        return false;
    }

    HAL_FLASH_Lock();
    return true;
}
