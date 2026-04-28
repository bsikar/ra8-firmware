/**
 * @file ra8d2_flash_regs.h
 * @brief Flash / MRAM controller register layout for the Renesas RA8D2
 *
 * @details
 * On RA8D2 the primary non-volatile code memory is MRAM, not
 * traditional NOR flash, but the controller interface follows the RA
 * "MRMS" IP. Writes to code-MRAM use a small store-window plus a
 * keyed flush; updates to anti-rollback counters, the start-up area
 * and other configuration-set words go through the MACI command
 * sequencer; reads of either region are transparent.
 *
 * All register offsets, field masks and key codes here are derived
 * from the Hardware User's Manual:
 *   - Chapter 7 "Option-Setting Memory" pages 278..299
 *   - Chapter 59 "MRAM" pages 3542..3625
 *
 * The companion driver (``libs/ra_hal/src/ra_flash.c``) cites the
 * exact HUM page each time it pokes one of these registers. That
 * keeps the math here a passive layout description: all policy
 * lives in the driver.
 *
 * @note FSP names a few of these registers slightly differently
 *       (e.g. ``ECCBYPC`` vs ``ECCEN``); we use the HUM names
 *       wherever they differ. References to FSP source files in
 *       comments are pointers, never copies of the FSP code.
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
 * Memory map: code-MRAM, extra-MRAM, OFS aliases
 * =============================================================================
 */

/**
 * @enum ra_flash_map_t
 * @brief Address bounds of the user-visible MRAM windows.
 *
 * @details
 * The code-MRAM lives at 0x02000000 and is 1 MiB. The extra-MRAM
 * (a.k.a. data flash) lives at 0x27000000 and is 12 KiB. Anti-
 * rollback counters and the start-up area-select fuse live in the
 * Option-Setting Memory window starting at 0x02C9F000.
 */
typedef enum : uintptr_t {
  /* HUM Ch 59.1 "Address Map" p 3543 */
  k_ra_flash_code_start = 0x02000000UL, /**< Start of code MRAM region.        */
  k_ra_flash_code_size  = 0x00100000UL, /**< 1 MiB of code MRAM.               */
  k_ra_flash_extra_start = 0x27000000UL, /**< Start of extra MRAM (data flash). */
  k_ra_flash_extra_size  = 0x00003000UL, /**< 12 KiB of extra MRAM.            */

  /* HUM Ch 7.2.21 "ARCCS Anti-Rollback Counter Configuration" p 296 */
  k_ra_flash_ofs_arccs_addr    = 0x02E17932UL,
  /* HUM Ch 7.2.22 "ARC_SEC" p 296 */
  k_ra_flash_ofs_arc_sec_addr  = 0x02F27E00UL,
  /* HUM Ch 7.2.23 "ARC_NSEC" p 297 */
  k_ra_flash_ofs_arc_nsec_addr = 0x02F27E08UL,
} ra_flash_map_t;

/**
 * @enum ra_mram_base_addr_t
 * @brief MRMS controller base addresses.
 *
 * @details
 * The MRMS register window is 0x3808 bytes. Secure alias starts at
 * 0x4013C000. The non-secure alias (BASE_NS_OFFSET = 0x10000000) is
 * 0x5013C000. The MACI command-issuing area is a small two-register
 * window at 0x40120000 (HUM Ch 59 p 3550).
 */
typedef enum : uintptr_t {
  /* HUM Ch 59.1 "Address Map" p 3543 */
  k_ra_mram_base_addr    = 0x4013C000UL, /**< Secure MRMS base.       */
  k_ra_mram_base_addr_ns = 0x5013C000UL, /**< Non-secure MRMS alias.  */
  k_ra_mram_cmd_base     = 0x40120000UL, /**< MACI command issue area. */
} ra_mram_base_addr_t;

/* =============================================================================
 * Register offsets within the MRMS window
 * =============================================================================
 */

/**
 * @enum ra_mram_off_t
 * @brief Byte offsets into the MRMS register window.
 *
 * @details
 * Values track the layout in ``R_MRMS_Type`` from the FSP CMSIS
 * device header (R7KA8D2KF_core0.h), which mirrors the HUM Ch 59
 * register descriptions on pages 3551..3624.
 */
