/**
 * @file test_epub_chapter_cov.c
 * @brief Coverage-boost tests for apps/shared_libs/epub/src/epub_chapter.c.
 *
 * @details
 * Exercises four groups of previously-uncovered code paths:
 *
 *   1. ``epub_manifest_count`` -- null-book early return.
 *   2. ``epub_manifest_item`` -- null/not-initialised and index-range guards.
 *   3. ``internal_font_init`` -- ``stbtt_InitFont`` failure.
 *      A 32-byte blob whose first four bytes match the OpenType 1.0 version
 *      tag (0x00 0x01 0x00 0x00) causes stbtt_GetFontOffsetForIndex to
 *      return offset 0, so the negative-offset guard passes.
 *      ``stbtt_InitFont`` then finds no required table-directory entries and
 *      drives the validation-failed return.
 *   4. ``internal_render_into`` and its ``epub_render_glyph`` call site.
 *      Uses the repository's required TrueType fixture from
 *      apps/shared_libs/third_party/litehtml/containers/test/fonts/ahem.ttf from the
 *      CMake-provided repository root; a missing fixture is a hard test
 *      failure. Malformed public inputs separately exercise the ZIP-stat
 *      failure and reversed-glyph-bounds guards, so neither path requires a
 *      source exclusion.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "epub.h"
#include "epub_internal.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "stb_truetype.h"
#include "unity_minimal.h"

/* ---------------------------------------------------------------------------
 * Sizing constants used by this coverage test TU.
 * ---------------------------------------------------------------------------
 */

/**
 * @enum cov_sizes_t
 * @brief Capacities and limits used by this coverage-boost test TU.
 */
typedef enum : uint32_t {
  k_cov_font_min_bytes = 16U,    /**< Minimum bytes epub_set_font() accepts.      */
  k_cov_glyph_pixels   = 4096U,  /**< Alpha-8 glyph scratch buffer size in bytes. */
  k_cov_font_load_cap  = 32768U, /**< Maximum font data capacity in bytes.        */
} cov_sizes_t;

/**
 * @enum cov_codepoints_t
 * @brief Code points used by the glyph-render tests.
 */
typedef enum : uint32_t {
  k_cov_cp_letter_a = 65U, /**< 'A': a real glyph with positive bitmap dimensions. */
} cov_codepoints_t;

/**
 * @enum cov_ttf_layout_t
 * @brief TrueType `loca` and `glyf` fields used by the malformed-font fixture.
 */
typedef enum : uint8_t {
  k_cov_ttf_loca_short_bytes = 2U,  /**< Bytes in a short loca entry.        */
  k_cov_ttf_loca_long_bytes  = 4U,  /**< Bytes in a long loca entry.         */
  k_cov_ttf_glyph_header     = 10U, /**< Bytes in the fixed glyf header.     */
  k_cov_ttf_x_min_ofs        = 2U,  /**< Big-endian xMin within glyf header. */
  k_cov_ttf_x_max_ofs        = 6U,  /**< Big-endian xMax within glyf header. */
  k_cov_ttf_be_shift         = 8U,  /**< Bits in each big-endian byte.       */
} cov_ttf_layout_t;

/**
 * @enum cov_ttf_bbox_t
 * @brief Raw signed-coordinate encodings that force xMax below xMin.
 */
typedef enum : uint16_t {
  k_cov_ttf_x_min_reversed = 0x4000U, /**< Positive xMin fixture value. */
  k_cov_ttf_x_max_reversed = 0xC000U, /**< Negative xMax fixture value. */
} cov_ttf_bbox_t;

/* ---------------------------------------------------------------------------
 * Fake TTF blob that passes stbtt_GetFontOffsetForIndex but fails
 * stbtt_InitFont, driving the malformed-font rejection in internal_font_init.
 *
 * Bytes 0-3 are the OpenType 1.0 sfVersion (0x00 0x01 0x00 0x00).
 * stbtt__isfont() matches the tag `stbtt_tag4(font, 0, 1, 0, 0)` and
 * returns true, so stbtt_GetFontOffsetForIndex(blob, 0) returns 0 (a
 * valid offset >= 0).  The following two bytes encode numTables = 0, so
 * stbtt_InitFont finds no table directory entries and returns 0.
 * ---------------------------------------------------------------------------
 */

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
 * @note Populated by internal_load_ahem() once in main().
 * @warning Do not modify directly.
 * @since 0.1.0
 */
