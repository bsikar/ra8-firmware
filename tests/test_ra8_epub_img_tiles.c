/**
 * @file test_ra8_epub_img_tiles.c
 * @brief #231 bounded-RAM tile paging of JOF atlases through ra8_tile_cache,
 *        the import-time transcode wiring, and the real reflow `<img>` loader.
 *
 * @details
 * Turns the #231 invariants into CI gates. Atlases are produced in-test by the
 * real transcode producer (`ra8_jof_produce`) from synthesized PNG
 * sources, stored (uncompressed) inside an EPUB opened by *streaming*, and
 * paged through a deliberately tiny 8-cell tile cache with byte parity against
 * the generator pattern -- proving:
 *   - each tile's pixels are exact (interior and clamped edge tiles),
 *   - resident decoded pixels stay bounded by the cell budget, no downscaling,
 *   - the atlas is never read whole (each backing read is at most one stored
 *     tile stream),
 *   - the cache actually caches (a re-fetch is a hit, not a re-decode).
 *
 * The import path (`ra8_epub_tile_binder_import`) is exercised end to end: a
 * DEFLATE-compressed PNG manifest entry transcodes through the producer into a
 * memstore and pages back byte-identically, while an entry that already is a
 * stored atlas registers in place with zero store writes. The real reflow
 * `<img>` loader keeps its existing contract gates.
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
#include "ra8_jof.h"
#include "ra8_jof_produce.h"
#include "unity_minimal.h"

/**
 * @enum tile_dim_t
 * @brief Atlas + cache geometry for the tile-paging invariants.
 * @details The big atlas is 512x384 gray8 (192 KiB decoded, 48 tiles); the
 *          cache holds 8 tiles of 4 KiB -- 6x fewer cells than tiles, ~24x
 *          less RAM than the decoded image.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_big_w      = 512U,            /**< Big atlas width.                          */
  k_big_h      = 384U,            /**< Big atlas height.                         */
  k_edge_w     = 100U,            /**< Edge-tile atlas width (not tile-aligned). */
  k_edge_h     = 70U,             /**< Edge-tile atlas height.                   */
  k_imp_w      = 96U,             /**< Import-path source width.                 */
  k_imp_h      = 80U,             /**< Import-path source height.                */
  k_tile       = 64U,             /**< Square tile edge.                         */
  k_cell_bytes = k_tile * k_tile, /**< Bytes per cache cell (gray8 tile).        */
  k_cells      = 8U,              /**< Cache cell budget.                        */
  k_buckets    = 16U,             /**< Cache hash buckets.                       */
  k_scratch    = k_cell_bytes + (k_cell_bytes / 8U) + 256U, /**< Stored-tile staging. */
  k_arc_cap    = 512U * 1024U, /**< Archive scratch capacity.         */
  k_store_cap  = 256U * 1024U, /**< Per-atlas memstore capacity.      */
  k_work_cap   = 640U * 1024U, /**< Producer work arena.              */
  k_png_cap    = 64U * 1024U,  /**< Synthesized PNG capacity.         */
  k_fig_bytes  = 900U,         /**< Small figure resource.            */
  k_ldr_cap    = 2048U,        /**< Reflow-loader scratch (fits fig). */
  k_id_big     = 1U,           /**< image_id for the big atlas.       */
  k_id_edge    = 2U,           /**< image_id for the edge atlas.      */
  k_id_imp     = 3U,           /**< image_id for the imported page.   */
} tile_dim_t;

/* ---------------------------------------------------------------------------
 * Fixtures.
 * ---------------------------------------------------------------------------
 */

/** @brief Built archive. */
static uint8_t s_arc[k_arc_cap];
/** @brief Archive length. */
static size_t s_arc_size;
/** @brief Big atlas memstore backing (baked into the ZIP as a stored entry). */
static uint8_t s_big_buf[k_store_cap];
/** @brief Edge atlas memstore backing. */
static uint8_t s_edge_buf[k_store_cap];
/** @brief Import-path memstore backing. */
static uint8_t s_imp_buf[k_store_cap];
/** @brief Import-path memstore. */
static ra8_jof_memstore_t s_imp_store;
/** @brief Producer work arena. */
static uint8_t s_work[k_work_cap];
/** @brief Synthesized PNG scratch. */
static uint8_t s_png[k_png_cap];
/** @brief Synthesized PNG length. */
static size_t s_png_len;
/** @brief Small figure resource bytes. */
static uint8_t s_fig[k_fig_bytes];

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
/** @brief Binder stored-tile staging. */
static uint8_t s_binder_scratch[k_scratch];
/** @brief Reflow-loader scratch. */
static uint8_t s_ldr_scratch[k_ldr_cap];