typedef enum : uint16_t {
  /* HUM Ch 59.5.1 "MRCPFB : Code MRAM Pre-Fetch Buffer Enable Register" p 3551 */
  k_ra_mram_off_mrcpfb    = 0x0000U,
  /* HUM Ch 59.5.2 "MRCFREQ : Code MRAM Frequency Notifications Register" p 3551 */
  k_ra_mram_off_mrcfreq   = 0x0004U,
  /* HUM Ch 59.5.3 "MREFREQ : Extra MRAM Frequency Notifications Register" p 3552 */
  k_ra_mram_off_mrefreq   = 0x0008U,
  /* HUM Ch 59.5.4 "MRCDECC : Code MRAM ECC Decoder Control Register" p 3554 */
  k_ra_mram_off_mrcdecc   = 0x0010U,
  /* HUM Ch 59.5.5 "MRCRAEINT : Code MRAM Read Access Error Interrupt Enable" p 3554 */
  k_ra_mram_off_mrcraeint = 0x0014U,
  /* HUM Ch 59.5.6 "MRCRAES : Code MRAM Read Access Error Status Register" p 3554 */
  k_ra_mram_off_mrcraes   = 0x0018U,
  /* HUM Ch 59.5.7 "MRCRTEA : Code MRAM TED Error Address Register" p 3555 */
  k_ra_mram_off_mrcrtea   = 0x001CU,
  /* HUM Ch 59.5.8 "MRCRDEA : Code MRAM DEC Error Address Register" p 3555 */
  k_ra_mram_off_mrcrdea   = 0x0020U,
  /* HUM Ch 59.5.9 "MRERAINT : Extra MRAM Read Access Error Interrupt Enable" p 3557 */
  k_ra_mram_off_mreraint  = 0x0034U,
  /* HUM Ch 59.5.10 "MRERAES : Extra MRAM Read Access Error Status Register" p 3557 */
  k_ra_mram_off_mreraes   = 0x0038U,
  /* HUM Ch 59.5.11 "MRERTEA : Extra MRAM TED Error Address Register" p 3558 */
  k_ra_mram_off_mrertea   = 0x003CU,
  /* HUM Ch 59.5.12 "MRERDEA : Extra MRAM DEC Error Address Register" p 3558 */
  k_ra_mram_off_mrerdea   = 0x0040U,
  /* HUM Ch 59.5.13 "MSAR : MRAM Security Attribution Register" p 3559 */
  k_ra_mram_off_msar      = 0x0100U,
  /* HUM Ch 59.5.14 "MREZS : Extra MRAM Zeroization Status Register" p 3565 */
  k_ra_mram_off_mrezs     = 0x0400U,
  /* HUM Ch 59.5.15 "MREZC : Extra MRAM Zeroization Control Register" p 3565 */
  k_ra_mram_off_mrezc     = 0x0404U,
  /* HUM Ch 59.5.16 "MASTAT : Extra MRAM Access Status Register" p 3577 */
  k_ra_mram_off_mastat    = 0x2010U,
  /* HUM Ch 59.5.17 "MPAEINT : Extra MRAM Access Error Interrupt Enable" p 3577 */
  k_ra_mram_off_mpaeint   = 0x2014U,
  /* HUM Ch 59.5.18 "MRDYIE : Extra MRAM Ready Interrupt Enable Register" p 3577 */
  k_ra_mram_off_mrdyie    = 0x2018U,
  /* HUM Ch 59.5.19 "MSADDR : MACI Command Start Address Register" p 3573 */
  k_ra_mram_off_msaddr    = 0x2030U,
  /* HUM Ch 59.5.20 "MCNTSELR : MRAM Counter Select Register" p 3576 */
  k_ra_mram_off_mcntselr  = 0x2048U,
  /* HUM Ch 59.5.21 "MCNTDTR0 : MRAM Counter Data Register 0" p 3576 */
  k_ra_mram_off_mcntdtr0  = 0x204CU,
  /* HUM Ch 59.5.22 "MCNTDTR1 : MRAM Counter Data Register 1" p 3577 */
  k_ra_mram_off_mcntdtr1  = 0x2050U,
  /* HUM Ch 59.5.23 "MCTRCNTR : MRAM Configuration Update Transfer Control" p 3580 */
  k_ra_mram_off_mctrcntr  = 0x2060U,
  /* HUM Ch 59.5.24 "MCTRLSR : MRAM Configuration Update Transfer List Select" p 3580 */
  k_ra_mram_off_mctrlsr   = 0x2064U,
  /* HUM Ch 59.5.25 "MCTRSTATR : MRAM Configuration Update Transfer Status" p 3580 */
  k_ra_mram_off_mctrstatr = 0x206CU,
  /* HUM Ch 59.5.26 "MSTATR : Extra MRAM Status Register" p 3578 */
  k_ra_mram_off_mstatr    = 0x2080U,
  /* HUM Ch 59.5.27 "MENTRYR : Extra MRAM Program Mode Entry Register" p 3582 */
  k_ra_mram_off_mentryr   = 0x2084U,
  /* HUM Ch 59.5.28 "MSUINITR : Extra MRAM Sequencer Set-Up Initialization" p 3585 */
  k_ra_mram_off_msuinitr  = 0x208CU,
  /* HUM Ch 59.5.29 "MCMDR : MACI Command Register" p 3589 */
  k_ra_mram_off_mcmdr     = 0x20A0U,
  /* HUM Ch 59.5.30 "MSUASMON : MRAM Start-Up Area Select Monitor Register" p 3593 */
  k_ra_mram_off_msuasmon  = 0x20DCU,
  /* HUM Ch 59.5.31 "MSUACR : MRAM Start-Up Area Control Register" p 3593 */
  k_ra_mram_off_msuacr    = 0x20E8U,
  /* HUM Ch 59.5.32 "MRPSC : MRAM Program Speed Control Register" p 3600 */
  k_ra_mram_off_mrpsc     = 0x2800U,
  /* HUM Ch 59.5.33 "MRCPC0 : Code MRAM Program Control Register" p 3601 */
  k_ra_mram_off_mrcpc0    = 0x3000U,
  /* HUM Ch 59.5.34 "MRCPC1 : Code MRAM Program Control for Secure Register" p 3601 */
  k_ra_mram_off_mrcpc1    = 0x3004U,
  /* HUM Ch 59.5.35 "MRCBPROT0 : Code MRAM Block Protection Register" p 3604 */
  k_ra_mram_off_mrcbprot0 = 0x3008U,
  /* HUM Ch 59.5.36 "MRCBPROT1 : Code MRAM Block Protection for Secure Register" p 3605 */
  k_ra_mram_off_mrcbprot1 = 0x300CU,
  /* HUM Ch 59.5.37 "MRCPS : Code MRAM Program Status Register" p 3601 */
  k_ra_mram_off_mrcps     = 0x3010U,
  /* HUM Ch 59.5.38 "MRCPAEINT : Code MRAM Program Access Error Interrupt Enable" p 3601 */
  k_ra_mram_off_mrcpaeint = 0x3014U,
  /* HUM Ch 59.5.39 "MRCPEA : Code MRAM Program Error Address Register" p 3601 */
  k_ra_mram_off_mrcpea    = 0x3018U,
  /* HUM Ch 59.5.40 "MRCFLR : Code MRAM Flush Register" p 3601 */
  k_ra_mram_off_mrcflr    = 0x3030U,
  /* HUM Ch 59.5.41 "MRCEECC : Code MRAM ECC Encoder Control Register" p 3624 */
  k_ra_mram_off_mrceecc   = 0x3804U,
} ra_mram_off_t;

