/**
 * @file ra8_vreg.h
 * @brief Internal Voltage Regulator (DCDC / LDO) driver -- full HUM Ch 68 surface
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * The RA8D2 core voltage rail (VDD) is sourced from one of two
 * regulators selected at flash-programming time by the OFS2.DCDCEN
 * option byte (HUM Ch 68.1):
 *
 *   - **DCDC mode** -- on-chip switching regulator with external 2.2-uH
 *     inductor and 47-uF capacitor on every VLO pin. Highest efficiency
 *     in High-Speed and Middle-Speed operating modes; **does not**
 *     support Software Standby, Deep Software Standby modes 1/2/3,
 *     Battery Backup or Voltage Scaling Control.
 *   - **External VDD mode** -- VDD supplied directly to the VCL pins
 *     from an external regulator; the on-chip DCDC is bypassed.
 *
 * Once the OFS byte selects a topology, the runtime API in this file
 * is responsible for:
 *
 *   - Bringing the regulator to a known state (`ra8_vreg_init`).
 *   - Selecting the supply-voltage range so the DCDC biases correctly
 *     for the board's actual VCC (`ra8_vreg_set_vccsel`).
 *   - Switching between DCDC and LDO power modes at runtime
 *     (`ra8_vreg_set_mode`) -- this is the same transition FSP performs
 *     in `bsp_clocks.c` when entering Subosc-speed mode.
 *   - Toggling the LDO charge-pump boost (`ra8_vreg_set_ldo_boost`) so
 *     the LDO can regulate VDD all the way down to the subosc-speed
 *     window.
 *   - Programming the OCP threshold (`ra8_vreg_set_ocp`) to match the
 *     board's expected current envelope.
 *   - Choosing the fast-startup or normal startup sequence
 *     (`ra8_vreg_set_fast_startup`).
 *   - Selecting one of the documented Low-Voltage profiles
 *     (`ra8_vreg_set_lv_profile`).
 *   - Parking the chip for any of the Software Standby variants
 *     (`ra8_vreg_enter_standby`) and restoring afterwards.
 *   - Reading status (`ra8_vreg_get_status`).
 *   - Powering the block down (`ra8_vreg_deinit`).
 *
 * ## What the driver intentionally does NOT do
 *
 *   - Reprogram OFS2 -- that's a one-shot factory operation handled
 *     by the FLASH driver.
 *   - Touch LVD trip points -- see `ra8_lvd` for that.
 *   - Sequence the full Software Standby exit (clock restart, RAM
 *     wake) -- see `ra8_lpm`. This driver only parks / restores the
 *     regulator state; the LPM driver wraps the WFI.
 *
 * ## State machine
 *
 * @par State Machine
 * @dot
 * digraph ra8_vreg_states {
 *   bgcolor="transparent";
 *   rankdir=LR;
 *   node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
 *         fillcolor="#e8eef7", color="#5a7ca6"];
 *   edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
 *
 *   __start [shape=circle, width=0.18, label="", fillcolor="#5a7ca6", color="#5a7ca6"];
 *   __end [shape=doublecircle, width=0.20, label="", fillcolor="#5a7ca6", color="#5a7ca6"];
 *
 *   Reset [label="Reset"];
 *   LDO [label="LDO"];
 *   DCDC [label="DCDC"];
 *   Standby [label="Standby"];
 *
 *   __start -> Reset;
 *   Reset -> LDO [label="ra8_vreg_init(LDO)"];
 *   Reset -> DCDC [label="ra8_vreg_init(DCDC)"];
 *   LDO -> DCDC [label="ra8_vreg_set_mode(DCDC) --\n22 us blocking"];
 *   DCDC -> LDO [label="ra8_vreg_set_mode(LDO) -- 60\nus blocking"];
 *   LDO -> Standby [label="ra8_vreg_enter_standby()"];
 *   DCDC -> Standby [label="k_ra8_err_invalid_state\n(must switch to LDO first)"];
 *   Standby -> LDO [label="ra8_vreg_exit_standby() (or\nDCDC if cached)"];
 *   LDO -> __end [label="ra8_vreg_deinit()"];
 *   DCDC -> __end [label="ra8_vreg_deinit()"];
 * }
 * @enddot
 *
 * | From   | To      | Function                  | Allowed?      |
 * |--------|---------|---------------------------|---------------|
 * | Reset  | LDO     | ra8_vreg_init              | always        |
 * | Reset  | DCDC    | ra8_vreg_init              | OFS2.DCDCEN=1 |
 * | LDO    | DCDC    | ra8_vreg_set_mode          | VCC >= 2.4 V  |
 * | DCDC   | LDO     | ra8_vreg_set_mode          | always        |
 * | LDO    | Standby | ra8_vreg_enter_standby     | always        |
 * | DCDC   | Standby | ra8_vreg_enter_standby     | rejected      |
 * | DCDC   | DSBY    | ra8_vreg_enter_standby     | rejected      |
 *
 * ## IRQ surface
 *
 * The voltage regulator block itself does not raise an interrupt on
 * RA8D2; over-current / under-voltage events are reported through the
 * LVD channels and the DCDC over-current detector inside the LPM
 * IRQ. This driver therefore exposes only `attach_handler` as a
 * forwarding hook -- the caller wires the LVD ISR to call back here
 * if a unified "VREG fault" callback is desired.
 *
 * @warning Misconfiguring the regulator can damage the chip or cause
 *          undervoltage resets. In particular:
 *            - DCDC mode requires VCC >= 2.4 V at all times. Below that
 *              threshold the regulator must be switched back to LDO via
 *              `ra8_vreg_set_mode(k_ra8_vreg_mode_ldo)`.
 *            - Switching to DCDC blocks for ~22 us with all interrupts
 *              disabled; switching back to LDO blocks for ~60 us.
 *            - VCCSEL must match the actual board VCC range or the DCDC
 *              efficiency curve will be wrong and the part may overheat.
 *            - Setting both LVO0E and LVO1E in LVOCR is undefined --
 *              use `ra8_vreg_set_lv_profile()` to enforce mutual
 *              exclusion.
 *
 * @see HUM Ch 68 "Internal Voltage Regulator" p 4032-4034.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_vreg_regs.h"

/**
 * @enum ra8_vreg_mode_t
 * @brief Runtime regulator mode selection (DCDCCTL.DCDCON).
 */
