// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

/*
 * Minimal STM32 HAL stub for host-side unit tests.
 * Provides just enough for App/Src/eq_profile.c to compile; the flash
 * functions are inert (tests exercise the RAM-side logic only).
 */

#ifndef STM32H5XX_HAL_STUB_H
#define STM32H5XX_HAL_STUB_H

#include <stdint.h>

typedef enum { HAL_OK = 0, HAL_ERROR = 1 } HAL_StatusTypeDef;

#define FLASH_TYPEPROGRAM_QUADWORD 0u
#define FLASH_BANK_2               2u

#define FLASH_CR_SER       (1u << 5)
#define FLASH_CR_SNB_Pos   6u
#define FLASH_CR_SNB       (0x3Fu << FLASH_CR_SNB_Pos)
#define FLASH_CR_BKSEL     (1u << 31)
#define FLASH_FLAG_BSY     (1u << 0)
#define FLASH_FLAG_WBNE    (1u << 1)
#define FLASH_FLAG_DBNE    (1u << 3)
#define FLASH_FLAG_ALL_ERRORS 0x00FC0000u

typedef struct {
    volatile uint32_t NSSR;
    volatile uint32_t NSCR;
} flash_stub_regs_t;

static flash_stub_regs_t flash_stub_regs;
#define FLASH_NS (&flash_stub_regs)

#define CLEAR_BIT(REG, BIT) ((REG) &= ~(BIT))
#define SET_BIT(REG, BIT)   ((REG) |= (BIT))

#define __HAL_FLASH_GET_FLAG(flag)   ((flash_stub_regs.NSSR & (flag)) != 0u)
#define __HAL_FLASH_CLEAR_FLAG(flag) (flash_stub_regs.NSSR &= ~(uint32_t)(flag))

static inline HAL_StatusTypeDef HAL_FLASH_Unlock(void) { return HAL_OK; }
static inline HAL_StatusTypeDef HAL_FLASH_Lock(void) { return HAL_OK; }
static inline HAL_StatusTypeDef HAL_FLASH_Program(uint32_t type, uint32_t addr,
                                                  uint32_t data) {
    (void)type;
    (void)addr;
    (void)data;
    return HAL_OK;
}
static inline void FLASH_Erase_Sector(uint32_t sector, uint32_t banks) {
    (void)sector;
    (void)banks;
}
static inline void HAL_ICACHE_Invalidate(void) {}
static inline uint32_t HAL_GetTick(void) { return 0; }

#endif // STM32H5XX_HAL_STUB_H
