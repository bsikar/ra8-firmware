/**
 * @file ra8_unarch_xz.h
 * @brief Bounded, fail-closed XZ/LZMA2 decoding over the vendored xz-embedded.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * The XZ leg of the archive-hardening subsystem, wrapping the vendored SOUP
 * decoder (`libs/third_party/xz_embedded/`, decode-only, 0BSD) behind two
 * bounded shapes:
 *
 * - ::ra8_unarch_xz_stream_begin / `_run` / `_end` -- a multi-call decode
 *   session in xz-embedded's `XZ_PREALLOC` mode. The decoder state and the
 *   LZMA2 dictionary are allocated once, up front, from the **caller's
 *   scratch buffer** through the zero-heap pool (`ra8_unarch_xz_pool.h`) and
 *   never grow: a stream whose declared dictionary exceeds the scratch is
 *   rejected fail-closed instead of allocating.
 * - ::ra8_unarch_xz_unwrap -- decode one whole `.xz` stream fetched through
 *   the shared seek+read seam (`ra8_unarch_io.h`) into a caller arena,
 *   charging every pass against the unified decompression-limits policy
 *   (`ra8_decomp_limits.h`): output cap, compression-ratio bound, and a
 *   per-loop iteration budget. This is the entry the wrapped comic/tar open
 *   paths use for `.tar.xz` content.
 *
 * Integrity is verified by the decoder (CRC32 and the `xz`(1) default
 * CRC64); a stream using an unsupported check or filter (SHA-256, BCJ,
 * delta) is rejected cleanly, never mis-decoded. Trailing bytes after the
 * stream footer are rejected (no multi-stream concatenation).
 *
 * @note Not thread-safe; the single-threaded reader loop serialises access
 *       (the XZ allocation pool is single-client by design).
 *
 * @see ra8_decomp_limits.h  The policy every decode is charged against.
 * @see ra8_unarch_io.h      The seek+read seam the unwrap consumes.
 * @see docs/SOUP/xz_embedded.md  Qualification record for the vendored tree.
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
#include "ra8_unarch_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum ra8_unarch_xz_dims_t
 * @brief Scratch sizing constants for the XZ decoder wrappers.
 * @details ::k_ra8_unarch_xz_state_reserve is the slice of a streaming
 *          session's scratch reserved for xz-embedded's decoder state
 *          (`struct xz_dec` plus the LZMA2 state, ~30 KiB measured;
 *          reserved with margin); the remainder becomes the preallocated
 *          LZMA2 dictionary, so `scratch_len - reserve` is the largest
 *          dictionary a stream may declare. ::k_ra8_unarch_xz_sig_len is
 *          the length of the XZ stream header magic used for container
 *          detection.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_unarch_xz_state_reserve = 64U * 1024U, /**< Decoder-state slice of the scratch. */
  k_ra8_unarch_xz_sig_len       = 6U,          /**< XZ stream header magic length.      */
} ra8_unarch_xz_dims_t;

/**
 * @struct ra8_unarch_xz_stream_t
 * @brief One multi-call XZ decode session (opaque decoder + liveness flag).
 *
 * @details Bound by ::ra8_unarch_xz_stream_begin over a caller scratch;
 *          `dec` points into that scratch (via the XZ pool) and is owned by
 *          the session until ::ra8_unarch_xz_stream_end. Treat the fields
 *          as read-only outside this module.
 *
 * @invariant `dec != NULL` exactly while `live` is true.
 * @invariant At most one session exists at a time (single-client pool).
 *
 * @par Example:
 * @code
 * ra8_unarch_xz_stream_t xs = {};
 * ra8_err_t err = ra8_unarch_xz_stream_begin(&xs, scratch, sizeof(scratch));
 * // ... feed via ra8_unarch_xz_stream_run ...
 * ra8_unarch_xz_stream_end(&xs);
 * @endcode
 *
 * @see ra8_unarch_xz_stream_begin()
 * @since Version 0.1.0
 */
typedef struct {
  struct xz_dec* dec;  /**< xz-embedded decoder state (pool-allocated). */
  bool           live; /**< Session is begun and not yet ended.         */
} ra8_unarch_xz_stream_t;

