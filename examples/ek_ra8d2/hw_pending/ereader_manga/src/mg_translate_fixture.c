/**
 * @file src/mg_translate_fixture.c
 * @brief Golden chapter `TRN1` sidecar + queued/translating job fixture builder.
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "mg_translate_fixture.h"

#include <string.h>

#include "ra8_gfx.h"
#include "ra8_gfx_font.h"
#include "ra8_translate.h"
#include "ra8_translate_job.h"
#include "ra8_translate_overlay.h"

typedef enum : uint32_t {
  k_mg_fix_work_cap   = 131072U,
  k_mg_fix_trn1_cap   = 4096U,
  k_mg_fix_pool_cap   = 512U,
  k_mg_fix_breaks_cap = 64U,
  k_mg_fix_bubbles    = 2U,
  k_mg_fix_page_w     = 1536U,
  k_mg_fix_page_h     = 2048U,
  k_mg_fix_b0_x       = 80U,
  k_mg_fix_b0_y       = 90U,
  k_mg_fix_b0_w       = 260U,
  k_mg_fix_b0_h       = 140U,
  k_mg_fix_b1_x       = 420U,
  k_mg_fix_b1_y       = 180U,
  k_mg_fix_b1_w       = 280U,
  k_mg_fix_b1_h       = 140U,
  k_mg_fix_model_ver  = 1U,
  k_mg_fix_batt_pct   = 90U,
  k_mg_fix_ch_queued  = 2U,
  k_mg_fix_ch_active  = 3U,
  k_mg_fix_ch_stale   = 4U,
  k_mg_fix_hash_seed1 = 0x41U,
  k_mg_fix_hash_seed4 = 0x99U,
  k_mg_fix_radius     = 12U,
} mg_fix_arena_const_t;

[[gnu::section(".sdram_data"), gnu::aligned(8)]] static uint8_t s_job_work[k_mg_fix_work_cap];
[[gnu::section(".sdram_data")]] static uint8_t                  s_trn1_ch1[k_mg_fix_trn1_cap];
[[gnu::section(".sdram_data")]] static uint8_t                  s_trn1_ch4[k_mg_fix_trn1_cap];

static size_t             s_trn1_ch1_len = 0U;
static size_t             s_trn1_ch4_len = 0U;
static size_t             s_job_need     = 0U;
static mg_translate_ctx_t s_ctx;
static bool               s_attached = false;

static void mg_fix_text_size(void* user, const char* str, int32_t* out_w, int32_t* out_h)
{
  (void)user;
  if ((str == nullptr) || (out_w == nullptr) || (out_h == nullptr)) {
    return;
  }
  uint32_t w = 0U;
  uint32_t h = 0U;
  if (ra8_gfx_text_size(str, &ra8_gfx_font_8x16, &w, &h) == k_ra8_ok) {
    *out_w = (w > (uint32_t)INT32_MAX) ? INT32_MAX : (int32_t)w;
    *out_h = (h > (uint32_t)INT32_MAX) ? INT32_MAX : (int32_t)h;
  }
}

static void mg_fix_fill_noop(void* user, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
  (void)user;
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  (void)color;
}

static void
mg_fix_draw_noop(void* user, int32_t x, int32_t y, const char* str, uint32_t fg, uint32_t bg)
{
  (void)user;
  (void)x;
  (void)y;
  (void)str;
  (void)fg;
  (void)bg;
}

static const ra8_widget_paint_t k_mg_fix_paint = {.user      = nullptr,
                                                  .fill_rect = mg_fix_fill_noop,
                                                  .draw_text = mg_fix_draw_noop,
                                                  .text_size = mg_fix_text_size};

static ra8_translate_sidecar_key_t mg_fix_make_key(uint8_t hash_seed)
{
  ra8_translate_sidecar_key_t key = {
    .detector_model_version = (uint32_t)k_mg_fix_model_ver,
    .ocr_model_version      = (uint32_t)k_mg_fix_model_ver,
    .nmt_model_version      = (uint32_t)k_mg_fix_model_ver,
    .glossary_revision      = (uint32_t)k_mg_fix_model_ver,
    .source_lang            = k_ra8_translate_lang_japanese,
  };
  memset(key.chapter_hash, (int)hash_seed, sizeof(key.chapter_hash));
  return key;
}

static bool mg_fix_append_pool(uint8_t*       pool,
                               size_t*        pool_pos,
                               const uint8_t* text,
                               size_t         len,
                               uint32_t*      out_off)
{
  if ((*pool_pos + len) > (size_t)k_mg_fix_pool_cap) {
    return false;
  }
  *out_off = (uint32_t)(*pool_pos);
  memcpy(&pool[*pool_pos], text, len);
  *pool_pos += len;
  return true;
}

static bool mg_fix_build_bubble(const ra8_translate_overlay_bubble_spec_t* spec,
                                const ra8_translate_overlay_style_t*       style,
                                const char*                                src_utf8,
                                const char*                                eng_utf8,
                                uint16_t                                   order,
                                uint16_t                                   conf_bps,
                                uint16_t*                                  breaks,
                                size_t*                                    break_pos,
                                uint8_t*                                   pool,
                                size_t*                                    pool_pos,
                                ra8_translate_sidecar_bubble_t*            out_b)
{
  const size_t                src_len = strlen(src_utf8);
  const size_t                eng_len = strlen(eng_utf8);
  ra8_translate_overlay_fit_t fit     = {};
  if (ra8_translate_overlay_fit_text(&k_mg_fix_paint,
                                     spec,
                                     style,
                                     (const uint8_t*)eng_utf8,
                                     eng_len,
                                     &breaks[*break_pos],
                                     (size_t)k_mg_fix_breaks_cap - *break_pos,
                                     &fit) != k_ra8_ok) {
    return false;
  }
  uint32_t src_off = 0U;
  uint32_t eng_off = 0U;
  if (!mg_fix_append_pool(pool, pool_pos, (const uint8_t*)src_utf8, src_len, &src_off) ||
      !mg_fix_append_pool(pool, pool_pos, (const uint8_t*)eng_utf8, eng_len, &eng_off)) {
    return false;
  }
  *out_b = (ra8_translate_sidecar_bubble_t){.x     = spec->x,
                                            .y     = spec->y,
                                            .w     = spec->w,
                                            .h     = spec->h,
                                            .shape = (ra8_translate_shape_t)spec->shape,
                                            .text_orientation =
                                              (ra8_translate_orientation_t)spec->text_orientation,
                                            .corner_radius      = spec->corner_radius,
                                            .reading_order      = order,
                                            .ocr_confidence_bps = conf_bps,
                                            .font_size_px       = fit.font_size_px,
                                            .line_count         = fit.line_count,
                                            .first_line_break   = (uint32_t)(*break_pos),
                                            .source_text_off    = src_off,
                                            .source_text_len    = (uint32_t)src_len,
                                            .english_text_off   = eng_off,
                                            .english_text_len   = (uint32_t)eng_len};
  *break_pos += (size_t)fit.line_count;
  return true;
}

static bool mg_fix_build_trn1(uint8_t hash_seed, uint8_t* out_buf, size_t* out_len)
{
  const ra8_translate_sidecar_key_t         key  = mg_fix_make_key(hash_seed);
  const ra8_translate_sidecar_page_t        page = {.page_index   = 0U,
                                                    .bubble_count = (uint16_t)k_mg_fix_bubbles,
                                                    .first_bubble = 0U,
                                                    .page_width   = (uint16_t)k_mg_fix_page_w,
                                                    .page_height  = (uint16_t)k_mg_fix_page_h};
  const ra8_translate_overlay_bubble_spec_t s0   = {.x = (uint16_t)k_mg_fix_b0_x,
                                                    .y = (uint16_t)k_mg_fix_b0_y,
                                                    .w = (uint16_t)k_mg_fix_b0_w,
                                                    .h = (uint16_t)k_mg_fix_b0_h,
                                                    .shape =
                                                      (uint8_t)k_ra8_translate_shape_rounded_rect,
                                                    .corner_radius = (uint16_t)k_mg_fix_radius};
  const ra8_translate_overlay_bubble_spec_t s1   = {.x     = (uint16_t)k_mg_fix_b1_x,
                                                    .y     = (uint16_t)k_mg_fix_b1_y,
                                                    .w     = (uint16_t)k_mg_fix_b1_w,
                                                    .h     = (uint16_t)k_mg_fix_b1_h,
                                                    .shape = (uint8_t)k_ra8_translate_shape_rect};
  /* Pure 7-bit ASCII source: UTF-8 "Ohayou!" in Hiragana followed by ASCII tag. */
  static const char              k_src0[] = "\xE3\x81\x8A\xE3\x81\xAF\xE3\x82\x88\xE3\x81\x86!";
  static const char              k_src1[] = "\xE5\x85\x88\xE8\xBC\xA9";
  uint16_t                       breaks[k_mg_fix_breaks_cap] = {};
  uint8_t                        pool[k_mg_fix_pool_cap]     = {};
  size_t                         bpos                        = 0U;
  size_t                         ppos                        = 0U;
  ra8_translate_sidecar_bubble_t bubbles[k_mg_fix_bubbles]   = {};
  if (!mg_fix_build_bubble(&s0,
                           &s_ctx.overlay_style,
                           k_src0,
                           "Good morning!",
                           0U,
                           (uint16_t)k_mg_trn_fix_b0_conf_bps,
                           breaks,
                           &bpos,
                           pool,
                           &ppos,
                           &bubbles[0]) ||
      !mg_fix_build_bubble(&s1,
                           &s_ctx.overlay_style,
                           k_src1,
                           "Let us head out!",
                           1U,
                           (uint16_t)k_mg_trn_fix_b1_conf_bps,
                           breaks,
                           &bpos,
                           pool,
                           &ppos,
                           &bubbles[1])) {
    return false;
  }
  return ra8_translate_sidecar_write(&key,
                                     &page,
                                     1U,
                                     bubbles,
                                     (uint32_t)k_mg_fix_bubbles,
                                     breaks,
                                     (uint32_t)bpos,
                                     pool,
                                     (uint32_t)ppos,
                                     out_buf,
                                     (size_t)k_mg_fix_trn1_cap,
                                     out_len) == k_ra8_ok;
}

