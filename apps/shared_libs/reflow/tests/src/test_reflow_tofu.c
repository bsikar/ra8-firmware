/**
 * @file test_reflow_tofu.c
 * @brief Missing-glyph (tofu) fallback vectors for reflow_render.c.
 *
 * @details
 * Drives the three pure helpers the render pass uses to decide whether a code
 * point the face cannot draw becomes a visible box, and how big that box is:
 *
 *  - ``priv_reflow_render_is_blank_cp`` over every blank code point the
 *    render pass must keep blank, plus inked neighbours of each.
 *  - ``priv_reflow_render_needs_tofu``'s ``index == 0 && !blank(cp)`` AND
 *    decision, N+1 vectors, against the production symbol rather than a
 *    mirror.
 *  - ``priv_reflow_render_tofu_rect``'s geometry: the box sits on the
 *    baseline, stays inside the advance, and falls back to fractions of the
 *    font size when the face reports no usable advance or ascent.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_test_output.h"
#include "reflow_internal.h"
#include "unity_minimal.h"

/**
 * @enum tofu_test_consts_t
 * @brief Fixture sizes for the missing-glyph geometry vectors.
 */
typedef enum : int32_t {
  k_tt_font_px     = 24,     /**< Representative body size in pixels.      */
  k_tt_advance_px  = 16,     /**< Advance a face reports at that size.     */
  k_tt_ascent_px   = 20,     /**< Face ascent at that size.                */
  k_tt_tiny_px     = 2,      /**< Advance too small for an inset box.      */
  k_tt_min_px      = 3,      /**< Mirror of k_priv_tofu_min_px.            */
  k_tt_glyph_found = 7,      /**< Any non-zero glyph index.                */
  k_tt_cp_letter_a = 0x41,   /**< LATIN CAPITAL LETTER A (inked).          */
  k_tt_cp_eacute   = 0xE9,   /**< LATIN SMALL LETTER E WITH ACUTE (inked). */
  k_tt_cp_cjk      = 0x4E00, /**< CJK UNIFIED IDEOGRAPH-4E00 (inked).      */
  k_tt_cp_space    = 0x20,   /**< SPACE (blank).                           */
  k_tt_cp_nbsp     = 0xA0,   /**< NO-BREAK SPACE (blank).                  */
  k_tt_cp_zwsp     = 0x200B, /**< ZERO WIDTH SPACE (blank).                */
  k_tt_cp_bom      = 0xFEFF, /**< ZERO WIDTH NO-BREAK SPACE (blank).       */
  k_tt_cp_replace  = 0xFFFD, /**< REPLACEMENT CHARACTER (inked).           */
  k_tt_cp_bad      = -1,     /**< Not a code point at all (inked -> box).  */
} tofu_test_consts_t;