static uint8_t s_font_buf[k_cov_font_load_cap];

/**
 * @var s_font_len
 * @brief Number of valid bytes in s_font_buf; 0 if not yet loaded.
 * @note Set by internal_load_ahem().
 * @since 0.1.0
 */
static size_t s_font_len = 0U;

/**
 * @var s_have_font
 * @brief True once internal_load_ahem() has successfully loaded ahem.ttf.
 * @note Asserted before the render tests execute.
 * @since 0.1.0
 */
static bool s_have_font = false;

/**
 * @var s_render_px
 * @brief Font pixel height passed to epub_render_glyph() in tests.
 * @note Floating-point; must use const because C23 enums cannot hold floats.
 * @since 0.1.0
 */
static const float s_render_px = 16.0F;

/**
 * @brief Load ahem.ttf from the vendored litehtml test-font directory.
 *
 * @details Constructs the absolute path to
 *          apps/shared_libs/third_party/litehtml/containers/test/fonts/ahem.ttf
 *          relative to the CMake-provided repository root and
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
 * @since 0.1.0 @retval true The named fixture condition holds. */
RA8_INTERNAL static bool internal_load_ahem(void)
{
  const int descriptor =
    open(RA8_TEST_REPO_ROOT "/apps/shared_libs/third_party/litehtml/containers/test/fonts/ahem.ttf",
         O_RDONLY);
  if (descriptor < 0) {
    s_have_font = false;
    return false;
  }
  size_t used = 0U;
  while (used < sizeof(s_font_buf)) {
    ssize_t got = -1;
    do {
      errno = 0;
      got   = read(descriptor, &s_font_buf[used], sizeof(s_font_buf) - used);
    } while ((got < 0) && (errno == EINTR));
    if (got <= 0) {
      break;
    }
    used += (size_t)got;
  }
  (void)close(descriptor);
  if (used < (size_t)k_cov_font_min_bytes) {
    s_have_font = false;
    return false;
  }
  s_font_len  = used;
  s_have_font = true;
  return true;
}

