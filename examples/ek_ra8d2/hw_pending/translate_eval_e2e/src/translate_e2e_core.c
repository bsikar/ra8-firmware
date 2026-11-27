/**
 * @file translate_e2e_core.c
 * @brief Freestanding core E2E runner for `ra8_translate` chapter jobs and `TPAK` packs.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "ra8_gfx.h"
#include "translate_e2e.h"

typedef enum : uint32_t {
  k_core_max_marian_seq = 16U,
  k_core_max_enc_ids    = 32U,
  k_core_max_text_bytes = 512U,
  k_core_max_steps      = 32U,
} te2e_core_const_t;

static uint16_t core_read_u16(const uint8_t* src, size_t off)
{
  return (uint16_t)src[off] | (uint16_t)((uint16_t)src[off + 1U] << 8U);
}

static uint32_t core_read_u32(const uint8_t* src, size_t off)
{
  return (uint32_t)src[off] | ((uint32_t)src[off + 1U] << 8U) | ((uint32_t)src[off + 2U] << 16U) |
         ((uint32_t)src[off + 3U] << 24U);
}

static float core_read_f32(const uint8_t* src, size_t off)
{
  const uint32_t bits = core_read_u32(src, off);
  float          out  = 0.0F;
  memcpy(&out, &bits, sizeof(out));
  return out;
}

static bool core_span_ok(size_t off, size_t len, size_t total)
{
  return (off <= total) && (len <= (total - off));
}

static void core_emit_str(translate_e2e_emit_fn emit_fn, void* ctx, const char* s)
{
  emit_fn(ctx, s, strlen(s));
}

static void core_emit_u64(translate_e2e_emit_fn emit_fn, void* ctx, uint64_t val)
{
  char   buf[24];
  size_t pos = sizeof(buf);
  if (val == 0U) {
    emit_fn(ctx, "0", 1U);
    return;
  }
  while (val > 0U) {
    buf[--pos] = (char)('0' + (val % 10U));
    val /= 10U;
  }
  emit_fn(ctx, &buf[pos], sizeof(buf) - pos);
}

static void
core_emit_json_str(translate_e2e_emit_fn emit_fn, void* ctx, const uint8_t* bytes, size_t len)
{
  static const char k_hex[] = "0123456789abcdef";
  emit_fn(ctx, "\"", 1U);
  for (size_t i = 0U; i < len; ++i) {
    const uint8_t c = bytes[i];
    if ((c == '"') || (c == '\\')) {
      const char esc[2] = {'\\', (char)c};
      emit_fn(ctx, esc, 2U);
    } else if (c < 0x20U) {
      const char esc[6] = {'\\', 'u', '0', '0', k_hex[(c >> 4U) & 0x0FU], k_hex[c & 0x0FU]};
      emit_fn(ctx, esc, 6U);
    } else {
      emit_fn(ctx, (const char*)&c, 1U);
    }
  }
  emit_fn(ctx, "\"", 1U);
}

static void core_gfx_text_size(void* user, const char* str, int32_t* out_w, int32_t* out_h)
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

static void core_fill_noop(void* user, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
  (void)user;
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  (void)color;
}

static void
core_draw_noop(void* user, int32_t x, int32_t y, const char* str, uint32_t fg, uint32_t bg)
{
  (void)user;
  (void)x;
  (void)y;
  (void)str;
  (void)fg;
  (void)bg;
}

static const ra8_widget_paint_t k_core_paint = {.user      = nullptr,
                                                .fill_rect = core_fill_noop,
                                                .draw_text = core_draw_noop,
                                                .text_size = core_gfx_text_size};

static ra8_err_t core_run_det_i8(const int8_t*                map,
                                 uint16_t                     mw,
                                 uint16_t                     mh,
                                 uint16_t                     pw,
                                 uint16_t                     ph,
                                 float                        scale,
                                 int8_t                       zp,
                                 uint8_t                      order,
                                 uint8_t*                     work,
                                 size_t                       work_cap,
                                 ra8_translate_det_bubble_t*  out_bubbles,
                                 ra8_translate_det_summary_t* out_sum)
{
  size_t    need = 0U;
  ra8_err_t err  = ra8_translate_det_work_bytes(mw, (uint16_t)k_te2e_max_det_comps, &need);
  if (err != k_ra8_ok) {
    return err;
  }
  if (need > work_cap) {
    return k_ra8_err_invalid_size;
  }
  const ra8_translate_det_cfg_t cfg = {
    .map_width              = mw,
    .map_height             = mh,
    .page_width             = pw,
    .page_height            = ph,
    .text_thresh_bps        = (uint16_t)k_ra8_translate_det_def_txt_bps,
    .bubble_thresh_bps      = (uint16_t)k_ra8_translate_det_def_bub_bps,
    .min_comp_area          = 2U,
    .group_gap_pct          = (uint16_t)k_ra8_translate_det_def_gap_pct,
    .furigana_max_ratio_pct = (uint16_t)k_ra8_translate_det_def_furi_pct,
    .furigana_max_gap_pct   = (uint16_t)k_ra8_translate_det_def_fgap_pct,
    .reading_order          = (ra8_translate_det_order_t)order,
    .flags                  = (uint8_t)k_ra8_translate_det_flag_drop_furigana,
    .max_components         = (uint16_t)k_te2e_max_det_comps,
  };
  ra8_translate_det_line_t lines[k_te2e_max_det_comps];
  return ra8_translate_det_post_i8(map,
                                   (size_t)mw * (size_t)mh,
                                   scale,
                                   zp,
                                   nullptr,
                                   0U,
                                   1.0F,
                                   0,
                                   &cfg,
                                   work,
                                   need,
                                   lines,
                                   k_te2e_max_det_comps,
                                   out_bubbles,
                                   k_te2e_max_det_comps,
                                   out_sum);
}

static ra8_err_t core_run_ocr_i8(const uint8_t*              tchr,
                                 size_t                      tchr_len,
                                 const int8_t*               logits,
                                 size_t                      logits_len,
                                 uint32_t                    time_steps,
                                 float                       scale,
                                 int8_t                      zp,
                                 uint8_t                     ocr_model,
                                 uint8_t*                    out_utf8,
                                 size_t                      out_cap,
                                 ra8_translate_ctc_result_t* out_res)
{
  ra8_translate_ctc_charset_header_t hdr = {};
  ra8_err_t                          err = ra8_translate_ctc_charset_open(tchr, tchr_len, &hdr);
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_translate_ctc_decode_cfg_t cfg = {
    .time_steps                = time_steps,
    .score_mode                = k_ra8_translate_ctc_score_logits,
    .low_confidence_thresh_bps = (uint16_t)k_ra8_translate_ctc_default_low_conf_bps,
    .expected_ocr_model        = ocr_model,
  };
  return ra8_translate_ctc_decode_i8(tchr,
                                     tchr_len,
                                     logits,
                                     logits_len,
                                     scale,
                                     zp,
                                     &cfg,
                                     out_utf8,
                                     out_cap,
                                     out_res);
}

static ra8_err_t core_run_bubble_nmt(const uint8_t* tvoc,
                                     size_t         tvoc_len,
                                     const uint8_t* tmnt,
                                     size_t         tmnt_len,
                                     const uint8_t* src_utf8,
                                     size_t         src_len,
                                     uint8_t*       work,
                                     size_t         work_cap,
                                     uint8_t*       out_eng,
                                     size_t         out_cap,
                                     size_t*        out_len,
                                     uint64_t*      out_weight_bytes)
{
  ra8_translate_vocab_header_t tok_hdr = {};
  ra8_err_t                    err     = ra8_translate_tok_open(tvoc, tvoc_len, &tok_hdr);
  if (err != k_ra8_ok) {
    return err;
  }
  size_t tok_need = 0U;
  err             = ra8_translate_tok_work_bytes(src_len, k_ra8_translate_tok_unigram, &tok_need);
  if (err != k_ra8_ok) {
    return err;
  }
  const size_t use_tok = (tok_need < 64U) ? 64U : tok_need;
  if (use_tok > work_cap) {
    return k_ra8_err_invalid_size;
  }
  uint32_t enc_ids[k_core_max_enc_ids] = {};
  size_t   enc_n                       = 0U;
  err                                  = ra8_translate_tok_encode(tvoc,
                                                                  tvoc_len,
                                                                  src_utf8,
                                                                  src_len,
                                                                  k_ra8_translate_tok_enc_add_eos,
                                                                  work,
                                                                  use_tok,
                                                                  enc_ids,
                                                                  k_core_max_enc_ids,
                                                                  &enc_n);
  if ((err != k_ra8_ok) || (enc_n == 0U)) {
    return (err != k_ra8_ok) ? err : k_ra8_err_invalid_arg;
  }
  ra8_translate_marian_header_t m_hdr = {};
  err =
    ra8_translate_marian_open(tmnt, tmnt_len, k_ra8_translate_marian_open_verify_payload, &m_hdr);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint16_t max_seq = (m_hdr.max_seq_len < (uint16_t)k_core_max_marian_seq)
                             ? m_hdr.max_seq_len
                             : (uint16_t)k_core_max_marian_seq;
  const uint16_t max_src = (enc_n <= (size_t)max_seq) ? (uint16_t)enc_n : max_seq;
  size_t         m_need  = 0U;
  err = ra8_translate_marian_work_bytes(tmnt, tmnt_len, max_src, max_seq, &m_need);
  if ((err != k_ra8_ok) || (m_need > work_cap)) {
    return (err != k_ra8_ok) ? err : k_ra8_err_invalid_size;
  }
  uint32_t                     out_ids[k_core_max_marian_seq] = {};
  ra8_translate_marian_stats_t m_stats                        = {};
  err = ra8_translate_marian_greedy_decode(tmnt,
                                           tmnt_len,
                                           enc_ids,
                                           max_src,
                                           max_seq,
                                           work,
                                           m_need,
                                           out_ids,
                                           k_core_max_marian_seq,
                                           &m_stats);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_weight_bytes += m_stats.weight_bytes_read;
  return ra8_translate_tok_decode(tvoc,
                                  tvoc_len,
                                  out_ids,
                                  m_stats.generated_tokens,
                                  out_eng,
                                  out_cap,
                                  out_len);
}

static ra8_err_t core_hook_ocr_page(void*                              user,
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
  (void)chapter;
  translate_e2e_arena_t* arena = (translate_e2e_arena_t*)user;
  if ((arena == nullptr) || (page_index >= (uint16_t)k_te2e_fixture_pages)) {
    return k_ra8_err_invalid_arg;
  }
  const translate_e2e_page_pack_t* pg = &arena->pages[page_index];
  ra8_translate_det_bubble_t       det_b[k_te2e_max_det_comps];
  ra8_translate_det_summary_t      det_sum = {};
  ra8_err_t                        err     = core_run_det_i8(pg->det_map,
                                                             pg->map_w,
                                                             pg->map_h,
                                                             pg->page_w,
                                                             pg->page_h,
                                                             pg->det_scale,
                                                             pg->det_zp,
                                                             k_ra8_translate_det_order_manga_rtl,
                                                             arena->model_work,
                                                             sizeof(arena->model_work),
                                                             det_b,
                                                             &det_sum);
  if (err != k_ra8_ok) {
    return err;
  }
  arena->furigana_dropped_total += det_sum.dropped_furigana;
  if ((det_sum.bubble_count != pg->bubble_count) || (det_sum.bubble_count > max_bubbles)) {
    return k_ra8_err_validation_failed;
  }
  size_t cursor = 0U;
  for (uint32_t bi = 0U; bi < det_sum.bubble_count; ++bi) {
    const translate_e2e_bubble_pack_t* bp      = &arena->bubbles[pg->first_bubble + bi];
    ra8_translate_ctc_result_t         ctc_res = {};
    err                                        = core_run_ocr_i8(bp->tchr_buf,
                                                                 bp->tchr_len,
                                                                 bp->logits_i8,
                                                                 bp->logits_len,
                                                                 bp->time_steps,
                                                                 bp->ocr_scale,
                                                                 bp->ocr_zp,
                                                                 k_ra8_translate_ocr_japanese,
                                                                 &out_text_buf[cursor],
                                                                 text_cap - cursor,
                                                                 &ctc_res);
    if (err != k_ra8_ok) {
      return err;
    }
    out_bubbles[bi] = (ra8_translate_job_ocr_bubble_t){
      .x                = det_b[bi].x,
      .y                = det_b[bi].y,
      .w                = det_b[bi].w,
      .h                = det_b[bi].h,
      .shape            = (ra8_translate_shape_t)det_b[bi].shape,
      .text_orientation = (ra8_translate_orientation_t)det_b[bi].text_orientation,
      .corner_radius = (det_b[bi].shape == (uint8_t)k_ra8_translate_shape_rounded_rect) ? 8U : 0U,
      .reading_order = det_b[bi].reading_order,
      .ocr_confidence_bps = ctc_res.confidence_bps,
      .source_off         = (uint32_t)cursor,
      .source_len         = ctc_res.text_bytes,
    };
    cursor += ctc_res.text_bytes;
  }
  *out_page_w       = pg->page_w;
  *out_page_h       = pg->page_h;
  *out_bubble_count = (uint16_t)det_sum.bubble_count;
  *out_text_len     = cursor;
  return k_ra8_ok;
}

static ra8_err_t core_hook_translate_term(void*                user,
                                          ra8_translate_lang_t source_lang,
                                          const uint8_t*       src_utf8,
                                          size_t               src_len,
                                          uint8_t*             out_eng_utf8,
                                          size_t               out_eng_cap,
                                          size_t*              out_eng_len)
{
  (void)source_lang;
  static const char      k_stem[] = "\xE3\x82\xBF\xE3\x83\x8A\xE3\x82\xAB";
  translate_e2e_arena_t* arena    = (translate_e2e_arena_t*)user;
  arena->term_calls++;
  const char* eng = ((src_len == (sizeof(k_stem) - 1U)) && (memcmp(src_utf8, k_stem, src_len) == 0))
                      ? "Tanaka"
                      : "Ally";
  const size_t elen = strlen(eng);
  if (elen > out_eng_cap) {
    return k_ra8_err_invalid_size;
  }
  memcpy(out_eng_utf8, eng, elen);
  *out_eng_len = elen;
  return k_ra8_ok;
}

static ra8_err_t core_hook_translate_batch(void*                           user,
                                           ra8_translate_nmt_model_t       nmt_model,
                                           const uint8_t*                  src_buf,
                                           size_t                          src_bytes,
                                           const ra8_translate_job_span_t* src_spans,
                                           uint16_t                        item_count,
                                           uint8_t*                        out_eng_buf,
                                           size_t                          out_eng_cap,
                                           ra8_translate_job_span_t*       out_eng_spans)
{
  (void)nmt_model;
  translate_e2e_arena_t* arena = (translate_e2e_arena_t*)user;
  arena->batch_calls++;
  size_t cursor = 0U;
  for (uint16_t i = 0U; i < item_count; ++i) {
    const uint32_t off = src_spans[i].off;
    const uint32_t len = src_spans[i].len;
    if (!core_span_ok(off, len, src_bytes)) {
      return k_ra8_err_invalid_arg;
    }
    const translate_e2e_bubble_pack_t* matched = nullptr;
    for (uint32_t bi = 0U; bi < (uint32_t)k_te2e_fixture_bubbles; ++bi) {
      const char* ms = arena->bubbles[bi].masked_src_utf8;
      if ((strlen(ms) == len) && (memcmp(&src_buf[off], ms, len) == 0)) {
        matched = &arena->bubbles[bi];
        break;
      }
    }
    if (matched == nullptr) {
      return k_ra8_err_not_found;
    }
    size_t    eng_len = 0U;
    ra8_err_t err     = core_run_bubble_nmt(matched->tvoc_buf,
                                            matched->tvoc_len,
                                            matched->tmnt_buf,
                                            matched->tmnt_len,
                                            &src_buf[off],
                                            len,
                                            arena->model_work,
                                            sizeof(arena->model_work),
                                            &out_eng_buf[cursor],
                                            out_eng_cap - cursor,
                                            &eng_len,
                                            &arena->weight_bytes_read);
    if (err != k_ra8_ok) {
      return err;
    }
    out_eng_spans[i] =
      (ra8_translate_job_span_t){.off = (uint32_t)cursor, .len = (uint32_t)eng_len};
    cursor += eng_len;
  }
  return k_ra8_ok;
}

static ra8_err_t core_hook_save_ckpt(void* user, const uint8_t* ckpt_bytes, size_t ckpt_len)
{
  translate_e2e_arena_t* arena = (translate_e2e_arena_t*)user;
  if (ckpt_len == 0U) {
    return k_ra8_ok;
  }
  if (ckpt_len > sizeof(arena->ckpt_snap)) {
    return k_ra8_err_invalid_size;
  }
  memcpy(arena->ckpt_snap, ckpt_bytes, ckpt_len);
  arena->ckpt_snap_len = ckpt_len;
  return k_ra8_ok;
}

static ra8_err_t core_hook_commit_clean(void*                              user,
                                        const ra8_translate_job_chapter_t* chapter,
                                        const uint8_t*                     trn1_bytes,
                                        size_t                             trn1_len,
                                        const uint8_t*                     tgl1_bytes,
                                        size_t                             tgl1_len)
{
  (void)chapter;
  translate_e2e_arena_t* arena = (translate_e2e_arena_t*)user;
  if ((trn1_len > sizeof(arena->trn1_clean)) || (tgl1_len > sizeof(arena->tgl1_out))) {
    return k_ra8_err_invalid_size;
  }
  memcpy(arena->trn1_clean, trn1_bytes, trn1_len);
  arena->trn1_clean_len = trn1_len;
  if (tgl1_len > 0U) {
    memcpy(arena->tgl1_out, tgl1_bytes, tgl1_len);
  }
  arena->tgl1_out_len = tgl1_len;
  return k_ra8_ok;
}

static ra8_err_t core_hook_commit_resume(void*                              user,
                                         const ra8_translate_job_chapter_t* chapter,
                                         const uint8_t*                     trn1_bytes,
                                         size_t                             trn1_len,
                                         const uint8_t*                     tgl1_bytes,
                                         size_t                             tgl1_len)
{
  (void)chapter;
  (void)tgl1_bytes;
  (void)tgl1_len;
  translate_e2e_arena_t* arena = (translate_e2e_arena_t*)user;
  if (trn1_len > sizeof(arena->trn1_resume)) {
    return k_ra8_err_invalid_size;
  }
  memcpy(arena->trn1_resume, trn1_bytes, trn1_len);
  arena->trn1_resume_len = trn1_len;
  return k_ra8_ok;
}

static ra8_translate_job_chapter_t
core_make_chapter(uint32_t title, uint32_t order, uint8_t unread, uint16_t pages)
{
  ra8_translate_job_chapter_t ch = {
    .sidecar_key   = {.detector_model_version = 1U,
                      .ocr_model_version      = 2U,
                      .nmt_model_version      = 3U,
                      .glossary_revision      = 1U,
                      .source_lang            = k_ra8_translate_lang_unknown},
    .title_id      = title,
    .chapter_order = order,
    .page_count    = pages,
    .lang_hint     = k_ra8_translate_lang_unknown,
    .unread        = unread,
    .trigger       = k_ra8_translate_job_trigger_mdl_download,
  };
  memset(ch.sidecar_key.chapter_hash, (int)(order + 1U), sizeof(ch.sidecar_key.chapter_hash));
  return ch;
}

static ra8_translate_job_policy_t core_default_policy(void)
{
  return (ra8_translate_job_policy_t){
    .battery_min_pct        = (uint8_t)k_ra8_translate_job_default_batt_pct,
    .battery_hysteresis_pct = 5U,
    .nmt_batch_max          = (uint8_t)k_ra8_translate_job_default_batch,
    .yield_to_reader        = 1U,
    .yield_to_display       = 1U,
    .overlay_style =
      {
        .fg_color     = (uint32_t)k_ra8_translate_overlay_default_fg,
        .bg_color     = (uint32_t)k_ra8_translate_overlay_default_bg,
        .border_color = (uint32_t)k_ra8_translate_overlay_default_border,
        .pad          = 4,
        .border_w     = 1,
        .line_gap     = 2,
        .align        = k_ra8_widget_align_center,
        .face         = k_ra8_widget_text_face_sans,
        .weight       = k_ra8_widget_text_weight_regular,
      },
  };
}

static bool core_verify_policy(translate_e2e_arena_t* arena, const ra8_translate_job_hooks_t* hooks)
{
  const ra8_translate_job_policy_t pol = core_default_policy();
  if (ra8_translate_job_init(arena->job_work, sizeof(arena->job_work), &pol) != k_ra8_ok) {
    return false;
  }
  const ra8_translate_job_chapter_t c2 = core_make_chapter(77U, 2U, 0U, 2U);
  const ra8_translate_job_chapter_t c3 = core_make_chapter(77U, 3U, 1U, 2U);
  const ra8_translate_job_chapter_t c1 = core_make_chapter(77U, 1U, 1U, 2U);
  if ((ra8_translate_job_enqueue(arena->job_work, sizeof(arena->job_work), &c2) != k_ra8_ok) ||
      (ra8_translate_job_enqueue(arena->job_work, sizeof(arena->job_work), &c3) != k_ra8_ok) ||
      (ra8_translate_job_enqueue(arena->job_work, sizeof(arena->job_work), &c1) != k_ra8_ok)) {
    return false;
  }
  const ra8_translate_job_env_t low_batt = {.battery_pct = 20U};
  ra8_translate_job_status_t    st       = {};
  if ((ra8_translate_job_step(arena->job_work, sizeof(arena->job_work), &low_batt, hooks, &st) !=
       k_ra8_ok) ||
      (st.state != k_ra8_translate_job_state_paused_battery)) {
    return false;
  }
  const ra8_translate_job_env_t busy_rdr = {.battery_pct   = 20U,
                                            .charging      = 1U,
                                            .reader_active = 1U};
  if ((ra8_translate_job_step(arena->job_work, sizeof(arena->job_work), &busy_rdr, hooks, &st) !=
       k_ra8_ok) ||
      (st.state != k_ra8_translate_job_state_yielded)) {
    return false;
  }
  const ra8_translate_job_env_t chg_ok = {.battery_pct = 20U, .charging = 1U};
  if ((ra8_translate_job_step(arena->job_work, sizeof(arena->job_work), &chg_ok, hooks, &st) !=
       k_ra8_ok) ||
      (st.active_chapter_order != 1U)) {
    return false;
  }
  return true;
}

static ra8_err_t core_verify_trn1(const uint8_t* trn1, size_t len, uint32_t* out_bubbles)
{
  static const char* const k_expected[k_te2e_fixture_bubbles] = {
    "Tanaka-san, wait for me!",
    "Run now!",
    "Tanaka-san!",
  };
  ra8_translate_sidecar_header_t hdr = {};
  ra8_err_t                      err = ra8_translate_sidecar_open(trn1, len, &hdr);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((hdr.page_count != (uint16_t)k_te2e_fixture_pages) ||
      (hdr.bubble_count != (uint32_t)k_te2e_fixture_bubbles) ||
      (hdr.key.source_lang != k_ra8_translate_lang_japanese)) {
    return k_ra8_err_validation_failed;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_te2e_fixture_bubbles; ++i) {
    const uint8_t* eng_ptr = nullptr;
    uint32_t       eng_len = 0U;
    err                    = ra8_translate_sidecar_english_text(trn1, len, i, &eng_ptr, &eng_len);
    if (err != k_ra8_ok) {
      return err;
    }
    if ((eng_len != strlen(k_expected[i])) || (memcmp(eng_ptr, k_expected[i], eng_len) != 0)) {
      return k_ra8_err_validation_failed;
    }
  }
  *out_bubbles = hdr.bubble_count;
  return k_ra8_ok;
}

static ra8_err_t core_run_step_loop(translate_e2e_arena_t*           arena,
                                    const ra8_translate_job_hooks_t* hooks,
                                    bool                             stop_mid_chapter,
                                    ra8_translate_job_status_t*      out_status)
{
  const ra8_translate_job_env_t env = {.battery_pct = 85U};
  for (uint32_t s = 0U; s < (uint32_t)k_core_max_steps; ++s) {
    ra8_err_t err =
      ra8_translate_job_step(arena->job_work, sizeof(arena->job_work), &env, hooks, out_status);
    if (err != k_ra8_ok) {
      return err;
    }
    if (stop_mid_chapter && (out_status->stage == k_ra8_translate_job_stage_translate_fit)) {
      return k_ra8_ok;
    }
    if (out_status->state == k_ra8_translate_job_state_completed) {
      return k_ra8_ok;
    }
  }
  return k_ra8_err_timeout;
}

ra8_err_t translate_e2e_run_chapter_suite(translate_e2e_arena_t*          arena,
                                          translate_e2e_chapter_report_t* out_report)
{
  if ((arena == nullptr) || (out_report == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  memset(out_report, 0, sizeof(*out_report));
  arena->furigana_dropped_total = 0U;
  arena->weight_bytes_read      = 0U;
  arena->term_calls             = 0U;
  arena->batch_calls            = 0U;
  arena->ckpt_snap_len          = 0U;
  arena->trn1_clean_len         = 0U;
  arena->trn1_resume_len        = 0U;

  size_t    job_need = 0U;
  ra8_err_t err      = ra8_translate_job_work_bytes(&job_need);
  if ((err != k_ra8_ok) || (job_need > sizeof(arena->job_work))) {
    return (err != k_ra8_ok) ? err : k_ra8_err_invalid_size;
  }
  err = translate_e2e_populate_chapter_fixtures(arena);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_translate_job_hooks_t hooks = {
    .user            = arena,
    .paint           = &k_core_paint,
    .ocr_page        = core_hook_ocr_page,
    .translate_term  = core_hook_translate_term,
    .translate_batch = core_hook_translate_batch,
    .save_checkpoint = core_hook_save_ckpt,
    .commit_chapter  = core_hook_commit_clean,
  };
  if (!core_verify_policy(arena, &hooks)) {
    return k_ra8_err_validation_failed;
  }
  out_report->policy_verified = 1U;

  const ra8_translate_job_policy_t  pol = core_default_policy();
  const ra8_translate_job_chapter_t ch =
    core_make_chapter(42U, 1U, 1U, (uint16_t)k_te2e_fixture_pages);
  if ((ra8_translate_job_init(arena->job_work, sizeof(arena->job_work), &pol) != k_ra8_ok) ||
      (ra8_translate_job_enqueue(arena->job_work, sizeof(arena->job_work), &ch) != k_ra8_ok)) {
    return k_ra8_err_invalid_state;
  }
  ra8_translate_job_status_t st = {};
  arena->furigana_dropped_total = 0U;
  err                           = core_run_step_loop(arena, &hooks, false, &st);
  if ((err != k_ra8_ok) || (st.state != k_ra8_translate_job_state_completed)) {
    return (err != k_ra8_ok) ? err : k_ra8_err_invalid_state;
  }
  out_report->furigana_dropped  = arena->furigana_dropped_total;
  out_report->weight_bytes_read = arena->weight_bytes_read;

  /* Now run to translate_fit stage, wipe workspace, restore TRJC, and assert bit-exact TRN1. */
  arena->ckpt_snap_len = 0U;
  if ((ra8_translate_job_init(arena->job_work, sizeof(arena->job_work), &pol) != k_ra8_ok) ||
      (ra8_translate_job_enqueue(arena->job_work, sizeof(arena->job_work), &ch) != k_ra8_ok)) {
    return k_ra8_err_invalid_state;
  }
  hooks.commit_chapter = core_hook_commit_resume;
  err                  = core_run_step_loop(arena, &hooks, true, &st);
  if ((err != k_ra8_ok) || (arena->ckpt_snap_len == 0U)) {
    return (err != k_ra8_ok) ? err : k_ra8_err_invalid_state;
  }
  memset(arena->job_work, 0, sizeof(arena->job_work));
  if ((ra8_translate_job_init(arena->job_work, sizeof(arena->job_work), &pol) != k_ra8_ok) ||
      (ra8_translate_job_restore_checkpoint(arena->job_work,
                                            sizeof(arena->job_work),
                                            arena->ckpt_snap,
                                            arena->ckpt_snap_len) != k_ra8_ok)) {
    return k_ra8_err_validation_failed;
  }
  err = core_run_step_loop(arena, &hooks, false, &st);
  if ((err != k_ra8_ok) || (st.state != k_ra8_translate_job_state_completed)) {
    return (err != k_ra8_ok) ? err : k_ra8_err_invalid_state;
  }
  if ((arena->trn1_clean_len == 0U) || (arena->trn1_clean_len != arena->trn1_resume_len) ||
      (memcmp(arena->trn1_clean, arena->trn1_resume, arena->trn1_clean_len) != 0)) {
    return k_ra8_err_validation_failed;
  }
  out_report->checkpoint_bit_exact = 1U;
  err = core_verify_trn1(arena->trn1_clean, arena->trn1_clean_len, &out_report->bubble_count);
  if (err != k_ra8_ok) {
    return err;
  }
  out_report->trn1_bytes = (uint32_t)arena->trn1_clean_len;
  out_report->trn1_crc32 = translate_e2e_crc32(arena->trn1_clean, arena->trn1_clean_len);
  return k_ra8_ok;
}

