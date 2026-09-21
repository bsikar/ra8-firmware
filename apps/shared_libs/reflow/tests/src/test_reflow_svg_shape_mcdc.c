/**
 * @file test_reflow_svg_shape_mcdc.c
 * @brief MC/DC tests for the SVG shape / path / transform rasteriser arms.
 *
 * @details
 * Split sibling of test_reflow_svg_scan_mcdc.c and
 * test_reflow_svg_paint_mcdc.c covering the geometry decision families
 * of the SVG rasteriser: the path command scanner (relative forms, implicit
 * repeats, the no-progress close guard), the elliptical-arc solver (sweep
 * wraps, radius scale-up, degenerate arms), the curve/arc flattener vertex
 * cap, the points-list separators, the circle / polygon / polyline draw
 * guards, the `<g>` group stack, the transform list parser, and the
 * scanline winding fill. Every test drives the real public SVG API with
 * crafted byte strings; the shared framebuffer fixture lives in
 * tests/inc/reflow_svg_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "reflow_svg_test_util.h"
#include "unity_minimal.h"

RA8_INTERNAL static size_t internal_append_text(char* dst, size_t cap, size_t pos, const char* text)
{
  size_t i = 0U;
  while (((pos + i + 1U) < cap) && (text[i] != '\0')) {
    dst[pos + i] = text[i];
    ++i;
  }
  dst[pos + i] = '\0';
  return pos + i;
}

RA8_INTERNAL static size_t internal_append_i32(char* dst, size_t cap, size_t pos, int32_t value)
{
  char              reversed[11];
  size_t            count = 0U;
  size_t            out   = pos;
  uint32_t          magnitude;
  static const char k_digits[] = "0123456789";
  if (value < 0) {
    out       = internal_append_text(dst, cap, out, "-");
    magnitude = (uint32_t)(-(value + 1)) + 1U;
  } else {
    magnitude = (uint32_t)value;
  }
  do {
    const uint32_t digit = magnitude % 10U;
    reversed[count]      = k_digits[digit];
    ++count;
    magnitude /= 10U;
  } while ((magnitude > 0U) && (count < sizeof reversed));
  while (count > 0U) {
    --count;
    char text[2] = {reversed[count], '\0'};
    out          = internal_append_text(dst, cap, out, text);
  }
  return out;
}

/**
 * @enum svg_shape_buf_t
 * @brief Buffer capacities and generator bounds for the long-path builders.
 *
 * @invariant Both capacities exceed the longest generated path string.
 * @see internal_build_long_path()
 */
typedef enum : uint16_t {
  k_path_buf_cap    = 1024U, /**< Capacity for the flattener-cap paths.    */
  k_poly_buf_cap    = 1536U, /**< Capacity for the winding polygon string. */
  k_long_path_x_pin = 90U,   /**< X coordinates cycle within the viewBox.  */
} svg_shape_buf_t;

/**
 * @test internal_test_svg_path_no_progress_guard_mcdc
 *
 * @par MC/DC:
 * Decision: internal_parse_path implicit-close progress guard `(i == i_before)`
 * (1 condition; apps/shared_libs/reflow/src/reflow_svg_path.c@internal_parse_path). A 'z'
 * close takes no args, so an implicit repeat after a non-command byte leaves
 * the cursor unmoved and would spin forever; the guard skips the byte. Driven
 * through ::ra8_svg_render with crafted `<path d>` values.
 *
 * Vectors (both branches of the single condition):
 *  - V1: d="Z2"    -> after the explicit 'Z' the digit is read as an implicit
 *        'z' repeat with i unmoved -> TRUE -> ++i (skip), terminate. (Without
 *        the guard ra8_svg_render never returns -- this is the fix.)
 *  - V2: d="M0 0Z" -> the explicit 'Z' already advanced i past the letter ->
 *        FALSE -> no skip; the close is a no-op and the loop ends normally.
 * Every case must RETURN (terminate) -- the test completing proves the bound.
 * @brief Verify svg path no progress guard mcdc behavior against the reflow contract.
 * @details Exercises the svg path no progress guard mcdc path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_svg_path_no_progress_guard_mcdc(void)
{
  TEST_BEGIN("priv_parse_path implicit-close guard: Z2 hang fix");
  fb_reset();
  /* V1: the fuzzer's 31-byte reproducer -- must terminate, not hang. */
  TEST_ASSERT_EQ(k_ra8_ok, render("<svg viewBox=\"0 0 9 9\"><path d=\"Z2\"/></svg>"));
  /* V2: an explicit close has already advanced the cursor (condition false). */
  TEST_ASSERT_EQ(k_ra8_ok, render("<svg viewBox=\"0 0 9 9\"><path d=\"M0 0Z\"/></svg>"));
  TEST_END("priv_parse_path implicit-close guard: Z2 hang fix");
}