/**
 * @brief Read a bounded big-endian uint16 from ::s_font_buf.
 * @param[in] offset Byte offset of the value.
 * @return Decoded uint16.
 * @pre Two bytes beginning at @p offset lie within ::s_font_len.
 * @pre ::s_font_buf contains the tracked TrueType fixture.
 * @post Font bytes are unchanged.
 * @post Return value depends only on the selected bytes.
 * @note File-local and thread-safe while the fixture is immutable.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t internal_font_be16(size_t offset)
{
  TEST_ASSERT((offset + (size_t)k_cov_ttf_loca_short_bytes) <= s_font_len);
  return (uint16_t)(((uint16_t)s_font_buf[offset] << (uint8_t)k_cov_ttf_be_shift) |
                    (uint16_t)s_font_buf[offset + 1U]);
}

/**
 * @brief Read a bounded big-endian uint32 from ::s_font_buf.
 * @param[in] offset Byte offset of the value.
 * @return Decoded uint32.
 * @pre Four bytes beginning at @p offset lie within ::s_font_len.
 * @pre ::s_font_buf contains the tracked TrueType fixture.
 * @post Font bytes are unchanged.
 * @post Return value depends only on the selected bytes.
 * @note File-local and thread-safe while the fixture is immutable.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_font_be32(size_t offset)
{
  TEST_ASSERT((offset + (size_t)k_cov_ttf_loca_long_bytes) <= s_font_len);
  uint32_t value = 0U;
  for (uint8_t byte = 0U; byte < (uint8_t)k_cov_ttf_loca_long_bytes; ++byte) {
    value = (value << (uint8_t)k_cov_ttf_be_shift) | s_font_buf[offset + byte];
  }
  return value;
}

/**
 * @brief Store a big-endian uint16 in ::s_font_buf.
 * @param[in] offset Byte offset of the value.
 * @param[in] value Value to encode.
 * @pre Two bytes beginning at @p offset lie within ::s_font_len.
 * @pre ::s_font_buf is writable.
 * @post Exactly the selected two bytes encode @p value.
 * @post Font length is unchanged.
 * @note File-local malformed-input fixture helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_font_set_be16(size_t offset, uint16_t value)
{
  TEST_ASSERT((offset + (size_t)k_cov_ttf_loca_short_bytes) <= s_font_len);
  s_font_buf[offset]      = (uint8_t)(value >> (uint8_t)k_cov_ttf_be_shift);
  s_font_buf[offset + 1U] = (uint8_t)value;
}

/**
 * @brief Resolve the fixed `glyf` header for the tracked letter-A glyph.
 * @return Byte offset of the glyph header in ::s_font_buf.
 * @pre ::internal_load_ahem succeeded.
 * @pre The font uses a supported short or long `loca` table.
 * @post Font bytes are unchanged.
 * @post The returned ten-byte header lies inside ::s_font_len.
 * @note Uses stb_truetype only to resolve the code point to a glyph index.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t internal_letter_a_glyph_header(void)
{
  stbtt_fontinfo font = {};
  TEST_ASSERT(stbtt_InitFont(&font, s_font_buf, (int)s_font_len, 0) != 0);
  const int glyph = stbtt_FindGlyphIndex(&font, (int)k_cov_cp_letter_a);
  TEST_ASSERT(glyph >= 0);
  TEST_ASSERT(font.loca >= 0);
  TEST_ASSERT(font.glyf >= 0);

  uint32_t relative = 0U;
  if (font.indexToLocFormat == 0) {
    const size_t loca = (size_t)font.loca + ((size_t)glyph * (size_t)k_cov_ttf_loca_short_bytes);
    relative          = (uint32_t)internal_font_be16(loca) * (uint32_t)k_cov_ttf_loca_short_bytes;
  } else {
    TEST_ASSERT_EQ(1, font.indexToLocFormat);
    const size_t loca = (size_t)font.loca + ((size_t)glyph * (size_t)k_cov_ttf_loca_long_bytes);
    relative          = internal_font_be32(loca);
  }
  const size_t header = (size_t)font.glyf + (size_t)relative;
  TEST_ASSERT((header + (size_t)k_cov_ttf_glyph_header) <= s_font_len);
  return header;
}

/* ---------------------------------------------------------------------------
 * Tests: epub_manifest_count null-book guard.
 * ---------------------------------------------------------------------------
 */

/**
 * @test internal_test_manifest_count_null_book
 * @brief epub_manifest_count(nullptr) returns zero.
 *
 * @details The existing suite calls epub_manifest_count() only on
 *          non-null books. This test passes a null pointer to drive the
 *          null-book return.
 *
 * @par MC/DC:
 * Decision: `if (book == nullptr)` (1 condition).
 * - V1 (this test): book=NULL -> true -> return zero.
 * - V2 (existing): book!=NULL -> false -> inspect the book state.
 * Pair (V1, V2) gives complete MC/DC for the 1-condition decision.
 *
 * @pre None.
 * @pre None.
 * @post No side effects.
 * @post No side effects.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_manifest_count_null_book(void)
{
  TEST_BEGIN("epub_manifest_count: null book -> 0");
  TEST_ASSERT_EQ(0, epub_manifest_count(nullptr));
  TEST_END("epub_manifest_count: null book -> 0");
}

/* ---------------------------------------------------------------------------
 * Tests: epub_manifest_item null, not-ready, and range guards.
 * ---------------------------------------------------------------------------
 */

/**
 * @test internal_test_manifest_item_null_and_not_ready
 * @brief epub_manifest_item() rejects null and not-ready books.
 *
 * @details Drives the `book == nullptr || book->in_use == 0U` compound
 *          decision, returning nullptr for either rejecting condition.
 *
 * @par MC/DC:
 * Decision: `if (book == nullptr || book->in_use == 0U)` (2 conditions, OR).
 * - V1 (this test): book=NULL -> C1=T (short-circuit) -> nullptr.
 * - V2 (this test): book!=NULL, in_use=0 -> C1=F, C2=T -> nullptr.
 * - V3 (existing): book!=NULL, in_use=1 -> C1=F, C2=F -> continue.
 * Pairs (V1,V3) isolate C1; pairs (V2,V3) isolate C2. N=2 -> N+1=3 vectors.
 *
 * @pre None.
 * @pre None.
 * @post No side effects.
 * @post No side effects.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_manifest_item_null_and_not_ready(void)
{
  TEST_BEGIN("epub_manifest_item: null + not-ready -> nullptr");
  epub_book_t book = {};
  /* V1: null pointer. */
  TEST_ASSERT_NULL(epub_manifest_item(nullptr, 0U));
  /* V2: non-null book with in_use == 0 (not initialised). */
  TEST_ASSERT_NULL(epub_manifest_item(&book, 0U));
  TEST_END("epub_manifest_item: null + not-ready -> nullptr");
}