typedef enum : uint8_t {
  k_ra8_vreg_mode_ldo  = 0U, /**< Low-dropout linear regulator (default at reset). */
  k_ra8_vreg_mode_dcdc = 1U, /**< Switching regulator (DCDC).                      */
} ra8_vreg_mode_t;

/**
 * @enum ra8_vreg_lv_profile_t
 * @brief Low-voltage operation profile (LVOCR).
 *
 * @details
 * The chip exposes two mutually exclusive low-voltage profiles:
 *
 *   - Profile 0 (LVO0E=1, LVO1E=0) -- relaxed timing for VDD in the
 *     1.6-1.8 V window.
 *   - Profile 1 (LVO0E=0, LVO1E=1) -- relaxed timing for VDD in the
 *     1.8-2.0 V window.
 *
 * The "off" value clears LVOCR entirely so the chip runs the full-speed
 * timing.
 */
typedef enum : uint8_t {
  k_ra8_vreg_lv_off = 0U, /**< Both LVO0E and LVO1E cleared (full-speed). */
  k_ra8_vreg_lv_p0  = 1U, /**< LVO0E set, LVO1E cleared.                  */
  k_ra8_vreg_lv_p1  = 2U, /**< LVO1E set, LVO0E cleared.                  */
} ra8_vreg_lv_profile_t;

/**
 * @enum ra8_vreg_standby_t
 * @brief Software Standby variant the caller intends to enter next.
 *
 * @details
 * Used by `ra8_vreg_enter_standby()` to validate the regulator state.
 * HUM Ch 68.2.2 explicitly forbids DCDC mode for any of these
 * variants -- the driver returns `k_ra8_err_invalid_state` if the
 * current mode is DCDC.
 */
typedef enum : uint8_t {
  k_ra8_vreg_standby_software       = 0U, /**< Software Standby mode.        */
  k_ra8_vreg_standby_deep_software1 = 1U, /**< Deep Software Standby mode 1. */
  k_ra8_vreg_standby_deep_software2 = 2U, /**< Deep Software Standby mode 2. */
  k_ra8_vreg_standby_deep_software3 = 3U, /**< Deep Software Standby mode 3. */
  k_ra8_vreg_standby_battery_backup = 4U, /**< Battery Backup wake source.   */
} ra8_vreg_standby_t;

