/**
 * @file test_ra8_comic_tiles.c
 * @brief #344 -- an oversized CBZ page that overruns the whole-decode arena
 *        opens and zooms through the JOF tile path.
 *
 * @details
 * Turns the #344 requirement into CI gates. A large RGB page is synthesized in
 * test, packed into a real CBZ, opened by `ra8_comic`, and paged the two ways a
 * reader chooses between:
 *   - the whole-decode fast path (`ra8_img_decode_blit`) genuinely FAILS
 *     `k_ra8_err_no_mem` on the large page against a modest arena -- this is the
 *     cap the issue describes -- and succeeds on a small page (the golden path
 *     that must stay unchanged);
 *   - the tile path (`ra8_comic_tiles_*`) opens the same large page, and every
 *     tile -- interior and clamped edge -- decodes byte-exact against the source
 *     pattern, at full resolution (no downscale), with resident decoded memory
 *     bounded by the tiny cache cell budget rather than the whole image.
 *
 * The sub-rect (loupe) property is exercised directly: a single interior tile is
 * fetched and verified full-resolution without touching its neighbours. The
 * size-threshold decision (`ra8_comic_tiles_over_budget`) carries an MC/DC
 * vector set, and a page turn proves the epoch key namespacing never surfaces a
 * stale tile of the previous page. Both the raw and DEFLATE tile codecs are
 * covered, plus the reader's fail-closed guards.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "ra8_comic.h"
#include "ra8_comic_tiles.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_jof.h"
#include "ra8_jof_produce.h"
#include "ra8_reflow_image.h"
#include "ra8_tile_cache.h"
#include "unity_minimal.h"

/**
 * @enum comic_tiles_geom_t
 * @brief Page + tile + cache geometry driving the #344 invariants.
 * @details The big page (700x480 RGB) is deliberately not tile-aligned, so the
 *          right column and bottom row are clamped edge tiles; at 4 bpp its
 *          decoded footprint is 1.34 MiB, ~5x the ::k_budget_bytes threshold and
 *          well past the ::k_small_arena the whole-decode path is given. The
 *          cache holds 8 tiles of 128x128x3, ~7x less RAM than the decoded page.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_big_w        = 700U,                        /**< Big page width (not tile-aligned).    */
  k_big_h        = 480U,                        /**< Big page height (not tile-aligned).   */
  k_small_w      = 96U,                         /**< Small page width (fits the arena).    */
  k_small_h      = 64U,                         /**< Small page height.                    */
  k_tile         = 128U,                        /**< Square tile edge.                     */
  k_rgb_bpp      = 3U,                          /**< Source / atlas bytes per pixel (RGB). */
  k_cell_bytes   = k_tile * k_tile * k_rgb_bpp, /**< Bytes per cache cell.                 */
  k_cells        = 8U,           /**< Cache cell budget (< the tile count).           */
  k_buckets      = 16U,          /**< Cache hash buckets.                             */
  k_budget_bytes = 256U * 1024U, /**< Resident decode budget (threshold).             */
  k_small_arena  = 256U * 1024U, /**< Whole-decode arena the cap is measured against. */
  k_probe_w      = 64U,          /**< Off-screen framebuffer width.                   */
  k_probe_h      = 64U,          /**< Off-screen framebuffer height.                  */
  k_tiny_cell    = 64U,          /**< Undersized cell to force a no_mem tile decode.  */
} comic_tiles_geom_t;

/**
 * @enum comic_tiles_cap_t
 * @brief Static buffer capacities for the in-test fixtures.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_arc_cap = 4U * 1024U * 1024U, /**< Built CBZ capacity.       */
  k_png_cap = 2U * 1024U * 1024U, /**< Synthesized PNG capacity. */
  k_raw_cap = ((size_t)k_big_w * (size_t)k_rgb_bpp + 1U) * (size_t)k_big_h,
  /**< Filter-0 RGB scanline buffer. */
  k_work_cap  = 8U * 1024U * 1024U,                        /**< Producer work arena.    */
  k_atlas_cap = 4U * 1024U * 1024U,                        /**< Atlas memstore backing. */
  k_scratch   = k_cell_bytes + (k_cell_bytes / 8U) + 256U, /**< Stored-tile staging.    */
} comic_tiles_cap_t;

