/**
 * @file ra8_lpm_regs.h
 * @brief Low Power Mode (LPM) register layout for the Renesas RA8D2
 * @ingroup grp_hal_system
 *
 * @details
 * Hand-derived register map covering every register the LPM HAL
 * driver in ``libs/ra8_hal/src/ra8_lpm.c`` touches. Sources:
 *
 *  - HUM Ch 11 "Low Power Mode" (pages 429..497) covers SBYCR,
 *    DPSBYCR, LPSCR, SSCR1, OPCCR, PDRAMSCR0/1, DPSIER0..3,
 *    DPSIFR0..3, DPSIEGR0..2, PLL1LDOCR, PLL2LDOCR, HOCOLDOCR.
 *  - HUM Ch 9 "Clock Generation Circuit" (pages 317..419) covers
 *    MOSCCR, MOCOCR, HOCOCR, LOCOCR, SOSCCR (per-oscillator stop
 *    bits used by the LPM clock-shutdown matrix).
 *  - HUM Ch 14 "Interrupt Controller Unit" (pages 549..553) covers
 *    WUPEN0 / WUPEN1 (wake-up source enables).
 *
 * Two distinct hardware base addresses are involved:
 *
 *  - SYSC = 0x4001_E000 -- SBYCR/DPSBYCR/LPSCR/SSCR1/OPCCR/PDRAMSCR
 *    family / DPSIER family / DPSIFR family / DPSIEGR family /
 *    PLL1LDOCR / PLL2LDOCR / HOCOLDOCR / MOSCCR / MOCOCR / HOCOCR /
 *    LOCOCR / SOSCCR / PRCR.
 *  - ICU  = 0x4000_C000 -- WUPEN0/WUPEN1 (this is the WUPEN sub-block
 *    of ICU; the IELSR/NMI sub-block lives at 0x4000_6000).
 *
 * The LPM driver works in terms of byte offsets relative to those
 * two bases. Every offset enumerator below is byte-accurate against
 * the HUM reference.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"

/* =============================================================================
 * Base addresses
 * =============================================================================
 */

/**
 * @enum ra8_lpm_addr_t
 * @brief Hardware base addresses used by the LPM HAL driver.
 *
 * @details
 * Both bases live inside the 0x4000_0000 peripheral window so the
 * host fake's blanket 8 MiB mmap covers them automatically.
 */
typedef enum : uintptr_t {
  k_ra8_lpm_sysc_base_addr = 0x4001E000UL, /**< SYSC base (HUM Ch 11.2.18 p 456).      */
  k_ra8_lpm_icu_base_addr  = 0x4000C000UL, /**< ICU/WUPEN base (HUM Ch 14.2.19 p 550). */
} ra8_lpm_addr_t;

/* =============================================================================
 * Register offsets
 * =============================================================================
 */

/**
 * @enum ra8_lpm_off_t
 * @brief Byte offsets of every LPM-relevant register from its block base.
 *
 * @details
 * Citations cite the exact HUM section + page where the offset is
 * documented. Offsets in the SYSC group are relative to
 * ``k_ra8_lpm_sysc_base_addr``; WUPEN offsets are relative to
 * ``k_ra8_lpm_icu_base_addr``.
 */
