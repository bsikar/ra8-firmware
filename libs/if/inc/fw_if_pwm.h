/**
 * @file fw_if_pwm.h
 * @brief Architecture-neutral PWM port: a period and a duty ratio on an output,
 * with counting and capture deliberately left to the timer port.
 * @ingroup grp_fw_pwm
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details
 * The second half of step 5 in `docs/PORTS.md`. `fw_if_timer.h` explains why
 * timer and PWM are two ports even though one GPT channel serves both on this
 * chip; this header is the half that drives a pin.
 *
 * The defining choice is how duty is expressed. A chip register holds duty as
 * a compare count, which only means something next to the period it was
 * computed against. Retune the period and every stored count silently becomes
 * a different ratio, which is exactly the "same duty, new frequency" operation
 * a motor or backlight driver performs most. So this port carries duty as a
 * ratio, ::fw_pwm_duty_t in Q16: 0 is always low, ::K_FW_PWM_DUTY_FULL
 * (65536) is always high, and the binding scales to counts against whatever
 * period is current. A caller changing frequency calls ::fw_pwm_set_period and
 * its duty stays the duty it asked for.
 *
 * Q16 rather than percent or permille because the GPT counters are 32-bit: a
 * permille duty throws away resolution a caller paid for in clock rate, while
 * 16 fractional bits are finer than any period this chip can usefully run.
 * The range is inclusive of 65536 so that 100% is expressible exactly, which a
 * uint16_t fraction cannot do.
 *
 * Polarity is fixed at open, not per duty write, because flipping it mid-run
 * is a glitch on the pin, not a configuration change. A backend that cannot
 * invert says so in its caps and is refused at the facade.
 *
 * Per the taxonomy in `docs/PORTS.md`, this uses (a) the caller-allocated
 * facade plus (b) a narrow ops struct the binding fills, the same shape as the
 * timer and clock ports. Handles are caller-owned; no allocation, and no chip,
 * operating-system or device header.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @brief Duty as a Q16 ratio of the period, 0 to ::K_FW_PWM_DUTY_FULL.
 *
 * @details Wider than 16 bits so the full-on value is representable exactly.
 */
typedef uint32_t fw_pwm_duty_t;

/** @brief Duty that holds the output at its active level for the whole period. */
#define K_FW_PWM_DUTY_FULL 65536U

/**
 * @enum fw_pwm_polarity_t
 * @brief Which level counts as "on" for the duty ratio.
 */
typedef enum : uint8_t {
  k_fw_pwm_pol_none        = 0U, /**< Not a polarity; rejected.    */
  k_fw_pwm_pol_active_high = 1U, /**< Duty is the time spent high. */
  k_fw_pwm_pol_active_low  = 2U, /**< Duty is the time spent low.  */
} fw_pwm_polarity_t;

/** @brief One past the last valid ::fw_pwm_polarity_t enumerator. */
#define K_FW_PWM_POL_COUNT 3U

/**
 * @struct fw_pwm_ch_t
 * @brief Which output a request is about.
 *
 * @details Board numbering, zero-based and dense, exactly as ::fw_timer_ch_t:
 *          a board routing GPT2 A and GPT5 B to its headers presents outputs 0
 *          and 1, and its binding maps them.
 */
typedef struct fw_pwm_ch_s {
  uint8_t index; /**< Board output, zero-based. */
} fw_pwm_ch_t;

/**
 * @struct fw_pwm_caps_t
 * @brief What a particular PWM backend can actually do.
 *
 * @details
 * `period_max` is the largest period in counts the backend accepts, for the
 * same reason ::fw_timer_caps_t carries `counter_max`: a period that does not
 * fit truncates into a different frequency instead of failing.
 * `has_active_low` says whether the output can be inverted.
 */
typedef struct fw_pwm_caps_s {
  uint8_t  channel_count;  /**< Board outputs available, may be zero.   */
  uint8_t  counter_bits;   /**< Counter width in bits, typically 16/32. */
  uint32_t period_max;     /**< Largest period in counts accepted.      */
  bool     has_active_low; /**< Whether active-low polarity works.      */
} fw_pwm_caps_t;

/**
 * @struct fw_pwm_iface_t
 * @brief The narrow ops struct a chip-and-board binding fills.
 *
 * @details
 * Context first on every op, every pointer required; ::fw_pwm_bind rejects a
 * NULL op as a malformed binding. `set_duty` receives the Q16 ratio, already
 * range-checked, and owns the scaling to counts against its current period.
 */
typedef struct fw_pwm_iface_s {
  /** @brief Report what this backend can do. */
  ra8_err_t (*get_caps)(void* ctx, fw_pwm_caps_t* out);
  /** @brief Claim an output at @p period counts with @p pol, duty zero. */
  ra8_err_t (*open)(void* ctx, fw_pwm_ch_t ch, uint32_t period, fw_pwm_polarity_t pol);
  /** @brief Release an output claimed by `open`, leaving it inactive. */
  ra8_err_t (*close)(void* ctx, fw_pwm_ch_t ch);
  /** @brief Begin driving the output. */
  ra8_err_t (*start)(void* ctx, fw_pwm_ch_t ch);
  /** @brief Stop driving, holding the inactive level. */
  ra8_err_t (*stop)(void* ctx, fw_pwm_ch_t ch);
  /** @brief Change the period, keeping the duty ratio. */
  ra8_err_t (*set_period)(void* ctx, fw_pwm_ch_t ch, uint32_t period);
  /** @brief Change the duty ratio, keeping the period. */
  ra8_err_t (*set_duty)(void* ctx, fw_pwm_ch_t ch, fw_pwm_duty_t duty);
} fw_pwm_iface_t;

