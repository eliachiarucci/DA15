// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * 2-Band EQ - Bass, Treble
 * Bass: Two-stage lowpass + boost (0 to +6 levels)
 * Treble: First-order highpass + boost (~1500Hz)
 */

#ifndef AUDIO_EQ_H_
#define AUDIO_EQ_H_

#include <stdint.h>
#include <stdbool.h>

// EQ band indices
#define EQ_BAND_BASS    0
#define EQ_BAND_TREBLE  1
#define EQ_NUM_BANDS    2

// EQ value range: -6 to +6 (cut/boost)
#define EQ_VALUE_MIN    (-6)
#define EQ_VALUE_MAX    6
#define EQ_VALUE_FLAT   0

// Initialize EQ (sets to flat/0dB)
void audio_eq_init(void);

// Set EQ band value (0 to 10 dB boost, only bass implemented)
void audio_eq_set_band(uint8_t band, int8_t value);

// Get EQ band value
int8_t audio_eq_get_band(uint8_t band);

// Reset filter state (call on stream start to avoid transient spikes)
void audio_eq_reset_state(void);

// Enable/disable EQ processing
void audio_eq_enable(bool enable);
bool audio_eq_is_enabled(void);

// Process audio buffer through EQ (in-place, stereo interleaved)
// Buffer contains 24-bit signed values in int32_t
// volume_scale: 0-256 (256 = unity gain, 0 = mute)
void audio_eq_process(int32_t* buffer, uint16_t sample_count, uint16_t volume_scale);

#endif /* AUDIO_EQ_H_ */