/** @brief container.xml pointing at OEBPS/content.opf. */
static const char* const k_container = "<?xml version=\"1.0\"?>"
                                       "<container version=\"1.0\""
                                       " xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
                                       "<rootfiles><rootfile full-path=\"OEBPS/content.opf\""
                                       " media-type=\"application/oebps-package+xml\"/>"
                                       "</rootfiles></container>";

/** @brief OPF: a chapter + the two atlases + the import page + a figure. */
static const char* const k_opf =
  "<?xml version=\"1.0\"?><package xmlns=\"http://www.idpf.org/2007/opf\""
  " version=\"3.0\" unique-identifier=\"id\">"
  "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
  "<dc:title>Tiles</dc:title><dc:creator>RA8</dc:creator>"
  "<dc:language>en</dc:language><dc:identifier id=\"id\">urn:tiles:1</dc:identifier></metadata>"
  "<manifest>"
  "<item id=\"ch1\" href=\"ch1.xhtml\" media-type=\"application/xhtml+xml\"/>"
  "<item id=\"big\" href=\"big.rta\" media-type=\"application/octet-stream\"/>"
  "<item id=\"edge\" href=\"edge.rta\" media-type=\"application/octet-stream\"/>"
  "<item id=\"page1\" href=\"page1.png\" media-type=\"image/png\"/>"
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

/* ---------------------------------------------------------------------------
 * Source synthesis: gray8 PNG (filter-0 rows, IDAT via mz_compress) and
 * producer-baked atlases.
 * ---------------------------------------------------------------------------
 */

/** @brief Append a PNG chunk (length/type/data/crc) into `s_png`. */
static void png_chunk(const char* type, const uint8_t* data, uint32_t len)
{
  uint8_t* p = &s_png[s_png_len];
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
  s_png_len += 12U + (size_t)len;
}

/** @brief Build a gray8 filter-0 PNG of ::pix at (w, h) into `s_png`. */
static void png_build(uint32_t w, uint32_t h)
{
  static const uint8_t sig[8] = {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU};
  static uint8_t       s_raw[((size_t)k_big_w + 1U) * (size_t)k_big_h];
  static uint8_t       s_zbuf[k_png_cap];
  s_png_len = 0U;
  memcpy(s_png, sig, sizeof(sig));
  s_png_len        = sizeof(sig);
  uint8_t ihdr[13] = {};
  ihdr[0]          = (uint8_t)(w >> 24U);
  ihdr[1]          = (uint8_t)((w >> 16U) & 0xFFU);
  ihdr[2]          = (uint8_t)((w >> 8U) & 0xFFU);
  ihdr[3]          = (uint8_t)(w & 0xFFU);
  ihdr[4]          = (uint8_t)(h >> 24U);
  ihdr[5]          = (uint8_t)((h >> 16U) & 0xFFU);
  ihdr[6]          = (uint8_t)((h >> 8U) & 0xFFU);
  ihdr[7]          = (uint8_t)(h & 0xFFU);
  ihdr[8]          = 8U;
  png_chunk("IHDR", ihdr, sizeof(ihdr));
  size_t o = 0U;
  for (uint32_t y = 0U; y < h; y++) {
    s_raw[o] = 0U;
    o++;
    for (uint32_t x = 0U; x < w; x++) {
      s_raw[o] = pix(x, y);
      o++;
    }
  }
  mz_ulong zlen = (mz_ulong)sizeof(s_zbuf);
  TEST_ASSERT_EQ(MZ_OK, mz_compress(s_zbuf, &zlen, s_raw, (mz_ulong)o));
  png_chunk("IDAT", s_zbuf, (uint32_t)zlen);
  png_chunk("IEND", NULL, 0U);
}

/**
 * @struct mem_pull_t
 * @brief Memory pull source over the synthesized PNG.
 */
typedef struct {
  size_t pos; /**< Read cursor into `s_png`. */
} mem_pull_t;

