/**
 * @file src/mg_translate.c
 * @brief Manga reader translation state, overlay paint adapter, bubble inspect
 *        popup, queued/translating chapter chrome, and missing/stale fallback.
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "mg_translate.h"

#include <string.h>

#include "ra8_check.h"
#include "ra8_gfx.h"
#include "ra8_gfx_font.h"
#include "ra8_translate_overlay.h"

/** @enum mg_trn_fmt_const_t @brief Numeric formatting and UTF-8 bit constants. */
typedef enum : uint32_t {
  k_mg_trn_dec_base      = 10U,
  k_mg_trn_hex_base      = 16U,
  k_mg_trn_dec_digits    = 12U,
  k_mg_trn_bps_per_pct   = 100U,
  k_mg_trn_tenths_div    = 10U,
  k_mg_trn_ascii_lo      = 0x20U,
  k_mg_trn_ascii_hi      = 0x7EU,
  k_mg_trn_utf8_2_min    = 0xC2U,
  k_mg_trn_utf8_2_max    = 0xDFU,
  k_mg_trn_utf8_3_min    = 0xE0U,
  k_mg_trn_utf8_3_max    = 0xEFU,
  k_mg_trn_utf8_4_min    = 0xF0U,
  k_mg_trn_utf8_4_max    = 0xF4U,
  k_mg_trn_utf8_cont_msk = 0xC0U,
  k_mg_trn_utf8_cont_val = 0x80U,
  k_mg_trn_utf8_low6_msk = 0x3FU,
  k_mg_trn_utf8_m5_msk   = 0x1FU,
  k_mg_trn_utf8_m4_msk   = 0x0FU,
  k_mg_trn_utf8_m3_msk   = 0x07U,
  k_mg_trn_hex_nib_bits  = 4U,
  k_mg_trn_hex_nib_mask  = 0xFU,
  k_mg_trn_min_hex_cols  = 4U,
  k_mg_trn_max_hex_cols  = 6U,
  k_mg_trn_glyph_w       = 8U,
  k_mg_trn_glyph_h       = 16U,
  k_mg_trn_shift_6       = 6U,
  k_mg_trn_shift_12      = 12U,
  k_mg_trn_shift_18      = 18U,
  k_mg_trn_default_pad   = 4U,
  k_mg_trn_default_bdr_w = 2U,
} mg_trn_fmt_const_t;

static_assert((uint32_t)k_mg_trn_job_none == (uint32_t)k_ra8_translate_job_chapter_none,
              "job none parity");
static_assert((uint32_t)k_mg_trn_job_queued == (uint32_t)k_ra8_translate_job_chapter_queued,
              "job queued parity");
static_assert((uint32_t)k_mg_trn_job_translating ==
                (uint32_t)k_ra8_translate_job_chapter_translating,
              "job translating parity");
static_assert((uint32_t)k_mg_trn_job_paused == (uint32_t)k_ra8_translate_job_chapter_paused,
              "job paused parity");

/** @brief Shared paint adapter state passed through `ra8_widget_paint_t::user`. */
typedef struct {
  const mg_trn_view_t* view;
} mg_trn_paint_ctx_t;

/* ===========================================================================
 * Bounded string and coordinate helpers
 * =========================================================================== */

static uint32_t mg_trn_str_cat(char* buf, uint32_t cap, uint32_t pos, const char* s)
{
  if ((buf == nullptr) || (cap == 0U) || (s == nullptr)) {
    return pos;
  }
  for (uint32_t i = 0U; (s[i] != '\0') && (pos < (cap - 1U)); ++i) {
    buf[pos++] = s[i];
  }
  buf[pos] = '\0';
  return pos;
}

static uint32_t mg_trn_uint_cat(char* buf, uint32_t cap, uint32_t pos, uint32_t val)
{
  if ((buf == nullptr) || (cap == 0U)) {
    return pos;
  }
  char     tmp[k_mg_trn_dec_digits] = {};
  uint32_t n                        = 0U;
  if (val == 0U) {
    tmp[n++] = '0';
  }
  while ((val > 0U) && (n < (uint32_t)k_mg_trn_dec_digits)) {
    tmp[n++] = (char)('0' + (val % (uint32_t)k_mg_trn_dec_base));
    val /= (uint32_t)k_mg_trn_dec_base;
  }
  for (uint32_t i = 0U; (i < n) && (pos < (cap - 1U)); ++i) {
    buf[pos++] = tmp[n - 1U - i];
  }
  buf[pos] = '\0';
  return pos;
}

static uint32_t mg_trn_cp_hex_cat(char* buf, uint32_t cap, uint32_t pos, uint32_t cp)
{
  pos                = mg_trn_str_cat(buf, cap, pos, "[U+");
  uint32_t digits    = (uint32_t)k_mg_trn_min_hex_cols;
  uint32_t threshold = 1U << ((uint32_t)k_mg_trn_hex_nib_bits * (uint32_t)k_mg_trn_min_hex_cols);
  while ((digits < (uint32_t)k_mg_trn_max_hex_cols) && (cp >= threshold)) {
    digits++;
    threshold <<= (uint32_t)k_mg_trn_hex_nib_bits;
  }
  for (uint32_t i = 0U; (i < digits) && (pos < (cap - 1U)); ++i) {
    const uint32_t shift = (digits - 1U - i) * (uint32_t)k_mg_trn_hex_nib_bits;
    const uint32_t nib   = (cp >> shift) & (uint32_t)k_mg_trn_hex_nib_mask;
    const char     ch =
      (char)((nib < (uint32_t)k_mg_trn_dec_base) ? ('0' + nib) : ('A' + (nib - k_mg_trn_dec_base)));
    buf[pos++] = ch;
  }
  return mg_trn_str_cat(buf, cap, pos, "]");
}

