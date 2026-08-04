/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_cgc_regs.h
 * @brief Clock Generation Circuit (CGC) field layouts for the Renesas RA8D2
 * @ingroup grp_hal_system
 *
 * @details
 * The CGC is logically a separate peripheral but physically lives
 * inside the SYSC register block (`ra8_system_regs.h`). This header
 * provides the bit-field encodings, divider ratios and clock-source
 * selectors needed to programme the PLL and clock tree from
 * `ra8_cgc.c`.
 *
 * ## Registers covered
 *
 * | Reg       | Offset | Width | Notes                                 |
 * |-----------|-------:|------:|---------------------------------------|
 * | SCKDIVCR  |  0x020 | 32    | PCLKA..E, BCLK, ICLK, FCLK dividers   |
 * | SCKDIVCR2 |  0x024 | 16    | CPUCLK0, CPUCLK1, NPUCLK, MRICLK      |
 * | SCKSCR    |  0x026 | 8     | System clock source select (CKSEL)    |
 * | PLLCR     |  0x02A | 8     | PLL1 stop control                     |
 * | PLLCCR    |  0x0AC | 32    | PLL1 multiplier / divider / source    |
 * | HOCOCR    |  0x036 | 8     | HOCO stop control (HCSTP)             |
 * | MOSCWTCR  |  0x0A2 | 8     | Main osc wait cycles                  |
 *
 * Full field layouts come from the RA8D2 Hardware User's Manual
 * section 10 ("Clock Generation Circuit").
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* =============================================================================
 * SCKDIVCR (System Clock Division Control Register) bit fields
 * =============================================================================
 *
 * SCKDIVCR is 32 bits wide. Each nibble selects a divider for one of
 * the clock domains. The divider code-to-ratio map below comes from
 * HUM section 10.2.2.
 */

/**
 * @enum ra8_sckdivcr_shift_t
 * @brief Bit shifts for the divider nibbles in SCKDIVCR.
 */
typedef enum : uint8_t {
  k_ra8_sckdivcr_pckd_shift = 0U,  /**< PCLKD divider nibble @ [3:0].   */
  k_ra8_sckdivcr_pckc_shift = 4U,  /**< PCLKC divider nibble @ [7:4].   */
  k_ra8_sckdivcr_pckb_shift = 8U,  /**< PCLKB divider nibble @ [11:8].  */
  k_ra8_sckdivcr_pcka_shift = 12U, /**< PCLKA divider nibble @ [15:12]. */
  k_ra8_sckdivcr_bck_shift  = 16U, /**< BCLK  divider nibble @ [19:16]. */
  k_ra8_sckdivcr_pcke_shift = 20U, /**< PCLKE divider nibble @ [23:20]. */
  k_ra8_sckdivcr_ick_shift  = 24U, /**< ICLK  divider nibble @ [27:24]. */
  k_ra8_sckdivcr_fck_shift  = 28U, /**< FCLK  divider nibble @ [31:28]. */
} ra8_sckdivcr_shift_t;

/**
 * @enum ra8_clock_div_t
 * @brief Divider codes written into SCKDIVCR nibbles.
 *
 * @details
 * The RA8 SCKDIVCR nibble encodes divide-by-2^N where the nibble value
 * is N, except that `0` means divide-by-1. See HUM Table 10.x for the
 * full list; we only expose the values the project uses.
 */
typedef enum : uint8_t {
  k_ra8_clock_div_1  = 0x0U, /**< Divide by 1.  */
  k_ra8_clock_div_2  = 0x1U, /**< Divide by 2.  */
  k_ra8_clock_div_4  = 0x2U, /**< Divide by 4.  */
  k_ra8_clock_div_8  = 0x3U, /**< Divide by 8.  */
  k_ra8_clock_div_16 = 0x4U, /**< Divide by 16. */
  k_ra8_clock_div_32 = 0x5U, /**< Divide by 32. */
  k_ra8_clock_div_64 = 0x6U, /**< Divide by 64. */
} ra8_clock_div_t;

