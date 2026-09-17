/**
 * @file ra8_board_ek_ra8d2_touch.c
 * @brief EK-RA8D2 BSP -- GoodIX GT911 bring-up on the board's own I2C wiring
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * Spends the three board facts in ::ra8_board_touch_wiring_t plus the live
 * PCLKA on the four-call sequence eight applications used to write out by
 * hand. Like the rest of the BSP it touches no MCU registers itself:
 * ``ra8_i3c`` owns the IIC_B / I3C citations and ``ra8_touch`` owns the GT911
 * protocol.
 *
 * Compiled into an application only when it declares ``ra8_io`` or
 * ``ra8_io_bus`` in ``LIBS`` (see ``cmake/ra8_app/sources.cmake``): the bus
 * facade this unit binds through lives in ``libs/ra8_io``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_board_ek_ra8d2_touch.h"

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_i2c_bus_ops.h"
#include "ra8_i3c.h"
#include "ra8_io_i2c_bus.h"
#include "ra8_io_i2c_bus_i3c_compat.h"
#include "ra8_touch.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_board.touch";

/**
 * @var s_touch_bus
 * @brief Bound bus handle the touch driver's injected seam points at.
 *
 * @details
 * ``ra8_touch`` keeps the ::ra8_i2c_bus_ops_t it is handed, and those ops carry
 * a pointer back to this handle, so the handle must out-live the open driver.
 * There is one GT911 on one board, so one module-owned handle is the whole
 * requirement -- which is what lets ::ra8_board_touch_open take no bus
 * argument.
 *
 * @note Written only by ::ra8_board_touch_open, from a single-threaded
 *       bring-up context.
 * @warning Never rebind this while the touch driver is open; the driver would
 *          keep issuing transfers through the handle as it changed underneath.
 * @since 0.1.0
 */
static ra8_io_i2c_bus_t s_touch_bus = {};

ra8_err_t ra8_board_touch_open(const ra8_board_touch_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  if (cfg->max_points > (uint8_t)k_ra8_touch_max_points) {
    return k_ra8_err_invalid_arg;
  }

  /* The bit-rate divider is solved against the LIVE PCLKA, not a constant.
   * Eight copies of this block fed it 60 MHz while the project's CGC tree
   * publishes 125 MHz, so none of them ran at the 400 kHz they asked for. */
  uint32_t        pclka_hz = 0U;
  const ra8_err_t clk_err  = ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &pclka_hz);
  if (clk_err != k_ra8_ok) { /* GCOVR_EXCL_BR_LINE -- const channel/handle cannot fail post-bind */
    /* ra8_cgc_get_clock_hz with a valid clock-id and non-null out always
     * returns k_ra8_ok; kept so a future id change cannot pass silently. */
    return clk_err; /* GCOVR_EXCL_LINE -- const channel/handle cannot fail post-bind */
  }

  const ra8_i3c_cfg_t bus_cfg = {
    .mode     = k_ra8_i3c_mode_i2c,
    .bus_hz   = (uint32_t)k_ra8_board_touch_bus_hz,
    .pclka_hz = pclka_hz,
  };
  const ra8_err_t bus_err = ra8_i3c_init((uint8_t)k_ra8_board_touch_i3c_channel, &bus_cfg);
  if (bus_err != k_ra8_ok) {
    return bus_err;
  }

  const ra8_err_t bind_err =
    ra8_io_i2c_bus_bind_i3c_compat(&s_touch_bus, (uint8_t)k_ra8_board_touch_i3c_channel);
  if (bind_err != k_ra8_ok) { /* GCOVR_EXCL_BR_LINE -- const channel/handle cannot fail post-bind */
    /* Rejects only a null handle or an out-of-range channel, and both are
     * compile-time constants here. */
    return bind_err; /* GCOVR_EXCL_LINE -- const channel/handle cannot fail post-bind */
  }

  ra8_i2c_bus_ops_t bus_ops = {};
  const ra8_err_t   ops_err = ra8_io_i2c_bus_as_ops(&s_touch_bus, &bus_ops);
  if (ops_err != k_ra8_ok) { /* GCOVR_EXCL_BR_LINE -- const channel/handle cannot fail post-bind */
    /* Rejects only an unbound or null handle; the bind above just succeeded. */
    return ops_err; /* GCOVR_EXCL_LINE -- const channel/handle cannot fail post-bind */
  }

  const ra8_touch_cfg_t touch_cfg = {
    .bus        = bus_ops,
    .target_7b  = (uint8_t)k_ra8_board_touch_target_7b,
    .irq_pin    = cfg->irq_pin,
    .max_points = (cfg->max_points == 0U) ? (uint8_t)k_ra8_touch_max_points : cfg->max_points,
  };
  return ra8_touch_open(&touch_cfg);
}
