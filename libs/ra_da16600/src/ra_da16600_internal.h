/**
 * @file ra_da16600_internal.h
 * @brief TU-shared surface for the DA16600 driver implementation.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Not part of the public API. The DA16600 driver implementation is split
 * across two translation units -- @c ra_da16600.c (lifecycle + Wi-Fi) and
 * @c ra_da16600_socket.c (TCP sockets + BLE) -- to stay under the
 * per-file line-count gate. This header carries the handful of helpers
 * and constants those two TUs share: the logging tag, the stack-buffer
 * sizing enums, and the three TU-private helpers that were promoted from
 * @c static to external linkage so the socket TU can reuse them.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra_err.h"

/**
 * @def RA_DA16600_TAG
 * @brief Logging tag used for every ``ra_log_error`` call in the driver.
 */
#define RA_DA16600_TAG "DA16600"

/**
 * @brief Compile-time sized stack-local capture buffers used in the driver.
 *
 * @details
 * All sizes are typed enums so the buffer-size sweep tool can prove
 * each function's local-frame budget without preprocessor expansion.
 */
typedef enum : uint16_t {
  k_ra_da16600_cmd_buf_bytes     = 96U,   /**< Per-command AT line buffer (TX). */
  k_ra_da16600_capture_buf_bytes = 256U,  /**< Per-command response capture. */
  k_ra_da16600_payload_max_bytes = 1460U, /**< One TCP MSS. UM-WI-046 5.2.5. */
} ra_da16600_internal_caps_t;

/**
 * @brief Stack-buffer sizing for decimal uint32_t formatting.
 *
 * @details
 * The widest decimal representation of a @c uint32_t is the literal
 * @c "4294967295" -- ten characters. Adding one trailing NUL puts the
 * total at eleven bytes. These two constants are typed enums so the
 * compiler picks a stable 8-bit width and the values appear by name in
 * the disassembly / debugger, satisfying NASA P10 Rule 8 and the
 * project-wide ``readability-magic-numbers`` lint gate.
 */
typedef enum : uint8_t {
  k_ra_da16600_u32_digit_max = 10U, /**< Max decimal digits in a uint32_t. */
  k_ra_da16600_u32_str_bytes = 11U, /**< Digits + NUL terminator. */
  k_ra_da16600_decimal_base  = 10U, /**< Decimal radix for the formatter. */
} ra_da16600_format_caps_t;

/**
 * @brief Append @p src to @p dst, NUL-terminate, bounded by @p cap.
 *
 * @details Strict-bounded variant of @c strcat that uses an explicit
 * offset cursor instead of re-scanning @p dst on every call. Used by
 * the AT-command formatters in both driver TUs to build a multi-piece
 * command line without ever pulling in @c <string.h>.
 *
 * Promoted from TU-private @c static linkage so the socket TU can reuse
 * the same bounded append; defined in @c ra_da16600.c.
 *
 * @param[in,out] dst Destination buffer (NUL-terminated on exit).
 * @param[in]     cap Capacity of @p dst including NUL.
 * @param[in,out] off In: current write offset. Out: updated offset.
 * @param[in]     src NUL-terminated string to append.
 *
 * @return Status flag.
 * @retval 1 Appended successfully; @p *off advanced past @p src.
 * @retval 0 Appending would overflow; @p dst forcibly NUL-terminated.
 *
 * @pre @p dst, @p off, @p src non-NULL.
 * @pre @p cap >= 1.
 * @post On success, ``dst[*off] == '\0'``.
 * @post On overflow, ``dst[cap-1] == '\0'`` and result is 0.
 *
 * @note Not thread-safe; caller must serialise driver access.
 * @since 0.1.0
 */
uint8_t ra_da16600_strcat_bounded(char* dst, size_t cap, size_t* off, const char* src);

/**
 * @brief Format a decimal uint32_t into @p dst, NUL-terminated.
 *
 * @details Avoids @c snprintf so the driver stays libc-free for the
 * target build. Writes at most ::k_ra_da16600_u32_str_bytes bytes
 * ("4294967295" + NUL).
 *
 * Promoted from TU-private @c static linkage so the socket TU can format
 * cid / length fields; defined in @c ra_da16600.c.
 *
 * @param[out] dst    Destination; must hold at least
 *                    ::k_ra_da16600_u32_str_bytes bytes.
 * @param[in]  value  Value to format.
 *
 * @pre @p dst is non-NULL.
 * @pre @p dst points to at least ::k_ra_da16600_u32_str_bytes bytes
 *      of writable storage.
 * @post @c dst is NUL-terminated.
 * @post Every byte written is a printable ASCII digit ('0'..'9').
 *
 * @note Pure function; safe to call concurrently.
 * @since 0.1.0
 */
void ra_da16600_format_u32(char* dst, uint32_t value);

/**
 * @brief Verify driver was initialized before any command call.
 *
 * @details Gate used by every public command entry point to reject
 * calls that arrived before ::ra_da16600_init returned ::k_ra_ok.
 * Keeps the per-function pre-condition logic concentrated in one
 * place.
 *
 * Promoted from TU-private @c static linkage so the socket TU shares the
 * single module-state gate; defined in @c ra_da16600.c.
 *
 * @return ::ra_err_t
 * @retval k_ra_ok                  Driver has been initialised.
 * @retval k_ra_err_not_initialized ::ra_da16600_init has not yet run.
 *
 * @pre The module state has been written by ::ra_da16600_init or
 *      zero-initialised.
 * @pre Caller is on the single owner thread (driver is single-instance).
 * @post No state mutated.
 * @post Return value is exactly one of the documented retvals.
 *
 * @note Not thread-safe; caller must serialise driver access.
 * @since 0.1.0
 */
ra_err_t ra_da16600_require_init(void);

#ifdef __cplusplus
}
#endif
