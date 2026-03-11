// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * Display / UI module
 * Owns all screen-drawing, menu state, brightness, timeout, and idle-dot logic.
 */

#include "display.h"
#include "app.h"
#include "audio_eq.h"
#include "audio_output.h"
#include "eq_profile.h"
#include "sh1106.h"
#include "stm32h5xx_hal.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Screen state
// ---------------------------------------------------------------------------
static screen_state_t screen_state = SCREEN_VOLUME;
static uint8_t menu_cursor = 0;
static uint8_t menu_editing = 0;
static uint32_t menu_blink_tick = 0;
static uint8_t menu_blink_on = 1;

// ---------------------------------------------------------------------------
// Menu layout
// ---------------------------------------------------------------------------
#define MENU_Y_START 2
#define MENU_ROW_H 12
#define MENU_VISIBLE ((SH1106_HEIGHT - MENU_Y_START) / MENU_ROW_H)

static uint8_t menu_scroll = 0;

static const char *menu_labels[] = {
    "< BACK", "EQ PROFILE", "BASS", "TREBLE", "BRIGHTNESS", "DISP. TIMEOUT",
    "DFU UPDATE"};

// Returns true if the menu item should be shown
static uint8_t is_menu_item_visible(uint8_t item) {
  if (item == MENU_BASS || item == MENU_TREBLE)
    return eq_profile_get_active() == EQ_PROFILE_OFF;
  return 1;
}

// Build list of currently visible menu items
static uint8_t visible_items[MENU_COUNT];
static uint8_t visible_count;

static void rebuild_visible_items(void) {
  visible_count = 0;
  for (uint8_t i = 0; i < MENU_COUNT; i++) {
    if (is_menu_item_visible(i))
      visible_items[visible_count++] = i;
  }
}

