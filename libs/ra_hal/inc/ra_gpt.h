/**
 * @file ra_gpt.h
 * @brief Full-featured General PWM Timer (GPT) driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Wave 3.5 full build-out of the RA8D2 GPT peripheral. Extends
 * the Wave 0 free-run stub with: descriptor-based init, deinit,
 * runtime period / duty reconfigure, status flags, interrupt
 * attach / dispatch, power transition.
 *
 * API surface:
 *
 *  - ``ra_gpt_init(channel, cfg)``       -- full config + MSTP enable
 *  - ``ra_gpt_deinit(channel)``          -- stop + MSTP release
 *  - ``ra_gpt_start_free_run``           -- Wave 0 legacy shim
 *  - ``ra_gpt_stop``                      -- Wave 0 legacy
 *  - ``ra_gpt_read``                      -- Wave 0 legacy
 *  - ``ra_gpt_set_period``                -- runtime GTPR/GTPBR change
 *  - ``ra_gpt_set_duty``                  -- runtime GTCCR[A/B] change
 *  - ``ra_gpt_get_status / clear_status`` -- GTST overflow/underflow
 *  - ``ra_gpt_attach_handler``            -- IRQ callback
 *  - ``ra_gpt_enter_stop / exit_stop``    -- power transition
 *  - ``ra_gpt_dispatch_ovf / und / ccra / ccrb`` -- ISR entry points
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/* =============================================================================
 * Types
 * =============================================================================
 */

/**
 * @enum ra_gpt_mode_t
 * @brief GPT counter-mode selection (GTCR.MD field).
 */
typedef enum : uint8_t {
  k_ra_gpt_mode_saw_pwm       = 0U, /**< Saw-wave PWM (up-count).     */
  k_ra_gpt_mode_saw_one_shot  = 1U, /**< Saw-wave one-shot.           */
  k_ra_gpt_mode_triangle_pwm  = 4U, /**< Triangle-wave PWM symmetric. */
  k_ra_gpt_mode_triangle_pwm2 = 5U, /**< Triangle-wave PWM 1.         */
  k_ra_gpt_mode_triangle_pwm3 = 6U, /**< Triangle-wave PWM 2.         */
} ra_gpt_mode_t;

/**
 * @enum ra_gpt_prescaler_t
 * @brief GTCR.TPCS clock prescaler.
 */
typedef enum : uint8_t {
  k_ra_gpt_ps_div_1    = 0U, /**< PCLKD / 1.      */
  k_ra_gpt_ps_div_4    = 1U, /**< PCLKD / 4.      */
  k_ra_gpt_ps_div_16   = 2U, /**< PCLKD / 16.     */
  k_ra_gpt_ps_div_64   = 3U, /**< PCLKD / 64.     */
  k_ra_gpt_ps_div_256  = 4U, /**< PCLKD / 256.    */
  k_ra_gpt_ps_div_1024 = 5U, /**< PCLKD / 1024.  */
} ra_gpt_prescaler_t;

/**
 * @struct ra_gpt_cfg_t
 * @brief Configuration descriptor for ``ra_gpt_init``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra_gpt_init`` in
 * ``libs/ra_hal/src/ra_gpt.c``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_gpt_mode_t      mode;       /**< Counter mode.                    */
  ra_gpt_prescaler_t prescaler;  /**< Clock divider.                   */
  uint32_t           period;     /**< GTPR period.                     */
  uint32_t           duty_a;     /**< GTCCRA compare/duty (PWM A).    */
  uint32_t           duty_b;     /**< GTCCRB compare/duty (PWM B).    */
  bool               auto_start; /**< True -> start after init.       */
} ra_gpt_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @enum ra_gpt_status_mask_t
 * @brief Bit mask of GPT status flags (GTST register subset).
 */
typedef enum : uint32_t {
  k_ra_gpt_status_none      = 0x00000000UL,
  k_ra_gpt_status_overflow  = 0x00000002UL, /**< GTST.TCFPO. */
  k_ra_gpt_status_underflow = 0x00000001UL, /**< GTST.TCFPU. */
  k_ra_gpt_status_ccra      = 0x00000010UL, /**< GTST.TCFA.  */
  k_ra_gpt_status_ccrb      = 0x00000020UL, /**< GTST.TCFB.  */
} ra_gpt_status_mask_t;

/**
 * @enum ra_gpt_ccr_sel_t
 * @brief Selector for ``ra_gpt_set_duty``.
 */
typedef enum : uint8_t {
  k_ra_gpt_ccr_a = 0U, /**< GTCCRA.    */
  k_ra_gpt_ccr_b = 1U, /**< GTCCRB.    */
} ra_gpt_ccr_sel_t;

