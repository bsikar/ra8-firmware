/**
 * @file ra8d2_dtc_regs.h
 * @brief DTC (Data Transfer Controller) register layout for the RA8D2
 *
 * @details
 * The DTC is a lighter-weight alternative to the DMAC for moving
 * small amounts of data in response to peripheral interrupts. Its
 * control registers live at `0x4000AC00` and the actual transfer
 * descriptors are stored in SRAM, indexed by the interrupt number.
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
  k_ra_dtc_base_addr = 0x4000AC00UL,
} ra_dtc_addr_t;

/* Register layout verified against FSP R_DTC_Type in
 * R7KA8D2KF_core0.h lines 6585-6732. */
typedef struct {
  volatile uint8_t  DTCCR; /**< +0x00 Control Register.            */
  volatile uint8_t  _r0[3];
  volatile uint32_t DTCVBR;   /**< +0x04 Vector Base Register.        */
  volatile uint8_t  DTCADMOD; /**< +0x08 Address Mode Register.       */
  volatile uint8_t  _r1[3];
  volatile uint8_t  DTCST; /**< +0x0C Module Start Register.       */
  volatile uint8_t  _r2;
  volatile uint16_t DTCSTS;  /**< +0x0E Status Register.             */
  volatile uint8_t  DTCCR_S; /**< +0x10 DTCCR for secure region.     */
  volatile uint8_t  _r3[3];
  volatile uint32_t DTCVBR_S; /**< +0x14 DTCVBR for secure region.    */
} r_dtc_regs_t;

/** @brief Get pointer to the DTC block. */
static inline volatile r_dtc_regs_t* ra_dtc(void)
{
  return (volatile r_dtc_regs_t*)k_ra_dtc_base_addr;
}

#ifdef __cplusplus
}
#endif