typedef enum : uint16_t {
  /* --- SYSC: lifecycle + LPSCR ---------------------------------------- */
  k_ra8_lpm_sbycr_off   = 0x00CU, /**< SBYCR    (8b)  -- HUM Ch 11.2.18 p 456. */
  k_ra8_lpm_lpscr_off   = 0xA90U, /**< LPSCR    (8b)  -- HUM Ch 11.2.20 p 457. */
  k_ra8_lpm_sscr1_off   = 0xA98U, /**< SSCR1    (8b)  -- HUM Ch 11.2.19 p 456. */
  k_ra8_lpm_dpsbycr_off = 0xA00U, /**< DPSBYCR  (8b)  -- HUM Ch 11.2.21 p 458. */
  k_ra8_lpm_opccr_off   = 0x0A0U, /**< OPCCR    (8b)  -- HUM Ch 11.2.11 p 451. */

  /* --- SYSC: PRCR (16-bit, write-protect key) ------------------------- */
  k_ra8_lpm_prcr_off = 0x3FAU, /**< PRCR     (16b) -- HUM Ch 11.2.18 p 456. */

  /* --- SYSC: RAM retention ------------------------------------------- */
  k_ra8_lpm_pdramscr0_off = 0x140U, /**< PDRAMSCR0 (16b) -- HUM Ch 11.2.16 p 453. */
  k_ra8_lpm_pdramscr1_off = 0x142U, /**< PDRAMSCR1 (8b)  -- HUM Ch 11.2.17 p 455. */

  /* --- SYSC: LDO standby --------------------------------------------- */
  k_ra8_lpm_pll1ldocr_off = 0xB04U, /**< PLL1LDOCR  (8b) -- HUM Ch 11.2.40 p 475. */
  k_ra8_lpm_pll2ldocr_off = 0xB08U, /**< PLL2LDOCR  (8b) -- HUM Ch 11.2.41 p 475. */
  k_ra8_lpm_hocoldocr_off = 0xB0CU, /**< HOCOLDOCR  (8b) -- HUM Ch 11.2.42 p 476. */

  /* --- SYSC: deep-standby cancel (DPSIER0..3 / DPSIFR0..3 / DPSIEGR0..2) */
  k_ra8_lpm_dpsier0_off = 0xA08U, /**< DPSIER0  (8b)  -- HUM Ch 11.2.22 p 459. */
  k_ra8_lpm_dpsier1_off = 0xA0CU, /**< DPSIER1  (8b)  -- HUM Ch 11.2.23 p 459. */
  k_ra8_lpm_dpsier2_off = 0xA10U, /**< DPSIER2  (8b)  -- HUM Ch 11.2.24 p 460. */
  k_ra8_lpm_dpsier3_off = 0xA14U, /**< DPSIER3  (8b)  -- HUM Ch 11.2.25 p 461. */

  k_ra8_lpm_dpsifr0_off = 0xA18U, /**< DPSIFR0  (8b)  -- HUM Ch 11.2.28 p 462. */
  k_ra8_lpm_dpsifr1_off = 0xA1CU, /**< DPSIFR1  (8b)  -- HUM Ch 11.2.29 p 462. */
  k_ra8_lpm_dpsifr2_off = 0xA20U, /**< DPSIFR2  (8b)  -- HUM Ch 11.2.30 p 462. */
  k_ra8_lpm_dpsifr3_off = 0xA24U, /**< DPSIFR3  (8b)  -- HUM Ch 11.2.31 p 462. */

  k_ra8_lpm_dpsiegr0_off = 0xA28U, /**< DPSIEGR0 (8b)  -- HUM Ch 11.2.34 p 463. */
  k_ra8_lpm_dpsiegr1_off = 0xA2CU, /**< DPSIEGR1 (8b)  -- HUM Ch 11.2.35 p 463. */
  k_ra8_lpm_dpsiegr2_off = 0xA30U, /**< DPSIEGR2 (8b)  -- HUM Ch 11.2.36 p 463. */

  /* --- SYSC: power-domain gating control ------------------------------ */
  k_ra8_lpm_pdctrgd_off = 0x110U, /**< PDCTRGD  (8b)  -- HUM Ch 11.2.14 p 452. */

  /* --- SYSC: per-oscillator stop bits (CGC sub-block, same SYSC base) -- */
  k_ra8_lpm_mosccr_off = 0x032U, /**< MOSCCR   (8b)  -- HUM Ch 9.2.13 p 343. */
  k_ra8_lpm_hococr_off = 0x036U, /**< HOCOCR   (8b)  -- HUM Ch 9.2.16 p 345. */
  k_ra8_lpm_mococr_off = 0x038U, /**< MOCOCR   (8b)  -- HUM Ch 9.2.18 p 346. */
  k_ra8_lpm_lococr_off = 0x400U, /**< LOCOCR   (8b)  -- HUM Ch 9.2.15 p 344. */
  k_ra8_lpm_sosccr_off = 0xC00U, /**< SOSCCR   (8b)  -- HUM Ch 9.2.14 p 343. */

  /* --- ICU: WUPEN0/WUPEN1 (32-bit) ----------------------------------- */
  k_ra8_lpm_wupen0_off = 0x1A0U, /**< WUPEN0   (32b) -- HUM Ch 14.2.19 p 550. */
  k_ra8_lpm_wupen1_off = 0x1A4U, /**< WUPEN1   (32b) -- HUM Ch 14.2.20 p 552. */
} ra8_lpm_off_t;

/* =============================================================================
 * Sleep mode (LPSCR.LPMD encoding)
 * =============================================================================
 */

