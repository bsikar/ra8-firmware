/**
 * @file test_mcdc_reflow.c
 * @brief MC/DC floor vectors for reachable compound decisions in ra8_reflow.
 *
 * @details
 * Supplements the existing reflow MC/DC suites with the independence vectors
 * the per-file MC/DC floor still reports as gaps:
 *  - ra8_reflow_css.c@priv_ra8_reflow_css_ci_eq -- the `(s == nullptr) || (lit == nullptr)`
 *    contract guard, driven directly through the TU-external comparator.
 *  - the SVG path/element scanners in ra8_reflow_svg_path.c / ra8_reflow_svg_doc.c,
 *    driven through the public ::ra8_svg_render entry with crafted markup whose
 *    separators and terminators exercise each condition of the scan decisions.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_reflow_css_internal.h"
#include "ra8_reflow_svg.h"
#include "unity_minimal.h"

/** @brief Framebuffer dimensions for the SVG render integration vectors. */
enum : int32_t {
  k_fb_w = 32,         /**< Framebuffer / render-box width.  */
  k_fb_h = 32,         /**< Framebuffer / render-box height. */
  k_bg   = 0x00FFFFFF, /**< Cleared-background RGB.          */
};

/** @brief Host framebuffer (ARGB8888) bound by the render vectors. */
static uint32_t s_reflow_fb[k_fb_w * k_fb_h];

/** @brief Bind + clear the framebuffer, then render a NUL-terminated SVG.
 * @details Exercises the reflow render path and preserves each documented result and bound.
 * @param[in] svg NUL-terminated SVG document supplied to the render helper.
 * @return A status code describing the completed reflow operation.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval nonzero Validation or bounded-resource checks rejected the operation.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_reflow_render(const char* svg)
{
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gfx_init(s_reflow_fb, (uint16_t)k_fb_w, (uint16_t)k_fb_h, k_ra8_gfx_format_argb8888));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear((uint32_t)k_bg));
  return ra8_svg_render((const uint8_t*)svg, strlen(svg), 0, 0, k_fb_w, k_fb_h);
}

/**
 * @test internal_test_css_ci_eq_null_guard_mcdc
 * @brief Independence vectors for the priv_ra8_reflow_css_ci_eq NULL contract guard.
 *
 * @par MC/DC:
 * Decision: `if ((s == nullptr) || (lit == nullptr))` (2 conditions, OR;
 * libs/ra8_reflow/src/ra8_reflow_css.c@priv_ra8_reflow_css_ci_eq).
 * Vectors (N+1 = 3 for N=2):
 *  - V1: s="red", lit="red"     -> C1 F, C2 F -> false (compares; returns true).
 *  - V2: s=nullptr, lit="red"   -> C1 T        -> true  (returns false early).
 *  - V3: s="red", lit=nullptr   -> C1 F, C2 T  -> true  (returns false early).
 * V1 vs V2 flip only C1 (s) and flip the outcome: s influences independently.
 * V1 vs V3 flip only C2 (lit) and flip the outcome: lit influences independently.
 *
 * @pre No external precondition is required beyond the documented fixture setup.
 * @post The comparator reports inequality on either NULL operand.
 * @since 0.1.0
 * @details Exercises the css ci eq null guard mcdc path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 */
RA8_INTERNAL static void internal_test_css_ci_eq_null_guard_mcdc(void)
{
  TEST_BEGIN("ra8_reflow_css_ci_eq MC/DC: (s==null) || (lit==null)");
  const char* const red = "red";
  /* V1: neither NULL -> guard false -> real comparison, equal strings match. */
  TEST_ASSERT(priv_ra8_reflow_css_ci_eq(red, 3U, "red"));
  TEST_ASSERT(!priv_ra8_reflow_css_ci_eq(red, 3U, "blue"));
  /* V2: s NULL (C1 true) -> guard true -> false. */
  TEST_ASSERT(!priv_ra8_reflow_css_ci_eq(nullptr, 3U, "red"));
  /* V3: lit NULL (C2 true) -> guard true -> false. */
  TEST_ASSERT(!priv_ra8_reflow_css_ci_eq(red, 3U, nullptr));
  TEST_END("ra8_reflow_css_ci_eq MC/DC: (s==null) || (lit==null)");
}