static ra8_err_t core_validate_tpak(const uint8_t* tpak_buf, size_t tpak_len, uint32_t* out_pages)
{
  if (tpak_len < (size_t)k_te2e_tpak_header_bytes) {
    return k_ra8_err_invalid_size;
  }
  const uint32_t magic   = core_read_u32(tpak_buf, 0U);
  const uint32_t version = core_read_u32(tpak_buf, 4U);
  const uint32_t pcount  = core_read_u32(tpak_buf, 8U);
  const uint32_t total   = core_read_u32(tpak_buf, 12U);
  const uint32_t crc     = core_read_u32(tpak_buf, 16U);
  if ((magic != (uint32_t)k_te2e_tpak_magic) || (version != (uint32_t)k_te2e_tpak_version)) {
    return k_ra8_err_not_supported;
  }
  if ((pcount == 0U) || (total != tpak_len)) {
    return k_ra8_err_invalid_size;
  }
  if (crc != translate_e2e_crc32_zero_win(tpak_buf, tpak_len, 16U, 4U)) {
    return k_ra8_err_crc_mismatch;
  }
  const size_t pages_off = (size_t)k_te2e_tpak_header_bytes;
  if (((uint64_t)pcount * (uint64_t)k_te2e_tpak_page_bytes) > (tpak_len - pages_off)) {
    return k_ra8_err_invalid_size;
  }
  const size_t bubbles_off   = pages_off + ((size_t)pcount * (size_t)k_te2e_tpak_page_bytes);
  size_t       total_bubbles = 0U;
  for (uint32_t pi = 0U; pi < pcount; ++pi) {
    const uint8_t* pe      = &tpak_buf[pages_off + ((size_t)pi * (size_t)k_te2e_tpak_page_bytes)];
    const uint32_t id_len  = core_read_u32(pe, 0U);
    const uint16_t pw      = core_read_u16(pe, 36U);
    const uint16_t ph      = core_read_u16(pe, 38U);
    const uint16_t mw      = core_read_u16(pe, 40U);
    const uint16_t mh      = core_read_u16(pe, 42U);
    const uint16_t bcnt    = core_read_u16(pe, 48U);
    const uint32_t det_off = core_read_u32(pe, 52U);
    const float    det_sc  = core_read_f32(pe, 56U);
    const uint32_t first_b = core_read_u32(pe, 60U);
    if ((id_len == 0U) || (id_len >= (uint32_t)k_te2e_tpak_page_id_cap) || (pw == 0U) ||
        (ph == 0U) || (mw == 0U) || (mh == 0U) || !isfinite(det_sc) || (det_sc <= 0.0F) ||
        !core_span_ok(det_off, (size_t)mw * (size_t)mh, tpak_len) || (first_b != total_bubbles)) {
      return k_ra8_err_invalid_arg;
    }
    total_bubbles += bcnt;
  }
  if (((uint64_t)total_bubbles * (uint64_t)k_te2e_tpak_bubble_bytes) > (tpak_len - bubbles_off)) {
    return k_ra8_err_invalid_size;
  }
  for (size_t bi = 0U; bi < total_bubbles; ++bi) {
    const uint8_t* be     = &tpak_buf[bubbles_off + (bi * (size_t)k_te2e_tpak_bubble_bytes)];
    const float    ocr_sc = core_read_f32(be, 20U);
    if (!isfinite(ocr_sc) || (ocr_sc <= 0.0F) || (core_read_u32(be, 16U) == 0U) ||
        !core_span_ok(core_read_u32(be, 0U), core_read_u32(be, 4U), tpak_len) ||
        !core_span_ok(core_read_u32(be, 8U), core_read_u32(be, 12U), tpak_len) ||
        !core_span_ok(core_read_u32(be, 24U), core_read_u32(be, 28U), tpak_len) ||
        !core_span_ok(core_read_u32(be, 32U), core_read_u32(be, 36U), tpak_len)) {
      return k_ra8_err_invalid_arg;
    }
  }
  *out_pages = pcount;
  return k_ra8_ok;
}