// Map menu_cursor (actual item) to visible row index, or -1
static int8_t cursor_to_visible_row(void) {
  for (uint8_t i = 0; i < visible_count; i++) {
    if (visible_items[i] == menu_cursor)
      return (int8_t)i;
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Brightness
// ---------------------------------------------------------------------------
static const uint8_t brightness_hw[] = {10, 80, 200};
static const char *brightness_names[] = {"LOW", "MID", "HIGH"};
static uint8_t brightness_level = 1;

// ---------------------------------------------------------------------------
// Display timeout
// ---------------------------------------------------------------------------
static const uint32_t timeout_ms[] = {0, 5000, 10000, 30000};
static const char *timeout_names[] = {"NEVER", "5s", "10s", "30s"};
static uint8_t timeout_level = 0;
static uint32_t last_activity_tick = 0;
static uint8_t display_is_off = 0;

#define MENU_TIMEOUT_MS 60000

// ---------------------------------------------------------------------------
// Display refresh
// ---------------------------------------------------------------------------
static uint8_t display_dirty = 1;
static uint32_t display_last_tick = 0;
#define DISPLAY_MIN_INTERVAL_MS 33

// ---------------------------------------------------------------------------
// Idle dot (OLED burn-in protection)
// ---------------------------------------------------------------------------
#define IDLE_DOT_SIZE 3
#define IDLE_DOT_PAD 6
#define IDLE_DOT_X0 IDLE_DOT_PAD
#define IDLE_DOT_X1 (IDLE_DOT_PAD + IDLE_DOT_SIZE + 1)
#define IDLE_DOT_Y IDLE_DOT_PAD
#define IDLE_DOT_SWITCH_MS (3600UL * 1000UL)

static uint8_t idle_dot_pos = 0;
static uint32_t idle_dot_tick = 0;

// ---------------------------------------------------------------------------
// Drawing helpers (static)
// ---------------------------------------------------------------------------
static void draw_volume_screen(void) {
  sh1106_clear();

  const char *power_str = "500mA";
  uint8_t pl = app_get_power_level();
  if (pl == 1)
    power_str = "1.5A";
  if (pl == 2)
    power_str = "3A";
  char buf[22];
  snprintf(buf, sizeof(buf), "USB: %s", power_str);
  sh1106_set_font_scale(1);
  sh1106_set_cursor(6, 6);
  sh1106_write_string(buf);

  char vol_buf[22];
  if (audio_output_is_local_muted()) {
    snprintf(vol_buf, sizeof(vol_buf), "MUTE");
  } else {
    snprintf(vol_buf, sizeof(vol_buf), "%d", audio_output_get_local_volume());
  }
  uint8_t len = (uint8_t)strlen(vol_buf);
  uint8_t text_w = (len * 6 - 1) * 4;
  uint8_t cx = (SH1106_WIDTH - text_w) / 2;
  sh1106_set_font_scale(4);
  sh1106_set_cursor(cx, 26);
  sh1106_write_string(vol_buf);

  sh1106_update();
}

static void get_menu_value_str(uint8_t item, char *buf, uint8_t buf_size) {
  switch (item) {
  case MENU_PROFILE: {
    const char *name = eq_profile_get_active_name();
    snprintf(buf, buf_size, "%.9s", name); // truncate for display
  } break;
  case MENU_BASS: {
    int8_t v = audio_eq_get_band(EQ_BAND_BASS);
    if (v > 0)
      snprintf(buf, buf_size, "+%d", v);
    else
      snprintf(buf, buf_size, "%d", v);
  } break;
  case MENU_TREBLE: {
    int8_t v = audio_eq_get_band(EQ_BAND_TREBLE);
    if (v > 0)
      snprintf(buf, buf_size, "+%d", v);
    else
      snprintf(buf, buf_size, "%d", v);
  } break;
  case MENU_BRIGHTNESS:
    snprintf(buf, buf_size, "%s", brightness_names[brightness_level]);
    break;
  case MENU_TIMEOUT:
    snprintf(buf, buf_size, "%s", timeout_names[timeout_level]);
    break;
  default:
    buf[0] = '\0';
    break;
  }
}

static void menu_update_scroll(void) {
  int8_t row = cursor_to_visible_row();
  if (row < 0) row = 0;
  if ((uint8_t)row < menu_scroll)
    menu_scroll = (uint8_t)row;
  else if ((uint8_t)row >= menu_scroll + MENU_VISIBLE)
    menu_scroll = (uint8_t)row - MENU_VISIBLE + 1;
}

static void draw_menu_screen(void) {
  sh1106_clear();
  sh1106_set_font_scale(1);

  rebuild_visible_items();
  menu_update_scroll();

  uint8_t end = menu_scroll + MENU_VISIBLE;
  if (end > visible_count)
    end = visible_count;

  for (uint8_t vi = menu_scroll; vi < end; vi++) {
    uint8_t item = visible_items[vi];
    uint8_t y = MENU_Y_START + (vi - menu_scroll) * MENU_ROW_H;

    sh1106_set_cursor(2, y + 2);
    sh1106_write_string(menu_labels[item]);

    if (item != MENU_BACK && item != MENU_DFU) {
      char val[12];
      get_menu_value_str(item, val, sizeof(val));
      uint8_t vlen = (uint8_t)strlen(val);
      if (vlen > 0) {
        uint8_t vx = SH1106_WIDTH - vlen * 6 - 2;
        sh1106_set_cursor(vx, y + 2);
        sh1106_write_string(val);
      }
    }

    if (item == menu_cursor) {
      if (!menu_editing || menu_blink_on) {
        sh1106_invert_region(0, y, SH1106_WIDTH, MENU_ROW_H);
      }
    }
  }

  sh1106_update();
}

static void draw_idle_screen(void) {
  sh1106_clear();
  uint8_t x = idle_dot_pos ? IDLE_DOT_X1 : IDLE_DOT_X0;
  sh1106_invert_region(x, IDLE_DOT_Y, IDLE_DOT_SIZE, IDLE_DOT_SIZE);
  sh1106_update();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void display_init(uint8_t brightness, uint8_t timeout) {
  if (brightness <= 2)
    brightness_level = brightness;
  if (timeout <= 3)
    timeout_level = timeout;
  sh1106_set_brightness(brightness_hw[brightness_level]);
  last_activity_tick = HAL_GetTick();
  display_dirty = 1;
}

void display_draw(uint32_t now) {
  if (!display_dirty || display_is_off)
    return;
  if (now - display_last_tick < DISPLAY_MIN_INTERVAL_MS)
    return;

  switch (screen_state) {
  case SCREEN_VOLUME:
    draw_volume_screen();
    break;
  case SCREEN_MENU:
    draw_menu_screen();
    break;
  case SCREEN_IDLE:
    draw_idle_screen();
    break;
  }
  display_dirty = 0;
  display_last_tick = now;
}

void display_check_timeout(uint32_t now) {
  if (screen_state == SCREEN_IDLE)
    return;

  // Menu: fixed 60s inactivity → back to volume
  if (screen_state == SCREEN_MENU) {
    if (now - last_activity_tick >= MENU_TIMEOUT_MS) {
      menu_editing = 0;
      screen_state = SCREEN_VOLUME;
      last_activity_tick = now;
      display_dirty = 1;
    }
    return;
  }

  // Volume: configurable timeout → idle dot
  if (timeout_level == 0)
    return;
  if (now - last_activity_tick >= timeout_ms[timeout_level]) {
    screen_state = SCREEN_IDLE;
    idle_dot_pos = now & 1;
    idle_dot_tick = now;
    display_dirty = 1;
  }
}

void display_blink_tick(uint32_t now) {
  if (!menu_editing || screen_state != SCREEN_MENU)
    return;
  if (now - menu_blink_tick < 500)
    return;
  menu_blink_on ^= 1;
  menu_blink_tick = now;
  int8_t row = cursor_to_visible_row();
  if (row < 0) return;
  uint8_t y = MENU_Y_START + ((uint8_t)row - menu_scroll) * MENU_ROW_H;
  sh1106_invert_region(0, y, SH1106_WIDTH, MENU_ROW_H);
  sh1106_update();
}

void display_idle_tick(uint32_t now) {
  if (screen_state != SCREEN_IDLE)
    return;
  if (now - idle_dot_tick >= IDLE_DOT_SWITCH_MS) {
    idle_dot_pos ^= 1;
    idle_dot_tick = now;
    display_dirty = 1;
  }
}

screen_state_t display_get_screen(void) { return screen_state; }

void display_set_screen(screen_state_t s) {
  screen_state = s;
  display_dirty = 1;
}

void display_set_dirty(void) { display_dirty = 1; }

void display_mark_activity(uint32_t now) {
  last_activity_tick = now;
  if (screen_state == SCREEN_IDLE) {
    screen_state = SCREEN_VOLUME;
    display_dirty = 1;
  }
  if (display_is_off) {
    sh1106_display_on();
    display_is_off = 0;
    display_dirty = 1;
  }
}

uint8_t display_get_menu_cursor(void) { return menu_cursor; }

uint8_t display_is_menu_editing(void) { return menu_editing; }

void display_menu_reset(void) {
  menu_cursor = 0;
  menu_scroll = 0;
  menu_editing = 0;
  display_dirty = 1;
}

void display_menu_enter_edit(void) {
  menu_editing = 1;
  menu_blink_on = 0;
  menu_blink_tick = HAL_GetTick();
  display_dirty = 1;
}

void display_menu_exit_edit(void) {
  menu_editing = 0;
  menu_blink_on = 1;
  display_dirty = 1;
}

void display_menu_navigate(int8_t delta) {
  rebuild_visible_items();
  int8_t cur_row = cursor_to_visible_row();
  if (cur_row < 0) cur_row = 0;

  int8_t new_row = cur_row + delta;
  if (new_row < 0) new_row = 0;
  if (new_row >= (int8_t)visible_count) new_row = (int8_t)visible_count - 1;

  uint8_t new_item = visible_items[new_row];
  if (new_item != menu_cursor) {
    uint8_t old_scroll = menu_scroll;
    menu_cursor = new_item;
    menu_update_scroll();
    if (menu_scroll != old_scroll) {
      display_dirty = 1;
    } else {
      uint8_t old_y = MENU_Y_START + ((uint8_t)cur_row - menu_scroll) * MENU_ROW_H;
      uint8_t new_y = MENU_Y_START + ((uint8_t)new_row - menu_scroll) * MENU_ROW_H;
      sh1106_invert_region(0, old_y, SH1106_WIDTH, MENU_ROW_H);
      sh1106_invert_region(0, new_y, SH1106_WIDTH, MENU_ROW_H);
      sh1106_update();
    }
  }
}

uint8_t display_get_brightness(void) { return brightness_level; }

void display_set_brightness(uint8_t level) {
  if (level > 2)
    level = 2;
  brightness_level = level;
  sh1106_set_brightness(brightness_hw[brightness_level]);
}

uint8_t display_get_timeout_level(void) { return timeout_level; }

void display_set_timeout_level(uint8_t level) {
  if (level > 3)
    level = 3;
  timeout_level = level;
}

void display_enter_idle(uint32_t now) {
  screen_state = SCREEN_IDLE;
  idle_dot_pos = now & 1;
  idle_dot_tick = now;
  menu_editing = 0;
  if (display_is_off) {
    sh1106_display_on();
    display_is_off = 0;
  }
  display_dirty = 1;
}
