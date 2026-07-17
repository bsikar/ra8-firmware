/**
 * @file ra8_book_chunked.h
 * @brief Demand-paged chunk reader for the "RBKC" `.rabook` container.
 * @ingroup grp_ereader
 *
 * @details
 * The bridge between the chunked on-disk container (see @ref
 * ra8_book_container_t in ra8_book.h) and the #147 page-cache stack: where
 * ra8_book_open() inflates *every* chunk into one resident SDRAM buffer, this
 * reader inflates *single* chunks on demand, so a book far larger than RAM is
 * read through an ::ra8_vmem cache with a bounded resident working set.
 *
 * ::ra8_book_chunked_read has exactly the ::ra8_vsource_read_fn signature, so a
 * bound reader plugs straight into ::ra8_vsource_add_paged as the backing of a
 * paged object whose byte space is the *inflated flat blob*:
 *
 * @code
 * ra8_book_chunked_t rd = {};
 * err = ra8_book_chunked_open(&rd, sd_file_read, &file, file_len, sh_inflate,
 *                            table_buf, k_table_entries, staging, sizeof staging);
 * err = ra8_vsource_add_paged(&vs, ra8_book_chunked_read, &rd,
 *                            0U, rd.inflated_total, &book_obj);
 * err = ra8_book_src_paged(&src, &vm, book_obj,
 *                         rd.chunk_bytes, (uint32_t)rd.inflated_total);
 * @endcode
 *
 * One chunk equals one cache frame equals one inflate call: the container is
 * produced with `chunk_bytes` equal to the reader's ::ra8_vmem frame size, each
 * chunk's zlib stream is staged through a caller-owned compressed-bytes buffer
 * and inflated directly into the frame ::ra8_vsource_loader hands over -- no
 * double-buffering and no second decompressed-chunk cache. Reads are therefore
 * *chunk-aligned by contract*: `offset` must be a multiple of `chunk_bytes`
 * and `len` must equal the chunk's exact inflated span, which is precisely
 * what ::ra8_vsource_loader passes when the cache's `frame_bytes` equals the
 * container's `chunk_bytes` (it clips the final frame to the object end).
 *
 * Zero allocation (NASA P10 Rule 3): the caller supplies the chunk-table and
 * staging storage at open.
 *
 * @note Not thread-safe; the single-threaded reader loop serialises access.
 *
 * @see ra8_book.h        Container layout + the resident open.
 * @see ra8_book_paged.h  Binds the paged object as a book source.
 * @see ra8_vsource.h     The registry ::ra8_book_chunked_read plugs into.
 *
 * @since Version 0.1.0
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#include "ra8_book.h"
#include "ra8_err.h"
#include "ra8_vsource.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct ra8_book_chunked_t
 * @brief One open chunked `.rabook` file: parsed geometry + caller storage.
 *
 * @details Populated by ::ra8_book_chunked_open; treat every field as read-only
 *          afterwards. `file_read` addresses the *container file bytes* (the
 *          compressed on-disk form); ::ra8_book_chunked_read then serves the
 *          *inflated flat-blob* byte space on top of it. `table` points at the
 *          caller's buffer holding `chunk_count + 1` payload-relative stream
 *          offsets; `staging` receives one chunk's compressed bytes per read.
 *
 * @invariant `table[0] == 0`, `table` is strictly increasing, and
 *            `table[chunk_count] == file length - payload_off`.
 * @invariant Every chunk's compressed length fits `staging_cap`.
 *
 * @see ra8_book_chunked_open()
 * @since Version 0.1.0
 */
typedef struct {
  ra8_vsource_read_fn file_read;      /**< Byte reader over the container file.       */
  void*               file_ctx;       /**< Context for @ref file_read.                */
  ra8_book_inflate_fn inflate;        /**< zlib-stream decompressor.                  */
  const uint64_t*     table;          /**< `chunk_count + 1` stream offsets (caller). */
  uint8_t*            staging;        /**< Compressed-chunk staging (caller).         */
  uint32_t            staging_cap;    /**< Capacity of @ref staging in bytes.         */
  uint64_t            payload_off;    /**< File offset of the first stream byte.      */
  uint64_t            inflated_total; /**< Flat-blob length this reader serves.       */
  uint32_t            chunk_bytes;    /**< Inflated bytes per chunk (last short).     */
  uint32_t            chunk_count;    /**< Number of chunks.                          */
} ra8_book_chunked_t;