/**
 * @enum ra8_vreg_ocp_t
 * @brief DCDC over-current protection threshold programming.
 *
 * @details
 * The chip only exposes a single OCP enable bit (DCDCCTL.OCPEN); the
 * "level" enums in this file map onto enabling OCP plus tracking the
 * caller's policy in software so a future silicon stepping with a
 * multi-level OCP can grow the API without breaking callers.
 *
 *   - off    -- OCPEN cleared; no over-current protection. Used during
 *               the LDO->DCDC handshake before the DCDC stabilises.
 *   - normal -- OCPEN set; standard threshold (this is the only level
 *               the silicon actually programs today).
 *   - low    -- Reserved for a future stepping; today the driver
 *               accepts the value but programs the same OCPEN bit.
 *   - high   -- Same as `low` but reserved for the high-current rail.
 */
typedef enum : uint8_t {
  k_ra8_vreg_ocp_off    = 0U, /**< OCPEN cleared.                      */
  k_ra8_vreg_ocp_normal = 1U, /**< OCPEN set; default threshold.       */
  k_ra8_vreg_ocp_low    = 2U, /**< OCPEN set; reserved for future use. */
  k_ra8_vreg_ocp_high   = 3U, /**< OCPEN set; reserved for future use. */
} ra8_vreg_ocp_t;

/**
 * @struct ra8_vreg_cfg_t
 * @brief Configuration descriptor for `ra8_vreg_init`.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused; each
 * member is read in `ra8_vreg_init` in `libs/ra8_hal/src/ra8_vreg.c`.
 */
typedef struct {
  ra8_vreg_mode_t       mode;         /**< Initial regulator mode.                         */
  ra8_vreg_vccsel_t     vccsel;       /**< Supply-voltage range (used only in DCDC mode).  */
  ra8_vreg_ocp_t        ocp;          /**< OCP threshold (DCDC mode only).                 */
  bool                  fast_startup; /**< Use fast-startup sequence (DCDCCTL.FST).        */
  bool                  ldo_boost;    /**< Enable LDO charge-pump boost (DCDCCTL.LCBOOST). */
  ra8_vreg_lv_profile_t lv_profile;   /**< Low-voltage operation profile (LVOCR).          */
} ra8_vreg_cfg_t;

/**
 * @struct ra8_vreg_status_t
 * @brief Snapshot of the regulator state.
 *
 * @details
 * Decoded view returned by `ra8_vreg_get_status()`. Both the raw
 * register values and the decoded enum forms are exposed so a caller
 * can pick whichever level of detail it needs.
 */
typedef struct {
  uint8_t               dcdcctl;      /**< Raw DCDCCTL value.                */
  uint8_t               vccsel;       /**< Raw VCCSEL value.                 */
  uint8_t               lvocr;        /**< Raw LVOCR value.                  */
  ra8_vreg_mode_t       mode;         /**< Decoded mode (LDO / DCDC).        */
  ra8_vreg_vccsel_t     vccsel_dec;   /**< Decoded VCCSEL.                   */
  ra8_vreg_lv_profile_t lv_profile;   /**< Decoded LVOCR profile.            */
  ra8_vreg_ocp_t        ocp;          /**< Decoded OCP setting.              */
  bool                  dcdc_ready;   /**< True when DCDCON=1 and PD=0.      */
  bool                  fast_startup; /**< True when DCDCCTL.FST is set.     */
  bool                  ldo_boost;    /**< True when DCDCCTL.LCBOOST is set. */
  bool                  io_buf_on;    /**< True when DCDCCTL.STOPZA is set.  */
} ra8_vreg_status_t;

/**
 * @typedef ra8_vreg_event_fn_t
 * @brief Voltage-regulator fault callback.
 *
 * @details
 * Invoked from `ra8_vreg_dispatch()`. The driver does not own an IRQ
 * line on RA8D2 -- a higher layer (typically the LVD or a user fault
 * router) wires its ISR to call `ra8_vreg_dispatch()` so this callback
 * can run.
 *
 * @param[in] ctx        Caller context recorded at attach time.
 * @param[in] status_word Snapshot of `DCDCCTL` at dispatch time.
 */