/**
 * @test internal_test_svg_scanners_mcdc
 * @brief Drive the SVG path/element scan decisions through ::ra8_svg_render.
 *
 * @par MC/DC:
 * Two decisions, both reached through the public ::ra8_svg_render parser:
 *
 * 1. path-data separator skip
 *    `while ((*i < dlen) && (priv_ra8_svgp_ws((char)d[*i]) || (d[*i] == ',')))`
 *    (3 conditions; libs/ra8_reflow/src/ra8_reflow_svg_path.c@internal_parse_path).
 *    The `<path>` d-string below feeds every arm:
 *     - A=(*i<dlen): true while separators remain; false when the trailing
 *       spaces run to the end of the d-string (loop exit at end of buffer).
 *     - B=ws: a space separator between coordinates.
 *     - C=(d==','): a comma separator between coordinates.
 *     - B=F,C=F: a digit/command byte ends the skip while A is still true.
 *    A-independence: the trailing-space run (A: T->F, B held T) flips exit.
 *    B-independence: space vs digit at the same position (C held F).
 *    C-independence: comma vs digit at the same position (B held F).
 *
 * 2. element self-close test
 *    `const bool self_close = (close > i) && (s[close - 1U] == '/')`
 *    (2 conditions; libs/ra8_reflow/src/ra8_reflow_svg_doc.c@internal_group_open).
 *     - `<g>`                        -> close == i          -> C1 F (control).
 *     - `<g transform='translate(1,1)'>` -> close > i, s[close-1] != '/' -> C1 T, C2 F.
 *     - `<g transform='translate(1,1)'/>` -> close > i, s[close-1] == '/' -> C1 T, C2 T.
 *    Pair (row2,row3) flips only C2 and flips self_close: C2 independent.
 *    Pair (row1,row2) flips only C1 (C2 held F) and flips (close>i): C1 independent.
 *
 * @pre No external precondition is required beyond the documented fixture setup.
 * @post ra8_svg_render parses each crafted document without error.
 * @since 0.1.0
 * @details Exercises the svg scanners mcdc path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 */
RA8_INTERNAL static void internal_test_svg_scanners_mcdc(void)
{
  TEST_BEGIN("ra8_svg path/element scan MC/DC");
  /* Path d-string: leading spaces, comma separators, a space separator, and a
   * trailing space run so the separator-skip loop exits on *i >= dlen. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_reflow_render("<svg viewBox='0 0 20 20'>"
                                        "<path d='  M1,2 L 3,4 6 8   ' fill='#010203'/>"
                                        "</svg>"));
  /* Bare group: close == i (no attributes between the name and '>'). */
  TEST_ASSERT_EQ(k_ra8_ok, internal_reflow_render("<svg viewBox='0 0 20 20'><g></g></svg>"));
  /* Attributed group, not self-closing: close > i, char before '>' is not '/'. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_reflow_render("<svg viewBox='0 0 20 20'>"
                           "<g transform='translate(1,1)'><rect width='4' height='4'/></g>"
                           "</svg>"));
  /* Attributed group, self-closing: close > i, char before '>' is '/'. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_reflow_render("<svg viewBox='0 0 20 20'>"
                                        "<g transform='translate(1,1)'/>"
                                        "</svg>"));
  TEST_END("ra8_svg path/element scan MC/DC");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros call exit(1) on the first failure.
 * @since 0.1.0
 */
int32_t main(void)
{
  internal_test_css_ci_eq_null_guard_mcdc();
  internal_test_svg_scanners_mcdc();
  return 0;
}
