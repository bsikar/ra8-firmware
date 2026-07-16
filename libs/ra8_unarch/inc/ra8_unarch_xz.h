/**
 * @file ra8_unarch_xz.h
 * @brief Bounded, fail-closed XZ/LZMA2 decoding over the vendored xz-embedded.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * The XZ leg of the archive-hardening subsystem. Two shapes over one SOUP
 * decoder (`libs/third_party/xz_embedded/`, decode-only, 0BSD):
 *
 * - ::ra8_unarch_xz_decode -- one-shot member decode (whole input buffer ->
 *   whole output buffer) in xz-embedded's `XZ_SINGLE` mode, where the output
 *   buffer doubles as the LZMA2 dictionary so **no dictionary allocation
 *   exists at all**. Used for policy-bounded `.xz`-wrapped single files.
 * - ::ra8_unarch_xz_stream_begin / `_run` / `_restart` / `_end` -- a
 *   multi-call session in `XZ_PREALLOC` mode for the seekable stream view
 *   over `.tar.xz` content (`ra8_unarch_stream.h`). The dictionary is
 *   allocated once, up front, from the **caller's scratch buffer** through
 *   the zero-heap pool (`ra8_unarch_xz_pool.h`) and never grows: a stream
 *   whose declared dictionary exceeds the scratch is rejected fail-closed
 *   instead of allocating.
 *
 * Every decode is charged against the unified decompression-limits policy
 * (`ra8_decomp_limits.h`): output cap, compression-ratio bound, and a
 * per-loop iteration budget. Integrity is verified (CRC32 and the `xz`(1)
 * default CRC64); a stream using an unsupported check or filter (SHA-256,
 * BCJ) is rejected cleanly, never mis-decoded.
 *
 * @note Not thread-safe; the single-threaded reader loop serialises access
 *       (the XZ allocation pool is single-client by design).
 *
 * @see ra8_decomp_limits.h  The policy every decode is charged against.
 * @see ra8_unarch_stream.h  The seekable view built on the streaming session.
 * @see docs/SOUP/xz_embedded.md  Qualification record for the vendored tree.
 *
 * @since Version 0.1.0
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_decomp_limits.h"
#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum ra8_unarch_xz_dims_t
 * @brief Scratch sizing constants for the XZ decoder wrappers.
 * @details ::k_ra8_unarch_xz_state_reserve is the slice of a streaming
 *          session's scratch reserved for xz-embedded's decoder state
 *          (`struct xz_dec`, ~30 KiB measured; reserved with margin); the
 *          remainder becomes the preallocated LZMA2 dictionary, so
 *          `scratch_len - reserve` is the largest dictionary a stream may
 *          declare. ::k_ra8_unarch_xz_scratch_min is the smallest scratch
 *          either wrapper accepts.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_unarch_xz_state_reserve = 64U * 1024U, /**< Decoder-state slice of the scratch. */
  k_ra8_unarch_xz_scratch_min   = 64U * 1024U, /**< Minimum accepted scratch length.    */
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
 * @see ra8_unarch_xz_stream_begin()
 * @since Version 0.1.0
 */
typedef struct {
  struct xz_dec* dec;  /**< xz-embedded decoder state (pool-allocated). */
  bool           live; /**< Session is begun and not yet ended.         */
} ra8_unarch_xz_stream_t;

