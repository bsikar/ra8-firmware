/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_vreg_regs.h
 * @brief Internal Voltage Regulator (VREG / DCDC / LDO) register layout for RA8D2
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * The RA8D2 supplies its core voltage (VDD) from one of two
 * regulators selected by the OFS2.DCDCEN option byte (factory
 * configuration -- not changed at runtime):
 *
 *   - DCDC mode  (OFS2.DCDCEN = 1) -- switching regulator with VLO inductor.
 *   - External VDD mode (OFS2.DCDCEN = 0) -- VDD supplied directly from VCL.
 *
 * The Hardware User's Manual chapter "Internal Voltage Regulator"
 * (HUM Ch 68 p 4032-4034) describes the pin / capacitor topology
 * for those two modes; the runtime control bits live in the SYSC
 * block alongside CGC / LPM and are documented separately in the
 * LPM chapter. Every citation in this file points at the Ch 68 page
 * range so the cite-check stays consistent.
 *
 * This header mirrors only the VREG-related offsets inside the SYSC
 * window (`0x4001E000`) so the driver in `libs/ra8_hal/src/ra8_vreg.c`
 * can stay self-contained without extending the shared
 * `ra8_system_regs.h`. The offsets are taken from the FSP CMSIS
 * device header `R7KA8D2KF_core0.h` (R_SYSTEM struct, lines 16168 ff.):
 *
 * | Offset | Reg     | Width | Purpose                                        |
 * |-------:|---------|------:|------------------------------------------------|
 * |  0x440 | DCDCCTL |     8 | DCDC/LDO main control (DCDCON, OCPEN, ...)   |
 * |  0x441 | VCCSEL  |     8 | DCDC working-voltage range select (VCCSEL[1:0])|
 * |  0xAB0 | LVOCR   |     8 | Low-voltage operation control (LVO0E, LVO1E)   |
 *
 * Bit-field layouts mirror FSP `R_SYSTEM_DCDCCTL_*` /
 * `R_SYSTEM_VCCSEL_*` / `R_SYSTEM_LVOCR_*` defines. They are
 * re-derived here as typed enums per CLAUDE.md so no FSP source is
 * copied verbatim.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @enum ra8_vreg_addr_t
 * @brief SYSC base address that hosts every VREG register.
 *
 * @details
 * The VREG control bits share the SYSC block with CGC, LPM, LVD and
 * battery-backup state. They live at fixed offsets in the SYSC
 * window; we re-publish that base here so the VREG driver does not
 * have to pull in `ra8_system_regs.h`.
 */
typedef enum : uintptr_t {
  k_ra8_vreg_sysc_base_addr = 0x4001E000UL, /**< R_SYSTEM (SYSC) base. */
} ra8_vreg_addr_t;

/**
 * @enum ra8_vreg_off_t
 * @brief Byte offsets of every VREG-related register inside SYSC.
 *
 * @details
 * Verified against FSP R_SYSTEM struct (R7KA8D2KF_core0.h):
 *
 *   - DCDCCTL @ 0x440 -- DCDC/LDO Control Register (8-bit).
 *   - VCCSEL  @ 0x441 -- DCDC Working Voltage Selection (8-bit).
 *   - LVOCR   @ 0xAB0 -- Low Voltage Operation Control (8-bit).
 *
 * The chapter we cite is HUM Ch 68 (Internal Voltage Regulator), even
 * though the runtime register descriptions live in the LPM chapter --
 * Ch 68 is the documentation root for the regulator block on RA8D2.
 */
typedef enum : uintptr_t {
  k_ra8_vreg_off_dcdcctl = 0x440U, /**< DCDCCTL offset. */
  k_ra8_vreg_off_vccsel  = 0x441U, /**< VCCSEL offset.  */
  k_ra8_vreg_off_lvocr   = 0xAB0U, /**< LVOCR offset.   */
} ra8_vreg_off_t;

/**
 * @enum ra8_vreg_dcdcctl_bit_t
 * @brief DCDCCTL bit positions (mirrors FSP R_SYSTEM_DCDCCTL_*_Pos).
 *
 * @details
 * - DCDCON  [0]   : LDO/DCDC on/off control. 0 = LDO, 1 = DCDC.
 * - OCPEN   [1]   : DCDC over-current protection enable.
 * - STOPZA  [4]   : DCDC IO buffer power control (driven during the
 *                   LDO->DCDC handshake; gated by DCDCON otherwise).
 * - LCBOOST [5]   : LDO charge-pump boost (used in subosc-speed mode
 *                   to keep the LDO regulating below ~1.8 V).
 * - FST     [6]   : DCDC fast-startup mode (skip the long Vref soak).
 * - PD      [7]   : DCDC VREF generate disable. 1 = Vref off, 0 = on.
 *
 * Bits 2 and 3 are reserved and must be written 0.
 */
