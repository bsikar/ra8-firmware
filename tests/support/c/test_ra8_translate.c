/**
 * @file test_ra8_translate.c
 * @brief Host C23 ABI acceptance tests for `libs/ra8_translate`.
 *
 * Exercises all public C ABI exports declared in `ra8_translate.h` against the
 * `libra8_translate.a` archive: round-trip `TRN1` sidecar serialization,
 * zero-copy UTF-8 span lookups, cache invalidation key matching, and fail-closed
 * null/truncation/CRC-mismatch error precedence.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_translate.h"
#include "unity_minimal.h"

static ra8_translate_sidecar_key_t make_key(ra8_translate_lang_t lang)
{
  ra8_translate_sidecar_key_t key = {
    .detector_model_version = 11U,
    .ocr_model_version      = 22U,
    .nmt_model_version      = 33U,
    .glossary_revision      = 7U,
    .source_lang            = lang,
    .reserved               = {0U, 0U, 0U},
  };
  for (size_t i = 0U; i < k_ra8_translate_chapter_hash_bytes; ++i) {
    key.chapter_hash[i] = (uint8_t)(0xA0U + (uint8_t)i);
  }
  return key;
}

static void test_null_guards_and_no_write(void)
{
  ra8_translate_sidecar_header_t hdr = {.total_bytes = 0x12345678U};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_translate_sidecar_open(nullptr, 0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_translate_sidecar_open(nullptr, 16U, &hdr));
  TEST_ASSERT_EQ(0x12345678U, hdr.total_bytes);

  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_translate_sidecar_open(nullptr, 0U, &hdr));
  TEST_ASSERT_EQ(0x12345678U, hdr.total_bytes);

  bool matched = true;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_translate_sidecar_key_matches(nullptr, nullptr, &matched));
  TEST_ASSERT(matched);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_translate_sidecar_required_bytes(1U, 1U, 1U, 10U, nullptr));
}

static void test_round_trip_and_invalidation(void)
{
  const ra8_translate_sidecar_key_t  key     = make_key(k_ra8_translate_lang_japanese);
  const ra8_translate_sidecar_page_t pages[] = {
    {.page_index   = 0U,
     .bubble_count = 2U,
     .first_bubble = 0U,
     .page_width   = 800U,
     .page_height  = 1200U},
  };
  const uint8_t                        pool[]    = "kanjihi there\x00"; /* 13 useful UTF-8 bytes */
  const uint16_t                       breaks[]  = {2U, 8U};
  const ra8_translate_sidecar_bubble_t bubbles[] = {
    {
      .x                  = 100U,
      .y                  = 40U,
      .w                  = 160U,
      .h                  = 90U,
      .shape              = k_ra8_translate_shape_rounded_rect,
      .text_orientation   = k_ra8_translate_orient_vertical_rl,
      .corner_radius      = 8U,
      .reading_order      = 1U,
      .ocr_confidence_bps = 9550U,
      .font_size_px       = 20U,
      .line_count         = 2U,
      .first_line_break   = 0U,
      .source_text_off    = 0U,
      .source_text_len    = 5U,
      .english_text_off   = 5U,
      .english_text_len   = 8U,
      .bubble_flags       = k_ra8_translate_bflag_glossary,
      .reserved           = {0U, 0U, 0U},
    },
    {
      .x                  = 10U,
      .y                  = 10U,
      .w                  = 50U,
      .h                  = 50U,
      .shape              = k_ra8_translate_shape_rect,
      .text_orientation   = k_ra8_translate_orient_horizontal,
      .corner_radius      = 0U,
      .reading_order      = 0U,
      .ocr_confidence_bps = 10000U,
      .font_size_px       = 0U,
      .line_count         = 0U,
      .first_line_break   = 2U,
      .source_text_off    = 0U,
      .source_text_len    = 0U,
      .english_text_off   = 5U,
      .english_text_len   = 0U,
      .bubble_flags       = 0U,
      .reserved           = {0U, 0U, 0U},
    },
  };
  const uint32_t pool_len = 13U;
  size_t         needed   = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_sidecar_required_bytes(1U, 2U, 2U, pool_len, &needed));
  TEST_ASSERT_EQ((size_t)(96U + 12U + 88U + 4U + 13U), needed);

  uint8_t buf[512] = {};
  size_t  written  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_sidecar_write(&key,
                                             pages,
                                             1U,
                                             bubbles,
                                             2U,
                                             breaks,
                                             2U,
                                             pool,
                                             pool_len,
                                             buf,
                                             sizeof(buf),
                                             &written));
  TEST_ASSERT_EQ(needed, written);

  ra8_translate_sidecar_header_t hdr = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_sidecar_open(buf, written, &hdr));
  TEST_ASSERT_EQ(1U, hdr.page_count);
  TEST_ASSERT_EQ(2U, hdr.bubble_count);

  bool matches = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_sidecar_key_matches(&hdr, &key, &matches));
  TEST_ASSERT(matches);

  ra8_translate_sidecar_key_t stale_key = key;
  stale_key.nmt_model_version += 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_sidecar_key_matches(&hdr, &stale_key, &matches));
  TEST_ASSERT(!matches);

  ra8_translate_sidecar_page_t page = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_sidecar_page_at(buf, written, 0U, &page));
  TEST_ASSERT_EQ(800U, page.page_width);

  ra8_translate_sidecar_bubble_t b0 = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_sidecar_bubble_at(buf, written, 0U, &b0));
  TEST_ASSERT_EQ(9550U, b0.ocr_confidence_bps);

  const uint8_t* src_ptr = nullptr;
  uint32_t       src_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_sidecar_source_text(buf, written, 0U, &src_ptr, &src_len));
  TEST_ASSERT_NOT_NULL(src_ptr);
  TEST_ASSERT_EQ(5U, src_len);
  TEST_ASSERT_EQ(0, memcmp(src_ptr, "kanji", 5U));

  const uint8_t* eng_ptr = nullptr;
  uint32_t       eng_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_sidecar_english_text(buf, written, 0U, &eng_ptr, &eng_len));
  TEST_ASSERT_NOT_NULL(eng_ptr);
  TEST_ASSERT_EQ(8U, eng_len);
  TEST_ASSERT_EQ(0, memcmp(eng_ptr, "hi there", 8U));

  uint16_t out_breaks[4] = {};
  uint16_t out_count     = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_sidecar_line_breaks(buf, written, 0U, out_breaks, 4U, &out_count));
  TEST_ASSERT_EQ(2U, out_count);
  TEST_ASSERT_EQ(2U, out_breaks[0]);
  TEST_ASSERT_EQ(8U, out_breaks[1]);

  /* Corrupt CRC must be detected fail-closed */
  buf[written - 1U] ^= 0x80U;
  TEST_ASSERT_EQ(k_ra8_err_crc_mismatch, ra8_translate_sidecar_open(buf, written, &hdr));
}

