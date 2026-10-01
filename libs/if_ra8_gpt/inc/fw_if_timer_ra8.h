/**
 * @file fw_if_timer_ra8.h
 * @brief RA8 GPT chip adapter for the neutral `fw_if_timer` port.
 * @ingroup grp_fw_timer
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Fills the nine `fw_timer_iface_t` ops over `ra8_gpt`. It is the chip half of
 * "a binding supplied by the chip and the board": what a GPT channel can do,
 * and how a neutral open, start, stop or period change lands on it.
 *
 * @par What "index" means here
 * As in the clock adapter, the index is the *chip* channel: timer 3 is GPT3.
 * The port wants dense board numbering, and renumbering is the board's job;
 * a board binding wraps this adapter and translates on the way in.
 *
 * @par Ten channels, not fourteen
 * `ra8_gpt_regs.h` sizes the block at fourteen channels, but the only place
 * the tree names counter widths is `ra8_pdg.h`, which names GPT320..GPT329:
 * ten 32-bit channels. Nothing in-tree establishes the width of channels
 * 10..13, and `fw_timer_caps_t` carries one `counter_max` for the whole
 * binding, so a guessed 32 there would let a caller hand a 16-bit counter a
 * period that silently wraps. This adapter therefore reports ten channels.
 * Widening it is a deliberate act with the hardware manual open.
 *
 * @par Period means wrap point
 * The period goes straight into GTPR. In saw-wave mode the counter runs
 * 0..GTPR inclusive, so a period of P is P + 1 counts per cycle; that matches
 * the port's "wrap point", and it is why ::k_fw_timer_ra8_counter_max can be
 * the full 32-bit range. The counter is clocked from PCLKD with no prescaler.
 *
 * @par What is refused
 *   - capture through the port's own open: a GPT capture needs a source
 *     route (pin edge or ELC event) and the port's open carries none, so the
 *     bare adapter reports `has_capture` false. A board binding that knows
 *     its routes opens capture with ::fw_timer_ra8_open_capture instead, then
 *     reads through the ordinary `capture_read` op;
 *   - opening a channel already open, through this port or through the PWM
 *     adapter on the same block: ::k_ra8_err_busy, because `ra8_gpt_init`
 *     takes a module-stop reference and reprograms the whole channel;
 *   - any op but open on a channel not opened here: ::k_ra8_err_invalid_state.
 *
 * @par World
 * `ra8_gpt.h` is `{World: S}`, so this adapter is too.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "fw_if_timer.h"
#include "ra8_err.h"

/**
 * @enum fw_timer_ra8_limits_t
 * @brief What this adapter reports through `get_caps`.
 */
typedef enum : uint32_t {
  k_fw_timer_ra8_channel_count = 10U,        /**< GPT320..GPT329.      */
  k_fw_timer_ra8_counter_bits  = 32U,        /**< GPT32 counter width. */
  k_fw_timer_ra8_counter_max   = UINT32_MAX, /**< Largest GTPR value.  */
} fw_timer_ra8_limits_t;

/**
 * @brief The ops struct this adapter fills.
 *
 * @details
 * Static storage with no context: the GPT block is a chip singleton, so
 * ::fw_timer_bind is called with a NULL `ctx` and the ops ignore it. Which
 * channels are open is library-scope state shared with the PWM adapter.
 *
 * @return Borrowed ops struct, never NULL.
 * @since 0.1.0
 */
const fw_timer_iface_t* fw_timer_ra8_iface(void);

/**
 * @brief Bind a handle to this chip adapter.
 *
 * @details
 * Convenience over `fw_timer_bind(tmr, fw_timer_ra8_iface(), nullptr)`.
 *
 * @param[out] tmr Caller-owned handle, contents ignored on entry.
 * @return As ::fw_timer_bind.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fw_timer_ra8_bind(fw_timer_t* tmr);

/**
 * @brief Open a channel for input capture on capture register A.
 *
 * @details
 * The board-side half of capture: the port's open carries no source, so a
 * board binding that knows which pin edge or ELC event feeds a timer calls
 * this in place of `open` when asked for ::k_fw_timer_mode_capture. The
 * counter runs free on PCLKD with wrap point @p period, stopped until
 * `start`; each selected edge latches the count into GTCCRA. The channel is
 * then an ordinary open timer channel: `start`, `stop`, `read`, `close` and
 * `capture_read` all apply. Pin routing stays the caller's job.
 *
 * @param[in] ch          Chip channel, 0..9.
 * @param[in] period      Wrap point; nonzero.
 * @param[in] source_mask OR of `k_ra8_gpt_cap_src_*` bits from
 *                        `ra8_gpt_capture.h`; nonzero, no reserved bit.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Open, armed, not running.
 * @retval k_ra8_err_not_found   @p ch past the ten channels.
 * @retval k_ra8_err_invalid_arg Zero @p period, or an empty or reserved
 *                               @p source_mask.
 * @retval k_ra8_err_busy        Either adapter already holds the channel.
 * @retval (other)               What `ra8_gpt_init` or
 *                               `ra8_gpt_capture_configure` reported; the
 *                               channel is released again.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_timer_ra8_open_capture(fw_timer_ch_t ch, uint32_t period, uint32_t source_mask);

#ifdef __cplusplus
}
#endif
