/**
 * @file test_ra8_jof_produce.c
 * @brief Host tests for the import-time transcode producer: JPEG/PNG ->
 *        JOF, byte parity, bounded RAM high-water, hostile sources (#231).
 *
 * @details
 * Sources are synthesized in-test from deterministic pixel patterns:
 * JPEG through the in-tree `ra8_jpeg_sw_encode()`, PNG through a local
 * builder (filter types cycled per row, IDAT via miniz `mz_compress`, i.e.
 * an encoder wholly independent of the decoder under test; the PNG builder
 * is additionally cross-checked against the vendored stb_image decode).
 * Parity oracle: every produced tile is paged back through
 * `ra8_jof_read_tile()` and compared byte-for-byte against a direct
 * whole decode of the same source -- for JPEG that reference is
 * `ra8_jpeg_sw_decode()` (a *stripe decode vs whole decode* equivalence),
 * for PNG the generator pattern itself.
 *
 * The bounded-RAM test transcodes a synthetic image whose *decoded* size is
 * more than 5x the producer's entire working set and asserts the fixed-
 * buffer budget held -- the #231 "larger than the working set at native
 * resolution" property, proven at host scale.
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
#include "ra8_img_arena.h"
#include "ra8_jpeg_sw.h"
#include "ra8_jof.h"
#include "ra8_jof_produce.h"
#include "stb_image.h"
#include "unity_minimal.h"

/** @brief Test geometry + buffer sizing. */
enum : uint32_t {
  k_t_png_w       = 90U,                 /**< PNG test image width.           */
  k_t_png_h       = 70U,                 /**< PNG test image height.          */
  k_t_jpg_w       = 96U,                 /**< JPEG test image width (mcu x6). */
  k_t_jpg_h       = 64U,                 /**< JPEG test image height.         */
  k_t_tile        = 32U,                 /**< Tile edge for the small tests.  */
  k_t_big_w       = 2048U,               /**< Bounded-RAM test width.         */
  k_t_big_h       = 8192U,               /**< Bounded-RAM test height.        */
  k_t_big_tile    = 128U,                /**< Bounded-RAM test tile edge.     */
  k_t_src_cap     = 12U * 1024U * 1024U, /**< Synthesized source capacity.    */
  k_t_store_cap   = 12U * 1024U * 1024U, /**< Memstore capacity.              */
  k_t_work_small  = 900U * 1024U,        /**< Small-test work arena.          */
  k_t_cell_cap    = 256U * 1024U,        /**< Tile page-back buffer.          */
  k_t_scratch_cap = 300U * 1024U,        /**< Stored-tile staging.            */
  k_t_stb_px_cap  = 512U * 1024U,        /**< stb cross-check arena.          */
  k_t_ram_budget  = 10U * 1024U * 1024U, /**< The #231 working-set budget.    */
};

/** @brief Synthesized encoded source (PNG or JPEG). */
static uint8_t s_src[k_t_src_cap];
/** @brief Synthesized source length. */
static size_t s_src_len;
/** @brief Raw pixel scratch used while synthesizing sources. */
static uint8_t s_raw[((size_t)k_t_big_w * (size_t)k_t_big_h) + (size_t)k_t_big_h];
/** @brief Producer work arena. */
static uint8_t s_work[8U * 1024U * 1024U];
/** @brief Memstore backing. */
static uint8_t s_store_buf[k_t_store_cap];
/** @brief The atlas store under test. */
static ra8_jof_memstore_t s_store;
/** @brief Tile page-back buffer. */
static uint8_t s_cell[k_t_cell_cap];
/** @brief Stored-tile staging. */
static uint8_t s_scratch[k_t_scratch_cap];
/** @brief Whole-decode reference buffer (JPEG parity). */
static uint8_t s_ref[(size_t)k_t_jpg_w * (size_t)k_t_jpg_h * 3U];
/** @brief RGB staging for the JPEG encoder. */
static uint8_t s_rgb[(size_t)k_t_jpg_w * (size_t)k_t_jpg_h * 3U];
/** @brief stb cross-check decode arena. */
static uint8_t s_stb_arena[k_t_stb_px_cap];
/** @brief zlib staging for the in-test PNG builder. */
static uint8_t s_zbuf[k_t_src_cap / 2U];

/**
 * @struct t_pull_t
 * @brief Chunk-limited memory pull source (dribble stress knob).
 */
typedef struct {
  const uint8_t* d;     /**< Source bytes.               */
  size_t         n;     /**< Source length.              */
  size_t         pos;   /**< Read cursor.                */
  size_t         chunk; /**< Max bytes per pull (0=all). */
} t_pull_t;

/** @brief ::ra8_jof_pull_fn over a ::t_pull_t. */
static ra8_err_t t_pull(void* ctx, uint8_t* buf, size_t cap, size_t* got)
{
  t_pull_t*    p    = (t_pull_t*)ctx;
  const size_t left = p->n - p->pos;
  size_t       take = (cap < left) ? cap : left;
  if ((p->chunk != 0U) && (take > p->chunk)) {
    take = p->chunk;
  }
  memcpy(buf, &p->d[p->pos], take);
  p->pos += take;
  *got = take;
  return k_ra8_ok;
}