static void test_language_selection(void)
{
  ra8_translate_lang_t hint = k_ra8_translate_lang_unknown;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_lang_from_hint((const uint8_t*)"ko-KR", 5U, &hint));
  TEST_ASSERT_EQ(k_ra8_translate_lang_korean, hint);

  ra8_translate_lang_selection_t sel       = {};
  const uint8_t                  ko_text[] = "\xec\x95\x88\xeb\x85\x95";
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_translate_lang_select(k_ra8_translate_lang_unknown, ko_text, sizeof(ko_text) - 1U, &sel));
  TEST_ASSERT_EQ(k_ra8_translate_lang_korean, sel.language);
  TEST_ASSERT_EQ(k_ra8_translate_ocr_korean, sel.ocr_model);
  TEST_ASSERT_EQ(k_ra8_translate_nmt_ko_en, sel.nmt_model);

  const uint8_t ja_text[] = "\xe3\x81\x93\xe3\x82\x93";
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_translate_lang_select(k_ra8_translate_lang_japanese, ja_text, sizeof(ja_text) - 1U, &sel));
  TEST_ASSERT_EQ(k_ra8_translate_lang_japanese, sel.language);
  TEST_ASSERT_EQ(k_ra8_translate_ocr_japanese, sel.ocr_model);
  TEST_ASSERT_EQ(k_ra8_translate_nmt_ja_en, sel.nmt_model);

  const uint8_t kanji[] = "\xe6\xbc\xa2\xe5\xad\x97";
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_translate_lang_select(k_ra8_translate_lang_chinese, kanji, sizeof(kanji) - 1U, &sel));
  TEST_ASSERT_EQ(k_ra8_translate_lang_chinese, sel.language);
  TEST_ASSERT_EQ(k_ra8_translate_lang_reason_deferred_to_hint, sel.reason);
  TEST_ASSERT_EQ(k_ra8_translate_ocr_chinese, sel.ocr_model);
  TEST_ASSERT_EQ(k_ra8_translate_nmt_zh_en, sel.nmt_model);

  const uint8_t bad_utf8[] = {0xFFU, 0xFEU};
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_translate_lang_select(k_ra8_translate_lang_chinese, bad_utf8, sizeof(bad_utf8), &sel));
  TEST_ASSERT_EQ(k_ra8_translate_lang_chinese, sel.language);
}

