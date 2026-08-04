/**
 * @file ra8_bkup_regs.h
 * @brief Battery Backup Function (VBATT / BAT*) register layout for the RA8D2
 * @ingroup grp_hal_system
 *
 * @details
 * The RA8D2 battery backup block lives inside the SYSC peripheral
 * window (base ``0x4001_E000``, non-secure alias ``0x5001_E000``).
 * The full SYSC layout is described in ``ra8_system_regs.h``; this
 * header narrowly mirrors only the BAT* / VBT* register offsets so
 * the battery-backup driver can be reviewed and extended without
 * touching the shared system header.
 *
 * ## Map (HUM Ch 12 "Battery Backup Function" p 498-519)
 *
 * | Offset | Reg          | Width | Purpose                                        |
 * |-------:|--------------|------:|------------------------------------------------|
 * | 0x3B0  | VBRSABAR     | 16    | Backup-register secure / non-secure boundary   |
 * | 0x3B4  | VBRPABARS    | 16    | Backup-register privilege boundary (Secure)    |
 * | 0x3B8  | VBRPABARNS   | 16    | Backup-register privilege boundary (Non-sec)   |
 * | 0x3D0  | BBFSAR       | 32    | Battery Backup security attribute              |
 * | 0xA84  | VBATTMNSELR  | 8     | VBATT voltage-monitor function select          |
 * | 0xA88  | VBTBPCR1     | 8     | Battery power-supply switch stop               |
 * | 0xC40  | VBTBER       | 8     | VBATT backup-register access enable (VBAE)     |
 * | 0xC45  | VBTBPCR2     | 8     | VBATT VDETBATT level + VDETE                   |
 * | 0xC46  | VBTBPSR      | 8     | VBATT power-supply status (VBPORF/VBPORM/SWM)  |
 * | 0xC48  | VBTADSR      | 8     | VBATT tamper-detection status (VBTADF[2:0])    |
 * | 0xC49  | VBTADCR1     | 8     | Tamper IRQ enable + backup-clear enable        |
 * | 0xC4A  | VBTADCR2     | 8     | RTC time-capture event source select           |
 * | 0xC4C  | VBTICTLR     | 8     | RTCICn input enable                            |
 * | 0xC4D  | VBTICTLR2    | 8     | RTCICn edge / noise-canceller select           |
 * | 0xC4E  | VBTIMONR     | 8     | RTCICn live input level monitor                |
 * | 0xC50  | VBTNCWCR     | 8     | Noise canceller width select                   |
 * | 0xC54  | VBTADCR3     | 8     | Tamper HUK Zeroization enable                  |
 * | 0xD00  | VBTBKRn      | 8     | 128 backup-register bytes (n = 0..127)         |
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
 * @enum ra8_bkup_addr_t
 * @brief Battery-backup register window base.
 *
 * @details
 * Same physical base as ``k_ra8_system_base_addr`` -- the BAT*
 * registers live inside the SYSC block. We expose a separate enum
 * value so the bkup driver does not have to depend on the system
 * header.
 */
typedef enum : uintptr_t {
  k_ra8_bkup_base_addr = 0x4001E000UL, /**< SYSC + battery backup base. */
} ra8_bkup_addr_t;

/**
 * @enum ra8_bkup_off_t
 * @brief Byte-offsets of every BAT* / VBT* register the driver touches.
 *
 * @details
 * Citations are HUM Ch 12.2.x section numbers, all inside p 498..519.
 */
