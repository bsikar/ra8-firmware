/**
 * @file fw_if_timer.h
 * @brief Architecture-neutral timer port: counting, periods and input capture,
 * with duty output deliberately left out.
 * @ingroup grp_fw_timer
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details
 * `docs/PORTS.md` puts timer and PWM at step 5 of the build order and says to
 * split them apart. This header is the timer half. The split is the whole
 * point, so it is worth saying why rather than leaving it as a filing
 * decision.
 *
 * On this chip one block does both jobs: a GPT channel counts, and the same
 * channel's compare registers drive a pin. Because the hardware fuses them,
 * every consumer inherited both vocabularies at once, and the result is
 * visible in the tree: an application that only wants to measure an interval
 * still opens a configuration carrying output polarity, stop level and
 * dead-time fields it must leave at their defaults and hope nobody reads as
 * intent. Worse, the two have different notions of what a period means. A
 * counter's period is where it wraps; a PWM's period is the denominator of a
 * duty ratio, and the useful operations on it are "same frequency, different
 * duty", which is meaningless to a counter.
 *
 * So they are two ports. A driver measuring an interval binds this one and
 * cannot reach a pin; a motor controller binds the PWM port and gets duty
 * semantics that mean something. A chip adapter is free to serve both from one
 * block, which is exactly what the RA8 adapter will do.
 *
 * What stays here: counting up to a period, free-running or one-shot, reading
 * the count, retuning the period, and latching the count on an external edge.
 * What leaves: anything that drives a pin.
 *
 * Counters are not interchangeable, which is why this port has caps. The GPT
 * channels on this chip are 32-bit and the AGT channels are 16-bit, so a
 * period a caller computed for one silently truncates on the other and the
 * failure shows up as a wrong interval, never as an error. ::fw_timer_open
 * therefore checks the period against the backend's own counter width before
 * the backend sees it.
 *
 * Per the taxonomy in `docs/PORTS.md`, this uses (a) the caller-allocated
 * facade plus (b) a narrow ops struct the binding fills. It does not use (c);
 * no Ring-3 driver needs a bridged subset today.
 *
 * Handles are caller-owned. This interface performs no allocation and contains
 * no chip, operating-system or device header.
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
 * @enum fw_timer_mode_t
 * @brief What a channel is being opened to do.
 *
 * @details
 * Three modes, each a different answer to "what happens when the count
 * reaches the period". Free-run wraps and keeps going, which is what a
 * monotonic interval measurement wants. One-shot stops, which is what a
 * timeout wants. Capture latches the count when an external edge arrives,
 * which is what a pulse-width or frequency measurement wants, and is the one
 * mode a backend may genuinely lack.
 *
 * There is no PWM mode here on purpose; see the file's rationale.
 */
typedef enum : uint8_t {
  k_fw_timer_mode_none     = 0U, /**< Not a mode; rejected.                 */
  k_fw_timer_mode_free_run = 1U, /**< Wrap at the period and keep counting. */
  k_fw_timer_mode_one_shot = 2U, /**< Stop on reaching the period.          */
  k_fw_timer_mode_capture  = 3U, /**< Latch the count on an external edge.  */
} fw_timer_mode_t;

/** @brief One past the last valid ::fw_timer_mode_t enumerator. */
#define K_FW_TIMER_MODE_COUNT 4U

/**
 * @struct fw_timer_ch_t
 * @brief Which channel a request is about.
 *
 * @details
 * `index` is the instance number as the *board* numbers them, zero-based and
 * dense, not a chip channel number. A board exposing GPT3 and GPT7 on headers
 * presents timers 0 and 1, and its binding maps them. That indirection is
 * what lets an application move between boards without renumbering.
 */
typedef struct fw_timer_ch_s {
  uint8_t index; /**< Board instance, zero-based. */
} fw_timer_ch_t;

/**
 * @struct fw_timer_caps_t
 * @brief What a particular timer backend can actually do.
 *
 * @details
 * Required by acceptance criterion 3 in `docs/PORTS.md`, and not a formality:
 * the two timer blocks on this chip differ in the one field a caller cannot
 * discover by trying. `counter_max` is the largest period the backend accepts,
 * so a caller computing counts from a clock rate can clamp or refuse instead
 * of handing over a value that wraps. A 16-bit counter reports 65535 here.
 *
 * `has_capture` and `has_one_shot` describe modes, and a backend lacking one
 * says so here *and* refuses the mode from ::fw_timer_open. Both, because a
 * caller that asks is told honestly and a caller that does not ask still
 * cannot get silent nonsense.
 */
