/**
 * @file test_ra8_tileatlas_produce_webp.c
 * @brief Host tests for WebP normalize-on-import: WebP -> JOF through the
 *        #231 producer, byte-consistent tiles, cross-format convergence, and
 *        fail-closed hostile handling (#290).
 *
 * @details
 * The #290 goal is that every source codec converges on one on-device format:
 * these tests drive a committed lossless (VP8L) and lossy (VP8) WebP through
 * `ra8_tileatlas_produce()` and prove:
 *
 *   1. **Golden pixels** -- the lossless WebP's decoded RGBA tiles page back
 *      byte-exact to its deterministic source pattern
 *      `((x*32)&255, (y*32)&255, ((x+y)*16)&255, 255)`.
 *   2. **Cross-format convergence** -- a PNG (colour type 6, RGBA) of the
 *      *same* pixels produces a **byte-identical** JOF atlas. Import genuinely
 *      normalizes: the reader cannot tell the source codec apart.
 *   3. **Lossy path** -- a VP8 WebP normalizes to a valid JOF (pixels not
 *      bit-compared; VP8 is lossy) so the whole codec family reaches JOF.
 *   4. **Fail-closed** -- untrusted WebP: no `webp_work` arena, a RIFF header
 *      that is not WEBP, an over-cap dimension, a truncated bitstream and a
 *      too-small arena all return contracted error codes, never a crash.
 *
 * The WebP fixtures are the committed files under tests/fixtures/webp/ (see
 * that dir's README for provenance), embedded inline here as in
 * tests/test_ra8_webp.c so the test needs no runtime file I/O. The PNG oracle
 * is built in-test with an encoder (miniz `mz_compress`) independent of the
 * decoder under test.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "ra8_err.h"
#include "ra8_tileatlas.h"
#include "ra8_tileatlas_produce.h"
#include "unity_minimal.h"

/* ------------------------------------------------------------------------- */
/* Committed WebP fixtures under tests/fixtures/webp/, embedded inline. */
/* ------------------------------------------------------------------------- */

/** 8x8 VP8L (lossless, -exact): decoded RGB is bit-exact to the pattern. */
static const uint8_t k_webp_lossless[] = {
  0x52, 0x49, 0x46, 0x46, 0x2C, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56,
  0x50, 0x38, 0x4C, 0x1F, 0x00, 0x00, 0x00, 0x2F, 0x07, 0xC0, 0x01, 0x00, 0xCD,
  0x65, 0x44, 0xFF, 0x63, 0x17, 0x85, 0x28, 0x78, 0xFF, 0x03, 0x42, 0x02, 0xC2,
  0x14, 0xFF, 0x77, 0x6A, 0x0E, 0x0C, 0x48, 0xC4, 0x04, 0x80, 0xAD, 0x0D, 0x00,
};

/** 8x8 VP8 (lossy): proves the lossy decode path normalizes to JOF. */
static const uint8_t k_webp_lossy[] = {
  0x52, 0x49, 0x46, 0x46, 0x4C, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50,
  0x38, 0x20, 0x40, 0x00, 0x00, 0x00, 0xD0, 0x01, 0x00, 0x9D, 0x01, 0x2A, 0x08, 0x00,
  0x08, 0x00, 0x01, 0x40, 0x26, 0x25, 0xA8, 0x02, 0x74, 0x01, 0x0F, 0x0C, 0x06, 0xC5,
  0xE0, 0x00, 0xFE, 0xFC, 0xD2, 0xFE, 0x92, 0x7B, 0xB2, 0xF7, 0x72, 0xC7, 0x79, 0xA0,
  0xF3, 0x66, 0x26, 0x95, 0xF3, 0x4F, 0xFF, 0xF5, 0x1F, 0xCD, 0x05, 0xC9, 0x25, 0xFE,
  0xFF, 0x9F, 0xB4, 0x46, 0x43, 0xFF, 0x80, 0xFF, 0xA2, 0x97, 0x0C, 0x00, 0x00, 0x00,
};

/** 8200x2 VP8L (solid): width exceeds the WebP per-axis cap (8192). */
static const uint8_t k_webp_wide[] = {
  0x52, 0x49, 0x46, 0x46, 0x18, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50, 0x38, 0x4C,
  0x0C, 0x00, 0x00, 0x00, 0x2F, 0x07, 0x60, 0x00, 0x00, 0x28, 0x45, 0x15, 0xEA, 0xD1, 0xFF, 0x00,
};

