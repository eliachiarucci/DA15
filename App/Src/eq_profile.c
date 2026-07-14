// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * Parametric EQ Profile System
 *
 * Flash storage: 8KB sector at 0x0801C000 (Bank 2, Sector 6).
 * On init, the entire store is loaded into RAM. Modifications happen
 * in RAM; flash save erases the sector and writes back in non-blocking
 * chunks via eq_profile_flash_task() to avoid stalling audio DMA.
 *
 * Audio processing: Direct Form II Transposed biquad cascade using
 * the Cortex-M33 single-precision FPU.
 */

#include "eq_profile.h"
#include "SEGGER_RTT.h"
#include "stm32h5xx_hal.h"
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Flash layout
// ---------------------------------------------------------------------------
#define PROFILES_BANK       FLASH_BANK_2
#define PROFILES_SECTOR     6U
#define PROFILES_ADDR       0x0801C000U
#define PROFILES_SIZE       8192U

#define PROFILE_MAGIC       0xEA150F1EU
#define PROFILE_VERSION     1U

// ---------------------------------------------------------------------------
// Flash store structure (lives in flash, mirrored in RAM)
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  profile_count;
    uint8_t  _pad[2];
    uint32_t checksum;
    uint8_t  _reserved[4];
    eq_profile_t profiles[EQ_MAX_PROFILES];
} eq_profile_store_t;

_Static_assert(sizeof(eq_profile_store_t) <= PROFILES_SIZE,
               "Profile store exceeds flash sector size");

// ---------------------------------------------------------------------------
// RAM state
// ---------------------------------------------------------------------------
static eq_profile_store_t store;
static uint8_t active_profile = EQ_PROFILE_OFF;

// Biquad state: Direct Form II Transposed (2 floats per filter per channel)
typedef struct {
    float s1, s2;
} biquad_state_t;

static biquad_state_t filter_state[EQ_MAX_FILTERS][2]; // [filter][channel]

// Cached pre-attenuation for the active profile
static float profile_preatt = 1.0f;

// Compute pre-attenuation from the sum of positive filter gains
// Conservative: assumes all boosting filters could overlap at one frequency
static float compute_profile_preatt(const eq_profile_t *prof) {
    float sum_db = 0.0f;
    for (uint8_t f = 0; f < prof->filter_count; f++) {
        const eq_filter_t *filt = &prof->filters[f];
        if (filt->enabled && filt->type != FILTER_OFF && filt->gain > 0.0f)
            sum_db += filt->gain;
    }
    if (sum_db <= 0.0f)
        return 1.0f;
    // 10^(-sum_db/20): exact, only computed on profile change
    float lin = powf(10.0f, -sum_db * 0.05f);
    if (lin < 0.01f) lin = 0.01f; // Floor at -40dB
    return lin;
}

// ---------------------------------------------------------------------------
// Non-blocking flash write state machine
// ---------------------------------------------------------------------------
// Quad-words to write per main loop tick. Kept small so one tick's writes
// stay well under the 2ms audio half-buffer deadline of the main loop.
#define FLASH_WRITES_PER_TICK 8

static eq_flash_status_t flash_op = EQ_FLASH_IDLE;
static uint32_t flash_write_offset;
static uint32_t flash_write_total;
static uint8_t  flash_pad_buf[16]; // For partial last quad-word

// ---------------------------------------------------------------------------
// CRC32 (same polynomial as zlib, computed in software)
// ---------------------------------------------------------------------------
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len) {
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320U;
            else
                crc >>= 1;
        }
    }
    return ~crc;
}

// ---------------------------------------------------------------------------
// Profile management
// ---------------------------------------------------------------------------
static bool is_profile_empty(const eq_profile_t *p) {
    return p->name[0] == '\0' || p->filter_count == 0;
}