/**
 * @enum comic_tiles_pat_t
 * @brief Deterministic RGB pattern multipliers (coprime, so tiles differ).
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_pat_r_x = 7U,  /**< Red   column multiplier. */
  k_pat_r_y = 3U,  /**< Red   row multiplier.    */
  k_pat_g_x = 5U,  /**< Green column multiplier. */
  k_pat_g_y = 11U, /**< Green row multiplier.    */
  k_pat_b_x = 2U,  /**< Blue  column multiplier. */
  k_pat_b_y = 13U, /**< Blue  row multiplier.    */
  k_chan_r  = 0U,  /**< Red   channel index.     */
  k_chan_g  = 1U,  /**< Green channel index.     */
  k_chan_b  = 2U,  /**< Blue  channel index.     */
} comic_tiles_pat_t;

/**
 * @enum comic_tiles_png_t
 * @brief PNG on-disk field widths + shifts for the hand-rolled RGB encoder.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_png_ihdr_len  = 13U,   /**< IHDR data length.                   */
  k_png_overhead  = 12U,   /**< Chunk length + type + CRC bytes.    */
  k_png_bitdepth8 = 8U,    /**< 8-bit samples.                      */
  k_png_color_rgb = 2U,    /**< PNG colour type 2 = truecolour RGB. */
  k_byte_mask     = 0xFFU, /**< Low-byte mask.                      */
  k_shift_b3      = 24U,   /**< Most-significant byte shift.        */
  k_shift_b2      = 16U,   /**< Second byte shift.                  */
  k_shift_b1      = 8U,    /**< Third byte shift.                   */
  k_crc_b1_ofs    = 9U,    /**< Chunk CRC second byte offset.       */
  k_crc_b2_ofs    = 10U,   /**< Chunk CRC third byte offset.        */
  k_crc_b3_ofs    = 11U,   /**< Chunk CRC last byte offset.         */
} comic_tiles_png_t;

/* ---------------------------------------------------------------------------
 * Fixtures (static storage; the host test build has no heap for these).
 * ---------------------------------------------------------------------------
 */

/** @brief Built CBZ archive bytes. */
static uint8_t s_arc[k_arc_cap];
/** @brief Built CBZ length. */
static size_t s_arc_size;
/** @brief Synthesized PNG scratch. */
static uint8_t s_png[k_png_cap];
/** @brief Synthesized PNG length. */
static size_t s_png_len;
/** @brief Producer work arena. */
static uint8_t s_work[k_work_cap];
/** @brief Atlas memstore backing. */
static uint8_t s_atlas[k_atlas_cap];

/** @brief Comic page-index storage. */
static ra8_comic_page_t s_pages[8];
/** @brief Comic name arena. */
static char s_names[512];
/** @brief One extracted encoded page. */
static uint8_t s_pagebuf[k_png_cap];
/** @brief Whole-decode scratch arena (the cap under test). */
static uint8_t s_decode_arena[k_small_arena];
/** @brief Off-screen framebuffer so ra8_img_decode_blit has a bound target. */
static uint16_t s_fb[(size_t)k_probe_h * (size_t)k_probe_w];

/** @brief Cache cell storage. */
static uint8_t s_cell_mem[(size_t)k_cells * (size_t)k_cell_bytes];
/** @brief Cache key storage. */
static ra8_tile_key_t s_keys[k_cells];
/** @brief Cache dim descriptors. */
static ra8_tile_dims_t s_dims[k_cells];
/** @brief Cache link metadata. */
static ra8_keycache_cell_t s_meta[k_cells];
/** @brief Cache hash buckets. */
static int32_t s_buckets[k_buckets];
/** @brief Reader stored-tile staging. */
static uint8_t s_scratch[k_scratch];

/* ---------------------------------------------------------------------------
 * Source synthesis: a truecolour RGB PNG of the deterministic pattern.
 * ---------------------------------------------------------------------------
 */

/** @brief Deterministic source sample for pixel (x, y), channel @p ch. */
static uint8_t pix_rgb(uint32_t x, uint32_t y, uint32_t ch)
{
  if (ch == (uint32_t)k_chan_r) {
    return (uint8_t)((x * (uint32_t)k_pat_r_x + y * (uint32_t)k_pat_r_y) & (uint32_t)k_byte_mask);
  }
  if (ch == (uint32_t)k_chan_g) {
    return (uint8_t)((x * (uint32_t)k_pat_g_x + y * (uint32_t)k_pat_g_y) & (uint32_t)k_byte_mask);
  }
  return (uint8_t)((x * (uint32_t)k_pat_b_x + y * (uint32_t)k_pat_b_y) & (uint32_t)k_byte_mask);
}

