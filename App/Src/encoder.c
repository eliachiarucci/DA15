// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * Rotary Encoder with Push Button
 * Quadrature decoding via full state transition table
 *
 * Uses 4x decoding (both edges of both channels A and B).
 * A state machine lookup table rejects invalid transitions caused by
 * contact bounce. Bounce between adjacent states produces alternating
 * +1/-1 that cancel in the accumulator, so no extra debounce timer
 * is needed for rotation.
 *
 * Button is polled from the main loop with software debounce and
 * long-press detection.
 *
 * Requires CubeMX config:
 * - PB15 (TRIM_A): GPIO_EXTI15, rising/falling edge, pull-up
 * - PB14 (TRIM_B): GPIO_EXTI14, rising/falling edge, pull-up
 * - PA8 (ENCODER_PUSH_I): GPIO input, pull-up
 * - NVIC: EXTI4_15 enabled
 */

#include "encoder.h"
#include "main.h"
#include "stm32f0xx_hal.h"

// Most encoders with detents: 1 detent = 1 full quadrature cycle = 4 edges
// Change to 2 if your encoder detents at half-cycles
#define COUNTS_PER_DETENT 4

#define BTN_DEBOUNCE_MS 50
#define LONG_PRESS_MS   1000

// Quadrature state transition table
// Index = (prev_AB << 2) | curr_AB  where AB = (A << 1) | B
// Value: +1 = CW step, -1 = CCW step, 0 = invalid/no-change (bounce rejected)
//
// CW sequence:  00 → 01 → 11 → 10 → 00
// CCW sequence: 00 → 10 → 11 → 01 → 00
static const int8_t qdec_table[16] = {
    /* prev=00 → */  0, +1, -1,  0,
    /* prev=01 → */ -1,  0,  0, +1,
    /* prev=10 → */ +1,  0,  0, -1,
    /* prev=11 → */  0, -1, +1,  0,
};

// Rotation state (ISR-updated)
static volatile int16_t encoder_accum = 0;
static volatile uint8_t prev_state = 0;  // previous AB state (2 bits)

// Button state (poll-updated)
static uint8_t btn_state = 0;
static uint8_t btn_raw_prev = 0;
static uint32_t btn_debounce_tick = 0;
static uint32_t btn_press_tick = 0;
static uint8_t btn_long_fired = 0;
static uint8_t short_press_pending = 0;
static uint8_t long_press_pending = 0;

void encoder_init(void) {
    encoder_accum = 0;
    btn_state = 0;
    btn_raw_prev = 0;
    btn_debounce_tick = 0;
    btn_press_tick = 0;
    btn_long_fired = 0;
    short_press_pending = 0;
    long_press_pending = 0;

    // Sample initial encoder state
    uint8_t a = HAL_GPIO_ReadPin(TRIM_A_GPIO_Port, TRIM_A_Pin);
    uint8_t b = HAL_GPIO_ReadPin(TRIM_B_GPIO_Port, TRIM_B_Pin);
    prev_state = (a << 1) | b;
}

void encoder_exti_callback(uint16_t GPIO_Pin) {
    if (GPIO_Pin == TRIM_A_Pin || GPIO_Pin == TRIM_B_Pin) {
        uint8_t a = HAL_GPIO_ReadPin(TRIM_A_GPIO_Port, TRIM_A_Pin);
        uint8_t b = HAL_GPIO_ReadPin(TRIM_B_GPIO_Port, TRIM_B_Pin);
        uint8_t curr_state = (a << 1) | b;

        // Look up transition in state table
        int8_t dir = qdec_table[(prev_state << 2) | curr_state];
        encoder_accum -= dir;  // negated to match physical CW = increase
        prev_state = curr_state;
    }
}

void encoder_poll(uint32_t now) {
    uint8_t btn_raw =
        (HAL_GPIO_ReadPin(ENCODER_PUSH_I_GPIO_Port, ENCODER_PUSH_I_Pin) ==
         GPIO_PIN_RESET)
            ? 1
            : 0;

    if (btn_raw != btn_raw_prev) {
        btn_debounce_tick = now;
        btn_raw_prev = btn_raw;
    }

    if ((now - btn_debounce_tick >= BTN_DEBOUNCE_MS) && btn_raw != btn_state) {
        uint8_t old_state = btn_state;
        btn_state = btn_raw;

        if (btn_state == 1 && old_state == 0) {
            btn_press_tick = now;
            btn_long_fired = 0;
        }

        if (btn_state == 0 && old_state == 1) {
            if (!btn_long_fired) {
                short_press_pending = 1;
            }
        }
    }

    if (btn_state == 1 && !btn_long_fired &&
        (now - btn_press_tick >= LONG_PRESS_MS)) {
        btn_long_fired = 1;
        long_press_pending = 1;
    }
}

int8_t encoder_get_delta(void) {
    // Brief critical section: extract whole detent steps, keep remainder
    __disable_irq();
    int16_t accum = encoder_accum;
    int8_t steps = (int8_t)(accum / COUNTS_PER_DETENT);
    encoder_accum = accum - (int16_t)steps * COUNTS_PER_DETENT;
    __enable_irq();
    return steps;
}

bool encoder_has_short_press(void) {
    if (short_press_pending) {
        short_press_pending = 0;
        return true;
    }
    return false;
}

bool encoder_has_long_press(void) {
    if (long_press_pending) {
        long_press_pending = 0;
        return true;
    }
    return false;
}
