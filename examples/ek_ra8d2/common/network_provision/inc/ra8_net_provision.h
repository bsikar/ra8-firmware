/**
 * @file examples/ek_ra8d2/common/network_provision/inc/ra8_net_provision.h
 * @brief Runtime network provisioning contract for EK-RA8D2 applications
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Defines the credential record, ASCII-hex wire parser, and bounded UART
 * receiver shared by examples that join a network at runtime. The interface
 * keeps credentials out of CMake, compiler command lines, build metadata, and
 * firmware images. UART operations are injected so the exact production
 * parser and receive state machine can run in host tests without hardware.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_net_provision_limit_t
 * @brief Static bounds for the version-one provisioning protocol
 * @details The maximum wire line includes the fixed prefix, three fields, two
 *          separators, and one newline. Every receive and decode loop is
 *          bounded by one of these values.
 * @invariant The text capacities include one trailing NUL beyond the protocol
 *            payload maxima.
 * @invariant The maximum line fits in a 16-bit length.
 * @par Example:
 * @code
 * uint8_t line[k_ra8_net_provision_line_bytes_max];
 * @endcode
 * @see ra8_net_provision_parse
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_net_provision_ssid_bytes_max = 32U,    /**< Maximum decoded SSID bytes.       */
  k_ra8_net_provision_psk_bytes_max  = 64U,    /**< Maximum decoded PSK text bytes.   */
  k_ra8_net_provision_url_bytes_max  = 511U,   /**< Maximum decoded optional URL.     */
  k_ra8_net_provision_prefix_bytes   = 8U,     /**< Bytes in `RA8NET1:`.              */
  k_ra8_net_provision_line_bytes_max = 1225U,  /**< Longest complete encoded line.    */
  k_ra8_net_provision_timeout_ms     = 30000U, /**< Standard application RX timeout.  */
  k_ra8_net_provision_timeout_ms_max = 60000U, /**< Longest accepted receive timeout. */
} ra8_net_provision_limit_t;

/**
 * @struct ra8_net_credentials
 * @brief Decoded runtime network configuration
 * @details Owns NUL-terminated SSID, PSK, and optional URL storage so no field
 *          points back into the UART line buffer. Lengths exclude the trailing
 *          NUL and permit callers to retain binary-safe bounds.
 * @invariant `ssid_len` is in 1..32 after a successful parse.
 * @invariant `psk_len` is 8..63, or 64 with hexadecimal text.
 * @par Example:
 * @code
 * ra8_net_credentials_t credentials = {};
 * ra8_net_provision_clear(&credentials);
 * @endcode
 * @see ra8_net_provision_clear
 * @since 0.1.0
 */
typedef struct ra8_net_credentials {
  /** NUL-terminated SSID storage. */
  char ssid[k_ra8_net_provision_ssid_bytes_max + 1U];
  /** NUL-terminated PSK storage. */
  char psk[k_ra8_net_provision_psk_bytes_max + 1U];
  /** NUL-terminated optional URL storage. */
  char url[k_ra8_net_provision_url_bytes_max + 1U];
  /** Decoded URL bytes, excluding the NUL. */
  uint16_t url_len;
  /** Decoded SSID bytes, excluding the NUL. */
  uint8_t ssid_len;
  /** Decoded PSK bytes, excluding the NUL. */
  uint8_t psk_len;
} ra8_net_credentials_t;

/**
 * @typedef ra8_net_provision_uart_write_fn
 * @brief Injected UART write operation
 * @param[in] data Bytes to transmit; non-null when `length` is non-zero.
 * @param[in] length Number of bytes to transmit.
 * @return Repository error code from the UART implementation.
 */
typedef ra8_err_t (*ra8_net_provision_uart_write_fn)(const uint8_t* data, size_t length);

/**
 * @typedef ra8_net_provision_uart_read_fn
 * @brief Injected non-blocking UART read operation
 * @param[out] data Destination buffer.
 * @param[in] capacity Writable bytes in `data`.
 * @param[out] out_length Number of bytes drained during this call.
 * @return Repository error code from the UART implementation.
 */
typedef ra8_err_t (*ra8_net_provision_uart_read_fn)(uint8_t* data,
                                                    size_t   capacity,
                                                    size_t*  out_length);

/**
 * @typedef ra8_net_provision_wait_fn
 * @brief Injected millisecond wait used between incomplete UART polls
 * @param[in] delay_ms Delay interval in milliseconds.
 * @return Nothing.
 */
typedef void (*ra8_net_provision_wait_fn)(uint32_t delay_ms);

/**
 * @struct ra8_net_provision_uart
 * @brief UART and pacing operations used by the bounded receiver
 * @details Production binds these rows to the board console and time service;
 *          host tests bind deterministic recorders. The receiver never writes
 *          received bytes, so the interface cannot accidentally echo secrets.
 * @invariant Every operation is non-null before a receive begins.
 * @invariant `read` is non-blocking and reports at most its supplied capacity.
 * @par Example:
 * @code
 * const ra8_net_provision_uart_t uart = {
 *   .write = board_write, .read = board_read, .wait_ms = board_wait,
 * };
 * @endcode
 * @see ra8_net_provision_receive
 * @since 0.1.0
 */
