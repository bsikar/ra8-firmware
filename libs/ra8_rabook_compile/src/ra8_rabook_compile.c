/**
 * @file ra8_rabook_compile.c
 * @brief RABOOK1 builder implementation (see ra8_rabook_compile.h).
 *
 * @details
 * Zero-heap builder back-end of the #149 on-device EPUB -> `.rabook` compiler.
 * Each builder call appends into a caller-provided arena and latches capacity
 * failures in the context. The sibling finalizer module owns canonical layout,
 * CRC-32, and output publication. Keeping construction separate from
 * serialization makes the mutable builder lifecycle independent of its memory
 * or streaming destination.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_rabook_compile.h"

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"

/** @brief Component tag for log messages from this module. */
static const char* const s_tag_rabook = "ra8_rabook_compile";

/**
 * @enum ra8_rabook_buffer_ptr_count_t
 * @brief Number of caller-owned arena pointers validated by init.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_rabook_buffer_ptr_count = 8U, /**< chapters..out member pointers. */
} ra8_rabook_buffer_ptr_count_t;

/** @brief Image-pool backing selected by the first non-empty image append. */
typedef enum : uint8_t {
  k_rabook_pool_none     = 0U, /**< No non-empty image has selected storage. */
  k_rabook_pool_internal = 1U, /**< Bytes live in `buf.image_pool`.          */
  k_rabook_pool_external = 2U, /**< Bytes live behind the read callback.     */
} ra8_rabook_pool_mode_t;

/**
 * @brief Reject a buffers struct with any NULL arena pointer.
 * @details Validates @p buf then each of its @ref k_rabook_buffer_ptr_count arena
 *          pointers in turn, naming the first NULL member in the log line. Keeps
 *          @ref ra8_rabook_compile_init flat (one table-driven check instead of ten
 *          inline guards).
 * @param[in] buf Caller-owned arenas to validate (checked for NULL).
 * @return Error code.
 * @retval k_ra8_ok           @p buf and every member pointer are non-NULL.
 * @retval k_ra8_err_null_ptr @p buf or one of its member pointers is NULL.
 * @pre @p buf, if non-NULL, addresses a fully-constructed buffers struct.
 * @pre The member pointers are stable for the duration of this call.
 * @post No field is modified (read-only validation).
 * @post On k_ra8_ok every arena pointer in @p buf is guaranteed non-NULL.
 * @note Not thread-safe in the sense of shared state, but holds none; pure.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_check_buffer_members(const ra8_rabook_buffers_t* buf)
{
  RA8_CHECK_NULL_PTR(buf, s_tag_rabook, "buf");

  const void* const members[k_rabook_buffer_ptr_count] = {
    buf->chapters,
    buf->nodes,
    buf->attrs,
    buf->stylesheets,
    buf->images,
    buf->string_pool,
    buf->image_pool,
    buf->out,
  };
  static const char* const names[k_rabook_buffer_ptr_count] = {
    "buf->chapters",
    "buf->nodes",
    "buf->attrs",
    "buf->stylesheets",
    "buf->images",
    "buf->string_pool",
    "buf->image_pool",
    "buf->out",
  };

  for (uint8_t i = 0U; i < (uint8_t)k_rabook_buffer_ptr_count; i++) {
    RA8_CHECK_NULL_PTR(members[i], s_tag_rabook, names[i]);
  }
  return k_ra8_ok;
}

ra8_err_t ra8_rabook_compile_init(ra8_rabook_ctx_t* ctx, const ra8_rabook_buffers_t* buf)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag_rabook, "ctx");
  const ra8_err_t buf_err = internal_check_buffer_members(buf);
  if (buf_err != k_ra8_ok) {
    return buf_err;
  }

  *ctx                   = (ra8_rabook_ctx_t){};
  ctx->buf               = *buf;
  ctx->cover_image_index = (uint32_t)k_ra8_book_nil;

  /* Reserve string-pool offset 0 for "" so it is the empty-string sentinel that
   * add_text/add_element store, matching the desktop StringPool.__init__ in
   * tools/epub_compile/epub_compile.py (offset 0 == ""). */
  (void)ra8_rabook_intern(ctx, "");
  return k_ra8_ok;
}

