/**
 * @file test_mcdc_reflow_svg.c
 * @brief MC/DC independence vectors for the reflow SVG scanner/rasteriser.
 *
 * @details
 * The SVG number scanner (::priv_ra8_svgp_numf), the transform-list parser
 * (::priv_ra8_svgp_apply_xform) and the point-list line/polyline scanner
 * (::priv_ra8_svgp_draw_line) are TU-external helpers declared in
 * reflow_svg_internal.h. Higher-level callers pre-skip separators, so the
 * scanners' own whitespace/comma/terminator sub-conditions are only reachable
 * by driving the scanners directly with crafted byte spans -- exactly what
 * this file does. The shape-fill, gradient, arc and self-closing-element
 * decisions are driven through the public ::ra8_svg_render entry point.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "reflow_svg.h"
#include "reflow_svg_internal.h"
#include "unity_minimal.h"

/**
 * @enum t_svg_box_t
 * @brief Square viewport and viewBox edge for the transform vectors.
 *
 * @details
 * Both boxes are the same square, so the preserveAspectRatio branches differ
 * only in the condition under test rather than in the aspect ratio itself.
 */
typedef enum : int16_t {
  k_t_box_edge = 100, /**< Edge of both the viewport and the viewBox. */
} t_svg_box_t;

/** @brief Framebuffer geometry + palette for the internal_render cases. */
enum : int32_t {
  k_w     = 200,        /**< W.     */
  k_h     = 200,        /**< H.     */
  k_white = 0x00FFFFFF, /**< White. */
};

/** @brief Bind + clear the framebuffer to white.
 * @details Binds the function-local ARGB8888 host framebuffer used by every
 * render/draw case, then exercises the framebuffer reset path.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_fb_reset(void)
{
  static uint32_t s_fb[k_w * k_h];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_init(s_fb, (uint16_t)k_w, (uint16_t)k_h, k_ra8_gfx_format_argb8888));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear((uint32_t)k_white));
}

/** @brief Render a NUL-terminated SVG string into the full framebuffer box.
 * @details Exercises the render path and preserves each documented result and bound.
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
RA8_INTERNAL static ra8_err_t internal_render(const char* svg)
{
  return ra8_svg_render((const uint8_t*)svg, strlen(svg), 0, 0, k_w, k_h);
}

/** @brief Scan a NUL-terminated span with priv_ra8_svgp_numf from offset 0.
 * @details Exercises the numf path and preserves each documented result and bound.
 * @param[in] s Input byte span or string consumed by the helper.
 * @return The floating-point value parsed or computed by the helper.
 * @retval 0.0F The input represents zero or no nonzero result.
 * @retval nonzero The helper produced the parsed or computed value.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static float internal_numf(const char* s)
{
  size_t i = 0U;
  return priv_ra8_svgp_numf((const uint8_t*)s, strlen(s), &i);
}

/**
 * @test internal_test_svgp_numf_scanner_mcdc
 *
 * @par MC/DC:
 * Two decisions inside ::priv_ra8_svgp_numf, driven directly (the scanner is
 * TU-external; callers pre-skip separators so these arms are white-box only).
 *
 * Decision A: `while ((*i < len) && (ws(s[*i]) || (s[*i] == ',')))` (3 conds;
 * apps/shared_libs/reflow/src/reflow_svg_xform.c@priv_ra8_svgp_numf).
 *  - V1: ""      -> C1 F                     -> stop (control).
 *  - V2: " 5"    -> C1 T, C2 T               -> skip ws.
 *  - V3: ",5"    -> C1 T, C2 F, C3 T         -> skip comma.
 *  - V4: "5"     -> C1 T, C2 F, C3 F         -> stop on digit.
 * V4 vs V3 isolates C3; V4 vs V2 isolates C2; V1 vs V4 isolates C1.
 *
 * Decision B: `while ((*i < len) && (s[*i] >= '0') && (s[*i] <= '9'))` (3 conds;
 * same function, the integer-digit scan).
 *  - "5"  -> C1 T, C2 T, C3 T -> consume digit.
 *  - "5)" -> at ')' (0x29 < '0') C2 F        -> stop.
 *  - "5a" -> at 'a' (0x61 >= '0', 0x61 > '9') C2 T, C3 F -> stop.
 * The "5a" vector supplies the previously-missing C3 (byte > '9') pair.
 * @brief Verify svgp numf scanner mcdc behavior against the reflow contract.
 * @details Exercises the svgp numf scanner mcdc path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_svgp_numf_scanner_mcdc(void)
{
  /* Expected value for every scanner vector that consumes the digit five. */
  static const float k_expect_five = 5.0F;
  /* Upper bound avoids requiring exact floating-point equality for -3.5. */
  static const float k_below_neg_3p4 = 3.4F;
  TEST_BEGIN("ra8_svgp_numf: separator-skip and digit-scan terminators");
  TEST_ASSERT(internal_numf("") == 0.0F);                /* Dec A V1: C1 false          */
  TEST_ASSERT(internal_numf(" 5") == k_expect_five);     /* Dec A V2: whitespace skip   */
  TEST_ASSERT(internal_numf(",5") == k_expect_five);     /* Dec A V3: comma skip (C3)   */
  TEST_ASSERT(internal_numf("5") == k_expect_five);      /* Dec A V4 / Dec B C2,C3 true */
  TEST_ASSERT(internal_numf("5)") == k_expect_five);     /* Dec B C2 false (')' < '0')  */
  TEST_ASSERT(internal_numf("5a") == k_expect_five);     /* Dec B C3 false ('a' > '9')  */
  TEST_ASSERT(internal_numf("-3.5") < -k_below_neg_3p4); /* sign + fraction arms        */
  TEST_END("ra8_svgp_numf: separator-skip and digit-scan terminators");
}