static ra8_err_t core_emit_tpak_page(const uint8_t*              tpak_buf,
                                     size_t                      pages_off,
                                     size_t                      bubbles_off,
                                     uint32_t                    pi,
                                     uint8_t*                    work,
                                     size_t                      work_cap,
                                     translate_e2e_emit_fn       emit_fn,
                                     void*                       emit_ctx,
                                     translate_e2e_tpak_stats_t* stats)
{
  const uint8_t* pe      = &tpak_buf[pages_off + ((size_t)pi * (size_t)k_te2e_tpak_page_bytes)];
  const uint32_t id_len  = core_read_u32(pe, 0U);
  const uint16_t pw      = core_read_u16(pe, 36U);
  const uint16_t ph      = core_read_u16(pe, 38U);
  const uint16_t mw      = core_read_u16(pe, 40U);
  const uint16_t mh      = core_read_u16(pe, 42U);
  const uint8_t  order   = pe[45U];
  const int8_t   det_zp  = (int8_t)pe[46U];
  const uint16_t bcnt    = core_read_u16(pe, 48U);
  const uint32_t det_off = core_read_u32(pe, 52U);
  const float    det_sc  = core_read_f32(pe, 56U);
  const uint32_t first_b = core_read_u32(pe, 60U);

  ra8_translate_det_bubble_t  det_b[k_te2e_max_det_comps];
  ra8_translate_det_summary_t det_sum = {};
  ra8_err_t                   err     = core_run_det_i8((const int8_t*)&tpak_buf[det_off],
                                                        mw,
                                                        mh,
                                                        pw,
                                                        ph,
                                                        det_sc,
                                                        det_zp,
                                                        order,
                                                        work,
                                                        work_cap,
                                                        det_b,
                                                        &det_sum);
  if (err != k_ra8_ok) {
    return err;
  }
  core_emit_str(emit_fn, emit_ctx, (pi > 0U) ? ",{\"page_id\":" : "{\"page_id\":");
  core_emit_json_str(emit_fn, emit_ctx, &pe[4U], id_len);
  core_emit_str(emit_fn, emit_ctx, "\r\n,\"boxes\":[");
  for (uint32_t i = 0U; i < det_sum.bubble_count; ++i) {
    core_emit_str(emit_fn, emit_ctx, (i > 0U) ? "\r\n,{\"x\":" : "{\"x\":");
    core_emit_u64(emit_fn, emit_ctx, det_b[i].x);
    core_emit_str(emit_fn, emit_ctx, ",\"y\":");
    core_emit_u64(emit_fn, emit_ctx, det_b[i].y);
    core_emit_str(emit_fn, emit_ctx, ",\"w\":");
    core_emit_u64(emit_fn, emit_ctx, det_b[i].w);
    core_emit_str(emit_fn, emit_ctx, ",\"h\":");
    core_emit_u64(emit_fn, emit_ctx, det_b[i].h);
    core_emit_str(emit_fn, emit_ctx, "}");
  }
  core_emit_str(emit_fn, emit_ctx, "]\r\n,\"bubbles\":[");
  uint64_t page_weights = 0U;
  for (uint16_t j = 0U; j < bcnt; ++j) {
    const uint8_t* be =
      &tpak_buf[bubbles_off + (((size_t)first_b + j) * (size_t)k_te2e_tpak_bubble_bytes)];
    uint8_t                    ocr_txt[k_core_max_text_bytes];
    uint8_t                    eng_txt[k_core_max_text_bytes];
    ra8_translate_ctc_result_t ctc_res = {};
    err = core_run_ocr_i8(&tpak_buf[core_read_u32(be, 0U)],
                          core_read_u32(be, 4U),
                          (const int8_t*)&tpak_buf[core_read_u32(be, 8U)],
                          core_read_u32(be, 12U),
                          core_read_u32(be, 16U),
                          core_read_f32(be, 20U),
                          (int8_t)be[40U],
                          be[41U],
                          ocr_txt,
                          sizeof(ocr_txt),
                          &ctc_res);
    if (err != k_ra8_ok) {
      return err;
    }
    size_t eng_len = 0U;
    err            = core_run_bubble_nmt(&tpak_buf[core_read_u32(be, 24U)],
                                         core_read_u32(be, 28U),
                                         &tpak_buf[core_read_u32(be, 32U)],
                                         core_read_u32(be, 36U),
                                         ocr_txt,
                                         ctc_res.text_bytes,
                                         work,
                                         work_cap,
                                         eng_txt,
                                         sizeof(eng_txt),
                                         &eng_len,
                                         &page_weights);
    if (err != k_ra8_ok) {
      return err;
    }
    core_emit_str(emit_fn, emit_ctx, (j > 0U) ? "\r\n,{\"source_text\":" : "{\"source_text\":");
    core_emit_json_str(emit_fn, emit_ctx, ocr_txt, ctc_res.text_bytes);
    core_emit_str(emit_fn, emit_ctx, "\r\n,\"english_text\":");
    core_emit_json_str(emit_fn, emit_ctx, eng_txt, eng_len);
    core_emit_str(emit_fn, emit_ctx, "}");
    stats->bubble_count++;
  }
  core_emit_str(emit_fn, emit_ctx, "]\r\n,\"furigana_dropped\":");
  core_emit_u64(emit_fn, emit_ctx, det_sum.dropped_furigana);
  core_emit_str(emit_fn, emit_ctx, ",\"weight_bytes\":");
  core_emit_u64(emit_fn, emit_ctx, page_weights);
  core_emit_str(emit_fn, emit_ctx, "}\r\n");
  stats->furigana_dropped += det_sum.dropped_furigana;
  stats->weight_bytes_read += page_weights;
  return k_ra8_ok;
}