typedef struct fw_timer_caps_s {
  uint8_t  channel_count; /**< Board instances available, may be zero.    */
  uint8_t  counter_bits;  /**< Counter width in bits, typically 16 or 32. */
  uint32_t counter_max;   /**< Largest period the backend accepts.        */
  bool     has_capture;   /**< Whether ::k_fw_timer_mode_capture works.   */
  bool     has_one_shot;  /**< Whether ::k_fw_timer_mode_one_shot works.  */
} fw_timer_caps_t;

/**
 * @struct fw_timer_iface_t
 * @brief The narrow ops struct a chip-and-board binding fills.
 *
 * @details
 * Each op takes the binding's own context first, so one binding can serve
 * several handles without file-scope state. Every pointer must be set: a NULL
 * op is a malformed binding, not a declined capability, and ::fw_timer_bind
 * rejects it. A backend without input capture fills `capture_read` with a
 * function returning ::k_ra8_err_not_supported and reports
 * ::fw_timer_caps_t::has_capture false.
 */
typedef struct fw_timer_iface_s {
  /** @brief Report what this backend can do. */
  ra8_err_t (*get_caps)(void *ctx, fw_timer_caps_t *out);
  /** @brief Claim a channel in @p mode with wrap point @p period. */
  ra8_err_t (*open)(void *ctx, fw_timer_ch_t ch, fw_timer_mode_t mode, uint32_t period);
  /** @brief Release a channel claimed by `open`. */
  ra8_err_t (*close)(void *ctx, fw_timer_ch_t ch);
  /** @brief Begin counting on an open channel. */
  ra8_err_t (*start)(void *ctx, fw_timer_ch_t ch);
  /** @brief Halt counting without releasing the channel. */
  ra8_err_t (*stop)(void *ctx, fw_timer_ch_t ch);
  /** @brief Current count. */
  ra8_err_t (*read)(void *ctx, fw_timer_ch_t ch, uint32_t *out_counts);
  /** @brief Change the wrap point of an open channel. */
  ra8_err_t (*set_period)(void *ctx, fw_timer_ch_t ch, uint32_t period);
  /** @brief Count latched by the most recent external edge. */
  ra8_err_t (*capture_read)(void *ctx, fw_timer_ch_t ch, uint32_t *out_counts);
} fw_timer_iface_t;

/**
 * @struct fw_timer_t
 * @brief Caller-owned binding handle.
 *
 * @details
 * Populated only by ::fw_timer_bind and opaque afterwards. The `caps` copy is
 * taken at bind time so every later call can range-check against it without a
 * round trip through the binding, and so a backend cannot change its own
 * limits underneath a caller mid-flight. The `bound` flag is what every entry
 * point checks: a zeroed handle gets ::k_ra8_err_not_initialized rather than a
 * jump through an uninitialised function pointer.
 */
typedef struct fw_timer_s {
  const fw_timer_iface_t *iface; /**< Binding ops, never NULL once bound. */
  void                   *ctx;   /**< Binding context, may be NULL.       */
  fw_timer_caps_t         caps;  /**< Snapshot taken by ::fw_timer_bind.  */
  bool                    bound; /**< Set by ::fw_timer_bind only.        */
} fw_timer_t;

