/**
 * @file ra8_board_ek_ra8d2_clock_profile.h
 * @brief EK-RA8D2 clock profile: the board's dense module numbering over the
 *        RA8 chip clock binding.
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * ``fw_if_clock.h`` numbers modules the way the *board* wires them: zero-based
 * and dense, so an application asks for UART 0 without knowing which SCI
 * channel carries it. ``fw_if_clock_ra8.h`` answers in chip instances, because
 * that is the only numbering the clock tree and the module-stop table know.
 * This file is the piece between them, and it is the only place on the EK-RA8D2
 * that knows the two numberings differ.
 *
 * The difference is not cosmetic here. The RA8D2 has ten SCI channels; this
 * board routes four of them to a connector, and they are not the first four.
 * The debug console is SCI8, so an application that hardcoded UART 8 to mean
 * the console would be writing a chip fact into portable code. Through this
 * profile the console is UART 3, and the board is free to be wrong about
 * which SCI that is without any application changing.
 *
 * What the profile publishes is what the board *wires*, not what the silicon
 * has. A kind the board routes nowhere is refused, even when the chip binding
 * would happily answer for it: an application that can gate a CAN controller
 * this board has no transceiver or connector for is being told a useful-looking
 * lie. The refusal is ::k_ra8_err_not_found, the same answer the chip binding
 * gives for a block the part does not have, because from the caller's side the
 * two are the same situation: there is no such module here.
 *
 * Every row below cites the board file that already names that channel, so the
 * profile and the driver that uses it cannot drift apart silently.
 *
 * Authoritative source: ``docs/reference/ek-ra8d2-v1-users-manual.pdf``
 * (Rev 1.01, R20UT5523EG0101, October 2025).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "fw_if_clock.h"
#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Translate a board-numbered module to the chip instance behind it.
 *
 * @details
 * The translation is a lookup, never a computation: the wired channels are not
 * a run (SCI 0, 2, 7, 8), so arithmetic on the board index would be wrong for
 * every kind whose instances are scattered.
 *
 * A kind this board routes nowhere is refused here rather than passed down, so
 * the chip binding is never asked about a block the board cannot reach.
 *
 * @param[in]  module   Board-numbered module, as ``fw_if_clock.h`` defines it.
 * @param[out] out_chip Chip-numbered module for ``fw_if_clock_ra8.h``.
 * @return Translation status.
 * @retval k_ra8_ok The board wires this module; @p out_chip is filled.
 * @retval k_ra8_err_invalid_arg @p out_chip is null.
 * @retval k_ra8_err_not_found This board wires no such module.
 *
 * @note Pure; safe from any context.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_board_clock_profile_to_chip(fw_clock_module_t module, fw_clock_module_t* out_chip);

/**
 * @brief How many instances of @p kind this board wires.
 *
 * @details
 * The dense count, so a caller can walk 0..count-1 without probing for the
 * first refusal. Zero for a kind the board routes nowhere.
 *
 * @param[in] kind Module kind.
 * @return Instance count, 0 when the board wires none or @p kind is out of range.
 *
 * @note Pure; safe from any context.
 * @since 0.1.0
 */
[[nodiscard]] uint8_t ra8_board_clock_profile_count(fw_clock_module_kind_t kind);

/**
 * @brief Bind @p clk to this board's clock profile.
 *
 * @details
 * The bound handle speaks board numbering. Reads and module-stop gating are
 * delegated to the RA8 chip binding once the index has been translated, so the
 * refcount behaviour, the zero-rate refusal, and the secure-world requirement
 * documented on ``fw_if_clock_ra8.h`` all hold unchanged through this layer.
 *
 * @param[out] clk Handle to bind.
 * @return Bind status.
 * @retval k_ra8_ok Bound.
 * @retval k_ra8_err_invalid_arg @p clk is null. Delegated to ``fw_clock_bind``,
 *         which uses the same code for a null handle and a malformed binding.
 *
 * @pre ``ra8_board_clocks_init`` has run, or rates read back as the reset tree.
 * @pre ``ra8_mstp_init`` has run before any gating call through @p clk.
 * @note Not thread-safe with respect to @p clk; the binding itself is stateless.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_clock_profile_bind(fw_clock_t* clk);

/**
 * @brief The board's own clock handle, bound to the profile above.
 *
 * @details
 * A consumer that only wants to ask this board a clock question should not
 * have to own storage and a bind call to do it. This returns a handle bound
 * to ``ra8_board_clock_profile_bind`` on first use and unchanged thereafter,
 * so a reach-in that used to read a chip domain directly becomes one call:
 *
 * @code
 * uint32_t hz = 0U;
 * const fw_clock_module_t core = {.kind = k_fw_clock_module_core, .index = 0U};
 * if (fw_clock_rate_for(ra8_board_clock(), core, &hz) != k_ra8_ok) {
 *   return err;
 * }
 * @endcode
 *
 * The binding is stateless and the profile tables are ``const``, so binding
 * is pure table assignment with nothing to fail: the only way
 * ``ra8_board_clock_profile_bind`` reports an error is a null handle, and the
 * handle here is a file-static. The return is therefore never NULL, which is
 * what lets the call above skip a status check on acquisition.
 *
 * @return Bound handle for this board. Never NULL.
 *
 * @pre ``ra8_board_clocks_init`` has run, or rates read back as the reset tree.
 * @pre ``ra8_mstp_init`` has run before any gating call through the handle.
 * @note Not thread-safe on first call: two threads racing the first
 *       acquisition both write the same table pointers, which is benign here,
 *       but no ordering is published. Acquire it once during bring-up, on the
 *       core that ran bring-up, if that matters to a caller.
 * @warning One handle for the whole board. A caller must not pass it to
 *          ``fw_clock_bind`` again or otherwise mutate it.
 * @since 0.1.0
 */
[[nodiscard]] const fw_clock_t* ra8_board_clock(void);

#ifdef __cplusplus
}
#endif