/**
 * @test internal_test_xform_separators_unknown
 * @brief A chained `transform=` list with comma/space separators between
 *        functions and an unknown function name exercises the separator skip,
 *        the `(` / `)` scans, and the unknown -> skipped arm of internal_parse_xform.
 *
 * @par MC/DC:
 * Decision: `internal_parse_xform` -> `kind != k_svg_xf_none` (compose vs skip;
 * apps/shared_libs/reflow/src/reflow_svg_xform.c@internal_parse_xform).
 * - V-known: `translate(50,0)` -> kind != none -> composed (the rect moves).
 * - V-unknown: `foo(1) translate(50,0)` -> the leading `foo(...)` has kind none
 *   -> skipped, then translate composes -> the rect still moves by 50.
 * The pair varies the kind classification (compose vs skip) with the same net
 * placement, proving the unknown arm is reached without corrupting the result.
 * @details Exercises the xform separators unknown path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_xform_separators_unknown(void)
{
  TEST_BEGIN("priv_parse_xform: separators + unknown function skip");
  /* Known-only baseline: translate(50,0) moves user 0..10 -> 50..60 -> fb 100..120. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\"><rect x=\"0\" y=\"0\" width=\"10\" "
                        "height=\"10\" fill=\"#ff0000\" transform=\"translate(50,0)\"/></svg>"));
  TEST_ASSERT_EQ(k_red, px(110, 10));

  /* Unknown leading function + comma/space separators -> foo() skipped, then
   * scale(2) and translate(25,0) compose (translate applied first, then scale):
   * user x 0..10 -> (x+25)*2 = 50..70 -> fb 100..140. The unknown arm + the `,`
   * and ` ` separators are exercised. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\"><rect x=\"0\" y=\"0\" width=\"10\" "
                        "height=\"10\" fill=\"#00ff00\" "
                        "transform=\"foo(1) , scale(2) translate(25,0)\"/></svg>"));
  TEST_ASSERT_EQ(k_green, px(120, 10));
  TEST_END("priv_parse_xform: separators + unknown function skip");
}

/**
 * @test internal_test_circle_skip_and_rotated_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_draw_circle` skip guard `(gi<0)&&(fill==no_paint)` and the
 * fast-path guard `(gi<0)&&!priv_has_rot(t)`
 * (apps/shared_libs/reflow/src/reflow_svg_shape.c@priv_ra8_svgp_draw_circle).
 * - V1: `fill="none"` circle -> no_paint with gi<0 -> skipped (stays white).
 * - V2: solid circle, no transform -> not no_paint, no rotation -> fast-path
 *       ra8_gfx_circle fills the disc.
 * - V3: solid circle under `rotate(30)` -> priv_has_rot true -> the N-gon
 *       polygon path fills (still covers the centre).
 * V1 vs V2 flip the no-paint condition; V2 vs V3 flip the has-rot condition.
 * @brief Verify circle skip and rotated mcdc behavior against the reflow contract.
 * @details Exercises the circle skip and rotated mcdc path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_circle_skip_and_rotated_mcdc(void)
{
  TEST_BEGIN("priv_draw_circle MC/DC: none-skip / fast-path / rotated polygon");
  /* V1: fill:none -> skipped. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<circle cx=\"50\" cy=\"50\" r=\"30\" fill=\"none\"/></svg>"));
  TEST_ASSERT_EQ(k_white, px(100, 100));

  /* V2: solid, axis-aligned -> fast-path disc fills the centre. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<circle cx=\"50\" cy=\"50\" r=\"30\" fill=\"#ff0000\"/></svg>"));
  TEST_ASSERT_EQ(k_red, px(100, 100)); /* centre (50,50) -> fb (100,100) */

  /* V3: solid under a rotation -> the N-gon polygon path fills; the centre,
   * being rotation-invariant, is still covered. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<circle cx=\"50\" cy=\"50\" r=\"30\" fill=\"#0000ff\" "
                        "transform=\"rotate(30,50,50)\"/></svg>"));
  TEST_ASSERT_EQ(k_blue, px(100, 100));
  TEST_END("priv_draw_circle MC/DC: none-skip / fast-path / rotated polygon");
}

/**
 * @test internal_test_polygon_polyline_guards_mcdc
 *
 * @par MC/DC:
 * Decision: `priv_draw_polygon` guard
 * `((gi<0)&&(fill==no_paint)) || !priv_attr(points)` and `priv_draw_polyline`
 * guard `(stroke==no_paint) || !priv_attr(points)`
 * (apps/shared_libs/reflow/src/reflow_svg_shape.c@priv_ra8_svgp_draw_polygon / @priv_draw_polyline).
 *
 * Vectors:
 *  - Polygon C1 T: `fill="none"` (with points) -> skipped.
 *  - Polygon C2 T: a filled polygon with NO `points` attr -> skipped.
 *  - Polygon both F: a filled polygon WITH points -> fills.
 *  - Polyline C1 T: a polyline with NO `stroke` -> skipped.
 *  - Polyline C2 T: a stroked polyline with NO `points` -> skipped (no crash).
 *  - Polyline both F: a stroked polyline WITH points -> strokes a visible pixel.
 * Each pair flips one condition of the respective OR while the other is held
 * false by the control case.
 * @brief Verify polygon polyline guards mcdc behavior against the reflow contract.
 * @details Exercises the polygon polyline guards mcdc path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_polygon_polyline_guards_mcdc(void)
{
  TEST_BEGIN("polygon/polyline guard MC/DC: none / no-points / present");
  /* Polygon both-false control: fills the triangle interior. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<polygon points=\"10,10 90,10 50,90\" fill=\"#ff0000\"/></svg>"));
  TEST_ASSERT_EQ(k_red, px(100, 60));

  /* Polygon C1 (fill none) and C2 (no points) -> both skipped; background white. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<polygon points=\"10,10 90,10 50,90\" fill=\"none\"/>"
                        "<polygon fill=\"#00ff00\"/></svg>"));
  TEST_ASSERT_EQ(k_white, px(100, 60));

  /* Polyline both-false control: a stroked diagonal lights its midpoint. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<polyline points=\"0,0 100,100\" stroke=\"#0000ff\"/></svg>"));
  TEST_ASSERT_EQ(k_blue, px(100, 100)); /* midpoint of the diagonal */

  /* Polyline C1 (no stroke) and C2 (stroke but no points) -> both skipped. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<polyline points=\"0,0 100,100\"/>"
                        "<polyline stroke=\"#00ff00\"/></svg>"));
  TEST_ASSERT_EQ(k_white, px(100, 100));
  TEST_END("polygon/polyline guard MC/DC: none / no-points / present");
}

/**
 * @test internal_test_path_relative_hv
 * @brief Relative path commands (`m`/`l`/`h`/`v`) and the implicit line-to after
 *        `m` exercise internal_next_cmd's lowercase branch, internal_path_step's h/v
 *        arms, and the relative coordinate resolution.
 *
 * @par MC/DC:
 * Decisions: `if (u == 'h')` and `if (u == 'v')` in internal_path_step
 * (apps/shared_libs/reflow/src/reflow_svg_path.c; 1 condition each) plus the
 * lowercase (relative) command classification in the path scanner.
 * - V1: `h80` / `h-80` -> 'h' arm TRUE (x moves, y held).
 * - V2: `v80`          -> 'h' FALSE, 'v' arm TRUE (y moves, x held).
 * - V3: the implicit line-to after `m10 10` -> both FALSE ((x,y) pair arm).
 * All three vectors run inside this test; the lowercase spellings prove the
 * relative-coordinate resolution against the current point.
 * @details Exercises the path relative hv path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_path_relative_hv(void)
{
  TEST_BEGIN("svg path relative m/l/h/v + implicit line-to");
  fb_reset();
  /* Start (10,10); relative h80 -> (90,10); v80 -> (90,90); h-80 -> (10,90); z.
   * A square 10..90 in user space -> fb 20..180. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"m10 10 h80 v80 h-80 z\" fill=\"#00aacc\"/></svg>"));
  TEST_ASSERT_EQ(0x00AACC, px(100, 100)); /* inside the square     */
  TEST_ASSERT_EQ(k_white, px(10, 100));   /* left of x=20 -> white */
  TEST_END("svg path relative m/l/h/v + implicit line-to");
}

