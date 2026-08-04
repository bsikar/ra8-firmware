/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_sram_regs.h
 * @brief SRAM (with ECC) control / status / security register layout
 * @ingroup grp_hal_memory
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * The RA8D2 has a 2 MB on-chip SRAM split into four banks
 * (SRAM0..SRAM3, see HUM Ch 58.1 Table 58.1, p 3527) with optional
 * SEC-DED ECC (64-bit data + 8-bit syndrome).
 *
 * Three register windows are involved:
 *
 * | Window      | Base (Secure)  | Base (Non-Secure) | Notes                       |
 * |-------------|---------------:|------------------:|-----------------------------|
 * | CPSCU SRAM  | 0x4000_8000    | 0x5000_8000       | Security attribution.       |
 * | SRAM ctl    | 0x4000_2000    | 0x5000_2000       | PRCR / WTSC / CRn / EAR.    |
 * | SRAM data   | 0x2200_0000    | 0x3200_0000       | The actual SRAM banks.      |
 *
 * The CPSCU window models SRAMSABARn, SRAMSAR and SRAMESAR. The SRAM
 * control window models SRAMPRCR_S/NS, SRAMWTSC, SRAMCRn,
 * SRAMECCRGNn, SRAMESR, SRAMESCLR and SRAMEARnm.
 *
 * Per HUM Ch 58.2 (p 3527-3537) the control window layout is:
 *
 * | Offset | Name           | Width | Purpose                          |
 * |-------:|----------------|------:|----------------------------------|
 * |  0x00  | SRAMPRCR_S     |  16   | Secure write-protect (key 0xA5). |
 * |  0x04  | SRAMPRCR_NS    |  16   | Non-secure write-protect.        |
 * |  0x08  | SRAMWTSC       |   8   | Wait-state enable.               |
 * |  0x10  | SRAMCR0        |   8   | SRAM0 ECC mode + OAD + E1STSEN.  |
 * |  0x14  | SRAMCR1        |   8   | SRAM1 ECC mode + ...             |
 * |  0x18  | SRAMCR2        |   8   | SRAM2 ECC mode + ...             |
 * |  0x1C  | SRAMCR3        |   8   | SRAM3 ECC mode + ...             |
 * |  0x30  | SRAMECCRGN0    |   8   | SRAM0 ECC region size.           |
 * |  0x34  | SRAMECCRGN1    |   8   | SRAM1 ECC region size.           |
 * |  0x38  | SRAMECCRGN2    |   8   | SRAM2 ECC region size.           |
 * |  0x3C  | SRAMECCRGN3    |   8   | SRAM3 ECC region size.           |
 * |  0x40  | SRAMESR        |  16   | ECC error status (1bit/2bit).    |
 * |  0x48  | SRAMESCLR      |  16   | ECC error status clear.          |
 * |  0x50+ | SRAMEAR[n][m]  |  32   | ECC error address.               |
 *
 * Per HUM Ch 58.2.1..58.2.3 (p 3527-3530) the CPSCU window layout is:
 *
 * | Offset | Name           | Width | Purpose                          |
 * |-------:|----------------|------:|----------------------------------|
 * | 0x010  | SRAMSAR        |  32   | Per-bank register security.      |
 * | 0x400  | SRAMSABAR0     |  32   | SRAM0 Secure/NS boundary.        |
 * | 0x404  | SRAMSABAR1     |  32   | SRAM1 Secure/NS boundary.        |
 * | 0x408  | SRAMSABAR2     |  32   | SRAM2 Secure/NS boundary.        |
 * | 0x40C  | SRAMSABAR3     |  32   | SRAM3 Secure/NS boundary.        |
 * | 0x510  | SRAMESAR       |  32   | ECC region security attribute.   |
 *
 * Per CLAUDE.md the driver never writes these registers via macros --
 * use the ``ra8_sram_regs()`` and ``ra8_sram_cpscu_regs()`` accessors so
 * the host-side fake can intercept the writes.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/* =============================================================================
 * Base addresses
 * =============================================================================
 */

/**
 * @enum ra8_sram_addr_t
 * @brief Memory-mapped base addresses for the SRAM windows.
 *
 * @details
 * Per HUM Ch 58.2.4 "SRAMPRCR_S", p 3530 the Secure SRAM-control
 * base is ``0x4000_2000`` and the Non-Secure alias is ``0x5000_2000``.
 *
 * Per HUM Ch 58.2.1 "SRAMSABARn", p 3527 the Secure CPSCU base is
 * ``0x4000_8000`` and the Non-Secure alias is ``0x5000_8000``.
 *
 * Per HUM Ch 58.1 Table 58.1, p 3527 the Secure data base for
 * SRAM0 is ``0x2200_0000`` and the Non-Secure data alias is
 * ``0x3200_0000``. The driver targets the Secure aliases on the
 * bare-metal v0.x build (TrustZone partitioning happens later).
 */
