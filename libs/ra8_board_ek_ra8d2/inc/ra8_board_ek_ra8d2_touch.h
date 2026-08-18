/**
 * @file ra8_board_ek_ra8d2_touch.h
 * @brief One-call bring-up of the EK-RA8D2 panel's GoodIX GT911 touch controller
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * The board has one touch controller, on one bus, at one address -- and getting
 * at it nonetheless cost every application four checked calls
 * (``ra8_i3c_init`` -> ``ra8_io_i2c_bus_bind_i3c_compat`` ->
 * ``ra8_io_i2c_bus_as_ops`` -> ``ra8_touch_open``) plus four constants it had
 * to know: the channel, the 7-bit address, the bus rate and the clock feeding
 * the bit-rate divider. Eight applications wrote that block, and all eight
 * hardcoded the same 60 MHz source clock rather than asking
 * ``ra8_cgc_get_clock_hz`` -- so on this project's 125 MHz PCLKA tree the
 * touch bus was not running at the 400 kHz any of them asked for.
 * ::ra8_board_touch_open is the board's answer to all four questions at once,
 * and it solves the divider against the LIVE clock.
 *
 * The seam underneath is untouched: ``ra8_touch_open`` still takes an injected
 * ::ra8_i2c_bus_ops_t, ``ra8_i3c_init`` and the ``ra8_io_i2c_bus_*`` binders
 * are still public, and a custom carrier board that puts the GT911 somewhere
 * else still wires it up by hand. This is a shortcut to the ON-BOARD wiring,
 * not a replacement for the injection point. Same shape as
 * ``ra8_board_pdm_mic_route`` for the microphone and
 * ``ra8_board_camera_select_parallel`` for the camera; touch was the one
 * on-board sensor that never got it.
 *
 * OPT-IN, DELIBERATELY. This header is NOT pulled in by the
 * ``ra8_board_ek_ra8d2.h`` umbrella, and the translation unit behind it is
 * compiled into an application only when that application declares ``ra8_io``
 * or ``ra8_io_bus`` in its ``LIBS``: the bus binding it performs lives in
 * ``libs/ra8_io``, which is not on every app's include path.
 *
 * @code
 * const ra8_board_touch_cfg_t cfg = {};  // board defaults, polled
 * if (ra8_board_touch_open(&cfg) != k_ra8_ok) {
 *   // UI still renders; taps just do not arrive.
 * }
 * @endcode
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

#include "ra8_err.h"

/**
 * @struct ra8_board_touch_cfg_t
 * @brief The two touch settings that are an APPLICATION choice, not a board fact.
 *
 * @details
 * Everything else -- channel, target address, bus rate, source clock -- comes
 * from the board (::ra8_board_touch_wiring_t) or from CGC, so it is not
 * offered here. Zero-initialise (`= {}`) to take the board defaults: the
 * GT911's full contact capacity, polled.
 *
 * @invariant `max_points` is 0 (meaning "board default") or at most
 *            ::k_ra8_touch_max_points.
 *
 * @code
 * // Single-contact tap detection, polled:
 * const ra8_board_touch_cfg_t cfg = {.max_points = 1U,
 *                                    .irq_pin    = k_ra8_touch_irq_pin_unset};
 * @endcode
 *
 * @see ra8_board_touch_open  Consumes this descriptor.
 * @see ra8_touch_cfg_t       The driver-level descriptor it is expanded into.
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t max_points; /**< Cap on reported contacts; 0 selects the board default
                       *   (the GT911's full ::k_ra8_touch_max_points capacity). */
  uint8_t irq_pin;    /**< Attention pin, or ::k_ra8_touch_irq_pin_unset for a
                       *   polled controller. Zero-initialising selects pin 0,
                       *   so state the sentinel explicitly when polling a board
                       *   whose pin 0 is wired to something else.             */
} ra8_board_touch_cfg_t;

/**
 * @brief Bring the on-board GT911 up and open the touch driver against it.
 *
 * @details
 * Reads PCLKA from ``ra8_cgc_get_clock_hz`` and initialises the board's I3C
 * channel in I2C-compatibility mode at ::k_ra8_board_touch_bus_hz against that
 * live rate, binds the channel through the ``ra8_io_i2c_bus`` facade, projects
 * it as an ::ra8_i2c_bus_ops_t, and opens ``ra8_touch`` at
 * ::k_ra8_board_touch_target_7b.
 *
 * @param[in] cfg Application-side settings; see ::ra8_board_touch_cfg_t. Must
 *                not be null. `max_points` must be 0 or at most
 *                ::k_ra8_touch_max_points.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 The GT911 answered and the driver is open.
 * @retval k_ra8_err_null_ptr       @p cfg was null.
 * @retval k_ra8_err_invalid_arg    `max_points` exceeds the GT911's capacity.
 * @retval k_ra8_err_invalid_state  ``ra8_touch_open`` was already called.
 * @retval k_ra8_err_hw_timeout     The I3C block did not come out of module
 *                                  stop, or the GT911 did not answer.
 * @retval k_ra8_err_*              Any other code propagated unchanged from
 *                                  ``ra8_i3c_init`` or ``ra8_touch_open``.
 *
 * @pre ``ra8_cgc_init()`` has run, so PCLKA reflects the application's clock
 *      tree rather than the reset default.
 * @pre SW4-5 selects the I3C routing for P400 / P401 (UM Table 20 p 28), and
 *      the Parallel Graphics Expansion Board is fitted.
 * @post On success the touch driver is open and ``ra8_touch_read`` may be
 *       called; the board's I3C channel is initialised in I2C-compat mode.
 * @post On any non-ok return the touch driver is left closed. The I3C channel
 *       may already have been initialised, which is harmless and idempotent.
 *
 * @note Not thread-safe; call once from the single-threaded bring-up path.
 * @warning The bus rate is only as accurate as PCLKA at the moment of the call.
 *          An application that retunes CGC afterwards must re-open, or call
 *          ``ra8_i3c_set_clock`` with the new rate.
 *
 * @see ra8_board_touch_wiring_t  The board facts this call spends.
 * @see ra8_touch_read            Reading contacts once this has succeeded.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_touch_open(const ra8_board_touch_cfg_t* cfg);

#ifdef __cplusplus
}
#endif
