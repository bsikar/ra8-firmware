/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_mipi_phy_regs.h
 * @brief MIPI D-PHY register layout for the RA8D2
 * @ingroup grp_hal_display
 *
 * @details
 * The MIPI PHY block at MMIO base ``0x4034_6C00`` is the physical
 * layer shared by the MIPI DSI host (Ch 65) and the MIPI CSI
 * receiver (Ch 66). It implements MIPI Alliance Specification
 * v2.1 D-PHY with up to 2 lanes at a maximum line rate of
 * 720 Mbps/lane (HUM Ch 64.1 "Overview" Table 64.1, p 3822).
 *
 * The block exposes a small set of 32-bit registers (HUM Ch 64.2
 * "Register Description" notes "Access the following registers
 * only in units of 32 bits.", p 3822):
 *
 *  | Offset | Name      | Purpose                              |
 *  |-------:|-----------|--------------------------------------|
 *  | 0x000  | DPHYREFCR | Reference clock setting (PCLKA MHz)  |
 *  | 0x004  | DPHYPLFCR | PLL frequency control (I/N/NF/P)     |
 *  | 0x008  | DPHYPLOCR | PLL operation control (PLLSTP)       |
 *  | 0x00C  | DPHYESCCR | Escape mode clock divisor (ESCDIV)   |
 *  | 0x010  | DPHYPWRCR | LDO power supply control (PWRSEN)    |
 *  | 0x01C  | DPHYSFR   | Status flags (PWRSF, PLLSF) -- RO    |
 *  | 0x020  | DPHYOCR   | Operation control (DPHYEN)           |
 *  | 0x024  | DPHYTIM1  | TINIT timing                         |
 *  | 0x028  | DPHYTIM2  | TCLK_PREP/SETT/MISS timing           |
 *  | 0x02C  | DPHYTIM3  | THS_PREP/SETT timing                 |
 *  | 0x030  | DPHYTIM4  | TCLK_ZERO/PRE/POST/TRAIL timing      |
 *  | 0x034  | DPHYTIM5  | THS_ZERO/TRAIL/EXIT timing           |
 *  | 0x038  | DPHYTIM6  | TLPX timing                          |
 *  | 0x048  | DPHYMDC   | Host/device mode (DSI vs CSI)       |
 *
 * Cross-checked against FSP ``R_MIPI_PHY`` register list in
 * ``r_mipi_phy.c`` and the bit-field definitions in the FSP
 * ``r_mipi_phy.h`` instance header.
 *
 * Note: ``k_ra8_mipi_phy_base_addr`` is already declared in
 * ``ra8_glcdc_regs.h`` as part of the ``ra8_display_addr_t``
 * enum that groups the GLCDC + MIPI + CEU bases. This header
 * uses that constant rather than redefining it.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_glcdc_regs.h" /* k_ra8_mipi_phy_base_addr */

/**
 * @enum ra8_mipi_phy_block_off_t
 * @brief 32-bit register offsets inside the MIPI PHY window.
 *
 * @details
 * Names mirror the HUM register acronyms (DPHYxxx). Verified
 * against HUM Ch 64.2 sections 64.2.1 .. 64.2.14, p 3822-3837.
 */
typedef enum : uint16_t {
  k_ra8_mipi_phy_off_refcr = 0x000U, /**< +0x000 DPHYREFCR -- ref clock cfg.  */
  k_ra8_mipi_phy_off_plfcr = 0x004U, /**< +0x004 DPHYPLFCR -- PLL freq ctrl.  */
  k_ra8_mipi_phy_off_plocr = 0x008U, /**< +0x008 DPHYPLOCR -- PLL op ctrl.    */
  k_ra8_mipi_phy_off_esccr = 0x00CU, /**< +0x00C DPHYESCCR -- escape clk div. */
  k_ra8_mipi_phy_off_pwrcr = 0x010U, /**< +0x010 DPHYPWRCR -- LDO pwr supply. */
  k_ra8_mipi_phy_off_sfr   = 0x01CU, /**< +0x01C DPHYSFR   -- status flags.   */
  k_ra8_mipi_phy_off_ocr   = 0x020U, /**< +0x020 DPHYOCR   -- D-PHY enable.   */
  k_ra8_mipi_phy_off_tim1  = 0x024U, /**< +0x024 DPHYTIM1  -- TINIT.          */
  k_ra8_mipi_phy_off_tim2  = 0x028U, /**< +0x028 DPHYTIM2  -- TCLK PREP/SETT. */
  k_ra8_mipi_phy_off_tim3  = 0x02CU, /**< +0x02C DPHYTIM3  -- THS PREP/SETT.  */
  k_ra8_mipi_phy_off_tim4  = 0x030U, /**< +0x030 DPHYTIM4  -- TCLK ZERO/PRE.  */
  k_ra8_mipi_phy_off_tim5  = 0x034U, /**< +0x034 DPHYTIM5  -- THS ZERO/TRL.   */
  k_ra8_mipi_phy_off_tim6  = 0x038U, /**< +0x038 DPHYTIM6  -- TLPX.           */
  k_ra8_mipi_phy_off_mdc   = 0x048U, /**< +0x048 DPHYMDC   -- host/device.    */
} ra8_mipi_phy_block_off_t;

/**
 * @enum ra8_mipi_phy_pwrcr_bit_t
 * @brief DPHYPWRCR bit positions / masks.
 *
 * @details
 * HUM Ch 64.2.5 "DPHYPWRCR : D-PHY Power Supplying Control
 * Register" p 3826: bit 0 = PWRSEN (0 = stop LDO, 1 = enable LDO).
 */
typedef enum : uint32_t {
  k_ra8_mipi_phy_pwrcr_pwrsen = 0x1U, /**< Bit 0 mask: enable D-PHY LDO. */
} ra8_mipi_phy_pwrcr_bit_t;

/**
 * @enum ra8_mipi_phy_plocr_bit_t
 * @brief DPHYPLOCR bit positions / masks.
 *
 * @details
 * HUM Ch 64.2.3 "DPHYPLOCR : D-PHY PLL Operation Control Register"
 * p 3824: bit 0 = PLLSTP (0 = run PLL, 1 = stop PLL). Reset = 1.
 */
typedef enum : uint32_t {
  k_ra8_mipi_phy_plocr_pllstp = 0x1U, /**< Bit 0 mask: stop the PLL. */
} ra8_mipi_phy_plocr_bit_t;

/**
 * @enum ra8_mipi_phy_ocr_bit_t
 * @brief DPHYOCR bit positions / masks.
 *
 * @details
 * HUM Ch 64.2.7 "DPHYOCR : D-PHY Operation Control Register"
 * p 3827: bit 0 = DPHYEN (0 = D-PHY off, 1 = D-PHY on).
 */
typedef enum : uint32_t {
  k_ra8_mipi_phy_ocr_dphyen = 0x1U, /**< Bit 0 mask: enable D-PHY. */
} ra8_mipi_phy_ocr_bit_t;

/**
 * @enum ra8_mipi_phy_sfr_bit_t
 * @brief DPHYSFR status flag positions / masks (read-only).
 *
 * @details
 * HUM Ch 64.2.6 "DPHYSFR : D-PHY Status Flag Register" p 3826:
 *  - bit 0  (PWRSF): D-PHY LDO power-on stable.
 *  - bit 8  (PLLSF): D-PHY PLL oscillation stable.
 *
 * The full status mask combines both flags (used by status-get
 * helpers that want to assert "PHY fully ready").
 */
typedef enum : uint32_t {
  k_ra8_mipi_phy_sfr_pwrsf      = 0x001U, /**< Bit 0 mask: LDO stable.       */
  k_ra8_mipi_phy_sfr_pllsf      = 0x100U, /**< Bit 8 mask: PLL clock stable. */
  k_ra8_mipi_phy_sfr_ready_mask = 0x101U, /**< PWRSF | PLLSF.                */
} ra8_mipi_phy_sfr_bit_t;

/**
 * @enum ra8_mipi_phy_mdc_bit_t
 * @brief DPHYMDC bit positions / masks.
 *
 * @details
 * HUM Ch 64.2.14 "DPHYMDC : D-PHY Mode Control Register" p 3836:
 *  - bit 0 (MASTEREN): 0 = device (CSI-2), 1 = host (DSI).
 */
typedef enum : uint32_t {
  k_ra8_mipi_phy_mdc_hosten = 0x1U, /**< DPHYMDC.MASTEREN bit 0: host (DSI) mode. */
} ra8_mipi_phy_mdc_bit_t;

/**
 * @enum ra8_mipi_phy_refcr_mask_t
 * @brief DPHYREFCR field masks.
 *
 * @details
 * HUM Ch 64.2.1 "DPHYREFCR : D-PHY Reference Clock Setting Register"
 * p 3822 documents the encoding as a strict ``RFREQ = pclka_mhz - 1``
 * mapping: ``0x27 = 40 MHz``, ``0x28 = 41 MHz``, ..., ``0x7C = 125 MHz``.
 * PCLKA must be >= 40 MHz to use MIPI DSI. Driver code MUST subtract
 * one before writing -- this matches FSP ``r_mipi_phy.c`` which writes
 * ``(pclka_hz / 1MHz) - 1``.
 */
typedef enum : uint32_t {
  k_ra8_mipi_phy_refcr_rfreq_mask = 0xFFU, /**< RFREQ[7:0] field. */
} ra8_mipi_phy_refcr_mask_t;

/**
 * @enum ra8_mipi_phy_refcr_bias_t
 * @brief Software-side bias used when encoding DPHYREFCR.RFREQ.
 *
 * @details
 * HUM Ch 64.2.1 p 3822: hardware encoding subtracts one from the
 * frequency in MHz before storing it in RFREQ[7:0]. Use this constant
 * everywhere the driver converts ``pclka_mhz`` to a register value.
 */
typedef enum : uint8_t {
  k_ra8_mipi_phy_refcr_rfreq_bias = 1U, /**< RFREQ encodes (MHz - 1). */
} ra8_mipi_phy_refcr_bias_t;

/**
 * @enum ra8_mipi_phy_esccr_mask_t
 * @brief DPHYESCCR field masks.
 *
 * @details
 * HUM Ch 64.2.4 "DPHYESCCR : D-PHY Escape Mode Clock Control
 * Register" p 3825: ESCDIV[4:0] controls the LP-state clock
 * divisor (1 .. 32). Must be set while the PLL is stopped.
 */
typedef enum : uint32_t {
  k_ra8_mipi_phy_esccr_escdiv_mask = 0x1FU, /**< ESCDIV[4:0] field. */
} ra8_mipi_phy_esccr_mask_t;

/**
 * @enum ra8_mipi_phy_tim1_mask_t
 * @brief DPHYTIM1 field masks.
 *
 * @details
 * HUM Ch 64.2.8 "DPHYTIM1 : D-PHY Timing Control Register 1" p 3827:
 * TINIT[18:0] = (TINIT count) where T_INIT = (TINIT + 1) * PCLKA.
 */
typedef enum : uint32_t {
  k_ra8_mipi_phy_tim1_tinit_mask = 0x0007FFFFUL, /**< TINIT[18:0] field. */
} ra8_mipi_phy_tim1_mask_t;

/**
 * @enum ra8_mipi_phy_tim_byte_shift_t
 * @brief Generic 8-bit byte-lane shifts used inside DPHYTIM2..5.
 *
 * @details
 * Many DPHYTIMx registers pack three / four 8-bit fields into the
 * 32-bit word. These shifts replace the magic numbers 0/8/16/24.
 */
typedef enum : uint8_t {
  k_ra8_mipi_phy_tim_shift_b0 = 0U,  /**< Byte 0 (bits 7:0).   */
  k_ra8_mipi_phy_tim_shift_b1 = 8U,  /**< Byte 1 (bits 15:8).  */
  k_ra8_mipi_phy_tim_shift_b2 = 16U, /**< Byte 2 (bits 23:16). */
  k_ra8_mipi_phy_tim_shift_b3 = 24U, /**< Byte 3 (bits 31:24). */
} ra8_mipi_phy_tim_byte_shift_t;

/**
 * @enum ra8_mipi_phy_tim_byte_mask_t
 * @brief 8-bit per-field mask applied before shifting in DPHYTIM2..5.
 */
typedef enum : uint32_t {
  k_ra8_mipi_phy_tim_byte_mask = 0xFFU, /**< Mask for each 8-bit lane. */
} ra8_mipi_phy_tim_byte_mask_t;

/**
 * @enum ra8_mipi_phy_tim6_mask_t
 * @brief DPHYTIM6 field masks.
 *
 * @details
 * HUM Ch 64.2.13 "DPHYTIM6 : D-PHY Timing Control Register 6"
 * p 3830: TLPX[7:0] -- only one byte-wide field, upper 24 bits
 * are reserved.
 */
typedef enum : uint32_t {
  k_ra8_mipi_phy_tim6_tlpx_mask = 0xFFU, /**< TLPX[7:0] field. */
} ra8_mipi_phy_tim6_mask_t;

/**
 * @enum ra8_mipi_phy_pll_field_shift_t
 * @brief DPHYPLFCR field positions.
 *
 * @details
 * HUM Ch 64.2.2 "DPHYPLFCR : D-PHY PLL Frequency Control Register"
 * p 3823:
 *  - IDIV   [1:0]   @ shift 0
 *  - NFMUL  [1:0]   @ shift 8
 *  - PMUL   [1:0]   @ shift 12
 *  - NMUL   [8:0]   @ shift 16
 */
typedef enum : uint8_t {
  k_ra8_mipi_phy_plfcr_shift_idiv  = 0U,  /**< IDIV[1:0]    field shift. */
  k_ra8_mipi_phy_plfcr_shift_nfmul = 8U,  /**< NFMUL[1:0]   field shift. */
  k_ra8_mipi_phy_plfcr_shift_pmul  = 12U, /**< PMUL[1:0]    field shift. */
  k_ra8_mipi_phy_plfcr_shift_nmul  = 16U, /**< NMUL[8:0]    field shift. */
} ra8_mipi_phy_pll_field_shift_t;

/**
 * @enum ra8_mipi_phy_pll_field_mask_t
 * @brief DPHYPLFCR field width masks (pre-shift, applied to value).
 */
typedef enum : uint32_t {
  k_ra8_mipi_phy_plfcr_mask_idiv  = 0x003U, /**< IDIV[1:0]   mask. */
  k_ra8_mipi_phy_plfcr_mask_nfmul = 0x003U, /**< NFMUL[1:0]  mask. */
  k_ra8_mipi_phy_plfcr_mask_pmul  = 0x003U, /**< PMUL[1:0]   mask. */
  k_ra8_mipi_phy_plfcr_mask_nmul  = 0x1FFU, /**< NMUL[8:0]   mask. */
} ra8_mipi_phy_pll_field_mask_t;

/**
 * @enum ra8_mipi_phy_lane_speed_limit_t
 * @brief Per-lane line-rate guardrails (Mbps).
 *
 * @details
 * HUM Ch 64.1 Table 64.1 p 3822: D-PHY supports 80 Mbps to 720 Mbps
 * per lane. The driver clamps the requested speed into this range
 * before computing PLL coefficients.
 */
typedef enum : uint16_t {
  k_ra8_mipi_phy_line_rate_min_mbps = 80U,  /**< Lower line-rate limit. */
  k_ra8_mipi_phy_line_rate_max_mbps = 720U, /**< Upper line-rate limit. */
} ra8_mipi_phy_lane_speed_limit_t;

/**
 * @enum ra8_mipi_phy_pll_band_t
 * @brief VCO frequency band guardrails (HUM Ch 64.1 Table 64.1 p 3822).
 *
 * @details
 * The PLL VCO must oscillate between 960 MHz and 3000 MHz. The
 * post-divider band depends on PMUL[1:0]:
 *
 *  - P = 1   : 960 .. 1440 MHz
 *  - P = 1/2 : 480 .. 1440 MHz
 *  - P = 1/4 : 240 .. 750  MHz
 *  - P = 1/8 : 120 .. 375  MHz
 *
 * Used by the driver-side band validator to reject impossible
 * combinations of NMUL_int / NFMUL / PMUL before they ever reach
 * the silicon.
 */
typedef enum : uint16_t {
  k_ra8_mipi_phy_vco_min_mhz = 960U,  /**< VCO lower bound (post NF+N). */
  k_ra8_mipi_phy_vco_max_mhz = 3000U, /**< VCO upper bound (post NF+N). */
  k_ra8_mipi_phy_pll_p1_min  = 960U,  /**< P=1   floor.                 */
  k_ra8_mipi_phy_pll_p1_max  = 1440U, /**< P=1   ceiling.               */
  k_ra8_mipi_phy_pll_p2_min  = 480U,  /**< P=1/2 floor.                 */
  k_ra8_mipi_phy_pll_p2_max  = 1440U, /**< P=1/2 ceiling.               */
  k_ra8_mipi_phy_pll_p4_min  = 240U,  /**< P=1/4 floor.                 */
  k_ra8_mipi_phy_pll_p4_max  = 750U,  /**< P=1/4 ceiling.               */
  k_ra8_mipi_phy_pll_p8_min  = 120U,  /**< P=1/8 floor.                 */
  k_ra8_mipi_phy_pll_p8_max  = 375U,  /**< P=1/8 ceiling.               */
} ra8_mipi_phy_pll_band_t;

/**
 * @enum ra8_mipi_phy_input_clk_t
 * @brief Input clock guardrails to PLL (HUM Ch 64.1 Table 64.1 p 3822).
 *
 * @details
 * The MOSC reference must be 8..48 MHz, and after IDIV division
 * the PLL input must land in 8..24 MHz.
 */
typedef enum : uint8_t {
  k_ra8_mipi_phy_mosc_min_mhz   = 8U,  /**< MOSC lower bound.                     */
  k_ra8_mipi_phy_mosc_max_mhz   = 48U, /**< MOSC upper bound.                     */
  k_ra8_mipi_phy_pll_in_min_mhz = 8U,  /**< Post-IDIV lower bound.                */
  k_ra8_mipi_phy_pll_in_max_mhz = 24U, /**< Post-IDIV upper bound.                */
  k_ra8_mipi_phy_lp_clk_min_mhz = 2U,  /**< fLPX lower bound (HUM 64.2.4 p 3825). */
  k_ra8_mipi_phy_lp_clk_max_mhz = 17U, /**< fLPX upper bound (HUM 64.2.4 p 3825). */
} ra8_mipi_phy_input_clk_t;

/**
 * @brief Pointer to a 32-bit MIPI PHY register at the given offset.
 *
 * @param[in] offset Register offset from ``ra8_mipi_phy_block_off_t``.
 * @return Volatile pointer to the 32-bit register.
 *
 * @note Per HUM Ch 64.2 p 3822, all MIPI PHY registers must be
 *       accessed in 32-bit units. Do not use byte / halfword writes.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile uint32_t* ra8_mipi_phy_reg32(ra8_mipi_phy_block_off_t offset)
{
  return (volatile uint32_t*)(k_ra8_mipi_phy_base_addr + (uintptr_t)offset);
}

#ifdef __cplusplus
}
#endif