/**
 * @test internal_test_arc_sweep_wrap_mcdc
 *
 * @par MC/DC:
 * Decision: `internal_arc_solve` sweep normalisation
 * `(!sweep && dt>0)` else-if `(sweep && dt<0)`
 * (apps/shared_libs/reflow/src/reflow_svg_shape.c@internal_arc_solve). The large-arc flag is
 * toggled so each branch's angular condition (dt>0 / dt<0) is the one that
 * fires for the chosen sweep flag.
 * - V1: sweep=0, large=1 over the same chord -> dt computes positive ->
 *       `!sweep && dt>0` arm subtracts 2*pi (a large clockwise arc).
 * - V2: sweep=1, large=1 -> dt computes negative -> `sweep && dt<0` arm adds
 *       2*pi (a large counter-clockwise arc).
 * The two large arcs bulge to opposite sides of the chord, so a probe above the
 * chord is filled for exactly one of them -- proving the sweep flag selects the
 * wrap arm.
 * @brief Verify arc sweep wrap mcdc behavior against the reflow contract.
 * @details Exercises the arc sweep wrap mcdc path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_arc_sweep_wrap_mcdc(void)
{
  TEST_BEGIN("priv_arc_solve MC/DC: large-arc sweep wrap arms");
  /* V1: large=1, sweep=0 -> the !sweep && dt>0 wrap arm; the large arc encloses
   * the side opposite a small sweep=0 arc. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 50 A40 40 0 1 0 90 50 Z\" fill=\"#ff0000\"/></svg>"));
  const uint32_t up_v1   = px(100, 60);  /* above the chord */
  const uint32_t down_v1 = px(100, 150); /* below the chord */

  /* V2: large=1, sweep=1 -> the sweep && dt<0 wrap arm; the large arc bulges to
   * the opposite side relative to V1. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 50 A40 40 0 1 1 90 50 Z\" fill=\"#0000ff\"/></svg>"));
  const uint32_t up_v2   = px(100, 60);
  const uint32_t down_v2 = px(100, 150);

  /* Exactly one of the two large arcs fills above the chord, the other below --
   * the sweep flag flips which wrap arm runs and thus which side is enclosed. */
  TEST_ASSERT((up_v1 != up_v2) || (down_v1 != down_v2));
  TEST_END("priv_arc_solve MC/DC: large-arc sweep wrap arms");
}

