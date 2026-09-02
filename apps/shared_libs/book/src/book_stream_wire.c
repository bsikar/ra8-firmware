/**
 * @file book_stream_wire.c
 * @brief Wire decoding and canonical layout validation for streamed books.
 *
 * @details
 * Owns the exact-read guard, fixed-header decoding, gap-free table geometry,
 * and interned-string boundary checks used by the semantic stream passes.
 * Keeping wire concerns here leaves book_stream.c responsible for coordinating
 * the chapter, DOM, attribute, stylesheet, image, and integrity validators.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 */
#include <string.h>

#include "book_stream_internal.h"
#include "ra8_attributes.h"

RA8_PRIV ra8_err_t priv_book_stream_read(const stream_validate_t* ctx,
                                         uint64_t                 off,
                                         uint8_t*                 dst,
                                         uint32_t                 len)
{
  if ((off > ctx->source_size) || ((uint64_t)len > (ctx->source_size - off))) {
    return k_ra8_err_invalid_size;
  }
  return ctx->read(ctx->read_ctx, off, dst, len);
}

/**
 * @brief Decode the fixed header from canonical little-endian wire bytes.
 * @details Copies the byte magic and decodes every scalar field explicitly;
 *          no packed-structure alias or host-endian assumption is used.
 * @param[in] raw Header wire bytes.
 * @param[out] hdr Decoded host-order header.
 * @pre @p raw holds exactly @ref k_book_sizeof_header bytes.
 * @pre @p hdr addresses one writable header object disjoint from @p raw.
 * @post Every header field is populated.
 * @post Bytes outside @p hdr are not modified.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_decode_header(const uint8_t* raw, book_header_t* hdr)
{
  (void)memcpy(hdr->magic, raw, sizeof(hdr->magic));
  hdr->format_version    = internal_book_stream_le32(&raw[k_stream_hdr_version]);
  hdr->total_size        = internal_book_stream_le32(&raw[k_stream_hdr_total]);
  hdr->flags             = internal_book_stream_le32(&raw[k_stream_hdr_flags]);
  hdr->title_off         = internal_book_stream_le32(&raw[k_stream_hdr_title]);
  hdr->author_off        = internal_book_stream_le32(&raw[k_stream_hdr_author]);
  hdr->language_off      = internal_book_stream_le32(&raw[k_stream_hdr_language]);
  hdr->identifier_off    = internal_book_stream_le32(&raw[k_stream_hdr_identifier]);
  hdr->cover_image_index = internal_book_stream_le32(&raw[k_stream_hdr_cover]);
  hdr->chapter_count     = internal_book_stream_le32(&raw[k_stream_hdr_chapter_count]);
  hdr->chapter_off       = internal_book_stream_le32(&raw[k_stream_hdr_chapter_off]);
  hdr->node_count        = internal_book_stream_le32(&raw[k_stream_hdr_node_count]);
  hdr->node_off          = internal_book_stream_le32(&raw[k_stream_hdr_node_off]);
  hdr->attr_count        = internal_book_stream_le32(&raw[k_stream_hdr_attr_count]);
  hdr->attr_off          = internal_book_stream_le32(&raw[k_stream_hdr_attr_off]);
  hdr->stylesheet_count  = internal_book_stream_le32(&raw[k_stream_hdr_style_count]);
  hdr->stylesheet_off    = internal_book_stream_le32(&raw[k_stream_hdr_style_off]);
  hdr->image_count       = internal_book_stream_le32(&raw[k_stream_hdr_image_count]);
  hdr->image_off         = internal_book_stream_le32(&raw[k_stream_hdr_image_off]);
  hdr->string_off        = internal_book_stream_le32(&raw[k_stream_hdr_string_off]);
  hdr->string_size       = internal_book_stream_le32(&raw[k_stream_hdr_string_size]);
  hdr->image_pool_off    = internal_book_stream_le32(&raw[k_stream_hdr_pool_off]);
  hdr->image_pool_size   = internal_book_stream_le32(&raw[k_stream_hdr_pool_size]);
  hdr->crc32_val         = internal_book_stream_le32(&raw[k_stream_hdr_crc]);
}

/**
 * @brief Require one segment to begin at the canonical cursor and advance it.
 * @details Enforces gap-free table layout and performs the count-by-element
 *          product in 64 bits before accepting a 32-bit wire offset.
 * @param[in] off Stored segment offset.
 * @param[in] count Number of records or bytes.
 * @param[in] elem Wire bytes per record.
 * @param[in,out] cursor Expected start and resulting end.
 * @return k_ra8_ok, or invalid-size for a gap, overlap, or 32-bit overflow.
 * @retval k_ra8_ok The segment begins at the cursor and its end is representable.
 * @retval k_ra8_err_invalid_size The offset differs or the end exceeds UINT32_MAX.
 * @pre @p cursor is non-NULL.
 * @pre @p cursor contains the validated end of the preceding segment.
 * @post Success advances @p cursor by count times elem.
 * @post Failure leaves @p cursor unchanged unless the start already matched.
 * @note Pure except for @p cursor.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_layout_segment(uint32_t off, uint32_t count, uint32_t elem, uint64_t* cursor)
{
  if ((uint64_t)off != *cursor) {
    return k_ra8_err_invalid_size;
  }
  const uint64_t end = *cursor + ((uint64_t)count * (uint64_t)elem);
  if (end > (uint64_t)UINT32_MAX) {
    return k_ra8_err_invalid_size;
  }
  *cursor = end;
  return k_ra8_ok;
}

/**
 * @brief Validate decoded header fields before walking the table layout.
 * @details Checks magic, supported features, exact source length, and scratch
 *          ownership-map capacity without reading any table data.
 * @param[in] ctx State containing a decoded header.
 * @return Strict decoded-header validation status.
 * @retval k_ra8_ok The decoded header fields are supported and bounded.
 * @retval k_ra8_err_invalid_arg Magic, version, or feature bits are invalid.
 * @retval k_ra8_err_invalid_size Source or scratch geometry is invalid.
 * @pre @p ctx is non-NULL and its header is decoded.
 * @pre ctx->source_size and scratch capacity describe accessible storage.
 * @post No state is modified.
 * @post Success permits validation of the canonical segment layout.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_header_fields(const stream_validate_t* ctx)
{
  static const char magic[8] = {'R', 'A', 'B', 'O', 'O', 'K', '1', '\0'};
  for (size_t i = 0U; i < sizeof(magic); ++i) {
    if (ctx->hdr.magic[i] != magic[i]) {
      return k_ra8_err_invalid_arg;
    }
  }
  if ((ctx->hdr.format_version != (uint32_t)k_book_format_version) ||
      ((ctx->hdr.flags & ~(uint32_t)k_book_flag_mask_known) != 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if ((uint64_t)ctx->hdr.total_size != ctx->source_size) {
    return k_ra8_err_invalid_size;
  }
  const uint64_t node_mark_bytes =
    ((uint64_t)ctx->hdr.node_count + k_stream_mark_round) / k_stream_bits_per_byte;
  return (node_mark_bytes > (uint64_t)ctx->scratch_cap) ? k_ra8_err_invalid_size : k_ra8_ok;
}

/**
 * @brief Validate version, flags, exact source length, and canonical layout.
 * @details Validates the decoded fields, then walks every table and pool in
 *          canonical wire order.
 * @param[in] ctx State containing a decoded header.
 * @return Strict header/layout validation status.
 * @retval k_ra8_ok The header and all segment extents are canonical.
 * @retval k_ra8_err_invalid_arg Magic, version, or feature bits are invalid.
 * @retval k_ra8_err_invalid_size Source, scratch, or segment geometry is invalid.
 * @pre @p ctx is non-NULL and its header is decoded.
 * @pre ctx->source_size and scratch capacity describe accessible storage.
 * @post No state is modified.
 * @post Success proves every later table and pool read is within the source.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_header_layout(const stream_validate_t* ctx)
{
  uint64_t  cursor = (uint64_t)k_book_sizeof_header;
  ra8_err_t err    = internal_validate_header_fields(ctx);
  if (err == k_ra8_ok) {
    err = internal_layout_segment(ctx->hdr.chapter_off,
                                  ctx->hdr.chapter_count,
                                  (uint32_t)k_book_sizeof_chapter,
                                  &cursor);
  }
  if (err == k_ra8_ok) {
    err = internal_layout_segment(ctx->hdr.node_off,
                                  ctx->hdr.node_count,
                                  (uint32_t)k_book_sizeof_node,
                                  &cursor);
  }
  if (err == k_ra8_ok) {
    err = internal_layout_segment(ctx->hdr.attr_off,
                                  ctx->hdr.attr_count,
                                  (uint32_t)k_book_sizeof_attr,
                                  &cursor);
  }
  if (err == k_ra8_ok) {
    err = internal_layout_segment(ctx->hdr.stylesheet_off,
                                  ctx->hdr.stylesheet_count,
                                  (uint32_t)k_book_sizeof_stylesheet,
                                  &cursor);
  }
  if (err == k_ra8_ok) {
    err = internal_layout_segment(ctx->hdr.image_off,
                                  ctx->hdr.image_count,
                                  (uint32_t)k_book_sizeof_image,
                                  &cursor);
  }
  if (err == k_ra8_ok) {
    err = internal_layout_segment(ctx->hdr.string_off, ctx->hdr.string_size, 1U, &cursor);
  }
  if (err == k_ra8_ok) {
    err = internal_layout_segment(ctx->hdr.image_pool_off, ctx->hdr.image_pool_size, 1U, &cursor);
  }
  if ((err == k_ra8_ok) && (cursor != (uint64_t)ctx->hdr.total_size)) {
    err = k_ra8_err_invalid_size;
  }
  return err;
}

RA8_PRIV ra8_err_t priv_book_stream_read_validate_header(stream_validate_t* ctx)
{
  uint8_t   raw[k_book_sizeof_header] = {};
  ra8_err_t err                       = priv_book_stream_read(ctx, 0U, raw, (uint32_t)sizeof(raw));
  if (err == k_ra8_ok) {
    internal_decode_header(raw, &ctx->hdr);
    err = internal_validate_header_layout(ctx);
  }
  return err;
}

RA8_PRIV ra8_err_t priv_book_stream_string_ref(const stream_validate_t* ctx, uint32_t off)
{
  if (off >= ctx->hdr.string_size) {
    return k_ra8_err_invalid_arg;
  }
  if (off == 0U) {
    return k_ra8_ok;
  }
  uint8_t   preceding = 0U;
  ra8_err_t err =
    priv_book_stream_read(ctx, (uint64_t)ctx->hdr.string_off + (uint64_t)off - 1U, &preceding, 1U);
  if ((err == k_ra8_ok) && (preceding != 0U)) {
    err = k_ra8_err_invalid_arg;
  }
  return err;
}

RA8_PRIV ra8_err_t priv_book_stream_nonempty_string_ref(const stream_validate_t* ctx, uint32_t off)
{
  ra8_err_t err   = priv_book_stream_string_ref(ctx, off);
  uint8_t   first = 0U;
  if (err == k_ra8_ok) {
    err = priv_book_stream_read(ctx, (uint64_t)ctx->hdr.string_off + off, &first, 1U);
  }
  if ((err == k_ra8_ok) && (first == 0U)) {
    err = k_ra8_err_invalid_arg;
  }
  return err;
}

RA8_PRIV ra8_err_t priv_book_stream_validate_string_envelope(const stream_validate_t* ctx)
{
  if (ctx->hdr.string_size == 0U) {
    return k_ra8_err_invalid_size;
  }
  uint8_t   first = 0U;
  uint8_t   last  = 0U;
  ra8_err_t err   = priv_book_stream_read(ctx, ctx->hdr.string_off, &first, 1U);
  if (err == k_ra8_ok) {
    err = priv_book_stream_read(ctx,
                                (uint64_t)ctx->hdr.string_off + (uint64_t)ctx->hdr.string_size - 1U,
                                &last,
                                1U);
  }
  if ((err == k_ra8_ok) && ((first != 0U) || (last != 0U))) {
    err = k_ra8_err_invalid_arg;
  }
  return err;
}
