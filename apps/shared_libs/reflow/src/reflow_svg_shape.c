/**
 * @file reflow_svg_shape.c
 * @brief SVG shape rasterisers: rect / circle / line / polygon + fill + arc (#112).
 *
 * @details The `<rect>`, `<circle>`, `<line>`, `<polygon>`, and `<polyline>`
 * rasterisers, the even-odd scanline polygon fill (solid and per-pixel
 * gradient), the gradient colour evaluator, and the elliptical-arc flatten
 * (shared with the `<path>` `A` command). Axis-aligned shapes keep the ra8_gfx
 * fast path; rotated / sheared shapes are mapped through the affine and
 * scanline-filled. No DOM, no heap. See reflow_svg_internal.h for the shared
 * geometry types and cross-TU helper contracts.
 *
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <math.h>

#include "ra8_attributes.h"
#include "ra8_gfx.h"
#include "reflow_svg_internal.h"

/** @brief 2*Pi, for wrapping an arc's signed sweep into range. */
static const float s_svg_2pi = 6.28318531F;

/* ===========================================================================
 * Basic shapes (rect / circle / line)
 * ===========================================================================
 */

/**
 * @brief Draw one SVG 'rect' element using its fill colour or gradient.
 *
 * @details Reads 'x', 'y', 'width', and 'height' attributes from the tag
 * span @p s[0..len). Axis-aligned solid rects use the @c ra8_gfx_rect
 * fast-path; under a rotating/shearing transform the 4 corners are mapped
 * through the full affine via @c priv_ra8_svgp_map_point and the resulting quad is
 * scanline-filled by @c priv_ra8_svgp_fill_poly or @c priv_ra8_svgp_fill_poly_grad.
 * Returns without drawing when both gi < 0 and fill == ::k_svg_no_paint.
 *
 * @param[in] s   Byte span of the element tag; must not be NULL.
 * @param[in] len Total valid bytes in @p s (the tag span length).
 * @param[in] t   Active coordinate transform; must not be NULL.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 *
 * @post All pixels inside the rect boundary are painted with the fill colour.
 * @post No pixels outside the rect boundary are modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
void priv_ra8_svgp_draw_rect(const uint8_t* s, size_t len, const svg_xform_t* t)
{
  int32_t        gi   = -1;
  const uint32_t fill = priv_ra8_svgp_resolve_fill(s, len, t, (uint32_t)k_svg_def_fill, &gi);
  if ((gi < 0) && (fill == (uint32_t)k_svg_no_paint)) {
    return;
  }
  const int32_t rx = priv_ra8_svgp_attr_num(s, len, "x", 0);
  const int32_t ry = priv_ra8_svgp_attr_num(s, len, "y", 0);
  const int32_t rw = priv_ra8_svgp_attr_num(s, len, "width", 0);
  const int32_t rh = priv_ra8_svgp_attr_num(s, len, "height", 0);
  if ((gi < 0) && !priv_ra8_svgp_has_rot(t)) {
    (void)ra8_gfx_rect(priv_ra8_svgp_mx(t, rx),
                       priv_ra8_svgp_my(t, ry),
                       priv_ra8_svgp_sx(t, rw),
                       priv_ra8_svgp_sx(t, rh),
                       fill,
                       true);
    return;
  }
  int32_t xs[k_svg_rect_pts] = {};
  int32_t ys[k_svg_rect_pts] = {};
  priv_ra8_svgp_map_point(t, rx, ry, &xs[0], &ys[0]);
  priv_ra8_svgp_map_point(t, rx + rw, ry, &xs[1], &ys[1]);
  priv_ra8_svgp_map_point(t, rx + rw, ry + rh, &xs[2], &ys[2]);
  priv_ra8_svgp_map_point(t, rx, ry + rh, &xs[3], &ys[3]);
  if (gi >= 0) {
    priv_ra8_svgp_fill_poly_grad(xs, ys, (int32_t)k_svg_rect_pts, &t->grads->g[gi]);
  } else {
    priv_ra8_svgp_fill_poly(xs, ys, (int32_t)k_svg_rect_pts, fill);
  }
}

/**
 * @brief Draw one SVG 'circle' element using its fill colour or gradient.
 *
 * @details Reads 'cx', 'cy', and 'r' attributes from the tag span @p s[0..len).
 * Axis-aligned solid circles use the @c ra8_gfx_circle fast-path; under a
 * rotating/shearing transform the circle is approximated by a @c k_svg_circle_seg
 * N-gon whose vertices are mapped through the full affine, then scanline-filled
 * by @c priv_ra8_svgp_fill_poly or @c priv_ra8_svgp_fill_poly_grad. Returns without drawing
 * when both gi < 0 and fill == ::k_svg_no_paint.
 *
 * @param[in] s   Byte span of the element tag; must not be NULL.
 * @param[in] len Total valid bytes in @p s (the tag span length).
 * @param[in] t   Active coordinate transform; must not be NULL.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 *
 * @post All pixels inside the circle boundary are painted with the fill colour.
 * @post No pixels outside the circle boundary are modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
void priv_ra8_svgp_draw_circle(const uint8_t* s, size_t len, const svg_xform_t* t)
{
  int32_t        gi   = -1;
  const uint32_t fill = priv_ra8_svgp_resolve_fill(s, len, t, (uint32_t)k_svg_def_fill, &gi);
  if ((gi < 0) && (fill == (uint32_t)k_svg_no_paint)) {
    return;
  }
  const int32_t cx = priv_ra8_svgp_attr_num(s, len, "cx", 0);
  const int32_t cy = priv_ra8_svgp_attr_num(s, len, "cy", 0);
  const int32_t r  = priv_ra8_svgp_attr_num(s, len, "r", 0);
  if ((gi < 0) && !priv_ra8_svgp_has_rot(t)) {
    (void)ra8_gfx_circle(priv_ra8_svgp_mx(t, cx),
                         priv_ra8_svgp_my(t, cy),
                         priv_ra8_svgp_sx(t, r),
                         fill,
                         true);
    return;
  }
  int32_t xs[k_svg_circle_seg] = {};
  int32_t ys[k_svg_circle_seg] = {};
  /* Bounded: k_svg_circle_seg vertices around the circle. */
  for (int32_t k = 0; k < (int32_t)k_svg_circle_seg; ++k) {
    const float ang = (s_svg_2pi * (float)k) / (float)k_svg_circle_seg;
    const float dx  = (float)r * cosf(ang);
    const float dy  = (float)r * sinf(ang);
    priv_ra8_svgp_map_point(t, cx + (int32_t)dx, cy + (int32_t)dy, &xs[k], &ys[k]);
  }
  if (gi >= 0) {
    priv_ra8_svgp_fill_poly_grad(xs, ys, (int32_t)k_svg_circle_seg, &t->grads->g[gi]);
  } else {
    priv_ra8_svgp_fill_poly(xs, ys, (int32_t)k_svg_circle_seg, fill);
  }
}

