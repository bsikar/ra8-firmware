/**
 * @file ra8_lvd_regs.h
 * @brief Programmable Voltage Detection (PVD / LVD) register layout for the RA8D2
 * @ingroup grp_hal_system
 *
 * @details
 * Four programmable voltage-monitor channels live inside the SYSC
 * register window at base 0x4001_E000:
 *
 *   - PVD1, PVD2 -- "m" series, NMI / IRQ + reset capable.
 *   - PVD4, PVD5 -- "n" series, reset-only.
 *
 * Each channel exposes up to five registers:
 *
 * | Register   | Width | Purpose                                         |
 * |------------|------:|-------------------------------------------------|
 * | PVDmCMPCR  | 8     | Comparator-control (PVDE + PVDLVL[4:0])         |
 * | PVDmCR0    | 8     | Detector control (RIE/DFDIS/CMPE/FSAMP/RI/RN)   |
 * | PVDmCR1    | 8     | IRQ control (IDTSEL/IRQSEL) -- m channels only  |
 * | PVDmSR     | 8     | Status (DET/MON)                -- m channels   |
 * | PVDmFCR    | 8     | Function control (RHSEL hysteresis-band select) |
 *
 * Register addresses verified against FSP `R_SYSTEM_Type` in
 * R7KA8D2KF_core0.h: LVD1CMPCR @ 0xA58, LVD1CR0 @ 0xA70,
 * LVD1CR1 @ 0xE0, LVD1SR @ 0xE1, LVD1FCR @ 0xB20, PVDLR @ 0xB34.
 *
 * The on-chip Voltage Monitor Lock Register (PVDLR) gates writes to
 * PVD4 / PVD5 control registers after power-on reset until software
 * writes 0 to it; see HUM Ch 8.2.10 "PVDLR : Voltage Monitor Lock
 * Register" p 309.
 *
 * The Programmable Voltage Detection Security Attribution Register
 * (PVDSAR) lives in the CPSCU window at 0x4000_8000 base; it carries
 * NONSEC0 / NONSEC1 attribution bits for PVD1 / PVD2.
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
 * @enum ra8_lvd_base_addr_t
 * @brief Memory-mapped base addresses backing the PVD register window.
 *
 * @details
 * The PVD block is fragmented across two peripheral windows:
 *
 *   - SYSC (R_SYSTEM @ 0x4001_E000) holds CMPCR, CR0, CR1, SR, FCR,
 *     and the PVDLR n-channel-lock register.
 *   - CPSCU (R_CPSCU @ 0x4000_8000) holds the PVDSAR security
 *     attribution register (HUM Ch 8.2.1 "PVDSAR" p 302).
 *
 * Offsets in ::ra8_lvd_off_t are stored as absolute addresses (the
 * SYSC / CPSCU base is already added in) so the accessor functions
 * can dereference them directly without per-call base-pointer
 * arithmetic.
 */
typedef enum : uintptr_t {
  k_ra8_lvd_sysc_base_addr  = 0x4001E000UL, /**< R_SYSTEM (SYSC) base. */
  k_ra8_lvd_cpscu_base_addr = 0x40008000UL, /**< R_CPSCU base.         */
} ra8_lvd_base_addr_t;

