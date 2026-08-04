/**
 * @file ra8_sdramc_regs.h
 * @brief External SDRAM controller register layout for the Renesas RA8D2
 * @ingroup grp_hal_memory
 *
 * @details
 * The RA8D2 has a built-in SDRAMC that drives the external SDRAM on
 * the EK-RA8D2 via the parallel bus. The board fits an ISSI
 * IS42S32160F -- a 512 Mbit (64 MB) SDR SDRAM organised as 16M x 32
 * bits, 13 row / 9 column / 4 bank -- memory-mapped at
 * `k_ra8_sdram_base_addr` (0x68000000).
 *
 * Register offsets follow the RA8D2 Hardware User's Manual Ch 15
 * "Buses" (the SDRAMC sub-block of R_BUS) and the FSP CMSIS device
 * header `R_BUS->SDRAM`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

typedef enum : uintptr_t {
  /* R_BUS_SDRAM is a sub-block inside R_BUS (R_BUS_BASE = 0x40003000)
   * at offset +0xC00, so the SDRAMC control registers sit at
   * 0x40003C00. */
  k_ra8_sdramc_base_addr = 0x40003C00UL, /**< R_BUS.SDRAM sub-block. */
  k_ra8_sdram_base_addr  = 0x68000000UL, /**< External SDRAM window. */
  /* SDRAM Clock Output Control Register lives in R_SYSTEM (0x4001E000)
   * at offset 0x53; it gates the SDCLK output to the external device.
   * HUM Ch 9 "Clock Generation Circuit". */
  k_ra8_system_sdckocr_addr = 0x4001E053UL, /**< R_SYSTEM.SDCKOCR. */
} ra8_sdramc_addr_t;

typedef enum : uint32_t {
  k_ra8_sdram_size_bytes = 0x04000000UL, /**< 64 MiB. */
} ra8_sdram_geom_t;

/**
 * @enum ra8_sdramc_off_t
 * @brief Register offsets within the SDRAMC sub-block.
 *
 * @details Each value is the byte offset of a register from
 * ::k_ra8_sdramc_base_addr per HUM Ch 15.3.11..15.3.22. The
 * ::r_sdramc_regs_t reserved-field sizes and its layout
 * static_asserts are both derived from these, so the struct carries
 * no bare offset literals.
 */
typedef enum : uint8_t {
  k_ra8_sdramc_off_sdccr  = 0x00U, /**< SDCCR  -- SDC Control.           */
  k_ra8_sdramc_off_sdcmod = 0x01U, /**< SDCMOD -- SDC Mode.              */
  k_ra8_sdramc_off_sdamod = 0x02U, /**< SDAMOD -- SDRAM Access Mode.     */
  k_ra8_sdramc_off_sdself = 0x10U, /**< SDSELF -- Self-Refresh Control.  */
  k_ra8_sdramc_off_sdrfcr = 0x14U, /**< SDRFCR -- Refresh Control.       */
  k_ra8_sdramc_off_sdrfen = 0x16U, /**< SDRFEN -- Auto-Refresh Control.  */
  k_ra8_sdramc_off_sdicr  = 0x20U, /**< SDICR  -- Init Sequence Control. */
  k_ra8_sdramc_off_sdir   = 0x24U, /**< SDIR   -- Initialization.        */
  k_ra8_sdramc_off_sdadr  = 0x40U, /**< SDADR  -- Address.               */
  k_ra8_sdramc_off_sdtr   = 0x44U, /**< SDTR   -- Timing.                */
  k_ra8_sdramc_off_sdmod  = 0x48U, /**< SDMOD  -- Mode.                  */
  k_ra8_sdramc_off_sdsr   = 0x50U, /**< SDSR   -- Status (RO).           */
} ra8_sdramc_off_t;

/**
 * @struct r_sdramc_regs_t
 * @brief SDRAM controller register window.
 *
 * @details Offsets verified against HUM Ch 15 "Buses" and the FSP
 * CMSIS `R_BUS->SDRAM` definition. Reserved-field sizes are computed
 * from ::ra8_sdramc_off_t, so a register-type change is caught by the
 * layout static_asserts below.
 *
 * @invariant SDTR sits at +0x44 and SDSR at +0x50.
 */
typedef struct {
  volatile uint8_t SDCCR;  /**< SDC Control Register.       */
  volatile uint8_t SDCMOD; /**< SDC Mode Register.          */
  volatile uint8_t SDAMOD; /**< SDRAM Access Mode Register. */
  /** Reserved. */
  volatile uint8_t _rsv0[k_ra8_sdramc_off_sdself - k_ra8_sdramc_off_sdamod - 1];
  volatile uint8_t SDSELF; /**< SDRAM Self-Refresh Control. */
  /** Reserved. */
  volatile uint8_t  _rsv1[k_ra8_sdramc_off_sdrfcr - k_ra8_sdramc_off_sdself - 1];
  volatile uint16_t SDRFCR; /**< SDRAM Refresh Control.      */
  volatile uint8_t  SDRFEN; /**< SDRAM Auto-Refresh Control. */
  /** Reserved. */
  volatile uint8_t _rsv2[k_ra8_sdramc_off_sdicr - k_ra8_sdramc_off_sdrfen - 1];
  volatile uint8_t SDICR; /**< Init Sequence Control. */
  /** Reserved. */
  volatile uint8_t  _rsv3[k_ra8_sdramc_off_sdir - k_ra8_sdramc_off_sdicr - 1];
  volatile uint16_t SDIR; /**< SDRAM Initialization Register. */
  /** Reserved. */
  volatile uint8_t _rsv4[k_ra8_sdramc_off_sdadr - k_ra8_sdramc_off_sdir - 2];
  volatile uint8_t SDADR; /**< SDRAM Address Register. */
  /** Reserved. */
  volatile uint8_t  _rsv5[k_ra8_sdramc_off_sdtr - k_ra8_sdramc_off_sdadr - 1];
  volatile uint32_t SDTR;  /**< SDRAM Timing Register. */
  volatile uint16_t SDMOD; /**< SDRAM Mode Register.   */
  /** Reserved. */
  volatile uint8_t _rsv6[k_ra8_sdramc_off_sdsr - k_ra8_sdramc_off_sdmod - 2];
  volatile uint8_t SDSR; /**< SDRAM Status Register (RO). */
} r_sdramc_regs_t;

static_assert(offsetof(r_sdramc_regs_t, SDTR) == (size_t)k_ra8_sdramc_off_sdtr,
              "SDRAMC SDTR offset");
static_assert(offsetof(r_sdramc_regs_t, SDSR) == (size_t)k_ra8_sdramc_off_sdsr,
              "SDRAMC SDSR offset");
static_assert(offsetof(r_sdramc_regs_t, SDICR) == (size_t)k_ra8_sdramc_off_sdicr,
              "SDRAMC SDICR offset");
static_assert(offsetof(r_sdramc_regs_t, SDADR) == (size_t)k_ra8_sdramc_off_sdadr,
              "SDRAMC SDADR offset");

/** @brief Get pointer to the SDRAMC register block. */
static inline volatile r_sdramc_regs_t* ra8_sdramc(void)
{
  return (volatile r_sdramc_regs_t*)k_ra8_sdramc_base_addr;
}

/** @brief Get pointer to the R_SYSTEM SDCLK-output control register. */
static inline volatile uint8_t* ra8_sdramc_sdckocr(void)
{
  return (volatile uint8_t*)k_ra8_system_sdckocr_addr;
}

#ifdef __cplusplus
}
#endif