/**
 * @brief Draw one SVG 'line' element using its stroke colour.
 *
 * @details Reads 'stroke' paint, 'x1', 'y1', 'x2', and 'y2' attributes from
 * the tag span @p s[0..len). Both endpoints are mapped through the full affine
 * via @c priv_ra8_svgp_map_point and the result is forwarded to @c ra8_gfx_line.
 * Returns without drawing when 'stroke' is absent or parses to ::k_svg_no_paint.
 * Note: 'fill' is ignored for 'line' elements (SVG specification).
 *
 * @param[in] s   Byte span of the element tag; must not be NULL.
 * @param[in] len Total valid bytes in @p s (the tag span length).
 * @param[in] t   Active coordinate transform; must not be NULL.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 *
 * @post The line from (x1,y1) to (x2,y2) is drawn in the stroke colour.
 * @post No drawing occurs when the stroke colour is ::k_svg_no_paint.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
void priv_ra8_svgp_draw_line(const uint8_t* s, size_t len, const svg_xform_t* t)
{
  const uint32_t stroke = priv_ra8_svgp_attr_paint(s, len, "stroke", (uint32_t)k_svg_no_paint);
  if (stroke == (uint32_t)k_svg_no_paint) {
    return;
  }
  const int32_t x1  = priv_ra8_svgp_attr_num(s, len, "x1", 0);
  const int32_t y1  = priv_ra8_svgp_attr_num(s, len, "y1", 0);
  const int32_t x2  = priv_ra8_svgp_attr_num(s, len, "x2", 0);
  const int32_t y2  = priv_ra8_svgp_attr_num(s, len, "y2", 0);
  int32_t       fx1 = 0;
  int32_t       fy1 = 0;
  int32_t       fx2 = 0;
  int32_t       fy2 = 0;
  priv_ra8_svgp_map_point(t, x1, y1, &fx1, &fy1);
  priv_ra8_svgp_map_point(t, x2, y2, &fx2, &fy2);
  (void)ra8_gfx_line(fx1, fy1, fx2, fy2, stroke);
}

/* ===========================================================================
 * Polygon / polyline (a `points` list + a scanline polygon fill)
 * ===========================================================================
 */

/**
 * @brief Parse a 'points' attribute value into framebuffer-space vertices.
 *
 * @details Scans @p v[0..vlen) for whitespace/comma-separated x,y integer
 * pairs via @c priv_ra8_svgp_num, maps each pair through the full affine via
 * @c priv_ra8_svgp_map_point, and stores the result in @p xs[@p n] / @p ys[@p n].
 * Terminates when @c k_svg_poly_max vertices have been collected or the end
 * of the span is reached. Returns the count of vertex pairs filled.
 *
 * @param[in]  v    Byte span containing the 'points' value text; must not be NULL.
 * @param[in]  vlen Total valid bytes in @p v.
 * @param[in]  t    Active coordinate transform; must not be NULL.
 * @param[out] xs   Array of at least @c k_svg_poly_max int32_t for X coords; must not be NULL.
 * @param[out] ys   Array of at least @c k_svg_poly_max int32_t for Y coords; must not be NULL.
 *
 * @return int32_t Number of vertices filled in @p xs / @p ys.
 * @retval 0             No valid x,y pairs found.
 * @retval 1..k_svg_poly_max  Count of vertices parsed and mapped.
 *
 * @pre  @p v is a valid pointer to at least @p vlen bytes.
 * @pre  @p xs and @p ys are valid arrays of at least @c k_svg_poly_max elements.
 *
 * @post @p xs[0..return-1] and @p ys[0..return-1] hold framebuffer coordinates.
 * @post The return value is in [0, k_svg_poly_max].
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static int32_t
internal_parse_points(const uint8_t* v, size_t vlen, const svg_xform_t* t, int32_t* xs, int32_t* ys)
{
  size_t  k = 0U;
  int32_t n = 0;
  /* Bounded: <= k_svg_poly_max pairs; k advances past each number. */
  while (n < (int32_t)k_svg_poly_max) {
    while ((k < vlen) && (priv_ra8_svgp_ws((char)v[k]) || (v[k] == (uint8_t)','))) {
      ++k;
    }
    if (k >= vlen) {
      break;
    }
    const int32_t ux = priv_ra8_svgp_num(v, vlen, &k);
    const int32_t uy = priv_ra8_svgp_num(v, vlen, &k);
    priv_ra8_svgp_map_point(t, ux, uy, &xs[n], &ys[n]);
    ++n;
  }
  return n;
}

