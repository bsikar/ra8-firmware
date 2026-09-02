/**
 * @file test_epub_chapter.c
 * @brief MC/DC unit tests for apps/shared_libs/epub/src/epub_chapter.c
 * @details Exercises accessor guards, successful metadata and TOC reads,
 *          resource lookup, chapter loading, and glyph-render dispatch.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "epub.h"
#include "epub_internal.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum epub_chapter_fixture_t
 * @brief Buffer capacities and payload sizes.
 */
typedef enum : uint8_t {
  k_epub_scratch_bytes = 64, /**< Capacity of the font and glyph scratch buffers. */
} epub_chapter_fixture_t;

typedef enum : size_t {
  k_test_epub_font_min_bytes = 16U, /**< Test EPUB font minimum bytes. */
} test_epub_font_t;

/**
 * @enum test_epub_count_t
 * @brief Named counts / indices for the accessor success-path fixtures.
 * @details Keeps the success-path tests free of bare numeric literals.
 */
typedef enum : uint16_t {
  k_test_epub_chap_count   = 7U,  /**< Spine length used by the count fixture.    */
  k_test_epub_font_count   = 3U,  /**< Embedded-font count fixture value.         */
  k_test_epub_toc_kind_val = 1U,  /**< `k_epub_toc_ncx`, as a plain value.        */
  k_test_epub_toc_entries  = 3U,  /**< TOC entry count used by the TOC fixtures.  */
  k_test_epub_chap_match1  = 1U,  /**< Expected chapter idx for the fragment hit. */
  k_test_epub_chap_match2  = 2U,  /**< Expected chapter idx for the exact hit.    */
  k_test_epub_depth_val    = 4U,  /**< Arbitrary TOC depth round-tripped back.    */
  k_test_epub_codepoint    = 65U, /**< 'A': code point handed to render_glyph.    */
} test_epub_count_t;

/** @brief Shared storage used across the chapter accessor and render fixtures. */
static struct {
  uint8_t font[k_epub_scratch_bytes];             /**< Valid font fixture storage. */
  uint8_t glyph[k_epub_scratch_bytes];            /**< Rendered glyph destination. */
  uint8_t bogus_font[k_test_epub_font_min_bytes]; /**< Rejected font fixture.      */
} s_fixture;

/** @brief Valid pixel height handed to render_glyph (font-init reject path). */
static const float s_test_glyph_px = 16.0F;

/** @brief Zero pixel height: drives the `font_size <= 0` invalid-arg branch. */
static const float s_test_glyph_px_zero = 0.0F;

