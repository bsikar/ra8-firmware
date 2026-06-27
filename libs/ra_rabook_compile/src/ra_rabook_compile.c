/**
 * @file ra_rabook_compile.c
 * @brief RABOOK1 emitter implementation (see ra_rabook_compile.h).
 *
 * @details
 * Zero-heap builder back-end of the #149 on-device EPUB -> `.rabook` compiler.
 * Each builder call appends into a caller-provided arena; @ref ra_rabook_finalize
 * lays the tables and pools out at the contract offsets, fills the 100-byte
 * header, and CRC-32s the body so the blob passes @ref ra_book_validate. The
 * table order, field packing and CRC variant mirror
 * `tools/epub_compile/epub_compile.py` for byte compatibility. Records are the
 * pinned, padding-free structs from ra_book.h; the target and the host test are
 * both little-endian, so a copy of those structs IS the wire layout.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "ra_rabook_compile.h"

#include <string.h>

#include "ra_check.h"

/** @brief Component tag for log messages from this module. */
static const char* const s_tag_rabook = "ra_rabook_compile";

/**
 * @enum ra_rabook_crc_t
 * @brief CRC-32/ISO-HDLC constants -- match ra_book_validate() and zlib.crc32.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_rabook_crc_init = 0xFFFFFFFFU, /**< CRC seed and final XOR mask. */
  k_rabook_crc_poly = 0xEDB88320U, /**< Reflected CRC-32 polynomial. */
} ra_rabook_crc_t;

/**
 * @enum ra_rabook_crc_bits_t
 * @brief Per-byte fold count for the bitwise CRC inner loop.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_rabook_crc_byte_bits = 8U, /**< Bit folds per input byte. */
} ra_rabook_crc_bits_t;

/**
 * @brief Compute a CRC-32/ISO-HDLC over a byte range (bitwise, no table).
 * @details Seeds with @ref k_rabook_crc_init, folds eight bits per byte with the
 *          reflected @ref k_rabook_crc_poly, and XORs the seed back out -- the
 *          exact variant @ref ra_book_validate expects (check value 0xCBF43926
 *          over "123456789"), so the emitted CRC verifies bit-for-bit on device.
 * @param[in] data Byte range to checksum (may be NULL iff @p len is 0).
 * @param[in] len  Number of bytes at @p data.
 * @return The CRC-32 of @p data.
 * @retval k_rabook_crc_init The empty range (@p len == 0) reduces to the seed XOR.
 * @pre @p data addresses at least @p len readable bytes (or @p len is 0).
 * @pre @p len is the true range length (no over-read).
 * @post The accumulator is XOR-folded back to a standard CRC-32 result.
 * @post @p data is not modified (read-only checksum).
 * @note Not thread-safe in the sense of shared state, but has none; pure.
 * @since 0.1.0
 */
static uint32_t rabook_crc32(const uint8_t* data, uint32_t len)
{
  uint32_t crc = (uint32_t)k_rabook_crc_init;
  for (uint32_t i = 0U; i < len; i++) {
    crc ^= (uint32_t)data[i];
    for (uint32_t bit = 0U; bit < (uint32_t)k_rabook_crc_byte_bits; bit++) {
      const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc                 = (crc >> 1U) ^ ((uint32_t)k_rabook_crc_poly & mask);
    }
  }
  return crc ^ (uint32_t)k_rabook_crc_init;
}

ra_err_t ra_rabook_compile_init(ra_rabook_ctx_t* ctx, const ra_rabook_buffers_t* buf)
{
  RA_CHECK_NULL_PTR(ctx, s_tag_rabook, "ctx");
  RA_CHECK_NULL_PTR(buf, s_tag_rabook, "buf");
  RA_CHECK_NULL_PTR(buf->chapters, s_tag_rabook, "buf->chapters");
  RA_CHECK_NULL_PTR(buf->nodes, s_tag_rabook, "buf->nodes");
  RA_CHECK_NULL_PTR(buf->attrs, s_tag_rabook, "buf->attrs");
  RA_CHECK_NULL_PTR(buf->stylesheets, s_tag_rabook, "buf->stylesheets");
  RA_CHECK_NULL_PTR(buf->images, s_tag_rabook, "buf->images");
  RA_CHECK_NULL_PTR(buf->string_pool, s_tag_rabook, "buf->string_pool");
  RA_CHECK_NULL_PTR(buf->image_pool, s_tag_rabook, "buf->image_pool");
  RA_CHECK_NULL_PTR(buf->out, s_tag_rabook, "buf->out");

  *ctx                  = (ra_rabook_ctx_t){};
  ctx->buf              = *buf;
  ctx->cover_image_index = (uint32_t)k_ra_book_nil;
  return k_ra_ok;
}

