/**
 * @file tests/mocks/src/test_app_translate_eval_e2e.c
 * @brief Host integration tests for `translate_eval_e2e` firmware runner and `TPAK` codec.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "translate_e2e.h"
#include "unity_minimal.h"

enum : uint32_t {
  k_test_trace_cap = 16384U,
  k_test_tpak_cap  = 65536U,
};

typedef struct {
  char   buf[k_test_trace_cap];
  size_t len;
  bool   overflowed;
} te2e_trace_sink_t;

static translate_e2e_arena_t s_arena;
static uint8_t               s_tpak_buf[k_test_tpak_cap];
static te2e_trace_sink_t     s_sink;

static void test_emit_sink(void* ctx, const char* bytes, size_t len)
{
  te2e_trace_sink_t* sink = (te2e_trace_sink_t*)ctx;
  if ((sink->len + len + 1U) > sizeof(sink->buf)) {
    sink->overflowed = true;
    return;
  }
  memcpy(&sink->buf[sink->len], bytes, len);
  sink->len += len;
  sink->buf[sink->len] = '\0';
}

static void test_chapter_suite_and_trjc_resume(void)
{
  TEST_BEGIN("translate_eval_e2e: chapter suite and TRJC bit-exact restore");
  memset(&s_arena, 0, sizeof(s_arena));
  translate_e2e_chapter_report_t report = {};
  TEST_ASSERT_EQ(k_ra8_ok, translate_e2e_run_chapter_suite(&s_arena, &report));
  TEST_ASSERT_EQ(1U, report.policy_verified);
  TEST_ASSERT_EQ(1U, report.checkpoint_bit_exact);
  TEST_ASSERT_EQ(3U, report.bubble_count);
  TEST_ASSERT_EQ(1U, report.furigana_dropped);
  TEST_ASSERT(report.trn1_bytes > 0U);
  TEST_ASSERT(report.trn1_crc32 != 0U);
  TEST_ASSERT(report.weight_bytes_read > 0U);
  TEST_END("translate_eval_e2e: chapter suite and TRJC bit-exact restore");
}

static void test_mini_tpak_e2e_trace(void)
{
  TEST_BEGIN("translate_eval_e2e: mini TPAK det->OCR->Marian trace emission");
  memset(&s_arena, 0, sizeof(s_arena));
  memset(&s_sink, 0, sizeof(s_sink));
  size_t tpak_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 translate_e2e_build_mini_tpak(&s_arena.pages[0],
                                               &s_arena.bubbles[0],
                                               s_tpak_buf,
                                               sizeof(s_tpak_buf),
                                               &tpak_len));
  TEST_ASSERT(tpak_len > (size_t)k_te2e_tpak_header_bytes);

  translate_e2e_tpak_stats_t stats = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 translate_e2e_run_tpak(s_tpak_buf,
                                        tpak_len,
                                        s_arena.model_work,
                                        sizeof(s_arena.model_work),
                                        test_emit_sink,
                                        &s_sink,
                                        &stats));
  TEST_ASSERT(!s_sink.overflowed);
  TEST_ASSERT_EQ(1U, stats.page_count);
  TEST_ASSERT_EQ(1U, stats.bubble_count);
  TEST_ASSERT_EQ(1U, stats.furigana_dropped);
  TEST_ASSERT(stats.weight_bytes_read > 0U);
  TEST_ASSERT(strstr(s_sink.buf, "EMU_TRACE_BEGIN") != nullptr);
  TEST_ASSERT(strstr(s_sink.buf, "EMU_TRACE_END") != nullptr);
  TEST_ASSERT(strstr(s_sink.buf, "\"page_id\":\"ja_mini_01\"") != nullptr);
  TEST_ASSERT(
    strstr(
      s_sink.buf,
      "\"source_text\":\"\xE4\xBB\x8A\xE6\x97\xA5\xE3\x81\xAF\xE9\x9B\xA8\xE3\x81\xA7\xE3\x81\x99\"") !=
    nullptr);
  TEST_ASSERT(strstr(s_sink.buf, "\"english_text\":\"Today it is raining.\"") != nullptr);
  TEST_ASSERT(strstr(s_sink.buf, "\"furigana_dropped\":1") != nullptr);
  TEST_END("translate_eval_e2e: mini TPAK det->OCR->Marian trace emission");
}

static void test_tpak_error_and_corruption_guards(void)
{
  TEST_BEGIN("translate_eval_e2e: corrupt and malformed TPAK rejection");
  memset(&s_arena, 0, sizeof(s_arena));
  size_t tpak_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 translate_e2e_build_mini_tpak(&s_arena.pages[0],
                                               &s_arena.bubbles[0],
                                               s_tpak_buf,
                                               sizeof(s_tpak_buf),
                                               &tpak_len));

  translate_e2e_tpak_stats_t stats = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, translate_e2e_run_chapter_suite(nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 translate_e2e_run_tpak(nullptr,
                                        tpak_len,
                                        s_arena.model_work,
                                        sizeof(s_arena.model_work),
                                        test_emit_sink,
                                        &s_sink,
                                        &stats));

  /* Truncated container rejects before emitting any trace output. */
  memset(&s_sink, 0, sizeof(s_sink));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 translate_e2e_run_tpak(s_tpak_buf,
                                        12U,
                                        s_arena.model_work,
                                        sizeof(s_arena.model_work),
                                        test_emit_sink,
                                        &s_sink,
                                        &stats));
  TEST_ASSERT_EQ(0U, s_sink.len);

  /* Corrupt payload byte fails IEEE CRC-32 check. */
  static uint8_t s_bad_buf[k_test_tpak_cap];
  memcpy(s_bad_buf, s_tpak_buf, tpak_len);
  s_bad_buf[tpak_len - 1U] ^= 0x5AU;
  TEST_ASSERT_EQ(k_ra8_err_crc_mismatch,
                 translate_e2e_run_tpak(s_bad_buf,
                                        tpak_len,
                                        s_arena.model_work,
                                        sizeof(s_arena.model_work),
                                        test_emit_sink,
                                        &s_sink,
                                        &stats));
  TEST_ASSERT_EQ(0U, s_sink.len);

  /* Bad magic fails cleanly. */
  memcpy(s_bad_buf, s_tpak_buf, tpak_len);
  s_bad_buf[0] ^= 0x01U;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 translate_e2e_run_tpak(s_bad_buf,
                                        tpak_len,
                                        s_arena.model_work,
                                        sizeof(s_arena.model_work),
                                        test_emit_sink,
                                        &s_sink,
                                        &stats));
  TEST_ASSERT_EQ(0U, s_sink.len);
  TEST_END("translate_eval_e2e: corrupt and malformed TPAK rejection");
}

int main(void)
{
  test_chapter_suite_and_trjc_resume();
  test_mini_tpak_e2e_trace();
  test_tpak_error_and_corruption_guards();
  return 0;
}
