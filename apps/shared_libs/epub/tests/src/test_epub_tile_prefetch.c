/**
 * @file test_epub_tile_prefetch.c
 * @brief Pan-direction tile prefetch through the EPUB tile binder (#341).
 *
 * @details
 * Registers a hand-built raw gray8 JOF atlas (a 3x2 tile grid) as an external
 * binder source, then proves ::epub_tile_binder_prefetch_pan warms the tile
 * column one step ahead of the viewport: the warmed tiles are resident on a
 * following fetch (the cache miss counter does not move), and the not-found /
 * NULL / off-grid guards hold. The atlas is built by hand -- the smallest
 * structurally valid JOF a raw tile reader accepts -- so this test needs no ZIP,
 * book, or codec machinery; residency (not pixel content) is the property under
 * test, so the tiles carry arbitrary deterministic fill.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "epub_img_tiles.h"
#include "jof.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_tile_cache.h"
#include "unity_minimal.h"

/**
 * @enum tp_geom_t
 * @brief Hand-built atlas geometry (a 3x2 grid of 4x4 gray8 tiles).
 */
typedef enum : uint16_t {
  k_tp_tile = 4U,  /**< Square tile edge, pixels. */
  k_tp_w    = 12U, /**< Atlas width  (3 * tile).  */
  k_tp_h    = 8U,  /**< Atlas height (2 * tile).  */
} tp_geom_t;

/**
 * @enum tp_const_t
 * @brief Fixture sizes, cache budget, and ids.
 */
typedef enum : uint32_t {
  k_tp_tile_bytes = 16U,  /**< gray8 4x4 tile payload.            */
  k_tp_tiles      = 6U,   /**< Tile count (3 cols * 2 rows).      */
  k_tp_cells      = 8U,   /**< Cache cells (>= frame + lead col). */
  k_tp_buckets    = 16U,  /**< Cache hash buckets.                */
  k_tp_image_id   = 7U,   /**< Binder image id.                   */
  k_tp_store_cap  = 512U, /**< Atlas memstore capacity.           */
} tp_const_t;

/**
 * @enum tp_layout_t
 * @brief JOF container field lengths used by the hand builder.
 */
typedef enum : uint32_t {
  k_tp_hdr_pad  = 12U, /**< Header pad to the fixed 32-byte length. */
  k_tp_ftr_tail = 8U,  /**< total_size u32 + "JOFE" magic (bytes).  */
} tp_layout_t;

/** @brief gray8 bytes-per-pixel for the atlas header. */
enum : uint8_t {
  k_tp_bpp_gray8 = 1U, /**< One channel: the reader path is gray8. */
};

/** @brief The atlas memstore (sink + pread over ::s_store_buf). */
static jof_memstore_t s_store;

/** @brief Append a little-endian u16 to the atlas memstore. @details Implements the tp put u16 fixture operation used only by this focused test executable. @param[in] v Fixture argument governed by the exercised interface contract. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_tp_put_u16(uint16_t v)
{
  const uint8_t b[2] = {(uint8_t)(v & 0xFFU), (uint8_t)(v >> 8U)};
  TEST_ASSERT_EQ(k_ra8_ok, jof_memstore_sink(&s_store, b, sizeof(b)));
}

/** @brief Append a little-endian u32 to the atlas memstore. @details Implements the tp put u32 fixture operation used only by this focused test executable. @param[in] v Fixture argument governed by the exercised interface contract. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_tp_put_u32(uint32_t v)
{
  const uint8_t b[4] = {(uint8_t)(v & 0xFFU),
                        (uint8_t)((v >> 8U) & 0xFFU),
                        (uint8_t)((v >> 16U) & 0xFFU),
                        (uint8_t)((v >> 24U) & 0xFFU)};
  TEST_ASSERT_EQ(k_ra8_ok, jof_memstore_sink(&s_store, b, sizeof(b)));
}

/** @brief Append raw bytes to the atlas memstore. @details Implements the tp put bytes fixture operation used only by this focused test executable. @param[in] p Fixture argument governed by the exercised interface contract. @param[in] n Fixture argument governed by the exercised interface contract. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_tp_put_bytes(const uint8_t* p, size_t n)
{
  TEST_ASSERT_EQ(k_ra8_ok, jof_memstore_sink(&s_store, p, n));
}

/**
 * @brief Write the 32-byte JOF header for the gray8 3x2 raw atlas.
 * @details Layout mirrors the writer in test_jof.c: magic, geometry,
 *          bpp+codec, a zero reserved u16, the tile count, then pad to 32.
 * @pre ::s_store is reset to empty.
 * @pre The geometry constants are consistent (width == cols * tile, etc.).
 * @post 32 header bytes have been appended to ::s_store.
 * @post No other fixture state is mutated.
 * @note Single-threaded host-test helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_tp_write_header(void)
{
  const uint8_t magic[4] = {'J', 'O', 'F', '1'};
  internal_tp_put_bytes(magic, sizeof(magic));
  internal_tp_put_u16((uint16_t)k_tp_w);
  internal_tp_put_u16((uint16_t)k_tp_h);
  internal_tp_put_u16((uint16_t)k_tp_tile);
  internal_tp_put_u16((uint16_t)k_tp_tile);
  const uint8_t bpp_codec[2] = {(uint8_t)k_tp_bpp_gray8, (uint8_t)k_jof_codec_raw};
  internal_tp_put_bytes(bpp_codec, sizeof(bpp_codec));
  internal_tp_put_u16(0U); /* reserved (must be zero) */
  internal_tp_put_u32((uint32_t)k_tp_tiles);
  const uint8_t pad[k_tp_hdr_pad] = {};
  internal_tp_put_bytes(pad, sizeof(pad));
}