uint32_t ra_rabook_intern(ra_rabook_ctx_t* ctx, const char* str)
{
  if (ctx == nullptr) {
    return (uint32_t)k_ra_book_nil;
  }
  if (str == nullptr) {
    ctx->failed = true;
    return (uint32_t)k_ra_book_nil;
  }
  if (ctx->failed) {
    return (uint32_t)k_ra_book_nil;
  }

  const uint32_t len = (uint32_t)strlen(str);
  /* De-dup: walk the pool string-by-string, return the first exact match. */
  uint32_t scan = 0U;
  while (scan < ctx->string_size) {
    const char*    cand     = &ctx->buf.string_pool[scan];
    const uint32_t cand_len = (uint32_t)strlen(cand);
    if (cand_len == len) {
      if (memcmp(cand, str, (size_t)len) == 0) {
        return scan;
      }
    }
    scan += cand_len + 1U; /* skip the candidate and its NUL */
  }

  /* Miss: append str + NUL, guarding the pool capacity. */
  const uint32_t need = len + 1U;
  if (need > (ctx->buf.string_cap - ctx->string_size)) {
    ctx->failed = true;
    return (uint32_t)k_ra_book_nil;
  }
  const uint32_t off = ctx->string_size;
  (void)memcpy(&ctx->buf.string_pool[off], str, (size_t)len);
  ctx->buf.string_pool[off + len] = '\0';
  ctx->string_size += need;
  return off;
}

uint32_t ra_rabook_add_element(ra_rabook_ctx_t*      ctx,
                               uint32_t              name_off,
                               const ra_book_attr_t* attrs,
                               uint16_t              attr_count)
{
  if (ctx == nullptr) {
    return (uint32_t)k_ra_book_nil;
  }
  if (ctx->failed) {
    return (uint32_t)k_ra_book_nil;
  }
  if (attr_count != 0U) {
    if (attrs == nullptr) {
      ctx->failed = true;
      return (uint32_t)k_ra_book_nil;
    }
  }
  if (ctx->node_count >= ctx->buf.node_cap) {
    ctx->failed = true;
    return (uint32_t)k_ra_book_nil;
  }
  if ((uint32_t)attr_count > (ctx->buf.attr_cap - ctx->attr_count)) {
    ctx->failed = true;
    return (uint32_t)k_ra_book_nil;
  }

  uint32_t first_attr = (uint32_t)k_ra_book_nil;
  if (attr_count != 0U) {
    first_attr = ctx->attr_count;
    for (uint16_t i = 0U; i < attr_count; i++) {
      ctx->buf.attrs[ctx->attr_count] = attrs[i];
      ctx->attr_count++;
    }
  }

  const uint32_t  idx  = ctx->node_count;
  ra_book_node_t* node = &ctx->buf.nodes[idx];
  *node                = (ra_book_node_t){};
  node->kind           = (uint8_t)k_ra_book_node_element;
  node->attr_count     = attr_count;
  node->name_off       = name_off;
  node->text_off       = 0U;
  node->first_attr     = first_attr;
  node->first_child    = (uint32_t)k_ra_book_nil;
  node->next_sibling   = (uint32_t)k_ra_book_nil;
  ctx->node_count++;
  return idx;
}

uint32_t ra_rabook_add_text(ra_rabook_ctx_t* ctx, uint32_t text_off)
{
  if (ctx == nullptr) {
    return (uint32_t)k_ra_book_nil;
  }
  if (ctx->failed) {
    return (uint32_t)k_ra_book_nil;
  }
  if (ctx->node_count >= ctx->buf.node_cap) {
    ctx->failed = true;
    return (uint32_t)k_ra_book_nil;
  }

  const uint32_t  idx  = ctx->node_count;
  ra_book_node_t* node = &ctx->buf.nodes[idx];
  *node                = (ra_book_node_t){};
  node->kind           = (uint8_t)k_ra_book_node_text;
  node->attr_count     = 0U;
  node->name_off       = 0U;
  node->text_off       = text_off;
  node->first_attr     = (uint32_t)k_ra_book_nil;
  node->first_child    = (uint32_t)k_ra_book_nil;
  node->next_sibling   = (uint32_t)k_ra_book_nil;
  ctx->node_count++;
  return idx;
}

ra_err_t ra_rabook_link_child(ra_rabook_ctx_t* ctx, uint32_t parent, uint32_t child)
{
  RA_CHECK_NULL_PTR(ctx, s_tag_rabook, "ctx");
  if (parent >= ctx->node_count) {
    ra_log_error(s_tag_rabook, "link_child parent out of range");
    return k_ra_err_invalid_arg;
  }
  if (child >= ctx->node_count) {
    ra_log_error(s_tag_rabook, "link_child child out of range");
    return k_ra_err_invalid_arg;
  }
  ctx->buf.nodes[parent].first_child = child;
  return k_ra_ok;
}