typedef enum : uint16_t {
  k_ra8_bkup_off_vbrsabar    = 0x3B0U, /**< VBRSABAR Ch 12.2.2 p 502.     */
  k_ra8_bkup_off_vbrpabars   = 0x3B4U, /**< VBRPABARS Ch 12.2.3 p 502.    */
  k_ra8_bkup_off_vbrpabarns  = 0x3B8U, /**< VBRPABARNS Ch 12.2.4 p 503.   */
  k_ra8_bkup_off_bbfsar      = 0x3D0U, /**< BBFSAR  Ch 12.2.1  p 500.     */
  k_ra8_bkup_off_vbattmnselr = 0xA84U, /**< VBATTMNSELR Ch 12.2.5 p 503.  */
  k_ra8_bkup_off_vbtbpcr1    = 0xA88U, /**< VBTBPCR1 Ch 12.2.11 p 507.    */
  k_ra8_bkup_off_vbtber      = 0xC40U, /**< VBTBER   Ch 12.2.6 p 504.     */
  k_ra8_bkup_off_vbtbpcr2    = 0xC45U, /**< VBTBPCR2 Ch 12.2.12 p 508.    */
  k_ra8_bkup_off_vbtbpsr     = 0xC46U, /**< VBTBPSR  Ch 12.2.13 p 509.    */
  k_ra8_bkup_off_vbtadsr     = 0xC48U, /**< VBTADSR  Ch 12.2.14 p 509.    */
  k_ra8_bkup_off_vbtadcr1    = 0xC49U, /**< VBTADCR1 Ch 12.2.15 p 510.    */
  k_ra8_bkup_off_vbtadcr2    = 0xC4AU, /**< VBTADCR2 Ch 12.2.16 p 511.    */
  k_ra8_bkup_off_vbtictlr    = 0xC4CU, /**< VBTICTLR Ch 12.2.8 p 505.     */
  k_ra8_bkup_off_vbtictlr2   = 0xC4DU, /**< VBTICTLR2 Ch 12.2.9 p 506.    */
  k_ra8_bkup_off_vbtimonr    = 0xC4EU, /**< VBTIMONR Ch 12.2.10 p 506.    */
  k_ra8_bkup_off_vbtncwcr    = 0xC50U, /**< VBTNCWCR Ch 12.2.18 p 511.    */
  k_ra8_bkup_off_vbtadcr3    = 0xC54U, /**< VBTADCR3 Ch 12.2.17 p 511.    */
  k_ra8_bkup_off_vbtbkr0     = 0xD00U, /**< VBTBKR0..127 Ch 12.2.7 p 505. */
} ra8_bkup_off_t;

/**
 * @enum ra8_bkup_limits_t
 * @brief Backup-register array dimensions and small numeric limits.
 */
typedef enum : uint16_t {
  k_ra8_bkup_reg_count       = 128U,    /**< 128 byte-wide VBTBKRn slots.                  */
  k_ra8_bkup_reg_stride      = 1U,      /**< 1 byte per VBTBKRn entry.                     */
  k_ra8_bkup_word_count      = 32U,     /**< 128 bytes / 4 = 32 uint32_t words.            */
  k_ra8_bkup_word_stride     = 4U,      /**< Bytes per uint32_t word.                      */
  k_ra8_bkup_chan_count      = 3U,      /**< RTCIC0..RTCIC2 tamper channels.               */
  k_ra8_bkup_saba_align      = 32U,     /**< SABA boundary granularity (HUM Ch 12.2.2).    */
  k_ra8_bkup_saba_align_mask = 0x1FU,   /**< Lower-5-bits-must-be-zero mask.               */
  k_ra8_bkup_saba_max        = 0xFFE0U, /**< Maximum boundary address (last 32-byte slot). */
} ra8_bkup_limits_t;

/**
 * @enum ra8_bkup_vbtber_bit_t
 * @brief Bit positions inside VBTBER (HUM Ch 12.2.6 p 504).
 */
typedef enum : uint8_t {
  k_ra8_bkup_vbtber_bit_vbae = 3U, /**< VBAE: VBTBKRn access enable. */
} ra8_bkup_vbtber_bit_t;

/**
 * @enum ra8_bkup_vbtber_mask_t
 * @brief Single-bit masks for VBTBER fields.
 */
typedef enum : uint8_t {
  k_ra8_bkup_vbtber_mask_vbae = 0x08U, /**< VBAE @ bit 3. */
} ra8_bkup_vbtber_mask_t;

/**
 * @enum ra8_bkup_vbtbpcr1_bit_t
 * @brief Bit positions inside VBTBPCR1 (HUM Ch 12.2.11 p 507).
 */
typedef enum : uint8_t {
  k_ra8_bkup_vbtbpcr1_bit_bpwswstp = 0U, /**< Battery power-supply switch stop. */
} ra8_bkup_vbtbpcr1_bit_t;

/** @brief Single-bit masks for VBTBPCR1. */
typedef enum : uint8_t {
  k_ra8_bkup_vbtbpcr1_mask_bpwswstp = 0x01U, /**< BPWSWSTP @ bit 0. */
} ra8_bkup_vbtbpcr1_mask_t;

