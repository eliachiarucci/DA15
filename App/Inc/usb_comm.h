// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * USB CDC Communication Protocol for EQ Profile Management
 *
 * Binary protocol over CDC virtual serial port:
 *   Request:  [CMD:1] [LEN:2 LE] [PAYLOAD:LEN] [CRC8:1]
 *   Response: [CMD|0x80:1] [LEN:2 LE] [STATUS:1] [PAYLOAD:LEN-1] [CRC8:1]
 */

#ifndef USB_COMM_H
#define USB_COMM_H

#include <stdint.h>

// Protocol commands
#define CMD_GET_DEVICE_INFO   0x01
#define CMD_GET_PROFILE_LIST  0x02
#define CMD_GET_PROFILE       0x03
#define CMD_SET_PROFILE       0x04
#define CMD_DELETE_PROFILE    0x05
#define CMD_SET_ACTIVE        0x06
#define CMD_SAVE_TO_FLASH     0x07
#define CMD_ENTER_DFU         0x08

// Response status codes
#define STATUS_OK             0x00
#define STATUS_ERR_INVALID_CMD    0x01
#define STATUS_ERR_INVALID_PARAM  0x02
#define STATUS_ERR_FLASH          0x03

// Firmware version
#define FW_VERSION_MAJOR  2
#define FW_VERSION_MINOR  0
#define FW_VERSION_PATCH  0

// Initialize CDC communication (call after tusb_init)
void usb_comm_init(void);

// Process incoming CDC data (call from main loop)
void usb_comm_task(void);

#endif // USB_COMM_H