/* =============================================================================
 * MRCPS field masks  (HUM Ch 59 "MRCPS" p 3601)
 * =============================================================================
 */

/**
 * @enum ra_mrcps_mask_t
 * @brief Bit masks for the MRCPS Code MRAM Program Status Register.
 *
 * @details
 * The error bits (PRGERRC, ECCERRC) are W1C; the busy bits
 * (ABUFEMP, ABUFFULL, PRGBSYC) are read-only status.
 */
typedef enum : uint8_t {
  /* HUM Ch 59 "MRCPS : Code MRAM Program Status Register" p 3601 */
  k_ra_mrcps_mask_prgerrc  = 0x01U, /**< Programming error.        */
  k_ra_mrcps_mask_eccerrc  = 0x02U, /**< ECC error.                */
  k_ra_mrcps_mask_abufemp  = 0x20U, /**< Address buffer empty.     */
  k_ra_mrcps_mask_abuffull = 0x40U, /**< Address buffer full.      */
  k_ra_mrcps_mask_prgbsyc  = 0x80U, /**< Code MRAM program busy.   */
  k_ra_mrcps_mask_errors   = 0x03U, /**< W1C: PRGERRC + ECCERRC.   */
} ra_mrcps_mask_t;

/* =============================================================================
 * MRCEECC / MRCDECC ECC controls  (HUM Ch 59 p 3554, p 3624)
 * =============================================================================
 */

/**
 * @enum ra_mrceecc_mask_t
 * @brief Field masks and key for the MRCEECC ECC encoder control.
 *
 * @details
 * The HUM names the enable bit ``ECCBYPC`` (bypass-clear, active high
 * for "encoder enabled"). We expose it as ``eccen`` to keep driver
 * code intent-readable.
 */