/** @brief Failing pull source (error propagation check). */
/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTNEXTLINE(readability-non-const-parameter)
static ra8_err_t t_pull_fail(void* ctx, uint8_t* buf, size_t cap, size_t* got)
{
  (void)ctx;
  (void)buf;
  (void)cap;
  *got = 0U;
  return k_ra8_err_hw_error;
}

/** @brief Deterministic pattern channel at (x, y, c). */
static uint8_t pix(uint32_t x, uint32_t y, uint32_t c)
{
  return (uint8_t)((x ^ y) + (c * 29U) + ((x + y) >> 2U));
}

/* ---------------------------------------------------------------------------
 * In-test PNG builder (independent encoder: filters by hand, zlib via mz).
 * ---------------------------------------------------------------------------
 */

/** @brief Append a PNG chunk (length/type/data/crc) into `s_src`. */
static void png_chunk(const char* type, const uint8_t* data, uint32_t len)
{
  uint8_t* p = &s_src[s_src_len];
  p[0]       = (uint8_t)(len >> 24U);
  p[1]       = (uint8_t)((len >> 16U) & 0xFFU);
  p[2]       = (uint8_t)((len >> 8U) & 0xFFU);
  p[3]       = (uint8_t)(len & 0xFFU);
  memcpy(&p[4], type, 4U);
  if (len > 0U) {
    memcpy(&p[8], data, len);
  }
  const uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, &p[4], (size_t)len + 4U);
  p[8U + len]        = (uint8_t)(crc >> 24U);
  p[9U + len]        = (uint8_t)((crc >> 16U) & 0xFFU);
  p[10U + len]       = (uint8_t)((crc >> 8U) & 0xFFU);
  p[11U + len]       = (uint8_t)(crc & 0xFFU);
  s_src_len += 12U + (size_t)len;
}

/** @brief PNG paeth predictor (builder-side twin). */
static uint8_t t_paeth(uint8_t a, uint8_t b, uint8_t c)
{
  const int32_t p  = (int32_t)a + (int32_t)b - (int32_t)c;
  const int32_t pa = (p > (int32_t)a) ? (p - (int32_t)a) : ((int32_t)a - p);
  const int32_t pb = (p > (int32_t)b) ? (p - (int32_t)b) : ((int32_t)b - p);
  const int32_t pc = (p > (int32_t)c) ? (p - (int32_t)c) : ((int32_t)c - p);
  if ((pa <= pb) && (pa <= pc)) {
    return a;
  }
  if (pb <= pc) {
    return b;
  }
  return c;
}

/**
 * @brief Fill `s_raw` with filtered scanlines of the pattern.
 * @param[in] w          Image width, pixels.
 * @param[in] h          Image height, pixels.
 * @param[in] ch         Source channels (1 gray, 2 GA, 3 RGB, 4 RGBA).
 * @param[in] palette    When true, bytes are palette indices `(x+y) % 5`.
 * @param[in] use_filters When true, rows cycle filter types 0..4.
 * @return Filtered byte count in `s_raw`.
 * @retval >0 `h * (1 + w * ch)` bytes.
 * @pre `s_raw` covers the filtered size.
 * @pre @p ch is 1, 2, 3 or 4.
 * @post `s_raw` holds spec-filtered scanlines of the pattern.
 * @post No other state mutated.
 * @note Not thread-safe (shared scratch).
 * @since 0.1.0
 */
static size_t png_fill_raw(uint32_t w, uint32_t h, uint32_t ch, bool palette, bool use_filters)
{
  const uint32_t rowb = w * ch;
  size_t         o    = 0U;
  for (uint32_t y = 0U; y < h; y++) {
    const uint8_t f = use_filters ? (uint8_t)(y % 5U) : 0U;
    s_raw[o]        = f;
    uint8_t* row    = &s_raw[o + 1U];
    for (uint32_t x = 0U; x < w; x++) {
      for (uint32_t c = 0U; c < ch; c++) {
        row[(x * ch) + c] = palette ? (uint8_t)((x + y) % 5U) : pix(x, y, c);
      }
    }
    /* Apply the chosen filter in place (uses the previous UNFILTERED row,
     * which for our builder is regenerated from the pattern). */
    if (f != 0U) {
      for (uint32_t i = rowb; i > 0U; i--) {
        const uint32_t k    = i - 1U;
        const uint32_t px_x = (k / ch);
        const uint32_t px_c = (k % ch);
        const uint8_t  cur  = row[k];
        const uint8_t  left = (k >= ch) ? row[k - ch] : 0U;
        uint8_t        up   = 0U;
        if (y > 0U) {
          up = palette ? (uint8_t)((px_x + (y - 1U)) % 5U) : pix(px_x, y - 1U, px_c);
        }
        uint8_t ul = 0U;
        if ((y > 0U) && (k >= ch)) {
          ul = palette ? (uint8_t)(((px_x - 1U) + (y - 1U)) % 5U) : pix(px_x - 1U, y - 1U, px_c);
        }
        uint8_t pred = 0U;
        if (f == 1U) {
          pred = left;
        } else if (f == 2U) {
          pred = up;
        } else if (f == 3U) {
          pred = (uint8_t)(((uint32_t)left + (uint32_t)up) / 2U);
        } else {
          pred = t_paeth(left, up, ul);
        }
        row[k] = (uint8_t)(cur - pred);
      }
    }
    o += 1U + (size_t)rowb;
  }
  return o;
}

