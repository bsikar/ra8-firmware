/**
 * @file ra8_reflow_cache.c
 * @brief Import-time pagination cache serialiser / loader for `ra8_reflow`.
 *
 * @details
 * Implements `ra8_reflow_cache.h`: turn a laid-out engine's `glyphs[]` +
 * `pages[]` into a keyed little-endian byte blob and back, so the
 * expensive `ra8_reflow_layout_chapter()` pass can be skipped when the
 * viewport / font / colours / font blob / chapter bytes are unchanged.
 *
 * The serialisation is explicit per field (no `memcpy` of whole structs),
 * which keeps the format compiler-independent and -- crucially -- keeps
 * the body checksum deterministic because struct padding bytes are never
 * hashed. Every loop is bounded by a count that the loader first checks
 * against the engine's static pools (`k_ra8_reflow_max_glyphs` /
 * `k_ra8_reflow_max_pages`) and the blob length, so a malformed blob can
 * never overrun (NASA P10 Rules 2 and 7).
 *
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_reflow_cache.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_reflow.h"

/* ===========================================================================
 * Internal numeric constants (no magic numbers).
 * ===========================================================================
 */

/**
 * @enum priv_cache_const_t
 * @brief FNV-1a parameters and byte-extraction shifts / mask.
 */
typedef enum : uint32_t {
  k_priv_fnv_offset = 0x811C9DC5U, /**< FNV-1a 32-bit offset basis. */
  k_priv_fnv_prime  = 0x01000193U, /**< FNV-1a 32-bit prime.        */
  k_priv_byte_mask  = 0xFFU,       /**< Low-byte mask.              */
  k_priv_shift_8    = 8U,          /**< Shift to byte 1.            */
  k_priv_shift_16   = 16U,         /**< Shift to byte 2.            */
  k_priv_shift_24   = 24U,         /**< Shift to byte 3.            */
} priv_cache_const_t;

/**
 * @enum priv_cache_off_t
 * @brief Fixed byte offsets within the serialised header.
 */
typedef enum : size_t {
  k_priv_checksum_off = 48U, /**< Body-checksum field offset. */
} priv_cache_off_t;

/**
 * @struct priv_cache_key_t
 * @brief Parsed header / invalidation key of a cache blob.
 */
typedef struct {
  uint32_t magic;         /**< Format magic.                 */
  uint32_t version;       /**< Format version.               */
  uint16_t viewport_w;    /**< Viewport width, pixels.       */
  uint16_t viewport_h;    /**< Viewport height, pixels.      */
  uint16_t font_px;       /**< Body font size, pixels.       */
  uint32_t body_color;    /**< Body text colour.             */
  uint32_t link_color;    /**< Anchor text colour.           */
  uint32_t font_len;      /**< Font blob length.             */
  uint32_t font_hash;     /**< FNV-1a of the font blob.      */
  uint32_t content_len;   /**< Chapter byte length.          */
  uint32_t content_hash;  /**< FNV-1a of the chapter bytes.  */
  uint32_t glyph_count;   /**< Serialised glyph count.       */
  uint32_t page_count;    /**< Serialised page count.        */
  uint32_t body_checksum; /**< FNV-1a of the record payload. */
} priv_cache_key_t;

/* ===========================================================================
 * Little-endian byte cursors
 * ===========================================================================
 */