/**
 * @test internal_test_mcdc_epub_open_private_guards
 * @brief Drive the resident-reader and allocator-binding guards directly.
 *
 * @par MC/DC:
 * `apps/shared_libs/epub/src/epub_open.c@priv_epub_mem_read` has a
 * four-condition OR. The first four calls make the media pointer, output
 * pointer, offset bound, and remaining-length bound independently true; the
 * fifth call makes every condition false and proves the copied suffix.
 * `apps/shared_libs/epub/src/epub_open.c@priv_epub_set_miniz_alloc` has a
 * two-condition OR. Its first two calls isolate each null operand and the
 * third makes both false, initializing the embedded arena and callbacks.
 * @details Uses the module's documented private MC/DC seam; no public EPUB ABI
 * is added or changed.
 * @pre The local book and ZIP descriptors are zero-initialized.
 * @pre The resident payload remains readable for the complete test.
 * @post Every guard has an independently determining vector.
 * @post The successful allocator binding is deinitialized before return.
 * @note Test-only helper with caller-owned fixtures.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_epub_open_private_guards(void)
{
  TEST_BEGIN("epub open private guard MC/DC");
  static const uint8_t bytes[]   = {0x11U, 0x22U, 0x33U, 0x44U};
  epub_mem_media_t     media     = {.data = bytes, .size = sizeof(bytes)};
  uint8_t              output[2] = {};
  TEST_ASSERT_EQ(0U, priv_epub_mem_read(nullptr, 0U, output, 1U));
  TEST_ASSERT_EQ(0U, priv_epub_mem_read((void*)&media, 0U, nullptr, 1U));
  TEST_ASSERT_EQ(0U, priv_epub_mem_read((void*)&media, sizeof(bytes) + 1U, output, 1U));
  TEST_ASSERT_EQ(0U, priv_epub_mem_read((void*)&media, sizeof(bytes) - 1U, output, 2U));
  TEST_ASSERT_EQ(sizeof(output), priv_epub_mem_read((void*)&media, 1U, output, sizeof(output)));
  TEST_ASSERT_EQ(bytes[1], output[0]);
  TEST_ASSERT_EQ(bytes[2], output[1]);

  mz_zip_archive zip  = {};
  epub_book_t    book = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, priv_epub_set_miniz_alloc(nullptr, &book));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, priv_epub_set_miniz_alloc(&zip, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, priv_epub_set_miniz_alloc(&zip, &book));
  TEST_ASSERT(zip.m_pAlloc == epub_miniz_alloc);
  TEST_ASSERT(zip.m_pFree == epub_miniz_free);
  TEST_ASSERT(zip.m_pRealloc == epub_miniz_realloc);
  TEST_ASSERT(zip.m_pAlloc_opaque == &book.miniz_arena);
  epub_miniz_arena_deinit(&book.miniz_arena);
  TEST_END("epub open private guard MC/DC");
}

/**
 * @test internal_test_mcdc_epub_get_chapter_count_null_pair
 *
 * @par MC/DC:
 * Decision: ``if (book == NULL || out_count == NULL)``
 * (2 conditions, apps/shared_libs/epub/src/epub_chapter.c around line 262)
 * Per DO-178C 6.4.4.3 N+1 = 3 vectors. @brief Verify mcdc epub get chapter count null pair behavior. @details Executes the mcdc epub get chapter count null pair scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_epub_get_chapter_count_null_pair(void)
{
  TEST_BEGIN("epub_get_chapter_count MC/DC: (book||out_count) NULL");
  epub_book_t book = {};
  uint16_t    cnt  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, epub_get_chapter_count(&book, &cnt));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_get_chapter_count(nullptr, &cnt));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_get_chapter_count(&book, nullptr));
  TEST_END("epub_get_chapter_count MC/DC: (book||out_count) NULL");
}

/**
 * @test internal_test_mcdc_epub_get_metadata_null_pair
 *
 * @par MC/DC:
 * Decision: ``if (book == NULL || out_meta == NULL)``
 * (2 conditions, apps/shared_libs/epub/src/epub_chapter.c around line 335)
 * Per DO-178C 6.4.4.3 N+1 = 3 vectors. @brief Verify mcdc epub get metadata null pair behavior. @details Executes the mcdc epub get metadata null pair scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_epub_get_metadata_null_pair(void)
{
  TEST_BEGIN("epub_get_metadata MC/DC: (book||out_meta) NULL");
  epub_book_t     book = {};
  epub_metadata_t meta = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, epub_get_metadata(&book, &meta));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_get_metadata(nullptr, &meta));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_get_metadata(&book, nullptr));
  TEST_END("epub_get_metadata MC/DC: (book||out_meta) NULL");
}

/**
 * @test internal_test_mcdc_epub_set_font_null_pair
 *
 * @par MC/DC:
 * Decision: ``if (book == NULL || font_data == NULL)``
 * (2 conditions, apps/shared_libs/epub/src/epub_chapter.c around line 403)
 * Per DO-178C 6.4.4.3 N+1 = 3 vectors. @brief Verify mcdc epub set font null pair behavior. @details Executes the mcdc epub set font null pair scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_epub_set_font_null_pair(void)
{
  TEST_BEGIN("epub_set_font MC/DC: (book||font_data) NULL");
  epub_book_t book = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 epub_set_font(&book, s_fixture.font, (size_t)k_test_epub_font_min_bytes));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 epub_set_font(nullptr, s_fixture.font, (size_t)k_test_epub_font_min_bytes));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 epub_set_font(&book, nullptr, (size_t)k_test_epub_font_min_bytes));
  TEST_END("epub_set_font MC/DC: (book||font_data) NULL");
}

/**
 * @test internal_test_mcdc_epub_get_resource_null_quad
 *
 * @par MC/DC:
 * Decision: ``if (book == NULL || path == NULL || out_buf == NULL || got_len == NULL)``
 * (4 conditions, OR; apps/shared_libs/epub/src/epub_chapter.c epub_get_resource).
 * Per DO-178C 6.4.4.3 a short-circuiting N-way OR needs N+1 = 5 vectors: one
 * control with every operand false plus one flipping each operand true.
 *
 * - V1: all non-NULL -> C1=F,C2=F,C3=F,C4=F -> overall F (guard not taken; the
 *   stack book has in_use==0 so the next check returns not_initialized).
 * - V2: book=NULL  -> C1=T (short-circuit) -> overall T. Pair (V1,V2) isolates C1.
 * - V3: path=NULL  -> C1=F,C2=T            -> overall T. Pair (V1,V3) isolates C2.
 * - V4: out_buf=NULL -> C1=F,C2=F,C3=T     -> overall T. Pair (V1,V4) isolates C3.
 * - V5: got_len=NULL -> C1=F,C2=F,C3=F,C4=T-> overall T. Pair (V1,V5) isolates C4.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * Each vector executes the production decision in epub_get_resource() so the
 * source branch counts under -fcoverage-mcdc. @brief Verify mcdc epub get resource null quad behavior. @details Executes the mcdc epub get resource null quad scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_epub_get_resource_null_quad(void)
{
  TEST_BEGIN("epub_get_resource MC/DC: (book||path||out_buf||got_len) NULL");
  epub_book_t book = {};
  size_t      got  = 0U;
  /* V1: every operand non-NULL -> guard false; book not initialized -> proceeds
   *     to the book-not-ready check (book is zeroed -> in_use==0). */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 epub_get_resource(&book, "OEBPS/x", s_fixture.font, sizeof(s_fixture.font), &got));
  /* V2: book NULL. */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    epub_get_resource(nullptr, "OEBPS/x", s_fixture.font, sizeof(s_fixture.font), &got));
  /* V3: path NULL. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 epub_get_resource(&book, nullptr, s_fixture.font, sizeof(s_fixture.font), &got));
  /* V4: out_buf NULL. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 epub_get_resource(&book, "OEBPS/x", nullptr, sizeof(s_fixture.font), &got));
  /* V5: got_len NULL. */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    epub_get_resource(&book, "OEBPS/x", s_fixture.font, sizeof(s_fixture.font), nullptr));
  TEST_END("epub_get_resource MC/DC: (book||path||out_buf||got_len) NULL");
}

