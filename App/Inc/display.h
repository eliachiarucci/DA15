// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// Screen / menu types
// ---------------------------------------------------------------------------
typedef enum {
  SCREEN_VOLUME,
  SCREEN_MENU,
  SCREEN_IDLE,
} screen_state_t;

typedef enum {
  MENU_BACK = 0,
  MENU_BASS,
  MENU_TREBLE,
  MENU_BRIGHTNESS,
  MENU_TIMEOUT,
  MENU_DFU,
  MENU_COUNT,
} menu_item_t;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void display_init(uint8_t brightness, uint8_t timeout);

// Rate-limited redraw (call every main-loop iteration)
void display_draw(uint32_t now);

// Display timeout check (call every main-loop iteration)
void display_check_timeout(uint32_t now);

// Menu edit-mode blink (call every main-loop iteration)
void display_blink_tick(uint32_t now);

// Idle dot 1-hour position switch (call every main-loop iteration)
void display_idle_tick(uint32_t now);

// ---------------------------------------------------------------------------
// Screen state
// ---------------------------------------------------------------------------
screen_state_t display_get_screen(void);
void display_set_screen(screen_state_t s);

// Mark the display as needing a full redraw
void display_set_dirty(void);

// Record user activity (resets timeout timer, wakes display if off)
void display_mark_activity(void);

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
uint8_t display_get_menu_cursor(void);
uint8_t display_is_menu_editing(void);
void display_menu_reset(void);
void display_menu_enter_edit(void);
void display_menu_exit_edit(void);
void display_menu_navigate(int8_t delta);

// ---------------------------------------------------------------------------
// Brightness / timeout
// ---------------------------------------------------------------------------
uint8_t display_get_brightness(void);
void display_set_brightness(uint8_t level);

uint8_t display_get_timeout_level(void);
void display_set_timeout_level(uint8_t level);

// ---------------------------------------------------------------------------
// Idle screen
// ---------------------------------------------------------------------------
void display_enter_idle(uint32_t now);

#endif