/**
 * @brief Append a little-endian `uint32_t` and advance the cursor.
 *
 * @details Writes the four bytes least-significant-first at `*off` and
 *          advances `*off` by 4.
 *
 * @param[out]    buf  Destination buffer (>= 4 bytes free at `*off`).
 * @param[in,out] off  Write cursor (advanced by 4).
 * @param[in]     word Value to store.
 *
 * @pre  `buf` and `off` are non-NULL.
 * @pre  `*off + 4` does not exceed the buffer capacity.
 * @post `*off` has advanced by 4.
 * @post Four little-endian bytes of `word` are written.
 * @note Internal byte cursor; no bounds check (caller pre-sizes).
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_put_u32(uint8_t* buf, size_t* off, uint32_t word)
{
  buf[(*off)++] = (uint8_t)(word & (uint32_t)k_priv_byte_mask);
  buf[(*off)++] = (uint8_t)((word >> (uint32_t)k_priv_shift_8) & (uint32_t)k_priv_byte_mask);
  buf[(*off)++] = (uint8_t)((word >> (uint32_t)k_priv_shift_16) & (uint32_t)k_priv_byte_mask);
  buf[(*off)++] = (uint8_t)((word >> (uint32_t)k_priv_shift_24) & (uint32_t)k_priv_byte_mask);
}

/**
 * @brief Append a little-endian `uint16_t` and advance the cursor.
 *
 * @details Writes the two bytes least-significant-first at `*off` and
 *          advances `*off` by 2.
 *
 * @param[out]    buf  Destination buffer (>= 2 bytes free at `*off`).
 * @param[in,out] off  Write cursor (advanced by 2).
 * @param[in]     half Value to store.
 *
 * @pre  `buf` and `off` are non-NULL.
 * @pre  `*off + 2` does not exceed the buffer capacity.
 * @post `*off` has advanced by 2.
 * @post Two little-endian bytes of `half` are written.
 * @note Internal byte cursor; no bounds check (caller pre-sizes).
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_put_u16(uint8_t* buf, size_t* off, uint16_t half)
{
  buf[(*off)++] = (uint8_t)(half & (uint16_t)k_priv_byte_mask);
  buf[(*off)++] = (uint8_t)((half >> (uint32_t)k_priv_shift_8) & (uint16_t)k_priv_byte_mask);
}

/**
 * @brief Read a little-endian `uint32_t` and advance the cursor.
 *
 * @details Reads four bytes least-significant-first from `*off` and
 *          advances `*off` by 4.
 *
 * @param[in]     buf Source buffer (>= 4 bytes readable at `*off`).
 * @param[in,out] off Read cursor (advanced by 4).
 *
 * @return The decoded 32-bit value.
 * @retval value The little-endian word assembled from four bytes.
 *
 * @pre  `buf` and `off` are non-NULL.
 * @pre  `*off + 4` does not exceed the buffer length.
 * @post `*off` has advanced by 4.
 * @post The buffer is not modified.
 * @note Internal byte cursor; no bounds check (caller pre-validates).
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_get_u32(const uint8_t* buf, size_t* off)
{
  uint32_t word = (uint32_t)buf[(*off)++];
  word |= (uint32_t)buf[(*off)++] << (uint32_t)k_priv_shift_8;
  word |= (uint32_t)buf[(*off)++] << (uint32_t)k_priv_shift_16;
  word |= (uint32_t)buf[(*off)++] << (uint32_t)k_priv_shift_24;
  return word;
}

/**
 * @brief Read a little-endian `uint16_t` and advance the cursor.
 *
 * @details Reads two bytes least-significant-first from `*off` and
 *          advances `*off` by 2.
 *
 * @param[in]     buf Source buffer (>= 2 bytes readable at `*off`).
 * @param[in,out] off Read cursor (advanced by 2).
 *
 * @return The decoded 16-bit value.
 * @retval value The little-endian half-word assembled from two bytes.
 *
 * @pre  `buf` and `off` are non-NULL.
 * @pre  `*off + 2` does not exceed the buffer length.
 * @post `*off` has advanced by 2.
 * @post The buffer is not modified.
 * @note Internal byte cursor; no bounds check (caller pre-validates).
 * @since 0.1.0
 */
RA8_INTERNAL
static uint16_t priv_get_u16(const uint8_t* buf, size_t* off)
{
  uint16_t half = (uint16_t)buf[(*off)++];
  half          = (uint16_t)(half | ((uint16_t)buf[(*off)++] << (uint32_t)k_priv_shift_8));
  return half;
}

/* ===========================================================================
 * Hash + record codecs
 * ===========================================================================
 */