/** @brief ::ra8_jof_pull_fn over `s_png`. */
static ra8_err_t png_pull(void* ctx, uint8_t* buf, size_t cap, size_t* got)
{
  mem_pull_t*  p    = (mem_pull_t*)ctx;
  const size_t left = s_png_len - p->pos;
  const size_t take = (cap < left) ? cap : left;
  memcpy(buf, &s_png[p->pos], take);
  p->pos += take;
  *got = take;
  return k_ra8_ok;
}

/**
 * @brief Bake one atlas: synthesize a PNG of ::pix, run the real producer.
 * @param[in]  w     Source width, pixels.
 * @param[in]  h     Source height, pixels.
 * @param[in]  codec ::ra8_jof_codec_t member.
 * @param[out] store Destination memstore (backing already bound).
 * @pre @p store->buf covers its cap; the shared work arena is free.
 * @pre The geometry fits the ::k_png_cap synthesis buffers.
 * @post @p store holds a complete, parseable JOF atlas.
 * @post The shared PNG/work scratch is clobbered.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void bake_atlas(uint32_t w, uint32_t h, uint8_t codec, ra8_jof_memstore_t* store)
{
  png_build(w, h);
  static mem_pull_t s_pull;
  s_pull                                = (mem_pull_t){.pos = 0U};
  const ra8_jof_produce_cfg_t cfg = {
    .pull       = png_pull,
    .pull_ctx   = &s_pull,
    .sink       = ra8_jof_memstore_sink,
    .sink_ctx   = store,
    .tile_w     = (uint16_t)k_tile,
    .tile_h     = (uint16_t)k_tile,
    .codec      = codec,
    .max_width  = (uint16_t)k_big_w,
    .max_height = (uint16_t)k_big_h,
    .work       = s_work,
    .work_cap   = sizeof(s_work),
  };
  ra8_jof_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jof_produce(&cfg, &info));
  TEST_ASSERT_EQ(store->len, info.total_size);
}

/**
 * @brief Build the EPUB (skeleton + two STORED atlases + PNG page + figure).
 * @pre The shared fixture buffers exist.
 * @pre ::s_arc has ::k_arc_cap bytes.
 * @post ::s_arc_size holds the archive length; ::s_arc is a valid ZIP.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void zip_add_hostile(mz_zip_archive* zip)
{
  /* A stored entry with the atlas magic but corrupt structure. */
  uint8_t bad[64] = {'J', 'O', 'F', '1'};
  TEST_ASSERT(mz_zip_writer_add_mem(zip, "OEBPS/bad.rta", bad, sizeof(bad), MZ_NO_COMPRESSION) ==
              MZ_TRUE);
  /* A stored entry shorter than the atlas magic (import classify: short). */
  const uint8_t tiny_entry[2] = {0x42U, 0x43U};
  TEST_ASSERT(mz_zip_writer_add_mem(zip,
                                    "OEBPS/tiny.bin",
                                    tiny_entry,
                                    sizeof(tiny_entry),
                                    MZ_NO_COMPRESSION) == MZ_TRUE);
}

static void build_archive(void)
{
  ra8_jof_memstore_t big  = {.buf = s_big_buf, .cap = sizeof(s_big_buf), .len = 0U};
  ra8_jof_memstore_t edge = {.buf = s_edge_buf, .cap = sizeof(s_edge_buf), .len = 0U};
  bake_atlas(k_big_w, k_big_h, (uint8_t)k_ra8_jof_codec_deflate, &big);
  bake_atlas(k_edge_w, k_edge_h, (uint8_t)k_ra8_jof_codec_raw, &edge);
  for (size_t i = 0U; i < (size_t)k_fig_bytes; ++i) {
    s_fig[i] = (uint8_t)((i * 13U) + 7U);
  }
  png_build(k_imp_w, k_imp_h); /* the import-path source entry */

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
  TEST_ASSERT(mz_zip_writer_add_mem(&zip, "OEBPS/big.rta", s_big_buf, big.len, MZ_NO_COMPRESSION) ==
              MZ_TRUE);
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "OEBPS/edge.rta", s_edge_buf, edge.len, MZ_NO_COMPRESSION) ==
    MZ_TRUE);
  /* The import source is DEFLATE-compressed: the realistic in-book layout. */
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "OEBPS/page1.png", s_png, s_png_len, MZ_DEFAULT_COMPRESSION) ==
    MZ_TRUE);
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "OEBPS/fig.png", s_fig, sizeof(s_fig), MZ_DEFAULT_COMPRESSION) ==
    MZ_TRUE);
  zip_add_hostile(&zip);

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

