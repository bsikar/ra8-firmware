/**
 * @file ra8_display_pal_backlight.h
 * @brief Hardware-agnostic display backlight controller and idle dimming policy.
 * @ingroup grp_graphics
 *
 * @details
 * Splits backlight control into two cooperative pieces:
 *   1. ``display_backlight_policy_t`` -- pure ``ACTIVE -> DIMMED -> OFF`` idle
 *      state machine driven by monotonic millisecond timestamps and explicit
 *      user-activity notifications. Hardware with continuous dimming sets
 *      ``supports_dim = 1``; binary GPIO panels (such as EK-RA8D2 J1 P514) set
 *      ``supports_dim = 0`` so the policy transitions cleanly ``ACTIVE -> OFF``.
 *   2. ``display_backlight_t`` -- driver frontend operating either a binary
 *      enable callback or an ``fw_pwm_t`` PWM output with Q16 duty scaling and
 *      optional enable gate, failing closed on unsupported intermediate steps.
 *
 * All storage is caller-owned; there is no heap allocation and no global state.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "fw_if_pwm.h"
#include "ra8_err.h"

/**
 * @enum display_backlight_mode_t
 * @brief Physical actuation mode bound to a ::display_backlight_t handle.
 */
typedef enum : uint8_t {
  k_display_backlight_mode_binary = 0U, /**< GPIO enable callback only (0 or 100%). */
  k_display_backlight_mode_pwm    = 1U, /**< Continuous PWM via fw_pwm_t (+ optional gate). */
} display_backlight_mode_t;

/**
 * @enum display_backlight_state_t
 * @brief Power-saving idle states tracked by ::display_backlight_policy_t.
 */
typedef enum : uint8_t {
  k_display_backlight_state_active = 0U, /**< Full configured active brightness. */
  k_display_backlight_state_dimmed = 1U, /**< Reduced idle brightness (PWM panels). */
  k_display_backlight_state_off    = 2U, /**< Backlight blanked after off timeout. */
} display_backlight_state_t;

/** @brief Default idle timeouts matching e-reader 30s auto-off contract. */
enum : uint32_t {
  k_display_backlight_default_dim_ms = 15000U, /**< Default PWM idle dim threshold. */
  k_display_backlight_default_off_ms = 30000U, /**< Default idle blank threshold. */
};

/** @brief Default active/dimmed percentage levels. */
enum : uint8_t {
  k_display_backlight_default_active_pct = 100U, /**< Default active brightness. */
  k_display_backlight_default_dimmed_pct = 25U,  /**< Default dimmed brightness. */
};

/**
 * @brief Binary enable callback used by ::display_backlight_cfg_t.
 *
 * @param[in] ctx Caller context pointer from ::display_backlight_cfg_t.
 * @param[in] on  1 to enable backlight, 0 to disable.
 *
 * @return ``k_ra8_ok`` on success or hardware error code.
 */
typedef ra8_err_t (*display_backlight_enable_fn_t)(void* ctx, uint8_t on);

/**
 * @struct display_backlight_cfg_t
 * @brief Hardware binding configuration passed to ::display_backlight_init.
 */
typedef struct display_backlight_cfg_s {
  const fw_pwm_t*               pwm;        /**< Bound fw_pwm_t when mode == pwm. */
  display_backlight_enable_fn_t set_enable; /**< Required in binary; optional gate in pwm. */
  void*                         ctx;        /**< Borrowed caller context passed to set_enable. */
  uint32_t                      pwm_period_ticks; /**< Period ticks passed to fw_pwm open (pwm). */
  display_backlight_mode_t      mode;             /**< Binary or PWM actuation mode. */
  fw_pwm_ch_t                   pwm_ch;           /**< Board PWM channel index (pwm mode). */
  fw_pwm_polarity_t             pwm_polarity;     /**< Active-high or active-low PWM polarity. */
  uint8_t                       reserved;         /**< Must be zero. */
} display_backlight_cfg_t;

/**
 * @struct display_backlight_t
 * @brief Caller-owned backlight controller instance.
 */
typedef struct display_backlight_s {
  display_backlight_cfg_t cfg;            /**< Latched configuration copy. */
  uint8_t                 initialized;    /**< 1 once display_backlight_init succeeds. */
  uint8_t                 enabled;        /**< Current gate enable state (0/1). */
  uint8_t                 pwm_running;    /**< 1 while fw_pwm output is started. */
  uint8_t                 brightness_pct; /**< Last applied brightness percent (0..100). */
} display_backlight_t;