typedef void (*ra8_vreg_event_fn_t)(void* ctx, uint8_t status_word);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise the internal voltage regulator from a config descriptor.
 *
 * @details
 * Programs DCDCCTL / VCCSEL / LVOCR according to `cfg`. Selecting
 * `k_ra8_vreg_mode_dcdc` runs the FSP-style four-step DCDC enable
 * sequence (enable IO buffer -> turn on Vref -> low-power Vref ->
 * switch DCDC + LDO off + OCP enable). The transition is purely
 * register writes here; on real silicon the caller must hold the
 * 22 us / 60 us delays prescribed by HUM Ch 68 between steps.
 *
 * Algorithm:
 *  1. Validate every field of `cfg`.
 *  2. Clear LVOCR so neither low-voltage profile is left over.
 *  3. Program VCCSEL.
 *  4. Issue the LDO->DCDC handshake (or write a single LDO value).
 *  5. Apply LDO charge-pump boost if requested.
 *  6. Apply the requested LV profile last.
 *  7. Cache the desired state for `ra8_vreg_exit_standby()`.
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok                 Success.
 * @retval k_ra8_err_null_ptr       `cfg` was NULL.
 * @retval k_ra8_err_invalid_arg    `cfg->vccsel`, `cfg->ocp` or
 *                                 `cfg->lv_profile` was out of range.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Caller has unlocked PRCR group 1 (LPM/VREG protection).
 * @post DCDCCTL reflects `cfg->mode | cfg->ocp | cfg->fast_startup |
 *                         cfg->ldo_boost`.
 * @post VCCSEL reflects `cfg->vccsel` when `cfg->mode == DCDC`.
 * @post LVOCR reflects `cfg->lv_profile`.
 *
 * @note Not thread-safe.
 *
 * @warning Switching modes without the prescribed wait times can latch
 *          the DCDC into an undefined state -- on real silicon,
 *          insert HUM Ch 68 software delays between steps.
 *
 * @see ra8_vreg_set_mode
 * @see ra8_vreg_set_vccsel
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vreg_init(const ra8_vreg_cfg_t* cfg);

/**
 * @brief Tear down the regulator -- return DCDCCTL/VCCSEL/LVOCR to reset state.
 *
 * @details
 * Forces the chip back into LDO mode (DCDCCTL = 0) and clears VCCSEL
 * and LVOCR. Safe to call from any state; if the chip was running in
 * DCDC mode it transitions to LDO first.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok Always (no failure path).
 *
 * @pre Caller has unlocked PRCR group 1.
 * @pre IRQs masked.
 * @post DCDCCTL == 0.
 * @post VCCSEL == 0.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vreg_deinit(void);

/* =============================================================================
 * Runtime control
 * =============================================================================
 */

