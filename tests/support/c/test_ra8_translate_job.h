/**
 * @file test_ra8_translate_job.h
 * @brief C23 unit tests for `ra8_translate_job_*` scheduler and `TRJC` checkpoints.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <string.h>

#include "ra8_translate.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_cjob_work_cap     = 131072U,
  k_cjob_ckpt_cap     = 8192U,
  k_cjob_trn1_cap     = 16384U,
  k_cjob_tgl1_cap     = 4096U,
  k_cjob_page_w       = 320U,
  k_cjob_page_h       = 240U,
  k_cjob_ocr_tensor_n = 16U,
  k_cjob_b0_x         = 20U,
  k_cjob_b0_y         = 20U,
  k_cjob_b0_w         = 180U,
  k_cjob_b0_h         = 80U,
  k_cjob_b1_x         = 30U,
  k_cjob_b1_y         = 40U,
  k_cjob_b1_w         = 190U,
  k_cjob_b1_h         = 90U,
  k_cjob_conf_bps     = 9300U,
  k_cjob_batt_min     = 25U,
  k_cjob_max_steps    = 24U,
} cjob_test_const_t;

typedef struct {
  uint32_t ocr_calls[4];
  uint32_t term_calls;
  uint32_t batch_calls;
  uint8_t  ckpt[k_cjob_ckpt_cap];
  size_t   ckpt_len;
  bool     ckpt_cleared;
  uint8_t  trn1[k_cjob_trn1_cap];
  size_t   trn1_len;
  uint8_t  tgl1[k_cjob_tgl1_cap];
  size_t   tgl1_len;
} cjob_mock_ctx_t;

static ra8_err_t cjob_run_ocr_preprocess(void)
{
  const uint8_t                            raw_roi[k_cjob_ocr_tensor_n] = {200U,
                                                                           210U,
                                                                           190U,
                                                                           220U,
                                                                           180U,
                                                                           175U,
                                                                           205U,
                                                                           215U,
                                                                           150U,
                                                                           160U,
                                                                           170U,
                                                                           180U,
                                                                           190U,
                                                                           200U,
                                                                           210U,
                                                                           220U};
  const ra8_translate_ocr_rect_t           crop = {.x = 0U, .y = 0U, .width = 4U, .height = 4U};
  const ra8_translate_ocr_image_spec_t     spec = {.stride_bytes = 4U,
                                                   .width        = 4U,
                                                   .height       = 4U,
                                                   .format = k_ra8_translate_ocr_format_gray8};
  const ra8_translate_ocr_preprocess_cfg_t cfg  = {
    .dst_width  = 2U,
    .dst_height = 2U,
    .dst_format = k_ra8_translate_ocr_format_gray8,
    .rotation   = k_ra8_translate_ocr_rot_none,
    .coord_mode = k_ra8_translate_ocr_coord_half_pixel,
  };
  const ra8_translate_ocr_quant_t qp = {
    .inv_std     = {1.0F, 1.0F, 1.0F},
    .input_scale = 1.0F,
    .quant_scale = 1.0F,
  };
  int8_t          tensor[4] = {};
  size_t          tlen      = 0U;
  const ra8_err_t prc       = ra8_translate_ocr_preprocess_i8(raw_roi,
                                                              sizeof(raw_roi),
                                                              &spec,
                                                              &crop,
                                                              &cfg,
                                                              &qp,
                                                              tensor,
                                                              sizeof(tensor),
                                                              &tlen);
  if (prc != k_ra8_ok) {
    return prc;
  }
  return (tlen == 4U) ? k_ra8_ok : k_ra8_err_validation_failed;
}

static ra8_err_t cjob_ocr_page(void*                              user,
                               const ra8_translate_job_chapter_t* chapter,
                               uint16_t                           page_index,
                               uint16_t                           max_bubbles,
                               uint16_t*                          out_w,
                               uint16_t*                          out_h,
                               ra8_translate_job_ocr_bubble_t*    out_bubbles,
                               uint16_t*                          out_count,
                               uint8_t*                           out_text,
                               size_t                             text_cap,
                               size_t*                            out_text_len)
{
  (void)chapter;
  (void)max_bubbles;
  cjob_mock_ctx_t* ctx = (cjob_mock_ctx_t*)user;
  if (page_index < 4U) {
    ctx->ocr_calls[page_index]++;
  }
  const ra8_err_t prc = cjob_run_ocr_preprocess();
  if (prc != k_ra8_ok) {
    return prc;
  }

  /* Japanese UTF-8 literals for Tanaka-san + wait (page 0) and Tanaka-san (page 1). */
  static const uint8_t k_p0[] = "\xe3\x82\xbf\xe3\x83\x8a\xe3\x82\xab\xe3\x81\x95\xe3\x82\x93"
                                " \xe5\xbe\x85\xe3\x81\xa3\xe3\x81\xa6!";
  static const uint8_t k_p1[] = "\xe3\x82\xbf\xe3\x83\x8a\xe3\x82\xab\xe3\x81\x95\xe3\x82\x93";
  const uint8_t*       src    = (page_index == 0U) ? k_p0 : k_p1;
  const size_t         slen   = (page_index == 0U) ? (sizeof(k_p0) - 1U) : (sizeof(k_p1) - 1U);
  if (slen > text_cap) {
    return k_ra8_err_invalid_size;
  }
  memcpy(out_text, src, slen);
  *out_w         = (uint16_t)k_cjob_page_w;
  *out_h         = (uint16_t)k_cjob_page_h;
  *out_count     = 1U;
  *out_text_len  = slen;
  out_bubbles[0] = (ra8_translate_job_ocr_bubble_t){
    .x                  = (uint16_t)((page_index == 0U) ? k_cjob_b0_x : k_cjob_b1_x),
    .y                  = (uint16_t)((page_index == 0U) ? k_cjob_b0_y : k_cjob_b1_y),
    .w                  = (uint16_t)((page_index == 0U) ? k_cjob_b0_w : k_cjob_b1_w),
    .h                  = (uint16_t)((page_index == 0U) ? k_cjob_b0_h : k_cjob_b1_h),
    .shape              = k_ra8_translate_shape_rect,
    .text_orientation   = k_ra8_translate_orient_horizontal,
    .ocr_confidence_bps = (uint16_t)k_cjob_conf_bps,
    .source_off         = 0U,
    .source_len         = (uint32_t)slen,
  };
  return k_ra8_ok;
}

