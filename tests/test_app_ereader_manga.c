/**
 * @file test_app_ereader_manga.c
 * @brief Host twin of examples/ek_ra8d2/hw_pending/ereader_manga: the viewable
 *        pan/zoom manga reader (Demo B) over the same baked page fixture.
 *
 * @details
 * Runs the app's exact presentation pipeline against the app's exact fixture:
 * transcode the baked 1536x2048 grayscale PNG through `ra8_jof_produce`
 * into a JOF atlas, page it through a small `ra8_tile_cache`, and render the
 * initial 1:1 top-left viewport with the shared `mg_reader` into a 1024x600
 * RGB565 framebuffer. Every stage is a deterministic integer pipeline, so the
 * framebuffer FNV asserted here IS the `hil.conf` banner crc: this test is the
 * golden the ra8_emulator / silicon run is compared against.
 *
 * The reader's pure navigation logic (tap-zone hit-test + pan/zoom mutation) is
 * unit-tested directly, and one decoded tile is byte-checked against the
 * generator's page pattern so the committed fixture and the reader cannot drift
 * apart silently. The app's `src/mg_reader.c` is compiled into this target (see
 * tests/CMakeLists.txt), so the tested render IS the production render.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../examples/ek_ra8d2/hw_pending/ereader_manga/inc/mg_reader.h"
#include "../examples/ek_ra8d2/hw_pending/ereader_manga/mg_page_fixture.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_jof.h"
#include "ra8_jof_produce.h"
#include "ra8_log.h"
#include "ra8_tile_cache.h"
#include "unity_minimal.h"

/**
 * @enum app_ereader_manga_fixture_t
 * @brief Buffer capacities and payload sizes.
 */
typedef enum : uint8_t {
  k_manga_line_cap = 48, /**< Capacity of the status-line scratch buffer. */
} app_ereader_manga_fixture_t;

/** @brief The app's geometry + budget constants (kept in lockstep with main.c).
 */
enum : uint32_t {
  k_t_fb_w       = 1024U,       /**< Panel framebuffer width, pixels.  */
  k_t_fb_h       = 600U,        /**< Panel framebuffer height, pixels. */
  k_t_tile_edge  = 256U,        /**< Import + cache tile edge, pixels. */
  k_t_cap_edge   = 2048U,       /**< Produce budget cap, pixels.       */
  k_t_cell_bytes = 256U * 256U, /**< Cache cell = one gray8 256 tile.  */
  /* Cache budget DERIVED from the viewport geometry, in lockstep with main.c's
   * k_mg_cells (#338): the 1:1 frame's tile span (ceil(extent/tile) + 1 for the
   * viewport not being tile-aligned) plus one tile of pan margin on each axis.
   */
  k_t_view_cols = ((k_t_fb_w + k_t_tile_edge - 1U) / k_t_tile_edge) + 1U, /**< 1:1 cols. */
  /** 1:1 content rows. */
  k_t_view_rows =
    (((k_t_fb_h - (uint32_t)k_mg_statusbar_h) + k_t_tile_edge - 1U) / k_t_tile_edge) + 1U,
  /** Frame + pan margin. */
  k_t_cells            = (k_t_view_cols + 1U) * (k_t_view_rows + 1U),
  k_t_stress_cells     = 4U,                 /**< Old sub-frame budget: eviction-stress case. */
  k_t_buckets          = 64U,                /**< Cache hash buckets (>= cells).              */
  k_t_pan_cycles       = 4U,                 /**< right/left oscillations in the thrash test. */
  k_t_thrash_min_ratio = 4U,                 /**< Min stress/sized decode-count ratio proved. */
  k_t_scratch          = 96U * 1024U,        /**< read_tile deflate staging.                  */
  k_t_work             = 3203832U,           /**< Producer work arena.                        */
  k_t_store            = 4U * 1024U * 1024U, /**< Atlas memstore.                             */
  k_t_image_id         = 1U,                 /**< Tile-cache key image id.                    */
  k_t_pan_step         = 256U,               /**< One edge-tap viewport slide.                */
  k_t_crc_hex_width    = 8U,                 /**< Printed producer-golden CRC width.          */
};

/** @brief Tile-grid coordinates the pan-prefetch test asserts on. */
enum : uint16_t {
  k_t_lead_col = 5U, /**< Column warmed one tile ahead of a rightward pan. */
};

/** @brief JOF bytes-per-pixel values exercised by the bpp guard test. */
enum : uint8_t {
  k_t_gray8_bpp  = 1U, /**< gray8: the only format the reader blits.       */
  k_t_colour_bpp = 3U, /**< RGB888: a colour atlas the reader must reject. */
};

/**
 * @brief Golden banner numbers (must match the app's UART banner + hil.conf).
 */
enum : uint32_t {
  k_t_page_w             = 1536U,       /**< Baked page width.          */
  k_t_page_h             = 2048U,       /**< Baked page height.         */
  k_t_tiles_want         = 48U,         /**< 6x8 tile grid.             */
  k_t_golden_atlas_bytes = 20384U,      /**< Expected atlas total_size. */
  k_t_golden_crc         = 0x5CBD900BU, /**< Expected framebuffer FNV.  */
};

/** @brief Expected tile-fill grays (must match gen_manga_page_fixture.py). */
enum : uint8_t {
  k_t_gray_c0r0 = 60U, /**< tile_gray(0,0) = 60 + 0.    */
  k_t_gray_c5r7 = 92U, /**< tile_gray(5,7) = 60 + 2*16. */
  k_t_gray_edge = 0U,  /**< Black inner tile frame.     */
};