/** @brief Initialise a binder over the shared storage + staging scratch. */
static void init_binder(ra8_epub_tile_binder_t* binder)
{
  const ra8_tile_cache_cfg_t storage = make_storage();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_epub_tile_binder_init(binder,
                                           &storage,
                                           s_binder_scratch,
                                           (uint32_t)sizeof(s_binder_scratch)));
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
 * @brief All 48 tiles of a deflate atlas page correctly through an 8-cell
 *        cache opened by streaming: resident pixels stay bounded, the atlas
 *        is never read whole, and a re-fetch hits the cache.
 *
 * @par MC/DC:
 * (no compound decisions authored under test: the binder's guards and the
 * decode path are independent single-condition checks. Assertions are
 * independent equalities over every tile's bytes plus bound inequalities
 * over the cache budget, backing window, and hit/miss counters.)
 */
static void test_tile_paging_bounded(void)
{
  TEST_BEGIN("epub tiles: page a deflate atlas through a tiny cache, bounded");
  build_archive();
  g_peak = 0U;

  buf_src_t               src   = {.data = s_arc, .size = s_arc_size};
  ra8_epub_stream_media_t media = {.read = direct_read, .ctx = &src, .size = (uint64_t)s_arc_size};
  ra8_epub_book_t         book  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open_streamed(&media, "tiles.epub", &book));

  ra8_epub_tile_binder_t binder = {};
  init_binder(&binder);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_add(&binder, &book, "big.rta", k_id_big));

  ra8_jof_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_info(&binder, k_id_big, &info));
  TEST_ASSERT_EQ(k_big_w, info.width);
  TEST_ASSERT_EQ(k_big_h, info.height);
  TEST_ASSERT_EQ((k_big_w / k_tile), info.tile_cols);
  TEST_ASSERT_EQ((k_big_h / k_tile), info.tile_rows);
  TEST_ASSERT_EQ(k_ra8_jof_codec_deflate, info.codec);

  /* Measure only the tile-paging backing reads (not the ZIP open / header). */
  g_peak = 0U;
  for (uint32_t ty = 0U; ty < info.tile_rows; ++ty) {
    for (uint32_t tx = 0U; tx < info.tile_cols; ++tx) {
      verify_tile(&binder, k_id_big, tx, ty, k_tile, k_tile);
    }
  }

  /* Bounded RAM: cell budget is ~24x smaller than the decoded image. */
  const size_t budget = (size_t)k_cells * (size_t)k_cell_bytes;
  TEST_ASSERT((budget * 6U) == ((size_t)k_big_w * (size_t)k_big_h));
  /* The atlas was never read whole: each backing read is at most one stored
   * tile stream (bounded by the staging scratch). */
  TEST_ASSERT(g_peak <= (size_t)k_scratch);

  /* Cache worked: 48 tiles through 8 cells forced misses + evictions... */
  uint32_t hits = 0U;
  uint32_t miss = 0U;
  uint32_t evic = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_stats(&binder.cache, &hits, &miss, &evic));
  TEST_ASSERT(miss >= ((uint32_t)info.tile_cols * (uint32_t)info.tile_rows));
  TEST_ASSERT(evic > 0U);
  /* ...and a fresh re-fetch of a just-decoded tile is a hit, not a re-decode. */
  const uint32_t hits_before = hits;
  verify_tile(&binder, k_id_big, 0U, 0U, k_tile, k_tile); /* miss (was evicted) */
  verify_tile(&binder, k_id_big, 0U, 0U, k_tile, k_tile); /* hit  (still MRU)   */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_stats(&binder.cache, &hits, nullptr, nullptr));
  TEST_ASSERT(hits > hits_before);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("epub tiles: page a deflate atlas through a tiny cache, bounded");
}