static int32_t mg_trn_floor_div(int32_t a, int32_t b)
{
  const int32_t div = (b > 0) ? b : 1;
  int32_t       q   = a / div;
  const int32_t r   = a % div;
  if ((r != 0) && (a < 0)) {
    q -= 1;
  }
  return q;
}

/* ===========================================================================
 * Per-title toggle storage & sidecar verification
 * =========================================================================== */

static int32_t mg_trn_find_title(const mg_translate_ctx_t* ctx, uint32_t title_id)
{
  if ((ctx == nullptr) || (title_id == 0U)) {
    return -1;
  }
  for (size_t i = 0U; i < (size_t)k_mg_trn_max_titles; ++i) {
    if (ctx->prefs[i].occupied && (ctx->prefs[i].title_id == title_id)) {
      return (int32_t)i;
    }
  }
  return -1;
}

static size_t mg_trn_alloc_title_slot(mg_translate_ctx_t* ctx, uint32_t title_id)
{
  const int32_t hit = mg_trn_find_title(ctx, title_id);
  if (hit >= 0) {
    return (size_t)hit;
  }
  for (size_t i = 0U; i < (size_t)k_mg_trn_max_titles; ++i) {
    if (!ctx->prefs[i].occupied) {
      return i;
    }
  }
  const size_t victim = ctx->lru_next % (size_t)k_mg_trn_max_titles;
  ctx->lru_next       = (victim + 1U) % (size_t)k_mg_trn_max_titles;
  return victim;
}

static mg_trn_sidecar_state_t mg_trn_eval_chapter(const mg_trn_chapter_t* ch)
{
  if ((ch == nullptr) || (ch->sidecar_buf == nullptr) || (ch->sidecar_len == 0U)) {
    return k_mg_trn_sidecar_none;
  }
  ra8_translate_sidecar_header_t hdr = {};
  if (ra8_translate_sidecar_open(ch->sidecar_buf, ch->sidecar_len, &hdr) != k_ra8_ok) {
    return k_mg_trn_sidecar_corrupt;
  }
  bool matches = false;
  if ((ra8_translate_sidecar_key_matches(&hdr, &ch->expected_key, &matches) != k_ra8_ok) ||
      !matches) {
    return k_mg_trn_sidecar_stale;
  }
  if (ch->page_index >= hdr.page_count) {
    return k_mg_trn_sidecar_stale;
  }
  return k_mg_trn_sidecar_ready;
}

static void mg_trn_refresh_active_sidecar(mg_translate_ctx_t* ctx)
{
  ctx->popup             = (mg_translate_popup_t){};
  ctx->last_damage_count = 0U;
  if ((ctx->title_id == 0U) || (ctx->active_chapter >= ctx->chapter_count)) {
    ctx->active_sidecar_state = k_mg_trn_sidecar_none;
    return;
  }
  ctx->active_sidecar_state = mg_trn_eval_chapter(&ctx->chapters[ctx->active_chapter]);
}

ra8_err_t mg_translate_init(mg_translate_ctx_t* ctx)
{
  RA8_CHECK_NULL_PTR(ctx, "MG_TRN", "ctx");
  *ctx               = (mg_translate_ctx_t){};
  ctx->overlay_style = (ra8_translate_overlay_style_t){
    .fg_color     = k_ra8_translate_overlay_default_fg,
    .bg_color     = k_ra8_translate_overlay_default_bg,
    .border_color = k_ra8_translate_overlay_default_border,
    .pad          = (int16_t)k_mg_trn_default_pad,
    .border_w     = (int16_t)k_mg_trn_default_bdr_w,
    .line_gap     = 2,
    .align        = k_ra8_widget_align_center,
    .face         = k_ra8_widget_text_face_sans,
    .weight       = k_ra8_widget_text_weight_regular,
  };
  return k_ra8_ok;
}

ra8_err_t mg_translate_bind_job(mg_translate_ctx_t* ctx, const uint8_t* work, size_t work_bytes)
{
  RA8_CHECK_NULL_PTR(ctx, "MG_TRN", "ctx");
  if ((work == nullptr) && (work_bytes != 0U)) {
    return k_ra8_err_null_ptr;
  }
  if ((work != nullptr) && (work_bytes == 0U)) {
    return k_ra8_err_invalid_size;
  }
  ctx->job_work       = work;
  ctx->job_work_bytes = work_bytes;
  return k_ra8_ok;
}

