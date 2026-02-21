// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * USB CDC Communication Protocol for EQ Profile Management
 *
 * State machine assembles frames from CDC byte stream, dispatches
 * commands to the eq_profile module, and sends responses.
 */

#include "usb_comm.h"
#include "eq_profile.h"
#include "tusb.h"
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
    uint8_t resp[6];
    resp[0] = FW_VERSION_MAJOR;
    resp[1] = FW_VERSION_MINOR;
    resp[2] = FW_VERSION_PATCH;
    resp[3] = EQ_MAX_PROFILES;
    resp[4] = EQ_MAX_FILTERS;
    resp[5] = eq_profile_get_active();
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
    send_ok(CMD_SET_ACTIVE, NULL, 0);
}

static void handle_save_to_flash(void) {
    if (!eq_profile_save_to_flash()) {
        send_error(CMD_SAVE_TO_FLASH, STATUS_ERR_FLASH);
        return;
    }

    send_ok(CMD_SAVE_TO_FLASH, NULL, 0);
}

// ---------------------------------------------------------------------------
// Frame dispatch
// ---------------------------------------------------------------------------
static void dispatch_command(void) {
    switch (rx_cmd) {
    case CMD_GET_DEVICE_INFO:   handle_get_device_info();  break;
    case CMD_GET_PROFILE_LIST:  handle_get_profile_list(); break;
    case CMD_GET_PROFILE:       handle_get_profile();      break;
    case CMD_SET_PROFILE:       handle_set_profile();      break;
    case CMD_DELETE_PROFILE:    handle_delete_profile();    break;
    case CMD_SET_ACTIVE:        handle_set_active();       break;
    case CMD_SAVE_TO_FLASH:     handle_save_to_flash();    break;
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
