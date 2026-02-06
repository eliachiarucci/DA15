// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

#ifndef SH1106_H
#define SH1106_H

#include "stm32f0xx_hal.h"
#include <stdint.h>

// Display dimensions (128x64)
#define SH1106_WIDTH  128
#define SH1106_HEIGHT 64

// I2C address (0x3C is most common, some modules use 0x3D)
#define SH1106_I2C_ADDR (0x3C << 1)

void sh1106_init(I2C_HandleTypeDef *hi2c);
void sh1106_clear(void);
void sh1106_clear_region(uint8_t x, uint8_t y, uint8_t w, uint8_t h);
void sh1106_update(void);
void sh1106_set_cursor(uint8_t x, uint8_t y);
void sh1106_set_font_scale(uint8_t scale);
void sh1106_write_char(char c);
void sh1106_write_string(const char *str);
void sh1106_write_string_centered(const char *str, uint8_t y);
uint8_t sh1106_is_busy(void);
void sh1106_set_brightness(uint8_t val);
void sh1106_invert_region(uint8_t x, uint8_t y, uint8_t w, uint8_t h);
void sh1106_display_off(void);
void sh1106_display_on(void);

#endif