ra8_err_t mg_translate_open_title(mg_translate_ctx_t*     ctx,
                                  uint32_t                title_id,
                                  const mg_trn_chapter_t* chapters,
                                  size_t                  chapter_count,
                                  size_t                  initial_chapter)
{
  RA8_CHECK_NULL_PTR(ctx, "MG_TRN", "ctx");
  RA8_CHECK_NULL_PTR(chapters, "MG_TRN", "chapters");
  if ((title_id == 0U) || (chapter_count == 0U) ||
      (chapter_count > (size_t)k_mg_trn_max_chapters) || (initial_chapter >= chapter_count)) {
    return k_ra8_err_invalid_arg;
  }
  ctx->title_id       = title_id;
  ctx->chapter_count  = chapter_count;
  ctx->active_chapter = initial_chapter;
  for (size_t i = 0U; i < chapter_count; ++i) {
    ctx->chapters[i] = chapters[i];
  }
  if (mg_trn_find_title(ctx, title_id) < 0) {
    const size_t slot = mg_trn_alloc_title_slot(ctx, title_id);
    ctx->prefs[slot] =
      (mg_trn_title_pref_t){.title_id = title_id, .show_translated = false, .occupied = true};
  }
  mg_trn_refresh_active_sidecar(ctx);
  return k_ra8_ok;
}

ra8_err_t mg_translate_select_chapter(mg_translate_ctx_t* ctx, size_t chapter_index)
{
  RA8_CHECK_NULL_PTR(ctx, "MG_TRN", "ctx");
  if ((ctx->title_id == 0U) || (chapter_index >= ctx->chapter_count)) {
    return k_ra8_err_range_check_failed;
  }
  ctx->active_chapter = chapter_index;
  mg_trn_refresh_active_sidecar(ctx);
  return k_ra8_ok;
}

bool mg_translate_next_chapter(mg_translate_ctx_t* ctx)
{
  if ((ctx == nullptr) || (ctx->title_id == 0U) || (ctx->chapter_count <= 1U)) {
    return false;
  }
  const size_t next = (ctx->active_chapter + 1U) % ctx->chapter_count;
  return mg_translate_select_chapter(ctx, next) == k_ra8_ok;
}

bool mg_translate_get_title_mode(const mg_translate_ctx_t* ctx, uint32_t title_id)
{
  const int32_t idx = mg_trn_find_title(ctx, title_id);
  return (idx >= 0) ? ctx->prefs[(size_t)idx].show_translated : false;
}

ra8_err_t
mg_translate_set_title_mode(mg_translate_ctx_t* ctx, uint32_t title_id, bool show_translated)
{
  RA8_CHECK_NULL_PTR(ctx, "MG_TRN", "ctx");
  if (title_id == 0U) {
    return k_ra8_err_invalid_arg;
  }
  const size_t slot = mg_trn_alloc_title_slot(ctx, title_id);
  ctx->prefs[slot]  = (mg_trn_title_pref_t){.title_id        = title_id,
                                            .show_translated = show_translated,
                                            .occupied        = true};
  if (ctx->title_id == title_id) {
    ctx->popup = (mg_translate_popup_t){};
  }
  return k_ra8_ok;
}

bool mg_translate_toggle_mode(mg_translate_ctx_t* ctx)
{
  if ((ctx == nullptr) || (ctx->title_id == 0U)) {
    return false;
  }
  const bool next = !mg_translate_get_title_mode(ctx, ctx->title_id);
  return mg_translate_set_title_mode(ctx, ctx->title_id, next) == k_ra8_ok;
}

mg_trn_sidecar_state_t mg_translate_sidecar_state(const mg_translate_ctx_t* ctx)
{
  return (ctx != nullptr) ? ctx->active_sidecar_state : k_mg_trn_sidecar_none;
}

bool mg_translate_is_fallback_active(const mg_translate_ctx_t* ctx)
{
  if ((ctx == nullptr) || (ctx->title_id == 0U)) {
    return false;
  }
  const bool wants_trn = mg_translate_get_title_mode(ctx, ctx->title_id);
  return wants_trn && (ctx->active_sidecar_state != k_mg_trn_sidecar_ready);
}

bool mg_translate_overlays_active(const mg_translate_ctx_t* ctx)
{
  if ((ctx == nullptr) || (ctx->title_id == 0U)) {
    return false;
  }
  return mg_translate_get_title_mode(ctx, ctx->title_id) &&
         (ctx->active_sidecar_state == k_mg_trn_sidecar_ready);
}

mg_trn_job_marker_t mg_translate_query_chapter_marker(const mg_translate_ctx_t* ctx,
                                                      size_t                    chapter_index)
{
  if ((ctx == nullptr) || (ctx->title_id == 0U) || (chapter_index >= ctx->chapter_count)) {
    return k_mg_trn_job_none;
  }
  const mg_trn_chapter_t* ch = &ctx->chapters[chapter_index];
  if ((ctx->job_work != nullptr) && (ctx->job_work_bytes > 0U)) {
    ra8_translate_job_chapter_state_t st = k_ra8_translate_job_chapter_none;
    if (ra8_translate_job_query_chapter(ctx->job_work,
                                        ctx->job_work_bytes,
                                        ctx->title_id,
                                        ch->chapter_order,
                                        &st) == k_ra8_ok) {
      return (mg_trn_job_marker_t)st;
    }
  }
  return ch->static_marker;
}

/* ===========================================================================
 * Chrome & popup formatters
 * =========================================================================== */