typedef enum : uint16_t {
  /* HUM Ch 59 "MRCEECC : Code MRAM ECC Encoder Control" p 3624 */
  k_ra_mrceecc_mask_eccen = 0x0001U, /**< Encoder enable bit.            */
  k_ra_mrceecc_key_shift  = 0xC000U, /**< Pre-shifted KEY[15:8]=0xC0.    */
} ra_mrceecc_mask_t;

/**
 * @enum ra_mrcdecc_mask_t
 * @brief Field masks and key for the MRCDECC ECC decoder control.
 */
typedef enum : uint16_t {
  /* HUM Ch 59 "MRCDECC : Code MRAM ECC Decoder Control" p 3554 */
  k_ra_mrcdecc_mask_dececen = 0x0002U, /**< Decoder enable (ECCSELC).    */
  k_ra_mrcdecc_key_shift    = 0x8C00U, /**< Pre-shifted KEY[15:8]=0x8C.  */
} ra_mrcdecc_mask_t;

/* =============================================================================
 * Read-error and access-error masks
 * =============================================================================
 */

/**
 * @enum ra_mrcraes_mask_t
 * @brief MRCRAES / MRERAES read-access error status bits.
 *
 * @details
 * Same layout in both code and extra registers (DEC at bit 0, TED at
 * bit 1), so the same enum is reused for both via the names
 * ``k_ra_mrcraes_mask_*``.
 */
typedef enum : uint8_t {
  /* HUM Ch 59 "MRCRAES : Code MRAM Read Access Error Status" p 3554 */
  k_ra_mrcraes_mask_decerrc = 0x01U, /**< 2-bit DEC error.          */
  k_ra_mrcraes_mask_tederrc = 0x02U, /**< 1-bit TED error.          */
  k_ra_mrcraes_mask_any     = 0x03U, /**< Either 1-bit or 2-bit. */
} ra_mrcraes_mask_t;

/**
 * @enum ra_mrcraeint_mask_t
 * @brief MRCRAEINT / MRERAINT IRQ-enable bits.
 *
 * @details
 * INTENBDC enables the DEC IRQ; INTENBTC enables the TED IRQ.
 */
typedef enum : uint8_t {
  /* HUM Ch 59 "MRCRAEINT : Code MRAM Read Access Error IRQ Enable" p 3554 */
  k_ra_mrcraeint_mask_intenbdc = 0x01U, /**< DEC error IRQ enable.   */
  k_ra_mrcraeint_mask_intenbtc = 0x02U, /**< TED error IRQ enable.   */
} ra_mrcraeint_mask_t;

/**
 * @enum ra_mrcpaeint_mask_t
 * @brief MRCPAEINT field mask.
 */
typedef enum : uint8_t {
  /* HUM Ch 59 "MRCPAEINT : Code MRAM Program Access Error IRQ Enable" p 3601 */
  k_ra_mrcpaeint_mask_mrcaeie = 0x80U, /**< Code program-error IRQ enable. */
} ra_mrcpaeint_mask_t;

/**
 * @enum ra_mpaeint_mask_t
 * @brief MPAEINT field masks (extra MRAM access-error IRQ enable).
 */
typedef enum : uint8_t {
  /* HUM Ch 59 "MPAEINT : Extra MRAM Access Error IRQ Enable" p 3577 */
  k_ra_mpaeint_mask_mreaeie = 0x08U, /**< Access-error IRQ enable.       */
  k_ra_mpaeint_mask_cmdlkie = 0x10U, /**< Command-lock IRQ enable.       */
} ra_mpaeint_mask_t;

/**
 * @enum ra_mrdyie_mask_t
 * @brief MRDYIE field mask.
 */
typedef enum : uint8_t {
  /* HUM Ch 59 "MRDYIE : Extra MRAM Ready Interrupt Enable" p 3577 */
  k_ra_mrdyie_mask_mrdyie = 0x01U, /**< MRDY IRQ enable.                  */
} ra_mrdyie_mask_t;

/**
 * @enum ra_mastat_mask_t
 * @brief MASTAT field masks.
 */
typedef enum : uint8_t {
  /* HUM Ch 59 "MASTAT : Extra MRAM Access Status Register" p 3577 */
  k_ra_mastat_mask_mreae = 0x08U, /**< Extra MRAM access error.        */
  k_ra_mastat_mask_cmdlk = 0x10U, /**< Command lock latched.           */
} ra_mastat_mask_t;

/**
 * @enum ra_mrezs_mask_t
 * @brief MREZS field masks (zeroization status).
 */
