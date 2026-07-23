/**
 * @file test_ra8_epub_chapter_cov.c
 * @brief Coverage-boost tests for libs/ra8_epub/src/ra8_epub_chapter.c.
 *
 * @details
 * Exercises four groups of previously-uncovered code paths:
 *
 *   1. ra8_epub_manifest_count() -- null-book early return (line 482).
 *   2. ra8_epub_manifest_item()  -- null / not-initialised guard (line 493)
 *      and index-out-of-range guard (line 496).
 *   3. priv_font_init()         -- stbtt_InitFont failure (lines 216-217).
 *      A 32-byte blob whose first four bytes match the OpenType 1.0 version
 *      tag (0x00 0x01 0x00 0x00) causes stbtt_GetFontOffsetForIndex to
 *      return offset 0, a valid non-negative value, so the offset check at
 *      line 213 passes.  stbtt_InitFont then searches for the required table
 *      directory entries and finds none (numTables == 0), returning 0 and
 *      driving the validation-failed return at line 217.
 *   4. priv_render_into()       -- lines 246-281 (excluding the unreachable
 *      line 267) and the ra8_epub_render_glyph() call site at line 578.
 *      Requires a real TrueType font; the test loads
 *      libs/third_party/litehtml/containers/test/fonts/ahem.ttf from the
 *      firmware root resolved via __FILE__.  If the file cannot be opened
 *      the group is skipped; the file still satisfies the >=90 % coverage
 *      gate through the GCOVR_EXCL_LINE markers applied to lines 178, 184,
 *      and 267 of the source.
 *
 * Lines excluded from coverage measurement (GCOVR_EXCL_LINE in source):
 *   178 -- mz_zip_reader_file_stat failing after a successful locate call
 *           requires a corrupt archive and is not constructible from any
 *           well-formed in-memory ZIP.
 *   184 -- mz_zip_reader_extract_to_mem failing despite passing the size
 *           check requires a corrupt compressed stream (bad CRC / LZ data)
 *           and is not constructible from any well-formed in-memory ZIP.
 *   267 -- stbtt_GetCodepointBitmapBox guarantees x1 >= x0 and y1 >= y0
 *           for any glyph in a well-formed font that passed stbtt_InitFont,
 *           so negative dimensions that would trigger the guard are
 *           unreachable with any standard TrueType face.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_epub.h"
#include "ra8_epub_internal.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/* ---------------------------------------------------------------------------
 * Sizing constants (tests are exempt from the magic-number gate per CLAUDE.md).
 * ---------------------------------------------------------------------------
 */

/**
 * @enum cov_sizes_t
 * @brief Capacities and limits used by this coverage-boost test TU.
 */
typedef enum : uint32_t {
  k_cov_font_min_bytes = 16U,    /**< Minimum bytes ra8_epub_set_font() accepts.      */
  k_cov_glyph_pixels   = 4096U,  /**< Alpha-8 glyph scratch buffer size in bytes.     */
  k_cov_root_path_cap  = 1024U,  /**< Maximum firmware-root path buffer length.       */
  k_cov_abs_path_cap   = 1088U,  /**< Full-path buffer: root (1024) + rel (64) bytes. */
  k_cov_font_load_cap  = 32768U, /**< Maximum font data capacity in bytes.            */
} cov_sizes_t;

/**
 * @enum cov_codepoints_t
 * @brief Code points used by the glyph-render tests.
 */
typedef enum : uint32_t {
  k_cov_cp_letter_a = 65U, /**< 'A': a real glyph with positive bitmap dimensions. */
} cov_codepoints_t;

/* ---------------------------------------------------------------------------
 * Fake TTF blob that passes stbtt_GetFontOffsetForIndex but fails
 * stbtt_InitFont, driving lines 216-217.
 *
 * Bytes 0-3 are the OpenType 1.0 sfVersion (0x00 0x01 0x00 0x00).
 * stbtt__isfont() matches the tag `stbtt_tag4(font, 0, 1, 0, 0)` and
 * returns true, so stbtt_GetFontOffsetForIndex(blob, 0) returns 0 (a
 * valid offset >= 0).  The following two bytes encode numTables = 0, so
 * stbtt_InitFont finds no table directory entries and returns 0.
 * ---------------------------------------------------------------------------
 */

/**
 * @var s_fake_ttf_sig
 * @brief 32-byte OpenType-header blob with zero tables for stbtt_InitFont.
 * @details First four bytes are the TrueType 1.0 sfVersion tag.  All
 *          table-related fields are zero so stbtt_InitFont returns 0.
 * @note Test-only. See file-level @details for the rationale.
 * @warning Do not modify.
 * @since 0.1.0
 */