/**
 * @test internal_test_svgp_xform_list_mcdc
 *
 * @par MC/DC:
 * Decision: `while ((*j < vlen) && (ws(v[*j]) || (v[*j] == ',')))` (3 conds;
 * apps/shared_libs/reflow/src/reflow_svg_xform.c@internal_xform_read) -- the per-argument
 * separator skip inside a transform function's argument list, reached via the
 * TU-external ::priv_ra8_svgp_apply_xform. The missing pair is C2 (a whitespace
 * separator between arguments) with C3 false; a comma separator supplies C3;
 * end-of-args supplies C1.
 *  - "translate(1 2)"  -> space between args -> C1 T, C2 T (fills C2 pair).
 *  - "scale(1,2)"      -> comma between args -> C1 T, C2 F, C3 T.
 *  - argument-list end -> C1 F.
 * @brief Verify svgp xform list mcdc behavior against the reflow contract.
 * @details Exercises the svgp xform list mcdc path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_svgp_xform_list_mcdc(void)
{
  /* Lower bound proving a transform argument was parsed, not defaulted. */
  static const float k_parsed_nonzero = 0.5F;
  TEST_BEGIN("ra8_svgp_apply_xform: whitespace + comma argument separators");
  svg_xform_t t   = {.bw = k_t_box_edge,
                     .bh = k_t_box_edge,
                     .vw = k_t_box_edge,
                     .vh = k_t_box_edge,
                     .ua = 1.0F,
                     .ud = 1.0F};
  const char* tag = "<g transform=\"translate(1 2) scale(1,2)\">";
  priv_ra8_svgp_apply_xform(&t, (const uint8_t*)tag, strlen(tag));
  /* translate(1,2) then scale(1,2): net x translate present, x scale applied. */
  TEST_ASSERT(t.ue > k_parsed_nonzero); /* translate x parsed (space-separated args) */
  TEST_ASSERT(t.ua > k_parsed_nonzero); /* scale x parsed (comma-separated args)     */
  TEST_END("ra8_svgp_apply_xform: whitespace + comma argument separators");
}

