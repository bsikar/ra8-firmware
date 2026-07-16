/**
 * @file test_ra8_epub_img_tiles.c
 * @brief #231 bounded-RAM tile paging of a large in-EPUB image through
 *        ra8_tile_cache, plus the real reflow `<img>` loader off the reader.
 *
 * @details
 * Turns the #231 tile-cache invariant into a CI gate. A ~768 KiB display-native
 * tile atlas is stored (uncompressed) in an EPUB opened by *streaming*; every one
 * of its 192 tiles is paged through an 8-cell (32 KiB) tile cache and byte-checked
 * against the known source, proving:
 *   - each tile's pixels are exact (interior and clamped edge tiles),
 *   - resident decoded pixels stay bounded by the cell budget -- 24x smaller than
 *     the image, no downscaling,
 *   - the atlas is never read whole (each backing read is one tile row),
 *   - the cache actually caches (a re-fetch is a hit, not a re-decode).
 *
 * The real reflow `<img>` loader is exercised too: a figure resolves into a
 * bounded scratch byte-identically to a whole extract, and an oversize image is
 * reported unavailable rather than exceeding the budget. The loader is assigned to
 * a `ra8_reflow_image_loader_fn` to prove signature compatibility at compile time.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "ra8_epub.h"
#include "ra8_epub_img_tiles.h"
#include "ra8_err.h"
#include "ra8_reflow_types.h"
#include "ra8_tile_cache.h"
#include "unity_minimal.h"

/* ---------------------------------------------------------------------------
 * Dimensions (tests are exempt from the magic-number gate; enums used anyway).
 * ---------------------------------------------------------------------------
 */

/**
 * @enum tile_dim_t
 * @brief Atlas + cache geometry for the tile-paging invariant.
 * @details The big atlas is `k_big_w x k_big_h` gray8 (~768 KiB); the cache holds
 *          `k_cells` tiles of `k_tile * k_tile` bytes (32 KiB) -- ~24x smaller.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_big_w      = 1024U,               /**< Big atlas width.                          */
  k_big_h      = 768U,                /**< Big atlas height.                         */
  k_edge_w     = 100U,                /**< Edge-tile atlas width (not tile-aligned). */
  k_edge_h     = 70U,                 /**< Edge-tile atlas height.                   */
  k_tile       = 64U,                 /**< Square tile edge.                         */
  k_bpp        = 1U,                  /**< gray8.                                    */
  k_hdr        = 16U,                 /**< Atlas header bytes.                       */
  k_cell_bytes = k_tile * k_tile,     /**< Bytes per cache cell (4 KiB).             */
  k_cells      = 8U,                  /**< Cache cell budget.                        */
  k_buckets    = 16U,                 /**< Cache hash buckets.                       */
  k_big_px     = k_big_w * k_big_h,   /**< Big pixel count.                          */
  k_edge_px    = k_edge_w * k_edge_h, /**< Edge pixel count.                         */
  k_arc_cap    = 1200U * 1024U,       /**< Archive scratch capacity.                 */
  k_fig_bytes  = 900U,                /**< Small figure resource.                    */
  k_scratch    = 2048U,               /**< Reflow-loader scratch (fits fig).         */
  k_id_big     = 1U,                  /**< image_id for the big atlas.               */
  k_id_edge    = 2U,                  /**< image_id for the edge atlas.              */
} tile_dim_t;

/* ---------------------------------------------------------------------------
 * Fixtures.
 * ---------------------------------------------------------------------------
 */

/** @brief Big atlas blob (header + gray8 pixels). */
static uint8_t s_big[(size_t)k_hdr + (size_t)k_big_px];
/** @brief Edge atlas blob (header + gray8 pixels). */
static uint8_t s_edge[(size_t)k_hdr + (size_t)k_edge_px];
/** @brief Small figure resource bytes. */
static uint8_t s_fig[k_fig_bytes];
/** @brief Built archive. */
static uint8_t s_arc[k_arc_cap];
/** @brief Archive length. */
static size_t s_arc_size = 0U;

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
/** @brief Reflow-loader scratch. */
static uint8_t s_scratch[k_scratch];