/* =============================================================================
 * SCKDIVCR2 bit fields (CPU clocks + MRAM clock)
 * =============================================================================
 */

/**
 * @enum ra8_sckdivcr2_shift_t
 * @brief Bit shifts for the divider nibbles in SCKDIVCR2.
 */
typedef enum : uint8_t {
  k_ra8_sckdivcr2_cpuclk0_shift = 0U,  /**< CPUCLK0 (M85) divider @ [3:0].   */
  k_ra8_sckdivcr2_cpuclk1_shift = 4U,  /**< CPUCLK1 (M33) divider @ [7:4].   */
  k_ra8_sckdivcr2_npuclk_shift  = 8U,  /**< NPUCLK        divider @ [11:8].  */
  k_ra8_sckdivcr2_mriclk_shift  = 12U, /**< MRAM bus clk  divider @ [15:12]. */
} ra8_sckdivcr2_shift_t;

/* =============================================================================
 * SCKSCR (System Clock Source Control Register)
 * =============================================================================
 */

/**
 * @enum ra8_cksel_t
 * @brief System clock source selector values written to SCKSCR.CKSEL.
 */
typedef enum : uint8_t {
  k_ra8_cksel_hoco  = 0U, /**< High-speed on-chip oscillator. */
  k_ra8_cksel_moco  = 1U, /**< Middle-speed on-chip osc.      */
  k_ra8_cksel_loco  = 2U, /**< Low-speed on-chip osc.         */
  k_ra8_cksel_main  = 3U, /**< Main oscillator (XTAL).        */
  k_ra8_cksel_subck = 4U, /**< Sub-clock osc.                 */
  k_ra8_cksel_pll1  = 5U, /**< PLL1 output.                   */
  k_ra8_cksel_pll2  = 6U, /**< PLL2 output.                   */
} ra8_cksel_t;

/* =============================================================================
 * PLLCCR (PLL1 Clock Control Register) -- RA8D2 layout
 * =============================================================================
 *
 * Cited from FSP CMSIS device header `R7KA8D2KF_core0.h` and
 * FSP `bsp_clocks.c`:
 *   - PLIDIV   [1:0]  -- input divider code
 *                       (0=/1, 1=/2, 2=/3, 3=/4, ... ratio = code+1 for /1../6)
 *   - PLSRCSEL [4]    -- PLL input source (0 = Main / XTAL, 1 = HOCO)
 *   - PLLMULNF [7:6]  -- fractional multiplier in 0.25 steps
 *   - PLLMUL   [16:8] -- integer multiplier (9 bits, 1..511)
 *
 * Composite multiplier = PLLMUL + (PLLMULNF / 4). FSP packs the value
 * as `(integer_mul * 4 + fraction_quarters) << 6`, which puts the
 * integer part in [16:8] and the fraction in [7:6] simultaneously.
 *
 * For the EK-RA8D2 quickstart target (XTAL=24 MHz, /3 in, x250.00,
 * P=/2, Q=/6, R=/5): PLLCCR = 0xFA02.
 */

/**
 * @enum ra8_pllccr_shift_t
 * @brief Field shifts inside the 32-bit PLLCCR.
 */
typedef enum : uint8_t {
  k_ra8_pllccr_shift_plidiv   = 0U, /**< PLIDIV   field starts at bit 0. */
  k_ra8_pllccr_shift_plsrcsel = 4U, /**< PLSRCSEL field is bit 4.        */
  k_ra8_pllccr_shift_quarters = 6U, /**< (mul * 4) goes here.            */
} ra8_pllccr_shift_t;

/**
 * @enum ra8_pllccr_mask_t
 * @brief Bitfield masks (post-shift) inside PLLCCR.
 */
typedef enum : uint32_t {
  k_ra8_pllccr_mask_plidiv   = 0x00000003UL, /**< PLIDIV   2-bit field. */
  k_ra8_pllccr_mask_quarters = 0x000007FFUL, /**< 11-bit (mul*4) field. */
} ra8_pllccr_mask_t;

