/**
 * @file test_svg.c
 * @brief Host unit tests for the minimal SVG subset (#112).
 *
 * @details Exercises the SVG sniff, the cover-wrapper `<image>` href
 * extraction, and the `<rect>`/`<circle>`/`<line>`/`<polygon>`/`<path>`
 * rasteriser (rendered into a host ra8_gfx framebuffer and checked
 * pixel-by-pixel), plus the null/arg guards.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_gfx.h"
#include "reflow_svg.h"
#include "unity_minimal.h"

/**
 * @enum t_svg_t
 * @brief Framebuffer pixel mask.
 */
typedef enum : uint32_t {
  k_t_rgb_mask = 0xFFFFFFU, /**< The 24 RGB bits, dropping the unused alpha byte
                                 so a comparison is not sensitive to it.         */
} t_svg_t;

enum : int32_t {
  k_w = 200, /**< W. */
  k_h = 200, /**< H. */
};

/** @brief Host framebuffer (ARGB8888) bound by the internal_render tests. */
static uint32_t s_fb[k_w * k_h];

/** @brief RGB (low 24 bits) of the framebuffer pixel at (x, y).
 * @details Exercises the px path and preserves each documented result and bound.
 * @param[in] x Caller-supplied x value used by the scenario.
 * @param[in] y Caller-supplied y value used by the scenario.
 * @return Low 24-bit RGB value of the requested framebuffer pixel.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 * @retval 0 The requested pixel is black.
 * @retval nonzero The pixel contains the returned RGB color.
 */
RA8_INTERNAL static uint32_t internal_px(int32_t x, int32_t y)
{
  return s_fb[(y * k_w) + x] & k_t_rgb_mask;
}

/** @brief Bind + clear the framebuffer to white.
 * @details Exercises the fb reset path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_fb_reset(void)
{
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_init(s_fb, (uint16_t)k_w, (uint16_t)k_h, k_ra8_gfx_format_argb8888));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear(0xFFFFFFU));
}

/** @brief Render a NUL-terminated SVG string into the full framebuffer box.
 * @details Exercises the render path and preserves each documented result and bound.
 * @param[in] svg NUL-terminated SVG document supplied to the render helper.
 * @return SVG renderer status for the supplied document.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 * @retval k_ra8_ok The SVG rendered successfully.
 * @retval nonzero Validation or rendering rejected the SVG.
 */
RA8_INTERNAL static ra8_err_t internal_render(const char* svg)
{
  return ra8_svg_render((const uint8_t*)svg, strlen(svg), 0, 0, k_w, k_h);
}

/**
 * @test internal_test_is_svg
 * @brief The sniff accepts `<svg` / `<?xml` (with BOM / whitespace), rejects PNG.
 *
 * @par MC/DC:
 * Decision: `priv_ra8_svgp_starts_ci(...,"<?xml") || priv_ra8_svgp_starts_ci(...,"<svg")`
 * (2 conditions, function `ra8_svg_is_svg`).
 * - V1 PNG bytes "\x89PNG.." -> C1=F, C2=F -> false (rejected).
 * - V2 "  \n<?xml..."        -> C1=T        -> true  (accepted, varies C1).
 * - V3 "<svg xmlns..."       -> C1=F, C2=T -> true  (accepted, varies C2).
 * V1+V2 prove the `<?xml` condition independent; V1+V3 prove `<svg`. N+1 = 3.
 * (The `bytes == nullptr || len == 0` guard is exercised, not driven to MC/DC.)
 * @details Exercises the is svg path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_is_svg(void)
{
  TEST_BEGIN("svg sniff");
  TEST_ASSERT(ra8_svg_is_svg((const uint8_t*)"<svg xmlns=...>", 15U));
  TEST_ASSERT(ra8_svg_is_svg((const uint8_t*)"  \n<?xml version=\"1.0\"?>", 23U));
  const uint8_t bom[] = {0xEFU, 0xBBU, 0xBFU, '<', 's', 'v', 'g', '>'};
  TEST_ASSERT(ra8_svg_is_svg(bom, sizeof(bom)));
  const uint8_t png[] = {0x89U, 'P', 'N', 'G', 0x0DU};
  TEST_ASSERT(!ra8_svg_is_svg(png, sizeof(png)));
  TEST_ASSERT(!ra8_svg_is_svg(nullptr, 0U));
  TEST_END("svg sniff");
}

/**
 * @test internal_test_image_href
 * @brief A cover-wrapper `<svg><image .../></svg>` yields its href slice.
 *
 * @par MC/DC:
 * (no compound decision is driven to MC/DC by this test -- it exercises href
 * extraction for `xlink:href`, plain `href`, and the no-`<image>` not-found
 * return. The `!attr(xlink:href) && !attr(href)` guard in `ra8_svg_image_href`
 * is reached only on its false outcomes here, so no condition is varied to flip
 * it, and the not-found case exits earlier via internal_tag_span.)
 * @details Exercises the image href path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_image_href(void)
{
  TEST_BEGIN("svg image-wrapper href");
  const char* svg = "<svg viewBox=\"0 0 600 800\"><image width=\"600\" height=\"800\" "
                    "xlink:href=\"images/cover.jpg\"/></svg>";
  size_t      off = 0U;
  size_t      vl  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_svg_image_href((const uint8_t*)svg, strlen(svg), &off, &vl));
  TEST_ASSERT_EQ(strlen("images/cover.jpg"), vl);
  TEST_ASSERT(strncmp(&svg[off], "images/cover.jpg", vl) == 0);

  /* Plain `href` (no xlink:) also works. */
  const char* svg2 = "<svg><image href=\"c.png\"/></svg>";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_svg_image_href((const uint8_t*)svg2, strlen(svg2), &off, &vl));
  TEST_ASSERT(strncmp(&svg2[off], "c.png", vl) == 0);

  /* No image -> not found. */
  const char* shapes = "<svg><rect width=\"1\" height=\"1\"/></svg>";
  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 ra8_svg_image_href((const uint8_t*)shapes, strlen(shapes), &off, &vl));
  TEST_END("svg image-wrapper href");
}