typedef enum : uint8_t {
  /* HUM Ch 59 "MREZS : Extra MRAM Zeroization Status Register" p 3565 */
  k_ra_mrezs_mask_whukzf  = 0x01U, /**< W-HUK Zero Flag.               */
  k_ra_mrezs_mask_whukexe = 0x02U, /**< W-HUK Zeroization Executing.   */
} ra_mrezs_mask_t;

/**
 * @enum ra_mrezc_t
 * @brief MREZC keyed control words (zeroization).
 *
 * @details
 * The HUM specifies WHUKZE[2:0] = 0b101 + KEY[15:8] = 0xA5 to launch
 * a full W-HUK zeroize.
 */
typedef enum : uint16_t {
  /* HUM Ch 59 "MREZC : Extra MRAM Zeroization Control" p 3565 */
  k_ra_mrezc_full_zero = 0xA505U,
} ra_mrezc_t;

/* =============================================================================
 * MENTRYR / MSUACR / MSUINITR / MSUASMON
 * =============================================================================
 */

/**
 * @enum ra_mentryr_t
 * @brief Keyed values and field masks for the MENTRYR P/E entry register.
 */
typedef enum : uint16_t {
  /* HUM Ch 59 "MENTRYR : Extra MRAM Program-Mode Entry" p 3582 */
  k_ra_mentryr_pe_enter      = 0xAA80U, /**< KEY=0xAA, MENTRY=1.      */
  k_ra_mentryr_read_mode     = 0xAA00U, /**< KEY=0xAA, MENTRY=0.      */
  k_ra_mentryr_mask_pe_mode  = 0x0080U, /**< MENTRY status bit.       */
} ra_mentryr_t;

/**
 * @enum ra_msuacr_t
 * @brief Keyed values for MSUACR start-up swap.
 *
 * @details
 * SAS[1:0] = 0b00 selects default; 0b01 selects alternate. KEY[15:8]
 * must be 0x66 for the write to take effect.
 */
typedef enum : uint16_t {
  /* HUM Ch 59 "MSUACR : Start-Up Area Control Register" p 3593 */
  k_ra_msuacr_key             = 0x6600U,
  k_ra_msuacr_swap_default    = 0x0000U,
  k_ra_msuacr_swap_alternate  = 0x0001U,
} ra_msuacr_t;

/**
 * @enum ra_msuasmon_mask_t
 * @brief MSUASMON read-only field masks.
 */
typedef enum : uint32_t {
  /* HUM Ch 59 "MSUASMON : Start-Up Area Monitor" p 3593 */
  k_ra_msuasmon_mask_fspr  = 0x00008000UL, /**< Protection flag.    */
  k_ra_msuasmon_mask_btflg = 0x80000000UL, /**< Boot-swap flag.     */
} ra_msuasmon_mask_t;

/**
 * @enum ra_msuinitr_t
 * @brief Keyed value + field mask for MSUINITR.
 */
typedef enum : uint16_t {
  /* HUM Ch 59 "MSUINITR : Extra MRAM Sequencer Set-Up Init" p 3585 */
  k_ra_msuinitr_full_init    = 0xAA01U, /**< KEY=0xAA, SUINIT=1.    */
  k_ra_msuinitr_mask_suinit  = 0x0001U, /**< SUINIT status bit.     */
} ra_msuinitr_t;

/* =============================================================================
 * MCMDR / MSTATR
 * =============================================================================
 */

/**
 * @enum ra_mcmdr_mask_t
 * @brief MCMDR field masks (MACI command status snapshot).
 */
typedef enum : uint16_t {
  /* HUM Ch 59 "MCMDR : MACI Command Register" p 3589 */
  k_ra_mcmdr_mask_pcmdr        = 0x00FFU, /**< Pre-command flag.     */
  k_ra_mcmdr_mask_cmdr         = 0xFF00U, /**< Command flag.         */
  k_ra_mcmdr_mask_cmd_progress = 0xFFFFU, /**< Any command in flight. */
} ra_mcmdr_mask_t;

/**
 * @enum ra_mstatr_mask_t
 * @brief MSTATR field masks (extra-MRAM status flags).
 */
typedef enum : uint32_t {
  /* HUM Ch 59 "MSTATR : Extra MRAM Status Register" p 3578 */
  k_ra_mstatr_mask_cfgseterr = 0x00000020UL, /**< Configuration set error. */
  k_ra_mstatr_mask_prgerr    = 0x00001000UL, /**< Program error.            */
  k_ra_mstatr_mask_ilglerr   = 0x00004000UL, /**< Illegal error.            */
  k_ra_mstatr_mask_mrdy      = 0x00008000UL, /**< MRE ready.                */
  k_ra_mstatr_mask_tzferr    = 0x00080000UL, /**< TrustZone filter error.   */
  k_ra_mstatr_mask_oterr     = 0x00100000UL, /**< Other error.              */
  k_ra_mstatr_mask_secerr    = 0x00200000UL, /**< Security error.           */
  k_ra_mstatr_mask_ilgcomerr = 0x00800000UL, /**< Illegal command error.    */
  /* Aggregate of all error bits used by the driver to gate command success. */
  k_ra_mstatr_mask_any_err   = 0x009850A0UL, /**< Composite error mask.     */
} ra_mstatr_mask_t;