/**
 * @brief Build a structurally valid raw gray8 3x2 JOF atlas into ::s_store.
 * @details Header, then six raw 4x4 tiles (each filled with its 1-based index),
 *          then a row-major index of (offset,len) pairs, then the 16-byte footer
 *          whose total_size equals the final store length.
 * @pre ::s_store_buf out-lives ::s_store.
 * @pre ::k_tp_store_cap is large enough for the whole atlas.
 * @post ::s_store holds a parseable atlas; `s_store.len` == the footer total.
 * @post No other fixture state is mutated.
 * @note Single-threaded host-test helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_tp_build_atlas(void)
{
  static uint8_t s_store_buf[k_tp_store_cap];
  s_store = (jof_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf), .len = 0U};
  internal_tp_write_header();
  const uint32_t tiles_off = (uint32_t)s_store.len; /* == 32 */
  for (uint32_t i = 0U; i < (uint32_t)k_tp_tiles; ++i) {
    uint8_t       tile[k_tp_tile_bytes];
    const uint8_t fill = (uint8_t)(i + 1U);
    (void)memset(tile, (int)fill, sizeof(tile));
    internal_tp_put_bytes(tile, sizeof(tile));
  }
  const uint32_t index_off = (uint32_t)s_store.len;
  for (uint32_t i = 0U; i < (uint32_t)k_tp_tiles; ++i) {
    internal_tp_put_u32(tiles_off + (i * (uint32_t)k_tp_tile_bytes));
    internal_tp_put_u32((uint32_t)k_tp_tile_bytes);
  }
  internal_tp_put_u32(index_off);
  internal_tp_put_u32((uint32_t)k_tp_tiles);
  internal_tp_put_u32((uint32_t)s_store.len + (uint32_t)k_tp_ftr_tail);
  const uint8_t fmagic[4] = {'J', 'O', 'F', 'E'};
  internal_tp_put_bytes(fmagic, sizeof(fmagic));
}

/** @brief Bind a binder over the fixture storage with the built atlas registered. @details Implements the tp init binder fixture operation used only by this focused test executable. @param[in,out] binder Fixture argument governed by the exercised interface contract. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_tp_init_binder(epub_tile_binder_t* binder)
{
  static uint8_t             s_cell_mem[(size_t)k_tp_cells * (size_t)k_tp_tile_bytes];
  static ra8_tile_key_t      s_keys[k_tp_cells];
  static ra8_tile_dims_t     s_dims[k_tp_cells];
  static ra8_keycache_cell_t s_meta[k_tp_cells];
  static int32_t             s_buckets[k_tp_buckets];
  const ra8_tile_cache_cfg_t storage = {.cell_mem     = s_cell_mem,
                                        .cell_bytes   = (uint32_t)k_tp_tile_bytes,
                                        .cell_count   = (uint32_t)k_tp_cells,
                                        .meta         = s_meta,
                                        .keys         = s_keys,
                                        .dims         = s_dims,
                                        .buckets      = s_buckets,
                                        .bucket_count = (uint32_t)k_tp_buckets};
  /* A raw atlas needs no deflate staging scratch. */
  TEST_ASSERT_EQ(k_ra8_ok, epub_tile_binder_init(binder, &storage, nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_ok,
                 epub_tile_binder_add_ext(binder,
                                          jof_memstore_pread,
                                          &s_store,
                                          s_store.len,
                                          (uint32_t)k_tp_image_id));
}