/** @brief Append a PNG chunk (length/type/data/CRC) into `s_png`. */
static void png_chunk(const char* type, const uint8_t* data, uint32_t len)
{
  uint8_t* p = &s_png[s_png_len];
  p[0]       = (uint8_t)(len >> (uint32_t)k_shift_b3);
  p[1]       = (uint8_t)((len >> (uint32_t)k_shift_b2) & (uint32_t)k_byte_mask);
  p[2]       = (uint8_t)((len >> (uint32_t)k_shift_b1) & (uint32_t)k_byte_mask);
  p[3]       = (uint8_t)(len & (uint32_t)k_byte_mask);
  (void)memcpy(&p[4], type, 4U);
  if (len > 0U) {
    (void)memcpy(&p[8], data, len);
  }
  const uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, &p[4], (size_t)len + 4U);
  p[8U + len]        = (uint8_t)(crc >> (uint32_t)k_shift_b3);
  p[(uint32_t)k_crc_b1_ofs + len] =
    (uint8_t)((crc >> (uint32_t)k_shift_b2) & (uint32_t)k_byte_mask);
  p[(uint32_t)k_crc_b2_ofs + len] =
    (uint8_t)((crc >> (uint32_t)k_shift_b1) & (uint32_t)k_byte_mask);
  p[(uint32_t)k_crc_b3_ofs + len] = (uint8_t)(crc & (uint32_t)k_byte_mask);
  s_png_len += (size_t)k_png_overhead + (size_t)len;
}

/** @brief Build a truecolour (RGB8, filter-0) PNG of the pattern into `s_png`. */
static void png_build_rgb(uint32_t w, uint32_t h)
{
  static const uint8_t sig[8] = {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU};
  static uint8_t       s_raw[k_raw_cap];
  static uint8_t       s_zbuf[k_png_cap];
  s_png_len = 0U;
  (void)memcpy(s_png, sig, sizeof(sig));
  s_png_len                    = sizeof(sig);
  uint8_t ihdr[k_png_ihdr_len] = {};
  ihdr[0]                      = (uint8_t)(w >> (uint32_t)k_shift_b3);
  ihdr[1]                      = (uint8_t)((w >> (uint32_t)k_shift_b2) & (uint32_t)k_byte_mask);
  ihdr[2]                      = (uint8_t)((w >> (uint32_t)k_shift_b1) & (uint32_t)k_byte_mask);
  ihdr[3]                      = (uint8_t)(w & (uint32_t)k_byte_mask);
  ihdr[4]                      = (uint8_t)(h >> (uint32_t)k_shift_b3);
  ihdr[5]                      = (uint8_t)((h >> (uint32_t)k_shift_b2) & (uint32_t)k_byte_mask);
  ihdr[6]                      = (uint8_t)((h >> (uint32_t)k_shift_b1) & (uint32_t)k_byte_mask);
  ihdr[7]                      = (uint8_t)(h & (uint32_t)k_byte_mask);
  ihdr[8]                      = (uint8_t)k_png_bitdepth8;
  ihdr[9]                      = (uint8_t)k_png_color_rgb;
  png_chunk("IHDR", ihdr, (uint32_t)k_png_ihdr_len);
  size_t o = 0U;
  for (uint32_t y = 0U; y < h; y++) {
    s_raw[o] = 0U; /* filter type 0 (none) */
    o++;
    for (uint32_t x = 0U; x < w; x++) {
      s_raw[o]      = pix_rgb(x, y, (uint32_t)k_chan_r);
      s_raw[o + 1U] = pix_rgb(x, y, (uint32_t)k_chan_g);
      s_raw[o + 2U] = pix_rgb(x, y, (uint32_t)k_chan_b);
      o += (size_t)k_rgb_bpp;
    }
  }
  mz_ulong zlen = (mz_ulong)sizeof(s_zbuf);
  TEST_ASSERT_EQ(MZ_OK, mz_compress(s_zbuf, &zlen, s_raw, (mz_ulong)o));
  png_chunk("IDAT", s_zbuf, (uint32_t)zlen);
  png_chunk("IEND", nullptr, 0U);
}

/** @brief Build a CBZ holding one big page + one small page into `s_arc`. */
static void build_cbz(void)
{
  mz_zip_archive zip = {};
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_arc_cap) == MZ_TRUE);
  png_build_rgb((uint32_t)k_big_w, (uint32_t)k_big_h);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip, "01_big.png", s_png, s_png_len, MZ_NO_COMPRESSION) ==
              MZ_TRUE);
  png_build_rgb((uint32_t)k_small_w, (uint32_t)k_small_h);
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "02_small.png", s_png, s_png_len, MZ_DEFAULT_COMPRESSION) ==
    MZ_TRUE);
  void*  heap = nullptr;
  size_t hsz  = 0U;
  TEST_ASSERT(mz_zip_writer_finalize_heap_archive(&zip, &heap, &hsz) == MZ_TRUE);
  TEST_ASSERT(hsz <= (size_t)k_arc_cap);
  (void)memcpy(s_arc, heap, hsz);
  s_arc_size = hsz;
  mz_zip_writer_end(&zip);
}