/**
 * @brief Sort @p m int32 values in @p a into ascending order using insertion sort.
 *
 * @details Uses the classic O(m^2) insertion sort, which is efficient for small
 * arrays (m is bounded by @c k_svg_poly_max scanline crossings per row). Works
 * in-place with a single temporary variable per outer iteration. Used by the
 * scanline fill algorithm to sort X-intercept coordinates before pairing them
 * for span fill.
 *
 * @param[in,out] a Array of @p m int32_t values to sort in place; must not be NULL.
 * @param[in]     m Count of valid elements in @p a; must be >= 0.
 *
 * @pre  @p a is a valid pointer to at least @p m elements.
 * @pre  @p m is in [0, k_svg_poly_max].
 *
 * @post @p a[0..m-1] are sorted in non-decreasing order.
 * @post The number of valid elements in @p a is unchanged.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_sort_i32(int32_t* a, int32_t m)
{
  /* Bounded: m <= k_svg_poly_max. */
  for (int32_t i = 1; i < m; ++i) {
    const int32_t key = a[i];
    int32_t       j   = i - 1;
    while ((j >= 0) && (a[j] > key)) {
      a[j + 1] = a[j];
      --j;
    }
    a[j + 1] = key;
  }
}

/**
 * @brief Collect framebuffer X coordinates where polygon edges cross scanline @p y.
 *
 * @details Iterates over all @p n edges of the polygon. For each edge from
 * vertex @c i to vertex @c (i+1)%n, checks whether scanline @p y passes through
 * it (using the half-open interval [y0,y1) or [y1,y0)). For crossing edges,
 * computes the X intercept via linear interpolation with 64-bit arithmetic and
 * stores it in @p xint. Stops early if @c k_svg_poly_max crossings are found.
 *
 * @param[in]  xs   Array of @p n polygon vertex X coordinates (framebuffer); must not be NULL.
 * @param[in]  ys   Array of @p n polygon vertex Y coordinates (framebuffer); must not be NULL.
 * @param[in]  n    Number of polygon vertices.
 * @param[in]  y    Scanline Y coordinate to test.
 * @param[out] xint Array of at least @c k_svg_poly_max int32_t for crossing X values; must not be NULL.
 *
 * @return int32_t Number of edge crossings written to @p xint.
 * @retval 0         No edges cross scanline @p y.
 * @retval m         @p m intercept X values in @p xint[0..m-1], unsorted.
 *
 * @pre  @p xs and @p ys are valid pointers to at least @p n elements.
 * @pre  @p xint is a valid array of at least @c k_svg_poly_max elements.
 *
 * @post @p xint[0..return-1] hold the X coordinates of edge-scanline intersections.
 * @post The return value is in [0, k_svg_poly_max].
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static int32_t
internal_scanline_x(const int32_t* xs, const int32_t* ys, int32_t n, int32_t y, int32_t* xint)
{
  int32_t m = 0;
  /* Bounded: one test per polygon edge (n edges). */
  /* mcdc-deactivated: the polygon point count n is itself capped at k_svg_poly_max by the point parser, so the crossing count m (<= edges processed <= i < n <= k_svg_poly_max) can never reach k_svg_poly_max before i exhausts n; (m < k_svg_poly_max) is invariantly true and its false arm is unreachable. */
  for (int32_t i = 0; (i < n) && (m < (int32_t)k_svg_poly_max); ++i) {
    const int32_t j  = (i + 1) % n;
    const int32_t y0 = ys[i];
    const int32_t y1 = ys[j];
    /* mcdc-deactivated: the fourth condition (y < y0) is the exact boolean negation of the first (y0 <= y), so it cannot be flipped independently of C1; the C4 independence pair is structurally unreachable. */
    if (((y0 <= y) && (y < y1)) || ((y1 <= y) && (y < y0))) {
      const int32_t x0      = xs[i];
      const int32_t x1      = xs[j];
      const int64_t y_delta = (int64_t)y - (int64_t)y0;
      const int64_t x_delta = (int64_t)x1 - (int64_t)x0;
      const int64_t divisor = (int64_t)y1 - (int64_t)y0;
      const int64_t offset  = (y_delta * x_delta) / divisor;
      xint[m]               = x0 + (int32_t)offset;
      ++m;
    }
  }
  return m;
}

