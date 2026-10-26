/**
 * @file fw_if_uart.h
 * @brief Architecture-neutral UART port: line configuration, transmit and
 * poll-read behind one narrow vtable.
 * @ingroup grp_fw_uart
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details
 * Portable code needs a console line, a modem AT channel and a debug trace
 * port, and every chip spells its serial block differently. This port carries
 * the three intents such a driver actually has -- configure a line, transmit
 * bytes, poll for received bytes -- and leaves the mechanism to a binding
 * supplied by the chip and board.
 *
 * UARTs are named in *board* numbering, dense and zero-based, never in chip
 * channel numbers: a board that wires a console and a modem presents UART 0
 * and UART 1 however the silicon allocates them, and its binding maps them.
 * That is the same rule the clock port set for module instances, and how many
 * UARTs exist is a board fact, so the membrane does not range-check the
 * number; the binding owns it and answers ::k_ra8_err_not_found for one it
 * does not carry.
 *
 * The line description is deliberately small: baud, data bits, parity, stop
 * bits. Flow control, FIFO thresholds, DMA wiring and hardware loopback are
 * chip and board policy and belong in a chip-specific binding header, which
 * the composition root may name because the composition root already knows
 * which chip it picked.
 *
 * Transmit is all-or-error: ::fw_uart_write returns ::k_ra8_ok only when the
 * whole buffer is on its way out, so a caller never has to track a partial
 * send. Receive is a poll: ::fw_uart_read fills up to the buffer's length,
 * reports how many bytes it got, and a quiet line is ::k_ra8_ok with a zero
 * count, not an error. Neither call blocks waiting for the other side, and a
 * caller that wants to block composes the poll with the OS port's sleep.
 *
 * Per the taxonomy in `docs/PORTS.md`, this uses (a) the caller-allocated
 * facade plus (b) a narrow ops struct the binding fills. Handles are
 * caller-owned. This interface performs no allocation and contains no chip,
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
#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum fw_uart_data_bits_t
 * @brief Character width on the wire.
 */
typedef enum : uint8_t {
  k_fw_uart_data_bits_none = 0U, /**< Not a width; rejected. */
  k_fw_uart_data_bits_7    = 1U, /**< Seven data bits.       */
  k_fw_uart_data_bits_8    = 2U, /**< Eight data bits.       */
} fw_uart_data_bits_t;

/** @brief One past the last valid ::fw_uart_data_bits_t enumerator. */
#define K_FW_UART_DATA_BITS_COUNT 3U

/**
 * @enum fw_uart_parity_t
 * @brief Parity on the wire.
 */
typedef enum : uint8_t {
  k_fw_uart_parity_none = 0U, /**< No parity bit. */
  k_fw_uart_parity_even = 1U, /**< Even parity.   */
  k_fw_uart_parity_odd  = 2U, /**< Odd parity.    */
} fw_uart_parity_t;

/** @brief One past the last valid ::fw_uart_parity_t enumerator. */
#define K_FW_UART_PARITY_COUNT 3U

/**
 * @enum fw_uart_stop_bits_t
 * @brief Stop bits on the wire.
 */
typedef enum : uint8_t {
  k_fw_uart_stop_bits_none = 0U, /**< Not a width; rejected. */
  k_fw_uart_stop_bits_one  = 1U, /**< One stop bit.          */
  k_fw_uart_stop_bits_two  = 2U, /**< Two stop bits.         */
} fw_uart_stop_bits_t;

/** @brief One past the last valid ::fw_uart_stop_bits_t enumerator. */
#define K_FW_UART_STOP_BITS_COUNT 3U

/**
 * @struct fw_uart_config_s
 * @brief Everything the port says about a line.
 */
typedef struct fw_uart_config_s {
  uint32_t            baud;      /**< Bits per second; zero is refused.     */
  fw_uart_data_bits_t data_bits; /**< Character width.                      */
  fw_uart_parity_t    parity;    /**< Parity mode.                          */
  fw_uart_stop_bits_t stop_bits; /**< Stop-bit width.                       */
} fw_uart_config_t;