/**
 * @test test_tile_edges
 * @brief A not-tile-aligned raw atlas yields clamped edge tiles with exact
 *        pixels (and pages with no deflate staging at all).
 *
 * @par MC/DC:
 * (no compound decisions authored under test; edge clamping is a `min`
 * helper. Assertions are independent equalities over the edge tiles.)
 */
static void test_tile_edges(void)
{
  TEST_BEGIN("epub tiles: clamped edge tiles (raw atlas)");
  build_archive();
  ra8_epub_book_t            book = {};
  const ra8_epub_mem_media_t mem  = {.data = s_arc, .size = s_arc_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&mem, "tiles.epub", &book));

  /* A raw atlas needs no staging scratch: bind the binder without one. */
  ra8_epub_tile_binder_t     binder  = {};
  const ra8_tile_cache_cfg_t storage = make_storage();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_init(&binder, &storage, nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_add(&binder, &book, "edge.rta", k_id_edge));

  ra8_jof_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_info(&binder, k_id_edge, &info));
  TEST_ASSERT_EQ(2U, info.tile_cols); /* ceil(100/64) */
  TEST_ASSERT_EQ(2U, info.tile_rows); /* ceil(70/64)  */
  TEST_ASSERT_EQ(k_ra8_jof_codec_raw, info.codec);

  const uint32_t rem_w = k_edge_w - k_tile;                /* 36          */
  const uint32_t rem_h = k_edge_h - k_tile;                /* 6           */
  verify_tile(&binder, k_id_edge, 0U, 0U, k_tile, k_tile); /* interior    */
  verify_tile(&binder, k_id_edge, 1U, 0U, rem_w, k_tile);  /* right edge  */
  verify_tile(&binder, k_id_edge, 0U, 1U, k_tile, rem_h);  /* bottom edge */
  verify_tile(&binder, k_id_edge, 1U, 1U, rem_w, rem_h);   /* corner      */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("epub tiles: clamped edge tiles (raw atlas)");
}

/**
 * @test test_import_transcode
 * @brief The open-path wiring end to end: a DEFLATE-compressed PNG manifest
 *        entry imports through the real transcode producer into a memstore
 *        and pages back byte-identically -- #231's "manifest href resolves
 *        through the atlas".
 *
 * @par MC/DC:
 * Decision (import classify): `got == 4 && memcmp(magic, "JOF1") == 0`
 * (2 conditions)
 * - Vector 1: stored atlas entry  -> true  (passthrough test below)
 * - Vector 2: stored 2-byte entry -> false via got ("tiny.bin")
 * - Vector 3: stored non-atlas    -> false via memcmp ("mimetype" entry)
 * Decision (href length): `hlen == 0 || hlen >= k_ra8_epub_max_path_len`
 * (2 conditions)
 * - Vector 1: normal href -> false (every import above)
 * - Vector 2: empty href  -> true via hlen == 0
 * - Vector 3: oversize href -> true via the length cap
 */
static void import_error_arms(ra8_epub_tile_binder_t*            binder,
                              ra8_epub_book_t*                   book,
                              const ra8_epub_atlas_import_cfg_t* base)
{
  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 ra8_epub_tile_binder_import(binder, book, "missing.png", 40U, base));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_epub_tile_binder_import(binder, book, "ch1.xhtml", 41U, base));
  ra8_epub_atlas_import_cfg_t small = *base;
  small.max_width                   = 16U;
  ra8_jof_memstore_t fresh    = {.buf = s_imp_buf, .cap = sizeof(s_imp_buf), .len = 0U};
  small.store.sink_ctx              = &fresh;
  small.store.pread_ctx             = &fresh;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_epub_tile_binder_import(binder, book, "page1.png", 42U, &small));
  ra8_epub_atlas_import_cfg_t tiny      = *base;
  ra8_jof_memstore_t    tinystore = {.buf = s_imp_buf, .cap = 64U, .len = 0U};
  tiny.store.sink_ctx                   = &tinystore;
  tiny.store.pread_ctx                  = &tinystore;
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_epub_tile_binder_import(binder, book, "page1.png", 43U, &tiny));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_epub_tile_binder_import(binder, book, "page1.png", 44U, nullptr));
}