static ra8_err_t mg_fix_ocr_stub(void*                              user,
                                 const ra8_translate_job_chapter_t* chapter,
                                 uint16_t                           page_index,
                                 uint16_t                           max_bubbles,
                                 uint16_t*                          out_page_w,
                                 uint16_t*                          out_page_h,
                                 ra8_translate_job_ocr_bubble_t*    out_bubbles,
                                 uint16_t*                          out_bubble_count,
                                 uint8_t*                           out_text_buf,
                                 size_t                             text_cap,
                                 size_t*                            out_text_len)
{
  (void)user;
  (void)chapter;
  (void)page_index;
  if ((max_bubbles < 1U) || (text_cap < 4U)) {
    return k_ra8_err_invalid_size;
  }
  *out_page_w       = (uint16_t)k_mg_fix_page_w;
  *out_page_h       = (uint16_t)k_mg_fix_page_h;
  *out_bubble_count = 1U;
  out_bubbles[0] =
    (ra8_translate_job_ocr_bubble_t){.x                  = (uint16_t)k_mg_fix_b0_x,
                                     .y                  = (uint16_t)k_mg_fix_b0_y,
                                     .w                  = (uint16_t)k_mg_fix_b0_w,
                                     .h                  = (uint16_t)k_mg_fix_b0_h,
                                     .ocr_confidence_bps = (uint16_t)k_mg_trn_fix_b0_conf_bps,
                                     .source_off         = 0U,
                                     .source_len         = 3U};
  memcpy(out_text_buf, "\xE3\x81\x8A", 3U);
  *out_text_len = 3U;
  return k_ra8_ok;
}