static void test_glossary_round_trip(void)
{
  uint8_t tgl_a[256] = {};
  size_t  len_a      = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_translate_glossary_init(k_ra8_translate_lang_japanese, 1U, tgl_a, sizeof(tgl_a), &len_a));

  const uint8_t tanaka[]   = "\xe3\x82\xbf\xe3\x83\x8a\xe3\x82\xab";
  uint8_t       tgl_b[256] = {};
  size_t        len_b      = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_glossary_upsert(tgl_a,
                                               len_a,
                                               tanaka,
                                               sizeof(tanaka) - 1U,
                                               (const uint8_t*)"Tanaka",
                                               6U,
                                               tgl_b,
                                               sizeof(tgl_b),
                                               &len_b));

  const uint8_t* eng_ptr = nullptr;
  size_t         eng_len = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_translate_glossary_lookup(tgl_b, len_b, tanaka, sizeof(tanaka) - 1U, &eng_ptr, &eng_len));
  TEST_ASSERT_NOT_NULL(eng_ptr);
  TEST_ASSERT_EQ(6U, eng_len);
  TEST_ASSERT_EQ(0, memcmp(eng_ptr, "Tanaka", 6U));

  const uint8_t bub[]      = "\xe3\x82\xbf\xe3\x83\x8a\xe3\x82\xab\xe3\x81\x95\xe3\x82\x93";
  uint8_t       masked[64] = {};
  size_t        masked_len = 0U;
  uint8_t       plan[32]   = {};
  size_t        plan_len   = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_glossary_mask(tgl_b,
                                             len_b,
                                             bub,
                                             sizeof(bub) - 1U,
                                             masked,
                                             sizeof(masked),
                                             &masked_len,
                                             plan,
                                             sizeof(plan),
                                             &plan_len));
  TEST_ASSERT_EQ(7U, masked_len);
  TEST_ASSERT_EQ(0, memcmp(masked, "__G00__", 7U));

  uint8_t     restored[64] = {};
  size_t      restored_len = 0U;
  const char* model_out    = "See __G00__!";
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_glossary_restore(tgl_b,
                                                len_b,
                                                plan,
                                                plan_len,
                                                (const uint8_t*)model_out,
                                                strlen(model_out),
                                                restored,
                                                sizeof(restored),
                                                &restored_len));
  TEST_ASSERT_EQ(15U, restored_len);
  TEST_ASSERT_EQ(0, memcmp(restored, "See Tanaka-san!", 15U));
}

static void test_ocr_preprocess(void)
{
  bool has_helium = true;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_translate_ocr_has_helium(nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_ocr_has_helium(&has_helium));

  size_t req_bytes = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_translate_ocr_required_bytes(3U, 2U, k_ra8_translate_ocr_format_gray8, nullptr));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_translate_ocr_required_bytes(3U, 2U, k_ra8_translate_ocr_format_gray8, &req_bytes));
  TEST_ASSERT_EQ(6U, req_bytes);

  const uint8_t                        src[6] = {10U, 11U, 20U, 21U, 30U, 31U};
  const ra8_translate_ocr_image_spec_t spec   = {
    .stride_bytes = 2U,
    .width        = 2U,
    .height       = 3U,
    .format       = k_ra8_translate_ocr_format_gray8,
    .reserved     = {0U, 0U, 0U},
  };
  const ra8_translate_ocr_rect_t           crop = {.x = 0U, .y = 0U, .width = 2U, .height = 3U};
  const ra8_translate_ocr_preprocess_cfg_t cfg  = {
    .dst_width  = 3U,
    .dst_height = 2U,
    .dst_format = k_ra8_translate_ocr_format_gray8,
    .rotation   = k_ra8_translate_ocr_rot_ccw_90,
    .coord_mode = k_ra8_translate_ocr_coord_align_corners,
    .reserved   = 0U,
  };
  const ra8_translate_ocr_quant_t quant = {
    .mean        = {0.0f, 0.0f, 0.0f},
    .inv_std     = {1.0f, 1.0f, 1.0f},
    .input_scale = 1.0f,
    .quant_scale = 2.0f,
    .zero_point  = -10,
    .reserved    = {0U, 0U, 0U},
  };

  uint8_t out_u8[6] = {};
  size_t  written   = 99U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_translate_ocr_resize_u8(src, sizeof(src), &spec, &crop, &cfg, out_u8, 2U, &written));
  TEST_ASSERT_EQ(99U, written);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_ocr_resize_u8(src,
                                             sizeof(src),
                                             &spec,
                                             &crop,
                                             &cfg,
                                             out_u8,
                                             sizeof(out_u8),
                                             &written));
  TEST_ASSERT_EQ(6U, written);
  const uint8_t expected_rot[6] = {11U, 21U, 31U, 10U, 20U, 30U};
  TEST_ASSERT_EQ(0, memcmp(out_u8, expected_rot, sizeof(expected_rot)));

  int8_t out_i8[6]   = {};
  int8_t fused_i8[6] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_ocr_normalize_i8(out_u8,
                                                sizeof(out_u8),
                                                k_ra8_translate_ocr_format_gray8,
                                                &quant,
                                                out_i8,
                                                sizeof(out_i8),
                                                &written));
  TEST_ASSERT_EQ(6U, written);
  TEST_ASSERT_EQ(-5, out_i8[0]);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_ocr_preprocess_i8(src,
                                                 sizeof(src),
                                                 &spec,
                                                 &crop,
                                                 &cfg,
                                                 &quant,
                                                 fused_i8,
                                                 sizeof(fused_i8),
                                                 &written));
  TEST_ASSERT_EQ(6U, written);
  TEST_ASSERT_EQ(0, memcmp(out_i8, fused_i8, sizeof(out_i8)));
}