/** @brief RGB565 panel framebuffer (the app's render target twin). */
static uint16_t s_fb[(size_t)k_t_fb_h * (size_t)k_t_fb_w];
/** @brief Tile-cache cell storage. */
static uint8_t s_cell_mem[(size_t)k_t_cells * (size_t)k_t_cell_bytes];
/** @brief Tile-cache keys. */
static ra8_tile_key_t s_keys[k_t_cells];
/** @brief Tile-cache dims. */
static ra8_tile_dims_t s_dims[k_t_cells];
/** @brief Tile-cache metadata. */
static ra8_keycache_cell_t s_meta[k_t_cells];
/** @brief Tile-cache buckets. */
static int32_t s_buckets[k_t_buckets];
/** @brief read_tile deflate staging. */
static uint8_t s_scratch[k_t_scratch];
/** @brief Producer work arena. */
static uint8_t s_work[k_t_work];
/** @brief Atlas memstore backing. */
static uint8_t s_store_buf[k_t_store];

/** @brief Eviction-stress cell storage (the old 4-cell budget). */
static uint8_t s_stress_cell_mem[(size_t)k_t_stress_cells * (size_t)k_t_cell_bytes];
/** @brief Eviction-stress cache keys. */
static ra8_tile_key_t s_stress_keys[k_t_stress_cells];
/** @brief Eviction-stress cache dims. */
static ra8_tile_dims_t s_stress_dims[k_t_stress_cells];
/** @brief Eviction-stress cache metadata. */
static ra8_keycache_cell_t s_stress_meta[k_t_stress_cells];
/** @brief Eviction-stress cache buckets. */
static int32_t s_stress_buckets[k_t_buckets];

/** @brief The atlas memstore. */
static ra8_jof_memstore_t s_store;
/** @brief Parsed atlas geometry. */
static ra8_jof_info_t s_info;
/** @brief Tile cache paging the atlas. */
static ra8_tile_cache_t s_cache;
/** @brief Decode-on-miss context. */
static mg_tile_src_t s_tile_src;
/** @brief The reader under test. */
static mg_reader_t s_reader;
/** @brief Decoded-tile probe buffer (off-stack: one full 256x256 gray8 tile).
 */
static uint8_t s_probe_cell[k_t_cell_bytes];

/**
 * @brief Report derived producer-work arena dimensions.
 * @details Composes the fixed page cap, tile edge, required work bytes, and
 * available fixture arena through a caller-local diagnostic sink.
 * @param[in] need Producer work capacity required for this fixture.
 * @pre ::s_work retains the caller-owned producer arena storage.
 * @pre @p need is the value returned by the production work-capacity query.
 * @post One complete arena-information line has been attempted on descriptor 2.
 * @post No descriptor is closed and no process-global sink is installed.
 * @note Destination failure does not alter the test verdict.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_report_arena(uint32_t need)
{
  ra8_test_output_t    output = {};
  ra8_test_output_fd_t state  = {};
  (void)internal_test_output_fd_init(&output, &state, STDERR_FILENO);
  (void)internal_test_output_text(&output, "[INFO] ereader-manga arena: cap=");
  (void)internal_test_output_u64(&output, (uint64_t)k_t_cap_edge);
  (void)internal_test_output_text(&output, "x");
  (void)internal_test_output_u64(&output, (uint64_t)k_t_cap_edge);
  (void)internal_test_output_text(&output, " tile=");
  (void)internal_test_output_u64(&output, (uint64_t)k_t_tile_edge);
  (void)internal_test_output_text(&output, " need=");
  (void)internal_test_output_u64(&output, need);
  (void)internal_test_output_text(&output, " have=");
  (void)internal_test_output_u64(&output, sizeof(s_work));
  (void)internal_test_output_text(&output, "\n");
}

/**
 * @brief Report the production manga render golden fields.
 * @details Composes parsed page geometry, tile count, atlas size, and the
 * fixed-width framebuffer CRC used by the hardware banner comparison.
 * @param[in] crc Observed framebuffer checksum.
 * @pre ::s_info contains the successfully parsed production atlas geometry.
 * @pre @p crc was computed after the complete reference render.
 * @post One complete golden-information line has been attempted on descriptor 2.
 * @post The parsed atlas and framebuffer remain unchanged.
 * @note Destination failure does not alter the asserted golden values.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_report_golden(uint32_t crc)
{
  ra8_test_output_t    output = {};
  ra8_test_output_fd_t state  = {};
  (void)internal_test_output_fd_init(&output, &state, STDERR_FILENO);
  (void)internal_test_output_text(&output, "[INFO] ereader-manga golden: page ");
  (void)internal_test_output_u64(&output, s_info.width);
  (void)internal_test_output_text(&output, "x");
  (void)internal_test_output_u64(&output, s_info.height);
  (void)internal_test_output_text(&output, " tiles=");
  (void)internal_test_output_u64(&output, s_info.tile_count);
  (void)internal_test_output_text(&output, " atlas=");
  (void)internal_test_output_u64(&output, s_info.total_size);
  (void)internal_test_output_text(&output, " crc=");
  (void)internal_test_output_hex64(&output, crc, (uint8_t)k_t_crc_hex_width, true);
  (void)internal_test_output_text(&output, "\n");
}

/**
 * @brief Report both cache budgets' pan-thrash counters.
 * @details Labels and composes the decode and eviction counts observed with
 * the undersized stress cache and the viewport-derived production cache.
 * @param[in] before_decodes Decode count for the stress cache.
 * @param[in] before_evicts Eviction count for the stress cache.
 * @param[in] after_decodes Decode count for the production-sized cache.
 * @param[in] after_evicts Eviction count for the production-sized cache.
 * @pre All counters were captured after the same deterministic pan sequence.
 * @pre The cache cell constants match the two fixtures being compared.
 * @post One complete thrash-information line has been attempted on descriptor 2.
 * @post No counter or cache state is modified.
 * @note Destination failure does not alter the thrash assertions.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_report_thrash(uint32_t before_decodes,
                                                uint32_t before_evicts,
                                                uint32_t after_decodes,
                                                uint32_t after_evicts)
{
  ra8_test_output_t    output = {};
  ra8_test_output_fd_t state  = {};
  (void)internal_test_output_fd_init(&output, &state, STDERR_FILENO);
  (void)internal_test_output_text(&output, "[INFO] ereader-manga pan thrash: stress(");
  (void)internal_test_output_u64(&output, (uint64_t)k_t_stress_cells);
  (void)internal_test_output_text(&output, " cells) decodes=");
  (void)internal_test_output_u64(&output, before_decodes);
  (void)internal_test_output_text(&output, " evicts=");
  (void)internal_test_output_u64(&output, before_evicts);
  (void)internal_test_output_text(&output, " ; sized(");
  (void)internal_test_output_u64(&output, (uint64_t)k_t_cells);
  (void)internal_test_output_text(&output, " cells) decodes=");
  (void)internal_test_output_u64(&output, after_decodes);
  (void)internal_test_output_text(&output, " evicts=");
  (void)internal_test_output_u64(&output, after_evicts);
  (void)internal_test_output_text(&output, "\n");
}

/**
 * @struct png_cursor_t
 * @brief Forward cursor over the baked PNG for the produce pull seam.
 */