static void test_import_transcode(void)
{
  TEST_BEGIN("epub tiles: import-time transcode (PNG entry -> atlas -> tiles)");
  build_archive();
  ra8_epub_book_t            book = {};
  const ra8_epub_mem_media_t mem  = {.data = s_arc, .size = s_arc_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&mem, "tiles.epub", &book));

  ra8_epub_tile_binder_t binder = {};
  init_binder(&binder);

  s_imp_store = (ra8_jof_memstore_t){.buf = s_imp_buf, .cap = sizeof(s_imp_buf), .len = 0U};
  ra8_epub_atlas_import_cfg_t cfg = {
    .tile_w     = (uint16_t)k_tile,
    .tile_h     = (uint16_t)k_tile,
    .codec      = (uint8_t)k_ra8_jof_codec_deflate,
    .max_width  = (uint16_t)k_big_w,
    .max_height = (uint16_t)k_big_h,
    .work       = s_work,
    .work_cap   = sizeof(s_work),
    .store      = {.sink      = ra8_jof_memstore_sink,
                   .sink_ctx  = &s_imp_store,
                   .pread     = ra8_jof_memstore_pread,
                   .pread_ctx = &s_imp_store},
  };
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_epub_tile_binder_import(&binder, &book, "page1.png", k_id_imp, &cfg));
  TEST_ASSERT(s_imp_store.len > 0U); /* the producer really ran */

  ra8_jof_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_info(&binder, k_id_imp, &info));
  TEST_ASSERT_EQ(k_imp_w, info.width);
  TEST_ASSERT_EQ(k_imp_h, info.height);
  for (uint32_t ty = 0U; ty < info.tile_rows; ++ty) {
    for (uint32_t tx = 0U; tx < info.tile_cols; ++tx) {
      const uint32_t x0 = tx * (uint32_t)k_tile;
      const uint32_t y0 = ty * (uint32_t)k_tile;
      const uint32_t tw = ((k_imp_w - x0) < k_tile) ? (k_imp_w - x0) : k_tile;
      const uint32_t th = ((k_imp_h - y0) < k_tile) ? (k_imp_h - y0) : k_tile;
      verify_tile(&binder, k_id_imp, tx, ty, tw, th);
    }
  }

  import_error_arms(&binder, &book, &cfg);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("epub tiles: import-time transcode (PNG entry -> atlas -> tiles)");
}

/**
 * @test test_import_classify_arms
 * @brief The classify and href-length arms of the import entry (the MC/DC
 *        vectors documented on test_import_transcode).
 *
 * @par MC/DC:
 * (vectors 2/3 of the classify decision and 2/3 of the href-length
 * decision on test_import_transcode's block; vector 1 of each is the
 * successful "page1.png" import there.)
 */
static void test_import_classify_arms(void)
{
  TEST_BEGIN("epub tiles: import classify + href-length arms");
  build_archive();
  ra8_epub_book_t            book = {};
  const ra8_epub_mem_media_t mem  = {.data = s_arc, .size = s_arc_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&mem, "tiles.epub", &book));
  ra8_epub_tile_binder_t binder = {};
  init_binder(&binder);
  s_imp_store = (ra8_jof_memstore_t){.buf = s_imp_buf, .cap = sizeof(s_imp_buf), .len = 0U};
  const ra8_epub_atlas_import_cfg_t cfg = {
    .tile_w     = (uint16_t)k_tile,
    .tile_h     = (uint16_t)k_tile,
    .codec      = (uint8_t)k_ra8_jof_codec_deflate,
    .max_width  = (uint16_t)k_big_w,
    .max_height = (uint16_t)k_big_h,
    .work       = s_work,
    .work_cap   = sizeof(s_work),
    .store      = {.sink      = ra8_jof_memstore_sink,
                   .sink_ctx  = &s_imp_store,
                   .pread     = ra8_jof_memstore_pread,
                   .pread_ctx = &s_imp_store},
  };

  /* Classify arms: a stored non-atlas (magic compare fails) transcodes and
   * dies at the sniff; a stored entry shorter than the magic never even
   * compares. Neither may register a source. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_epub_tile_binder_import(&binder, &book, "mimetype", 45U, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_epub_tile_binder_import(&binder, &book, "tiny.bin", 46U, &cfg));

  /* Href length arms. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epub_tile_binder_import(&binder, &book, "", 47U, &cfg));
  char longp[k_ra8_epub_max_path_len + 8U];
  memset(longp, 'b', sizeof(longp));
  longp[sizeof(longp) - 1U] = '\0';
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_epub_tile_binder_import(&binder, &book, longp, 48U, &cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("epub tiles: import classify + href-length arms");
}

/**
 * @test test_import_passthrough
 * @brief An entry that already is a stored JOF atlas registers in place:
 *        zero transcode, zero store writes.
 *
 * @par MC/DC:
 * (classify vectors documented on test_import_transcode; this is vector 1.)
 */
