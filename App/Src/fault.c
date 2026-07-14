// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

#include "fault.h"
#include "SEGGER_RTT.h"
#include "stm32h5xx_hal.h"

#define FAULT_MAGIC 0xFA17C0DEUL

// Lives in .noinit: neither loaded nor zeroed at startup, so it survives
// every reset except power loss (see STM32H503xx_FLASH.ld)
static fault_record_t fault_record __attribute__((section(".noinit")));

static uint8_t reset_cause;

void fault_capture(uint8_t type) {
    uint8_t count = 1;
    if (fault_record.magic == FAULT_MAGIC && fault_record.count < 255)
        count = (uint8_t)(fault_record.count + 1);

    fault_record.type = type;
    fault_record.count = count;
    fault_record.cfsr = SCB->CFSR;
    fault_record.hfsr = SCB->HFSR;
    fault_record.mmfar = SCB->MMFAR;
    fault_record.bfar = SCB->BFAR;
    fault_record.msp = __get_MSP();
    fault_record.psp = __get_PSP();
    fault_record.uptime_ms = HAL_GetTick();
    fault_record.magic = FAULT_MAGIC; // written last: record complete
}

void fault_boot_report(void) {
    uint32_t rsr = RCC->RSR;
    reset_cause = 0;
    if (rsr & RCC_RSR_PINRSTF)  reset_cause |= RESET_CAUSE_PIN;
    if (rsr & RCC_RSR_BORRSTF)  reset_cause |= RESET_CAUSE_BOR;
    if (rsr & RCC_RSR_SFTRSTF)  reset_cause |= RESET_CAUSE_SW;
    if (rsr & RCC_RSR_IWDGRSTF) reset_cause |= RESET_CAUSE_IWDG;
    if (rsr & RCC_RSR_WWDGRSTF) reset_cause |= RESET_CAUSE_WWDG;
    if (rsr & RCC_RSR_LPWRRSTF) reset_cause |= RESET_CAUSE_LPWR;
    SET_BIT(RCC->RSR, RCC_RSR_RMVF); // clear flags for the next boot

    SEGGER_RTT_printf(0, "[fault] reset cause: 0x%02x%s\n", reset_cause,
                      (reset_cause & RESET_CAUSE_IWDG) ? " (WATCHDOG BITE)"
                                                       : "");

    if (fault_record.magic == FAULT_MAGIC) {
        SEGGER_RTT_printf(
            0,
            "[fault] stored fault: type=%d count=%d uptime=%ums\n"
            "[fault]   CFSR=%08x HFSR=%08x MMFAR=%08x BFAR=%08x\n"
            "[fault]   MSP=%08x PSP=%08x\n",
            fault_record.type, fault_record.count,
            (unsigned)fault_record.uptime_ms, (unsigned)fault_record.cfsr,
            (unsigned)fault_record.hfsr, (unsigned)fault_record.mmfar,
            (unsigned)fault_record.bfar, (unsigned)fault_record.msp,
            (unsigned)fault_record.psp);
    }
}

bool fault_get_last(fault_record_t *out) {
    if (fault_record.magic != FAULT_MAGIC)
        return false;
    *out = fault_record;
    return true;
}

void fault_clear(void) {
    fault_record.magic = 0;
    fault_record.count = 0;
}

uint8_t fault_get_reset_cause(void) { return reset_cause; }
