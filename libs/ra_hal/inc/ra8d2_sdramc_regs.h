/**
 * @file ra8d2_sdramc_regs.h
 * @brief External SDRAM controller register layout for the Renesas RA8D2
 *
 * @details
 * The RA8D2 has a built-in SDRAMC that drives the 64 MB external
 * SDRAM on the EK-RA8D2 via the parallel bus. Memory-maps the
 * external SDRAM at `k_ra_sdram_base_addr` (0x68000000) per the
 * FSP `BSP_FEATURE_SDRAM_START_ADDRESS`.
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
  /* FSP R_BUS_SDRAM is a sub-block inside R_BUS (R_BUS_BASE=0x40003000)
   * at offset +0xC00, so SDRAMC control registers sit at 0x40003C00.
   * Our previous 0x4003C000 was a digit transposition. */
  k_ra_sdramc_base_addr = 0x40003C00UL, /**< R_BUS.SDRAM sub-block.    */
  k_ra_sdram_base_addr  = 0x68000000UL, /**< External SDRAM window.    */
} ra_sdramc_addr_t;

typedef enum : uint32_t {
  k_ra_sdram_size_bytes = 0x04000000UL, /**< 64 MiB. */
} ra_sdram_geom_t;

/**
 * @struct r_sdramc_regs_t
 * @brief SDRAM controller register window (partial -- extend on demand).
 */
typedef struct {
  volatile uint16_t SDCCR; /**< +0x00 SDC Control Register.           */
  volatile uint16_t _r0;
  volatile uint8_t  SDCMOD; /**< +0x04 Mode Register.                   */
  volatile uint8_t  _r1[3];
  volatile uint8_t  SDAMOD; /**< +0x08 Access Mode.                     */
  volatile uint8_t  _r2[3];
  volatile uint32_t SDTR;  /**< +0x0C Timing.                          */
  volatile uint16_t SDADR; /**< +0x10 Address.                         */
  volatile uint16_t _r3;
  volatile uint16_t SDRFCR; /**< +0x14 Refresh Control.                 */
  volatile uint8_t  SDRFEN; /**< +0x16 Refresh Enable.                  */
  volatile uint8_t  _r4;
  volatile uint8_t  SDICR; /**< +0x18 Initialisation Control.          */
  volatile uint8_t  _r5[3];
  volatile uint16_t SDIR; /**< +0x1C Init Register.                   */
} r_sdramc_regs_t;

/** @brief Get pointer to the SDRAMC register block. */
static inline volatile r_sdramc_regs_t* ra_sdramc(void)
{
  return (volatile r_sdramc_regs_t*)k_ra_sdramc_base_addr;
}

#ifdef __cplusplus
}
#endif