/**
 * @typedef ra_gpt_event_fn_t
 * @brief GPT IRQ event callback signature.
 *
 * @param[in] ctx         Caller-supplied context.
 * @param[in] status_mask OR of ``k_ra_gpt_status_*`` bits.
 */
typedef void (*ra_gpt_event_fn_t)(void* ctx, uint32_t status_mask);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise a GPT channel with a full config descriptor.
 *
 * @param[in] channel GPT channel (0..13).
 * @param[in] cfg     Non-NULL configuration descriptor.
 * @return ``ra_err_t`` error code.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra_mstp_init`` has been called.
 * @post On success, GTCR + GTPR + GTCCR are programmed and
 *       (if ``cfg->auto_start``) GTSTR is set.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_gpt_init(uint8_t channel, const ra_gpt_cfg_t* cfg);

/**
 * @brief Tear down a channel (stop + MSTP release).
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_gpt_deinit(uint8_t channel);

/* =============================================================================
 * Legacy polling API
 * =============================================================================
 */

/**
 * @brief Configure a GPT channel as a free-running up-counter.
 *
 * @param[in] channel GPT channel (0..13).
 * @param[in] period  Period in timer ticks (write to GTPR).
 * @return `ra_err_t` error code.
 *
 * @note The counter runs from PCLKD (or a configurable source) with
 *       no prescaler. The caller is responsible for choosing a
 *       sensible period value given the current PCLKD.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_gpt_start_free_run(uint8_t channel, uint32_t period);

/**
 * @brief Stop a GPT channel.
 *
 * @param[in] channel GPT channel (0..13).
 * @return `ra_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_gpt_stop(uint8_t channel);

/**
 * @brief Read the current counter value.
 *
 * @param[in]  channel GPT channel (0..13).
 * @param[out] out     Receive counter value.
 * @return `ra_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_gpt_read(uint8_t channel, uint32_t* out);

/* =============================================================================
 * Runtime reconfigure
 * =============================================================================
 */

/**
 * @brief Change the GPT period (GTPR / GTPBR) at runtime.
 * @param[in] channel GPT channel.
 * @param[in] period  New GTPR value.
 * @return ``ra_err_t`` error code.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_gpt_set_period(uint8_t channel, uint32_t period);

/**
 * @brief Change one compare register (GTCCRA / GTCCRB) at runtime.
 * @param[in] channel GPT channel.
 * @param[in] which   Which compare register.
 * @param[in] value   New compare value.
 * @return ``ra_err_t`` error code.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_gpt_set_duty(uint8_t channel, ra_gpt_ccr_sel_t which, uint32_t value);

/* =============================================================================
 * Status
 * =============================================================================
 */

/**
 * @brief Read GTST overflow / underflow / compare flags.
 * @param[in]  channel  GPT channel.
 * @param[out] out_mask OR of ``k_ra_gpt_status_*``.
 * @return ``ra_err_t`` error code.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_gpt_get_status(uint8_t channel, uint32_t* out_mask);

/**
 * @brief Clear GTST flag bits (write-0 to clear).
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_gpt_clear_status(uint8_t channel, uint32_t mask);

/* =============================================================================
 * Interrupt path
 * =============================================================================
 */

/**
 * @brief Attach a GPT event callback for a channel.
 * @param[in] channel GPT channel.
 * @param[in] fn      Callback fired on ISR dispatch.
 * @param[in] ctx     Context passed to the callback.
 * @return ``ra_err_t`` error code.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_gpt_attach_handler(uint8_t channel, ra_gpt_event_fn_t fn, void* ctx);

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @brief Put a channel into MSTP-gated stop.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_gpt_enter_stop(uint8_t channel);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_gpt_exit_stop(uint8_t channel);

/* =============================================================================
 * ISR dispatch
 * =============================================================================
 */

/**
 * @brief Dispatch GPT overflow (GTCIV) -- clear + fire callback.
 * @param[in] channel GPT channel.
 * @since 0.2.0
 */
void ra_gpt_dispatch_ovf(uint8_t channel);

/**
 * @brief Dispatch GPT underflow (GTCIU) -- clear + fire callback.
 * @param[in] channel GPT channel.
 * @since 0.2.0
 */
void ra_gpt_dispatch_und(uint8_t channel);

/**
 * @brief Dispatch GPT compare-match A (GTCIA) -- clear + fire callback.
 * @param[in] channel GPT channel.
 * @since 0.2.0
 */
void ra_gpt_dispatch_ccra(uint8_t channel);

/**
 * @brief Dispatch GPT compare-match B (GTCIB) -- clear + fire callback.
 * @param[in] channel GPT channel.
 * @since 0.2.0
 */
void ra_gpt_dispatch_ccrb(uint8_t channel);

#ifdef __cplusplus
}
#endif