ra8_err_t mg_translate_format_mode_pill(const mg_translate_ctx_t* ctx, char* buf, uint32_t cap)
{
  RA8_CHECK_NULL_PTR(ctx, "MG_TRN", "ctx");
  RA8_CHECK_NULL_PTR(buf, "MG_TRN", "buf");
  if (cap == 0U) {
    return k_ra8_err_invalid_size;
  }
  buf[0] = '\0';
  if (mg_translate_overlays_active(ctx)) {
    (void)mg_trn_str_cat(buf, cap, 0U, "TRN");
  } else if (mg_translate_is_fallback_active(ctx)) {
    (void)mg_trn_str_cat(buf, cap, 0U, "ORIG!");
  } else {
    (void)mg_trn_str_cat(buf, cap, 0U, "ORIG");
  }
  return k_ra8_ok;
}

static const char* mg_trn_sidecar_tag(mg_trn_sidecar_state_t st)
{
  switch (st) {
    case k_mg_trn_sidecar_ready:
      return "READY";
    case k_mg_trn_sidecar_stale:
      return "STALE";
    case k_mg_trn_sidecar_corrupt:
      return "BAD";
    case k_mg_trn_sidecar_none:
    default:
      return "MISS";
  }
}

ra8_err_t mg_translate_format_chapter_pill(const mg_translate_ctx_t* ctx, char* buf, uint32_t cap)
{
  RA8_CHECK_NULL_PTR(ctx, "MG_TRN", "ctx");
  RA8_CHECK_NULL_PTR(buf, "MG_TRN", "buf");
  if (cap == 0U) {
    return k_ra8_err_invalid_size;
  }
  buf[0] = '\0';
  if ((ctx->title_id == 0U) || (ctx->active_chapter >= ctx->chapter_count)) {
    return k_ra8_ok;
  }
  uint32_t pos = mg_trn_str_cat(buf, cap, 0U, "CH");
  pos          = mg_trn_uint_cat(buf, cap, pos, ctx->chapters[ctx->active_chapter].chapter_order);
  pos          = mg_trn_str_cat(buf, cap, pos, ":");
  const mg_trn_job_marker_t mk = mg_translate_query_chapter_marker(ctx, ctx->active_chapter);
  switch (mk) {
    case k_mg_trn_job_queued:
      (void)mg_trn_str_cat(buf, cap, pos, "QUEUED");
      break;
    case k_mg_trn_job_translating:
      (void)mg_trn_str_cat(buf, cap, pos, "TRANS..");
      break;
    case k_mg_trn_job_paused:
      (void)mg_trn_str_cat(buf, cap, pos, "PAUSED");
      break;
    case k_mg_trn_job_none:
    default:
      (void)mg_trn_str_cat(buf, cap, pos, mg_trn_sidecar_tag(ctx->active_sidecar_state));
      break;
  }
  return k_ra8_ok;
}

ra8_err_t
mg_translate_format_popup_header(const mg_translate_popup_t* popup, char* buf, uint32_t cap)
{
  RA8_CHECK_NULL_PTR(popup, "MG_TRN", "popup");
  RA8_CHECK_NULL_PTR(buf, "MG_TRN", "buf");
  if (cap == 0U) {
    return k_ra8_err_invalid_size;
  }
  buf[0]              = '\0';
  const uint32_t pct  = (uint32_t)popup->ocr_confidence_bps / (uint32_t)k_mg_trn_bps_per_pct;
  const uint32_t frac = ((uint32_t)popup->ocr_confidence_bps % (uint32_t)k_mg_trn_bps_per_pct) /
                        (uint32_t)k_mg_trn_tenths_div;
  uint32_t       pos  = mg_trn_str_cat(buf, cap, 0U, "ORIG ");
  pos                 = mg_trn_uint_cat(buf, cap, pos, pct);
  pos                 = mg_trn_str_cat(buf, cap, pos, ".");
  pos                 = mg_trn_uint_cat(buf, cap, pos, frac);
  pos                 = mg_trn_str_cat(buf, cap, pos, "% (#");
  pos                 = mg_trn_uint_cat(buf, cap, pos, (uint32_t)popup->reading_order + 1U);
  (void)mg_trn_str_cat(buf, cap, pos, ")");
  return k_ra8_ok;
}