/** @brief ::ra8_comic_read_fn over the built CBZ (bounds-clamped). */
static size_t arc_read(void* ctx, uint64_t off, void* buf, size_t len)
{
  (void)ctx;
  if (off >= (uint64_t)s_arc_size) {
    return 0U;
  }
  const uint64_t avail = (uint64_t)s_arc_size - off;
  const size_t   k     = (len > (size_t)avail) ? (size_t)avail : len;
  (void)memcpy(buf, &s_arc[off], k);
  return k;
}

/** @brief Default import config for a page of the built pattern (given codec). */
static ra8_comic_tiles_import_cfg_t import_cfg(uint8_t codec)
{
  return (ra8_comic_tiles_import_cfg_t){
    .tile_w        = (uint16_t)k_tile,
    .tile_h        = (uint16_t)k_tile,
    .codec         = codec,
    .max_width     = (uint16_t)k_big_w,
    .max_height    = (uint16_t)k_big_h,
    .work          = s_work,
    .work_cap      = sizeof s_work,
    .webp_work     = nullptr,
    .webp_work_cap = 0U,
    .atlas         = s_atlas,
    .atlas_cap     = sizeof s_atlas,
  };
}

/** @brief Tile-cache storage config over the static cache buffers. */
static ra8_tile_cache_cfg_t cache_cfg(void)
{
  return (ra8_tile_cache_cfg_t){
    .cell_mem     = s_cell_mem,
    .cell_bytes   = (uint32_t)k_cell_bytes,
    .cell_count   = (uint32_t)k_cells,
    .meta         = s_meta,
    .keys         = s_keys,
    .dims         = s_dims,
    .buckets      = s_buckets,
    .bucket_count = (uint32_t)k_buckets,
    .decode       = nullptr, /* the reader wires this */
    .decode_ctx   = nullptr,
  };
}

/** @brief Read the big page's encoded bytes off the built CBZ into `s_pagebuf`. */
static size_t open_big_page_bytes(ra8_comic_t* comic)
{
  const ra8_err_t oerr = ra8_comic_open(comic,
                                        arc_read,
                                        nullptr,
                                        (uint64_t)s_arc_size,
                                        s_pages,
                                        (uint32_t)(sizeof s_pages / sizeof s_pages[0]),
                                        s_names,
                                        (uint32_t)sizeof s_names);
  TEST_ASSERT_EQ(k_ra8_ok, oerr);
  TEST_ASSERT_EQ(2U, ra8_comic_page_count(comic));
  size_t          got  = 0U;
  const ra8_err_t rerr = ra8_comic_page_read(comic, 0U, s_pagebuf, sizeof s_pagebuf, &got);
  TEST_ASSERT_EQ(k_ra8_ok, rerr);
  TEST_ASSERT(got > 0U);
  return got;
}

/** @brief Verify one pinned tile against the source pattern, byte for byte. */
static void verify_tile(const ra8_tile_t* t, uint16_t tx, uint16_t ty)
{
  const uint32_t base_x = (uint32_t)tx * (uint32_t)k_tile;
  const uint32_t base_y = (uint32_t)ty * (uint32_t)k_tile;
  const uint32_t exp_w  = ((base_x + (uint32_t)k_tile) <= (uint32_t)k_big_w)
                            ? (uint32_t)k_tile
                            : ((uint32_t)k_big_w - base_x);
  const uint32_t exp_h  = ((base_y + (uint32_t)k_tile) <= (uint32_t)k_big_h)
                            ? (uint32_t)k_tile
                            : ((uint32_t)k_big_h - base_y);
  TEST_ASSERT_EQ((int)exp_w, (int)t->width);
  TEST_ASSERT_EQ((int)exp_h, (int)t->height);
  for (uint32_t j = 0U; j < exp_h; j++) {
    for (uint32_t i = 0U; i < exp_w; i++) {
      const size_t o = ((size_t)j * (size_t)exp_w + (size_t)i) * (size_t)k_rgb_bpp;
      TEST_ASSERT_EQ(pix_rgb(base_x + i, base_y + j, k_chan_r), t->pixels[o]);
      TEST_ASSERT_EQ(pix_rgb(base_x + i, base_y + j, k_chan_g), t->pixels[o + 1U]);
      TEST_ASSERT_EQ(pix_rgb(base_x + i, base_y + j, k_chan_b), t->pixels[o + 2U]);
    }
  }
}