/**
 * @brief Even-odd scanline fill of polygon (xs,ys)[0..n) with a solid colour.
 *
 * @details Computes the bounding Y range of the polygon, then for each
 * scanline Y in [ymin, ymax] calls @c internal_scanline_x to collect intercepts,
 * sorts them via @c internal_sort_i32, and fills pairs of spans with @c ra8_gfx_line.
 * Returns immediately for polygons with fewer than @c k_svg_poly_min vertices.
 *
 * @param[in] xs    Array of @p n framebuffer X coordinates; must not be NULL.
 * @param[in] ys    Array of @p n framebuffer Y coordinates; must not be NULL.
 * @param[in] n     Number of polygon vertices.
 * @param[in] color Fill colour in 0x00RRGGBB format.
 *
 * @pre  @p xs and @p ys are valid pointers to at least @p n elements.
 * @pre  @p n <= @c k_svg_poly_max.
 *
 * @post All pixels inside the even-odd polygon boundary are painted @p color.
 * @post No drawing occurs when @p n < @c k_svg_poly_min.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
void priv_ra8_svgp_fill_poly(const int32_t* xs, const int32_t* ys, int32_t n, uint32_t color)
{
  if (n < (int32_t)k_svg_poly_min) {
    return;
  }
  int32_t ymin = ys[0];
  int32_t ymax = ys[0];
  for (int32_t i = 1; i < n; ++i) {
    ymin = (ys[i] < ymin) ? ys[i] : ymin;
    ymax = (ys[i] > ymax) ? ys[i] : ymax;
  }
  /* Bounded: ymax - ymin <= the framebuffer-box height. */
  for (int32_t y = ymin; y <= ymax; ++y) {
    int32_t       xint[k_svg_poly_max] = {};
    const int32_t m                    = internal_scanline_x(xs, ys, n, y, xint);
    internal_sort_i32(xint, m);
    for (int32_t k = 0; (k + 1) < m; k += 2) {
      (void)ra8_gfx_line(xint[k], y, xint[k + 1], y, color);
    }
  }
}

/**
 * @brief Linear-interpolate two 0x00RRGGBB colours by factor @p f in [0, 1].
 *
 * @details Extracts the R, G, and B channels from @p a and @p b, interpolates
 * each independently as @c (channel_a + (channel_b - channel_a) * f), and
 * recombines into a 0x00RRGGBB result. The factor @p f is not clamped by this
 * function; the gradient evaluator is responsible for providing a value in [0,1].
 * Used by @c internal_grad_eval to blend adjacent gradient stops.
 *
 * @param[in] a Colour A in 0x00RRGGBB format.
 * @param[in] b Colour B in 0x00RRGGBB format.
 * @param[in] f Interpolation factor; 0.0F returns @p a, 1.0F returns @p b.
 *
 * @return uint32_t The interpolated colour in 0x00RRGGBB format.
 * @retval 0x000000..0xFFFFFF  The per-channel linear blend of @p a and @p b at @p f.
 *
 * @pre  @p f is in the range [0.0F, 1.0F] to produce a value between @p a and @p b.
 * @pre  @p a and @p b have bits [31:24] clear (0x00RRGGBB format).
 *
 * @post The return value has bits [31:24] clear.
 * @post Each channel of the return value is clamped to [0, 255] by construction.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_col_lerp(uint32_t a, uint32_t b, float f)
{
  const uint32_t ra_bits = (a >> (uint32_t)k_svg_sh_r) & (uint32_t)k_svg_chan_mask;
  const uint32_t ga_bits = (a >> (uint32_t)k_svg_hex_chan) & (uint32_t)k_svg_chan_mask;
  const uint32_t ba_bits = a & (uint32_t)k_svg_chan_mask;
  const uint32_t rb_bits = (b >> (uint32_t)k_svg_sh_r) & (uint32_t)k_svg_chan_mask;
  const uint32_t gb_bits = (b >> (uint32_t)k_svg_hex_chan) & (uint32_t)k_svg_chan_mask;
  const uint32_t bb_bits = b & (uint32_t)k_svg_chan_mask;
  const int32_t  ra      = (int32_t)ra_bits;
  const int32_t  ga      = (int32_t)ga_bits;
  const int32_t  ba      = (int32_t)ba_bits;
  const int32_t  rb      = (int32_t)rb_bits;
  const int32_t  gb      = (int32_t)gb_bits;
  const int32_t  bb      = (int32_t)bb_bits;
  const int32_t  rd      = rb - ra;
  const int32_t  gd      = gb - ga;
  const int32_t  bd      = bb - ba;
  const int32_t  r       = ra + (int32_t)((float)rd * f);
  const int32_t  g       = ga + (int32_t)((float)gd * f);
  const int32_t  bl      = ba + (int32_t)((float)bd * f);
  return ((uint32_t)r << (uint32_t)k_svg_sh_r) | ((uint32_t)g << (uint32_t)k_svg_hex_chan) |
         (uint32_t)bl;
}

/**
 * @brief Evaluate gradient @p g at bbox-relative point (@p px, @p py).
 *
 * @details For a linear gradient the scalar parameter is the projection of
 * (@p px - x1, @p py - y1) onto the vector (x2 - x1, y2 - y1) normalised by
 * the squared length of that vector. For a radial gradient the parameter is
 * the Euclidean distance from (x1, y1) divided by radius x2. The parameter
 * is compared against stop offsets; values before the first stop return the
 * first stop colour, values after the last return the last stop colour, and
 * values between two stops are linearly interpolated by @c internal_col_lerp.
 * Returns the default fill colour when @p g->nstops is zero.
 *
 * @param[in] g   Gradient to evaluate; must not be NULL and must have nstops >= 1.
 * @param[in] px  Bounding-box-relative X position in [0, 1].
 * @param[in] py  Bounding-box-relative Y position in [0, 1].
 *
 * @return uint32_t The evaluated colour in 0x00RRGGBB format.
 * @retval (uint32_t)k_svg_def_fill  When @p g->nstops is 0.
 * @retval 0x000000..0xFFFFFF        Interpolated or clamped stop colour.
 *
 * @pre  @p g is a valid non-NULL pointer to an initialised @c svg_grad_t.
 * @pre  @p px and @p py are bounding-box-relative coordinates (typically in [0,1]).
 *
 * @post The return value has bits [31:24] clear (0x00RRGGBB format).
 * @post @p g is not modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_grad_eval(const svg_grad_t* g, float px, float py)
{
  if (g->nstops == 0U) {
    return (uint32_t)k_svg_def_fill;
  }
  float p = 0.0F;
  if (g->kind == (uint8_t)k_svg_grad_radial) {
    const float dx = px - g->x1;
    const float dy = py - g->y1;
    const float rr = (g->x2 > 0.0F) ? g->x2 : 1.0F;
    p              = sqrtf((dx * dx) + (dy * dy)) / rr;
  } else {
    const float vx   = g->x2 - g->x1;
    const float vy   = g->y2 - g->y1;
    const float len2 = (vx * vx) + (vy * vy);
    p                = (len2 > 0.0F) ? ((((px - g->x1) * vx) + ((py - g->y1) * vy)) / len2) : 0.0F;
  }
  if (p <= g->stops[0].off) {
    return g->stops[0].col;
  }
  const uint8_t last = (uint8_t)(g->nstops - 1U);
  if (p >= g->stops[last].off) {
    return g->stops[last].col;
  }
  /* Bounded: at most nstops-1 stop brackets. */
  for (uint8_t i = 0U; i < last; ++i) {
    const float o0 = g->stops[i].off;
    const float o1 = g->stops[i + 1U].off;
    /* mcdc-deactivated: the loop is entered only when p > stops[0].off, and reaching bracket i without returning requires p to exceed every earlier stop's upper offset, so p >= o0 (== stops[i].off) invariantly holds regardless of stop ordering; (p >= o0) cannot be flipped false. */
    if ((p >= o0) && (p <= o1)) {
      const float f = (o1 > o0) ? ((p - o0) / (o1 - o0)) : 0.0F;
      return internal_col_lerp(g->stops[i].col, g->stops[i + 1U].col, f);
    }
  }
  return g->stops[last].col;
}