/**
 * @test internal_test_mcdc_epub_get_embedded_font_count_null_pair
 *
 * @par MC/DC:
 * Decision: ``if (book == NULL || out_count == NULL)``
 * (2 conditions, OR; apps/shared_libs/epub/src/epub_chapter.c
 * epub_get_embedded_font_count). Per DO-178C 6.4.4.3 N+1 = 3 vectors.
 *
 * - V1: book!=NULL, out_count!=NULL -> C1=F,C2=F -> overall F (guard not taken;
 *   the zeroed book has in_use==0 -> next check returns not_initialized).
 * - V2: book=NULL                   -> C1=T (short-circuit) -> overall T.
 * - V3: book!=NULL, out_count=NULL  -> C1=F,C2=T -> overall T.
 * Pair (V1,V2) isolates C1; pair (V1,V3) isolates C2. @brief Verify mcdc epub get embedded font count null pair behavior. @details Executes the mcdc epub get embedded font count null pair scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_epub_get_embedded_font_count_null_pair(void)
{
  TEST_BEGIN("epub_get_embedded_font_count MC/DC: (book||out_count) NULL");
  epub_book_t book = {};
  uint16_t    cnt  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, epub_get_embedded_font_count(&book, &cnt));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_get_embedded_font_count(nullptr, &cnt));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_get_embedded_font_count(&book, nullptr));
  TEST_END("epub_get_embedded_font_count MC/DC: (book||out_count) NULL");
}

/**
 * @test internal_test_mcdc_epub_get_embedded_font_null_triple
 *
 * @par MC/DC:
 * Decision: ``if (book == NULL || out_buf == NULL || got_len == NULL)``
 * (3 conditions, OR; apps/shared_libs/epub/src/epub_chapter.c
 * epub_get_embedded_font). Per DO-178C 6.4.4.3 N+1 = 4 vectors.
 *
 * - V1: all non-NULL          -> C1=F,C2=F,C3=F -> overall F (guard not taken;
 *   the zeroed book is not ready -> next check returns not_initialized).
 * - V2: book=NULL             -> C1=T -> overall T. Pair (V1,V2) isolates C1.
 * - V3: out_buf=NULL          -> C1=F,C2=T -> overall T. Pair (V1,V3) isolates C2.
 * - V4: got_len=NULL          -> C1=F,C2=F,C3=T -> overall T. Pair (V1,V4) isolates C3. @brief Verify mcdc epub get embedded font null triple behavior. @details Executes the mcdc epub get embedded font null triple scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_epub_get_embedded_font_null_triple(void)
{
  TEST_BEGIN("epub_get_embedded_font MC/DC: (book||out_buf||got_len) NULL");
  epub_book_t book = {};
  size_t      got  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 epub_get_embedded_font(&book, 0U, s_fixture.font, sizeof(s_fixture.font), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 epub_get_embedded_font(nullptr, 0U, s_fixture.font, sizeof(s_fixture.font), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 epub_get_embedded_font(&book, 0U, nullptr, sizeof(s_fixture.font), &got));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    epub_get_embedded_font(&book, 0U, s_fixture.font, sizeof(s_fixture.font), nullptr));
  TEST_END("epub_get_embedded_font MC/DC: (book||out_buf||got_len) NULL");
}

/**
 * @test internal_test_mcdc_epub_internal_join_path_guard
 *
 * @par MC/DC:
 * Decision at apps/shared_libs/epub/src/epub_chapter.c
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
 * Test executes via priv_epub_join_path() so the production
 * source decision counts under -fcoverage-mcdc. @brief Verify mcdc epub internal join path guard behavior. @details Executes the mcdc epub internal join path guard scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_epub_internal_join_path_guard(void)
{
  TEST_BEGIN("epub MC/DC: join_path (dst||cap=0) guard");
  char buf[16];
  /* V1: both conditions false -> proceeds, writes "abc/x". */
  buf[0] = '!';
  priv_epub_join_path("abc", "x", buf, sizeof(buf));
  TEST_ASSERT_EQ(0, strcmp(buf, "abcx"));
  /* V2: dst NULL -> early return (no crash). */
  priv_epub_join_path("abc", "x", nullptr, sizeof(buf));
  /* V3: cap == 0 -> early return (buf untouched). */
  buf[0] = '!';
  priv_epub_join_path("abc", "x", buf, 0U);
  TEST_ASSERT_EQ('!', buf[0]);
  TEST_END("epub MC/DC: join_path (dst||cap=0) guard");
}

/**
 * @test internal_test_mcdc_epub_internal_join_path_dir_loop
 *
 * @par MC/DC:
 * Decision at apps/shared_libs/epub/src/epub_chapter.c
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
 * with C2=T held). @brief Verify mcdc epub internal join path dir loop behavior. @details Executes the mcdc epub internal join path dir loop scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_epub_internal_join_path_dir_loop(void)
{
  TEST_BEGIN("epub MC/DC: join_path dir-copy loop AND");
  char buf[16];
  /* V1: ample cap -> loop exits via C2 (NUL). */
  priv_epub_join_path("ab", nullptr, buf, sizeof(buf));
  TEST_ASSERT_EQ(0, strcmp(buf, "ab"));
  /* V2: cap=2 -> only 1 byte fits (off+1<cap stops at off=1). */
  priv_epub_join_path("XYZ", nullptr, buf, 2U);
  TEST_ASSERT_EQ(1, strlen(buf));
  TEST_ASSERT_EQ('X', buf[0]);
  TEST_END("epub MC/DC: join_path dir-copy loop AND");
}