/**
 * @test internal_test_render_shapes
 * @brief rect / circle / line internal_render at viewBox-scaled positions + colours.
 *
 * @par MC/DC:
 * (no compound decision is driven to MC/DC by this test -- it verifies rect /
 * circle / line rasterisation by rendered-pixel assertions; it does not vary any
 * single decision's conditions independently.)
 * @details Exercises the render shapes path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_render_shapes(void)
{
  TEST_BEGIN("svg render shapes");
  internal_fb_reset();
  /* viewBox 100x100 into a 200x200 box -> 2x scale. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_render("<svg viewBox=\"0 0 100 100\">"
                    "<rect x=\"0\" y=\"0\" width=\"50\" height=\"50\" fill=\"#ff0000\"/>"
                    "<circle cx=\"75\" cy=\"75\" r=\"20\" fill=\"blue\"/>"
                    "<line x1=\"0\" y1=\"50\" x2=\"100\" y2=\"50\" stroke=\"#00ff00\"/>"
                    "</svg>"));
  TEST_ASSERT_EQ(0xFF0000, internal_px(20, 20));   /* inside the red rect (0..100) */
  TEST_ASSERT_EQ(0x0000FF, internal_px(150, 150)); /* circle centre (cx=75->150)   */
  TEST_ASSERT_EQ(0x00FF00, internal_px(100, 100)); /* green line at y=50->100      */
  TEST_ASSERT_EQ(0xFFFFFF, internal_px(190, 20));  /* untouched top-right -> white */
  TEST_END("svg render shapes");
}

/**
 * @test internal_test_render_polygon
 * @brief A `<polygon>` is filled by the scanline rasteriser (inside vs outside).
 *
 * @par MC/DC:
 * (no compound decision is driven to MC/DC by this test -- it verifies the
 * scanline polygon fill (inside vs outside a triangle) by rendered-pixel
 * assertions; it does not vary any single decision's conditions independently.)
 * @details Exercises the render polygon path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_render_polygon(void)
{
  TEST_BEGIN("svg render polygon");
  internal_fb_reset();
  /* Downward triangle in a 100x100 viewBox -> 2x: screen (20,20)(180,20)(100,180). */
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_render("<svg viewBox=\"0 0 100 100\">"
                                 "<polygon points=\"10,10 90,10 50,90\" fill=\"#ff00ff\"/></svg>"));
  TEST_ASSERT_EQ(0xFF00FF, internal_px(100, 100)); /* inside the triangle (x 60..140) */
  TEST_ASSERT_EQ(0xFFFFFF, internal_px(30, 100));  /* left of the left edge -> white  */
  TEST_END("svg render polygon");
}

/**
 * @test internal_test_render_path
 * @brief A `<path>` of M/L/V/H/Z line commands fills as a polygon.
 *
 * @par MC/DC:
 * (no compound decision is driven to MC/DC by this test -- it verifies M/L/V/H/Z
 * path filling as a polygon by rendered-pixel assertions; it does not vary any
 * single decision's conditions independently.)
 * @details Exercises the render path path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_render_path(void)
{
  TEST_BEGIN("svg render path");
  internal_fb_reset();
  /* Square (10,10)-(90,90) in a 100x100 viewBox -> screen (20,20)-(180,180). */
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_render("<svg viewBox=\"0 0 100 100\">"
                                 "<path d=\"M10 10 L90 10 V90 H10 Z\" fill=\"#00aacc\"/></svg>"));
  TEST_ASSERT_EQ(0x00AACC, internal_px(100, 100)); /* inside the square     */
  TEST_ASSERT_EQ(0xFFFFFF, internal_px(10, 100));  /* left of x=20 -> white */
  TEST_END("svg render path");
}

