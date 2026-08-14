/**
 * @file ra8_rabook_compile.c
 * @brief RABOOK1 emitter implementation (see ra8_rabook_compile.h).
 *
 * @details
 * Zero-heap builder back-end of the #149 on-device EPUB -> `.rabook` compiler.
 * Each builder call appends into a caller-provided arena; @ref ra8_rabook_finalize
 * lays the tables and pools out at the contract offsets, fills the 100-byte
 * header, and CRC-32s the body so the blob passes @ref ra8_book_validate. The
 * table order, field packing and CRC variant mirror
 * `tools/epub_compile/epub_compile.py` for byte compatibility. Records are the
 * pinned, padding-free structs from ra8_book.h; the target and the host test are
 * both little-endian, so a copy of those structs IS the wire layout.
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

/**
 * @enum ra8_rabook_crc_t
 * @brief CRC-32/ISO-HDLC constants -- match ra8_book_validate() and zlib.crc32.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_rabook_crc_init = 0xFFFFFFFFU, /**< CRC seed and final XOR mask. */
  k_rabook_crc_poly = 0xEDB88320U, /**< Reflected CRC-32 polynomial. */
} ra8_rabook_crc_t;

/**
 * @enum ra8_rabook_crc_bits_t
 * @brief Per-byte fold count for the bitwise CRC inner loop.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_rabook_crc_byte_bits = 8U, /**< Bit folds per input byte. */
} ra8_rabook_crc_bits_t;

/** @brief Image-pool backing selected by the first non-empty image append. */
typedef enum : uint8_t {
  k_rabook_pool_none     = 0U, /**< No non-empty image has selected storage. */
  k_rabook_pool_internal = 1U, /**< Bytes live in `buf.image_pool`.          */
  k_rabook_pool_external = 2U, /**< Bytes live behind the read callback.     */
} ra8_rabook_pool_mode_t;

/**
 * @brief Continue a CRC-32/ISO-HDLC accumulator over one byte range.
 * @details Folds eight bits per byte with the reflected
 *          @ref k_rabook_crc_poly. The caller seeds with
 *          @ref k_rabook_crc_init and applies the final XOR after every segment,
 *          yielding the exact variant @ref ra8_book_validate expects.
 * @param[in] crc  Incoming, not-yet-finalized CRC accumulator.
 * @param[in] data Byte range to checksum (may be NULL iff @p len is 0).
 * @param[in] len  Number of bytes at @p data.
 * @return Updated, not-yet-finalized CRC accumulator.
 * @retval uint32_t Accumulator after all @p len bytes have been folded.
 * @pre @p data addresses at least @p len readable bytes (or @p len is 0).
 * @pre @p len is the true range length (no over-read).
 * @post A zero-length range returns @p crc unchanged.
 * @post @p data is not modified (read-only checksum).
 * @note Not thread-safe in the sense of shared state, but has none; pure.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t s_crc_update(uint32_t crc, const uint8_t* data, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    crc ^= (uint32_t)data[i];
    for (uint32_t bit = 0U; bit < (uint32_t)k_rabook_crc_byte_bits; bit++) {
      const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc                 = (crc >> 1U) ^ ((uint32_t)k_rabook_crc_poly & mask);
    }
  }
  return crc;
}

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
static ra8_err_t s_check_buffer_members(const ra8_rabook_buffers_t* buf)
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
  const ra8_err_t buf_err = s_check_buffer_members(buf);
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
s_append_attrs(ra8_rabook_ctx_t* ctx, const ra8_book_attr_t* attrs, uint16_t attr_count)
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

  const uint32_t first_attr = s_append_attrs(ctx, attrs, attr_count);

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
static uint32_t s_append_image_descriptor(ra8_rabook_ctx_t* ctx,
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
  if ((data_size != 0U) && (ctx->image_pool_mode == (uint8_t)k_rabook_pool_external)) {
    ctx->failed = true;
    return (uint32_t)k_ra8_book_nil;
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

  return s_append_image_descriptor(ctx,
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
  if ((out_data_off == nullptr) || ctx->failed) {
    ctx->failed = true;
    return (uint32_t)k_ra8_book_nil;
  }
  if ((data_size != 0U) && (ctx->image_pool_mode == (uint8_t)k_rabook_pool_internal)) {
    ctx->failed = true;
    return (uint32_t)k_ra8_book_nil;
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
  return s_append_image_descriptor(ctx,
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

/**
 * @struct ra8_rabook_layout_t
 * @brief Computed byte offsets of every table / pool plus the total blob size.
 * @details All offsets are measured from the start of the output buffer; each
 *          table abuts the next so a table's byte length is the delta to the
 *          following offset.
 * @invariant `k_ra8_book_sizeof_header <= off_chap <= off_node <= ... <= total`.
 * @since 0.1.0
 */