/**
 * @struct display_backlight_policy_cfg_t
 * @brief Timing and level thresholds passed to ::display_backlight_policy_init.
 */
typedef struct display_backlight_policy_cfg_s {
  uint32_t dim_after_ms; /**< Idle threshold before DIMMED; 0 skips DIMMED. */
  uint32_t off_after_ms; /**< Idle threshold before OFF; must exceed 0. */
  uint8_t  active_pct;   /**< Active brightness (1..100; 100 required when supports_dim == 0). */
  uint8_t  dimmed_pct;   /**< Dimmed brightness (1..active_pct when DIMMED reachable). */
  uint8_t  supports_dim; /**< 1 if hardware supports continuous dimming, 0 for binary only. */
  uint8_t  reserved;     /**< Must be zero. */
} display_backlight_policy_cfg_t;

/**
 * @struct display_backlight_policy_t
 * @brief Caller-owned 3-state idle backlight policy state machine.
 */
typedef struct display_backlight_policy_s {
  display_backlight_policy_cfg_t cfg;              /**< Validated policy thresholds. */
  uint32_t                       last_activity_ms; /**< Timestamp of last wake/activity event. */
  display_backlight_state_t      state;            /**< Current ACTIVE/DIMMED/OFF state. */
  uint8_t                        current_pct;      /**< Target percent matching current state. */
  uint8_t                        initialized;      /**< 1 once policy_init succeeds. */
  uint8_t                        reserved;         /**< Zero padding. */
} display_backlight_policy_t;

/**
 * @struct display_backlight_policy_decision_t
 * @brief Decision emitted by ::display_backlight_policy_activity and ::display_backlight_policy_step.
 */
typedef struct display_backlight_policy_decision_s {
  display_backlight_state_t state;      /**< Resulting backlight state. */
  uint8_t                   target_pct; /**< Resulting brightness percentage (0..100). */
  uint8_t                   changed;    /**< 1 if state or target_pct changed on this call. */
  uint8_t                   on;         /**< 1 if target_pct > 0, 0 when OFF. */
} display_backlight_policy_decision_t;