/**
 * @enum t_webp_const_t
 * @brief Geometry + buffer sizing for the 8x8 WebP fixtures (no magic numbers).
 */
typedef enum : uint32_t {
  k_t_dim       = 8U,                /**< Fixture width and height, pixels.      */
  k_t_bpp       = 4U,                /**< RGBA8888 bytes per pixel.              */
  k_t_tile      = 4U,                /**< Tile edge (8/4 = a 2x2 tile grid).     */
  k_t_grid      = 4U,                /**< Tiles in the 2x2 grid.                 */
  k_t_alpha     = 255U,              /**< Opaque alpha in the golden pattern.    */
  k_t_riff_ofs  = 8U,                /**< Offset of the WEBP fourCC in the head. */
  k_t_src_cap   = 4U * 1024U,        /**< PNG-oracle source capacity.            */
  k_t_store_cap = 64U * 1024U,       /**< Memstore capacity per atlas.           */
  k_t_work_cap  = 1024U * 1024U,     /**< Streaming pixel-path arena (deflate    */
                                     /* scratch dominates -- see work_bytes()). */
  k_t_webp_cap = 4U * 1024U * 1024U, /**< WebP whole-frame arena. */
  k_t_cell_cap = 64U * 1024U,        /**< Tile page-back buffer.  */
  /* Two under-sized WebP arenas that make the transcode's arena-room decision
   * `(used >= acap) || (frame64 >= (acap - used))` fail closed, one per
   * condition. The lossless fixture is 52 bytes and the bump aligns to 8, so
   * the resident source occupies `used = ceil(52 / 8) * 8 = 56` bytes. */
  k_t_webp_cap_srcfill = 56U, /**< == used: the aligned source fills the arena.   */
  k_t_webp_cap_noframe = 64U, /**< > used, < used + frame: no room for the frame. */
} t_webp_const_t;

/** @brief Streaming pixel-path work arena. */
static uint8_t s_work[k_t_work_cap];
/** @brief WebP whole-frame arena (source + RGBA frame + libwebp scratch). */
static uint8_t s_webp_work[k_t_webp_cap];
/** @brief Atlas store A backing (WebP-produced). */
static uint8_t s_store_a[k_t_store_cap];
/** @brief Atlas store B backing (PNG-produced, convergence oracle). */
static uint8_t s_store_b[k_t_store_cap];
/** @brief Tile page-back buffer. */
static uint8_t s_cell[k_t_cell_cap];
/** @brief Stored-tile staging for the deflate page-back. */
static uint8_t s_scratch[k_t_cell_cap];
/** @brief In-test PNG builder scratch (the RGBA oracle source). */
static uint8_t s_png[k_t_src_cap];
/** @brief In-test PNG builder length. */
static size_t s_png_len;
/** @brief Filtered-scanline staging for the PNG builder. */
static uint8_t s_raw[k_t_dim * ((k_t_dim * k_t_bpp) + 1U)];
/** @brief zlib staging for the PNG builder. */
static uint8_t s_zbuf[k_t_src_cap];

/**
 * @struct t_mem_pull_t
 * @brief Memory pull source over an embedded byte array.
 */
typedef struct {
  const uint8_t* d;   /**< Source bytes.  */
  size_t         n;   /**< Source length. */
  size_t         pos; /**< Read cursor.   */
} t_mem_pull_t;

/** @brief ::ra8_tileatlas_pull_fn over a ::t_mem_pull_t. */
static ra8_err_t t_mem_pull(void* ctx, uint8_t* buf, size_t cap, size_t* got)
{
  t_mem_pull_t* p    = (t_mem_pull_t*)ctx;
  const size_t  left = p->n - p->pos;
  const size_t  take = (cap < left) ? cap : left;
  memcpy(buf, &p->d[p->pos], take);
  p->pos += take;
  *got = take;
  return k_ra8_ok;
}

/** @brief The golden RGBA pattern channel at (x, y, c) (matches the fixture). */
static uint8_t t_pix(uint32_t x, uint32_t y, uint32_t c)
{
  if (c == 0U) {
    return (uint8_t)((x * 32U) & 0xFFU);
  }
  if (c == 1U) {
    return (uint8_t)((y * 32U) & 0xFFU);
  }
  if (c == 2U) {
    return (uint8_t)(((x + y) * 16U) & 0xFFU);
  }
  return (uint8_t)k_t_alpha;
}

