/**
 * @file ra8_rabook_container.h
 * @brief Zero-heap streaming writer for the chunked RBKC `.rabook` container.
 * @ingroup grp_ereader
 *
 * @details Wraps a flat RABOOK1 blob in the random-access RBKC container used by
 * the reader. Input and output are callback-driven, so the same producer works
 * over POSIX files, `fw_if_fs`, RA8 VFS volumes, RAM, or RPC-backed storage.
 * Each flat chunk is read into caller storage, compressed as one independent
 * RFC 1950 stream, and written to a caller-owned staging destination. No heap,
 * global workspace, filesystem API, or device knowledge is present here.
 *
 * [Ring 4 / EPUB Compiler] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Read a bounded range from the flat RABOOK1 source.
 * @param[in,out] ctx Caller-owned source context.
 * @param[in] offset Absolute byte offset in the flat blob.
 * @param[out] dst Destination spanning @p requested writable bytes.
 * @param[in] requested Exact requested byte count.
 * @param[out] out_read Actual bytes copied, never greater than @p requested.
 * @return Backend status; non-ok values are propagated.
 * @pre All pointers are non-NULL and the requested range is in the declared blob.
 * @post The container writer rejects a successful short read.
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_rabook_flat_read_fn)(void*     ctx,
                                             uint32_t  offset,
                                             uint8_t*  dst,
                                             uint32_t  requested,
                                             uint32_t* out_read);

/**
 * @brief Write a bounded range at an absolute staging-destination offset.
 * @param[in,out] ctx Caller-owned destination context.
 * @param[in] offset Absolute byte offset in the RBKC staging object.
 * @param[in] src Source spanning @p requested readable bytes.
 * @param[in] requested Exact requested byte count.
 * @param[out] out_written Actual bytes stored, never greater than @p requested.
 * @return Backend status; non-ok values are propagated.
 * @pre All pointers are non-NULL and the destination supports bounded random writes.
 * @post The container writer rejects a successful short write.
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_rabook_write_at_fn)(void*          ctx,
                                            uint64_t       offset,
                                            const uint8_t* src,
                                            uint32_t       requested,
                                            uint32_t*      out_written);

/** @brief Caller-owned transient storage for one RBKC write. */
typedef struct {
  uint8_t*  input;          /**< One uncompressed flat chunk.                */
  uint8_t*  compressed;     /**< One complete zlib stream.                   */
  void*     compressor;     /**< `tdefl_compressor` storage, 8-byte aligned. */
  uint64_t* offsets;        /**< Payload-relative offset table.              */
  uint32_t  input_cap;      /**< Writable bytes at @p input.                 */
  uint32_t  compressed_cap; /**< Writable bytes at @p compressed.            */
  uint32_t  compressor_cap; /**< Writable bytes at @p compressor.            */
  uint32_t  offset_cap;     /**< Number of uint64 entries at @p offsets.     */
} ra8_rabook_container_workspace_t;

/**
 * @brief Stream one flat RABOOK1 blob into a chunked RBKC staging object.
 *
 * @details Writes the fixed little-endian header, reserves the complete offset
 *          table, then reads and zlib-compresses each independent flat chunk.
 *          The table is back-filled only after all streams succeed. The caller
 *          must target a private staging object and validate it before atomic
 *          publication; this function deliberately owns neither filenames nor
 *          commit policy.
 *
 * @param[in] read Source read callback.
 * @param[in,out] read_ctx Context passed to @p read.
 * @param[in] flat_len Exact nonzero flat RABOOK1 length.
 * @param[in] chunk_bytes Inflated bytes per independently compressed chunk.
 * @param[in] write_at Random-write callback for the staging destination.
 * @param[in,out] write_ctx Context passed to @p write_at.
 * @param[in,out] ws Exclusive caller-owned workspace.
 * @param[out] out_len Final RBKC byte length.
 *
 * @return Container write status.
 * @retval k_ra8_ok Complete RBKC bytes were written and @p out_len was set.
 * @retval k_ra8_err_null_ptr A required pointer or workspace member was NULL.
 * @retval k_ra8_err_invalid_arg @p flat_len or @p chunk_bytes was zero.
 * @retval k_ra8_err_invalid_size A workspace/table bound was insufficient, an
 *                                offset overflowed, or a callback was short.
 * @retval k_ra8_err_no_mem The compressed-chunk destination was too small.
 * @return Any other source, destination, or compressor error is propagated.
 *
 * @pre @p ws storage is non-overlapping and exclusively owned for the call.
 * @pre @p read serves every byte in `[0, flat_len)` repeatably.
 * @pre @p write_at targets a private staging object and supports back-filling.
 * @post On success the output conforms to ::book_container_t.
 * @post On failure @p out_len is zero and the caller must abort its staging object.
 * @post No dynamically allocated memory or global mutable state is used.
 *
 * @note Not thread-safe with respect to the supplied callbacks or workspace.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rabook_container_write(ra8_rabook_flat_read_fn           read,
                                                   void*                             read_ctx,
                                                   uint32_t                          flat_len,
                                                   uint32_t                          chunk_bytes,
                                                   ra8_rabook_write_at_fn            write_at,
                                                   void*                             write_ctx,
                                                   ra8_rabook_container_workspace_t* ws,
                                                   uint64_t*                         out_len);

#ifdef __cplusplus
} /* extern "C" */
#endif