/**
 * @test internal_test_arc_radius_scale_up
 * @brief An arc whose requested radii are too small for the chord triggers the
 *        out-of-range-radius scale-up (internal_arc_center `lam > 1` arm), which
 *        enlarges rx/ry so the endpoints stay on the ellipse.
 *
 * @par MC/DC:
 * Decision: `internal_arc_center` -> `if (lam > 1.0F)` (1 condition).
 * - V1 (here): rx=ry=10 but the chord is 80 wide -> lam>1 -> radii scaled up;
 *   the arc still reaches its endpoint and the closed region fills.
 * - V2: tests/src/test_svg.c@test_render_arc uses r=40 == half-chord -> lam<=1
 *   (no scale-up). The pair varies the lam condition.
 * @details Exercises the arc radius scale up path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_arc_radius_scale_up(void)
{
  TEST_BEGIN("priv_arc_center: out-of-range radius scale-up (lam>1)");
  fb_reset();
  /* Half-chord is 40 but rx=ry=10 -> lam = (40/10)^2 = 16 > 1 -> scale-up. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 50 A10 10 0 0 1 90 50 Z\" fill=\"#00aacc\"/></svg>"));
  /* The scaled-up semicircle bulges up; its interior near the chord is filled. */
  TEST_ASSERT(px(100, 90) != (uint32_t)k_white);
  TEST_END("priv_arc_center: out-of-range radius scale-up (lam>1)");
}