uint32_t ra8_rabook_intern(ra8_rabook_ctx_t* ctx, const char* str)
{
  if (ctx == nullptr) {
    return (uint32_t)k_ra8_book_nil;
  }
  if (str == nullptr) {
    ctx->failed = true;
    return (uint32_t)k_ra8_book_nil;
  }
  if (ctx->failed) {
    return (uint32_t)k_ra8_book_nil;
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
    return (uint32_t)k_ra8_book_nil;
  }
  const uint32_t off = ctx->string_size;
  (void)memcpy(&ctx->buf.string_pool[off], str, (size_t)len);
  ctx->buf.string_pool[off + len] = '\0';
  ctx->string_size += need;
  return off;
}

/**
 * @brief Append @p attr_count attribute records to the attr table contiguously.
 * @details Copies each record and advances `ctx->attr_count`. The caller must have
 *          already checked that @p attr_count attributes fit the remaining attr
 *          arena, so this helper never overflows and never sets `failed`.
 * @param[in,out] ctx        Builder context (non-NULL, capacity pre-checked).
 * @param[in]     attrs      Attribute records to copy (non-NULL iff @p attr_count > 0).
 * @param[in]     attr_count Number of attribute records to append.
 * @return The index of the first appended attribute, or @ref k_ra8_book_nil when
 *         @p attr_count is 0.
 * @retval k_ra8_book_nil @p attr_count is 0 (no attributes appended).
 * @pre @p ctx and (when @p attr_count > 0) @p attrs are non-NULL.
 * @pre `attr_cap - attr_count >= attr_count` for the incoming count.
 * @post On a non-zero count, `ctx->attr_count` grows by @p attr_count.
 * @post On a zero count, the attr table is unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t
internal_append_attrs(ra8_rabook_ctx_t* ctx, const ra8_book_attr_t* attrs, uint16_t attr_count)
{
  if (attr_count == 0U) {
    return (uint32_t)k_ra8_book_nil;
  }

  const uint32_t first_attr = ctx->attr_count;
  for (uint16_t i = 0U; i < attr_count; i++) {
    ctx->buf.attrs[ctx->attr_count] = attrs[i];
    ctx->attr_count++;
  }
  return first_attr;
}

uint32_t ra8_rabook_add_element(ra8_rabook_ctx_t*      ctx,
                                uint32_t               name_off,
                                const ra8_book_attr_t* attrs,
                                uint16_t               attr_count)
{
  if (ctx == nullptr) {
    return (uint32_t)k_ra8_book_nil;
  }
  if (ctx->failed) {
    return (uint32_t)k_ra8_book_nil;
  }
  if (attr_count != 0U) {
    if (attrs == nullptr) {
      ctx->failed = true;
      return (uint32_t)k_ra8_book_nil;
    }
  }
  if (ctx->node_count >= ctx->buf.node_cap) {
    ctx->failed = true;
    return (uint32_t)k_ra8_book_nil;
  }
  if ((uint32_t)attr_count > (ctx->buf.attr_cap - ctx->attr_count)) {
    ctx->failed = true;
    return (uint32_t)k_ra8_book_nil;
  }

  const uint32_t first_attr = internal_append_attrs(ctx, attrs, attr_count);

  const uint32_t   idx  = ctx->node_count;
  ra8_book_node_t* node = &ctx->buf.nodes[idx];
  *node                 = (ra8_book_node_t){};
  node->kind            = (uint8_t)k_ra8_book_node_element;
  node->attr_count      = attr_count;
  node->name_off        = name_off;
  node->text_off        = 0U;
  node->first_attr      = first_attr;
  node->first_child     = (uint32_t)k_ra8_book_nil;
  node->next_sibling    = (uint32_t)k_ra8_book_nil;
  ctx->node_count++;
  return idx;
}

uint32_t ra8_rabook_add_text(ra8_rabook_ctx_t* ctx, uint32_t text_off)
{
  if (ctx == nullptr) {
    return (uint32_t)k_ra8_book_nil;
  }
  if (ctx->failed) {
    return (uint32_t)k_ra8_book_nil;
  }
  if (ctx->node_count >= ctx->buf.node_cap) {
    ctx->failed = true;
    return (uint32_t)k_ra8_book_nil;
  }

  const uint32_t   idx  = ctx->node_count;
  ra8_book_node_t* node = &ctx->buf.nodes[idx];
  *node                 = (ra8_book_node_t){};
  node->kind            = (uint8_t)k_ra8_book_node_text;
  node->attr_count      = 0U;
  node->name_off        = 0U;
  node->text_off        = text_off;
  node->first_attr      = (uint32_t)k_ra8_book_nil;
  node->first_child     = (uint32_t)k_ra8_book_nil;
  node->next_sibling    = (uint32_t)k_ra8_book_nil;
  ctx->node_count++;
  return idx;
}

ra8_err_t ra8_rabook_link_child(ra8_rabook_ctx_t* ctx, uint32_t parent, uint32_t child)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag_rabook, "ctx");
  if (parent >= ctx->node_count) {
    ra8_log_error(s_tag_rabook, "link_child parent out of range");
    return k_ra8_err_invalid_arg;
  }
  if (child >= ctx->node_count) {
    ra8_log_error(s_tag_rabook, "link_child child out of range");
    return k_ra8_err_invalid_arg;
  }
  ctx->buf.nodes[parent].first_child = child;
  return k_ra8_ok;
}

ra8_err_t ra8_rabook_link_sibling(ra8_rabook_ctx_t* ctx, uint32_t node, uint32_t sibling)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag_rabook, "ctx");
  if (node >= ctx->node_count) {
    ra8_log_error(s_tag_rabook, "link_sibling node out of range");
    return k_ra8_err_invalid_arg;
  }
  if (sibling >= ctx->node_count) {
    ra8_log_error(s_tag_rabook, "link_sibling sibling out of range");
    return k_ra8_err_invalid_arg;
  }
  ctx->buf.nodes[node].next_sibling = sibling;
  return k_ra8_ok;
}

uint32_t ra8_rabook_add_chapter(ra8_rabook_ctx_t* ctx,
                                uint32_t          title_off,
                                uint32_t          href_off,
                                uint32_t          root_node)
{
  if (ctx == nullptr) {
    return (uint32_t)k_ra8_book_nil;
  }
  if (ctx->failed) {
    return (uint32_t)k_ra8_book_nil;
  }
  if (ctx->chapter_count >= ctx->buf.chapter_cap) {
    ctx->failed = true;
    return (uint32_t)k_ra8_book_nil;
  }

  const uint32_t      idx = ctx->chapter_count;
  ra8_book_chapter_t* ch  = &ctx->buf.chapters[idx];
  ch->title_off           = title_off;
  ch->href_off            = href_off;
  ch->root_node           = root_node;
  ctx->chapter_count++;
  return idx;
}

/**
 * @brief Append one already-bounded image descriptor.
 * @details Initializes every wire field and advances the descriptor count; pool
 *          ownership and capacity checks remain with the public append APIs.
 * @param[in,out] ctx Builder with descriptor capacity available.
 * @param[in] id_off Image identifier string offset.
 * @param[in] width Image width in pixels.
 * @param[in] height Image height in pixels.
 * @param[in] format Encoded image format.
 * @param[in] pixel_format Decoded pixel format.
 * @param[in] data_off Logical image-pool byte offset.
 * @param[in] data_size Encoded byte count.
 * @return Appended descriptor index.
 * @retval uint32_t Valid image-table index.
 * @pre @p ctx is non-NULL and has image descriptor capacity.
 * @pre @p data_off and @p data_size describe a reserved logical pool span.
 * @post The image count advances by one.
 * @post The descriptor raw size equals its encoded data size.
 * @note Internal helper; performs no pool I/O.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_append_image_descriptor(ra8_rabook_ctx_t* ctx,
                                                 uint32_t          id_off,
                                                 uint16_t          width,
                                                 uint16_t          height,
                                                 uint8_t           format,
                                                 uint8_t           pixel_format,
                                                 uint32_t          data_off,
                                                 uint32_t          data_size)
{
  const uint32_t    idx = ctx->image_count;
  ra8_book_image_t* img = &ctx->buf.images[idx];
  *img                  = (ra8_book_image_t){};
  img->id_off           = id_off;
  img->width            = width;
  img->height           = height;
  img->format           = format;
  img->pixel_format     = pixel_format;
  img->data_off         = data_off;
  img->data_size        = data_size;
  img->raw_size         = data_size;
  ctx->image_count++;
  return idx;
}

uint32_t ra8_rabook_add_image(ra8_rabook_ctx_t* ctx,
                              uint32_t          id_off,
                              uint16_t          width,
                              uint16_t          height,
                              uint8_t           format,
                              uint8_t           pixel_format,
                              const uint8_t*    data,
                              uint32_t          data_size)
{
  if (ctx == nullptr) {
    return (uint32_t)k_ra8_book_nil;
  }
  if (ctx->failed) {
    return (uint32_t)k_ra8_book_nil;
  }
  if (data_size != 0U) {
    if (ctx->image_pool_mode == (uint8_t)k_rabook_pool_external) {
      ctx->failed = true;
      return (uint32_t)k_ra8_book_nil;
    }
  }
  if (data_size != 0U) {
    if (data == nullptr) {
      ctx->failed = true;
      return (uint32_t)k_ra8_book_nil;
    }
  }
  if (ctx->image_count >= ctx->buf.image_cap) {
    ctx->failed = true;
    return (uint32_t)k_ra8_book_nil;
  }
  if (data_size > (ctx->buf.image_pool_cap - ctx->image_pool_size)) {
    ctx->failed = true;
    return (uint32_t)k_ra8_book_nil;
  }

  const uint32_t data_off = ctx->image_pool_size;
  if (data_size != 0U) {
    ctx->image_pool_mode = (uint8_t)k_rabook_pool_internal;
    (void)memcpy(&ctx->buf.image_pool[data_off], data, (size_t)data_size);
    ctx->image_pool_size += data_size;
  }

  return internal_append_image_descriptor(ctx,
                                          id_off,
                                          width,
                                          height,
                                          format,
                                          pixel_format,
                                          data_off,
                                          data_size);
}

uint32_t ra8_rabook_add_image_external(ra8_rabook_ctx_t* ctx,
                                       uint32_t          id_off,
                                       uint16_t          width,
                                       uint16_t          height,
                                       uint8_t           format,
                                       uint8_t           pixel_format,
                                       uint32_t          data_size,
                                       uint32_t*         out_data_off)
{
  if (ctx == nullptr) {
    return (uint32_t)k_ra8_book_nil;
  }
  if (out_data_off == nullptr) {
    ctx->failed = true;
    return (uint32_t)k_ra8_book_nil;
  }
  if (ctx->failed) {
    ctx->failed = true;
    return (uint32_t)k_ra8_book_nil;
  }
  if (data_size != 0U) {
    if (ctx->image_pool_mode == (uint8_t)k_rabook_pool_internal) {
      ctx->failed = true;
      return (uint32_t)k_ra8_book_nil;
    }
  }
  if (ctx->image_count >= ctx->buf.image_cap) {
    ctx->failed = true;
    return (uint32_t)k_ra8_book_nil;
  }
  if (data_size > (UINT32_MAX - ctx->image_pool_size)) {
    ctx->failed = true;
    return (uint32_t)k_ra8_book_nil;
  }

  const uint32_t data_off = ctx->image_pool_size;
  if (data_size != 0U) {
    ctx->image_pool_mode = (uint8_t)k_rabook_pool_external;
    ctx->image_pool_size += data_size;
  }

  *out_data_off = data_off;
  return internal_append_image_descriptor(ctx,
                                          id_off,
                                          width,
                                          height,
                                          format,
                                          pixel_format,
                                          data_off,
                                          data_size);
}

uint32_t
ra8_rabook_add_stylesheet(ra8_rabook_ctx_t* ctx, uint32_t source_off, uint32_t scope_chapter)
{
  if (ctx == nullptr) {
    return (uint32_t)k_ra8_book_nil;
  }
  if (ctx->failed) {
    return (uint32_t)k_ra8_book_nil;
  }
  if (ctx->stylesheet_count >= ctx->buf.stylesheet_cap) {
    ctx->failed = true;
    return (uint32_t)k_ra8_book_nil;
  }

  const uint32_t         idx = ctx->stylesheet_count;
  ra8_book_stylesheet_t* ss  = &ctx->buf.stylesheets[idx];
  ss->source_off             = source_off;
  ss->scope_chapter          = scope_chapter;
  ctx->stylesheet_count++;
  return idx;
}

ra8_err_t ra8_rabook_set_metadata(ra8_rabook_ctx_t* ctx,
                                  uint32_t          title_off,
                                  uint32_t          author_off,
                                  uint32_t          language_off,
                                  uint32_t          identifier_off,
                                  uint32_t          cover_image_index)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag_rabook, "ctx");
  if (ctx->failed) {
    ra8_log_error(s_tag_rabook, "set_metadata after a builder overflow");
    return k_ra8_err_no_mem;
  }
  ctx->title_off         = title_off;
  ctx->author_off        = author_off;
  ctx->language_off      = language_off;
  ctx->identifier_off    = identifier_off;
  ctx->cover_image_index = cover_image_index;
  return k_ra8_ok;
}