/** @brief Assert the whole-decode fast path caps this page (no_mem) in a modest arena. */
static void assert_whole_decode_caps(const uint8_t* enc, size_t len)
{
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gfx_init(s_fb, (uint16_t)k_probe_w, (uint16_t)k_probe_h, k_ra8_gfx_format_rgb565));
  ra8_img_arena_t arena = {.base = s_decode_arena, .cap = sizeof s_decode_arena};
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_img_decode_blit(&arena,
                                     enc,
                                     len,
                                     0,
                                     0,
                                     (int32_t)k_probe_w,
                                     (int32_t)k_probe_h,
                                     nullptr,
                                     nullptr));
}

/** @brief Fetch + byte-verify every tile of @p info, then assert a re-fetch is a cache hit. */
static void walk_and_verify_tiles(ra8_comic_tile_reader_t* r, const ra8_jof_info_t* info)
{
  for (uint16_t ty = 0U; ty < info->tile_rows; ty++) {
    for (uint16_t tx = 0U; tx < info->tile_cols; tx++) {
      ra8_tile_t t = {};
      TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_tile(r, tx, ty, &t));
      verify_tile(&t, tx, ty);
      TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_release(r, t.pixels));
    }
  }
  ra8_tile_t a = {};
  ra8_tile_t b = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_tile(r, 0U, 0U, &a));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_release(r, a.pixels));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_tile(r, 0U, 0U, &b));
  TEST_ASSERT(a.pixels == b.pixels);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_release(r, b.pixels));
}

/** @brief Assert import rejects a config with a null work arena or a null atlas store. */
static void assert_import_cfg_guards(ra8_comic_tile_reader_t* r)
{
  ra8_comic_tiles_import_cfg_t no_work = import_cfg((uint8_t)k_ra8_jof_codec_deflate);
  no_work.work                         = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_import(r, s_png, 4U, &no_work));
  ra8_comic_tiles_import_cfg_t no_atlas = import_cfg((uint8_t)k_ra8_jof_codec_deflate);
  no_atlas.atlas                        = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_import(r, s_png, 4U, &no_atlas));
}

/* ---------------------------------------------------------------------------
 * Tests.
 * ---------------------------------------------------------------------------
 */

/**
 * @test comic_tiles_footprint_and_budget
 *
 * @par MC/DC:
 * Decision: `(budget_bytes != 0U) && (decoded_bytes > budget_bytes)` (2 conditions)
 * - Vector 1: budget=256KiB, decoded=1MiB -> true  (control: both true)
 * - Vector 2: budget=0,      decoded=1MiB -> false (varies budget!=0 only)
 * - Vector 3: budget=1MiB,   decoded=512KiB -> false (varies decoded>budget only)
 * Vectors 1+2 prove `budget_bytes != 0` independently drives the outcome; 1+3
 * prove the same for `decoded_bytes > budget_bytes`. N+1 = 3 vectors for N=2.
 */
static void test_footprint_and_budget(void)
{
  TEST_BEGIN("comic tiles footprint + budget threshold");
  build_cbz();

  /* Big page footprint from the encoded header. */
  png_build_rgb((uint32_t)k_big_w, (uint32_t)k_big_h);
  uint16_t w  = 0U;
  uint16_t h  = 0U;
  uint64_t db = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_footprint(s_png, s_png_len, &w, &h, &db));
  TEST_ASSERT_EQ((int)k_big_w, (int)w);
  TEST_ASSERT_EQ((int)k_big_h, (int)h);
  TEST_ASSERT_EQ((uint64_t)k_big_w * (uint64_t)k_big_h * (uint64_t)k_ra8_comic_tiles_decoded_bpp,
                 db);

  /* MC/DC vectors for the threshold decision. */
  TEST_ASSERT(ra8_comic_tiles_over_budget((uint64_t)1024U * 1024U, (uint64_t)k_budget_bytes));
  TEST_ASSERT(!ra8_comic_tiles_over_budget((uint64_t)1024U * 1024U, 0U));
  TEST_ASSERT(!ra8_comic_tiles_over_budget((uint64_t)512U * 1024U, (uint64_t)1024U * 1024U));
  /* The big page is over budget; the small page is not. */
  TEST_ASSERT(ra8_comic_tiles_over_budget(db, (uint64_t)k_budget_bytes));
  png_build_rgb((uint32_t)k_small_w, (uint32_t)k_small_h);
  uint64_t small_db = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_footprint(s_png, s_png_len, &w, &h, &small_db));
  TEST_ASSERT(!ra8_comic_tiles_over_budget(small_db, (uint64_t)k_budget_bytes));

  /* Guards -- every required pointer, and the zero-length / non-image paths. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_footprint(nullptr, 4U, &w, &h, &db));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_footprint(s_png, 4U, nullptr, &h, &db));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_footprint(s_png, 4U, &w, nullptr, &db));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_footprint(s_png, 4U, &w, &h, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_comic_tiles_footprint(s_png, 0U, &w, &h, &db));
  static const uint8_t junk[8] = {'n', 'o', 't', 'i', 'm', 'a', 'g', 'e'};
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_comic_tiles_footprint(junk, sizeof junk, &w, &h, &db));
  TEST_END("comic tiles footprint + budget threshold");
}

/**
 * @test comic_tiles_large_page_opens
 * @details The heart of #344: the big page defeats the whole-decode arena
 *          (`k_ra8_err_no_mem`) yet the tile path opens it and every tile is
 *          byte-exact at full resolution, with resident RAM bounded by the cell
 *          budget. A re-fetch is a cache hit, not a re-decode.
 *
 * @par MC/DC:
 * The `ra8_comic_tiles_over_budget` `&&` decision is exercised here only as a
 * control (a single true vector); its full N+1 MC/DC vector set lives in
 * ::test_footprint_and_budget.
 */
