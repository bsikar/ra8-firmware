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
  return 0;
}