/**
 * @brief One-shot bounded XZ decode: whole @p src into @p out.
 *
 * @details Runs xz-embedded in `XZ_SINGLE` mode (output buffer == LZMA2
 *          dictionary; zero dictionary allocation). The decoder state is
 *          bump-allocated from @p scratch and released before return. The
 *          decode is capped at `min(out_cap, policy output cap)` and the
 *          result is charged against the policy's ratio bound, so a bomb
 *          is rejected without unbounded RAM or CPU. Stream integrity
 *          (CRC32/CRC64) is verified by the decoder; SHA-256-checked or
 *          BCJ-filtered streams are rejected as unsupported.
 *
 * @param[in]  src         XZ stream bytes (non-NULL).
 * @param[in]  src_len     Length of @p src in bytes (> 0).
 * @param[out] out         Destination buffer (non-NULL, @p out_cap bytes).
 * @param[in]  out_cap     Capacity of @p out in bytes (> 0).
 * @param[in]  scratch     Decoder-state scratch (non-NULL, 8-byte aligned).
 * @param[in]  scratch_len Scratch length, >= ::k_ra8_unarch_xz_scratch_min.
 * @param[in]  limits      Policy to enforce, or NULL for the default.
 * @param[out] out_len     Receives the decoded byte count (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Stream decoded and verified.
 * @retval k_ra8_err_null_ptr          A required pointer was NULL.
 * @retval k_ra8_err_invalid_size      A zero length / undersized or
 *                                     misaligned scratch.
 * @retval k_ra8_err_busy              Another XZ decode is in flight.
 * @retval k_ra8_err_no_mem            Scratch too small for the decoder
 *                                     state, or @p out too small for the
 *                                     stream.
 * @retval k_ra8_err_not_supported     Not an XZ stream, or one using an
 *                                     unsupported check/filter.
 * @retval k_ra8_err_validation_failed Corrupt / truncated stream or a
 *                                     failed integrity check.
 * @retval k_ra8_err_decomp_output_cap Output would exceed the policy cap.
 * @retval k_ra8_err_decomp_ratio     Output breached the ratio bound.
 *
 * @pre No other XZ session is live (single-threaded reader loop).
 * @pre @p scratch is aligned per `ra8_unarch_xz_pool.h`.
 * @post On k_ra8_ok, `out[0..*out_len)` holds the decoded bytes.
 * @post On any error `*out_len == 0`; the pool is released either way.
 *
 * @note Not thread-safe.
 * @see ra8_unarch_xz_stream_begin()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_unarch_xz_decode(const uint8_t*             src,
                                             size_t                     src_len,
                                             uint8_t*                   out,
                                             size_t                     out_cap,
                                             void*                      scratch,
                                             uint32_t                   scratch_len,
                                             const ra8_decomp_limits_t* limits,
                                             size_t*                    out_len);

/**
 * @brief Begin a multi-call XZ decode session over a caller scratch.
 *
 * @details Installs @p scratch as the XZ allocation arena and creates an
 *          `XZ_PREALLOC` decoder whose dictionary budget is
 *          `scratch_len - k_ra8_unarch_xz_state_reserve`: the dictionary is
 *          allocated once when the stream header declares its size, and a
 *          stream declaring more is rejected (no growth, no fallback).
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
 *          the loop regardless.
 *
 * @param[in,out] xs       Live session (non-NULL).
 * @param[in]     in       Input chunk (non-NULL when @p in_len > 0).
 * @param[in]     in_len   Input bytes available.
 * @param[out]    in_used  Receives input bytes consumed (non-NULL).
 * @param[out]    out      Output chunk buffer (non-NULL).
 * @param[in]     out_cap  Output capacity in bytes (> 0).
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
 * @retval k_ra8_err_validation_failed Corrupt / truncated stream or a
 *                                     failed integrity check.
 *
 * @pre ::ra8_unarch_xz_stream_begin succeeded on @p xs.
 * @pre @p out holds @p out_cap writable bytes.
 * @post On k_ra8_ok, `out[0..*out_used)` holds newly decoded bytes.
 * @post On any error the session must be ended (it is not recoverable).
 *
 * @note Not thread-safe.
 * @see ra8_unarch_xz_stream_restart()
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
 * @brief Rewind a live session to decode the same stream from byte 0 again.
 *
 * @details `xz_dec_reset` without freeing: the preallocated dictionary and
 *          state are kept, so the seekable stream view can restart the
 *          decode on a backward seek at zero allocation cost.
 *
 * @param[in,out] xs Live session (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Session rewound to stream start.
 * @retval k_ra8_err_null_ptr      @p xs was NULL.
 * @retval k_ra8_err_invalid_state @p xs is not live.
 *
 * @pre ::ra8_unarch_xz_stream_begin succeeded on @p xs.
 * @pre The caller restarts its input feed from offset 0.
 * @post The next `_run` expects the stream header again.
 * @post No allocation state changed.
 *
 * @note Not thread-safe.
 * @see ra8_unarch_xz_stream_run()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_unarch_xz_stream_restart(ra8_unarch_xz_stream_t* xs);

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

#ifdef __cplusplus
}
#endif
