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

#include "ra_da16600.h"
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
  k_ra_da16600_capture_buf_bytes = 256U,  /**< Per-command response capture.    */
  k_ra_da16600_payload_max_bytes = 1460U, /**< One TCP MSS. UM-WI-046 5.2.5.    */
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
  k_ra_da16600_u32_str_bytes = 11U, /**< Digits + NUL terminator.          */
  k_ra_da16600_decimal_base  = 10U, /**< Decimal radix for the formatter.  */
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

/**
 * @brief Build the ``AT+WFJAP=<ssid>,4,<key>`` Wi-Fi-connect command.
 *
 * @details Formats the four fixed/variable pieces of the WFJAP command with
 * the bounded appender. Security mode 4 ("WPA2-PSK") is hard-coded per
 * UM-WI-046 Table 4.5-1. Promoted from TU-private @c static linkage so the
 * append-overflow OR chain can be exercised for MC/DC with a caller-chosen
 * @p cmd_cap (the production 96-byte buffer never overflows on the length-
 * validated ssid/passkey, so the prefix/ssid/separator arms are not otherwise
 * reachable); defined in @c ra_da16600.c.
 *
 * @param[out] cmd     Caller-owned command buffer.
 * @param[in]  cmd_cap Capacity of @p cmd in bytes (incl. NUL).
 * @param[in]  ssid    NUL-terminated SSID.
 * @param[in]  passkey NUL-terminated WPA2 passphrase.
 *
 * @return ::ra_err_t
 * @retval k_ra_ok              Command formatted into @p cmd.
 * @retval k_ra_err_invalid_size One of the four concatenations overflowed.
 *
 * @pre @p cmd, @p ssid, @p passkey are non-NULL.
 * @pre @p cmd_cap >= 1.
 * @post On success, @p cmd is a NUL-terminated AT command line.
 * @post On overflow, @p cmd is NUL-terminated at @c cmd_cap-1.
 * @note Not thread-safe; caller must serialise driver access.
 * @note Test-access only outside the defining TU.
 *
 * @par MC/DC:
 * Decision (4 conditions, OR over ``ra_da16600_strcat_bounded(...) == 0``):
 *  - V1: cmd_cap large -> F,F,F,F -> false (all appends fit; k_ra_ok).
 *  - V2: cmd_cap < len("AT+WFJAP=")            -> T,.,.,. (prefix append fails).
 *  - V3: cmd_cap fits prefix only              -> F,T,.,. (ssid append fails).
 *  - V4: cmd_cap fits prefix+ssid only         -> F,F,T,. (",4," append fails).
 *  - V5: cmd_cap fits prefix+ssid+",4," only   -> F,F,F,T (passkey append fails).
 * N+1 = 5 vectors for N=4 conditions: minimal MC/DC.
 *
 * @since 0.1.0
 */
ra_err_t
ra_da16600_build_wfjap_cmd(char* cmd, size_t cmd_cap, const char* ssid, const char* passkey);

/**
 * @brief Build the ``AT+TRTS=<port>`` (listen) or ``AT+TRTC=<ip>,<port>``
 *        (connect) TCP-open command.
 *
 * @details UM-WI-046 sections 5.2.3 (listen) / 5.2.4 (connect) define the two
 * command shapes. Promoted from TU-private @c static linkage so both the
 * 2-condition listen OR and the 4-condition connect OR append-overflow chains
 * can be exercised for MC/DC with a caller-chosen @p cmd_cap; defined in
 * @c ra_da16600_socket.c.
 *
 * @param[out] cmd       Caller-owned command buffer.
 * @param[in]  cmd_cap   Capacity of @p cmd in bytes.
 * @param[in]  role      Listen vs connect role.
 * @param[in]  remote_ip Dotted-decimal IPv4 (connect role only).
 * @param[in]  port      TCP port in host byte order.
 *
 * @return ::ra_err_t
 * @retval k_ra_ok              Command formatted into @p cmd.
 * @retval k_ra_err_invalid_size A concatenation overflowed @p cmd_cap.
 *
 * @pre @p cmd is non-NULL and @p cmd_cap >= 1.
 * @pre When @p role is connect, @p remote_ip is non-NULL.
 * @post On success, @p cmd is a NUL-terminated AT command line.
 * @post On overflow, @p cmd is NUL-terminated at @c cmd_cap-1.
 * @note Not thread-safe; caller must serialise driver access.
 * @note Test-access only outside the defining TU.
 *
 * @par MC/DC:
 * Listen OR (2 conditions): V1 all-fit F,F; V2 prefix-append T; V3
 * port-append F,T. Connect OR (4 conditions): V1 all-fit F,F,F,F; V2 "AT+TRTC="
 * append T; V3 remote_ip append F,T; V4 "," append F,F,T; V5 port append
 * F,F,F,T. Each arm reached by sizing @p cmd_cap so exactly that append fails.
 *
 * @since 0.1.0
 */