/* =============================================================================
 * MRPSC / MRCPC0 / MRCPC1 / MRCBPROT0 / MRCBPROT1 / MRCFLR
 * =============================================================================
 */

/**
 * @enum ra_mrpsc_mask_t
 * @brief MRPSC field masks (high-speed program enable).
 */
typedef enum : uint8_t {
  /* HUM Ch 59 "MRPSC : MRAM Program Speed Control Register" p 3600 */
  k_ra_mrpsc_mask_mhspen = 0x01U, /**< High-speed program enable.    */
} ra_mrpsc_mask_t;

/**
 * @enum ra_mrcpc0_t
 * @brief Keyed values for MRCPC0 (non-secure program-gate).
 *
 * @details
 * KEY[15:8] = 0x86. Bit 0 enables programming when set.
 */
typedef enum : uint16_t {
  /* HUM Ch 59 "MRCPC0 : Code MRAM Program Control Register" p 3601 */
  k_ra_mrcpc0_key_enable  = 0x8601U,
  k_ra_mrcpc0_key_disable = 0x8600U,
} ra_mrcpc0_t;

/**
 * @enum ra_mrcpc1_t
 * @brief Keyed values for MRCPC1 (secure program-gate).
 *
 * @details
 * KEY[15:8] = 0x68.
 */
typedef enum : uint16_t {
  /* HUM Ch 59 "MRCPC1 : Code MRAM Program Control for Secure Register" p 3601 */
  k_ra_mrcpc1_key_enable  = 0x6801U,
  k_ra_mrcpc1_key_disable = 0x6800U,
} ra_mrcpc1_t;

/**
 * @enum ra_mrcbprot0_t
 * @brief Keyed values for MRCBPROT0 (non-secure block-protection cancel).
 *
 * @details
 * BPCN0 = 0 locks the block (default protect-on); = 1 cancels.
 * KEY[15:8] = 0x88.
 */
typedef enum : uint16_t {
  /* HUM Ch 59 "MRCBPROT0 : Code MRAM Block Protection (NS)" p 3604 */
  k_ra_mrcbprot0_key_lock   = 0x8800U, /**< Keep block protect on.   */
  k_ra_mrcbprot0_key_unlock = 0x8801U, /**< Cancel block protect.    */
} ra_mrcbprot0_t;

/**
 * @enum ra_mrcbprot1_t
 * @brief Keyed values for MRCBPROT1 (secure block-protection cancel).
 *
 * @details
 * KEY[15:8] = 0x44.
 */
typedef enum : uint16_t {
  /* HUM Ch 59 "MRCBPROT1 : Code MRAM Block Protection (S)" p 3605 */
  k_ra_mrcbprot1_key_lock   = 0x4400U,
  k_ra_mrcbprot1_key_unlock = 0x4401U,
} ra_mrcbprot1_t;

/**
 * @enum ra_mrcflr_t
 * @brief Keyed value for MRCFLR (write-buffer flush).
 *
 * @details
 * 0xC301 sets MRCFL=1 with KEY[15:8]=0xC3 (HUM Ch 59 p 3601).
 */
typedef enum : uint16_t {
  /* HUM Ch 59 "MRCFLR : Code MRAM Flush Register" p 3601 */
  k_ra_mrcflr_key_flush = 0xC301U,
} ra_mrcflr_t;

/* =============================================================================
 * Update-transfer registers (MCTRCNTR / MCTRSTATR / MCTRLSR)
 * =============================================================================
 */

/**
 * @enum ra_mctrcntr_t
 * @brief Keyed values + masks for MCTRCNTR.
 *
 * @details
 * KEY[15:8] = 0xA5 per the standard RA family transfer-trigger
 * convention. Bit 0 (TRTRG) starts the transfer.
 */
typedef enum : uint16_t {
  /* HUM Ch 59 "MCTRCNTR : MRAM Update Transfer Control" p 3580 */
  k_ra_mctrcntr_key        = 0xA500U,
  k_ra_mctrcntr_mask_start = 0x0001U,
} ra_mctrcntr_t;

