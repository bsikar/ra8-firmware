/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_reflow_cache.h
 * @brief Import-time pagination cache for `ra8_reflow` (#79).
 * @ingroup grp_ereader
 *
 * @details
 * Laying out a chapter (`ra8_reflow_layout_chapter()`) is the expensive
 * step in the e-reader open path: it tokenizes the XHTML, measures every
 * glyph through `stb_truetype`, and runs the greedy line-break /
 * page-break engine. For a given book that result never changes unless
 * the *viewport*, *font*, *colours*, *font blob* or *chapter bytes*
 * change -- so it is wasteful to repeat it every time the book is opened.
 *
 * This module serialises the laid-out result -- the flat `glyphs[]`
 * array and the `pages[]` index ranges, which is all the renderer needs
 * (render is a flat glyph walk) -- into a small, self-describing byte
 * blob, **keyed** by everything that would invalidate the layout:
 *
 *   - viewport width / height,
 *   - body font size (`font_px`),
 *   - body + link colours,
 *   - the font blob (length + FNV-1a hash),
 *   - the chapter content (length + FNV-1a hash).
 *
 * The blob is storage-agnostic: the application persists it however it
 * already stores data (e.g. a file on the microSD via
 * `ra8_fs_write_file()` / `ra8_fs_open()` + `ra8_fs_read()`), so this
 * module pulls in **no** filesystem dependency. The intended flow:
 *
 * @code
 *   // open path -- skip layout when the cache is valid:
 *   if (ra8_reflow_cache_load(&eng, body, body_len, blob, blob_len) != k_ra8_ok) {
 *     uint32_t pages = 0U;
 *     (void)ra8_reflow_layout_chapter(&eng, body, body_len, &pages);
 *     size_t n = 0U;
 *     (void)ra8_reflow_cache_serialize(&eng, body, body_len, blob, sizeof blob, &n);
 *     // ... persist blob[0..n) for next time ...
 *   }
 * @endcode
 *
 * `ra8_reflow_cache_load()` validates the blob's key against the engine's
 * current viewport / font / colours and the supplied content; on any
 * mismatch it returns `k_ra8_err_invalid_state` so the caller re-lays-out.
 * A body checksum guards against a corrupt blob.
 *
 * The blob is **device-local**: it is written and read back by the same
 * firmware build (same struct layout, same endianness), but the on-wire
 * form is still explicit little-endian per field -- so the checksum is
 * deterministic (no padding bytes) and a future format change is caught
 * by the version field rather than silently mis-parsed.
 *
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_reflow.h"

/* ===========================================================================
 * Format identity
 * ===========================================================================
 */

/**
 * @enum ra8_reflow_cache_id_t
 * @brief On-wire format identity for a serialised pagination cache.
 *
 * @details
 * `k_ra8_reflow_cache_magic` is the first 4 bytes of every blob (stored
 * little-endian). `k_ra8_reflow_cache_version` is bumped whenever the
 * serialised layout changes; a load of a different version is treated as
 * stale (re-layout) rather than mis-parsed.
 */
typedef enum : uint32_t {
  k_ra8_reflow_cache_magic   = 0x52464331U, /**< Blob magic ('R''F''C''1'). */
  k_ra8_reflow_cache_version = 2U,          /**< Serialised format version. */
} ra8_reflow_cache_id_t;

/**
 * @enum ra8_reflow_cache_layout_t
 * @brief Byte sizes of the serialised header and per-record payloads.
 *
 * @details
 * The blob is `header + glyph_count * glyph_bytes + page_count *
 * page_bytes`. A glyph serialises its 7 meaningful fields (x, y, cp,
 * colour, font_px, style, reserved = 4+4+4+4+2+1+1); `reserved` carries
 * the 1-based `<a>` link id used by hyperlink navigation, so it must
 * round-trip (it is layout state, not padding). A page serialises its
 * two `uint32_t` index fields.
 */
typedef enum : size_t {
  k_ra8_reflow_cache_header_bytes = 52U, /**< Fixed header size, bytes.   */
  k_ra8_reflow_cache_glyph_bytes  = 20U, /**< Serialised bytes per glyph. */
  k_ra8_reflow_cache_page_bytes   = 8U,  /**< Serialised bytes per page.  */
} ra8_reflow_cache_layout_t;

/* ===========================================================================
 * Public API
 * ===========================================================================
 */

/**
 * @brief Report the exact serialised size of an engine's current layout.
 *
 * @details
 * Lets the caller size (or bounds-check) the destination buffer before
 * calling `ra8_reflow_cache_serialize()`. The value is
 * `k_ra8_reflow_cache_header_bytes +
 *  engine->glyph_count * k_ra8_reflow_cache_glyph_bytes +
 *  engine->page_count  * k_ra8_reflow_cache_page_bytes`.
 *
 * @param[in]  engine    Initialised engine (a chapter need not be laid
 *                       out yet; an empty layout serialises to just the
 *                       header).
 * @param[out] out_bytes Receives the required buffer size in bytes.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                  Size reported.
 * @retval k_ra8_err_null_ptr        `engine` or `out_bytes` is NULL.
 * @retval k_ra8_err_not_initialized `engine->in_use == 0`.
 *
 * @pre  `engine` non-NULL and `engine->in_use == 1`.
 * @post `*out_bytes >= k_ra8_reflow_cache_header_bytes`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_reflow_cache_size(const ra8_reflow_t* engine, size_t* out_bytes);

/**
 * @brief Serialise the laid-out pages of `engine` into a keyed byte blob.
 *
 * @details
 * Writes the header (magic, version, the invalidation key, glyph / page
 * counts and a body checksum) followed by the flat `glyphs[]` array and
 * the `pages[]` index ranges, each field little-endian. The key captures
 * the engine's viewport, font size, colours, the font blob (length +
 * FNV-1a hash) and the supplied content (length + FNV-1a hash) so a
 * later `ra8_reflow_cache_load()` can detect any change that would
 * invalidate the layout.
 *
 * @param[in]  engine      Laid-out engine handle.
 * @param[in]  content     Chapter bytes the layout was produced from
 *                         (hashed into the key). May be NULL only when
 *                         `content_len == 0`.
 * @param[in]  content_len Length of `content`, bytes.
 * @param[out] out_buf     Destination buffer.
 * @param[in]  out_cap     Capacity of `out_buf`, bytes.
 * @param[out] out_len     Receives the number of bytes written.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                  Serialised; `*out_len` bytes written.
 * @retval k_ra8_err_null_ptr        `engine`, `out_buf` or `out_len` NULL.
 * @retval k_ra8_err_not_initialized `engine->in_use == 0`.
 * @retval k_ra8_err_invalid_arg     `content` NULL with `content_len > 0`.
 * @retval k_ra8_err_invalid_size    `out_cap` smaller than the blob size.
 *
 * @pre  `engine->in_use == 1`; a chapter has been laid out.
 * @post On success the first `*out_len` bytes of `out_buf` are a valid
 *       blob accepted by `ra8_reflow_cache_load()`.
 *
 * @see ra8_reflow_cache_size()
 * @see ra8_reflow_cache_load()
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_reflow_cache_serialize(const ra8_reflow_t* engine,
                                                   const uint8_t*      content,
                                                   size_t              content_len,
                                                   uint8_t*            out_buf,
                                                   size_t              out_cap,
                                                   size_t*             out_len);

/**
 * @brief Validate a cache blob and, if it matches, restore the layout.
 *
 * @details
 * Parses the blob header, then validates -- in order -- the magic, the
 * format version, the record counts (against the engine's static pools
 * and the blob length), the invalidation key (viewport / font / colours
 * / font hash / content hash) and the body checksum. Only when **all**
 * pass are the `glyphs[]` and `pages[]` arrays copied back into `engine`
 * and `engine->xhtml_buf` / `xhtml_len` repointed at `content` (so a
 * later `ra8_reflow_set_font_size()` can still re-flow). On any mismatch
 * the engine is left untouched and the caller should re-lay-out.
 *
 * @param[in,out] engine      Engine initialised with the **same**
 *                            viewport / font / colours the blob was made
 *                            with (the load validates this).
 * @param[in]     content     Chapter bytes (hashed and compared to the
 *                            blob's content hash). May be NULL only when
 *                            `content_len == 0`.
 * @param[in]     content_len Length of `content`, bytes.
 * @param[in]     buf         Serialised blob.
 * @param[in]     len         Length of `buf`, bytes.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                   Loaded; glyphs / pages restored.
 * @retval k_ra8_err_null_ptr         `engine` or `buf` is NULL.
 * @retval k_ra8_err_not_initialized  `engine->in_use == 0`.
 * @retval k_ra8_err_invalid_arg      `content` NULL with `content_len>0`.
 * @retval k_ra8_err_not_found        Magic mismatch (not a cache blob).
 * @retval k_ra8_err_invalid_state    Version or key mismatch -- the layout
 *                                   is stale; re-lay-out and re-serialise.
 * @retval k_ra8_err_invalid_size     Truncated blob or counts exceed pools.
 * @retval k_ra8_err_checksum_mismatch Body checksum failed (corrupt blob).
 *
 * @pre  `engine->in_use == 1`.
 * @post On `k_ra8_ok`, `engine->glyph_count` / `page_count` and the
 *       restored arrays match the serialised layout and rendering may
 *       proceed without `ra8_reflow_layout_chapter()`.
 * @post On any non-`k_ra8_ok` return the engine is unchanged.
 *
 * @see ra8_reflow_cache_serialize()
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_reflow_cache_load(ra8_reflow_t*  engine,
                                              const uint8_t* content,
                                              size_t         content_len,
                                              const uint8_t* buf,
                                              size_t         len);

#ifdef __cplusplus
}
#endif
