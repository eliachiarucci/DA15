// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * USB CDC Communication Protocol for EQ Profile Management
 *
 * State machine assembles frames from CDC byte stream, dispatches
 * commands to the eq_profile module, and sends responses.
 */

#include "usb_comm.h"
#include "app.h"
#include "audio_output.h"
#include "eq_profile.h"
#include "settings.h"
#include "usb_descriptors.h"
#include "stm32h5xx_hal.h"
#include "tusb.h"
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Frame limits
// ---------------------------------------------------------------------------
#define MAX_PAYLOAD_SIZE  512  // Largest payload: SET_PROFILE (~384 bytes)
#define FRAME_HEADER_SIZE 3   // CMD + LEN(2)
#define FRAME_CRC_SIZE    1

// ---------------------------------------------------------------------------
// CRC8 (polynomial 0x07, same as SMBus)
// ---------------------------------------------------------------------------
static uint8_t crc8_update(uint8_t crc, const uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x80)
                crc = (crc << 1) ^ 0x07;
            else
                crc <<= 1;
        }
    }
    return crc;
}

static uint8_t crc8(const uint8_t *data, uint32_t len) {
    return crc8_update(0x00, data, len);
}

// ---------------------------------------------------------------------------
// Frame assembly state machine
// ---------------------------------------------------------------------------
typedef enum {
    RX_WAIT_CMD,
    RX_WAIT_LEN_LO,
    RX_WAIT_LEN_HI,
    RX_WAIT_PAYLOAD,
    RX_WAIT_CRC,
} rx_state_t;

static rx_state_t rx_state;
static uint8_t rx_cmd;
static uint16_t rx_len;
static uint16_t rx_pos;
static uint8_t rx_buf[MAX_PAYLOAD_SIZE];

// TX buffer (reuse for responses)
static uint8_t tx_buf[FRAME_HEADER_SIZE + 1 + MAX_PAYLOAD_SIZE + FRAME_CRC_SIZE];

// Deferred response for async operations (e.g. SAVE_TO_FLASH)
static uint8_t deferred_cmd = 0;

// ---------------------------------------------------------------------------
// Response helpers
// ---------------------------------------------------------------------------
static void send_response(uint8_t cmd, uint8_t status,
                          const uint8_t *payload, uint16_t payload_len) {
    uint16_t total_payload = 1 + payload_len; // status + payload
    tx_buf[0] = cmd | 0x80;
    tx_buf[1] = (uint8_t)(total_payload & 0xFF);
    tx_buf[2] = (uint8_t)(total_payload >> 8);
    tx_buf[3] = status;
    if (payload_len > 0 && payload != NULL)
        memcpy(&tx_buf[4], payload, payload_len);

    uint16_t frame_len = FRAME_HEADER_SIZE + total_payload;
    tx_buf[frame_len] = crc8(tx_buf, frame_len);
    frame_len += FRAME_CRC_SIZE;

    tud_cdc_write(tx_buf, frame_len);
    tud_cdc_write_flush();
}

static void send_ok(uint8_t cmd, const uint8_t *payload, uint16_t len) {
    send_response(cmd, STATUS_OK, payload, len);
}

