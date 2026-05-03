/**
 * @file test_ra_epub_chapter.c
 * @brief MC/DC unit tests for libs/ra_epub/src/ra_epub_chapter.c
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra_epub.h"
#include "ra_err.h"
#include "unity_minimal.h"

typedef enum : size_t {
  k_test_epub_font_min_bytes = 16U,
} test_epub_font_t;

static uint8_t s_font_buf[64];

/**
 * @test test_mcdc_epub_get_chapter_count_null_pair
 *
 * @par MC/DC:
 * Decision: ``if (book == NULL || out_count == NULL)``
 * (2 conditions, libs/ra_epub/src/ra_epub_chapter.c around line 262)
 * Per DO-178C 6.4.4.3 N+1 = 3 vectors.
 */
static void test_mcdc_epub_get_chapter_count_null_pair(void)
{
  TEST_BEGIN("epub_get_chapter_count MC/DC: (book||out_count) NULL");
  ra_epub_book_t book = {};
  uint16_t       cnt  = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized,
                 (int32_t)ra_epub_get_chapter_count(&book, &cnt));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_epub_get_chapter_count(NULL, &cnt));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_epub_get_chapter_count(&book, NULL));
  TEST_END("epub_get_chapter_count MC/DC: (book||out_count) NULL");
}

/**
 * @test test_mcdc_epub_get_metadata_null_pair
 *
 * @par MC/DC:
 * Decision: ``if (book == NULL || out_meta == NULL)``
 * (2 conditions, libs/ra_epub/src/ra_epub_chapter.c around line 335)
 * Per DO-178C 6.4.4.3 N+1 = 3 vectors.
 */
static void test_mcdc_epub_get_metadata_null_pair(void)
{
  TEST_BEGIN("epub_get_metadata MC/DC: (book||out_meta) NULL");
  ra_epub_book_t     book = {};
  ra_epub_metadata_t meta = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_epub_get_metadata(&book, &meta));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_epub_get_metadata(NULL, &meta));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_epub_get_metadata(&book, NULL));
  TEST_END("epub_get_metadata MC/DC: (book||out_meta) NULL");
}

/**
 * @test test_mcdc_epub_set_font_null_pair
 *
 * @par MC/DC:
 * Decision: ``if (book == NULL || font_data == NULL)``
 * (2 conditions, libs/ra_epub/src/ra_epub_chapter.c around line 403)
 * Per DO-178C 6.4.4.3 N+1 = 3 vectors.
 */
static void test_mcdc_epub_set_font_null_pair(void)
{
  TEST_BEGIN("epub_set_font MC/DC: (book||font_data) NULL");
  ra_epub_book_t book = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized,
                 (int32_t)ra_epub_set_font(&book, s_font_buf, (size_t)k_test_epub_font_min_bytes));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_epub_set_font(NULL, s_font_buf, (size_t)k_test_epub_font_min_bytes));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_epub_set_font(&book, NULL, (size_t)k_test_epub_font_min_bytes));
  TEST_END("epub_set_font MC/DC: (book||font_data) NULL");
}

int32_t main(void)
{
  test_mcdc_epub_get_chapter_count_null_pair();
  test_mcdc_epub_get_metadata_null_pair();
  test_mcdc_epub_set_font_null_pair();
  (void)fprintf(stderr, "[OK ] test_ra_epub_chapter.c\n");
  return 0;
}
