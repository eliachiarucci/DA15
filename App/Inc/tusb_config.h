// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * TinyUSB Configuration for STM32H503 USB Audio (UAC1)
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------+
// Board Specific Configuration
//--------------------------------------------------------------------+

// RHPort number used for device
#define BOARD_TUD_RHPORT      0

// RHPort max operational speed
#define BOARD_TUD_MAX_SPEED   OPT_MODE_FULL_SPEED

//--------------------------------------------------------------------+
// Common Configuration
//--------------------------------------------------------------------+

#define CFG_TUSB_MCU          OPT_MCU_STM32H5
#define CFG_TUSB_OS           OPT_OS_NONE

// CFG_TUSB_DEBUG is defined by compiler in DEBUG build
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG        0
#endif

// Enable Device stack
#define CFG_TUD_ENABLED       1

// Default is max speed that hardware controller could support with on-chip PHY
#define CFG_TUD_MAX_SPEED     BOARD_TUD_MAX_SPEED

// Memory alignment for internal buffer
#define CFG_TUSB_MEM_ALIGN    __attribute__ ((aligned(4)))

//--------------------------------------------------------------------+
// Device Configuration
//--------------------------------------------------------------------+

#define CFG_TUD_ENDPOINT0_SIZE    64

//------------- CLASS -------------//
#define CFG_TUD_AUDIO             1
#define CFG_TUD_CDC               1
#define CFG_TUD_MSC               0
#define CFG_TUD_HID               0
#define CFG_TUD_MIDI              0
#define CFG_TUD_VENDOR            0
#define CFG_TUD_DFU_RUNTIME       1

//--------------------------------------------------------------------+
// AUDIO CLASS DRIVER CONFIGURATION
//--------------------------------------------------------------------+

// Audio format: 48kHz, 24-bit stereo (3 bytes per sample over USB)
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX             2
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX     3
#define CFG_TUD_AUDIO_FUNC_1_RESOLUTION_RX             24

// Sample rate (single rate for Full-Speed UAC1)
#define CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE_FS        48000

// Full-Speed endpoint size calculation
// EP size = samples_per_frame * bytes_per_sample * channels
// At 48kHz, Full-Speed: 48 samples/ms * 3 bytes * 2 channels = 288 bytes
// Add 1 sample margin: 49 * 3 * 2 = 294 bytes
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_FS    (49 * CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX * CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX)

// Maximum EP size (Full-Speed only device)
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX   CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_FS

// Software buffer size for endpoint OUT
// AUDIO_FEEDBACK_METHOD_FIFO_COUNT needs buffer size >= 4 * EP size
// 16 packets = ~16ms headroom with feedback (4x minimum)
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ    (16 * CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_FS)

// Enable EP OUT for audio data reception
#define CFG_TUD_AUDIO_ENABLE_EP_OUT                    1

// Enable feedback endpoint for asynchronous mode
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP               1

// No EP IN (this is a speaker, not microphone)
#define CFG_TUD_AUDIO_ENABLE_EP_IN                     0

// No encoding/decoding
#define CFG_TUD_AUDIO_ENABLE_ENCODING                  0
#define CFG_TUD_AUDIO_ENABLE_DECODING                  0

// Type I format only
#define CFG_TUD_AUDIO_ENABLE_TYPE_I_ENCODING           0
#define CFG_TUD_AUDIO_ENABLE_TYPE_I_DECODING           0

// Number of audio functions
#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN                  0  // Calculated in usb_descriptors.h

// Control buffer size
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ               64

//--------------------------------------------------------------------+
// CDC CLASS DRIVER CONFIGURATION
//--------------------------------------------------------------------+

#define CFG_TUD_CDC_RX_BUFSIZE    512
#define CFG_TUD_CDC_TX_BUFSIZE    512

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
