/**
 * @file ra8_cbz_container.h
 * @brief Per-page demand-paged reader for the "RCBZ" comic-container format.
 * @ingroup grp_ereader
 *
 * @details
 * A CBZ (a ZIP of comic/manga page images) is architecturally easier to page
 * than a `.rabook`: the ZIP already compresses every entry independently, so
 * there is no whole-archive DEFLATE stream to chunk. This reader is the
 * on-device half of that observation. The host tool
 * `tools/epub_compile/cbz_container.py` walks the source CBZ central directory
 * once, pre-decodes each page to the panel-native 4bpp raster (the exact #212
 * transcode `cbz_compile.py` uses), independently zlib-compresses each page,
 * and emits the flat "RCBZ" container this file reads back.
 *
 * Where ::ra8_book_chunked_t serves one *flat blob* split into uniform
 * `chunk_bytes` chunks, ::ra8_cbz_t serves a *page-indexed* container: the
 * central-directory-derived manifest maps each page to its own compressed
 * stream plus its decode metadata (width, height, format, inflated raster
 * length). Each page is read a page at a time -- one ::ra8_cbz_page_read is one
 * SD-read burst plus one inflate -- so a multi-GB omnibus is read with a
 * bounded working set of a few pages, never resident in full.
 *
 * ::ra8_cbz_page_read has exactly the ::ra8_vsource_read_fn signature, so a
 * per-page cursor bound by ::ra8_cbz_page_bind plugs straight into
 * ::ra8_vsource_add_paged as the backing of a paged object whose byte space is
 * *that one page's inflated raster*:
 *
 * @code
 * ra8_cbz_t cbz = {};
 * err = ra8_cbz_open(&cbz, sd_file_read, &file, file_len, sh_inflate,
 *                   offsets_buf, k_offset_entries, meta_buf, sizeof meta_buf,
 *                   staging, sizeof staging);
 * ra8_cbz_page_t cur = {};
 * err = ra8_cbz_page_bind(&cbz, page_idx, &cur);
 * uint32_t oid = 0U;
 * err = ra8_vsource_add_paged(&vs, ra8_cbz_page_read, &cur, 0U, cur.raw_size, &oid);
 * @endcode
 *
 * @par Container layout
 * @code
 *   [0]  "RCBZ" magic (4 bytes)
 *   [4]  uint32 page_count       number of pages (> 0)
 *   [8]  uint32 flags            ra8_book_flag_t bits (k_ra8_book_flag_rtl, ...)
 *   [12] uint32 reserved         must be 0
 *   [16] uint64 offset[page_count + 1]   each page's zlib stream is
 *        [payload + offset[i], payload + offset[i+1]); offset[0] == 0 and
 *        offset[page_count] == payload length; strictly increasing
 *   [..] meta[page_count]        12 bytes each: uint32 raw_size, uint16 width,
 *        uint16 height, uint8 format (ra8_book_image_format_t), 3 reserved bytes
 *   [..] payload: page_count concatenated zlib (RFC 1950) streams, page i
 *        inflating to exactly meta[i].raw_size bytes of pre-decoded raster
 * @endcode
 *
 * @par Reads are page-granular by contract
 * ::ra8_cbz_page_read serves a *whole page* per call: `offset` must be 0 and
 * `len` must equal the page's `raw_size`. This is precisely what
 * ::ra8_vsource_loader passes for a per-page object when the cache's
 * `frame_bytes` is at least the page's raster length (the loader clips the
 * read to the object end). A page whose raster exceeds one cache frame needs
 * sub-page tiling for pan/zoom -- that is ::ra8_tile_cache's job (issue #211),
 * not this container's; the manifest exposes each page's dimensions so a tiler
 * can slice a page this reader hands back whole.
 *
 * Zero allocation (NASA P10 Rule 3): the caller supplies the offset-table,
 * metadata and staging storage at open.
 *
 * @note Not thread-safe; the single-threaded reader loop serialises access.
 *
 * @see @ref md_docs_2formats_2RCBZ -- the full RCBZ wire-format specification
 *      (rationale, algorithms, worked example, failure modes).
 * @see ra8_book_chunked.h  The uniform-chunk flat-blob reader this mirrors.
 * @see ra8_vsource.h       The registry ::ra8_cbz_page_read plugs into.
 * @see ra8_book.h          ::ra8_book_flag_t / ::ra8_book_image_format_t reused here.
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
 * @enum ra8_cbz_layout_t
 * @brief Fixed byte sizes of the "RCBZ" container's header and table records.
 * @details The format is a binary wire layout shared with the host producer
 *          (`tools/epub_compile/cbz_container.py`), so each record's size is
 *          part of the contract. Decoding is memcpy-based, so no field needs
 *          natural alignment.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_cbz_magic_len  = 4U,  /**< Length of the "RCBZ" magic.                       */
  k_ra8_cbz_header_len = 16U, /**< Fixed header bytes ahead of the offset table.     */
  k_ra8_cbz_offset_len = 8U,  /**< One offset-table entry (uint64 LE stream offset). */
  k_ra8_cbz_meta_len   = 12U, /**< One metadata-table entry (see @ref ra8_cbz_t).    */
} ra8_cbz_layout_t;

