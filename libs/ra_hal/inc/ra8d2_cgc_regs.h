/**
 * @file ra8d2_cgc_regs.h
 * @brief Clock Generation Circuit (CGC) field layouts for the Renesas RA8D2
 *
 * @details
 * The CGC is logically a separate peripheral but physically lives
 * inside the SYSC register block (`ra8d2_system_regs.h`). This header
 * provides the bit-field encodings, divider ratios and clock-source
 * selectors needed to programme the PLL and clock tree from
 * `ra_cgc.c`.
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
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
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
 * @enum ra_sckdivcr_shift_t
 * @brief Bit shifts for the divider nibbles in SCKDIVCR.
 */
typedef enum : uint8_t {
  k_ra_sckdivcr_pckd_shift = 0U,  /**< PCLKD divider nibble @ [3:0].   */
  k_ra_sckdivcr_pckc_shift = 4U,  /**< PCLKC divider nibble @ [7:4].   */
  k_ra_sckdivcr_pckb_shift = 8U,  /**< PCLKB divider nibble @ [11:8].  */
  k_ra_sckdivcr_pcka_shift = 12U, /**< PCLKA divider nibble @ [15:12]. */
  k_ra_sckdivcr_bck_shift  = 16U, /**< BCLK  divider nibble @ [19:16]. */
  k_ra_sckdivcr_pcke_shift = 20U, /**< PCLKE divider nibble @ [23:20]. */
  k_ra_sckdivcr_ick_shift  = 24U, /**< ICLK  divider nibble @ [27:24]. */
  k_ra_sckdivcr_fck_shift  = 28U, /**< FCLK  divider nibble @ [31:28]. */
} ra_sckdivcr_shift_t;

/**
 * @enum ra_clock_div_t
 * @brief Divider codes written into SCKDIVCR nibbles.
 *
 * @details
 * The RA8 SCKDIVCR nibble encodes divide-by-2^N where the nibble value
 * is N, except that `0` means divide-by-1. See HUM Table 10.x for the
 * full list; we only expose the values the project uses.
 */
typedef enum : uint8_t {
  k_ra_clock_div_1   = 0x0U, /**< Divide by 1.  */
  k_ra_clock_div_2   = 0x1U, /**< Divide by 2.  */
  k_ra_clock_div_4   = 0x2U, /**< Divide by 4.  */
  k_ra_clock_div_8   = 0x3U, /**< Divide by 8.  */
  k_ra_clock_div_16  = 0x4U, /**< Divide by 16. */
  k_ra_clock_div_32  = 0x5U, /**< Divide by 32. */
  k_ra_clock_div_64  = 0x6U, /**< Divide by 64. */
} ra_clock_div_t;

/* =============================================================================
 * SCKDIVCR2 bit fields (CPU clocks + MRAM clock)
 * =============================================================================
 */

/**
 * @enum ra_sckdivcr2_shift_t
 * @brief Bit shifts for the divider nibbles in SCKDIVCR2.
 */
typedef enum : uint8_t {
  k_ra_sckdivcr2_cpuclk0_shift = 0U,  /**< CPUCLK0 (M85) divider @ [3:0].    */
  k_ra_sckdivcr2_cpuclk1_shift = 4U,  /**< CPUCLK1 (M33) divider @ [7:4].    */
  k_ra_sckdivcr2_npuclk_shift  = 8U,  /**< NPUCLK        divider @ [11:8].   */
  k_ra_sckdivcr2_mriclk_shift  = 12U, /**< MRAM bus clk  divider @ [15:12].  */
} ra_sckdivcr2_shift_t;

/* =============================================================================
 * SCKSCR (System Clock Source Control Register)
 * =============================================================================
 */

/**
 * @enum ra_cksel_t
 * @brief System clock source selector values written to SCKSCR.CKSEL.
 */
typedef enum : uint8_t {
  k_ra_cksel_hoco  = 0U, /**< High-speed on-chip oscillator. */
  k_ra_cksel_moco  = 1U, /**< Middle-speed on-chip osc.      */
  k_ra_cksel_loco  = 2U, /**< Low-speed on-chip osc.         */
  k_ra_cksel_main  = 3U, /**< Main oscillator (XTAL).        */
  k_ra_cksel_subck = 4U, /**< Sub-clock osc.                 */
  k_ra_cksel_pll1  = 5U, /**< PLL1 output.                   */
  k_ra_cksel_pll2  = 6U, /**< PLL2 output.                   */
} ra_cksel_t;

/* =============================================================================
 * PLLCCR (PLL1 Clock Control Register)
 * =============================================================================
 *
 * On RA8D2, PLL1 multiplies the main crystal up to ~1 GHz internally.
 * Field layout (HUM section 10.2.x):
 *   [0]     PLSRCSEL  -- PLL input source (0 = Main, 1 = HOCO)
 *   [7:2]   PLIDIV    -- input divider (0..n)
 *   [13:8]  PLLMUL    -- multiplier code
 *
 * Detailed bit layout varies between RA8 variants; confirm against
 * HUM R01UH1065EJ when we actually bring up the PLL.
 */

/**
 * @enum ra_plsrcsel_t
 * @brief PLL1 input source selection.
 */
typedef enum : uint8_t {
  k_ra_plsrcsel_main = 0U, /**< Main-oscillator input (default on EK board). */
  k_ra_plsrcsel_hoco = 1U, /**< HOCO input (useful for XTAL-less boards).    */
} ra_plsrcsel_t;

/* =============================================================================
 * HOCOCR / MOSCWTCR bit fields
 * =============================================================================
 */

/**
 * @enum ra_hococr_bit_t
 * @brief Bit positions in HOCOCR.
 */
typedef enum : uint8_t {
  k_ra_hococr_hcstp = 0U, /**< HCSTP: 0 operating, 1 stopped. */
} ra_hococr_bit_t;

/**
 * @enum ra_moscwtcr_val_t
 * @brief Values written to MOSCWTCR to control main-osc wait cycles.
 *
 * @details
 * The wait count covers main-osc stabilisation at startup. `0x09`
 * (2^16 cycles) is the default recommended value for the 24 MHz
 * crystal on the EK-RA8D2 and yields ~2.7 ms of wait time.
 */
typedef enum : uint8_t {
  k_ra_moscwtcr_2_to_16_cycles = 0x09U,
} ra_moscwtcr_val_t;

#ifdef __cplusplus
}
#endif
