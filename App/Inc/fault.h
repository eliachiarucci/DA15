// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * Fault management
 *
 * Fault handlers call fault_capture(), which snapshots the Cortex-M33 fault
 * status registers into a .noinit RAM record that survives the watchdog
 * reset. On the next boot, fault_boot_report() logs the reset cause and any
 * stored fault over RTT; the host can also fetch/clear the record over CDC
 * (CMD_GET_FAULT_INFO / CMD_CLEAR_FAULT).
 */

#ifndef FAULT_H
#define FAULT_H

#include <stdbool.h>
#include <stdint.h>

// Fault origin recorded by fault_capture()
typedef enum {
    FAULT_NONE = 0,
    FAULT_HARD = 1,
    FAULT_MEM = 2,
    FAULT_BUS = 3,
    FAULT_USAGE = 4,
    FAULT_NMI = 5,          // NMI from an unknown source (not flash ECC)
    FAULT_ERROR_HANDLER = 6, // HAL Error_Handler()
} fault_type_t;

// Reset cause bits (decoded from RCC->RSR at boot)
#define RESET_CAUSE_PIN  (1u << 0)
#define RESET_CAUSE_BOR  (1u << 1) // brown-out / power-on
#define RESET_CAUSE_SW   (1u << 2) // NVIC_SystemReset
#define RESET_CAUSE_IWDG (1u << 3) // watchdog bite
#define RESET_CAUSE_WWDG (1u << 4)
#define RESET_CAUSE_LPWR (1u << 5)

typedef struct {
    uint32_t magic;     // FAULT_MAGIC when the record is valid
    uint8_t type;       // fault_type_t
    uint8_t count;      // faults since last clear (saturates at 255)
    uint8_t _pad[2];
    uint32_t cfsr;      // SCB->CFSR (configurable fault status)
    uint32_t hfsr;      // SCB->HFSR (hard fault status)
    uint32_t mmfar;     // faulting address for memory faults (if valid)
    uint32_t bfar;      // faulting address for bus faults (if valid)
    uint32_t msp;       // main stack pointer at capture (frame is nearby)
    uint32_t psp;       // process stack pointer at capture
    uint32_t uptime_ms; // HAL tick at capture
} fault_record_t;

// Snapshot fault state into .noinit RAM. Safe from any fault context.
void fault_capture(uint8_t type);

// Decode + latch the reset cause and log any stored fault. Call once at boot.
void fault_boot_report(void);

// Copy the stored record into *out. Returns false if none is stored.
bool fault_get_last(fault_record_t *out);

// Invalidate the stored record.
void fault_clear(void);

// RESET_CAUSE_* bits for the current boot (valid after fault_boot_report).
uint8_t fault_get_reset_cause(void);

#endif // FAULT_H