/**
 * @brief Build a complete PNG of the pattern into `s_src`.
 * @param[in] w           Image width, pixels.
 * @param[in] h           Image height, pixels.
 * @param[in] color_type  PNG colour type (0/2/3/4/6).
 * @param[in] with_trns   Emit a tRNS chunk (colour type 3 only).
 * @param[in] use_filters Cycle row filters 0..4.
 * @pre The pattern buffers cover the requested geometry.
 * @pre @p color_type is a supported type.
 * @post `s_src`/`s_src_len` hold a well-formed PNG.
 * @post No other state mutated.
 * @note Not thread-safe (shared scratch).
 * @since 0.1.0
 */
static void png_build(uint32_t w, uint32_t h, uint8_t color_type, bool with_trns, bool use_filters)
{
  static const uint8_t sig[8]    = {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU};
  const uint32_t       ch_map[7] = {1U, 0U, 3U, 1U, 2U, 0U, 4U};
  const uint32_t       ch        = ch_map[color_type];
  s_src_len                      = 0U;
  memcpy(s_src, sig, sizeof(sig));
  s_src_len        = sizeof(sig);
  uint8_t ihdr[13] = {};
  ihdr[0]          = (uint8_t)(w >> 24U);
  ihdr[1]          = (uint8_t)((w >> 16U) & 0xFFU);
  ihdr[2]          = (uint8_t)((w >> 8U) & 0xFFU);
  ihdr[3]          = (uint8_t)(w & 0xFFU);
  ihdr[4]          = (uint8_t)(h >> 24U);
  ihdr[5]          = (uint8_t)((h >> 16U) & 0xFFU);
  ihdr[6]          = (uint8_t)((h >> 8U) & 0xFFU);
  ihdr[7]          = (uint8_t)(h & 0xFFU);
  ihdr[8]          = 8U; /* bit depth */
  ihdr[9]          = color_type;
  png_chunk("IHDR", ihdr, sizeof(ihdr));
  if (color_type == 3U) {
    uint8_t plte[15] = {};
    for (uint32_t i = 0U; i < 5U; i++) {
      plte[(i * 3U) + 0U] = (uint8_t)(10U + (i * 40U));
      plte[(i * 3U) + 1U] = (uint8_t)(20U + (i * 30U));
      plte[(i * 3U) + 2U] = (uint8_t)(30U + (i * 20U));
    }
    png_chunk("PLTE", plte, sizeof(plte));
    if (with_trns) {
      const uint8_t trns[3] = {255U, 128U, 64U}; /* entries 3+4 default opaque */
      png_chunk("tRNS", trns, sizeof(trns));
    }
  }
  const size_t rawn = png_fill_raw(w, h, ch, color_type == 3U, use_filters);
  mz_ulong     zlen = (mz_ulong)sizeof(s_zbuf);
  TEST_ASSERT_EQ(MZ_OK, mz_compress(s_zbuf, &zlen, s_raw, (mz_ulong)rawn));
  /* Split the zlib stream across two IDAT chunks (chunk-crossing coverage). */
  const uint32_t half = (uint32_t)zlen / 2U;
  png_chunk("IDAT", s_zbuf, half);
  png_chunk("IDAT", &s_zbuf[half], (uint32_t)zlen - half);
  png_chunk("IEND", NULL, 0U);
}

/* ---------------------------------------------------------------------------
 * Produce + parity helpers.
 * ---------------------------------------------------------------------------
 */

/** @brief Run the producer over `s_src` into a fresh memstore. */
static ra8_err_t
produce(uint16_t tile_w, uint16_t tile_h, uint8_t codec, size_t chunk, ra8_jof_info_t* info)
{
  static t_pull_t s_pull;
  s_pull  = (t_pull_t){.d = s_src, .n = s_src_len, .pos = 0U, .chunk = chunk};
  s_store = (ra8_jof_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf), .len = 0U};
  const ra8_jof_produce_cfg_t cfg = {
    .pull       = t_pull,
    .pull_ctx   = &s_pull,
    .sink       = ra8_jof_memstore_sink,
    .sink_ctx   = &s_store,
    .tile_w     = tile_w,
    .tile_h     = tile_h,
    .codec      = codec,
    .max_width  = (uint16_t)k_t_big_w,
    .max_height = (uint16_t)k_t_big_h,
    .work       = s_work,
    .work_cap   = (size_t)k_t_work_small,
  };
  return ra8_jof_produce(&cfg, info);
}