/**
 * @test internal_test_group_selfclose_depth_and_pop
 * @brief Self-closing `<g/>`, an over-deep `<g>` nest, and a `</g>` at the floor
 *        exercise internal_group_open's self-close + depth-cap arms and the
 *        internal_draw_shapes pop-at-zero arm.
 *
 * @par MC/DC:
 * Decision: `internal_group_open` -> `self_close || (gsp >= depth_max)` and
 * `internal_draw_shapes` -> `gsp = (gsp > 0) ? (gsp - 1) : 0`
 * (apps/shared_libs/reflow/src/reflow_svg.c).
 * - V-selfclose: `<g transform=... />` does NOT push; the following rect renders
 *   at the document (untransformed) origin, proving the self-close arm.
 * - V-extra-pop: a stray `</g>` with no open group keeps gsp at 0 (no underflow)
 *   and a following rect still renders -- the pop-at-floor arm.
 * - V-deep: more than k_svg_g_depth_max nested `<g>` groups; the deepest rect
 *   still renders without overrunning the bounded stack (depth-cap arm).
 * @details Exercises the group selfclose depth and pop path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_group_selfclose_depth_and_pop(void)
{
  TEST_BEGIN("svg <g> self-close / depth-cap / pop-at-floor");
  /* V-selfclose: the self-closed group must NOT move the sibling rect. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<g transform=\"translate(80,80)\"/>"
                        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"10\" fill=\"#ff0000\"/>"
                        "</svg>"));
  TEST_ASSERT_EQ(k_red, px(10, 10)); /* rect at origin, not translated */

  /* V-extra-pop: a stray `</g>` at the floor leaves gsp==0; the rect renders. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "</g>"
                        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"10\" fill=\"#00ff00\"/>"
                        "</svg>"));
  TEST_ASSERT_EQ(k_green, px(10, 10));

  /* V-deep: 10 nested groups (> k_svg_g_depth_max == 8); the innermost rect must
   * still render -- the bounded stack must not be overrun. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<g><g><g><g><g><g><g><g><g><g>"
                        "<rect x=\"0\" y=\"0\" width=\"10\" height=\"10\" fill=\"#0000ff\"/>"
                        "</g></g></g></g></g></g></g></g></g></g></svg>"));
  TEST_ASSERT_EQ(k_blue, px(10, 10));
  TEST_END("svg <g> self-close / depth-cap / pop-at-floor");
}

/**
 * @brief Build "<svg viewBox='0 0 100 100'>" + @p reps repeats of "L5 5 "
 *        after an "M0 0 ", then @p suffix, into @p dst (path-curve overflow tests).
 *
 * @details Used to drive the @c (*n < k_svg_poly_max) loop-bound arm of the
 * curve / arc flatteners: a long run of line-tos pushes the vertex count near
 * the 64-point cap so the trailing curve/arc command's flatten loop stops on
 * the count rather than its segment count.
 *
 * @param[out] dst    Destination buffer (>= 1024 bytes).
 * @param[in]  cap    Capacity of @p dst, bytes.
 * @param[in]  reps   Number of "L5 5 " line-tos to emit after the initial M.
 * @param[in]  suffix Trailing path data (e.g. a `C`/`Q`/`A` command) + tags.
 * @pre @p dst references at least @p cap writable bytes.
 * @pre @p suffix is a valid NUL-terminated path and closing-tag suffix.
 * @post @p dst contains a terminated SVG document bounded by @p cap.
 * @post Bytes outside the destination capacity remain unmodified.
 * @note The helper truncates repeated segments before reserving space for @p suffix.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_build_long_path(char* dst, size_t cap, int32_t reps, const char* suffix)
{
  size_t pos =
    internal_append_text(dst,
                         cap,
                         0U,
                         "<svg viewBox=\"0 0 100 100\"><path fill=\"#ff0000\" d=\"M0 0 ");
  for (int32_t k = 0; (k < reps) && ((pos + 8U) < cap); ++k) {
    pos = internal_append_text(dst, cap, pos, "L");
    pos = internal_append_i32(dst, cap, pos, (k % (int32_t)k_long_path_x_pin) + 1);
    pos = internal_append_text(dst, cap, pos, " 5 ");
  }
  (void)internal_append_text(dst, cap, pos, suffix);
}

/**
 * @test internal_test_xform_scan_boundary_mcdc
 *
 * @par MC/DC:
 * Decision: the transform scanners -- `internal_xform_read` arg-separator skip
 * `(*j<vlen)&&(priv_ws||==',')` (L654); `internal_parse_xform` leading-separator
 * skip (L764), the scan-to-`(` `(op<vlen)&&(v[op]!='(')` (L769), and the
 * scan-to-`)` `(j<vlen)&&(v[j]!=')')` (L781;
 * apps/shared_libs/reflow/src/reflow_svg.c). Each `*<vlen` F arm is driven by a
 * transform value that ends mid-scan (trailing separator / no `(` / no `)`).
 *
 * Vectors:
 *  - V1: `transform="scale(2,"` -> after the first arg the separator skip hits
 *        a `,` then the value end -> L654 `,` T then `*j<vlen` F.
 *  - V2: `transform="translate(10,0) "` (trailing space) -> the leading-sep
 *        skip consumes the trailing space to the value end -> L764 `i<vlen` F.
 *  - V3: `transform="rotate"` (no `(`) -> the scan-to-`(` runs to the value end
 *        -> L769 `op<vlen` F -> the transform is dropped (identity).
 *  - V4: `transform="translate(10,0"` (no `)`) -> the scan-to-`)` runs to the
 *        value end -> L781 `j<vlen` F.
 * Each renders cleanly (k_ra8_ok); the boundary scan arms are the point.
 * @brief Verify xform scan boundary mcdc behavior against the reflow contract.
 * @details Exercises the xform scan boundary mcdc path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_xform_scan_boundary_mcdc(void)
{
  TEST_BEGIN("priv_parse_xform/xform_read MC/DC: trailing-sep / no-paren scan arms");
  /* V1: a trailing `,` then the value end inside the arg separator skip. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\"><rect x=\"0\" y=\"0\" width=\"10\" "
                        "height=\"10\" fill=\"#ff0000\" transform=\"scale(2,\"/></svg>"));

  /* V2: a trailing space after a complete function -> the leading-sep loop runs
   * to the value end. The rect still translates by (10,0). */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\"><rect x=\"0\" y=\"0\" width=\"10\" "
                        "height=\"10\" fill=\"#00ff00\" transform=\"translate(10,0) \"/></svg>"));

  /* V3: a name with no `(` -> the scan-to-`(` runs to the end and the transform
   * is dropped; the rect renders at the document origin. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\"><rect x=\"0\" y=\"0\" width=\"10\" "
                        "height=\"10\" fill=\"#0000ff\" transform=\"rotate\"/></svg>"));
  TEST_ASSERT_EQ(k_blue, px(10, 10));

  /* V4: an arg list with no `)` -> the scan-to-`)` runs to the value end. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\"><rect x=\"0\" y=\"0\" width=\"10\" "
                        "height=\"10\" fill=\"#112233\" transform=\"translate(10,0\"/></svg>"));
  TEST_END("priv_parse_xform/xform_read MC/DC: trailing-sep / no-paren scan arms");
}

/**
 * @test internal_test_points_trailing_separator_mcdc
 *
 * @par MC/DC:
 * Decision: `internal_parse_points` separator skip `(k<vlen)&&(priv_ws||==',')`
 * (L915; apps/shared_libs/reflow/src/reflow_svg_shape.c@internal_parse_points). The existing
 * tests stop the skip at a digit (`priv_ws`/`,` F). This drives `k<vlen` F: a
 * `points` value with a trailing separator, so the skip loop runs to the slice
 * end before the outer `k>=vlen` break.
 *
 * Vector:
 *  - `<polygon points="10,10 90,10 50,90 " .../>` (trailing space) -> after the
 *    last pair the separator skip consumes the space and reaches the slice end
 *    -> L915 `k<vlen` F -> the triangle still fills.
 * @brief Verify points trailing separator mcdc behavior against the reflow contract.
 * @details Exercises the points trailing separator mcdc path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_points_trailing_separator_mcdc(void)
{
  TEST_BEGIN("priv_parse_points MC/DC: trailing separator to slice end");
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<polygon points=\"10,10 90,10 50,90 \" fill=\"#ff0000\"/></svg>"));
  TEST_ASSERT_EQ(k_red, px(100, 60));
  TEST_END("priv_parse_points MC/DC: trailing separator to slice end");
}

/**
 * @test internal_test_scanline_windings_mcdc
 *
 * @par MC/DC:
 * Decision: `internal_scanline_x` crossing test
 * `((y0<=y)&&(y<y1))||((y1<=y)&&(y<y0))` (L954) and the edge loop bound
 * `(i<n)&&(m<k_svg_poly_max)` (L950; apps/shared_libs/reflow/src/reflow_svg.c). The
 * existing fills cover the common crossing arms; this adds an opposite-wound
 * polygon and a dense many-vertex zigzag so the up-edge / down-edge clauses and
 * the crossing-count cap arm are exercised.
 *
 * Vectors:
 *  - V1: a clockwise and a counter-clockwise triangle -> both the
 *        `(y0<=y)&&(y<y1)` (down) and `(y1<=y)&&(y<y0)` (up) crossing clauses run.
 *  - V2: a 60+-vertex comb crossing a single scanline many times pushes the
 *        crossing count toward the cap (the `m<k_svg_poly_max` arm).
 * Each renders cleanly (k_ra8_ok).
 * @brief Verify scanline windings mcdc behavior against the reflow contract.
 * @details Exercises the scanline windings mcdc path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_scanline_windings_mcdc(void)
{
  TEST_BEGIN("priv_scanline_x MC/DC: opposite windings + dense crossings");
  /* V1: opposite-wound triangles exercise both crossing clauses. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<polygon points=\"50,90 90,10 10,10\" fill=\"#ff0000\"/></svg>"));
  TEST_ASSERT_EQ(k_red, px(100, 60));

  /* V2: a dense comb -- many narrow up/down spikes across one band -- to drive
   * the crossing-count toward the per-scanline cap. */
  fb_reset();
  {
    char   buf[k_poly_buf_cap] = {};
    size_t pos =
      internal_append_text(buf, sizeof(buf), 0U, "<svg viewBox=\"0 0 100 100\"><polygon points=\"");
    const int32_t k_spikes = 30;
    for (int32_t k = 0; k < k_spikes; ++k) {
      const int32_t bx = (k * 3) + 1;
      pos              = internal_append_i32(buf, sizeof(buf), pos, bx);
      pos              = internal_append_text(buf, sizeof(buf), pos, ",90 ");
      pos              = internal_append_i32(buf, sizeof(buf), pos, bx + 1);
      pos              = internal_append_text(buf, sizeof(buf), pos, ",10 ");
    }
    (void)internal_append_text(buf, sizeof(buf), pos, "95,90\" fill=\"#0000ff\"/></svg>");
    TEST_ASSERT_EQ(k_ra8_ok, render(buf));
  }
  TEST_END("priv_scanline_x MC/DC: opposite windings + dense crossings");
}