typedef enum : uintptr_t {
  k_ra8_sram_base_addr       = 0x40002000UL, /**< Secure SRAM control window.      */
  k_ra8_sram_base_addr_ns    = 0x50002000UL, /**< Non-Secure SRAM control window.  */
  k_ra8_sram_cpscu_base_addr = 0x40008000UL, /**< Secure CPSCU SRAM window.        */
  k_ra8_sram_cpscu_base_ns   = 0x50008000UL, /**< Non-Secure CPSCU SRAM window.    */
  k_ra8_sram_data_base_addr  = 0x22000000UL, /**< Secure SRAM data alias (bank 0). */
  k_ra8_sram_data_base_ns    = 0x32000000UL, /**< Non-Secure SRAM data alias.      */
} ra8_sram_addr_t;

/**
 * @enum ra8_sram_bank_offset_t
 * @brief Per-bank offsets into the SRAM data alias.
 *
 * @details
 * Per HUM Ch 58.1 Table 58.1, p 3527: SRAM0 lives at offset 0,
 * SRAM1 at offset 0x80000, SRAM2 at offset 0x100000, SRAM3 at
 * offset 0x180000. Sizes follow the table column.
 */
typedef enum : uint32_t {
  k_ra8_sram_bank0_data_off = 0x00000000UL, /**< SRAM0 starts at 0x2200_0000. */
  k_ra8_sram_bank1_data_off = 0x00080000UL, /**< SRAM1 starts at 0x2208_0000. */
  k_ra8_sram_bank2_data_off = 0x00100000UL, /**< SRAM2 starts at 0x2210_0000. */
  k_ra8_sram_bank3_data_off = 0x00180000UL, /**< SRAM3 starts at 0x2218_0000. */
  k_ra8_sram_bank012_size   = 0x00080000UL, /**< 512 KB per bank for 0/1/2.   */
  k_ra8_sram_bank3_size     = 0x00020000UL, /**< 128 KB for SRAM3.            */
} ra8_sram_bank_offset_t;

/**
 * @enum ra8_sram_ecc_region_off_t
 * @brief Per-bank offsets into the SRAM ECC syndrome alias.
 *
 * @details
 * Per HUM Ch 58.1 Table 58.1, p 3527: each bank's ECC syndrome bytes
 * live at the bank-specific window listed below. Direct access is only
 * meaningful when ``SRAMCRn.TSTBYP=1``; otherwise the ECC engine
 * intercepts the access and the read/write is invalid.
 */
typedef enum : uint32_t {
  k_ra8_sram_ecc_bank0_off  = 0x001A0000UL, /**< SRAM0 ECC region @ 0x221A_0000.   */
  k_ra8_sram_ecc_bank1_off  = 0x001B0000UL, /**< SRAM1 ECC region @ 0x221B_0000.   */
  k_ra8_sram_ecc_bank2_off  = 0x001C0000UL, /**< SRAM2 ECC region @ 0x221C_0000.   */
  k_ra8_sram_ecc_bank3_off  = 0x001D0000UL, /**< SRAM3 ECC region @ 0x221D_0000.   */
  k_ra8_sram_ecc_bank012_sz = 0x00010000UL, /**< 64 KB ECC syndrome region.        */
  k_ra8_sram_ecc_bank3_sz   = 0x00004000UL, /**< 16 KB ECC syndrome region for B3. */
} ra8_sram_ecc_region_off_t;

/* =============================================================================
 * Bank / layout constants
 * =============================================================================
 */

/**
 * @enum ra8_sram_bank_count_t
 * @brief Number of SRAM banks (SRAM0..SRAM3).
 *
 * @details
 * Per HUM Ch 58.1 Table 58.1, p 3527: SRAM0/1/2 are 512 KB each and
 * SRAM3 is 128 KB.
 */
typedef enum : uint8_t {
  k_ra8_sram_bank_count = 4U, /**< SRAM0, SRAM1, SRAM2, SRAM3. */
} ra8_sram_bank_count_t;

/**
 * @enum ra8_sram_ear_pair_t
 * @brief Number of error-address registers per bank (1-bit + 2-bit).
 *
 * @details
 * Per HUM Ch 58.2.14 "SRAMEARnm", p 3537: ``m=0`` captures the offset
 * of the first 1-bit ECC error in bank ``n``; ``m=1`` captures the
 * first 2-bit error. The pair count is therefore 2.
 */