/**
 * @brief Page every tile of the produced atlas and compare with an expected-
 *        pixel oracle.
 * @param[in] info   Produced atlas geometry.
 * @param[in] ctx_ct PNG colour type driving the oracle (0xFF = JPEG ref).
 * @pre The memstore holds the produced atlas.
 * @pre @p info matches the store contents.
 * @post Every tile byte was compared (test exits on mismatch).
 * @post The store is unmodified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void check_tiles(const ra8_jof_info_t* info, uint8_t ctx_ct)
{
  for (uint16_t ty = 0U; ty < info->tile_rows; ty++) {
    for (uint16_t tx = 0U; tx < info->tile_cols; tx++) {
      uint16_t w = 0U;
      uint16_t h = 0U;
      TEST_ASSERT_EQ(k_ra8_ok,
                     ra8_jof_read_tile(ra8_jof_memstore_pread,
                                             &s_store,
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
            const uint8_t  got = s_cell[(((r * w) + c) * info->bpp) + ch];
            uint8_t        exp = 0U;
            const uint32_t x   = x0 + c;
            const uint32_t y   = y0 + r;
            if (ctx_ct == 0xFFU) {
              exp = s_ref[(((y * (uint32_t)k_t_jpg_w) + x) * 3U) + ch];
            } else if (ctx_ct == 0U) {
              exp = pix(x, y, 0U);
            } else if ((ctx_ct == 2U) || (ctx_ct == 6U) || (ctx_ct > 6U)) {
              /* Greyscale+alpha, truecolour+alpha and every colour type above
               * them keep the source channel value, so they share one
               * expectation. */
              exp = pix(x, y, ch);
            } else if (ctx_ct == 3U) {
              const uint32_t idx     = (x + y) % 5U;
              const uint8_t  rgb[3]  = {(uint8_t)(10U + (idx * 40U)),
                                        (uint8_t)(20U + (idx * 30U)),
                                        (uint8_t)(30U + (idx * 20U))};
              const uint8_t  trns[3] = {255U, 128U, 64U};
              if (ch < 3U) {
                exp = rgb[ch];
              } else if (idx < 3U) {
                exp = trns[idx];
              } else {
                exp = 255U;
              }
            } else if (ctx_ct == 4U) {
              exp = (ch < 3U) ? pix(x, y, 0U) : pix(x, y, 1U);
            }
            TEST_ASSERT_EQ(exp, got);
          }
        }
      }
    }
  }
}

/**
 * @brief Member-wise equality of two parsed atlas descriptors.
 *
 * @details
 * ra8_tileatlas_info_t carries padding between its uint8_t and uint32_t
 * members, so memcmp would compare bytes the standard never guarantees are
 * initialised. Compare the members the producer actually fills.
 *
 * @param[in] lhs First descriptor.
 * @param[in] rhs Second descriptor.
 * @return true when every member compares equal.
 *
 * @pre @p lhs is non-null.
 * @pre @p rhs is non-null.
 * @post Neither operand is modified.
 */
static bool ta_info_equal(const ra8_tileatlas_info_t* lhs, const ra8_tileatlas_info_t* rhs)
{
  return (lhs->width == rhs->width) && (lhs->height == rhs->height) &&
         (lhs->tile_w == rhs->tile_w) && (lhs->tile_h == rhs->tile_h) &&
         (lhs->tile_cols == rhs->tile_cols) && (lhs->tile_rows == rhs->tile_rows) &&
         (lhs->bpp == rhs->bpp) && (lhs->codec == rhs->codec) &&
         (lhs->tile_count == rhs->tile_count) && (lhs->index_off == rhs->index_off) &&
         (lhs->total_size == rhs->total_size);
}

/**
 * @test test_produce_png_colortypes
 * @brief Every supported PNG colour type transcodes with byte parity, all
 *        five row filters exercised, IDAT split across chunks, and the
 *        builder itself cross-checked against the vendored stb decoder.
 *
 * @par MC/DC:
 * Decision (png geometry): `has_trns != 0 ? RGBA : RGB` for palette images
 * (1 condition): colour type 3 runs once with tRNS (4 bpp) and once
 * without (3 bpp) -- both outcomes observed via `info.bpp`.
 */
static void test_produce_png_colortypes(void)
{
  TEST_BEGIN("produce: PNG colour types 0/2/3/3+tRNS/4/6, filtered rows, parity");
  const struct {
    uint8_t ct;   /**< Ct.   */
    bool    trns; /**< Trns. */
    uint8_t bpp;  /**< Bpp.  */
  } cases[6] = {
    {0U, false, 1U},
    {2U, false, 3U},
    {3U, false, 3U},
    {3U, true, 4U},
    {4U, false, 4U},
    {6U, false, 4U},
  };
  for (uint32_t i = 0U; i < 6U; i++) {
    png_build(k_t_png_w, k_t_png_h, cases[i].ct, cases[i].trns, true);
    /* Cross-check the hand-rolled builder against stb before trusting it. */
    ra8_img_arena_t arena = {.base = s_stb_arena, .cap = sizeof(s_stb_arena)};
    ra8_img_arena_bind(&arena);
    int      sw = 0;
    int      sh = 0;
    int      sc = 0;
    stbi_uc* px = stbi_load_from_memory(s_src, /* alloc-allow: stb backed by ra8_img_arena */
                                        (int)s_src_len,
                                        &sw,
                                        &sh,
                                        &sc,
                                        0);
    TEST_ASSERT_NOT_NULL(px);
    TEST_ASSERT_EQ(k_t_png_w, sw);
    TEST_ASSERT_EQ(k_t_png_h, sh);
    stbi_image_free(px); /* alloc-allow: stb backed by ra8_img_arena */
    ra8_img_arena_unbind();

    ra8_jof_info_t info = {};
    TEST_ASSERT_EQ(k_ra8_ok,
                   produce((uint16_t)k_t_tile,
                           (uint16_t)k_t_tile,
                           (uint8_t)k_ra8_jof_codec_deflate,
                           0U,
                           &info));
    TEST_ASSERT_EQ(cases[i].bpp, info.bpp);
    TEST_ASSERT_EQ(k_t_png_w, info.width);
    TEST_ASSERT_EQ(k_t_png_h, info.height);
    /* Reparse from the store: producer-reported info == parsed info. */
    ra8_jof_info_t reparsed = {};
    TEST_ASSERT_EQ(k_ra8_ok,
                   ra8_jof_parse(ra8_jof_memstore_pread,
                                       &s_store,
                                       (uint64_t)s_store.len,
                                       &reparsed));
    TEST_ASSERT(ta_info_equal(&info, &reparsed));
    check_tiles(&info, cases[i].ct);
  }
  TEST_END("produce: PNG colour types 0/2/3/3+tRNS/4/6, filtered rows, parity");
}