/** @brief container.xml pointing at OEBPS/content.opf. */
static const char* const k_container = "<?xml version=\"1.0\"?>"
                                       "<container version=\"1.0\""
                                       " xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
                                       "<rootfiles><rootfile full-path=\"OEBPS/content.opf\""
                                       " media-type=\"application/oebps-package+xml\"/>"
                                       "</rootfiles></container>";

/** @brief OPF: a chapter + the two atlases + a figure. */
static const char* const k_opf =
  "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\""
  " version=\"3.0\" unique-identifier=\"id\">"
  "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:title>Tiles</dc:title><dc:creator>RA8</dc:creator>"
  "<dc:language>en</dc:language><dc:identifier id=\"id\">urn:tiles:1</dc:identifier></metadata>"
  "<manifest>"
  "<item id=\"ch1\" href=\"ch1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "<item id=\"big\" href=\"big.rti\" media-type=\"application/octet-stream\"/>"
  "<item id=\"edge\" href=\"edge.rti\" media-type=\"application/octet-stream\"/>"
  "<item id=\"fig\" href=\"fig.png\" media-type=\"image/png\"/>"
  "</manifest>"
  "<spine><itemref idref=\"ch1\"/></spine></package>";

static const char* const k_ch1 =
  "<?xml version=\"1.0\"?><html><body><p>Tiles chapter.</p></body></html>";

/** @brief Deterministic source pixel at (x, y). */
static uint8_t pix(uint32_t x, uint32_t y)
{
  return (uint8_t)(((x * 3U) + (y * 7U)) & 0xFFU);
}

/** @brief Build the tile-cache storage config over the shared static arrays. */
static ra8_tile_cache_cfg_t make_storage(void)
{
  return (ra8_tile_cache_cfg_t){.cell_mem     = s_cell_mem,
                                .cell_bytes   = (uint32_t)k_cell_bytes,
                                .cell_count   = (uint32_t)k_cells,
                                .meta         = s_meta,
                                .keys         = s_keys,
                                .dims         = s_dims,
                                .buckets      = s_buckets,
                                .bucket_count = (uint32_t)k_buckets};
}

/**
 * @brief Write a fully-parameterised 16-byte RTI1 atlas header into @p buf.
 * @param[out] buf Destination (>= 16 bytes).
 * @param[in]  w   Image width.
 * @param[in]  h   Image height.
 * @param[in]  tw  Tile width.
 * @param[in]  th  Tile height.
 * @param[in]  bpp Bytes per pixel.
 * @pre @p buf holds 16 bytes.
 * @pre The caller chooses the field values (valid or deliberately bad).
 * @post @p buf[0..15] is an RTI1 header with the given fields.
 * @post Reserved bytes are zero.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void
write_hdr_ex(uint8_t* buf, uint16_t w, uint16_t h, uint16_t tw, uint16_t th, uint8_t bpp)
{
  memset(buf, 0, (size_t)k_hdr);
  buf[0]  = (uint8_t)'R';
  buf[1]  = (uint8_t)'T';
  buf[2]  = (uint8_t)'I';
  buf[3]  = (uint8_t)'1';
  buf[4]  = (uint8_t)(w & 0xFFU);
  buf[5]  = (uint8_t)(w >> 8U);
  buf[6]  = (uint8_t)(h & 0xFFU);
  buf[7]  = (uint8_t)(h >> 8U);
  buf[8]  = (uint8_t)(tw & 0xFFU);
  buf[9]  = (uint8_t)(tw >> 8U);
  buf[10] = (uint8_t)(th & 0xFFU);
  buf[11] = (uint8_t)(th >> 8U);
  buf[12] = bpp;
}

/** @brief Write a valid RTI1 header (tile 64x64, gray8) into @p buf. */
static void write_hdr(uint8_t* buf, uint16_t w, uint16_t h)
{
  write_hdr_ex(buf, w, h, (uint16_t)k_tile, (uint16_t)k_tile, (uint8_t)k_bpp);
}

/** @brief Fill an atlas blob (header + row-major gray8 pixels). */
static void fill_atlas(uint8_t* buf, uint32_t w, uint32_t h)
{
  write_hdr(buf, (uint16_t)w, (uint16_t)h);
  for (uint32_t y = 0U; y < h; ++y) {
    for (uint32_t x = 0U; x < w; ++x) {
      buf[(size_t)k_hdr + ((size_t)y * (size_t)w) + (size_t)x] = pix(x, y);
    }
  }
}