ra8_err_t translate_e2e_run_tpak(const uint8_t*              tpak_buf,
                                 size_t                      tpak_len,
                                 uint8_t*                    model_work,
                                 size_t                      model_work_cap,
                                 translate_e2e_emit_fn       emit_fn,
                                 void*                       emit_ctx,
                                 translate_e2e_tpak_stats_t* out_stats)
{
  if ((tpak_buf == nullptr) || (model_work == nullptr) || (emit_fn == nullptr) ||
      (out_stats == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  memset(out_stats, 0, sizeof(*out_stats));
  uint32_t  page_count = 0U;
  ra8_err_t err        = core_validate_tpak(tpak_buf, tpak_len, &page_count);
  if (err != k_ra8_ok) {
    return err;
  }
  const size_t pages_off   = (size_t)k_te2e_tpak_header_bytes;
  const size_t bubbles_off = pages_off + ((size_t)page_count * (size_t)k_te2e_tpak_page_bytes);
  core_emit_str(emit_fn, emit_ctx, "EMU_TRACE_BEGIN\r\n{\"schema\":1,\"pages\":[\r\n");
  for (uint32_t pi = 0U; pi < page_count; ++pi) {
    err = core_emit_tpak_page(tpak_buf,
                              pages_off,
                              bubbles_off,
                              pi,
                              model_work,
                              model_work_cap,
                              emit_fn,
                              emit_ctx,
                              out_stats);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  core_emit_str(emit_fn, emit_ctx, "]}\r\nEMU_TRACE_END\r\n");
  out_stats->page_count = page_count;
  return k_ra8_ok;
}