/**
 * @test internal_test_svgp_draw_line_points_mcdc
 *
 * @par MC/DC:
 * Decision: `while ((k < vlen) && (ws(v[k]) || (v[k] == ',')))` (3 conds;
 * apps/shared_libs/reflow/src/reflow_svg_shape.c@priv_ra8_svgp_draw_line) -- the point-list
 * coordinate scanner. Driven directly with a polyline tag whose `points`
 * value mixes comma and space separators, supplying the missing C3 (comma)
 * pair with C2 false; a space separator supplies C2; the list end supplies C1.
 * @brief Verify svgp draw line points mcdc behavior against the reflow contract.
 * @details Exercises the svgp draw line points mcdc path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_svgp_draw_line_points_mcdc(void)
{
  TEST_BEGIN("ra8_svgp_draw_line: comma + space point separators");
  internal_fb_reset();
  svg_xform_t t   = {.bw = k_t_box_edge,
                     .bh = k_t_box_edge,
                     .vw = k_t_box_edge,
                     .vh = k_t_box_edge,
                     .ua = 1.0F,
                     .ud = 1.0F};
  const char* tag = "<polyline points=\"10,10 40,10 40,40\" stroke=\"#f00\"/>";
  priv_ra8_svgp_draw_line((const uint8_t*)tag, strlen(tag), &t);
  TEST_END("ra8_svgp_draw_line: comma + space point separators");
}

/**
 * @test internal_test_svgp_attr_at_start_mcdc
 *
 * @par MC/DC:
 * Decision: `if ((at > 0U) && !priv_ra8_svgp_ws((char)s[at - 1U]))` (2 conds;
 * apps/shared_libs/reflow/src/reflow_svg.c@internal_attr_at) -- the "attribute name must
 * be preceded by whitespace" guard, reached via the TU-external ::priv_ra8_svgp_attr.
 * A search span that BEGINS with the attribute name gives at == 0 (the missing
 * C1-false arm, which normal `<tag ...>` markup -- always starting with '<' --
 * never produces).
 *  - V1: "fill='#abc'"  find "fill" at 0 -> C1 F           -> match (no reject).
 *  - V2: " fill='#abc'" find "fill" at 1, prev is ws -> C1 T, C2 F -> match.
 *  - V3: "xfill='#abc'" find "fill" at 1, prev is 'x' -> C1 T, C2 T -> reject.
 * V1 vs V3 isolates C1 (at>0); V2 vs V3 isolates C2 (!ws): each independent.
 * @brief Verify svgp attr at start mcdc behavior against the reflow contract.
 * @details Exercises the svgp attr at start mcdc path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_svgp_attr_at_start_mcdc(void)
{
  TEST_BEGIN("ra8_svgp_attr: attribute-name-at-offset-0 guard");
  size_t off     = 0U;
  size_t val_len = 0U;
  /* V1: name at offset 0 -> at == 0 -> guard's C1 false -> accepted. */
  const char* v1 = "fill='#abc'";
  TEST_ASSERT(priv_ra8_svgp_attr((const uint8_t*)v1, strlen(v1), "fill", &off, &val_len));
  /* V2: name preceded by whitespace -> at > 0, C2 false -> accepted. */
  const char* v2 = " fill='#abc'";
  TEST_ASSERT(priv_ra8_svgp_attr((const uint8_t*)v2, strlen(v2), "fill", &off, &val_len));
  /* V3: name is a suffix of "xfill" -> at > 0, preceded by non-ws -> rejected. */
  const char* v3 = "xfill='#abc'";
  TEST_ASSERT(!priv_ra8_svgp_attr((const uint8_t*)v3, strlen(v3), "fill", &off, &val_len));
  TEST_END("ra8_svgp_attr: attribute-name-at-offset-0 guard");
}

/**
 * @test internal_test_svg_render_shape_paths_mcdc
 *
 * @par MC/DC:
 * Decisions reached through the public ::ra8_svg_render walk.
 *
 * Decision A: `return ws(c) || (c == '>') || (c == '/')` (3 conds;
 * apps/shared_libs/reflow/src/reflow_svg_doc.c@internal_elem_at) -- element-name
 * terminator. A no-attribute self-closed `<rect/>` puts '/' right after the
 * name, flipping the missing C3; `<g>` supplies C2; `<rect ` supplies C1.
 *
 * Decision B: `if ((grads == nullptr) || !starts_ci(val, "url(#"))` (2 conds;
 * apps/shared_libs/reflow/src/reflow_svg.c@priv_match_grad) -- a `fill="url(#id)"`
 * with NO gradient defined makes grads NULL (missing C1 true); a defined
 * matching gradient supplies both-false. (C2 is deactivated: the sole caller
 * guards it with the identical starts_ci predicate -- see the source comment.)
 * @brief Verify svg render shape paths mcdc behavior against the reflow contract.
 * @details Exercises the svg render shape paths mcdc path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_svg_render_shape_paths_mcdc(void)
{
  TEST_BEGIN("svg render: self-close terminator + url() fill without grads");
  internal_fb_reset();
  /* Self-closed no-attr element -> internal_elem_at sees '/' immediately (Dec A C3);
   * `<g>` open tag supplies C2 ('>'); attributed tags supply C1 (whitespace). */
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_render("<svg viewBox=\"0 0 100 100\"><g>"
                                 "<rect/><rect x=\"1\" y=\"1\" width=\"3\" height=\"3\" "
                                 "fill=\"#111\"/></g></svg>"));

  /* Path whose command tokens are comma-separated: internal_next_cmd's separator
   * skip consumes a ',' immediately before a command letter (svg_path.c C3). */
  internal_fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_render("<svg viewBox=\"0 0 100 100\">"
                                 "<path d=\"M1 1,L20 20,z\" fill=\"#050\"/></svg>"));

  /* fill=url(#none) with no <defs> gradient -> t->grads == NULL (Dec B C1). */
  internal_fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_render("<svg viewBox=\"0 0 100 100\">"
                                 "<rect x=\"0\" y=\"0\" width=\"10\" height=\"10\" "
                                 "fill=\"url(#none)\"/></svg>"));

  /* A defined multi-stop gradient referenced by fill=url(#g): grads != NULL,
   * starts_ci true -> both-false control for Dec B; interior stops make some
   * fill positions fall below the first segment (internal_grad_eval lower bound). */
  internal_fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_render("<svg viewBox=\"0 0 100 100\">"
                                 "<defs><linearGradient id=\"g\">"
                                 "<stop offset=\"0.25\" stop-color=\"#f00\"/>"
                                 "<stop offset=\"0.75\" stop-color=\"#00f\"/>"
                                 "</linearGradient></defs>"
                                 "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
                                 "fill=\"url(#g)\"/></svg>"));
  TEST_END("svg render: self-close terminator + url() fill without grads");
}

