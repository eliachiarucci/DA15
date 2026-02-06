// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * TinyUSB Audio Class Callbacks for UAC1 Speaker
 */

#include "tusb.h"
#include "usb_descriptors.h"
#include "audio_output.h"

//--------------------------------------------------------------------+
// Audio State
//--------------------------------------------------------------------+

// Current states
static uint8_t mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];       // +1 for master channel 0
static int16_t volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1];     // +1 for master channel 0
static uint32_t current_sample_rate = 48000;

// Streaming state
static volatile bool audio_streaming = false;

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

uint32_t usb_audio_get_sample_rate(void) {
    return current_sample_rate;
}

bool usb_audio_is_streaming(void) {
    return audio_streaming;
}

uint16_t usb_audio_read(uint8_t* buffer, uint16_t max_length) {
    return tud_audio_read(buffer, max_length);
}

uint16_t usb_audio_available(void) {
    return tud_audio_available();
}

int8_t usb_audio_get_volume(void) {
    // Return master volume (channel 0), clamped to int8_t range
    int16_t vol = volume[0];
    if (vol < -90) vol = -90;
    if (vol > 0) vol = 0;
    return (int8_t)vol;
}

int8_t usb_audio_get_volume_0_100(void) {
    int16_t vol = volume[0];
    if (vol <= -90) return 0;
    if (vol >= 0) return 100;
    return (int8_t)((vol + 90) * 100 / 90);
}

bool usb_audio_is_muted(void) {
    // Return true if master channel is muted
    return mute[0] != 0;
}

//--------------------------------------------------------------------+
// UAC1 Helper Functions
//--------------------------------------------------------------------+

static bool audio10_set_req_ep(tusb_control_request_t const* p_request, uint8_t* pBuff) {
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);

    switch (ctrlSel) {
        case AUDIO10_EP_CTRL_SAMPLING_FREQ:
            if (p_request->bRequest == AUDIO10_CS_REQ_SET_CUR) {
                // Request uses 3 bytes
                TU_VERIFY(p_request->wLength == 3);

                current_sample_rate = tu_unaligned_read32(pBuff) & 0x00FFFFFF;
                return true;
            }
            break;

        default:
            return false;
    }

    return false;
}

static bool audio10_get_req_ep(uint8_t rhport, tusb_control_request_t const* p_request) {
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);

    switch (ctrlSel) {
        case AUDIO10_EP_CTRL_SAMPLING_FREQ:
            if (p_request->bRequest == AUDIO10_CS_REQ_GET_CUR) {
                uint8_t freq[3];
                freq[0] = (uint8_t)(current_sample_rate & 0xFF);
                freq[1] = (uint8_t)((current_sample_rate >> 8) & 0xFF);
                freq[2] = (uint8_t)((current_sample_rate >> 16) & 0xFF);
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, freq, sizeof(freq));
            }
            break;

        default:
            return false;
    }

    return false;
}

static bool audio10_set_req_entity(tusb_control_request_t const* p_request, uint8_t* pBuff) {
    uint8_t channelNum = TU_U16_LOW(p_request->wValue);
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
    uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

    // If request is for our feature unit
    if (entityID == UAC1_ENTITY_FEATURE_UNIT) {
        switch (ctrlSel) {
            case AUDIO10_FU_CTRL_MUTE:
                if (p_request->bRequest == AUDIO10_CS_REQ_SET_CUR) {
                    TU_VERIFY(p_request->wLength == 1);
                    mute[channelNum] = pBuff[0];

                    // Apply mute to DAC
                    audio_output_set_mute(mute[0] || mute[1] || mute[2]);
                    return true;
                }
                break;

            case AUDIO10_FU_CTRL_VOLUME:
                if (p_request->bRequest == AUDIO10_CS_REQ_SET_CUR) {
                    TU_VERIFY(p_request->wLength == 2);
                    volume[channelNum] = (int16_t)tu_unaligned_read16(pBuff) / 256;
                    return true;
                }
                break;

            default:
                return false;
        }
    }

    return false;
}