/**
 * @brief Whether @p sig begins with the XZ stream header magic.
 *
 * @details Pure signature probe over the six magic bytes
 *          (`FD 37 7A 58 5A 00`) so open paths can route a wrapped archive
 *          without constructing a decoder.
 *
 * @param[in] sig     Leading archive bytes (may be NULL).
 * @param[in] sig_len Readable length of @p sig in bytes.
 *
 * @return Whether the bytes begin an XZ stream.
 * @retval true  The six magic bytes match.
 * @retval false @p sig is NULL, too short, or not XZ.
 *
 * @pre @p sig holds @p sig_len readable bytes when non-NULL.
 * @pre @p sig_len is the true readable length (untrusted values are safe).
 * @post No state is modified (pure read).
 * @post The result depends only on the first six bytes.
 *
 * @note Thread-safe: pure read.
 * @see ra8_unarch_xz_unwrap()
 * @since Version 0.1.0
 */
[[nodiscard]] bool ra8_unarch_xz_magic(const uint8_t* sig, size_t sig_len);

/**
 * @brief Begin a multi-call XZ decode session over a caller scratch.
 *
 * @details Installs @p scratch as the XZ allocation arena and creates an
 *          `XZ_PREALLOC` decoder whose dictionary budget is
 *          `scratch_len - k_ra8_unarch_xz_state_reserve`: the dictionary is
 *          allocated once at init and never grows, and a stream declaring
 *          a larger dictionary is rejected during `_run` (no growth, no
 *          fallback).
 *
 * @param[out] xs          Session to bind (non-NULL).
 * @param[in]  scratch     Scratch buffer (non-NULL, 8-byte aligned).
 * @param[in]  scratch_len Scratch length, > ::k_ra8_unarch_xz_state_reserve.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Session live; feed it via `_run`.
 * @retval k_ra8_err_null_ptr     @p xs or @p scratch was NULL.
 * @retval k_ra8_err_invalid_size Scratch too small or misaligned.
 * @retval k_ra8_err_busy         Another XZ session is in flight.
 * @retval k_ra8_err_no_mem       Decoder-state allocation failed.
 *
 * @pre No other XZ session is live (single-client pool).
 * @pre @p scratch out-lives the session.
 * @post On k_ra8_ok, `xs->live` is true until ::ra8_unarch_xz_stream_end.
 * @post On any error the pool is released and @p xs is dead.
 *
 * @note Not thread-safe.
 * @see ra8_unarch_xz_stream_run()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_unarch_xz_stream_begin(ra8_unarch_xz_stream_t* xs, void* scratch, uint32_t scratch_len);

/**
 * @brief Feed one input chunk through a live session, producing output.
 *
 * @details One `xz_dec_run` pass: consumes up to @p in_len input bytes and
 *          writes up to @p out_cap output bytes, reporting both counts and
 *          whether the stream ended. The caller loops -- charging its own
 *          ::ra8_decomp_budget_t per pass -- until `*end` or an error.
 *          Returning with zero consumed and zero produced is legal decoder
 *          behaviour on tiny buffers; the caller's iteration budget bounds
 *          the loop regardless (the decoder itself also reports a stuck
 *          stream as a validation failure).
 *
 * @param[in,out] xs       Live session (non-NULL).
 * @param[in]     in       Input chunk (non-NULL when @p in_len > 0).
 * @param[in]     in_len   Input bytes available.
 * @param[out]    in_used  Receives input bytes consumed (non-NULL).
 * @param[out]    out      Output chunk buffer (non-NULL).
 * @param[in]     out_cap  Output capacity in bytes.
 * @param[out]    out_used Receives output bytes produced (non-NULL).
 * @param[out]    end      Receives true when the stream verified and ended
 *                         (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Pass completed; inspect the counts.
 * @retval k_ra8_err_null_ptr          A required pointer was NULL.
 * @retval k_ra8_err_invalid_state     @p xs is not live.
 * @retval k_ra8_err_no_mem            Declared dictionary exceeds the
 *                                     session's scratch budget.
 * @retval k_ra8_err_not_supported     Unsupported format/check/filter.
 * @retval k_ra8_err_validation_failed Corrupt / truncated stream, a failed
 *                                     integrity check, or a stuck stream.
 *
 * @pre ::ra8_unarch_xz_stream_begin succeeded on @p xs.
 * @pre @p out holds @p out_cap writable bytes.
 * @post On k_ra8_ok, `out[0..*out_used)` holds newly decoded bytes.
 * @post On any error the session must be ended (it is not recoverable).
 *
 * @note Not thread-safe.
 * @see ra8_unarch_xz_stream_end()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_unarch_xz_stream_run(ra8_unarch_xz_stream_t* xs,
                                                 const uint8_t*          in,
                                                 size_t                  in_len,
                                                 size_t*                 in_used,
                                                 uint8_t*                out,
                                                 size_t                  out_cap,
                                                 size_t*                 out_used,
                                                 bool*                   end);

/**
 * @brief End a session: free the decoder and release the allocation pool.
 *
 * @details Safe on a NULL or never-begun session (teardown paths call it
 *          unconditionally). After this the caller's scratch is dead
 *          storage again and a new session may begin.
 *
 * @param[in,out] xs Session to end (may be NULL).
 *
 * @pre @p xs, when live, owns the installed pool.
 * @pre No further `_run` calls follow on @p xs.
 * @post `xs->live` is false and the pool is uninstalled.
 * @post A subsequent ::ra8_unarch_xz_stream_begin may reuse the scratch.
 *
 * @note Not thread-safe.
 * @see ra8_unarch_xz_stream_begin()
 * @since Version 0.1.0
 */