/**
 * @brief Per-pixel gradient scanline fill of polygon (xs,ys)[0..n).
 *
 * @details Computes the bounding box of the polygon, then for each scanline Y
 * in [ymin, ymax] collects edge intercepts via @c internal_scanline_x and sorts
 * them. For each filled span, iterates over pixels and evaluates the gradient
 * at the bounding-box-relative position @c ((x-xmin)/bw, (y-ymin)/bh) via
 * @c internal_grad_eval. Each pixel is written individually with @c ra8_gfx_pixel.
 * Returns immediately when @p n < @c k_svg_poly_min.
 *
 * @param[in] xs Array of @p n framebuffer X coordinates; must not be NULL.
 * @param[in] ys Array of @p n framebuffer Y coordinates; must not be NULL.
 * @param[in] n  Number of polygon vertices.
 * @param[in] g  Gradient descriptor to evaluate per pixel; must not be NULL.
 *
 * @pre  @p xs and @p ys are valid pointers to at least @p n elements.
 * @pre  @p g is a valid non-NULL pointer with @p g->nstops >= 1.
 *
 * @post All pixels inside the even-odd polygon boundary are painted with
 *       the gradient colour evaluated at their bounding-box-relative position.
 * @post No drawing occurs when @p n < @c k_svg_poly_min.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
void priv_ra8_svgp_fill_poly_grad(const int32_t*    xs,
                                  const int32_t*    ys,
                                  int32_t           n,
                                  const svg_grad_t* g)
{
  if (n < (int32_t)k_svg_poly_min) {
    return;
  }
  int32_t xmin = xs[0];
  int32_t xmax = xs[0];
  int32_t ymin = ys[0];
  int32_t ymax = ys[0];
  for (int32_t i = 1; i < n; ++i) {
    xmin = (xs[i] < xmin) ? xs[i] : xmin;
    xmax = (xs[i] > xmax) ? xs[i] : xmax;
    ymin = (ys[i] < ymin) ? ys[i] : ymin;
    ymax = (ys[i] > ymax) ? ys[i] : ymax;
  }
  const float bw = (xmax > xmin) ? (float)((int64_t)xmax - (int64_t)xmin) : 1.0F;
  const float bh = (ymax > ymin) ? (float)((int64_t)ymax - (int64_t)ymin) : 1.0F;
  /* Bounded: ymax - ymin <= the framebuffer-box height. */
  for (int32_t y = ymin; y <= ymax; ++y) {
    int32_t       xint[k_svg_poly_max] = {};
    const int32_t m                    = internal_scanline_x(xs, ys, n, y, xint);
    internal_sort_i32(xint, m);
    const float py = (float)((int64_t)y - (int64_t)ymin) / bh;
    for (int32_t k = 0; (k + 1) < m; k += 2) {
      /* Bounded: xint[k+1] - xint[k] <= the framebuffer-box width. */
      for (int32_t x = xint[k]; x <= xint[k + 1]; ++x) {
        const float px = (float)((int64_t)x - (int64_t)xmin) / bw;
        (void)ra8_gfx_pixel(x, y, internal_grad_eval(g, px, py));
      }
    }
  }
}

