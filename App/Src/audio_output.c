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
#include "eq_profile.h"
#include "main.h"
#include "sh1106.h"
#include "stm32h5xx_hal.h"
#include "tusb.h"
#include "usb_audio.h"
#include <string.h>


// Debug: set to 1 to enable RTT logging
#define AUDIO_DEBUG 0

// Swap L/R channels (Necessary for DA15)
#ifndef NO_SWAP_CHANNELS
#define SWAP_CHANNELS 1
#else
#define SWAP_CHANNELS 0
#endif

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

// PCM5102A anti-pop: 1 LSB DC offset prevents the DAC's Zero Data Detect
// from engaging analog mute during silence. Inaudible (AC-coupled output).
// 24-bit sample value 1, left-justified in 32-bit I2S word = 0x00000100.
#define SILENCE_DC_OFFSET 1
#define SILENCE_I2S_WORD  ((uint32_t)SILENCE_DC_OFFSET << 8)

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

// Volume ramping: smooths transitions to prevent clicks
static uint32_t prev_volume_scale = 0;

#if AUDIO_DEBUG
// Debug counters
static volatile uint32_t underrun_count = 0;
static volatile uint32_t partial_fill_count = 0;
static volatile uint32_t full_fill_count = 0;
static volatile uint32_t last_report_tick = 0;

// FIFO level tracking (sampled at each DMA half-complete)
static volatile int16_t fifo_min_delta = 0;   // min deviation from midpoint
static volatile int16_t fifo_max_delta = 0;   // max deviation from midpoint
static volatile int32_t fifo_sum_delta = 0;    // sum for averaging
static volatile uint16_t fifo_sample_count = 0; // number of samples
#define FIFO_MIDPOINT (PREBUFFER_THRESHOLD / 2)

static void fifo_track_level(void) {
  int16_t delta = (int16_t)usb_audio_available() - FIFO_MIDPOINT;
  if (fifo_sample_count == 0) {
    fifo_min_delta = delta;
    fifo_max_delta = delta;
  } else {
    if (delta < fifo_min_delta) fifo_min_delta = delta;
    if (delta > fifo_max_delta) fifo_max_delta = delta;
  }
  fifo_sum_delta += delta;
  fifo_sample_count++;
}
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

uint8_t audio_output_get_dac(void) {
  return HAL_GPIO_ReadPin(DAC_MUTE_GPIO_Port, DAC_MUTE_Pin) == GPIO_PIN_SET ? 1 : 0;
}

uint8_t audio_output_get_amp(void) {
  return HAL_GPIO_ReadPin(AMP_EN_GPIO_Port, AMP_EN_Pin) == GPIO_PIN_SET ? 1 : 0;
}

void audio_output_set_dac(uint8_t enable) {
  if (enable)
    unmute_dac();
  else
    mute_dac();
}

void audio_output_set_amp(uint8_t enable) {
  if (enable)
    enable_amplifier();
  else
    disable_amplifier();
}

//--------------------------------------------------------------------+
// Helper Functions
//--------------------------------------------------------------------+

// Volume lookup table: proper dB-to-linear conversion
// Maps USB volume (index 0 = -90dB, index 90 = 0dB) to linear scale (0-65536)
// Formula: round(65536 * 10^((i-90)/20)), min 1 for non-mute
static const uint32_t volume_table[91] = {
        0,     2,     3,     3,     3,     4,     4,     5,     5,     6,  // -90..-81 dB
        7,     7,     8,     9,    10,    12,    13,    15,    16,    18,  // -80..-71 dB
       21,    23,    26,    29,    33,    37,    41,    46,    52,    58,  // -70..-61 dB
       66,    74,    83,    93,   104,   117,   131,   147,   165,   185,  // -60..-51 dB
      207,   233,   261,   293,   328,   369,   414,   464,   521,   584,  // -50..-41 dB
      655,   735,   825,   926,  1039,  1165,  1308,  1467,  1646,  1847,  // -40..-31 dB
     2072,  2325,  2609,  2927,  3285,  3685,  4135,  4640,  5206,  5841,  // -30..-21 dB
     6554,  7353,  8250,  9257, 10387, 11654, 13076, 14672, 16462, 18471,  // -20..-11 dB
    20724, 23253, 26090, 29274, 32846, 36854, 41350, 46396, 52057, 58409,  // -10.. -1 dB
    65536,                                                                  //   0 dB = unity
};

