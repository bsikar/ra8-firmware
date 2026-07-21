/**
 * @file fuzz_ra8_stbtt.c
 * @brief libFuzzer harness for stbtt_InitFont() + glyph rasterisation.
 *
 * @details
 * The e-reader renders text with fonts embedded in the book file: EPUB
 * @font-face resources parsed by ra8_epub and rasterised through stb_truetype
 * (see priv_font_init / priv_render_into in libs/ra8_epub/src/ra8_epub_chapter.c
 * and the ra8_reflow layout driver). The font bytes are fully attacker-
 * controlled. This harness mirrors that path exactly: validate the offset with
 * stbtt_GetFontOffsetForIndex(), initialise a stbtt_fontinfo, then rasterise a
 * handful of code points via the no-allocation GetCodepointBitmapBox() +
 * MakeCodepointBitmap() two-step the firmware uses. stb_truetype trusts the
 * table offsets inside the font with minimal bounds checking, so a crafted
 * font can drive an out-of-bounds read -- exactly what ASan / UBSan surface
 * here. The internal rasteriser allocations are served by the same heap-free
 * ra8_stbtt bump arena the firmware links, so no host heap is involved.
 *
 * Build via tests/fuzz/CMakeLists.txt with -DRA8_FUZZ=ON.
 * Run for a longer fuzz session via scripts/checks/run_fuzz.sh.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "fuzz_entry.h"
#include "ra8_stbtt_guard.h" /* sfnt table-directory bounds guard (#217)       */
#include "stb_truetype.h"    /* stbtt_fontinfo + glyph API (declarations only) */

/**
 * @enum stbtt_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_stbtt_u_20 = 20, /**< Log2 of the fuzz input-size cap (1 MiB). */
} stbtt_uint8_const_t;

enum : int32_t {
  k_fuzz_ttf_px        = 24,  /**< Rasterisation height, pixels.      */
  k_fuzz_glyph_max_dim = 128, /**< Per-axis glyph bitmap cap, pixels. */
};

enum : uint32_t {
  k_fuzz_glyph_bytes = 128U * 128U, /**< Alpha-8 bitmap scratch, bytes. */
};

/** @brief Alpha-8 destination for the rasterised glyph (stride == width). */
static uint8_t s_bitmap[k_fuzz_glyph_bytes];

/** @brief A representative Latin-1 / punctuation code-point spread to render. */
static const int32_t s_codepoints[] = {0x41, 0x67, 0x30, 0x20, 0xE9, 0x2014};

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size == 0U || size > (1U << k_stbtt_u_20)) {
    return 0;
  }
  /* stbtt_GetFontOffsetForIndex returns -1 for non-TTF input; stbtt_InitFont
   * would dereference past the buffer if handed that offset (mirrors the guard
   * in ra8_epub_chapter.c priv_font_init). */
  const int offset = stbtt_GetFontOffsetForIndex(data, 0);
  if (offset < 0) {
    return 0;
  }
  /* Mirror the firmware's #217 hardening: bound-check the sfnt table directory
   * before stbtt_InitFont walks it (see ra8_stbtt_guard.h and priv_font_init in
   * libs/ra8_epub/src/ra8_epub_chapter.c). Without this, stbtt__find_table reads
   * past the buffer on a crafted font. */
  if (!ra8_stbtt_sfnt_dir_in_bounds(data, size, (uint32_t)offset)) {
    return 0;
  }
  stbtt_fontinfo font;
  if (stbtt_InitFont(&font, data, (int)size, offset) == 0) {
    return 0;
  }

  const float scale = stbtt_ScaleForPixelHeight(&font, (float)k_fuzz_ttf_px);
  for (size_t i = 0U; i < sizeof s_codepoints / sizeof s_codepoints[0]; ++i) {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    stbtt_GetCodepointBitmapBox(&font, s_codepoints[i], scale, scale, &x0, &y0, &x1, &y1);
    /* stb_truetype can hand back extreme box coordinates (down to INT_MIN)
     * for a crafted font, so compute the extents in int64_t: the naive
     * `x1 - x0` overflows int and is undefined behaviour. The bound below
     * (<= k_fuzz_glyph_max_dim) then discards any implausible box. */
    const int64_t w = (int64_t)x1 - (int64_t)x0;
    const int64_t h = (int64_t)y1 - (int64_t)y0;
    if (w <= 0 || h <= 0 || w > k_fuzz_glyph_max_dim || h > k_fuzz_glyph_max_dim) {
      continue;
    }
    if ((size_t)w * (size_t)h > sizeof s_bitmap) {
      continue;
    }
    stbtt_MakeCodepointBitmap(&font,
                              s_bitmap,
                              (int)w,
                              (int)h,
                              (int)w,
                              scale,
                              scale,
                              s_codepoints[i]);
  }
  return 0;
}