/**
 * @struct fw_pwm_t
 * @brief Caller-owned binding handle.
 *
 * @details Populated only by ::fw_pwm_bind. Caps are snapshotted there, and
 *          every entry point checks `bound` so a zeroed handle is refused
 *          rather than jumped through.
 */
typedef struct fw_pwm_s {
  const fw_pwm_iface_t* iface; /**< Binding ops, never NULL once bound. */
  void*                 ctx;   /**< Binding context, may be NULL.       */
  fw_pwm_caps_t         caps;  /**< Snapshot taken by ::fw_pwm_bind.    */
  bool                  bound; /**< Set by ::fw_pwm_bind only.          */
} fw_pwm_t;

/**
 * @brief Bind a handle to a chip-and-board PWM binding.
 *
 * @param[out] pwm   Caller-owned storage, contents ignored on entry.
 * @param[in]  iface Ops struct; borrowed, must outlive @p pwm.
 * @param[in]  ctx   Binding context, handed back to every op. May be NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Handle is bound and carries the caps.
 * @retval k_ra8_err_invalid_arg   @p pwm or @p iface NULL, or an op unset.
 * @retval k_ra8_err_invalid_state Backend reported a zero width or period_max.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fw_pwm_bind(fw_pwm_t* pwm, const fw_pwm_iface_t* iface, void* ctx);

/**
 * @brief Copy out what this backend can do.
 *
 * @param[in]  pwm Bound handle.
 * @param[out] out Caps on success; zeroed on any failure.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Caps written.
 * @retval k_ra8_err_invalid_arg     @p pwm or @p out NULL.
 * @retval k_ra8_err_not_initialized Handle not bound.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fw_pwm_get_caps(const fw_pwm_t* pwm, fw_pwm_caps_t* out);

/**
 * @brief Claim an output at a period and polarity, duty starting at zero.
 *
 * @param[in] pwm    Bound handle.
 * @param[in] ch     Which board output.
 * @param[in] period Period in counts; zero is rejected.
 * @param[in] pol    Which level duty measures.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Output claimed.
 * @retval k_ra8_err_invalid_arg     Zero period or unenumerated polarity.
 * @retval k_ra8_err_out_of_range    Period above ::fw_pwm_caps_t::period_max.
 * @retval k_ra8_err_not_found       No such output on this board.
 * @retval k_ra8_err_not_supported   Active-low on a backend that cannot invert.
 * @retval k_ra8_err_not_initialized Handle not bound.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_pwm_open(const fw_pwm_t* pwm, fw_pwm_ch_t ch, uint32_t period, fw_pwm_polarity_t pol);

/**
 * @brief Release an output.
 *
 * @param[in] pwm Bound handle.
 * @param[in] ch  Which board output.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok            Output released.
 * @retval k_ra8_err_not_found No such output on this board.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fw_pwm_close(const fw_pwm_t* pwm, fw_pwm_ch_t ch);

/**
 * @brief Begin driving an open output.
 *
 * @param[in] pwm Bound handle.
 * @param[in] ch  Which board output.
 *
 * @return ra8_err_t As ::fw_pwm_close.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fw_pwm_start(const fw_pwm_t* pwm, fw_pwm_ch_t ch);

/**
 * @brief Stop driving, holding the inactive level.
 *
 * @param[in] pwm Bound handle.
 * @param[in] ch  Which board output.
 *
 * @return ra8_err_t As ::fw_pwm_close.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fw_pwm_stop(const fw_pwm_t* pwm, fw_pwm_ch_t ch);

/**
 * @brief Change the period of an open output; the duty ratio is kept.
 *
 * @param[in] pwm    Bound handle.
 * @param[in] ch     Which board output.
 * @param[in] period New period in counts; zero is rejected.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Period accepted.
 * @retval k_ra8_err_invalid_arg  Zero period.
 * @retval k_ra8_err_out_of_range Period above ::fw_pwm_caps_t::period_max.
 * @retval k_ra8_err_not_found    No such output on this board.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fw_pwm_set_period(const fw_pwm_t* pwm, fw_pwm_ch_t ch, uint32_t period);

/**
 * @brief Change the duty ratio of an open output; the period is kept.
 *
 * @param[in] pwm  Bound handle.
 * @param[in] ch   Which board output.
 * @param[in] duty Q16 ratio, 0 to ::K_FW_PWM_DUTY_FULL inclusive.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Duty accepted.
 * @retval k_ra8_err_out_of_range Duty above ::K_FW_PWM_DUTY_FULL.
 * @retval k_ra8_err_not_found    No such output on this board.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fw_pwm_set_duty(const fw_pwm_t* pwm, fw_pwm_ch_t ch, fw_pwm_duty_t duty);

#ifdef __cplusplus
}
#endif