/**
 * @enum ra8_bkup_vbtbpcr2_bit_t
 * @brief Bit positions inside VBTBPCR2 (HUM Ch 12.2.12 p 508).
 */
typedef enum : uint8_t {
  k_ra8_bkup_vbtbpcr2_bit_vdete = 4U, /**< VDETE: VCC drop-detection enable. */
  k_ra8_bkup_vbtbpcr2_shift_lvl = 0U, /**< VDETLVL[2:0] @ [2:0].             */
} ra8_bkup_vbtbpcr2_bit_t;

/** @brief Field masks for VBTBPCR2. */
typedef enum : uint8_t {
  k_ra8_bkup_vbtbpcr2_mask_vdete = 0x10U, /**< VDETE @ bit 4. */
  k_ra8_bkup_vbtbpcr2_mask_lvl   = 0x07U, /**< VDETLVL[2:0].  */
} ra8_bkup_vbtbpcr2_mask_t;

/**
 * @enum ra8_bkup_vbtbpsr_bit_t
 * @brief Bit positions inside VBTBPSR (HUM Ch 12.2.13 p 509).
 */
typedef enum : uint8_t {
  k_ra8_bkup_vbtbpsr_bit_vbporf = 0U, /**< VBATT_POR flag (W0C).   */
  k_ra8_bkup_vbtbpsr_bit_vbporm = 4U, /**< VBATT_R level monitor.  */
  k_ra8_bkup_vbtbpsr_bit_swm    = 5U, /**< Battery switch monitor. */
} ra8_bkup_vbtbpsr_bit_t;

/** @brief Single-bit masks for VBTBPSR. */
typedef enum : uint8_t {
  k_ra8_bkup_vbtbpsr_mask_vbporf = 0x01U, /**< VBATT_POR flag. */
  k_ra8_bkup_vbtbpsr_mask_vbporm = 0x10U, /**< VBATT_R OK.     */
  k_ra8_bkup_vbtbpsr_mask_swm    = 0x20U, /**< Switch monitor. */
} ra8_bkup_vbtbpsr_mask_t;

/** @brief Single-bit masks for VBATTMNSELR (HUM Ch 12.2.5 p 503). */
typedef enum : uint8_t {
  k_ra8_bkup_vbattmnselr_mask_vbtmnsel = 0x01U, /**< VBTMNSEL @ bit 0. */
} ra8_bkup_vbattmnselr_mask_t;

/** @brief Single-bit masks for VBTADSR tamper-detection flags (HUM Ch 12.2.14 p 510). */
typedef enum : uint8_t {
  k_ra8_bkup_vbtadsr_mask_vbtadf0 = 0x01U, /**< VBTADF0: RTCIC0 event. */
  k_ra8_bkup_vbtadsr_mask_vbtadf1 = 0x02U, /**< VBTADF1: RTCIC1 event. */
  k_ra8_bkup_vbtadsr_mask_vbtadf2 = 0x04U, /**< VBTADF2: RTCIC2 event. */
  k_ra8_bkup_vbtadsr_mask_all     = 0x07U, /**< Combined VBTADF[2:0].  */
} ra8_bkup_vbtadsr_mask_t;

/** @brief Bit masks for VBTADCR1 (HUM Ch 12.2.15 p 510). */
typedef enum : uint8_t {
  k_ra8_bkup_vbtadcr1_mask_vbtadie0 = 0x01U, /**< VBTADIE0 @ bit 0. */
  k_ra8_bkup_vbtadcr1_mask_vbtadie1 = 0x02U, /**< VBTADIE1 @ bit 1. */
  k_ra8_bkup_vbtadcr1_mask_vbtadie2 = 0x04U, /**< VBTADIE2 @ bit 2. */
  k_ra8_bkup_vbtadcr1_mask_vbtadce0 = 0x10U, /**< VBTADCE0 @ bit 4. */
  k_ra8_bkup_vbtadcr1_mask_vbtadce1 = 0x20U, /**< VBTADCE1 @ bit 5. */
  k_ra8_bkup_vbtadcr1_mask_vbtadce2 = 0x40U, /**< VBTADCE2 @ bit 6. */
  k_ra8_bkup_vbtadcr1_mask_ie_all   = 0x07U, /**< All IE bits.      */
  k_ra8_bkup_vbtadcr1_mask_ce_all   = 0x70U, /**< All CE bits.      */
} ra8_bkup_vbtadcr1_mask_t;