/**
 * @enum ra8_sleep_mode_t
 * @brief Software-selectable sleep modes (LPSCR.LPMD encoding).
 *
 * @details
 * Values are the LPSCR.LPMD field encoding from HUM Ch 11.2.20
 * p 457 *plus* two pseudo-encodings (``k_ra8_sleep_mode_sleep`` and
 * ``k_ra8_sleep_mode_deep_sleep``) for which LPMD stays at 0
 * (System Active) and only the Cortex-M85 SCR.SLEEPDEEP bit
 * differs. The numeric values for the standby modes are the literal
 * LPMD bit pattern so the driver can write ``mode`` directly into
 * LPSCR.
 *
 * | Value | Meaning                              | LPMD | SLEEPDEEP |
 * |------:|--------------------------------------|-----:|:---------:|
 * |   0   | Sleep -- CPU stop, peripherals run   |  0x0 |     0     |
 * |   2   | Deep Sleep -- CPU power-gate eligible|  0x0 |     1     |
 * |   5   | Software Standby                     |  0x5 |     1     |
 * |   8   | Deep Software Standby 1              |  0x8 |     1     |
 * |   9   | Deep Software Standby 2              |  0x9 |     1     |
 * |  10   | Deep Software Standby 3              |  0xA |     1     |
 *
 * The split between Deep Software Standby 1/2/3 follows HUM Ch 11.1
 * Table 11.3 (p 431..432): variants 2 and 3 progressively stop more
 * domains (LOCO, sub-clock detection, voltage monitors).
 */
typedef enum : uint8_t {
  k_ra8_sleep_mode_sleep          = 0U,   /**< CPU sleep, peripherals run.       */
  k_ra8_sleep_mode_deep_sleep     = 2U,   /**< CPU deep sleep (SCR.SLEEPDEEP=1). */
  k_ra8_sleep_mode_software_std   = 0x5U, /**< Software Standby (LPMD=0x5).      */
  k_ra8_sleep_mode_deep_standby_1 = 0x8U, /**< Deep Standby 1 (LPMD=0x8).        */
  k_ra8_sleep_mode_deep_standby_2 = 0x9U, /**< Deep Standby 2 (LPMD=0x9).        */
  k_ra8_sleep_mode_deep_standby_3 = 0xAU, /**< Deep Standby 3 (LPMD=0xA).        */
} ra8_sleep_mode_t;

/* =============================================================================
 * Bit fields and masks
 * =============================================================================
 */

/**
 * @enum ra8_lpm_sbycr_bits_t
 * @brief SBYCR bit positions and reset value (HUM Ch 11.2.18 p 456).
 */
typedef enum : uint8_t {
  k_ra8_lpm_sbycr_ope_mask  = 0x40U, /**< OPE @ bit 6 -- bus-signal output keep. */
  k_ra8_lpm_sbycr_reset_val = 0x40U, /**< Cold-reset value (OPE=1, all else 0).  */
} ra8_lpm_sbycr_bits_t;

/**
 * @enum ra8_lpm_dpsbycr_bits_t
 * @brief DPSBYCR bit fields and reset value (HUM Ch 11.2.21 p 458).
 *
 * @details
 * DCSSMODE[1:0] occupies bits 3:2; IOKEEP is bit 6. Reset value is
 * 0x14 -- bit 4 reads-as-1 plus DCSSMODE=01b (128 us soft-start).
 */
typedef enum : uint8_t {
  k_ra8_lpm_dpsbycr_dcssmode_shift = 2U,    /**< DCSSMODE[1:0] starts at bit 2. */
  k_ra8_lpm_dpsbycr_dcssmode_mask  = 0x0CU, /**< DCSSMODE[1:0] mask.            */
  k_ra8_lpm_dpsbycr_iokeep_mask    = 0x40U, /**< IOKEEP @ bit 6.                */
  k_ra8_lpm_dpsbycr_reset_val      = 0x14U, /**< Cold-reset value.              */
} ra8_lpm_dpsbycr_bits_t;

/**
 * @enum ra8_lpm_dcssmode_t
 * @brief DPSBYCR.DCSSMODE encoding (HUM Ch 11.2.21 p 458).
 *
 * @details
 * Selects the wake-up time from Deep Software Standby with DCDC.
 * Encoding 0b00 is "Setting prohibited"; the driver only exposes
 * the three valid encodings.
 */
typedef enum : uint8_t {
  k_ra8_lpm_dcssmode_128us = 0x1U, /**< 128 us soft-start (cold-reset default). */
  k_ra8_lpm_dcssmode_256us = 0x2U, /**< 256 us soft-start.                      */
  k_ra8_lpm_dcssmode_512us = 0x3U, /**< 512 us soft-start.                      */
} ra8_lpm_dcssmode_t;