/**
 * @brief Switch between LDO and DCDC at runtime.
 *
 * @details
 * Performs the LDO->DCDC or DCDC->LDO transition. On real silicon
 * the caller must respect the 22 us / 60 us blocking windows; in
 * driver code the writes are issued immediately and the caller
 * supplies the timer.
 *
 * @param[in] mode Target regulator mode.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok Success.
 * @retval k_ra8_err_invalid_arg `mode` outside the enum.
 *
 * @pre Caller has unlocked PRCR group 1.
 * @pre VCC supply is in the range previously programmed via
 *      `ra8_vreg_set_vccsel`.
 * @post DCDCCTL.DCDCON reflects `mode`.
 * @post On DCDC mode entry, OCPEN is enabled.
 *
 * @note Not thread-safe.
 *
 * @warning Entering DCDC mode while VCC < 2.4 V will cause an
 *          undervoltage reset.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vreg_set_mode(ra8_vreg_mode_t mode);

/**
 * @brief Set the DCDC supply-voltage-range select.
 *
 * @details
 * VCCSEL is only consulted by the chip in DCDC mode -- this driver
 * still permits writing it in any mode so the caller can pre-stage
 * the value before switching modes.
 *
 * @param[in] sel One of `k_ra8_vreg_vccsel_*`.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok Success.
 * @retval k_ra8_err_invalid_arg `sel` is outside the 2-bit field.
 *
 * @pre Caller has unlocked PRCR group 1.
 * @pre `sel` is one of the documented enum values.
 * @post VCCSEL reflects `sel`.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vreg_set_vccsel(ra8_vreg_vccsel_t sel);

/**
 * @brief Program the DCDC over-current-protection level.
 *
 * @details
 * Sets or clears DCDCCTL.OCPEN according to `level`. The non-`off`
 * levels program the same hardware bit today (the silicon only has
 * one OCP threshold) but the driver records the requested level so a
 * future stepping can grow the API without breaking callers.
 *
 * @param[in] level OCP level enum.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok Success.
 * @retval k_ra8_err_invalid_arg `level` outside the enum.
 *
 * @pre Caller has unlocked PRCR group 1.
 * @pre Regulator is in DCDC mode (or the call is staging for a
 *      subsequent LDO->DCDC switch).
 * @post DCDCCTL.OCPEN matches `level != k_ra8_vreg_ocp_off`.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vreg_set_ocp(ra8_vreg_ocp_t level);

/**
 * @brief Enable / disable the DCDC fast-startup sequence.
 *
 * @details
 * When fast-startup is enabled, the LDO->DCDC handshake skips the
 * long Vref soak and uses a single 22 us total transition (vs the
 * default ~32 us). Useful when the firmware needs to leave Software
 * Standby quickly and the analog Vref can be guaranteed stable from
 * the previous run.
 *
 * @param[in] enable True to set DCDCCTL.FST, false to clear.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok Always.
 *
 * @pre Caller has unlocked PRCR group 1.
 * @pre Caller understands that fast-startup is only valid when the
 *      DCDC has previously stabilised.
 * @post DCDCCTL.FST matches `enable`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vreg_set_fast_startup(bool enable);

/**
 * @brief Enable / disable the LDO charge-pump boost.
 *
 * @details
 * The LDO needs the charge-pump boost (DCDCCTL.LCBOOST) when the chip
 * is targeted for Subosc-speed mode -- without it the LDO cannot
 * regulate VDD all the way down. FSP enables LCBOOST as part of the
 * "LDO_BOOST" power-mode enum returned by `R_BSP_PowerModeSet`; this
 * driver exposes it as a standalone toggle so the caller can mix it
 * with arbitrary OPCCR settings.
 *
 * @param[in] enable True to set LCBOOST, false to clear.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok Always.
 *
 * @pre Caller has unlocked PRCR group 1.
 * @pre Regulator is in LDO mode (LCBOOST is ignored in DCDC mode).
 * @post DCDCCTL.LCBOOST matches `enable`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vreg_set_ldo_boost(bool enable);

/**
 * @brief Select a low-voltage operation profile.
 *
 * @details
 * Writes LVOCR to enforce mutual exclusion of LVO0E and LVO1E.
 * Callers that just want full-speed timing pass `k_ra8_vreg_lv_off`.
 *
 * @param[in] profile One of `k_ra8_vreg_lv_off / lv_p0 / lv_p1`.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok Success.
 * @retval k_ra8_err_invalid_arg `profile` outside the enum.
 *
 * @pre Caller has unlocked PRCR group 1.
 * @pre Caller has set VDD into the corresponding voltage window.
 * @post LVOCR reflects exactly one bit (or zero).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vreg_set_lv_profile(ra8_vreg_lv_profile_t profile);

/* =============================================================================
 * Status
 * =============================================================================
 */

