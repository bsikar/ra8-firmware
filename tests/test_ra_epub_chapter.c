/**
 * @file test_ra_epub_chapter.c
 * @brief MC/DC unit tests for libs/ra_epub/src/ra_epub_chapter.c
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_epub.h"
#include "ra_epub_internal.h"
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

/**
 * @test test_mcdc_epub_internal_join_path_guard
 *
 * @par MC/DC:
 * Decision at libs/ra_epub/src/ra_epub_chapter.c
 *   ``if (dst == NULL || cap == 0U)``  (2 conditions, OR)
 *
 * - V1: dst!=NULL, cap=8   -> C1=F, C2=F -> overall F (function proceeds).
 * - V2: dst=NULL,  cap=8   -> C1=T (short-circuit) -> overall T (early return).
 *   Pair (V1,V2) isolates C1 with C2=F held.
 * - V3: dst!=NULL, cap=0   -> C1=F, C2=T -> overall T (early return).
 *   Pair (V1,V3) isolates C2 with C1=F held.
 *
 * N=2 -> N+1=3 vectors. Minimal MC/DC.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * Test executes via ra_epub_internal_join_path() so the production
 * source decision counts under -fcoverage-mcdc.
 */
static void test_mcdc_epub_internal_join_path_guard(void)
{
  TEST_BEGIN("epub MC/DC: join_path (dst||cap=0) guard");
  char buf[16];
  /* V1: both conditions false -> proceeds, writes "abc/x". */
  buf[0] = '!';
  ra_epub_internal_join_path("abc", "x", buf, sizeof(buf));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)strcmp(buf, "abcx"));
  /* V2: dst NULL -> early return (no crash). */
  ra_epub_internal_join_path("abc", "x", NULL, sizeof(buf));
  /* V3: cap == 0 -> early return (buf untouched). */
  buf[0] = '!';
  ra_epub_internal_join_path("abc", "x", buf, 0U);
  TEST_ASSERT_EQ((int32_t)'!', (int32_t)buf[0]);
  TEST_END("epub MC/DC: join_path (dst||cap=0) guard");
}

/**
 * @test test_mcdc_epub_internal_join_path_dir_loop
 *
 * @par MC/DC:
 * Decision at libs/ra_epub/src/ra_epub_chapter.c
 *   ``while (off + 1U < cap && dir[off] != '\0')``  (2 conditions, AND)
 *
 * - V1: cap=16, dir="ab"      -> at off=0: C1=T,C2=T (loop). off=1: C1=T,C2=T.
 *                                off=2: C1=T,C2=F (exit via C2 nul).
 * - V2: cap=2,  dir="ab"      -> at off=0: C1=T,C2=T. off=1: C1=F (exit via C1
 *                                cap-1 reached). Pair (V1@off=2, V2@off=1)
 *                                isolates C1 with C2=T held.
 *
 * N=2 -> 2 vectors give MC/DC for the 2 independence pairs (the C2-only
 * flip is V1@off=0->V1@off=2, the C1-only flip is V2@off=1 vs V1@off=1
 * with C2=T held).
 */
static void test_mcdc_epub_internal_join_path_dir_loop(void)
{
  TEST_BEGIN("epub MC/DC: join_path dir-copy loop AND");
  char buf[16];
  /* V1: ample cap -> loop exits via C2 (NUL). */
  ra_epub_internal_join_path("ab", NULL, buf, sizeof(buf));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)strcmp(buf, "ab"));
  /* V2: cap=2 -> only 1 byte fits (off+1<cap stops at off=1). */
  ra_epub_internal_join_path("XYZ", NULL, buf, 2U);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)strlen(buf));
  TEST_ASSERT_EQ((int32_t)'X', (int32_t)buf[0]);
  TEST_END("epub MC/DC: join_path dir-copy loop AND");
}