/**
 * @brief Convert brightness percentage (0..100) to an fw_pwm Q16 duty value (0..K_FW_PWM_DUTY_FULL).
 *
 * @param[in]  percent Requested brightness percentage (0..100).
 * @param[out] out_q16 Converted Q16 duty cycle.
 *
 * @retval k_ra8_ok              Converted duty written.
 * @retval k_ra8_err_null_ptr    @p out_q16 is nullptr.
 * @retval k_ra8_err_invalid_arg @p percent > 100.
 *
 * @par Calling context: task-safe, reentrant (pure stateless conversion).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t display_backlight_percent_to_q16(uint8_t percent, uint32_t* out_q16);

/**
 * @brief Bind and initialize a backlight controller instance at 0% (OFF).
 *
 * @param[out] dev Caller-owned handle storage.
 * @param[in]  cfg Mode and hardware binding configuration.
 *
 * @retval k_ra8_ok                Initialized in dark (0%) state.
 * @retval k_ra8_err_null_ptr      @p dev, @p cfg, or required callback/handle is nullptr.
 * @retval k_ra8_err_invalid_arg   Invalid mode, zero period, or bad polarity/channel.
 * @retval k_ra8_err_not_initialized Bound @p cfg->pwm handle is unbound.
 * @retval k_ra8_err_not_supported Requested active-low polarity not supported by PWM backend.
 *
 * @par Calling context: task-only, non-reentrant (caller owns serialization on @p dev).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t display_backlight_init(display_backlight_t*           dev,
                                               const display_backlight_cfg_t* cfg);

/**
 * @brief Blank the backlight and release any claimed PWM output.
 *
 * @param[in,out] dev Initialized controller handle.
 *
 * @retval k_ra8_ok                  Backlight blanked and handle marked uninitialized.
 * @retval k_ra8_err_null_ptr        @p dev is nullptr.
 * @retval k_ra8_err_not_initialized @p dev is not initialized.
 *
 * @par Calling context: task-only, non-reentrant (caller owns serialization on @p dev).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t display_backlight_deinit(display_backlight_t* dev);

/**
 * @brief Apply a brightness percentage to an initialized backlight controller.
 *
 * @details
 * Binary mode supports only 0 and 100 and fails closed with
 * ``k_ra8_err_not_supported`` for 1..99 without mutating the output. PWM mode
 * scales 0..100 to Q16 duty, stops the PWM channel on 0, and enables/starts on 1..100.
 *
 * @param[in,out] dev     Initialized controller handle.
 * @param[in]     percent Requested brightness (0..100).
 *
 * @retval k_ra8_ok                  Hardware output updated.
 * @retval k_ra8_err_null_ptr        @p dev is nullptr.
 * @retval k_ra8_err_not_initialized @p dev (or its PWM binding) is uninitialized.
 * @retval k_ra8_err_not_supported   Intermediate brightness (1..99) in binary mode.
 * @retval k_ra8_err_invalid_arg     @p percent > 100.
 *
 * @par Calling context: task-only, non-reentrant (caller owns serialization on @p dev).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t display_backlight_set_brightness(display_backlight_t* dev, uint8_t percent);

/**
 * @brief Apply a ::display_backlight_policy_decision_t to a controller when @p decision->changed != 0.
 *
 * @param[in,out] dev      Initialized controller handle.
 * @param[in]     decision Decision emitted by policy step/activity.
 *
 * @retval k_ra8_ok                  Applied change or no-op when changed == 0.
 * @retval k_ra8_err_null_ptr        @p dev or @p decision is nullptr.
 * @retval k_ra8_err_not_initialized @p dev is not initialized.
 * @retval k_ra8_err_invalid_arg     Out-of-range @p decision fields.
 *
 * @par Calling context: task-only, non-reentrant (caller owns serialization on @p dev).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
display_backlight_apply_decision(display_backlight_t*                       dev,
                                 const display_backlight_policy_decision_t* decision);

/**
 * @brief Initialize a caller-owned 3-state idle backlight policy in ACTIVE state.
 *
 * @param[out] policy Caller-owned policy state.
 * @param[in]  cfg    Thresholds and dimming support capability.
 * @param[in]  now_ms Initial monotonic timestamp in milliseconds.
 *
 * @retval k_ra8_ok                Policy active with last_activity_ms == @p now_ms.
 * @retval k_ra8_err_null_ptr      @p policy or @p cfg is nullptr.
 * @retval k_ra8_err_invalid_arg   Zero off_after_ms, dim_after_ms > off_after_ms, or invalid pct.
 * @retval k_ra8_err_not_supported Binary policy requested active_pct != 100.
 *
 * @par Calling context: task-only, non-reentrant (caller owns serialization on @p policy).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t display_backlight_policy_init(display_backlight_policy_t*           policy,
                                                      const display_backlight_policy_cfg_t* cfg,
                                                      uint32_t                              now_ms);

/**
 * @brief Record user/display activity, resetting the idle timer and waking to ACTIVE.
 *
 * @param[in,out] policy Initialized policy instance.
 * @param[in]     now_ms Current monotonic timestamp in milliseconds.
 * @param[out]    out    Resulting decision (changed == 1 if waking from DIMMED/OFF).
 *
 * @retval k_ra8_ok                  Activity recorded and @p out populated.
 * @retval k_ra8_err_null_ptr        @p policy or @p out is nullptr.
 * @retval k_ra8_err_not_initialized @p policy is not initialized.
 *
 * @par Calling context: task-only, non-reentrant (caller owns serialization on @p policy).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t display_backlight_policy_activity(display_backlight_policy_t* policy,
                                                          uint32_t                    now_ms,
                                                          display_backlight_policy_decision_t* out);

/**
 * @brief Advance the idle timeout state machine using wrap-safe elapsed time.
 *
 * @param[in,out] policy Initialized policy instance.
 * @param[in]     now_ms Current monotonic timestamp in milliseconds.
 * @param[out]    out    Resulting decision (changed == 1 on ACTIVE->DIMMED or *->OFF).
 *
 * @retval k_ra8_ok                  Policy stepped and @p out populated.
 * @retval k_ra8_err_null_ptr        @p policy or @p out is nullptr.
 * @retval k_ra8_err_not_initialized @p policy is not initialized.
 *
 * @par Calling context: task-only, non-reentrant (caller owns serialization on @p policy).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t display_backlight_policy_step(display_backlight_policy_t*          policy,
                                                      uint32_t                             now_ms,
                                                      display_backlight_policy_decision_t* out);

#ifdef __cplusplus
}
#endif