/* ------------------------------------------------------------------------- */
/* Produce helpers. */
/* ------------------------------------------------------------------------- */

/**
 * @brief Run the producer over an embedded WebP into @p store.
 * @param[in]  src       WebP bytes.
 * @param[in]  src_len   WebP byte count.
 * @param[in]  webp_work WebP whole-frame arena (NULL disables WebP).
 * @param[in]  webp_cap  Capacity of @p webp_work.
 * @param[out] store     Fresh memstore to receive the atlas.
 * @param[out] info      Receives the produced geometry.
 * @return Producer result code.
 * @pre @p src holds @p src_len readable bytes.
 * @pre @p store backing covers its `cap`.
 * @post On k_ra8_ok the store holds one JOF atlas.
 * @post On error the store may hold a partial (discarded) atlas.
 * @note Not thread-safe (shared work arenas).
 * @since 0.1.0
 */
static ra8_err_t produce_webp(const uint8_t*            src,
                              size_t                    src_len,
                              uint8_t*                  webp_work,
                              size_t                    webp_cap,
                              ra8_tileatlas_memstore_t* store,
                              ra8_tileatlas_info_t*     info)
{
  static t_mem_pull_t pull;
  pull   = (t_mem_pull_t){.d = src, .n = src_len, .pos = 0U};
  *store = (ra8_tileatlas_memstore_t){.buf = store->buf, .cap = store->cap, .len = 0U};
  const ra8_tileatlas_produce_cfg_t cfg = {
    .pull          = t_mem_pull,
    .pull_ctx      = &pull,
    .sink          = ra8_tileatlas_memstore_sink,
    .sink_ctx      = store,
    .tile_w        = (uint16_t)k_t_tile,
    .tile_h        = (uint16_t)k_t_tile,
    .codec         = (uint8_t)k_ra8_tileatlas_codec_deflate,
    .max_width     = (uint16_t)k_t_dim,
    .max_height    = (uint16_t)k_t_dim,
    .work          = s_work,
    .work_cap      = sizeof(s_work),
    .webp_work     = webp_work,
    .webp_work_cap = webp_cap,
  };
  return ra8_tileatlas_produce(&cfg, info);
}

/**
 * @brief Build an 8-bit RGBA (colour type 6) PNG of the golden pattern.
 * @details Filter 0 (none) on every row; IDAT via miniz `mz_compress`; CRCs via
 *          `mz_crc32`. Produces the exact RGBA pixels the lossless WebP decodes
 *          to, so both sources yield a byte-identical JOF atlas.
 * @pre The pattern buffers cover the 8x8 geometry.
 * @pre `s_png` covers the built PNG.
 * @post `s_png` / `s_png_len` hold a well-formed RGBA PNG.
 * @post No other state mutated.
 * @note Not thread-safe (shared scratch).
 * @since 0.1.0
 */