/**
 * @test internal_test_mcdc_epub_internal_join_path_name_loop
 *
 * @par MC/DC:
 * Decision at apps/shared_libs/epub/src/epub_chapter.c
 *   ``while (off + 1U < cap && name[i] != '\0')``  (2 conditions, AND)
 *
 * - V1: dir=NULL, name="cd",   cap=16 -> exits via C2 (NUL).
 * - V2: dir=NULL, name="cdef", cap=3  -> exits via C1 (cap-1 reached).
 *   Pair isolates C1 with C2=T held; (V1@off=2 vs V1@off=0) isolates C2.
 *
 * N=2 -> 2 vectors give MC/DC. @brief Verify mcdc epub internal join path name loop behavior. @details Executes the mcdc epub internal join path name loop scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_epub_internal_join_path_name_loop(void)
{
  TEST_BEGIN("epub MC/DC: join_path name-copy loop AND");
  char buf[16];
  /* V1: ample cap, name only -> exits via NUL. */
  priv_epub_join_path(nullptr, "cd", buf, sizeof(buf));
  TEST_ASSERT_EQ(0, strcmp(buf, "cd"));
  /* V2: cap=3 -> only 2 bytes fit. */
  priv_epub_join_path(nullptr, "cdef", buf, 3U);
  TEST_ASSERT_EQ(2, strlen(buf));
  TEST_ASSERT_EQ('c', buf[0]);
  TEST_ASSERT_EQ('d', buf[1]);
  TEST_END("epub MC/DC: join_path name-copy loop AND");
}

/**
 * @test internal_test_mcdc_epub_internal_glyph_dim_invalid
 *
 * @par MC/DC:
 * Decision at apps/shared_libs/epub/src/epub_chapter.c (call site) -> helper at
 * apps/shared_libs/epub/src/epub_chapter.c:
 *   ``w < 0 || h < 0`` (2 conditions, OR).
 * - V1: w=10, h=10 -> false
 * - V2: w=-1, h=10 -> true (varies left)
 * - V3: w=10, h=-1 -> true (varies right)
 * N+1 = 3. @brief Verify mcdc epub internal glyph dim invalid behavior. @details Executes the mcdc epub internal glyph dim invalid scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_epub_internal_glyph_dim_invalid(void)
{
  TEST_BEGIN("epub MC/DC: glyph_dim_invalid OR");
  TEST_ASSERT(!priv_epub_glyph_dim_invalid(10, 10));
  TEST_ASSERT(priv_epub_glyph_dim_invalid(-1, 10));
  TEST_ASSERT(priv_epub_glyph_dim_invalid(10, -1));
  TEST_END("epub MC/DC: glyph_dim_invalid OR");
}

/**
 * @test internal_test_mcdc_epub_internal_book_not_ready
 *
 * @par MC/DC:
 * Decision at apps/shared_libs/epub/src/epub_chapter.c lines 300, 369 (call sites)
 * -> helper at apps/shared_libs/epub/src/epub_chapter.c:
 *   ``in_use == 0 || zip_archive_active == 0`` (2 conditions, OR).
 * - V1: in_use=1, zip=1 -> false
 * - V2: in_use=0, zip=1 -> true (varies left)
 * - V3: in_use=1, zip=0 -> true (varies right)
 * N+1 = 3. @brief Verify mcdc epub internal book not ready behavior. @details Executes the mcdc epub internal book not ready scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_epub_internal_book_not_ready(void)
{
  TEST_BEGIN("epub MC/DC: book_not_ready OR");
  TEST_ASSERT(!priv_epub_book_not_ready(1U, 1U));
  TEST_ASSERT(priv_epub_book_not_ready(0U, 1U));
  TEST_ASSERT(priv_epub_book_not_ready(1U, 0U));
  TEST_END("epub MC/DC: book_not_ready OR");
}

/**
 * @test internal_test_epub_get_chapter_count_success
 * @brief Exercise the success path of `epub_get_chapter_count` (in_use==1).
 *
 * @par Coverage:
 * The existing MC/DC test only drives the NULL guard and the not-initialized
 * arm; this adds the `*out_count = book->chapter_count; return k_ra8_ok;` body
 * that runs once the book is marked open.
 *
 * @par MC/DC:
 * (no compound decision is exercised for MC/DC by this case -- it drives the
 * in_use==1 success body `*out_count = book->chapter_count; return k_ra8_ok`;
 * the `book == NULL || out_count == NULL` guard is reached only as the
 * both-false control here, and its N+1 = 3 vectors live in
 * internal_test_mcdc_epub_get_chapter_count_null_pair) @details Executes the epub get chapter count success scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_epub_get_chapter_count_success(void)
{
  TEST_BEGIN("epub_get_chapter_count success path");
  epub_book_t book   = {};
  book.in_use        = 1U;
  book.chapter_count = (uint16_t)k_test_epub_chap_count;
  uint16_t cnt       = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, epub_get_chapter_count(&book, &cnt));
  TEST_ASSERT_EQ(k_test_epub_chap_count, cnt);
  TEST_END("epub_get_chapter_count success path");
}

/**
 * @test internal_test_epub_get_metadata_success
 * @brief Exercise the success path of `epub_get_metadata` (in_use==1).
 *
 * @par Coverage:
 * Runs the four `out_meta->* = book->*` assignments + `return k_ra8_ok`, which
 * the guard-only MC/DC test never reaches. Confirms the returned pointers alias
 * the book storage.
 *
 * @par MC/DC:
 * (no compound decision is exercised for MC/DC by this case -- it drives the
 * in_use==1 success body (the four `out_meta->* = book->*` copies + return);
 * the `book == NULL || out_meta == NULL` guard is reached only as the both-false
 * control here, and its N+1 = 3 vectors live in
 * internal_test_mcdc_epub_get_metadata_null_pair) @details Executes the epub get metadata success scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_epub_get_metadata_success(void)
{
  TEST_BEGIN("epub_get_metadata success path");
  epub_book_t book = {};
  book.in_use      = 1U;
  (void)strcpy(book.title, "T");
  (void)strcpy(book.author, "A");
  (void)strcpy(book.language, "en");
  (void)strcpy(book.identifier, "id");
  epub_metadata_t meta = {};
  TEST_ASSERT_EQ(k_ra8_ok, epub_get_metadata(&book, &meta));
  TEST_ASSERT_EQ(0, strcmp(meta.title, "T"));
  TEST_ASSERT_EQ(0, strcmp(meta.author, "A"));
  TEST_ASSERT_EQ(0, strcmp(meta.language, "en"));
  TEST_ASSERT_EQ(0, strcmp(meta.identifier, "id"));
  TEST_END("epub_get_metadata success path");
}

/**
 * @test internal_test_epub_get_embedded_font_count_success
 * @brief Exercise the success path of `epub_get_embedded_font_count`.
 *
 * @par Coverage:
 * Adds the `*out_count = book->embedded_font_count; return k_ra8_ok;` body to the
 * guard-only coverage that already exists.
 *
 * @par MC/DC:
 * (no compound decision is exercised for MC/DC by this case -- it drives the
 * in_use==1 success body `*out_count = book->embedded_font_count; return k_ra8_ok`;
 * the `book == NULL || out_count == NULL` guard is reached only as the both-false
 * control here, and its N+1 = 3 vectors live in
 * internal_test_mcdc_epub_get_embedded_font_count_null_pair) @details Executes the epub get embedded font count success scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_epub_get_embedded_font_count_success(void)
{
  TEST_BEGIN("epub_get_embedded_font_count success path");
  epub_book_t book         = {};
  book.in_use              = 1U;
  book.embedded_font_count = (uint16_t)k_test_epub_font_count;
  uint16_t cnt             = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, epub_get_embedded_font_count(&book, &cnt));
  TEST_ASSERT_EQ(k_test_epub_font_count, cnt);
  TEST_END("epub_get_embedded_font_count success path");
}

/**
 * @test internal_test_epub_get_toc_kind
 *
 * @par MC/DC:
 * Decision: ``if (book == NULL || out_kind == NULL)`` (2 conditions, OR;
 * epub_get_toc_kind). Per DO-178C 6.4.4.3 N+1 = 3 vectors:
 * - V1: book!=NULL, out_kind!=NULL -> C1=F,C2=F -> overall F (guard not taken;
 *   the zeroed book has in_use==0 so the next check returns not_initialized).
 * - V2: book=NULL                  -> C1=T (short-circuit) -> overall T.
 * - V3: book!=NULL, out_kind=NULL  -> C1=F,C2=T -> overall T.
 * Pair (V1,V2) isolates C1; pair (V1,V3) isolates C2. The in_use==1 call then
 * runs the `*out_kind = book->toc_kind; return k_ra8_ok;` success body. @brief Verify epub get toc kind behavior. @details Executes the epub get toc kind scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_epub_get_toc_kind(void)
{
  TEST_BEGIN("epub_get_toc_kind MC/DC + success");
  epub_book_t book = {};
  uint8_t     kind = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, epub_get_toc_kind(&book, &kind)); /* V1 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_get_toc_kind(nullptr, &kind));      /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_get_toc_kind(&book, nullptr));      /* V3 */
  book.in_use   = 1U;
  book.toc_kind = (uint8_t)k_test_epub_toc_kind_val;
  TEST_ASSERT_EQ(k_ra8_ok, epub_get_toc_kind(&book, &kind));
  TEST_ASSERT_EQ(k_test_epub_toc_kind_val, kind);
  TEST_END("epub_get_toc_kind MC/DC + success");
}