/**
 * @enum ra8_lvd_off_t
 * @brief Absolute addresses of every PVD-related register on the RA8D2.
 *
 * @details
 * Addresses are absolute (SYSC base + register offset) so the
 * `ra8_lvd_reg8` / `ra8_lvd_reg32` accessor functions reduce to a single
 * volatile cast. Verified against FSP `R_SYSTEM_Type` register-offset
 * comments in R7KA8D2KF_core0.h.
 *
 *   - PVD1CR1   @ SYSC + 0x0E0 (HUM 8.2.6 p 307)
 *   - PVD1SR    @ SYSC + 0x0E1 (HUM 8.2.7 p 307)
 *   - PVD2CR1   @ SYSC + 0x0E2
 *   - PVD2SR    @ SYSC + 0x0E3
 *   - PVD1CMPCR @ SYSC + 0xA58 (HUM 8.2.2 p 303)
 *   - PVD2CMPCR @ SYSC + 0xA5C (HUM 8.2.2 p 303)
 *   - PVD4CMPCR @ SYSC + 0xA64 (HUM 8.2.3 p 304)
 *   - PVD5CMPCR @ SYSC + 0xA68 (HUM 8.2.3 p 304)
 *   - PVD1CR0   @ SYSC + 0xA70 (HUM 8.2.4 p 305)
 *   - PVD2CR0   @ SYSC + 0xA74 (HUM 8.2.4 p 305)
 *   - PVD4CR0   @ SYSC + 0xA7C (HUM 8.2.5 p 306)
 *   - PVD5CR0   @ SYSC + 0xA80 (HUM 8.2.5 p 306)
 *   - PVD1FCR   @ SYSC + 0xB20 (HUM 8.2.8 p 308)
 *   - PVD2FCR   @ SYSC + 0xB24
 *   - PVD4FCR   @ SYSC + 0xB2C (HUM 8.2.9 p 309)
 *   - PVD5FCR   @ SYSC + 0xB30
 *   - PVDLR     @ SYSC + 0xB34 (HUM 8.2.10 p 309)
 *   - PVDSAR    @ CPSCU + 0x3CC (HUM 8.2.1 p 302; FSP `R_CPSCU_Type.LVDSAR`
 *     in R7KA8D2KF_core0.h confirms 0x3CC, not 0x4F8 -- the prior 0x4F8
 *     value was wrong and has been corrected as part of the FSP cross-
 *     verification round.)
 */
typedef enum : uintptr_t {
  /* PVD1 -- m series. */
  k_ra8_lvd_pvd1_cr1_off   = 0x4001E0E0UL, /**< PVD1CR1.   HUM 8.2.6 p 307 */
  k_ra8_lvd_pvd1_sr_off    = 0x4001E0E1UL, /**< PVD1SR.    HUM 8.2.7 p 307 */
  k_ra8_lvd_pvd1_cmpcr_off = 0x4001EA58UL, /**< PVD1CMPCR. HUM 8.2.2 p 303 */
  k_ra8_lvd_pvd1_cr0_off   = 0x4001EA70UL, /**< PVD1CR0.   HUM 8.2.4 p 305 */
  k_ra8_lvd_pvd1_fcr_off   = 0x4001EB20UL, /**< PVD1FCR.   HUM 8.2.8 p 308 */
  /* PVD2 -- m series. */
  k_ra8_lvd_pvd2_cr1_off   = 0x4001E0E2UL, /**< PVD2CR1.   HUM 8.2.6 p 307 */
  k_ra8_lvd_pvd2_sr_off    = 0x4001E0E3UL, /**< PVD2SR.    HUM 8.2.7 p 307 */
  k_ra8_lvd_pvd2_cmpcr_off = 0x4001EA5CUL, /**< PVD2CMPCR. HUM 8.2.2 p 303 */
  k_ra8_lvd_pvd2_cr0_off   = 0x4001EA74UL, /**< PVD2CR0.   HUM 8.2.4 p 305 */
  k_ra8_lvd_pvd2_fcr_off   = 0x4001EB24UL, /**< PVD2FCR.   HUM 8.2.8 p 308 */
  /* PVD4 -- n series (no CR1, no SR). */
  k_ra8_lvd_pvd4_cmpcr_off = 0x4001EA64UL, /**< PVD4CMPCR. HUM 8.2.3 p 304 */
  k_ra8_lvd_pvd4_cr0_off   = 0x4001EA7CUL, /**< PVD4CR0.   HUM 8.2.5 p 306 */
  k_ra8_lvd_pvd4_fcr_off   = 0x4001EB2CUL, /**< PVD4FCR.   HUM 8.2.9 p 309 */
  /* PVD5 -- n series (no CR1, no SR). */
  k_ra8_lvd_pvd5_cmpcr_off = 0x4001EA68UL, /**< PVD5CMPCR. HUM 8.2.3 p 304 */
  k_ra8_lvd_pvd5_cr0_off   = 0x4001EA80UL, /**< PVD5CR0.   HUM 8.2.5 p 306 */
  k_ra8_lvd_pvd5_fcr_off   = 0x4001EB30UL, /**< PVD5FCR.   HUM 8.2.9 p 309 */
  /* Lock + security registers. */
  k_ra8_lvd_pvdlr_off  = 0x4001EB34UL, /**< PVDLR. HUM 8.2.10 p 309 */
  k_ra8_lvd_pvdsar_off = 0x400083CCUL, /**< PVDSAR. HUM 8.2.1 p 302
                                       *   (FSP R_CPSCU_Type.LVDSAR @ 0x3CC). */
} ra8_lvd_off_t;