/**
 * @brief 32-bit FNV-1a hash over a byte range.
 *
 * @details Standard FNV-1a: seed with the offset basis, then for each
 *          byte XOR-in and multiply by the FNV prime. Used for both the
 *          font and content key hashes and the body checksum.
 *
 * @param[in] data Bytes to hash (may be NULL only when `len == 0`).
 * @param[in] len  Number of bytes.
 *
 * @return The FNV-1a digest.
 * @retval value The 32-bit hash of `data[0 .. len)`.
 *
 * @pre  `data` is non-NULL or `len == 0`.
 * @pre  `data[0 .. len)` is readable.
 * @post No input is modified.
 * @post `len == 0` yields the FNV offset basis.
 * @note Not a cryptographic hash; collision-resistance is not required.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_fnv1a(const uint8_t* data, size_t len)
{
  uint32_t hash = (uint32_t)k_priv_fnv_offset;
  for (size_t i = 0U; i < len; ++i) {
    hash ^= (uint32_t)data[i];
    hash *= (uint32_t)k_priv_fnv_prime;
  }
  return hash;
}

/**
 * @brief Serialise one glyph (20 bytes) and advance the cursor.
 *
 * @details Writes the seven meaningful fields (x, y, cp, colour, font_px,
 *          style, reserved). `reserved` holds the 1-based `<a>` link id
 *          (0 = not a link), which is layout state the renderer needs for
 *          hyperlink hit-testing -- it is **not** padding, so it is stored.
 *
 * @param[out]    buf   Destination buffer (>= 20 bytes free at `*off`).
 * @param[in,out] off   Write cursor (advanced by 20).
 * @param[in]     glyph Glyph to serialise.
 *
 * @pre  `buf`, `off` and `glyph` are non-NULL.
 * @pre  `*off + 20` does not exceed the buffer capacity.
 * @post `*off` has advanced by 20.
 * @post The glyph's seven fields are encoded little-endian.
 * @note Mirror of priv_get_glyph().
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_put_glyph(uint8_t* buf, size_t* off, const ra8_reflow_glyph_t* glyph)
{
  priv_put_u32(buf, off, (uint32_t)glyph->x);
  priv_put_u32(buf, off, (uint32_t)glyph->y);
  priv_put_u32(buf, off, (uint32_t)glyph->cp);
  priv_put_u32(buf, off, glyph->color);
  priv_put_u16(buf, off, glyph->font_px);
  buf[(*off)++] = glyph->style;
  buf[(*off)++] = glyph->reserved;
}

/**
 * @brief Deserialise one glyph (20 bytes) and advance the cursor.
 *
 * @details Reads the seven serialised fields, including the `reserved`
 *          link id, so the restored struct is byte-identical to the
 *          original (no field is silently zeroed).
 *
 * @param[in]     buf   Source buffer (>= 20 bytes readable at `*off`).
 * @param[in,out] off   Read cursor (advanced by 20).
 * @param[out]    glyph Receives the decoded glyph.
 *
 * @pre  `buf`, `off` and `glyph` are non-NULL.
 * @pre  `*off + 20` does not exceed the buffer length.
 * @post `*off` has advanced by 20.
 * @post `glyph->reserved` equals the serialised link id.
 * @note Mirror of priv_put_glyph().
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_get_glyph(const uint8_t* buf, size_t* off, ra8_reflow_glyph_t* glyph)
{
  glyph->x        = (int32_t)priv_get_u32(buf, off);
  glyph->y        = (int32_t)priv_get_u32(buf, off);
  glyph->cp       = (int32_t)priv_get_u32(buf, off);
  glyph->color    = priv_get_u32(buf, off);
  glyph->font_px  = priv_get_u16(buf, off);
  glyph->style    = buf[(*off)++];
  glyph->reserved = buf[(*off)++];
}

/**
 * @brief Serialise one page record (8 bytes) and advance the cursor.
 *
 * @details Writes the two `uint32_t` glyph-range fields little-endian.
 *
 * @param[out]    buf  Destination buffer (>= 8 bytes free at `*off`).
 * @param[in,out] off  Write cursor (advanced by 8).
 * @param[in]     page Page record to serialise.
 *
 * @pre  `buf`, `off` and `page` are non-NULL.
 * @pre  `*off + 8` does not exceed the buffer capacity.
 * @post `*off` has advanced by 8.
 * @post Both page-range fields are encoded little-endian.
 * @note Mirror of priv_get_page().
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_put_page(uint8_t* buf, size_t* off, const ra8_reflow_page_t* page)
{
  priv_put_u32(buf, off, page->glyph_first);
  priv_put_u32(buf, off, page->glyph_count);
}

/**
 * @brief Deserialise one page record (8 bytes) and advance the cursor.
 *
 * @details Reads the two `uint32_t` glyph-range fields little-endian.
 *
 * @param[in]     buf  Source buffer (>= 8 bytes readable at `*off`).
 * @param[in,out] off  Read cursor (advanced by 8).
 * @param[out]    page Receives the decoded page record.
 *
 * @pre  `buf`, `off` and `page` are non-NULL.
 * @pre  `*off + 8` does not exceed the buffer length.
 * @post `*off` has advanced by 8.
 * @post Both page-range fields are populated.
 * @note Mirror of priv_put_page().
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_get_page(const uint8_t* buf, size_t* off, ra8_reflow_page_t* page)
{
  page->glyph_first = priv_get_u32(buf, off);
  page->glyph_count = priv_get_u32(buf, off);
}

/* ===========================================================================
 * Header codecs + key compare
 * ===========================================================================
 */