/** @brief Bit masks for VBTADCR2 (HUM Ch 12.2.16 p 511). */
typedef enum : uint8_t {
  k_ra8_bkup_vbtadcr2_mask_vbrtces0 = 0x01U, /**< VBRTCES0 @ bit 0.      */
  k_ra8_bkup_vbtadcr2_mask_vbrtces1 = 0x02U, /**< VBRTCES1 @ bit 1.      */
  k_ra8_bkup_vbtadcr2_mask_vbrtces2 = 0x04U, /**< VBRTCES2 @ bit 2.      */
  k_ra8_bkup_vbtadcr2_mask_all      = 0x07U, /**< All event-source bits. */
} ra8_bkup_vbtadcr2_mask_t;

/** @brief Bit masks for VBTADCR3 (HUM Ch 12.2.17 p 511). */
typedef enum : uint8_t {
  k_ra8_bkup_vbtadcr3_mask_vbtadze0 = 0x01U, /**< VBTADZE0 @ bit 0. */
  k_ra8_bkup_vbtadcr3_mask_vbtadze1 = 0x02U, /**< VBTADZE1 @ bit 1. */
  k_ra8_bkup_vbtadcr3_mask_vbtadze2 = 0x04U, /**< VBTADZE2 @ bit 2. */
  k_ra8_bkup_vbtadcr3_mask_all      = 0x07U, /**< All zeroize bits. */
} ra8_bkup_vbtadcr3_mask_t;

/** @brief Bit masks for VBTICTLR (HUM Ch 12.2.8 p 505). */
typedef enum : uint8_t {
  k_ra8_bkup_vbtictlr_mask_vch0inen = 0x01U, /**< VCH0INEN @ bit 0.      */
  k_ra8_bkup_vbtictlr_mask_vch1inen = 0x02U, /**< VCH1INEN @ bit 1.      */
  k_ra8_bkup_vbtictlr_mask_vch2inen = 0x04U, /**< VCH2INEN @ bit 2.      */
  k_ra8_bkup_vbtictlr_mask_all      = 0x07U, /**< All input-enable bits. */
} ra8_bkup_vbtictlr_mask_t;

/** @brief Bit masks for VBTICTLR2 (HUM Ch 12.2.9 p 506). */
typedef enum : uint8_t {
  k_ra8_bkup_vbtictlr2_mask_vch0nce = 0x01U, /**< VCH0NCE @ bit 0. */
  k_ra8_bkup_vbtictlr2_mask_vch1nce = 0x02U, /**< VCH1NCE @ bit 1. */
  k_ra8_bkup_vbtictlr2_mask_vch2nce = 0x04U, /**< VCH2NCE @ bit 2. */
  k_ra8_bkup_vbtictlr2_mask_vch0eg  = 0x10U, /**< VCH0EG @ bit 4.  */
  k_ra8_bkup_vbtictlr2_mask_vch1eg  = 0x20U, /**< VCH1EG @ bit 5.  */
  k_ra8_bkup_vbtictlr2_mask_vch2eg  = 0x40U, /**< VCH2EG @ bit 6.  */
  k_ra8_bkup_vbtictlr2_mask_nce_all = 0x07U, /**< All NCE bits.    */
  k_ra8_bkup_vbtictlr2_mask_eg_all  = 0x70U, /**< All EG bits.     */
} ra8_bkup_vbtictlr2_mask_t;

/** @brief Bit masks for VBTIMONR (HUM Ch 12.2.10 p 506). */
typedef enum : uint8_t {
  k_ra8_bkup_vbtimonr_mask_vch0mon = 0x01U, /**< VCH0MON @ bit 0. */
  k_ra8_bkup_vbtimonr_mask_vch1mon = 0x02U, /**< VCH1MON @ bit 1. */
  k_ra8_bkup_vbtimonr_mask_vch2mon = 0x04U, /**< VCH2MON @ bit 2. */
  k_ra8_bkup_vbtimonr_mask_all     = 0x07U, /**< All MON bits.    */
} ra8_bkup_vbtimonr_mask_t;