typedef struct ra8_net_provision_uart {
  ra8_net_provision_uart_write_fn write;   /**< Non-secret prompt transmitter.    */
  ra8_net_provision_uart_read_fn  read;    /**< Non-blocking input drain.         */
  ra8_net_provision_wait_fn       wait_ms; /**< Incomplete-poll pacing operation. */
} ra8_net_provision_uart_t;

/**
 * @var k_ra8_net_provision_ready_prompt
 * @brief Exact non-secret line emitted before the receiver drains UART input
 * @details The HIL provisioner waits for this versioned prompt before sending
 *          one `RA8NET1` line to a freshly flashed credential-free image.
 * @note This constant never contains user or network data.
 * @since 0.1.0
 */
extern const char k_ra8_net_provision_ready_prompt[];

/**
 * @brief Parse one complete version-one ASCII-hex provisioning line
 *
 * @details
 * Accepts exactly `RA8NET1:<ssid_hex>:<psk_hex>:<url_hex>\n`. Each pair of
 * hexadecimal characters decodes to one output byte. SSID is required and at
 * most 32 bytes. PSK is 8..63 bytes, or exactly 64 hexadecimal characters.
 * URL is optional and at most 511 bytes. Decoded C0 and DEL control bytes are
 * rejected because the existing Wi-Fi and media APIs consume printable text.
 *
 * @param[in] line Complete line bytes, including the final newline.
 * @param[in] line_length Number of readable bytes at `line`.
 * @param[out] out Decoded record; cleared before parsing and on every failure.
 *
 * @return Repository error code.
 * @retval k_ra8_ok The complete line was valid and decoded.
 * @retval k_ra8_err_null_ptr `line` or `out` was null.
 * @retval k_ra8_err_invalid_size A line or decoded field exceeded its bound.
 * @retval k_ra8_err_protocol_error Prefix, separators, newline, or hex syntax was invalid.
 *
 * @pre `line` addresses `line_length` readable bytes when non-null.
 * @pre `out` addresses one writable credential record.
 * @post On success, every output field is bounded and NUL-terminated.
 * @post On failure, every byte of `out` is zero.
 *
 * @note Thread-safe for distinct input and output objects.
 * @warning Treat the input and successful output as secret-bearing memory.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_net_provision_parse(const uint8_t* line, size_t line_length, ra8_net_credentials_t* out);

/**
 * @brief Prompt once and receive one provisioning line within a fixed timeout
 *
 * @details
 * Writes `k_ra8_net_provision_ready_prompt`, then drains the injected
 * non-blocking UART into fixed stack storage. It never echoes input. Every
 * incomplete poll waits one millisecond; therefore `timeout_ms` is both the
 * poll limit and a lower bound on elapsed wait time. The budget is capped at
 * 60 seconds. The raw line buffer is explicitly zeroed before every return.
 *
 * @param[in] uart Complete UART operation table.
 * @param[in] timeout_ms Receive budget in milliseconds, 1..60000.
 * @param[out] out Decoded record; cleared before receive and on every failure.
 *
 * @return Repository error code.
 * @retval k_ra8_ok One valid line was received and decoded.
 * @retval k_ra8_err_null_ptr `uart`, an operation row, or `out` was null.
 * @retval k_ra8_err_invalid_arg `timeout_ms` was zero or above the fixed cap.
 * @retval k_ra8_err_invalid_size Input filled the fixed line buffer.
 * @retval k_ra8_err_timeout No complete line arrived within the budget.
 * @retval k_ra8_err_protocol_error The complete line was malformed.
 *
 * @pre The UART was initialized before this call.
 * @pre `read` is non-blocking and never reports more than its capacity.
 * @post The prompt was attempted before the first read.
 * @post Raw line storage is zeroed and failure leaves `out` zeroed.
 *
 * @note Not thread-safe when callers share one UART.
 * @warning The UART is a controlled-bench provisioning channel, not durable storage.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_net_provision_receive(const ra8_net_provision_uart_t* uart,
                                                  uint32_t                        timeout_ms,
                                                  ra8_net_credentials_t*          out);

/**
 * @brief Explicitly erase one decoded credential record
 * @details Uses the repository secure-memory primitive so optimization cannot
 *          discard the overwrite. A null record is accepted for cleanup paths
 *          that do not know whether provisioning completed.
 * @param[in,out] credentials Record to erase; null is a no-op.
 * @return Nothing.
 * @pre `credentials` is null or addresses a writable record.
 * @pre No concurrent consumer is reading the record.
 * @post Every byte in a non-null record is zero.
 * @post The erase cannot be removed as a dead store by optimization.
 * @note Call immediately after the final synchronous credential consumer.
 * @warning This does not erase copies already retained by another subsystem.
 * @since 0.1.0
 */
void ra8_net_provision_clear(ra8_net_credentials_t* credentials);