/**
 * @brief Hash the engine's font blob (0 when no font is bound).
 *
 * @details Computes the FNV-1a digest over `engine->font_data` so a
 *          different font face / subset invalidates a cached layout.
 *
 * @param[in] engine Engine whose font blob is hashed.
 *
 * @return The font-blob hash, or 0 when no font is bound.
 * @retval 0     `engine->font_data` is NULL.
 * @retval value FNV-1a of `font_data[0 .. font_len)`.
 *
 * @pre  `engine` is non-NULL.
 * @pre  When bound, `font_data[0 .. font_len)` is readable.
 * @post No state is modified.
 * @post Equal blobs hash equal.
 * @note Shared by serialise and key-compare so both see the same value.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_font_hash(const ra8_reflow_t* engine)
{
  if (engine->font_data == nullptr) {
    return 0U;
  }
  return priv_fnv1a(engine->font_data, engine->font_len);
}

/**
 * @brief Write the fixed-size header (checksum left as a zero placeholder).
 *
 * @details Encodes the magic, version, the invalidation key (viewport /
 *          font / colours / font hash / content hash) and the record
 *          counts. The body-checksum field is written as zero and
 *          back-patched by the caller once the payload exists.
 *
 * @param[out] buf          Destination buffer (>= header bytes).
 * @param[in]  engine       Engine providing viewport / font / colours.
 * @param[in]  content_len  Chapter byte length for the key.
 * @param[in]  content_hash FNV-1a of the chapter bytes for the key.
 *
 * @pre  `buf` and `engine` are non-NULL.
 * @pre  `buf` has at least `k_ra8_reflow_cache_header_bytes` capacity.
 * @post Header bytes `[0, k_ra8_reflow_cache_header_bytes)` are written.
 * @post The body-checksum field holds a zero placeholder.
 * @note Pairs with priv_get_header().
 * @since 0.1.0
 */
RA8_INTERNAL
static void
priv_put_header(uint8_t* buf, const ra8_reflow_t* engine, size_t content_len, uint32_t content_hash)
{
  size_t off = 0U;
  priv_put_u32(buf, &off, (uint32_t)k_ra8_reflow_cache_magic);
  priv_put_u16(buf, &off, (uint16_t)k_ra8_reflow_cache_version);
  priv_put_u16(buf, &off, 0U); /* reserved */
  priv_put_u16(buf, &off, engine->viewport_w);
  priv_put_u16(buf, &off, engine->viewport_h);
  priv_put_u16(buf, &off, engine->font_px);
  priv_put_u16(buf, &off, 0U); /* reserved16 */
  priv_put_u32(buf, &off, engine->body_color);
  priv_put_u32(buf, &off, engine->link_color);
  priv_put_u32(buf, &off, (uint32_t)engine->font_len);
  priv_put_u32(buf, &off, priv_font_hash(engine));
  priv_put_u32(buf, &off, (uint32_t)content_len);
  priv_put_u32(buf, &off, content_hash);
  priv_put_u32(buf, &off, engine->glyph_count);
  priv_put_u32(buf, &off, engine->page_count);
  priv_put_u32(buf, &off, 0U); /* body_checksum placeholder */
}

