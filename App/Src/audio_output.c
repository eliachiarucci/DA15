// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * Audio Output via I2S DMA
 * Reads 24-bit packed audio from TinyUSB FIFO and outputs via I2S in 32-bit
 * frames
 */

#include "audio_output.h"
#include "SEGGER_RTT.h"
#include "app.h"
#include "audio_eq.h"
#include "main.h"
#include "sh1106.h"
#include "stm32f0xx_hal.h"
#include "tusb.h"
#include "usb_audio.h"
#include <string.h>


// Debug: set to 1 to enable RTT logging
#define AUDIO_DEBUG 0

// Swap L/R channels: set to 1 to swap stereo channels
#define SWAP_CHANNELS 1

// External I2S handle from main.c
extern I2S_HandleTypeDef hi2s1;

//--------------------------------------------------------------------+
// Configuration
//--------------------------------------------------------------------+

// Audio format: 48kHz, 24-bit stereo in 32-bit I2S frames
// USB: 3 bytes per sample (packed 24-bit)
// I2S: 32-bit frames = 2 x uint16_t per channel
#define STEREO_FRAMES_PER_HALF 240 // 4ms at 48kHz

// I2S DMA buffer: 4 uint16_t per stereo frame (2 per channel in 32-bit mode)
#define I2S_HALFWORDS_PER_HALF (STEREO_FRAMES_PER_HALF * 4) // 384
#define I2S_HALFWORDS_TOTAL (I2S_HALFWORDS_PER_HALF * 2)    // 768

// HAL_I2S_Transmit_DMA Size parameter: number of "data samples"
// For 24-bit, HAL internally doubles this for DMA transfers
#define I2S_DMA_SIZE (STEREO_FRAMES_PER_HALF * 2 * 2) // 384

// USB bytes per half: 96 frames x 2 channels x 3 bytes = 576
#define USB_BYTES_PER_HALF (STEREO_FRAMES_PER_HALF * 2 * 3) // 576

// Mono samples per half (L + R)
#define MONO_SAMPLES_PER_HALF (STEREO_FRAMES_PER_HALF * 2) // 192

// Pre-buffer threshold: wait for this much data before starting DMA
// Wait for 4x the half-buffer size (~8ms) to give feedback mechanism time to
// stabilize Must be less than CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ (16 * 294 =
// 4704 bytes)
#define PREBUFFER_THRESHOLD (USB_BYTES_PER_HALF * 3)

//--------------------------------------------------------------------+
// State
//--------------------------------------------------------------------+

// I2S DMA buffer (double buffer for circular DMA)
// 24-bit in 32-bit frames: each stereo frame = 4 uint16_t
static uint16_t i2s_buffer[I2S_HALFWORDS_TOTAL] __attribute__((aligned(4)));

// Temporary buffer for reading packed 24-bit USB data
static uint8_t usb_read_buf[USB_BYTES_PER_HALF];

// Streaming state
static volatile uint8_t streaming = 0;
static volatile uint8_t dma_running = 0;
static volatile uint8_t prebuffering = 0;

// Buffer fill tracking
static volatile uint8_t first_half_needs_fill = 0;
static volatile uint8_t second_half_needs_fill = 0;

// Last sample for smooth underrun handling (prevents clicks)
// 24-bit signed values stored in int32_t
static int32_t last_sample_left = 0;
static int32_t last_sample_right = 0;

// Local pre-attenuation (encoder-controlled)
static uint8_t local_volume = 100; // 0-100, 100 = unity
static uint8_t local_muted = 0;
static uint8_t usb_muted = 0;

#if AUDIO_DEBUG
// Debug counters
static volatile uint32_t underrun_count = 0;
static volatile uint32_t partial_fill_count = 0;
static volatile uint32_t full_fill_count = 0;
static volatile uint32_t last_report_tick = 0;
#endif

//--------------------------------------------------------------------+
// Hardware Control
//--------------------------------------------------------------------+

static void unmute_dac(void) {
  HAL_GPIO_WritePin(DAC_MUTE_GPIO_Port, DAC_MUTE_Pin, GPIO_PIN_SET);
}

static void mute_dac(void) {
  HAL_GPIO_WritePin(DAC_MUTE_GPIO_Port, DAC_MUTE_Pin, GPIO_PIN_RESET);
}

static void enable_amplifier(void) {
  HAL_GPIO_WritePin(AMP_EN_GPIO_Port, AMP_EN_Pin, GPIO_PIN_SET);
}