/**
 * @test internal_test_epub_get_toc_count
 *
 * @par MC/DC:
 * Decision: ``if (book == NULL || out_count == NULL)`` (2 conditions, OR;
 * epub_get_toc_count). Per DO-178C 6.4.4.3 N+1 = 3 vectors:
 * - V1: both non-NULL -> overall F (in_use==0 -> not_initialized).
 * - V2: book=NULL     -> C1=T -> overall T.
 * - V3: out_count=NULL-> C1=F,C2=T -> overall T.
 * Then in_use==1 drives the `*out_count = book->toc_count` success body. @brief Verify epub get toc count behavior. @details Executes the epub get toc count scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_epub_get_toc_count(void)
{
  TEST_BEGIN("epub_get_toc_count MC/DC + success");
  epub_book_t book = {};
  uint16_t    cnt  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, epub_get_toc_count(&book, &cnt)); /* V1 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_get_toc_count(nullptr, &cnt));      /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_get_toc_count(&book, nullptr));     /* V3 */
  book.in_use    = 1U;
  book.toc_count = (uint16_t)k_test_epub_toc_entries;
  TEST_ASSERT_EQ(k_ra8_ok, epub_get_toc_count(&book, &cnt));
  TEST_ASSERT_EQ(k_test_epub_toc_entries, cnt);
  TEST_END("epub_get_toc_count MC/DC + success");
}

/**
 * @test internal_test_epub_get_toc_entry
 *
 * @par MC/DC:
 * Decision: ``if (book == NULL || out_entry == NULL)`` (2 conditions, OR;
 * epub_get_toc_entry). N+1 = 3 vectors (V1 both-false, V2 book=NULL,
 * V3 out_entry=NULL). The remaining calls additionally drive the
 * single-condition `idx >= toc_count` range guard (out-of-range arm) and the
 * `*out_entry = book->toc[idx]` success copy. @brief Verify epub get toc entry behavior. @details Executes the epub get toc entry scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_epub_get_toc_entry(void)
{
  TEST_BEGIN("epub_get_toc_entry MC/DC + range + success");
  epub_book_t      book  = {};
  epub_toc_entry_t entry = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, epub_get_toc_entry(&book, 0U, &entry)); /* V1 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_get_toc_entry(nullptr, 0U, &entry));      /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_get_toc_entry(&book, 0U, nullptr));       /* V3 */
  book.in_use    = 1U;
  book.toc_count = 1U;
  (void)strcpy(book.toc[0].href, "ch1.xhtml");
  book.toc[0].depth = (uint8_t)k_test_epub_depth_val;
  /* idx >= toc_count -> out_of_range. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, epub_get_toc_entry(&book, book.toc_count, &entry));
  /* idx in range -> success copy. */
  TEST_ASSERT_EQ(k_ra8_ok, epub_get_toc_entry(&book, 0U, &entry));
  TEST_ASSERT_EQ(0, strcmp(entry.href, "ch1.xhtml"));
  TEST_ASSERT_EQ(k_test_epub_depth_val, entry.depth);
  TEST_END("epub_get_toc_entry MC/DC + range + success");
}