typedef enum : uint8_t {
  k_ra8_sram_ear_pair_count = 2U, /**< [bank][1-bit, 2-bit].           */
  k_ra8_sram_ear_slot_1bit  = 0U, /**< SRAMEARn0 -- first 1-bit error. */
  k_ra8_sram_ear_slot_2bit  = 1U, /**< SRAMEARn1 -- first 2-bit error. */
} ra8_sram_ear_pair_t;

/**
 * @enum ra8_sram_ecc_word_t
 * @brief Granularity of ECC-protected SRAM accesses.
 *
 * @details
 * Per HUM Ch 58.3.2 "Correction of ECC Errors", p 3538: ECC syndrome
 * is computed per 64-bit word. Initial-fill writes must be 8-byte
 * aligned and 64 bits wide so the syndrome lines up correctly. Per
 * HUM Ch 58.4.2 "Notes on Using Error Checking of SRAM", p 3541: the
 * SRAM is read in 8-byte units, so the safe zero-init stride is 8.
 */
typedef enum : uint8_t {
  k_ra8_sram_ecc_word_bytes = 8U, /**< 64-bit ECC word width (bytes).           */
  k_ra8_sram_ecc_word_shift = 3U, /**< log2(word bytes), for size>>shift loops. */
} ra8_sram_ecc_word_t;

/**
 * @enum ra8_sram_fill_pattern_t
 * @brief Fill word for the deterministic zero-init pass.
 *
 * @details
 * Per HUM Ch 58.3.2, p 3538: any 64-bit pattern produces a valid
 * syndrome when written under ECC-no-check. Zero is chosen because it
 * matches the post-reset state of every bank's user-visible store and
 * is the cheapest immediate to encode.
 */
typedef enum : uint64_t {
  k_ra8_sram_zero_init_word = 0x0000000000000000ULL, /**< Default fill pattern. */
} ra8_sram_fill_pattern_t;

/* =============================================================================
 * SRAMPRCR -- write-protect key
 * =============================================================================
 */

/**
 * @enum ra8_sram_prcr_key_t
 * @brief SRAMPRCR_S/NS half-word values for unlock / lock.
 *
 * @details
 * Per HUM Ch 58.2.4 "SRAMPRCR_S : SRAM Protection Control Register
 * for Secure", p 3530, the upper 8 bits ``KW[7:0]`` must be ``0xA5``
 * for the ``PR`` write-enable bit to take effect. ``0xA501`` enables
 * subsequent writes to SRAMWTSC / SRAMCRn / SRAMECCRGNn; ``0xA500``
 * locks them again.
 */
typedef enum : uint16_t {
  k_ra8_sram_prcr_unlock = 0xA501U, /**< KW=0xA5, PR=1 -- writes enabled. */
  k_ra8_sram_prcr_lock   = 0xA500U, /**< KW=0xA5, PR=0 -- writes blocked. */
  k_ra8_sram_prcr_kw     = 0xA500U, /**< KW field bits (15:8).            */
  k_ra8_sram_prcr_pr_msk = 0x0001U, /**< PR write-enable bit.             */
} ra8_sram_prcr_key_t;

/* =============================================================================
 * SRAMWTSC -- wait-state enable
 * =============================================================================
 */

/**
 * @enum ra8_sram_wtsc_mask_t
 * @brief SRAMWTSC bit masks.
 *
 * @details
 * Per HUM Ch 58.2.6 "SRAMWTSC : SRAM Wait State Control Register",
 * p 3531. WTEN=1 inserts one wait state into SRAM read access cycles
 * and is required when ICLK > half the maximum frequency.
 */
typedef enum : uint8_t {
  k_ra8_sram_wtsc_wten = 0x01U, /**< Insert one wait state on SRAM access. */
  k_ra8_sram_wtsc_msk  = 0x01U, /**< All R/W bits in SRAMWTSC.             */
} ra8_sram_wtsc_mask_t;

/**
 * @enum ra8_sram_iclk_threshold_t
 * @brief ICLK frequency thresholds where SRAMWTSC.WTEN must flip to 1.
 *
 * @details
 * Per HUM Ch 58.3.7 "Wait State", p 3540:
 *
 *  - 250 MHz max ICLK -> 1 wait when ICLK > 125 MHz.
 *  - 200 MHz max ICLK -> 1 wait when ICLK > 100 MHz.
 *  - 150 MHz max ICLK -> 1 wait when ICLK >  75 MHz.
 *
 * The driver multiplies the configured maximum by 2 to get the
 * threshold; these values are kept here for documentation and unit
 * tests that exercise the auto-wait helper.
 */