/**
 * @enum ra8_lpm_sscr1_bits_t
 * @brief SSCR1 bit fields (HUM Ch 11.2.19 p 456).
 */
typedef enum : uint8_t {
  k_ra8_lpm_sscr1_ss2fr_mask  = 0x01U, /**< SS2FR @ bit 0 -- fast return. */
  k_ra8_lpm_sscr1_ss2lp_shift = 2U,    /**< SS2LP[1:0] starts at bit 2.   */
  k_ra8_lpm_sscr1_ss2lp_mask  = 0x0CU, /**< SS2LP[1:0] mask.              */
} ra8_lpm_sscr1_bits_t;

/**
 * @enum ra8_lpm_sscr1_ss2lp_t
 * @brief SSCR1.SS2LP encoding (HUM Ch 11.2.19 p 456).
 */
typedef enum : uint8_t {
  k_ra8_lpm_ss2lp_default = 0x0U, /**< SS2LP_0 -- default mode.        */
  k_ra8_lpm_ss2lp_low     = 0x1U, /**< SS2LP_1 -- low power setting 1. */
} ra8_lpm_sscr1_ss2lp_t;

/**
 * @enum ra8_lpm_lpscr_bits_t
 * @brief LPSCR bit fields (HUM Ch 11.2.20 p 457).
 */
typedef enum : uint8_t {
  k_ra8_lpm_lpscr_lpmd_mask = 0x0FU, /**< LPMD[3:0] occupies bits 3..0. */
} ra8_lpm_lpscr_bits_t;

/**
 * @enum ra8_lpm_opccr_bits_t
 * @brief OPCCR bit fields (HUM Ch 11.2.11 p 451).
 */
typedef enum : uint8_t {
  k_ra8_lpm_opccr_opcm_mask   = 0x03U, /**< OPCM[1:0] @ bits 1..0 -- power mode.     */
  k_ra8_lpm_opccr_opcmtsf_msk = 0x10U, /**< OPCMTSF @ bit 4 -- mode-transition flag. */
} ra8_lpm_opccr_bits_t;

/**
 * @enum ra8_lpm_prcr_bits_t
 * @brief PRCR (write-protect) key + group masks (HUM Ch 11.2.18 p 456).
 *
 * @details
 * The protection register is 16 bits wide. The upper byte must be
 * 0xA5 on every write; the lower byte selects which protected
 * groups are unlocked. PRC1 (bit 1) gates LPM/SYSC writes used by
 * this driver.
 */
typedef enum : uint16_t {
  k_ra8_lpm_prcr_key      = 0xA500U, /**< Mandatory password in upper byte.  */
  k_ra8_lpm_prcr_prc0_msk = 0x0001U, /**< PRC0 -- CGC + LVD group.           */
  k_ra8_lpm_prcr_prc1_msk = 0x0002U, /**< PRC1 -- LPM/SYSC group.            */
  k_ra8_lpm_prcr_prc2_msk = 0x0008U, /**< PRC3 (LVD reset) -- not used here. */
} ra8_lpm_prcr_bits_t;

/**
 * @enum ra8_lpm_pdramscr0_groups_t
 * @brief Useful PDRAMSCR0 RKEEP[12:0] preset masks (HUM Ch 11.2.16 p 453).
 *
 * @details
 * Per HUM Table 11.5 the RKEEP bits group as:
 *
 *  - SRAM0 = b3..b0 (must be all 1 or all 0 -- 0xF or 0x0).
 *  - SRAM1 = b7..b4 (mask 0xF0).
 *  - SRAM2 = b11..b8 (mask 0xF00).
 *  - SRAM3 = b12 (single bit, mask 0x1000).
 *
 * Bit 13 reads-as-1; bit 14 reads-as-1; bit 15 reads-as-0. The
 * "all keep" mask therefore sets bits 12..0 plus the read-as-1
 * bits 14..13, which is the cold-reset value.
 */
typedef enum : uint16_t {
  k_ra8_lpm_pdramscr0_sram0_keep = 0x000FU, /**< Retain SRAM0 (b3..b0).  */
  k_ra8_lpm_pdramscr0_sram1_keep = 0x00F0U, /**< Retain SRAM1 (b7..b4).  */
  k_ra8_lpm_pdramscr0_sram2_keep = 0x0F00U, /**< Retain SRAM2 (b11..b8). */
  k_ra8_lpm_pdramscr0_sram3_keep = 0x1000U, /**< Retain SRAM3 (b12).     */
  k_ra8_lpm_pdramscr0_all_keep   = 0x1FFFU, /**< Retain all banks.       */
  k_ra8_lpm_pdramscr0_writable   = 0x7FFFU, /**< b14..b0 writable mask.  */
} ra8_lpm_pdramscr0_groups_t;