/**
 * @brief Read the regulator status into a caller-supplied snapshot.
 *
 * @param[out] out Non-NULL receiver for the snapshot.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok           Success.
 * @retval k_ra8_err_null_ptr `out` was NULL.
 *
 * @pre `out` is a valid writable pointer.
 * @pre Single-threaded read or with IRQs masked.
 * @post `out->dcdcctl` / `out->vccsel` / `out->lvocr` reflect the
 *       live register values.
 * @post `out->mode` / `out->vccsel_dec` / `out->dcdc_ready` reflect
 *       the decoded state.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vreg_get_status(ra8_vreg_status_t* out);

/**
 * @brief Clear the bits in DCDCCTL named by `mask`.
 *
 * @details
 * Convenience helper for ISR bookkeeping. The caller passes a mask
 * built from `k_ra8_vreg_mask_*` values; the corresponding bits are
 * cleared in DCDCCTL while the rest are preserved. Any bits outside
 * `k_ra8_vreg_mask_dcdcctl_all` are rejected with
 * `k_ra8_err_invalid_arg`.
 *
 * @param[in] mask Bit mask of DCDCCTL bits to clear.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok               Success.
 * @retval k_ra8_err_invalid_arg  `mask` includes reserved bits.
 *
 * @pre Caller has unlocked PRCR group 1.
 * @pre `mask` only contains bits from `k_ra8_vreg_mask_dcdcctl_all`.
 * @post DCDCCTL has the bits in `mask` cleared.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vreg_clear_status(uint8_t mask);

/**
 * @brief Reset the regulator to its OFS-byte default state.
 *
 * @details
 * Equivalent to `ra8_vreg_deinit()` followed by clearing the cached
 * "desired" state -- useful for restarting after a recoverable fault
 * (LVD trip, watchdog warning) without having to re-derive the full
 * config.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok Always.
 *
 * @pre Caller has unlocked PRCR group 1.
 * @pre IRQs masked.
 * @post DCDCCTL == 0, VCCSEL == 0, LVOCR == 0.
 * @post Cached "desired" state cleared.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vreg_reset(void);

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @brief Park the regulator for one of the Software Standby variants.
 *
 * @details
 * Software Standby, Deep Software Standby modes 1/2/3, Battery Backup
 * and Voltage Scaling Control are all incompatible with DCDC mode
 * (HUM Ch 68.2.2 p 4033). The driver:
 *
 *   1. Validates `variant` against the enum.
 *   2. Caches the current DCDCCTL/VCCSEL/LVOCR for restore.
 *   3. If the chip is in DCDC mode, switches to LDO and clears OCPEN.
 *
 * The actual WFI is the caller's responsibility (see `ra8_lpm`).
 *
 * @param[in] variant Software Standby variant the caller intends to enter.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok                 Regulator parked.
 * @retval k_ra8_err_invalid_arg    `variant` outside the enum.
 *
 * @pre Caller has unlocked PRCR group 1.
 * @pre Caller drives the chip into Software Standby immediately after.
 * @post DCDCCTL.DCDCON == 0 (LDO mode).
 * @post DCDCCTL.OCPEN == 0.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vreg_enter_standby(ra8_vreg_standby_t variant);

/**
 * @brief Legacy alias for `ra8_vreg_enter_standby(k_ra8_vreg_standby_software)`.
 *
 * @details
 * Kept as a thin wrapper because the lower-level LPM driver still
 * calls the older signature. Internally just forwards.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok Always.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vreg_enter_stop(void);

/**
 * @brief Restore the regulator after Software Standby exit.
 *
 * @details
 * If the firmware was running in DCDC mode before
 * `ra8_vreg_enter_standby`, this restores VCCSEL and re-enables DCDC
 * with OCP. Otherwise it leaves the LDO running.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok Success.
 *
 * @pre Caller has unlocked PRCR group 1.
 * @pre `ra8_vreg_init` previously ran (so the desired state is cached).
 * @post DCDCCTL/VCCSEL match the cached state.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vreg_exit_standby(void);

/**
 * @brief Legacy alias for `ra8_vreg_exit_standby()`.
 * @return ra8_err_t -- forwards to `ra8_vreg_exit_standby()`.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vreg_exit_stop(void);

/* =============================================================================
 * IRQ hook
 * =============================================================================
 */

/**
 * @brief Attach a fault callback (LVD / over-current dispatch hook).
 *
 * @details
 * The callback is invoked synchronously from `ra8_vreg_dispatch()`.
 * Setting `fn = nullptr` detaches.
 *
 * @param[in] fn  Callback or `nullptr` to clear.
 * @param[in] ctx Context forwarded to the callback.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok Always.
 *
 * @pre Single-threaded init or IRQs masked.
 * @pre `ctx` may be NULL if the callback does not need it.
 * @post Subsequent `ra8_vreg_dispatch` calls invoke `fn(ctx, ...)`.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vreg_attach_handler(ra8_vreg_event_fn_t fn, void* ctx);

/**
 * @brief Fire the registered fault callback with the current DCDCCTL value.
 *
 * @details
 * Intended to be called from the LVD / fault-router ISR. Snapshots
 * DCDCCTL (HUM Ch 11 "Voltage Regulator (VREG)", p 581) and forwards
 * the value to the registered callback. No-op when no callback is
 * attached.
 *
 * @pre Called from ISR context or a host-test driver.
 * @pre VREG MSTP gate is open.
 *
 * @post Registered callback executed once.
 * @post DCDCCTL is unchanged.
 *
 * @note Not thread-safe; pair with NVIC masking.
 * @since 0.1.0
 */
void ra8_vreg_dispatch(void);

#ifdef __cplusplus
}
#endif