// Fill I2S buffer with DC-offset silence (prevents PCM5102A zero-detect mute)
static void fill_with_silence(uint16_t *buffer, uint16_t frame_count) {
  uint32_t *buf32 = (uint32_t *)buffer;
  for (uint16_t i = 0; i < frame_count; i++) {
    buf32[i * 2] = SILENCE_I2S_WORD;
    buf32[i * 2 + 1] = SILENCE_I2S_WORD;
  }
}

// Fill I2S buffer with held last sample (less audible than silence on underrun)
static void fill_with_hold(uint16_t *buffer, uint16_t frame_count) {
  uint32_t *buf32 = (uint32_t *)buffer;
  uint32_t l_val = (uint32_t)last_sample_left << 8;
  uint32_t r_val = (uint32_t)last_sample_right << 8;

  for (uint16_t i = 0; i < frame_count; i++) {
    buf32[i * 2] = l_val;
    buf32[i * 2 + 1] = r_val;
  }
}

#if !NO_POWER_SCALING
// Power-based pre-scaling factors (0-256 scale)
// 0 = 500mA:  -4dB = 10^(-4/20) = 0.631 = 161/256
// 1 = 1500mA: -2dB = 10^(-2/20) = 0.794 = 203/256
// 2 = 3000mA:  0dB = 1.0         = 256/256
static const uint16_t power_scale_table[3] = {161, 203, 256};
#endif

// Get volume scale for audio processing (0-65536, 65536 = unity)
// Combines: USB host volume × power pre-scaling × local pre-attenuation
static uint32_t get_volume_scale(void) {
  if (local_muted || usb_muted)
    return 0;

  int8_t vol_db = usb_audio_get_volume();
  if (vol_db < -90)
    vol_db = -90;
  if (vol_db > 0)
    vol_db = 0;

  uint32_t scale = volume_table[vol_db + 90];

#if !NO_POWER_SCALING
  uint8_t power_level = app_get_power_level();
  if (power_level > 2)
    power_level = 2;

  // Apply power-based pre-scaling: (scale * power_scale) / 256
  scale = (scale * power_scale_table[power_level]) >> 8;
#endif

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

  // EQ processing (operates on 24-bit values in int32_t)
  // Volume is applied separately below with per-sample ramping to prevent clicks
  uint32_t cur_vol = get_volume_scale();
  if (eq_profile_get_active() != EQ_PROFILE_OFF)
    eq_profile_process(proc, sample_count, 65536);
  else
    audio_eq_process(proc, sample_count, 65536);

  // Per-sample volume ramping: linearly interpolate from prev to current
  // over the buffer to avoid step discontinuities (clicks) on volume changes.
  // Uses fixed-point increment scaled by 1/sample_count.
  if (cur_vol != prev_volume_scale || cur_vol < 65536) {
    uint32_t v0 = prev_volume_scale;
    int32_t delta = (int32_t)cur_vol - (int32_t)v0;
    for (uint16_t i = 0; i < sample_count; i++) {
      uint32_t v = v0 + (int32_t)(((int64_t)delta * i) / sample_count);
      proc[i] = (int32_t)(((int64_t)proc[i] * v) >> 16);
    }
  }
  prev_volume_scale = cur_vol;

  // Save last samples before packing (pack overwrites in-place)
  if (sample_count >= 2) {
    last_sample_left = proc[sample_count - 2] ? proc[sample_count - 2] : SILENCE_DC_OFFSET;
    last_sample_right = proc[sample_count - 1] ? proc[sample_count - 1] : SILENCE_DC_OFFSET;
  }

  // Pack int32_t (24-bit) to uint32_t for word-mode DMA (in-place,
  // forward-safe: proc[i] and out32[i] share the same address at offset i*4)
  // Add DC offset to prevent PCM5102A zero-detect from engaging analog mute
  uint32_t *out32 = (uint32_t *)i2s_dest;
  for (uint16_t i = 0; i < sample_count; i++) {
    int32_t s = proc[i];
    if (s == 0) s = SILENCE_DC_OFFSET;
    out32[i] = (uint32_t)s << 8;
  }

  return stereo_frames;
}

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