/**
 * @brief Draw one SVG 'polygon' element as a filled polygon.
 *
 * @details Resolves the fill (solid or gradient), reads the 'points' attribute,
 * parses vertices into framebuffer space via @c internal_parse_points, and fills
 * the resulting polygon with @c priv_ra8_svgp_fill_poly or @c priv_ra8_svgp_fill_poly_grad.
 * Returns without drawing when the fill is absent/none or the 'points'
 * attribute is not present.
 *
 * @param[in] s   Byte span of the element tag; must not be NULL.
 * @param[in] len Total valid bytes in @p s.
 * @param[in] t   Active coordinate transform; must not be NULL.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 *
 * @post All pixels inside the polygon boundary are painted with the fill colour.
 * @post No drawing occurs when fill is absent or 'points' is missing.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
void priv_ra8_svgp_draw_polygon(const uint8_t* s, size_t len, const svg_xform_t* t)
{
  int32_t        gi   = -1;
  const uint32_t fill = priv_ra8_svgp_resolve_fill(s, len, t, (uint32_t)k_svg_def_fill, &gi);
  size_t         off  = 0U;
  size_t         vl   = 0U;
  if (((gi < 0) && (fill == (uint32_t)k_svg_no_paint)) ||
      !priv_ra8_svgp_attr(s, len, "points", &off, &vl)) {
    return;
  }
  int32_t       xs[k_svg_poly_max] = {};
  int32_t       ys[k_svg_poly_max] = {};
  const int32_t n                  = internal_parse_points(&s[off], vl, t, xs, ys);
  if (gi >= 0) {
    priv_ra8_svgp_fill_poly_grad(xs, ys, n, &t->grads->g[gi]);
  } else {
    priv_ra8_svgp_fill_poly(xs, ys, n, fill);
  }
}

/**
 * @brief Draw one SVG 'polyline' element as connected stroke line segments.
 *
 * @details Reads the 'stroke' paint and the 'points' attribute. Parses vertex
 * pairs into framebuffer space via @c internal_parse_points and connects adjacent
 * vertices with @c ra8_gfx_line. Returns without drawing when the stroke colour
 * is absent/none or the 'points' attribute is not present.
 *
 * @param[in] s   Byte span of the element tag; must not be NULL.
 * @param[in] len Total valid bytes in @p s.
 * @param[in] t   Active coordinate transform; must not be NULL.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 *
 * @post All (n-1) line segments between adjacent vertices are drawn in the stroke colour.
 * @post No drawing occurs when stroke is absent or 'points' is missing.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
void priv_ra8_svgp_draw_polyline(const uint8_t* s, size_t len, const svg_xform_t* t)
{
  const uint32_t stroke = priv_ra8_svgp_attr_paint(s, len, "stroke", (uint32_t)k_svg_no_paint);
  size_t         off    = 0U;
  size_t         vl     = 0U;
  if ((stroke == (uint32_t)k_svg_no_paint) || !priv_ra8_svgp_attr(s, len, "points", &off, &vl)) {
    return;
  }
  int32_t       xs[k_svg_poly_max] = {};
  int32_t       ys[k_svg_poly_max] = {};
  const int32_t n                  = internal_parse_points(&s[off], vl, t, xs, ys);
  /* Bounded: n - 1 segments. */
  for (int32_t i = 0; (i + 1) < n; ++i) {
    (void)ra8_gfx_line(xs[i], ys[i], xs[i + 1], ys[i + 1], stroke);
  }
}

/**
 * @brief Compute the absolute value of a float without using libm @c fabsf.
 *
 * @details Returns @c -v when @p v is negative, otherwise returns @p v unchanged.
 * Used in @c internal_arc_center and @c internal_arc_segs to avoid dragging in the full
 * libm dependency for a trivial operation that the compiler can inline.
 *
 * @param[in] v Float value whose absolute value is needed.
 *
 * @return float The absolute value of @p v.
 * @retval -v  When @p v < 0.0F.
 * @retval v   When @p v >= 0.0F.
 *
 * @pre  @p v is any finite float value (NaN and infinity return themselves).
 * @pre  No external state is required.
 *
 * @post The return value is >= 0.0F for all finite @p v.
 * @post @p v is not modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static float internal_absf(float v)
{
  return (v < 0.0F) ? -v : v;
}

/**
 * @struct svg_arc_t
 * @brief Centre-parametrisation of an SVG elliptical arc.
 *
 * @details Result of ::internal_arc_center -- the centre, the (radius-corrected)
 * semi-axes, the x-axis rotation (as cos/sin), and the start angle + signed
 * sweep, ready to sample. @c ok is false for a degenerate arc (zero radius or
 * coincident endpoints), in which case the caller draws a straight line.
 */
typedef struct {
  float cx;      /**< Centre X (user space).             */
  float cy;      /**< Centre Y (user space).             */
  float rx;      /**< Corrected semi-axis X.             */
  float ry;      /**< Corrected semi-axis Y.             */
  float cos_phi; /**< cos(x-axis rotation).              */
  float sin_phi; /**< sin(x-axis rotation).              */
  float t1;      /**< Start angle, radians.              */
  float dt;      /**< Signed sweep, radians.             */
  bool  ok;      /**< False => degenerate (draw a line). */
} svg_arc_t;