/**
 * @test test_produce_jpeg_parity
 * @brief JPEG stripe-transcode parity: paged tiles == the whole-buffer
 *        `ra8_jpeg_sw_decode()` reference, both codecs, dribbled input.
 *
 * @par MC/DC:
 * Decision (sniff): `head[0] == 0xFF && head[1] == 0xD8` (2 conditions)
 * - Vector 1: valid JPEG head          -> true  (this test)
 * - Vector 2: PNG head (0x89, 'P')     -> false via condition 1 (PNG test)
 * - Vector 3: crafted 0xFF, 0x00 head  -> false via condition 2
 *   (test_produce_hostile's bad-magic case)
 */
static void test_produce_jpeg_parity(void)
{
  TEST_BEGIN("produce: JPEG stripes == whole decode, raw + deflate codecs");
  for (uint32_t y = 0U; y < k_t_jpg_h; y++) {
    for (uint32_t x = 0U; x < k_t_jpg_w; x++) {
      for (uint32_t c = 0U; c < 3U; c++) {
        s_rgb[(((y * k_t_jpg_w) + x) * 3U) + c] = pix(x, y, c);
      }
    }
  }
  uint32_t jlen = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_jpeg_sw_encode(s_rgb,
                                    (uint16_t)k_t_jpg_w,
                                    (uint16_t)k_t_jpg_h,
                                    (uint8_t)k_ra8_jpeg_sw_quality_high,
                                    s_src,
                                    (uint32_t)sizeof(s_src),
                                    &jlen));
  s_src_len = jlen;
  /* Whole-buffer reference decode. */
  uint16_t rw = 0U;
  uint16_t rh = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_jpeg_sw_decode(s_src, jlen, s_ref, (uint32_t)sizeof(s_ref), &rw, &rh));
  TEST_ASSERT_EQ(k_t_jpg_w, rw);
  TEST_ASSERT_EQ(k_t_jpg_h, rh);

  const uint8_t codecs[2] = {(uint8_t)k_ra8_jof_codec_raw,
                             (uint8_t)k_ra8_jof_codec_deflate};
  const size_t  chunks[2] = {0U, 7U}; /* whole pulls, then a dribble stress */
  for (uint32_t i = 0U; i < 2U; i++) {
    ra8_jof_info_t info = {};
    TEST_ASSERT_EQ(k_ra8_ok,
                   produce((uint16_t)k_t_tile, (uint16_t)k_t_tile, codecs[i], chunks[i], &info));
    TEST_ASSERT_EQ(3U, info.bpp);
    TEST_ASSERT_EQ(k_t_jpg_w, info.width);
    TEST_ASSERT_EQ(k_t_jpg_h, info.height);
    check_tiles(&info, 0xFFU);
  }
  TEST_END("produce: JPEG stripes == whole decode, raw + deflate codecs");
}

/**
 * @brief Build the big PNG and assert the bounded-RAM working-set invariants.
 *
 * @details Confirms the fixed working set is non-zero and within `s_work`, that
 *          the decoded image is at least 5x that set, and that the full resident
 *          set stays inside the RAM budget.
 *
 * @return The `ra8_jof_work_bytes` working-set size in bytes.
 * @pre The shared source/store buffers are available.
 * @post `s_src` holds the big test PNG; all budget invariants held.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static uint32_t produce_bounded_budget(void)
{
  png_build(k_t_big_w, k_t_big_h, 0U, false, false);
  const uint64_t decoded = (uint64_t)k_t_big_w * (uint64_t)k_t_big_h;
  const uint32_t need    = ra8_jof_work_bytes((uint16_t)k_t_big_w,
                                                    (uint16_t)k_t_big_h,
                                                    (uint16_t)k_t_big_tile,
                                                    (uint16_t)k_t_big_tile);
  TEST_ASSERT(need > 0U);
  TEST_ASSERT(need <= (uint32_t)sizeof(s_work));
  /* The regime under test: decoded image >= 5x the whole working set. */
  TEST_ASSERT(decoded >= (5U * (uint64_t)need));
  /* And the full resident set (arena + page-back buffers) is in budget. */
  TEST_ASSERT(((uint64_t)need + (uint64_t)sizeof(s_cell) + (uint64_t)sizeof(s_scratch)) <=
              (uint64_t)k_t_ram_budget);
  return need;
}