static bool audio10_get_req_entity(uint8_t rhport, tusb_control_request_t const* p_request) {
    uint8_t channelNum = TU_U16_LOW(p_request->wValue);
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
    uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

    // If request is for our feature unit
    if (entityID == UAC1_ENTITY_FEATURE_UNIT) {
        switch (ctrlSel) {
            case AUDIO10_FU_CTRL_MUTE:
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &mute[channelNum], 1);

            case AUDIO10_FU_CTRL_VOLUME:
                switch (p_request->bRequest) {
                    case AUDIO10_CS_REQ_GET_CUR: {
                        int16_t vol = volume[channelNum] * 256;  // Convert to 1/256 dB units
                        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &vol, sizeof(vol));
                    }

                    case AUDIO10_CS_REQ_GET_MIN: {
                        int16_t min = -90 * 256;  // -90 dB in 1/256 dB units
                        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &min, sizeof(min));
                    }

                    case AUDIO10_CS_REQ_GET_MAX: {
                        int16_t max = 0 * 256;  // 0 dB in 1/256 dB units
                        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &max, sizeof(max));
                    }

                    case AUDIO10_CS_REQ_GET_RES: {
                        int16_t res = 256;  // 1 dB resolution
                        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &res, sizeof(res));
                    }

                    default:
                        return false;
                }
                break;

            default:
                return false;
        }
    }

    return false;
}

//--------------------------------------------------------------------+
// TinyUSB Audio Callbacks
//--------------------------------------------------------------------+

// Invoked when audio class specific set request received for an EP
bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const* p_request, uint8_t* pBuff) {
    (void) rhport;
    return audio10_set_req_ep(p_request, pBuff);
}

// Invoked when audio class specific get request received for an EP
bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const* p_request) {
    return audio10_get_req_ep(rhport, p_request);
}

// Invoked when audio class specific set request received for an entity
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const* p_request, uint8_t* buf) {
    (void) rhport;
    return audio10_set_req_entity(p_request, buf);
}

// Invoked when audio class specific get request received for an entity
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const* p_request) {
    return audio10_get_req_entity(rhport, p_request);
}

// Invoked when interface is set
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const* p_request) {
    (void) rhport;
    uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
    uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));

    if (itf == ITF_NUM_AUDIO_STREAMING) {
        if (alt != 0) {
            // Start streaming
            audio_streaming = true;
            audio_output_start_streaming();
        }
    }

    return true;
}

// Invoked when interface is closed (alt setting 0)
bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const* p_request) {
    (void) rhport;
    uint8_t const itf = tu_u16_low(tu_le16toh(p_request->wIndex));
    uint8_t const alt = tu_u16_low(tu_le16toh(p_request->wValue));

    if (itf == ITF_NUM_AUDIO_STREAMING && alt == 0) {
        // Stop streaming
        audio_streaming = false;
        audio_output_stop_streaming();
    }

    return true;
}

// Invoked when feedback parameters are requested
void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t* feedback_param) {
    (void) func_id;
    (void) alt_itf;

    // Use FIFO count method for feedback - TinyUSB calculates feedback based on FIFO fill level
    feedback_param->method = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
    feedback_param->sample_freq = current_sample_rate;

    // Set explicit threshold at half the FIFO size for stable feedback
    // This gives the feedback algorithm a clear target to maintain
    feedback_param->fifo_count.fifo_threshold = CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 2;
}

//--------------------------------------------------------------------+
// Device Callbacks
//--------------------------------------------------------------------+

// Invoked when device is mounted
void tud_mount_cb(void) {
    // Device connected
}

// Invoked when device is unmounted
void tud_umount_cb(void) {
    audio_streaming = false;
    audio_output_stop_streaming();
}

// Invoked when usb bus is suspended
void tud_suspend_cb(bool remote_wakeup_en) {
    (void) remote_wakeup_en;
    audio_streaming = false;
    audio_output_stop_streaming();
}

// Invoked when usb bus is resumed
void tud_resume_cb(void) {
    // Resume handled by host sending new set interface
}