/**
 * @brief Parse the fixed-size header into a key struct.
 *
 * @details Decodes every header field in the same order priv_put_header()
 *          wrote them, skipping the two reserved half-words.
 *
 * @param[in]  buf Source buffer (>= header bytes readable).
 * @param[out] key Receives the parsed header fields.
 *
 * @pre  `buf` and `key` are non-NULL.
 * @pre  `buf` has at least `k_ra8_reflow_cache_header_bytes` readable.
 * @post `*key` is fully populated from the header.
 * @post `buf` is not modified.
 * @note Pairs with priv_put_header().
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_get_header(const uint8_t* buf, priv_cache_key_t* key)
{
  size_t off   = 0U;
  key->magic   = priv_get_u32(buf, &off);
  key->version = priv_get_u16(buf, &off);
  (void)priv_get_u16(buf, &off); /* reserved */
  key->viewport_w = priv_get_u16(buf, &off);
  key->viewport_h = priv_get_u16(buf, &off);
  key->font_px    = priv_get_u16(buf, &off);
  (void)priv_get_u16(buf, &off); /* reserved16 */
  key->body_color    = priv_get_u32(buf, &off);
  key->link_color    = priv_get_u32(buf, &off);
  key->font_len      = priv_get_u32(buf, &off);
  key->font_hash     = priv_get_u32(buf, &off);
  key->content_len   = priv_get_u32(buf, &off);
  key->content_hash  = priv_get_u32(buf, &off);
  key->glyph_count   = priv_get_u32(buf, &off);
  key->page_count    = priv_get_u32(buf, &off);
  key->body_checksum = priv_get_u32(buf, &off);
}

/**
 * @brief Total serialised size for the given record counts.
 *
 * @details `header + glyph_count * glyph_bytes + page_count * page_bytes`.
 *          Shared by the size query, the serialiser and the loader so all
 *          three agree on the layout.
 *
 * @param[in] glyph_count Number of glyph records.
 * @param[in] page_count  Number of page records.
 *
 * @return The total blob size in bytes.
 * @retval value `>= k_ra8_reflow_cache_header_bytes`.
 *
 * @pre  `glyph_count` and `page_count` are within the engine pools.
 * @pre  The product does not overflow `size_t` (counts are pool-bounded).
 * @post The result includes the fixed header.
 * @post Zero counts return exactly the header size.
 * @note Pure function of its arguments.
 * @since 0.1.0
 */
RA8_INTERNAL
static size_t priv_blob_size(uint32_t glyph_count, uint32_t page_count)
{
  return (size_t)k_ra8_reflow_cache_header_bytes +
         ((size_t)glyph_count * (size_t)k_ra8_reflow_cache_glyph_bytes) +
         ((size_t)page_count * (size_t)k_ra8_reflow_cache_page_bytes);
}

/**
 * @brief True when the blob's key matches the engine + supplied content.
 *
 * @details Compares every keyed attribute -- viewport, font size, body /
 *          link colours, font length + hash, and content length + hash --
 *          so any change that would alter the layout is detected.
 *
 * @param[in] engine      Engine holding the current viewport / font / colours.
 * @param[in] key         Parsed blob header.
 * @param[in] content     Chapter bytes to hash and compare.
 * @param[in] content_len Length of `content`, bytes.
 *
 * @return Whether the blob is valid for this engine + content.
 * @retval true  Every keyed attribute matches.
 * @retval false At least one attribute differs (layout is stale).
 *
 * @pre  `engine` and `key` are non-NULL.
 * @pre  `content` is non-NULL or `content_len == 0`.
 * @post No state is modified.
 * @post A `true` result means the cached glyphs are safe to restore.
 * @note Recomputes the font and content hashes on each call.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_key_matches(const ra8_reflow_t*     engine,
                             const priv_cache_key_t* key,
                             const uint8_t*          content,
                             size_t                  content_len)
{
  /* One simple decision per attribute (no compound boolean): any single
   * mismatch makes the cached layout stale. */
  if (key->viewport_w != engine->viewport_w) {
    return false;
  }
  if (key->viewport_h != engine->viewport_h) {
    return false;
  }
  if (key->font_px != engine->font_px) {
    return false;
  }
  if (key->body_color != engine->body_color) {
    return false;
  }
  if (key->link_color != engine->link_color) {
    return false;
  }
  if (key->font_len != (uint32_t)engine->font_len) {
    return false;
  }
  if (key->font_hash != priv_font_hash(engine)) {
    return false;
  }
  if (key->content_len != (uint32_t)content_len) {
    return false;
  }
  if (key->content_hash != priv_fnv1a(content, content_len)) {
    return false;
  }
  return true;
}