/**
 * @test internal_test_render_cubic
 * @brief A `<path>` cubic `C` is flattened (collinear controls == a line edge).
 *
 * @par MC/DC:
 * (no compound decision is driven to MC/DC by this test -- it exercises the
 * cubic `C` flatten (internal_flatten_cubic) by rendered-pixel assertions; it does
 * not vary any single decision's conditions independently.)
 * @details Exercises the render cubic path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_render_cubic(void)
{
  TEST_BEGIN("svg render cubic path");
  internal_fb_reset();
  /* M..C..L..L..Z where C's controls are collinear -> the top edge is straight,
   * giving the rectangle (10,10)-(90,90); exercises internal_flatten_cubic. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_render(
      "<svg viewBox=\"0 0 100 100\">"
      "<path d=\"M10 10 C30 10 70 10 90 10 L90 90 L10 90 Z\" fill=\"#0088cc\"/></svg>"));
  TEST_ASSERT_EQ(0x0088CC, internal_px(100, 100)); /* inside the rectangle */
  TEST_ASSERT_EQ(0xFFFFFF, internal_px(10, 100));  /* outside -> white     */
  TEST_END("svg render cubic path");
}

/**
 * @test internal_test_render_quad
 * @brief A `<path>` quadratic `Q` is flattened (collinear control == a line edge).
 *
 * @par MC/DC:
 * (no compound decision is driven to MC/DC by this test -- it exercises the
 * quadratic `Q` flatten (internal_flatten_quad) by rendered-pixel assertions; it
 * does not vary any single decision's conditions independently.)
 * @details Exercises the render quad path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_render_quad(void)
{
  TEST_BEGIN("svg render quadratic path");
  internal_fb_reset();
  /* Q's control (50,10) is collinear with its endpoints (10,10)->(90,10), so the
   * top edge is straight -> the rectangle (10,10)-(90,90); exercises internal_flatten_quad. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_render("<svg viewBox=\"0 0 100 100\">"
                    "<path d=\"M10 10 Q50 10 90 10 L90 90 L10 90 Z\" fill=\"#0088cc\"/></svg>"));
  TEST_ASSERT_EQ(0x0088CC, internal_px(100, 100)); /* inside the rectangle */
  TEST_ASSERT_EQ(0xFFFFFF, internal_px(10, 100));  /* outside -> white     */
  TEST_END("svg render quadratic path");
}

/**
 * @test internal_test_render_smooth_t
 * @brief A smooth quadratic `T` with no preceding `Q` uses the current point as
 *        its control (no reflection), so a collinear run stays a straight edge.
 *
 * @par MC/DC:
 * Decision: `st->kind == want` in internal_smooth_ctrl (1 condition).
 * - Vector A (this test): T after `M`, st->kind=0 != 'q' -> false -> control = p0.
 * - Vector B (internal_test_render_smooth_s): S after `C`, st->kind='c' == 'c' -> true ->
 *   control = reflected. A+B exercise both outcomes of the lone condition (N+1=2).
 * @details Exercises the render smooth t path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_render_smooth_t(void)
{
  TEST_BEGIN("svg render smooth-T (no reflect)");
  internal_fb_reset();
  /* T with no preceding Q -> control = current point (10,10); from (10,10) to
   * (90,10) on y=10 -> a straight top edge -> the rectangle (10,10)-(90,90). */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_render("<svg viewBox=\"0 0 100 100\">"
                    "<path d=\"M10 10 T90 10 L90 90 L10 90 Z\" fill=\"#0088cc\"/></svg>"));
  TEST_ASSERT_EQ(0x0088CC, internal_px(100, 100)); /* inside the rectangle */
  TEST_ASSERT_EQ(0xFFFFFF, internal_px(10, 100));  /* outside -> white     */
  TEST_END("svg render smooth-T (no reflect)");
}

/**
 * @test internal_test_render_smooth_s
 * @brief A smooth cubic `S` after a `C` reflects the previous control; a fully
 *        collinear run still yields a straight edge (exercises the reflect path).
 *
 * @par MC/DC:
 * Decision: `st->kind == want` in internal_smooth_ctrl (1 condition).
 * - Vector B (this test): S after `C`, st->kind='c' == want('c') -> true ->
 *   control = reflected previous control point.
 * - Vector A (::internal_test_render_smooth_t): T after `M`, st->kind=0 != 'q' -> false
 *   -> control = current point. A+B exercise both outcomes of the lone
 *   condition (N+1 = 2).
 * @details Exercises the render smooth s path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_render_smooth_s(void)
{
  TEST_BEGIN("svg render smooth-S (reflect)");
  internal_fb_reset();
  /* C ends at (50,10) with ctrl2 (40,10); S reflects it to (60,10); all controls
   * and endpoints stay on y=10 -> a straight top edge -> rectangle (10,10)-(90,90). */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_render("<svg viewBox=\"0 0 100 100\">"
                    "<path d=\"M10 10 C20 10 40 10 50 10 S80 10 90 10 L90 90 L10 90 Z\" "
                    "fill=\"#0088cc\"/></svg>"));
  TEST_ASSERT_EQ(0x0088CC, internal_px(100, 100)); /* inside the rectangle */
  TEST_ASSERT_EQ(0xFFFFFF, internal_px(10, 100));  /* outside -> white     */
  TEST_END("svg render smooth-S (reflect)");
}