ra_err_t ra_da16600_build_tcp_open_cmd(char*                    cmd,
                                       size_t                   cmd_cap,
                                       ra_da16600_socket_role_t role,
                                       const char*              remote_ip,
                                       uint16_t                 port);

/**
 * @brief Build the ``AT+TRDTS=<cid>,<len>,`` send-data header (no payload).
 *
 * @details UM-WI-046 section 5.2.5 defines the header layout; the raw binary
 * payload is appended by the caller after this helper returns. @p out_off is
 * the new write offset. Promoted from TU-private @c static linkage so both the
 * 3-condition and 2-condition append-overflow OR chains can be exercised for
 * MC/DC with a caller-chosen @p cmd_cap; defined in @c ra_da16600_socket.c.
 *
 * @param[out] cmd     Caller-owned command buffer.
 * @param[in]  cmd_cap Capacity of @p cmd in bytes.
 * @param[in]  sock    TCP socket / cid.
 * @param[in]  len     Payload length in bytes.
 * @param[out] out_off Final write offset into @p cmd.
 *
 * @return ::ra_err_t
 * @retval k_ra_ok              Header formatted, @p *out_off updated.
 * @retval k_ra_err_invalid_size A concatenation overflowed @p cmd_cap.
 *
 * @pre @p cmd and @p out_off are non-NULL.
 * @pre @p cmd_cap >= 1.
 * @post On success, @p cmd[*out_off] is NUL and the header is complete.
 * @post On overflow, @p cmd is NUL-terminated and @p *out_off is 0.
 * @note Not thread-safe; caller must serialise driver access.
 * @note Test-access only outside the defining TU.
 *
 * @par MC/DC:
 * First OR (3 conditions, "AT+TRDTS=" / cid / ","): V1 all-fit F,F,F; V2
 * prefix T; V3 cid F,T; V4 "," F,F,T. Second OR (2 conditions, len / ","):
 * V1 all-fit F,F; V2 len-append T; V3 "," F,T. Each arm reached by sizing
 * @p cmd_cap so exactly that append fails.
 *
 * @since 0.1.0
 */
ra_err_t ra_da16600_build_trdts_header(char*               cmd,
                                       size_t              cmd_cap,
                                       ra_da16600_socket_t sock,
                                       size_t              len,
                                       size_t*             out_off);

/**
 * @brief Build the ``AT+TRTRM=<cid>`` terminate-session command.
 *
 * @details UM-WI-046 section 5.2.7 defines the command. Extracted from
 * ::ra_da16600_tcp_close and promoted from an inline build to TU-external
 * linkage so its 2-condition append-overflow OR chain can be exercised for
 * MC/DC with a caller-chosen @p cmd_cap; defined in @c ra_da16600_socket.c.
 *
 * @param[out] cmd     Caller-owned command buffer.
 * @param[in]  cmd_cap Capacity of @p cmd in bytes.
 * @param[in]  sock    TCP socket / cid to terminate.
 *
 * @return ::ra_err_t
 * @retval k_ra_ok              Command formatted into @p cmd.
 * @retval k_ra_err_invalid_size A concatenation overflowed @p cmd_cap.
 *
 * @pre @p cmd is non-NULL and @p cmd_cap >= 1.
 * @pre None beyond the pointer/capacity contract.
 * @post On success, @p cmd is a NUL-terminated AT command line.
 * @post On overflow, @p cmd is NUL-terminated at @c cmd_cap-1.
 * @note Not thread-safe; caller must serialise driver access.
 * @note Test-access only outside the defining TU.
 *
 * @par MC/DC:
 * Decision (2 conditions, OR over ``ra_da16600_strcat_bounded(...) == 0``):
 *  - V1: cmd_cap large             -> F,F -> false (k_ra_ok).
 *  - V2: cmd_cap < len("AT+TRTRM=") -> T,. (prefix append fails).
 *  - V3: cmd_cap fits prefix only   -> F,T (cid append fails).
 * N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 *
 * @since 0.1.0
 */
ra_err_t ra_da16600_build_trtrm_cmd(char* cmd, size_t cmd_cap, ra_da16600_socket_t sock);

#ifdef __cplusplus
}
#endif