/**
 * @brief Validate a blob's header structure and report its total size.
 *
 * @details
 * Checks, in order, that `len` covers the fixed header, that the magic
 * and format version match, and that the record counts do not exceed the
 * engine's static pools (`k_ra8_reflow_max_glyphs` /
 * `k_ra8_reflow_max_pages`), then computes the expected total blob size
 * and confirms `len` covers it. Pure structural validation -- the
 * invalidation key and the body checksum are verified by the caller.
 *
 * @param[in]  buf  Serialised blob (at least `len` readable bytes).
 * @param[in]  len  Length of `buf`, bytes.
 * @param[out] key  Receives the parsed header fields.
 * @param[out] need Receives the expected total blob size, bytes.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                Header valid; `*need` set.
 * @retval k_ra8_err_not_found     Magic mismatch (not a cache blob).
 * @retval k_ra8_err_invalid_state Format version mismatch.
 * @retval k_ra8_err_invalid_size  Short header, record counts beyond the
 *                                engine pools, or `len` below the size.
 *
 * @pre  `buf`, `key` and `need` are non-NULL.
 * @pre  `len` is the readable length of `buf`.
 * @post On `k_ra8_ok`, `*key` is fully populated and `*need <= len`.
 * @post On any failure the return code names the first failed check.
 *
 * @note Internal helper for `ra8_reflow_cache_load()`; depends only on its
 *       inputs (no shared state).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_parse_header(const uint8_t* buf, size_t len, priv_cache_key_t* key, size_t* need)
{
  if (len < (size_t)k_ra8_reflow_cache_header_bytes) {
    return k_ra8_err_invalid_size;
  }
  priv_get_header(buf, key);
  if (key->magic != (uint32_t)k_ra8_reflow_cache_magic) {
    return k_ra8_err_not_found;
  }
  if (key->version != (uint32_t)k_ra8_reflow_cache_version) {
    return k_ra8_err_invalid_state;
  }
  if (key->glyph_count > (uint32_t)k_ra8_reflow_max_glyphs) {
    return k_ra8_err_invalid_size;
  }
  if (key->page_count > (uint32_t)k_ra8_reflow_max_pages) {
    return k_ra8_err_invalid_size;
  }
  *need = priv_blob_size(key->glyph_count, key->page_count);
  if (len < *need) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/* ===========================================================================
 * Public API
 * ===========================================================================
 */

