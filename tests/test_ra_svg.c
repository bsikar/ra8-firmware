/**
 * @file test_ra_svg.c
 * @brief Host unit tests for the minimal SVG subset (#112).
 *
 * @details Exercises the SVG sniff, the cover-wrapper `<image>` href
 * extraction, and the `<rect>`/`<circle>`/`<line>`/`<polygon>`/`<path>`
 * rasteriser (rendered into a host ra_gfx framebuffer and checked
 * pixel-by-pixel), plus the null/arg guards.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra_gfx.h"
#include "ra_reflow_svg.h"
#include "unity_minimal.h"

enum : int32_t {
  k_w = 200,
  k_h = 200,
};

/** @brief Host framebuffer (ARGB8888) bound by the render tests. */
static uint32_t s_fb[k_w * k_h];

/** @brief RGB (low 24 bits) of the framebuffer pixel at (x, y). */
static uint32_t px(int32_t x, int32_t y)
{
  return s_fb[(y * k_w) + x] & 0xFFFFFFU;
}

/** @brief Bind + clear the framebuffer to white. */
static void fb_reset(void)
{
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_gfx_init(s_fb, (uint16_t)k_w, (uint16_t)k_h, k_ra_gfx_format_argb8888));
  TEST_ASSERT_EQ(k_ra_ok, ra_gfx_clear(0xFFFFFFU));
}

/** @brief Render a NUL-terminated SVG string into the full framebuffer box. */
static ra_err_t render(const char* svg)
{
  return ra_svg_render((const uint8_t*)svg, strlen(svg), 0, 0, k_w, k_h);
}

/**
 * @test test_is_svg
 * @brief The sniff accepts `<svg` / `<?xml` (with BOM / whitespace), rejects PNG.
 */
static void test_is_svg(void)
{
  TEST_BEGIN("svg sniff");
  TEST_ASSERT(ra_svg_is_svg((const uint8_t*)"<svg xmlns=...>", 15U));
  TEST_ASSERT(ra_svg_is_svg((const uint8_t*)"  \n<?xml version=\"1.0\"?>", 23U));
  const uint8_t bom[] = {0xEFU, 0xBBU, 0xBFU, '<', 's', 'v', 'g', '>'};
  TEST_ASSERT(ra_svg_is_svg(bom, sizeof(bom)));
  const uint8_t png[] = {0x89U, 'P', 'N', 'G', 0x0DU};
  TEST_ASSERT(!ra_svg_is_svg(png, sizeof(png)));
  TEST_ASSERT(!ra_svg_is_svg(nullptr, 0U));
  TEST_END("svg sniff");
}

/**
 * @test test_image_href
 * @brief A cover-wrapper `<svg><image .../></svg>` yields its href slice.
 */
static void test_image_href(void)
{
  TEST_BEGIN("svg image-wrapper href");
  const char* svg = "<svg viewBox=\"0 0 600 800\"><image width=\"600\" height=\"800\" "
                    "xlink:href=\"images/cover.jpg\"/></svg>";
  size_t      off = 0U;
  size_t      vl  = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_svg_image_href((const uint8_t*)svg, strlen(svg), &off, &vl));
  TEST_ASSERT_EQ((int)strlen("images/cover.jpg"), (int)vl);
  TEST_ASSERT(memcmp(&svg[off], "images/cover.jpg", vl) == 0);

  /* Plain `href` (no xlink:) also works. */
  const char* svg2 = "<svg><image href=\"c.png\"/></svg>";
  TEST_ASSERT_EQ(k_ra_ok, ra_svg_image_href((const uint8_t*)svg2, strlen(svg2), &off, &vl));
  TEST_ASSERT(memcmp(&svg2[off], "c.png", vl) == 0);

  /* No image -> not found. */
  const char* shapes = "<svg><rect width=\"1\" height=\"1\"/></svg>";
  TEST_ASSERT_EQ(k_ra_err_not_found,
                 ra_svg_image_href((const uint8_t*)shapes, strlen(shapes), &off, &vl));
  TEST_END("svg image-wrapper href");
}

/**
 * @test test_render_shapes
 * @brief rect / circle / line render at viewBox-scaled positions + colours.
 */
static void test_render_shapes(void)
{
  TEST_BEGIN("svg render shapes");
  fb_reset();
  /* viewBox 100x100 into a 200x200 box -> 2x scale. */
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<rect x=\"0\" y=\"0\" width=\"50\" height=\"50\" fill=\"#ff0000\"/>"
                        "<circle cx=\"75\" cy=\"75\" r=\"20\" fill=\"blue\"/>"
                        "<line x1=\"0\" y1=\"50\" x2=\"100\" y2=\"50\" stroke=\"#00ff00\"/>"
                        "</svg>"));
  TEST_ASSERT_EQ(0xFF0000, (int)px(20, 20));   /* inside the red rect (0..100) */
  TEST_ASSERT_EQ(0x0000FF, (int)px(150, 150)); /* circle centre (cx=75->150)   */
  TEST_ASSERT_EQ(0x00FF00, (int)px(100, 100)); /* green line at y=50->100       */
  TEST_ASSERT_EQ(0xFFFFFF, (int)px(190, 20));  /* untouched top-right -> white  */
  TEST_END("svg render shapes");
}