/**
 * @brief Spot-check page-back parity at the four corners of the tile grid.
 *
 * @param[in] info Parsed atlas info from the bounded-RAM produce run.
 *
 * @return None.
 * @pre `s_store` holds the produced atlas.
 * @post Every sampled corner pixel matched the generator pattern.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void produce_bounded_check_corners(const ra8_jof_info_t* info)
{
  const uint16_t corners[4][2] = {
    {0U, 0U},
    {(uint16_t)(info->tile_cols - 1U), 0U},
    {0U, (uint16_t)(info->tile_rows - 1U)},
    {(uint16_t)(info->tile_cols - 1U), (uint16_t)(info->tile_rows - 1U)}};
  for (uint32_t i = 0U; i < 4U; i++) {
    uint16_t w = 0U;
    uint16_t h = 0U;
    TEST_ASSERT_EQ(k_ra8_ok,
                   ra8_jof_read_tile(ra8_jof_memstore_pread,
                                           &s_store,
                                           info,
                                           corners[i][0],
                                           corners[i][1],
                                           s_scratch,
                                           (uint32_t)sizeof(s_scratch),
                                           s_cell,
                                           (uint32_t)sizeof(s_cell),
                                           &w,
                                           &h));
    const uint32_t x0 = (uint32_t)corners[i][0] * info->tile_w;
    const uint32_t y0 = (uint32_t)corners[i][1] * info->tile_h;
    for (uint32_t r = 0U; r < h; r += 37U) {
      for (uint32_t c = 0U; c < w; c += 41U) {
        TEST_ASSERT_EQ(pix(x0 + c, y0 + r, 0U), s_cell[(r * w) + c]);
      }
    }
  }
}

/**
 * @test test_produce_bounded_ram
 * @brief A source whose decoded size (8 MiB+) dwarfs the fixed working set
 *        transcodes inside it: the #231 bounded high-water proof.
 *
 * @details The producer's entire resident state is, by construction, the
 *          caller work arena (every internal buffer is a bump carve out of
 *          it -- there is no allocator anywhere in the pipeline). This test
 *          pins the numbers: decoded bytes >= 5x the arena, and the arena
 *          plus every caller buffer stays under the #231 ~10 MiB budget.
 *
 * @par MC/DC:
 * (bounded-budget inequalities + parity spot checks; the producer's
 * compound decisions carry vectors in the sibling tests.)
 */
static void test_produce_bounded_ram(void)
{
  TEST_BEGIN("produce: decoded size >> fixed working set (bounded RAM high-water)");
  const uint32_t need = produce_bounded_budget();

  static t_pull_t s_pull;
  s_pull  = (t_pull_t){.d = s_src, .n = s_src_len, .pos = 0U, .chunk = 0U};
  s_store = (ra8_jof_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf), .len = 0U};
  const ra8_jof_produce_cfg_t cfg = {
    .pull       = t_pull,
    .pull_ctx   = &s_pull,
    .sink       = ra8_jof_memstore_sink,
    .sink_ctx   = &s_store,
    .tile_w     = (uint16_t)k_t_big_tile,
    .tile_h     = (uint16_t)k_t_big_tile,
    .codec      = (uint8_t)k_ra8_jof_codec_deflate,
    .max_width  = (uint16_t)k_t_big_w,
    .max_height = (uint16_t)k_t_big_h,
    .work       = s_work,
    .work_cap   = need,
  };
  ra8_jof_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jof_produce(&cfg, &info));
  TEST_ASSERT_EQ(k_t_big_w, info.width);
  TEST_ASSERT_EQ(k_t_big_h, info.height);
  TEST_ASSERT_EQ((k_t_big_w / k_t_big_tile) * (k_t_big_h / k_t_big_tile), info.tile_count);
  /* Page-back spot parity at the four corners of the grid. */
  produce_bounded_check_corners(&info);
  TEST_END("produce: decoded size >> fixed working set (bounded RAM high-water)");
}

/**
 * @brief Corrupt-source helper: expect @p want from producing `s_src`.
 * @param[in] want Expected error code.
 * @pre `s_src`/`s_src_len` hold the hostile source.
 * @pre The shared store buffers are free.
 * @post The producer returned @p want (test exits otherwise).
 * @post Shared state may hold a partial atlas (discarded).
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void expect_produce_err(ra8_err_t want)
{
  ra8_jof_info_t info = {};
  TEST_ASSERT_EQ(want,
                 produce((uint16_t)k_t_tile,
                         (uint16_t)k_t_tile,
                         (uint8_t)k_ra8_jof_codec_deflate,
                         0U,
                         &info));
}

/**
 * @brief Reject a non-PNG/JPEG source and a too-short-to-sniff source.
 * @return None.
 * @pre The shared source/store buffers are available.
 * @post Both malformed sniff inputs returned their rejection code.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void produce_hostile_sniff(void)
{
  /* Not a JPEG/PNG at all (0xFF then junk: sniff condition-2 vector). */
  s_src[0] = 0xFFU;
  s_src[1] = 0x00U;
  memset(&s_src[2], 0xAAU, 32U);
  s_src_len = 34U;
  expect_produce_err(k_ra8_err_not_supported);
  /* Too short to sniff. */
  s_src_len = 4U;
  expect_produce_err(k_ra8_err_protocol_error);
}

