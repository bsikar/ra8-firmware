/**
 * @file ra8_modem_at.h
 * @brief Cellular modem AT command/response driver layered on UART
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Tiny AT-command transport for SIM7600 / Quectel BG95 / similar
 * 3GPP modems. Sits on top of a UART-like byte transport and
 * provides:
 *
 * - ``ra8_modem_at_init``: bind a byte transport, response buffer,
 *   and default per-command timeout configuration.
 * - ``ra8_modem_at_send_cmd``: write an AT command terminated with
 *   ``"\r"`` and block until the modem replies with ``OK``,
 *   ``ERROR`` or a caller-specified expected prefix.
 * - ``ra8_modem_at_send_cmd_capture``: same as ``send_cmd`` but copy
 *   any response payload lines (everything between echo and final
 *   result code) into a caller buffer.
 * - ``ra8_modem_at_register_unsolicited_handler``: register a
 *   callback for unsolicited result codes (URCs) such as
 *   ``+CMTI:`` (SMS arrival) or ``+CREG:`` (network registration
 *   change). The driver scans every received line; lines that do
 *   not belong to a pending command are dispatched to the matching
 *   handler.
 *
 * ## State machine
 *
 * The line accumulator runs as:
 *
 * - ``k_ra8_modem_at_state_idle``      -- waiting for command.
 * - ``k_ra8_modem_at_state_await_echo``-- expecting the modem to
 *   echo the command we just transmitted (``ATE1`` default).
 * - ``k_ra8_modem_at_state_await_resp``-- collecting payload + final
 *   result code.
 * - ``k_ra8_modem_at_state_done``      -- result code matched,
 *   transition back to idle on next call.
 *
 * Lines are accumulated byte-by-byte from the byte transport. CR/LF
 * pairs split the stream into discrete lines. Each line is either
 *
 * - the command echo (silently dropped),
 * - a final result code (``OK``, ``ERROR``, ``+CME ERROR:`` ...),
 * - the caller-specified ``expected_response`` prefix, or
 * - a URC (dispatched if a registered prefix matches).
 *
 * Anything else is treated as response payload and (optionally)
 * captured into the user buffer.
 *
 * ## Memory
 *
 * NASA Power-of-10 Rule 3 compliant -- zero dynamic allocation.
 * The line accumulator buffer is owned by the caller and passed in
 * via ``ra8_modem_at_cfg_t``. Unsolicited-handler slots live in a
 * fixed-size table inside the module.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @brief Compile-time tunables.
 *
 * @details
 * All limits are typed C23 enums so the compiler picks an exact
 * width and the values appear by name in the debugger.
 */
typedef enum : uint8_t {
  k_ra8_modem_at_max_unsolicited = 8U, /**< Maximum number of registered URC handlers. */
  k_ra8_modem_at_max_prefix_len =
    16U, /**< Maximum bytes of a URC prefix (including ``+`` and ``:``). */
} ra8_modem_at_limits_t;

/**
 * @brief Default per-command timeout when caller passes 0 ms.
 */
typedef enum : uint16_t {
  k_ra8_modem_at_default_timeout_ms = 1000U, /**< RA8 modem at default timeout ms. */
} ra8_modem_at_timeouts_t;

/**
 * @struct ra8_modem_at_io_t
 * @brief Byte-level transport injected into the AT driver (DIP).
 *
 * @details
 * Decouples the AT layer from any concrete UART implementation. The
 * production wiring binds these pointers to an ``ra8_uart``-backed
 * adapter; unit tests bind them to an in-memory FIFO. Function
 * pointers here are explicitly permitted by the project's NASA
 * Rule 9 deviation for Dependency Inversion.
 *
 * @invariant All three function pointers must be non-NULL.
 *
 * @see ra8_modem_at_init()
 */
typedef struct {
  /**
   * @brief Transmit a single byte (blocking).
   *
   * @param[in] ctx     Opaque transport context (e.g. SCI channel).
   * @param[in] byte    Byte to send.
   * @return ``k_ra8_ok`` on success, ``ra8_err_t`` otherwise.
   */
  ra8_err_t (*tx_byte)(void* ctx, uint8_t byte);

  /**
   * @brief Try to receive a single byte without blocking.
   *
   * @param[in]  ctx       Opaque transport context.
   * @param[out] out_byte  Receive slot, only written on success.
   * @return ``k_ra8_ok`` if a byte was popped,
   *         ``k_ra8_err_no_data`` if the RX FIFO is empty.
   */
  ra8_err_t (*rx_byte)(void* ctx, uint8_t* out_byte);

  /**
   * @brief Return monotonic time in milliseconds.
   *
   * @details
   * Used to enforce per-command timeouts. The timebase need only be
   * monotonic; absolute epoch is irrelevant.
   *
   * @param[in] ctx Opaque transport context.
   * @return Milliseconds since some fixed reference point.
   */
  uint32_t (*now_ms)(void* ctx);

  /**
   * @brief Opaque context passed to ``tx_byte``/``rx_byte``/``now_ms``.
   */
  void* ctx;
} ra8_modem_at_io_t;