/**
 * @brief Bind a handle to a chip-and-board timer binding.
 *
 * @details
 * Calls `get_caps` once and keeps the answer. A backend reporting a zero
 * counter width, or a `counter_max` of zero, is refused here: both make every
 * later period check meaningless, and failing at bind names the buggy binding
 * instead of leaving a nonsense limit for a driver to trip over.
 *
 * @param[out] tmr   Caller-owned storage, contents ignored on entry.
 * @param[in]  iface Ops struct; borrowed, must outlive @p tmr.
 * @param[in]  ctx   Binding context, handed back to every op. May be NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Handle is bound and carries the caps.
 * @retval k_ra8_err_invalid_arg   @p tmr or @p iface NULL, or an op unset.
 * @retval k_ra8_err_invalid_state Backend reported unusable caps.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_timer_bind(fw_timer_t *tmr, const fw_timer_iface_t *iface, void *ctx);

/**
 * @brief Copy out what this backend can do.
 *
 * @param[in]  tmr Bound handle.
 * @param[out] out Caps on success; zeroed on any failure.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Caps written.
 * @retval k_ra8_err_invalid_arg     @p tmr or @p out NULL.
 * @retval k_ra8_err_not_initialized Handle not bound.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fw_timer_get_caps(const fw_timer_t *tmr, fw_timer_caps_t *out);

/**
 * @brief Claim a channel for a mode and wrap point.
 *
 * @details
 * Everything checkable is checked before the backend sees the request: the
 * mode is enumerated, the channel index is within the board's count, the
 * period is non-zero, and the period fits the counter. A mode the backend
 * declared absent is refused here too, so a caller gets the same answer
 * whether or not it read the caps first.
 *
 * @param[in] tmr    Bound handle.
 * @param[in] ch     Which board instance.
 * @param[in] mode   What the channel is for.
 * @param[in] period Wrap point in counts; zero is rejected.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Channel claimed.
 * @retval k_ra8_err_invalid_arg     Mode outside the enumeration, or a zero
 *                                   period.
 * @retval k_ra8_err_out_of_range    Period above ::fw_timer_caps_t::counter_max.
 * @retval k_ra8_err_not_found       No such channel on this board.
 * @retval k_ra8_err_not_supported   Backend lacks that mode.
 * @retval k_ra8_err_not_initialized Handle not bound.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_timer_open(const fw_timer_t *tmr, fw_timer_ch_t ch, fw_timer_mode_t mode, uint32_t period);

/**
 * @brief Release a channel.
 *
 * @param[in] tmr Bound handle.
 * @param[in] ch  Which board instance.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok            Channel released.
 * @retval k_ra8_err_not_found No such channel on this board.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fw_timer_close(const fw_timer_t *tmr, fw_timer_ch_t ch);

/**
 * @brief Begin counting.
 *
 * @param[in] tmr Bound handle.
 * @param[in] ch  Which board instance.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Counting.
 * @retval k_ra8_err_invalid_state Channel not open.
 * @retval k_ra8_err_not_found     No such channel on this board.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fw_timer_start(const fw_timer_t *tmr, fw_timer_ch_t ch);

/**
 * @brief Halt counting, leaving the channel open.
 *
 * @param[in] tmr Bound handle.
 * @param[in] ch  Which board instance.
 *
 * @return ra8_err_t As ::fw_timer_start.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fw_timer_stop(const fw_timer_t *tmr, fw_timer_ch_t ch);

/**
 * @brief Read the current count.
 *
 * @param[in]  tmr        Bound handle.
 * @param[in]  ch         Which board instance.
 * @param[out] out_counts Count on success; zero on any failure.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Count written.
 * @retval k_ra8_err_invalid_arg @p out_counts NULL.
 * @retval k_ra8_err_not_found   No such channel on this board.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_timer_read(const fw_timer_t *tmr, fw_timer_ch_t ch, uint32_t *out_counts);

/**
 * @brief Change an open channel's wrap point.
 *
 * @details Range-checked exactly as ::fw_timer_open checks it, because a
 *          retune can overflow a counter just as easily as an open can.
 *
 * @param[in] tmr    Bound handle.
 * @param[in] ch     Which board instance.
 * @param[in] period New wrap point in counts; zero is rejected.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Period accepted.
 * @retval k_ra8_err_invalid_arg  Zero period.
 * @retval k_ra8_err_out_of_range Period above the counter width.
 * @retval k_ra8_err_not_found    No such channel on this board.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_timer_set_period(const fw_timer_t *tmr, fw_timer_ch_t ch, uint32_t period);

/**
 * @brief Read the count latched by the most recent external edge.
 *
 * @details Refused at the facade when the backend declared no capture support,
 *          so a caller never has to tell "this backend cannot" apart from
 *          "no edge has arrived yet".
 *
 * @param[in]  tmr        Bound handle.
 * @param[in]  ch         Which board instance.
 * @param[out] out_counts Latched count on success; zero on any failure.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Latched count written.
 * @retval k_ra8_err_invalid_arg   @p out_counts NULL.
 * @retval k_ra8_err_not_supported Backend has no input capture.
 * @retval k_ra8_err_would_block   No edge latched since the channel opened.
 * @retval k_ra8_err_not_found     No such channel on this board.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_timer_capture_read(const fw_timer_t *tmr, fw_timer_ch_t ch, uint32_t *out_counts);

#ifdef __cplusplus
}
#endif