/**
 * @brief Solve an arc's centre + start/sweep angles (F.6.5.2 / F.6.5.5-6).
 *
 * @details Finishes @c internal_arc_center from its rotated half-chord @p (x1p,y1p),
 * the (radius-corrected) semi-axes @p (rx,ry), and the endpoint midpoint
 * @p (mx,my). Fills @p a's centre, start angle, signed sweep, radii, and sets
 * @p a->ok to true. The @c cos_phi / @c sin_phi fields of @p a must already be
 * set by the caller. The signed sweep @p a->dt is adjusted so its direction is
 * consistent with @p sweep (positive angle) and @p large (large-arc) flags.
 *
 * @param[in,out] a     Arc struct; centre, angles, radii, and ok are written.
 * @param[in]     x1p   Rotated half-chord X (F.6.5.1, user space).
 * @param[in]     y1p   Rotated half-chord Y (F.6.5.1, user space).
 * @param[in]     rx    Corrected semi-axis X.
 * @param[in]     ry    Corrected semi-axis Y.
 * @param[in]     mx    Endpoint midpoint X (user space).
 * @param[in]     my    Endpoint midpoint Y (user space).
 * @param[in]     large Large-arc flag from the 'A' command.
 * @param[in]     sweep Sweep (positive-angle) flag from the 'A' command.
 *
 * @pre  @p a is a valid non-NULL pointer with @c cos_phi / @c sin_phi already set.
 * @pre  @p rx > 0.0F and @p ry > 0.0F (degenerate radii handled by the caller).
 *
 * @post @p a->ok is set to true.
 * @post @p a->cx, @p a->cy, @p a->t1, @p a->dt, @p a->rx, @p a->ry are valid.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_arc_solve(svg_arc_t* a,
                               float      x1p,
                               float      y1p,
                               float      rx,
                               float      ry,
                               float      mx,
                               float      my,
                               bool       large,
                               bool       sweep)
{
  const float rx2 = rx * rx;
  const float ry2 = ry * ry;
  const float num = (rx2 * ry2) - (rx2 * y1p * y1p) - (ry2 * x1p * x1p);
  const float den = (rx2 * y1p * y1p) + (ry2 * x1p * x1p);
  float       co  = sqrtf((num > 0.0F) ? (num / den) : 0.0F);
  if (large == sweep) {
    co = -co;
  }
  const float cxp = co * ((rx * y1p) / ry);
  const float cyp = -co * ((ry * x1p) / rx);
  a->cx           = (a->cos_phi * cxp) - (a->sin_phi * cyp) + mx;
  a->cy           = (a->sin_phi * cxp) + (a->cos_phi * cyp) + my;
  a->t1           = atan2f((y1p - cyp) / ry, (x1p - cxp) / rx);
  float dt        = atan2f((-y1p - cyp) / ry, (-x1p - cxp) / rx) - a->t1;
  if (!sweep && (dt > 0.0F)) {
    dt -= s_svg_2pi;
  } else if (sweep && (dt < 0.0F)) {
    dt += s_svg_2pi;
  } else {
    /* The sweep already has the requested direction. */
  }
  a->dt = dt;
  a->rx = rx;
  a->ry = ry;
  a->ok = true;
}

