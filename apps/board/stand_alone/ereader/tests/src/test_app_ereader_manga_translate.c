/**
 * @file apps/board/stand_alone/ereader/tests/src/test_app_ereader_manga_translate.c
 * @brief Host integration tests for `ereader_manga` translation UI (`mg_translate`).
 *
 * @details
 * Exercises the production `mg_reader.c`, `mg_translate.c`, and
 * `mg_translate_fixture.c` modules together against a baked 1536x2048 JOF page
 * and real `libs/ra8_translate` Zig archives:
 *   1. Unbound `mg_reader` baseline preserves original `crc=5CBD900B` render.
 *   2. Per-title Original/Translated toggle persistence across titles/chapters.
 *   3. Bubble tap hit-testing in 1:1, panned, and FIT zoom showing original
 *      UTF-8 text and OCR confidence without breaking background pan/zoom.
 *   4. Queued, translating, and paused chapter markers from `ra8_translate_job`.
 *   5. Fail-closed fallback to unmasked original pixels on missing, corrupt,
 *      stale-hash, stale-model, stale-glossary, or out-of-range sidecars.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "jof.h"
#include "jof_produce.h"
#include "mg_page_fixture.h"
#include "mg_reader.h"
#include "mg_translate.h"
#include "mg_translate_fixture.h"
#include "ra8_attributes.h"
#include "ra8_boot_entry.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_log.h"
#include "ra8_tile_cache.h"
#include "ra8_translate.h"
#include "ra8_translate_job.h"
#include "unity_minimal.h"

enum : uint32_t {
  k_mt_fb_w       = 1024U,
  k_mt_fb_h       = 600U,
  k_mt_tile_edge  = 256U,
  k_mt_cap_edge   = 2048U,
  k_mt_cell_bytes = 256U * 256U,
  k_mt_view_cols  = ((k_mt_fb_w + k_mt_tile_edge - 1U) / k_mt_tile_edge) + 1U,
  k_mt_view_rows =
    (((k_mt_fb_h - (uint32_t)k_mg_statusbar_h) + k_mt_tile_edge - 1U) / k_mt_tile_edge) + 1U,
  k_mt_cells        = (k_mt_view_cols + 1U) * (k_mt_view_rows + 1U),
  k_mt_buckets      = 64U,
  k_mt_scratch      = 96U * 1024U,
  k_mt_work         = 3203832U,
  k_mt_store        = 4U * 1024U * 1024U,
  k_mt_image_id     = 1U,
  k_mt_golden_crc   = 0x5CBD900BU,
  k_mt_title_alt    = 202U,
  k_mt_title_base   = 300U,
  k_mt_extra_titles = 20U,
  k_mt_toggle_x     = 950U,
  k_mt_chap_x       = 800U,
  k_mt_bar_y        = 24U,
  k_mt_b0_panel_x   = 200U,
  k_mt_b0_panel_y   = 180U,
  k_mt_b1_panel_x   = 500U,
  k_mt_b1_panel_y   = 250U,
  k_mt_pan_r_x      = 950U,
  k_mt_pan_r_y      = 300U,
  k_mt_zoom_bg_x    = 500U,
  k_mt_zoom_bg_y    = 390U,
  k_mt_fit_b1_x     = 445U,
  k_mt_fit_b1_y     = 118U,
  k_mt_popup_hdr_w  = 32U,
  k_mt_trn1_cap     = 4096U,
  k_mt_work_cap     = 131072U,
  k_mt_corrupt_off  = 20U,
};

static uint16_t            s_fb[(size_t)k_mt_fb_h * (size_t)k_mt_fb_w];
static uint16_t            s_orig_fb[(size_t)k_mt_fb_h * (size_t)k_mt_fb_w];
static uint8_t             s_cell_mem[(size_t)k_mt_cells * (size_t)k_mt_cell_bytes];
static ra8_tile_key_t      s_keys[k_mt_cells];
static ra8_tile_dims_t     s_dims[k_mt_cells];
static ra8_keycache_cell_t s_meta[k_mt_cells];
static int32_t             s_buckets[k_mt_buckets];
static uint8_t             s_scratch[k_mt_scratch];
static uint8_t             s_work[k_mt_work];
static uint8_t             s_store_buf[k_mt_store];
static jof_memstore_t      s_store;
static jof_info_t          s_info;
static ra8_tile_cache_t    s_cache;
static mg_tile_src_t       s_tile_src;
static mg_reader_t         s_reader;

typedef struct {
  const uint8_t* data;
  uint32_t       len;
  uint32_t       pos;
} mt_png_cursor_t;

static ra8_err_t mt_png_pull(void* ctx, uint8_t* buf, size_t cap, size_t* got)
{
  mt_png_cursor_t* c     = (mt_png_cursor_t*)ctx;
  const uint32_t   avail = c->len - c->pos;
  const size_t     take  = ((uint32_t)cap < avail) ? cap : (size_t)avail;
  memcpy(buf, &c->data[c->pos], take);
  c->pos += (uint32_t)take;
  *got = take;
  return k_ra8_ok;
}

RA8_INTERNAL static void mt_silent_log(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}

static void mt_setup_reader(void)
{
  const uint32_t need = jof_work_bytes((uint16_t)k_mt_cap_edge,
                                       (uint16_t)k_mt_cap_edge,
                                       (uint16_t)k_mt_tile_edge,
                                       (uint16_t)k_mt_tile_edge);
  TEST_ASSERT((need > 0U) && (need <= (uint32_t)sizeof(s_work)));
  mt_png_cursor_t cur          = {.data = k_mg_png, .len = k_mg_png_len, .pos = 0U};
  s_store                      = (jof_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf)};
  const jof_produce_cfg_t pcfg = {.pull       = mt_png_pull,
                                  .pull_ctx   = &cur,
                                  .sink       = jof_memstore_sink,
                                  .sink_ctx   = &s_store,
                                  .tile_w     = (uint16_t)k_mt_tile_edge,
                                  .tile_h     = (uint16_t)k_mt_tile_edge,
                                  .codec      = (uint8_t)k_jof_codec_deflate,
                                  .max_width  = (uint16_t)k_mt_cap_edge,
                                  .max_height = (uint16_t)k_mt_cap_edge,
                                  .work       = s_work,
                                  .work_cap   = sizeof(s_work)};
  TEST_ASSERT_EQ(k_ra8_ok, jof_produce(&pcfg, &s_info));
  TEST_ASSERT_EQ(k_ra8_ok, jof_parse(jof_memstore_pread, &s_store, s_store.len, &s_info));
  s_tile_src                      = (mg_tile_src_t){.info        = &s_info,
                                                    .pread       = jof_memstore_pread,
                                                    .pread_ctx   = &s_store,
                                                    .scratch     = s_scratch,
                                                    .scratch_cap = (uint32_t)sizeof(s_scratch)};
  const ra8_tile_cache_cfg_t ccfg = {.cell_mem     = s_cell_mem,
                                     .cell_bytes   = (uint32_t)k_mt_cell_bytes,
                                     .cell_count   = (uint32_t)k_mt_cells,
                                     .meta         = s_meta,
                                     .keys         = s_keys,
                                     .dims         = s_dims,
                                     .buckets      = s_buckets,
                                     .bucket_count = (uint32_t)k_mt_buckets,
                                     .decode       = mg_tile_decode,
                                     .decode_ctx   = &s_tile_src};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(&s_cache, &ccfg));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gfx_init(s_fb, (uint16_t)k_mt_fb_w, (uint16_t)k_mt_fb_h, k_ra8_gfx_format_rgb565));
  const mg_reader_cfg_t rcfg = {.fb       = s_fb,
                                .fb_w     = (int32_t)k_mt_fb_w,
                                .fb_h     = (int32_t)k_mt_fb_h,
                                .cache    = &s_cache,
                                .info     = &s_info,
                                .image_id = (uint32_t)k_mt_image_id};
  TEST_ASSERT_EQ(k_ra8_ok, mg_reader_init(&s_reader, &rcfg));
}

static bool mt_content_matches_orig(void)
{
  const size_t row_start = (size_t)k_mg_statusbar_h;
  const size_t row_limit =
    (size_t)k_mt_fb_h - (size_t)k_mg_minimap_h - (size_t)k_mg_minimap_pad - 1U;
  for (size_t row = row_start; row < row_limit; ++row) {
    const size_t off = row * (size_t)k_mt_fb_w;
    if (memcmp(&s_fb[off], &s_orig_fb[off], (size_t)k_mt_fb_w * sizeof(uint16_t)) != 0) {
      return false;
    }
  }
  return true;
}

static void test_trn_unbound_baseline(void)
{
  TEST_BEGIN("ereader_manga_translate: unbound baseline unchanged");
  mt_setup_reader();
  TEST_ASSERT(s_reader.trn == nullptr);
  TEST_ASSERT_EQ(k_ra8_ok, mg_reader_render(&s_reader));
  TEST_ASSERT_EQ((uint32_t)k_mt_golden_crc, mg_fnv1a(s_fb, (uint32_t)sizeof(s_fb)));
  memcpy(s_orig_fb, s_fb, sizeof(s_fb));
  TEST_ASSERT_EQ(k_mg_zone_none,
                 mg_zone_hit(&s_reader, (int32_t)k_mt_toggle_x, (int32_t)k_mt_bar_y));
  char st[k_mt_popup_hdr_w] = {};
  TEST_ASSERT_EQ(k_ra8_ok, mg_reader_status(&s_reader, st, (uint32_t)sizeof(st)));
  TEST_ASSERT_EQ(0, strcmp(st, "MANGA  1:1  x=0 y=0"));
  TEST_END("ereader_manga_translate: unbound baseline unchanged");
}

static void test_trn_per_title_toggle(void)
{
  TEST_BEGIN("ereader_manga_translate: per-title toggle and LRU isolation");
  mt_setup_reader();
  TEST_ASSERT(mg_translate_fixture_attach(&s_reader));
  const mg_translate_ctx_t* ctrn = mg_translate_fixture_ctx();
  TEST_ASSERT(ctrn != nullptr);
  mg_translate_ctx_t* trn = s_reader.trn;
  TEST_ASSERT(!mg_translate_get_title_mode(trn, (uint32_t)k_mg_trn_fix_title_id));
  TEST_ASSERT_EQ(k_ra8_ok, mg_reader_render(&s_reader));
  TEST_ASSERT(mt_content_matches_orig());

  char mode_lbl[k_mg_trn_label_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 mg_translate_format_mode_pill(trn, mode_lbl, (uint32_t)sizeof(mode_lbl)));
  TEST_ASSERT_EQ(0, strcmp(mode_lbl, "ORIG"));

  TEST_ASSERT_EQ(k_mg_zone_trn_toggle,
                 mg_zone_hit(&s_reader, (int32_t)k_mt_toggle_x, (int32_t)k_mt_bar_y));
  TEST_ASSERT(mg_reader_tap(&s_reader, (int32_t)k_mt_toggle_x, (int32_t)k_mt_bar_y));
  TEST_ASSERT(mg_translate_get_title_mode(trn, (uint32_t)k_mg_trn_fix_title_id));
  TEST_ASSERT(mg_translate_overlays_active(trn));
  TEST_ASSERT_EQ(k_ra8_ok, mg_reader_render(&s_reader));
  TEST_ASSERT(!mt_content_matches_orig());
  TEST_ASSERT(trn->last_damage_count >= 2U);
  TEST_ASSERT_EQ(k_ra8_ok,
                 mg_translate_format_mode_pill(trn, mode_lbl, (uint32_t)sizeof(mode_lbl)));
  TEST_ASSERT_EQ(0, strcmp(mode_lbl, "TRN"));

  const mg_trn_chapter_t alt = {.chapter_order = 1U};
  TEST_ASSERT_EQ(k_ra8_ok, mg_translate_open_title(trn, (uint32_t)k_mt_title_alt, &alt, 1U, 0U));
  TEST_ASSERT(!mg_translate_get_title_mode(trn, (uint32_t)k_mt_title_alt));
  TEST_ASSERT(mg_translate_get_title_mode(trn, (uint32_t)k_mg_trn_fix_title_id));

  for (uint32_t i = 0U; i < (uint32_t)k_mt_extra_titles; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok,
                   mg_translate_set_title_mode(trn, (uint32_t)k_mt_title_base + i, (i & 1U) == 0U));
  }
  TEST_ASSERT(mg_translate_get_title_mode(trn, (uint32_t)k_mt_title_base + k_mt_extra_titles - 2U));
  TEST_ASSERT(
    !mg_translate_get_title_mode(trn, (uint32_t)k_mt_title_base + k_mt_extra_titles - 1U));
  TEST_END("ereader_manga_translate: per-title toggle and LRU isolation");
}

static void test_trn_bubble_tap_and_fit_coords(void)
{
  TEST_BEGIN("ereader_manga_translate: bubble tap source + OCR confidence");
  mt_setup_reader();
  TEST_ASSERT(mg_translate_fixture_attach(&s_reader));
  mg_translate_ctx_t* trn = s_reader.trn;
  TEST_ASSERT(mg_reader_tap(&s_reader, (int32_t)k_mt_toggle_x, (int32_t)k_mt_bar_y));
  TEST_ASSERT_EQ(k_ra8_ok, mg_reader_render(&s_reader));
  const uint32_t crc_no_popup = mg_fnv1a(s_fb, (uint32_t)sizeof(s_fb));

  TEST_ASSERT(mg_reader_tap(&s_reader, (int32_t)k_mt_b0_panel_x, (int32_t)k_mt_b0_panel_y));
  TEST_ASSERT(trn->popup.open);
  TEST_ASSERT_EQ(0U, trn->popup.reading_order);
  TEST_ASSERT_EQ((uint16_t)k_mg_trn_fix_b0_conf_bps, trn->popup.ocr_confidence_bps);
  TEST_ASSERT_EQ(
    0,
    strcmp(trn->popup.source_utf8, "\xE3\x81\x8A\xE3\x81\xAF\xE3\x82\x88\xE3\x81\x86!"));
  char hdr[k_mg_trn_label_cap]    = {};
  char body[k_mg_trn_preview_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 mg_translate_format_popup_header(&trn->popup, hdr, (uint32_t)sizeof(hdr)));
  TEST_ASSERT_EQ(k_ra8_ok,
                 mg_translate_format_popup_body(&trn->popup, body, (uint32_t)sizeof(body)));
  TEST_ASSERT_EQ(0, strcmp(hdr, "ORIG 96.5% (#1)"));
  TEST_ASSERT_EQ(0, strcmp(body, "[U+304A][U+306F][U+3088][U+3046]!"));
  TEST_ASSERT_EQ(k_ra8_ok, mg_reader_render(&s_reader));
  TEST_ASSERT(crc_no_popup != mg_fnv1a(s_fb, (uint32_t)sizeof(s_fb)));

  TEST_ASSERT(mg_reader_tap(&s_reader, (int32_t)k_mt_b1_panel_x, (int32_t)k_mt_b1_panel_y));
  TEST_ASSERT(trn->popup.open);
  TEST_ASSERT_EQ(1U, trn->popup.reading_order);
  TEST_ASSERT_EQ((uint16_t)k_mg_trn_fix_b1_conf_bps, trn->popup.ocr_confidence_bps);
  TEST_ASSERT_EQ(k_ra8_ok,
                 mg_translate_format_popup_header(&trn->popup, hdr, (uint32_t)sizeof(hdr)));
  TEST_ASSERT_EQ(0, strcmp(hdr, "ORIG 88.4% (#2)"));

  TEST_ASSERT(mg_reader_tap(&s_reader, (int32_t)k_mt_b1_panel_x, (int32_t)k_mt_b1_panel_y));
  TEST_ASSERT(!trn->popup.open);

  TEST_ASSERT(mg_reader_tap(&s_reader, (int32_t)k_mt_zoom_bg_x, (int32_t)k_mt_zoom_bg_y));
  TEST_ASSERT_EQ((uint8_t)k_mg_zoom_fit, s_reader.zoom);
  TEST_ASSERT(mg_reader_tap(&s_reader, (int32_t)k_mt_fit_b1_x, (int32_t)k_mt_fit_b1_y));
  TEST_ASSERT(trn->popup.open);
  TEST_ASSERT_EQ(1U, trn->popup.reading_order);
  TEST_ASSERT(mg_reader_tap(&s_reader, (int32_t)k_mt_zoom_bg_x, (int32_t)k_mt_zoom_bg_y));
  TEST_ASSERT(!trn->popup.open);
  TEST_ASSERT(mg_reader_tap(&s_reader, (int32_t)k_mt_zoom_bg_x, (int32_t)k_mt_zoom_bg_y));
  TEST_ASSERT_EQ((uint8_t)k_mg_zoom_full, s_reader.zoom);
  TEST_ASSERT(mg_reader_tap(&s_reader, (int32_t)k_mt_pan_r_x, (int32_t)k_mt_pan_r_y));
  TEST_ASSERT_EQ(256, s_reader.view_x);
  TEST_ASSERT(mg_reader_tap(&s_reader, (int32_t)k_mt_b1_panel_x - 256, (int32_t)k_mt_b1_panel_y));
  TEST_ASSERT(trn->popup.open);
  TEST_ASSERT_EQ(1U, trn->popup.reading_order);
  TEST_END("ereader_manga_translate: bubble tap source + OCR confidence");
}

static void test_trn_queued_translating_markers_and_fallbacks(void)
{
  TEST_BEGIN("ereader_manga_translate: queued markers and missing/stale fallback");
  mt_setup_reader();
  TEST_ASSERT(mg_translate_fixture_attach(&s_reader));
  mg_translate_ctx_t* trn = s_reader.trn;
  TEST_ASSERT(mg_reader_tap(&s_reader, (int32_t)k_mt_toggle_x, (int32_t)k_mt_bar_y));

  char chap_lbl[k_mg_trn_label_cap] = {};
  char mode_lbl[k_mg_trn_label_cap] = {};
  TEST_ASSERT_EQ(k_mg_trn_job_none, mg_translate_query_chapter_marker(trn, 0U));
  TEST_ASSERT_EQ(k_mg_trn_job_queued, mg_translate_query_chapter_marker(trn, 1U));
  TEST_ASSERT_EQ(k_mg_trn_job_translating, mg_translate_query_chapter_marker(trn, 2U));
  TEST_ASSERT_EQ(k_mg_trn_job_none, mg_translate_query_chapter_marker(trn, 3U));

  TEST_ASSERT(mg_reader_tap(&s_reader, (int32_t)k_mt_chap_x, (int32_t)k_mt_bar_y));
  TEST_ASSERT_EQ(k_mg_trn_sidecar_none, mg_translate_sidecar_state(trn));
  TEST_ASSERT(mg_translate_is_fallback_active(trn));
  TEST_ASSERT(!mg_translate_overlays_active(trn));
  TEST_ASSERT_EQ(k_ra8_ok, mg_reader_render(&s_reader));
  TEST_ASSERT(mt_content_matches_orig());
  TEST_ASSERT_EQ(k_ra8_ok,
                 mg_translate_format_chapter_pill(trn, chap_lbl, (uint32_t)sizeof(chap_lbl)));
  TEST_ASSERT_EQ(k_ra8_ok,
                 mg_translate_format_mode_pill(trn, mode_lbl, (uint32_t)sizeof(mode_lbl)));
  TEST_ASSERT_EQ(0, strcmp(chap_lbl, "CH2:QUEUED"));
  TEST_ASSERT_EQ(0, strcmp(mode_lbl, "ORIG!"));

  TEST_ASSERT(mg_reader_tap(&s_reader, (int32_t)k_mt_chap_x, (int32_t)k_mt_bar_y));
  TEST_ASSERT_EQ(k_ra8_ok,
                 mg_translate_format_chapter_pill(trn, chap_lbl, (uint32_t)sizeof(chap_lbl)));
  TEST_ASSERT_EQ(0, strcmp(chap_lbl, "CH3:TRANS.."));

  TEST_ASSERT(mg_reader_tap(&s_reader, (int32_t)k_mt_chap_x, (int32_t)k_mt_bar_y));
  TEST_ASSERT_EQ(k_mg_trn_sidecar_stale, mg_translate_sidecar_state(trn));
  TEST_ASSERT(mg_translate_is_fallback_active(trn));
  TEST_ASSERT_EQ(k_ra8_ok, mg_reader_render(&s_reader));
  TEST_ASSERT(mt_content_matches_orig());
  TEST_ASSERT_EQ(k_ra8_ok,
                 mg_translate_format_chapter_pill(trn, chap_lbl, (uint32_t)sizeof(chap_lbl)));
  TEST_ASSERT_EQ(0, strcmp(chap_lbl, "CH4:STALE"));

  TEST_ASSERT(mg_reader_tap(&s_reader, (int32_t)k_mt_chap_x, (int32_t)k_mt_bar_y));
  TEST_ASSERT_EQ(0U, trn->active_chapter);
  TEST_ASSERT(mg_translate_overlays_active(trn));
  TEST_END("ereader_manga_translate: queued markers and missing/stale fallback");
}

static void test_trn_corrupt_and_error_guards(void)
{
  TEST_BEGIN("ereader_manga_translate: corrupt sidecar and defensive error guards");
  mt_setup_reader();
  TEST_ASSERT(mg_translate_fixture_attach(&s_reader));
  mg_translate_ctx_t* src_ctx = s_reader.trn;
  static uint8_t      bad_buf[k_mt_trn1_cap];
  const size_t        good_len = src_ctx->chapters[0].sidecar_len;
  TEST_ASSERT((good_len > (size_t)k_mt_corrupt_off) && (good_len <= sizeof(bad_buf)));
  memcpy(bad_buf, src_ctx->chapters[0].sidecar_buf, good_len);
  bad_buf[k_mt_corrupt_off] ^= 0xFFU;

  mg_translate_ctx_t local = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, mg_translate_init(nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, mg_translate_init(&local));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, mg_translate_bind_job(&local, bad_buf, 0U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, mg_translate_bind_job(&local, nullptr, 4U));

  const mg_trn_chapter_t corrupt_ch = {.chapter_order = 9U,
                                       .expected_key  = src_ctx->chapters[0].expected_key,
                                       .sidecar_buf   = bad_buf,
                                       .sidecar_len   = good_len,
                                       .static_marker = k_mg_trn_job_paused};
  TEST_ASSERT_EQ(k_ra8_ok, mg_translate_open_title(&local, 77U, &corrupt_ch, 1U, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, mg_translate_set_title_mode(&local, 77U, true));
  TEST_ASSERT_EQ(k_mg_trn_sidecar_corrupt, mg_translate_sidecar_state(&local));
  TEST_ASSERT(mg_translate_is_fallback_active(&local));
  TEST_ASSERT(!mg_translate_overlays_active(&local));
  char lbl[k_mg_trn_label_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok, mg_translate_format_chapter_pill(&local, lbl, (uint32_t)sizeof(lbl)));
  TEST_ASSERT_EQ(0, strcmp(lbl, "CH9:PAUSED"));

  mg_trn_chapter_t oob_page = src_ctx->chapters[0];
  oob_page.page_index       = 99U;
  TEST_ASSERT_EQ(k_ra8_ok, mg_translate_open_title(&local, 78U, &oob_page, 1U, 0U));
  TEST_ASSERT_EQ(k_mg_trn_sidecar_stale, mg_translate_sidecar_state(&local));
  TEST_END("ereader_manga_translate: corrupt sidecar and defensive error guards");
}

int main(void)
{
  ra8_log_set_byte_sink(mt_silent_log, nullptr);
  test_trn_unbound_baseline();
  test_trn_per_title_toggle();
  test_trn_bubble_tap_and_fit_coords();
  test_trn_queued_translating_markers_and_fallbacks();
  test_trn_corrupt_and_error_guards();
  (void)internal_test_output_fd_text(STDERR_FILENO, "[OK  ] test_app_ereader_manga_translate.c\n");
  ra8_log_set_byte_sink(nullptr, nullptr);
  return 0;
}