ra_err_t ra_rabook_link_sibling(ra_rabook_ctx_t* ctx, uint32_t node, uint32_t sibling)
{
  RA_CHECK_NULL_PTR(ctx, s_tag_rabook, "ctx");
  if (node >= ctx->node_count) {
    ra_log_error(s_tag_rabook, "link_sibling node out of range");
    return k_ra_err_invalid_arg;
  }
  if (sibling >= ctx->node_count) {
    ra_log_error(s_tag_rabook, "link_sibling sibling out of range");
    return k_ra_err_invalid_arg;
  }
  ctx->buf.nodes[node].next_sibling = sibling;
  return k_ra_ok;
}

uint32_t ra_rabook_add_chapter(ra_rabook_ctx_t* ctx,
                               uint32_t         title_off,
                               uint32_t         href_off,
                               uint32_t         root_node)
{
  if (ctx == nullptr) {
    return (uint32_t)k_ra_book_nil;
  }
  if (ctx->failed) {
    return (uint32_t)k_ra_book_nil;
  }
  if (ctx->chapter_count >= ctx->buf.chapter_cap) {
    ctx->failed = true;
    return (uint32_t)k_ra_book_nil;
  }

  const uint32_t     idx = ctx->chapter_count;
  ra_book_chapter_t* ch  = &ctx->buf.chapters[idx];
  ch->title_off          = title_off;
  ch->href_off           = href_off;
  ch->root_node          = root_node;
  ctx->chapter_count++;
  return idx;
}

uint32_t ra_rabook_add_image(ra_rabook_ctx_t* ctx,
                             uint32_t         id_off,
                             uint16_t         width,
                             uint16_t         height,
                             uint8_t          format,
                             const uint8_t*   data,
                             uint32_t         data_size)
{
  if (ctx == nullptr) {
    return (uint32_t)k_ra_book_nil;
  }
  if (ctx->failed) {
    return (uint32_t)k_ra_book_nil;
  }
  if (data_size != 0U) {
    if (data == nullptr) {
      ctx->failed = true;
      return (uint32_t)k_ra_book_nil;
    }
  }
  if (ctx->image_count >= ctx->buf.image_cap) {
    ctx->failed = true;
    return (uint32_t)k_ra_book_nil;
  }
  if (data_size > (ctx->buf.image_pool_cap - ctx->image_pool_size)) {
    ctx->failed = true;
    return (uint32_t)k_ra_book_nil;
  }

  const uint32_t data_off = ctx->image_pool_size;
  if (data_size != 0U) {
    (void)memcpy(&ctx->buf.image_pool[data_off], data, (size_t)data_size);
    ctx->image_pool_size += data_size;
  }

  const uint32_t   idx = ctx->image_count;
  ra_book_image_t* img = &ctx->buf.images[idx];
  *img                 = (ra_book_image_t){};
  img->id_off          = id_off;
  img->width           = width;
  img->height          = height;
  img->format          = format;
  img->data_off        = data_off;
  img->data_size       = data_size;
  img->raw_size        = data_size;
  ctx->image_count++;
  return idx;
}

uint32_t ra_rabook_add_stylesheet(ra_rabook_ctx_t* ctx,
                                  uint32_t         source_off,
                                  uint32_t         scope_chapter)
{
  if (ctx == nullptr) {
    return (uint32_t)k_ra_book_nil;
  }
  if (ctx->failed) {
    return (uint32_t)k_ra_book_nil;
  }
  if (ctx->stylesheet_count >= ctx->buf.stylesheet_cap) {
    ctx->failed = true;
    return (uint32_t)k_ra_book_nil;
  }

  const uint32_t        idx = ctx->stylesheet_count;
  ra_book_stylesheet_t* ss  = &ctx->buf.stylesheets[idx];
  ss->source_off            = source_off;
  ss->scope_chapter         = scope_chapter;
  ctx->stylesheet_count++;
  return idx;
}

ra_err_t ra_rabook_set_metadata(ra_rabook_ctx_t* ctx,
                                uint32_t         title_off,
                                uint32_t         author_off,
                                uint32_t         language_off,
                                uint32_t         identifier_off,
                                uint32_t         cover_image_index)
{
  RA_CHECK_NULL_PTR(ctx, s_tag_rabook, "ctx");
  if (ctx->failed) {
    ra_log_error(s_tag_rabook, "set_metadata after a builder overflow");
    return k_ra_err_no_mem;
  }
  ctx->title_off         = title_off;
  ctx->author_off        = author_off;
  ctx->language_off      = language_off;
  ctx->identifier_off    = identifier_off;
  ctx->cover_image_index = cover_image_index;
  return k_ra_ok;
}

