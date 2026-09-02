/**
 * @file unarch_gzip.h
 * @brief Clean-room gzip member decoder (RFC 1952) over the miniz DEFLATE core.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * The gzip leg of the archive-hardening subsystem: a first-party RFC 1952
 * container parser wrapped around the vendored miniz `tinfl` DEFLATE
 * decompressor (the same SOUP core the ZIP / RBKC paths already use).
 * ::unarch_gzip_unwrap decodes exactly one gzip member fetched through
 * the shared seek+read seam (`unarch_io.h`) into a caller arena:
 *
 * - The header is parsed fail-closed: magic, DEFLATE compression method,
 *   reserved flag bits rejected, bounded FEXTRA / FNAME / FCOMMENT fields,
 *   and the optional FHCRC header checksum verified when present.
 * - The DEFLATE stream is inflated in bounded passes, each charged against
 *   the unified decompression-limits policy (`ra8_decomp_limits.h`):
 *   output cap, compression-ratio bound, iteration budget.
 * - The trailer's CRC32 and ISIZE are both verified against the produced
 *   bytes, and trailing bytes after the member are rejected (no
 *   multi-member concatenation).
 *
 * @par Clean-room licensing
 * The container grammar is written from RFC 1952; only the DEFLATE
 * bitstream itself is delegated to the vendored miniz (see
 * `docs/SOUP/miniz.md`). This wrapper is first-party MIT.
 *
 * @note Not thread-safe: the DEFLATE state is a module-static single-client
 *       arena (zero heap, NASA P10 Rule 3), serialised by the
 *       single-threaded reader loop.
 *
 * @see ra8_decomp_limits.h  The policy every decode is charged against.
 * @see unarch_io.h      The seek+read seam the decoder consumes.
 * @see unarch_tar.h     The tar walker layered over unwrapped bytes.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 *
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_decomp_limits.h"
#include "ra8_err.h"
#include "unarch_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum unarch_gzip_dims_t
 * @brief Grammar sizes and fail-closed bounds of the gzip decoder.
 * @details The fixed sizes come from RFC 1952; the string bound is this
 *          decoder's fail-closed limit on the optional NUL-terminated
 *          FNAME / FCOMMENT fields (a hostile header cannot stall the
 *          parser with an unterminated string).
 * @since Version 0.1.0
 */
typedef enum : uint16_t {
  k_unarch_gzip_sig_len = 2U,    /**< Magic length (1F 8B).                */
  k_unarch_gzip_str_max = 2048U, /**< Max FNAME / FCOMMENT bytes accepted. */
} unarch_gzip_dims_t;

/**
 * @brief Whether @p sig begins with the gzip member magic (`1F 8B`).
 *
 * @details Pure signature probe so open paths can route a wrapped archive
 *          without constructing a decoder.
 *
 * @param[in] sig     Leading archive bytes (may be NULL).
 * @param[in] sig_len Readable length of @p sig in bytes.
 *
 * @return Whether the bytes begin a gzip member.
 * @retval true  Both magic bytes match.
 * @retval false @p sig is NULL, too short, or not gzip.
 *
 * @pre @p sig holds @p sig_len readable bytes when non-NULL.
 * @pre @p sig_len is the true readable length (untrusted values are safe).
 * @post No state is modified (pure read).
 * @post The result depends only on the first two bytes.
 *
 * @note Thread-safe: pure read.
 * @see unarch_gzip_unwrap()
 * @since Version 0.1.0
 */
[[nodiscard]] bool unarch_gzip_magic(const uint8_t* sig, size_t sig_len);

/**
 * @brief Decode one whole gzip member from a read seam into a caller arena.
 *
 * @details Parses the RFC 1952 header, inflates the DEFLATE stream in
 *          bounded passes charged against @p limits, and verifies the
 *          trailer (payload CRC32 + ISIZE). Fail-closed on everything
 *          else: a non-gzip or non-DEFLATE header, reserved flag bits,
 *          oversized name/comment fields, a failed header or payload
 *          checksum, truncation, corruption, output overrun, a policy
 *          breach, and trailing bytes after the member.
 *
 * @param[in]  read    Byte reader over the gzip member (non-NULL).
 * @param[in]  ctx     Context passed to @p read.
 * @param[in]  size    Member length in bytes (> 0).
 * @param[out] out     Destination arena (non-NULL, @p out_cap bytes).
 * @param[in]  out_cap Capacity of @p out in bytes (> 0).
 * @param[in]  limits  Policy to enforce, or NULL for the default.
 * @param[out] out_len Receives the decoded byte count (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Member decoded and verified.
 * @retval k_ra8_err_null_ptr          A required pointer was NULL.
 * @retval k_ra8_err_invalid_size      @p size or @p out_cap is 0.
 * @retval k_ra8_err_invalid_arg       @p limits has a zero field.
 * @retval k_ra8_err_not_supported     Not gzip, not DEFLATE, or reserved
 *                                     flag bits set.
 * @retval k_ra8_err_no_mem            @p out is too small for the member.
 * @retval k_ra8_err_checksum_mismatch Header FHCRC, payload CRC32, or
 *                                     ISIZE verification failed.
 * @retval k_ra8_err_validation_failed Truncated / corrupt stream,
 *                                     oversized header field, or trailing
 *                                     bytes after the member.
 * @retval k_ra8_err_decomp_output_cap Output exceeds the policy cap.
 * @retval k_ra8_err_decomp_ratio      Output breached the ratio bound.
 * @retval k_ra8_err_decomp_iterations The decode loop budget ran out.
 *
 * @pre @p read serves offsets `[0, size)` of the member.
 * @pre No other gzip decode is in flight (module-static DEFLATE state).
 * @post On k_ra8_ok, `out[0..*out_len)` holds the decoded bytes.
 * @post On any error `*out_len == 0`.
 *
 * @note Not thread-safe (single-client DEFLATE state).
 * @see unarch_gzip_magic()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t unarch_gzip_unwrap(unarch_read_fn             read,
                                           void*                      ctx,
                                           uint64_t                   size,
                                           uint8_t*                   out,
                                           size_t                     out_cap,
                                           const ra8_decomp_limits_t* limits,
                                           size_t*                    out_len);

#ifdef __cplusplus
}
#endif