typedef struct {
  uint32_t off_chap;   /**< Chapter table offset (== header size). */
  uint32_t off_node;   /**< Node table offset.                     */
  uint32_t off_attr;   /**< Attribute table offset.                */
  uint32_t off_style;  /**< Stylesheet table offset.               */
  uint32_t off_image;  /**< Image table offset.                    */
  uint32_t off_string; /**< String-pool offset.                    */
  uint32_t off_pool;   /**< Image-pool offset.                     */
  uint32_t total;      /**< Total blob size in bytes.              */
} ra8_rabook_layout_t;

/**
 * @brief Compute the table / pool offsets and total blob size, overflow-guarded.
 * @details Sums the fixed header, the five count*record-size tables, and the two
 *          pools in 64-bit arithmetic so no intermediate product or running offset
 *          can wrap a 32-bit value; the final total is rejected if it exceeds
 *          @c UINT32_MAX before being narrowed back into @p lay.
 * @param[in]  ctx Builder context with the running counts / sizes (non-NULL).
 * @param[out] lay Receives the contract offsets and total (non-NULL).
 * @return Error code.
 * @retval k_ra8_ok               Offsets computed and stored in @p lay.
 * @retval k_ra8_err_invalid_size The blob would exceed 32-bit addressing.
 * @pre @p ctx and @p lay are non-NULL (caller-validated).
 * @pre Each `*_count` / `*_size` reflects the bytes actually appended.
 * @post On k_ra8_ok, `lay` offsets are monotonically non-decreasing.
 * @post On error @p lay is left in an indeterminate state and must be ignored.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_compute_layout(const ra8_rabook_ctx_t* ctx, ra8_rabook_layout_t* lay)
{
  const uint64_t chap_bytes = (uint64_t)ctx->chapter_count * (uint64_t)k_ra8_book_sizeof_chapter;
  const uint64_t node_bytes = (uint64_t)ctx->node_count * (uint64_t)k_ra8_book_sizeof_node;
  const uint64_t attr_bytes = (uint64_t)ctx->attr_count * (uint64_t)k_ra8_book_sizeof_attr;
  const uint64_t style_bytes =
    (uint64_t)ctx->stylesheet_count * (uint64_t)k_ra8_book_sizeof_stylesheet;
  const uint64_t image_bytes = (uint64_t)ctx->image_count * (uint64_t)k_ra8_book_sizeof_image;

  const uint64_t off_chap   = (uint64_t)k_ra8_book_sizeof_header;
  const uint64_t off_node   = off_chap + chap_bytes;
  const uint64_t off_attr   = off_node + node_bytes;
  const uint64_t off_style  = off_attr + attr_bytes;
  const uint64_t off_image  = off_style + style_bytes;
  const uint64_t off_string = off_image + image_bytes;
  const uint64_t off_pool   = off_string + (uint64_t)ctx->string_size;
  const uint64_t total      = off_pool + (uint64_t)ctx->image_pool_size;

  if (total > (uint64_t)UINT32_MAX) {
    ra8_log_error(s_tag_rabook, "finalize: layout overflows 32-bit offsets");
    return k_ra8_err_invalid_size;
  }

  lay->off_chap   = (uint32_t)off_chap;
  lay->off_node   = (uint32_t)off_node;
  lay->off_attr   = (uint32_t)off_attr;
  lay->off_style  = (uint32_t)off_style;
  lay->off_image  = (uint32_t)off_image;
  lay->off_string = (uint32_t)off_string;
  lay->off_pool   = (uint32_t)off_pool;
  lay->total      = (uint32_t)total;
  return k_ra8_ok;
}

/** @brief One resident body segment in canonical serialization order. */
typedef struct {
  const uint8_t* data; /**< First readable byte. */
  uint32_t       len;  /**< Segment byte count.  */
} ra8_rabook_segment_t;