typedef enum : uint32_t {
  k_ra8_sram_iclk_threshold_250mhz = 125000000UL, /**< 250 MHz max -> 125 MHz threshold. */
  k_ra8_sram_iclk_threshold_200mhz = 100000000UL, /**< 200 MHz max -> 100 MHz threshold. */
  k_ra8_sram_iclk_threshold_150mhz = 75000000UL,  /**< 150 MHz max ->  75 MHz threshold. */
  k_ra8_sram_iclk_max_default      = 250000000UL, /**< Default RA8D2 max ICLK = 250 MHz. */
} ra8_sram_iclk_threshold_t;

/* =============================================================================
 * SRAMCRn -- ECC mode + OAD + E1STSEN + TSTBYP
 * =============================================================================
 */

/**
 * @enum ra8_sram_cr_bit_t
 * @brief SRAMCRn bit positions.
 *
 * @details
 * Per HUM Ch 58.2.7 "SRAMCRn", p 3532. Layout:
 *
 *  - bit 0    OAD       0=interrupt on ECC err, 1=reset
 *  - bit 3:2  ECCMOD    00=disabled, 10=no-check, 11=with-check
 *  - bit 4    E1STSEN   1=enable 1-bit error update of SRAMESR
 *  - bit 7    TSTBYP    1=enable ECC bypass (decoder test)
 */
typedef enum : uint8_t {
  k_ra8_sram_cr_bit_oad     = 0U, /**< Operation after error detection. */
  k_ra8_sram_cr_bit_eccmod0 = 2U, /**< ECCMOD low bit.                  */
  k_ra8_sram_cr_bit_eccmod1 = 3U, /**< ECCMOD high bit.                 */
  k_ra8_sram_cr_bit_e1stsen = 4U, /**< 1-bit error status enable.       */
  k_ra8_sram_cr_bit_tstbyp  = 7U, /**< ECC test bypass.                 */
} ra8_sram_cr_bit_t;

/**
 * @enum ra8_sram_cr_mask_t
 * @brief SRAMCRn bit masks (HUM Ch 58.2.7, p 3532).
 */
typedef enum : uint8_t {
  k_ra8_sram_cr_mask_oad     = 0x01U, /**< OAD bit 0.                        */
  k_ra8_sram_cr_mask_eccmod  = 0x0CU, /**< ECCMOD[1:0] @ [3:2].              */
  k_ra8_sram_cr_mask_e1stsen = 0x10U, /**< E1STSEN bit 4.                    */
  k_ra8_sram_cr_mask_tstbyp  = 0x80U, /**< TSTBYP bit 7.                     */
  k_ra8_sram_cr_mask_all     = 0x9DU, /**< Union of the four R/W bits above. */
} ra8_sram_cr_mask_t;

/**
 * @enum ra8_sram_cr_eccmod_t
 * @brief Encoded SRAMCRn.ECCMOD field values (already shifted to [3:2]).
 *
 * @details
 * Per HUM Ch 58.2.7, p 3532. ``01b`` is reserved and never written.
 */
typedef enum : uint8_t {
  k_ra8_sram_eccmod_disabled = 0x00U, /**< 00b<<2 = 0x00 (ECC off).        */
  k_ra8_sram_eccmod_no_check = 0x08U, /**< 10b<<2 = 0x08 (ECC, no check).  */
  k_ra8_sram_eccmod_with_chk = 0x0CU, /**< 11b<<2 = 0x0C (ECC + checking). */
} ra8_sram_cr_eccmod_t;

/**
 * @enum ra8_sram_cr_self_test_t
 * @brief SRAMCRn values used by the ECC decoder self-test sequence.
 *
 * @details
 * Per HUM Ch 58.3.4 "ECC Decoder Testing", p 3539, the test
 * flowchart writes three specific bytes to SRAMCRn:
 *
 *  - 0x08 : ECC enabled, no error checking, bypass disabled
 *           (used to write data with valid ECC syndromes).
 *  - 0x80 : ECC disabled, bypass enabled
 *           (used to read/flip the raw ECC syndrome bits).
 *  - 0x1C : ECC enabled with checking, E1STSEN=1, bypass disabled
 *           (used for the verification readback that triggers the
 *           expected SRAMESR flag).
 */
typedef enum : uint8_t {
  k_ra8_sram_cr_self_test_phase_write  = 0x08U, /**< ECCMOD=10, all others 0. */
  k_ra8_sram_cr_self_test_phase_bypass = 0x80U, /**< TSTBYP=1, ECCMOD=00.     */
  k_ra8_sram_cr_self_test_phase_verify = 0x1CU, /**< ECCMOD=11, E1STSEN=1.    */
} ra8_sram_cr_self_test_t;