/**
 * @enum ra8_lvd_cmpcr_mask_t
 * @brief Bit masks for PVDmCMPCR / PVDnCMPCR.
 *
 * @details
 * Per HUM Ch 8.2.2 "PVDmCMPCR" p 303 and HUM Ch 8.2.3 "PVDnCMPCR" p 304:
 *
 *   - bits [4:0] = PVDLVL (detection-voltage level select).
 *   - bit  7     = PVDE   (voltage-detection enable).
 */
typedef enum : uint8_t {
  k_ra8_lvd_cmpcr_mask_pvdlvl = 0x1FU, /**< PVDLVL[4:0] threshold field. */
  k_ra8_lvd_cmpcr_mask_pvde   = 0x80U, /**< PVDE detector enable.        */
} ra8_lvd_cmpcr_mask_t;

/**
 * @enum ra8_lvd_cr0_mask_t
 * @brief Bit masks and shifts for PVDmCR0 / PVDnCR0.
 *
 * @details
 * Per HUM Ch 8.2.4 "PVDmCR0" p 305:
 *
 *   - bit 0    = RIE   (interrupt / reset enable).
 *   - bit 1    = DFDIS (digital-filter disable).
 *   - bit 2    = CMPE  (comparator-output enable).
 *   - bit 3    = "Read as 1, write 1" (always-set marker on m channels).
 *   - bits 5:4 = FSAMP (sampling-clock divider).
 *   - bit 6    = RI    (reset selected).
 *   - bit 7    = RN    (reset-negate timing).
 *
 * PVDnCR0 (HUM Ch 8.2.5 p 306) only has RE @ bit 0, DFDIS @ bit 1,
 * CMPE @ bit 2, FSAMP @ bits 5:4, plus a reserved "read-as-1, write-1"
 * marker at bit 6.
 */
typedef enum : uint8_t {
  k_ra8_lvd_cr0_mask_rie    = 0x01U, /**< RIE (m) -- IRQ/reset enable.   */
  k_ra8_lvd_cr0_mask_re     = 0x01U, /**< RE  (n) -- reset-only enable.  */
  k_ra8_lvd_cr0_mask_dfdis  = 0x02U, /**< DFDIS digital-filter disable.  */
  k_ra8_lvd_cr0_mask_cmpe   = 0x04U, /**< CMPE comparator-output enable. */
  k_ra8_lvd_cr0_mask_bit3   = 0x08U, /**< Reserved bit3 -- write-1 (m).  */
  k_ra8_lvd_cr0_mask_fsamp  = 0x30U, /**< FSAMP[1:0] divider field.      */
  k_ra8_lvd_cr0_mask_ri     = 0x40U, /**< RI reset-on-cross select (m).  */
  k_ra8_lvd_cr0_mask_n_bit6 = 0x40U, /**< Reserved bit6 -- write-1 (n).  */
  k_ra8_lvd_cr0_mask_rn     = 0x80U, /**< RN reset-negate timing (m).    */
} ra8_lvd_cr0_mask_t;