/**
 * @test internal_test_manifest_item_out_of_range
 * @brief epub_manifest_item() returns nullptr for an out-of-range index.
 *
 * @details Opens a book with manifest_count == 0, then requests index 0 to
 *          drive the `index >= book->manifest_count` rejection.
 *
 * @par MC/DC:
 * (no compound decision is exercised for MC/DC by this case -- it drives the
 * single-condition `index >= book->manifest_count` out-of-range guard on a ready
 * book (manifest_count == 0, index 0). The compound
 * `book == nullptr || book->in_use == 0U` guard is reached only as the both-false
 * control here; its N+1 = 3 vectors live in internal_test_manifest_item_null_and_not_ready)
 *
 * @pre None.
 * @pre None.
 * @post No side effects.
 * @post No side effects.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_manifest_item_out_of_range(void)
{
  TEST_BEGIN("epub_manifest_item: out-of-range index -> nullptr");
  epub_book_t book    = {};
  book.in_use         = 1U;
  book.manifest_count = 0U;
  TEST_ASSERT_NULL(epub_manifest_item(&book, 0U));
  TEST_END("epub_manifest_item: out-of-range index -> nullptr");
}

/* ---------------------------------------------------------------------------
 * Tests: internal_font_init stbtt_InitFont failure path.
 * ---------------------------------------------------------------------------
 */

/**
 * @test internal_test_render_glyph_stbtt_init_fail
 * @brief A fake TTF signature makes stbtt_InitFont fail.
 *
 * @details s_fake_ttf_sig starts with 0x00 0x01 0x00 0x00 (the OpenType 1.0
 *          sfVersion).  stbtt__isfont() recognises the tag and returns true,
 *          so stbtt_GetFontOffsetForIndex(blob, 0) returns 0 (a valid offset
 *          >= 0, passing the negative-offset guard).
 *          stbtt_InitFont then searches for the required table entries via
 *          stbtt__find_table(); because numTables == 0 in the blob, all
 *          lookups return 0 and stbtt_InitFont returns 0 (failure), driving
 *          the validation-failed return.
 *
 * @par MC/DC:
 * Decision: `stbtt_InitFont(...) == 0` (1 condition).
 * - V1 (this test): stbtt_InitFont returns 0 -> true -> validation failure.
 * - V2 (internal_test_render_glyph_success): nonzero -> false -> continue.
 * Pair (V1, V2) gives complete MC/DC for the 1-condition decision.
 *
 * @pre s_fake_ttf_sig must be >= k_cov_font_min_bytes (it is 32 bytes).
 * @pre None.
 * @post No side effects.
 * @post No side effects.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_render_glyph_stbtt_init_fail(void)
{
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
    0x00U, /* rangeShift = 0; remaining bytes are zero. */
  };
  TEST_BEGIN("epub_render_glyph: fake TTF sig -> stbtt_InitFont fails -> validation_failed");
  epub_book_t book = {};
  book.in_use      = 1U;
  uint32_t w       = 0U;
  uint32_t h       = 0U;
  /* Install the fake-signature blob; >= k_cov_font_min_bytes so set_font accepts it. */
  TEST_ASSERT_EQ(k_ra8_ok, epub_set_font(&book, s_fake_ttf_sig, sizeof(s_fake_ttf_sig)));
  /* The font offset is valid, but stbtt_InitFont rejects the empty table set. */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 epub_render_glyph(&book,
                                   (int32_t)k_cov_cp_letter_a,
                                   s_render_px,
                                   s_glyph_buf,
                                   sizeof(s_glyph_buf),
                                   &w,
                                   &h));
  TEST_END("epub_render_glyph: fake TTF sig -> stbtt_InitFont fails -> validation_failed");
}