typedef struct test_tvoc_piece_s {
  const char* text;
  float       score;
  uint8_t     kind;
} test_tvoc_piece_t;

static void test_wr_u16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8U) & 0xFFU);
}

static void test_wr_u32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8U) & 0xFFU);
  p[2] = (uint8_t)((v >> 16U) & 0xFFU);
  p[3] = (uint8_t)((v >> 24U) & 0xFFU);
}

static uint32_t test_crc32_ieee(const uint8_t* data, size_t len)
{
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0U; i < len; ++i) {
    crc ^= data[i];
    for (uint32_t b = 0U; b < 8U; ++b) {
      uint32_t mask = (crc & 1U) ? 0xEDB88320U : 0U;
      crc           = (crc >> 1U) ^ mask;
    }
  }
  return ~crc;
}

static size_t build_test_tvoc(uint8_t                  model_type,
                              uint8_t                  flags,
                              uint32_t                 bos_id,
                              uint32_t                 eos_id,
                              const test_tvoc_piece_t* pieces,
                              uint32_t                 count,
                              uint8_t*                 out_buf)
{
  const size_t hdr_bytes   = 56U;
  const size_t table_off   = hdr_bytes;
  const size_t sorted_off  = table_off + (size_t)count * 12U;
  const size_t pool_off    = sorted_off + (size_t)count * 4U;
  size_t       pool_cursor = 0U;
  float        min_score   = -10.0F;
  bool         saw_normal  = false;

  for (uint32_t i = 0U; i < count; ++i) {
    size_t   len = strlen(pieces[i].text);
    uint32_t score_bits;
    memcpy(&score_bits, &pieces[i].score, sizeof(score_bits));
    if (pieces[i].kind == k_ra8_translate_tok_piece_normal &&
        (!saw_normal || pieces[i].score < min_score)) {
      min_score  = pieces[i].score;
      saw_normal = true;
    }
    uint8_t* rec = out_buf + table_off + (size_t)i * 12U;
    test_wr_u32(rec + 0U, (uint32_t)pool_cursor);
    test_wr_u16(rec + 4U, (uint16_t)len);
    rec[6] = pieces[i].kind;
    rec[7] = 0U;
    test_wr_u32(rec + 8U, score_bits);
    memcpy(out_buf + pool_off + pool_cursor, pieces[i].text, len);
    pool_cursor += len;
  }

  uint32_t sorted[32];
  for (uint32_t i = 0U; i < count; ++i) {
    sorted[i] = i;
  }
  for (uint32_t i = 1U; i < count; ++i) {
    uint32_t key = sorted[i];
    uint32_t j   = i;
    while (j > 0U) {
      uint32_t prev = sorted[j - 1U];
      int      cmp  = strcmp(pieces[prev].text, pieces[key].text);
      if (cmp < 0 || (cmp == 0 && prev < key)) {
        break;
      }
      sorted[j] = prev;
      --j;
    }
    sorted[j] = key;
  }
  for (uint32_t i = 0U; i < count; ++i) {
    test_wr_u32(out_buf + sorted_off + (size_t)i * 4U, sorted[i]);
  }

  size_t   total = pool_off + pool_cursor;
  uint32_t min_bits;
  memcpy(&min_bits, &min_score, sizeof(min_bits));
  test_wr_u32(out_buf + 0U, 0x434F5654U);
  test_wr_u16(out_buf + 4U, 1U);
  out_buf[6] = model_type;
  out_buf[7] = flags;
  test_wr_u32(out_buf + 8U, (uint32_t)total);
  test_wr_u32(out_buf + 12U, 0U);
  test_wr_u32(out_buf + 16U, count);
  test_wr_u32(out_buf + 20U, 0U);
  test_wr_u32(out_buf + 24U, bos_id);
  test_wr_u32(out_buf + 28U, eos_id);
  test_wr_u32(out_buf + 32U, k_ra8_translate_tok_id_none);
  test_wr_u32(out_buf + 36U, min_bits);
  test_wr_u32(out_buf + 40U, (uint32_t)table_off);
  test_wr_u32(out_buf + 44U, (uint32_t)sorted_off);
  test_wr_u32(out_buf + 48U, (uint32_t)pool_off);
  test_wr_u32(out_buf + 52U, 0U);
  test_wr_u32(out_buf + 12U, test_crc32_ieee(out_buf, total));
  return total;
}