// ---------------------------------------------------------------------------
// Coefficient validation
// Host-supplied filters must never reach the amplifier unchecked: NaN/Inf
// coefficients pass the output clamps (NaN comparisons are false) and an
// unstable pole pair turns into full-scale oscillation.
// ---------------------------------------------------------------------------
static bool filter_is_sane(const eq_filter_t *f) {
    if (!f->enabled || f->type == FILTER_OFF)
        return true; // bypassed: never runs

    if (!isfinite(f->b0) || !isfinite(f->b1) || !isfinite(f->b2) ||
        !isfinite(f->a1) || !isfinite(f->a2) ||
        !isfinite(f->freq) || !isfinite(f->gain) || !isfinite(f->q))
        return false;

    // BIBO stability triangle for the denominator: |a2| < 1, |a1| < 1 + a2
    if (fabsf(f->a2) >= 1.0f)
        return false;
    if (fabsf(f->a1) >= 1.0f + f->a2)
        return false;

    return true;
}

static bool profile_is_sane(const eq_profile_t *p) {
    uint8_t count = p->filter_count;
    if (count > EQ_MAX_FILTERS)
        count = EQ_MAX_FILTERS;
    for (uint8_t f = 0; f < count; f++) {
        if (!filter_is_sane(&p->filters[f]))
            return false;
    }
    return true;
}

void eq_profile_init(void) {
    const eq_profile_store_t *flash =
        (const eq_profile_store_t *)PROFILES_ADDR;

    // Try to load from flash
    if (flash->magic == PROFILE_MAGIC && flash->version == PROFILE_VERSION) {
        uint32_t crc = crc32_update(
            0, (const uint8_t *)flash->profiles, sizeof(flash->profiles));
        if (crc == flash->checksum) {
            memcpy(&store, flash, sizeof(store));

            // Drop any stored profile with corrupt/unstable coefficients
            // (e.g. written by older firmware without validation)
            uint8_t dropped = 0;
            for (uint8_t i = 0; i < EQ_MAX_PROFILES; i++) {
                if (!is_profile_empty(&store.profiles[i]) &&
                    !profile_is_sane(&store.profiles[i])) {
                    memset(&store.profiles[i], 0, sizeof(eq_profile_t));
                    dropped++;
                }
            }
            if (dropped) {
                store.profile_count = 0;
                for (uint8_t i = 0; i < EQ_MAX_PROFILES; i++) {
                    if (!is_profile_empty(&store.profiles[i]))
                        store.profile_count++;
                }
                SEGGER_RTT_printf(0, "[eq] dropped %d invalid profiles\n",
                                  dropped);
            }

            SEGGER_RTT_printf(0, "[eq] loaded %d profiles from flash\n",
                              store.profile_count);
            eq_profile_reset_state();
            return;
        }
        SEGGER_RTT_printf(0, "[eq] flash CRC mismatch, using defaults\n");
    } else {
        SEGGER_RTT_printf(0, "[eq] no valid profile store in flash\n");
    }

    // Initialize empty store
    memset(&store, 0, sizeof(store));
    store.magic = PROFILE_MAGIC;
    store.version = PROFILE_VERSION;
    active_profile = EQ_PROFILE_OFF;
    eq_profile_reset_state();
}

const eq_profile_t *eq_profile_get(uint8_t id) {
    if (id >= EQ_MAX_PROFILES)
        return NULL;
    if (is_profile_empty(&store.profiles[id]))
        return NULL;
    return &store.profiles[id];
}

bool eq_profile_set(uint8_t id, const eq_profile_t *p) {
    if (id >= EQ_MAX_PROFILES || p == NULL)
        return false;
    if (!profile_is_sane(p))
        return false;

    memcpy(&store.profiles[id], p, sizeof(eq_profile_t));

    // Ensure name is null-terminated
    store.profiles[id].name[EQ_PROFILE_NAME_LEN - 1] = '\0';

    // Clamp filter count
    if (store.profiles[id].filter_count > EQ_MAX_FILTERS)
        store.profiles[id].filter_count = EQ_MAX_FILTERS;

    // Recalculate pre-attenuation if this is the active profile
    if (id == active_profile)
        profile_preatt = compute_profile_preatt(&store.profiles[id]);

    // Recount
    store.profile_count = 0;
    for (uint8_t i = 0; i < EQ_MAX_PROFILES; i++) {
        if (!is_profile_empty(&store.profiles[i]))
            store.profile_count++;
    }

    return true;
}

