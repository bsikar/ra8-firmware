/**
 * @file fw_if_uart_stub.h
 * @brief Stub chip adapter for the neutral `fw_if_uart` port: the slice-5
 * proof binding.
 * @ingroup grp_fw_uart
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details
 * `fw_if_uart.h` says a UART is named in board numbering and that its line
 * description is what a chip-and-board binding owns between them. This file
 * is that sentence's proof binding: the `if_<chip>` half of epic RA8FW-618's
 * slice 5, standing in for a real chip adapter the way the proof stack
 * ra8_core + ra8_fmt + ra8_xml stands in for a real portable application.
 *
 * @par What it is, and what it is not
 * It is a complete binding of the port: ops that are never null, a board
 * that carries two lines in dense numbering, and the same bind contract the
 * RA8 adapter answers to, so the facade's guards run against a binding that
 * is not the RA8 one. It is not a port of any chip. The profile is
 * arbitrary: a line comes up at the stub board's firmware default of 115200
 * 8N1, a write is captured into a per-line ring, a read drains a per-line
 * receive queue (a quiet line is ::k_ra8_ok with a zero count), and every
 * line past index 1 answers ::k_ra8_err_not_found. A real port (an
 * `if_esp32c6` set, say) replaces this member by member, and nothing above
 * `libs/if` changes.
 *
 * @par World
 * The stub reads no hardware and names no vendor header, so unlike the RA8
 * adapters this one carries no {World: S} boundary: it builds for every
 * freestanding target the portability gate runs.
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
#include <stddef.h>
#include <stdint.h>

#include "fw_if_uart.h"
#include "ra8_err.h"

/**
 * @brief The stub profile's configure op, drop-in for ::fw_uart_iface_t.
 *
 * @details
 * Pure state change: no register is touched, which is what lets the profile
 * be tested on the host without a fake peripheral block. A zero baud or an
 * out-of-range field is refused, and a line the board does not carry answers
 * ::k_ra8_err_not_found.
 *
 * @param[in] ctx  Ignored; the stub serves one port.
 * @param[in] uart Line in board numbering.
 * @param[in] cfg  Line description.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for a malformed description, or
 *         ::k_ra8_err_not_found when the board has no such line.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_uart_stub_configure(void *ctx, uint8_t uart, fw_uart_config_t cfg);

/**
 * @brief The stub profile's write op, drop-in for ::fw_uart_iface_t.
 *
 * @details
 * Captures bytes into the line's ring in order. All-or-error holds by
 * construction: the stub line never refuses a byte, so ::k_ra8_ok means the
 * whole buffer was taken. A zero-length write is satisfied without waking
 * the peripheral, the port's own contract for the request.
 *
 * @param[in] ctx   Ignored; the stub serves one port.
 * @param[in] uart  Line in board numbering.
 * @param[in] bytes Buffer to send; may be NULL only when @p len is zero.
 * @param[in] len   Bytes to send.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for a NULL buffer with a
 *         nonzero length, or ::k_ra8_err_not_found when the board has no
 *         such line.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_uart_stub_write(void *ctx, uint8_t uart, const uint8_t *bytes, size_t len);

/**
 * @brief The stub profile's read op, drop-in for ::fw_uart_iface_t.
 *
 * @details
 * Drains the line's receive queue, up to @p len bytes; a quiet line is
 * ::k_ra8_ok with a zero count, the port's own contract for the poll. The
 * queue is primed by the profile's test seam, so a quiet board is the
 * normal state and the count asserts nothing about real silicon.
 *
 * @param[in]  ctx       Ignored; the stub serves one port.
 * @param[in]  uart      Line in board numbering.
 * @param[out] out       Buffer to fill; may be NULL only when @p len is zero.
 * @param[in]  len       Buffer capacity in bytes.
 * @param[out] out_count Receives the bytes read, at most @p len.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for a NULL output count or a
 *         NULL buffer with a nonzero length, or ::k_ra8_err_not_found when
 *         the board has no such line.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_uart_stub_read(void *ctx, uint8_t uart, uint8_t *out, size_t len,
                  size_t *out_count);

/**
 * @brief The ops struct this adapter fills.
 *
 * @details
 * Static storage with no context, because the stub profile is a singleton:
 * ::fw_uart_bind is called with a NULL `ctx` and the ops ignore it. The
 * pointer outlives any handle bound to it.
 *
 * @return Borrowed ops struct, never NULL.
 * @since 0.1.0
 */
const fw_uart_iface_t *fw_uart_stub_iface(void);

/**
 * @brief Bind a handle to the stub adapter.
 *
 * @details
 * Convenience over `fw_uart_bind(uart, fw_uart_stub_iface(), nullptr)`, so a
 * composition root does not have to name the context it is not passing.
 *
 * @param[out] uart Caller-owned handle, contents ignored on entry.
 * @return As ::fw_uart_bind.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fw_uart_stub_bind(fw_uart_t *uart);

#ifdef __cplusplus
}
#endif
