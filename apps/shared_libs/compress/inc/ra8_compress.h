/**
 * @file ra8_compress.h
 * @brief App-domain DEFLATE compress / decompress (zero-heap, firmware-safe).
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * Raw-DEFLATE (RFC 1951, no zlib/gzip header) buffer compression over the
 * vendored miniz, sized for the fabric's `.rabook` RBKC chunk streams and any
 * compress-on-write / decompress-on-read path.
 *
 * Both directions are heap-free, which matters because this firmware traps the
 * allocator (`_sbrk`). Decompression uses `tinfl_decompress_mem_to_mem`, whose
 * working state lives on miniz's own (SOUP-exempt) stack. Compression needs a
 * ~100 KiB `tdefl_compressor`, which the **caller provides** as a scratch buffer
 * -- so an app that never compresses pays no RAM, and the caller chooses where
 * the buffer lives (SRAM or SDRAM). The buffer must be at least
 * ::k_ra8_compress_scratch_bytes and 8-byte aligned.
 *
 * This header is opt-in: it pulls in miniz, so it is NOT part of the `ra8_io.h`
 * umbrella. Apps that compress include it directly and add `miniz` to their
 * `ra8_add_app(... LIBS ...)`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "miniz.h"
#include "ra8_err.h"

/**
 * @enum ra8_compress_const_t
 * @brief Compression scratch sizing.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  /** @brief Minimum compress scratch size (one miniz `tdefl_compressor`). */
  k_ra8_compress_scratch_bytes = (uint32_t)sizeof(tdefl_compressor),
} ra8_compress_const_t;

/**
 * @brief Raw-DEFLATE compress `src` into `out`.
 *
 * @param[in]  src         Bytes to compress.
 * @param[in]  src_len     Number of input bytes.
 * @param[out] out         Destination for the compressed stream.
 * @param[in]  out_cap     Capacity of `out` in bytes.
 * @param[in]  scratch     Caller buffer >= ::k_ra8_compress_scratch_bytes,
 *                         8-byte aligned (holds the miniz compressor).
 * @param[in]  scratch_len Size of `scratch` in bytes.
 * @param[out] out_len     Compressed byte count on success.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Compressed; `*out_len` set.
 * @retval k_ra8_err_null_ptr     Any pointer argument was NULL.
 * @retval k_ra8_err_invalid_size `scratch_len` is below the minimum.
 * @retval k_ra8_err_no_mem       `out` was too small for the result.
 * @retval k_ra8_fail             The compressor reported an error.
 *
 * @pre `scratch` is at least ::k_ra8_compress_scratch_bytes and 8-aligned.
 * @pre `out` is writable for `out_cap` bytes.
 * @post On success `out[0 .. *out_len)` decompresses back to `src`.
 * @post On any non-ok return `out` content is unspecified.
 *
 * @note Not thread-safe with respect to the same scratch.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_compress(const uint8_t* src,
                                     uint32_t       src_len,
                                     uint8_t*       out,
                                     uint32_t       out_cap,
                                     void*          scratch,
                                     uint32_t       scratch_len,
                                     uint32_t*      out_len);

/**
 * @brief Zlib-wrap and DEFLATE-compress one bounded byte range.
 *
 * @details Uses the same caller-owned `tdefl_compressor` scratch and bounded
 *          output sink as ::ra8_compress, but emits an RFC 1950 zlib stream
 *          (two-byte header plus Adler-32 trailer). This is the wire shape used
 *          by each independently compressed chunk in an RBKC `.rabook`
 *          container. Keeping the wrapper choice explicit prevents raw RFC 1951
 *          streams from being mislabeled as zlib data.
 *
 * @param[in]  src         Bytes to compress.
 * @param[in]  src_len     Number of input bytes.
 * @param[out] out         Destination for the zlib stream.
 * @param[in]  out_cap     Writable capacity of @p out.
 * @param[in]  scratch     Caller buffer for one `tdefl_compressor`.
 * @param[in]  scratch_len Size of @p scratch in bytes.
 * @param[out] out_len     Receives the complete zlib-stream length.
 *
 * @return Canonical compression status.
 * @retval k_ra8_ok               A complete zlib stream was produced.
 * @retval k_ra8_err_null_ptr     A required pointer was NULL.
 * @retval k_ra8_err_invalid_size Compressor scratch was too small.
 * @retval k_ra8_err_no_mem       The bounded output could not hold the stream.
 * @retval k_ra8_fail             The compressor rejected the input.
 *
 * @pre @p scratch is at least ::k_ra8_compress_scratch_bytes and 8-byte aligned.
 * @pre @p out spans @p out_cap writable bytes.
 * @post On success, inflating `out[0..*out_len)` as zlib reproduces @p src.
 * @post No allocator is called; all transient state is caller-owned.
 *
 * @note Not thread-safe with respect to the same buffers.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_compress_zlib(const uint8_t* src,
                                          uint32_t       src_len,
                                          uint8_t*       out,
                                          uint32_t       out_cap,
                                          void*          scratch,
                                          uint32_t       scratch_len,
                                          uint32_t*      out_len);

/**
 * @brief Raw-DEFLATE decompress `src` into `out`.
 *
 * @param[in]  src     Compressed stream (raw DEFLATE).
 * @param[in]  src_len Number of input bytes.
 * @param[out] out     Destination for the decompressed bytes.
 * @param[in]  out_cap Capacity of `out` (must hold the whole result).
 * @param[out] out_len Decompressed byte count on success.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok            Decompressed; `*out_len` set.
 * @retval k_ra8_err_null_ptr  Any pointer argument was NULL.
 * @retval k_ra8_err_no_mem    `out` was too small, or the stream is corrupt.
 *
 * @pre `out` is large enough to hold the whole decompressed stream.
 * @pre `src` is a raw-DEFLATE stream produced compatibly.
 * @post On success `out[0 .. *out_len)` holds the original bytes.
 * @post On any non-ok return `out` content is unspecified.
 *
 * @note Heap-free; not thread-safe with respect to `out`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_decompress(const uint8_t* src,
                                       uint32_t       src_len,
                                       uint8_t*       out,
                                       uint32_t       out_cap,
                                       uint32_t*      out_len);

#ifdef __cplusplus
}
#endif