/**
 * @brief Open a chunked `.rabook` container for demand-paged chunk reads.
 *
 * @details
 * Reads the fixed "RBKC" header through @p file_read, validates it (magic,
 * non-zero geometry, chunk count consistent with the inflated total), loads
 * the chunk table into @p table_buf, then validates the table: `offset[0] ==
 * 0`, strictly increasing, `offset[chunk_count]` equal to the payload length
 * implied by @p file_len, and every chunk's compressed length within
 * @p staging_cap. On success @p rd is bound and ::ra8_book_chunked_read may
 * serve reads; the caller-owned @p table_buf and @p staging must out-live it.
 *
 * @param[out] rd                Reader to populate (caller-owned).
 * @param[in]  file_read         Byte reader over the container file (non-NULL).
 * @param[in]  file_ctx          Context passed to @p file_read.
 * @param[in]  file_len          Container file length in bytes.
 * @param[in]  inflate           zlib decompressor (see @ref ra8_book_inflate_fn).
 * @param[in]  table_buf         Caller buffer for the chunk table (non-NULL).
 * @param[in]  table_cap_entries Capacity of @p table_buf in uint64 entries;
 *                               must be >= `chunk_count + 1`.
 * @param[in]  staging           Caller buffer for one compressed chunk (non-NULL).
 * @param[in]  staging_cap       Capacity of @p staging in bytes; must cover the
 *                               largest compressed chunk in the file.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Reader bound; geometry fully validated.
 * @retval k_ra8_err_null_ptr     A required pointer argument was NULL.
 * @retval k_ra8_err_invalid_arg  Bad magic / header fields / chunk-table shape.
 * @retval k_ra8_err_invalid_size @p file_len shorter than header + table, the
 *                               table needs more than @p table_cap_entries, a
 *                               compressed chunk exceeds @p staging_cap, or the
 *                               table's byte length overflows one read call.
 * @retval k_ra8_err_*            A @p file_read error, returned verbatim.
 *
 * @pre @p file_read serves offsets `[0, file_len)` of the container file.
 * @pre @p table_buf and @p staging out-live every read through @p rd.
 * @post On k_ra8_ok, @p rd is bound and ::ra8_book_chunked_read serves
 *       `[0, rd->inflated_total)` of the flat blob.
 * @post On any error @p rd is left unbound (its `table` stays NULL).
 *
 * @note Not thread-safe.
 * @see ra8_book_chunked_read()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_book_chunked_open(ra8_book_chunked_t* rd,
                                              ra8_vsource_read_fn file_read,
                                              void*               file_ctx,
                                              uint64_t            file_len,
                                              ra8_book_inflate_fn inflate,
                                              uint64_t*           table_buf,
                                              uint32_t            table_cap_entries,
                                              uint8_t*            staging,
                                              uint32_t            staging_cap);

/**
 * @brief Serve one chunk-aligned read of the inflated flat blob.
 *
 * @details
 * The ::ra8_vsource_read_fn implementation: looks the chunk covering @p offset
 * up in the resident table, reads its compressed zlib stream into the staging
 * buffer through the file reader, and inflates it directly into @p buf. By
 * contract @p offset is chunk-aligned and @p len equals the chunk's exact
 * inflated span, `min(chunk_bytes, inflated_total - offset)` -- exactly what
 * ::ra8_vsource_loader passes when the ::ra8_vmem `frame_bytes` equals the
 * container's `chunk_bytes`. One call is one SD-read burst plus one inflate.
 *
 * @param[in]  ctx    The bound ::ra8_book_chunked_t (as a void cookie).
 * @param[in]  offset Chunk-aligned byte offset within the inflated blob.
 * @param[out] buf    Destination for exactly @p len inflated bytes (non-NULL).
 * @param[in]  len    The chunk's exact inflated span at @p offset.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Chunk inflated into @p buf.
 * @retval k_ra8_err_null_ptr      @p ctx or @p buf was NULL.
 * @retval k_ra8_err_invalid_state @p ctx was never bound by ::ra8_book_chunked_open.
 * @retval k_ra8_err_invalid_arg   @p offset is not chunk-aligned, or @p len is
 *                                not the chunk's exact inflated span.
 * @retval k_ra8_err_out_of_range  @p offset is at or past the inflated total.
 * @retval k_ra8_err_invalid_size  The stream inflated to the wrong length.
 * @retval k_ra8_err_*             A file-read or inflater error, verbatim.
 *
 * @pre @p ctx was populated by ::ra8_book_chunked_open.
 * @pre @p buf addresses at least @p len writable bytes.
 * @post On k_ra8_ok, `buf[0..len)` holds the flat blob's bytes at @p offset.
 * @post On any error @p buf contents are unspecified.
 *
 * @note Not thread-safe: the staging buffer is reused across calls.
 * @see ra8_vsource_add_paged()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_book_chunked_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len);

#ifdef __cplusplus
}
#endif
