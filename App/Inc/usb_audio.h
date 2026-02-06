// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * USB Audio API for TinyUSB UAC1
 */

#ifndef USB_AUDIO_H_
#define USB_AUDIO_H_

#include <stdint.h>
#include <stdbool.h>

// Get current sample rate set by host
uint32_t usb_audio_get_sample_rate(void);

// Check if audio is currently streaming
bool usb_audio_is_streaming(void);

// Read audio data from USB FIFO
// Returns number of bytes read
uint16_t usb_audio_read(uint8_t* buffer, uint16_t max_length);

// Get number of bytes available in USB FIFO
uint16_t usb_audio_available(void);

// Get current volume in dB (-90 to 0)
int8_t usb_audio_get_volume(void);

// Get current volume mapped 0 to 100
int8_t usb_audio_get_volume_0_100(void);

// Check if muted
bool usb_audio_is_muted(void);

#endif /* USB_AUDIO_H_ */