typedef struct {
  const uint8_t* data; /**< Baked PNG bytes.        */
  uint32_t       len;  /**< Total length.           */
  uint32_t       pos;  /**< Bytes delivered so far. */
} png_cursor_t;

/** @brief produce pull seam over the baked PNG. */
static ra8_err_t png_pull(void* ctx, uint8_t* buf, size_t cap, size_t* got)
{
  png_cursor_t*  c     = (png_cursor_t*)ctx;
  const uint32_t avail = c->len - c->pos;
  const size_t   take  = ((uint32_t)cap < avail) ? cap : (size_t)avail;
  memcpy(buf, &c->data[c->pos], take);
  c->pos += (uint32_t)take;
  *got = take;
  return k_ra8_ok;
}

/** @brief Produce + parse the atlas and bind the cache + reader (shared setup).
 */
static void setup_reader(void)
{
  png_cursor_t cur = {.data = k_mg_png, .len = k_mg_png_len, .pos = 0U};
  s_store = (ra8_jof_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf), .len = 0U};
  const ra8_jof_produce_cfg_t pcfg = {.pull       = png_pull,
                                      .pull_ctx   = &cur,
                                      .sink       = ra8_jof_memstore_sink,
                                      .sink_ctx   = &s_store,
                                      .tile_w     = (uint16_t)k_t_tile_edge,
                                      .tile_h     = (uint16_t)k_t_tile_edge,
                                      .codec      = (uint8_t)k_ra8_jof_codec_deflate,
                                      .max_width  = (uint16_t)k_t_cap_edge,
                                      .max_height = (uint16_t)k_t_cap_edge,
                                      .work       = s_work,
                                      .work_cap   = sizeof(s_work),
                                      .webp_work  = NULL};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jof_produce(&pcfg, &s_info));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jof_parse(ra8_jof_memstore_pread, &s_store, s_store.len, &s_info));
  s_tile_src                      = (mg_tile_src_t){.info        = &s_info,
                                                    .pread       = ra8_jof_memstore_pread,
                                                    .pread_ctx   = &s_store,
                                                    .scratch     = s_scratch,
                                                    .scratch_cap = (uint32_t)sizeof(s_scratch)};
  const ra8_tile_cache_cfg_t ccfg = {.cell_mem     = s_cell_mem,
                                     .cell_bytes   = (uint32_t)k_t_cell_bytes,
                                     .cell_count   = (uint32_t)k_t_cells,
                                     .meta         = s_meta,
                                     .keys         = s_keys,
                                     .dims         = s_dims,
                                     .buckets      = s_buckets,
                                     .bucket_count = (uint32_t)k_t_buckets,
                                     .decode       = mg_tile_decode,
                                     .decode_ctx   = &s_tile_src};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(&s_cache, &ccfg));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gfx_init(s_fb, (uint16_t)k_t_fb_w, (uint16_t)k_t_fb_h, k_ra8_gfx_format_rgb565));
  const mg_reader_cfg_t rcfg = {.fb       = s_fb,
                                .fb_w     = (int32_t)k_t_fb_w,
                                .fb_h     = (int32_t)k_t_fb_h,
                                .cache    = &s_cache,
                                .info     = &s_info,
                                .image_id = (uint32_t)k_t_image_id};
  TEST_ASSERT_EQ(k_ra8_ok, mg_reader_init(&s_reader, &rcfg));
}

/** @brief Append a little-endian u16 to a memstore (hand atlas builder). */
static void t_put_u16(ra8_jof_memstore_t* st, uint16_t v)
{
  const uint8_t b[2] = {(uint8_t)(v & 0xFFU), (uint8_t)(v >> 8U)};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jof_memstore_sink(st, b, sizeof(b)));
}

/** @brief Append a little-endian u32 to a memstore (hand atlas builder). */
static void t_put_u32(ra8_jof_memstore_t* st, uint32_t v)
{
  const uint8_t b[4] = {(uint8_t)(v & 0xFFU),
                        (uint8_t)((v >> 8U) & 0xFFU),
                        (uint8_t)((v >> 16U) & 0xFFU),
                        (uint8_t)((v >> 24U) & 0xFFU)};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jof_memstore_sink(st, b, sizeof(b)));
}

/** @brief Append raw bytes to a memstore (hand atlas builder). */
static void t_put_bytes(ra8_jof_memstore_t* st, const uint8_t* p, size_t n)
{
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jof_memstore_sink(st, p, n));
}