/**
 * @test test_mcdc_epub_internal_join_path_name_loop
 *
 * @par MC/DC:
 * Decision at libs/ra_epub/src/ra_epub_chapter.c
 *   ``while (off + 1U < cap && name[i] != '\0')``  (2 conditions, AND)
 *
 * - V1: dir=NULL, name="cd",   cap=16 -> exits via C2 (NUL).
 * - V2: dir=NULL, name="cdef", cap=3  -> exits via C1 (cap-1 reached).
 *   Pair isolates C1 with C2=T held; (V1@off=2 vs V1@off=0) isolates C2.
 *
 * N=2 -> 2 vectors give MC/DC.
 */
static void test_mcdc_epub_internal_join_path_name_loop(void)
{
  TEST_BEGIN("epub MC/DC: join_path name-copy loop AND");
  char buf[16];
  /* V1: ample cap, name only -> exits via NUL. */
  ra_epub_internal_join_path(NULL, "cd", buf, sizeof(buf));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)strcmp(buf, "cd"));
  /* V2: cap=3 -> only 2 bytes fit. */
  ra_epub_internal_join_path(NULL, "cdef", buf, 3U);
  TEST_ASSERT_EQ((int32_t)2, (int32_t)strlen(buf));
  TEST_ASSERT_EQ((int32_t)'c', (int32_t)buf[0]);
  TEST_ASSERT_EQ((int32_t)'d', (int32_t)buf[1]);
  TEST_END("epub MC/DC: join_path name-copy loop AND");
}

/**
 * @test test_mcdc_epub_internal_glyph_dim_invalid
 *
 * @par MC/DC:
 * Decision at libs/ra_epub/src/ra_epub_chapter.c (call site) -> helper at
 * libs/ra_epub/src/ra_epub_chapter.c:
 *   ``w < 0 || h < 0`` (2 conditions, OR).
 * - V1: w=10, h=10 -> false
 * - V2: w=-1, h=10 -> true (varies left)
 * - V3: w=10, h=-1 -> true (varies right)
 * N+1 = 3.
 */
static void test_mcdc_epub_internal_glyph_dim_invalid(void)
{
  TEST_BEGIN("epub MC/DC: glyph_dim_invalid OR");
  TEST_ASSERT(!ra_epub_internal_glyph_dim_invalid(10, 10));
  TEST_ASSERT(ra_epub_internal_glyph_dim_invalid(-1, 10));
  TEST_ASSERT(ra_epub_internal_glyph_dim_invalid(10, -1));
  TEST_END("epub MC/DC: glyph_dim_invalid OR");
}

/**
 * @test test_mcdc_epub_internal_book_not_ready
 *
 * @par MC/DC:
 * Decision at libs/ra_epub/src/ra_epub_chapter.c lines 300, 369 (call sites)
 * -> helper at libs/ra_epub/src/ra_epub_chapter.c:
 *   ``in_use == 0 || zip_archive_active == 0`` (2 conditions, OR).
 * - V1: in_use=1, zip=1 -> false
 * - V2: in_use=0, zip=1 -> true (varies left)
 * - V3: in_use=1, zip=0 -> true (varies right)
 * N+1 = 3.
 */
static void test_mcdc_epub_internal_book_not_ready(void)
{
  TEST_BEGIN("epub MC/DC: book_not_ready OR");
  TEST_ASSERT(!ra_epub_internal_book_not_ready(1U, 1U));
  TEST_ASSERT(ra_epub_internal_book_not_ready(0U, 1U));
  TEST_ASSERT(ra_epub_internal_book_not_ready(1U, 0U));
  TEST_END("epub MC/DC: book_not_ready OR");
}

int32_t main(void)
{
  test_mcdc_epub_get_chapter_count_null_pair();
  test_mcdc_epub_get_metadata_null_pair();
  test_mcdc_epub_set_font_null_pair();
  test_mcdc_epub_internal_join_path_guard();
  test_mcdc_epub_internal_join_path_dir_loop();
  test_mcdc_epub_internal_join_path_name_loop();
  test_mcdc_epub_internal_glyph_dim_invalid();
  test_mcdc_epub_internal_book_not_ready();
  (void)fprintf(stderr, "[OK ] test_ra_epub_chapter.c\n");
  return 0;
}