static size_t mg_trn_decode_utf8(const uint8_t* src, size_t len, size_t off, uint32_t* out_cp)
{
  const uint8_t b0 = src[off];
  if ((b0 >= (uint8_t)k_mg_trn_utf8_2_min) && (b0 <= (uint8_t)k_mg_trn_utf8_2_max) &&
      ((off + 1U) < len) &&
      ((src[off + 1U] & (uint8_t)k_mg_trn_utf8_cont_msk) == (uint8_t)k_mg_trn_utf8_cont_val)) {
    *out_cp = (((uint32_t)b0 & k_mg_trn_utf8_m5_msk) << k_mg_trn_shift_6) |
              ((uint32_t)src[off + 1U] & k_mg_trn_utf8_low6_msk);
    return 2U;
  }
  if ((b0 >= (uint8_t)k_mg_trn_utf8_3_min) && (b0 <= (uint8_t)k_mg_trn_utf8_3_max) &&
      ((off + 2U) < len) &&
      ((src[off + 1U] & (uint8_t)k_mg_trn_utf8_cont_msk) == (uint8_t)k_mg_trn_utf8_cont_val) &&
      ((src[off + 2U] & (uint8_t)k_mg_trn_utf8_cont_msk) == (uint8_t)k_mg_trn_utf8_cont_val)) {
    *out_cp = (((uint32_t)b0 & k_mg_trn_utf8_m4_msk) << k_mg_trn_shift_12) |
              (((uint32_t)src[off + 1U] & k_mg_trn_utf8_low6_msk) << k_mg_trn_shift_6) |
              ((uint32_t)src[off + 2U] & k_mg_trn_utf8_low6_msk);
    return 3U;
  }
  if ((b0 >= (uint8_t)k_mg_trn_utf8_4_min) && (b0 <= (uint8_t)k_mg_trn_utf8_4_max) &&
      ((off + 3U) < len) &&
      ((src[off + 1U] & (uint8_t)k_mg_trn_utf8_cont_msk) == (uint8_t)k_mg_trn_utf8_cont_val) &&
      ((src[off + 2U] & (uint8_t)k_mg_trn_utf8_cont_msk) == (uint8_t)k_mg_trn_utf8_cont_val) &&
      ((src[off + 3U] & (uint8_t)k_mg_trn_utf8_cont_msk) == (uint8_t)k_mg_trn_utf8_cont_val)) {
    *out_cp = (((uint32_t)b0 & k_mg_trn_utf8_m3_msk) << k_mg_trn_shift_18) |
              (((uint32_t)src[off + 1U] & k_mg_trn_utf8_low6_msk) << k_mg_trn_shift_12) |
              (((uint32_t)src[off + 2U] & k_mg_trn_utf8_low6_msk) << k_mg_trn_shift_6) |
              ((uint32_t)src[off + 3U] & k_mg_trn_utf8_low6_msk);
    return 4U;
  }
  return 0U;
}

ra8_err_t mg_translate_format_popup_body(const mg_translate_popup_t* popup, char* buf, uint32_t cap)
{
  RA8_CHECK_NULL_PTR(popup, "MG_TRN", "popup");
  RA8_CHECK_NULL_PTR(buf, "MG_TRN", "buf");
  if (cap == 0U) {
    return k_ra8_err_invalid_size;
  }
  buf[0]             = '\0';
  const uint8_t* src = (const uint8_t*)popup->source_utf8;
  const size_t   len = (popup->source_len < (uint32_t)k_mg_trn_src_cap)
                         ? (size_t)popup->source_len
                         : ((size_t)k_mg_trn_src_cap - 1U);
  uint32_t       pos = 0U;
  size_t         off = 0U;
  while ((off < len) && (pos < (cap - 1U))) {
    const uint8_t b = src[off];
    if ((b >= (uint8_t)k_mg_trn_ascii_lo) && (b <= (uint8_t)k_mg_trn_ascii_hi)) {
      buf[pos++] = (char)b;
      off++;
      continue;
    }
    uint32_t     cp  = 0U;
    const size_t adv = mg_trn_decode_utf8(src, len, off, &cp);
    if (adv > 0U) {
      pos = mg_trn_cp_hex_cat(buf, cap, pos, cp);
      off += adv;
    } else {
      buf[pos++] = '?';
      off++;
    }
  }
  buf[pos] = '\0';
  return k_ra8_ok;
}

/* ===========================================================================
 * Hit-testing: status-bar chrome and page bubbles
 * =========================================================================== */

mg_trn_bar_hit_t mg_translate_hit_bar(const mg_translate_ctx_t* ctx,
                                      int32_t                   fb_w,
                                      int32_t                   statusbar_h,
                                      int32_t                   x,
                                      int32_t                   y)
{
  if ((ctx == nullptr) || (ctx->title_id == 0U) || (fb_w < (int32_t)k_mg_trn_min_chrome_w)) {
    return k_mg_trn_bar_none;
  }
  if ((y < (int32_t)k_mg_trn_pill_y) ||
      (y >= ((int32_t)k_mg_trn_pill_y + (int32_t)k_mg_trn_pill_h)) || (y >= statusbar_h)) {
    return k_mg_trn_bar_none;
  }
  const int32_t mode_x = fb_w - (int32_t)k_mg_trn_pill_pad - (int32_t)k_mg_trn_mode_pill_w;
  if ((x >= mode_x) && (x < (mode_x + (int32_t)k_mg_trn_mode_pill_w))) {
    return k_mg_trn_bar_toggle_mode;
  }
  const int32_t chap_x = mode_x - (int32_t)k_mg_trn_pill_gap - (int32_t)k_mg_trn_chap_pill_w;
  if ((x >= chap_x) && (x < (chap_x + (int32_t)k_mg_trn_chap_pill_w))) {
    return k_mg_trn_bar_next_chapter;
  }
  return k_mg_trn_bar_none;
}