/** @brief Bit masks for VBTNCWCR (HUM Ch 12.2.18 p 511). */
typedef enum : uint8_t {
  k_ra8_bkup_vbtncwcr_mask_vincw = 0x07U, /**< VINCW[2:0] @ [2:0]. */
} ra8_bkup_vbtncwcr_mask_t;

/** @brief BBFSAR target masks (HUM Ch 12.2.1 p 500). */
typedef enum : uint32_t {
  k_ra8_bkup_bbfsar_mask_nonsec0 = 0x01UL, /**< VBATTMNSELR.          */
  k_ra8_bkup_bbfsar_mask_nonsec1 = 0x02UL, /**< VBTBER.               */
  k_ra8_bkup_bbfsar_mask_nonsec2 = 0x04UL, /**< VBTICTLR/2/MONR.      */
  k_ra8_bkup_bbfsar_mask_nonsec3 = 0x08UL, /**< VBTBPCR1/2 + VBTBPSR. */
  k_ra8_bkup_bbfsar_mask_nonsec4 = 0x10UL, /**< VBTADSR/CR1/CR2.      */
  k_ra8_bkup_bbfsar_mask_nonsec5 = 0x20UL, /**< VBTADCR3.             */
  k_ra8_bkup_bbfsar_mask_nonsec6 = 0x40UL, /**< VBTNCWCR.             */
  k_ra8_bkup_bbfsar_mask_all     = 0x7FUL, /**< All NONSEC bits.      */
} ra8_bkup_bbfsar_mask_t;

/* =============================================================================
 * Inline accessor functions (mirrors style of ra8_system_regs.h)
 * =============================================================================
 */

/** @brief Pointer to 8-bit VBATTMNSELR (HUM Ch 12.2.5 p 503). */
static inline volatile uint8_t* ra8_bkup_vbattmnselr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_bkup_base_addr +
                             (uintptr_t)k_ra8_bkup_off_vbattmnselr);
}

/** @brief Pointer to 8-bit VBTBPCR1 (HUM Ch 12.2.11 p 507). */
static inline volatile uint8_t* ra8_bkup_vbtbpcr1(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_bkup_base_addr + (uintptr_t)k_ra8_bkup_off_vbtbpcr1);
}

/** @brief Pointer to 8-bit VBTBER (HUM Ch 12.2.6 p 504). */
static inline volatile uint8_t* ra8_bkup_vbtber(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_bkup_base_addr + (uintptr_t)k_ra8_bkup_off_vbtber);
}

/** @brief Pointer to 8-bit VBTBPCR2 (HUM Ch 12.2.12 p 508). */
static inline volatile uint8_t* ra8_bkup_vbtbpcr2(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_bkup_base_addr + (uintptr_t)k_ra8_bkup_off_vbtbpcr2);
}

/** @brief Pointer to 8-bit VBTBPSR (HUM Ch 12.2.13 p 509). */
static inline volatile uint8_t* ra8_bkup_vbtbpsr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_bkup_base_addr + (uintptr_t)k_ra8_bkup_off_vbtbpsr);
}

/** @brief Pointer to 8-bit VBTADSR (HUM Ch 12.2.14 p 509). */
static inline volatile uint8_t* ra8_bkup_vbtadsr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_bkup_base_addr + (uintptr_t)k_ra8_bkup_off_vbtadsr);
}

/** @brief Pointer to 8-bit VBTADCR1 (HUM Ch 12.2.15 p 510). */
static inline volatile uint8_t* ra8_bkup_vbtadcr1(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_bkup_base_addr + (uintptr_t)k_ra8_bkup_off_vbtadcr1);
}

/** @brief Pointer to 8-bit VBTADCR2 (HUM Ch 12.2.16 p 511). */
static inline volatile uint8_t* ra8_bkup_vbtadcr2(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_bkup_base_addr + (uintptr_t)k_ra8_bkup_off_vbtadcr2);
}

/** @brief Pointer to 8-bit VBTADCR3 (HUM Ch 12.2.17 p 511). */
static inline volatile uint8_t* ra8_bkup_vbtadcr3(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_bkup_base_addr + (uintptr_t)k_ra8_bkup_off_vbtadcr3);
}