/**
 * @brief Convert an SVG endpoint-parametrised arc to centre parametrisation.
 *
 * @details Implements the SVG 1.1 implementation-notes F.6.5 algorithm:
 * out-of-range radii are scaled up (F.6.6.2), the centre is solved via
 * @c internal_arc_solve (F.6.5.2), and the start angle plus signed sweep are
 * derived (F.6.5.5/6) honouring the large-arc and sweep flags. Pure
 * (no MMIO/heap); uses libm @c cosf, @c sinf, @c sqrtf.
 *
 * @param[in] p0      Arc start (current point, user space).
 * @param[in] p_end   Arc end (user space, already absolute).
 * @param[in] rx_in   Requested semi-axis X (its absolute value is used).
 * @param[in] ry_in   Requested semi-axis Y (its absolute value is used).
 * @param[in] rot_deg X-axis rotation in degrees.
 * @param[in] large   Large-arc flag.
 * @param[in] sweep   Sweep (positive-angle) flag.
 *
 * @return svg_arc_t The centre parametrisation of the arc.
 * @retval {.ok=false}  Zero radius, or @p p0 and @p p_end are coincident.
 * @retval {.ok=true}   All fields valid: centre, semi-axes, start angle, sweep.
 *
 * @pre  @p rx_in and @p ry_in fit in a float without overflow (SVG coords are bounded).
 * @pre  @p p0 and @p p_end are valid user-space points.
 *
 * @post When `ok == true`, the fields `cx`, `cy`, `rx`, `ry`, `t1`, and `dt`
 *       are all valid.
 * @post When `ok == false`, no other field in the returned struct is
 *       meaningful.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static svg_arc_t internal_arc_center(svg_pt_t p0,
                                     svg_pt_t p_end,
                                     int32_t  rx_in,
                                     int32_t  ry_in,
                                     int32_t  rot_deg,
                                     bool     large,
                                     bool     sweep)
{
  static const float k_pi       = 3.14159265F;
  static const float k_deg_half = 180.0F;
  static const float k_half     = 0.5F;
  svg_arc_t          a          = {};
  float              rx         = internal_absf((float)rx_in);
  float              ry         = internal_absf((float)ry_in);
  if ((rx == 0.0F) || (ry == 0.0F) || ((p0.x == p_end.x) && (p0.y == p_end.y))) {
    a.ok = false;
    return a;
  }
  const float phi = (float)rot_deg * (k_pi / k_deg_half);
  a.cos_phi       = cosf(phi);
  a.sin_phi       = sinf(phi);
  const float dx  = (float)((int64_t)p0.x - (int64_t)p_end.x) * k_half;
  const float dy  = (float)((int64_t)p0.y - (int64_t)p_end.y) * k_half;
  const float x1p = (a.cos_phi * dx) + (a.sin_phi * dy);
  const float y1p = -(a.sin_phi * dx) + (a.cos_phi * dy);
  const float lam = ((x1p * x1p) / (rx * rx)) + ((y1p * y1p) / (ry * ry));
  if (lam > 1.0F) {
    const float s = sqrtf(lam);
    rx *= s;
    ry *= s;
  }
  const float mx = (float)((int64_t)p0.x + (int64_t)p_end.x) * k_half;
  const float my = (float)((int64_t)p0.y + (int64_t)p_end.y) * k_half;
  internal_arc_solve(&a, x1p, y1p, rx, ry, mx, my, large, sweep);
  return a;
}

/**
 * @brief Compute a bounded segment count for an arc of signed sweep @p dt.
 *
 * @details Divides the absolute value of @p dt by the target radians-per-segment
 * function-local arc step (~pi/8) and adds 1 to ensure at least one segment.
 * The result is clamped to @c k_svg_arc_seg_max (24) to keep the vertex count
 * bounded regardless of the sweep angle.
 *
 * @param[in] dt Signed arc sweep in radians; negative values are valid.
 *
 * @return int32_t Number of segments to use when flattening the arc.
 * @retval 1..k_svg_arc_seg_max  Always within this range.
 *
 * @pre  @p dt is a finite float; the absolute value is used for the calculation.
 * @pre  @p dt is finite so division by the positive local arc step is defined.
 *
 * @post The return value is in [1, k_svg_arc_seg_max].
 * @post @p dt is not modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static int32_t internal_arc_segs(float dt)
{
  static const float k_arc_step = 0.39269908F;
  int32_t            segs       = (int32_t)(internal_absf(dt) / k_arc_step) + 1;
  if (segs > (int32_t)k_svg_arc_seg_max) {
    segs = (int32_t)k_svg_arc_seg_max;
  }
  return segs;
}

/**
 * @brief Flatten an SVG elliptical arc into the polygon vertex array.
 *
 * @details Converts the endpoint-parametrised arc (P0 to @p p_end, with
 * radii, rotation, and flags from @p args) to centre form via @c internal_arc_center.
 * When the arc is degenerate (@c ok==false), appends a single line-to @p p_end.
 * Otherwise samples the arc at @c internal_arc_segs(@p a.dt) parameter values using
 * trigonometry (the ellipse equation in the rotated frame) and appends each
 * mapped framebuffer point. Bounded by @c k_svg_poly_max.
 *
 * @param[in]     t      Active coordinate transform; must not be NULL.
 * @param[in]     p0     Arc start point (current path position, user space).
 * @param[in]     args   Arc argument array indexed by @c svg_arc_* constants; must not be NULL.
 * @param[in]     p_end  Arc end point (absolute, user space).
 * @param[out]    xs     Vertex X array of at least @c k_svg_poly_max elements; must not be NULL.
 * @param[out]    ys     Vertex Y array of at least @c k_svg_poly_max elements; must not be NULL.
 * @param[in,out] n      Current vertex count; updated by this function.
 *
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @pre  @p args has at least @c k_svg_path_args (7) valid int32_t entries.
 *
 * @post @p *n is increased by the number of arc sample points appended.
 * @post @p *n <= @c k_svg_poly_max on exit.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
void priv_ra8_svgp_flatten_arc(const svg_xform_t* t,
                               svg_pt_t           p0,
                               const int32_t*     args,
                               svg_pt_t           p_end,
                               int32_t*           xs,
                               int32_t*           ys,
                               int32_t*           n)
{
  const svg_arc_t a = internal_arc_center(p0,
                                          p_end,
                                          args[k_svg_arc_rx],
                                          args[k_svg_arc_ry],
                                          args[k_svg_arc_rot],
                                          args[k_svg_arc_large] != 0,
                                          args[k_svg_arc_sweep] != 0);
  if (!a.ok) {
    if (*n < (int32_t)k_svg_poly_max) {
      priv_ra8_svgp_map_point(t, p_end.x, p_end.y, &xs[*n], &ys[*n]);
      ++(*n);
    }
    return;
  }
  const int32_t segs = internal_arc_segs(a.dt);
  /* Bounded: segs (<= k_svg_arc_seg_max) samples, capped by k_svg_poly_max. */
  for (int32_t j = 1; (j <= segs) && (*n < (int32_t)k_svg_poly_max); ++j) {
    const float th = a.t1 + (a.dt * ((float)j / (float)segs));
    const float ex = a.rx * cosf(th);
    const float ey = a.ry * sinf(th);
    const float px = a.cx + (a.cos_phi * ex) - (a.sin_phi * ey);
    const float py = a.cy + (a.sin_phi * ex) + (a.cos_phi * ey);
    priv_ra8_svgp_map_point(t, (int32_t)px, (int32_t)py, &xs[*n], &ys[*n]);
    ++(*n);
  }
}