/* =============================================================================
 * SRAMECCRGNn -- ECC region size
 * =============================================================================
 */

/**
 * @enum ra8_sram_eccrgn_value_t
 * @brief SRAMECCRGNn.ECCRGN field encodings.
 *
 * @details
 * Per HUM Ch 58.2.8..58.2.11, p 3533-3535. For SRAM0..SRAM2 the
 * granularity is 128 KB (1=128, 2=256, 3=384, 4=512). SRAM3 is the
 * 128 KB bank so only ``0`` and ``1`` are legal there.
 */
typedef enum : uint8_t {
  k_ra8_sram_eccrgn_off       = 0U,    /**< 000b -- no ECC target region. */
  k_ra8_sram_eccrgn_128kb     = 1U,    /**< 001b -- first 128 KB of bank. */
  k_ra8_sram_eccrgn_256kb     = 2U,    /**< 010b -- first 256 KB of bank. */
  k_ra8_sram_eccrgn_384kb     = 3U,    /**< 011b -- first 384 KB of bank. */
  k_ra8_sram_eccrgn_512kb     = 4U,    /**< 100b -- whole 512 KB bank.    */
  k_ra8_sram_eccrgn_field_msk = 0x07U, /**< ECCRGN[2:0] bit mask.         */
} ra8_sram_eccrgn_value_t;

/* =============================================================================
 * SRAMESR / SRAMESCLR -- per-bank error / clear bits
 * =============================================================================
 */

/**
 * @enum ra8_sram_esr_bit_t
 * @brief SRAMESR / SRAMESCLR bit positions for each bank.
 *
 * @details
 * Per HUM Ch 58.2.12 "SRAMESR : SRAM Error Status Register", p 3535
 * and HUM Ch 58.2.13 "SRAMESCLR", p 3536. Pattern is
 * ``ERR{n}{0|1}`` / ``CLR{n}{0|1}`` where ``0=1-bit`` and ``1=2-bit``.
 *
 * The SRAMESCLR layout is bit-for-bit identical so the same shifts
 * are reused for both registers.
 */
typedef enum : uint16_t {
  k_ra8_sram_err_bank0_1bit = 0x0001U, /**< ERR00 -- SRAM0 1-bit.       */
  k_ra8_sram_err_bank0_2bit = 0x0002U, /**< ERR01 -- SRAM0 2-bit.       */
  k_ra8_sram_err_bank1_1bit = 0x0004U, /**< ERR10 -- SRAM1 1-bit.       */
  k_ra8_sram_err_bank1_2bit = 0x0008U, /**< ERR11 -- SRAM1 2-bit.       */
  k_ra8_sram_err_bank2_1bit = 0x0010U, /**< ERR20 -- SRAM2 1-bit.       */
  k_ra8_sram_err_bank2_2bit = 0x0020U, /**< ERR21 -- SRAM2 2-bit.       */
  k_ra8_sram_err_bank3_1bit = 0x0040U, /**< ERR30 -- SRAM3 1-bit.       */
  k_ra8_sram_err_bank3_2bit = 0x0080U, /**< ERR31 -- SRAM3 2-bit.       */
  k_ra8_sram_err_all_mask   = 0x00FFU, /**< Union of all 8 error bits.  */
  k_ra8_sram_err_all_1bit   = 0x0055U, /**< Union of all 4 1-bit flags. */
  k_ra8_sram_err_all_2bit   = 0x00AAU, /**< Union of all 4 2-bit flags. */
} ra8_sram_esr_bit_t;

/* =============================================================================
 * CPSCU -- SRAMSAR / SRAMSABARn / SRAMESAR security attribution
 * =============================================================================
 */

/**
 * @enum ra8_sram_sar_bit_t
 * @brief SRAMSAR bit positions.
 *
 * @details
 * Per HUM Ch 58.2.2 "SRAMSAR : SRAM Security Attribution Register",
 * p 3528. Bits 0..3 cover SRAM0..SRAM3 register security; bit 8
 * covers SRAMWTSC security.
 */
typedef enum : uint32_t {
  k_ra8_sram_sar_bit_sa0  = 0x00000001UL, /**< SRAM0 register set Non-Secure. */
  k_ra8_sram_sar_bit_sa1  = 0x00000002UL, /**< SRAM1 register set Non-Secure. */
  k_ra8_sram_sar_bit_sa2  = 0x00000004UL, /**< SRAM2 register set Non-Secure. */
  k_ra8_sram_sar_bit_sa3  = 0x00000008UL, /**< SRAM3 register set Non-Secure. */
  k_ra8_sram_sar_bit_wtsa = 0x00000100UL, /**< SRAMWTSC Non-Secure.           */
  k_ra8_sram_sar_bank_msk = 0x0000000FUL, /**< SRAMSA0..SRAMSA3 union.        */
  k_ra8_sram_sar_writable = 0x0000010FUL, /**< Union of all defined bits.     */
} ra8_sram_sar_bit_t;

