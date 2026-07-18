/**
 * @file ra8_cbz_container.c
 * @brief Implementation of the per-page demand-paged "RCBZ" comic reader.
 *
 * @details
 * The callback-driven half of the CBZ container: the fixed header, the
 * offset table and the metadata table are pulled through the caller's file
 * reader at open and validated once; each ::ra8_cbz_page_read then stages one
 * page's compressed stream and inflates it into the caller's frame. The format
 * is page-indexed rather than uniform-chunked (see ra8_book_chunked.c for the
 * flat-blob variant), so every page carries its own inflated raster length and
 * decode metadata in the manifest.
 *
 * Every decision is a single condition on purpose, so the reader carries no
 * MC/DC obligation of its own (mirroring ra8_book_chunked.c).
 *
 * @since Version 0.1.0
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "ra8_cbz_container.h"

#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"

/** @brief Log tag for comic-container diagnostics. */
static const char* const s_tag_cbz = "ra8_cbz";

/**
 * @brief Decode a little-endian uint32 from unaligned container bytes.
 * @details memcpy-based so the source may sit at any alignment; the whole
 *          firmware is little-endian, so the bytes map straight to the value.
 * @param[in] p Pointer to the four bytes to decode (non-NULL, caller-bounded).
 * @return The decoded 32-bit value.
 * @retval 0 When the four bytes are all zero.
 * @pre @p p addresses at least four readable bytes.
 * @pre The host is little-endian (RA8D2 target and x86_64 test host both are).
 * @post No state is modified (pure read).
 * @post The result is a pure function of the four bytes at @p p.
 * @note Thread-safe: pure read.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static uint32_t s_le32(const uint8_t* p)
{
  uint32_t v = 0U;
  (void)memcpy(&v, p, sizeof(v));
  return v;
}

/**
 * @brief Decode a little-endian uint16 from unaligned container bytes.
 * @details memcpy-based so the source may sit at any alignment; the whole
 *          firmware is little-endian, so the bytes map straight to the value.
 * @param[in] p Pointer to the two bytes to decode (non-NULL, caller-bounded).
 * @return The decoded 16-bit value.
 * @retval 0 When the two bytes are all zero.
 * @pre @p p addresses at least two readable bytes.
 * @pre The host is little-endian (RA8D2 target and x86_64 test host both are).
 * @post No state is modified (pure read).
 * @post The result is a pure function of the two bytes at @p p.
 * @note Thread-safe: pure read.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static uint16_t s_le16(const uint8_t* p)
{
  uint16_t v = 0U;
  (void)memcpy(&v, p, sizeof(v));
  return v;
}

/**
 * @brief Decode one page's metadata record into its component fields.
 *
 * @details Reads the 12-byte record at `meta + page * k_ra8_cbz_meta_len` and
 *          splits it into raster length, dimensions and format. Pure decode:
 *          the caller must have bounds-checked @p page against the page count.
 *
 * @param[in]  meta         Base of the resident metadata table (non-NULL).
 * @param[in]  page         Page index (`< page_count`, caller-bounded).
 * @param[out] out_width    Receives the pixel width.
 * @param[out] out_height   Receives the pixel height.
 * @param[out] out_format   Receives the @ref ra8_book_image_format_t.
 * @param[out] out_raw_size Receives the inflated raster length.
 *
 * @pre @p meta holds at least `(page + 1) * k_ra8_cbz_meta_len` bytes.
 * @pre Every output pointer is a distinct, writable location.
 * @post All four outputs hold page @p page's decoded fields.
 * @post No input state is modified.
 *
 * @note Thread-safe: pure read of an immutable table.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void s_read_meta(const uint8_t* meta,
                        uint32_t       page,
                        uint16_t*      out_width,
                        uint16_t*      out_height,
                        uint8_t*       out_format,
                        uint32_t*      out_raw_size)
{
  const uint8_t* rec = &meta[(size_t)page * (size_t)k_ra8_cbz_meta_len];
  *out_raw_size      = s_le32(&rec[k_ra8_cbz_meta_off_raw]);
  *out_width         = s_le16(&rec[k_ra8_cbz_meta_off_width]);
  *out_height        = s_le16(&rec[k_ra8_cbz_meta_off_height]);
  *out_format        = rec[k_ra8_cbz_meta_off_format];
}

/**
 * @brief Parse + validate the fixed "RCBZ" header into page count and flags.
 *
 * @details Checks the magic, requires a non-zero page count, requires the
 *          reserved word to be zero, and rejects any feature-flag bit outside
 *          ::k_ra8_book_flag_mask_known. Field decoding is memcpy-based so the
 *          header buffer needs no alignment.
 *
 * @param[in]  hdr        First @ref k_ra8_cbz_header_len bytes of the container.
 * @param[out] out_pages  Receives the page count.
 * @param[out] out_flags  Receives the feature-flag word.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok              Header well-formed; outputs populated.
 * @retval k_ra8_err_invalid_arg Bad magic, zero page count, non-zero reserved
 *                              word, or an unknown feature-flag bit.
 *
 * @pre @p hdr holds at least @ref k_ra8_cbz_header_len readable bytes.
 * @pre @p out_pages and @p out_flags are distinct, writable locations.
 * @post On k_ra8_ok both outputs are populated and internally consistent.
 * @post On any error no output is modified.
 *
 * @note Thread-safe: reads only @p hdr, writes only the outputs.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_parse_header(const uint8_t* hdr, uint32_t* out_pages, uint32_t* out_flags)
{
  if (memcmp(hdr, "RCBZ", (size_t)k_ra8_cbz_magic_len) != 0) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t pages    = s_le32(&hdr[k_ra8_cbz_hdr_off_page_count]);
  const uint32_t flags    = s_le32(&hdr[k_ra8_cbz_hdr_off_flags]);
  const uint32_t reserved = s_le32(&hdr[k_ra8_cbz_hdr_off_reserved]);
  if (pages == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (reserved != 0U) {
    return k_ra8_err_invalid_arg;
  }
  if ((flags & ~(uint32_t)k_ra8_book_flag_mask_known) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  *out_pages = pages;
  *out_flags = flags;
  return k_ra8_ok;
}

/**
 * @brief Validate the offset table's shape against the payload extent.
 *
 * @details Requires `offsets[0] == 0`, strictly increasing entries, an end
 *          offset equal to @p payload_len (the streams exactly tile the file
 *          tail), and every page's compressed length within the staging budget.
 *
 * @param[in] cbz         Reader carrying `page_count` and `staging_cap`.
 * @param[in] offsets     Loaded offset table (`page_count + 1` entries).
 * @param[in] payload_len Stream bytes remaining after the two tables.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok               Table well-formed; every stream fits staging.
 * @retval k_ra8_err_invalid_arg  Bad start / non-monotonic / bad end offset.
 * @retval k_ra8_err_invalid_size A compressed page exceeds the staging budget.
 *
 * @pre `cbz->page_count` and `cbz->staging_cap` were populated by the caller.
 * @pre @p offsets holds `cbz->page_count + 1` decoded entries.
 * @post No state is modified (pure validation).
 * @post On k_ra8_ok every page's stream is loadable through the staging buffer.
 *
 * @note Thread-safe: reads only its arguments.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
s_check_offsets(const ra8_cbz_t* cbz, const uint64_t* offsets, uint64_t payload_len)
{
  if (offsets[0] != 0U) {
    return k_ra8_err_invalid_arg;
  }
  uint64_t max_clen = 0U;
  for (uint32_t i = 0U; i < cbz->page_count; ++i) { /* bound: validated page_count */
    if (offsets[i + 1U] <= offsets[i]) {
      return k_ra8_err_invalid_arg;
    }
    const uint64_t clen = offsets[i + 1U] - offsets[i];
    if (clen > max_clen) {
      max_clen = clen;
    }
  }
  if (offsets[cbz->page_count] != payload_len) {
    return k_ra8_err_invalid_arg;
  }
  if (max_clen > (uint64_t)cbz->staging_cap) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Validate every page's inflated raster length is non-zero.
 *
 * @details Each page must inflate to at least one byte for its paged object to
 *          be registrable (::ra8_vsource_add_paged rejects a zero size); a
 *          degenerate zero-raster page is corrupt input.
 *
 * @param[in] meta       Base of the loaded metadata table (non-NULL).
 * @param[in] page_count Number of metadata records to check.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok              Every page has a non-zero raster length.
 * @retval k_ra8_err_invalid_arg Some page declares a zero raster length.
 *
 * @pre @p meta holds `page_count * k_ra8_cbz_meta_len` readable bytes.
 * @pre @p page_count matches the header's validated page count.
 * @post No state is modified (pure validation).
 * @post On k_ra8_ok every page's raster length is usable as an object size.
 *
 * @note Thread-safe: reads only its arguments.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_check_meta(const uint8_t* meta, uint32_t page_count)
{
  for (uint32_t i = 0U; i < page_count; ++i) { /* bound: validated page_count */
    const uint8_t* rec = &meta[(size_t)i * (size_t)k_ra8_cbz_meta_len];
    if (s_le32(&rec[k_ra8_cbz_meta_off_raw]) == 0U) {
      return k_ra8_err_invalid_arg;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Load + validate the offset and metadata tables through the reader.
 *
 * @details Sizes both tables from the already-parsed `page_count`, bounds them
 *          against the caller buffers, the file length and the one-call uint32
 *          read limit, reads each in one burst, then validates the offset shape
 *          and every page's raster length. On success `cbz->offsets`,
 *          `cbz->meta` and `cbz->payload_off` are bound.
 *
 * @param[in,out] cbz         Reader carrying the parsed `page_count`.
 * @param[in]     file_len    Container file length in bytes.
 * @param[out]    offsets_buf Caller buffer receiving the offset table.
 * @param[in]     offsets_cap Capacity of @p offsets_buf in uint64 entries.
 * @param[out]    meta_buf    Caller buffer receiving the metadata table.
 * @param[in]     meta_cap    Capacity of @p meta_buf in bytes.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok               Tables loaded and validated; @p cbz bound.
 * @retval k_ra8_err_invalid_arg  Offset shape or a raster length is invalid.
 * @retval k_ra8_err_invalid_size A table exceeds a caller buffer / file bounds /
 *                               the one-call read limit, or a page exceeds
 *                               staging.
 * @retval k_ra8_err_*            A file-read error, returned verbatim.
 *
 * @pre @p cbz's `page_count` / `staging_cap` were populated by the caller.
 * @pre @p offsets_buf and @p meta_buf cover their declared capacities.
 * @post On k_ra8_ok, `cbz->offsets`, `cbz->meta` and `cbz->payload_off` are set.
 * @post On any error `cbz->offsets` stays NULL (reader unbound).
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_load_tables(ra8_cbz_t* cbz,
                               uint64_t   file_len,
                               uint64_t*  offsets_buf,
                               uint32_t   offsets_cap,
                               uint8_t*   meta_buf,
                               uint32_t   meta_cap)
{
  const uint64_t entries      = (uint64_t)cbz->page_count + 1U;
  const uint64_t offset_bytes = entries * (uint64_t)k_ra8_cbz_offset_len;
  const uint64_t meta_bytes   = (uint64_t)cbz->page_count * (uint64_t)k_ra8_cbz_meta_len;
  if (entries > (uint64_t)offsets_cap) {
    return k_ra8_err_invalid_size;
  }
  if (meta_bytes > (uint64_t)meta_cap) {
    return k_ra8_err_invalid_size;
  }
  if (offset_bytes > (uint64_t)UINT32_MAX) {
    return k_ra8_err_invalid_size;
  }
  const uint64_t tables_end = (uint64_t)k_ra8_cbz_header_len + offset_bytes + meta_bytes;
  if (file_len < tables_end) {
    return k_ra8_err_invalid_size;
  }
  ra8_err_t err = cbz->file_read(cbz->file_ctx,
                                 (uint64_t)k_ra8_cbz_header_len,
                                 (uint8_t*)offsets_buf,
                                 (uint32_t)offset_bytes);
  if (err != k_ra8_ok) {
    return err;
  }
  err = cbz->file_read(cbz->file_ctx,
                       (uint64_t)k_ra8_cbz_header_len + offset_bytes,
                       meta_buf,
                       (uint32_t)meta_bytes);
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_err_t shape = s_check_offsets(cbz, offsets_buf, file_len - tables_end);
  if (shape != k_ra8_ok) {
    return shape;
  }
  const ra8_err_t mchk = s_check_meta(meta_buf, cbz->page_count);
  if (mchk != k_ra8_ok) {
    return mchk;
  }
  cbz->offsets     = offsets_buf;
  cbz->meta        = meta_buf;
  cbz->payload_off = tables_end;
  return k_ra8_ok;
}

/**
 * @brief The argument-checked body of ::ra8_cbz_open.
 *
 * @details Reads and parses the fixed header, then loads + validates both
 *          tables. The caller has already null-checked every argument and
 *          pre-bound the reader's callbacks and buffers.
 *
 * @param[in,out] cbz         Reader with callbacks/buffers pre-bound.
 * @param[in]     file_len    Container file length in bytes.
 * @param[out]    offsets_buf Caller buffer for the offset table.
 * @param[in]     offsets_cap Capacity of @p offsets_buf in uint64 entries.
 * @param[out]    meta_buf    Caller buffer for the metadata table.
 * @param[in]     meta_cap    Capacity of @p meta_buf in bytes.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok               Reader bound; geometry fully validated.
 * @retval k_ra8_err_invalid_arg  Bad magic / header fields / table shape.
 * @retval k_ra8_err_invalid_size Short file or a caller buffer too small.
 * @retval k_ra8_err_*            A file-read error, returned verbatim.
 *
 * @pre Every pointer argument was null-checked by the caller.
 * @pre `cbz->file_read` / `cbz->staging` / `cbz->staging_cap` are pre-bound.
 * @post On k_ra8_ok, `cbz->offsets` is bound and reads may be served.
 * @post On any error `cbz->offsets` stays NULL (reader unbound).
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_open_body(ra8_cbz_t* cbz,
                             uint64_t   file_len,
                             uint64_t*  offsets_buf,
                             uint32_t   offsets_cap,
                             uint8_t*   meta_buf,
                             uint32_t   meta_cap)
{
  if (file_len < (uint64_t)k_ra8_cbz_header_len) {
    return k_ra8_err_invalid_size;
  }
  uint8_t   hdr[k_ra8_cbz_header_len] = {};
  ra8_err_t err = cbz->file_read(cbz->file_ctx, 0U, hdr, (uint32_t)sizeof(hdr));
  if (err != k_ra8_ok) {
    return err;
  }
  err = s_parse_header(hdr, &cbz->page_count, &cbz->flags);
  if (err != k_ra8_ok) {
    return err;
  }
  return s_load_tables(cbz, file_len, offsets_buf, offsets_cap, meta_buf, meta_cap);
}

/**
 * @brief Reject any NULL required argument to ::ra8_cbz_open.
 *
 * @details Runs the six mandatory null guards so the entry point stays within
 *          the function-size budget. `file_ctx` and the capacity scalars are
 *          allowed to be zero, so they are not checked here.
 *
 * @param[in] cbz         Reader-to-populate pointer.
 * @param[in] file_read   Container byte reader.
 * @param[in] inflate     zlib decompressor.
 * @param[in] offsets_buf Caller offset-table buffer.
 * @param[in] meta_buf    Caller metadata-table buffer.
 * @param[in] staging     Caller compressed-page staging buffer.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok           Every required argument is non-NULL.
 * @retval k_ra8_err_null_ptr Some required argument was NULL.
 *
 * @pre The caller forwards ::ra8_cbz_open's arguments unchanged.
 * @pre No argument is dereferenced before this check returns k_ra8_ok.
 * @post On k_ra8_ok every checked pointer is safe to dereference.
 * @post On any error the reason is logged against ::s_tag_cbz.
 *
 * @note Thread-safe: reads only its pointer arguments.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_open_reject_null(const ra8_cbz_t*    cbz,
                                    ra8_vsource_read_fn file_read,
                                    ra8_book_inflate_fn inflate,
                                    const uint64_t*     offsets_buf,
                                    const uint8_t*      meta_buf,
                                    const uint8_t*      staging)
{
  RA8_CHECK_NULL_PTR(cbz, s_tag_cbz, "open: null cbz");
  RA8_CHECK_NULL_PTR((const void*)file_read, s_tag_cbz, "open: null file_read");
  RA8_CHECK_NULL_PTR((const void*)inflate, s_tag_cbz, "open: null inflate");
  RA8_CHECK_NULL_PTR(offsets_buf, s_tag_cbz, "open: null offsets_buf");
  RA8_CHECK_NULL_PTR(meta_buf, s_tag_cbz, "open: null meta_buf");
  RA8_CHECK_NULL_PTR(staging, s_tag_cbz, "open: null staging");
  return k_ra8_ok;
}

ra8_err_t ra8_cbz_open(ra8_cbz_t*          cbz,
                       ra8_vsource_read_fn file_read,
                       void*               file_ctx,
                       uint64_t            file_len,
                       ra8_book_inflate_fn inflate,
                       uint64_t*           offsets_buf,
                       uint32_t            offsets_cap,
                       uint8_t*            meta_buf,
                       uint32_t            meta_cap,
                       uint8_t*            staging,
                       uint32_t            staging_cap)
{
  const ra8_err_t nerr =
    s_open_reject_null(cbz, file_read, inflate, offsets_buf, meta_buf, staging);
  if (nerr != k_ra8_ok) {
    return nerr;
  }
  *cbz             = (ra8_cbz_t){};
  cbz->file_read   = file_read;
  cbz->file_ctx    = file_ctx;
  cbz->inflate     = inflate;
  cbz->staging     = staging;
  cbz->staging_cap = staging_cap;
  return s_open_body(cbz, file_len, offsets_buf, offsets_cap, meta_buf, meta_cap);
}

ra8_err_t ra8_cbz_page_info(const ra8_cbz_t* cbz,
                            uint32_t         page,
                            uint16_t*        out_width,
                            uint16_t*        out_height,
                            uint8_t*         out_format,
                            uint32_t*        out_raw_size)
{
  RA8_CHECK_NULL_PTR(cbz, s_tag_cbz, "info: null cbz");
  RA8_CHECK_NULL_PTR(out_width, s_tag_cbz, "info: null out_width");
  RA8_CHECK_NULL_PTR(out_height, s_tag_cbz, "info: null out_height");
  RA8_CHECK_NULL_PTR(out_format, s_tag_cbz, "info: null out_format");
  RA8_CHECK_NULL_PTR(out_raw_size, s_tag_cbz, "info: null out_raw_size");
  if (cbz->meta == nullptr) {
    return k_ra8_err_invalid_state;
  }
  if (page >= cbz->page_count) {
    return k_ra8_err_out_of_range;
  }
  s_read_meta(cbz->meta, page, out_width, out_height, out_format, out_raw_size);
  return k_ra8_ok;
}

ra8_err_t ra8_cbz_page_bind(const ra8_cbz_t* cbz, uint32_t page, ra8_cbz_page_t* out_cur)
{
  RA8_CHECK_NULL_PTR(cbz, s_tag_cbz, "bind: null cbz");
  RA8_CHECK_NULL_PTR(out_cur, s_tag_cbz, "bind: null out_cur");
  *out_cur = (ra8_cbz_page_t){};
  if (cbz->meta == nullptr) {
    return k_ra8_err_invalid_state;
  }
  if (page >= cbz->page_count) {
    return k_ra8_err_out_of_range;
  }
  uint16_t w   = 0U;
  uint16_t h   = 0U;
  uint8_t  fmt = 0U;
  uint32_t raw = 0U;
  s_read_meta(cbz->meta, page, &w, &h, &fmt, &raw);
  out_cur->cbz      = cbz;
  out_cur->page     = page;
  out_cur->raw_size = raw;
  return k_ra8_ok;
}

/**
 * @brief Stage one page's compressed stream and inflate it into @p buf.
 *
 * @details The I/O tail of ::ra8_cbz_page_read after the geometry checks: reads
 *          page @p page's zlib stream into staging through the file reader,
 *          inflates it into @p buf, and requires the produced length to equal
 *          @p raw_size. Split out so the contract checks and the staged I/O
 *          each stay within the function-size budget.
 *
 * @param[in]  cbz      Bound reader (tables validated at open).
 * @param[in]  page     Page index (`< page_count`, caller-bounded).
 * @param[in]  raw_size The page's exact inflated raster length.
 * @param[out] buf      Destination for exactly @p raw_size inflated bytes.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok               Page inflated into @p buf.
 * @retval k_ra8_err_invalid_size The stream inflated to the wrong length.
 * @retval k_ra8_err_*            A file-read or inflater error, verbatim.
 *
 * @pre @p page was bounds-checked against the validated page count.
 * @pre @p buf addresses at least @p raw_size writable bytes.
 * @post On k_ra8_ok, `buf[0..raw_size)` holds the page's inflated raster.
 * @post On any error @p buf contents are unspecified.
 *
 * @note Not thread-safe: the staging buffer is reused across calls.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
s_stage_and_inflate_page(const ra8_cbz_t* cbz, uint32_t page, uint32_t raw_size, uint8_t* buf)
{
  const uint64_t clen = cbz->offsets[page + 1U] - cbz->offsets[page];
  ra8_err_t      err  = cbz->file_read(cbz->file_ctx,
                                       cbz->payload_off + cbz->offsets[page],
                                       cbz->staging,
                                       (uint32_t)clen);
  if (err != k_ra8_ok) {
    return err;
  }
  size_t produced = 0U;
  err             = cbz->inflate(cbz->staging, (size_t)clen, buf, (size_t)raw_size, &produced);
  if (err != k_ra8_ok) {
    return err;
  }
  if (produced != (size_t)raw_size) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_cbz_page_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag_cbz, "read: null ctx");
  RA8_CHECK_NULL_PTR(buf, s_tag_cbz, "read: null buf");
  const ra8_cbz_page_t* cur = (const ra8_cbz_page_t*)ctx;
  if (cur->cbz == nullptr) {
    return k_ra8_err_invalid_state;
  }
  if (cur->cbz->offsets == nullptr) {
    return k_ra8_err_invalid_state;
  }
  if (offset != 0U) {
    return k_ra8_err_invalid_arg;
  }
  if ((uint64_t)len != (uint64_t)cur->raw_size) {
    return k_ra8_err_invalid_arg;
  }
  return s_stage_and_inflate_page(cur->cbz, cur->page, cur->raw_size, buf);
}