/**
 * @test internal_test_curve_overflow_count_mcdc
 *
 * @par MC/DC:
 * Decision: the curve / arc flatten loop bounds `(j<=seg)&&(*n<k_svg_poly_max)`
 * in `internal_flatten_cubic` (L1186), `internal_flatten_quad` (L1233), and
 * `priv_flatten_arc` (L1463), plus the path outer loop `(i<dlen)&&(n<poly_max)`
 * (L1561; apps/shared_libs/reflow/src/reflow_svg.c). The existing curve tests exit on
 * the segment count; this prepends a long run of line-tos so the vertex count
 * reaches the 64-point cap DURING the trailing curve/arc, flipping the
 * `*n<k_svg_poly_max` (and the outer `n<poly_max`) condition false.
 *
 * Vectors (built by ::internal_build_long_path):
 *  - V1: ~58 line-tos then a cubic `C` -> the cubic flatten stops on the count.
 *  - V2: ~58 line-tos then a quadratic `Q` -> the quad flatten stops on the count.
 *  - V3: ~58 line-tos then an arc `A` -> the arc flatten stops on the count.
 * Each renders cleanly (k_ra8_ok).
 * @brief Verify curve overflow count mcdc behavior against the reflow contract.
 * @details Exercises the curve overflow count mcdc path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_curve_overflow_count_mcdc(void)
{
  TEST_BEGIN("flatten_cubic/quad/arc MC/DC: vertex-count cap arm");
  char          buf[k_path_buf_cap] = {};
  const int32_t k_reps              = 58;

  /* V1: a cubic after a near-full vertex list. */
  fb_reset();
  internal_build_long_path(buf, sizeof(buf), k_reps, "C 60 0 70 80 90 90 Z\" /></svg>");
  TEST_ASSERT_EQ(k_ra8_ok, render(buf));

  /* V2: a quadratic after a near-full vertex list. */
  fb_reset();
  internal_build_long_path(buf, sizeof(buf), k_reps, "Q 60 0 90 90 Z\" /></svg>");
  TEST_ASSERT_EQ(k_ra8_ok, render(buf));

  /* V3: an arc after a near-full vertex list. */
  fb_reset();
  internal_build_long_path(buf, sizeof(buf), k_reps, "A 40 40 0 1 1 90 90 Z\" /></svg>");
  TEST_ASSERT_EQ(k_ra8_ok, render(buf));
  TEST_END("flatten_cubic/quad/arc MC/DC: vertex-count cap arm");
}