/**
 * @enum ra8_cbz_header_field_off_t
 * @brief Byte offsets of the fields inside the fixed "RCBZ" header.
 * @details Mirrors the layout documented on @ref ra8_cbz_container.h; the magic
 *          occupies `[0, k_ra8_cbz_magic_len)` and is matched, not decoded.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_cbz_hdr_off_page_count = 4U,  /**< uint32 LE: number of pages.      */
  k_ra8_cbz_hdr_off_flags      = 8U,  /**< uint32 LE: ra8_book_flag_t bits. */
  k_ra8_cbz_hdr_off_reserved   = 12U, /**< uint32 LE: reserved, must be 0.  */
} ra8_cbz_header_field_off_t;

/**
 * @enum ra8_cbz_meta_field_off_t
 * @brief Byte offsets of the fields inside one metadata-table entry.
 * @details Entry `i` starts at `meta + i * k_ra8_cbz_meta_len`; the three bytes
 *          after `format` are reserved padding (0).
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_cbz_meta_off_raw    = 0U, /**< uint32 LE: inflated raster length in bytes. */
  k_ra8_cbz_meta_off_width  = 4U, /**< uint16 LE: pixel width.                     */
  k_ra8_cbz_meta_off_height = 6U, /**< uint16 LE: pixel height.                    */
  k_ra8_cbz_meta_off_format = 8U, /**< uint8: @ref ra8_book_image_format_t.        */
} ra8_cbz_meta_field_off_t;

/**
 * @struct ra8_cbz_t
 * @brief One open "RCBZ" container: parsed geometry + caller-owned storage.
 *
 * @details Populated by ::ra8_cbz_open; treat every field as read-only
 *          afterwards. `file_read` addresses the *container file bytes*;
 *          ::ra8_cbz_page_read then serves each page's *inflated raster* on top
 *          of it. `offsets` points at the caller's buffer holding
 *          `page_count + 1` payload-relative stream offsets; `meta` points at
 *          the caller's buffer holding `page_count` 12-byte metadata records;
 *          `staging` receives one page's compressed bytes per read.
 *
 * @invariant `offsets[0] == 0`, `offsets` is strictly increasing, and
 *            `offsets[page_count] == file length - payload_off`.
 * @invariant Every page's compressed length fits `staging_cap`.
 *
 * @see ra8_cbz_open()
 * @since Version 0.1.0
 */