/**
 * @test internal_test_epub_toc_entry_to_chapter
 *
 * @par MC/DC:
 * Top guard ``if (book == NULL || out_chapter_idx == NULL)`` (2 conditions, OR):
 * V1 both-false (proceeds), V2 book=NULL, V3 out_chapter_idx=NULL -> N+1 = 3.
 *
 * @par Coverage:
 * Drives the whole match loop body that the suite never exercised:
 * - the `#fragment` strip ternary, both arms (entry 0 carries `#sec`, entries
 *   1 and 2 do not);
 * - the `strlen(path) != base_len` continue (entry 0 skips "a.xhtml");
 * - the `strncmp(...) != 0` continue (entry 2's base "z.xhtml" skips the
 *   equal-length-but-different "a.xhtml");
 * - the match `return k_ra8_ok` (entries 0 and 2);
 * - the loop fall-through `return k_ra8_err_not_found` (entry 1 matches nothing);
 * - the `toc_idx >= toc_count` range guard. @brief Verify epub toc entry to chapter behavior. @details Executes the epub toc entry to chapter scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_epub_toc_entry_to_chapter(void)
{
  TEST_BEGIN("epub_toc_entry_to_chapter MC/DC + match loop");
  epub_book_t book = {};
  uint16_t    ci   = 0U;
  /* MC/DC null OR before the book is marked open is fine; in_use==0 path is the
   * control outcome for V1 below once we set it open. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_toc_entry_to_chapter(nullptr, 0U, &ci));   /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, epub_toc_entry_to_chapter(&book, 0U, nullptr)); /* V3 */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 epub_toc_entry_to_chapter(&book, 0U, &ci)); /* in_use==0 */

  book.in_use        = 1U;
  book.chapter_count = (uint16_t)k_test_epub_toc_entries;
  (void)strcpy(book.chapter_paths[0], "a.xhtml");
  (void)strcpy(book.chapter_paths[1], "ch3.xhtml");
  (void)strcpy(book.chapter_paths[2], "z.xhtml");
  book.toc_count = (uint16_t)k_test_epub_toc_entries;
  (void)strcpy(book.toc[0].href, "ch3.xhtml#sec"); /* fragment -> base "ch3.xhtml" */
  (void)strcpy(book.toc[1].href, "missing.xhtml"); /* no spine match               */
  (void)strcpy(book.toc[2].href, "z.xhtml");       /* exact match, no fragment     */

  /* V1 (book!=NULL, out!=NULL) overall-false control: resolves the fragment. */
  TEST_ASSERT_EQ(k_ra8_ok, epub_toc_entry_to_chapter(&book, 0U, &ci));
  TEST_ASSERT_EQ(k_test_epub_chap_match1, ci);
  /* Exact (no-fragment) match drives the strncmp-mismatch continue at idx 0. */
  TEST_ASSERT_EQ(k_ra8_ok, epub_toc_entry_to_chapter(&book, 2U, &ci));
  TEST_ASSERT_EQ(k_test_epub_chap_match2, ci);
  /* No spine path matches -> loop falls through to not_found. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, epub_toc_entry_to_chapter(&book, 1U, &ci));
  /* toc_idx out of range. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, epub_toc_entry_to_chapter(&book, book.toc_count, &ci));
  TEST_END("epub_toc_entry_to_chapter MC/DC + match loop");
}

/**
 * @test internal_test_epub_load_chapter_guards
 *
 * @par MC/DC:
 * Decision: ``if (book == NULL || out_xhtml == NULL || got_len == NULL)``
 * (3 conditions, OR; epub_load_chapter). Per DO-178C 6.4.4.3 N+1 = 4 vectors:
 * - V1: all non-NULL (ready book, max_len==0) -> overall F -> invalid_size.
 * - V2: book=NULL    -> C1=T -> overall T.
 * - V3: out_xhtml=NULL -> C1=F,C2=T -> overall T.
 * - V4: got_len=NULL -> C1=F,C2=F,C3=T -> overall T.
 *
 * @par Coverage:
 * Also drives the `book_not_ready` not-initialized arm, the `max_len == 0`
 * invalid-size arm, and the `idx >= chapter_count` out-of-range arm -- all the
 * pre-extract guards (the miniz extract itself needs a real archive and is left
 * to the integration fixtures). @brief Verify epub load chapter guards behavior. @details Executes the epub load chapter guards scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_epub_load_chapter_guards(void)
{
  TEST_BEGIN("epub_load_chapter guards (MC/DC null triple + ranges)");
  epub_book_t book        = {};
  uint8_t     buf[8]      = {};
  size_t      got         = 0U;
  book.in_use             = 1U;
  book.zip_archive_active = 1U;
  /* V1: ready, all non-NULL, max_len==0 -> guard false -> invalid_size. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, epub_load_chapter(&book, 0U, buf, 0U, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 epub_load_chapter(nullptr, 0U, buf, sizeof(buf), &got)); /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 epub_load_chapter(&book, 0U, nullptr, sizeof(buf), &got)); /* V3 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 epub_load_chapter(&book, 0U, buf, sizeof(buf), nullptr)); /* V4 */
  /* idx >= chapter_count (count is 0) -> out_of_range. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, epub_load_chapter(&book, 0U, buf, sizeof(buf), &got));
  /* book not ready -> not_initialized. */
  book.in_use = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, epub_load_chapter(&book, 0U, buf, sizeof(buf), &got));
  TEST_END("epub_load_chapter guards (MC/DC null triple + ranges)");
}