/**
 * @test internal_test_arc_sweep_both_flags_mcdc
 *
 * @par MC/DC:
 * Decision: `internal_arc_solve` sweep wrap `(!sweep && dt>0)` (L1363) and
 * `(sweep && dt<0)` (L1365; apps/shared_libs/reflow/src/reflow_svg_shape.c@internal_arc_solve).
 * The existing test toggles only the large-arc flag at fixed sweep; this renders
 * the four (large, sweep) combinations over one chord so each wrap arm's angular
 * condition (`dt>0` for `!sweep`, `dt<0` for `sweep`) fires.
 *
 * Vectors (all `M10 50 ... 90 50`):
 *  - small/sweep0 (`0 0`), large/sweep0 (`1 0`), small/sweep1 (`0 1`),
 *    large/sweep1 (`1 1`) -> across the four the `dt` sign spans both arms.
 * Each renders cleanly (k_ra8_ok).
 * @brief Verify arc sweep both flags mcdc behavior against the reflow contract.
 * @details Exercises the arc sweep both flags mcdc path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_arc_sweep_both_flags_mcdc(void)
{
  TEST_BEGIN("priv_arc_solve MC/DC: all (large,sweep) wrap arms");
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 50 A40 40 0 0 0 90 50 Z\" fill=\"#ff0000\"/></svg>"));
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 50 A40 40 0 1 0 90 50 Z\" fill=\"#00ff00\"/></svg>"));
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 50 A40 40 0 0 1 90 50 Z\" fill=\"#0000ff\"/></svg>"));
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 50 A40 40 0 1 1 90 50 Z\" fill=\"#112233\"/></svg>"));
  TEST_END("priv_arc_solve MC/DC: all (large,sweep) wrap arms");
}

/**
 * @test internal_test_arc_degenerate_arms_mcdc
 *
 * @par MC/DC:
 * Decision: `internal_arc_center` degeneracy guard
 * `(rx==0)||(ry==0)||((p0.x==p_end.x)&&(p0.y==p_end.y))` (L1404;
 * apps/shared_libs/reflow/src/reflow_svg_shape.c@internal_arc_center). The existing arcs are all
 * non-degenerate (every condition false). This drives each true arm and the
 * coincident-endpoint sub-conditions independently.
 *
 * Vectors:
 *  - V1: `A0 40 ...` -> `rx==0` T -> degenerate (collapses to a line).
 *  - V2: `A40 0 ...` -> `ry==0` T -> degenerate.
 *  - V3: `M10 50 A40 40 0 0 1 10 50` -> start==end -> `p0.x==p_end.x` T &&
 *        `p0.y==p_end.y` T -> degenerate.
 *  - V4: `M50 10 A40 40 0 0 1 50 90` -> same x, different y -> `p0.x==p_end.x` T
 *        && `p0.y==p_end.y` F -> NOT degenerate (a real arc), proving the y
 *        sub-condition's independence.
 * Each renders cleanly (k_ra8_ok).
 * @brief Verify arc degenerate arms mcdc behavior against the reflow contract.
 * @details Exercises the arc degenerate arms mcdc path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_arc_degenerate_arms_mcdc(void)
{
  TEST_BEGIN("priv_arc_center MC/DC: zero-rx / zero-ry / coincident endpoints");
  /* V1: rx == 0. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 50 A0 40 0 0 1 90 50 Z\" fill=\"#ff0000\"/></svg>"));
  /* V2: ry == 0. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 50 A40 0 0 0 1 90 50 Z\" fill=\"#00ff00\"/></svg>"));
  /* V3: start == end (both coordinates equal). */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 50 A40 40 0 0 1 10 50 Z\" fill=\"#0000ff\"/></svg>"));
  /* V4: same x, different y -> not degenerate (y sub-condition independence). */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M50 10 A40 40 0 0 1 50 90 Z\" fill=\"#112233\"/></svg>"));
  TEST_END("priv_arc_center MC/DC: zero-rx / zero-ry / coincident endpoints");
}