void audio_output_init(void) {
  SEGGER_RTT_printf(0, "[audio] init start\n");

  // Initialize EQ
  audio_eq_init();

  // Fill buffer with DC-offset silence (prevents PCM5102A zero-detect pop)
  fill_with_silence(i2s_buffer, STEREO_FRAMES_PER_HALF * 2);

  last_sample_left = SILENCE_DC_OFFSET;
  last_sample_right = SILENCE_DC_OFFSET;

  // Anti-pop sequence:
  // DAC mute is hi-Z (not grounded), so we must unmute with I2S silence
  // first to give the amp a defined, clean input before enabling it.
  mute_dac();
  disable_amplifier();

  // Start I2S DMA with DC-offset silence
  SEGGER_RTT_printf(0, "[audio] I2S DMA start (buf=%p, size=%d)...\n",
                    i2s_buffer, I2S_DMA_SIZE);
  HAL_StatusTypeDef rc = HAL_I2S_Transmit_DMA(&hi2s1, i2s_buffer, I2S_DMA_SIZE);
  SEGGER_RTT_printf(0, "[audio] I2S DMA result: %d\n", rc);
  dma_running = 1;

  // Unmute DAC — now outputting DC-offset silence via I2S
  unmute_dac();
  SEGGER_RTT_printf(0, "[audio] DAC unmuted, waiting 500ms...\n");

  // Let DAC output settle, then enable amplifier into a stable signal
  HAL_Delay(500);
  enable_amplifier();
  SEGGER_RTT_printf(0, "[audio] amp enabled, init done\n");
}

void audio_output_start_streaming(void) {
  if (streaming) {
    return;
  }

  streaming = 1;
  prebuffering = 1;

  audio_eq_reset_state();
  eq_profile_reset_state();

  last_sample_left = SILENCE_DC_OFFSET;
  last_sample_right = SILENCE_DC_OFFSET;

  // Clear stale callback flags from idle period
  first_half_needs_fill = 0;
  second_half_needs_fill = 0;
}

void audio_output_stop_streaming(void) {
  streaming = 0;
  prebuffering = 0;

  last_sample_left = SILENCE_DC_OFFSET;
  last_sample_right = SILENCE_DC_OFFSET;

  first_half_needs_fill = 0;
  second_half_needs_fill = 0;
}

void audio_output_task(void) {
  if (!streaming) {
    // Not streaming — fill completed halves with DC-offset silence so DMA
    // doesn't loop stale audio. Safe: we only write the half DMA just finished.
    if (first_half_needs_fill) {
      fill_with_silence(&i2s_buffer[0], STEREO_FRAMES_PER_HALF);
      first_half_needs_fill = 0;
    }
    if (second_half_needs_fill) {
      fill_with_silence(&i2s_buffer[I2S_HALFWORDS_PER_HALF],
                        STEREO_FRAMES_PER_HALF);
      second_half_needs_fill = 0;
    }
    return;
  }

  // Prebuffering phase: fill with DC-offset silence while waiting for data
  if (prebuffering) {
    if (first_half_needs_fill) {
      fill_with_silence(&i2s_buffer[0], STEREO_FRAMES_PER_HALF);
      first_half_needs_fill = 0;
    }
    if (second_half_needs_fill) {
      fill_with_silence(&i2s_buffer[I2S_HALFWORDS_PER_HALF],
                        STEREO_FRAMES_PER_HALF);
      second_half_needs_fill = 0;
    }

    uint16_t available = usb_audio_available();
    if (available >= PREBUFFER_THRESHOLD) {
      prebuffering = 0;
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
    uint16_t fifo_now = usb_audio_available();
    int16_t avg_delta = 0;
    if (fifo_sample_count > 0)
      avg_delta = (int16_t)(fifo_sum_delta / fifo_sample_count);

    SEGGER_RTT_printf(0,
        "FIFO: now=%d mid=%d | delta min=%d avg=%d max=%d | "
        "fills=%lu partial=%lu under=%lu\n",
        fifo_now, FIFO_MIDPOINT,
        fifo_min_delta, avg_delta, fifo_max_delta,
        full_fill_count, partial_fill_count, underrun_count);

    // Reset counters
    full_fill_count = 0;
    partial_fill_count = 0;
    underrun_count = 0;
    fifo_min_delta = 0;
    fifo_max_delta = 0;
    fifo_sum_delta = 0;
    fifo_sample_count = 0;
    last_report_tick = now;
  }
#endif
}

static void update_mute_state(void) {
  // Only local mute uses hardware DAC mute (user-initiated, accepts the pop).
  // USB mute is handled digitally via get_volume_scale() to avoid PCM5102A
  // zero-detect pop on every host mute/unmute toggle.
  if (local_muted) {
    mute_dac();
  } else if (dma_running) {
    unmute_dac();
  }
}

void audio_output_set_mute(uint8_t mute) {
  usb_muted = mute;
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
    first_half_needs_fill = 1;
#if AUDIO_DEBUG
    fifo_track_level();
#endif
  }
}

// Called when second half of buffer has been sent (full transfer complete)
void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef *hi2s) {
  if (hi2s->Instance == SPI1) {
    second_half_needs_fill = 1;
#if AUDIO_DEBUG
    fifo_track_level();
#endif
  }
}