static ra8_err_t cjob_translate_term(void*                user,
                                     ra8_translate_lang_t source_lang,
                                     const uint8_t*       src_utf8,
                                     size_t               src_len,
                                     uint8_t*             out_eng_utf8,
                                     size_t               out_eng_cap,
                                     size_t*              out_eng_len)
{
  (void)source_lang;
  (void)src_utf8;
  (void)src_len;
  cjob_mock_ctx_t* ctx = (cjob_mock_ctx_t*)user;
  ctx->term_calls++;
  static const uint8_t k_eng[] = "Tanaka";
  const size_t         elen    = sizeof(k_eng) - 1U;
  if (out_eng_cap < elen) {
    return k_ra8_err_invalid_size;
  }
  memcpy(out_eng_utf8, k_eng, elen);
  *out_eng_len = elen;
  return k_ra8_ok;
}

static ra8_err_t cjob_translate_batch(void*                           user,
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
  (void)src_bytes;
  cjob_mock_ctx_t* ctx = (cjob_mock_ctx_t*)user;
  ctx->batch_calls++;
  size_t cursor = 0U;
  for (uint16_t i = 0U; i < item_count; ++i) {
    const uint8_t* item = src_buf + src_spans[i].off;
    const bool     wait = (memchr(item, ' ', src_spans[i].len) != nullptr);
    const char*    rep  = wait ? "__G00__, hold on!" : "__G00__!";
    const size_t   rlen = strlen(rep);
    if (cursor + rlen > out_eng_cap) {
      return k_ra8_err_invalid_size;
    }
    memcpy(out_eng_buf + cursor, rep, rlen);
    out_eng_spans[i] = (ra8_translate_job_span_t){.off = (uint32_t)cursor, .len = (uint32_t)rlen};
    cursor += rlen;
  }
  return k_ra8_ok;
}