static void build_rgba_png(void)
{
  static const uint8_t sig[8] = {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU};
  const uint32_t       rowb   = (uint32_t)k_t_dim * (uint32_t)k_t_bpp;
  size_t               o      = 0U;
  for (uint32_t y = 0U; y < (uint32_t)k_t_dim; y++) {
    s_raw[o] = 0U; /* filter type 0 (none) */
    o++;
    for (uint32_t x = 0U; x < (uint32_t)k_t_dim; x++) {
      for (uint32_t c = 0U; c < (uint32_t)k_t_bpp; c++) {
        s_raw[o] = t_pix(x, y, c);
        o++;
      }
    }
  }
  uint8_t ihdr[13] = {};
  ihdr[3]          = (uint8_t)k_t_dim; /* width  (big-endian, high bytes 0) */
  ihdr[7]          = (uint8_t)k_t_dim; /* height                            */
  ihdr[8]          = 8U;               /* bit depth                         */
  ihdr[9]          = 6U;               /* colour type: RGBA                 */
  s_png_len        = 0U;
  memcpy(s_png, sig, sizeof(sig));
  s_png_len         = sizeof(sig);
  uint8_t chunk[64] = {};
  /* IHDR */
  const uint32_t ihdr_len = (uint32_t)sizeof(ihdr);
  chunk[3]                = (uint8_t)ihdr_len;
  memcpy(&chunk[4], "IHDR", 4U);
  memcpy(&chunk[8], ihdr, ihdr_len);
  uint32_t crc              = (uint32_t)mz_crc32(MZ_CRC32_INIT, &chunk[4], 4U + (size_t)ihdr_len);
  chunk[8U + ihdr_len + 0U] = (uint8_t)(crc >> 24U);
  chunk[8U + ihdr_len + 1U] = (uint8_t)((crc >> 16U) & 0xFFU);
  chunk[8U + ihdr_len + 2U] = (uint8_t)((crc >> 8U) & 0xFFU);
  chunk[8U + ihdr_len + 3U] = (uint8_t)(crc & 0xFFU);
  memcpy(&s_png[s_png_len], chunk, 12U + (size_t)ihdr_len);
  s_png_len += 12U + (size_t)ihdr_len;
  /* IDAT */
  mz_ulong zlen = (mz_ulong)sizeof(s_zbuf);
  TEST_ASSERT_EQ(MZ_OK,
                 mz_compress(s_zbuf, &zlen, s_raw, (mz_ulong)((rowb + 1U) * (uint32_t)k_t_dim)));
  uint8_t* p = &s_png[s_png_len];
  p[0]       = (uint8_t)(zlen >> 24U);
  p[1]       = (uint8_t)((zlen >> 16U) & 0xFFU);
  p[2]       = (uint8_t)((zlen >> 8U) & 0xFFU);
  p[3]       = (uint8_t)(zlen & 0xFFU);
  memcpy(&p[4], "IDAT", 4U);
  memcpy(&p[8], s_zbuf, (size_t)zlen);
  crc           = (uint32_t)mz_crc32(MZ_CRC32_INIT, &p[4], 4U + (size_t)zlen);
  p[8U + zlen]  = (uint8_t)(crc >> 24U);
  p[9U + zlen]  = (uint8_t)((crc >> 16U) & 0xFFU);
  p[10U + zlen] = (uint8_t)((crc >> 8U) & 0xFFU);
  p[11U + zlen] = (uint8_t)(crc & 0xFFU);
  s_png_len += 12U + (size_t)zlen;
  /* IEND */
  uint8_t iend[12] = {0U, 0U, 0U, 0U, 'I', 'E', 'N', 'D', 0xAEU, 0x42U, 0x60U, 0x82U};
  memcpy(&s_png[s_png_len], iend, sizeof(iend));
  s_png_len += sizeof(iend);
}

/**
 * @brief Page every tile of a produced atlas and compare to the golden pattern.
 * @param[in] store The memstore holding the atlas.
 * @param[in] info  Its parsed geometry.
 * @pre @p info describes @p store's atlas.
 * @pre The atlas encodes the golden RGBA pattern.
 * @post Every tile byte was compared (test exits on mismatch).
 * @post @p store is unmodified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void check_golden_tiles(ra8_tileatlas_memstore_t* store, const ra8_tileatlas_info_t* info)
{
  for (uint16_t ty = 0U; ty < info->tile_rows; ty++) {
    for (uint16_t tx = 0U; tx < info->tile_cols; tx++) {
      uint16_t w = 0U;
      uint16_t h = 0U;
      TEST_ASSERT_EQ(k_ra8_ok,
                     ra8_tileatlas_read_tile(ra8_tileatlas_memstore_pread,
                                             store,
                                             info,
                                             tx,
                                             ty,
                                             s_scratch,
                                             (uint32_t)sizeof(s_scratch),
                                             s_cell,
                                             (uint32_t)sizeof(s_cell),
                                             &w,
                                             &h));
      const uint32_t x0 = (uint32_t)tx * info->tile_w;
      const uint32_t y0 = (uint32_t)ty * info->tile_h;
      for (uint32_t r = 0U; r < h; r++) {
        for (uint32_t c = 0U; c < w; c++) {
          for (uint32_t ch = 0U; ch < info->bpp; ch++) {
            const uint8_t got = s_cell[(((r * w) + c) * info->bpp) + ch];
            TEST_ASSERT_EQ(t_pix(x0 + c, y0 + r, ch), got);
          }
        }
      }
    }
  }
}

/* ------------------------------------------------------------------------- */
/* Tests. */
/* ------------------------------------------------------------------------- */