static void disable_amplifier(void) {
  HAL_GPIO_WritePin(AMP_EN_GPIO_Port, AMP_EN_Pin, GPIO_PIN_RESET);
}

//--------------------------------------------------------------------+
// Helper Functions
//--------------------------------------------------------------------+

// Volume lookup table: maps index (0-90) to linear scale (0-256)
// Uses power curve (x^5) for very gradual low-end response
// Formula: round(256 * (i/90)^5)
static const uint16_t volume_table[91] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   // 0-10%
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   // 11-20%
    0,   0,   0,   0,   0,   0,   1,   1,   1,   1,   // 21-30%
    1,   1,   2,   2,   2,   2,   3,   3,   3,   4,   // 31-40%
    5,   5,   6,   7,   8,   8,   9,   10,  11,  12,  // 41-50%
    14,  15,  17,  19,  20,  22,  24,  26,  29,  32,  // 51-60%
    34,  37,  40,  43,  47,  51,  55,  59,  64,  69,  // 61-70%
    72,  78,  84,  90,  97,  103, 110, 118, 126, 135, // 71-80%
    142, 151, 161, 171, 181, 192, 204, 216, 229, 243, // 81-90%
    256,                                              // 100% = unity gain
};

// Fill I2S buffer with held last sample (less audible than silence on underrun)
static void fill_with_hold(uint16_t *buffer, uint16_t frame_count) {
  uint16_t l_hi, l_lo, r_hi, r_lo;
  l_hi = (uint16_t)((last_sample_left >> 8) & 0xFFFF);
  l_lo = (uint16_t)((last_sample_left & 0xFF) << 8);
  r_hi = (uint16_t)((last_sample_right >> 8) & 0xFFFF);
  r_lo = (uint16_t)((last_sample_right & 0xFF) << 8);

  for (uint16_t i = 0; i < frame_count; i++) {
    buffer[i * 4] = l_hi;
    buffer[i * 4 + 1] = l_lo;
    buffer[i * 4 + 2] = r_hi;
    buffer[i * 4 + 3] = r_lo;
  }
}

// Power-based pre-scaling factors (0-256 scale)
// 0 = 500mA:  -6dB = 10^(-6/20) = 0.501 = 128/256
// 1 = 1500mA: -4dB = 10^(-4/20) = 0.631 = 161/256
// 2 = 3000mA: -2dB = 10^(-2/20) = 0.794 = 203/256
static const uint16_t power_scale_table[3] = {128, 161, 203};

// Get volume scale for audio processing (0-256, 256 = unity)
// Combines: USB host volume × power pre-scaling × local pre-attenuation
static uint16_t get_volume_scale(void) {
  if (local_muted)
    return 0;

  int8_t vol_db = usb_audio_get_volume();
  if (vol_db < -90)
    vol_db = -90;
  if (vol_db > 0)
    vol_db = 0;

  uint16_t vol_scale = volume_table[vol_db + 90];
  uint8_t power_level = app_get_power_level();
  if (power_level > 2)
    power_level = 2;

  // Apply power-based pre-scaling: (vol_scale * power_scale) / 256
  uint16_t scale = (vol_scale * power_scale_table[power_level]) >> 8;

  // Apply local pre-attenuation: quadratic curve for perceptually linear feel
  // vol²/10000 * 256: vol=50→64(25%), vol=75→144(56%), vol=100→256(100%)
  uint32_t local_sq = (uint32_t)local_volume * local_volume; // 0-10000
  uint16_t local_scale = (uint16_t)((local_sq * 256) / 10000);
  scale = (scale * local_scale) >> 8;

  return scale;
}

// Read packed 24-bit USB audio data, process EQ+volume, write to I2S buffer
// Returns number of stereo frames written
static uint16_t read_audio_data(uint16_t *i2s_dest,
                                uint16_t usb_bytes_to_read) {
  uint16_t bytes_read = usb_audio_read(usb_read_buf, usb_bytes_to_read);
  if (bytes_read < 6)
    return 0; // Need at least one stereo frame (6 bytes)

  uint16_t stereo_frames = bytes_read / 6;
  uint16_t sample_count = stereo_frames * 2; // Mono samples (L + R)

  // Unpack 24-bit little-endian to int32_t (sign-extended)
  // Uses the I2S destination as scratch space (int32_t overlay, same size)
  int32_t *proc = (int32_t *)i2s_dest;
  for (uint16_t i = 0; i < sample_count; i++) {
    uint8_t *src = &usb_read_buf[i * 3];
    uint32_t raw = src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16);
    // Sign-extend from 24-bit
    if (raw & 0x800000)
      raw |= 0xFF000000;
    proc[i] = (int32_t)raw;
  }