/**
 * @struct fw_uart_iface_s
 * @brief The narrow ops struct a chip-and-board binding fills.
 *
 * @details
 * Three entries, because the port has three intents. Each takes the binding's
 * own context as its first argument, so one binding can serve more than one
 * instance without a file-scope variable. A NULL op is a malformed binding,
 * not a declined capability, and ::fw_uart_bind rejects it.
 */
typedef struct fw_uart_iface_s {
  /** @brief Put @p uart on @p cfg's line parameters. */
  ra8_err_t (*configure)(void *ctx, uint8_t uart, fw_uart_config_t cfg);
  /** @brief Transmit @p len bytes. Returns ok only when all of them went. */
  ra8_err_t (*write)(void *ctx, uint8_t uart, const uint8_t *bytes, size_t len);
  /** @brief Poll for received bytes, up to @p len; a quiet line reads zero. */
  ra8_err_t (*read)(void *ctx, uint8_t uart, uint8_t *out, size_t len,
                    size_t *out_count);
} fw_uart_iface_t;

/**
 * @struct fw_uart_s
 * @brief Caller-owned binding handle.
 *
 * @details
 * Populated only by ::fw_uart_bind and treated as opaque afterwards. The
 * `bound` flag is what every entry point checks: a zeroed handle is not bound,
 * so a caller that forgets to bind gets ::k_ra8_err_not_initialized rather
 * than a jump through an uninitialised function pointer.
 */
typedef struct fw_uart_s {
  const fw_uart_iface_t *iface; /**< Binding ops, never NULL once bound. */
  void                   *ctx;  /**< Binding context, may be NULL.       */
  bool                    bound; /**< Set by ::fw_uart_bind only.        */
} fw_uart_t;

/**
 * @brief Bind a handle to a chip-and-board UART binding.
 *
 * @param[out] uart  Caller-owned storage, contents ignored on entry.
 * @param[in]  iface Ops struct; borrowed, must outlive @p uart.
 * @param[in]  ctx   Binding context, handed back to every op. May be NULL.
 * @return ::k_ra8_ok, or ::k_ra8_err_invalid_arg when @p uart or @p iface is
 *         NULL or @p iface leaves an op unset.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_uart_bind(fw_uart_t *uart, const fw_uart_iface_t *iface, void *ctx);

/**
 * @brief Put one UART on a line description.
 *
 * @param[in] uart Bound handle.
 * @param[in] uart UART in board numbering.
 * @param[in] cfg  Line description; a zero baud or an out-of-range field is
 *                 rejected before the binding is reached.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for an unbound handle or a
 *         malformed cfg, ::k_ra8_err_not_found when the board has no such
 *         UART, or whatever the binding reported.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_uart_configure(const fw_uart_t *uart, uint8_t uart_index, fw_uart_config_t cfg);

/**
 * @brief Transmit bytes on one UART.
 *
 * @details
 * All-or-error: on ::k_ra8_ok the whole buffer is on its way out, and on any
 * error the binding tells what it could not do, so a caller never tracks a
 * partial send. A zero-length write is satisfied without waking the
 * peripheral.
 *
 * @param[in] uart  Bound handle.
 * @param[in] uart_index UART in board numbering.
 * @param[in] bytes Buffer to send; may be NULL only when @p len is zero.
 * @param[in] len   Bytes to send.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for a NULL buffer with a
 *         nonzero length or an unbound handle, or whatever the binding
 *         reported.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_uart_write(const fw_uart_t *uart, uint8_t uart_index, const uint8_t *bytes,
              size_t len);

/**
 * @brief Poll one UART for received bytes.
 *
 * @details
 * A quiet line is ::k_ra8_ok with a zero count, not an error: the caller asked
 * what arrived and the honest answer is "nothing yet". The count is never
 * above @p len.
 *
 * @param[in]  uart  Bound handle.
 * @param[in]  uart_index UART in board numbering.
 * @param[out] out   Buffer to fill; may be NULL only when @p len is zero.
 * @param[in]  len   Buffer capacity in bytes.
 * @param[out] out_count On success, bytes actually read, at most @p len.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for a NULL output count, a NULL
 *         buffer with a nonzero length, or an unbound handle, or whatever the
 *         binding reported.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_uart_read(const fw_uart_t *uart, uint8_t uart_index, uint8_t *out, size_t len,
             size_t *out_count);

#ifdef __cplusplus
}
#endif