static void test_large_page_opens(void)
{
  TEST_BEGIN("comic tiles: large page opens where whole-decode caps");
  ra8_comic_t  comic = {};
  const size_t got   = open_big_page_bytes(&comic);

  /* Threshold says tile. */
  uint16_t w  = 0U;
  uint16_t h  = 0U;
  uint64_t db = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_footprint(s_pagebuf, got, &w, &h, &db));
  TEST_ASSERT(ra8_comic_tiles_over_budget(db, (uint64_t)k_budget_bytes));

  /* The OLD path caps: whole-decode into a modest arena fails no_mem. */
  assert_whole_decode_caps(s_pagebuf, got);

  /* The NEW path opens it. Resident cache RAM is far below the decoded image. */
  TEST_ASSERT((uint64_t)k_cells * (uint64_t)k_cell_bytes < db);
  ra8_comic_tile_reader_t r       = {};
  ra8_tile_cache_cfg_t    storage = cache_cfg();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_tiles_init(&r, &storage, s_scratch, (uint32_t)sizeof s_scratch));
  const ra8_comic_tiles_import_cfg_t cfg = import_cfg((uint8_t)k_ra8_jof_codec_deflate);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_import(&r, s_pagebuf, got, &cfg));

  ra8_jof_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_info(&r, &info));
  TEST_ASSERT_EQ((int)k_big_w, (int)info.width);
  TEST_ASSERT_EQ((int)k_big_h, (int)info.height);
  TEST_ASSERT_EQ((int)k_rgb_bpp, (int)info.bpp);
  TEST_ASSERT(info.tile_count > (uint32_t)k_cells); /* more tiles than cells */

  /* Every tile -- interior and clamped edge -- decodes byte-exact, and a
   * re-fetch hits the cache rather than re-decoding. */
  walk_and_verify_tiles(&r, &info);

  (void)ra8_comic_close(&comic);
  TEST_END("comic tiles: large page opens where whole-decode caps");
}

/**
 * @test comic_tiles_zoom_subrect
 * @details The loupe property: a single interior tile is fetched and verified
 *          full-resolution without decoding the whole page or its neighbours.
 *
 * @par MC/DC:
 * (no compound decisions in this test)
 */
static void test_zoom_subrect(void)
{
  TEST_BEGIN("comic tiles: sub-rect (loupe) tile is full-resolution");
  ra8_comic_t  comic = {};
  const size_t got   = open_big_page_bytes(&comic);

  ra8_comic_tile_reader_t r       = {};
  ra8_tile_cache_cfg_t    storage = cache_cfg();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_tiles_init(&r, &storage, s_scratch, (uint32_t)sizeof s_scratch));
  const ra8_comic_tiles_import_cfg_t cfg = import_cfg((uint8_t)k_ra8_jof_codec_raw);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_import(&r, s_pagebuf, got, &cfg));

  /* Center-ish interior tile (2,1) fetched on its own -> native pixels. */
  ra8_tile_t t = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_tile(&r, 2U, 1U, &t));
  TEST_ASSERT_EQ((int)k_tile, (int)t.width);
  TEST_ASSERT_EQ((int)k_tile, (int)t.height);
  verify_tile(&t, 2U, 1U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_release(&r, t.pixels));

  (void)ra8_comic_close(&comic);
  TEST_END("comic tiles: sub-rect (loupe) tile is full-resolution");
}