typedef enum : uint8_t {
  k_ra8_vreg_bit_dcdcon  = 0U, /**< LDO/DCDC on/off control.             */
  k_ra8_vreg_bit_ocpen   = 1U, /**< DCDC over-current protection enable. */
  k_ra8_vreg_bit_stopza  = 4U, /**< DCDC IO buffer power control.        */
  k_ra8_vreg_bit_lcboost = 5U, /**< LDO charge-pump boost (subosc mode). */
  k_ra8_vreg_bit_fst     = 6U, /**< DCDC fast-startup mode.              */
  k_ra8_vreg_bit_pd      = 7U, /**< DCDC VREF generate disable.          */
} ra8_vreg_dcdcctl_bit_t;

/**
 * @enum ra8_vreg_dcdcctl_mask_t
 * @brief DCDCCTL bit masks.
 *
 * @details
 * `k_ra8_vreg_mask_dcdcctl_all` is the union of every writable bit
 * (0,1,4,5,6,7 = 0xF3) and is used by `ra8_vreg_clear_status()` to
 * reject malformed masks.
 */
typedef enum : uint8_t {
  k_ra8_vreg_mask_dcdcon      = 0x01U,          /**< Bit 0.                                    */
  k_ra8_vreg_mask_ocpen       = 0x02U,          /**< Bit 1.                                    */
  k_ra8_vreg_mask_stopza      = 0x10U,          /**< Bit 4.                                    */
  k_ra8_vreg_mask_lcboost     = 0x20U,          /**< Bit 5.                                    */
  k_ra8_vreg_mask_fst         = 0x40U,          /**< Bit 6.                                    */
  k_ra8_vreg_mask_pd          = 0x80U,          /**< Bit 7.                                    */
  k_ra8_vreg_mask_dcdcctl_all = (uint8_t)0xF3U, /**< Union of all writable bits (0,1,4,5,6,7). */
} ra8_vreg_dcdcctl_mask_t;

/**
 * @enum ra8_vreg_dcdcctl_step_t
 * @brief Pre-baked DCDCCTL values used by the LDO->DCDC handshake.
 *
 * @details
 * The FSP-style DCDC-enable sequence walks DCDCCTL through several
 * intermediate values before settling on the final 0x13 (DCDCON +
 * OCPEN + STOPZA). These constants name those intermediate values so
 * the driver does not have to use raw hex literals.
 *
 *   - `lp_vref`   = 0x10 (STOPZA on, everything else off) -- low-power
 *     Vref selected.
 *   - `dcdc_on`   = 0x11 (STOPZA + DCDCON) -- LDO off, DCDC on, OCP off.
 *   - `with_ocp`  = 0x13 (STOPZA + DCDCON + OCPEN) -- final DCDC state.
 *   - `fast_on`   = 0x53 (STOPZA + DCDCON + OCPEN + FST) -- final DCDC
 *     state with fast-startup enabled.
 */
typedef enum : uint8_t {
  k_ra8_vreg_dcdc_step_lp_vref  = 0x10U, /**< STOPZA only -- low-power Vref step.    */
  k_ra8_vreg_dcdc_step_dcdc_on  = 0x11U, /**< STOPZA + DCDCON -- LDO off, DCDC on.   */
  k_ra8_vreg_dcdc_step_with_ocp = 0x13U, /**< STOPZA + DCDCON + OCPEN -- final DCDC. */
  k_ra8_vreg_dcdc_step_fast_on  = 0x53U, /**< STOPZA + DCDCON + OCPEN + FST -- fast. */
} ra8_vreg_dcdcctl_step_t;

/**
 * @enum ra8_vreg_vccsel_t
 * @brief VCCSEL[1:0] supply-voltage range selection (DCDC mode only).
 *
 * @details
 * From FSP R_SYSTEM_VCCSEL_VCCSEL_Msk (mask 0x03). The chip uses the
 * VCCSEL value to bias the DCDC switching regulator for the supplied
 * VCC range; the firmware picks the range that matches the board's
 * external supply.
 */
