// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * Persistent Settings Storage
 * Stores user settings in the last flash page (0x0801F800, 2KB).
 * Uses sequential record writing for basic wear leveling.
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t local_volume;    // 0-100
    uint8_t local_muted;     // 0 or 1
    int8_t  bass;            // -6 to +6
    int8_t  treble;          // -6 to +6
    uint8_t brightness;      // 0=LOW, 1=MID, 2=HIGH
    uint8_t display_timeout; // 0=Never, 1=2s, 2=5s, 3=10s
    uint8_t active_profile;  // 0-9=profile, 0xFF=OFF (legacy bass/treble)
} settings_t;

// Load settings from flash. Returns false if no valid settings found.
bool settings_load(settings_t *out);

// Save settings to flash. Returns false on flash error.
bool settings_save(const settings_t *s);

#endif // SETTINGS_H
