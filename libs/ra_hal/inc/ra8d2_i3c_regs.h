/**
 * @file ra8d2_i3c_regs.h
 * @brief I3C Bus Interface register layout for the Renesas RA8D2
 *
 * @details
 * Single I3C instance at `0x4011F000`. introduces a minimal
 * register window covering the bit-transfer engine, device-address
 * table, and basic interrupt / status registers -- enough to bring
 * up the driver lifecycle + IRQ path without a full I3C command
 * engine.
 *
 * Full CCC (Common Command Code) handling, IBI (In-Band Interrupt)
 * support, and the HDR / DDR modes land with the first consumer
 * driver.
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
  k_ra_i3c_base_addr = 0x4035F000UL, /**< I3C0 peripheral base (FSP R_I3C0_BASE). */
} ra_i3c_addr_t;

typedef enum : uint8_t {
  k_ra_i3c_instance_count  = 1U,
  k_ra_i3c_pad0_word_count = 5U, /**< Reserved padding after MSDVAD. */
  k_ra_i3c_pad1_word_count = 3U, /**< Reserved padding after RSTCTL. */
} ra_i3c_limits_t;

/**
 * @struct r_i3c_regs_t
 * @brief Minimal I3C register window (partial).
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is accessed via ``ra_i3c()`` in
 * ``libs/ra_hal/src/ra_i3c.c`` once the full command engine
 * lands.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  volatile uint32_t PRTS;   /**< +0x00 Protocol Selection. */
  volatile uint32_t BCTL;   /**< +0x04 Bus Control. */
  volatile uint32_t MSDVAD; /**< +0x08 Master/Slave Device Addr. */
  volatile uint32_t _r0[k_ra_i3c_pad0_word_count];
  volatile uint32_t RSTCTL; /**< +0x20 Reset Control. */
  volatile uint32_t _r1[k_ra_i3c_pad1_word_count];
  volatile uint32_t INST;  /**< +0x30 Interrupt Status. */
  volatile uint32_t INSTE; /**< +0x34 Interrupt Status Enable. */
  volatile uint32_t IE;    /**< +0x38 Interrupt Enable. */
  volatile uint32_t _r2;
  volatile uint32_t BST;  /**< +0x40 Bus Status. */
  volatile uint32_t BSTE; /**< +0x44 Bus Status Enable. */
  volatile uint32_t BIE;  /**< +0x48 Bus Interrupt Enable. */
} r_i3c_regs_t;
/* cppcheck-suppress-end [unusedStructMember] */

/** @brief Get pointer to the single I3C instance. */
static inline volatile r_i3c_regs_t* ra_i3c(void)
{
  return (volatile r_i3c_regs_t*)k_ra_i3c_base_addr;
}

#ifdef __cplusplus
}
#endif