static ra8_err_t mg_fix_batch_stub(void*                           user,
                                   ra8_translate_nmt_model_t       nmt_model,
                                   const uint8_t*                  src_buf,
                                   size_t                          src_bytes,
                                   const ra8_translate_job_span_t* src_spans,
                                   uint16_t                        item_count,
                                   uint8_t*                        out_eng_buf,
                                   size_t                          out_eng_cap,
                                   ra8_translate_job_span_t*       out_eng_spans)
{
  (void)user;
  (void)nmt_model;
  (void)src_buf;
  (void)src_bytes;
  (void)src_spans;
  if ((item_count != 1U) || (out_eng_cap < 2U)) {
    return k_ra8_err_invalid_size;
  }
  memcpy(out_eng_buf, "Hi", 2U);
  out_eng_spans[0] = (ra8_translate_job_span_t){.off = 0U, .len = 2U};
  return k_ra8_ok;
}

static ra8_err_t mg_fix_commit_stub(void*                              user,
                                    const ra8_translate_job_chapter_t* chapter,
                                    const uint8_t*                     trn1_bytes,
                                    size_t                             trn1_len,
                                    const uint8_t*                     tgl1_bytes,
                                    size_t                             tgl1_len)
{
  (void)user;
  (void)chapter;
  (void)trn1_bytes;
  (void)trn1_len;
  (void)tgl1_bytes;
  (void)tgl1_len;
  return k_ra8_ok;
}