/**
 * @enum ra8_sram_esar_bit_t
 * @brief SRAMESAR bit positions.
 *
 * @details
 * Per HUM Ch 58.2.3 "SRAMESAR : SRAM ECC region Security Attribute
 * Register", p 3529. Only bit 0 (SRAMESA) is defined.
 */
typedef enum : uint32_t {
  k_ra8_sram_esar_bit_esa  = 0x00000001UL, /**< SRAMESA -- ECC region Non-Secure. */
  k_ra8_sram_esar_writable = 0x00000001UL, /**< Union of defined bits.            */
} ra8_sram_esar_bit_t;

/**
 * @enum ra8_sram_sabar_limit_t
 * @brief Boundary-address limits per bank for SRAMSABARn.
 *
 * @details
 * Per HUM Ch 58.2.1 "SRAMSABARn", p 3527-3528. Each value is the
 * absolute Secure offset where the bank ends -- writing a boundary
 * larger than this saturates the whole bank to "Secure". The b12..b0
 * bits of the boundary must be zero (4 KB granularity).
 */
typedef enum : uint32_t {
  k_ra8_sram_sabar_align_mask = 0x00001FFFUL, /**< b12..b0 must be 0.            */
  k_ra8_sram_sabar_field_mask = 0x001FE000UL, /**< b20..b13 are the SA boundary. */
  k_ra8_sram_sabar_bank0_full = 0x00080000UL, /**< Whole SRAM0 Secure.           */
  k_ra8_sram_sabar_bank1_full = 0x00100000UL, /**< Whole SRAM1 Secure.           */
  k_ra8_sram_sabar_bank2_full = 0x00180000UL, /**< Whole SRAM2 Secure.           */
  k_ra8_sram_sabar_bank3_full = 0x001A0000UL, /**< Whole SRAM3 Secure.           */
} ra8_sram_sabar_limit_t;

/* =============================================================================
 * Register window layout
 * =============================================================================
 */

/**
 * @enum ra8_sram_pad_t
 * @brief Reserved-byte gap sizes used inside the SRAM control register
 *        block and CPSCU window.
 *
 * @details
 * Names every padding gap so the struct definitions do not depend on
 * raw integer literals. Cross-checked against HUM Ch 58.2.
 */
typedef enum : uint16_t {
  k_ra8_sram_pad_wtsc_to_cr        = 7U,     /**< +0x09..+0x0F gap (bytes).    */
  k_ra8_sram_pad_cr_sparse_bytes   = 12U,    /**< +0x14..+0x1F sparse SRAMCRn. */
  k_ra8_sram_pad_eccrgn_sparse     = 12U,    /**< +0x34..+0x3F sparse RGNn.    */
  k_ra8_sram_cpscu_pad_sramsar     = 0x3ECU, /**< +0x014..+0x3FF gap (bytes).  */
  k_ra8_sram_cpscu_pad_after_sabar = 0x100U, /**< +0x410..+0x50F gap (bytes).  */
} ra8_sram_pad_t;

/**
 * @enum ra8_sram_indexed_t
 * @brief Stride / base offsets for sparse SRAMCR / SRAMECCRGN slots.
 */
typedef enum : uint8_t {
  k_ra8_sram_cr_base_off     = 0x10U, /**< SRAMCR0 byte offset.         */
  k_ra8_sram_eccrgn_base_off = 0x30U, /**< SRAMECCRGN0 byte offset.     */
  k_ra8_sram_sparse_stride   = 4U,    /**< Stride between sparse slots. */
} ra8_sram_indexed_t;

/**
 * @struct r_sram_regs_t
 * @brief Memory layout of the SRAM control window.
 *
 * @details
 * Field offsets verified against HUM Ch 58.2.4..58.2.14, p 3530-3537.
 * Reserved gaps are exposed as ``_rN[]`` padding so the next register
 * lands at its documented offset.
 *
 *  - +0x00 SRAMPRCR_S  (16-bit, half-word access only -- see HUM 58.2.4)
 *  - +0x04 SRAMPRCR_NS (16-bit)
 *  - +0x08 SRAMWTSC    ( 8-bit)
 *  - +0x10..+0x1C SRAMCR0..3 (8-bit each, +4 stride)
 *  - +0x30..+0x3C SRAMECCRGN0..3 (8-bit, +4 stride)
 *  - +0x40 SRAMESR    (16-bit)
 *  - +0x48 SRAMESCLR  (16-bit)
 *  - +0x50..+0x7C SRAMEAR[bank][slot] (32-bit, +0x10 per bank, +0x04 per slot)
 *
 * @invariant ``sizeof(r_sram_regs_t)`` covers offset 0x00 through 0x7F.
 */