ra_err_t ra_rabook_finalize(ra_rabook_ctx_t* ctx, const void** out_blob, uint32_t* out_len)
{
  RA_CHECK_NULL_PTR(ctx, s_tag_rabook, "ctx");
  RA_CHECK_NULL_PTR(out_blob, s_tag_rabook, "out_blob");
  RA_CHECK_NULL_PTR(out_len, s_tag_rabook, "out_len");
  if (ctx->failed) {
    ra_log_error(s_tag_rabook, "finalize after a builder overflow");
    return k_ra_err_no_mem;
  }

  /* Table / pool byte extents (header is the fixed prologue). */
  const uint32_t chap_bytes  = ctx->chapter_count * (uint32_t)k_ra_book_sizeof_chapter;
  const uint32_t node_bytes  = ctx->node_count * (uint32_t)k_ra_book_sizeof_node;
  const uint32_t attr_bytes  = ctx->attr_count * (uint32_t)k_ra_book_sizeof_attr;
  const uint32_t style_bytes = ctx->stylesheet_count * (uint32_t)k_ra_book_sizeof_stylesheet;
  const uint32_t image_bytes = ctx->image_count * (uint32_t)k_ra_book_sizeof_image;

  const uint32_t off_chap   = (uint32_t)k_ra_book_sizeof_header;
  const uint32_t off_node   = off_chap + chap_bytes;
  const uint32_t off_attr   = off_node + node_bytes;
  const uint32_t off_style  = off_attr + attr_bytes;
  const uint32_t off_image  = off_style + style_bytes;
  const uint32_t off_string = off_image + image_bytes;
  const uint32_t off_pool   = off_string + ctx->string_size;
  const uint32_t total      = off_pool + ctx->image_pool_size;

  if (total > ctx->buf.out_cap) {
    ra_log_error(s_tag_rabook, "finalize: blob exceeds output capacity");
    return k_ra_err_invalid_size;
  }

  uint8_t* out = ctx->buf.out;
  (void)memcpy(&out[off_chap], ctx->buf.chapters, (size_t)chap_bytes);
  (void)memcpy(&out[off_node], ctx->buf.nodes, (size_t)node_bytes);
  (void)memcpy(&out[off_attr], ctx->buf.attrs, (size_t)attr_bytes);
  (void)memcpy(&out[off_style], ctx->buf.stylesheets, (size_t)style_bytes);
  (void)memcpy(&out[off_image], ctx->buf.images, (size_t)image_bytes);
  (void)memcpy(&out[off_string], ctx->buf.string_pool, (size_t)ctx->string_size);
  (void)memcpy(&out[off_pool], ctx->buf.image_pool, (size_t)ctx->image_pool_size);

  /* CRC the body (everything after the 100-byte header), then write the header. */
  const uint32_t body_len = total - (uint32_t)k_ra_book_sizeof_header;
  const uint32_t crc      = rabook_crc32(&out[k_ra_book_sizeof_header], body_len);

  ra_book_header_t hdr = {};
  (void)memcpy(hdr.magic, "RABOOK1", sizeof(hdr.magic));
  hdr.format_version    = (uint32_t)k_ra_book_format_version;
  hdr.total_size        = total;
  hdr.flags             = ctx->flags;
  hdr.title_off         = ctx->title_off;
  hdr.author_off        = ctx->author_off;
  hdr.language_off      = ctx->language_off;
  hdr.identifier_off    = ctx->identifier_off;
  hdr.cover_image_index = ctx->cover_image_index;
  hdr.chapter_count     = ctx->chapter_count;
  hdr.chapter_off       = off_chap;
  hdr.node_count        = ctx->node_count;
  hdr.node_off          = off_node;
  hdr.attr_count        = ctx->attr_count;
  hdr.attr_off          = off_attr;
  hdr.stylesheet_count  = ctx->stylesheet_count;
  hdr.stylesheet_off    = off_style;
  hdr.image_count       = ctx->image_count;
  hdr.image_off         = off_image;
  hdr.string_off        = off_string;
  hdr.string_size       = ctx->string_size;
  hdr.image_pool_off    = off_pool;
  hdr.image_pool_size   = ctx->image_pool_size;
  hdr.crc32             = crc;
  (void)memcpy(out, &hdr, sizeof(hdr));

  *out_blob = out;
  *out_len  = total;
  return k_ra_ok;
}