/**
 * @enum ra_mctrlsr_mask_t
 * @brief MCTRLSR field mask (transfer-list select).
 */
typedef enum : uint8_t {
  /* HUM Ch 59 "MCTRLSR : MRAM Update Transfer List Select" p 3580 */
  k_ra_mctrlsr_mask_list_sel = 0x07U,
} ra_mctrlsr_mask_t;

/**
 * @enum ra_mctrstatr_mask_t
 * @brief MCTRSTATR field masks.
 *
 * @details
 * The HUM only defines TRBUSY (bit 0) and TRMD (bit 2). The driver
 * surfaces an additional "done" view as TRMD inverted from TRBUSY,
 * and an "err" view masking the upper bits for forward-compat.
 */
typedef enum : uint16_t {
  /* HUM Ch 59 "MCTRSTATR : MRAM Update Transfer Status" p 3580 */
  k_ra_mctrstatr_mask_busy = 0x0001U, /**< TRBUSY.                   */
  k_ra_mctrstatr_mask_done = 0x0004U, /**< TRMD.                     */
  k_ra_mctrstatr_mask_err  = 0x00F8U, /**< Reserved-future error.    */
} ra_mctrstatr_mask_t;

/* =============================================================================
 * MCNTSELR (counter select) and ARC layout
 * =============================================================================
 */

/**
 * @enum ra_mcntselr_t
 * @brief MCNTSELR field-value codes for the four counter families.
 */
typedef enum : uint8_t {
  /* HUM Ch 59 "MCNTSELR : MRAM Counter Select Register" p 3576 */
  k_ra_mcntselr_sec     = 1U, /**< ARC_SEC.                         */
  k_ra_mcntselr_oembl   = 2U, /**< ARC_OEMBL (OEM bootloader).      */
  k_ra_mcntselr_nsec_0  = 4U, /**< ARC_NSEC index 0; +1/+2/+3 follow. */
  k_ra_mcntselr_mask    = 0x07U, /**< CNTSEL[2:0].                  */
} ra_mcntselr_t;

/**
 * @enum ra_arc_limits_t
 * @brief Per-counter bit limits and ARCCS field masks.
 *
 * @details
 * Values come from BSP_FEATURE_FLASH_ARC_*_MAX_COUNT in the FSP
 * RA8D1/RA8T2 BSP feature header (the RA8D2 inherits the same MRAM
 * macro). HUM Ch 7.2.21 p 296 fixes the ARCCS.ARCNS encoding.
 */
typedef enum : uint32_t {
  /* HUM Ch 7.2.22 "ARC_SEC" p 296 */
  k_ra_arc_sec_max_bits   = 64UL,
  /* HUM Ch 7.2.21 "ARCCS OEMBL counter" p 296 */
  k_ra_arc_oembl_max_bits = 64UL,
  /* HUM Ch 7.2.21 "ARCCS NSEC single-counter mode" p 296 */
  k_ra_arc_nsec_single    = 256UL,
  /* HUM Ch 7.2.21 "ARCCS NSEC multi-counter mode" p 296 */
  k_ra_arc_nsec_multiple  = 64UL,
  /* HUM Ch 7.2.21 "ARCCS ARCNS field mask" p 296 */
  k_ra_arc_arccs_mask     = 0x03UL,
  /* HUM Ch 7.2.21 "ARCCS ARCNS=1 selects single big counter" p 296 */
  k_ra_arc_arcns_single   = 1UL,
} ra_arc_limits_t;

/* =============================================================================
 * MACI command opcodes and configuration-set targets
 * =============================================================================
 */

/**
 * @enum ra_maci_cmd_t
 * @brief MACI command opcodes used by the driver.
 *
 * @details
 * Pulled from the FSP r_mram.c command list (lines 36..43); each
 * value is the literal opcode the HUM Ch 59 p 3550 "MACI Command-
 * Issuing Area" expects when streaming bytes through MACI_CMD8.
 */
typedef enum : uint8_t {
  /* HUM Ch 59 "MACI Command-Issuing Area" p 3550 */
  k_ra_maci_cmd_status_clear       = 0x50U,
  k_ra_maci_cmd_forced_stop        = 0xB3U,
  k_ra_maci_cmd_config_set_1       = 0x40U,
  k_ra_maci_cmd_config_set_2       = 0x08U,
  k_ra_maci_cmd_increment_counter  = 0x35U,
  k_ra_maci_cmd_read_counter       = 0x39U,
  k_ra_maci_cmd_final              = 0xD0U,
} ra_maci_cmd_t;