/** @brief Number of resident body segments before the image-pool payload. */
typedef enum : uint8_t {
  k_rabook_segment_chapters = 0U, /**< Chapter table.                 */
  k_rabook_segment_nodes    = 1U, /**< DOM node table.                */
  k_rabook_segment_attrs    = 2U, /**< DOM attribute table.           */
  k_rabook_segment_styles   = 3U, /**< Stylesheet table.              */
  k_rabook_segment_images   = 4U, /**< Image descriptor table.        */
  k_rabook_segment_strings  = 5U, /**< Interned string pool.          */
  k_rabook_segment_count    = 6U, /**< Five tables plus the string pool. */
} ra8_rabook_segment_count_t;

/**
 * @brief Populate the canonical resident body-segment sequence.
 * @details Forms views over the five fixed tables and string pool; the external
 *          or internal image payload remains a separate final segment.
 * @param[in]  ctx Builder holding resident table and string storage.
 * @param[in]  lay Computed canonical section offsets.
 * @param[out] seg Six writable segment descriptors.
 * @pre All arguments are non-NULL.
 * @pre @p lay was computed from @p ctx.
 * @post Every segment points into a caller-owned builder arena.
 * @post Segment lengths exactly cover the body through the string pool.
 * @note The returned views remain valid while the builder arenas remain alive.
 * @since 0.1.0
 */
RA8_INTERNAL
static void s_segments(const ra8_rabook_ctx_t*    ctx,
                       const ra8_rabook_layout_t* lay,
                       ra8_rabook_segment_t       seg[k_rabook_segment_count])
{
  seg[k_rabook_segment_chapters] =
    (ra8_rabook_segment_t){(const uint8_t*)ctx->buf.chapters, lay->off_node - lay->off_chap};
  seg[k_rabook_segment_nodes] =
    (ra8_rabook_segment_t){(const uint8_t*)ctx->buf.nodes, lay->off_attr - lay->off_node};
  seg[k_rabook_segment_attrs] =
    (ra8_rabook_segment_t){(const uint8_t*)ctx->buf.attrs, lay->off_style - lay->off_attr};
  seg[k_rabook_segment_styles] =
    (ra8_rabook_segment_t){(const uint8_t*)ctx->buf.stylesheets, lay->off_image - lay->off_style};
  seg[k_rabook_segment_images] =
    (ra8_rabook_segment_t){(const uint8_t*)ctx->buf.images, lay->off_string - lay->off_image};
  seg[k_rabook_segment_strings] =
    (ra8_rabook_segment_t){(const uint8_t*)ctx->buf.string_pool, ctx->string_size};
}

