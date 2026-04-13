/**
 * @file ra8d2_sdhi_regs.h
 * @brief SDHI (SD host interface) register layout for the Renesas RA8D2
 *
 * @details
 * Two SDHI instances at `0x40252000` (SDHI0) and `0x40252400`
 * (SDHI1). Wave 5.4 introduces a minimal register window covering
 * command issue, response, status, and interrupt mask -- enough to
 * bring up the driver lifecycle + IRQ path without a full SD card
 * command engine.
 *
 * Full CMD response decoding and block transfer support land with
 * the first consumer driver.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum : uintptr_t {
  k_ra_sdhi0_base_addr = 0x40252000UL,
  k_ra_sdhi1_base_addr = 0x40252400UL,
  k_ra_sdhi_stride     = 0x400UL,
} ra_sdhi_addr_t;

typedef enum : uint8_t {
  k_ra_sdhi_instance_count = 2U,
} ra_sdhi_limits_t;

/**
 * @struct r_sdhi_regs_t
 * @brief Minimal SDHI register window (partial).
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is accessed via ``ra_sdhi()`` in
 * ``libs/ra_hal/src/ra_sdhi.c`` once the full command engine
 * lands. Today only SD_INFO1 / SD_INFO2 / mask regs / CLK_CTRL
 * are driven; the other 64-bit registers are reserved for the
 * follow-up block-transfer commit.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  volatile uint64_t SD_CMD;        /**< +0x00 Command register (64-bit). */
  volatile uint64_t SD_ARG;        /**< +0x08 Argument.                  */
  volatile uint64_t SD_STOP;       /**< +0x10 Stop / block count.        */
  volatile uint64_t SD_SECCNT;     /**< +0x18 Sector count.              */
  volatile uint64_t SD_RSP10;      /**< +0x20 Response 0.                */
  volatile uint64_t SD_RSP32;      /**< +0x28 Response 1.                */
  volatile uint64_t SD_RSP54;      /**< +0x30 Response 2.                */
  volatile uint64_t SD_RSP76;      /**< +0x38 Response 3.                */
  volatile uint64_t SD_INFO1;      /**< +0x40 Info 1 (IRQ status).       */
  volatile uint64_t SD_INFO2;      /**< +0x48 Info 2 (transfer state).   */
  volatile uint64_t SD_INFO1_MASK; /**< +0x50 Info 1 mask.            */
  volatile uint64_t SD_INFO2_MASK; /**< +0x58 Info 2 mask.            */
  volatile uint64_t SD_CLK_CTRL;   /**< +0x60 Clock control.          */
} r_sdhi_regs_t;
/* cppcheck-suppress-end [unusedStructMember] */

/** @brief Get pointer to SDHI instance N (0 or 1). */
static inline volatile r_sdhi_regs_t* ra_sdhi(uint8_t instance)
{
  if (instance >= (uint8_t)k_ra_sdhi_instance_count) {
    return nullptr;
  }
  return (volatile r_sdhi_regs_t*)(k_ra_sdhi0_base_addr +
                                   ((uintptr_t)instance * (uintptr_t)k_ra_sdhi_stride));
}

#ifdef __cplusplus
}
#endif
