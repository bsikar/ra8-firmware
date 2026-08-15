/**
 * @file ra8_book_stream.c
 * @brief Strict callback-driven validation of a RABOOK1 flat blob.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 */
#include "ra8_book_stream.h"

#include <string.h>

#include "ra8_attributes.h"
#include "ra8_book_internal.h"

/** @brief Wire offsets inside the fixed 100-byte RABOOK1 header. */
typedef enum : uint8_t {
  k_stream_hdr_version       = 8U,
  k_stream_hdr_total         = 12U,
  k_stream_hdr_flags         = 16U,
  k_stream_hdr_title         = 20U,
  k_stream_hdr_author        = 24U,
  k_stream_hdr_language      = 28U,
  k_stream_hdr_identifier    = 32U,
  k_stream_hdr_cover         = 36U,
  k_stream_hdr_chapter_count = 40U,
  k_stream_hdr_chapter_off   = 44U,
  k_stream_hdr_node_count    = 48U,
  k_stream_hdr_node_off      = 52U,
  k_stream_hdr_attr_count    = 56U,
  k_stream_hdr_attr_off      = 60U,
  k_stream_hdr_style_count   = 64U,
  k_stream_hdr_style_off     = 68U,
  k_stream_hdr_image_count   = 72U,
  k_stream_hdr_image_off     = 76U,
  k_stream_hdr_string_off    = 80U,
  k_stream_hdr_string_size   = 84U,
  k_stream_hdr_pool_off      = 88U,
  k_stream_hdr_pool_size     = 92U,
  k_stream_hdr_crc           = 96U,
} stream_header_off_t;

/** @brief Wire offsets inside a 24-byte DOM node record. */
typedef enum : uint8_t {
  k_stream_node_kind         = 0U,
  k_stream_node_reserved     = 1U,
  k_stream_node_attr_count   = 2U,
  k_stream_node_name         = 4U,
  k_stream_node_text         = 8U,
  k_stream_node_first_attr   = 12U,
  k_stream_node_first_child  = 16U,
  k_stream_node_next_sibling = 20U,
} stream_node_off_t;

/** @brief Wire offsets inside a 24-byte image descriptor. */
typedef enum : uint8_t {
  k_stream_image_id        = 0U,
  k_stream_image_width     = 4U,
  k_stream_image_height    = 6U,
  k_stream_image_format    = 8U,
  k_stream_image_pixfmt    = 9U,
  k_stream_image_reserved  = 10U,
  k_stream_image_data_off  = 12U,
  k_stream_image_data_size = 16U,
  k_stream_image_raw_size  = 20U,
} stream_image_off_t;

/** @brief Bit geometry shared by wire decoding and the node ownership map. */
typedef enum : uint8_t {
  k_stream_bits_per_byte = 8U,  /**< Bits represented by one byte.             */
  k_stream_mark_round    = 7U,  /**< Numerator bias for ceiling division by 8. */
  k_stream_le_shift_3    = 24U, /**< Bit shift of byte three in a uint32.       */
} stream_bit_t;

/** @brief Immutable validation state shared by the bounded table passes. */
typedef struct {
  ra8_book_stream_read_fn read;        /**< Exact source callback.       */
  void*                   read_ctx;    /**< Callback context.            */
  uint64_t                source_size; /**< Exact flat-source byte size. */
  uint8_t*                scratch;     /**< Caller transfer buffer.      */
  uint32_t                scratch_cap; /**< Transfer-buffer capacity.    */
  ra8_book_header_t       hdr;         /**< Decoded host-order header.   */
} stream_validate_t;

/** @brief Decode one little-endian 16-bit field from unaligned bytes. */
RA8_INTERNAL
static uint16_t internal_le16(const uint8_t* p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << k_stream_bits_per_byte));
}

/** @brief Decode one little-endian 32-bit field from unaligned bytes. */
RA8_INTERNAL
static uint32_t internal_le32(const uint8_t* p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << k_stream_bits_per_byte) |
         ((uint32_t)p[2] << (2U * k_stream_bits_per_byte)) |
         ((uint32_t)p[3] << k_stream_le_shift_3);
}