/**
 * @enum ra8_plidiv_t
 * @brief PLL input divider codes for PLIDIV[1:0] in PLLCCR.
 *
 * @details
 * Quickstart EK-RA8D2 picks /3 (code 2). Code = ratio - 1 for the
 * supported ratios 1, 2, 3.
 */
typedef enum : uint8_t {
  k_ra8_plidiv_div1 = 0U, /**< PLL input divide by 1. */
  k_ra8_plidiv_div2 = 1U, /**< PLL input divide by 2. */
  k_ra8_plidiv_div3 = 2U, /**< PLL input divide by 3. */
} ra8_plidiv_t;

/**
 * @enum ra8_plsrcsel_t
 * @brief PLL1 input source selection (PLLCCR.PLSRCSEL, bit 4).
 */
typedef enum : uint8_t {
  k_ra8_plsrcsel_main = 0U, /**< Main-oscillator input (default on EK board). */
  k_ra8_plsrcsel_hoco = 1U, /**< HOCO input (useful for XTAL-less boards).    */
} ra8_plsrcsel_t;

/* =============================================================================
 * PLLCCR2 (PLL1 Output Divider Register) -- RA8D2 layout
 * =============================================================================
 *
 * Cited from FSP CMSIS device header `R7KA8D2KF_core0.h` and
 * FSP `bsp_clocks.c`:
 *   - PLODIVP [3:0]   -- PLL1P output divider code
 *   - PLODIVQ [7:4]   -- PLL1Q output divider code
 *   - PLODIVR [11:8]  -- PLL1R output divider code
 *
 * Code-to-ratio map matches FSP `BSP_CLOCKS_PLL_DIV_*`:
 *   /1=0, /2=1, /3=2, /4=3, /5=4, /6=5, /8=7, /9=8, /16=15.
 * For ratios 1..6 the code is `ratio - 1`; /7 is not supported.
 *
 * Quickstart EK-RA8D2: P=/2 (1), Q=/6 (5), R=/5 (4) -> PLLCCR2 = 0x451.
 */

/**
 * @enum ra8_pllccr2_shift_t
 * @brief Field shifts inside the 16-bit PLLCCR2.
 */
typedef enum : uint8_t {
  k_ra8_pllccr2_shift_plodivp = 0U, /**< PLODIVP field starts at bit 0. */
  k_ra8_pllccr2_shift_plodivq = 4U, /**< PLODIVQ field starts at bit 4. */
  k_ra8_pllccr2_shift_plodivr = 8U, /**< PLODIVR field starts at bit 8. */
} ra8_pllccr2_shift_t;

/**
 * @enum ra8_plodiv_t
 * @brief PLL output divider codes for PLODIVP/Q/R fields in PLLCCR2.
 *
 * @details
 * Encoded as `ratio - 1` for ratios 1..6, plus discrete codes for
 * /8, /9, /16. The quickstart EK-RA8D2 uses /2 for P, /6 for Q, /5 for R.
 *
 * @warning Per HUM Ch 9.2.7 (PLLCCR2) and Ch 9.2.10/9.2.12 (PLL2CCR2)
 * the bit pattern `0000` is listed as "Setting prohibited". Writing
 * code 0 in any PLODIVP/Q/R field is silently rejected by hardware:
 * the entire 16-bit register write is dropped and the register stays
 * at its previous (often reset-default) value. Use a valid divider
 * code in every field, even for outputs you do not consume.
 * `k_ra8_plodiv_div1` is therefore intentionally absent from this enum.
 */
typedef enum : uint8_t {
  k_ra8_plodiv_div2  = 1U,  /**< PLL output divide by 2.  */
  k_ra8_plodiv_div3  = 2U,  /**< PLL output divide by 3.  */
  k_ra8_plodiv_div4  = 3U,  /**< PLL output divide by 4.  */
  k_ra8_plodiv_div5  = 4U,  /**< PLL output divide by 5.  */
  k_ra8_plodiv_div6  = 5U,  /**< PLL output divide by 6.  */
  k_ra8_plodiv_div8  = 7U,  /**< PLL output divide by 8.  */
  k_ra8_plodiv_div9  = 8U,  /**< PLL output divide by 9.  */
  k_ra8_plodiv_div16 = 15U, /**< PLL output divide by 16. */
} ra8_plodiv_t;

