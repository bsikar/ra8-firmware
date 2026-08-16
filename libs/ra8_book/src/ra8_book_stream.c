/**
 * @file ra8_book_stream.c
 * @brief Strict callback-driven validation of a RABOOK1 flat blob.
 *
 * @details
 * Decodes the canonical little-endian header and validates every table,
 * string, DOM edge, image extent, and body byte through an exact random-read
 * callback. Bounded caller scratch supplies both transfer storage and the DOM
 * ownership map, so validation neither maps the source nor allocates memory.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 */
#include "ra8_book_stream.h"

#include <string.h>

#include "ra8_attributes.h"
#include "ra8_book_internal.h"
#include "ra8_book_stream_internal.h"

/**
 * @brief Decode one little-endian 16-bit field from unaligned bytes.
 * @details Combines two octets explicitly so behavior is independent of host
 *          byte order and does not require an aligned integer load.
 * @param[in] p Readable two-byte wire field.
 * @return Host-order unsigned field value.
 * @retval UINT16_C(0) Both wire bytes are zero.
 * @retval UINT16_MAX Both wire bytes are 0xff.
 * @pre @p p addresses at least two readable bytes.
 * @pre The source bytes remain stable for the duration of the two reads.
 * @post No memory or external state is modified.
 * @post The result is exactly `p[0] | (p[1] << 8)`.
 * @note Pure and thread-safe for immutable input.
 * @since Version 0.1.0
 */
RA8_INTERNAL static uint16_t internal_le16(const uint8_t* p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << k_stream_bits_per_byte));
}

/**
 * @brief Decode one little-endian 32-bit field from unaligned bytes.
 * @details Combines four octets explicitly so behavior is independent of host
 *          byte order and does not require an aligned integer load.
 * @param[in] p Readable four-byte wire field.
 * @return Host-order unsigned field value.
 * @retval UINT32_C(0) All four wire bytes are zero.
 * @retval UINT32_MAX All four wire bytes are 0xff.
 * @pre @p p addresses at least four readable bytes.
 * @pre The source bytes remain stable for the duration of the four reads.
 * @post No memory or external state is modified.
 * @post The result is the exact canonical little-endian decoding.
 * @note Pure and thread-safe for immutable input.
 * @since Version 0.1.0
 */
RA8_INTERNAL static uint32_t internal_le32(const uint8_t* p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << k_stream_bits_per_byte) |
         ((uint32_t)p[2] << (2U * k_stream_bits_per_byte)) |
         ((uint32_t)p[3] << k_stream_le_shift_3);
}