static void test_tokenizer_unigram_bpe_and_errors(void)
{
  const test_tvoc_piece_t uni_pieces[] = {
    {"<unk>", 0.0F, k_ra8_translate_tok_piece_unknown},
    {"<s>", 0.0F, k_ra8_translate_tok_piece_control},
    {"</s>", 0.0F, k_ra8_translate_tok_piece_control},
    {"__G00__", 0.0F, k_ra8_translate_tok_piece_user_defined},
    {"\xe2\x96\x81", -2.0F, k_ra8_translate_tok_piece_normal},
    {"\xe2\x96\x81\xeb\x82\x98\xeb\x8a\x94", -1.0F, k_ra8_translate_tok_piece_normal},
    {"\xe2\x96\x81\xe7\xa7\x81\xe3\x81\xaf", -1.0F, k_ra8_translate_tok_piece_normal},
    {"\xe2\x96\x81\xe4\xbb\x96\xe5\x9c\xa8", -1.0F, k_ra8_translate_tok_piece_normal},
    {"\xe2\x96\x81Hello", -0.5F, k_ra8_translate_tok_piece_normal},
    {"!", -0.4F, k_ra8_translate_tok_piece_normal},
  };
  uint8_t tvoc[1024] = {};
  uint8_t flags =
    (uint8_t)(k_ra8_translate_tok_flag_add_dummy_prefix | k_ra8_translate_tok_flag_remove_extra_ws |
              k_ra8_translate_tok_flag_escape_ws | k_ra8_translate_tok_flag_norm_fullwidth);
  size_t tvoc_len =
    build_test_tvoc(k_ra8_translate_tok_unigram, flags, 1U, 2U, uni_pieces, 10U, tvoc);

  ra8_translate_vocab_header_t hdr = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_translate_tok_open(nullptr, tvoc_len, &hdr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_translate_tok_open(nullptr, tvoc_len, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_tok_open(tvoc, tvoc_len, &hdr));
  TEST_ASSERT_EQ(10U, hdr.vocab_size);

  const uint8_t*                 p_ptr  = nullptr;
  uint16_t                       p_len  = 0U;
  ra8_translate_tok_piece_kind_t p_kind = k_ra8_translate_tok_piece_normal;
  float                          p_scr  = 0.0F;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_tok_piece_info(tvoc, tvoc_len, 8U, &p_ptr, &p_len, &p_kind, &p_scr));
  TEST_ASSERT_EQ(8U, p_len);
  TEST_ASSERT_EQ(0, memcmp(p_ptr, "\xe2\x96\x81Hello", 8U));

  size_t work_bytes = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_tok_work_bytes(32U, k_ra8_translate_tok_unigram, &work_bytes));

  uint8_t       work[2048] = {};
  uint32_t      ids[16]    = {};
  size_t        id_count   = 0U;
  const uint8_t ja[]       = "\xe7\xa7\x81\xe3\x81\xaf\xef\xbc\x81";
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_tok_encode(tvoc,
                                          tvoc_len,
                                          ja,
                                          sizeof(ja) - 1U,
                                          k_ra8_translate_tok_enc_add_eos,
                                          work,
                                          work_bytes,
                                          ids,
                                          16U,
                                          &id_count));
  TEST_ASSERT_EQ(3U, id_count);
  TEST_ASSERT_EQ(6U, ids[0]);
  TEST_ASSERT_EQ(9U, ids[1]);
  TEST_ASSERT_EQ(2U, ids[2]);

  const uint32_t en_ids[] = {1U, 8U, 4U, 3U, 9U, 2U};
  uint8_t        dec[64]  = {};
  size_t         dec_len  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_tok_decode(tvoc, tvoc_len, en_ids, 6U, dec, sizeof(dec), &dec_len));
  TEST_ASSERT_EQ(14U, dec_len);
  TEST_ASSERT_EQ(0, memcmp(dec, "Hello __G00__!", 14U));

  /* BPE model round-trip through the same C ABI entry points. */
  const test_tvoc_piece_t bpe_pieces[] = {
    {"<unk>", 0.0F, k_ra8_translate_tok_piece_unknown},
    {"\xe2\x96\x81", -9.0F, k_ra8_translate_tok_piece_normal},
    {"a", -10.0F, k_ra8_translate_tok_piece_normal},
    {"b", -11.0F, k_ra8_translate_tok_piece_normal},
    {"ab", -1.0F, k_ra8_translate_tok_piece_normal},
    {"\xe2\x96\x81"
     "ab",
     -2.0F,
     k_ra8_translate_tok_piece_normal},
  };
  uint8_t bpe_tvoc[512] = {};
  size_t  bpe_len       = build_test_tvoc(k_ra8_translate_tok_bpe,
                                          flags,
                                          k_ra8_translate_tok_id_none,
                                          k_ra8_translate_tok_id_none,
                                          bpe_pieces,
                                          6U,
                                          bpe_tvoc);
  size_t  bpe_work      = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_tok_work_bytes(8U, k_ra8_translate_tok_bpe, &bpe_work));
  size_t bpe_n = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_tok_encode(bpe_tvoc,
                                          bpe_len,
                                          (const uint8_t*)"ab",
                                          2U,
                                          0U,
                                          work,
                                          bpe_work,
                                          ids,
                                          16U,
                                          &bpe_n));
  TEST_ASSERT_EQ(1U, bpe_n);
  TEST_ASSERT_EQ(5U, ids[0]);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_translate_tok_decode(bpe_tvoc, bpe_len, ids, bpe_n, dec, sizeof(dec), &dec_len));
  TEST_ASSERT_EQ(2U, dec_len);
  TEST_ASSERT_EQ(0, memcmp(dec, "ab", 2U));

  /* Corrupt CRC triggers failure without touching output. */
  uint32_t sentinel_id = 0xDEADBEEFU;
  ids[0]               = sentinel_id;
  id_count             = 77U;
  tvoc[tvoc_len - 1U] ^= 0x01U;
  TEST_ASSERT_EQ(k_ra8_err_crc_mismatch,
                 ra8_translate_tok_encode(tvoc,
                                          tvoc_len,
                                          ja,
                                          sizeof(ja) - 1U,
                                          0U,
                                          work,
                                          work_bytes,
                                          ids,
                                          16U,
                                          &id_count));
  TEST_ASSERT_EQ(sentinel_id, ids[0]);
  TEST_ASSERT_EQ(77U, id_count);
}

