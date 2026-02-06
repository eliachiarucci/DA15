// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * Audio Output via I2S DMA
 * Works with TinyUSB audio FIFO
 */

#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <stdint.h>

// Initialize audio output hardware
void audio_output_init(void);

// Start streaming (called when USB alt interface is set to streaming mode)
void audio_output_start_streaming(void);

// Stop streaming (called when USB alt interface is set to 0 or disconnect)
void audio_output_stop_streaming(void);

// Audio processing task - call this from main loop
// Reads from USB FIFO and feeds I2S DMA buffer
void audio_output_task(void);

// Set USB mute state (called from USB volume control)
void audio_output_set_mute(uint8_t mute);

// Local pre-attenuation (encoder-controlled, independent of USB volume)
// Volume 0-100: 100 = unity gain, 0 = silence
void audio_output_set_local_volume(uint8_t vol);
uint8_t audio_output_get_local_volume(void);

// Local mute toggle (independent of USB mute)
void audio_output_toggle_local_mute(void);
uint8_t audio_output_is_local_muted(void);

#endif // AUDIO_OUTPUT_H