static void mg_trn_store_popup(mg_translate_ctx_t*                   ctx,
                               uint32_t                              bubble_idx,
                               const ra8_translate_sidecar_bubble_t* b)
{
  const mg_trn_chapter_t* ch  = &ctx->chapters[ctx->active_chapter];
  const uint8_t*          ptr = nullptr;
  uint32_t                len = 0U;
  (void)ra8_translate_sidecar_source_text(ch->sidecar_buf, ch->sidecar_len, bubble_idx, &ptr, &len);
  const uint32_t copy_len =
    (len < (uint32_t)k_mg_trn_src_cap) ? len : ((uint32_t)k_mg_trn_src_cap - 1U);
  ctx->popup = (mg_translate_popup_t){.open               = true,
                                      .bubble_index       = bubble_idx,
                                      .reading_order      = b->reading_order,
                                      .ocr_confidence_bps = b->ocr_confidence_bps,
                                      .source_len         = copy_len};
  if ((ptr != nullptr) && (copy_len > 0U)) {
    memcpy(ctx->popup.source_utf8, ptr, (size_t)copy_len);
  }
  ctx->popup.source_utf8[copy_len] = '\0';
}

static bool mg_trn_panel_to_page(const mg_trn_view_t* view,
                                 int32_t              x,
                                 int32_t              y,
                                 int32_t*             out_px,
                                 int32_t*             out_py)
{
  if ((view == nullptr) || (x < view->ox) || (y < (view->content_y0 + view->oy)) ||
      (x >= view->fb_w) || (y >= view->fb_h)) {
    return false;
  }
  const int32_t s = (view->scale > 0) ? view->scale : 1;
  *out_px         = view->view_x + ((x - view->ox) * s);
  *out_py         = view->view_y + ((y - (view->content_y0 + view->oy)) * s);
  return true;
}

static bool mg_trn_find_bubble_at(const mg_trn_chapter_t*         ch,
                                  int32_t                         px,
                                  int32_t                         py,
                                  uint32_t*                       out_idx,
                                  ra8_translate_sidecar_bubble_t* out_bubble)
{
  ra8_translate_sidecar_page_t page = {};
  if (ra8_translate_sidecar_page_at(ch->sidecar_buf, ch->sidecar_len, ch->page_index, &page) !=
      k_ra8_ok) {
    return false;
  }
  bool     found    = false;
  uint32_t best_ord = 0U;
  for (uint16_t i = 0U; i < page.bubble_count; ++i) {
    const uint32_t                 gidx = page.first_bubble + (uint32_t)i;
    ra8_translate_sidecar_bubble_t b    = {};
    if (ra8_translate_sidecar_bubble_at(ch->sidecar_buf, ch->sidecar_len, gidx, &b) != k_ra8_ok) {
      continue;
    }
    const bool hit_x = (px >= (int32_t)b.x) && (px < ((int32_t)b.x + (int32_t)b.w));
    const bool hit_y = (py >= (int32_t)b.y) && (py < ((int32_t)b.y + (int32_t)b.h));
    if (hit_x && hit_y && (!found || ((uint32_t)b.reading_order >= best_ord))) {
      found       = true;
      best_ord    = (uint32_t)b.reading_order;
      *out_idx    = gidx;
      *out_bubble = b;
    }
  }
  return found;
}

bool mg_translate_tap_page(mg_translate_ctx_t* ctx, const mg_trn_view_t* view, int32_t x, int32_t y)
{
  if ((ctx == nullptr) || (view == nullptr) || !mg_translate_overlays_active(ctx)) {
    return false;
  }
  int32_t px = 0;
  int32_t py = 0;
  if (!mg_trn_panel_to_page(view, x, y, &px, &py)) {
    return false;
  }
  const mg_trn_chapter_t*        ch   = &ctx->chapters[ctx->active_chapter];
  uint32_t                       bidx = 0U;
  ra8_translate_sidecar_bubble_t b    = {};
  if (mg_trn_find_bubble_at(ch, px, py, &bidx, &b)) {
    if (ctx->popup.open && (ctx->popup.bubble_index == bidx)) {
      ctx->popup = (mg_translate_popup_t){};
    } else {
      mg_trn_store_popup(ctx, bidx, &b);
    }
    return true;
  }
  if (ctx->popup.open) {
    ctx->popup = (mg_translate_popup_t){};
    return true;
  }
  return false;
}

/* ===========================================================================
 * Viewport overlay paint adapter + chrome renderer
 * =========================================================================== */

static void
mg_trn_paint_fill_rect(void* user, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
  const mg_trn_paint_ctx_t* pc = (const mg_trn_paint_ctx_t*)user;
  if ((pc == nullptr) || (pc->view == nullptr) || (w <= 0) || (h <= 0)) {
    return;
  }
  const mg_trn_view_t* v  = pc->view;
  const int32_t        s  = (v->scale > 0) ? v->scale : 1;
  const int32_t        x0 = v->ox + mg_trn_floor_div(x - v->view_x, s);
  const int32_t        y0 = v->content_y0 + v->oy + mg_trn_floor_div(y - v->view_y, s);
  const int32_t        x1 = v->ox + mg_trn_floor_div((x + w) - v->view_x, s);
  const int32_t        y1 = v->content_y0 + v->oy + mg_trn_floor_div((y + h) - v->view_y, s);
  int32_t              rx = x0;
  int32_t              ry = y0;
  int32_t              rw = (x1 > x0) ? (x1 - x0) : 1;
  int32_t              rh = (y1 > y0) ? (y1 - y0) : 1;
  if (rx < 0) {
    rw += rx;
    rx = 0;
  }
  if (ry < v->content_y0) {
    rh -= (v->content_y0 - ry);
    ry = v->content_y0;
  }
  if ((rx + rw) > v->fb_w) {
    rw = v->fb_w - rx;
  }
  if ((ry + rh) > v->fb_h) {
    rh = v->fb_h - ry;
  }
  if ((rw > 0) && (rh > 0)) {
    (void)ra8_gfx_rect(rx, ry, rw, rh, color, true);
  }
}