/**
 * @enum ra8_lpm_pdramscr1_bits_t
 * @brief PDRAMSCR1 bit positions (HUM Ch 11.2.17 p 455).
 */
typedef enum : uint8_t {
  k_ra8_lpm_pdramscr1_rkeep0_mask = 0x01U, /**< RKEEP0 @ bit 0 -- CPU0 TCM. */
  k_ra8_lpm_pdramscr1_rkeep1_mask = 0x02U, /**< RKEEP1 @ bit 1 -- CPU1 TCM. */
} ra8_lpm_pdramscr1_bits_t;

/**
 * @enum ra8_lpm_ldocr_bits_t
 * @brief PLL[12]LDOCR / HOCOLDOCR shared bit fields
 *        (HUM Ch 11.2.40..42 p 475..476).
 */
typedef enum : uint8_t {
  k_ra8_lpm_ldocr_ldostp_mask = 0x01U, /**< LDOSTP @ bit 0 -- stop in normal mode. */
  k_ra8_lpm_ldocr_skeep_mask  = 0x02U, /**< SKEEP  @ bit 1 -- retain in standby.   */
  k_ra8_lpm_ldocr_skeep_shift = 1U,    /**< SKEEP starts at bit 1.                 */
} ra8_lpm_ldocr_bits_t;

/**
 * @enum ra8_lpm_ldo_state_t
 * @brief LDOCR.SKEEP encoding for ``ra8_lpm_set_ldo_standby``.
 *
 * @details
 * SKEEP=1 retains the LDO state across Software Standby; SKEEP=0
 * disables the LDO during standby for additional power savings.
 */
typedef enum : uint8_t {
  k_ra8_lpm_ldo_disabled = 0U, /**< LDO stops during standby (SKEEP=0). */
  k_ra8_lpm_ldo_retained = 1U, /**< LDO retains state (SKEEP=1).        */
} ra8_lpm_ldo_state_t;

/**
 * @enum ra8_lpm_clock_stop_bits_t
 * @brief Per-oscillator stop bit (HUM Ch 9.2.13..18 p 343..346).
 *
 * @details
 * Every oscillator control register (MOSCCR/HOCOCR/MOCOCR/LOCOCR/
 * SOSCCR) places its stop bit at bit 0:
 *
 *  - MOSCCR.MOSTP = bit 0
 *  - HOCOCR.HCSTP = bit 0
 *  - MOCOCR.MCSTP = bit 0
 *  - LOCOCR.LCSTP = bit 0
 *  - SOSCCR.SOSTP = bit 0
 */
typedef enum : uint8_t {
  k_ra8_lpm_clock_stop_mask = 0x01U, /**< STOP @ bit 0 in every OCR. */
} ra8_lpm_clock_stop_bits_t;

/**
 * @enum ra8_lpm_pdctr_bits_t
 * @brief PDCTRGD power-gating field masks (HUM Ch 11.2.14 p 452).
 *
 * @details
 * The graphics power domain covers MIPI DSI, MIPI CSI, VIN, **DRW** and
 * GLCDC (HUM Ch 11.5.1 Table 11.7 p 480). Its reset value is @c 0x81 --
 * @c PDPGSF = 1 (domain gated OFF) and @c PDDE = 1 (power off the target
 * domain) -- so every one of those peripherals is unpowered until firmware
 * clears @c PDDE. Bench-confirmed on an EK-RA8D2: with @c PDCTRGD = 0x81
 * the DRW @c HWREVISION register reads @c 0x00000000; after clearing
 * @c PDDE it reads @c 0x0FBE0107.
 *
 * @c PDDE polarity is inverted relative to the obvious reading: 0 powers
 * the domain ON, 1 powers it OFF.
 */
typedef enum : uint8_t {
  k_ra8_lpm_pdctr_pdde_mask   = 0x01U, /**< PDDE   @ bit 0: 1 = powered OFF. */
  k_ra8_lpm_pdctr_pdcsf_mask  = 0x40U, /**< PDCSF  @ bit 6: control busy.    */
  k_ra8_lpm_pdctr_pdpgsf_mask = 0x80U, /**< PDPGSF @ bit 7: 1 = gated off.   */
} ra8_lpm_pdctr_bits_t;

/**
 * @enum ra8_lpm_dpsier_idx_t
 * @brief Selector for DPSIER0..3 / DPSIFR0..3 / DPSIEGR0..2 helpers
 *        (HUM Ch 11.2.22..36 p 459..463).
 */