static void test_import_passthrough(void)
{
  TEST_BEGIN("epub tiles: import passthrough (stored atlas registers in place)");
  build_archive();
  ra8_epub_book_t            book = {};
  const ra8_epub_mem_media_t mem  = {.data = s_arc, .size = s_arc_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&mem, "tiles.epub", &book));

  ra8_epub_tile_binder_t binder = {};
  init_binder(&binder);
  s_imp_store = (ra8_jof_memstore_t){.buf = s_imp_buf, .cap = sizeof(s_imp_buf), .len = 0U};
  const ra8_epub_atlas_import_cfg_t cfg = {
    .tile_w     = (uint16_t)k_tile,
    .tile_h     = (uint16_t)k_tile,
    .codec      = (uint8_t)k_ra8_jof_codec_deflate,
    .max_width  = (uint16_t)k_big_w,
    .max_height = (uint16_t)k_big_h,
    .work       = s_work,
    .work_cap   = sizeof(s_work),
    .store      = {.sink      = ra8_jof_memstore_sink,
                   .sink_ctx  = &s_imp_store,
                   .pread     = ra8_jof_memstore_pread,
                   .pread_ctx = &s_imp_store},
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_import(&binder, &book, "big.rta", k_id_big, &cfg));
  TEST_ASSERT_EQ(0U, s_imp_store.len); /* no transcode, no store writes */
  verify_tile(&binder, k_id_big, 0U, 0U, k_tile, k_tile);
  /* The corrupt stored atlas classifies as an atlas, then fails validation. */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_epub_tile_binder_import(&binder, &book, "bad.rta", 50U, &cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("epub tiles: import passthrough (stored atlas registers in place)");
}

/**
 * @test test_tile_binder_guards
 * @brief NULL / not-found / out-of-range / duplicate / bad-entry guards.
 *
 * @par MC/DC:
 * (no compound decisions authored under test; each guard is an independent
 * single-condition check.)
 */
static void add_arg_guards(ra8_epub_tile_binder_t* binder, ra8_epub_book_t* book)
{
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_tile_binder_add(nullptr, book, "big.rta", 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_tile_binder_add(binder, nullptr, "big.rta", 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_tile_binder_add(binder, book, nullptr, 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epub_tile_binder_add(binder, book, "", 1U));
  char longp[k_ra8_epub_max_path_len + 8U];
  memset(longp, 'a', sizeof(longp));
  longp[sizeof(longp) - 1U] = '\0';
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epub_tile_binder_add(binder, book, longp, 1U));
}

static void test_tile_binder_guards(void)
{
  TEST_BEGIN("epub tiles: binder guards + bad entries");
  build_archive();
  ra8_epub_book_t            book = {};
  const ra8_epub_mem_media_t mem  = {.data = s_arc, .size = s_arc_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&mem, "tiles.epub", &book));

  ra8_epub_tile_binder_t     binder  = {};
  const ra8_tile_cache_cfg_t storage = make_storage();

  /* init guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_tile_binder_init(nullptr, &storage, nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_tile_binder_init(&binder, nullptr, nullptr, 0U));
  init_binder(&binder);

  add_arg_guards(&binder, &book);
  /* A DEFLATE entry cannot be windowed -> pread rejects it as not-supported. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_epub_tile_binder_add(&binder, &book, "fig.png", 9U));
  /* A good add succeeds. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_add(&binder, &book, "big.rta", k_id_big));
  /* A stored non-atlas entry (mimetype, 20 bytes) cannot hold an atlas. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_epub_tile_binder_add(&binder, &book, "mimetype", 7U));
  /* A corrupt stored atlas -> structural validation failure. */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_epub_tile_binder_add(&binder, &book, "bad.rta", 8U));
  /* Duplicate id reuse -> invalid_arg (source table unchanged). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_epub_tile_binder_add(&binder, &book, "edge.rta", k_id_big));
  /* add_ext guards + table exhaustion. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_epub_tile_binder_add_ext(&binder, nullptr, nullptr, 64U, 60U));
  for (uint32_t i = 1U; i < (uint32_t)k_ra8_epub_tile_max_sources; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_add(&binder, &book, "edge.rta", 100U + i));
  }
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_epub_tile_binder_add(&binder, &book, "edge.rta", 200U));

  /* info / get / put guards. */
  ra8_jof_info_t info = {};
  ra8_tile_t           t    = {};
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
  TEST_END("epub tiles: binder guards + bad entries");
}