/* =============================================================================
 * PLL2CCR / PLL2CR / PLL2CCR2 -- PLL2 register encodings
 * =============================================================================
 *
 * PLL2 has the same field layout as PLL1 except the multiplier is 8 bits
 * (vs 9 for PLL1) and the register width is 16 bits (vs 32). The shifts
 * and the (mul*4 + quarters) packing trick are identical, so we reuse
 * `k_ra8_pllccr_shift_*`, `k_ra8_plidiv_*`, `k_ra8_plsrcsel_*` and
 * `k_ra8_plodiv_*` from above and only add what is unique to PLL2:
 *
 *   - PLL2CR.PLL2STP at bit 0 (write 1 = stop, 0 = run).
 *   - PLL2MUL is 8 bits (1..255), still encoded as `mul * 4 + quarters`
 *     shifted by 6 so the integer part falls in [15:8] and the fraction
 *     in [7:6].
 *
 * Cross-reference: FSP CMSIS device header (RA8 Gen2 family)
 * `R_SYSTEM_PLL2CR_PLL2STP_Pos`, `R_SYSTEM_PLL2CCR_PL2IDIV_Pos`,
 * `R_SYSTEM_PLL2CCR_PL2SRCSEL_Pos`, `R_SYSTEM_PLL2CCR_PLL2MUL_Pos`,
 * `R_SYSTEM_PLL2CCR2_PL2ODIVP_Pos` etc.
 */

/**
 * @enum ra8_pll2cr_val_t
 * @brief Discrete values written to the 8-bit PLL2CR register.
 */
typedef enum : uint8_t {
  k_ra8_pll2cr_run  = 0x00U, /**< Clear PLL2STP -> PLL2 runs.  */
  k_ra8_pll2cr_stop = 0x01U, /**< Set PLL2STP   -> PLL2 stops. */
} ra8_pll2cr_val_t;

/**
 * @enum ra8_pll2ccr_mask_t
 * @brief Field masks (post-shift) inside PLL2CCR.
 *
 * @details RA8D2's PLL2MUL field is 9 bits wide (HUM Ch 9.2.9), so the
 * (mul*4 + quarters) payload fits in 11 bits (9 integer + 2 fractional)
 * once shifted by 6 to span PLL2MULNF[7:6] and PLL2MUL[16:8].
 */
typedef enum : uint32_t {
  k_ra8_pll2ccr_mask_plidiv   = 0x00000003U, /**< PL2IDIV   2-bit field. */
  k_ra8_pll2ccr_mask_quarters = 0x000007FFU, /**< 11-bit (mul*4) field.  */
} ra8_pll2ccr_mask_t;

/* =============================================================================
 * HOCOCR / MOSCWTCR bit fields
 * =============================================================================
 */

/**
 * @enum ra8_hococr_bit_t
 * @brief Bit positions in HOCOCR.
 */
typedef enum : uint8_t {
  k_ra8_hococr_hcstp = 0U, /**< HCSTP: 0 operating, 1 stopped. */
} ra8_hococr_bit_t;

/**
 * @enum ra8_moscwtcr_val_t
 * @brief Values written to MOSCWTCR to control main-osc wait cycles.
 *
 * @details
 * The wait count covers main-osc stabilisation at startup. `0x09`
 * (2^16 cycles) is the default recommended value for the 24 MHz
 * crystal on the EK-RA8D2 and yields ~2.7 ms of wait time.
 */
typedef enum : uint8_t {
  k_ra8_moscwtcr_2_to_16_cycles = 0x09U, /**< RA8 moscwtcr 2 to 16 cycles. */
} ra8_moscwtcr_val_t;

#ifdef __cplusplus
}
#endif