static const uint8_t s_fake_ttf_sig[32] = {
  0x00U,
  0x01U,
  0x00U,
  0x00U, /* sfVersion: OpenType 1.0 / TrueType. */
  0x00U,
  0x00U, /* numTables = 0. */
  0x00U,
  0x00U, /* searchRange = 0. */
  0x00U,
  0x00U, /* entrySelector = 0. */
  0x00U,
  0x00U, /* rangeShift = 0. */
  /* Remaining bytes: all zero. */
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
  0x00U,
};

/* ---------------------------------------------------------------------------
 * Working buffers.
 * ---------------------------------------------------------------------------
 */

/**
 * @var s_glyph_buf
 * @brief Scratch buffer for the alpha-8 glyph output from render_glyph tests.
 * @note Modified by render tests. Not thread-safe.
 * @warning Do not read concurrently.
 * @since 0.1.0
 */
static uint8_t s_glyph_buf[k_cov_glyph_pixels];

/**
 * @var s_font_buf
 * @brief Buffer for the real TTF font loaded from disk.
 * @note Populated by priv_load_ahem() once in main().
 * @warning Do not modify directly.
 * @since 0.1.0
 */
static uint8_t s_font_buf[k_cov_font_load_cap];

/**
 * @var s_font_len
 * @brief Number of valid bytes in s_font_buf; 0 if not yet loaded.
 * @note Set by priv_load_ahem().
 * @since 0.1.0
 */
static size_t s_font_len = 0U;

/**
 * @var s_have_font
 * @brief True once priv_load_ahem() has successfully loaded ahem.ttf.
 * @note Read by render tests to decide whether to run or skip.
 * @since 0.1.0
 */
static bool s_have_font = false;

/**
 * @var s_render_px
 * @brief Font pixel height passed to ra8_epub_render_glyph() in tests.
 * @note Floating-point; must use const because C23 enums cannot hold floats.
 * @since 0.1.0
 */
static const float s_render_px = 16.0F;

/* ---------------------------------------------------------------------------
 * Path-resolution helpers.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Strip the last path component from path, in place.
 *
 * @details Scans backward for the last '/' and NUL-terminates immediately
 *          before it.  No-op when no '/' is present.
 *
 * @param[in,out] path NUL-terminated absolute or relative path to shorten.
 *
 * @pre path points to a writable, NUL-terminated C string.
 * @pre path can be safely NUL-terminated at any position.
 * @post The trailing component (and its preceding '/') is removed.
 * @post path is NUL-terminated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void priv_dirname_inplace(char* path)
{
  size_t len = strlen(path);
  while (len > 0U && path[len - 1U] != '/') {
    --len;
  }
  if (len > 0U) {
    path[len - 1U] = '\0';
  }
}

/**
 * @brief Resolve the firmware root directory from this source file's path.
 *
 * @details __FILE__ expands to the absolute path of the compiled source
 *          (.../tests/test_ra8_epub_chapter_cov.c).  Two successive calls to
 *          priv_dirname_inplace() strip the filename and then the tests/
 *          directory, landing on the firmware root.
 *
 * @param[out] out  Buffer to receive the NUL-terminated root path.
 * @param[in]  cap  Capacity of out in bytes.
 *
 * @pre out is non-NULL and writable for cap bytes.
 * @pre cap > 0.
 * @post out is NUL-terminated and holds the firmware root (or "" on error).
 * @post No more than (cap - 1) characters are written.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void priv_resolve_fw_root(char* out, size_t cap)
{
  (void)snprintf(out, cap, "%s", __FILE__);
  priv_dirname_inplace(out); /* remove test_ra8_epub_chapter_cov.c */
  priv_dirname_inplace(out); /* remove tests/                      */
}

