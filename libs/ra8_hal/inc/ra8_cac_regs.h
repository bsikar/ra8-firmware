/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_cac_regs.h
 * @brief Clock Accuracy Check (CAC) register layout for the Renesas RA8D2
 * @ingroup grp_hal_system
 *
 * @details
 * The CAC compares a reference clock (external crystal / pin) against
 * a measurement clock (HOCO / MOCO / PLL output) and raises an error
 * interrupt if the two drift apart by more than a programmed
 * tolerance. Used by the CGC driver to validate that the PLL output
 * is really at the target frequency after bring-up.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum : uintptr_t {
  k_ra8_cac_base_addr = 0x40202400UL, /**< RA8 cac base address. */
} ra8_cac_addr_t;

typedef struct {
  volatile uint8_t  CACR0;   /**< +0x00 Control 0.   */
  volatile uint8_t  CACR1;   /**< +0x01 Control 1.   */
  volatile uint8_t  CACR2;   /**< +0x02 Control 2.   */
  volatile uint8_t  CAICR;   /**< +0x03 Int Control. */
  volatile uint8_t  CASTR;   /**< +0x04 Status.      */
  volatile uint8_t  _r0;     /**< Reserved.          */
  volatile uint16_t CAULVR;  /**< +0x06 Upper Limit. */
  volatile uint16_t CALLVR;  /**< +0x08 Lower Limit. */
  volatile uint16_t CACNTBR; /**< +0x0A Counter Buf. */
} r_cac_regs_t;

/** @brief Get pointer to the CAC block. */
static inline volatile r_cac_regs_t* ra8_cac(void)
{
  return (volatile r_cac_regs_t*)k_ra8_cac_base_addr;
}

#ifdef __cplusplus
}
#endif