void ra8_unarch_xz_stream_end(ra8_unarch_xz_stream_t* xs);

/**
 * @brief Decode one whole XZ stream from a read seam into a caller arena.
 *
 * @details Runs a full streaming session over `[0, size)` of the backing:
 *          input is fetched in small fixed chunks, each decode pass is
 *          charged against @p limits (iteration budget, output cap, ratio
 *          bound), and the stream's integrity check is verified before
 *          success. Fail-closed on everything else: truncation, corruption,
 *          an unsupported check/filter, a dictionary larger than the
 *          scratch budget, output overrun, and trailing bytes after the
 *          stream footer.
 *
 * @param[in]  read        Byte reader over the XZ stream (non-NULL).
 * @param[in]  ctx         Context passed to @p read.
 * @param[in]  size        Stream length in bytes (> 0).
 * @param[out] out         Destination arena (non-NULL, @p out_cap bytes).
 * @param[in]  out_cap     Capacity of @p out in bytes (> 0).
 * @param[in]  scratch     Session scratch (non-NULL, 8-byte aligned,
 *                         > ::k_ra8_unarch_xz_state_reserve bytes).
 * @param[in]  scratch_len Scratch length in bytes.
 * @param[in]  limits      Policy to enforce, or NULL for the default.
 * @param[out] out_len     Receives the decoded byte count (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Stream decoded and verified.
 * @retval k_ra8_err_null_ptr          A required pointer was NULL.
 * @retval k_ra8_err_invalid_size      @p size / @p out_cap is 0, or the
 *                                     scratch is undersized / misaligned.
 * @retval k_ra8_err_invalid_arg       @p limits has a zero field.
 * @retval k_ra8_err_busy              Another XZ session is in flight.
 * @retval k_ra8_err_no_mem            Decoder-state allocation failed, the
 *                                     declared dictionary exceeds the
 *                                     scratch budget, or @p out is too
 *                                     small for the stream.
 * @retval k_ra8_err_not_supported     Not an XZ stream, or one using an
 *                                     unsupported check/filter.
 * @retval k_ra8_err_validation_failed Corrupt / truncated / trailing-byte
 *                                     stream or a failed integrity check.
 * @retval k_ra8_err_decomp_output_cap Output exceeds the policy cap.
 * @retval k_ra8_err_decomp_ratio      Output breached the ratio bound.
 * @retval k_ra8_err_decomp_iterations The decode loop budget ran out.
 *
 * @pre No other XZ session is live (single-threaded reader loop).
 * @pre @p read serves offsets `[0, size)` of the stream.
 * @post On k_ra8_ok, `out[0..*out_len)` holds the decoded bytes.
 * @post On any error `*out_len == 0`; the pool is released either way.
 *
 * @note Not thread-safe.
 * @see ra8_unarch_xz_stream_run()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_unarch_xz_unwrap(ra8_unarch_read_fn         read,
                                             void*                      ctx,
                                             uint64_t                   size,
                                             uint8_t*                   out,
                                             size_t                     out_cap,
                                             void*                      scratch,
                                             uint32_t                   scratch_len,
                                             const ra8_decomp_limits_t* limits,
                                             size_t*                    out_len);

#ifdef __cplusplus
}
#endif