/* ---------------------------------------------------------------------------
 * Tests: internal_render_into through the public glyph renderer.
 * ---------------------------------------------------------------------------
 */

/**
 * @test internal_test_render_glyph_reversed_bbox
 * @brief Reversed raw `glyf` x coordinates are rejected before rasterisation.
 * @details The tracked ahem.ttf is copied into writable fixture storage. This
 *          case resolves letter A through stb_truetype's ordinary code-point
 *          mapping, changes only that glyph's xMin/xMax header fields so xMax
 *          is below xMin, and calls the public EPUB font/render APIs. Restoring
 *          the original coordinates provides the success control.
 * @par MC/DC:
 * Decision: `priv_epub_glyph_dim_invalid(w, h)` in `internal_render_into`.
 * - V1: reversed x coordinates -> w < 0 -> true -> validation failure.
 * - V2: original coordinates -> w >= 0 and h >= 0 -> false -> render succeeds.
 * The existing direct predicate vectors independently exercise the h arm; this
 * public-API pair proves malformed font bytes can reach the production guard.
 * @pre ::internal_load_ahem succeeded and populated ::s_font_buf.
 * @pre The tracked fixture maps letter A to a non-empty TrueType glyf record.
 * @post The original xMin/xMax bytes are restored before the success control.
 * @post Public outputs remain zero on rejection and become positive on success.
 * @note Single-threaded because it temporarily mutates ::s_font_buf.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_render_glyph_reversed_bbox(void)
{
  TEST_BEGIN("epub_render_glyph: reversed glyf bbox -> validation_failed");
  TEST_ASSERT(s_have_font);
  const size_t   header         = internal_letter_a_glyph_header();
  const size_t   x_min          = header + (size_t)k_cov_ttf_x_min_ofs;
  const size_t   x_max          = header + (size_t)k_cov_ttf_x_max_ofs;
  const uint16_t original_x_min = internal_font_be16(x_min);
  const uint16_t original_x_max = internal_font_be16(x_max);
  internal_font_set_be16(x_min, (uint16_t)k_cov_ttf_x_min_reversed);
  internal_font_set_be16(x_max, (uint16_t)k_cov_ttf_x_max_reversed);

  epub_book_t book = {};
  book.in_use      = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, epub_set_font(&book, s_font_buf, s_font_len));
  uint32_t width  = UINT32_MAX;
  uint32_t height = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 epub_render_glyph(&book,
                                   (int32_t)k_cov_cp_letter_a,
                                   s_render_px,
                                   s_glyph_buf,
                                   sizeof(s_glyph_buf),
                                   &width,
                                   &height));
  TEST_ASSERT_EQ(0U, width);
  TEST_ASSERT_EQ(0U, height);

  internal_font_set_be16(x_min, original_x_min);
  internal_font_set_be16(x_max, original_x_max);
  TEST_ASSERT_EQ(k_ra8_ok,
                 epub_render_glyph(&book,
                                   (int32_t)k_cov_cp_letter_a,
                                   s_render_px,
                                   s_glyph_buf,
                                   sizeof(s_glyph_buf),
                                   &width,
                                   &height));
  TEST_ASSERT(width > 0U);
  TEST_ASSERT(height > 0U);
  TEST_END("epub_render_glyph: reversed glyf bbox -> validation_failed");
}

/**
 * @test internal_test_render_glyph_no_mem
 * @brief internal_render_into rejects a bitmap larger than max_pixels.
 *
 * @details Loads ahem.ttf and requests codepoint 'A' (k_cov_cp_letter_a) at
 *          s_render_px with max_pixels == 0.  The glyph has positive w and h,
 *          so total = w * h > 0 == max_pixels and the function returns
 *          k_ra8_err_no_mem after computing its scale and bounding box.
 *
 * @par MC/DC:
 * Decision: `total > max_pixels` (1 condition).
 * - V1 (this test): total > 0 with max_pixels=0 -> true -> no memory.
 * - V2 (internal_test_render_glyph_success): sufficient capacity -> false.
 * Pair (V1, V2) gives complete MC/DC for the 1-condition decision.
 *
 * @pre s_have_font must be true (internal_load_ahem() called in main).
 * @pre s_font_buf holds a valid TTF accepted by stbtt_InitFont.
 * @post s_glyph_buf contents are undefined (no write occurs with max_pixels=0).
 * @post No other side effects.
 * @note Not thread-safe; `main` requires the tracked font before this runs.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_render_glyph_no_mem(void)
{
  TEST_BEGIN("epub_render_glyph: internal_render_into total > max_pixels -> no_mem");
  epub_book_t book = {};
  book.in_use      = 1U;
  uint32_t w       = 0U;
  uint32_t h       = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, epub_set_font(&book, s_font_buf, s_font_len));
  /* max_pixels == 0: any non-space glyph satisfies total > max_pixels. */
  TEST_ASSERT_EQ(
    k_ra8_err_no_mem,
    epub_render_glyph(&book, (int32_t)k_cov_cp_letter_a, s_render_px, s_glyph_buf, 0U, &w, &h));
  TEST_END("epub_render_glyph: internal_render_into total > max_pixels -> no_mem");
}