static void
mg_trn_paint_draw_text(void* user, int32_t x, int32_t y, const char* str, uint32_t fg, uint32_t bg)
{
  const mg_trn_paint_ctx_t* pc = (const mg_trn_paint_ctx_t*)user;
  if ((pc == nullptr) || (pc->view == nullptr) || (str == nullptr)) {
    return;
  }
  const mg_trn_view_t* v  = pc->view;
  const int32_t        s  = (v->scale > 0) ? v->scale : 1;
  const int32_t        dx = v->ox + mg_trn_floor_div(x - v->view_x, s);
  const int32_t        dy = v->content_y0 + v->oy + mg_trn_floor_div(y - v->view_y, s);
  if ((dx >= v->fb_w) || (dy >= v->fb_h) || ((dy + (int32_t)k_mg_trn_glyph_h) <= v->content_y0)) {
    return;
  }
  (void)ra8_gfx_text_out(dx, dy, str, &ra8_gfx_font_8x16, fg, bg);
}

static void mg_trn_paint_text_size(void* user, const char* str, int32_t* out_w, int32_t* out_h)
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
  } else {
    *out_w = 0;
    *out_h = 0;
  }
}

ra8_err_t mg_translate_render_overlays(mg_translate_ctx_t* ctx, const mg_trn_view_t* view)
{
  RA8_CHECK_NULL_PTR(ctx, "MG_TRN", "ctx");
  RA8_CHECK_NULL_PTR(view, "MG_TRN", "view");
  ctx->last_damage_count = 0U;
  if (!mg_translate_overlays_active(ctx)) {
    return k_ra8_ok; /* Fallback / Original mode leaves page clean. */
  }
  const mg_trn_chapter_t*  ch    = &ctx->chapters[ctx->active_chapter];
  mg_trn_paint_ctx_t       pctx  = {.view = view};
  const ra8_widget_paint_t paint = {.user      = &pctx,
                                    .fill_rect = mg_trn_paint_fill_rect,
                                    .draw_text = mg_trn_paint_draw_text,
                                    .text_size = mg_trn_paint_text_size};
  (void)ra8_gfx_set_clip(0, view->content_y0, view->fb_w, view->fb_h - view->content_y0);
  const ra8_err_t rc = ra8_translate_overlay_render_page(&paint,
                                                         ch->sidecar_buf,
                                                         ch->sidecar_len,
                                                         ch->page_index,
                                                         &ctx->overlay_style,
                                                         ctx->last_damage,
                                                         (size_t)k_mg_trn_damage_cap,
                                                         &ctx->last_damage_count);
  (void)ra8_gfx_reset_clip();
  return rc;
}

static ra8_err_t mg_trn_draw_pill(int32_t     x,
                                  int32_t     y,
                                  int32_t     w,
                                  int32_t     h,
                                  const char* text,
                                  uint32_t    fg,
                                  uint32_t    bg)
{
  (void)ra8_gfx_rect(x, y, w, h, bg, true);
  (void)ra8_gfx_rect(x, y, w, h, (uint32_t)k_mg_trn_col_border, false);
  uint32_t tw = 0U;
  uint32_t th = 0U;
  (void)ra8_gfx_text_size(text, &ra8_gfx_font_8x16, &tw, &th);
  const int32_t tx = x + ((w - (int32_t)tw) / 2);
  const int32_t ty = y + ((h - (int32_t)th) / 2);
  (void)ra8_gfx_set_clip(x + 2, y + 2, w - 4, h - 4);
  const ra8_err_t rc = ra8_gfx_text_out(tx, ty, text, &ra8_gfx_font_8x16, fg, bg);
  (void)ra8_gfx_reset_clip();
  return rc;
}

static void mg_trn_pick_chap_colors(const mg_translate_ctx_t* ctx, uint32_t* fg, uint32_t* bg)
{
  const mg_trn_job_marker_t mk = mg_translate_query_chapter_marker(ctx, ctx->active_chapter);
  if (mk != k_mg_trn_job_none) {
    *bg = (uint32_t)k_mg_trn_col_busy_bg;
    *fg = (uint32_t)k_mg_trn_col_trn_fg;
    return;
  }
  switch (ctx->active_sidecar_state) {
    case k_mg_trn_sidecar_ready:
      *bg = (uint32_t)k_mg_trn_col_ready_bg;
      *fg = (uint32_t)k_mg_trn_col_popup_fg;
      break;
    case k_mg_trn_sidecar_stale:
    case k_mg_trn_sidecar_corrupt:
      *bg = (uint32_t)k_mg_trn_col_warn_bg;
      *fg = (uint32_t)k_mg_trn_col_popup_fg;
      break;
    case k_mg_trn_sidecar_none:
    default:
      *bg = (uint32_t)k_mg_trn_col_orig_bg;
      *fg = (uint32_t)k_mg_trn_col_popup_fg;
      break;
  }
}

