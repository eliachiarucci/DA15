// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * 2-Band EQ - Bass, Treble
 * Bass: Bandpass-style boost (~50-180Hz) for punch/thump
 *       Highpass at ~50Hz + two-stage lowpass at ~180Hz
 * Treble: First-order highpass + boost (~1700Hz)
 * Full 24-bit precision using split-multiply to avoid int32_t overflow on Cortex-M0
 */

#include "audio_eq.h"
#include <string.h>

//--------------------------------------------------------------------+
// Split-multiply for 24-bit x Q12 without overflow
// Decomposes: (a * b) >> 12 = (a_hi * b) >> 4 + (a_lo * b) >> 12
// where a_hi = a >> 8 (signed 16-bit), a_lo = a & 0xFF (unsigned 8-bit)
// Max error: ±1 LSB per operation (rounding)
// Cost: 2 MUL + 2 shift + 1 AND + 1 ADD = ~7 cycles on M0
//--------------------------------------------------------------------+
static inline int32_t mul_q12(int32_t a, int32_t b) {
    int32_t hi = a >> 8;
    int32_t lo = a & 0xFF;
    return (hi * b >> 4) + (lo * b >> 12);
}

//--------------------------------------------------------------------+
// Bass: Bandpass-style boost (Q12 format, 4096 = 1.0)
// Highpass at ~50Hz to cut sub-bass rumble
// Lowpass at ~180Hz to focus on punch/thump frequencies
// Effective boost range: ~50-180Hz (kick drum / bass punch territory)
//--------------------------------------------------------------------+

#define Q12_SHIFT 12
#define BASS_LP_ALPHA  95     // ~0.0233 * 4096 for ~180Hz
#define BASS_LP_BETA   4001   // 4096 - 95
#define BASS_HP_ALPHA  27     // ~0.0065 * 4096 for ~50Hz
#define BASS_HP_BETA   4069   // 4096 - 27

//--------------------------------------------------------------------+
// Treble: First-order highpass + boost (Q12 format)
// fc ~= 1500Hz at 48kHz
// highpass = in - lowpass, then boost high frequencies
//--------------------------------------------------------------------+

#define TREBLE_LP_ALPHA  817    // ~0.1995 * 4096 for ~1700Hz lowpass
#define TREBLE_LP_BETA   3279   // 4096 - 817

// 24-bit signed range limits
#define AUDIO_24BIT_MAX  8388607
#define AUDIO_24BIT_MIN  (-8388608)

// Fixed pre-attenuation: -5dB = 0.562 * 4096 = 2303
#define PREATT_SCALE     2303

// Gain table: level 0-7 maps to gain in Q12
// Extended to 7 to support +1 bass offset (user +6 = internal +7)
// Linear from 0 to 1.167 at max
static const int16_t gain_table[8] = {
    0,      // level 0: bypass
    683,    // level 1: 0.167 (1/6)
    1365,   // level 2: 0.333 (2/6)
    2048,   // level 3: 0.5   (3/6)
    2731,   // level 4: 0.667 (4/6)
    3413,   // level 5: 0.833 (5/6)
    4096,   // level 6: 1.0   (6/6)
    4779,   // level 7: 1.167 (7/6)
};

//--------------------------------------------------------------------+
// State
//--------------------------------------------------------------------+

static int8_t bass_level = 0;
static int8_t treble_level = 0;
static bool eq_enabled = true;

// Bass filter state: highpass (for sub-bass cut) + two-stage lowpass
static int32_t bass_hp_lp_left = 0;   // Lowpass state for computing highpass
static int32_t bass_hp_lp_right = 0;
static int32_t lp1_left = 0;          // First lowpass stage
static int32_t lp1_right = 0;
static int32_t lp2_left = 0;          // Second lowpass stage
static int32_t lp2_right = 0;

// First-order lowpass state for treble highpass (hp = in - lp)
static int32_t treble_lp_left = 0;
static int32_t treble_lp_right = 0;

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

void audio_eq_init(void) {
    bass_level = 0;
    treble_level = 0;
    eq_enabled = true;
    // Bass filter state (highpass + two-stage lowpass)
    bass_hp_lp_left = 0;
    bass_hp_lp_right = 0;
    lp1_left = 0;
    lp1_right = 0;
    lp2_left = 0;
    lp2_right = 0;
    // Treble filter state
    treble_lp_left = 0;
    treble_lp_right = 0;
}

void audio_eq_set_band(uint8_t band, int8_t value) {
    if (value < -6) value = -6;
    else if (value > 6) value = 6;

    if (band == EQ_BAND_BASS)
        bass_level = value;
    else if (band == EQ_BAND_TREBLE)
        treble_level = value;
}

int8_t audio_eq_get_band(uint8_t band) {
    if (band == EQ_BAND_BASS) return (int8_t)bass_level;
    if (band == EQ_BAND_TREBLE) return (int8_t)treble_level;
    return 0;
}

void audio_eq_reset_state(void) {
    bass_hp_lp_left = 0;
    bass_hp_lp_right = 0;
    lp1_left = 0;
    lp1_right = 0;
    lp2_left = 0;
    lp2_right = 0;
    treble_lp_left = 0;
    treble_lp_right = 0;
}

void audio_eq_enable(bool enable) {
    eq_enabled = enable;
    if (!enable) {
        bass_hp_lp_left = 0;
        bass_hp_lp_right = 0;
        lp1_left = 0;
        lp1_right = 0;
        lp2_left = 0;
        lp2_right = 0;
        treble_lp_left = 0;
        treble_lp_right = 0;
    }
}

