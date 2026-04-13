/**
 * @file ra8d2_ether_regs.h
 * @brief Ethernet controller base addresses for the Renesas RA8D2
 *
 * @details
 * RA8D2 has a gigabit Ethernet subsystem composed of several blocks
 * (GMAC A/B, MII forwarder, TSNSW, GPTP). Base addresses below come
 * from R7KA8D2KF. Drivers land with a real network stack.
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
  k_ra_etha0_base_addr = 0x403CA000UL,
  k_ra_etha1_base_addr = 0x403CC000UL,
  k_ra_rmac0_base_addr = 0x403CB000UL,
  k_ra_rmac1_base_addr = 0x403CD000UL,
  k_ra_mfwd_base_addr  = 0x403C0000UL,
  k_ra_gptp_base_addr  = 0x403E0000UL,
  k_ra_eswm_base_addr  = 0x403C8000UL,
  k_ra_gwca0_base_addr = 0x403CE000UL,
} ra_ether_addr_t;

/**
 * @struct r_eswm_regs_t
 * @brief Minimal Ethernet Switch Module (ESWM) register window.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is accessed via ``ra_eswm()`` in
 * ``libs/ra_hal/src/ra_eth.c``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  volatile uint32_t ESWM_CTRL; /**< +0x00 Control.              */
  volatile uint32_t ESWM_STS;  /**< +0x04 Status.               */
  volatile uint32_t ESWM_IE;   /**< +0x08 Interrupt Enable.     */
  volatile uint32_t ESWM_ICLR; /**< +0x0C Interrupt Clear.      */
} r_eswm_regs_t;
/* cppcheck-suppress-end [unusedStructMember] */

/** @brief Get pointer to the ESWM register block. */
static inline volatile r_eswm_regs_t* ra_eswm(void)
{
  return (volatile r_eswm_regs_t*)k_ra_eswm_base_addr;
}

#ifdef __cplusplus
}
#endif