/**
 * @brief Hand-build a minimal genuine bpp=3 (RGB888) JOF atlas into @p st.
 *
 * @details A 1x1 image with one 1x1 raw tile: structurally complete and
 *          accepted by ``ra8_jof_parse`` (bpp 3 is a legal header value), but
 *          not gray8 -- so ::mg_reader_init must fail closed on it (#339). The
 *          layout mirrors the writer in test_ra8_jof.c: 32-byte header, the
 * tile stream, an 8-byte index entry, then the 16-byte footer.
 *
 * @param[out] st Memstore over ::s_store_buf to fill (reset to empty here).
 *
 * @pre ::s_store_buf out-lives @p st.
 * @pre @p st is non-NULL.
 * @post @p st holds a parseable bpp=3 atlas; `st->len` == the footer
 * total_size.
 * @post No other test state is mutated.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void build_bpp3_atlas(ra8_jof_memstore_t* st)
{
  *st = (ra8_jof_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf), .len = 0U};
  const uint8_t magic[4] = {'J', 'O', 'F', '1'};
  t_put_bytes(st, magic, sizeof(magic));
  t_put_u16(st, 1U); /* width  */
  t_put_u16(st, 1U); /* height */
  t_put_u16(st, 1U); /* tile_w */
  t_put_u16(st, 1U); /* tile_h */
  const uint8_t bpp_codec[2] = {(uint8_t)k_t_colour_bpp, (uint8_t)k_ra8_jof_codec_raw};
  t_put_bytes(st, bpp_codec, sizeof(bpp_codec)); /* bpp=3, codec=raw        */
  t_put_u16(st, 0U);                             /* reserved (must be zero) */
  t_put_u32(st, 1U);                             /* tile_count              */
  const uint8_t hdr_pad[12] = {};
  t_put_bytes(st, hdr_pad, sizeof(hdr_pad)); /* header padded to 32 bytes */

  const uint32_t tile_off = (uint32_t)st->len; /* == 32 */
  const uint8_t  px[3]    = {0x10U, 0x20U, 0x30U};
  t_put_bytes(st, px, sizeof(px)); /* one RGB888 pixel, raw codec */
  const uint32_t tile_len = (uint32_t)st->len - tile_off;

  const uint32_t index_off = (uint32_t)st->len;
  t_put_u32(st, tile_off);
  t_put_u32(st, tile_len);

  t_put_u32(st, index_off);
  t_put_u32(st, 1U);                     /* footer tile_count                          */
  t_put_u32(st, (uint32_t)st->len + 8U); /* total = current + remaining 8 footer bytes */
  const uint8_t fmagic[4] = {'J', 'O', 'F', 'E'};
  t_put_bytes(st, fmagic, sizeof(fmagic));
}

/**
 * @struct pan_store_t
 * @brief Caller-owned tile-cache storage passed to ::run_pan_thrash.
 *
 * @details Lets one pan sequence run against caches of different cell counts
 *          (the 4-cell stress budget vs the derived sized budget) so their
 *          decode / eviction counts can be compared.
 */
typedef struct {
  uint8_t*             cell_mem;     /**< `cell_count * k_t_cell_bytes` of tile storage. */
  ra8_tile_key_t*      keys;         /**< `cell_count` key entries.                      */
  ra8_tile_dims_t*     dims;         /**< `cell_count` dim descriptors.                  */
  ra8_keycache_cell_t* meta;         /**< `cell_count` link-metadata entries.            */
  int32_t*             buckets;      /**< `bucket_count` hash-bucket heads.              */
  uint32_t             bucket_count; /**< Number of hash buckets.                        */
  uint32_t             cell_count;   /**< Number of cells in this cache.                 */
} pan_store_t;

/**
 * @brief Render an oscillating one-tile pan and report decode / eviction
 * totals.
 *
 * @details Binds a fresh tile cache of @p store->cell_count cells over the
 *          already-produced atlas (::s_info / ::s_tile_src), renders the
 * initial 1:1 frame, then oscillates the viewport right/left by one tile
 *          ::k_t_pan_cycles times, rendering each step. The cache's cumulative
 *          miss (== decode) and eviction counters are read back at the end.
 *
 * @param[in]  store         Cache storage + cell count to exercise.
 * @param[out] out_decodes   Total decode-on-miss count across the pan.
 * @param[out] out_evictions Total tiles evicted across the pan.
 *
 * @pre ::setup_reader ran (atlas produced, gfx bound, ::s_tile_src set).
 * @pre @p store arrays cover @p store->cell_count / bucket_count entries.
 * @post @p out_decodes and @p out_evictions hold the cache's final counters.
 * @post No file-scope reader/cache state is left bound.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void run_pan_thrash(const pan_store_t* store, uint32_t* out_decodes, uint32_t* out_evictions)
{
  ra8_tile_cache_t           cache = {};
  const ra8_tile_cache_cfg_t ccfg  = {.cell_mem     = store->cell_mem,
                                      .cell_bytes   = (uint32_t)k_t_cell_bytes,
                                      .cell_count   = store->cell_count,
                                      .meta         = store->meta,
                                      .keys         = store->keys,
                                      .dims         = store->dims,
                                      .buckets      = store->buckets,
                                      .bucket_count = store->bucket_count,
                                      .decode       = mg_tile_decode,
                                      .decode_ctx   = &s_tile_src};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(&cache, &ccfg));
  mg_reader_t           reader = {};
  const mg_reader_cfg_t rcfg   = {.fb       = s_fb,
                                  .fb_w     = (int32_t)k_t_fb_w,
                                  .fb_h     = (int32_t)k_t_fb_h,
                                  .cache    = &cache,
                                  .info     = &s_info,
                                  .image_id = (uint32_t)k_t_image_id};
  TEST_ASSERT_EQ(k_ra8_ok, mg_reader_init(&reader, &rcfg));
  TEST_ASSERT_EQ(k_ra8_ok, mg_reader_render(&reader)); /* initial 1:1 frame */
  for (uint32_t i = 0U; i < (uint32_t)k_t_pan_cycles; ++i) {
    TEST_ASSERT(mg_reader_tap(&reader, 950, 300)); /* pan right one tile */
    TEST_ASSERT_EQ(k_ra8_ok, mg_reader_render(&reader));
    TEST_ASSERT(mg_reader_tap(&reader, 50, 300)); /* pan left one tile */
    TEST_ASSERT_EQ(k_ra8_ok, mg_reader_render(&reader));
  }
  uint32_t decodes = 0U;
  uint32_t evicts  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_stats(&cache, NULL, &decodes, &evicts));
  *out_decodes   = decodes;
  *out_evictions = evicts;
}