#if SWAP_CHANNELS
  // Swap L/R channels
  for (uint16_t i = 0; i < sample_count; i += 2) {
    int32_t tmp = proc[i];
    proc[i] = proc[i + 1];
    proc[i + 1] = tmp;
  }
#endif

  // EQ + volume processing (operates on 24-bit values in int32_t)
  audio_eq_process(proc, sample_count, get_volume_scale());

  // Save last samples before packing (pack overwrites the int32_t data
  // in-place)
  if (sample_count >= 2) {
    last_sample_left = proc[sample_count - 2];
    last_sample_right = proc[sample_count - 1];
  }

  // Convert int32_t (24-bit) to I2S 32-bit frame format (in-place,
  // forward-safe) proc[i] at offset i*4 bytes, writing to i2s_dest[i*2] at
  // offset i*4 bytes = same location
  for (uint16_t i = 0; i < sample_count; i++) {
    int32_t s = proc[i];
    i2s_dest[i * 2] = (uint16_t)((s >> 8) & 0xFFFF);
    i2s_dest[i * 2 + 1] = (uint16_t)((s & 0xFF) << 8);
  }

  return stereo_frames;
}

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

void audio_output_init(void) {
  // Initialize EQ
  audio_eq_init();

  // Clear buffer with silence
  memset(i2s_buffer, 0, sizeof(i2s_buffer));

  last_sample_left = 0;
  last_sample_right = 0;

  // Anti-pop sequence:
  // DAC mute is hi-Z (not grounded), so we must unmute with I2S silence
  // first to give the amp a defined, clean input before enabling it.
  mute_dac();
  disable_amplifier();

  // Start I2S DMA with silence so DAC has a defined zero output
  HAL_I2S_Transmit_DMA(&hi2s1, i2s_buffer, I2S_DMA_SIZE);
  dma_running = 1;

  // Unmute DAC — now outputting clean silence via I2S
  unmute_dac();

  // Let DAC output settle, then enable amplifier into a stable signal
  HAL_Delay(500);
  enable_amplifier();
}

void audio_output_start_streaming(void) {
  if (streaming) {
    return;
  }

  streaming = 1;
  prebuffering = 1; // Start in prebuffering mode

  // Clear buffer with silence
  memset(i2s_buffer, 0, sizeof(i2s_buffer));

  last_sample_left = 0;
  last_sample_right = 0;

  first_half_needs_fill = 0;
  second_half_needs_fill = 0;

  // Don't enable amplifier yet - wait until DMA is running and DAC is unmuted
}

void audio_output_stop_streaming(void) {
  streaming = 0;
  prebuffering = 0;

  // Mute DAC (keep amp enabled to avoid pop on re-enable)
  mute_dac();

  // Stop DMA, clear buffer, restart with silence to keep DAC output clean
  if (dma_running) {
    HAL_I2S_DMAStop(&hi2s1);
    dma_running = 0;
  }

  memset(i2s_buffer, 0, sizeof(i2s_buffer));

  HAL_I2S_Transmit_DMA(&hi2s1, i2s_buffer, I2S_DMA_SIZE);
  dma_running = 1;

  // Unmute — DAC now outputs clean silence
  unmute_dac();
}