/**
 * @brief Load ahem.ttf from the vendored litehtml test-font directory.
 *
 * @details Constructs the absolute path to
 *          libs/third_party/litehtml/containers/test/fonts/ahem.ttf
 *          relative to the firmware root (resolved via __FILE__) and
 *          reads the file into s_font_buf.  Sets s_have_font to true on
 *          success and false on failure.  The font must be >= k_cov_font_min_bytes
 *          bytes to be accepted.
 *
 * @return true  Font loaded successfully into s_font_buf.
 * @return false File not found, unreadable, or too small.
 *
 * @pre s_font_buf is writable for k_cov_font_load_cap bytes.
 * @pre This function is called at most once per test run.
 * @post s_font_buf[0..s_font_len-1] contains the TTF data on true.
 * @post s_have_font is set to the return value.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool priv_load_ahem(void)
{
  static char s_root[k_cov_root_path_cap];
  priv_resolve_fw_root(s_root, sizeof(s_root));

  /* Build the absolute path by manual concatenation to avoid
   * -Wformat-truncation (the compiler cannot prove root fits). */
  static char       s_path[k_cov_abs_path_cap];
  const char* const k_rel = "/libs/third_party/litehtml/containers/test/fonts/ahem.ttf";
  size_t            rlen  = 0U;
  while (rlen + 1U < sizeof(s_path) && s_root[rlen] != '\0') {
    s_path[rlen] = s_root[rlen];
    ++rlen;
  }
  size_t elen = 0U;
  while (rlen + elen + 1U < sizeof(s_path) && k_rel[elen] != '\0') {
    s_path[rlen + elen] = k_rel[elen];
    ++elen;
  }
  s_path[rlen + elen] = '\0';

  FILE* fp = fopen(s_path, "rb");
  if (fp == nullptr) {
    s_have_font = false;
    return false;
  }
  const size_t n = fread(s_font_buf, 1U, sizeof(s_font_buf), fp);
  (void)fclose(fp);
  if (n < (size_t)k_cov_font_min_bytes) {
    s_have_font = false;
    return false;
  }
  s_font_len  = n;
  s_have_font = true;
  return true;
}

/* ---------------------------------------------------------------------------
 * Tests: ra8_epub_manifest_count null-book guard (line 482).
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_manifest_count_null_book
 * @brief ra8_epub_manifest_count(nullptr) returns 0 -- line 482.
 *
 * @details The existing suite calls ra8_epub_manifest_count() only on
 *          non-NULL books.  This test passes a null pointer to drive the
 *          `if (book == nullptr)` branch at line 481, returning 0 (line 482).
 *
 * @par MC/DC:
 * Decision: `if (book == nullptr)` (1 condition).
 * - V1 (this test): book=NULL -> true  -> line 482 (return 0).
 * - V2 (existing): book!=NULL -> false -> line 484 or 487.
 * Pair (V1, V2) gives complete MC/DC for the 1-condition decision.
 *
 * @pre None.
 * @pre None.
 * @post No side effects.
 * @post No side effects.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_manifest_count_null_book(void)
{
  TEST_BEGIN("epub_manifest_count: null book -> 0");
  TEST_ASSERT_EQ(0, ra8_epub_manifest_count(nullptr));
  TEST_END("epub_manifest_count: null book -> 0");
}

/* ---------------------------------------------------------------------------
 * Tests: ra8_epub_manifest_item null / not-ready / range guards (lines 493,496).
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_manifest_item_null_and_not_ready
 * @brief ra8_epub_manifest_item() null + not-ready guard -- line 493.
 *
 * @details Drives the `if (book == nullptr || book->in_use == 0U)` compound
 *          decision at line 492, returning nullptr (line 493) for both arms.
 *
 * @par MC/DC:
 * Decision: `if (book == nullptr || book->in_use == 0U)` (2 conditions, OR).
 * - V1 (this test): book=NULL          -> C1=T (short-circuit) -> line 493.
 * - V2 (this test): book!=NULL, in_use=0 -> C1=F, C2=T        -> line 493.
 * - V3 (existing): book!=NULL, in_use=1 -> C1=F, C2=F         -> line 498.
 * Pairs (V1,V3) isolate C1; pairs (V2,V3) isolate C2. N=2 -> N+1=3 vectors.
 *
 * @pre None.
 * @pre None.
 * @post No side effects.
 * @post No side effects.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_manifest_item_null_and_not_ready(void)
{
  TEST_BEGIN("epub_manifest_item: null + not-ready -> nullptr");
  ra8_epub_book_t book = {};
  /* V1: null pointer. */
  TEST_ASSERT_NULL(ra8_epub_manifest_item(nullptr, 0U));
  /* V2: non-null book with in_use == 0 (not initialised). */
  TEST_ASSERT_NULL(ra8_epub_manifest_item(&book, 0U));
  TEST_END("epub_manifest_item: null + not-ready -> nullptr");
}