/**
 * @test internal_test_render_arc
 * @brief A `<path>` elliptical arc `A` is centre-parametrised + flattened. A
 *        semicircle (chord == diameter) with sweep=1 bulges to one side; the
 *        closed half-disk fills there, not across the chord.
 *
 * @par MC/DC:
 * Decision: the `internal_arc_center` degenerate guard
 * `rx==0 || ry==0 || (start==end)` (which selects line-fallback vs a real arc).
 * - Vector A (here): rx=ry=40>0, start!=end -> all false -> a real arc renders.
 * - Vector B (::internal_test_render_arc_degenerate): rx=0 -> true -> straight line.
 * A+B vary the radius condition alone, proving it independently flips the
 * outcome (N+1 = 2 for the controlling pair).
 * @details Exercises the render arc path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_render_arc(void)
{
  TEST_BEGIN("svg render arc (sweep 1)");
  internal_fb_reset();
  /* (10,50)->(90,50), r=40 == half-chord -> exact semicircle about (50,50);
   * sweep=1 bulges UP (peak ~ (50,10)); the upper half-disk fills. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_render("<svg viewBox=\"0 0 100 100\">"
                    "<path d=\"M10 50 A40 40 0 0 1 90 50 Z\" fill=\"#0088cc\"/></svg>"));
  TEST_ASSERT_EQ(0x0088CC, internal_px(100, 60));  /* user (50,30): inside the upper bulge    */
  TEST_ASSERT_EQ(0xFFFFFF, internal_px(100, 150)); /* user (50,75): across the chord -> white */
  TEST_END("svg render arc (sweep 1)");
}

/**
 * @test internal_test_render_arc_sweep0
 * @brief The same arc with sweep=0 bulges the opposite way (the sweep flag
 *        independently flips the swept side).
 *
 * @par MC/DC:
 * Decision: the sweep normalisation `(!sweep && dt>0) | (sweep && dt<0)` that
 * wraps the signed sweep. Vector here: sweep=0 -> the first arm governs (dt is
 * negative, untouched) -> downward bulge. ::internal_test_render_arc (sweep=1) takes the
 * second arm (dt wrapped +2pi) -> upward bulge. The two vary the sweep flag
 * alone and produce opposite fills, proving its independent influence.
 * @details Exercises the render arc sweep0 path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_render_arc_sweep0(void)
{
  TEST_BEGIN("svg render arc (sweep 0)");
  internal_fb_reset();
  /* Same chord, sweep=0 -> bulges DOWN (peak ~ (50,90)); lower half-disk fills. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_render("<svg viewBox=\"0 0 100 100\">"
                    "<path d=\"M10 50 A40 40 0 0 0 90 50 Z\" fill=\"#0088cc\"/></svg>"));
  TEST_ASSERT_EQ(0x0088CC, internal_px(100, 150)); /* user (50,75): inside the lower bulge    */
  TEST_ASSERT_EQ(0xFFFFFF, internal_px(100, 60));  /* user (50,30): across the chord -> white */
  TEST_END("svg render arc (sweep 0)");
}

/**
 * @test internal_test_render_arc_degenerate
 * @brief A zero-radius arc (`rx=0`) collapses to a straight line to its
 *        endpoint -- here the path closes into a plain rectangle.
 *
 * @par MC/DC:
 * Decision: the internal_arc_center degenerate guard
 * `(rx == 0) || (ry == 0) || ((p0.x == p_end.x) && (p0.y == p_end.y))`.
 * - Vector B (this test): rx=0 -> true -> line-fallback (the `A` collapses to a
 *   straight edge).
 * - Vector A (::internal_test_render_arc): rx=ry=40>0, start!=end -> all false -> a real
 *   arc renders. A+B vary the radius condition alone, proving it independently
 *   flips the outcome (N+1 = 2 for the controlling pair).
 * @details Exercises the render arc degenerate path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_render_arc_degenerate(void)
{
  TEST_BEGIN("svg render arc (degenerate rx=0)");
  internal_fb_reset();
  /* rx=0 -> the A is a line (10,50)->(90,50); L/L/Z close a rectangle 10..90 x 50..90. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_render("<svg viewBox=\"0 0 100 100\">"
                                 "<path d=\"M10 50 A0 40 0 0 1 90 50 L90 90 L10 90 Z\" "
                                 "fill=\"#0088cc\"/></svg>"));
  TEST_ASSERT_EQ(0x0088CC, internal_px(100, 140)); /* user (50,70): inside the rectangle         */
  TEST_ASSERT_EQ(0xFFFFFF, internal_px(100, 190)); /* user (50,95): below the rectangle -> white */
  TEST_END("svg render arc (degenerate rx=0)");
}