typedef enum : uint8_t {
  k_ra8_lpm_dpsier_idx_0 = 0U, /**< DPSIER0 / DPSIFR0 / DPSIEGR0. */
  k_ra8_lpm_dpsier_idx_1 = 1U, /**< DPSIER1 / DPSIFR1 / DPSIEGR1. */
  k_ra8_lpm_dpsier_idx_2 = 2U, /**< DPSIER2 / DPSIFR2 / DPSIEGR2. */
  k_ra8_lpm_dpsier_idx_3 =
    3U, /**< DPSIER3 / DPSIFR3 (HUM Ch 11.2.25 p 461 -- DPSIEGR3 @ 0xA34 exists, unexposed here). */
  /** RA8 lpm dpsier index count. */
  k_ra8_lpm_dpsier_idx_count = 4U,
} ra8_lpm_dpsier_idx_t;

/**
 * @enum ra8_lpm_dpsier2_bits_t
 * @brief DPSIER2 cancel-source bit positions (HUM Ch 11.2.24 p 460).
 */
typedef enum : uint8_t {
  k_ra8_lpm_dpsier2_dpvd1ie_mask = 0x01U, /**< DPVD1IE @ bit 0. */
  k_ra8_lpm_dpsier2_dpvd2ie_mask = 0x02U, /**< DPVD2IE @ bit 1. */
  k_ra8_lpm_dpsier2_drtciie_mask = 0x04U, /**< DRTCIIE @ bit 2. */
  k_ra8_lpm_dpsier2_drtcaie_mask = 0x08U, /**< DRTCAIE @ bit 3. */
  k_ra8_lpm_dpsier2_dnmie_mask   = 0x10U, /**< DNMIE   @ bit 4. */
} ra8_lpm_dpsier2_bits_t;

/**
 * @enum ra8_lpm_dpsier3_bits_t
 * @brief DPSIER3 cancel-source bit positions (HUM Ch 11.2.25 p 461).
 */
typedef enum : uint8_t {
  k_ra8_lpm_dpsier3_dusbfsie_mask  = 0x01U, /**< DUSBFSIE  @ bit 0. */
  k_ra8_lpm_dpsier3_dusbhsie_mask  = 0x02U, /**< DUSBHSIE  @ bit 1. */
  k_ra8_lpm_dpsier3_dulpt0ie_mask  = 0x04U, /**< DULPT0IE  @ bit 2. */
  k_ra8_lpm_dpsier3_dulpt1ie_mask  = 0x08U, /**< DULPT1IE  @ bit 3. */
  k_ra8_lpm_dpsier3_diwdtie_mask   = 0x20U, /**< DIWDTIE   @ bit 5. */
  k_ra8_lpm_dpsier3_dsostdie_mask  = 0x40U, /**< DSOSTDIE  @ bit 6. */
  k_ra8_lpm_dpsier3_dvbattadie_msk = 0x80U, /**< DVBATTADIE@ bit 7. */
} ra8_lpm_dpsier3_bits_t;

/**
 * @enum ra8_lpm_wupen0_bits_t
 * @brief WUPEN0 bit masks for common wake-up sources
 *        (HUM Ch 14.2.19 p 550).
 *
 * @details
 * WUPEN0 packs IRQ0..IRQ15 into bits 15..0 plus assorted peripheral
 * wake-up enables in bits 31..16. The driver provides per-source
 * masks for the bits the firmware actually targets; raw 32-bit
 * writes work for everything else.
 */