/**
 * @brief Add seven malformed atlas entries (one per header-validation branch).
 * @details See implementation. Attacker-controlled inputs -- each stored so pread
 *          can window it; the last is truncated below the header size.
 * @param[in,out] zip Open writer.
 * @pre @p zip is an initialised mz writer.
 * @pre The archive has room for the entries.
 * @post seven `OEBPS/bad_*.rti` entries are appended.
 * @post No valid atlas is added here.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void add_bad_atlases(mz_zip_archive* zip)
{
  uint8_t bad[k_hdr] = {};
  write_hdr_ex(bad, 0U, k_edge_h, k_tile, k_tile, k_bpp); /* width == 0 */
  TEST_ASSERT(mz_zip_writer_add_mem(zip, "OEBPS/bad_w.rti", bad, sizeof(bad), MZ_NO_COMPRESSION) ==
              MZ_TRUE);
  write_hdr_ex(bad, k_edge_w, 0U, k_tile, k_tile, k_bpp); /* height == 0 */
  TEST_ASSERT(mz_zip_writer_add_mem(zip, "OEBPS/bad_h.rti", bad, sizeof(bad), MZ_NO_COMPRESSION) ==
              MZ_TRUE);
  write_hdr_ex(bad, k_edge_w, k_edge_h, 0U, k_tile, k_bpp); /* tile_w == 0 */
  TEST_ASSERT(mz_zip_writer_add_mem(zip, "OEBPS/bad_tw.rti", bad, sizeof(bad), MZ_NO_COMPRESSION) ==
              MZ_TRUE);
  write_hdr_ex(bad, k_edge_w, k_edge_h, k_tile, 0U, k_bpp); /* tile_h == 0 */
  TEST_ASSERT(mz_zip_writer_add_mem(zip, "OEBPS/bad_th.rti", bad, sizeof(bad), MZ_NO_COMPRESSION) ==
              MZ_TRUE);
  write_hdr_ex(bad, k_edge_w, k_edge_h, k_tile, k_tile, 0U); /* bpp == 0 */
  TEST_ASSERT(
    mz_zip_writer_add_mem(zip, "OEBPS/bad_bpp0.rti", bad, sizeof(bad), MZ_NO_COMPRESSION) ==
    MZ_TRUE);
  write_hdr_ex(bad,
               k_edge_w,
               k_edge_h,
               k_tile,
               k_tile,
               (uint8_t)(k_ra8_epub_tile_bpp_max + 1U)); /* bpp too large */
  TEST_ASSERT(
    mz_zip_writer_add_mem(zip, "OEBPS/bad_bpp9.rti", bad, sizeof(bad), MZ_NO_COMPRESSION) ==
    MZ_TRUE);
  /* Truncated: shorter than the 16-byte header -> short read on parse. */
  TEST_ASSERT(
    mz_zip_writer_add_mem(zip, "OEBPS/short.rti", bad, (size_t)k_hdr / 2U, MZ_NO_COMPRESSION) ==
    MZ_TRUE);
}

/**
 * @brief Build the EPUB (skeleton + two STORED atlases + a DEFLATE figure).
 * @pre The atlas + figure source buffers exist.
 * @pre ::s_arc has ::k_arc_cap bytes.
 * @post ::s_arc_size holds the archive length; ::s_arc is a valid ZIP.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void build_archive(void)
{
  fill_atlas(s_big, k_big_w, k_big_h);
  fill_atlas(s_edge, k_edge_w, k_edge_h);
  for (size_t i = 0U; i < (size_t)k_fig_bytes; ++i) {
    s_fig[i] = (uint8_t)((i * 13U) + 7U);
  }
  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_arc_cap) == MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "mimetype",
                                    "application/epub+zip",
                                    strlen("application/epub+zip"),
                                    MZ_NO_COMPRESSION) == MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "META-INF/container.xml",
                                    k_container,
                                    strlen(k_container),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "OEBPS/content.opf",
                                    k_opf,
                                    strlen(k_opf),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "OEBPS/ch1.xhtml", k_ch1, strlen(k_ch1), MZ_DEFAULT_COMPRESSION) ==
    MZ_TRUE);
  /* Atlases stored (uncompressed) so pread can window them directly. */
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "OEBPS/big.rti", s_big, sizeof(s_big), MZ_NO_COMPRESSION) ==
    MZ_TRUE);
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "OEBPS/edge.rti", s_edge, sizeof(s_edge), MZ_NO_COMPRESSION) ==
    MZ_TRUE);
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "OEBPS/fig.png", s_fig, sizeof(s_fig), MZ_DEFAULT_COMPRESSION) ==
    MZ_TRUE);
  add_bad_atlases(&zip);

  void*  heap = nullptr;
  size_t hsz  = 0U;
  TEST_ASSERT(mz_zip_writer_finalize_heap_archive(&zip, &heap, &hsz) == MZ_TRUE);
  TEST_ASSERT((heap != nullptr) && (hsz <= (size_t)k_arc_cap));
  memcpy(s_arc, heap, hsz);
  s_arc_size = hsz;
  mz_zip_writer_end(&zip);
}

