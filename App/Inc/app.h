// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

#ifndef APP_H
#define APP_H

#include <stdint.h>

void app_init(void);
void app_loop(void);

// Returns power level: 0=500mA, 1=1500mA, 2=3000mA
uint8_t app_get_power_level(void);

// Reboot into STM32 system bootloader (USB DFU mode)
void app_reboot_to_dfu(void);

#endif