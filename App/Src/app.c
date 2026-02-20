// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * Application Entry Point
 * Initializes hardware/USB, runs main loop, handles input and settings.
 */

#include "app.h"
#include "SEGGER_RTT.h"
#include "audio_eq.h"
#include "audio_output.h"
#include "display.h"
#include "encoder.h"
#include "main.h"
#include "settings.h"
#include "sh1106.h"
#include "stm32h5xx_hal.h"
#include "tusb.h"
#include <stdint.h>

// External handles from main.c
extern ADC_HandleTypeDef hadc1;
extern I2C_HandleTypeDef hi2c2;

// ---------------------------------------------------------------------------
// Settings save debounce
// ---------------------------------------------------------------------------
#define SETTINGS_SAVE_DELAY_MS 2000
static uint32_t settings_save_tick = 0;
static uint8_t settings_dirty = 0;

// ---------------------------------------------------------------------------
// USB idle detection
// ---------------------------------------------------------------------------
#define USB_STATE_DEBOUNCE_MS 2000
static uint8_t usb_was_mounted = 0;
static uint8_t usb_stable = 1;
static uint32_t usb_change_tick = 0;
static uint8_t usb_change_pending = 0;

// ---------------------------------------------------------------------------
// USB power detection
// ---------------------------------------------------------------------------
static uint16_t cc1_voltage = 0;
static uint16_t cc2_voltage = 0;
static uint8_t max_power_available = 0; // 0=500mA, 1=1500mA, 2=3000mA

// CC voltage thresholds for USB-C power detection (mV)
#define CC_THRESHOLD_500MA  150
#define CC_THRESHOLD_1500MA 700
#define CC_THRESHOLD_3000MA 1300

static uint16_t adc_read_next_mv(void) {
  uint16_t mv = 0;
  if (HAL_ADC_PollForConversion(&hadc1, 10) == HAL_OK) {
    mv = (HAL_ADC_GetValue(&hadc1) * 3300U) / 4095U;
  }
  HAL_Delay(50);
  return mv;
}

static void read_usb_detection_voltages(void) {
  if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK) {
    SEGGER_RTT_printf(0, "ADC calibration failed\n");
    return;
  }
  HAL_Delay(50);

  if (HAL_ADC_Start(&hadc1) != HAL_OK) {
    SEGGER_RTT_printf(0, "ADC start failed\n");
    return;
  }

  cc1_voltage = adc_read_next_mv();
  cc2_voltage = adc_read_next_mv();

  SEGGER_RTT_printf(0, "CC1 Voltage : %dmV\n", cc1_voltage);
  SEGGER_RTT_printf(0, "CC2 Voltage: %dmV\n", cc2_voltage);

  HAL_ADC_Stop(&hadc1);

  uint16_t highest_CC_voltage = cc1_voltage;
  if (cc2_voltage > highest_CC_voltage) {
    highest_CC_voltage = cc2_voltage;
  }

  if (highest_CC_voltage > CC_THRESHOLD_3000MA) {
    max_power_available = 2; // 3000mA
  } else if (highest_CC_voltage > CC_THRESHOLD_1500MA) {
    max_power_available = 1; // 1500mA
  } else {
    max_power_available = 0; // 500mA
  }
}

uint8_t app_get_power_level(void) { return max_power_available; }

// ---------------------------------------------------------------------------
// DFU bootloader reboot
// ---------------------------------------------------------------------------
#define DFU_MAGIC_ADDR  ((volatile uint32_t *)0x20007FF0)  // end of 32KB RAM
#define DFU_MAGIC_VALUE 0xDEADBEEFUL

void app_reboot_to_dfu(void) {
  sh1106_clear();
  sh1106_set_font_scale(1);
  sh1106_write_string_centered("UPDATE MODE", 28);
  sh1106_update();

  while (sh1106_is_busy()) {}

  *DFU_MAGIC_ADDR = DFU_MAGIC_VALUE;
  NVIC_SystemReset();
}