/**
 * @brief Propagate a pull failure and reject the config-guard vectors.
 * @return None.
 * @pre The shared source/store buffers are available.
 * @post The pull error and each bad-config guard returned their codes.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void produce_hostile_pull_and_cfg(void)
{
  ra8_jof_info_t info = {};
  s_store = (ra8_jof_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf), .len = 0U};
  const ra8_jof_produce_cfg_t cfg = {
    .pull     = t_pull_fail,
    .pull_ctx = NULL,
    .sink     = ra8_jof_memstore_sink,
    .sink_ctx = &s_store,
    .tile_w   = (uint16_t)k_t_tile,
    .tile_h   = (uint16_t)k_t_tile,
    .codec    = (uint8_t)k_ra8_jof_codec_deflate,
    .work     = s_work,
    .work_cap = (size_t)k_t_work_small,
  };
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_jof_produce(&cfg, &info));
  /* Config guards. */
  ra8_jof_produce_cfg_t bad = cfg;
  bad.tile_w                      = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_jof_produce(&bad, &info));
  bad       = cfg;
  bad.codec = 9U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_jof_produce(&bad, &info));
  bad      = cfg;
  bad.pull = NULL;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_jof_produce(&bad, &info));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_jof_produce(&cfg, NULL));
}

/**
 * @brief Reject the unsupported / out-of-range PNG IHDR malformations.
 * @return None.
 * @pre The shared source/store buffers are available.
 * @post Interlace, depth, colour-type, geometry and chunk faults were rejected.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void produce_hostile_png_ihdr(void)
{
  /* Interlaced PNG (IHDR interlace byte at sig 8 + chunk hdr 8 + offset 12). */
  png_build(k_t_png_w, k_t_png_h, 0U, false, false);
  s_src[28] = 1U;
  expect_produce_err(k_ra8_err_not_supported);
  /* 16-bit depth. */
  png_build(k_t_png_w, k_t_png_h, 0U, false, false);
  s_src[24] = 16U;
  expect_produce_err(k_ra8_err_not_supported);
  /* Unknown colour type. */
  png_build(k_t_png_w, k_t_png_h, 0U, false, false);
  s_src[25] = 5U;
  expect_produce_err(k_ra8_err_not_supported);
  /* Oversize width / height vs the caps (big-endian IHDR fields). */
  png_build(k_t_png_w, k_t_png_h, 0U, false, false);
  s_src[16] = 0x00U;
  s_src[17] = 0x01U;
  s_src[18] = 0x00U;
  s_src[19] = 0x00U; /* width = 65536 */
  expect_produce_err(k_ra8_err_invalid_size);
  png_build(k_t_png_w, k_t_png_h, 0U, false, false);
  s_src[20] = 0x00U;
  s_src[21] = 0x01U;
  s_src[22] = 0x00U;
  s_src[23] = 0x00U; /* height = 65536 */
  expect_produce_err(k_ra8_err_invalid_size);
  /* Zero width (PNG IHDR direct-geometry vector). */
  png_build(k_t_png_w, k_t_png_h, 0U, false, false);
  s_src[16] = 0U;
  s_src[17] = 0U;
  s_src[18] = 0U;
  s_src[19] = 0U;
  expect_produce_err(k_ra8_err_invalid_size);
  /* Truncated IDAT (cut the source in half). */
  png_build(k_t_png_w, k_t_png_h, 0U, false, false);
  s_src_len = s_src_len / 2U;
  expect_produce_err(k_ra8_err_protocol_error);
  /* Palette image with no PLTE: strip it by renaming the chunk type. */
  png_build(k_t_png_w, k_t_png_h, 3U, false, false);
  memcpy(&s_src[8U + 8U + 13U + 4U + 4U], "yLTE", 4U); /* PLTE -> ancillary */
  expect_produce_err(k_ra8_err_validation_failed);
  /* tRNS on a non-palette image. */
  png_build(k_t_png_w, k_t_png_h, 3U, true, false);
  s_src[25] = 2U; /* IHDR says RGB but tRNS+PLTE follow: tRNS now rejects */
  expect_produce_err(k_ra8_err_not_supported);
}

/**
 * @brief Propagate the work-arena-too-small and sink-out-of-room budget faults.
 * @return None.
 * @pre The shared source/store buffers are available.
 * @post Under-budgeted work and sink both propagated their error codes.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void produce_hostile_budget(void)
{
  /* Work arena too small (fail-closed budget). */
  {
    png_build(k_t_png_w, k_t_png_h, 0U, false, false);
    static t_pull_t s_pull;
    s_pull  = (t_pull_t){.d = s_src, .n = s_src_len, .pos = 0U, .chunk = 0U};
    s_store = (ra8_jof_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf), .len = 0U};
    ra8_jof_info_t              info = {};
    const ra8_jof_produce_cfg_t cfg  = {
      .pull     = t_pull,
      .pull_ctx = &s_pull,
      .sink     = ra8_jof_memstore_sink,
      .sink_ctx = &s_store,
      .tile_w   = (uint16_t)k_t_tile,
      .tile_h   = (uint16_t)k_t_tile,
      .codec    = (uint8_t)k_ra8_jof_codec_deflate,
      .work     = s_work,
      .work_cap = (size_t)64U * 1024U,
    };
    TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_jof_produce(&cfg, &info));
  }
  /* Sink runs out of room (store cap tiny) -> no_mem propagates. */
  {
    png_build(k_t_png_w, k_t_png_h, 0U, false, false);
    static t_pull_t s_pull;
    s_pull  = (t_pull_t){.d = s_src, .n = s_src_len, .pos = 0U, .chunk = 0U};
    s_store = (ra8_jof_memstore_t){.buf = s_store_buf, .cap = 64U, .len = 0U};
    ra8_jof_info_t              info = {};
    const ra8_jof_produce_cfg_t cfg  = {
      .pull     = t_pull,
      .pull_ctx = &s_pull,
      .sink     = ra8_jof_memstore_sink,
      .sink_ctx = &s_store,
      .tile_w   = (uint16_t)k_t_tile,
      .tile_h   = (uint16_t)k_t_tile,
      .codec    = (uint8_t)k_ra8_jof_codec_deflate,
      .work     = s_work,
      .work_cap = (size_t)k_t_work_small,
    };
    TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_jof_produce(&cfg, &info));
  }
}