/**
 * @brief Read an exact, already-sized source span.
 * @param[in] ctx Validation source.
 * @param[in] off Source byte offset.
 * @param[out] dst Destination buffer.
 * @param[in] len Exact byte count.
 * @return Callback status, or invalid-size when the request exceeds the source.
 * @pre All pointers are non-NULL.
 * @post Success fills all @p len destination bytes.
 * @note Not thread-safe with respect to @p ctx.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_read(const stream_validate_t* ctx, uint64_t off, uint8_t* dst, uint32_t len)
{
  if ((off > ctx->source_size) || ((uint64_t)len > (ctx->source_size - off))) {
    return k_ra8_err_invalid_size;
  }
  return ctx->read(ctx->read_ctx, off, dst, len);
}

/**
 * @brief Decode the fixed header from canonical little-endian wire bytes.
 * @param[in] raw Header wire bytes.
 * @param[out] hdr Decoded host-order header.
 * @pre @p raw holds exactly @ref k_ra8_book_sizeof_header bytes.
 * @post Every header field is populated.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_decode_header(const uint8_t* raw, ra8_book_header_t* hdr)
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
 * @param[in] off Stored segment offset.
 * @param[in] count Number of records or bytes.
 * @param[in] elem Wire bytes per record.
 * @param[in,out] cursor Expected start and resulting end.
 * @return k_ra8_ok, or invalid-size for a gap, overlap, or 32-bit overflow.
 * @pre @p cursor is non-NULL.
 * @post Success advances @p cursor by count times elem.
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
 * @param[in] ctx State containing a decoded header.
 * @return Strict header/layout validation status.
 * @pre @p ctx is non-NULL and its header is decoded.
 * @post No state is modified.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_header_layout(const stream_validate_t* ctx)
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
 * @param[in] ctx Validated layout state.
 * @param[in] off Offset relative to the string pool.
 * @return k_ra8_ok when @p off is in-range and begins an interned string.
 * @pre String-pool envelope bytes were validated.
 * @post No state is modified.
 * @note May perform one single-byte callback read.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_string_ref(const stream_validate_t* ctx, uint32_t off)
{
  if (off >= ctx->hdr.string_size) {
    return k_ra8_err_invalid_arg;
  }
  if (off == 0U) {
    return k_ra8_ok;
  }
  uint8_t   preceding = 0U;
  ra8_err_t err =
    internal_read(ctx, (uint64_t)ctx->hdr.string_off + (uint64_t)off - 1U, &preceding, 1U);
  if ((err == k_ra8_ok) && (preceding != 0U)) {
    err = k_ra8_err_invalid_arg;
  }
  return err;
}

/**
 * @brief Require a string reference to name a non-empty interned string.
 * @param[in] ctx Validated layout state.
 * @param[in] off Offset relative to the string pool.
 * @return k_ra8_ok when the boundary starts with a non-NUL byte.
 * @pre String-pool envelope bytes were validated.
 * @post No state is modified.
 * @note Performs at most two one-byte callback reads.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_nonempty_string_ref(const stream_validate_t* ctx, uint32_t off)
{
  ra8_err_t err   = internal_string_ref(ctx, off);
  uint8_t   first = 0U;
  if (err == k_ra8_ok) {
    err = internal_read(ctx, (uint64_t)ctx->hdr.string_off + off, &first, 1U);
  }
  if ((err == k_ra8_ok) && (first == 0U)) {
    err = k_ra8_err_invalid_arg;
  }
  return err;
}

/**
 * @brief Validate the empty-string sentinel and terminal pool NUL.
 * @param[in] ctx Validated layout state.
 * @return k_ra8_ok when the pool is non-empty and bounded by NUL bytes.
 * @pre String layout lies within the source.
 * @post No state is modified.
 * @note Performs at most two one-byte reads.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_string_envelope(const stream_validate_t* ctx)
{
  if (ctx->hdr.string_size == 0U) {
    return k_ra8_err_invalid_size;
  }
  uint8_t   first = 0U;
  uint8_t   last  = 0U;
  ra8_err_t err   = internal_read(ctx, ctx->hdr.string_off, &first, 1U);
  if (err == k_ra8_ok) {
    err = internal_read(ctx,
                        (uint64_t)ctx->hdr.string_off + (uint64_t)ctx->hdr.string_size - 1U,
                        &last,
                        1U);
  }
  if ((err == k_ra8_ok) && ((first != 0U) || (last != 0U))) {
    err = k_ra8_err_invalid_arg;
  }
  return err;
}

/**
 * @brief Validate the four metadata strings and optional cover index.
 * @param[in] ctx Validation state.
 * @return Metadata validation status.
 * @pre Header and string envelope are valid.
 * @post No state is modified.
 * @note Not thread-safe with respect to the source callback.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_metadata(const stream_validate_t* ctx)
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
 * @param[in] ctx Validation state.
 * @return Chapter-table validation status.
 * @pre Header layout and string envelope are valid.
 * @post No state is modified.
 * @note Iteration is bounded by chapter_count.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_chapters(const stream_validate_t* ctx)
{
  uint8_t rec[k_ra8_book_sizeof_chapter] = {};
  for (uint32_t i = 0U; i < ctx->hdr.chapter_count; ++i) {
    const uint64_t off =
      (uint64_t)ctx->hdr.chapter_off + ((uint64_t)i * (uint64_t)k_ra8_book_sizeof_chapter);
    ra8_err_t err = internal_read(ctx, off, rec, (uint32_t)sizeof(rec));
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
      err = internal_read(ctx,
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
 * @param[in] link Candidate node index or nil.
 * @param[in] current Index of the owning node.
 * @param[in] count Total node count.
 * @return k_ra8_ok for nil or a strictly forward in-range link.
 * @pre @p current is less than @p count.
 * @post No state is modified.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_forward_link(uint32_t link, uint32_t current, uint32_t count)
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
 * @param[in] ctx Validation state whose scratch holds ownership bits.
 * @param[in] link Candidate node index or nil.
 * @param[in] current Index of the linking node.
 * @return k_ra8_ok for nil or one unique strictly-forward reference.
 * @pre Ownership scratch was cleared and sized for node_count bits.
 * @post Success on a non-nil link marks exactly one target bit.
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

/**
 * @brief Validate one element node and advance canonical attribute ownership.
 * @param[in] ctx Validation state.
 * @param[in] rec Decoded-wire node bytes.
 * @param[in,out] attr_cursor Next unowned attribute index.
 * @return Element validation status.
 * @pre Node kind is element and @p attr_cursor is in range.
 * @post Success consumes exactly the node's contiguous attribute span.
 * @note Not thread-safe with respect to the source callback.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_validate_element(const stream_validate_t* ctx, const uint8_t* rec, uint32_t* attr_cursor)
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

/**
 * @brief Validate one text-node invariant set.
 * @param[in] ctx Validation state.
 * @param[in] rec Decoded-wire node bytes.
 * @return Text-node validation status.
 * @pre Node kind is text.
 * @post No state is modified.
 * @note Not thread-safe with respect to the source callback.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_text(const stream_validate_t* ctx, const uint8_t* rec)
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
 * @param[in] ctx Validation state.
 * @return Node-table validation status.
 * @pre Header layout and string envelope are valid.
 * @post No state is modified.
 * @note Forward links make cycles impossible without recursion or a visited set.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_nodes(const stream_validate_t* ctx)
{
  uint8_t  rec[k_ra8_book_sizeof_node] = {};
  uint32_t attr_cursor                 = 0U;
  for (uint32_t i = 0U; i < ctx->hdr.node_count; ++i) {
    const uint64_t off =
      (uint64_t)ctx->hdr.node_off + ((uint64_t)i * (uint64_t)k_ra8_book_sizeof_node);
    ra8_err_t err = internal_read(ctx, off, rec, (uint32_t)sizeof(rec));
    if (err != k_ra8_ok) {
      return err;
    }
    if (rec[k_stream_node_reserved] != 0U) {
      return k_ra8_err_invalid_arg;
    }
    if (rec[k_stream_node_kind] == (uint8_t)k_ra8_book_node_element) {
      err = internal_validate_element(ctx, rec, &attr_cursor);
    } else if (rec[k_stream_node_kind] == (uint8_t)k_ra8_book_node_text) {
      err = internal_validate_text(ctx, rec);
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
 * @param[in] ctx Validation state.
 * @return Attribute-table validation status.
 * @pre Node validation proved exact attribute ownership.
 * @post No state is modified.
 * @note Iteration is bounded by attr_count.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_attrs(const stream_validate_t* ctx)
{
  uint8_t rec[k_ra8_book_sizeof_attr] = {};
  for (uint32_t i = 0U; i < ctx->hdr.attr_count; ++i) {
    ra8_err_t err =
      internal_read(ctx,
                    (uint64_t)ctx->hdr.attr_off + ((uint64_t)i * (uint64_t)k_ra8_book_sizeof_attr),
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

/**
 * @brief Validate every stylesheet source and scope.
 * @param[in] ctx Validation state.
 * @return Stylesheet-table validation status.
 * @pre Header layout and string envelope are valid.
 * @post No state is modified.
 * @note Iteration is bounded by stylesheet_count.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_styles(const stream_validate_t* ctx)
{
  uint8_t rec[k_ra8_book_sizeof_stylesheet] = {};
  for (uint32_t i = 0U; i < ctx->hdr.stylesheet_count; ++i) {
    ra8_err_t err = internal_read(ctx,
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
 * @param[in] rec Image descriptor wire bytes.
 * @return Raster semantic validation status.
 * @pre @p rec names the raster format.
 * @post No state is modified.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_raster(const uint8_t* rec)
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
  if ((expect > (uint64_t)UINT32_MAX) ||
      (internal_le32(&rec[k_stream_image_data_size]) != expect) ||
      (internal_le32(&rec[k_stream_image_raw_size]) != expect)) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Validate one SVG image's zero extent/depth and raw-storage length.
 * @param[in] rec Image descriptor wire bytes.
 * @return SVG semantic validation status.
 * @pre @p rec names the SVG format.
 * @post No state is modified.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_svg(const uint8_t* rec)
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
 * @param[in] ctx Validation state.
 * @return Image-table validation status.
 * @pre Header layout and string envelope are valid.
 * @post No state is modified.
 * @note Iteration is bounded by image_count.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_images(const stream_validate_t* ctx)
{
  uint8_t  rec[k_ra8_book_sizeof_image] = {};
  uint32_t pool_cursor                  = 0U;
  for (uint32_t i = 0U; i < ctx->hdr.image_count; ++i) {
    ra8_err_t err = internal_read(ctx,
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
 * @param[in] ctx Validation state.
 * @return Full-body CRC validation status.
 * @pre Header layout is valid and scratch capacity is non-zero.
 * @post No source or validation state is modified.
 * @note Iteration is bounded by total_size and scratch_cap.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_crc(const stream_validate_t* ctx)
{
  uint64_t at  = (uint64_t)k_ra8_book_sizeof_header;
  uint32_t crc = 0U;
  while (at < (uint64_t)ctx->hdr.total_size) {
    uint64_t remain = (uint64_t)ctx->hdr.total_size - at;
    uint32_t span   = ctx->scratch_cap;
    if (remain < (uint64_t)span) {
      span = (uint32_t)remain;
    }
    const ra8_err_t err = internal_read(ctx, at, ctx->scratch, span);
    if (err != k_ra8_ok) {
      return err;
    }
    crc = ra8_book_crc32_extend(crc, ctx->scratch, span);
    at += span;
  }
  return (crc == ctx->hdr.crc32) ? k_ra8_ok : k_ra8_err_range_check_failed;
}

/**
 * @brief Run the strict passes after public argument validation.
 * @param[in,out] ctx Initialized validation state.
 * @return First strict validation error, or k_ra8_ok.
 * @pre All pointers and scratch capacity are valid.
 * @post Success leaves ctx->hdr fully decoded and validated.
 * @note Not thread-safe with respect to the callback source.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_body(stream_validate_t* ctx)
{
  uint8_t   raw[k_ra8_book_sizeof_header] = {};
  ra8_err_t err                           = internal_read(ctx, 0U, raw, (uint32_t)sizeof(raw));
  if (err == k_ra8_ok) {
    internal_decode_header(raw, &ctx->hdr);
    err = internal_validate_header_layout(ctx);
  }
  if (err == k_ra8_ok) {
    err = internal_validate_string_envelope(ctx);
  }
  if (err == k_ra8_ok) {
    err = internal_validate_metadata(ctx);
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
    err = internal_validate_styles(ctx);
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