/**
 * @brief Read one exact external-pool chunk, rejecting a short success.
 * @details Calls the supplied reader once and converts a successful short count
 *          into @ref k_ra8_err_invalid_size.
 * @param[in]  read Reader callback.
 * @param[in,out] read_ctx Reader context.
 * @param[in]  offset Logical pool offset.
 * @param[out] dst Destination buffer.
 * @param[in]  len Exact requested byte count.
 * @return Exact-transfer status.
 * @retval k_ra8_ok Exactly @p len bytes were read.
 * @retval k_ra8_err_invalid_size The callback reported a short success.
 * @pre Callback and destination pointers are non-NULL.
 * @pre @p dst spans @p len writable bytes.
 * @post Success initializes every requested destination byte.
 * @post A callback error is propagated unchanged.
 * @note Does not retry partial reads.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_read_exact(ra8_rabook_image_read_fn read,
                              void*                    read_ctx,
                              uint32_t                 offset,
                              uint8_t*                 dst,
                              uint32_t                 len)
{
  uint32_t        got = 0U;
  const ra8_err_t err = read(read_ctx, offset, dst, len, &got);
  if (err != k_ra8_ok) {
    return err;
  }
  return (got == len) ? k_ra8_ok : k_ra8_err_invalid_size;
}

/**
 * @brief Append one exact stream chunk, rejecting a short success.
 * @details Zero-length segments are skipped; non-empty segments are submitted
 *          once and must be accepted completely.
 * @param[in] write Writer callback.
 * @param[in,out] write_ctx Writer context.
 * @param[in] src Source bytes.
 * @param[in] len Exact requested byte count.
 * @return Exact-transfer status.
 * @retval k_ra8_ok The complete segment was appended, or @p len was zero.
 * @retval k_ra8_err_invalid_size The callback reported a short success.
 * @pre @p write is non-NULL.
 * @pre @p src spans @p len readable bytes when @p len is nonzero.
 * @post Success advances the logical destination by @p len bytes.
 * @post A callback error is propagated unchanged.
 * @note Does not retry partial writes.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
s_write_exact(ra8_rabook_write_fn write, void* write_ctx, const uint8_t* src, uint32_t len)
{
  if (len == 0U) {
    return k_ra8_ok;
  }
  uint32_t        wrote = 0U;
  const ra8_err_t err   = write(write_ctx, src, len, &wrote);
  if (err != k_ra8_ok) {
    return err;
  }
  return (wrote == len) ? k_ra8_ok : k_ra8_err_invalid_size;
}

/**
 * @brief Fold the logical image pool into an in-progress CRC.
 * @details Reads internal bytes directly or walks an external pool through
 *          bounded caller scratch, preserving one CRC accumulator across chunks.
 * @param[in] ctx Builder describing the logical image pool.
 * @param[in] read External reader, when selected.
 * @param[in,out] read_ctx External reader context.
 * @param[in,out] scratch External transfer scratch.
 * @param[in] scratch_cap Scratch capacity in bytes.
 * @param[in,out] crc Running CRC accumulator.
 * @return Pool-read status.
 * @retval k_ra8_ok Every pool byte was folded exactly once.
 * @retval k_ra8_err_invalid_size An external read was short.
 * @pre @p ctx and @p crc are non-NULL.
 * @pre External mode supplies a non-NULL reader and nonzero scratch capacity.
 * @post Success updates @p crc through the whole logical pool.
 * @post Failure leaves the partially updated accumulator unpublished.
 * @note External reads are bounded by @p scratch_cap.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_crc_image_pool(const ra8_rabook_ctx_t*  ctx,
                                  ra8_rabook_image_read_fn read,
                                  void*                    read_ctx,
                                  uint8_t*                 scratch,
                                  uint32_t                 scratch_cap,
                                  uint32_t*                crc)
{
  if (ctx->image_pool_size == 0U) {
    return k_ra8_ok;
  }
  if (ctx->image_pool_mode != (uint8_t)k_rabook_pool_external) {
    *crc = s_crc_update(*crc, ctx->buf.image_pool, ctx->image_pool_size);
    return k_ra8_ok;
  }
  uint32_t offset = 0U;
  while (offset < ctx->image_pool_size) {
    uint32_t span = ctx->image_pool_size - offset;
    if (span > scratch_cap) {
      span = scratch_cap;
    }
    const ra8_err_t err = s_read_exact(read, read_ctx, offset, scratch, span);
    if (err != k_ra8_ok) {
      return err;
    }
    *crc = s_crc_update(*crc, scratch, span);
    offset += span;
  }
  return k_ra8_ok;
}

/**
 * @brief Fill the fixed RABOOK1 header from one layout and CRC.
 * @details Copies builder counts, metadata, flags, pool sizes, canonical offsets,
 *          magic, version, and finalized body CRC into the pinned wire struct.
 * @param[in] ctx Builder state to serialize.
 * @param[in] lay Canonical layout computed from @p ctx.
 * @param[in] crc Finalized body CRC-32.
 * @return Complete RABOOK1 header value.
 * @retval ra8_book_header_t Header ready for byte emission.
 * @pre @p ctx and @p lay are non-NULL.
 * @pre @p lay was computed successfully from @p ctx.
 * @post Every header count and offset matches the builder and layout.
 * @post The returned header owns no pointers into builder storage.
 * @note Pure construction over caller-owned state.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_book_header_t
s_make_header(const ra8_rabook_ctx_t* ctx, const ra8_rabook_layout_t* lay, uint32_t crc)
{
  ra8_book_header_t hdr = {};
  (void)memcpy(hdr.magic, "RABOOK1", sizeof(hdr.magic));
  hdr.format_version    = (uint32_t)k_ra8_book_format_version;
  hdr.total_size        = lay->total;
  hdr.flags             = ctx->flags;
  hdr.title_off         = ctx->title_off;
  hdr.author_off        = ctx->author_off;
  hdr.language_off      = ctx->language_off;
  hdr.identifier_off    = ctx->identifier_off;
  hdr.cover_image_index = ctx->cover_image_index;
  hdr.chapter_count     = ctx->chapter_count;
  hdr.chapter_off       = lay->off_chap;
  hdr.node_count        = ctx->node_count;
  hdr.node_off          = lay->off_node;
  hdr.attr_count        = ctx->attr_count;
  hdr.attr_off          = lay->off_attr;
  hdr.stylesheet_count  = ctx->stylesheet_count;
  hdr.stylesheet_off    = lay->off_style;
  hdr.image_count       = ctx->image_count;
  hdr.image_off         = lay->off_image;
  hdr.string_off        = lay->off_string;
  hdr.string_size       = ctx->string_size;
  hdr.image_pool_off    = lay->off_pool;
  hdr.image_pool_size   = ctx->image_pool_size;
  hdr.crc32             = crc;
  return hdr;
}

/**
 * @brief Emit the logical image pool through the exact-write callback.
 * @details Writes an internal pool directly or rereads an external pool in
 *          caller-scratch-sized chunks and appends each chunk exactly once.
 * @param[in] ctx Builder describing the logical pool.
 * @param[in] read External reader, when selected.
 * @param[in,out] read_ctx External reader context.
 * @param[in] write Destination writer.
 * @param[in,out] write_ctx Destination writer context.
 * @param[in,out] scratch External transfer scratch.
 * @param[in] scratch_cap Scratch capacity in bytes.
 * @return Pool emission status.
 * @retval k_ra8_ok The entire pool was appended.
 * @retval k_ra8_err_invalid_size A read or write callback was short.
 * @pre @p ctx and @p write are non-NULL.
 * @pre External mode supplies a reader and nonzero scratch capacity.
 * @post Success appends exactly `ctx->image_pool_size` bytes.
 * @post Callback failures are propagated unchanged.
 * @note External reads never exceed @p scratch_cap.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_write_image_pool(const ra8_rabook_ctx_t*  ctx,
                                    ra8_rabook_image_read_fn read,
                                    void*                    read_ctx,
                                    ra8_rabook_write_fn      write,
                                    void*                    write_ctx,
                                    uint8_t*                 scratch,
                                    uint32_t                 scratch_cap)
{
  if (ctx->image_pool_size == 0U) {
    return k_ra8_ok;
  }
  if (ctx->image_pool_mode != (uint8_t)k_rabook_pool_external) {
    return s_write_exact(write, write_ctx, ctx->buf.image_pool, ctx->image_pool_size);
  }
  uint32_t offset = 0U;
  while (offset < ctx->image_pool_size) {
    uint32_t span = ctx->image_pool_size - offset;
    if (span > scratch_cap) {
      span = scratch_cap;
    }
    ra8_err_t err = s_read_exact(read, read_ctx, offset, scratch, span);
    if (err == k_ra8_ok) {
      err = s_write_exact(write, write_ctx, scratch, span);
    }
    if (err != k_ra8_ok) {
      return err;
    }
    offset += span;
  }
  return k_ra8_ok;
}

/**
 * @brief Compute the finalized CRC for all canonical body segments.
 * @details Folds resident segments in layout order, then reads and folds the
 *          logical image pool before applying the CRC final XOR.
 * @param[in] ctx Builder describing the logical body.
 * @param[in] seg Canonical resident segment sequence.
 * @param[in] read External image-pool reader, when selected.
 * @param[in,out] read_ctx External reader context.
 * @param[in,out] scratch External transfer scratch.
 * @param[in] scratch_cap Scratch capacity in bytes.
 * @param[out] out_crc Finalized body CRC.
 * @return CRC pass status.
 * @retval k_ra8_ok Every logical body byte was folded.
 * @retval k_ra8_err_invalid_size An external read was short.
 * @pre @p ctx, @p seg, and @p out_crc are non-NULL.
 * @pre External mode supplies a reader and nonzero scratch capacity.
 * @post Success initializes @p out_crc with the RABOOK1 body CRC.
 * @post Callback failures are propagated unchanged.
 * @note External reads never exceed @p scratch_cap.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_crc_body(const ra8_rabook_ctx_t*    ctx,
                            const ra8_rabook_segment_t seg[k_rabook_segment_count],
                            ra8_rabook_image_read_fn   read,
                            void*                      read_ctx,
                            uint8_t*                   scratch,
                            uint32_t                   scratch_cap,
                            uint32_t*                  out_crc)
{
  uint32_t crc = (uint32_t)k_rabook_crc_init;
  for (uint8_t i = 0U; i < (uint8_t)k_rabook_segment_count; ++i) {
    crc = s_crc_update(crc, seg[i].data, seg[i].len);
  }
  const ra8_err_t err = s_crc_image_pool(ctx, read, read_ctx, scratch, scratch_cap, &crc);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_crc = crc ^ (uint32_t)k_rabook_crc_init;
  return k_ra8_ok;
}

/**
 * @brief Emit the fixed header followed by the canonical logical body.
 * @details Appends each resident segment exactly, then emits the internal or
 *          externally reread image pool.
 * @param[in] ctx Builder describing the logical image pool.
 * @param[in] hdr Finalized RABOOK1 header.
 * @param[in] seg Canonical resident segment sequence.
 * @param[in] read External image-pool reader, when selected.
 * @param[in,out] read_ctx External reader context.
 * @param[in] write Destination writer.
 * @param[in,out] write_ctx Destination writer context.
 * @param[in,out] scratch External transfer scratch.
 * @param[in] scratch_cap Scratch capacity in bytes.
 * @return Emission status.
 * @retval k_ra8_ok Header and complete body were appended.
 * @retval k_ra8_err_invalid_size A callback reported a short success.
 * @pre @p ctx, @p hdr, @p seg, and @p write are non-NULL.
 * @pre @p hdr describes the supplied segment sequence and image pool.
 * @post Success appends exactly the header total-size byte count.
 * @post Callback failures are propagated unchanged.
 * @note External image bytes are reread after the CRC pass.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_write_book(const ra8_rabook_ctx_t*    ctx,
                              const ra8_book_header_t*   hdr,
                              const ra8_rabook_segment_t seg[k_rabook_segment_count],
                              ra8_rabook_image_read_fn   read,
                              void*                      read_ctx,
                              ra8_rabook_write_fn        write,
                              void*                      write_ctx,
                              uint8_t*                   scratch,
                              uint32_t                   scratch_cap)
{
  ra8_err_t err = s_write_exact(write, write_ctx, (const uint8_t*)hdr, (uint32_t)sizeof(*hdr));
  for (uint8_t i = 0U; (i < (uint8_t)k_rabook_segment_count) && (err == k_ra8_ok); ++i) {
    err = s_write_exact(write, write_ctx, seg[i].data, seg[i].len);
  }
  if (err != k_ra8_ok) {
    return err;
  }
  return s_write_image_pool(ctx, read, read_ctx, write, write_ctx, scratch, scratch_cap);
}

/**
 * @brief Validate streaming-finalizer arguments and builder state.
 * @details Enforces common pointer requirements, sticky builder failure, and
 *          the reader/scratch contract selected by a non-empty external pool.
 * @param[in] ctx Builder to finalize.
 * @param[in] image_read External image-pool reader, when selected.
 * @param[in] write Destination writer.
 * @param[in] scratch External transfer scratch pointer.
 * @param[in] scratch_cap Scratch capacity in bytes.
 * @param[out] out_len Final serialized length destination.
 * @return Validation status.
 * @retval k_ra8_ok Arguments satisfy the selected pool mode.
 * @retval k_ra8_err_null_ptr A required pointer is NULL.
 * @retval k_ra8_err_invalid_size External mode has zero scratch capacity.
 * @retval k_ra8_err_no_mem Builder failure was already latched.
 * @pre Pointer values may be NULL for validation.
 * @pre @p ctx, when non-NULL, addresses an initialized builder.
 * @post Success permits layout, CRC, and emission passes.
 * @post No builder or output state is modified.
 * @note Internal-pool mode ignores @p image_read and scratch arguments.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_validate_stream_args(const ra8_rabook_ctx_t*  ctx,
                                        ra8_rabook_image_read_fn image_read,
                                        ra8_rabook_write_fn      write,
                                        const uint8_t*           scratch,
                                        uint32_t                 scratch_cap,
                                        const uint32_t*          out_len)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag_rabook, "ctx");
  RA8_CHECK_NULL_PTR((const void*)write, s_tag_rabook, "write");
  RA8_CHECK_NULL_PTR(out_len, s_tag_rabook, "out_len");
  if (ctx->failed) {
    return k_ra8_err_no_mem;
  }
  const bool external =
    (ctx->image_pool_size != 0U) && (ctx->image_pool_mode == (uint8_t)k_rabook_pool_external);
  if (external && ((image_read == nullptr) || (scratch == nullptr))) {
    return k_ra8_err_null_ptr;
  }
  if (external && (scratch_cap == 0U)) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_rabook_finalize_stream(const ra8_rabook_ctx_t*  ctx,
                                     ra8_rabook_image_read_fn image_read,
                                     void*                    image_ctx,
                                     ra8_rabook_write_fn      write,
                                     void*                    write_ctx,
                                     uint8_t*                 scratch,
                                     uint32_t                 scratch_cap,
                                     uint32_t*                out_len)
{
  const ra8_err_t arg_err =
    s_validate_stream_args(ctx, image_read, write, scratch, scratch_cap, out_len);
  if (arg_err != k_ra8_ok) {
    return arg_err;
  }

  ra8_rabook_layout_t lay     = {};
  const ra8_err_t     lay_err = s_compute_layout(ctx, &lay);
  if (lay_err != k_ra8_ok) {
    return lay_err;
  }
  ra8_rabook_segment_t seg[k_rabook_segment_count] = {};
  s_segments(ctx, &lay, seg);
  uint32_t  crc = 0U;
  ra8_err_t err = s_crc_body(ctx, seg, image_read, image_ctx, scratch, scratch_cap, &crc);
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_book_header_t hdr = s_make_header(ctx, &lay, crc);
  err = s_write_book(ctx, &hdr, seg, image_read, image_ctx, write, write_ctx, scratch, scratch_cap);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_len = lay.total;
  return k_ra8_ok;
}

/** @brief Bounded in-memory sink used by the legacy finalizer wrapper. */
typedef struct {
  uint8_t* data; /**< Output arena base.  */
  uint32_t cap;  /**< Output arena bytes. */
  uint32_t used; /**< Bytes appended.     */
} ra8_rabook_memory_sink_t;