/**
 * @test internal_test_render_fill_none
 * @brief `fill="none"` rects draw nothing; default fill is black.
 *
 * @par MC/DC:
 * Decision: `(gi < 0) && (fill == k_svg_no_paint)` in priv_ra8_svgp_draw_rect (the
 * skip-when-unpainted guard, 2 conditions).
 * - V1 fill="none":  gi<0=T, fill==no_paint=T -> true  -> skip (stays white).
 * - V2 no fill attr: gi<0=T, fill==no_paint=F -> false -> paint (black default).
 * - V3 fill="url(#g)" (::internal_test_render_gradient): gi>=0 so gi<0=F -> false ->
 *   paint (gradient). V1+V2 prove the no-paint condition independent; V1+V3
 *   prove the gradient-index condition. N+1 = 3.
 * @details Exercises the render fill none path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_render_fill_none(void)
{
  TEST_BEGIN("svg fill none + default");
  internal_fb_reset();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_render("<svg viewBox=\"0 0 200 200\">"
                    "<rect x=\"0\" y=\"0\" width=\"100\" height=\"200\" fill=\"none\"/>"
                    "<rect x=\"100\" y=\"0\" width=\"100\" height=\"200\"/>"
                    "</svg>"));
  TEST_ASSERT_EQ(0xFFFFFF, internal_px(50, 100));  /* fill:none -> stays white      */
  TEST_ASSERT_EQ(0x000000, internal_px(150, 100)); /* no fill attr -> black default */
  TEST_END("svg fill none + default");
}

/**
 * @test internal_test_render_transform
 * @brief `transform=` translate / scale on a shape and on a `<g>` group move
 *        and resize the rendered output (viewBox 100->box 200 == 2x).
 *
 * @par MC/DC:
 * (no compound decision is driven to MC/DC by this test -- it verifies basic
 * translate / scale / `<g>`-group placement by rendered-pixel assertions. The
 * transform-argument and has-rotation compound decisions are driven to MC/DC by
 * ::internal_test_transform_args_mcdc and ::internal_test_render_rotate.)
 * @details Exercises the render transform path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_render_transform(void)
{
  TEST_BEGIN("svg transform translate/scale/group");
  /* translate(40,0): rect user x 10..30 -> 50..70 -> fb 100..140; y -> fb 20..60. */
  internal_fb_reset();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_render("<svg viewBox=\"0 0 100 100\"><rect x=\"10\" y=\"10\" width=\"20\" "
                    "height=\"20\" fill=\"#ff0000\" transform=\"translate(40,0)\"/></svg>"));
  TEST_ASSERT_EQ(0xFF0000, internal_px(120, 40)); /* inside the translated rect */
  TEST_ASSERT_EQ(0xFFFFFF, internal_px(40, 40));  /* original position vacated  */

  /* scale(2): rect user 10..20 -> 20..40 -> fb 40..80 (both axes). */
  internal_fb_reset();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_render("<svg viewBox=\"0 0 100 100\"><rect x=\"10\" y=\"10\" width=\"10\" "
                    "height=\"10\" fill=\"#0000ff\" transform=\"scale(2)\"/></svg>"));
  TEST_ASSERT_EQ(0x0000FF, internal_px(60, 60)); /* inside the scaled-up rect   */
  TEST_ASSERT_EQ(0xFFFFFF, internal_px(28, 28)); /* unscaled position now white */

  /* <g transform="translate(50,50)"> moves the child rect (0..10 -> fb 100..120). */
  internal_fb_reset();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_render("<svg viewBox=\"0 0 100 100\"><g transform=\"translate(50,50)\">"
                    "<rect x=\"0\" y=\"0\" width=\"10\" height=\"10\" fill=\"#00ff00\"/>"
                    "</g></svg>"));
  TEST_ASSERT_EQ(0x00FF00, internal_px(110, 110)); /* inside the grouped+moved rect */
  TEST_ASSERT_EQ(0xFFFFFF, internal_px(10, 10));   /* group origin vacated          */
  TEST_END("svg transform translate/scale/group");
}