/**
 * @test test_webp_lossless_golden
 * @brief A lossless WebP normalizes to JOF whose tiles page back byte-exact to
 *        the source pattern, with the `webp_work` arena sized by
 *        `ra8_tileatlas_webp_work_bytes()` proving the sizing guarantee.
 *
 * @par MC/DC:
 * Decision (WebP sniff): `RIFF(head[0..3]) && WEBP(head[8..11])` (2 conditions)
 * - Vector 1: RIFF + WEBP  -> true  (this test decodes the WebP)
 * - Vector 2: not RIFF     -> false via cond 1 (the JPEG/PNG sibling tests)
 * - Vector 3: RIFF, not WEBP -> false via cond 2 (test_webp_hostile below)
 */
static void test_webp_lossless_golden(void)
{
  TEST_BEGIN("produce webp: lossless -> JOF golden pixels + sized arena");
  const uint32_t need =
    ra8_tileatlas_webp_work_bytes((uint16_t)k_t_dim, (uint16_t)k_t_dim, sizeof(k_webp_lossless));
  TEST_ASSERT(need > 0U);
  TEST_ASSERT(need <= sizeof(s_webp_work));

  ra8_tileatlas_memstore_t store = {.buf = s_store_a, .cap = sizeof(s_store_a), .len = 0U};
  ra8_tileatlas_info_t     info  = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 produce_webp(k_webp_lossless,
                              sizeof(k_webp_lossless),
                              s_webp_work,
                              (size_t)need,
                              &store,
                              &info));
  TEST_ASSERT_EQ(k_t_bpp, info.bpp);
  TEST_ASSERT_EQ(k_t_dim, info.width);
  TEST_ASSERT_EQ(k_t_dim, info.height);
  TEST_ASSERT_EQ(k_t_grid, info.tile_count);

  ra8_tileatlas_info_t reparsed = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_tileatlas_parse(ra8_tileatlas_memstore_pread, &store, (uint64_t)store.len, &reparsed));
  TEST_ASSERT_EQ(0, memcmp(&info, &reparsed, sizeof(info)));
  check_golden_tiles(&store, &info);
  TEST_END("produce webp: lossless -> JOF golden pixels + sized arena");
}

/**
 * @test test_webp_png_byte_identical
 * @brief The #290 convergence proof: a lossless WebP and an RGBA PNG of the
 *        same pixels produce a **byte-identical** JOF atlas -- the reader sees
 *        one normalized format regardless of source codec.
 *
 * @par MC/DC:
 * (equality of two produced byte streams; the producer's compound decisions
 * carry vectors in the sibling produce tests.)
 */
static void test_webp_png_byte_identical(void)
{
  TEST_BEGIN("produce webp: lossless WebP atlas == RGBA PNG atlas (byte-identical)");
  ra8_tileatlas_memstore_t sa = {.buf = s_store_a, .cap = sizeof(s_store_a), .len = 0U};
  ra8_tileatlas_info_t     ia = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 produce_webp(k_webp_lossless,
                              sizeof(k_webp_lossless),
                              s_webp_work,
                              sizeof(s_webp_work),
                              &sa,
                              &ia));

  build_rgba_png();
  static t_mem_pull_t pull;
  pull                                  = (t_mem_pull_t){.d = s_png, .n = s_png_len, .pos = 0U};
  ra8_tileatlas_memstore_t          sb  = {.buf = s_store_b, .cap = sizeof(s_store_b), .len = 0U};
  const ra8_tileatlas_produce_cfg_t cfg = {
    .pull       = t_mem_pull,
    .pull_ctx   = &pull,
    .sink       = ra8_tileatlas_memstore_sink,
    .sink_ctx   = &sb,
    .tile_w     = (uint16_t)k_t_tile,
    .tile_h     = (uint16_t)k_t_tile,
    .codec      = (uint8_t)k_ra8_tileatlas_codec_deflate,
    .max_width  = (uint16_t)k_t_dim,
    .max_height = (uint16_t)k_t_dim,
    .work       = s_work,
    .work_cap   = sizeof(s_work),
  };
  ra8_tileatlas_info_t ib = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tileatlas_produce(&cfg, &ib));

  /* Same reported geometry AND the same atlas bytes, end to end. */
  TEST_ASSERT_EQ(0, memcmp(&ia, &ib, sizeof(ia)));
  TEST_ASSERT_EQ(sa.len, sb.len);
  TEST_ASSERT(sa.len > 0U);
  TEST_ASSERT_EQ(0, memcmp(sa.buf, sb.buf, sa.len));
  TEST_END("produce webp: lossless WebP atlas == RGBA PNG atlas (byte-identical)");
}