/**
 * @test test_manga_work_arena_covers_advertised_cap
 * @brief The app's static work arena covers the producer's own worst-case
 *        requirement for the cap the app advertises.
 *
 * @details
 * `ra8_jof_produce()` carves its band, tile stage and compressed-tile
 * bound from the *decoded* geometry, so an arena that is too small for the
 * advertised `max_width`/`max_height` does not fail on every source -- only on
 * one wide enough, or deep enough in bpp, to exhaust it. That makes an
 * under-sized arena a latent capability lie rather than an obvious bug: the app
 * advertises a cap it fails closed on. This test is the authoritative,
 * exact form of the agreement (the app's static_assert only tests the band
 * term, a lower bound). A 2 MiB arena backs a cap of just 1016 px, which is
 * what this app shipped against a declared 2048.
 *
 * @par MC/DC:
 * Decision: `(need == 0U) || (need > sizeof arena)` (2 conditions), the boot
 * guard in the app's `mg_build_atlas()`.
 * - Vector 1: need=3203832 (in-range), arena=3203832 -> false (control).
 * - Vector 2: need=0 (nonsense caps: tile edge 0), arena=3203832 -> true
 *             (varies the first condition only).
 * - Vector 3: need=3203832, arena=2 MiB (the shipped-and-fixed size) -> true
 *             (varies the second condition only).
 * Vectors 1+2 prove the zero-return independently affects the outcome; 1+3
 * prove the same for the size comparison. N+1 = 3 vectors for N=2.
 */
static void test_manga_work_arena_covers_advertised_cap(void)
{
  TEST_BEGIN("ereader_manga: work arena covers the advertised produce cap");
  const uint32_t need = ra8_jof_work_bytes((uint16_t)k_t_cap_edge,
                                           (uint16_t)k_t_cap_edge,
                                           (uint16_t)k_t_tile_edge,
                                           (uint16_t)k_t_tile_edge);
  internal_report_arena(need);
  /* Vector 1: the real configuration -- a usable requirement the arena covers.
   */
  TEST_ASSERT(need != 0U);
  TEST_ASSERT(sizeof(s_work) >= (size_t)need);
  /* Vector 2: nonsense geometry returns 0, which the boot guard rejects. */
  TEST_ASSERT_EQ(0U, ra8_jof_work_bytes((uint16_t)k_t_cap_edge, (uint16_t)k_t_cap_edge, 0U, 0U));
  /* Vector 3: the previously shipped 2 MiB arena does NOT cover this cap --
   * the defect this test exists to keep fixed. */
  TEST_ASSERT(need > (2U * 1024U * 1024U));
  TEST_END("ereader_manga: work arena covers the advertised produce cap");
}

/**
 * @test test_manga_render_golden
 * @brief produce -> tile-cache -> mg_reader initial render reproduces the
 * golden banner numbers (page geometry, atlas byte count, framebuffer crc) --
 *        the exact values hil.conf asserts on ra8_emulator / silicon.
 *
 * @par MC/DC:
 * (integration gate: the compound decisions in the producer, atlas reader and
 * tile cache carry their vectors in test_ra8_jof*.c / test_ra8_tile_cache*;
 * here the oracle is the end-to-end golden framebuffer hash.)
 */
static void test_manga_render_golden(void)
{
  TEST_BEGIN("ereader_manga: produce + tile render == golden banner");
  setup_reader();
  TEST_ASSERT_EQ(k_t_page_w, s_info.width);
  TEST_ASSERT_EQ(k_t_page_h, s_info.height);
  TEST_ASSERT_EQ(k_t_tiles_want, s_info.tile_count);
  TEST_ASSERT_EQ(k_t_golden_atlas_bytes, s_info.total_size);

  TEST_ASSERT_EQ(k_ra8_ok, mg_reader_render(&s_reader));
  const uint32_t crc = mg_fnv1a(s_fb, (uint32_t)sizeof(s_fb));
  internal_report_golden(crc);
  TEST_ASSERT_EQ(k_t_golden_crc, crc);
  TEST_END("ereader_manga: produce + tile render == golden banner");
}

/**
 * @test test_manga_fixture_pattern
 * @brief One decoded tile matches the generator's page pattern (frame + fill),
 *        so the committed PNG fixture and gen_manga_page_fixture.py cannot
 * drift.
 *
 * @par MC/DC:
 * (fixture-integrity check; the read_tile decode decisions are covered in
 * test_ra8_jof.c. Here the oracle is the known solid-fill / frame pixels.)
 */