/**
 * @test test_render_polygon
 * @brief A `<polygon>` is filled by the scanline rasteriser (inside vs outside).
 */
static void test_render_polygon(void)
{
  TEST_BEGIN("svg render polygon");
  fb_reset();
  /* Downward triangle in a 100x100 viewBox -> 2x: screen (20,20)(180,20)(100,180). */
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<polygon points=\"10,10 90,10 50,90\" fill=\"#ff00ff\"/></svg>"));
  TEST_ASSERT_EQ(0xFF00FF, (int)px(100, 100)); /* inside the triangle (x 60..140) */
  TEST_ASSERT_EQ(0xFFFFFF, (int)px(30, 100));  /* left of the left edge -> white   */
  TEST_END("svg render polygon");
}

/**
 * @test test_render_path
 * @brief A `<path>` of M/L/V/H/Z line commands fills as a polygon.
 */
static void test_render_path(void)
{
  TEST_BEGIN("svg render path");
  fb_reset();
  /* Square (10,10)-(90,90) in a 100x100 viewBox -> screen (20,20)-(180,180). */
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 100 100\">"
                        "<path d=\"M10 10 L90 10 V90 H10 Z\" fill=\"#00aacc\"/></svg>"));
  TEST_ASSERT_EQ(0x00AACC, (int)px(100, 100)); /* inside the square     */
  TEST_ASSERT_EQ(0xFFFFFF, (int)px(10, 100));  /* left of x=20 -> white  */
  TEST_END("svg render path");
}

/**
 * @test test_render_cubic
 * @brief A `<path>` cubic `C` is flattened (collinear controls == a line edge).
 */
static void test_render_cubic(void)
{
  TEST_BEGIN("svg render cubic path");
  fb_reset();
  /* M..C..L..L..Z where C's controls are collinear -> the top edge is straight,
   * giving the rectangle (10,10)-(90,90); exercises priv_flatten_cubic. */
  TEST_ASSERT_EQ(
    k_ra_ok,
    render("<svg viewBox=\"0 0 100 100\">"
           "<path d=\"M10 10 C30 10 70 10 90 10 L90 90 L10 90 Z\" fill=\"#0088cc\"/></svg>"));
  TEST_ASSERT_EQ(0x0088CC, (int)px(100, 100)); /* inside the rectangle */
  TEST_ASSERT_EQ(0xFFFFFF, (int)px(10, 100));  /* outside -> white      */
  TEST_END("svg render cubic path");
}

/**
 * @test test_render_fill_none
 * @brief `fill="none"` rects draw nothing; default fill is black.
 */
static void test_render_fill_none(void)
{
  TEST_BEGIN("svg fill none + default");
  fb_reset();
  TEST_ASSERT_EQ(k_ra_ok,
                 render("<svg viewBox=\"0 0 200 200\">"
                        "<rect x=\"0\" y=\"0\" width=\"100\" height=\"200\" fill=\"none\"/>"
                        "<rect x=\"100\" y=\"0\" width=\"100\" height=\"200\"/>"
                        "</svg>"));
  TEST_ASSERT_EQ(0xFFFFFF, (int)px(50, 100));  /* fill:none -> stays white   */
  TEST_ASSERT_EQ(0x000000, (int)px(150, 100)); /* no fill attr -> black default */
  TEST_END("svg fill none + default");
}

/**
 * @test test_guards
 * @brief NULL / non-positive box arguments are rejected.
 */
static void test_guards(void)
{
  TEST_BEGIN("svg guards");
  size_t a = 0U;
  size_t b = 0U;
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_svg_render(nullptr, 1U, 0, 0, 10, 10));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_svg_render((const uint8_t*)"<svg/>", 6U, 0, 0, 0, 10));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_svg_render((const uint8_t*)"<svg/>", 6U, 0, 0, 10, -1));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_svg_image_href(nullptr, 1U, &a, &b));
  TEST_END("svg guards");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros exit(1) on the first failure.
 */
int32_t main(void)
{
  test_is_svg();
  test_image_href();
  test_render_shapes();
  test_render_polygon();
  test_render_path();
  test_render_cubic();
  test_render_fill_none();
  test_guards();
  (void)fprintf(stderr, "[OK ] test_ra_svg.c\n");
  return 0;
}