/**
 * @test test_webp_lossy_path
 * @brief A lossy (VP8) WebP normalizes to a valid, reparseable JOF atlas; the
 *        whole WebP codec family reaches the one format (pixels not bit-checked,
 *        VP8 is lossy).
 *
 * @par MC/DC:
 * No compound decision under test; this drives the lossy VP8 arm end to end (it
 * is the both-false control of the transcode arena-room decision -- ample arena,
 * decode proceeds). The producer's compound decisions carry their vectors in
 * test_webp_hostile and test_webp_work_bytes.
 */
static void test_webp_lossy_path(void)
{
  TEST_BEGIN("produce webp: lossy VP8 -> valid JOF1");
  ra8_tileatlas_memstore_t store = {.buf = s_store_a, .cap = sizeof(s_store_a), .len = 0U};
  ra8_tileatlas_info_t     info  = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 produce_webp(k_webp_lossy,
                              sizeof(k_webp_lossy),
                              s_webp_work,
                              sizeof(s_webp_work),
                              &store,
                              &info));
  TEST_ASSERT_EQ(k_t_bpp, info.bpp);
  TEST_ASSERT_EQ(k_t_dim, info.width);
  TEST_ASSERT_EQ(k_t_dim, info.height);
  ra8_tileatlas_info_t reparsed = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_tileatlas_parse(ra8_tileatlas_memstore_pread, &store, (uint64_t)store.len, &reparsed));
  /* One tile pages back at the tile bpp (content is lossy, not compared). */
  uint16_t w = 0U;
  uint16_t h = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_tileatlas_read_tile(ra8_tileatlas_memstore_pread,
                                         &store,
                                         &info,
                                         0U,
                                         0U,
                                         s_scratch,
                                         (uint32_t)sizeof(s_scratch),
                                         s_cell,
                                         (uint32_t)sizeof(s_cell),
                                         &w,
                                         &h));
  TEST_ASSERT_EQ(k_t_tile, w);
  TEST_ASSERT_EQ(k_t_tile, h);
  TEST_END("produce webp: lossy VP8 -> valid JOF1");
}

/**
 * @test test_webp_hostile
 * @brief Untrusted WebP inputs fail closed with the contracted codes, never a
 *        crash or a partial atlas trusted as valid.
 *
 * @par MC/DC:
 * Decision A (WebP sniff, cond 2): `RIFF && WEBP` with RIFF true but WEBP false
 * exercises the "RIFF header that is not WEBP" arm -> the dispatch falls through
 * to `k_ra8_err_not_supported` (pairs with test_webp_lossless_golden's true
 * vector to prove cond 2 independently flips the outcome).
 *
 * Decision B (transcode arena room, in `ra8_ta_priv_webp_transcode`):
 * `if ((used >= acap) || (frame64 >= (uint64_t)(acap - used)))` (2 conditions):
 * - V1: acap large -> false (both false: the decode proceeds; covered by
 *   test_webp_lossless_golden / test_webp_png_byte_identical / lossy).
 * - V2: acap == used (k_t_webp_cap_srcfill) -> true via cond 1 `used >= acap`
 *   (the aligned source alone fills the arena; cond 2 short-circuited).
 * - V3: used < acap < used + frame (k_t_webp_cap_noframe) -> cond 1 false,
 *   cond 2 `frame64 >= acap - used` true (source fits, the frame does not).
 * V1+V2 prove cond 1's independent influence; V1+V3 prove cond 2's. N+1 = 3.
 */