static void test_manga_fixture_pattern(void)
{
  TEST_BEGIN("ereader_manga: decoded tile matches the baked page pattern");
  setup_reader();
  uint16_t tw = 0U;
  uint16_t th = 0U;
  /* Tile (0,0): black inner frame at the corner, solid gray fill left of label.
   */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_jof_read_tile(ra8_jof_memstore_pread,
                                   &s_store,
                                   &s_info,
                                   0U,
                                   0U,
                                   s_scratch,
                                   (uint32_t)sizeof(s_scratch),
                                   s_probe_cell,
                                   (uint32_t)sizeof(s_probe_cell),
                                   &tw,
                                   &th));
  TEST_ASSERT_EQ(k_t_tile_edge, tw);
  TEST_ASSERT_EQ(k_t_tile_edge, th);
  TEST_ASSERT_EQ(k_t_gray_edge, s_probe_cell[0U]);                /* top-left frame pixel   */
  TEST_ASSERT_EQ(k_t_gray_c0r0, s_probe_cell[(128U * tw) + 10U]); /* fill left of the label */
  /* Tile (5,7): the bottom-right tile carries its own distinct fill gray. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_jof_read_tile(ra8_jof_memstore_pread,
                                   &s_store,
                                   &s_info,
                                   5U,
                                   7U,
                                   s_scratch,
                                   (uint32_t)sizeof(s_scratch),
                                   s_probe_cell,
                                   (uint32_t)sizeof(s_probe_cell),
                                   &tw,
                                   &th));
  TEST_ASSERT_EQ(k_t_gray_c5r7, s_probe_cell[(128U * tw) + 10U]);
  TEST_END("ereader_manga: decoded tile matches the baked page pattern");
}

/**
 * @test test_manga_zone_hit
 * @brief mg_zone_hit classifies every tap-zone (status / four edges / centre)
 *        and rejects out-of-bounds taps.
 *
 * @par MC/DC:
 * Decision cascade in mg_zone_hit (each arm a single-condition decision):
 * - x<0            -> none  : (-1,300) true;   others false.
 * - x>=fb_w        -> none  : (1024,300) true; (950,300) false.
 * - y<statusbar    -> none  : (500,20) true;   (500,100) false.
 * - y>=fb_h        -> none  : (500,600) true;  (500,500) false.
 * - y<status+band  -> up    : (500,100) true.
 * - y>=fb_h-band   -> down  : (500,500) true.
 * - x<band         -> left  : (50,300) true.
 * - x>=fb_w-band   -> right : (950,300) true.
 * - else           -> zoom  : (500,300).
 * Each independent-influence pair varies exactly one condition across the two
 * listed vectors, so the nine vectors give MC/DC over the cascade.
 */
static void test_manga_zone_hit(void)
{
  TEST_BEGIN("ereader_manga: mg_zone_hit tap-zone classification");
  setup_reader();
  TEST_ASSERT_EQ(k_mg_zone_none, mg_zone_hit(&s_reader, 500, 20));   /* status bar     */
  TEST_ASSERT_EQ(k_mg_zone_none, mg_zone_hit(&s_reader, -1, 300));   /* left of panel  */
  TEST_ASSERT_EQ(k_mg_zone_none, mg_zone_hit(&s_reader, 1024, 300)); /* right of panel */
  TEST_ASSERT_EQ(k_mg_zone_none, mg_zone_hit(&s_reader, 500, 600));  /* below panel    */
  TEST_ASSERT_EQ(k_mg_zone_pan_up, mg_zone_hit(&s_reader, 500, 100));
  TEST_ASSERT_EQ(k_mg_zone_pan_down, mg_zone_hit(&s_reader, 500, 500));
  TEST_ASSERT_EQ(k_mg_zone_pan_left, mg_zone_hit(&s_reader, 50, 300));
  TEST_ASSERT_EQ(k_mg_zone_pan_right, mg_zone_hit(&s_reader, 950, 300));
  TEST_ASSERT_EQ(k_mg_zone_zoom, mg_zone_hit(&s_reader, 500, 300));
  TEST_ASSERT_EQ(k_mg_zone_none, mg_zone_hit(NULL, 500, 300)); /* null guard */
  TEST_END("ereader_manga: mg_zone_hit tap-zone classification");
}

/**
 * @test test_manga_tap_nav
 * @brief mg_reader_tap pans the viewport, clamps at the page edge, and toggles
 *        zoom; panning is a no-op while fit-page shows the whole page.
 *
 * @par MC/DC:
 * Decision `(view_x != old) || (view_y != old)` in mg_pan (2 conditions):
 * - Vector 1: pan right from (0,0)   -> view_x changes -> true (varies x).
 * - Vector 2: pan right again at max  -> neither changes -> false (control).
 * - Vector 3: pan down from (0,0)    -> view_y changes -> true (varies y).
 * Vectors 1+2 prove view_x independently drives the outcome; 2+3 prove view_y.
 */
static void test_manga_tap_nav(void)
{
  TEST_BEGIN("ereader_manga: mg_reader_tap pan / clamp / zoom");
  setup_reader();
  TEST_ASSERT_EQ(0, s_reader.view_x);
  /* One right-edge tap slides the viewport one step. */
  TEST_ASSERT(mg_reader_tap(&s_reader, 950, 300));
  TEST_ASSERT_EQ(k_t_pan_step, s_reader.view_x);
  /* A second lands at the page edge (page_w - fb_w = 512). */
  TEST_ASSERT(mg_reader_tap(&s_reader, 950, 300));
  TEST_ASSERT_EQ(k_t_page_w - k_t_fb_w, s_reader.view_x);
  /* A third is clamped: no change, no redraw. */
  TEST_ASSERT(!mg_reader_tap(&s_reader, 950, 300));
  /* A centre tap toggles to fit-page and re-pins the viewport to (0,0). */
  TEST_ASSERT(mg_reader_tap(&s_reader, 500, 300));
  TEST_ASSERT_EQ(k_mg_zoom_fit, s_reader.zoom);
  TEST_ASSERT_EQ(0, s_reader.view_x);
  /* Panning is a no-op while the whole page is shown. */
  TEST_ASSERT(!mg_reader_tap(&s_reader, 950, 300));
  /* A down-edge tap at 1:1 slides the viewport vertically. */
  TEST_ASSERT(mg_reader_tap(&s_reader, 500, 300)); /* back to 1:1 */
  TEST_ASSERT(mg_reader_tap(&s_reader, 500, 500));
  TEST_ASSERT_EQ(k_t_pan_step, s_reader.view_y);
  TEST_END("ereader_manga: mg_reader_tap pan / clamp / zoom");
}

/**
 * @test test_manga_status_text
 * @brief mg_reader_status renders the zoom + viewport into the status string.
 *
 * @par MC/DC:
 * Decision `r->zoom == k_mg_zoom_full` selecting the "1:1"/"FIT" fragment:
 * - full zoom -> "1:1" branch (true); fit zoom -> "FIT" branch (false).
 */