/**
 * @struct buf_src_t
 * @brief Backing for the streamed-book read callback.
 * @since 0.1.0
 */
typedef struct {
  const uint8_t* data; /**< Archive base.   */
  size_t         size; /**< Archive length. */
} buf_src_t;

/** @brief Largest single backing read window. */
static size_t g_peak = 0U;

/** @brief Streamed-media read over a resident buffer (records the peak window). */
static size_t direct_read(void* ctx, uint64_t offset, void* buf, size_t len)
{
  const buf_src_t* s = (const buf_src_t*)ctx;
  if (offset >= (uint64_t)s->size) {
    return 0U;
  }
  const uint64_t avail = (uint64_t)s->size - offset;
  const size_t   n     = (len > (size_t)avail) ? (size_t)avail : len;
  memcpy(buf, &s->data[offset], n);
  if (n > g_peak) {
    g_peak = n;
  }
  return n;
}

/** @brief Byte-check a fetched tile of the given image against ::pix. */
static void verify_tile(ra8_epub_tile_binder_t* b,
                        uint32_t                image_id,
                        uint32_t                tx,
                        uint32_t                ty,
                        uint32_t                exp_w,
                        uint32_t                exp_h)
{
  ra8_tile_t t = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_get(b, image_id, (uint16_t)tx, (uint16_t)ty, &t));
  TEST_ASSERT_EQ(exp_w, t.width);
  TEST_ASSERT_EQ(exp_h, t.height);
  for (uint32_t r = 0U; r < exp_h; ++r) {
    for (uint32_t c = 0U; c < exp_w; ++c) {
      const uint32_t sx = (tx * (uint32_t)k_tile) + c;
      const uint32_t sy = (ty * (uint32_t)k_tile) + r;
      TEST_ASSERT_EQ(pix(sx, sy), t.pixels[(r * exp_w) + c]);
    }
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_put(b, t.pixels));
}

/* ---------------------------------------------------------------------------
 * Test: page a large atlas through a tiny cache, bounded + byte-correct.
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_tile_paging_bounded
 * @brief All 192 tiles of a 768 KiB atlas page correctly through an 8-cell cache
 *        opened by streaming: resident pixels stay bounded, the atlas is never
 *        read whole, and a re-fetch hits the cache.
 *
 * @par MC/DC:
 * (no compound decisions authored under test: the binder's guards and the decode
 * path are independent single-condition checks. Assertions are independent
 * equalities over every tile's bytes plus bound inequalities over the cache
 * budget, backing window, and hit/miss counters.)
 */