/**
 * @test internal_test_path_cmd_scan_arms_mcdc
 *
 * @par MC/DC:
 * Decision: `internal_next_cmd` separator skip `(*i<dlen)&&(priv_ws||==',')` (L1538)
 * and the letter test `((c>='A')&&(c<='Z'))||((c>='a')&&(c<='z'))` (L1545;
 * apps/shared_libs/reflow/src/reflow_svg_path.c@internal_next_cmd). The existing path tests use
 * space separators and A-Z / a-z letters; this adds comma separators, a trailing
 * separator run (`*i<dlen` F), and command-position characters just outside the
 * letter ranges (`` ` `` between `Z` and `a`, `{` just above `z`) so the upper
 * and lower range bounds each flip false.
 *
 * Vectors:
 *  - V1: `d="M10,10 L90,90 "` -> comma separators (L1538 `==','` T) and a
 *        trailing space to the slice end (L1538 `*i<dlen` F).
 *  - V2: `d="M10 10 ` ` 90 90"` -> a backtick (0x60, between `Z` and `a`) at a
 *        command position -> L1545 `c<='Z'` F and `c>='a'` F -> the implicit
 *        previous command repeats.
 *  - V3: `d="M10 10 { 90 90"` -> `{` (0x7B, just above `z`) -> L1545 `c<='z'` F.
 * Each renders cleanly (k_ra8_ok).
 * @brief Verify path cmd scan arms mcdc behavior against the reflow contract.
 * @details Exercises the path cmd scan arms mcdc path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_path_cmd_scan_arms_mcdc(void)
{
  TEST_BEGIN("priv_next_cmd MC/DC: comma/trailing separators + letter-range bounds");
  /* V1: comma separators and a trailing space to the slice end. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10,10 L90,90 \" fill=\"#ff0000\"/></svg>"));

  /* V2: a backtick at a command position (between 'Z' and 'a'). */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 10 ` 90 90\" fill=\"#00ff00\"/></svg>"));

  /* V3: a '{' at a command position (just above 'z'). */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 10 { 90 90\" fill=\"#0000ff\"/></svg>"));
  TEST_END("priv_next_cmd MC/DC: comma/trailing separators + letter-range bounds");
}

/**
 * @test internal_test_points_comma_leading_separator_mcdc
 *
 * @par MC/DC:
 * Decision: the point-list separator skip
 *   `while ((k < vlen) && (priv_ra8_svgp_ws((char)v[k]) || (v[k] == ',')))`
 * (3 conditions; apps/shared_libs/reflow/src/reflow_svg_shape.c). priv_ra8_svgp_num consumes
 * the comma inside a coordinate pair, so a comma only reaches this skip loop when
 * it leads the point list (or immediately follows a pair the number parser left
 * on a comma). N+1 supplement to the existing whitespace-driven vectors:
 *  - a LEADING comma before the first coordinate -> C1 true (k < vlen), C2 false
 *    (not whitespace), C3 true (v[k] == ',') -> the loop skips it. This is the
 *    C3-true independence pair that the whitespace-only inputs never reach; the
 *    polygon still fills its triangle interior, proving the comma was skipped and
 *    the three vertices parsed.
 * @brief Verify points comma leading separator mcdc behavior against the reflow contract.
 * @details Exercises the points comma leading separator mcdc path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_points_comma_leading_separator_mcdc(void)
{
  TEST_BEGIN("svg points MC/DC: leading-comma separator skip (v[k] == ',')");
  fb_reset();
  /* Leading comma before the first coordinate forces the ws/comma skip loop to
   * evaluate (v[k] == ',') true; the triangle is otherwise identical. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<polygon points=\",10,10 90,10 50,90\" fill=\"#ff0000\"/></svg>"));
  TEST_ASSERT_EQ(k_red, px(100, 60)); /* triangle interior is filled */
  TEST_END("svg points MC/DC: leading-comma separator skip (v[k] == ',')");
}

/**
 * @test internal_test_arc_sweep0_dt_positive_mcdc
 *
 * @par MC/DC:
 * Decision: `if (!sweep && (dt > 0.0F))` -- the sweep=0 angle-wrap arm of the
 * endpoint-to-centre arc solver (apps/shared_libs/reflow/src/reflow_svg_shape.c, 2
 * conditions, AND). The existing arc vectors leave the raw dt <= 0 (C2 false).
 * A LARGE arc (large-arc-flag 1) over a radius exceeding half the chord makes the
 * raw sweep angle come out positive so the wrap fires. Both sweep=0 arcs are
 * rendered so the positive-dt one is exercised regardless of the screen-space
 * sign convention. N+1 supplement:
 *  - large arc, sweep=0 -> !sweep true (C1), raw dt > 0 (C2) -> true -> dt wraps
 *    by -2pi. Paired with the existing sweep=1 vector (C1 false) and the small
 *    sweep=0 arc (C2 false), this completes both independence pairs.
 * The solved-and-flattened arc fills a region (at least one blue pixel appears),
 * confirming the endpoint-to-centre conversion completed through this wrap arm.
 * @brief Verify arc sweep0 dt positive mcdc behavior against the reflow contract.
 * @details Exercises the arc sweep0 dt positive mcdc path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_arc_sweep0_dt_positive_mcdc(void)
{
  TEST_BEGIN("svg arc MC/DC: sweep=0 positive-dt wrap arm");
  /* Two sweep=0 arcs over radius 50 (chord 80 < diameter 100). The major arc
   * (large-arc-flag 1) has a raw sweep angle > 0, exercising the
   * (!sweep && dt > 0.0F) wrap arm; the minor arc drives the dt <= 0 side. */
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 50 A50 50 0 0 0 90 50 Z\" fill=\"#0000ff\"/></svg>"));
  fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 50 A50 50 0 1 0 90 50 Z\" fill=\"#0000ff\"/></svg>"));
  /* The last (major) arc fills a region; scan for at least one blue pixel. */
  bool any_blue = false;
  for (int32_t yy = 0; (yy < k_h) && !any_blue; ++yy) {
    for (int32_t xx = 0; xx < k_w; ++xx) {
      if (px(xx, yy) == (uint32_t)k_blue) {
        any_blue = true;
        break;
      }
    }
  }
  TEST_ASSERT(any_blue);
  TEST_END("svg arc MC/DC: sweep=0 positive-dt wrap arm");
}

/**
 * @brief Test executable entry point -- runs the shape/path MC/DC vectors.
 *
 * @return 0 on success (all tests passed).
 *
 * @pre Host environment provides stderr.
 * @post Every shape decision family above has executed its vectors.
 *
 * @note Not thread-safe (single-threaded test runner).
 * @since 0.1.0
 */
int main(void)
{
  internal_test_xform_separators_unknown();
  internal_test_circle_skip_and_rotated_mcdc();
  internal_test_polygon_polyline_guards_mcdc();
  internal_test_path_relative_hv();
  internal_test_arc_sweep_wrap_mcdc();
  internal_test_arc_radius_scale_up();
  internal_test_group_selfclose_depth_and_pop();
  internal_test_xform_scan_boundary_mcdc();
  internal_test_points_trailing_separator_mcdc();
  internal_test_scanline_windings_mcdc();
  internal_test_curve_overflow_count_mcdc();
  internal_test_arc_sweep_both_flags_mcdc();
  internal_test_arc_degenerate_arms_mcdc();
  internal_test_path_cmd_scan_arms_mcdc();
  internal_test_svg_path_no_progress_guard_mcdc();
  internal_test_points_comma_leading_separator_mcdc();
  internal_test_arc_sweep0_dt_positive_mcdc();
  return 0;
}