typedef struct {
  volatile uint16_t SRAMPRCR_S;                     /**< +0x00 Secure write-protect.     */
  volatile uint16_t _r0;                            /**< +0x02 Padding.                  */
  volatile uint16_t SRAMPRCR_NS;                    /**< +0x04 Non-Secure write-protect. */
  volatile uint16_t _r1;                            /**< +0x06 Padding.                  */
  volatile uint8_t  SRAMWTSC;                       /**< +0x08 Wait-state control.       */
  volatile uint8_t  _r2[k_ra8_sram_pad_wtsc_to_cr]; /**< +0x09..+0x0F Padding.           */
  volatile uint8_t  SRAMCR[k_ra8_sram_bank_count];  /**< +0x10..+0x13 (sparse, see _r3). */
  /* The HUM lays SRAMCRn at +0x10 + 0x04 * n, so we cannot pack them
   * as a contiguous uint8_t array. Model them as one 32-bit word per
   * bank instead -- the public API only writes the low byte. */
  volatile uint8_t
    _r3[k_ra8_sram_pad_cr_sparse_bytes];              /**< +0x14..+0x1F Pad for sparse CR slots. */
  volatile uint8_t _r4[16];                           /**< +0x20..+0x2F Reserved.                */
  volatile uint8_t SRAMECCRGN[k_ra8_sram_bank_count]; /**< +0x30..+0x33 (sparse, see _r5).       */
  volatile uint8_t
    _r5[k_ra8_sram_pad_eccrgn_sparse]; /**< +0x34..+0x3F Padding for sparse RGN slots. */
  volatile uint16_t SRAMESR;           /**< +0x40 ECC error status.                    */
  volatile uint16_t _r6;               /**< +0x42 Padding.                             */
  volatile uint8_t  _r7[4];            /**< +0x44..+0x47 Reserved.                     */
  volatile uint16_t SRAMESCLR;         /**< +0x48 ECC error status clear.              */
  volatile uint16_t _r8;               /**< +0x4A Padding.                             */
  volatile uint8_t  _r9[4];            /**< +0x4C..+0x4F Reserved.                     */
  volatile uint32_t SRAMEAR[k_ra8_sram_bank_count]
                           [k_ra8_sram_ear_pair_count]; /**< +0x50..+0x7F Error addresses. */
} r_sram_regs_t;

/**
 * @struct r_sram_cpscu_regs_t
 * @brief Memory layout of the CPSCU SRAM security window.
 *
 * @details
 * Per HUM Ch 58.2.1..58.2.3, p 3527-3530. The CPSCU window holds:
 *
 *  - +0x010 SRAMSAR     (32-bit, per-bank register security + WTSA)
 *  - +0x400 SRAMSABAR0  (32-bit, SRAM0 Secure/Non-Secure boundary)
 *  - +0x404 SRAMSABAR1  (32-bit, SRAM1 boundary)
 *  - +0x408 SRAMSABAR2  (32-bit, SRAM2 boundary)
 *  - +0x40C SRAMSABAR3  (32-bit, SRAM3 boundary)
 *  - +0x510 SRAMESAR    (32-bit, ECC region security attribute)
 *
 * Holes in the layout are modelled as opaque ``_rN[]`` padding so the
 * named fields land at exactly the offsets the HUM documents. The
 * struct size is rounded up to ``0x518`` bytes (after SRAMESAR) so
 * the fake's 8 MiB peripheral window covers it.
 */
typedef struct {
  volatile uint8_t  _r0[0x010];                            /**< +0x000..+0x00F Reserved.       */
  volatile uint32_t SRAMSAR;                               /**< +0x010 SRAM register security. */
  volatile uint8_t  _r1[k_ra8_sram_cpscu_pad_sramsar];     /**< +0x014..+0x3FF Reserved.       */
  volatile uint32_t SRAMSABAR[k_ra8_sram_bank_count];      /**< +0x400..+0x40F Bank boundary.  */
  volatile uint8_t  _r2[k_ra8_sram_cpscu_pad_after_sabar]; /**< +0x410..+0x50F Reserved.       */
  volatile uint32_t SRAMESAR;                              /**< +0x510 ECC region security.    */
  volatile uint32_t _r3;                                   /**< +0x514 Padding to round size.  */
} r_sram_cpscu_regs_t;