static ra8_err_t mg_trn_render_popup(const mg_translate_ctx_t* ctx, int32_t fb_w, int32_t fb_h)
{
  if (!ctx->popup.open) {
    return k_ra8_ok;
  }
  const int32_t px = (int32_t)k_mg_trn_popup_x;
  const int32_t pw = fb_w - px - (int32_t)k_mg_trn_popup_right_m;
  const int32_t ph = (int32_t)k_mg_trn_popup_h;
  const int32_t py = fb_h - ph - (int32_t)k_mg_trn_popup_bottom;
  if ((pw <= ((int32_t)k_mg_trn_popup_pad * 2)) || (py <= 0)) {
    return k_ra8_ok;
  }
  (void)ra8_gfx_rect(px, py, pw, ph, (uint32_t)k_mg_trn_col_popup_bg, true);
  (void)ra8_gfx_rect(px, py, pw, ph, (uint32_t)k_mg_trn_col_border, false);
  char hdr[k_mg_trn_label_cap]    = {};
  char body[k_mg_trn_preview_cap] = {};
  (void)mg_translate_format_popup_header(&ctx->popup, hdr, (uint32_t)sizeof(hdr));
  (void)mg_translate_format_popup_body(&ctx->popup, body, (uint32_t)sizeof(body));
  (void)ra8_gfx_set_clip(px + (int32_t)k_mg_trn_popup_pad,
                         py + 2,
                         pw - ((int32_t)k_mg_trn_popup_pad * 2),
                         ph - 4);
  (void)ra8_gfx_text_out(px + (int32_t)k_mg_trn_popup_pad,
                         py + (int32_t)k_mg_trn_popup_pad,
                         hdr,
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_mg_trn_col_popup_fg,
                         (uint32_t)k_mg_trn_col_popup_bg);
  (void)ra8_gfx_text_out(px + (int32_t)k_mg_trn_popup_pad,
                         py + (int32_t)k_mg_trn_popup_pad + (int32_t)k_mg_trn_popup_line_gap,
                         body,
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_mg_trn_col_popup_sub,
                         (uint32_t)k_mg_trn_col_popup_bg);
  (void)ra8_gfx_reset_clip();
  return k_ra8_ok;
}

ra8_err_t mg_translate_render_chrome(const mg_translate_ctx_t* ctx, int32_t fb_w, int32_t fb_h)
{
  RA8_CHECK_NULL_PTR(ctx, "MG_TRN", "ctx");
  if ((ctx->title_id == 0U) || (fb_w < (int32_t)k_mg_trn_min_chrome_w)) {
    return mg_trn_render_popup(ctx, fb_w, fb_h);
  }
  char mode_lbl[k_mg_trn_label_cap] = {};
  char chap_lbl[k_mg_trn_label_cap] = {};
  (void)mg_translate_format_mode_pill(ctx, mode_lbl, (uint32_t)sizeof(mode_lbl));
  (void)mg_translate_format_chapter_pill(ctx, chap_lbl, (uint32_t)sizeof(chap_lbl));

  uint32_t mode_bg = (uint32_t)k_mg_trn_col_orig_bg;
  uint32_t mode_fg = (uint32_t)k_mg_trn_col_popup_fg;
  if (mg_translate_overlays_active(ctx)) {
    mode_bg = (uint32_t)k_mg_trn_col_trn_bg;
    mode_fg = (uint32_t)k_mg_trn_col_trn_fg;
  } else if (mg_translate_is_fallback_active(ctx)) {
    mode_bg = (uint32_t)k_mg_trn_col_warn_bg;
  }
  uint32_t chap_fg = 0U;
  uint32_t chap_bg = 0U;
  mg_trn_pick_chap_colors(ctx, &chap_fg, &chap_bg);

  const int32_t   mode_x = fb_w - (int32_t)k_mg_trn_pill_pad - (int32_t)k_mg_trn_mode_pill_w;
  const int32_t   chap_x = mode_x - (int32_t)k_mg_trn_pill_gap - (int32_t)k_mg_trn_chap_pill_w;
  const ra8_err_t r1     = mg_trn_draw_pill(mode_x,
                                            (int32_t)k_mg_trn_pill_y,
                                            (int32_t)k_mg_trn_mode_pill_w,
                                            (int32_t)k_mg_trn_pill_h,
                                            mode_lbl,
                                            mode_fg,
                                            mode_bg);
  if (r1 != k_ra8_ok) {
    return r1;
  }
  const ra8_err_t r2 = mg_trn_draw_pill(chap_x,
                                        (int32_t)k_mg_trn_pill_y,
                                        (int32_t)k_mg_trn_chap_pill_w,
                                        (int32_t)k_mg_trn_pill_h,
                                        chap_lbl,
                                        chap_fg,
                                        chap_bg);
  if (r2 != k_ra8_ok) {
    return r2;
  }
  return mg_trn_render_popup(ctx, fb_w, fb_h);
}