bool audio_eq_is_enabled(void) {
    return eq_enabled;
}

void audio_eq_process(int32_t *buffer, uint16_t sample_count, uint16_t volume_scale) {
    // If EQ disabled or all bands at effective 0, apply pre-attenuation + volume only
    // Bass has +1 offset, so user's -1 is the true bypass point
    if (!eq_enabled || (bass_level == -1 && treble_level == 0)) {
        for (uint16_t i = 0; i < sample_count; i++) {
            int32_t sample = mul_q12(buffer[i], PREATT_SCALE);
            sample = (sample * volume_scale) >> 8;
            buffer[i] = sample;
        }
        return;
    }

    // Apply +1 offset to bass so user's "0" gives a subtle boost (internal +1)
    // User -6 to +6 maps to internal -5 to +7
    int8_t effective_bass = bass_level + 1;
    // No clamping needed - table now supports 0-8

    int8_t abs_bass = effective_bass < 0 ? -effective_bass : effective_bass;
    int8_t abs_treble = treble_level < 0 ? -treble_level : treble_level;
    int32_t bass_gain = gain_table[abs_bass];
    bass_gain = (bass_gain << 1) + bass_gain;  // 3x multiplier for ~+13dB max bass
    int32_t treble_gain = gain_table[abs_treble];
    treble_gain = treble_gain + (treble_gain >> 2);  // 1.25x multiplier for treble

    // Process stereo interleaved: L, R, L, R, ...
    // All filter math at full 24-bit precision using split-multiply
    for (uint16_t i = 0; i < sample_count; i += 2) {
        // Fixed -5dB pre-attenuation for EQ headroom
        int32_t out_l = mul_q12(buffer[i], PREATT_SCALE);
        int32_t out_r = mul_q12(buffer[i + 1], PREATT_SCALE);

        // Bass processing (bandpass-style: highpass @50Hz + lowpass @180Hz + boost/cut)
        // This focuses the boost on "thump" frequencies (50-180Hz) instead of all bass
        if (effective_bass != 0) {
            int32_t in_l = out_l;
            int32_t in_r = out_r;

            // Highpass at ~50Hz: removes sub-bass rumble, keeps punch
            // hp = in - lp, where lp tracks the sub-bass
            bass_hp_lp_left = mul_q12(in_l, BASS_HP_ALPHA) + mul_q12(bass_hp_lp_left, BASS_HP_BETA);
            bass_hp_lp_right = mul_q12(in_r, BASS_HP_ALPHA) + mul_q12(bass_hp_lp_right, BASS_HP_BETA);
            int32_t hp_l = in_l - bass_hp_lp_left;
            int32_t hp_r = in_r - bass_hp_lp_right;

            // Two-stage lowpass at ~180Hz on the highpassed signal
            // This isolates the 50-180Hz "thump" band
            lp1_left = mul_q12(hp_l, BASS_LP_ALPHA) + mul_q12(lp1_left, BASS_LP_BETA);
            lp1_right = mul_q12(hp_r, BASS_LP_ALPHA) + mul_q12(lp1_right, BASS_LP_BETA);
            lp2_left = mul_q12(lp1_left, BASS_LP_ALPHA) + mul_q12(lp2_left, BASS_LP_BETA);
            lp2_right = mul_q12(lp1_right, BASS_LP_ALPHA) + mul_q12(lp2_right, BASS_LP_BETA);

            // Boost (positive) or cut (negative): out = in ± gain * bandpassed
            int32_t bl = mul_q12(lp2_left, bass_gain);
            int32_t br = mul_q12(lp2_right, bass_gain);
            if (effective_bass > 0) {
                out_l = in_l + bl;
                out_r = in_r + br;
            } else {
                out_l = in_l - bl;
                out_r = in_r - br;
            }
        }

        // Treble processing (first-order highpass + boost/cut at ~1700Hz)
        if (treble_level != 0) {
            int32_t in_l = out_l;
            int32_t in_r = out_r;

            // First-order lowpass (to subtract for highpass)
            treble_lp_left = mul_q12(in_l, TREBLE_LP_ALPHA) + mul_q12(treble_lp_left, TREBLE_LP_BETA);
            treble_lp_right = mul_q12(in_r, TREBLE_LP_ALPHA) + mul_q12(treble_lp_right, TREBLE_LP_BETA);

            // Highpass = input - lowpass
            int32_t hp_l = in_l - treble_lp_left;
            int32_t hp_r = in_r - treble_lp_right;

            // Boost (positive) or cut (negative): out = in ± gain * highpass
            int32_t tl = mul_q12(hp_l, treble_gain);
            int32_t tr = mul_q12(hp_r, treble_gain);
            if (treble_level > 0) {
                out_l = in_l + tl;
                out_r = in_r + tr;
            } else {
                out_l = in_l - tl;
                out_r = in_r - tr;
            }
        }

        // Hard limit to 24-bit signed range
        if (out_l > AUDIO_24BIT_MAX) out_l = AUDIO_24BIT_MAX;
        else if (out_l < AUDIO_24BIT_MIN) out_l = AUDIO_24BIT_MIN;
        if (out_r > AUDIO_24BIT_MAX) out_r = AUDIO_24BIT_MAX;
        else if (out_r < AUDIO_24BIT_MIN) out_r = AUDIO_24BIT_MIN;

        // Apply volume (24-bit * 8-bit = fits int32_t after clamp above)
        if (volume_scale < 256) {
            out_l = (out_l * volume_scale) >> 8;
            out_r = (out_r * volume_scale) >> 8;
        }

        buffer[i] = out_l;
        buffer[i + 1] = out_r;
    }
}