/**
 * @test internal_test_epub_get_cover_image_guards
 *
 * @par MC/DC:
 * Decision: ``if (book == NULL || out_buf == NULL || got_len == NULL)``
 * (3 conditions, OR; epub_get_cover_image). N+1 = 4 vectors (V1 all non-NULL
 * with a ready book + max_len==0 -> invalid_size; V2 book=NULL; V3 out_buf=NULL;
 * V4 got_len=NULL).
 *
 * @par Coverage:
 * Also drives the not-ready arm and the empty-`cover_path` not-found arm (the
 * `book->cover_path[0] == '\0'` branch), which precede any archive extract. @brief Verify epub get cover image guards behavior. @details Executes the epub get cover image guards scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_epub_get_cover_image_guards(void)
{
  TEST_BEGIN("epub_get_cover_image guards (MC/DC null triple + not_found)");
  epub_book_t book        = {};
  uint8_t     buf[8]      = {};
  size_t      got         = 0U;
  book.in_use             = 1U;
  book.zip_archive_active = 1U;
  /* V1: ready, all non-NULL, max_len==0 -> invalid_size. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, epub_get_cover_image(&book, buf, 0U, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 epub_get_cover_image(nullptr, buf, sizeof(buf), &got)); /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 epub_get_cover_image(&book, nullptr, sizeof(buf), &got)); /* V3 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 epub_get_cover_image(&book, buf, sizeof(buf), nullptr)); /* V4 */
  /* cover_path empty -> not_found (before any archive lookup). */
  TEST_ASSERT_EQ(k_ra8_err_not_found, epub_get_cover_image(&book, buf, sizeof(buf), &got));
  /* book not ready -> not_initialized. */
  book.zip_archive_active = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, epub_get_cover_image(&book, buf, sizeof(buf), &got));
  TEST_END("epub_get_cover_image guards (MC/DC null triple + not_found)");
}

/**
 * @test internal_test_epub_get_resource_ready_guards
 * @brief Drive `epub_get_resource`'s ready-book pre-extract guards.
 *
 * @par Coverage:
 * The existing MC/DC test only reaches the NULL quad and the not-initialized
 * arm (in_use==0). This marks the book ready so the `max_len == 0` invalid-size
 * arm runs (the only remaining guard before the archive extract).
 *
 * @par MC/DC:
 * (no compound decision is exercised for MC/DC by this case -- it drives the
 * single-condition `max_len == 0` invalid-size arm on a ready book. The
 * `book == NULL || path == NULL || out_buf == NULL || got_len == NULL` quad is
 * reached only as the both-false control (all non-NULL), with its N+1 = 5
 * vectors in internal_test_mcdc_epub_get_resource_null_quad; the `in_use == 0 ||
 * zip_archive_active == 0` not-ready OR is held both-false, its vectors in
 * internal_test_mcdc_epub_internal_book_not_ready) @details Executes the epub get resource ready guards scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_epub_get_resource_ready_guards(void)
{
  TEST_BEGIN("epub_get_resource ready max_len==0 guard");
  epub_book_t book        = {};
  uint8_t     buf[8]      = {};
  size_t      got         = 0U;
  book.in_use             = 1U;
  book.zip_archive_active = 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, epub_get_resource(&book, "style.css", buf, 0U, &got));
  TEST_END("epub_get_resource ready max_len==0 guard");
}

/**
 * @test internal_test_epub_get_embedded_font_guards
 * @brief Drive `epub_get_embedded_font`'s ready-book pre-extract guards.
 *
 * @par Coverage:
 * Adds the `max_len == 0` invalid-size arm and the `idx >= embedded_font_count`
 * out-of-range arm (the existing test stops at the NULL triple + not-init).
 *
 * @par MC/DC:
 * (no compound decision is exercised for MC/DC by this case -- it drives the
 * single-condition `max_len == 0` invalid-size arm and the single-condition
 * `idx >= embedded_font_count` out-of-range arm on a ready book. The
 * `book == NULL || out_buf == NULL || got_len == NULL` triple is reached only as
 * the both-false control (all non-NULL); its N+1 = 4 vectors live in
 * internal_test_mcdc_epub_get_embedded_font_null_triple, and the not-ready OR's vectors
 * in internal_test_mcdc_epub_internal_book_not_ready) @details Executes the epub get embedded font guards scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_epub_get_embedded_font_guards(void)
{
  TEST_BEGIN("epub_get_embedded_font ready guards (size + range)");
  epub_book_t book         = {};
  uint8_t     buf[8]       = {};
  size_t      got          = 0U;
  book.in_use              = 1U;
  book.zip_archive_active  = 1U;
  book.embedded_font_count = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, epub_get_embedded_font(&book, 0U, buf, 0U, &got));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, epub_get_embedded_font(&book, 0U, buf, sizeof(buf), &got));
  TEST_END("epub_get_embedded_font ready guards (size + range)");
}

/**
 * @brief Render the fixture code point with the shared glyph buffer and size.
 *
 * @param[in]  book Book handle (nullptr exercises the book guard).
 * @param[in]  px   Glyph pixel size (0 exercises the size guard).
 * @param[out] buf  Glyph output buffer (nullptr exercises the buffer guard).
 * @param[out] w    Width sink (nullptr exercises the width guard).
 * @param[out] h    Height sink (nullptr exercises the height guard).
 *
 * @return The `epub_render_glyph` result code.
 * @pre None (drives the guard contract).
 * @post No state beyond @p buf / @p w / @p h is modified.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0 @details Implements the erg render fixture operation used only by this focused test executable. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. */