typedef enum : uint8_t {
  k_ra8_vreg_vccsel_2v4_to_2v7 = 0U, /**< 2.4 V <= VCC < 2.7 V.  */
  k_ra8_vreg_vccsel_2v7_to_3v0 = 1U, /**< 2.7 V <= VCC < 3.0 V.  */
  k_ra8_vreg_vccsel_3v0_to_3v6 = 2U, /**< 3.0 V <= VCC <= 3.6 V. */
  k_ra8_vreg_vccsel_mask       = 3U, /**< 2-bit field mask.      */
} ra8_vreg_vccsel_t;

/**
 * @enum ra8_vreg_lvocr_mask_t
 * @brief LVOCR low-voltage-operation enables.
 *
 * @details
 * `LVO0E` selects "low-voltage profile 0" (relaxed timing for the
 * 1.6-1.8 V VDD window); `LVO1E` selects profile 1 (1.8-2.0 V).
 * Only one of the two should be set at a time; the driver enforces
 * this via the `k_ra8_vreg_lvo_*` enum.
 */
typedef enum : uint8_t {
  k_ra8_vreg_mask_lvo0e   = 0x01U, /**< LVO0E -- low-voltage profile 0 enable. */
  k_ra8_vreg_mask_lvo1e   = 0x02U, /**< LVO1E -- low-voltage profile 1 enable. */
  k_ra8_vreg_mask_lvo_all = 0x03U, /**< Union of every LVOCR writable bit.     */
} ra8_vreg_lvocr_mask_t;

/**
 * @enum ra8_vreg_timing_us_t
 * @brief Stabilisation delays prescribed by the LPM chapter for
 *        every documented DCDC <-> LDO transition.
 *
 * @details
 * The numbers come from the LPM chapter sub-section "Switching
 * between DCDC and LDO power modes" referenced by HUM Ch 68:
 *
 *   - LDO -> DCDC IO buffer enable: ~10 us soak before turning on Vref.
 *   - LDO -> DCDC Vref stabilisation: ~10 us.
 *   - DCDC switch-on: ~2 us before OCP can be enabled.
 *   - LDO stabilisation after DCDC -> LDO switch: ~60 us.
 *   - DCDC fast-startup uses ~22 us total (skipping the long Vref soak).
 *
 * The driver does not call ``R_BSP_SoftwareDelay`` here -- it stages
 * the values so the caller can wrap each step in its own delay
 * provider when running on real silicon.
 */
typedef enum : uint16_t {
  k_ra8_vreg_delay_iobuf_us = 10U, /**< STOPZA -> Vref handshake soak.     */
  k_ra8_vreg_delay_vref_us  = 10U, /**< Vref stabilisation.                */
  k_ra8_vreg_delay_dcdc_us  = 2U,  /**< DCDC switch-on settle.             */
  k_ra8_vreg_delay_ldo_us   = 60U, /**< LDO stabilisation after DCDC->LDO. */
  k_ra8_vreg_delay_fast_us  = 22U, /**< DCDC fast-startup total.           */
} ra8_vreg_timing_us_t;

/**
 * @brief Get a volatile pointer to the 8-bit DCDCCTL register.
 *
 * @details
 * On the embedded target this returns a pointer to the actual SYSC
 * memory window; on the host unit-test build the fake mmap
 * (`tests/mocks/ra8_fake_mmap.c`) maps anonymous RAM at the same
 * virtual address so reads / writes through this pointer behave
 * sensibly in test mode.
 *
 * @return Volatile pointer to DCDCCTL (8 bits).
 */
static inline volatile uint8_t* ra8_vreg_dcdcctl(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_vreg_sysc_base_addr +
                             (uintptr_t)k_ra8_vreg_off_dcdcctl);
}

/**
 * @brief Get a volatile pointer to the 8-bit VCCSEL register.
 * @return Volatile pointer to VCCSEL.
 */
static inline volatile uint8_t* ra8_vreg_vccsel(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_vreg_sysc_base_addr +
                             (uintptr_t)k_ra8_vreg_off_vccsel);
}

/**
 * @brief Get a volatile pointer to the 8-bit LVOCR register.
 * @return Volatile pointer to LVOCR.
 */
static inline volatile uint8_t* ra8_vreg_lvocr(void)
{
  return (volatile uint8_t*)((uintptr_t)k_ra8_vreg_sysc_base_addr +
                             (uintptr_t)k_ra8_vreg_off_lvocr);
}

#ifdef __cplusplus
}
#endif
