// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * Host-side unit tests for the 2-band EQ (App/Src/audio_eq.c).
 * audio_eq.c is pure C with no hardware dependencies, so it compiles
 * and runs unmodified on the host.
 */

#include "audio_eq.h"
#include "test_util.h"
#include <stdint.h>
#include <string.h>

#define BUF_SAMPLES 256 // 128 stereo frames

// Reference volume scaling, same math the firmware uses
static int32_t ref_volume(int32_t sample, uint32_t scale) {
    return (int32_t)(((int64_t)sample * scale) >> 16);
}

static void fill_ramp(int32_t *buf, uint16_t n) {
    for (uint16_t i = 0; i < n; i++)
        buf[i] = (int32_t)((i * 65537) % 16777216) - 8388608; // 24-bit range
}

static void test_flat_unity_is_identity(void) {
    int32_t buf[BUF_SAMPLES], orig[BUF_SAMPLES];
    audio_eq_init();
    fill_ramp(buf, BUF_SAMPLES);
    memcpy(orig, buf, sizeof(buf));

    audio_eq_process(buf, BUF_SAMPLES, 65536);
    CHECK(memcmp(buf, orig, sizeof(buf)) == 0);
}

static void test_flat_applies_volume(void) {
    int32_t buf[BUF_SAMPLES], orig[BUF_SAMPLES];
    audio_eq_init();
    fill_ramp(buf, BUF_SAMPLES);
    memcpy(orig, buf, sizeof(buf));

    audio_eq_process(buf, BUF_SAMPLES, 32768); // -6dB-ish (half)
    for (uint16_t i = 0; i < BUF_SAMPLES; i++)
        CHECK_EQ_I32(buf[i], ref_volume(orig[i], 32768));
}

static void test_flat_mute_is_silence(void) {
    int32_t buf[BUF_SAMPLES];
    audio_eq_init();
    fill_ramp(buf, BUF_SAMPLES);

    audio_eq_process(buf, BUF_SAMPLES, 0);
    for (uint16_t i = 0; i < BUF_SAMPLES; i++)
        CHECK_EQ_I32(buf[i], 0);
}

static void test_band_set_get_and_clamp(void) {
    audio_eq_init();
    audio_eq_set_band(EQ_BAND_BASS, 4);
    audio_eq_set_band(EQ_BAND_TREBLE, -3);
    CHECK_EQ_I32(audio_eq_get_band(EQ_BAND_BASS), 4);
    CHECK_EQ_I32(audio_eq_get_band(EQ_BAND_TREBLE), -3);

    audio_eq_set_band(EQ_BAND_BASS, 100);   // clamps to +6
    audio_eq_set_band(EQ_BAND_TREBLE, -100); // clamps to -6
    CHECK_EQ_I32(audio_eq_get_band(EQ_BAND_BASS), EQ_VALUE_MAX);
    CHECK_EQ_I32(audio_eq_get_band(EQ_BAND_TREBLE), EQ_VALUE_MIN);
}

static void test_boost_output_stays_in_24bit_range(void) {
    // Full-scale low-frequency square wave through max boost on both bands:
    // the output limiter must hold the 24-bit range
    int32_t buf[BUF_SAMPLES];
    audio_eq_init();
    audio_eq_set_band(EQ_BAND_BASS, EQ_VALUE_MAX);
    audio_eq_set_band(EQ_BAND_TREBLE, EQ_VALUE_MAX);

    for (int block = 0; block < 64; block++) {
        for (uint16_t i = 0; i < BUF_SAMPLES; i += 2) {
            int32_t v = ((block % 8) < 4) ? 8388607 : -8388608;
            buf[i] = v;
            buf[i + 1] = v;
        }
        audio_eq_process(buf, BUF_SAMPLES, 65536);
        for (uint16_t i = 0; i < BUF_SAMPLES; i++) {
            CHECK(buf[i] <= 8388607);
            CHECK(buf[i] >= -8388608);
        }
    }
}

static void test_boost_actually_changes_signal(void) {
    // A low-frequency signal with bass boost must differ from the input
    int32_t buf[BUF_SAMPLES], orig[BUF_SAMPLES];
    audio_eq_init();
    audio_eq_set_band(EQ_BAND_BASS, EQ_VALUE_MAX);

    for (uint16_t i = 0; i < BUF_SAMPLES; i += 2) {
        buf[i] = 1000000;
        buf[i + 1] = 1000000;
    }
    memcpy(orig, buf, sizeof(buf));

    // Run several blocks so the filters settle
    int changed = 0;
    for (int block = 0; block < 16; block++) {
        memcpy(buf, orig, sizeof(buf));
        audio_eq_process(buf, BUF_SAMPLES, 65536);
        if (memcmp(buf, orig, sizeof(buf)) != 0)
            changed = 1;
    }
    CHECK(changed);
}

static void test_reset_state_gives_zero_output_for_zero_input(void) {
    int32_t buf[BUF_SAMPLES];
    audio_eq_init();
    audio_eq_set_band(EQ_BAND_BASS, 6);
    audio_eq_set_band(EQ_BAND_TREBLE, 6);

    // Pump signal through to charge the filter state
    fill_ramp(buf, BUF_SAMPLES);
    audio_eq_process(buf, BUF_SAMPLES, 65536);

    // After a state reset, silence in must be silence out (all state zero)
    audio_eq_reset_state();
    memset(buf, 0, sizeof(buf));
    audio_eq_process(buf, BUF_SAMPLES, 65536);
    for (uint16_t i = 0; i < BUF_SAMPLES; i++)
        CHECK_EQ_I32(buf[i], 0);
}

static void test_disable_bypasses_eq(void) {
    int32_t buf[BUF_SAMPLES], orig[BUF_SAMPLES];
    audio_eq_init();
    audio_eq_set_band(EQ_BAND_BASS, 6);
    audio_eq_enable(false);
    CHECK(!audio_eq_is_enabled());

    fill_ramp(buf, BUF_SAMPLES);
    memcpy(orig, buf, sizeof(buf));
    audio_eq_process(buf, BUF_SAMPLES, 65536);
    CHECK(memcmp(buf, orig, sizeof(buf)) == 0);

    audio_eq_enable(true);
    CHECK(audio_eq_is_enabled());
}

int main(void) {
    test_flat_unity_is_identity();
    test_flat_applies_volume();
    test_flat_mute_is_silence();
    test_band_set_get_and_clamp();
    test_boost_output_stays_in_24bit_range();
    test_boost_actually_changes_signal();
    test_reset_state_gives_zero_output_for_zero_input();
    test_disable_bypasses_eq();
    return test_summary("audio_eq");
}