static bool mg_fix_setup_jobs(void)
{
  if ((ra8_translate_job_work_bytes(&s_job_need) != k_ra8_ok) ||
      (s_job_need > (size_t)k_mg_fix_work_cap)) {
    return false;
  }
  const ra8_translate_job_policy_t pol = {.battery_min_pct  = 25U,
                                          .nmt_batch_max    = 8U,
                                          .yield_to_reader  = 1U,
                                          .yield_to_display = 1U,
                                          .overlay_style    = s_ctx.overlay_style};
  if (ra8_translate_job_init(s_job_work, s_job_need, &pol) != k_ra8_ok) {
    return false;
  }
  const ra8_translate_job_chapter_t ch3   = {.sidecar_key   = mg_fix_make_key(3U),
                                             .title_id      = (uint32_t)k_mg_trn_fix_title_id,
                                             .chapter_order = (uint32_t)k_mg_fix_ch_active,
                                             .page_count    = 2U,
                                             .lang_hint     = k_ra8_translate_lang_japanese,
                                             .unread        = 1U,
                                             .trigger = k_ra8_translate_job_trigger_user_request};
  const ra8_translate_job_chapter_t ch2   = {.sidecar_key   = mg_fix_make_key(2U),
                                             .title_id      = (uint32_t)k_mg_trn_fix_title_id,
                                             .chapter_order = (uint32_t)k_mg_fix_ch_queued,
                                             .page_count    = 2U,
                                             .lang_hint     = k_ra8_translate_lang_japanese,
                                             .unread        = 1U,
                                             .trigger = k_ra8_translate_job_trigger_mdl_download};
  const ra8_translate_job_hooks_t   hooks = {.paint           = &k_mg_fix_paint,
                                             .ocr_page        = mg_fix_ocr_stub,
                                             .translate_batch = mg_fix_batch_stub,
                                             .commit_chapter  = mg_fix_commit_stub};
  const ra8_translate_job_env_t     env   = {.battery_pct = (uint8_t)k_mg_fix_batt_pct};
  ra8_translate_job_status_t        st    = {};
  if ((ra8_translate_job_enqueue(s_job_work, s_job_need, &ch3) != k_ra8_ok) ||
      (ra8_translate_job_step(s_job_work, s_job_need, &env, &hooks, &st) != k_ra8_ok) ||
      (ra8_translate_job_enqueue(s_job_work, s_job_need, &ch2) != k_ra8_ok)) {
    return false;
  }
  return mg_translate_bind_job(&s_ctx, s_job_work, s_job_need) == k_ra8_ok;
}

bool mg_translate_fixture_attach(mg_reader_t* r)
{
  if ((r == nullptr) || (mg_translate_init(&s_ctx) != k_ra8_ok)) {
    return false;
  }
  if (!mg_fix_build_trn1((uint8_t)k_mg_fix_hash_seed1, s_trn1_ch1, &s_trn1_ch1_len) ||
      !mg_fix_build_trn1((uint8_t)k_mg_fix_hash_seed4, s_trn1_ch4, &s_trn1_ch4_len) ||
      !mg_fix_setup_jobs()) {
    return false;
  }
  const mg_trn_chapter_t chapters[k_mg_trn_fix_chapter_count] = {
    {.chapter_order = 1U,
     .expected_key  = mg_fix_make_key((uint8_t)k_mg_fix_hash_seed1),
     .sidecar_buf   = s_trn1_ch1,
     .sidecar_len   = s_trn1_ch1_len,
     .page_index    = 0U},
    {.chapter_order = (uint32_t)k_mg_fix_ch_queued, .expected_key = mg_fix_make_key(2U)},
    {.chapter_order = (uint32_t)k_mg_fix_ch_active, .expected_key = mg_fix_make_key(3U)},
    {.chapter_order = (uint32_t)k_mg_fix_ch_stale,
     .expected_key  = mg_fix_make_key((uint8_t)(k_mg_fix_hash_seed4 + 1U)),
     .sidecar_buf   = s_trn1_ch4,
     .sidecar_len   = s_trn1_ch4_len,
     .page_index    = 0U},
  };
  if (mg_translate_open_title(&s_ctx,
                              (uint32_t)k_mg_trn_fix_title_id,
                              chapters,
                              (size_t)k_mg_trn_fix_chapter_count,
                              0U) != k_ra8_ok) {
    return false;
  }
  mg_reader_set_translate(r, &s_ctx);
  s_attached = true;
  return true;
}

const mg_translate_ctx_t* mg_translate_fixture_ctx(void)
{
  return s_attached ? &s_ctx : nullptr;
}
