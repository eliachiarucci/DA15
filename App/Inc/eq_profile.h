// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * Parametric EQ Profile System
 *
 * Supports up to 10 profiles, each with up to 10 biquad filters.
 * Filter types: bell, low shelf, high shelf, low pass, high pass.
 * Coefficients are pre-computed by the PC app and stored alongside
 * filter parameters (freq/gain/Q) for display purposes.
 *
 * Profiles are stored in a dedicated 8KB flash sector (0x0801C000).
 * The active profile index is stored in the settings sector.
 */

#ifndef EQ_PROFILE_H
#define EQ_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

#define EQ_MAX_PROFILES     10
#define EQ_MAX_FILTERS      10
#define EQ_PROFILE_NAME_LEN 16

#define EQ_PROFILE_OFF      0xFF

// ---------------------------------------------------------------------------
// Filter types
// ---------------------------------------------------------------------------
typedef enum {
    FILTER_OFF = 0,
    FILTER_BELL,
    FILTER_LOW_SHELF,
    FILTER_HIGH_SHELF,
    FILTER_LOW_PASS,
    FILTER_HIGH_PASS,
} eq_filter_type_t;

// ---------------------------------------------------------------------------
// Single biquad filter (36 bytes)
// ---------------------------------------------------------------------------
typedef struct {
    float b0, b1, b2, a1, a2;  // biquad coefficients (normalized, a0=1)
    float freq;                  // center/corner frequency (Hz)
    float gain;                  // gain (dB)
    float q;                     // Q factor
    uint8_t type;                // eq_filter_type_t
    uint8_t enabled;             // 0=bypass, 1=active
    uint8_t _pad[2];
} eq_filter_t;

// ---------------------------------------------------------------------------
// EQ profile (380 bytes)
// ---------------------------------------------------------------------------
typedef struct {
    char name[EQ_PROFILE_NAME_LEN]; // null-terminated
    uint8_t filter_count;            // 0 to EQ_MAX_FILTERS
    uint8_t _pad[3];
    eq_filter_t filters[EQ_MAX_FILTERS];
} eq_profile_t;

// ---------------------------------------------------------------------------
// Profile management
// ---------------------------------------------------------------------------

// Load profiles from flash into RAM. Call once at startup.
void eq_profile_init(void);

// Get a profile by ID (0-9). Returns NULL if slot is empty.
const eq_profile_t *eq_profile_get(uint8_t id);

// Write a profile to a slot (RAM only). Returns false if id >= MAX.
bool eq_profile_set(uint8_t id, const eq_profile_t *p);

// Clear a profile slot (RAM only). Returns false if id >= MAX.
bool eq_profile_delete(uint8_t id);

// Number of non-empty profile slots.
uint8_t eq_profile_count(void);

// Persist all profiles from RAM to flash (erases sector, writes all).
bool eq_profile_save_to_flash(void);

// ---------------------------------------------------------------------------
// Active profile
// ---------------------------------------------------------------------------

// Set the active profile. EQ_PROFILE_OFF = use legacy bass/treble.
// Takes effect immediately (resets biquad state).
void eq_profile_set_active(uint8_t id);

// Get the active profile ID (EQ_PROFILE_OFF if none).
uint8_t eq_profile_get_active(void);

// Get the active profile name, or "OFF" if none.
const char *eq_profile_get_active_name(void);

// ---------------------------------------------------------------------------
// Audio processing
// ---------------------------------------------------------------------------

// Process audio through the active profile's biquad cascade.
// Same signature as audio_eq_process() for easy swapping.
// buffer: stereo interleaved int32_t (24-bit signed values)
// sample_count: total mono samples (frames * 2)
// volume_scale: 0-256 (256 = unity)
void eq_profile_process(int32_t *buffer, uint16_t sample_count,
                        uint16_t volume_scale);

// Clear biquad filter state (call on stream start to avoid transients).
void eq_profile_reset_state(void);

#endif // EQ_PROFILE_H