static void test_manga_status_text(void)
{
  TEST_BEGIN("ereader_manga: mg_reader_status formatting");
  setup_reader();
  char line[k_manga_line_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok, mg_reader_status(&s_reader, line, (uint32_t)sizeof(line)));
  TEST_ASSERT_EQ(0, strcmp(line, "MANGA  1:1  x=0 y=0"));
  TEST_ASSERT(mg_reader_tap(&s_reader, 950, 300)); /* pan right one step */
  TEST_ASSERT_EQ(k_ra8_ok, mg_reader_status(&s_reader, line, (uint32_t)sizeof(line)));
  TEST_ASSERT_EQ(0, strcmp(line, "MANGA  1:1  x=256 y=0"));
  TEST_END("ereader_manga: mg_reader_status formatting");
}

/**
 * @test test_manga_bpp_guard
 * @brief ::mg_reader_init rejects a colour (bpp != 1) JOF atlas fail-closed and
 *        still binds a valid gray8 atlas (#339).
 *
 * @details The reader's blit path reads one byte per pixel and expands gray8; a
 *          bpp=3 atlas would be mis-read one byte per pixel and rendered as
 *          garbage. This feeds a genuine, parseable bpp=3 atlas and asserts the
 *          bind fails with ::k_ra8_err_not_supported, then confirms the real
 *          gray8 atlas still binds and renders -- the guard does not regress
 * the supported format.
 *
 * @par MC/DC:
 * Decision `cfg->info->bpp != k_mg_gray8_bpp` in mg_reader_check_geometry,
 * reached through ::mg_reader_init (1 condition):
 * - Vector 1: bpp=3 -> true  -> k_ra8_err_not_supported (the colour atlas).
 * - Vector 2: bpp=1 -> false -> k_ra8_ok (the produced gray8 atlas).
 * The two vectors toggle the sole condition, giving MC/DC over the guard.
 */
static void test_manga_bpp_guard(void)
{
  TEST_BEGIN("ereader_manga: reader rejects a non-gray8 (colour) JOF atlas");
  /* A genuine, parseable bpp=3 atlas -- rejected, not mis-rendered. */
  ra8_jof_memstore_t store3 = {};
  build_bpp3_atlas(&store3);
  ra8_jof_info_t info3 = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jof_parse(ra8_jof_memstore_pread, &store3, store3.len, &info3));
  TEST_ASSERT_EQ(k_t_colour_bpp, info3.bpp);
  mg_reader_t           reader3 = {};
  const mg_reader_cfg_t rcfg3   = {.fb       = s_fb,
                                   .fb_w     = (int32_t)k_t_fb_w,
                                   .fb_h     = (int32_t)k_t_fb_h,
                                   .cache    = &s_cache,
                                   .info     = &info3,
                                   .image_id = (uint32_t)k_t_image_id};
  TEST_ASSERT_EQ(k_ra8_err_not_supported, mg_reader_init(&reader3, &rcfg3));

  /* The supported gray8 path still binds and renders. */
  setup_reader();
  TEST_ASSERT_EQ(k_t_gray8_bpp, s_info.bpp);
  TEST_ASSERT_EQ(k_ra8_ok, mg_reader_render(&s_reader));
  TEST_END("ereader_manga: reader rejects a non-gray8 (colour) JOF atlas");
}

/**
 * @test test_manga_pan_no_thrash
 * @brief The derived tile-cache budget stops the pan thrash: a representative
 *        oscillating pan never evicts an on-screen tile, where the old 4-cell
 *        budget re-decodes a full screen every frame (#338).
 *
 * @details Runs the identical oscillating one-tile pan against two caches over
 *          the same atlas -- the old 4-cell stress budget and the derived
 *          ::k_t_cells budget -- and compares their cumulative decode /
 * eviction counters. The sized cache holds the whole oscillation working set,
 * so it evicts nothing and re-decodes nothing; the stress cache thrashes.
 *
 * @par MC/DC:
 * (measurement gate: the tile-cache LRU/eviction decisions carry their vectors
 * in test_ra8_tile_cache*.c and test_ra8_keycache*.c; here the oracle is the
 * eviction / decode counters read back over a representative pan.)
 */
static void test_manga_pan_no_thrash(void)
{
  TEST_BEGIN("ereader_manga: sized tile cache stops the pan thrash");
  setup_reader(); /* produce the atlas, bind gfx + s_tile_src */

  const pan_store_t stress = {.cell_mem     = s_stress_cell_mem,
                              .keys         = s_stress_keys,
                              .dims         = s_stress_dims,
                              .meta         = s_stress_meta,
                              .buckets      = s_stress_buckets,
                              .bucket_count = (uint32_t)k_t_buckets,
                              .cell_count   = (uint32_t)k_t_stress_cells};
  const pan_store_t sized  = {.cell_mem     = s_cell_mem,
                              .keys         = s_keys,
                              .dims         = s_dims,
                              .meta         = s_meta,
                              .buckets      = s_buckets,
                              .bucket_count = (uint32_t)k_t_buckets,
                              .cell_count   = (uint32_t)k_t_cells};

  uint32_t before_decodes = 0U;
  uint32_t before_evicts  = 0U;
  uint32_t after_decodes  = 0U;
  uint32_t after_evicts   = 0U;
  run_pan_thrash(&stress, &before_decodes, &before_evicts);
  run_pan_thrash(&sized, &after_decodes, &after_evicts);

  internal_report_thrash(before_decodes, before_evicts, after_decodes, after_evicts);

  /* The sized cache holds the whole oscillation working set: it never evicts,
   * so no on-screen tile is ever re-decoded (the thrash is structurally gone).
   */
  TEST_ASSERT_EQ(0U, after_evicts);
  /* The old 4-cell budget thrashes: it evicts and re-decodes every frame. */
  TEST_ASSERT(before_evicts > 0U);
  TEST_ASSERT(before_decodes > after_decodes);
  TEST_ASSERT(before_decodes >= (after_decodes * (uint32_t)k_t_thrash_min_ratio));
  TEST_END("ereader_manga: sized tile cache stops the pan thrash");
}

