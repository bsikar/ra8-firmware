/**
 * @file ra8_board_ek_ra8d2_console_stream.c
 * @brief EK-RA8D2 BSP -- bind the debug console into an ra8_io byte stream
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * The whole translation unit is the board half of one question: which SCI
 * channel is the console, and has it been brought up? ``ra8_io_stream_uart``
 * already knows how to write bytes at a channel by polling, so this unit adds
 * no second copy of that -- it answers the board question and forwards.
 *
 * Compiled into an application only when that application declares ``ra8_io``
 * in its ``LIBS`` (see ``cmake/ra8_app/sources.cmake``); it is the one BSP unit
 * that reaches outside the libraries every app already links.
 *
 * Touches no MCU registers: ``ra8_io_stream_uart`` -> ``ra8_sci`` owns the HUM
 * citations for the transmit path.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_board_ek_ra8d2_console_stream.h"

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_board_ek_ra8d2_internal.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_uart.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_board.console_stream";

/**
 * @var s_console_sink
 * @brief Sink state backing every stream handle bound to the board console.
 *
 * @details
 * The console is a board singleton -- one J-Link OB VCOM bridge on one SCI
 * channel -- so its sink state is module-owned rather than caller-supplied.
 * That is what lets ::ra8_board_console_stream take only a handle. Two handles
 * bound from this module share these bytes and therefore address the same
 * console, which is the intended behaviour.
 *
 * @note Written only by ::ra8_board_console_stream, from a single-threaded
 *       application context.
 * @warning Do not write this directly; ``ra8_io_stream_uart_init`` owns its
 *          layout.
 * @since 0.1.0
 */
static ra8_io_stream_uart_state_t s_console_sink = {};

ra8_err_t ra8_board_console_stream(ra8_io_stream_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (!priv_ra8_board_uart_console_is_up()) {
    return k_ra8_err_not_initialized;
  }
  return ra8_io_stream_uart_init(out,
                                 &s_console_sink,
                                 (uint8_t)k_ra8_board_uart_console_sci_channel);
}