typedef enum : uint32_t {
  k_ovl_page_w         = 200U,
  k_ovl_page_h         = 260U,
  k_ovl_art_bg         = 0x707070U,
  k_ovl_white          = 0xFFFFFFU,
  k_ovl_black          = 0x000000U,
  k_ovl_b0_x           = 16U,
  k_ovl_b0_y           = 20U,
  k_ovl_b0_w           = 140U,
  k_ovl_b0_h           = 70U,
  k_ovl_b1_x           = 20U,
  k_ovl_b1_y           = 110U,
  k_ovl_b1_w           = 64U,
  k_ovl_b1_h           = 130U,
  k_ovl_pad            = 4U,
  k_ovl_cap_breaks     = 32U,
  k_ovl_trn1_cap       = 1024U,
  k_ovl_dmg_cap        = 4U,
  k_ovl_font_px_small  = 16U,
  k_ovl_font_px_medium = 32U,
  k_ovl_font_px_large  = 45U,
  k_ovl_glyph_w_small  = 8U,
  k_ovl_glyph_w_medium = 14U,
  k_ovl_glyph_w_large  = 22U,
} test_ovl_const_t;

typedef struct {
  uint32_t pixels[k_ovl_page_w * k_ovl_page_h];
  uint32_t draw_calls;
  uint32_t fill_calls;
} test_ovl_canvas_t;

static void ovl_fill_rect(void* user, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
  test_ovl_canvas_t* cv = (test_ovl_canvas_t*)user;
  cv->fill_calls++;
  for (int32_t py = y; py < y + h; ++py) {
    if (py < 0 || py >= (int32_t)k_ovl_page_h) {
      continue;
    }
    for (int32_t px = x; px < x + w; ++px) {
      if (px < 0 || px >= (int32_t)k_ovl_page_w) {
        continue;
      }
      cv->pixels[(size_t)py * k_ovl_page_w + (size_t)px] = color;
    }
  }
}

static uint32_t ovl_style_metrics(ra8_widget_text_size_t sz, uint32_t* out_char_w)
{
  if (sz == k_ra8_widget_text_size_ui_26) {
    *out_char_w = k_ovl_glyph_w_medium;
    return k_ovl_font_px_medium;
  }
  if (sz == k_ra8_widget_text_size_body_38 || sz == k_ra8_widget_text_size_3) {
    *out_char_w = k_ovl_glyph_w_large;
    return k_ovl_font_px_large;
  }
  *out_char_w = k_ovl_glyph_w_medium;
  return k_ovl_font_px_medium;
}

static void ovl_text_size(void* user, const char* str, int32_t* out_w, int32_t* out_h)
{
  (void)user;
  *out_w = (int32_t)(strlen(str) * k_ovl_glyph_w_small);
  *out_h = (int32_t)k_ovl_font_px_small;
}

static void ovl_text_size_style(void*                    user,
                                const char*              str,
                                ra8_widget_text_face_t   face,
                                ra8_widget_text_weight_t weight,
                                ra8_widget_text_size_t   sz,
                                int32_t*                 out_w,
                                int32_t*                 out_h)
{
  (void)user;
  (void)face;
  (void)weight;
  uint32_t cw = 0U;
  *out_h      = (int32_t)ovl_style_metrics(sz, &cw);
  *out_w      = (int32_t)(strlen(str) * (size_t)cw);
}

static void
ovl_draw_text(void* user, int32_t x, int32_t y, const char* str, uint32_t fg, uint32_t bg)
{
  test_ovl_canvas_t* cv = (test_ovl_canvas_t*)user;
  (void)bg;
  cv->draw_calls++;
  if (y >= 0 && y < (int32_t)k_ovl_page_h && x >= 0 && x < (int32_t)k_ovl_page_w &&
      str[0] != '\0') {
    cv->pixels[(size_t)y * k_ovl_page_w + (size_t)x] = fg;
  }
}