/**
 * @test internal_test_transform_args_mcdc
 * @brief The transform argument decision `has2 ? a2 : (is_sc ? a1 : 0)`.
 *
 * @par MC/DC:
 * Decision (per axis, in internal_parse_xform): the second-argument value selects on
 * `(is_scale, has_second_arg)`.
 * - Vector 1: `scale(2)`      -> is_sc=T, has2=F -> sy defaults to a1 (=2).
 * - Vector 2: `scale(2,3)`    -> is_sc=T, has2=T -> sy = a2 (=3), not a1.
 * - Vector 3: `translate(40)` -> is_sc=F, has2=F -> ty defaults to 0, not a1.
 * 1+2 prove has2 independently flips sy (2 vs 3); 1+3 prove is_sc independently
 * flips the default (a1 vs 0). N+1 = 3 vectors for the 2-condition decision.
 * @details Exercises the transform args mcdc path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_transform_args_mcdc(void)
{
  TEST_BEGIN("svg transform arg mc/dc");
  /* V1 scale(2): uniform -> rect 10..20 -> fb 40..80 both axes. */
  internal_fb_reset();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_render("<svg viewBox=\"0 0 100 100\"><rect x=\"10\" y=\"10\" width=\"10\" "
                    "height=\"10\" fill=\"#112233\" transform=\"scale(2)\"/></svg>"));
  TEST_ASSERT_EQ(0x112233, internal_px(60, 60)); /* y scaled by 2 like x */

  /* V2 scale(2,3): y scaled by 3 -> user y 30..60 -> fb 60..120 (not 40..80). */
  internal_fb_reset();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_render("<svg viewBox=\"0 0 100 100\"><rect x=\"10\" y=\"10\" width=\"10\" "
                    "height=\"10\" fill=\"#445566\" transform=\"scale(2,3)\"/></svg>"));
  TEST_ASSERT_EQ(0x445566, internal_px(60, 90)); /* inside only if y-scale == 3 */
  TEST_ASSERT_EQ(0xFFFFFF, internal_px(60, 50)); /* white -> y-scale != 2       */

  /* V3 translate(40): ty defaults to 0 (not 40) -> rect y stays fb 20..40. */
  internal_fb_reset();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_render("<svg viewBox=\"0 0 100 100\"><rect x=\"10\" y=\"10\" width=\"10\" "
                    "height=\"10\" fill=\"#778899\" transform=\"translate(40)\"/></svg>"));
  TEST_ASSERT_EQ(0x778899, internal_px(110, 30));  /* x moved, y unchanged (ty=0) */
  TEST_ASSERT_EQ(0xFFFFFF, internal_px(110, 110)); /* white -> ty != 40           */
  TEST_END("svg transform arg mc/dc");
}

/**
 * @test internal_test_render_rotate
 * @brief `transform=` rotate / skew / matrix drive the generalized affine
 *        polygon path; axis-aligned transforms keep the fast-path.
 *
 * @par MC/DC:
 * Decision `priv_has_rot`: `(ub != 0) || (uc != 0)` selects the generalized
 * affine-polygon path over the axis-aligned `ra8_gfx_rect`/`circle` fast-path.
 * - Vector 1: `scale(2)`  -> ub=0, uc=0 -> false (fast-path, axis-aligned).
 * - Vector 2: `skewY(45)` -> ub=tan45!=0, uc=0 -> true (shears in y).
 * - Vector 3: `skewX(45)` -> ub=0, uc=tan45!=0 -> true (shears in x).
 * 1+2 prove ub independently flips the path; 1+3 prove uc independently does.
 * N+1 = 3 vectors for the 2-condition decision.
 * @details Exercises the render rotate path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_render_rotate(void)
{
  TEST_BEGIN("svg transform rotate/skew");
  /* rotate(90,50,50): map (x,y)->(100-y,x). A horizontal bar user x30..70,y45..55
   * becomes a vertical bar user x45..55,y30..70 (fb is 2x: viewBox 100 -> box 200). */
  internal_fb_reset();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_render("<svg viewBox=\"0 0 100 100\"><rect x=\"30\" y=\"45\" width=\"40\" "
                    "height=\"10\" fill=\"#ff0000\" transform=\"rotate(90,50,50)\"/></svg>"));
  TEST_ASSERT_EQ(0xFF0000, internal_px(100, 100)); /* rotation centre -> inside the bar */
  TEST_ASSERT_EQ(0xFFFFFF, internal_px(70, 100));  /* original bar's left end vacated   */

  /* V1 scale(2): ub=uc=0 -> fast-path; rect 10..20 -> fb 40..80. */
  internal_fb_reset();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_render("<svg viewBox=\"0 0 100 100\"><rect x=\"10\" y=\"10\" width=\"10\" "
                    "height=\"10\" fill=\"#00ff00\" transform=\"scale(2)\"/></svg>"));
  TEST_ASSERT_EQ(0x00FF00, internal_px(60, 60)); /* inside the axis-aligned scaled rect */

  /* V3 skewX(45): x'=x+y. Rect user x10..20,y40..50 -> parallelogram spanning user
   * x~55..65 at y=45; user(60,45)=fb(120,90) is covered only because uc!=0. */
  internal_fb_reset();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_render("<svg viewBox=\"0 0 100 100\"><rect x=\"10\" y=\"40\" width=\"10\" "
                    "height=\"10\" fill=\"#0000ff\" transform=\"skewX(45)\"/></svg>"));
  TEST_ASSERT_EQ(0x0000FF, internal_px(120, 90)); /* inside the x-sheared rect (uc!=0) */
  TEST_ASSERT_EQ(0xFFFFFF, internal_px(30, 90));  /* original rect position vacated    */

  /* V2 skewY(45): y'=y+x. Rect user x40..50,y10..20 -> spans user y~55..65 at x=45;
   * user(45,60)=fb(90,120) is covered only because ub!=0. */
  internal_fb_reset();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_render("<svg viewBox=\"0 0 100 100\"><rect x=\"40\" y=\"10\" width=\"10\" "
                    "height=\"10\" fill=\"#ffaa00\" transform=\"skewY(45)\"/></svg>"));
  TEST_ASSERT_EQ(0xFFAA00, internal_px(90, 120)); /* inside the y-sheared rect (ub!=0) */

  /* matrix(2,0,0,2,0,0): b=c=0 -> fast-path, equivalent to scale(2). */
  internal_fb_reset();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_render("<svg viewBox=\"0 0 100 100\"><rect x=\"10\" y=\"10\" width=\"10\" "
                    "height=\"10\" fill=\"#aa00ff\" transform=\"matrix(2,0,0,2,0,0)\"/></svg>"));
  TEST_ASSERT_EQ(0xAA00FF, internal_px(60, 60)); /* matrix scale == scale(2) */
  TEST_END("svg transform rotate/skew");
}

