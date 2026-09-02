/**
 * @file ra8_board_ek_ra8d2_console_stream.h
 * @brief EK-RA8D2 debug console as an ``ra8_io_stream`` byte sink
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * ``ra8_board_uart_console_write()`` is the right primitive -- bytes and a
 * length -- but it is the ONLY thing the board console offers, so an
 * application that wants to print a number writes its own adapter first.
 * Meanwhile ``ra8_io_stream`` already carries ``puts`` / ``putc`` /
 * ``put_u32`` / ``put_u64`` / ``put_hex`` and cannot be aimed at the console
 * without knowing that it is SCI8 and repeating its PD02 / PD03 pin routing.
 * ::ra8_board_console_stream closes that gap in one call, so the whole stream
 * API becomes reachable from an application that knows only "the board has a
 * console".
 *
 * OPT-IN, DELIBERATELY. This header is NOT pulled in by the
 * ``ra8_board_ek_ra8d2.h`` umbrella, and the translation unit behind it is
 * compiled into an application only when that application declares ``ra8_io``
 * in its ``LIBS``. The rest of the BSP depends solely on libraries every app
 * already compiles; the stream binding is the one part that does not, so an
 * application that never asks for a stream pays neither the include path nor
 * the object code. Mirrors how ``ra8_compress.h`` is kept out of the
 * ``ra8_io.h`` umbrella for the same reason.
 *
 * @code
 * ra8_io_stream_t con = {};
 * (void)ra8_board_uart_console_init(115200U);
 * if (ra8_board_console_stream(&con) == k_ra8_ok) {
 *   (void)ra8_io_stream_puts(&con, "dtc: copied ");
 *   (void)ra8_io_stream_put_u32(&con, copied);
 *   (void)ra8_io_stream_puts(&con, "B err=");
 *   (void)ra8_io_stream_puts(&con, ra8_err_to_str(err));
 *   (void)ra8_io_stream_putc(&con, '\n');
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

#include "ra8_err.h"
#include "ra8_io_stream.h"

/**
 * @brief Bind the J-Link OB VCOM console into a caller-owned stream handle.
 *
 * @details
 * Hands back an ::ra8_io_stream_t whose sink is the board's debug console, so
 * every ``ra8_io_stream_*`` writer lands on the same UART that
 * ``ra8_board_uart_console_write()`` drives. The board supplies the two facts
 * the application would otherwise have to know: that the console is SCI channel
 * ::k_ra8_board_uart_console_sci_channel, and that
 * ``ra8_board_uart_console_init()`` has already routed PD02 / PD03 and
 * programmed the bit-rate register for the live PCLKA.
 *
 * The sink state lives in this module (the console is a board singleton, so
 * there is exactly one), which is why the caller supplies only the handle.
 * Binding twice is harmless and yields two handles onto the same console.
 *
 * @param[out] out Caller-owned stream handle to bind; zero-initialise (`= {}`)
 *                 before the call. On success it writes to the console.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  @p out is bound and usable.
 * @retval k_ra8_err_null_ptr        @p out was null.
 * @retval k_ra8_err_not_initialized ``ra8_board_uart_console_init()`` has not
 *                                   succeeded, so there is no console to bind.
 *
 * @pre ``ra8_board_uart_console_init()`` returned ::k_ra8_ok.
 * @pre @p out out-lives every write made through it.
 * @post On success @p out carries a sink aimed at the board console.
 * @post On any non-ok return @p out is left exactly as the caller passed it.
 *
 * @note Not thread-safe with respect to the console; one writer at a time.
 * @warning Bytes written through the returned stream reach the SCI channel
 *          directly. Interleaving them with ``ra8_board_uart_console_write()``
 *          from an interrupt would interleave on the wire.
 *
 * @see ra8_board_uart_console_init  Brings the console up; call it first.
 * @see ra8_io_stream_puts           One of the writers this unlocks.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_console_stream(ra8_io_stream_t* out);

#ifdef __cplusplus
}
#endif