/** @brief Fetch tile (tx,ty) of the reader image through ::s_cache, then
 * release. */
static void assert_tile_resident(uint16_t tx, uint16_t ty)
{
  const ra8_tile_key_t key = {.image_id = (uint32_t)k_t_image_id,
                              .tile_x   = tx,
                              .tile_y   = ty,
                              .zoom     = 0U};
  ra8_tile_t           t   = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_get(&s_cache, &key, &t));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_put(&s_cache, t.pixels));
}

/**
 * @test test_manga_pan_prefetch
 * @brief After a right pan + render, ::mg_reader_prefetch has warmed the next
 *        column of tiles into the cache, so they are resident before the
 * follow- on pan needs them -- and the read-ahead evicts no on-screen tile
 * (#341).
 *
 * @details The panned 1:1 frame straddles tile columns 1..4, rows 0..2; the
 *          prefetch warms column 5 (rows 0..2) one step ahead in the pan
 *          direction. Residency is proved by the miss counter not moving across
 * a fetch of the warmed tiles, and the budget bound by the eviction counter not
 * moving (a warmed lead column plus the visible frame fit inside the derived
 * ::k_t_cells budget).
 *
 * @par MC/DC:
 * (integration gate: the pan-prefetch direction / edge / budget decisions carry
 * their vectors in test_ra8_tile_cache*.c; here the oracle is the miss /
 * eviction counters read back after a real pan + render + prefetch.)
 */
static void test_manga_pan_prefetch(void)
{
  TEST_BEGIN("ereader_manga: pan prefetch warms the next column, resident");
  setup_reader();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, mg_reader_prefetch(NULL)); /* null guard             */
  TEST_ASSERT_EQ(k_ra8_ok, mg_reader_render(&s_reader));        /* initial frame at (0,0) */
  /* No pan yet: prefetch is a no-op that still succeeds. */
  TEST_ASSERT_EQ(k_ra8_ok, mg_reader_prefetch(&s_reader));

  /* One right-edge tap pans to view_x=256; render the newly exposed frame. */
  TEST_ASSERT(mg_reader_tap(&s_reader, 950, 300));
  TEST_ASSERT_EQ(k_t_pan_step, s_reader.view_x);
  TEST_ASSERT_EQ(k_ra8_ok, mg_reader_render(&s_reader));

  /* Idle-window read-ahead warms the column one tile ahead (col 5, rows 0..2).
   */
  TEST_ASSERT_EQ(k_ra8_ok, mg_reader_prefetch(&s_reader));

  uint32_t miss_before = 0U;
  uint32_t evic_before = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_stats(&s_cache, NULL, &miss_before, &evic_before));
  assert_tile_resident(k_t_lead_col, 0U); /* predicted lead-column tiles ... */
  assert_tile_resident(k_t_lead_col, 2U);
  assert_tile_resident(1U, 0U); /* ... and an on-screen tile: both still resident */
  uint32_t miss_after = 0U;
  uint32_t evic_after = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_stats(&s_cache, NULL, &miss_after, &evic_after));
  TEST_ASSERT_EQ(miss_before, miss_after); /* every fetch was a cache hit       */
  TEST_ASSERT_EQ(evic_before, evic_after); /* budget respected: nothing evicted */

  /* The vertical directions warm the same way (down then up). */
  TEST_ASSERT(mg_reader_tap(&s_reader, 500, 500)); /* pan down */
  TEST_ASSERT_EQ(k_ra8_ok, mg_reader_render(&s_reader));
  TEST_ASSERT_EQ(k_ra8_ok, mg_reader_prefetch(&s_reader));
  TEST_ASSERT(mg_reader_tap(&s_reader, 500, 100)); /* pan up */
  TEST_ASSERT_EQ(k_ra8_ok, mg_reader_render(&s_reader));
  TEST_ASSERT_EQ(k_ra8_ok, mg_reader_prefetch(&s_reader));
  TEST_END("ereader_manga: pan prefetch warms the next column, resident");
}

/**
 * @brief Discard production log bytes during host error-path tests.
 * @details Redirects the logger away from host-unmapped ITM while leaving the
 * production error paths and their sanitizer instrumentation intact.
 * @param[in] context Unused caller context.
 * @param[in] byte Unused log byte.
 * @pre Installed before any vector that intentionally triggers an error log.
 * @pre The host test runs in one process and does not replace the sink concurrently.
 * @post The supplied byte is discarded without I/O or MMIO access.
 * @post No test fixture or production state is mutated.
 * @note This host-only callback does not suppress sanitizer diagnostics.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_host_log_sink(void* context, uint8_t byte)
{
  (void)context;
  (void)byte;
}

/**
 * @brief Test entry point -- runs the ereader_manga host-twin gates.
 * @return 0 on success; unity_minimal.h exits non-zero on first failure.
 * @pre None.
 * @pre None.
 * @post Every gate ran (or the process exited on first failure).
 * @post Raw descriptor 2 carries the derived golden banner numbers.
 * @note Not thread-safe. No SIGALRM / timers used.
 * @since 0.1.0
 */
int32_t main(void)
{
  ra8_log_set_byte_sink(internal_host_log_sink, nullptr);
  test_manga_work_arena_covers_advertised_cap();
  test_manga_render_golden();
  test_manga_fixture_pattern();
  test_manga_zone_hit();
  test_manga_tap_nav();
  test_manga_status_text();
  test_manga_bpp_guard();
  test_manga_pan_no_thrash();
  test_manga_pan_prefetch();
  (void)internal_test_output_fd_text(STDERR_FILENO, "[OK  ] test_app_ereader_manga.c\n");
  ra8_log_set_byte_sink(nullptr, nullptr);
  return 0;
}