typedef struct {
  ra8_vsource_read_fn file_read;   /**< Byte reader over the container file.      */
  void*               file_ctx;    /**< Context for @ref file_read.               */
  ra8_book_inflate_fn inflate;     /**< zlib-stream decompressor.                 */
  const uint64_t*     offsets;     /**< `page_count + 1` stream offsets (caller). */
  const uint8_t*      meta;        /**< `page_count` metadata records (caller).   */
  uint8_t*            staging;     /**< Compressed-page staging (caller).         */
  uint32_t            staging_cap; /**< Capacity of @ref staging in bytes.        */
  uint64_t            payload_off; /**< File offset of the first stream byte.     */
  uint32_t            page_count;  /**< Number of pages.                          */
  uint32_t            flags;       /**< @ref ra8_book_flag_t bits (RTL, ...).     */
} ra8_cbz_t;

/**
 * @struct ra8_cbz_page_t
 * @brief A cursor onto one page, used as the ::ra8_vsource_read_fn context.
 *
 * @details Bound by ::ra8_cbz_page_bind; pass its address as the `ctx` of
 *          ::ra8_vsource_add_paged so the paged object's read callback resolves
 *          to exactly this page. `raw_size` is the page's inflated raster
 *          length -- the object size to register, and the only `len` value
 *          ::ra8_cbz_page_read accepts for this page.
 *
 * @invariant `page < cbz->page_count` and `raw_size == meta[page].raw_size`.
 *
 * @see ra8_cbz_page_bind()
 * @see ra8_cbz_page_read()
 * @since Version 0.1.0
 */
typedef struct {
  const ra8_cbz_t* cbz;      /**< The open container this page belongs to. */
  uint32_t         page;     /**< Page index (`< cbz->page_count`).        */
  uint32_t         raw_size; /**< Inflated raster length of this page.     */
} ra8_cbz_page_t;

/**
 * @brief Open an "RCBZ" container for per-page demand-paged reads.
 *
 * @details
 * Reads the fixed header through @p file_read, validates it (magic, non-zero
 * page count, no unknown feature-flag bit, zero reserved word), loads the
 * offset table and metadata table into the caller buffers, then validates the
 * offset table (`offset[0] == 0`, strictly increasing, `offset[page_count]`
 * equal to the payload length implied by @p file_len, every page's compressed
 * length within @p staging_cap) and every page's raster length (non-zero,
 * within @p staging_cap after inflate is not checked here -- only the
 * compressed span is). On success @p cbz is bound and ::ra8_cbz_page_read may
 * serve reads; the caller-owned buffers must out-live it.
 *
 * @param[out] cbz                Reader to populate (caller-owned).
 * @param[in]  file_read          Byte reader over the container file (non-NULL).
 * @param[in]  file_ctx           Context passed to @p file_read.
 * @param[in]  file_len           Container file length in bytes.
 * @param[in]  inflate            zlib decompressor (see @ref ra8_book_inflate_fn).
 * @param[in]  offsets_buf        Caller buffer for the offset table (non-NULL).
 * @param[in]  offsets_cap        Capacity of @p offsets_buf in uint64 entries;
 *                                must be >= `page_count + 1`.
 * @param[in]  meta_buf           Caller buffer for the metadata table (non-NULL).
 * @param[in]  meta_cap           Capacity of @p meta_buf in bytes; must be
 *                                >= `page_count * k_ra8_cbz_meta_len`.
 * @param[in]  staging            Caller buffer for one compressed page (non-NULL).
 * @param[in]  staging_cap        Capacity of @p staging in bytes; must cover the
 *                                largest compressed page in the file.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Reader bound; geometry fully validated.
 * @retval k_ra8_err_null_ptr     A required pointer argument was NULL.
 * @retval k_ra8_err_invalid_arg  Bad magic / header field / offset-table shape /
 *                               zero page raster length.
 * @retval k_ra8_err_invalid_size @p file_len shorter than header + tables, a
 *                               table needs more than the caller budgeted, a
 *                               compressed page exceeds @p staging_cap, or a
 *                               table's byte length overflows one read call.
 * @retval k_ra8_err_*            A @p file_read error, returned verbatim.
 *
 * @pre @p file_read serves offsets `[0, file_len)` of the container file.
 * @pre @p offsets_buf, @p meta_buf and @p staging out-live every read.
 * @post On k_ra8_ok, @p cbz is bound and each page's raster is servable.
 * @post On any error @p cbz is left unbound (its `offsets` stays NULL).
 *
 * @note Not thread-safe.
 * @see ra8_cbz_page_read()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cbz_open(ra8_cbz_t*          cbz,
                                     ra8_vsource_read_fn file_read,
                                     void*               file_ctx,
                                     uint64_t            file_len,
                                     ra8_book_inflate_fn inflate,
                                     uint64_t*           offsets_buf,
                                     uint32_t            offsets_cap,
                                     uint8_t*            meta_buf,
                                     uint32_t            meta_cap,
                                     uint8_t*            staging,
                                     uint32_t            staging_cap);

/**
 * @brief Number of pages in an open container.
 * @details Reads the page count parsed by ::ra8_cbz_open, guarding against a
 *          NULL or not-yet-bound reader so a shelf view can query it safely
 *          before a container is opened.
 * @param[in] cbz Container bound by ::ra8_cbz_open (non-NULL).
 * @return The page count, or 0 for a NULL / unbound reader.
 * @retval 0 @p cbz is NULL or was never bound by ::ra8_cbz_open.
 * @pre @p cbz was populated by ::ra8_cbz_open.
 * @pre @p cbz out-lives the call.
 * @post No state is modified (pure read).
 * @post The result equals the container header's page count.
 * @note Thread-safe: pure read over an immutable, bound reader.
 * @since Version 0.1.0
 */