typedef enum : uint32_t {
  k_ra8_lpm_wupen0_irq0    = 1UL << 0,              /**< IRQ0  @ bit 0.              */
  k_ra8_lpm_wupen0_irq1    = 1UL << 1,              /**< IRQ1  @ bit 1.              */
  k_ra8_lpm_wupen0_irq2    = 1UL << 2,              /**< IRQ2  @ bit 2.              */
  k_ra8_lpm_wupen0_irq3    = 1UL << 3,              /**< IRQ3  @ bit 3.              */
  k_ra8_lpm_wupen0_irq4    = 1UL << 4,              /**< IRQ4  @ bit 4.              */
  k_ra8_lpm_wupen0_irq5    = 1UL << 5,              /**< IRQ5  @ bit 5.              */
  k_ra8_lpm_wupen0_irq6    = 1UL << 6,              /**< IRQ6  @ bit 6.              */
  k_ra8_lpm_wupen0_irq7    = 1UL << 7,              /**< IRQ7  @ bit 7.              */
  k_ra8_lpm_wupen0_iwdt    = 1UL << 16,             /**< IWDTWUPEN.                  */
  k_ra8_lpm_wupen0_pvd1    = 1UL << 18,             /**< PVD1WUPEN.                  */
  k_ra8_lpm_wupen0_pvd2    = 1UL << 19,             /**< PVD2WUPEN.                  */
  k_ra8_lpm_wupen0_vbatt   = 1UL << 20,             /**< VBATTWUPEN.                 */
  k_ra8_lpm_wupen0_rtcalm  = 1UL << 24,             /**< RTCALMWUPEN.                */
  k_ra8_lpm_wupen0_rtcprd  = 1UL << 25,             /**< RTCPRDWUPEN.                */
  k_ra8_lpm_wupen0_usbhs   = 1UL << 26,             /**< USBHSWUPEN.                 */
  k_ra8_lpm_wupen0_usbfs   = 1UL << 27,             /**< USBFS0WUPEN.                */
  k_ra8_lpm_wupen0_agt1ud  = 1UL << 28,             /**< AGT1UDWUPEN.                */
  k_ra8_lpm_wupen0_agt1ca  = 1UL << 29,             /**< AGT1CAWUPEN.                */
  k_ra8_lpm_wupen0_agt1cb  = 1UL << 30,             /**< AGT1CBWUPEN.                */
  k_ra8_lpm_wupen0_riic0   = 1UL << 31,             /**< RIIC0WUPEN.                 */
  k_ra8_lpm_wupen0_acmphs0 = k_ra8_lpm_wupen0_pvd1, /**< legacy alias for PVD1 path. */
} ra8_lpm_wupen0_bits_t;

/**
 * @enum ra8_lpm_wupen1_bits_t
 * @brief WUPEN1 bit masks for ULPT/I3C/SOSC/PDM/COMP sources
 *        (HUM Ch 14.2.20 p 552).
 */
typedef enum : uint32_t {
  k_ra8_lpm_wupen1_comphs0 = 1UL << 3,  /**< COMPHS0WUPEN @ bit 3.  */
  k_ra8_lpm_wupen1_sosc    = 1UL << 7,  /**< SOSCWUPEN    @ bit 7.  */
  k_ra8_lpm_wupen1_ulpt0u  = 1UL << 8,  /**< ULP0UWUPEN   @ bit 8.  */
  k_ra8_lpm_wupen1_ulpt0a  = 1UL << 9,  /**< ULP0AWUPEN   @ bit 9.  */
  k_ra8_lpm_wupen1_ulpt0b  = 1UL << 10, /**< ULP0BWUPEN   @ bit 10. */
  k_ra8_lpm_wupen1_i3c0    = 1UL << 11, /**< I3CWUPEN     @ bit 11. */
  k_ra8_lpm_wupen1_ulpt1u  = 1UL << 12, /**< ULP1UWUPEN   @ bit 12. */
  k_ra8_lpm_wupen1_ulpt1a  = 1UL << 13, /**< ULP1AWUPEN   @ bit 13. */
  k_ra8_lpm_wupen1_ulpt1b  = 1UL << 14, /**< ULP1BWUPEN   @ bit 14. */
  k_ra8_lpm_wupen1_pdm     = 1UL << 15, /**< PDMWUPEN     @ bit 15. */
} ra8_lpm_wupen1_bits_t;

/**
 * @enum ra8_lpm_scb_t
 * @brief ARM Cortex-M System Control Block address used for SLEEPDEEP.
 *
 * @details
 * SCB->SCR lives at 0xE000_E000 + 0x10 in the System Control Space;
 * the SLEEPDEEP bit instructs WFI to enter Deep Sleep / Software
 * Standby instead of plain CPU sleep. This is an Arm core register
 * (Armv8-M SCS), not an RA8D2 peripheral, so it carries no HUM
 * citation. The host unit-test build backs the SCS window with RAM
 * (``ra8_fake_mmap`` core region at 0xE0000000), so the driver's
 * read-modify-write sequence runs unchanged on every build.
 *
 * @invariant The single enumerator is the architectural SCR address.
 *
 * @see ra8_lpm_scb_scr()  Accessor returning the typed pointer.
 * @see ra8_lpm_scb_field_t  SCR field masks used by the LPM driver.
 * @since 0.1.0
 */
typedef enum : uintptr_t {
  k_ra8_lpm_scb_scr_addr = 0xE000ED10UL, /**< SCB->SCR address (Cortex-M85). */
} ra8_lpm_scb_t;