/** @brief Fetch tile (tx,ty) of the fixture image and release it (must succeed). @details Implements the tp fetch ok fixture operation used only by this focused test executable. @param[in,out] b Fixture argument governed by the exercised interface contract. @param[in] tx Fixture argument governed by the exercised interface contract. @param[in] ty Fixture argument governed by the exercised interface contract. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_tp_fetch_ok(epub_tile_binder_t* b, uint16_t tx, uint16_t ty)
{
  ra8_tile_t t = {};
  TEST_ASSERT_EQ(k_ra8_ok, epub_tile_binder_get(b, (uint32_t)k_tp_image_id, tx, ty, &t));
  TEST_ASSERT_EQ(k_ra8_ok, epub_tile_binder_put(b, t.pixels));
}

/**
 * @test internal_test_binder_prefetch_pan
 * @brief A pan-right prefetch through the binder warms the lead column of the
 *        3x2 atlas; the warmed tiles are then resident (a fetch hits, no
 *        re-decode) and the not-found / NULL / off-grid guards hold (#341).
 *
 * @par MC/DC:
 * (no compound decisions authored under test: the binder forwards to
 * ::ra8_tile_cache_prefetch_pan after a single-condition source lookup. Warmed-
 * tile residency is proved by an unchanged miss counter across a fetch, and each
 * guard is an independent single-condition check.) @details Executes the binder prefetch pan scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_binder_prefetch_pan(void)
{
  TEST_BEGIN("epub tile prefetch: pan warms the lead column, resident");
  internal_tp_build_atlas();
  epub_tile_binder_t binder = {};
  internal_tp_init_binder(&binder);

  /* Visible col 0 rows 0..1; pan right -> warm col 1 rows 0..1 (2 tiles). */
  const ra8_tile_rect_t view   = {.tx0 = 0U, .ty0 = 0U, .tx1 = 0U, .ty1 = 1U};
  uint16_t              warmed = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 epub_tile_binder_prefetch_pan(&binder,
                                               (uint32_t)k_tp_image_id,
                                               &view,
                                               k_ra8_tile_pan_right,
                                               (uint16_t)k_tp_cells,
                                               &warmed));
  TEST_ASSERT_EQ(2, warmed);

  /* Warmed tiles are resident: fetching them does not raise the miss counter. */
  uint32_t miss_before = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_stats(&binder.cache, nullptr, &miss_before, nullptr));
  internal_tp_fetch_ok(&binder, 1U, 0U);
  internal_tp_fetch_ok(&binder, 1U, 1U);
  uint32_t miss_after = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_stats(&binder.cache, nullptr, &miss_after, nullptr));
  TEST_ASSERT_EQ(miss_before, miss_after); /* both fetches were cache hits */

  /* Guards: unknown image, NULL binder / view, and an unordered rectangle. */
  TEST_ASSERT_EQ(
    k_ra8_err_not_found,
    epub_tile_binder_prefetch_pan(&binder, 999U, &view, k_ra8_tile_pan_right, 4U, &warmed));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 epub_tile_binder_prefetch_pan(nullptr,
                                               (uint32_t)k_tp_image_id,
                                               &view,
                                               k_ra8_tile_pan_right,
                                               4U,
                                               &warmed));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 epub_tile_binder_prefetch_pan(&binder,
                                               (uint32_t)k_tp_image_id,
                                               nullptr,
                                               k_ra8_tile_pan_right,
                                               4U,
                                               &warmed));
  const ra8_tile_rect_t bad = {.tx0 = 2U, .ty0 = 0U, .tx1 = 1U, .ty1 = 0U}; /* tx0 > tx1 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 epub_tile_binder_prefetch_pan(&binder,
                                               (uint32_t)k_tp_image_id,
                                               &bad,
                                               k_ra8_tile_pan_right,
                                               4U,
                                               &warmed));
  TEST_END("epub tile prefetch: pan warms the lead column, resident");
}

/**
 * @brief Test entry point -- runs the EPUB binder pan-prefetch gate.
 * @return 0 on success; unity_minimal.h exits non-zero on first failure.
 * @pre None.
 * @pre None.
 * @post The gate ran (or the process exited on first failure).
 * @post stderr carries the per-test RUN/PASS log.
 * @note Not thread-safe. No SIGALRM / timers used.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_binder_prefetch_pan();
  return 0;
}