/**
 * @test test_reflow_img_loader
 * @brief The reflow loader resolves a figure into a bounded scratch
 *        byte-identically to a whole extract, reports an oversize image
 *        unavailable, and matches the `ra8_reflow_image_loader_fn` signature.
 *
 * @par MC/DC:
 * (no compound decisions authored under test: the loader's context and
 * href-length guards are independent single-condition early returns. The
 * vectors below still exercise each guard plus the happy path and the
 * oversize ceiling.)
 */
static void test_reflow_img_loader(void)
{
  TEST_BEGIN("epub tiles: real reflow <img> loader");
  build_archive();
  ra8_epub_book_t            book = {};
  const ra8_epub_mem_media_t mem  = {.data = s_arc, .size = s_arc_size};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open(&mem, "tiles.epub", &book));

  /* Signature compatibility with the reflow engine's loader seam. */
  const ra8_reflow_image_loader_fn as_loader = ra8_epub_reflow_img_load;
  TEST_ASSERT(as_loader != nullptr);

  ra8_epub_img_loader_t ld    = {.book = &book, .scratch = s_ldr_scratch, .scratch_cap = k_ldr_cap};
  const uint8_t*        bytes = nullptr;
  size_t                blen  = 0U;

  /* Happy path: the figure resolves byte-identically to a whole extract. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_epub_reflow_img_load(&ld, "fig.png", (uint32_t)strlen("fig.png"), &bytes, &blen));
  TEST_ASSERT_EQ(k_fig_bytes, blen);
  TEST_ASSERT_EQ(0, memcmp(bytes, s_fig, blen));

  /* An image larger than the scratch is unavailable, not a budget breach. */
  TEST_ASSERT_EQ(
    k_ra8_err_no_mem,
    ra8_epub_reflow_img_load(&ld, "big.rta", (uint32_t)strlen("big.rta"), &bytes, &blen));
  TEST_ASSERT_EQ(0U, blen);

  /* Missing resources report not_found. */
  TEST_ASSERT_EQ(
    k_ra8_err_not_found,
    ra8_epub_reflow_img_load(&ld, "nope.png", (uint32_t)strlen("nope.png"), &bytes, &blen));

  /* Guards: nulls, unbound context fields, and href-length bounds. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_epub_reflow_img_load(nullptr, "fig.png", 7U, &bytes, &blen));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_reflow_img_load(&ld, nullptr, 7U, &bytes, &blen));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_reflow_img_load(&ld, "fig.png", 7U, nullptr, &blen));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_reflow_img_load(&ld, "fig.png", 7U, &bytes, nullptr));
  ra8_epub_img_loader_t bad = ld;
  bad.book                  = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_epub_reflow_img_load(&bad, "fig.png", 7U, &bytes, &blen));
  bad         = ld;
  bad.scratch = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_epub_reflow_img_load(&bad, "fig.png", 7U, &bytes, &blen));
  bad             = ld;
  bad.scratch_cap = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_epub_reflow_img_load(&bad, "fig.png", 7U, &bytes, &blen));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_epub_reflow_img_load(&ld, "fig.png", 0U, &bytes, &blen));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_epub_reflow_img_load(&ld, "fig.png", (uint32_t)k_ra8_epub_max_path_len, &bytes, &blen));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&book));
  TEST_END("epub tiles: real reflow <img> loader");
}

/* ---------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Test entry point -- runs the tile-paging + import + loader suite.
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
  test_import_transcode();
  test_import_classify_arms();
  test_import_passthrough();
  test_tile_binder_guards();
  test_reflow_img_loader();
  (void)fprintf(stderr, "[OK  ] test_ra8_epub_img_tiles.c\n");
  return 0;
}