/**
 * @brief Append one chunk to a bounded memory sink.
 * @details Checks remaining capacity before copying and reports zero written on
 *          exhaustion, preserving the exact-write callback contract.
 * @param[in,out] opaque Memory-sink context.
 * @param[in] src Source bytes.
 * @param[in] requested Requested append length.
 * @param[out] out_written Actual appended length.
 * @return Sink status.
 * @retval k_ra8_ok The full request was copied.
 * @retval k_ra8_err_invalid_size The destination lacks capacity.
 * @pre Context, source, and count pointers are non-NULL.
 * @pre The sink's used count does not exceed its capacity.
 * @post Success advances used and reports @p requested.
 * @post Exhaustion reports zero and leaves the sink unchanged.
 * @note Used only by the legacy in-memory finalizer wrapper.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
s_memory_write(void* opaque, const uint8_t* src, uint32_t requested, uint32_t* out_written)
{
  ra8_rabook_memory_sink_t* sink = (ra8_rabook_memory_sink_t*)opaque;
  *out_written                   = 0U;
  if (requested > (sink->cap - sink->used)) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(&sink->data[sink->used], src, (size_t)requested);
  sink->used += requested;
  *out_written = requested;
  return k_ra8_ok;
}

/**
 * @brief Validate the legacy in-memory finalizer and output capacity.
 * @details Checks result pointers and sticky builder failure, then computes the
 *          canonical layout without publishing bytes to prove the sink fits.
 * @param[in] ctx Builder to finalize.
 * @param[out] out_blob Final output pointer destination.
 * @param[out] out_len Final serialized length destination.
 * @return Validation status.
 * @retval k_ra8_ok Builder layout fits the configured output arena.
 * @retval k_ra8_err_null_ptr A required pointer is NULL.
 * @retval k_ra8_err_no_mem Builder failure was already latched.
 * @retval k_ra8_err_invalid_size Layout overflows or exceeds output capacity.
 * @pre Pointer values may be NULL for validation.
 * @pre @p ctx, when non-NULL, addresses an initialized builder.
 * @post Success permits the streaming wrapper to use the configured sink.
 * @post No builder, output bytes, or result pointers are modified.
 * @note The streaming finalizer recomputes the same layout before emission.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
s_validate_memory_finalize(ra8_rabook_ctx_t* ctx, const void** out_blob, const uint32_t* out_len)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag_rabook, "ctx");
  RA8_CHECK_NULL_PTR(out_blob, s_tag_rabook, "out_blob");
  RA8_CHECK_NULL_PTR(out_len, s_tag_rabook, "out_len");
  if (ctx->failed) {
    ra8_log_error(s_tag_rabook, "finalize after a builder overflow");
    return k_ra8_err_no_mem;
  }
  ra8_rabook_layout_t lay = {};
  RA8_RETURN_ON_ERROR(s_compute_layout(ctx, &lay), s_tag_rabook, "layout");
  return (lay.total <= ctx->buf.out_cap) ? k_ra8_ok : k_ra8_err_invalid_size;
}

ra8_err_t ra8_rabook_finalize(ra8_rabook_ctx_t* ctx, const void** out_blob, uint32_t* out_len)
{
  const ra8_err_t arg_err = s_validate_memory_finalize(ctx, out_blob, out_len);
  if (arg_err != k_ra8_ok) {
    return arg_err;
  }
  ra8_rabook_memory_sink_t sink = {.data = ctx->buf.out, .cap = ctx->buf.out_cap};
  uint32_t                 len  = 0U;
  const ra8_err_t          err =
    ra8_rabook_finalize_stream(ctx, nullptr, nullptr, s_memory_write, &sink, nullptr, 0U, &len);
  if (err != k_ra8_ok) {
    return err;
  }

  *out_blob = ctx->buf.out;
  *out_len  = len;
  return k_ra8_ok;
}