static void test_webp_hostile(void)
{
  TEST_BEGIN("produce webp: hostile / unsupported WebP fail closed");
  ra8_tileatlas_memstore_t store = {.buf = s_store_a, .cap = sizeof(s_store_a), .len = 0U};
  ra8_tileatlas_info_t     info  = {};

  /* A valid WebP but no webp_work arena: WebP disabled -> not_supported. */
  TEST_ASSERT_EQ(
    k_ra8_err_not_supported,
    produce_webp(k_webp_lossless, sizeof(k_webp_lossless), nullptr, 0U, &store, &info));

  /* RIFF container that is not a WEBP form (sniff condition-2 vector). */
  uint8_t riff_junk[16] =
    {'R', 'I', 'F', 'F', 0x08U, 0U, 0U, 0U, 'X', 'Y', 'Z', 'W', 0U, 0U, 0U, 0U};
  TEST_ASSERT_EQ(
    k_ra8_err_not_supported,
    produce_webp(riff_junk, sizeof(riff_junk), s_webp_work, sizeof(s_webp_work), &store, &info));

  /* Over-cap dimension (8200 wide > 8192): rejected before any tile. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 produce_webp(k_webp_wide,
                              sizeof(k_webp_wide),
                              s_webp_work,
                              sizeof(s_webp_work),
                              &store,
                              &info));

  /* Truncated lossless bitstream: decode fails closed (no crash). */
  TEST_ASSERT(k_ra8_ok != produce_webp(k_webp_lossless,
                                       sizeof(k_webp_lossless) / 2U,
                                       s_webp_work,
                                       sizeof(s_webp_work),
                                       &store,
                                       &info));

  /* Decision B V2: the aligned source alone fills the arena (used >= acap). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 produce_webp(k_webp_lossless,
                              sizeof(k_webp_lossless),
                              s_webp_work,
                              (size_t)k_t_webp_cap_srcfill,
                              &store,
                              &info));

  /* Decision B V3: source fits but leaves no room for the frame + scratch
   * (frame64 >= acap - used) -> invalid_size. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 produce_webp(k_webp_lossless,
                              sizeof(k_webp_lossless),
                              s_webp_work,
                              (size_t)k_t_webp_cap_noframe,
                              &store,
                              &info));
  TEST_END("produce webp: hostile / unsupported WebP fail closed");
}

/**
 * @test test_webp_work_bytes
 * @brief The WebP arena calculator rejects nonsense caps and scales sanely.
 *
 * @par MC/DC:
 * Decision: `max_width == 0 || max_width > 8192 || max_height == 0 ||
 * max_height > 8192 || max_src_bytes == 0` (5 conditions)
 * - Vector 1: all in range     -> false (returns > 0)
 * - Vector 2: width 0          -> true  (varies condition 1)
 * - Vector 3: width 9000       -> true  (varies condition 2)
 * - Vector 4: height 0         -> true  (varies condition 3)
 * - Vector 5: height 9000      -> true  (varies condition 4)
 * - Vector 6: src 0            -> true  (varies condition 5)
 */
static void test_webp_work_bytes(void)
{
  TEST_BEGIN("produce webp: work-arena calculator bounds");
  TEST_ASSERT(ra8_tileatlas_webp_work_bytes(512U, 512U, 4096U) > 0U);
  TEST_ASSERT_EQ(0U, ra8_tileatlas_webp_work_bytes(0U, 512U, 4096U));
  TEST_ASSERT_EQ(0U, ra8_tileatlas_webp_work_bytes(9000U, 512U, 4096U));
  TEST_ASSERT_EQ(0U, ra8_tileatlas_webp_work_bytes(512U, 0U, 4096U));
  TEST_ASSERT_EQ(0U, ra8_tileatlas_webp_work_bytes(512U, 9000U, 4096U));
  TEST_ASSERT_EQ(0U, ra8_tileatlas_webp_work_bytes(512U, 512U, 0U));
  /* A wider frame costs strictly more arena (frame term dominates). */
  TEST_ASSERT(ra8_tileatlas_webp_work_bytes(1024U, 512U, 4096U) >
              ra8_tileatlas_webp_work_bytes(512U, 512U, 4096U));
  TEST_END("produce webp: work-arena calculator bounds");
}

/**
 * @brief Test entry point -- runs the WebP normalize-on-import suite in order.
 * @return 0 on success; unity_minimal.h exits non-zero on first failure.
 * @pre None.
 * @pre None.
 * @post All tests executed (or the process exited on first failure).
 * @post stderr carries a per-test RUN/PASS log.
 * @note Not thread-safe. No SIGALRM / timers used.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_webp_lossless_golden();
  test_webp_png_byte_identical();
  test_webp_lossy_path();
  test_webp_hostile();
  test_webp_work_bytes();
  (void)fprintf(stderr, "[OK  ] test_ra8_tileatlas_produce_webp.c\n");
  return 0;
}