static void send_error(uint8_t cmd, uint8_t status) {
    send_response(cmd, status, NULL, 0);
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------
static void handle_get_device_info(void) {
    uint8_t resp[9];
    resp[0] = HW_MODEL;
    resp[1] = HW_VERSION_MAJOR;
    resp[2] = HW_VERSION_MINOR;
    resp[3] = FW_VERSION_MAJOR;
    resp[4] = FW_VERSION_MINOR;
    resp[5] = FW_VERSION_PATCH;
    resp[6] = EQ_MAX_PROFILES;
    resp[7] = EQ_MAX_FILTERS;
    resp[8] = eq_profile_get_active();
    send_ok(CMD_GET_DEVICE_INFO, resp, sizeof(resp));
}

static void handle_get_profile_list(void) {
    // Response: [count:1] then [id:1, name:16]... for each non-empty profile
    uint8_t resp[1 + EQ_MAX_PROFILES * 17]; // worst case
    uint8_t count = 0;
    uint8_t pos = 1; // skip count byte

    for (uint8_t i = 0; i < EQ_MAX_PROFILES; i++) {
        const eq_profile_t *p = eq_profile_get(i);
        if (p != NULL) {
            resp[pos++] = i;
            memcpy(&resp[pos], p->name, EQ_PROFILE_NAME_LEN);
            pos += EQ_PROFILE_NAME_LEN;
            count++;
        }
    }
    resp[0] = count;
    send_ok(CMD_GET_PROFILE_LIST, resp, pos);
}

static void handle_get_active(void) {
    uint8_t id = eq_profile_get_active();
    send_ok(CMD_GET_ACTIVE, &id, 1);
}

static void handle_get_profile(void) {
    if (rx_len < 1) {
        send_error(CMD_GET_PROFILE, STATUS_ERR_INVALID_PARAM);
        return;
    }

    uint8_t id = rx_buf[0];
    const eq_profile_t *p = eq_profile_get(id);
    if (p == NULL) {
        send_error(CMD_GET_PROFILE, STATUS_ERR_INVALID_PARAM);
        return;
    }

    send_ok(CMD_GET_PROFILE, (const uint8_t *)p, sizeof(eq_profile_t));
}

static void handle_set_profile(void) {
    if (rx_len < 1 + sizeof(eq_profile_t)) {
        send_error(CMD_SET_PROFILE, STATUS_ERR_INVALID_PARAM);
        return;
    }

    uint8_t id = rx_buf[0];
    eq_profile_t profile;
    memcpy(&profile, &rx_buf[1], sizeof(eq_profile_t));

    if (!eq_profile_set(id, &profile)) {
        send_error(CMD_SET_PROFILE, STATUS_ERR_INVALID_PARAM);
        return;
    }

    send_ok(CMD_SET_PROFILE, NULL, 0);
}

static void handle_delete_profile(void) {
    if (rx_len < 1) {
        send_error(CMD_DELETE_PROFILE, STATUS_ERR_INVALID_PARAM);
        return;
    }

    uint8_t id = rx_buf[0];
    if (!eq_profile_delete(id)) {
        send_error(CMD_DELETE_PROFILE, STATUS_ERR_INVALID_PARAM);
        return;
    }

    send_ok(CMD_DELETE_PROFILE, NULL, 0);
}

static void handle_set_active(void) {
    if (rx_len < 1) {
        send_error(CMD_SET_ACTIVE, STATUS_ERR_INVALID_PARAM);
        return;
    }

    uint8_t id = rx_buf[0];
    eq_profile_set_active(id);
    app_save_settings();
    send_ok(CMD_SET_ACTIVE, NULL, 0);
}

static void handle_get_manufacturer(void) {
    const char *str = usb_desc_get_manufacturer();
    send_ok(CMD_GET_MANUFACTURER, (const uint8_t *)str, (uint16_t)strlen(str));
}

static void handle_get_product(void) {
    const char *str = usb_desc_get_product();
    send_ok(CMD_GET_PRODUCT, (const uint8_t *)str, (uint16_t)strlen(str));
}

static void handle_set_manufacturer(void) {
    if (rx_len == 0 || rx_len > USB_STRING_MAX_LEN) {
        send_error(CMD_SET_MANUFACTURER, STATUS_ERR_INVALID_PARAM);
        return;
    }
    char str[USB_STRING_MAX_LEN + 1];
    memcpy(str, rx_buf, rx_len);
    str[rx_len] = '\0';
    usb_desc_set_manufacturer(str);
    send_ok(CMD_SET_MANUFACTURER, NULL, 0);
}

static void handle_set_product(void) {
    if (rx_len == 0 || rx_len > USB_STRING_MAX_LEN) {
        send_error(CMD_SET_PRODUCT, STATUS_ERR_INVALID_PARAM);
        return;
    }
    char str[USB_STRING_MAX_LEN + 1];
    memcpy(str, rx_buf, rx_len);
    str[rx_len] = '\0';
    usb_desc_set_product(str);
    send_ok(CMD_SET_PRODUCT, NULL, 0);
}

static void handle_get_audio_itf(void) {
    const char *str = usb_desc_get_audio_itf();
    send_ok(CMD_GET_AUDIO_ITF, (const uint8_t *)str, (uint16_t)strlen(str));
}

static void handle_set_audio_itf(void) {
    if (rx_len == 0 || rx_len > USB_STRING_MAX_LEN) {
        send_error(CMD_SET_AUDIO_ITF, STATUS_ERR_INVALID_PARAM);
        return;
    }
    char str[USB_STRING_MAX_LEN + 1];
    memcpy(str, rx_buf, rx_len);
    str[rx_len] = '\0';
    usb_desc_set_audio_itf(str);
    send_ok(CMD_SET_AUDIO_ITF, NULL, 0);
}

static void handle_enter_dfu(void) {
    send_ok(CMD_ENTER_DFU, NULL, 0);
    tud_cdc_write_flush();
    app_reboot_to_dfu();
}

static void handle_get_dfu_serial(void) {
    // H5 UID_BASE (0x08FFF800) is in user-flash range — requires MPU region
    // with non-cacheable attribute to avoid HardFault when ICACHE is enabled.
    // DFU bootloader serial (same algorithm as F2/F4): 8 hex of (UID0+UID2),
    // then 4 hex of (UID1 >> 16) = 12 uppercase hex chars.
    uint32_t uid0 = HAL_GetUIDw0();
    uint32_t uid1 = HAL_GetUIDw1();
    uint32_t uid2 = HAL_GetUIDw2();

    char serial[13];
    snprintf(serial, sizeof(serial), "%08lX%04lX",
             (unsigned long)(uid0 + uid2), (unsigned long)(uid1 >> 16));
    send_ok(CMD_GET_DFU_SERIAL, (const uint8_t *)serial, 12);
}

static void handle_get_dac(void) {
    uint8_t state = audio_output_get_dac();
    send_ok(CMD_GET_DAC, &state, 1);
}

static void handle_get_amp(void) {
    uint8_t state = audio_output_get_amp();
    send_ok(CMD_GET_AMP, &state, 1);
}

static void handle_set_dac(void) {
    if (rx_len < 1 || rx_buf[0] > 1) {
        send_error(CMD_SET_DAC, STATUS_ERR_INVALID_PARAM);
        return;
    }
    audio_output_set_dac(rx_buf[0]);
    send_ok(CMD_SET_DAC, NULL, 0);
}

static void handle_set_amp(void) {
    if (rx_len < 1 || rx_buf[0] > 1) {
        send_error(CMD_SET_AMP, STATUS_ERR_INVALID_PARAM);
        return;
    }
    audio_output_set_amp(rx_buf[0]);
    send_ok(CMD_SET_AMP, NULL, 0);
}

static void handle_reboot(void) {
    // Persist any pending string changes to flash before resetting
    if (!settings_save_strings(usb_desc_get_manufacturer(),
                               usb_desc_get_product(),
                               usb_desc_get_audio_itf())) {
        send_error(CMD_REBOOT, STATUS_ERR_FLASH);
        return;
    }

    send_ok(CMD_REBOOT, NULL, 0);
    tud_cdc_write_flush();
    uint32_t deadline = HAL_GetTick() + 20;
    while (HAL_GetTick() < deadline)
        tud_task();
    NVIC_SystemReset();
}

static void handle_save_to_flash(void) {
    if (!eq_profile_start_flash_save()) {
        send_error(CMD_SAVE_TO_FLASH, STATUS_ERR_FLASH);
        return;
    }

    // Response deferred — sent from usb_comm_task when flash completes
    deferred_cmd = CMD_SAVE_TO_FLASH;
}

// ---------------------------------------------------------------------------
// Frame dispatch
// ---------------------------------------------------------------------------
static void dispatch_command(void) {
    switch (rx_cmd) {
    case CMD_GET_DEVICE_INFO:   handle_get_device_info();  break;
    case CMD_GET_PROFILE_LIST:  handle_get_profile_list(); break;
    case CMD_GET_ACTIVE:        handle_get_active();       break;
    case CMD_GET_PROFILE:       handle_get_profile();      break;
    case CMD_SET_PROFILE:       handle_set_profile();      break;
    case CMD_DELETE_PROFILE:    handle_delete_profile();    break;
    case CMD_SET_ACTIVE:        handle_set_active();       break;
    case CMD_SAVE_TO_FLASH:     handle_save_to_flash();    break;
    case CMD_GET_MANUFACTURER:  handle_get_manufacturer(); break;
    case CMD_GET_PRODUCT:       handle_get_product();      break;
    case CMD_GET_AUDIO_ITF:     handle_get_audio_itf();    break;
    case CMD_SET_MANUFACTURER:  handle_set_manufacturer(); break;
    case CMD_SET_PRODUCT:       handle_set_product();      break;
    case CMD_SET_AUDIO_ITF:     handle_set_audio_itf();    break;
    case CMD_GET_DAC:           handle_get_dac();          break;
    case CMD_GET_AMP:           handle_get_amp();          break;
    case CMD_SET_DAC:           handle_set_dac();          break;
    case CMD_SET_AMP:           handle_set_amp();          break;
    case CMD_ENTER_DFU:         handle_enter_dfu();        break;
    case CMD_GET_DFU_SERIAL:    handle_get_dfu_serial();   break;
    case CMD_REBOOT:            handle_reboot();           break;
    default:
        send_error(rx_cmd, STATUS_ERR_INVALID_CMD);
        break;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void usb_comm_init(void) {
    rx_state = RX_WAIT_CMD;
    rx_pos = 0;
}

void usb_comm_task(void) {
    // Check for deferred flash save response
    if (deferred_cmd == CMD_SAVE_TO_FLASH) {
        eq_flash_status_t s = eq_profile_flash_status();
        if (s == EQ_FLASH_DONE_OK) {
            send_ok(CMD_SAVE_TO_FLASH, NULL, 0);
            deferred_cmd = 0;
        } else if (s == EQ_FLASH_DONE_ERR) {
            send_error(CMD_SAVE_TO_FLASH, STATUS_ERR_FLASH);
            deferred_cmd = 0;
        }
    }

    if (!tud_cdc_available())
        return;

    while (tud_cdc_available()) {
        uint8_t byte;
        if (tud_cdc_read(&byte, 1) != 1)
            break;

        switch (rx_state) {
        case RX_WAIT_CMD:
            rx_cmd = byte;
            rx_state = RX_WAIT_LEN_LO;
            break;

        case RX_WAIT_LEN_LO:
            rx_len = byte;
            rx_state = RX_WAIT_LEN_HI;
            break;

        case RX_WAIT_LEN_HI:
            rx_len |= (uint16_t)byte << 8;
            rx_pos = 0;
            if (rx_len == 0) {
                rx_state = RX_WAIT_CRC;
            } else if (rx_len > MAX_PAYLOAD_SIZE) {
                // Frame too large, reset
                rx_state = RX_WAIT_CMD;
            } else {
                rx_state = RX_WAIT_PAYLOAD;
            }
            break;

        case RX_WAIT_PAYLOAD:
            rx_buf[rx_pos++] = byte;
            if (rx_pos >= rx_len)
                rx_state = RX_WAIT_CRC;
            break;

        case RX_WAIT_CRC: {
            // CRC8 over header (cmd + len_lo + len_hi) then payload
            uint8_t header[3] = {rx_cmd, (uint8_t)(rx_len & 0xFF),
                                 (uint8_t)(rx_len >> 8)};
            uint8_t expected = crc8_update(0x00, header, 3);
            if (rx_len > 0)
                expected = crc8_update(expected, rx_buf, rx_len);

            if (expected == byte)
                dispatch_command();

            rx_state = RX_WAIT_CMD;
        } break;
        }
    }
}
