// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * Firmware version — single source of truth.
 *
 * Consumed by:
 *   - usb_descriptors.c  (bcdDevice, shown by the host OS)
 *   - usb_comm.h/.c      (CMD_GET_DEVICE_INFO response)
 *   - app.c              (RTT boot banner)
 */

#ifndef VERSION_H
#define VERSION_H

#define FW_VERSION_MAJOR 1
#define FW_VERSION_MINOR 1
#define FW_VERSION_PATCH 0

// "1.1.0" — assembled from the numbers above, never edited by hand
#define FW_VERSION_STR_(x) #x
#define FW_VERSION_STR(x) FW_VERSION_STR_(x)
#define FW_VERSION_STRING          \
    FW_VERSION_STR(FW_VERSION_MAJOR) "." FW_VERSION_STR(FW_VERSION_MINOR) \
    "." FW_VERSION_STR(FW_VERSION_PATCH)

// USB bcdDevice is binary-coded decimal 0xJJMN (JJ=major, M=minor, N=patch)
#define FW_VERSION_BCD                                        \
    ((((FW_VERSION_MAJOR) / 10) << 12) |                      \
     (((FW_VERSION_MAJOR) % 10) << 8)  |                      \
     ((FW_VERSION_MINOR) << 4)         |                      \
     (FW_VERSION_PATCH))

_Static_assert(FW_VERSION_MAJOR <= 99, "bcdDevice major is 2 BCD digits");
_Static_assert(FW_VERSION_MINOR <= 9, "bcdDevice minor is 1 BCD digit");
_Static_assert(FW_VERSION_PATCH <= 9, "bcdDevice patch is 1 BCD digit");

#endif // VERSION_H