static inline uint32_t ra8_cbz_page_count(const ra8_cbz_t* cbz)
{
  if (cbz == nullptr) {
    return 0U;
  }
  if (cbz->offsets == nullptr) {
    return 0U;
  }
  return cbz->page_count;
}

/**
 * @brief Whether the container declares right-to-left reading order (manga).
 * @details Decodes ::k_ra8_book_flag_rtl from the header `flags`; the reader
 *          mirrors its page-turn zones for an RTL book.
 * @param[in] cbz Container bound by ::ra8_cbz_open (non-NULL).
 * @return Whether the RTL flag bit is set (false for a NULL / unbound reader).
 * @retval true  The comic reads right-to-left.
 * @retval false The comic reads left-to-right.
 * @pre @p cbz was populated by ::ra8_cbz_open.
 * @pre @p cbz out-lives the call.
 * @post No state is modified (pure read).
 * @post The result is stable for the lifetime of the bound reader.
 * @note Thread-safe: pure read over an immutable, bound reader.
 * @see ra8_book_flag_t
 * @since Version 0.1.0
 */
static inline bool ra8_cbz_is_rtl(const ra8_cbz_t* cbz)
{
  if (cbz == nullptr) {
    return false;
  }
  if (cbz->offsets == nullptr) {
    return false;
  }
  return (cbz->flags & (uint32_t)k_ra8_book_flag_rtl) != 0U;
}