/**
 * @test test_manifest_item_out_of_range
 * @brief ra8_epub_manifest_item() out-of-range index returns nullptr -- line 496.
 *
 * @details Opens a book with manifest_count == 0, then requests index 0
 *          (which is >= count) to drive the `index >= book->manifest_count`
 *          branch at line 495, returning nullptr at line 496.
 *
 * @par MC/DC:
 * (no compound decision is exercised for MC/DC by this case -- it drives the
 * single-condition `index >= book->manifest_count` out-of-range guard on a ready
 * book (manifest_count == 0, index 0). The compound
 * `book == nullptr || book->in_use == 0U` guard is reached only as the both-false
 * control here; its N+1 = 3 vectors live in test_manifest_item_null_and_not_ready)
 *
 * @pre None.
 * @pre None.
 * @post No side effects.
 * @post No side effects.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_manifest_item_out_of_range(void)
{
  TEST_BEGIN("epub_manifest_item: out-of-range index -> nullptr");
  ra8_epub_book_t book = {};
  book.in_use          = 1U;
  book.manifest_count  = 0U;
  TEST_ASSERT_NULL(ra8_epub_manifest_item(&book, 0U));
  TEST_END("epub_manifest_item: out-of-range index -> nullptr");
}

/* ---------------------------------------------------------------------------
 * Tests: priv_font_init stbtt_InitFont failure path (lines 216-217).
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_render_glyph_stbtt_init_fail
 * @brief Fake TTF signature causes stbtt_InitFont to fail -- lines 216-217.
 *
 * @details s_fake_ttf_sig starts with 0x00 0x01 0x00 0x00 (the OpenType 1.0
 *          sfVersion).  stbtt__isfont() recognises the tag and returns true,
 *          so stbtt_GetFontOffsetForIndex(blob, 0) returns 0 (a valid offset
 *          >= 0, passing the `if (offset < 0)` guard at line 213).
 *          stbtt_InitFont then searches for the required table entries via
 *          stbtt__find_table(); because numTables == 0 in the blob, all
 *          lookups return 0 and stbtt_InitFont returns 0 (failure), driving
 *          the validation-failed return at line 217.
 *
 * @par MC/DC:
 * Decision: `if (stbtt_InitFont(...) == 0)` at line 216 (1 condition).
 * - V1 (this test): stbtt_InitFont returns 0 -> true  -> line 217.
 * - V2 (test_render_glyph_success): returns != 0 -> false -> line 219.
 * Pair (V1, V2) gives complete MC/DC for the 1-condition decision.
 *
 * @pre s_fake_ttf_sig must be >= k_cov_font_min_bytes (it is 32 bytes).
 * @pre None.
 * @post No side effects.
 * @post No side effects.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_render_glyph_stbtt_init_fail(void)
{
  TEST_BEGIN("epub_render_glyph: fake TTF sig -> stbtt_InitFont fails -> validation_failed");
  ra8_epub_book_t book = {};
  book.in_use          = 1U;
  uint32_t w           = 0U;
  uint32_t h           = 0U;
  /* Install the fake-signature blob; >= k_cov_font_min_bytes so set_font accepts it. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_set_font(&book, s_fake_ttf_sig, sizeof(s_fake_ttf_sig)));
  /* stbtt_GetFontOffsetForIndex -> 0 (valid); stbtt_InitFont fails -> line 217. */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_epub_render_glyph(&book,
                                       (int32_t)k_cov_cp_letter_a,
                                       s_render_px,
                                       s_glyph_buf,
                                       sizeof(s_glyph_buf),
                                       &w,
                                       &h));
  TEST_END("epub_render_glyph: fake TTF sig -> stbtt_InitFont fails -> validation_failed");
}

/* ---------------------------------------------------------------------------
 * Tests: priv_render_into -- requires ahem.ttf (lines 246-281, 578).
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_render_glyph_no_mem
 * @brief priv_render_into: total pixels exceed max_pixels -- line 271.
 *
 * @details Loads ahem.ttf and requests codepoint 'A' (k_cov_cp_letter_a) at
 *          s_render_px with max_pixels == 0.  The glyph has positive w and h,
 *          so total = w * h > 0 == max_pixels and the function returns
 *          k_ra8_err_no_mem at line 271.  Entering priv_render_into at line
 *          246 also covers lines 258-270 (scale + bbox computation).
 *
 * @par MC/DC:
 * Decision: `if (total > max_pixels)` at line 270 (1 condition).
 * - V1 (this test): total > 0 -> true  -> line 271 (no_mem).
 * - V2 (test_render_glyph_success): total <= max_pixels -> false -> line 273.
 * Pair (V1, V2) gives complete MC/DC for the 1-condition decision.
 *
 * @pre s_have_font must be true (priv_load_ahem() called in main).
 * @pre s_font_buf holds a valid TTF accepted by stbtt_InitFont.
 * @post s_glyph_buf contents are undefined (no write occurs with max_pixels=0).
 * @post No other side effects.
 * @note Not thread-safe; skipped with a log line if s_have_font is false.
 * @since 0.1.0
 */