/**
 * @enum ra8_lpm_scb_field_t
 * @brief ARM Cortex-M85 SCB.SCR field bits used for sleep entry.
 *
 * @details
 * Only SLEEPDEEP is driven by the LPM driver; sibling bits such as
 * SLEEPONEXIT and SEVONPEND are preserved by read-modify-write.
 *
 * @invariant Each enumerator is a single-bit mask within SCR.
 *
 * @see ra8_lpm_scb_scr()  Accessor returning the typed pointer.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_lpm_scb_scr_sleepdeep = 0x4UL, /**< SCR.SLEEPDEEP @ bit 2. */
} ra8_lpm_scb_field_t;

/* =============================================================================
 * Inline accessors
 * =============================================================================
 */

/**
 * @brief Return a typed pointer to the Cortex-M SCB->SCR register.
 *
 * @details
 * SCR is an Arm core register (Armv8-M System Control Space), shared
 * by the LPM driver (SLEEPDEEP toggling around WFI) and the host unit
 * tests (staging / asserting the toggle sequence through the fake mmap
 * core window).
 *
 * @return Volatile pointer suitable for direct dereference.
 *
 * @pre Executing on the Cortex-M85 (or a host build with the SCS
 *      window RAM-backed by ``ra8_fake_mmap``).
 * @pre The address enum matches the Armv8-M architectural SCS layout.
 *
 * @post No register has been modified by this call.
 * @post Returned pointer is non-NULL.
 *
 * @note Thread safety: pointer arithmetic is reentrant; the caller
 *       must serialize concurrent register writes.
 * @since 0.1.0
 */
RA8_HW_REGISTER_ACCESS
static inline volatile uint32_t* ra8_lpm_scb_scr(void)
{
  return (volatile uint32_t*)k_ra8_lpm_scb_scr_addr;
}

/**
 * @brief Return a typed pointer to an 8-bit SYSC register.
 *
 * @param[in] off Byte offset (one of ``k_ra8_lpm_*_off``).
 * @return Volatile pointer suitable for direct dereference.
 *
 * @pre ``off`` corresponds to a documented 8-bit register.
 * @pre SYSC bus is clocked.
 *
 * @post No register has been modified by this call.
 * @post Returned pointer is non-NULL.
 *
 * @note Thread safety: pointer arithmetic is reentrant; the caller
 *       must serialize concurrent register writes.
 * @since 0.1.0
 */
RA8_HW_REGISTER_ACCESS
static inline volatile uint8_t* ra8_lpm_sysc_reg8(ra8_lpm_off_t off)
{
  return (volatile uint8_t*)(k_ra8_lpm_sysc_base_addr + (uintptr_t)off);
}

/**
 * @brief Return a typed pointer to a 16-bit SYSC register.
 *
 * @param[in] off Byte offset (one of ``k_ra8_lpm_*_off``).
 * @return Volatile pointer suitable for direct dereference.
 *
 * @pre ``off`` corresponds to a documented 16-bit register
 *      (PDRAMSCR0 or PRCR).
 * @pre SYSC bus is clocked.
 *
 * @post No register has been modified by this call.
 * @post Returned pointer is non-NULL.
 *
 * @note Thread safety: pointer arithmetic is reentrant; the caller
 *       must serialize concurrent register writes.
 * @since 0.1.0
 */
RA8_HW_REGISTER_ACCESS
static inline volatile uint16_t* ra8_lpm_sysc_reg16(ra8_lpm_off_t off)
{
  return (volatile uint16_t*)(k_ra8_lpm_sysc_base_addr + (uintptr_t)off);
}

/**
 * @brief Return a typed pointer to a 32-bit ICU/WUPEN register.
 *
 * @param[in] off Byte offset (``k_ra8_lpm_wupen0_off`` or
 *                ``k_ra8_lpm_wupen1_off``).
 * @return Volatile pointer suitable for direct dereference.
 *
 * @pre ``off`` corresponds to a documented 32-bit ICU register.
 * @pre ICU bus is clocked.
 *
 * @post No register has been modified by this call.
 * @post Returned pointer is non-NULL.
 *
 * @note Thread safety: pointer arithmetic is reentrant; the caller
 *       must serialize concurrent register writes.
 * @since 0.1.0
 */
RA8_HW_REGISTER_ACCESS
static inline volatile uint32_t* ra8_lpm_icu_reg32(ra8_lpm_off_t off)
{
  return (volatile uint32_t*)(k_ra8_lpm_icu_base_addr + (uintptr_t)off);
}

#ifdef __cplusplus
}
#endif