/**
 * @test internal_test_blank_code_points
 *
 * @brief Every blank code point stays blank; inked neighbours do not.
 * @details Performs one bounded, deterministic operation for this host test.
 * @return Nothing.
 * @pre Module state is consistent.
 * @post Failures are reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_blank_code_points(void)
{
  TEST_BEGIN("reflow tofu: blank code points draw nothing");
  /* The full blank set the render pass must keep empty. */
  TEST_ASSERT(priv_reflow_render_is_blank_cp(0x09));
  TEST_ASSERT(priv_reflow_render_is_blank_cp(0x0A));
  TEST_ASSERT(priv_reflow_render_is_blank_cp(0x0D));
  TEST_ASSERT(priv_reflow_render_is_blank_cp((int32_t)k_tt_cp_space));
  TEST_ASSERT(priv_reflow_render_is_blank_cp((int32_t)k_tt_cp_nbsp));
  TEST_ASSERT(priv_reflow_render_is_blank_cp((int32_t)k_tt_cp_zwsp));
  TEST_ASSERT(priv_reflow_render_is_blank_cp(0x200C));
  TEST_ASSERT(priv_reflow_render_is_blank_cp(0x200D));
  TEST_ASSERT(priv_reflow_render_is_blank_cp(0x2060));
  TEST_ASSERT(priv_reflow_render_is_blank_cp((int32_t)k_tt_cp_bom));

  /* Neighbours of the blank code points are ordinary inked characters. */
  TEST_ASSERT(!priv_reflow_render_is_blank_cp((int32_t)k_tt_cp_letter_a));
  TEST_ASSERT(!priv_reflow_render_is_blank_cp((int32_t)k_tt_cp_eacute));
  TEST_ASSERT(!priv_reflow_render_is_blank_cp((int32_t)k_tt_cp_cjk));
  TEST_ASSERT(!priv_reflow_render_is_blank_cp(0x1F)); /* just below TAB's block */
  TEST_ASSERT(!priv_reflow_render_is_blank_cp(0x21)); /* just above SPACE       */
  TEST_ASSERT(!priv_reflow_render_is_blank_cp(0x9F)); /* just below NBSP        */
  TEST_ASSERT(!priv_reflow_render_is_blank_cp(0x200A));
  TEST_ASSERT(!priv_reflow_render_is_blank_cp(0x200E));
  TEST_ASSERT(!priv_reflow_render_is_blank_cp((int32_t)k_tt_cp_replace));

  /* A value that is not a scalar code point must not be hidden. */
  TEST_ASSERT(!priv_reflow_render_is_blank_cp((int32_t)k_tt_cp_bad));
  TEST_END("reflow tofu: blank code points draw nothing");
}

/**
 * @test internal_test_mcdc_needs_tofu
 *
 * @brief MC/DC vectors for ``index == 0 && !blank(cp)``.
 * @details Performs one bounded, deterministic operation for this host test.
 * @return Nothing.
 * @pre Module state is consistent.
 * @post Failures are reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 *
 * @par MC/DC:
 * Decision: ``(glyph_index == 0) && (!is_blank_cp(cp))``
 * (2 conditions, apps/shared_libs/reflow/src/reflow_render.c@priv_reflow_render_needs_tofu)
 *  - V1 index!=0, cp inked -> false (control; varies C1 against V2)
 *  - V2 index==0, cp inked -> true
 *  - V3 index==0, cp blank -> false (varies C2 against V2)
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_needs_tofu(void)
{
  TEST_BEGIN("reflow tofu MC/DC: index==0 && !blank");
  /* V1: the face has a glyph, so nothing is substituted. */
  TEST_ASSERT(!priv_reflow_render_needs_tofu((int32_t)k_tt_glyph_found, (int32_t)k_tt_cp_eacute));
  /* V2: no glyph for an inked character -> draw the box. */
  TEST_ASSERT(priv_reflow_render_needs_tofu(0, (int32_t)k_tt_cp_eacute));
  /* V3: no glyph, but the character carries no ink -> stay blank. */
  TEST_ASSERT(!priv_reflow_render_needs_tofu(0, (int32_t)k_tt_cp_space));

  /* The live cases this exists for: a CJK ideograph and a decoded entity the
   * baked Latin subset cannot draw must both become visible boxes. */
  TEST_ASSERT(priv_reflow_render_needs_tofu(0, (int32_t)k_tt_cp_cjk));
  TEST_ASSERT(priv_reflow_render_needs_tofu(0, (int32_t)k_tt_cp_replace));
  /* A missing zero-width joiner still draws nothing. */
  TEST_ASSERT(!priv_reflow_render_needs_tofu(0, (int32_t)k_tt_cp_zwsp));
  TEST_END("reflow tofu MC/DC: index==0 && !blank");
}