/**
 * @enum ra_msaddr_t
 * @brief Well-known MSADDR targets for configuration-set writes.
 *
 * @details
 * 0x02C9F070 is the start-up-area select (BTFLG / BTSIZE) word
 * group; the SAS halfword sits 4 bytes deeper at 0x02C9F074.
 */
typedef enum : uint32_t {
  /* HUM Ch 7 "Option-Setting Memory" p 278 */
  k_ra_msaddr_config_set_startup = 0x02C9F070UL,
  /* HUM Ch 7 "OFS SAS region" p 278 */
  k_ra_msaddr_ofs_sas            = 0x02C9F074UL,
} ra_msaddr_t;

/* =============================================================================
 * Driver-facing data constants
 * =============================================================================
 */

/**
 * @enum ra_mram_data_const_t
 * @brief Sizes used by the driver when chunking writes / counter reads.
 */
typedef enum : uint32_t {
  /* HUM Ch 59.4.2 "Code MRAM programming" p 3548 */
  k_ra_mram_write_size_bytes      = 32UL, /**< Minimum programmable unit.   */
  /* HUM Ch 59.4.2 "Block layout" p 3548 */
  k_ra_mram_block_size_bytes      = 32UL, /**< One programmable block.      */
  /* HUM Ch 59 "Configuration-set MACI sequence" p 3550 */
  k_ra_mram_config_set_word_count = 8UL,  /**< 8 halfwords per MACI write.  */
  /* HUM Ch 7.2.23 "ARC_NSEC layout" p 297 */
  k_ra_mram_arc_max_words         = 16UL, /**< Max 32-bit words per ARC.    */
} ra_mram_data_const_t;

/* =============================================================================
 * Inline accessors: register windows and MACI command issue area
 * =============================================================================
 */

/**
 * @brief Pointer to a byte-wide MRMS register at offset ``off``.
 *
 * @param[in] off Byte offset into the MRMS window.
 * @return Volatile pointer suitable for a single-byte load/store.
 *
 * @pre ``off`` is one of the ``k_ra_mram_off_*`` values (the driver
 *      casts the enum to ``uint16_t`` at the call site).
 * @pre Caller has powered the MRMS controller through CGC.
 * @post No side effect; returns a valid pointer for the host
 *       simulator (mmap-backed) or hardware bus address on target.
 *
 * @note Accessor is ``static inline`` so each driver TU embeds the
 *       address arithmetic without function-call overhead.
 */
static inline volatile uint8_t* ra_mram_reg8(uint16_t off)
{
  return (volatile uint8_t*)(k_ra_mram_base_addr + (uintptr_t)off);
}

/**
 * @brief Pointer to a halfword-wide MRMS register at offset ``off``.
 *
 * @param[in] off Byte offset into the MRMS window; must be 16-bit aligned.
 * @return Volatile pointer for a 16-bit load/store.
 *
 * @pre ``off`` & 1 == 0.
 * @pre Caller has powered the MRMS controller.
 * @post No side effect.
 */
static inline volatile uint16_t* ra_mram_reg16(uint16_t off)
{
  return (volatile uint16_t*)(k_ra_mram_base_addr + (uintptr_t)off);
}

/**
 * @brief Pointer to a word-wide MRMS register at offset ``off``.
 *
 * @param[in] off Byte offset into the MRMS window; must be 32-bit aligned.
 * @return Volatile pointer for a 32-bit load/store.
 *
 * @pre ``off`` & 3 == 0.
 * @pre Caller has powered the MRMS controller.
 * @post No side effect.
 */
static inline volatile uint32_t* ra_mram_reg32(uint16_t off)
{
  return (volatile uint32_t*)(k_ra_mram_base_addr + (uintptr_t)off);
}

/**
 * @brief Pointer to the MACI command-issuing area, byte-wide.
 *
 * @return Volatile pointer for an 8-bit MACI opcode store.
 *
 * @pre Controller is in P/E mode (HUM Ch 59 p 3582 transitions).
 * @post Returns the same address every call; pure pointer
 *       arithmetic, no side effect on the bus.
 */
static inline volatile uint8_t* ra_mram_cmd8(void)
{
  return (volatile uint8_t*)k_ra_mram_cmd_base;
}

/**
 * @brief Pointer to the MACI command-issuing area, halfword-wide.
 *
 * @return Volatile pointer for a 16-bit MACI payload store.
 *
 * @pre Controller is in P/E mode.
 * @post No side effect.
 */
static inline volatile uint16_t* ra_mram_cmd16(void)
{
  return (volatile uint16_t*)k_ra_mram_cmd_base;
}

#ifdef __cplusplus
}
#endif