RA8_INTERNAL static ra8_err_t
internal_erg_render(epub_book_t* book, float px, uint8_t* buf, uint32_t* w, uint32_t* h)
{
  return epub_render_glyph(book,
                           (int32_t)k_test_epub_codepoint,
                           px,
                           buf,
                           sizeof(s_fixture.glyph),
                           w,
                           h);
}

/**
 * @test internal_test_epub_render_glyph_guards
 *
 * @par MC/DC:
 * Decision A: ``if (book == NULL || out_bitmap == NULL || out_w == NULL ||
 * out_h == NULL)`` (4 conditions, OR; epub_render_glyph). N+1 = 5 vectors:
 * - V1: all non-NULL (book unopened) -> overall F -> not_initialized.
 * - V2: book=NULL    -> C1=T.
 * - V3: out_bitmap=NULL -> C1=F,C2=T.
 * - V4: out_w=NULL   -> C1=F,C2=F,C3=T.
 * - V5: out_h=NULL   -> C1=F,C2=F,C3=F,C4=T.
 *
 * Decision B: ``if (book->in_use == 0 || book->font_data == NULL)``
 * (2 conditions, OR). N+1 = 3 vectors:
 * - W1: in_use==1 + font set -> overall F (proceeds to the font_size check).
 * - W2: in_use==0            -> C1=T -> not_initialized.
 * - W3: in_use==1, font_data==NULL -> C1=F,C2=T -> not_initialized.
 *
 * @par Coverage:
 * Also drives the `font_size <= 0` invalid-arg arm and the `internal_font_init`
 * validation-failed arm (a non-TTF blob makes stbtt reject the font offset). @brief Verify epub render glyph guards behavior. @details Executes the epub render glyph guards scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_epub_render_glyph_guards(void)
{
  TEST_BEGIN("epub_render_glyph MC/DC null quad + not-ready + reject");
  epub_book_t book = {};
  uint32_t    w    = 1U;
  uint32_t    h    = 1U;
  /* Decision A, V1 control: unopened book, all pointers valid -> not_init. */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 internal_erg_render(&book, s_test_glyph_px, s_fixture.glyph, &w, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 internal_erg_render(nullptr, s_test_glyph_px, s_fixture.glyph, &w, &h)); /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 internal_erg_render(&book, s_test_glyph_px, nullptr, &w, &h)); /* V3 */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    internal_erg_render(&book, s_test_glyph_px, s_fixture.glyph, nullptr, &h)); /* V4 */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    internal_erg_render(&book, s_test_glyph_px, s_fixture.glyph, &w, nullptr)); /* V5 */

  /* Decision B, W3: open but no font installed -> not_initialized. */
  book.in_use = 1U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 internal_erg_render(&book, s_test_glyph_px, s_fixture.glyph, &w, &h));

  /* Install a (non-TTF) font blob: W1 holds, then font_size<=0 -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 epub_set_font(&book, s_fixture.bogus_font, (size_t)k_test_epub_font_min_bytes));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 internal_erg_render(&book, s_test_glyph_px_zero, s_fixture.glyph, &w, &h));
  /* W1 + valid size, but the blob is not a TTF -> internal_font_init rejects it. */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 internal_erg_render(&book, s_test_glyph_px, s_fixture.glyph, &w, &h));

  /* Decision B, W2: in_use==0 short-circuits regardless of font_data. */
  book.in_use = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 internal_erg_render(&book, s_test_glyph_px, s_fixture.glyph, &w, &h));
  TEST_END("epub_render_glyph MC/DC null quad + not-ready + reject");
}

int main(void)
{
  static void (*const s_test_roster[])(void) = {
    internal_test_mcdc_epub_open_private_guards,
    internal_test_mcdc_epub_get_chapter_count_null_pair,
    internal_test_mcdc_epub_get_metadata_null_pair,
    internal_test_mcdc_epub_set_font_null_pair,
    internal_test_mcdc_epub_get_resource_null_quad,
    internal_test_mcdc_epub_get_embedded_font_count_null_pair,
    internal_test_mcdc_epub_get_embedded_font_null_triple,
    internal_test_mcdc_epub_internal_join_path_guard,
    internal_test_mcdc_epub_internal_join_path_dir_loop,
    internal_test_mcdc_epub_internal_join_path_name_loop,
    internal_test_mcdc_epub_internal_glyph_dim_invalid,
    internal_test_mcdc_epub_internal_book_not_ready,
    internal_test_epub_get_chapter_count_success,
    internal_test_epub_get_metadata_success,
    internal_test_epub_get_embedded_font_count_success,
    internal_test_epub_get_toc_kind,
    internal_test_epub_get_toc_count,
    internal_test_epub_get_toc_entry,
    internal_test_epub_toc_entry_to_chapter,
    internal_test_epub_load_chapter_guards,
    internal_test_epub_get_cover_image_guards,
    internal_test_epub_get_resource_ready_guards,
    internal_test_epub_get_embedded_font_guards,
    internal_test_epub_render_glyph_guards,
  };
  const size_t test_count = sizeof(s_test_roster) / sizeof(s_test_roster[0]);
  for (size_t i = 0U; i < test_count; ++i) {
    s_test_roster[i]();
  }
  return 0;
}