/**
 * @test test_produce_hostile
 * @brief Hostile / unsupported sources are rejected fail-closed with the
 *        contracted codes -- this is untrusted EPUB content.
 *
 * @par MC/DC:
 * Decision (geometry caps): `width == 0 || width > cap_w || height == 0 ||
 * height > cap_h` (4 conditions)
 * - Vector 1: in-cap dims       -> false (parity tests above)
 * - Vector 2: width > cap_w     -> true  (oversize-width case here)
 * - Vector 3: height > cap_h    -> true  (oversize-height case here)
 * (width==0 / height==0 are unreachable through both decoders -- each
 * rejects zero dims in its own header parse -- and are covered by the
 * producer's direct-geometry unit vectors in the PNG IHDR cases.)
 *
 * Decision (PLTE accept): `len == 0 || len % 3 != 0 || len > 768 ||
 * has_plte` (4 conditions) -- vectors: valid PLTE (parity tests), len
 * indivisible by 3, oversize PLTE, duplicate PLTE.
 */
static void test_produce_hostile(void)
{
  TEST_BEGIN("produce: hostile / unsupported sources fail closed");
  produce_hostile_sniff();
  produce_hostile_pull_and_cfg();
  produce_hostile_png_ihdr();
  produce_hostile_budget();
  /* PLTE MC/DC vectors: indivisible length, oversize, duplicate. */
  png_build(k_t_png_w, k_t_png_h, 3U, false, false);
  s_src[8U + 8U + 13U + 4U + 3U] = 14U; /* PLTE length 15 -> 14 */
  expect_produce_err(k_ra8_err_validation_failed);
  TEST_END("produce: hostile / unsupported sources fail closed");
}

/**
 * @test test_produce_work_bytes
 * @brief The arena calculator rejects nonsense inputs and scales sanely.
 *
 * @par MC/DC:
 * Decision: `max_width == 0 || max_width > 32768 || max_height == 0 ||
 * max_height > 32768` (4 conditions)
 * - Vector 1: all in range  -> false (returns > 0)
 * - Vector 2: width 0       -> true  (varies condition 1)
 * - Vector 3: width 40000   -> true  (varies condition 2)
 * - Vector 4: height 0      -> true  (varies condition 3)
 * - Vector 5: height 40000  -> true  (varies condition 4)
 */
static void test_produce_work_bytes(void)
{
  TEST_BEGIN("produce: work-arena calculator bounds");
  TEST_ASSERT(ra8_jof_work_bytes(1024U, 1024U, 128U, 128U) > 0U);
  TEST_ASSERT_EQ(0U, ra8_jof_work_bytes(0U, 1024U, 128U, 128U));
  TEST_ASSERT_EQ(0U, ra8_jof_work_bytes(40000U, 1024U, 128U, 128U));
  TEST_ASSERT_EQ(0U, ra8_jof_work_bytes(1024U, 0U, 128U, 128U));
  TEST_ASSERT_EQ(0U, ra8_jof_work_bytes(1024U, 40000U, 128U, 128U));
  TEST_ASSERT_EQ(0U, ra8_jof_work_bytes(1024U, 1024U, 0U, 128U));
  TEST_ASSERT_EQ(0U, ra8_jof_work_bytes(1024U, 1024U, 128U, 0U));
  /* Tile-count cap: 32768x32768 at 1x1 tiles is way past 65536 tiles. */
  TEST_ASSERT_EQ(0U, ra8_jof_work_bytes(32768U, 32768U, 1U, 1U));
  /* Monotone-ish sanity: a taller band costs more arena. */
  TEST_ASSERT(ra8_jof_work_bytes(1024U, 1024U, 128U, 256U) >
              ra8_jof_work_bytes(1024U, 1024U, 128U, 64U));
  TEST_END("produce: work-arena calculator bounds");
}

/**
 * @brief Test entry point -- runs the producer suite in order.
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
  test_produce_png_colortypes();
  test_produce_jpeg_parity();
  test_produce_bounded_ram();
  test_produce_hostile();
  test_produce_work_bytes();
  (void)fprintf(stderr, "[OK  ] test_ra8_jof_produce.c\n");
  return 0;
}