/**
 * @test internal_test_render_gradient
 * @brief A `fill="url(#id)"` linear gradient interpolates across the shape; an
 *        unmatched ref is skipped; a solid fill is unaffected.
 *
 * @par MC/DC:
 * Decision in priv_resolve_fill: a fill is a gradient iff
 * `is_url_ref(value) && gradient_matched`.
 * - Vector 1: `fill="#112233"` -> is_url=F -> solid (the rect paints #112233).
 * - Vector 2: `fill="url(#g)"` with `#g` defined -> is_url=T, matched=T -> gradient.
 * - Vector 3: `fill="url(#no)"` undefined -> is_url=T, matched=F -> skipped (white).
 * 1+2 prove is_url independently flips solid-vs-gradient; 2+3 prove the match flips
 * gradient-vs-skip. N+1 = 3 vectors for the 2-condition decision.
 * @details Exercises the render gradient path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_render_gradient(void)
{
  TEST_BEGIN("svg linear gradient");
  /* V2: black->white horizontal linear gradient over the full 100x100 viewBox
   * (box 200x200, objectBoundingBox default x1=0,y1=0,x2=1,y2=0). */
  const char* lin =
    "<svg viewBox=\"0 0 100 100\"><defs><linearGradient id=\"g\">"
    "<stop offset=\"0\" stop-color=\"#000000\"/><stop offset=\"1\" stop-color=\"#ffffff\"/>"
    "</linearGradient></defs><rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
    "fill=\"url(#g)\"/></svg>";
  internal_fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok, internal_render(lin));
  TEST_ASSERT_EQ(0x7F7F7F, internal_px(100, 100)); /* midpoint blend (0.5) */
  TEST_ASSERT((int)internal_px(10, 100) <
              (int)internal_px(100, 100)); /* darker toward the left stop */
  TEST_ASSERT((int)internal_px(190, 100) >
              (int)internal_px(100, 100)); /* lighter toward the right stop */

  /* V3: unmatched url(#no) -> skipped -> background stays white. */
  internal_fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_render("<svg viewBox=\"0 0 100 100\"><rect x=\"0\" y=\"0\" width=\"100\" "
                                 "height=\"100\" fill=\"url(#no)\"/></svg>"));
  TEST_ASSERT_EQ(0xFFFFFF, internal_px(100, 100));

  /* V1: a plain solid fill is unchanged by the gradient path. */
  internal_fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_render("<svg viewBox=\"0 0 100 100\"><rect x=\"0\" y=\"0\" width=\"100\" "
                                 "height=\"100\" fill=\"#112233\"/></svg>"));
  TEST_ASSERT_EQ(0x112233, internal_px(100, 100));

  /* Multi-stop: #000000@0 -> #ff0000@0.5 -> #ffffff@1 exercises both brackets of
   * the stop search in internal_grad_eval (the left point lands in [0,0.5], the right
   * in [0.5,1]); the middle stop is reproduced exactly. */
  internal_fb_reset();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_render("<svg viewBox=\"0 0 100 100\"><defs><linearGradient id=\"g3\">"
                    "<stop offset=\"0\" stop-color=\"#000000\"/>"
                    "<stop offset=\"0.5\" stop-color=\"#ff0000\"/>"
                    "<stop offset=\"1\" stop-color=\"#ffffff\"/></linearGradient></defs>"
                    "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"url(#g3)\"/>"
                    "</svg>"));
  TEST_ASSERT_EQ(0xFF0000, internal_px(100, 100));          /* p=0.5 -> the middle stop   */
  TEST_ASSERT(((int)internal_px(50, 100) & 0x00FF00) == 0); /* left bracket: no green     */
  TEST_ASSERT(((int)internal_px(50, 100) & 0x0000FF) == 0); /* left bracket: no blue yet  */
  TEST_ASSERT(((int)internal_px(50, 100) & 0xFF0000) > 0);  /* left bracket: red rising   */
  TEST_ASSERT(((int)internal_px(150, 100) & 0x0000FF) > 0); /* right bracket: blue rising */
  TEST_ASSERT((int)internal_px(150, 100) >
              (int)internal_px(50, 100)); /* brighter past the middle stop */
  TEST_END("svg linear gradient");
}