static ra8_err_t cjob_save_ckpt(void* user, const uint8_t* ckpt_bytes, size_t ckpt_len)
{
  cjob_mock_ctx_t* ctx = (cjob_mock_ctx_t*)user;
  if (ckpt_len == 0U) {
    ctx->ckpt_len     = 0U;
    ctx->ckpt_cleared = true;
    return k_ra8_ok;
  }
  if (ckpt_len > sizeof(ctx->ckpt)) {
    return k_ra8_err_invalid_size;
  }
  memcpy(ctx->ckpt, ckpt_bytes, ckpt_len);
  ctx->ckpt_len     = ckpt_len;
  ctx->ckpt_cleared = false;
  return k_ra8_ok;
}

static ra8_err_t cjob_commit(void*                              user,
                             const ra8_translate_job_chapter_t* chapter,
                             const uint8_t*                     trn1_bytes,
                             size_t                             trn1_len,
                             const uint8_t*                     tgl1_bytes,
                             size_t                             tgl1_len)
{
  (void)chapter;
  cjob_mock_ctx_t* ctx = (cjob_mock_ctx_t*)user;
  if (trn1_len > sizeof(ctx->trn1) || tgl1_len > sizeof(ctx->tgl1)) {
    return k_ra8_err_invalid_size;
  }
  memcpy(ctx->trn1, trn1_bytes, trn1_len);
  ctx->trn1_len = trn1_len;
  if (tgl1_len > 0U) {
    memcpy(ctx->tgl1, tgl1_bytes, tgl1_len);
  }
  ctx->tgl1_len = tgl1_len;
  return k_ra8_ok;
}

static ra8_translate_job_chapter_t
cjob_make_chapter(uint32_t title, uint32_t order, uint8_t unread, uint16_t pages)
{
  ra8_translate_job_chapter_t ch = {
    .title_id      = title,
    .chapter_order = order,
    .page_count    = pages,
    .lang_hint     = k_ra8_translate_lang_unknown,
    .unread        = unread,
    .trigger       = k_ra8_translate_job_trigger_mdl_download,
  };
  memset(ch.sidecar_key.chapter_hash, (int)(order + 1U), sizeof(ch.sidecar_key.chapter_hash));
  ch.sidecar_key.detector_model_version = 1U;
  ch.sidecar_key.ocr_model_version      = 1U;
  ch.sidecar_key.nmt_model_version      = 1U;
  ch.sidecar_key.glossary_revision      = 1U;
  return ch;
}

static ra8_err_t cjob_run_to_done(uint8_t*                         work,
                                  size_t                           work_bytes,
                                  const ra8_translate_job_env_t*   env,
                                  const ra8_translate_job_hooks_t* hooks,
                                  ra8_translate_job_status_t*      out_st)
{
  for (uint32_t i = 0U; i < k_cjob_max_steps; ++i) {
    const ra8_err_t rc = ra8_translate_job_step(work, work_bytes, env, hooks, out_st);
    if (rc != k_ra8_ok || out_st->state == k_ra8_translate_job_state_completed) {
      return rc;
    }
  }
  return k_ra8_err_timeout;
}

static uint8_t s_cjob_w1[k_cjob_work_cap] = {};
static uint8_t s_cjob_w2[k_cjob_work_cap] = {};

static ra8_translate_job_policy_t cjob_make_policy(uint32_t bg)
{
  return (ra8_translate_job_policy_t){
    .battery_min_pct  = (uint8_t)k_cjob_batt_min,
    .nmt_batch_max    = 4U,
    .yield_to_reader  = 1U,
    .yield_to_display = 1U,
    .overlay_style    = {.bg_color = bg},
  };
}