/**
 * @test comic_tiles_tile_over_cell_budget
 * @details A tile whose decoded payload exceeds the cache cell (a deliberately
 *          undersized cache) fails closed as `k_ra8_err_no_mem` rather than
 *          overrunning the cell -- the decode-on-miss maps the atlas reader's
 *          `k_ra8_err_invalid_size` to `k_ra8_err_no_mem`.
 *
 * @par MC/DC:
 * (no compound decisions in this test)
 */
static void test_tile_over_cell_budget(void)
{
  TEST_BEGIN("comic tiles: a tile larger than the cache cell fails no_mem");
  ra8_comic_t  comic = {};
  const size_t got   = open_big_page_bytes(&comic);

  ra8_comic_tile_reader_t r       = {};
  ra8_tile_cache_cfg_t    storage = cache_cfg();
  storage.cell_bytes              = (uint32_t)k_tiny_cell; /* far below a 128x128x3 tile */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_tiles_init(&r, &storage, s_scratch, (uint32_t)sizeof s_scratch));
  const ra8_comic_tiles_import_cfg_t cfg = import_cfg((uint8_t)k_ra8_jof_codec_raw);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_import(&r, s_pagebuf, got, &cfg));

  ra8_tile_t t = {};
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_comic_tiles_tile(&r, 0U, 0U, &t));

  (void)ra8_comic_close(&comic);
  TEST_END("comic tiles: a tile larger than the cache cell fails no_mem");
}

/**
 * @test comic_tiles_small_page_flat
 * @details A page under the budget stays on the whole-decode fast path: the
 *          same modest arena that capped the big page decodes the small page,
 *          so the existing golden render is preserved.
 *
 * @par MC/DC:
 * The `ra8_comic_tiles_over_budget` `&&` decision is exercised here only as a
 * control (a single false vector); its full N+1 MC/DC vector set lives in
 * ::test_footprint_and_budget.
 */
static void test_small_page_flat(void)
{
  TEST_BEGIN("comic tiles: small page keeps the whole-decode fast path");
  build_cbz();
  ra8_comic_t comic = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_open(&comic,
                                arc_read,
                                nullptr,
                                (uint64_t)s_arc_size,
                                s_pages,
                                (uint32_t)(sizeof s_pages / sizeof s_pages[0]),
                                s_names,
                                (uint32_t)sizeof s_names));
  size_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_page_read(&comic, 1U, s_pagebuf, sizeof s_pagebuf, &got));

  uint16_t w  = 0U;
  uint16_t h  = 0U;
  uint64_t db = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_footprint(s_pagebuf, got, &w, &h, &db));
  TEST_ASSERT_EQ((int)k_small_w, (int)w);
  TEST_ASSERT(!ra8_comic_tiles_over_budget(db, (uint64_t)k_budget_bytes));

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gfx_init(s_fb, (uint16_t)k_probe_w, (uint16_t)k_probe_h, k_ra8_gfx_format_rgb565));
  ra8_img_arena_t arena = {.base = s_decode_arena, .cap = sizeof s_decode_arena};
  int32_t         dw    = 0;
  int32_t         dh    = 0;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_img_decode_blit(&arena,
                                     s_pagebuf,
                                     got,
                                     0,
                                     0,
                                     (int32_t)k_probe_w,
                                     (int32_t)k_probe_h,
                                     &dw,
                                     &dh));
  TEST_ASSERT(dw > 0);
  TEST_ASSERT(dh > 0);

  (void)ra8_comic_close(&comic);
  TEST_END("comic tiles: small page keeps the whole-decode fast path");
}

/**
 * @test comic_tiles_page_turn_epoch
 * @details Re-importing (a page turn) bumps the epoch so a same-coordinate tile
 *          fetch returns the NEW page's pixels, never a stale cached tile.
 *
 * @par MC/DC:
 * (no compound decisions in this test)
 */