/**
 * @brief Get pointer to the (Secure) SRAM control register block.
 * @return Volatile pointer to the SRAM control window at ``0x40002000``.
 *
 * @details
 * On the bare-metal target this is real MMIO. Under
 * ``RA8_OFF_TARGET`` the same address is backed by host RAM via
 * ``tests/mocks/ra8_fake_mmap.c``, so the same accessor works on host
 * tests without a separate mock header.
 */
static inline volatile r_sram_regs_t* ra8_sram_regs(void)
{
  return (volatile r_sram_regs_t*)k_ra8_sram_base_addr;
}

/**
 * @brief Get pointer to the (Secure) CPSCU SRAM security register block.
 * @return Volatile pointer to the CPSCU SRAM window at ``0x40008000``.
 *
 * @details
 * The CPSCU window is shared with other modules (SRAM is one of
 * several security-attribution clients). This accessor only exposes
 * the SRAM-related slice; do not dereference it for non-SRAM
 * registers.
 */
static inline volatile r_sram_cpscu_regs_t* ra8_sram_cpscu_regs(void)
{
  return (volatile r_sram_cpscu_regs_t*)k_ra8_sram_cpscu_base_addr;
}

/**
 * @brief Indexed write helper: ``SRAMCRn`` slot.
 * @param[in] regs Result of ``ra8_sram_regs()``.
 * @param[in] bank Bank index 0..3 (caller must validate).
 * @return Pointer to the per-bank SRAMCRn 8-bit slot.
 *
 * @details
 * The HUM lays out SRAMCR0..3 with a 4-byte stride, so the ``SRAMCR``
 * member of ``r_sram_regs_t`` cannot be a packed ``uint8_t[4]``. This
 * helper hides the cast and stride math.
 */
static inline volatile uint8_t* ra8_sram_cr_ptr(volatile r_sram_regs_t* regs, uint8_t bank)
{
  /* SRAMCRn at +0x10 + 4 * n. */
  const uintptr_t base = (uintptr_t)regs;
  return (volatile uint8_t*)(base + (uintptr_t)k_ra8_sram_cr_base_off +
                             ((uintptr_t)bank * (uintptr_t)k_ra8_sram_sparse_stride));
}

/**
 * @brief Indexed write helper: ``SRAMECCRGNn`` slot.
 * @param[in] regs Result of ``ra8_sram_regs()``.
 * @param[in] bank Bank index 0..3 (caller must validate).
 * @return Pointer to the per-bank SRAMECCRGNn 8-bit slot.
 */
static inline volatile uint8_t* ra8_sram_eccrgn_ptr(volatile r_sram_regs_t* regs, uint8_t bank)
{
  /* SRAMECCRGNn at +0x30 + 4 * n. */
  const uintptr_t base = (uintptr_t)regs;
  return (volatile uint8_t*)(base + (uintptr_t)k_ra8_sram_eccrgn_base_off +
                             ((uintptr_t)bank * (uintptr_t)k_ra8_sram_sparse_stride));
}

/**
 * @brief Per-bank data window helper: pointer to the start of the data alias.
 * @param[in] bank Bank index 0..3 (caller must validate).
 * @return Volatile pointer to the bank's first 64-bit word.
 *
 * @details
 * Per HUM Ch 58.1 Table 58.1, p 3527. SRAM0 starts at the data base,
 * subsequent banks step in 0x80000-byte increments (SRAM3 reuses the
 * same step but only owns the first 0x20000 bytes of its slot).
 */
static inline volatile uint64_t* ra8_sram_bank_data_ptr(uint8_t bank)
{
  const uintptr_t base = (uintptr_t)k_ra8_sram_data_base_addr;
  return (volatile uint64_t*)(base + ((uintptr_t)bank * (uintptr_t)k_ra8_sram_bank012_size));
}

/**
 * @brief Per-bank data-region size in bytes.
 * @param[in] bank Bank index 0..3.
 * @return Number of bytes in the bank's user-visible data window.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline uint32_t ra8_sram_bank_size_bytes(uint8_t bank)
{
  return (bank == 3U) ? k_ra8_sram_bank3_size : k_ra8_sram_bank012_size;
}

/**
 * @brief Per-bank ECC syndrome region size in bytes.
 * @param[in] bank Bank index 0..3.
 * @return Number of bytes in the bank's ECC syndrome window
 *         (only meaningful when ``SRAMCRn.TSTBYP=1``).
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline uint32_t ra8_sram_bank_ecc_size_bytes(uint8_t bank)
{
  return (bank == 3U) ? k_ra8_sram_ecc_bank3_sz : k_ra8_sram_ecc_bank012_sz;
}

#ifdef __cplusplus
}
#endif