bool eq_profile_delete(uint8_t id) {
    if (id >= EQ_MAX_PROFILES)
        return false;

    memset(&store.profiles[id], 0, sizeof(eq_profile_t));

    // Recount
    store.profile_count = 0;
    for (uint8_t i = 0; i < EQ_MAX_PROFILES; i++) {
        if (!is_profile_empty(&store.profiles[i]))
            store.profile_count++;
    }

    // If deleted profile was active, deactivate
    if (id == active_profile)
        active_profile = EQ_PROFILE_OFF;

    return true;
}

uint8_t eq_profile_count(void) {
    return store.profile_count;
}

// ---------------------------------------------------------------------------
// Non-blocking flash save
// ---------------------------------------------------------------------------
bool eq_profile_start_flash_save(void) {
    if (flash_op == EQ_FLASH_ERASING || flash_op == EQ_FLASH_BUSY)
        return false;

    // Update checksum
    store.checksum = crc32_update(
        0, (const uint8_t *)store.profiles, sizeof(store.profiles));

    // Start the sector erase WITHOUT waiting for completion: the sector is
    // in bank 2 while code executes from bank 1 (read-while-write), so the
    // CPU — and the audio loop — keep running. Completion is polled in
    // eq_profile_flash_task().
    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    FLASH_Erase_Sector(PROFILES_SECTOR, PROFILES_BANK);
    flash_op = EQ_FLASH_ERASING;
    return true;
}

void eq_profile_flash_task(void) {
    if (flash_op == EQ_FLASH_ERASING) {
        // Same completion condition FLASH_WaitForLastOperation polls on.
        // BSY latches as soon as START is written (the blocking HAL erase
        // relies on the same behavior), and this task first runs a full
        // main-loop pass after the erase was started.
        if ((FLASH_NS->NSSR &
             (FLASH_FLAG_BSY | FLASH_FLAG_WBNE | FLASH_FLAG_DBNE)) != 0U)
            return;

        // Deassert the erase request (mirrors the tail of HAL_FLASHEx_Erase)
        CLEAR_BIT(FLASH_NS->NSCR, FLASH_CR_SER | FLASH_CR_SNB | FLASH_CR_BKSEL);

        if (__HAL_FLASH_GET_FLAG(FLASH_FLAG_ALL_ERRORS)) {
            __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
            HAL_FLASH_Lock();
            SEGGER_RTT_printf(0, "[eq] flash erase failed\n");
            flash_op = EQ_FLASH_DONE_ERR;
            return;
        }

        // Erase done — start incremental writes on the next ticks
        flash_write_total = (sizeof(eq_profile_store_t) + 15U) & ~15U;
        flash_write_offset = 0;
        flash_op = EQ_FLASH_BUSY;
        return;
    }

    if (flash_op != EQ_FLASH_BUSY)
        return;

    const uint8_t *src = (const uint8_t *)&store;
    uint32_t total = sizeof(eq_profile_store_t);

    // Write up to FLASH_WRITES_PER_TICK quad-words per call
    for (uint8_t n = 0; n < FLASH_WRITES_PER_TICK && flash_write_offset < flash_write_total; n++) {
        uint32_t addr = PROFILES_ADDR + flash_write_offset;

        if (flash_write_offset + 16 <= total) {
            // Full quad-word from source
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_QUADWORD, addr,
                                  (uint32_t)(uintptr_t)(src + flash_write_offset)) != HAL_OK) {
                HAL_FLASH_Lock();
                SEGGER_RTT_printf(0, "[eq] flash write failed at offset %lu\n",
                                  flash_write_offset);
                flash_op = EQ_FLASH_DONE_ERR;
                return;
            }
        } else {
            // Partial last quad-word: pad with 0xFF
            memset(flash_pad_buf, 0xFF, 16);
            uint32_t remaining = total - flash_write_offset;
            memcpy(flash_pad_buf, src + flash_write_offset, remaining);
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_QUADWORD, addr,
                                  (uint32_t)(uintptr_t)flash_pad_buf) != HAL_OK) {
                HAL_FLASH_Lock();
                SEGGER_RTT_printf(0, "[eq] flash write failed at offset %lu\n",
                                  flash_write_offset);
                flash_op = EQ_FLASH_DONE_ERR;
                return;
            }
        }
        flash_write_offset += 16;
    }

    if (flash_write_offset >= flash_write_total) {
        HAL_FLASH_Lock();
        SEGGER_RTT_printf(0, "[eq] saved %d profiles to flash\n",
                          store.profile_count);
        flash_op = EQ_FLASH_DONE_OK;
    }
}