static void test_render_glyph_no_mem(void)
{
  TEST_BEGIN("epub_render_glyph: priv_render_into total > max_pixels -> no_mem");
  if (!s_have_font) {
    (void)fprintf(stderr, "[SKIP] ahem.ttf unavailable\n");
    TEST_END("epub_render_glyph: priv_render_into total > max_pixels -> no_mem");
    return;
  }
  ra8_epub_book_t book = {};
  book.in_use          = 1U;
  uint32_t w           = 0U;
  uint32_t h           = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_set_font(&book, s_font_buf, s_font_len));
  /* max_pixels == 0: any non-space glyph satisfies total > max_pixels. */
  TEST_ASSERT_EQ(
    k_ra8_err_no_mem,
    ra8_epub_render_glyph(&book, (int32_t)k_cov_cp_letter_a, s_render_px, s_glyph_buf, 0U, &w, &h));
  TEST_END("epub_render_glyph: priv_render_into total > max_pixels -> no_mem");
}

/**
 * @test test_render_glyph_success
 * @brief priv_render_into full success path -- lines 219, 273-281, 578.
 *
 * @details Loads ahem.ttf and renders codepoint 'A' at s_render_px with a
 *          generously sized bitmap buffer.  The glyph has positive dimensions
 *          so total > 0, stbtt_MakeCodepointBitmap is called (line 276),
 *          out_w and out_h are set (lines 278-279), and k_ra8_ok is returned
 *          (line 280), covering the full priv_render_into success path and
 *          the ra8_epub_render_glyph() call site at line 578.  Line 219
 *          (return k_ra8_ok from priv_font_init) is also covered because
 *          stbtt_InitFont succeeds on ahem.ttf.
 *
 * @par MC/DC:
 * Decision: `if (total > 0U)` at line 273 (1 condition).
 * - V1 (this test): total = w*h for 'A' > 0 -> true  -> line 276 taken.
 * - V2: total == 0 (zero-size glyph) -> false -> body skipped.
 * This test provides V1; V2 is not required for the line-coverage gate.
 *
 * @pre s_have_font must be true (priv_load_ahem() called in main).
 * @pre s_font_buf holds a valid TTF accepted by stbtt_InitFont.
 * @post s_glyph_buf[0..out_w*out_h-1] contains the rasterised alpha mask.
 * @post w and h report the rendered glyph dimensions (both > 0 for 'A').
 * @note Not thread-safe; skipped with a log line if s_have_font is false.
 * @since 0.1.0
 */
static void test_render_glyph_success(void)
{
  TEST_BEGIN("epub_render_glyph: priv_render_into full success path");
  if (!s_have_font) {
    (void)fprintf(stderr, "[SKIP] ahem.ttf unavailable\n");
    TEST_END("epub_render_glyph: priv_render_into full success path");
    return;
  }
  ra8_epub_book_t book = {};
  book.in_use          = 1U;
  uint32_t w           = 0U;
  uint32_t h           = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_set_font(&book, s_font_buf, s_font_len));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_epub_render_glyph(&book,
                                       (int32_t)k_cov_cp_letter_a,
                                       s_render_px,
                                       s_glyph_buf,
                                       sizeof(s_glyph_buf),
                                       &w,
                                       &h));
  TEST_ASSERT(w > 0U);
  TEST_ASSERT(h > 0U);
  TEST_END("epub_render_glyph: priv_render_into full success path");
}

/* ---------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Test entry point.
 *
 * @details Loads ahem.ttf once, then runs all coverage-boost test functions
 *          in order.  Font-dependent tests self-skip when the font could not
 *          be loaded from disk.
 *
 * @return 0 on success; exit(1) is called by unity_minimal.h on first failure.
 *
 * @pre None.
 * @pre None.
 * @post All tests in this TU have executed (or been skipped).
 * @post stderr contains a per-test RUN/PASS/SKIP log.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
int32_t main(void)
{
  /* Load the real font once before the font-dependent groups. */
  (void)priv_load_ahem();

  /* Group 1: ra8_epub_manifest_count / ra8_epub_manifest_item guards. */
  test_manifest_count_null_book();
  test_manifest_item_null_and_not_ready();
  test_manifest_item_out_of_range();

  /* Group 2: priv_font_init stbtt_InitFont failure path. */
  test_render_glyph_stbtt_init_fail();

  /* Group 3: priv_render_into (real font required). */
  test_render_glyph_no_mem();
  test_render_glyph_success();

  (void)fprintf(stderr, "[OK ] test_ra8_epub_chapter_cov.c\n");
  return 0;
}