[[nodiscard]] ra8_err_t ra8_reflow_cache_size(const ra8_reflow_t* engine, size_t* out_bytes)
{
  if ((engine == nullptr) || (out_bytes == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra8_err_not_initialized;
  }
  *out_bytes = priv_blob_size(engine->glyph_count, engine->page_count);
  return k_ra8_ok;
}

/**
 * @brief Shared serialize/load precheck: init state, content args, and the
 *        #109 multi-face cache-bypass invariant.
 *
 * @details Per-glyph embedded face indices (#109) are not part of the cache
 * key/format, so a book with any registered `@font-face` is never cached: it
 * live-layouts instead, and a persisted page can never be mis-served under a
 * different face set. The caller checks its own NULL pointers first.
 *
 * @param[in] engine      Engine (already NULL-checked by the caller).
 * @param[in] content     Chapter bytes, or NULL iff @p content_len is 0.
 * @param[in] content_len Length of @p content, bytes.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok                Engine is initialised, content args are valid,
 *                                and no `@font-face` faces are registered.
 * @retval k_ra8_err_not_initialized Engine `in_use` flag is clear.
 * @retval k_ra8_err_invalid_arg   `content` is NULL but `content_len` is non-zero.
 * @retval k_ra8_err_invalid_state At least one `@font-face` face is registered;
 *                                the cache must be bypassed for this chapter.
 *
 * @pre  `engine` is non-NULL (the caller validates this before calling).
 * @pre  If `content` is non-NULL, `content[0 .. content_len)` is readable.
 * @post No engine state or caller memory is modified.
 * @post The return code unambiguously names the first failing precondition.
 *
 * @note Not thread-safe; relies on the caller's synchronisation context.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_cache_precheck(const ra8_reflow_t* engine, const uint8_t* content, size_t content_len)
{
  if (engine->in_use == 0U) {
    return k_ra8_err_not_initialized;
  }
  if ((content == nullptr) && (content_len != 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if (engine->face_count != 0U) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_reflow_cache_serialize(const ra8_reflow_t* engine,
                                                   const uint8_t*      content,
                                                   size_t              content_len,
                                                   uint8_t*            out_buf,
                                                   size_t              out_cap,
                                                   size_t*             out_len)
{
  if ((engine == nullptr) || (out_buf == nullptr) || (out_len == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  const ra8_err_t verr = priv_cache_precheck(engine, content, content_len);
  if (verr != k_ra8_ok) {
    return verr;
  }
  const size_t need = priv_blob_size(engine->glyph_count, engine->page_count);
  if (out_cap < need) {
    return k_ra8_err_invalid_size;
  }

  priv_put_header(out_buf, engine, content_len, priv_fnv1a(content, content_len));

  size_t off = (size_t)k_ra8_reflow_cache_header_bytes;
  for (uint32_t i = 0U; i < engine->glyph_count; ++i) {
    priv_put_glyph(out_buf, &off, &engine->glyphs[i]);
  }
  for (uint32_t i = 0U; i < engine->page_count; ++i) {
    priv_put_page(out_buf, &off, &engine->pages[i]);
  }

  const uint32_t checksum = priv_fnv1a(&out_buf[(size_t)k_ra8_reflow_cache_header_bytes],
                                       need - (size_t)k_ra8_reflow_cache_header_bytes);
  size_t         ck_off   = (size_t)k_priv_checksum_off;
  priv_put_u32(out_buf, &ck_off, checksum);
  *out_len = need;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_reflow_cache_load(ra8_reflow_t*  engine,
                                              const uint8_t* content,
                                              size_t         content_len,
                                              const uint8_t* buf,
                                              size_t         len)
{
  if ((engine == nullptr) || (buf == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  const ra8_err_t verr = priv_cache_precheck(engine, content, content_len);
  if (verr != k_ra8_ok) {
    return verr;
  }

  priv_cache_key_t key  = {};
  size_t           need = 0U;
  const ra8_err_t  perr = priv_parse_header(buf, len, &key, &need);
  if (perr != k_ra8_ok) {
    return perr;
  }
  if (!priv_key_matches(engine, &key, content, content_len)) {
    return k_ra8_err_invalid_state;
  }

  const uint32_t checksum = priv_fnv1a(&buf[(size_t)k_ra8_reflow_cache_header_bytes],
                                       need - (size_t)k_ra8_reflow_cache_header_bytes);
  if (checksum != key.body_checksum) {
    return k_ra8_err_checksum_mismatch;
  }

  size_t off = (size_t)k_ra8_reflow_cache_header_bytes;
  for (uint32_t i = 0U; i < key.glyph_count; ++i) {
    priv_get_glyph(buf, &off, &engine->glyphs[i]);
  }
  for (uint32_t i = 0U; i < key.page_count; ++i) {
    priv_get_page(buf, &off, &engine->pages[i]);
  }
  engine->glyph_count = key.glyph_count;
  engine->page_count  = key.page_count;
  engine->xhtml_buf   = content;
  engine->xhtml_len   = content_len;
  return k_ra8_ok;
}
