/**
 * @file ra8_gfx_font.h
 * @brief Bitmap font descriptor + bundled font handles for ra8_gfx.
 * @ingroup grp_ereader
 *
 * @details
 * The font format is intentionally tiny: a packed array of glyph rows,
 * one bit per pixel, MSB on the left. Rows are padded up to whole bytes,
 * so an 8x16 glyph takes 16 bytes and a 6x8 glyph takes 8 bytes. ASCII
 * codepoints `first_codepoint..last_codepoint` (inclusive) are stored
 * back-to-back.
 *
 * The bundled 8x16 font is reproduced from the public-domain IBM PC VGA
 * font (BIOS code page 437). The same glyph bitmaps appear in dozens of
 * BSD/MIT licensed sources -- the canonical reference used here is the
 * VileR "oldschool PC fonts" archive which states:
 *
 *   "All the original IBM-made fonts (and their identical re-creations)
 *    can be considered to be in the public domain."
 *   -- https://int10h.org/oldschool-pc-fonts/readme/ (retrieved 2026)
 *
 * Only ASCII 0x20..0x7E (95 glyphs) are bundled to keep the binary
 * footprint at 95 * 16 = 1520 bytes.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

/**
 * @struct ra8_gfx_font_t
 * @brief Monochrome, fixed-width bitmap font descriptor.
 *
 * @details
 * Glyphs are packed left-to-right, MSB-first within each byte; rows are
 * stored top-to-bottom; glyphs are stored in ascending codepoint order.
 *
 * @invariant glyph_width >= 1 && glyph_height >= 1
 * @invariant bytes_per_glyph == ((glyph_width + 7) / 8) * glyph_height
 * @invariant first_codepoint <= last_codepoint
 */
typedef struct {
  const uint8_t* glyph_data;   /**< Packed glyph bitmaps; size = bytes_per_glyph * glyph_count. */
  uint8_t        glyph_width;  /**< Glyph width in pixels.                                      */
  uint8_t        glyph_height; /**< Glyph height in pixels.                                     */
  uint8_t bytes_per_glyph;     /**< Bytes occupied by one glyph (rows * ceil(width/8)).         */
  uint8_t first_codepoint;     /**< Lowest stored codepoint (typically 0x20, space).            */
  uint8_t last_codepoint;      /**< Highest stored codepoint (typically 0x7E, '~').             */
} ra8_gfx_font_t;

/**
 * @enum ra8_gfx_text_face_t
 * @brief Text face selectable by the face-aware draw and measure calls.
 *
 * @details The sans face retains the bundled IBM 8x16 face. The serif face
 *          uses the Literata Latin-1/common-punctuation atlas generated from
 *          the project's licensed font asset. Unsupported Unicode scalars
 *          draw as the question-mark replacement glyph.
 */
typedef enum : uint8_t {
  k_ra8_gfx_text_face_sans  = 0U, /**< Bundled IBM 8x16 sans face. */
  k_ra8_gfx_text_face_serif = 1U, /**< Literata regular serif face. */
} ra8_gfx_text_face_t;

/**
 * @brief Bundled 8x16 IBM PC VGA bitmap font, ASCII 0x20..0x7E.
 *
 * @details
 * Public-domain glyph bitmaps reproduced from the IBM BIOS codepage 437
 * font, restricted to printable ASCII to keep the static footprint
 * minimal (95 * 16 = 1520 bytes).
 *
 * @since 0.1.0
 */
/* NOLINTNEXTLINE(readability-identifier-naming) -- API symbol kept un-prefixed by design. */
extern const ra8_gfx_font_t ra8_gfx_font_8x16;