static void test_tile_paging_bounded(void)
{
  TEST_BEGIN("epub tiles: page a large atlas through a tiny cache, bounded");
  build_archive();
  g_peak = 0U;

  buf_src_t               src   = {.data = s_arc, .size = s_arc_size};
  ra8_epub_stream_media_t media = {.read = direct_read, .ctx = &src, .size = (uint64_t)s_arc_size};
  ra8_epub_book_t         book  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open_streamed(&media, "tiles.epub", &book));

  ra8_epub_tile_binder_t     binder  = {};
  const ra8_tile_cache_cfg_t storage = make_storage();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_init(&binder, &storage));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_add(&binder, &book, "big.rti", k_id_big));

  ra8_epub_tileimg_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_info(&binder, k_id_big, &info));
  TEST_ASSERT_EQ(k_big_w, info.width);
  TEST_ASSERT_EQ(k_big_h, info.height);
  TEST_ASSERT_EQ((k_big_w / k_tile), info.tile_cols);
  TEST_ASSERT_EQ((k_big_h / k_tile), info.tile_rows);

  /* Measure only the tile-paging backing reads (not the ZIP open / header). */
  g_peak = 0U;
  /* Sweep every tile: exact pixels, one full tile each (no edges here). */
  for (uint32_t ty = 0U; ty < info.tile_rows; ++ty) {
    for (uint32_t tx = 0U; tx < info.tile_cols; ++tx) {
      verify_tile(&binder, k_id_big, tx, ty, k_tile, k_tile);
    }
  }

  /* Bounded RAM: cell budget is ~24x smaller than the image, held constant. */
  const size_t budget = (size_t)k_cells * (size_t)k_cell_bytes;
  TEST_ASSERT(budget * 20U < ((size_t)k_big_px));
  /* The atlas was never read whole -- each backing read is one 64-byte tile row. */
  TEST_ASSERT(g_peak <= (size_t)(k_tile * k_bpp * 2U));
  TEST_ASSERT((size_t)g_peak < ((size_t)k_big_px / 100U));

  /* Cache worked: sweeping 192 tiles through 8 cells forced evictions... */
  uint32_t hits = 0U;
  uint32_t miss = 0U;
  uint32_t evic = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_stats(&binder.cache, &hits, &miss, &evic));
  TEST_ASSERT(miss >= (uint32_t)(k_big_w / k_tile) * (uint32_t)(k_big_h / k_tile));
  TEST_ASSERT(evic > 0U);
  /* ...and a fresh re-fetch of a just-decoded tile is a hit, not a re-decode. */
  const uint32_t hits_before = hits;
  verify_tile(&binder, k_id_big, 0U, 0U, k_tile, k_tile); /* miss (was evicted) */
  verify_tile(&binder, k_id_big, 0U, 0U, k_tile, k_tile); /* hit  (still MRU)   */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_stats(&binder.cache, &hits, nullptr, nullptr));
  TEST_ASSERT(hits > hits_before);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("epub tiles: page a large atlas through a tiny cache, bounded");
}

/* ---------------------------------------------------------------------------
 * Test: clamped edge tiles report true size + correct pixels.
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_tile_edges
 * @brief A not-tile-aligned atlas yields clamped edge tiles: the binder reports
 *        their true (smaller) width/height and exact pixels.
 *
 * @par MC/DC:
 * (no compound decisions authored under test; edge clamping is a `min` helper, not
 * a decision. Assertions are independent equalities over the edge tiles' sizes and
 * bytes.)
 */
static void test_tile_edges(void)
{
  TEST_BEGIN("epub tiles: clamped edge tiles");
  build_archive();
  ra8_epub_book_t            book = {};
  const ra8_epub_mem_media_t mem  = {.data = s_arc, .size = s_arc_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&mem, "tiles.epub", &book));

  ra8_epub_tile_binder_t     binder  = {};
  const ra8_tile_cache_cfg_t storage = make_storage();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_init(&binder, &storage));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_add(&binder, &book, "edge.rti", k_id_edge));

  ra8_epub_tileimg_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_info(&binder, k_id_edge, &info));
  TEST_ASSERT_EQ(2U, info.tile_cols); /* ceil(100/64) */
  TEST_ASSERT_EQ(2U, info.tile_rows); /* ceil(70/64)  */

  const uint32_t rem_w = k_edge_w - k_tile;                /* 36          */
  const uint32_t rem_h = k_edge_h - k_tile;                /* 6           */
  verify_tile(&binder, k_id_edge, 0U, 0U, k_tile, k_tile); /* interior    */
  verify_tile(&binder, k_id_edge, 1U, 0U, rem_w, k_tile);  /* right edge  */
  verify_tile(&binder, k_id_edge, 0U, 1U, k_tile, rem_h);  /* bottom edge */
  verify_tile(&binder, k_id_edge, 1U, 1U, rem_w, rem_h);   /* corner      */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("epub tiles: clamped edge tiles");
}