/**
 * @struct ra8_modem_at_cfg_t
 * @brief Configuration handed to ``ra8_modem_at_init``.
 *
 * @invariant ``line_buf != NULL``.
 * @invariant ``line_buf_len >= 16``.
 */
typedef struct {
  ra8_modem_at_io_t io;                 /**< Byte transport (must be fully populated). */
  uint8_t*          line_buf;           /**< Caller-owned line accumulator buffer.     */
  uint16_t          line_buf_len;       /**< Bytes in ``line_buf`` (>= 16).            */
  uint16_t          default_timeout_ms; /**< 0 means use compiled default.             */
} ra8_modem_at_cfg_t;

/**
 * @brief Unsolicited result-code (URC) handler signature.
 *
 * @param[in] line  NUL-terminated full line received from the modem
 *                  (e.g. ``"+CMTI: \"SM\",3"``).
 * @param[in] ctx   Opaque pointer registered with the handler.
 */
typedef void (*ra8_modem_at_urc_fn_t)(const char* line, void* ctx);

/**
 * @brief Bind transport, buffer and timeout policy.
 *
 * @details
 * Resets all internal state and copies ``cfg`` into module-static
 * storage. Must be called once before any other API. Re-calling is
 * legal -- previous URC handlers are cleared.
 *
 * @param[in] cfg Pointer to a fully populated configuration.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Initialized successfully.
 * @retval k_ra8_err_null_ptr       ``cfg``, ``cfg->line_buf`` or any
 *                                 IO function pointer is NULL.
 * @retval k_ra8_err_invalid_size   ``line_buf_len`` smaller than 16.
 *
 * @pre ``cfg != NULL``.
 * @pre All members of ``cfg->io`` non-NULL.
 * @post Internal state reset to ``k_ra8_modem_at_state_idle``.
 * @post URC table empty.
 *
 * @note Not thread-safe; the AT driver assumes a single owner.
 *
 * @par Example:
 * @code
 * static uint8_t s_at_line[256];
 * ra8_modem_at_cfg_t cfg = {
 *   .io = {
 *     .tx_byte = my_uart_tx, .rx_byte = my_uart_rx, .now_ms = my_now,
 *     .ctx = (void*)1U,
 *   },
 *   .line_buf = s_at_line,
 *   .line_buf_len = sizeof s_at_line,
 *   .default_timeout_ms = 2000U,
 * };
 * (void)ra8_modem_at_init(&cfg);
 * @endcode
 *
 * @see ra8_modem_at_send_cmd()
 * @see ra8_modem_at_register_unsolicited_handler()
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_modem_at_init(const ra8_modem_at_cfg_t* cfg);

/**
 * @brief Send an AT command and wait for ``OK``/``ERROR``/expected.
 *
 * @details
 * Writes ``cmd`` followed by ``"\r"`` byte-by-byte through
 * ``io.tx_byte``, then drives the line accumulator until either:
 *
 * - the final result code ``OK`` arrives -> ``k_ra8_ok``,
 * - the final result code ``ERROR`` (or ``+CME ERROR:``,
 *   ``+CMS ERROR:``) arrives -> ``k_ra8_err_hw_error``,
 * - a line beginning with ``expected_response`` arrives (also
 *   counts as success and the function continues to drain until
 *   ``OK``) -> ``k_ra8_ok``,
 * - ``timeout_ms`` (or the configured default if 0) elapses ->
 *   ``k_ra8_err_hw_timeout``.
 *
 * @param[in] cmd               NUL-terminated AT command without
 *                              the trailing ``"\r"``.
 * @param[in] expected_response Optional NUL-terminated prefix to
 *                              wait for (e.g. ``"+CSQ:"``). May be
 *                              NULL when only ``OK`` matters.
 * @param[in] timeout_ms        Per-command timeout in ms. 0 selects
 *                              the configured default.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Final ``OK`` (and expected, if set) seen.
 * @retval k_ra8_err_null_ptr        ``cmd`` was NULL.
 * @retval k_ra8_err_not_initialized ``ra8_modem_at_init`` not yet called.
 * @retval k_ra8_err_hw_error        Modem replied ``ERROR``/``+CME ERROR``.
 * @retval k_ra8_err_hw_timeout      No final result code in time.
 *
 * @pre ``ra8_modem_at_init`` returned ``k_ra8_ok``.
 * @pre ``cmd`` NUL-terminated and shorter than ``line_buf_len``.
 * @post Driver returned to ``k_ra8_modem_at_state_idle``.
 * @post Line accumulator is empty.
 *
 * @note Blocking. URC handlers are invoked inline while waiting.
 * @warning Not re-entrant.
 *
 * @see ra8_modem_at_send_cmd_capture()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_modem_at_send_cmd(const char* cmd, const char* expected_response, uint16_t timeout_ms);

/**
 * @brief Send an AT command and capture the response payload.
 *
 * @details
 * Identical wait policy to ``ra8_modem_at_send_cmd`` but every
 * non-echo, non-final-result, non-URC line is appended to
 * ``out_buf`` with ``'\n'`` separators. Truncation past
 * ``buf_len - 1`` bytes is silent; the buffer is always
 * NUL-terminated.
 *
 * @param[in]  cmd         NUL-terminated AT command (no trailing CR).
 * @param[out] out_buf     Caller buffer for captured payload.
 * @param[in]  buf_len     Bytes in ``out_buf`` (>= 1).
 * @param[in]  timeout_ms  Per-command timeout in ms (0 -> default).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Command finished with ``OK``.
 * @retval k_ra8_err_null_ptr        ``cmd`` or ``out_buf`` NULL.
 * @retval k_ra8_err_invalid_size    ``buf_len`` is 0.
 * @retval k_ra8_err_not_initialized ``ra8_modem_at_init`` not called.
 * @retval k_ra8_err_hw_error        Modem replied ``ERROR``.
 * @retval k_ra8_err_hw_timeout      No final result code in time.
 *
 * @pre ``out_buf`` points to writable storage of ``buf_len`` bytes.
 * @post ``out_buf`` is NUL-terminated.
 *
 * @note Blocking. Response payload is line-separated with ``'\n'``.
 *
 * @see ra8_modem_at_send_cmd()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_modem_at_send_cmd_capture(const char* cmd, char* out_buf, size_t buf_len, uint16_t timeout_ms);

/**
 * @brief Register a URC (unsolicited result code) handler.
 *
 * @details
 * Whenever the line accumulator emits a complete line that starts
 * with ``prefix`` and the driver is not currently expecting that
 * prefix as a command response, ``fn(line, ctx)`` is invoked.
 * Registration order is preserved, but only one handler per
 * distinct prefix is permitted -- duplicate registration replaces
 * the previous handler.
 *
 * @param[in] prefix  NUL-terminated prefix string (e.g. ``"+CMTI:"``).
 *                    Must be shorter than ``k_ra8_modem_at_max_prefix_len``.
 * @param[in] fn      Handler function (must be non-NULL).
 * @param[in] ctx     Opaque pointer passed to ``fn`` on every match.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Handler installed (or replaced).
 * @retval k_ra8_err_null_ptr        ``prefix`` or ``fn`` NULL.
 * @retval k_ra8_err_invalid_size    ``prefix`` too long or empty.
 * @retval k_ra8_err_no_mem          Handler table full.
 * @retval k_ra8_err_not_initialized Module not initialized.
 *
 * @pre ``ra8_modem_at_init`` returned ``k_ra8_ok``.
 * @post Future matching lines invoke ``fn`` exactly once each.
 *
 * @note Not thread-safe with concurrent ``ra8_modem_at_send_cmd``.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_modem_at_register_unsolicited_handler(const char* prefix, ra8_modem_at_urc_fn_t fn, void* ctx);

/**
 * @brief Pump the RX path without sending a command.
 *
 * @details
 * Drains any queued bytes, dispatches URC handlers, and returns.
 * Useful when the application is otherwise idle but wants to
 * service unsolicited modem traffic. Does not block beyond a
 * single drain pass.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Drain completed.
 * @retval k_ra8_err_not_initialized Module not initialized.
 *
 * @pre ``ra8_modem_at_init`` returned ``k_ra8_ok``.
 * @post RX FIFO drained for the current call.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_modem_at_poll(void);

#ifdef __cplusplus
}
#endif
