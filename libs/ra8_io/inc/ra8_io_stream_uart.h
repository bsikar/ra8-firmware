/**
 * @file ra8_io_stream_uart.h
 * @brief ra8_io byte-stream sink over a UART/SCI channel.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Streams bytes out of an `ra8_sci` channel by polling -- the "print to the host
 * over the serial console" target. The application must have brought the channel
 * up with `ra8_sci_init` first. The data path is exercised on ra8_emulator / HIL;
 * on the host this sink binds and validates only.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_io_stream.h"

/**
 * @struct ra8_io_stream_uart_state_t
 * @brief Caller-owned private state for a UART stream sink.
 *
 * @details Zero-initialise and pass to ::ra8_io_stream_uart_init. Treat the
 *          field as private.
 *
 * @invariant `channel` indexes an initialised SCI channel.
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t channel; /**< SCI channel index (private). */
} ra8_io_stream_uart_state_t;

/**
 * @brief Bind a UART stream sink into a caller-owned stream handle.
 *
 * @param[out] s       Stream handle to bind (zero-initialised by the caller).
 * @param[out] state   Caller-owned sink state to populate.
 * @param[in]  channel SCI channel index to write to.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Sink bound; `s` is usable.
 * @retval k_ra8_err_null_ptr `s` or `state` was NULL.
 *
 * @pre The SCI channel has been initialised with `ra8_sci_init`.
 * @pre `s` and `state` out-live every call made through the stream.
 * @post On success `s` writes bytes out of `channel`.
 * @post On any non-ok return `s` and `state` are left unbound/untouched.
 *
 * @note Not thread-safe with respect to the same stream.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_io_stream_uart_init(ra8_io_stream_t* s, ra8_io_stream_uart_state_t* state, uint8_t channel);

#ifdef __cplusplus
}
#endif