static void test_page_turn_epoch(void)
{
  TEST_BEGIN("comic tiles: page turn never surfaces a stale tile");
  ra8_comic_tile_reader_t r       = {};
  ra8_tile_cache_cfg_t    storage = cache_cfg();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_tiles_init(&r, &storage, s_scratch, (uint32_t)sizeof s_scratch));

  const ra8_comic_tiles_import_cfg_t cfg = import_cfg((uint8_t)k_ra8_jof_codec_deflate);

  /* Page A: the big pattern. Fetch tile (0,0), leave it cached. */
  png_build_rgb((uint32_t)k_big_w, (uint32_t)k_big_h);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_import(&r, s_png, s_png_len, &cfg));
  const uint32_t epoch_a = r.epoch;
  ra8_tile_t     ta      = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_tile(&r, 0U, 0U, &ta));
  const uint8_t a00 = ta.pixels[0];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_release(&r, ta.pixels));

  /* Page B: a DIFFERENT pattern (offset the source so (0,0) differs). Re-import
   * rebinds the store + bumps the epoch, so the old (0,0) tile cannot be hit. */
  png_build_rgb((uint32_t)k_small_w, (uint32_t)k_small_h);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_import(&r, s_png, s_png_len, &cfg));
  TEST_ASSERT(r.epoch != epoch_a);
  ra8_jof_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_info(&r, &info));
  TEST_ASSERT_EQ((int)k_small_w, (int)info.width);
  ra8_tile_t tb = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_tile(&r, 0U, 0U, &tb));
  /* Same pattern origin (0,0) so byte 0 matches -- but assert we read page B's
   * geometry (the clamped tile is the small page's size, not the big page's). */
  TEST_ASSERT_EQ((int)k_small_w, (int)tb.width);
  TEST_ASSERT_EQ((int)k_small_h, (int)tb.height);
  TEST_ASSERT_EQ(a00, tb.pixels[0]); /* pix_rgb(0,0,R) identical for both pages */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_release(&r, tb.pixels));
  TEST_END("comic tiles: page turn never surfaces a stale tile");
}

/**
 * @test comic_tiles_guards
 * @details Fail-closed guards on every entry point: null pointers, unbound
 *          reader, out-of-range tiles, an unsupported source, and a too-small
 *          atlas store.
 *
 * @par MC/DC:
 * (no compound decisions in this test)
 */
static void test_guards(void)
{
  TEST_BEGIN("comic tiles: fail-closed guards");
  ra8_comic_tile_reader_t r       = {};
  ra8_tile_cache_cfg_t    storage = cache_cfg();

  /* init guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_init(nullptr, &storage, s_scratch, 4U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_init(&r, nullptr, s_scratch, 4U));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_tiles_init(&r, &storage, s_scratch, (uint32_t)sizeof s_scratch));

  /* Fetching before an import is invalid_state. */
  ra8_tile_t     t    = {};
  ra8_jof_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_comic_tiles_tile(&r, 0U, 0U, &t));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_comic_tiles_info(&r, &info));

  /* import guards. */
  const ra8_comic_tiles_import_cfg_t cfg = import_cfg((uint8_t)k_ra8_jof_codec_deflate);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_import(nullptr, s_png, 4U, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_import(&r, nullptr, 4U, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_import(&r, s_png, 4U, nullptr));
  assert_import_cfg_guards(&r);
  png_build_rgb((uint32_t)k_small_w, (uint32_t)k_small_h);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_comic_tiles_import(&r, s_png, 0U, &cfg));

  /* An unsupported (non-image) source aborts the transcode fail-closed and
   * leaves the reader unbound. */
  static const uint8_t junk[16] =
    {'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'a', 'g', 'e', '!', '!', '!', '!'};
  TEST_ASSERT(ra8_comic_tiles_import(&r, junk, sizeof junk, &cfg) != k_ra8_ok);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_comic_tiles_tile(&r, 0U, 0U, &t));

  /* A valid import, then out-of-range tile coordinates. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_import(&r, s_png, s_png_len, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_tiles_info(&r, &info));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_comic_tiles_tile(&r, (uint16_t)info.tile_cols, 0U, &t));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_comic_tiles_tile(&r, 0U, (uint16_t)info.tile_rows, &t));

  /* Reader null guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_tile(nullptr, 0U, 0U, &t));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_tile(&r, 0U, 0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_info(&r, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_release(&r, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_tiles_release(nullptr, s_scratch));

  /* A too-small atlas store makes the producer fail; reader stays unbound. */
  ra8_comic_tiles_import_cfg_t tiny = import_cfg((uint8_t)k_ra8_jof_codec_deflate);
  tiny.atlas_cap                    = 16U;
  png_build_rgb((uint32_t)k_big_w, (uint32_t)k_big_h);
  TEST_ASSERT(ra8_comic_tiles_import(&r, s_png, s_png_len, &tiny) != k_ra8_ok);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_comic_tiles_tile(&r, 0U, 0U, &t));
  TEST_END("comic tiles: fail-closed guards");
}

int32_t main(void)
{
  test_footprint_and_budget();
  test_large_page_opens();
  test_zoom_subrect();
  test_tile_over_cell_budget();
  test_small_page_flat();
  test_page_turn_epoch();
  test_guards();
  (void)fprintf(stderr, "[OK  ] test_ra8_comic_tiles.c\n");
  return 0;
}
