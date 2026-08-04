/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_epub_open.c
 * @brief MC/DC unit tests for libs/ra8_epub/src/ra8_epub_open.c
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_epub.h"
#include "ra8_err.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_epub_dummy_byte = 0x42U, /**< Test EPUB dummy byte. */
} test_epub_byte_t;

typedef enum : size_t {
  k_test_epub_size_zero    = 0U,  /**< Test EPUB size zero.    */
  k_test_epub_size_nonzero = 16U, /**< Test EPUB size nonzero. */
} test_epub_size_t;

static uint8_t s_blob[16];

/**
 * @test test_mcdc_epub_open_media_or_book_null
 *
 * @par MC/DC:
 * Decision: ``if (media == NULL || out_book == NULL)``
 * (2 conditions, libs/ra8_epub/src/ra8_epub_open.c around line 326)
 * Per DO-178C 6.4.4.3 N+1 = 3 vectors.
 */
static void test_mcdc_epub_open_media_or_book_null(void)
{
  TEST_BEGIN("epub_open MC/DC: (media==NULL || out_book==NULL)");
  ra8_epub_book_t            book  = {};
  const ra8_epub_mem_media_t media = {.data = s_blob, .size = (size_t)k_test_epub_size_nonzero};
  s_blob[0]                        = (uint8_t)k_test_epub_dummy_byte;
  TEST_ASSERT(ra8_epub_open(&media, nullptr, &book) != k_ra8_err_null_ptr);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_open(nullptr, nullptr, &book));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epub_open(&media, nullptr, nullptr));
  TEST_END("epub_open MC/DC: (media==NULL || out_book==NULL)");
}

/**
 * @test test_mcdc_epub_open_mem_data_or_size
 *
 * @par MC/DC:
 * Decision: ``if (mem->data == NULL || mem->size == 0U)``
 * (2 conditions, libs/ra8_epub/src/ra8_epub_open.c around line 330)
 * Per DO-178C 6.4.4.3 N+1 = 3 vectors.
 */
static void test_mcdc_epub_open_mem_data_or_size(void)
{
  TEST_BEGIN("epub_open MC/DC: (mem->data==NULL || mem->size==0)");
  ra8_epub_book_t            book = {};
  const ra8_epub_mem_media_t v1m  = {.data = s_blob, .size = (size_t)k_test_epub_size_nonzero};
  TEST_ASSERT(ra8_epub_open(&v1m, nullptr, &book) != k_ra8_err_invalid_arg);
  const ra8_epub_mem_media_t v2m = {.data = nullptr, .size = (size_t)k_test_epub_size_nonzero};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epub_open(&v2m, nullptr, &book));
  const ra8_epub_mem_media_t v3m = {.data = s_blob, .size = (size_t)k_test_epub_size_zero};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epub_open(&v3m, nullptr, &book));
  TEST_END("epub_open MC/DC: (mem->data==NULL || mem->size==0)");
}

int32_t main(void)
{
  test_mcdc_epub_open_media_or_book_null();
  test_mcdc_epub_open_mem_data_or_size();
  (void)fprintf(stderr, "[OK ] test_ra8_epub_open.c\n");
  return 0;
}