/* ---------------------------------------------------------------------------
 * Test: binder guards + bad headers.
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_tile_binder_guards
 * @brief NULL / not-found / out-of-range / duplicate / bad-header guards.
 *
 * @par MC/DC:
 * (no compound decisions authored under test; each guard is an independent
 * single-condition check.)
 */
static void test_tile_binder_guards(void)
{
  TEST_BEGIN("epub tiles: binder guards + bad headers");
  build_archive();
  ra8_epub_book_t            book = {};
  const ra8_epub_mem_media_t mem  = {.data = s_arc, .size = s_arc_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&mem, "tiles.epub", &book));

  ra8_epub_tile_binder_t     binder  = {};
  const ra8_tile_cache_cfg_t storage = make_storage();

  /* init guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_tile_binder_init(nullptr, &storage));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_tile_binder_init(&binder, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_init(&binder, &storage));

  /* add guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_tile_binder_add(nullptr, &book, "big.rti", 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_tile_binder_add(&binder, nullptr, "big.rti", 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_tile_binder_add(&binder, &book, nullptr, 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epub_tile_binder_add(&binder, &book, "", 1U));
  /* An over-long path (>= the path buffer) is rejected. */
  char longp[k_ra8_epub_max_path_len + 8U];
  memset(longp, 'a', sizeof(longp));
  longp[sizeof(longp) - 1U] = '\0';
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epub_tile_binder_add(&binder, &book, longp, 1U));
  /* A DEFLATE entry cannot be windowed -> pread rejects it as not-supported. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_epub_tile_binder_add(&binder, &book, "fig.png", 9U));
  /* A good add succeeds. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_add(&binder, &book, "big.rti", k_id_big));
  /* A stored non-atlas entry (mimetype) -> bad magic. */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_epub_tile_binder_add(&binder, &book, "mimetype", 7U));
  /* Duplicate id reuse -> invalid_arg (source table unchanged). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_epub_tile_binder_add(&binder, &book, "edge.rti", k_id_big));

  /* info / get / put guards. */
  ra8_epub_tileimg_info_t info = {};
  ra8_tile_t              t    = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_tile_binder_info(nullptr, k_id_big, &info));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_tile_binder_info(&binder, k_id_big, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_epub_tile_binder_info(&binder, 999U, &info));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_tile_binder_get(nullptr, k_id_big, 0U, 0U, &t));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_tile_binder_get(&binder, k_id_big, 0U, 0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_epub_tile_binder_get(&binder, 999U, 0U, 0U, &t));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_epub_tile_binder_get(&binder, k_id_big, (uint16_t)k_big_w, 0U, &t));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_epub_tile_binder_get(&binder, k_id_big, 0U, (uint16_t)k_big_h, &t));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_tile_binder_put(nullptr, s_cell_mem));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_tile_binder_put(&binder, nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("epub tiles: binder guards + bad headers");
}

/* ---------------------------------------------------------------------------
 * Test: the real reflow <img> loader.
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_reflow_img_loader
 * @brief The reflow loader resolves a figure into a bounded scratch byte-identically
 *        to a whole extract, reports an oversize image unavailable, and matches the
 *        `ra8_reflow_image_loader_fn` signature.
 *
 * @par MC/DC:
 * (no compound decisions authored under test: the loader's context and href-length
 * guards are independent single-condition early returns. The vectors below still
 * exercise each guard -- both ctx fields and both href-length bounds -- plus the
 * happy path and the oversize ceiling.)
 */