// TinyUSB DFU Runtime callback
void tud_dfu_runtime_reboot_to_dfu_cb(void) { app_reboot_to_dfu(); }

// ---------------------------------------------------------------------------
// Settings helper
// ---------------------------------------------------------------------------
static void mark_settings_dirty(uint32_t now) {
  settings_dirty = 1;
  settings_save_tick = now;
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------
static void handle_short_press(uint32_t now) {
  display_mark_activity(now);
  screen_state_t s = display_get_screen();

  if (s == SCREEN_VOLUME) {
    audio_output_toggle_local_mute();
    mark_settings_dirty(now);
    display_set_dirty();
  } else if (s == SCREEN_MENU) {
    if (display_is_menu_editing()) {
      display_menu_exit_edit();
    } else if (display_get_menu_cursor() == MENU_BACK) {
      display_set_screen(SCREEN_VOLUME);
    } else if (display_get_menu_cursor() == MENU_DFU) {
      app_reboot_to_dfu();
    } else {
      display_menu_enter_edit();
    }
  }
}

static void handle_long_press(uint32_t now) {
  display_mark_activity(now);
  screen_state_t s = display_get_screen();

  if (s == SCREEN_VOLUME) {
    display_set_screen(SCREEN_MENU);
    display_menu_reset();
  } else if (s == SCREEN_MENU) {
    display_menu_exit_edit();
    display_set_screen(SCREEN_VOLUME);
  }
}

static int16_t clamp_i16(int16_t val, int16_t lo, int16_t hi) {
  if (val < lo) return lo;
  if (val > hi) return hi;
  return val;
}

static void handle_encoder_rotate(int8_t delta, uint32_t now) {
  display_mark_activity(now);
  screen_state_t s = display_get_screen();

  if (s == SCREEN_VOLUME) {
    int16_t vol = (int16_t)audio_output_get_local_volume() + delta;
    audio_output_set_local_volume((uint8_t)clamp_i16(vol, 0, 100));
    mark_settings_dirty(now);
    display_set_dirty();
  } else if (s == SCREEN_MENU) {
    if (!display_is_menu_editing()) {
      display_menu_navigate(delta);
    } else {
      switch (display_get_menu_cursor()) {
      case MENU_BASS: {
        int8_t v = (int8_t)clamp_i16(audio_eq_get_band(EQ_BAND_BASS) + delta,
                                      EQ_VALUE_MIN, EQ_VALUE_MAX);
        audio_eq_set_band(EQ_BAND_BASS, v);
        mark_settings_dirty(now);
        display_set_dirty();
      } break;
      case MENU_TREBLE: {
        int8_t v = (int8_t)clamp_i16(audio_eq_get_band(EQ_BAND_TREBLE) + delta,
                                      EQ_VALUE_MIN, EQ_VALUE_MAX);
        audio_eq_set_band(EQ_BAND_TREBLE, v);
        mark_settings_dirty(now);
        display_set_dirty();
      } break;
      case MENU_BRIGHTNESS: {
        int8_t b = (int8_t)clamp_i16(display_get_brightness() + (delta > 0 ? 1 : -1), 0, 2);
        display_set_brightness((uint8_t)b);
        mark_settings_dirty(now);
        display_set_dirty();
      } break;
      case MENU_TIMEOUT: {
        int8_t t = (int8_t)clamp_i16(display_get_timeout_level() + (delta > 0 ? 1 : -1), 0, 3);
        display_set_timeout_level((uint8_t)t);
        mark_settings_dirty(now);
        display_set_dirty();
      } break;
      default:
        break;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------
void app_init(void) {
  SEGGER_RTT_printf(0, "\n=== DA15 boot ===\n");

  // Read USB detection voltages (CC1, CC2, DN, DP)
  read_usb_detection_voltages();

  // Initialize OLED display
  SEGGER_RTT_printf(0, "[init] OLED init...\n");
  sh1106_init(&hi2c2);
  HAL_Delay(1000);

  // Initialize audio output hardware
  SEGGER_RTT_printf(0, "[init] audio output init...\n");
  audio_output_init();

  // Initialize TinyUSB device stack
  SEGGER_RTT_printf(0, "[init] TinyUSB init...\n");
  tusb_rhport_init_t dev_init = {.role = TUSB_ROLE_DEVICE,
                                 .speed = TUSB_SPEED_AUTO};
  tusb_init(BOARD_TUD_RHPORT, &dev_init);
  SEGGER_RTT_printf(0, "[init] TinyUSB init done\n");

  // Default EQ (flat)
  audio_eq_set_band(EQ_BAND_BASS, 0);
  audio_eq_set_band(EQ_BAND_TREBLE, 0);

  // Initialize encoder
  encoder_init();
  SEGGER_RTT_printf(0, "[init] encoder done\n");

  // Load persistent settings
  uint8_t brightness = 1;
  uint8_t timeout = 0;

  SEGGER_RTT_printf(0, "[init] loading settings...\n");
  settings_t saved;
  if (settings_load(&saved)) {
    SEGGER_RTT_printf(0, "[init] settings loaded OK\n");
    audio_output_set_local_volume(saved.local_volume);
    if (saved.local_muted) {
      audio_output_toggle_local_mute();
    }
    audio_eq_set_band(EQ_BAND_BASS, saved.bass);
    audio_eq_set_band(EQ_BAND_TREBLE, saved.treble);
    brightness = saved.brightness;
    timeout = saved.display_timeout;
  } else {
    SEGGER_RTT_printf(0, "[init] no valid settings, using defaults\n");
  }

  // Initialize display module (applies brightness, starts activity timer)
  SEGGER_RTT_printf(0, "[init] display init...\n");
  display_init(brightness, timeout);

  SEGGER_RTT_printf(0, "[init] complete, entering main loop\n");
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
void app_loop(void) {
  uint32_t now = HAL_GetTick();

  // --- High priority: USB + audio ---
  tud_task();
  audio_output_task();

  // --- USB connection monitoring (idle screen for OLED burn-in protection) ---
  // Any USB state change must hold stable for 3s before taking effect.
  uint8_t usb_active = tud_mounted() && !tud_suspended();
  if (usb_active)
    usb_was_mounted = 1;

  if (usb_was_mounted) {
    if (usb_active != usb_stable) {
      if (!usb_change_pending) {
        usb_change_pending = 1;
        usb_change_tick = now;
      } else if (now - usb_change_tick >= USB_STATE_DEBOUNCE_MS) {
        usb_change_pending = 0;
        usb_stable = usb_active;
        if (!usb_stable) {
          if (display_get_screen() != SCREEN_IDLE)
            display_enter_idle(now);
        } else {
          if (display_get_screen() == SCREEN_IDLE)
            display_mark_activity(now);
        }
      }
    } else {
      usb_change_pending = 0;
    }
  }

  // --- Idle dot position switch ---
  display_idle_tick(now);

  // --- Encoder input (drain events always, act only when USB active) ---
  encoder_poll(now);

  if (encoder_has_short_press() && usb_active) {
    handle_short_press(now);
  }
  if (encoder_has_long_press() && usb_active) {
    handle_long_press(now);
  }

  int8_t delta = encoder_get_delta();
  if (delta != 0 && usb_active) {
    handle_encoder_rotate(delta, now);
  }

  // --- Debounced settings save ---
  if (settings_dirty && (now - settings_save_tick >= SETTINGS_SAVE_DELAY_MS)) {
    settings_t s = {
        .local_volume = audio_output_get_local_volume(),
        .local_muted = audio_output_is_local_muted(),
        .bass = audio_eq_get_band(EQ_BAND_BASS),
        .treble = audio_eq_get_band(EQ_BAND_TREBLE),
        .brightness = display_get_brightness(),
        .display_timeout = display_get_timeout_level(),
    };
    settings_save(&s);
    settings_dirty = 0;
  }

  // --- Display timeout ---
  display_check_timeout(now);

  // --- Menu edit blink ---
  display_blink_tick(now);

  // --- Display update (rate-limited) ---
  display_draw(now);
}