/**
 * @enum ra8_lvd_cr0_shift_t
 * @brief Bit shifts for PVDmCR0 / PVDnCR0 multi-bit fields.
 *
 * @details
 * Per HUM Ch 8.2.4 "PVDmCR0" p 305 -- FSAMP[1:0] occupies bits [5:4].
 */
typedef enum : uint8_t {
  k_ra8_lvd_cr0_shift_fsamp = 4U, /**< FSAMP[1:0] starts at bit 4. */
} ra8_lvd_cr0_shift_t;

/**
 * @enum ra8_lvd_cr1_mask_t
 * @brief Bit masks for PVDmCR1 (m channels only).
 *
 * @details
 * Per HUM Ch 8.2.6 "PVDmCR1 : Voltage Monitor m Circuit Control Register"
 * p 307:
 *
 *   - bits [1:0] = IDTSEL (edge selector: rise / fall / both / prohibited).
 *   - bit  2     = IRQSEL (NMI vs maskable IRQ).
 */
typedef enum : uint8_t {
  k_ra8_lvd_cr1_mask_idtsel = 0x03U, /**< IDTSEL[1:0] edge-select field.  */
  k_ra8_lvd_cr1_mask_irqsel = 0x04U, /**< IRQSEL maskable / NMI selector. */
} ra8_lvd_cr1_mask_t;

/**
 * @enum ra8_lvd_sr_mask_t
 * @brief Bit masks for PVDmSR (m channels only).
 *
 * @details
 * Per HUM Ch 8.2.7 "PVDmSR : Voltage Monitor m Circuit Status Register"
 * p 307:
 *
 *   - bit 0 = DET (latched threshold-crossing flag, write-0-to-clear).
 *   - bit 1 = MON (live "VCC above Vdetm" flag).
 */
typedef enum : uint8_t {
  k_ra8_lvd_sr_mask_det = 0x01U, /**< DET latched-crossing flag.     */
  k_ra8_lvd_sr_mask_mon = 0x02U, /**< MON live above-threshold flag. */
} ra8_lvd_sr_mask_t;

/**
 * @enum ra8_lvd_fcr_mask_t
 * @brief Bit mask for PVDmFCR / PVDnFCR.
 *
 * @details
 * Per HUM Ch 8.2.8 "PVDmFCR" p 308 and HUM Ch 8.2.9 "PVDnFCR" p 309 the
 * only writable bit is RHSEL @ bit 0.
 */
typedef enum : uint8_t {
  k_ra8_lvd_fcr_mask_rhsel = 0x01U, /**< RHSEL hysteresis-band selector. */
} ra8_lvd_fcr_mask_t;

/**
 * @enum ra8_lvd_pvdlr_t
 * @brief PVDLR (Voltage Monitor Lock Register) field constants.
 *
 * @details
 * Per HUM Ch 8.2.10 "PVDLR : Voltage Monitor Lock Register" p 309:
 *
 *   - bit 0 = LOCK. Resets to 1; writing 0 once after a qualifying
 *     reset releases the lock. Writing any value after that
 *     permanently re-locks until the next POR / RES / PVD0.
 */
typedef enum : uint8_t {
  k_ra8_lvd_pvdlr_mask_lock    = 0x01U, /**< LOCK bit position.            */
  k_ra8_lvd_pvdlr_value_unlock = 0x00U, /**< Write 0 to release lock.      */
  k_ra8_lvd_pvdlr_value_relock = 0x01U, /**< Any nonzero re-locks forever. */
} ra8_lvd_pvdlr_t;

/**
 * @enum ra8_lvd_pvdsar_mask_t
 * @brief PVDSAR (PVD Security Attribution Register) bit masks.
 *
 * @details
 * Per HUM Ch 8.2.1 "PVDSAR : Programmable Voltage Detection Security
 * Attribution Register" p 302. PVDSAR is a 32-bit register; only bits
 * [1:0] are functional. Bits [31:2] are reserved (read-as-0,
 * write-0).
 */