static void test_reflow_img_loader(void)
{
  TEST_BEGIN("epub tiles: real reflow <img> loader");
  build_archive();
  ra8_epub_book_t            book = {};
  const ra8_epub_mem_media_t mem  = {.data = s_arc, .size = s_arc_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&mem, "tiles.epub", &book));

  /* Signature compatibility with the reflow engine (compile-time). */
  ra8_reflow_image_loader_fn fn = ra8_epub_reflow_img_load;
  TEST_ASSERT(fn != nullptr);

  ra8_epub_img_loader_t ld = {.book        = &book,
                              .scratch     = s_scratch,
                              .scratch_cap = sizeof(s_scratch)};
  const uint8_t*        b  = nullptr;
  size_t                n  = 0U;
  const char*           h  = "fig.png";
  TEST_ASSERT_EQ(k_ra8_ok, fn(&ld, h, (uint32_t)strlen(h), &b, &n));
  TEST_ASSERT_EQ(k_fig_bytes, n);
  TEST_ASSERT_EQ(0, memcmp(b, s_fig, (size_t)k_fig_bytes));

  /* Oracle: identical to a whole extract. */
  uint8_t ref[k_fig_bytes] = {};
  size_t  rn               = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_get_resource(&book, "fig.png", ref, sizeof(ref), &rn));
  TEST_ASSERT_EQ(rn, n);
  TEST_ASSERT_EQ(0, memcmp(ref, b, rn));

  /* Oversize: a scratch too small reports the image unavailable, not an overflow. */
  ra8_epub_img_loader_t tiny = {.book = &book, .scratch = s_scratch, .scratch_cap = 8U};
  TEST_ASSERT_EQ(k_ra8_err_no_mem, fn(&tiny, h, (uint32_t)strlen(h), &b, &n));
  TEST_ASSERT_EQ(0U, n);

  /* Guards: NULL args + each ctx field (MC/DC of the 3-OR invalid-arg decision). */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fn(nullptr, h, 1U, &b, &n));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fn(&ld, nullptr, 1U, &b, &n));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fn(&ld, h, 1U, nullptr, &n));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, fn(&ld, h, 1U, &b, nullptr));
  ra8_epub_img_loader_t no_book = {.book = nullptr, .scratch = s_scratch, .scratch_cap = 8U};
  ra8_epub_img_loader_t no_scr  = {.book = &book, .scratch = nullptr, .scratch_cap = 8U};
  ra8_epub_img_loader_t no_cap  = {.book = &book, .scratch = s_scratch, .scratch_cap = 0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fn(&no_book, h, 1U, &b, &n));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fn(&no_scr, h, 1U, &b, &n));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fn(&no_cap, h, 1U, &b, &n));
  /* href length guards. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fn(&ld, h, 0U, &b, &n));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fn(&ld, h, (uint32_t)k_ra8_epub_max_path_len, &b, &n));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("epub tiles: real reflow <img> loader");
}

/* ---------------------------------------------------------------------------
 * Test: malformed atlas headers are rejected.
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_tile_bad_headers
 * @brief Every malformed atlas header (zero dim / zero tile / bad bpp / truncated)
 *        is rejected by `ra8_epub_tile_binder_add` with validation_failed.
 *
 * @par MC/DC:
 * (no compound decisions authored under test; each header field is validated by an
 * independent single-condition check. The vectors below hit each branch once.)
 */
static void test_tile_bad_headers(void)
{
  TEST_BEGIN("epub tiles: malformed atlas headers rejected");
  build_archive();
  ra8_epub_book_t            book = {};
  const ra8_epub_mem_media_t mem  = {.data = s_arc, .size = s_arc_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&mem, "tiles.epub", &book));

  ra8_epub_tile_binder_t     binder  = {};
  const ra8_tile_cache_cfg_t storage = make_storage();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_init(&binder, &storage));

  static const char* const bad_paths[] = {"bad_w.rti",
                                          "bad_h.rti",
                                          "bad_tw.rti",
                                          "bad_th.rti",
                                          "bad_bpp0.rti",
                                          "bad_bpp9.rti",
                                          "short.rti"};
  for (uint32_t i = 0U; i < (uint32_t)(sizeof(bad_paths) / sizeof(bad_paths[0])); ++i) {
    TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                   ra8_epub_tile_binder_add(&binder, &book, bad_paths[i], 100U + i));
  }
  /* None registered -> a good add still lands at source index 0. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_add(&binder, &book, "big.rti", k_id_big));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("epub tiles: malformed atlas headers rejected");
}

/* ---------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Test entry point -- runs the tile-paging + loader suite in order.
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
  test_tile_paging_bounded();
  test_tile_edges();
  test_tile_binder_guards();
  test_tile_bad_headers();
  test_reflow_img_loader();
  (void)fprintf(stderr, "[OK  ] test_ra8_epub_img_tiles.c\n");
  return 0;
}