/**
 * @test internal_test_tofu_rect_geometry
 *
 * @brief The box sits on the baseline and stays inside the advance.
 * @details Performs one bounded, deterministic operation for this host test.
 * @return Nothing.
 * @pre Module state is consistent.
 * @post Failures are reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_tofu_rect_geometry(void)
{
  TEST_BEGIN("reflow tofu: box geometry");
  priv_reflow_tofu_rect_t r = {};

  /* Nominal metrics: inset 16/8 = 2 a side, so the box is 12 wide, and the
   * height is three quarters of the ascent. */
  TEST_ASSERT(priv_reflow_render_tofu_rect((int32_t)k_tt_advance_px,
                                           (int32_t)k_tt_ascent_px,
                                           (int32_t)k_tt_font_px,
                                           &r));
  TEST_ASSERT_EQ(2, r.x_off);
  TEST_ASSERT_EQ(12, r.w);
  TEST_ASSERT_EQ(15, r.h);
  /* Sitting on the baseline is the invariant the render pass relies on. */
  TEST_ASSERT_EQ(-r.h, r.y_off);
  /* The box never spills past the advance it stands in for. */
  TEST_ASSERT((r.x_off + r.w) <= (int32_t)k_tt_advance_px);

  /* No usable ascent -> height falls back to two thirds of the font size. */
  TEST_ASSERT(priv_reflow_render_tofu_rect((int32_t)k_tt_advance_px, 0, (int32_t)k_tt_font_px, &r));
  TEST_ASSERT_EQ(16, r.h);
  TEST_ASSERT_EQ(-r.h, r.y_off);

  /* No usable advance -> width falls back to half the font size (12), inset 1. */
  TEST_ASSERT(priv_reflow_render_tofu_rect(0, (int32_t)k_tt_ascent_px, (int32_t)k_tt_font_px, &r));
  TEST_ASSERT_EQ(1, r.x_off);
  TEST_ASSERT_EQ(10, r.w);
  TEST_END("reflow tofu: box geometry");
}

/**
 * @test internal_test_tofu_rect_bounds
 *
 * @brief Degenerate inputs give a minimum box or a clean refusal.
 * @details Performs one bounded, deterministic operation for this host test.
 * @return Nothing.
 * @pre Module state is consistent.
 * @post Failures are reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_tofu_rect_bounds(void)
{
  TEST_BEGIN("reflow tofu: degenerate metrics");
  priv_reflow_tofu_rect_t r = {};

  /* An advance too narrow to inset still yields a legible box, flush left. */
  TEST_ASSERT(priv_reflow_render_tofu_rect((int32_t)k_tt_tiny_px,
                                           (int32_t)k_tt_ascent_px,
                                           (int32_t)k_tt_font_px,
                                           &r));
  TEST_ASSERT_EQ(0, r.x_off);
  TEST_ASSERT_EQ((int32_t)k_tt_min_px, r.w);

  /* A one-pixel font: both edges clamp to the minimum and the box still
   * sits on the baseline. */
  TEST_ASSERT(priv_reflow_render_tofu_rect(0, 0, 1, &r));
  TEST_ASSERT_EQ((int32_t)k_tt_min_px, r.w);
  TEST_ASSERT_EQ((int32_t)k_tt_min_px, r.h);
  TEST_ASSERT_EQ(-r.h, r.y_off);

  /* Refusals: no output storage, and a font size that cannot be drawn. */
  TEST_ASSERT(!priv_reflow_render_tofu_rect((int32_t)k_tt_advance_px,
                                            (int32_t)k_tt_ascent_px,
                                            (int32_t)k_tt_font_px,
                                            nullptr));
  TEST_ASSERT(
    !priv_reflow_render_tofu_rect((int32_t)k_tt_advance_px, (int32_t)k_tt_ascent_px, 0, &r));
  TEST_ASSERT(
    !priv_reflow_render_tofu_rect((int32_t)k_tt_advance_px, (int32_t)k_tt_ascent_px, -1, &r));
  TEST_END("reflow tofu: degenerate metrics");
}

int main(void)
{
  internal_test_blank_code_points();
  internal_test_mcdc_needs_tofu();
  internal_test_tofu_rect_geometry();
  internal_test_tofu_rect_bounds();
  TEST_ASSERT_EQ(k_ra8_test_output_ok,
                 internal_test_output_fd_text(STDERR_FILENO, "[OK ] test_reflow_tofu.c\n"));
  return 0;
}