eq_flash_status_t eq_profile_flash_status(void) {
    eq_flash_status_t s = flash_op;
    // Auto-reset terminal states so caller doesn't need to ack
    if (s == EQ_FLASH_DONE_OK || s == EQ_FLASH_DONE_ERR)
        flash_op = EQ_FLASH_IDLE;
    return s;
}

bool eq_profile_flash_busy(void) {
    return flash_op == EQ_FLASH_ERASING || flash_op == EQ_FLASH_BUSY;
}

// ---------------------------------------------------------------------------
// Active profile
// ---------------------------------------------------------------------------
void eq_profile_set_active(uint8_t id) {
    if (id != EQ_PROFILE_OFF && id >= EQ_MAX_PROFILES)
        return;
    if (id != EQ_PROFILE_OFF && is_profile_empty(&store.profiles[id]))
        return;

    active_profile = id;

    if (id != EQ_PROFILE_OFF)
        profile_preatt = compute_profile_preatt(&store.profiles[id]);
    else
        profile_preatt = 1.0f;
}

uint8_t eq_profile_get_active(void) {
    return active_profile;
}

const char *eq_profile_get_active_name(void) {
    if (active_profile == EQ_PROFILE_OFF)
        return "OFF";
    if (active_profile >= EQ_MAX_PROFILES)
        return "OFF";
    if (is_profile_empty(&store.profiles[active_profile]))
        return "OFF";
    return store.profiles[active_profile].name;
}

// ---------------------------------------------------------------------------
// Audio processing - Direct Form II Transposed biquad cascade
// ---------------------------------------------------------------------------
void eq_profile_reset_state(void) {
    memset(filter_state, 0, sizeof(filter_state));
}

// 24-bit range limits
#define SAMPLE_MAX  8388607.0f
#define SAMPLE_MIN -8388608.0f
#define SAMPLE_SCALE 8388608.0f

void eq_profile_process(int32_t *buffer, uint16_t sample_count,
                        uint32_t volume_scale) {
    if (active_profile == EQ_PROFILE_OFF || active_profile >= EQ_MAX_PROFILES)
        return;

    const eq_profile_t *prof = &store.profiles[active_profile];
    if (is_profile_empty(prof))
        return;

    const float vol = (float)volume_scale * (1.0f / 65536.0f);
    const float pre_scale = profile_preatt * (1.0f / SAMPLE_SCALE);

    // Process stereo pairs
    for (uint16_t i = 0; i < sample_count; i += 2) {
        // Apply pre-attenuation before biquads to prevent clipping
        // from Q-dependent overshoot at shelf transition frequencies
        float samples[2] = {
            (float)buffer[i]     * pre_scale,
            (float)buffer[i + 1] * pre_scale,
        };

        // Run biquad cascade for each enabled filter
        for (uint8_t f = 0; f < prof->filter_count; f++) {
            const eq_filter_t *filt = &prof->filters[f];
            if (!filt->enabled || filt->type == FILTER_OFF)
                continue;

            for (uint8_t ch = 0; ch < 2; ch++) {
                biquad_state_t *st = &filter_state[f][ch];
                float x = samples[ch];

                // DF2T: y = b0*x + s1
                //       s1 = b1*x - a1*y + s2
                //       s2 = b2*x - a2*y
                float y = filt->b0 * x + st->s1;
                st->s1  = filt->b1 * x - filt->a1 * y + st->s2;
                st->s2  = filt->b2 * x - filt->a2 * y;

                samples[ch] = y;
            }
        }

        // Apply pre-attenuation + volume, convert back to int32_t
        for (uint8_t ch = 0; ch < 2; ch++) {
            float out = samples[ch] * vol * SAMPLE_SCALE;

            // Hard limit to 24-bit range
            if (out > SAMPLE_MAX) out = SAMPLE_MAX;
            if (out < SAMPLE_MIN) out = SAMPLE_MIN;

            buffer[i + ch] = (int32_t)out;
        }
    }
}
