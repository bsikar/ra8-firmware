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

/**
 * @brief Compute a CRC-32/ISO-HDLC over a byte range (bitwise, no table).
 * @details Seeds with @ref k_rabook_crc_init, folds eight bits per byte with the
 *          reflected @ref k_rabook_crc_poly, and XORs the seed back out -- the
 *          exact variant @ref ra8_book_validate expects (check value 0xCBF43926
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
RA8_INTERNAL
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
    (void)memcpy(&ctx->buf.image_pool[data_off], data, (size_t)data_size);
    ctx->image_pool_size += data_size;
  }

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

/**
 * @brief Serialise the tables, pools and CRC-checked header into @p out.
 * @details Copies each table / pool to its contract offset (a table's length is
 *          the delta to the next offset), CRC-32s every byte after the 100-byte
 *          header, then fills and writes the @ref ra8_book_header_t prologue so the
 *          blob passes @ref ra8_book_validate.
 * @param[out] out Output buffer holding at least @p lay->total bytes (non-NULL).
 * @param[in]  ctx Builder context with the source tables / pools (non-NULL).
 * @param[in]  lay Pre-computed layout offsets / total (non-NULL).
 * @pre @p out, @p ctx and @p lay are non-NULL (caller-validated).
 * @pre @p out has room for @p lay->total bytes and @p lay came from @p ctx.
 * @post `out[0..lay->total)` holds a complete RABOOK1 blob with a valid CRC.
 * @post Only @p out is written; @p ctx and @p lay are unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void s_write_blob(uint8_t* out, const ra8_rabook_ctx_t* ctx, const ra8_rabook_layout_t* lay)
{
  (void)memcpy(&out[lay->off_chap], ctx->buf.chapters, (size_t)(lay->off_node - lay->off_chap));
  (void)memcpy(&out[lay->off_node], ctx->buf.nodes, (size_t)(lay->off_attr - lay->off_node));
  (void)memcpy(&out[lay->off_attr], ctx->buf.attrs, (size_t)(lay->off_style - lay->off_attr));
  (void)memcpy(&out[lay->off_style],
               ctx->buf.stylesheets,
               (size_t)(lay->off_image - lay->off_style));
  (void)memcpy(&out[lay->off_image], ctx->buf.images, (size_t)(lay->off_string - lay->off_image));
  (void)memcpy(&out[lay->off_string], ctx->buf.string_pool, (size_t)ctx->string_size);
  (void)memcpy(&out[lay->off_pool], ctx->buf.image_pool, (size_t)ctx->image_pool_size);

  const uint32_t body_len = lay->total - (uint32_t)k_ra8_book_sizeof_header;
  const uint32_t crc      = rabook_crc32(&out[k_ra8_book_sizeof_header], body_len);

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
  (void)memcpy(out, &hdr, sizeof(hdr));
}

ra8_err_t ra8_rabook_finalize(ra8_rabook_ctx_t* ctx, const void** out_blob, uint32_t* out_len)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag_rabook, "ctx");
  RA8_CHECK_NULL_PTR(out_blob, s_tag_rabook, "out_blob");
  RA8_CHECK_NULL_PTR(out_len, s_tag_rabook, "out_len");
  if (ctx->failed) {
    ra8_log_error(s_tag_rabook, "finalize after a builder overflow");
    return k_ra8_err_no_mem;
  }

  ra8_rabook_layout_t lay     = {};
  const ra8_err_t     lay_err = s_compute_layout(ctx, &lay);
  if (lay_err != k_ra8_ok) {
    return lay_err;
  }
  if (lay.total > ctx->buf.out_cap) {
    ra8_log_error(s_tag_rabook, "finalize: blob exceeds output capacity");
    return k_ra8_err_invalid_size;
  }

  s_write_blob(ctx->buf.out, ctx, &lay);

  *out_blob = ctx->buf.out;
  *out_len  = lay.total;
  return k_ra8_ok;
}