/*
 * @brief Read an exact, already-sized source span.
 * @details Performs an overflow-safe bounds proof before invoking the injected
 *          exact-read callback, so malformed wire extents never reach storage.
 * @param[in] ctx Validation source.
 * @param[in] off Source byte offset.
 * @param[out] dst Destination buffer.
 * @param[in] len Exact byte count.
 * @return Callback status, or invalid-size when the request exceeds the source.
 * @retval k_ra8_ok The callback supplied exactly the requested bytes.
 * @retval k_ra8_err_invalid_size The half-open request exceeds the source.
 * @pre All pointers are non-NULL.
 * @pre @p dst addresses at least @p len writable bytes.
 * @post Success fills all @p len destination bytes.
 * @post An out-of-range request does not invoke the callback.
 * @note Not thread-safe with respect to @p ctx.
 * @since Version 0.1.0
 */
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
 * @pre @p raw holds exactly @ref k_ra8_book_sizeof_header bytes.
 * @pre @p hdr addresses one writable header object disjoint from @p raw.
 * @post Every header field is populated.
 * @post Bytes outside @p hdr are not modified.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_decode_header(const uint8_t* raw, ra8_book_header_t* hdr)
{
  (void)memcpy(hdr->magic, raw, sizeof(hdr->magic));
  hdr->format_version    = internal_le32(&raw[k_stream_hdr_version]);
  hdr->total_size        = internal_le32(&raw[k_stream_hdr_total]);
  hdr->flags             = internal_le32(&raw[k_stream_hdr_flags]);
  hdr->title_off         = internal_le32(&raw[k_stream_hdr_title]);
  hdr->author_off        = internal_le32(&raw[k_stream_hdr_author]);
  hdr->language_off      = internal_le32(&raw[k_stream_hdr_language]);
  hdr->identifier_off    = internal_le32(&raw[k_stream_hdr_identifier]);
  hdr->cover_image_index = internal_le32(&raw[k_stream_hdr_cover]);
  hdr->chapter_count     = internal_le32(&raw[k_stream_hdr_chapter_count]);
  hdr->chapter_off       = internal_le32(&raw[k_stream_hdr_chapter_off]);
  hdr->node_count        = internal_le32(&raw[k_stream_hdr_node_count]);
  hdr->node_off          = internal_le32(&raw[k_stream_hdr_node_off]);
  hdr->attr_count        = internal_le32(&raw[k_stream_hdr_attr_count]);
  hdr->attr_off          = internal_le32(&raw[k_stream_hdr_attr_off]);
  hdr->stylesheet_count  = internal_le32(&raw[k_stream_hdr_style_count]);
  hdr->stylesheet_off    = internal_le32(&raw[k_stream_hdr_style_off]);
  hdr->image_count       = internal_le32(&raw[k_stream_hdr_image_count]);
  hdr->image_off         = internal_le32(&raw[k_stream_hdr_image_off]);
  hdr->string_off        = internal_le32(&raw[k_stream_hdr_string_off]);
  hdr->string_size       = internal_le32(&raw[k_stream_hdr_string_size]);
  hdr->image_pool_off    = internal_le32(&raw[k_stream_hdr_pool_off]);
  hdr->image_pool_size   = internal_le32(&raw[k_stream_hdr_pool_size]);
  hdr->crc32             = internal_le32(&raw[k_stream_hdr_crc]);
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
 * @retval k_ra8_ok The segment begins at the cursor and its end is
 * representable.
 * @retval k_ra8_err_invalid_size The offset differs or the end exceeds
 * UINT32_MAX.
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
 * @brief Validate version, flags, exact source length, and canonical layout.
 * @details Checks magic and supported features, verifies scratch ownership-map
 *          capacity, then walks every table and pool in canonical wire order.
 * @param[in] ctx State containing a decoded header.
 * @return Strict header/layout validation status.
 * @retval k_ra8_ok The header and all segment extents are canonical.
 * @retval k_ra8_err_invalid_arg Magic, version, or feature bits are invalid.
 * @retval k_ra8_err_invalid_size Source, scratch, or segment geometry is
 * invalid.
 * @pre @p ctx is non-NULL and its header is decoded.
 * @pre ctx->source_size and scratch capacity describe accessible storage.
 * @post No state is modified.
 * @post Success proves every later table and pool read is within the source.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_header_layout(const stream_validate_t* ctx)
{
  static const char magic[8] = {'R', 'A', 'B', 'O', 'O', 'K', '1', '\0'};
  if (memcmp(ctx->hdr.magic, magic, sizeof(magic)) != 0) {
    return k_ra8_err_invalid_arg;
  }
  if ((ctx->hdr.format_version != (uint32_t)k_ra8_book_format_version) ||
      ((ctx->hdr.flags & ~(uint32_t)k_ra8_book_flag_mask_known) != 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if ((uint64_t)ctx->hdr.total_size != ctx->source_size) {
    return k_ra8_err_invalid_size;
  }
  const uint64_t node_mark_bytes =
    ((uint64_t)ctx->hdr.node_count + k_stream_mark_round) / k_stream_bits_per_byte;
  if (node_mark_bytes > (uint64_t)ctx->scratch_cap) {
    return k_ra8_err_invalid_size;
  }

  uint64_t  cursor = (uint64_t)k_ra8_book_sizeof_header;
  ra8_err_t err    = internal_layout_segment(ctx->hdr.chapter_off,
                                             ctx->hdr.chapter_count,
                                             (uint32_t)k_ra8_book_sizeof_chapter,
                                             &cursor);
  if (err == k_ra8_ok) {
    err = internal_layout_segment(ctx->hdr.node_off,
                                  ctx->hdr.node_count,
                                  (uint32_t)k_ra8_book_sizeof_node,
                                  &cursor);
  }
  if (err == k_ra8_ok) {
    err = internal_layout_segment(ctx->hdr.attr_off,
                                  ctx->hdr.attr_count,
                                  (uint32_t)k_ra8_book_sizeof_attr,
                                  &cursor);
  }
  if (err == k_ra8_ok) {
    err = internal_layout_segment(ctx->hdr.stylesheet_off,
                                  ctx->hdr.stylesheet_count,
                                  (uint32_t)k_ra8_book_sizeof_stylesheet,
                                  &cursor);
  }
  if (err == k_ra8_ok) {
    err = internal_layout_segment(ctx->hdr.image_off,
                                  ctx->hdr.image_count,
                                  (uint32_t)k_ra8_book_sizeof_image,
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

/**
 * @brief Require a referenced string offset to name a string boundary.
 * @details Accepts the empty-string sentinel at zero; other offsets must be
 *          in range and immediately preceded by a NUL terminator.
 * @param[in] ctx Validated layout state.
 * @param[in] off Offset relative to the string pool.
 * @return k_ra8_ok when @p off is in-range and begins an interned string.
 * @retval k_ra8_ok @p off names the sentinel or a validated string boundary.
 * @retval k_ra8_err_invalid_arg The offset is outside the pool or mid-string.
 * @pre String-pool envelope bytes were validated.
 * @pre @p ctx contains a callback and canonical string-pool extent.
 * @post No state is modified.
 * @post Success permits subsequent reads beginning at the referenced offset.
 * @note May perform one single-byte callback read.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_string_ref(const stream_validate_t* ctx, uint32_t off)
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

/**
 * @brief Require a string reference to name a non-empty interned string.
 * @details First proves the offset is a string boundary, then rejects a NUL as
 *          the referenced string's first byte.
 * @param[in] ctx Validated layout state.
 * @param[in] off Offset relative to the string pool.
 * @return k_ra8_ok when the boundary starts with a non-NUL byte.
 * @retval k_ra8_ok The reference names a non-empty interned string.
 * @retval k_ra8_err_invalid_arg The reference is invalid or names an empty
 * string.
 * @pre String-pool envelope bytes were validated.
 * @pre @p ctx contains a callback and canonical string-pool extent.
 * @post No state is modified.
 * @post Success proves at least one non-NUL byte follows the boundary.
 * @note Performs at most two one-byte callback reads.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_nonempty_string_ref(const stream_validate_t* ctx,
                                                           uint32_t                 off)
{
  ra8_err_t err   = internal_string_ref(ctx, off);
  uint8_t   first = 0U;
  if (err == k_ra8_ok) {
    err = priv_book_stream_read(ctx, (uint64_t)ctx->hdr.string_off + off, &first, 1U);
  }
  if ((err == k_ra8_ok) && (first == 0U)) {
    err = k_ra8_err_invalid_arg;
  }
  return err;
}

/*
 * @brief Validate the empty-string sentinel and terminal pool NUL.
 * @details Requires a non-empty string pool whose first and last bytes are NUL,
 *          establishing bounded sentinels for all later reference checks.
 * @param[in] ctx Validated layout state.
 * @return k_ra8_ok when the pool is non-empty and bounded by NUL bytes.
 * @retval k_ra8_ok Both required sentinel bytes are present.
 * @retval k_ra8_err_invalid_size The string pool is empty.
 * @retval k_ra8_err_invalid_arg Either boundary byte is non-NUL.
 * @pre String layout lies within the source.
 * @pre @p ctx contains a usable exact-read callback.
 * @post No state is modified.
 * @post Success establishes the pool's leading and trailing sentinels.
 * @note Performs at most two one-byte reads.
 * @since Version 0.1.0
 */
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

/*
 * @brief Validate the four metadata strings and optional cover index.
 * @details Validates every metadata string boundary and requires any non-nil
 *          cover reference to select an existing image descriptor.
 * @param[in] ctx Validation state.
 * @return Metadata validation status.
 * @retval k_ra8_ok All metadata references are valid.
 * @retval k_ra8_err_invalid_arg A string or cover-image reference is invalid.
 * @pre Header and string envelope are valid.
 * @pre Image count and metadata offsets are decoded from the validated header.
 * @post No state is modified.
 * @post Success makes all metadata references safe for later lookup.
 * @note Not thread-safe with respect to the source callback.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_book_stream_validate_metadata(const stream_validate_t* ctx)
{
  const uint32_t refs[4] = {
    ctx->hdr.title_off,
    ctx->hdr.author_off,
    ctx->hdr.language_off,
    ctx->hdr.identifier_off,
  };
  for (uint8_t i = 0U; i < (uint8_t)(sizeof(refs) / sizeof(refs[0])); ++i) {
    const ra8_err_t err = internal_string_ref(ctx, refs[i]);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  if ((ctx->hdr.cover_image_index != (uint32_t)k_ra8_book_nil) &&
      (ctx->hdr.cover_image_index >= ctx->hdr.image_count)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Validate every chapter string and root-node index.
 * @details Requires canonical title/id strings, an element root with no
 * sibling, and unique ownership of every chapter root in the scratch bitset.
 * @param[in] ctx Validation state.
 * @return Chapter-table validation status.
 * @retval k_ra8_ok Every chapter record and root is valid and unique.
 * @retval k_ra8_err_invalid_arg A reference, root kind, or ownership rule
 * fails.
 * @pre Header layout and string envelope are valid.
 * @pre The node ownership scratch bitset is zeroed and sufficiently large.
 * @post No source bytes or decoded header state are modified.
 * @post Success marks each chapter root exactly once in caller scratch.
 * @note Iteration is bounded by chapter_count.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_chapters(const stream_validate_t* ctx)
{
  uint8_t rec[k_ra8_book_sizeof_chapter] = {};
  for (uint32_t i = 0U; i < ctx->hdr.chapter_count; ++i) {
    const uint64_t off =
      (uint64_t)ctx->hdr.chapter_off + ((uint64_t)i * (uint64_t)k_ra8_book_sizeof_chapter);
    ra8_err_t err = priv_book_stream_read(ctx, off, rec, (uint32_t)sizeof(rec));
    if (err != k_ra8_ok) {
      return err;
    }
    err = internal_string_ref(ctx, internal_le32(&rec[0]));
    if (err == k_ra8_ok) {
      err = internal_nonempty_string_ref(ctx, internal_le32(&rec[4]));
    }
    const uint32_t root = internal_le32(&rec[8]);
    if ((err == k_ra8_ok) && (root >= ctx->hdr.node_count)) {
      err = k_ra8_err_invalid_arg;
    }
    if (err == k_ra8_ok) {
      uint8_t node[k_ra8_book_sizeof_node] = {};
      err = priv_book_stream_read(ctx,
                                  (uint64_t)ctx->hdr.node_off +
                                    ((uint64_t)root * (uint64_t)k_ra8_book_sizeof_node),
                                  node,
                                  (uint32_t)sizeof(node));
      if ((err == k_ra8_ok) &&
          ((node[k_stream_node_kind] != (uint8_t)k_ra8_book_node_element) ||
           (internal_le32(&node[k_stream_node_next_sibling]) != (uint32_t)k_ra8_book_nil))) {
        err = k_ra8_err_invalid_arg;
      }
    }
    if (err == k_ra8_ok) {
      const uint32_t byte = root / 8U;
      const uint8_t  mask = (uint8_t)(1U << (root % 8U));
      if ((ctx->scratch[byte] & mask) != 0U) {
        err = k_ra8_err_invalid_arg;
      } else {
        ctx->scratch[byte] |= mask;
      }
    }
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Validate one optional forward node link.
 * @details Treats nil as absent and otherwise requires the target to be both
 *          in range and greater than the owner, which excludes cycles.
 * @param[in] link Candidate node index or nil.
 * @param[in] current Index of the owning node.
 * @param[in] count Total node count.
 * @return k_ra8_ok for nil or a strictly forward in-range link.
 * @retval k_ra8_ok The link is nil or a valid forward target.
 * @retval k_ra8_err_invalid_arg The target is backward, self, or out of range.
 * @pre @p current is less than @p count.
 * @pre @p count is the validated node-table record count.
 * @post No state is modified.
 * @post Success proves a non-nil link cannot introduce a backward cycle.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_forward_link(uint32_t link, uint32_t current, uint32_t count)
{
  if (link == (uint32_t)k_ra8_book_nil) {
    return k_ra8_ok;
  }
  if ((link <= current) || (link >= count)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Record one unique incoming node reference in the caller bitset.
 * @details Validates forward-link geometry before testing and setting the
 *          corresponding ownership bit; duplicate parents fail closed.
 * @param[in] ctx Validation state whose scratch holds ownership bits.
 * @param[in] link Candidate node index or nil.
 * @param[in] current Index of the linking node.
 * @return k_ra8_ok for nil or one unique strictly-forward reference.
 * @retval k_ra8_ok The link is nil or was newly marked.
 * @retval k_ra8_err_invalid_arg The link is invalid or already owned.
 * @pre Ownership scratch was cleared and sized for node_count bits.
 * @pre @p current names an existing node in ctx->hdr.
 * @post Success on a non-nil link marks exactly one target bit.
 * @post Nil success leaves the ownership map unchanged.
 * @note Not thread-safe; mutates caller scratch only.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_mark_forward_link(const stream_validate_t* ctx, uint32_t link, uint32_t current)
{
  const ra8_err_t err = internal_forward_link(link, current, ctx->hdr.node_count);
  if ((err != k_ra8_ok) || (link == (uint32_t)k_ra8_book_nil)) {
    return err;
  }
  const uint32_t byte = link / 8U;
  const uint8_t  mask = (uint8_t)(1U << (link % 8U));
  if ((ctx->scratch[byte] & mask) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  ctx->scratch[byte] |= mask;
  return k_ra8_ok;
}

/*
 * @brief Validate one element node and advance canonical attribute ownership.
 * @details Requires element-only fields, a non-empty name, and either no
 *          attributes or the exact next contiguous attribute-table span.
 * @param[in] ctx Validation state.
 * @param[in] rec Decoded-wire node bytes.
 * @param[in,out] attr_cursor Next unowned attribute index.
 * @return Element validation status.
 * @retval k_ra8_ok The element fields and attribute span are canonical.
 * @retval k_ra8_err_invalid_arg An element invariant or reference is invalid.
 * @pre Node kind is element and @p attr_cursor is in range.
 * @pre @p rec addresses one complete node wire record.
 * @post Success consumes exactly the node's contiguous attribute span.
 * @post Failure never advances beyond the advertised attribute count.
 * @note Not thread-safe with respect to the source callback.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_book_stream_validate_element(const stream_validate_t* ctx,
                                                     const uint8_t*           rec,
                                                     uint32_t*                attr_cursor)
{
  if (internal_le32(&rec[k_stream_node_text]) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  ra8_err_t err = internal_nonempty_string_ref(ctx, internal_le32(&rec[k_stream_node_name]));
  if (err != k_ra8_ok) {
    return err;
  }
  const uint32_t first = internal_le32(&rec[k_stream_node_first_attr]);
  const uint32_t count = internal_le16(&rec[k_stream_node_attr_count]);
  if (count == 0U) {
    return (first == (uint32_t)k_ra8_book_nil) ? k_ra8_ok : k_ra8_err_invalid_arg;
  }
  if ((first != *attr_cursor) || (count > (ctx->hdr.attr_count - *attr_cursor))) {
    return k_ra8_err_invalid_arg;
  }
  *attr_cursor += count;
  return k_ra8_ok;
}

/*
 * @brief Validate one text-node invariant set.
 * @details Rejects element-only fields on text records, then validates the
 *          text string boundary through the shared string-pool contract.
 * @param[in] ctx Validation state.
 * @param[in] rec Decoded-wire node bytes.
 * @return Text-node validation status.
 * @retval k_ra8_ok The text node fields and string reference are valid.
 * @retval k_ra8_err_invalid_arg An element-only field or string reference is
 * invalid.
 * @pre Node kind is text.
 * @pre @p rec addresses one complete node wire record.
 * @post No state is modified.
 * @post Success proves the node owns no children or attributes.
 * @note Not thread-safe with respect to the source callback.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_book_stream_validate_text(const stream_validate_t* ctx, const uint8_t* rec)
{
  if ((internal_le16(&rec[k_stream_node_attr_count]) != 0U) ||
      (internal_le32(&rec[k_stream_node_name]) != 0U) ||
      (internal_le32(&rec[k_stream_node_first_attr]) != (uint32_t)k_ra8_book_nil) ||
      (internal_le32(&rec[k_stream_node_first_child]) != (uint32_t)k_ra8_book_nil)) {
    return k_ra8_err_invalid_arg;
  }
  return internal_string_ref(ctx, internal_le32(&rec[k_stream_node_text]));
}

/**
 * @brief Validate every DOM node and exact attribute ownership.
 * @details Walks nodes once, validates kind-specific fields, marks unique
 *          forward edges, and finally requires every node and attribute owned.
 * @param[in] ctx Validation state.
 * @return Node-table validation status.
 * @retval k_ra8_ok All nodes, links, and attribute spans are canonical.
 * @retval k_ra8_err_invalid_arg A node, link, ownership, or span rule fails.
 * @pre Header layout and string envelope are valid.
 * @pre Chapter validation has already marked each root in scratch.
 * @post No source bytes or decoded header state are modified.
 * @post Success leaves every node ownership bit set exactly once.
 * @note Forward links make cycles impossible without recursion or a visited
 * set.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_nodes(const stream_validate_t* ctx)
{
  uint8_t  rec[k_ra8_book_sizeof_node] = {};
  uint32_t attr_cursor                 = 0U;
  for (uint32_t i = 0U; i < ctx->hdr.node_count; ++i) {
    const uint64_t off =
      (uint64_t)ctx->hdr.node_off + ((uint64_t)i * (uint64_t)k_ra8_book_sizeof_node);
    ra8_err_t err = priv_book_stream_read(ctx, off, rec, (uint32_t)sizeof(rec));
    if (err != k_ra8_ok) {
      return err;
    }
    if (rec[k_stream_node_reserved] != 0U) {
      return k_ra8_err_invalid_arg;
    }
    if (rec[k_stream_node_kind] == (uint8_t)k_ra8_book_node_element) {
      err = priv_book_stream_validate_element(ctx, rec, &attr_cursor);
    } else if (rec[k_stream_node_kind] == (uint8_t)k_ra8_book_node_text) {
      err = priv_book_stream_validate_text(ctx, rec);
    } else {
      err = k_ra8_err_invalid_arg;
    }
    if (err == k_ra8_ok) {
      err = internal_mark_forward_link(ctx, internal_le32(&rec[k_stream_node_first_child]), i);
    }
    if (err == k_ra8_ok) {
      err = internal_mark_forward_link(ctx, internal_le32(&rec[k_stream_node_next_sibling]), i);
    }
    if (err != k_ra8_ok) {
      return err;
    }
  }
  if (attr_cursor != ctx->hdr.attr_count) {
    return k_ra8_err_invalid_arg;
  }
  for (uint32_t i = 0U; i < ctx->hdr.node_count; ++i) {
    const uint8_t mask = (uint8_t)(1U << (i % 8U));
    if ((ctx->scratch[i / 8U] & mask) == 0U) {
      return k_ra8_err_invalid_arg;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Validate every attribute name/value string reference.
 * @details Reads each fixed-size attribute record, requires a non-empty name,
 *          and accepts an empty or non-empty value at a valid boundary.
 * @param[in] ctx Validation state.
 * @return Attribute-table validation status.
 * @retval k_ra8_ok Every attribute string reference is valid.
 * @retval k_ra8_err_invalid_arg A name or value offset is not a string
 * boundary.
 * @pre Node validation proved exact attribute ownership.
 * @pre Header layout bounds every attribute record in the source.
 * @post No state is modified.
 * @post Success makes every attribute record safe for string lookup.
 * @note Iteration is bounded by attr_count.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_attrs(const stream_validate_t* ctx)
{
  uint8_t rec[k_ra8_book_sizeof_attr] = {};
  for (uint32_t i = 0U; i < ctx->hdr.attr_count; ++i) {
    ra8_err_t err = priv_book_stream_read(ctx,
                                          (uint64_t)ctx->hdr.attr_off +
                                            ((uint64_t)i * (uint64_t)k_ra8_book_sizeof_attr),
                                          rec,
                                          (uint32_t)sizeof(rec));
    if (err == k_ra8_ok) {
      err = internal_nonempty_string_ref(ctx, internal_le32(&rec[0]));
    }
    if (err == k_ra8_ok) {
      err = internal_string_ref(ctx, internal_le32(&rec[4]));
    }
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/*
 * @brief Validate every stylesheet source and scope.
 * @details Validates each stylesheet source string and permits only nil or an
 *          existing chapter index as its optional scope.
 * @param[in] ctx Validation state.
 * @return Stylesheet-table validation status.
 * @retval k_ra8_ok Every stylesheet source and scope is valid.
 * @retval k_ra8_err_invalid_arg A source boundary or scope index is invalid.
 * @pre Header layout and string envelope are valid.
 * @pre Chapter count is the validated table-record count.
 * @post No state is modified.
 * @post Success makes each stylesheet reference safe for later lookup.
 * @note Iteration is bounded by stylesheet_count.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_book_stream_validate_styles(const stream_validate_t* ctx)
{
  uint8_t rec[k_ra8_book_sizeof_stylesheet] = {};
  for (uint32_t i = 0U; i < ctx->hdr.stylesheet_count; ++i) {
    ra8_err_t err = priv_book_stream_read(ctx,
                                          (uint64_t)ctx->hdr.stylesheet_off +
                                            ((uint64_t)i * (uint64_t)k_ra8_book_sizeof_stylesheet),
                                          rec,
                                          (uint32_t)sizeof(rec));
    if (err == k_ra8_ok) {
      err = internal_string_ref(ctx, internal_le32(&rec[0]));
    }
    const uint32_t scope = internal_le32(&rec[4]);
    if ((err == k_ra8_ok) && (scope != (uint32_t)k_ra8_book_nil) &&
        (scope >= ctx->hdr.chapter_count)) {
      err = k_ra8_err_invalid_arg;
    }
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Validate one raster image's dimensions, depth, and exact byte count.
 * @details Requires non-zero dimensions, a supported gray depth, and exact
 *          packed-pixel data and raw lengths computed with 64-bit arithmetic.
 * @param[in] rec Image descriptor wire bytes.
 * @return Raster semantic validation status.
 * @retval k_ra8_ok Raster geometry and byte lengths are exact.
 * @retval k_ra8_err_invalid_arg Dimensions or pixel format are unsupported.
 * @retval k_ra8_err_invalid_size A computed or stored pixel extent is invalid.
 * @pre @p rec names the raster format.
 * @pre @p rec addresses one complete image wire record.
 * @post No state is modified.
 * @post Success proves the raster payload size from its dimensions and depth.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_raster(const uint8_t* rec)
{
  const uint32_t width  = internal_le16(&rec[k_stream_image_width]);
  const uint32_t height = internal_le16(&rec[k_stream_image_height]);
  const uint8_t  pixfmt = rec[k_stream_image_pixfmt];
  if ((width == 0U) || (height == 0U) ||
      ((pixfmt != (uint8_t)k_ra8_book_pixfmt_gray4) &&
       (pixfmt != (uint8_t)k_ra8_book_pixfmt_gray8))) {
    return k_ra8_err_invalid_arg;
  }
  const uint64_t pixels = (uint64_t)width * (uint64_t)height;
  const uint64_t expect =
    (pixfmt == (uint8_t)k_ra8_book_pixfmt_gray4) ? ((pixels + 1U) / 2U) : pixels;
  // mcdc-deactivated: internal_validate_raster overflow backstop; width and height are decoded from 16-bit wire fields, so `pixels` is at most 65535*65535 == 0xFFFE0001 and `expect` (pixels, or half of it for gray4) can never exceed UINT32_MAX -- the first condition is provably constant-false and no input can flip it.
  if ((expect > (uint64_t)UINT32_MAX) ||
      (internal_le32(&rec[k_stream_image_data_size]) != expect) ||
      (internal_le32(&rec[k_stream_image_raw_size]) != expect)) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Validate one SVG image's zero extent/depth and raw-storage length.
 * @details Enforces the SVG sentinel geometry and requires a non-empty stored
 *          source whose encoded and raw byte lengths are identical.
 * @param[in] rec Image descriptor wire bytes.
 * @return SVG semantic validation status.
 * @retval k_ra8_ok SVG sentinel fields and storage length are canonical.
 * @retval k_ra8_err_invalid_arg A sentinel field or byte length is invalid.
 * @pre @p rec names the SVG format.
 * @pre @p rec addresses one complete image wire record.
 * @post No state is modified.
 * @post Success proves the SVG source occupies a non-empty exact payload span.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_svg(const uint8_t* rec)
{
  if ((internal_le16(&rec[k_stream_image_width]) != 0U) ||
      (internal_le16(&rec[k_stream_image_height]) != 0U) ||
      (rec[k_stream_image_pixfmt] != (uint8_t)k_ra8_book_pixfmt_gray4)) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t data_size = internal_le32(&rec[k_stream_image_data_size]);
  return ((data_size != 0U) && (data_size == internal_le32(&rec[k_stream_image_raw_size])))
           ? k_ra8_ok
           : k_ra8_err_invalid_arg;
}

/**
 * @brief Validate every image descriptor and exact gap-free pool tiling.
 * @details Validates IDs and format semantics, then advances a pool cursor that
 *          rejects gaps, overlap, and trailing unowned image bytes.
 * @param[in] ctx Validation state.
 * @return Image-table validation status.
 * @retval k_ra8_ok Every image is valid and exactly tiles the pool.
 * @retval k_ra8_err_invalid_arg An ID, reserved field, or format rule fails.
 * @retval k_ra8_err_invalid_size Image byte geometry or pool tiling is invalid.
 * @pre Header layout and string envelope are valid.
 * @pre Every image record and the image pool lie within the validated source.
 * @post No state is modified.
 * @post Success proves each pool byte belongs to exactly one image in order.
 * @note Iteration is bounded by image_count.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_images(const stream_validate_t* ctx)
{
  uint8_t  rec[k_ra8_book_sizeof_image] = {};
  uint32_t pool_cursor                  = 0U;
  for (uint32_t i = 0U; i < ctx->hdr.image_count; ++i) {
    ra8_err_t err = priv_book_stream_read(ctx,
                                          (uint64_t)ctx->hdr.image_off +
                                            ((uint64_t)i * (uint64_t)k_ra8_book_sizeof_image),
                                          rec,
                                          (uint32_t)sizeof(rec));
    if (err == k_ra8_ok) {
      err = internal_nonempty_string_ref(ctx, internal_le32(&rec[k_stream_image_id]));
    }
    if ((err == k_ra8_ok) && (internal_le16(&rec[k_stream_image_reserved]) != 0U)) {
      err = k_ra8_err_invalid_arg;
    }
    if (err == k_ra8_ok) {
      if (rec[k_stream_image_format] == (uint8_t)k_ra8_book_image_gray4) {
        err = internal_validate_raster(rec);
      } else if (rec[k_stream_image_format] == (uint8_t)k_ra8_book_image_svg) {
        err = internal_validate_svg(rec);
      } else {
        err = k_ra8_err_invalid_arg;
      }
    }
    const uint32_t data_off  = internal_le32(&rec[k_stream_image_data_off]);
    const uint32_t data_size = internal_le32(&rec[k_stream_image_data_size]);
    if ((err == k_ra8_ok) &&
        ((data_off != pool_cursor) || (data_size > (ctx->hdr.image_pool_size - pool_cursor)))) {
      err = k_ra8_err_invalid_size;
    }
    if (err != k_ra8_ok) {
      return err;
    }
    pool_cursor += data_size;
  }
  return (pool_cursor == ctx->hdr.image_pool_size) ? k_ra8_ok : k_ra8_err_invalid_size;
}

/**
 * @brief Hash every body byte through the caller transfer buffer.
 * @details Reads the body in scratch-sized exact spans and extends the shared
 *          CRC convention without retaining the complete source.
 * @param[in] ctx Validation state.
 * @return Full-body CRC validation status.
 * @retval k_ra8_ok The computed body CRC equals the header value.
 * @retval k_ra8_err_range_check_failed The computed CRC differs.
 * @pre Header layout is valid and scratch capacity is non-zero.
 * @pre @p ctx contains a usable exact-read callback and writable scratch.
 * @post No source bytes or decoded header state are modified.
 * @post Success proves every body byte contributed exactly once in wire order.
 * @note Iteration is bounded by total_size and scratch_cap.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_crc(const stream_validate_t* ctx)
{
  uint64_t at  = (uint64_t)k_ra8_book_sizeof_header;
  uint32_t crc = 0U;
  while (at < (uint64_t)ctx->hdr.total_size) {
    uint64_t remain = (uint64_t)ctx->hdr.total_size - at;
    uint32_t span   = ctx->scratch_cap;
    if (remain < (uint64_t)span) {
      span = (uint32_t)remain;
    }
    const ra8_err_t err = priv_book_stream_read(ctx, at, ctx->scratch, span);
    if (err != k_ra8_ok) {
      return err;
    }
    crc = priv_book_crc32_extend(crc, ctx->scratch, span);
    at += span;
  }
  return (crc == ctx->hdr.crc32) ? k_ra8_ok : k_ra8_err_range_check_failed;
}

/**
 * @brief Run the strict passes after public argument validation.
 * @details Decodes the header, validates canonical layout and each semantic
 *          table in dependency order, then verifies the body CRC last.
 * @param[in,out] ctx Initialized validation state.
 * @return First strict validation error, or k_ra8_ok.
 * @retval k_ra8_ok Every structural, semantic, and integrity pass succeeded.
 * @retval k_ra8_err_invalid_arg A decoded semantic invariant failed.
 * @retval k_ra8_err_invalid_size A source, table, pool, or scratch extent
 * failed.
 * @pre All pointers and scratch capacity are valid.
 * @pre ctx->source_size is at least the fixed wire-header length.
 * @post Success leaves ctx->hdr fully decoded and validated.
 * @post Failure is returned immediately without publishing an output header.
 * @note Not thread-safe with respect to the callback source.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_body(stream_validate_t* ctx)
{
  uint8_t   raw[k_ra8_book_sizeof_header] = {};
  ra8_err_t err = priv_book_stream_read(ctx, 0U, raw, (uint32_t)sizeof(raw));
  if (err == k_ra8_ok) {
    internal_decode_header(raw, &ctx->hdr);
    err = internal_validate_header_layout(ctx);
  }
  if (err == k_ra8_ok) {
    err = priv_book_stream_validate_string_envelope(ctx);
  }
  if (err == k_ra8_ok) {
    err = priv_book_stream_validate_metadata(ctx);
  }
  if (err == k_ra8_ok) {
    const uint32_t mark_bytes =
      (ctx->hdr.node_count / 8U) + ((ctx->hdr.node_count % 8U) != 0U ? 1U : 0U);
    (void)memset(ctx->scratch, 0, mark_bytes);
  }
  if (err == k_ra8_ok) {
    err = internal_validate_chapters(ctx);
  }
  if (err == k_ra8_ok) {
    err = internal_validate_nodes(ctx);
  }
  if (err == k_ra8_ok) {
    err = internal_validate_attrs(ctx);
  }
  if (err == k_ra8_ok) {
    err = priv_book_stream_validate_styles(ctx);
  }
  if (err == k_ra8_ok) {
    err = internal_validate_images(ctx);
  }
  if (err == k_ra8_ok) {
    err = internal_validate_crc(ctx);
  }
  return err;
}

ra8_err_t ra8_book_validate_stream_strict(ra8_book_stream_read_fn read,
                                          void*                   read_ctx,
                                          uint64_t                source_size,
                                          uint8_t*                scratch,
                                          uint32_t                scratch_cap,
                                          ra8_book_header_t*      out_header)
{
  if (out_header == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out_header = (ra8_book_header_t){};
  if ((read == nullptr) || (scratch == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((source_size < (uint64_t)k_ra8_book_sizeof_header) || (scratch_cap == 0U)) {
    return k_ra8_err_invalid_size;
  }
  stream_validate_t ctx = {
    .read        = read,
    .read_ctx    = read_ctx,
    .source_size = source_size,
    .scratch     = scratch,
    .scratch_cap = scratch_cap,
    .hdr         = {},
  };
  const ra8_err_t err = internal_validate_body(&ctx);
  if (err == k_ra8_ok) {
    *out_header = ctx.hdr;
  }
  return err;
}