static ra8_translate_job_hooks_t cjob_make_hooks(cjob_mock_ctx_t*          ctx,
                                                 const ra8_widget_paint_t* paint)
{
  return (ra8_translate_job_hooks_t){
    .user            = ctx,
    .paint           = paint,
    .ocr_page        = cjob_ocr_page,
    .translate_term  = cjob_translate_term,
    .translate_batch = cjob_translate_batch,
    .save_checkpoint = cjob_save_ckpt,
    .commit_chapter  = cjob_commit,
  };
}

static void
cjob_run_golden(size_t need, const ra8_widget_paint_t* paint, uint32_t bg, cjob_mock_ctx_t* out_ctx)
{
  *out_ctx                                = (cjob_mock_ctx_t){};
  const ra8_translate_job_hooks_t   hooks = cjob_make_hooks(out_ctx, paint);
  const ra8_translate_job_policy_t  pol   = cjob_make_policy(bg);
  const ra8_translate_job_chapter_t ch    = cjob_make_chapter(11U, 1U, 1U, 2U);
  const ra8_translate_job_env_t     env   = {.battery_pct = 90U};
  ra8_translate_job_status_t        st    = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_job_init(s_cjob_w1, need, &pol));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_job_enqueue(s_cjob_w1, need, &ch));
  TEST_ASSERT_EQ(k_ra8_ok, cjob_run_to_done(s_cjob_w1, need, &env, &hooks, &st));
  TEST_ASSERT(out_ctx->ckpt_cleared && out_ctx->trn1_len > 0U);
}

static void cjob_run_reset_resume(size_t                    need,
                                  const ra8_widget_paint_t* paint,
                                  uint32_t                  bg,
                                  const cjob_mock_ctx_t*    golden)
{
  cjob_mock_ctx_t                   rctx   = {};
  const ra8_translate_job_hooks_t   rhooks = cjob_make_hooks(&rctx, paint);
  const ra8_translate_job_policy_t  pol    = cjob_make_policy(bg);
  const ra8_translate_job_chapter_t ch     = cjob_make_chapter(11U, 1U, 1U, 2U);
  const ra8_translate_job_env_t     env    = {.battery_pct = 90U};
  ra8_translate_job_status_t        st     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_job_init(s_cjob_w2, need, &pol));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_job_enqueue(s_cjob_w2, need, &ch));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_job_step(s_cjob_w2, need, &env, &rhooks, &st));
  uint8_t      snap[k_cjob_ckpt_cap] = {};
  const size_t slen                  = rctx.ckpt_len;
  memcpy(snap, rctx.ckpt, slen);
  const ra8_translate_job_env_t yenv = {.battery_pct = 10U, .reader_active = 1U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_job_step(s_cjob_w2, need, &yenv, &rhooks, &st));
  TEST_ASSERT_EQ(k_ra8_translate_job_state_yielded, st.state);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_job_cancel(s_cjob_w2, need, 11U, 1U, &rhooks));
  memset(s_cjob_w2, 0xA5, need);
  rctx.ocr_calls[0] = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_job_init(s_cjob_w2, need, &pol));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_job_restore_checkpoint(s_cjob_w2, need, snap, slen));
  ra8_translate_job_chapter_state_t qst = k_ra8_translate_job_chapter_none;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_job_query_chapter(s_cjob_w2, need, 11U, 1U, &qst));
  TEST_ASSERT_EQ(k_ra8_translate_job_chapter_translating, qst);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_job_status(s_cjob_w2, need, &st));
  const ra8_translate_job_env_t cenv = {.battery_pct = 8U, .charging = 1U};
  TEST_ASSERT_EQ(k_ra8_ok, cjob_run_to_done(s_cjob_w2, need, &cenv, &rhooks, &st));
  TEST_ASSERT(rctx.ocr_calls[0] == 0U && rctx.trn1_len == golden->trn1_len &&
              memcmp(golden->trn1, rctx.trn1, golden->trn1_len) == 0);
}
