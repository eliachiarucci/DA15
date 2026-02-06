// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * Rotary Encoder with Push Button
 * Software quadrature decoding via EXTI interrupts
 * Polling-based button with debounce and long-press detection
 *
 * TRIM_A (PB15) = Channel A, TRIM_B (PB14) = Channel B
 * ENCODER_PUSH_I (PA8) = Push button
 */

#ifndef ENCODER_H
#define ENCODER_H

#include <stdint.h>
#include <stdbool.h>

// Initialize encoder GPIO and state
void encoder_init(void);

// Poll button state - call from main loop
void encoder_poll(uint32_t now);

// Get accumulated rotation delta since last call
// Positive = clockwise, negative = counter-clockwise
int8_t encoder_get_delta(void);

// Check if short press occurred since last call (one-shot)
bool encoder_has_short_press(void);

// Check if long press occurred since last call (one-shot)
bool encoder_has_long_press(void);

// EXTI callback - called from HAL_GPIO_EXTI_Callback (rotation only)
void encoder_exti_callback(uint16_t GPIO_Pin);

#endif /* ENCODER_H */
