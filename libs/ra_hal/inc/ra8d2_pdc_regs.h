/**
 * @file ra8d2_pdc_regs.h
 * @brief Parallel Data Capture (PDC) register layout
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * PDC is the parallel-bus camera bridge. It accepts a parallel
 * pixel stream from a CMOS sensor (PIXCLK + HSYNC + VSYNC + 8-bit
 * data) and DMA-fills a contiguous framebuffer in SRAM/SDRAM via
 * its 22-stage 32-bit FIFO (PCDR). Distinct from CEU (which has
 * its own Y/Cb/Cr separator + scaler).
 *
 * Register window mirrors FSP's R_PDC_Type at base 0x40094000
 * (size 28 bytes = 0x1C). Field layout traced against the RA6
 * CMSIS header (canonical PDC IP block reused across the RA
 * family) and the Renesas FSP r_pdc driver. PDC is NOT exposed
 * via the RA8D2 MSTPCR enum so this header pairs with a driver
 * that does not call ``ra_mstp_enable``; the block is gated
 * implicitly via its CGC peripheral-clock domain instead.
 *
 * Register summary (HUM RA8D2 video chapter, parallel-data path):
 *  - +0x00 PCCR0  -- control: clock divider, sync polarities,
 *                    endian select, PCKO output enable, IE bits
 *                    for FE/DR/OV/UDR/VER/HER, plus PRST.
 *  - +0x04 PCCR1  -- operation enable (PCE).
 *  - +0x08 PCSR   -- status flags (FBSY/FEMPF/FEF/OVRF/UDRF/
 *                    VERF/HERF). FEF/OVRF/UDRF/VERF/HERF are
 *                    write-1-clear; FBSY/FEMPF are read-only.
 *  - +0x0C PCMONR -- live HSYNC / VSYNC pin levels (RO).
 *  - +0x10 PCDR   -- 32-bit FIFO read port (RO, DMAC-driven).
 *  - +0x14 VCR    -- vertical capture window (VST + VSZ).
 *  - +0x18 HCR    -- horizontal capture window (HST + HSZ).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @enum ra_pdc_addr_t
 * @brief Memory-mapped base address for the PDC block.
 */
typedef enum : uintptr_t {
  k_ra_pdc_base_addr = 0x40094000UL, /**< Canonical PDC base (FSP R_PDC_BASE). */
} ra_pdc_addr_t;

/**
 * @enum ra_pdc_pccr0_bit_t
 * @brief Bit positions inside PCCR0.
 */
typedef enum : uint8_t {
  k_ra_pdc_bit_pcke   = 0U,  /**< PCKO output clock enable.            */
  k_ra_pdc_bit_vps    = 1U,  /**< VSYNC polarity select.               */
  k_ra_pdc_bit_hps    = 2U,  /**< HSYNC polarity select.               */
  k_ra_pdc_bit_prst   = 3U,  /**< PDC software reset (write 1, self-clear). */
  k_ra_pdc_bit_dfie   = 4U,  /**< Data-ready (FIFO) interrupt enable.  */
  k_ra_pdc_bit_feie   = 5U,  /**< Frame-end interrupt enable.          */
  k_ra_pdc_bit_ovie   = 6U,  /**< FIFO overrun interrupt enable.       */
  k_ra_pdc_bit_udrie  = 7U,  /**< FIFO underrun interrupt enable.      */
  k_ra_pdc_bit_verie  = 8U,  /**< Vertical setting error int. enable.  */
  k_ra_pdc_bit_herie  = 9U,  /**< Horizontal setting error int. enable.*/
  k_ra_pdc_bit_pckoe  = 10U, /**< PCKO output pin enable.              */
  k_ra_pdc_bit_pckdiv = 11U, /**< PCKO frequency divider field [13:11].*/
  k_ra_pdc_bit_eds    = 14U, /**< Endian select (0 = LE, 1 = BE).      */
} ra_pdc_pccr0_bit_t;

/**
 * @enum ra_pdc_pccr1_bit_t
 * @brief Bit positions inside PCCR1.
 */
typedef enum : uint8_t {
  k_ra_pdc_bit_pce = 0U, /**< PDC operation enable. */
} ra_pdc_pccr1_bit_t;

/**
 * @enum ra_pdc_pcsr_bit_t
 * @brief Bit positions inside PCSR.
 */
typedef enum : uint8_t {
  k_ra_pdc_bit_fbsy  = 0U, /**< Frame busy flag (RO).                 */
  k_ra_pdc_bit_fempf = 1U, /**< FIFO empty flag (RO).                 */
  k_ra_pdc_bit_fef   = 2U, /**< Frame-end flag (W1C).                 */
  k_ra_pdc_bit_ovrf  = 3U, /**< FIFO overrun flag (W1C).              */
  k_ra_pdc_bit_udrf  = 4U, /**< FIFO underrun flag (W1C).             */
  k_ra_pdc_bit_verf  = 5U, /**< Vertical line setting error (W1C).    */
  k_ra_pdc_bit_herf  = 6U, /**< Horizontal byte setting error (W1C).  */
} ra_pdc_pcsr_bit_t;