/** @brief Pointer to 8-bit VBTICTLR (HUM Ch 12.2.8 p 505). */
static inline volatile uint8_t* ra8_bkup_vbtictlr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_bkup_base_addr + (uintptr_t)k_ra8_bkup_off_vbtictlr);
}

/** @brief Pointer to 8-bit VBTICTLR2 (HUM Ch 12.2.9 p 506). */
static inline volatile uint8_t* ra8_bkup_vbtictlr2(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_bkup_base_addr + (uintptr_t)k_ra8_bkup_off_vbtictlr2);
}

/** @brief Pointer to 8-bit VBTIMONR (HUM Ch 12.2.10 p 506). */
static inline volatile uint8_t* ra8_bkup_vbtimonr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_bkup_base_addr + (uintptr_t)k_ra8_bkup_off_vbtimonr);
}

/** @brief Pointer to 8-bit VBTNCWCR (HUM Ch 12.2.18 p 511). */
static inline volatile uint8_t* ra8_bkup_vbtncwcr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_bkup_base_addr + (uintptr_t)k_ra8_bkup_off_vbtncwcr);
}

/** @brief Pointer to 32-bit BBFSAR (HUM Ch 12.2.1 p 500). */
static inline volatile uint32_t* ra8_bkup_bbfsar(void)
{
  return (volatile uint32_t*)((uintptr_t)k_ra8_bkup_base_addr + (uintptr_t)k_ra8_bkup_off_bbfsar);
}

/** @brief Pointer to 16-bit VBRSABAR (HUM Ch 12.2.2 p 502). */
static inline volatile uint16_t* ra8_bkup_vbrsabar(void)
{
  return (volatile uint16_t*)((uintptr_t)k_ra8_bkup_base_addr + (uintptr_t)k_ra8_bkup_off_vbrsabar);
}

/** @brief Pointer to 16-bit VBRPABARS (HUM Ch 12.2.3 p 502). */
static inline volatile uint16_t* ra8_bkup_vbrpabars(void)
{
  return (volatile uint16_t*)((uintptr_t)k_ra8_bkup_base_addr +
                              (uintptr_t)k_ra8_bkup_off_vbrpabars);
}

/** @brief Pointer to 16-bit VBRPABARNS (HUM Ch 12.2.4 p 503). */
static inline volatile uint16_t* ra8_bkup_vbrpabarns(void)
{
  return (volatile uint16_t*)((uintptr_t)k_ra8_bkup_base_addr +
                              (uintptr_t)k_ra8_bkup_off_vbrpabarns);
}

/**
 * @brief Pointer to one VBTBKRn byte (HUM Ch 12.2.7 p 505).
 *
 * @param[in] index Backup-register index in 0..127. Out-of-range
 *                  indices return ``nullptr`` (the caller is expected
 *                  to validate, but the accessor is defensive).
 * @return Volatile pointer to the byte slot, or ``nullptr`` if
 *         ``index >= k_ra8_bkup_reg_count``.
 */
static inline volatile uint8_t* ra8_bkup_vbtbkr(uint16_t index)
{
  if (index >= k_ra8_bkup_reg_count) {
    return nullptr;
  }
  return (volatile uint8_t*)(k_ra8_bkup_base_addr + (uintptr_t)k_ra8_bkup_off_vbtbkr0 +
                             ((uintptr_t)index * (uintptr_t)k_ra8_bkup_reg_stride));
}

/**
 * @brief Pointer to one VBTBKR 32-bit word slot (4 bytes).
 *
 * @param[in] word_index Word index in 0..31. The 128-byte VBTBKRn
 *                       array is little-endian per HUM Ch 12.2.7.
 * @return Volatile pointer to the word slot, or ``nullptr`` if out of
 *         range.
 */
static inline volatile uint32_t* ra8_bkup_vbtbkr_word(uint8_t word_index)
{
  if ((uint16_t)word_index >= k_ra8_bkup_word_count) {
    return nullptr;
  }
  return (volatile uint32_t*)(k_ra8_bkup_base_addr + (uintptr_t)k_ra8_bkup_off_vbtbkr0 +
                              ((uintptr_t)word_index * (uintptr_t)k_ra8_bkup_word_stride));
}

#ifdef __cplusplus
}
#endif