typedef enum : uint32_t {
  k_ra8_lvd_pvdsar_mask_nonsec0 = 0x00000001UL, /**< NONSEC0: PVD1 -> NS. */
  k_ra8_lvd_pvdsar_mask_nonsec1 = 0x00000002UL, /**< NONSEC1: PVD2 -> NS. */
  k_ra8_lvd_pvdsar_mask_all     = 0x00000003UL, /**< Both PVD1 + PVD2 NS. */
} ra8_lvd_pvdsar_mask_t;

/**
 * @enum ra8_lvd_filter_const_t
 * @brief Constants used by `ra8_lvd_filter_delay_us`.
 *
 * @details
 * Implements the HUM Table 8.4 step 8 / Table 8.6 step 8 wait formula:
 *
 *     us = (factor * 2^(div+1) + extra) * 1_000_000 / loco_hz + 1
 *
 * with factor = 2 and extra = 3 cycles (HUM Ch 8.2.4 p 305 "FSAMP[1:0]
 * bits"). The default LOCO frequency is 32.768 kHz.
 */
typedef enum : uint32_t {
  k_ra8_lvd_filter_factor   = 2U,       /**< "2s" multiplier in HUM formula.  */
  k_ra8_lvd_filter_extra    = 3U,       /**< "+3" LOCO cycles in HUM formula. */
  k_ra8_lvd_us_per_sec      = 1000000U, /**< Microseconds per second.         */
  k_ra8_lvd_loco_hz_default = 32768U,   /**< Nominal RA8D2 LOCO frequency.    */
} ra8_lvd_filter_const_t;

/**
 * @enum ra8_lvd_misc_const_t
 * @brief Miscellaneous PVD constants.
 *
 * @details
 * `k_ra8_lvd_nmi_channel_count` is the number of monitor m channels
 * (PVD1, PVD2) -- both have an RI bit that, when set, blocks Deep
 * Software Standby modes 2/3 (HUM Ch 8.2.4 p 305 "RI bit").
 */
typedef enum : uint8_t {
  k_ra8_lvd_nmi_channel_count = 2U, /**< PVD1 + PVD2 (m channels). */
} ra8_lvd_misc_const_t;

/**
 * @brief Get a volatile `uint8_t*` to a PVD 8-bit register.
 *
 * @details
 * The offset enum values are absolute virtual addresses (SYSC base
 * already added in) so the accessor reduces to a single cast. This
 * lets the driver re-use the same accessor for the SYSC-resident
 * registers and the CPSCU-resident PVDSAR (whose 32-bit accessor
 * uses ::ra8_lvd_reg32).
 *
 * @param[in] off Register offset constant from ::ra8_lvd_off_t.
 * @return Volatile pointer to the register byte.
 *
 * @pre `off` is one of the enum values in ::ra8_lvd_off_t.
 * @post Hardware state unchanged.
 *
 * @note Thread safety: pure pointer cast, MT-safe.
 * @since 0.1.0
 */
static inline volatile uint8_t* ra8_lvd_reg8(ra8_lvd_off_t off)
{
  return (volatile uint8_t*)(uintptr_t)off;
}

/**
 * @brief Get a volatile `uint32_t*` to a PVD 32-bit register.
 *
 * @details
 * Currently only ::k_ra8_lvd_pvdsar_off is read or written via this
 * accessor (HUM Ch 8.2.1 "PVDSAR" p 302). Other PVD registers are
 * 8-bit and use ::ra8_lvd_reg8.
 *
 * @param[in] off Register offset constant from ::ra8_lvd_off_t.
 * @return Volatile pointer to the 32-bit register.
 *
 * @pre `off` points to a 4-byte-aligned register (PVDSAR satisfies this).
 * @post Hardware state unchanged.
 *
 * @note Thread safety: pure pointer cast, MT-safe.
 * @since 0.1.0
 */
static inline volatile uint32_t* ra8_lvd_reg32(ra8_lvd_off_t off)
{
  return (volatile uint32_t*)(uintptr_t)off;
}

#ifdef __cplusplus
}
#endif