/**
 * @brief Read one page's manifest entry: dimensions, format and raster length.
 *
 * @details Decodes the page's 12-byte metadata record from the resident table.
 *          This is the "central directory as manifest" query: it needs no file
 *          I/O and no inflate, so a shelf/cover view can size a page or pick the
 *          cover image without paging any raster in.
 *
 * @param[in]  cbz          Container bound by ::ra8_cbz_open (non-NULL).
 * @param[in]  page         Page index (`< ra8_cbz_page_count(cbz)`).
 * @param[out] out_width    Receives the page pixel width (non-NULL).
 * @param[out] out_height   Receives the page pixel height (non-NULL).
 * @param[out] out_format   Receives the @ref ra8_book_image_format_t (non-NULL).
 * @param[out] out_raw_size Receives the inflated raster length (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Outputs populated from the manifest.
 * @retval k_ra8_err_null_ptr      A required pointer argument was NULL.
 * @retval k_ra8_err_invalid_state @p cbz was never bound by ::ra8_cbz_open.
 * @retval k_ra8_err_out_of_range  @p page is at or past the page count.
 *
 * @pre @p cbz was populated by ::ra8_cbz_open.
 * @pre All four output pointers are distinct, writable locations.
 * @post On k_ra8_ok every output is populated from page @p page.
 * @post On any error no output is modified.
 *
 * @note Thread-safe: pure read of an immutable table.
 * @see ra8_cbz_page_bind()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cbz_page_info(const ra8_cbz_t* cbz,
                                          uint32_t         page,
                                          uint16_t*        out_width,
                                          uint16_t*        out_height,
                                          uint8_t*         out_format,
                                          uint32_t*        out_raw_size);

/**
 * @brief Bind a per-page cursor for use as a ::ra8_vsource_read_fn context.
 *
 * @details Populates @p out_cur with the container pointer, the page index and
 *          the page's inflated raster length, so the caller can register the
 *          page as a paged object:
 *          `ra8_vsource_add_paged(&vs, ra8_cbz_page_read, out_cur, 0,
 *          out_cur->raw_size, &id)`. The cursor must out-live the paged object.
 *
 * @param[in]  cbz     Container bound by ::ra8_cbz_open (non-NULL).
 * @param[in]  page    Page index (`< ra8_cbz_page_count(cbz)`).
 * @param[out] out_cur Cursor to populate (non-NULL, caller-owned).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Cursor bound to @p page.
 * @retval k_ra8_err_null_ptr      @p cbz or @p out_cur was NULL.
 * @retval k_ra8_err_invalid_state @p cbz was never bound by ::ra8_cbz_open.
 * @retval k_ra8_err_out_of_range  @p page is at or past the page count.
 *
 * @pre @p cbz was populated by ::ra8_cbz_open.
 * @pre @p out_cur is a writable ::ra8_cbz_page_t.
 * @post On k_ra8_ok, @p out_cur addresses page @p page and out-lives its object.
 * @post On any error @p out_cur is left unbound (its `cbz` stays NULL).
 *
 * @note Not thread-safe with respect to a concurrent ::ra8_cbz_open on @p cbz.
 * @see ra8_cbz_page_read()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_cbz_page_bind(const ra8_cbz_t* cbz, uint32_t page, ra8_cbz_page_t* out_cur);

/**
 * @brief Serve one whole page's inflated raster -- the ::ra8_vsource_read_fn.
 *
 * @details
 * The ::ra8_vsource_read_fn implementation for a page cursor: reads the page's
 * compressed zlib stream into the staging buffer through the file reader and
 * inflates it directly into @p buf. By contract @p offset is 0 and @p len
 * equals the page's `raw_size` -- exactly what ::ra8_vsource_loader passes for a
 * per-page object when the cache `frame_bytes` is at least the page's raster
 * length. One call is one SD-read burst plus one inflate.
 *
 * @param[in]  ctx    The bound ::ra8_cbz_page_t (as a void cookie).
 * @param[in]  offset Byte offset within the page raster; must be 0.
 * @param[out] buf    Destination for exactly @p len inflated bytes (non-NULL).
 * @param[in]  len    Must equal the page's `raw_size`.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Page inflated into @p buf.
 * @retval k_ra8_err_null_ptr      @p ctx or @p buf was NULL.
 * @retval k_ra8_err_invalid_state @p ctx or its container was never bound.
 * @retval k_ra8_err_invalid_arg   @p offset is non-zero, or @p len is not the
 *                                page's exact raster length.
 * @retval k_ra8_err_invalid_size  The stream inflated to the wrong length.
 * @retval k_ra8_err_*             A file-read or inflater error, verbatim.
 *
 * @pre @p ctx was populated by ::ra8_cbz_page_bind.
 * @pre @p buf addresses at least @p len writable bytes.
 * @post On k_ra8_ok, `buf[0..len)` holds the page's inflated raster.
 * @post On any error @p buf contents are unspecified.
 *
 * @note Not thread-safe: the staging buffer is reused across calls.
 * @see ra8_vsource_add_paged()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cbz_page_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len);

#ifdef __cplusplus
}
#endif
