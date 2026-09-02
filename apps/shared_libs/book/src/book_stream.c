/**
 * @file book_stream.c
 * @brief Strict callback-driven validation of a RABOOK1 flat blob.
 *
 * @details
 * Coordinates canonical wire-header validation with every semantic table,
 * DOM edge, image extent, and body-integrity pass. Bounded caller scratch
 * supplies both transfer storage and the DOM ownership map, so validation
 * neither maps the source nor allocates memory.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 */
#include "book_stream.h"

#include <string.h>

#include "book_internal.h"
#include "book_stream_internal.h"
#include "ra8_attributes.h"

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
    const ra8_err_t err = priv_book_stream_string_ref(ctx, refs[i]);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  if ((ctx->hdr.cover_image_index != (uint32_t)k_book_nil) &&
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
  uint8_t rec[k_book_sizeof_chapter] = {};
  for (uint32_t i = 0U; i < ctx->hdr.chapter_count; ++i) {
    const uint64_t off =
      (uint64_t)ctx->hdr.chapter_off + ((uint64_t)i * (uint64_t)k_book_sizeof_chapter);
    ra8_err_t err = priv_book_stream_read(ctx, off, rec, (uint32_t)sizeof(rec));
    if (err != k_ra8_ok) {
      return err;
    }
    err = priv_book_stream_string_ref(ctx, internal_book_stream_le32(&rec[0]));
    if (err == k_ra8_ok) {
      err = priv_book_stream_nonempty_string_ref(ctx, internal_book_stream_le32(&rec[4]));
    }
    const uint32_t root = internal_book_stream_le32(&rec[8]);
    if ((err == k_ra8_ok) && (root >= ctx->hdr.node_count)) {
      err = k_ra8_err_invalid_arg;
    }
    if (err == k_ra8_ok) {
      uint8_t node[k_book_sizeof_node] = {};
      err = priv_book_stream_read(ctx,
                                  (uint64_t)ctx->hdr.node_off +
                                    ((uint64_t)root * (uint64_t)k_book_sizeof_node),
                                  node,
                                  (uint32_t)sizeof(node));
      if ((err == k_ra8_ok) && ((node[k_stream_node_kind] != (uint8_t)k_book_node_element) ||
                                (internal_book_stream_le32(&node[k_stream_node_next_sibling]) !=
                                 (uint32_t)k_book_nil))) {
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
  if (link == (uint32_t)k_book_nil) {
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
  if ((err != k_ra8_ok) || (link == (uint32_t)k_book_nil)) {
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
  if (internal_book_stream_le32(&rec[k_stream_node_text]) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  ra8_err_t err =
    priv_book_stream_nonempty_string_ref(ctx, internal_book_stream_le32(&rec[k_stream_node_name]));
  if (err != k_ra8_ok) {
    return err;
  }
  const uint32_t first = internal_book_stream_le32(&rec[k_stream_node_first_attr]);
  const uint32_t count = internal_book_stream_le16(&rec[k_stream_node_attr_count]);
  if (count == 0U) {
    return (first == (uint32_t)k_book_nil) ? k_ra8_ok : k_ra8_err_invalid_arg;
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
  if ((internal_book_stream_le16(&rec[k_stream_node_attr_count]) != 0U) ||
      (internal_book_stream_le32(&rec[k_stream_node_name]) != 0U) ||
      (internal_book_stream_le32(&rec[k_stream_node_first_attr]) != (uint32_t)k_book_nil) ||
      (internal_book_stream_le32(&rec[k_stream_node_first_child]) != (uint32_t)k_book_nil)) {
    return k_ra8_err_invalid_arg;
  }
  return priv_book_stream_string_ref(ctx, internal_book_stream_le32(&rec[k_stream_node_text]));
}

/**
 * @brief Validate one node record's fields and mark its forward links.
 * @details Dispatches to the kind-specific validator (element or text), then
 *          marks the node's child and sibling links so the caller's
 *          ownership-coverage pass can prove every node was reached exactly
 *          once.
 * @param[in] ctx Validation state.
 * @param[in] rec Decoded fixed-size node record.
 * @param[in,out] attr_cursor Running attribute-ownership cursor.
 * @param[in] index Node index within the table, for forward-link marking.
 * @return Node validation status.
 * @retval k_ra8_ok The node, its links, and its attribute span are canonical.
 * @retval k_ra8_err_invalid_arg A kind, link, or attribute-span rule fails.
 * @pre @p rec was read from a valid node-table offset.
 * @pre @p attr_cursor reflects every prior node's attribute ownership.
 * @post Success advances `*attr_cursor` past this node's owned attributes.
 * @post Success marks this node's forward child/sibling links in scratch.
 * @note Not thread-safe with respect to the source callback.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_validate_one_node(const stream_validate_t* ctx,
                                                         const uint8_t*           rec,
                                                         uint32_t*                attr_cursor,
                                                         uint32_t                 index)
{
  ra8_err_t err;
  if (rec[k_stream_node_kind] == (uint8_t)k_book_node_element) {
    err = priv_book_stream_validate_element(ctx, rec, attr_cursor);
  } else if (rec[k_stream_node_kind] == (uint8_t)k_book_node_text) {
    err = priv_book_stream_validate_text(ctx, rec);
  } else {
    err = k_ra8_err_invalid_arg;
  }
  if (err == k_ra8_ok) {
    err = internal_mark_forward_link(ctx,
                                     internal_book_stream_le32(&rec[k_stream_node_first_child]),
                                     index);
  }
  if (err == k_ra8_ok) {
    err = internal_mark_forward_link(ctx,
                                     internal_book_stream_le32(&rec[k_stream_node_next_sibling]),
                                     index);
  }
  return err;
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
  uint8_t  rec[k_book_sizeof_node] = {};
  uint32_t attr_cursor             = 0U;
  for (uint32_t i = 0U; i < ctx->hdr.node_count; ++i) {
    const uint64_t off = (uint64_t)ctx->hdr.node_off + ((uint64_t)i * (uint64_t)k_book_sizeof_node);
    ra8_err_t      err = priv_book_stream_read(ctx, off, rec, (uint32_t)sizeof(rec));
    if (err != k_ra8_ok) {
      return err;
    }
    if (rec[k_stream_node_reserved] != 0U) {
      return k_ra8_err_invalid_arg;
    }
    err = internal_validate_one_node(ctx, rec, &attr_cursor, i);
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
  uint8_t rec[k_book_sizeof_attr] = {};
  for (uint32_t i = 0U; i < ctx->hdr.attr_count; ++i) {
    ra8_err_t err = priv_book_stream_read(ctx,
                                          (uint64_t)ctx->hdr.attr_off +
                                            ((uint64_t)i * (uint64_t)k_book_sizeof_attr),
                                          rec,
                                          (uint32_t)sizeof(rec));
    if (err == k_ra8_ok) {
      err = priv_book_stream_nonempty_string_ref(ctx, internal_book_stream_le32(&rec[0]));
    }
    if (err == k_ra8_ok) {
      err = priv_book_stream_string_ref(ctx, internal_book_stream_le32(&rec[4]));
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
  uint8_t rec[k_book_sizeof_stylesheet] = {};
  for (uint32_t i = 0U; i < ctx->hdr.stylesheet_count; ++i) {
    ra8_err_t err = priv_book_stream_read(ctx,
                                          (uint64_t)ctx->hdr.stylesheet_off +
                                            ((uint64_t)i * (uint64_t)k_book_sizeof_stylesheet),
                                          rec,
                                          (uint32_t)sizeof(rec));
    if (err == k_ra8_ok) {
      err = priv_book_stream_string_ref(ctx, internal_book_stream_le32(&rec[0]));
    }
    const uint32_t scope = internal_book_stream_le32(&rec[4]);
    if ((err == k_ra8_ok) && (scope != (uint32_t)k_book_nil) && (scope >= ctx->hdr.chapter_count)) {
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
  const uint32_t width  = internal_book_stream_le16(&rec[k_stream_image_width]);
  const uint32_t height = internal_book_stream_le16(&rec[k_stream_image_height]);
  const uint8_t  pixfmt = rec[k_stream_image_pixfmt];
  if ((width == 0U) || (height == 0U) ||
      ((pixfmt != (uint8_t)k_book_pixfmt_gray4) && (pixfmt != (uint8_t)k_book_pixfmt_gray8))) {
    return k_ra8_err_invalid_arg;
  }
  const uint64_t pixels = (uint64_t)width * (uint64_t)height;
  const uint64_t expect = (pixfmt == (uint8_t)k_book_pixfmt_gray4) ? ((pixels + 1U) / 2U) : pixels;
  // mcdc-deactivated: internal_validate_raster overflow backstop; width and height are decoded from 16-bit wire fields, so `pixels` is at most 65535*65535 == 0xFFFE0001 and `expect` (pixels, or half of it for gray4) can never exceed UINT32_MAX -- the first condition is provably constant-false and no input can flip it.
  if ((expect > (uint64_t)UINT32_MAX) ||
      (internal_book_stream_le32(&rec[k_stream_image_data_size]) != expect) ||
      (internal_book_stream_le32(&rec[k_stream_image_raw_size]) != expect)) {
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
  if ((internal_book_stream_le16(&rec[k_stream_image_width]) != 0U) ||
      (internal_book_stream_le16(&rec[k_stream_image_height]) != 0U) ||
      (rec[k_stream_image_pixfmt] != (uint8_t)k_book_pixfmt_gray4)) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t data_size = internal_book_stream_le32(&rec[k_stream_image_data_size]);
  return ((data_size != 0U) &&
          (data_size == internal_book_stream_le32(&rec[k_stream_image_raw_size])))
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
  uint8_t  rec[k_book_sizeof_image] = {};
  uint32_t pool_cursor              = 0U;
  for (uint32_t i = 0U; i < ctx->hdr.image_count; ++i) {
    ra8_err_t err = priv_book_stream_read(ctx,
                                          (uint64_t)ctx->hdr.image_off +
                                            ((uint64_t)i * (uint64_t)k_book_sizeof_image),
                                          rec,
                                          (uint32_t)sizeof(rec));
    if (err == k_ra8_ok) {
      err =
        priv_book_stream_nonempty_string_ref(ctx,
                                             internal_book_stream_le32(&rec[k_stream_image_id]));
    }
    if ((err == k_ra8_ok) && (internal_book_stream_le16(&rec[k_stream_image_reserved]) != 0U)) {
      err = k_ra8_err_invalid_arg;
    }
    if (err == k_ra8_ok) {
      if (rec[k_stream_image_format] == (uint8_t)k_book_image_gray4) {
        err = internal_validate_raster(rec);
      } else if (rec[k_stream_image_format] == (uint8_t)k_book_image_svg) {
        err = internal_validate_svg(rec);
      } else {
        err = k_ra8_err_invalid_arg;
      }
    }
    const uint32_t data_off  = internal_book_stream_le32(&rec[k_stream_image_data_off]);
    const uint32_t data_size = internal_book_stream_le32(&rec[k_stream_image_data_size]);
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
  uint64_t at  = (uint64_t)k_book_sizeof_header;
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
  return (crc == ctx->hdr.crc32_val) ? k_ra8_ok : k_ra8_err_range_check_failed;
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
  ra8_err_t err = priv_book_stream_read_validate_header(ctx);
  if (err == k_ra8_ok) {
    err = priv_book_stream_validate_string_envelope(ctx);
  }
  if (err == k_ra8_ok) {
    err = priv_book_stream_validate_metadata(ctx);
  }
  if (err == k_ra8_ok) {
    const uint32_t mark_bytes =
      (ctx->hdr.node_count / 8U) + (((ctx->hdr.node_count % 8U) != 0U) ? 1U : 0U);
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

ra8_err_t book_validate_stream_strict(book_stream_read_fn read,
                                      void*               read_ctx,
                                      uint64_t            source_size,
                                      uint8_t*            scratch,
                                      uint32_t            scratch_cap,
                                      book_header_t*      out_header)
{
  if (out_header == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out_header = (book_header_t){};
  if ((read == nullptr) || (scratch == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((source_size < (uint64_t)k_book_sizeof_header) || (scratch_cap == 0U)) {
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