/**
 * @test internal_test_render_gradient_radial
 * @brief A radial gradient is the first stop at the centre, the last at the edge.
 *
 * @par MC/DC:
 * (no compound decision is driven to MC/DC by this test -- it verifies radial
 * gradient interpolation (first stop at the centre, last at the edge) by
 * rendered-pixel assertions. The gradient-fill compound decision is driven to
 * MC/DC by ::internal_test_render_gradient.)
 * @details Exercises the render gradient radial path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_render_gradient_radial(void)
{
  TEST_BEGIN("svg radial gradient");
  /* white centre -> black edge (default cx=cy=r=0.5, objectBoundingBox). */
  const char* rad =
    "<svg viewBox=\"0 0 100 100\"><defs><radialGradient id=\"r\">"
    "<stop offset=\"0\" stop-color=\"#ffffff\"/><stop offset=\"1\" stop-color=\"#000000\"/>"
    "</radialGradient></defs><rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
    "fill=\"url(#r)\"/></svg>";
  internal_fb_reset();
  TEST_ASSERT_EQ(k_ra8_ok, internal_render(rad));
  TEST_ASSERT_EQ(0xFFFFFF, internal_px(100, 100)); /* centre -> first stop (white)  */
  TEST_ASSERT_EQ(0x000000, internal_px(6, 6));     /* corner past r -> last (black) */
  TEST_ASSERT((int)internal_px(100, 100) >
              (int)internal_px(40, 40)); /* brightness falls with radius */
  TEST_END("svg radial gradient");
}

/**
 * @test internal_test_guards
 * @brief NULL / non-positive box arguments are rejected.
 *
 * @par MC/DC:
 * Decision: `(w <= 0) || (h <= 0)` in ra8_svg_render (2 conditions).
 * - V1 w=200,h=200 (::internal_test_render_shapes internal_render path): F,F -> false -> renders.
 * - V2 w=0, h=10: T,F -> true -> k_ra8_err_invalid_arg (varies w).
 * - V3 w=10,h=-1: F,T -> true -> k_ra8_err_invalid_arg (varies h).
 * V1+V2 prove the width condition independent; V1+V3 prove the height. N+1 = 3.
 * (The null-pointer guards on both entry points are single-condition checks.)
 * @details Exercises the guards path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_guards(void)
{
  TEST_BEGIN("svg guards");
  size_t a = 0U;
  size_t b = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_svg_render(nullptr, 1U, 0, 0, 10, 10));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_svg_render((const uint8_t*)"<svg/>", 6U, 0, 0, 0, 10));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_svg_render((const uint8_t*)"<svg/>", 6U, 0, 0, 10, -1));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_svg_image_href(nullptr, 1U, &a, &b));
  TEST_END("svg guards");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros exit(1) on the first failure.
 */
int main(void)
{
  internal_test_is_svg();
  internal_test_image_href();
  internal_test_render_shapes();
  internal_test_render_polygon();
  internal_test_render_path();
  internal_test_render_cubic();
  internal_test_render_quad();
  internal_test_render_smooth_t();
  internal_test_render_smooth_s();
  internal_test_render_arc();
  internal_test_render_arc_sweep0();
  internal_test_render_arc_degenerate();
  internal_test_render_fill_none();
  internal_test_render_transform();
  internal_test_transform_args_mcdc();
  internal_test_render_rotate();
  internal_test_render_gradient();
  internal_test_render_gradient_radial();
  internal_test_guards();
  return 0;
}