/**
 * @enum ra_pdc_pcmonr_bit_t
 * @brief Bit positions inside PCMONR.
 */
typedef enum : uint8_t {
  k_ra_pdc_bit_vsync_live = 0U, /**< Live VSYNC pin level. */
  k_ra_pdc_bit_hsync_live = 1U, /**< Live HSYNC pin level. */
} ra_pdc_pcmonr_bit_t;

/**
 * @enum ra_pdc_mask_t
 * @brief Field masks used by the driver.
 */
typedef enum : uint32_t {
  k_ra_pdc_mask_pccr0_pcke   = (1UL << 0),
  k_ra_pdc_mask_pccr0_vps    = (1UL << 1),
  k_ra_pdc_mask_pccr0_hps    = (1UL << 2),
  k_ra_pdc_mask_pccr0_prst   = (1UL << 3),
  k_ra_pdc_mask_pccr0_dfie   = (1UL << 4),
  k_ra_pdc_mask_pccr0_feie   = (1UL << 5),
  k_ra_pdc_mask_pccr0_ovie   = (1UL << 6),
  k_ra_pdc_mask_pccr0_udrie  = (1UL << 7),
  k_ra_pdc_mask_pccr0_verie  = (1UL << 8),
  k_ra_pdc_mask_pccr0_herie  = (1UL << 9),
  k_ra_pdc_mask_pccr0_pckoe  = (1UL << 10),
  k_ra_pdc_mask_pccr0_pckdiv = (0x7UL << 11),
  k_ra_pdc_mask_pccr0_eds    = (1UL << 14),

  k_ra_pdc_mask_pccr1_pce = (1UL << 0),

  k_ra_pdc_mask_pcsr_fbsy  = (1UL << 0),
  k_ra_pdc_mask_pcsr_fempf = (1UL << 1),
  k_ra_pdc_mask_pcsr_fef   = (1UL << 2),
  k_ra_pdc_mask_pcsr_ovrf  = (1UL << 3),
  k_ra_pdc_mask_pcsr_udrf  = (1UL << 4),
  k_ra_pdc_mask_pcsr_verf  = (1UL << 5),
  k_ra_pdc_mask_pcsr_herf  = (1UL << 6),
  /** All write-1-clear bits in PCSR. */
  k_ra_pdc_mask_pcsr_w1c = (1UL << 2) | (1UL << 3) | (1UL << 4) | (1UL << 5) | (1UL << 6),

  k_ra_pdc_mask_vcr_vst = 0x00000FFFUL,
  k_ra_pdc_mask_vcr_vsz = 0x0FFF0000UL,
  k_ra_pdc_mask_hcr_hst = 0x00000FFFUL,
  k_ra_pdc_mask_hcr_hsz = 0x0FFF0000UL,
} ra_pdc_mask_t;

/**
 * @enum ra_pdc_shift_t
 * @brief Field shift counts.
 */
typedef enum : uint8_t {
  k_ra_pdc_shift_pckdiv = 11U,
  k_ra_pdc_shift_size   = 16U, /**< VSZ / HSZ live in [27:16].          */
} ra_pdc_shift_t;

/**
 * @enum ra_pdc_limit_t
 * @brief Range limits for capture-window programming.
 */
typedef enum : uint16_t {
  k_ra_pdc_max_position = 0x0FFFU, /**< 12-bit fields, max 4095.        */
} ra_pdc_limit_t;

/**
 * @struct r_pdc_regs_t
 * @brief PDC register window (FSP R_PDC_Type, size 0x1C).
 *
 * @details
 * cppcheck cannot see tests/, which only ever access these fields
 * through ``ra_pdc()``; the suppression silences the false
 * "unusedStructMember" diagnostic on the register definitions.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  volatile uint32_t PCCR0;  /**< +0x00 Control register 0.   */
  volatile uint32_t PCCR1;  /**< +0x04 Control register 1 (PCE). */
  volatile uint32_t PCSR;   /**< +0x08 Status register.      */
  volatile uint32_t PCMONR; /**< +0x0C Pin monitor register. */
  volatile uint32_t PCDR;   /**< +0x10 FIFO data register.   */
  volatile uint32_t VCR;    /**< +0x14 Vertical capture register. */
  volatile uint32_t HCR;    /**< +0x18 Horizontal capture register. */
} r_pdc_regs_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Get a pointer to the PDC register block.
 * @return Volatile pointer to the PDC register window.
 */
static inline volatile r_pdc_regs_t* ra_pdc(void)
{
  return (volatile r_pdc_regs_t*)k_ra_pdc_base_addr;
}

#ifdef __cplusplus
}
#endif