static void ovl_draw_text_style(void*                    user,
                                int32_t                  x,
                                int32_t                  y,
                                const char*              str,
                                ra8_widget_text_face_t   face,
                                ra8_widget_text_weight_t weight,
                                ra8_widget_text_size_t   sz,
                                uint32_t                 fg,
                                uint32_t                 bg)
{
  (void)face;
  (void)weight;
  (void)sz;
  ovl_draw_text(user, x, y, str, fg, bg);
}

static void test_overlay_fit_render_and_damage(void)
{
  test_ovl_canvas_t cv = {};
  for (size_t i = 0U; i < k_ovl_page_w * k_ovl_page_h; ++i) {
    cv.pixels[i] = k_ovl_art_bg;
  }

  const ra8_widget_paint_t paint = {
    .user            = &cv,
    .fill_rect       = ovl_fill_rect,
    .draw_text       = ovl_draw_text,
    .text_size       = ovl_text_size,
    .draw_text_style = ovl_draw_text_style,
    .text_size_style = ovl_text_size_style,
  };
  const ra8_translate_overlay_style_t style = {
    .fg_color = k_ovl_black,
    .bg_color = k_ovl_white,
    .pad      = (int16_t)k_ovl_pad,
    .align    = k_ra8_widget_align_center,
    .face     = k_ra8_widget_text_face_sans,
    .weight   = k_ra8_widget_text_weight_regular,
  };
  const ra8_translate_overlay_bubble_spec_t wide_b = {
    .x     = (uint16_t)k_ovl_b0_x,
    .y     = (uint16_t)k_ovl_b0_y,
    .w     = (uint16_t)k_ovl_b0_w,
    .h     = (uint16_t)k_ovl_b0_h,
    .shape = k_ra8_translate_shape_rect,
  };
  const ra8_translate_overlay_bubble_spec_t narrow_b = {
    .x                = (uint16_t)k_ovl_b1_x,
    .y                = (uint16_t)k_ovl_b1_y,
    .w                = (uint16_t)k_ovl_b1_w,
    .h                = (uint16_t)k_ovl_b1_h,
    .shape            = k_ra8_translate_shape_ellipse,
    .text_orientation = k_ra8_translate_orient_vertical_rl,
  };

  ra8_ui_rect_t safe = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_overlay_safe_rect(&narrow_b, &style, &safe));
  TEST_ASSERT(safe.w > 0 && safe.h > 0 && safe.w < (int32_t)k_ovl_b1_w);

  const uint8_t               eng0[]                = "Hey!";
  const uint8_t               eng1[]                = "Wait for me!";
  uint16_t                    br0[k_ovl_cap_breaks] = {};
  uint16_t                    br1[k_ovl_cap_breaks] = {};
  ra8_translate_overlay_fit_t fit0                  = {};
  ra8_translate_overlay_fit_t fit1                  = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_overlay_fit_text(&paint,
                                                &wide_b,
                                                &style,
                                                eng0,
                                                sizeof(eng0) - 1U,
                                                br0,
                                                k_ovl_cap_breaks,
                                                &fit0));
  TEST_ASSERT_EQ(k_ovl_font_px_large, fit0.font_size_px);
  TEST_ASSERT_EQ(1U, fit0.line_count);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_overlay_fit_text(&paint,
                                                &narrow_b,
                                                &style,
                                                eng1,
                                                sizeof(eng1) - 1U,
                                                br1,
                                                k_ovl_cap_breaks,
                                                &fit1));
  TEST_ASSERT_EQ(k_ovl_font_px_small, fit1.font_size_px);
  TEST_ASSERT(fit1.line_count >= 2U);

  ra8_ui_rect_t direct_dmg = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_overlay_render_bubble(&paint,
                                                     &wide_b,
                                                     &style,
                                                     fit0.font_size_px,
                                                     eng0,
                                                     sizeof(eng0) - 1U,
                                                     br0,
                                                     fit0.line_count,
                                                     &direct_dmg));
  TEST_ASSERT_EQ((int32_t)k_ovl_b0_x, direct_dmg.x);
  TEST_ASSERT_EQ((int32_t)k_ovl_b0_w, direct_dmg.w);
  TEST_ASSERT_EQ(k_ovl_white,
                 cv.pixels[(size_t)(k_ovl_b0_y + 1U) * k_ovl_page_w + k_ovl_b0_x + 1U]);
  TEST_ASSERT_EQ(k_ovl_art_bg, cv.pixels[0]);

  for (size_t i = 0U; i < k_ovl_page_w * k_ovl_page_h; ++i) {
    cv.pixels[i] = k_ovl_art_bg;
  }

  const ra8_translate_sidecar_key_t  key     = make_key(k_ra8_translate_lang_japanese);
  const ra8_translate_sidecar_page_t pages[] = {
    {.page_index   = 0U,
     .bubble_count = 2U,
     .first_bubble = 0U,
     .page_width   = (uint16_t)k_ovl_page_w,
     .page_height  = (uint16_t)k_ovl_page_h},
  };
  uint8_t pool[k_ovl_cap_breaks * 2U] = {};
  memcpy(pool, eng0, sizeof(eng0) - 1U);
  memcpy(pool + (sizeof(eng0) - 1U), eng1, sizeof(eng1) - 1U);
  uint16_t all_breaks[k_ovl_cap_breaks * 2U] = {};
  for (size_t i = 0U; i < fit0.line_count; ++i) {
    all_breaks[i] = br0[i];
  }
  for (size_t i = 0U; i < fit1.line_count; ++i) {
    all_breaks[fit0.line_count + i] = br1[i];
  }
  const ra8_translate_sidecar_bubble_t recs[] = {
    {.x                  = wide_b.x,
     .y                  = wide_b.y,
     .w                  = wide_b.w,
     .h                  = wide_b.h,
     .shape              = wide_b.shape,
     .text_orientation   = wide_b.text_orientation,
     .reading_order      = 0U,
     .ocr_confidence_bps = 9500U,
     .font_size_px       = fit0.font_size_px,
     .line_count         = fit0.line_count,
     .first_line_break   = 0U,
     .source_text_off    = 0U,
     .source_text_len    = 0U,
     .english_text_off   = 0U,
     .english_text_len   = (uint32_t)(sizeof(eng0) - 1U)},
    {.x                  = narrow_b.x,
     .y                  = narrow_b.y,
     .w                  = narrow_b.w,
     .h                  = narrow_b.h,
     .shape              = narrow_b.shape,
     .text_orientation   = narrow_b.text_orientation,
     .reading_order      = 1U,
     .ocr_confidence_bps = 9200U,
     .font_size_px       = fit1.font_size_px,
     .line_count         = fit1.line_count,
     .first_line_break   = fit0.line_count,
     .source_text_off    = 0U,
     .source_text_len    = 0U,
     .english_text_off   = (uint32_t)(sizeof(eng0) - 1U),
     .english_text_len   = (uint32_t)(sizeof(eng1) - 1U)},
  };
  uint8_t trn1[k_ovl_trn1_cap] = {};
  size_t  trn1_len             = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_sidecar_write(&key,
                                             pages,
                                             1U,
                                             recs,
                                             2U,
                                             all_breaks,
                                             (uint32_t)fit0.line_count + fit1.line_count,
                                             pool,
                                             (uint32_t)((sizeof(eng0) - 1U) + (sizeof(eng1) - 1U)),
                                             trn1,
                                             sizeof(trn1),
                                             &trn1_len));

  ra8_ui_rect_t dmgs[k_ovl_dmg_cap] = {};
  size_t        dmg_count           = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_translate_overlay_render_page(&paint, trn1, trn1_len, 0U, &style, dmgs, 1U, &dmg_count));
  TEST_ASSERT_EQ(k_ovl_art_bg,
                 cv.pixels[(size_t)(k_ovl_b0_y + 1U) * k_ovl_page_w + k_ovl_b0_x + 1U]);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_overlay_render_page(&paint,
                                                   trn1,
                                                   trn1_len,
                                                   0U,
                                                   &style,
                                                   dmgs,
                                                   k_ovl_dmg_cap,
                                                   &dmg_count));
  TEST_ASSERT_EQ(2U, dmg_count);
  TEST_ASSERT_EQ((int32_t)k_ovl_b0_x, dmgs[0].x);
  TEST_ASSERT_EQ((int32_t)k_ovl_b1_x, dmgs[1].x);
  TEST_ASSERT_EQ(k_ovl_art_bg, cv.pixels[(size_t)k_ovl_b1_y * k_ovl_page_w + k_ovl_b1_x]);
}

int main(void)
{
  TEST_BEGIN("test_null_guards_and_no_write");
  test_null_guards_and_no_write();
  TEST_END("test_null_guards_and_no_write");

  TEST_BEGIN("test_round_trip_and_invalidation");
  test_round_trip_and_invalidation();
  TEST_END("test_round_trip_and_invalidation");

  TEST_BEGIN("test_language_selection");
  test_language_selection();
  TEST_END("test_language_selection");

  TEST_BEGIN("test_glossary_round_trip");
  test_glossary_round_trip();
  TEST_END("test_glossary_round_trip");

  TEST_BEGIN("test_ocr_preprocess");
  test_ocr_preprocess();
  TEST_END("test_ocr_preprocess");

  TEST_BEGIN("test_tokenizer_unigram_bpe_and_errors");
  test_tokenizer_unigram_bpe_and_errors();
  TEST_END("test_tokenizer_unigram_bpe_and_errors");

  TEST_BEGIN("test_overlay_fit_render_and_damage");
  test_overlay_fit_render_and_damage();
  TEST_END("test_overlay_fit_render_and_damage");
  return 0;
}