void audio_output_task(void) {
  if (!streaming) {
    return;
  }

  // Prebuffering phase: wait for enough data before starting DMA
  if (prebuffering) {
    uint16_t available = usb_audio_available();

    if (available >= PREBUFFER_THRESHOLD) {
      // Fill first half of buffer
      read_audio_data(&i2s_buffer[0], USB_BYTES_PER_HALF);

      // Check if we have enough for second half too
      available = usb_audio_available();
      if (available >= USB_BYTES_PER_HALF) {
        read_audio_data(&i2s_buffer[I2S_HALFWORDS_PER_HALF],
                        USB_BYTES_PER_HALF);
      }

      // Stop idle DMA, restart with audio data
      prebuffering = 0;
      if (dma_running) {
        HAL_I2S_DMAStop(&hi2s1);
      }
      HAL_I2S_Transmit_DMA(&hi2s1, i2s_buffer, I2S_DMA_SIZE);
      dma_running = 1;
    }
    return;
  }

  // Normal streaming: fill buffers as needed

  // Fill first half of buffer if needed
  if (first_half_needs_fill) {
    uint16_t available = usb_audio_available();

    if (available >= USB_BYTES_PER_HALF) {
      // Full fill
      read_audio_data(&i2s_buffer[0], USB_BYTES_PER_HALF);
      first_half_needs_fill = 0;
#if AUDIO_DEBUG
      full_fill_count++;
#endif
    } else if (available >= 6) {
      // Partial fill - read what we can, hold the rest
      uint16_t frames_read = read_audio_data(&i2s_buffer[0], available);
      uint16_t frames_remaining = STEREO_FRAMES_PER_HALF - frames_read;
      fill_with_hold(&i2s_buffer[frames_read * 4], frames_remaining);
      first_half_needs_fill = 0;
#if AUDIO_DEBUG
      partial_fill_count++;
      SEGGER_RTT_printf(0, "PARTIAL1: avail=%d, frames=%d\n", available,
                        frames_read);
#endif
    } else {
      // No data available - fill with held last sample
      fill_with_hold(&i2s_buffer[0], STEREO_FRAMES_PER_HALF);
      first_half_needs_fill = 0;
#if AUDIO_DEBUG
      underrun_count++;
      SEGGER_RTT_printf(0, "UNDERRUN1: t=%lu\n", HAL_GetTick());
#endif
    }
  }

  // Fill second half of buffer if needed
  if (second_half_needs_fill) {
    uint16_t available = usb_audio_available();

    if (available >= USB_BYTES_PER_HALF) {
      // Full fill
      read_audio_data(&i2s_buffer[I2S_HALFWORDS_PER_HALF], USB_BYTES_PER_HALF);
      second_half_needs_fill = 0;
#if AUDIO_DEBUG
      full_fill_count++;
#endif
    } else if (available >= 6) {
      // Partial fill - read what we can, hold the rest
      uint16_t frames_read =
          read_audio_data(&i2s_buffer[I2S_HALFWORDS_PER_HALF], available);
      uint16_t frames_remaining = STEREO_FRAMES_PER_HALF - frames_read;
      fill_with_hold(&i2s_buffer[I2S_HALFWORDS_PER_HALF + frames_read * 4],
                     frames_remaining);
      second_half_needs_fill = 0;
#if AUDIO_DEBUG
      partial_fill_count++;
      SEGGER_RTT_printf(0, "PARTIAL2: avail=%d, frames=%d\n", available,
                        frames_read);
#endif
    } else {
      // No data available - fill with held last sample
      fill_with_hold(&i2s_buffer[I2S_HALFWORDS_PER_HALF],
                     STEREO_FRAMES_PER_HALF);
      second_half_needs_fill = 0;
#if AUDIO_DEBUG
      underrun_count++;
      SEGGER_RTT_printf(0, "UNDERRUN2: t=%lu\n", HAL_GetTick());
#endif
    }
  }

#if AUDIO_DEBUG
  // Periodic status report every 2 seconds
  uint32_t now = HAL_GetTick();
  if (now - last_report_tick >= 2000) {
    uint16_t fifo_level = usb_audio_available();
    (void)fifo_level;
    // Reset counters for next period
    full_fill_count = 0;
    partial_fill_count = 0;
    underrun_count = 0;
    last_report_tick = now;
  }
#endif
}

static void update_mute_state(void) {
  if (usb_muted || local_muted) {
    mute_dac();
  } else if (dma_running) {
    unmute_dac();
  }
}

void audio_output_set_mute(uint8_t mute) {
  usb_muted = mute;
  update_mute_state();
}

void audio_output_set_local_volume(uint8_t vol) {
  if (vol > 100)
    vol = 100;
  local_volume = vol;
}

uint8_t audio_output_get_local_volume(void) { return local_volume; }

void audio_output_toggle_local_mute(void) {
  local_muted = !local_muted;
  update_mute_state();
}

uint8_t audio_output_is_local_muted(void) { return local_muted; }

//--------------------------------------------------------------------+
// HAL I2S DMA Callbacks
//--------------------------------------------------------------------+

// Called when first half of buffer has been sent
void HAL_I2S_TxHalfCpltCallback(I2S_HandleTypeDef *hi2s) {
  if (hi2s->Instance == SPI1) {
    // First half has been transmitted, mark it for refill
    first_half_needs_fill = 1;
  }
}

// Called when second half of buffer has been sent (full transfer complete)
void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *hi2s) {
  if (hi2s->Instance == SPI1) {
    // Second half has been transmitted, mark it for refill
    second_half_needs_fill = 1;
  }
}