/**
 * @test internal_test_render_glyph_success
 * @brief Exercise the full successful internal_render_into path.
 *
 * @details Loads ahem.ttf and renders codepoint 'A' at s_render_px with a
 *          generously sized bitmap buffer.  The glyph has positive dimensions
 *          so stbtt_MakeCodepointBitmap writes the alpha mask, the dimensions
 *          are published, and k_ra8_ok is returned through epub_render_glyph.
 *          This also supplies the successful internal_font_init control.
 *
 * @par MC/DC:
 * Decision: `total > 0U` (1 condition).
 * - V1 (this test): total = w*h for 'A' > 0 -> true -> rasterize.
 * - V2: total == 0 (zero-size glyph) -> false -> body skipped.
 * This test provides V1; V2 is not required for the line-coverage gate.
 *
 * @pre s_have_font must be true (internal_load_ahem() called in main).
 * @pre s_font_buf holds a valid TTF accepted by stbtt_InitFont.
 * @post s_glyph_buf[0..out_w*out_h-1] contains the rasterised alpha mask.
 * @post w and h report the rendered glyph dimensions (both > 0 for 'A').
 * @note Not thread-safe; `main` requires the tracked font before this runs.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_render_glyph_success(void)
{
  TEST_BEGIN("epub_render_glyph: internal_render_into full success path");
  epub_book_t book = {};
  book.in_use      = 1U;
  uint32_t w       = 0U;
  uint32_t h       = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, epub_set_font(&book, s_font_buf, s_font_len));
  TEST_ASSERT_EQ(k_ra8_ok,
                 epub_render_glyph(&book,
                                   (int32_t)k_cov_cp_letter_a,
                                   s_render_px,
                                   s_glyph_buf,
                                   sizeof(s_glyph_buf),
                                   &w,
                                   &h));
  TEST_ASSERT(w > 0U);
  TEST_ASSERT(h > 0U);
  TEST_END("epub_render_glyph: internal_render_into full success path");
}

/* ---------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Test entry point.
 *
 * @details Requires ahem.ttf once, then runs all coverage-boost test functions
 *          in order. A missing or invalid tracked fixture fails the test run.
 *
 * @return 0 on success; exit(1) is called by unity_minimal.h on first failure.
 *
 * @pre None.
 * @pre None.
 * @post All tests in this TU have executed.
 * @post stderr contains a per-test RUN/PASS log.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
int main(void)
{
  /* Load the real font once before the font-dependent groups. */
  TEST_ASSERT(internal_load_ahem());

  /* Group 1: epub_manifest_count / epub_manifest_item guards. */
  internal_test_manifest_count_null_book();
  internal_test_manifest_item_null_and_not_ready();
  internal_test_manifest_item_out_of_range();

  /* Group 2: internal_font_init stbtt_InitFont failure path. */
  internal_test_render_glyph_stbtt_init_fail();

  /* Group 3: internal_render_into (real font required). */
  internal_test_render_glyph_reversed_bbox();
  internal_test_render_glyph_no_mem();
  internal_test_render_glyph_success();

  return 0;
}