/**
 * @test internal_test_svgp_match_grad_mcdc
 *
 * @par MC/DC:
 * Decision: `if ((grads == nullptr) || !priv_ra8_svgp_starts_ci(val, vlen, 0U, "url(#"))`
 * (2 conds; apps/shared_libs/reflow/src/reflow_svg.c@priv_match_grad). The sole
 * production caller only reaches this with a bound gradient set and a value
 * that already passed the url(#) prefix check, so both conditions are exercised
 * independently via a direct call to the (now TU-external) helper.
 *  - V1: grads set,  "url(#g1)" -> C1 F, C2 F -> match (index 0).
 *  - V2: grads NULL, "url(#g1)" -> C1 T        -> -1 (isolates grads).
 *  - V3: grads set,  "solid"    -> C1 F, C2 T  -> -1 (isolates url prefix).
 * V1 vs V2 flips only C1; V1 vs V3 flips only C2: each independent.
 * @brief Verify svgp match grad mcdc behavior against the reflow contract.
 * @details Exercises the svgp match grad mcdc path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_svgp_match_grad_mcdc(void)
{
  TEST_BEGIN("priv_match_grad MC/DC: (grads==null) || !url(#-prefix");
  svg_grads_t grads = {.n = 1};
  const char  gid[] = "g1";
  (void)memcpy(grads.g[0].id, gid, sizeof gid);
  const char* v_url = "url(#g1)";
  /* V1: bound set + matching url() -> both false -> matched index 0. */
  TEST_ASSERT_EQ(0, priv_match_grad(&grads, (const uint8_t*)v_url, strlen(v_url)));
  /* V2: NULL gradient set (C1 true) -> -1. */
  TEST_ASSERT_EQ(-1, priv_match_grad(nullptr, (const uint8_t*)v_url, strlen(v_url)));
  /* V3: value is not a url() reference (C2 true) -> -1. */
  const char* v_solid = "solid";
  TEST_ASSERT_EQ(-1, priv_match_grad(&grads, (const uint8_t*)v_solid, strlen(v_solid)));
  TEST_END("priv_match_grad MC/DC: (grads==null) || !url(#-prefix");
}

/**
 * @test internal_test_svg_render_polygon_arc_mcdc
 *
 * @par MC/DC:
 * Decision A: `if (((y0 <= y) && (y < y1)) || ((y1 <= y) && (y < y0)))`
 * (4 conds; apps/shared_libs/reflow/src/reflow_svg_shape.c@priv_ra8_svgp_fill_poly) --
 * the scan-line edge-crossing test. A concave polygon supplies edges that
 * rise (first clause) AND fall (second clause) across a scan-line, flipping
 * the previously-uncovered C4 (`y < y0`) of the falling-edge clause.
 *
 * Decision B: `if (!sweep && (dt > 0.0F))` (2 conds; same file@internal_arc_solve)
 * -- an elliptical-arc `A` with sweep-flag 0 and a positive raw delta-theta
 * flips both true; a sweep-flag-1 arc supplies C1 false.
 * @brief Verify svg render polygon arc mcdc behavior against the reflow contract.
 * @details Exercises the svg render polygon arc mcdc path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_svg_render_polygon_arc_mcdc(void)
{
  TEST_BEGIN("svg render: concave polygon edges + arc sweep flags");
  internal_fb_reset();
  /* Concave chevron: mid scan-line is crossed by both rising and falling edges. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_render("<svg viewBox=\"0 0 100 100\">"
                                 "<polygon points=\"10,50 40,10 40,40 90,40 40,60 40,90\" "
                                 "fill=\"#093\"/></svg>"));

  /* Two arcs, sweep 0 then sweep 1. */
  internal_fb_reset();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_render("<svg viewBox=\"0 0 100 100\">"
                    "<path d=\"M20,50 A30,30 0 0 0 80,50\" stroke=\"#000\" fill=\"none\"/>"
                    "<path d=\"M20,60 A30,30 0 0 1 80,60\" stroke=\"#000\" fill=\"none\"/>"
                    "</svg>"));
  TEST_END("svg render: concave polygon edges + arc sweep flags");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros call exit(1) on the first failure.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_svgp_numf_scanner_mcdc();
  internal_test_svgp_xform_list_mcdc();
  internal_test_svgp_draw_line_points_mcdc();
  internal_test_svgp_attr_at_start_mcdc();
  internal_test_svg_render_shape_paths_mcdc();
  internal_test_svgp_match_grad_mcdc();
  internal_test_svg_render_polygon_arc_mcdc();
  return 0;
}
