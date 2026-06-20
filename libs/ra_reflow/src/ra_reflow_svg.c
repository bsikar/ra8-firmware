/**
 * @file ra_reflow_svg.c
 * @brief Implementation of the minimal SVG subset (#112).
 *
 * @details Pure string-scanning SVG parser + a `<rect>`/`<circle>`/`<line>`/
 * `<polygon>`/`<polyline>`/`<path>` rasteriser over ra_gfx (with a scanline
 * polygon fill). No DOM, no heap. See ra_reflow_svg.h for the scope.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 *
 * @since 0.1.0
 */

#include "ra_reflow_svg.h"

#include <string.h>

#include "ra_gfx.h"

/**
 * @enum priv_svg_consts_t
 * @brief Parse + colour constants for the SVG subset.
 */
typedef enum : uint32_t {
  k_svg_no_paint  = 0xFFFFFFFFU, /**< `none` / absent paint sentinel.    */
  k_svg_def_fill  = 0x000000U,   /**< SVG default fill is black.         */
  k_svg_dec       = 10U,         /**< Decimal base.                      */
  k_svg_hex_base  = 16U,         /**< Hex base / "not a hex digit".      */
  k_svg_hex_a10   = 10U,         /**< Value of hex 'a'/'A'.              */
  k_svg_hex_nib   = 4U,          /**< Bits per hex nibble.               */
  k_svg_hex_chan  = 8U,          /**< Bits per colour channel.           */
  k_svg_hex3      = 3U,          /**< `#rgb` digit count.                */
  k_svg_hex6      = 6U,          /**< `#rrggbb` digit count.             */
  k_svg_viewbox_n = 4U,          /**< `viewBox` number count.            */
  k_svg_bom0      = 0xEFU,       /**< UTF-8 BOM byte 0.                  */
  k_svg_bom1      = 0xBBU,       /**< UTF-8 BOM byte 1.                  */
  k_svg_bom2      = 0xBFU,       /**< UTF-8 BOM byte 2.                  */
  k_svg_bom_len   = 3U,          /**< UTF-8 BOM length.                 */
  k_svg_poly_max  = 64U,         /**< Max points / scanline crossings.  */
  k_svg_poly_min  = 3U,          /**< Min points for a fillable polygon.*/
  k_svg_path_args = 7U,          /**< Max args of a path command (`A`). */
  k_svg_path_ep   = 2U,          /**< Endpoint pair size (x, y); M/L/T. */
  k_svg_argc_quad = 4U,          /**< Arg count of `S` / `Q`.          */
  k_svg_argc_cube = 6U,          /**< Arg count of `C`.                */
  k_svg_curve_seg = 12U,         /**< Segments per cubic-Bezier flatten.*/
  k_svg_cube_ey   = 5U,          /**< `C` endpoint-y argument index.   */
} priv_svg_consts_t;

/** @brief Cubic Bernstein middle coefficient (3) for the Bezier flatten. */
static const float s_svg_bez3 = 3.0F;
/** @brief Quadratic Bernstein middle coefficient (2) for the Bezier flatten. */
static const float s_svg_bez2 = 2.0F;

/**
 * @struct svg_pt_t
 * @brief A 2-D point in SVG user space (an x/y pair).
 */
typedef struct {
  int32_t x; /**< X coordinate. */
  int32_t y; /**< Y coordinate. */
} svg_pt_t;

/**
 * @struct svg_xform_t
 * @brief SVG-user-space -> framebuffer-box coordinate transform.
 */
typedef struct {
  int32_t bx; /**< Box left.        */
  int32_t by; /**< Box top.         */
  int32_t bw; /**< Box width.       */
  int32_t bh; /**< Box height.      */
  int32_t vx; /**< viewBox min-x.   */
  int32_t vy; /**< viewBox min-y.   */
  int32_t vw; /**< viewBox width.   */
  int32_t vh; /**< viewBox height.  */
} svg_xform_t;

/* ===========================================================================
 * Pure string / number helpers
 * ===========================================================================
 */

/** @brief True for XML whitespace. */
static bool priv_ws(char c)
{
  return (c == ' ') || (c == '\t') || (c == '\n') || (c == '\r') || (c == '\f');
}

/** @brief ASCII-fold to lower case. */
static char priv_lc(char c)
{
  return ((c >= 'A') && (c <= 'Z')) ? (char)(c + ('a' - 'A')) : c;
}

/** @brief True iff @p s[at..] begins (case-insensitively) with @p lit. */
static bool priv_starts_ci(const uint8_t* s, size_t len, size_t at, const char* lit)
{
  const size_t n = strlen(lit);
  if ((at + n) > len) {
    return false;
  }
  for (size_t k = 0U; k < n; ++k) {
    if (priv_lc((char)s[at + k]) != priv_lc(lit[k])) {
      return false;
    }
  }
  return true;
}

/** @brief Index of the first case-insensitive occurrence of @p lit, or @p len. */
static size_t priv_find_ci(const uint8_t* s, size_t len, size_t from, const char* lit)
{
  const size_t n = strlen(lit);
  if (n == 0U) {
    return len;
  }
  size_t i = from;
  /* Bounded: i advances by 1 each step, capped by len - n + 1. */
  while ((i + n) <= len) {
    if (priv_starts_ci(s, len, i, lit)) {
      return i;
    }
    ++i;
  }
  return len;
}

/** @brief Hex value of @p c, or k_svg_hex_base if not a hex digit. */
static uint8_t priv_hex(char c)
{
  if ((c >= '0') && (c <= '9')) {
    return (uint8_t)(c - '0');
  }
  const char l = priv_lc(c);
  if ((l >= 'a') && (l <= 'f')) {
    return (uint8_t)((l - 'a') + (int)k_svg_hex_a10);
  }
  return (uint8_t)k_svg_hex_base;
}

/**
 * @brief Parse the integer part of one SVG number at @p s[*i], skipping leading
 *        separators (whitespace / commas); truncates any fraction.
 */
static int32_t priv_num(const uint8_t* s, size_t len, size_t* i)
{
  while ((*i < len) && (priv_ws((char)s[*i]) || (s[*i] == ','))) {
    ++(*i);
  }
  int32_t sign = 1;
  if ((*i < len) && ((s[*i] == '-') || (s[*i] == '+'))) {
    sign = (s[*i] == '-') ? -1 : 1;
    ++(*i);
  }
  int32_t v = 0;
  while ((*i < len) && (s[*i] >= '0') && (s[*i] <= '9')) {
    v = (v * (int32_t)k_svg_dec) + (int32_t)(s[*i] - '0');
    ++(*i);
  }
  /* Skip a fractional part (truncated). */
  if ((*i < len) && (s[*i] == '.')) {
    ++(*i);
    while ((*i < len) && (s[*i] >= '0') && (s[*i] <= '9')) {
      ++(*i);
    }
  }
  return v * sign;
}

/* ===========================================================================
 * Attribute + colour parsing
 * ===========================================================================
 */

/** @brief True iff attribute @p name begins at @p s[at] on a word boundary. */
static bool priv_attr_at(const uint8_t* s, size_t len, size_t at, const char* name)
{
  const size_t n = strlen(name);
  if (!priv_starts_ci(s, len, at, name)) {
    return false;
  }
  if ((at > 0U) && !priv_ws((char)s[at - 1U])) {
    return false; /* must be preceded by whitespace */
  }
  const size_t after = at + n;
  if (after >= len) {
    return false;
  }
  return (s[after] == '=') || priv_ws((char)s[after]);
}

/**
 * @brief Find attribute @p name in tag span @p s[0..len); return its value slice.
 *
 * @return true with @p *voff / @p *vlen set when found; false otherwise.
 */
static bool priv_attr(const uint8_t* s, size_t len, const char* name, size_t* voff, size_t* vlen)
{
  /* Bounded: i advances by 1 each step, capped by len. */
  for (size_t i = 0U; i < len; ++i) {
    if (!priv_attr_at(s, len, i, name)) {
      continue;
    }
    size_t j = i + strlen(name);
    while ((j < len) && (s[j] != '=')) {
      ++j;
    }
    ++j; /* past '=' */
    while ((j < len) && priv_ws((char)s[j])) {
      ++j;
    }
    if ((j >= len) || ((s[j] != '"') && (s[j] != '\''))) {
      return false;
    }
    const char q = (char)s[j];
    ++j;
    const size_t start = j;
    while ((j < len) && (s[j] != (uint8_t)q)) {
      ++j;
    }
    *voff = start;
    *vlen = j - start;
    return true;
  }
  return false;
}

/** @brief Read attribute @p name as an integer; @p def if absent. */
static int32_t priv_attr_num(const uint8_t* s, size_t len, const char* name, int32_t def)
{
  size_t off = 0U;
  size_t vl  = 0U;
  if (!priv_attr(s, len, name, &off, &vl)) {
    return def;
  }
  size_t k = 0U;
  return priv_num(&s[off], vl, &k);
}

/** @brief Case-insensitive compare of a span against a NUL literal. */
static bool priv_ci_eq(const uint8_t* s, size_t len, const char* lit)
{
  size_t k = 0U;
  for (; (k < len) && (lit[k] != '\0'); ++k) {
    if (priv_lc((char)s[k]) != priv_lc(lit[k])) {
      return false;
    }
  }
  return (k == len) && (lit[k] == '\0');
}

/** @brief Parse hex digits (no `#`) at @p s into 0xRRGGBB, or k_svg_no_paint. */
static uint32_t priv_hex_color(const uint8_t* s, size_t len)
{
  uint32_t rgb = 0U;
  if (len == (size_t)k_svg_hex6) {
    for (size_t i = 0U; i < len; ++i) {
      const uint8_t v = priv_hex((char)s[i]);
      if (v >= (uint8_t)k_svg_hex_base) {
        return (uint32_t)k_svg_no_paint;
      }
      rgb = (rgb << (uint32_t)k_svg_hex_nib) | (uint32_t)v;
    }
    return rgb;
  }
  if (len == (size_t)k_svg_hex3) {
    for (size_t i = 0U; i < len; ++i) {
      const uint8_t v = priv_hex((char)s[i]);
      if (v >= (uint8_t)k_svg_hex_base) {
        return (uint32_t)k_svg_no_paint;
      }
      rgb =
        (rgb << (uint32_t)k_svg_hex_chan) | ((uint32_t)v << (uint32_t)k_svg_hex_nib) | (uint32_t)v;
    }
    return rgb;
  }
  return (uint32_t)k_svg_no_paint;
}

/** @brief Map a named SVG colour to RGB, or k_svg_no_paint if unknown / `none`. */
static uint32_t priv_named_color(const uint8_t* s, size_t len)
{
  static const struct {
    const char* name;
    uint32_t    rgb;
  } k_names[] = {
    {"black", 0x000000U},
    {"white", 0xFFFFFFU},
    {"red", 0xFF0000U},
    {"green", 0x008000U},
    {"blue", 0x0000FFU},
    {"gray", 0x808080U},
    {"grey", 0x808080U},
    {"silver", 0xC0C0C0U},
    {"maroon", 0x800000U},
    {"navy", 0x000080U},
    {"yellow", 0xFFFF00U},
    {"orange", 0xFFA500U},
  };
  for (size_t k = 0U; k < (sizeof(k_names) / sizeof(k_names[0])); ++k) {
    if (priv_ci_eq(s, len, k_names[k].name)) {
      return k_names[k].rgb;
    }
  }
  return (uint32_t)k_svg_no_paint;
}

/** @brief Parse an SVG paint value (`#rgb`/`#rrggbb`/name/`none`) to RGB. */
static uint32_t priv_paint(const uint8_t* s, size_t len)
{
  if (len == 0U) {
    return (uint32_t)k_svg_no_paint;
  }
  if (s[0] == '#') {
    return priv_hex_color(&s[1], len - 1U);
  }
  return priv_named_color(s, len);
}

/** @brief Read attribute @p name as a paint; @p def if absent (k_svg_no_paint = skip). */
static uint32_t priv_attr_paint(const uint8_t* s, size_t len, const char* name, uint32_t def)
{
  size_t off = 0U;
  size_t vl  = 0U;
  if (!priv_attr(s, len, name, &off, &vl)) {
    return def;
  }
  return priv_paint(&s[off], vl);
}

/* ===========================================================================
 * Coordinate transform + shape drawing
 * ===========================================================================
 */

/** @brief Map a user-space x to a framebuffer x. */
static int32_t priv_mx(const svg_xform_t* t, int32_t sx)
{
  return t->bx + (int32_t)(((int64_t)(sx - t->vx) * (int64_t)t->bw) / (int64_t)t->vw);
}

/** @brief Map a user-space y to a framebuffer y. */
static int32_t priv_my(const svg_xform_t* t, int32_t sy)
{
  return t->by + (int32_t)(((int64_t)(sy - t->vy) * (int64_t)t->bh) / (int64_t)t->vh);
}

/** @brief Scale a user-space length by the x-axis ratio. */
static int32_t priv_sx(const svg_xform_t* t, int32_t sw)
{
  return (int32_t)(((int64_t)sw * (int64_t)t->bw) / (int64_t)t->vw);
}

/** @brief Draw one `<rect>` (span @p s[0..len)) with its fill. */
static void priv_draw_rect(const uint8_t* s, size_t len, const svg_xform_t* t)
{
  const uint32_t fill = priv_attr_paint(s, len, "fill", (uint32_t)k_svg_def_fill);
  if (fill == (uint32_t)k_svg_no_paint) {
    return;
  }
  const int32_t rx = priv_attr_num(s, len, "x", 0);
  const int32_t ry = priv_attr_num(s, len, "y", 0);
  const int32_t rw = priv_attr_num(s, len, "width", 0);
  const int32_t rh = priv_attr_num(s, len, "height", 0);
  (void)ra_gfx_rect(priv_mx(t, rx), priv_my(t, ry), priv_sx(t, rw), priv_sx(t, rh), fill, true);
}

/** @brief Draw one `<circle>` (span @p s[0..len)) with its fill. */
static void priv_draw_circle(const uint8_t* s, size_t len, const svg_xform_t* t)
{
  const uint32_t fill = priv_attr_paint(s, len, "fill", (uint32_t)k_svg_def_fill);
  if (fill == (uint32_t)k_svg_no_paint) {
    return;
  }
  const int32_t cx = priv_attr_num(s, len, "cx", 0);
  const int32_t cy = priv_attr_num(s, len, "cy", 0);
  const int32_t r  = priv_attr_num(s, len, "r", 0);
  (void)ra_gfx_circle(priv_mx(t, cx), priv_my(t, cy), priv_sx(t, r), fill, true);
}

/** @brief Draw one `<line>` (span @p s[0..len)) with its stroke. */
static void priv_draw_line(const uint8_t* s, size_t len, const svg_xform_t* t)
{
  const uint32_t stroke = priv_attr_paint(s, len, "stroke", (uint32_t)k_svg_no_paint);
  if (stroke == (uint32_t)k_svg_no_paint) {
    return;
  }
  const int32_t x1 = priv_attr_num(s, len, "x1", 0);
  const int32_t y1 = priv_attr_num(s, len, "y1", 0);
  const int32_t x2 = priv_attr_num(s, len, "x2", 0);
  const int32_t y2 = priv_attr_num(s, len, "y2", 0);
  (void)ra_gfx_line(priv_mx(t, x1), priv_my(t, y1), priv_mx(t, x2), priv_my(t, y2), stroke);
}

/* ===========================================================================
 * Polygon / polyline (a `points` list + a scanline polygon fill)
 * ===========================================================================
 */

/** @brief Parse a `points` value into framebuffer-space vertices; return count. */
static int32_t
priv_parse_points(const uint8_t* v, size_t vlen, const svg_xform_t* t, int32_t* xs, int32_t* ys)
{
  size_t  k = 0U;
  int32_t n = 0;
  /* Bounded: <= k_svg_poly_max pairs; k advances past each number. */
  while (n < (int32_t)k_svg_poly_max) {
    while ((k < vlen) && (priv_ws((char)v[k]) || (v[k] == ','))) {
      ++k;
    }
    if (k >= vlen) {
      break;
    }
    const int32_t ux = priv_num(v, vlen, &k);
    const int32_t uy = priv_num(v, vlen, &k);
    xs[n]            = priv_mx(t, ux);
    ys[n]            = priv_my(t, uy);
    ++n;
  }
  return n;
}

/** @brief Ascending insertion sort of @p m int32 values (small @p m). */
static void priv_sort_i32(int32_t* a, int32_t m)
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

/** @brief Collect the x-coords where polygon edges cross scanline @p y. */
static int32_t
priv_scanline_x(const int32_t* xs, const int32_t* ys, int32_t n, int32_t y, int32_t* xint)
{
  int32_t m = 0;
  /* Bounded: one test per polygon edge (n edges). */
  for (int32_t i = 0; (i < n) && (m < (int32_t)k_svg_poly_max); ++i) {
    const int32_t j  = (i + 1) % n;
    const int32_t y0 = ys[i];
    const int32_t y1 = ys[j];
    if (((y0 <= y) && (y < y1)) || ((y1 <= y) && (y < y0))) {
      const int32_t x0 = xs[i];
      const int32_t x1 = xs[j];
      xint[m] = x0 + (int32_t)(((int64_t)(y - y0) * (int64_t)(x1 - x0)) / (int64_t)(y1 - y0));
      ++m;
    }
  }
  return m;
}

/** @brief Even-odd scanline fill of polygon @p (xs,ys)[0..n) with @p color. */
static void priv_fill_poly(const int32_t* xs, const int32_t* ys, int32_t n, uint32_t color)
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
    const int32_t m                    = priv_scanline_x(xs, ys, n, y, xint);
    priv_sort_i32(xint, m);
    for (int32_t k = 0; (k + 1) < m; k += 2) {
      (void)ra_gfx_line(xint[k], y, xint[k + 1], y, color);
    }
  }
}

/** @brief Draw one `<polygon>` (span @p s[0..len)) as a filled polygon. */
static void priv_draw_polygon(const uint8_t* s, size_t len, const svg_xform_t* t)
{
  const uint32_t fill = priv_attr_paint(s, len, "fill", (uint32_t)k_svg_def_fill);
  size_t         off  = 0U;
  size_t         vl   = 0U;
  if ((fill == (uint32_t)k_svg_no_paint) || !priv_attr(s, len, "points", &off, &vl)) {
    return;
  }
  int32_t       xs[k_svg_poly_max] = {};
  int32_t       ys[k_svg_poly_max] = {};
  const int32_t n                  = priv_parse_points(&s[off], vl, t, xs, ys);
  priv_fill_poly(xs, ys, n, fill);
}

/** @brief Draw one `<polyline>` (span @p s[0..len)) as connected stroke segments. */
static void priv_draw_polyline(const uint8_t* s, size_t len, const svg_xform_t* t)
{
  const uint32_t stroke = priv_attr_paint(s, len, "stroke", (uint32_t)k_svg_no_paint);
  size_t         off    = 0U;
  size_t         vl     = 0U;
  if ((stroke == (uint32_t)k_svg_no_paint) || !priv_attr(s, len, "points", &off, &vl)) {
    return;
  }
  int32_t       xs[k_svg_poly_max] = {};
  int32_t       ys[k_svg_poly_max] = {};
  const int32_t n                  = priv_parse_points(&s[off], vl, t, xs, ys);
  /* Bounded: n - 1 segments. */
  for (int32_t i = 0; (i + 1) < n; ++i) {
    (void)ra_gfx_line(xs[i], ys[i], xs[i + 1], ys[i + 1], stroke);
  }
}

/* ===========================================================================
 * Path (`d` mini-language; curves approximated by their endpoint chords)
 * ===========================================================================
 */

/** @brief Argument count of an upper-cased path command, or -1 if unknown. */
static int32_t priv_cmd_argc(char u)
{
  switch (u) {
    case 'm':
    case 'l':
    case 't':
      return (int32_t)k_svg_path_ep;
    case 'h':
    case 'v':
      return 1;
    case 'c':
      return (int32_t)k_svg_argc_cube;
    case 's':
    case 'q':
      return (int32_t)k_svg_argc_quad;
    case 'a':
      return (int32_t)k_svg_path_args;
    case 'z':
      return 0;
    default:
      return -1;
  }
}

/** @brief Advance the current point @p (cx,cy) to a command's endpoint. */
static void
priv_path_step(char u, bool rel, const int32_t* args, int32_t na, int32_t* cx, int32_t* cy)
{
  if (u == 'h') {
    *cx = rel ? (*cx + args[0]) : args[0];
    return;
  }
  if (u == 'v') {
    *cy = rel ? (*cy + args[0]) : args[0];
    return;
  }
  const int32_t ex = args[na - (int32_t)k_svg_path_ep];
  const int32_t ey = args[na - 1];
  *cx              = rel ? (*cx + ex) : ex;
  *cy              = rel ? (*cy + ey) : ey;
}

/** @brief One axis of a cubic Bezier at parameter @p tt (control points c0..c3). */
static float priv_bezier1(float tt, float c0, float c1, float c2, float c3)
{
  const float mt = 1.0F - tt;
  return ((mt * mt * mt) * c0) + (s_svg_bez3 * (mt * mt) * tt * c1) +
         (s_svg_bez3 * mt * (tt * tt) * c2) + ((tt * tt * tt) * c3);
}

/** @brief Flatten a cubic Bezier (P0..P3, user space) into @p (xs,ys); grow @p n. */
static void priv_flatten_cubic(const svg_xform_t* t,
                               svg_pt_t           p0,
                               svg_pt_t           p1,
                               svg_pt_t           p2,
                               svg_pt_t           p3,
                               int32_t*           xs,
                               int32_t*           ys,
                               int32_t*           n)
{
  /* Bounded: k_svg_curve_seg samples, capped by k_svg_poly_max. */
  for (int32_t j = 1; (j <= (int32_t)k_svg_curve_seg) && (*n < (int32_t)k_svg_poly_max); ++j) {
    const float tt = (float)j / (float)k_svg_curve_seg;
    const float bx = priv_bezier1(tt, (float)p0.x, (float)p1.x, (float)p2.x, (float)p3.x);
    const float by = priv_bezier1(tt, (float)p0.y, (float)p1.y, (float)p2.y, (float)p3.y);
    xs[*n]         = priv_mx(t, (int32_t)bx);
    ys[*n]         = priv_my(t, (int32_t)by);
    ++(*n);
  }
}

/** @brief Resolve an absolute/relative point from arg pair @p (ax,ay) about @p (cx,cy). */
static svg_pt_t priv_arg_pt(bool rel, int32_t cx, int32_t cy, int32_t ax, int32_t ay)
{
  const svg_pt_t p = {.x = rel ? (cx + ax) : ax, .y = rel ? (cy + ay) : ay};
  return p;
}

/**
 * @struct path_state_t
 * @brief Running cursor state while parsing a `<path>` `d` string.
 *
 * @details Carries the current point plus the previous curve's last control
 * point and kind, so the smooth commands `S`/`s` and `T`/`t` can reflect it.
 */
typedef struct {
  int32_t  cx;   /**< Current point X (user space).                            */
  int32_t  cy;   /**< Current point Y (user space).                            */
  svg_pt_t ctrl; /**< Last control (abs): quad ctrl (Q/T) or cubic ctrl2 (C/S).*/
  char     kind; /**< 'q' if the last command was Q/T, 'c' if C/S, else 0.     */
} path_state_t;

/** @brief One axis of a quadratic Bezier at @p tt (control points c0..c2). */
static float priv_bezier_q1(float tt, float c0, float c1, float c2)
{
  const float mt = 1.0F - tt;
  return ((mt * mt) * c0) + (s_svg_bez2 * mt * tt * c1) + ((tt * tt) * c2);
}

/** @brief Flatten a quadratic Bezier (P0..P2, user space) into @p (xs,ys); grow @p n. */
static void priv_flatten_quad(const svg_xform_t* t,
                              svg_pt_t           p0,
                              svg_pt_t           p1,
                              svg_pt_t           p2,
                              int32_t*           xs,
                              int32_t*           ys,
                              int32_t*           n)
{
  /* Bounded: k_svg_curve_seg samples, capped by k_svg_poly_max. */
  for (int32_t j = 1; (j <= (int32_t)k_svg_curve_seg) && (*n < (int32_t)k_svg_poly_max); ++j) {
    const float tt = (float)j / (float)k_svg_curve_seg;
    const float bx = priv_bezier_q1(tt, (float)p0.x, (float)p1.x, (float)p2.x);
    const float by = priv_bezier_q1(tt, (float)p0.y, (float)p1.y, (float)p2.y);
    xs[*n]         = priv_mx(t, (int32_t)bx);
    ys[*n]         = priv_my(t, (int32_t)by);
    ++(*n);
  }
}

/** @brief Reflect @p ctrl about the current point @p cur (smooth-curve control). */
static svg_pt_t priv_reflect(svg_pt_t cur, svg_pt_t ctrl)
{
  const svg_pt_t r = {.x = (2 * cur.x) - ctrl.x, .y = (2 * cur.y) - ctrl.y};
  return r;
}

/** @brief Smooth-curve first control: reflect the prev control iff its kind
 *         matches @p want (per SVG `S`/`T`), else the current point @p p0. */
static svg_pt_t priv_smooth_ctrl(svg_pt_t p0, const path_state_t* st, char want)
{
  return (st->kind == want) ? priv_reflect(p0, st->ctrl) : p0;
}

/** @brief Flatten a cubic (P0..P3) into the list; record ctrl2 + advance @p st. */
static int32_t priv_emit_cubic(const svg_xform_t* t,
                               svg_pt_t           p0,
                               svg_pt_t           p1,
                               svg_pt_t           p2,
                               svg_pt_t           p3,
                               path_state_t*      st,
                               int32_t*           xs,
                               int32_t*           ys,
                               int32_t            n)
{
  int32_t m = n;
  priv_flatten_cubic(t, p0, p1, p2, p3, xs, ys, &m);
  st->ctrl = p2;
  st->kind = 'c';
  st->cx   = p3.x;
  st->cy   = p3.y;
  return m;
}

/** @brief Flatten a quadratic (P0..P2) into the list; record ctrl + advance @p st. */
static int32_t priv_emit_quad(const svg_xform_t* t,
                              svg_pt_t           p0,
                              svg_pt_t           p1,
                              svg_pt_t           p2,
                              path_state_t*      st,
                              int32_t*           xs,
                              int32_t*           ys,
                              int32_t            n)
{
  int32_t m = n;
  priv_flatten_quad(t, p0, p1, p2, xs, ys, &m);
  st->ctrl = p1;
  st->kind = 'q';
  st->cx   = p2.x;
  st->cy   = p2.y;
  return m;
}

/**
 * @brief Flatten a cubic/quadratic `<path>` curve command into the vertex list.
 *
 * @details Handles `C`/`c`, `S`/`s` (smooth cubic), `Q`/`q`, and `T`/`t` (smooth
 * quadratic); `S`/`T` reflect the previous matching control point. Returns -1 for
 * any other command so the caller falls back to the endpoint-chord path -- used
 * by the elliptical arc `A`/`a`, which is not flattened (it needs libm trig and
 * is tracked as the remaining curve on #141).
 *
 * @return The new vertex count, or -1 if @p u is not a supported curve command.
 */
static int32_t priv_path_curve(const svg_xform_t* t,
                               char               u,
                               bool               rel,
                               const int32_t*     args,
                               path_state_t*      st,
                               int32_t*           xs,
                               int32_t*           ys,
                               int32_t            n)
{
  const svg_pt_t p0 = {.x = st->cx, .y = st->cy};
  if ((u == 'q') || (u == 't')) {
    const bool     sm = (u == 't');
    const svg_pt_t p1 =
      sm ? priv_smooth_ctrl(p0, st, 'q') : priv_arg_pt(rel, st->cx, st->cy, args[0], args[1]);
    const int32_t  e  = sm ? 0 : 2;
    const svg_pt_t p2 = priv_arg_pt(rel, st->cx, st->cy, args[e], args[e + 1]);
    return priv_emit_quad(t, p0, p1, p2, st, xs, ys, n);
  }
  if ((u == 'c') || (u == 's')) {
    const bool     sm = (u == 's');
    const svg_pt_t p1 =
      sm ? priv_smooth_ctrl(p0, st, 'c') : priv_arg_pt(rel, st->cx, st->cy, args[0], args[1]);
    const int32_t  b  = sm ? 0 : 2;
    const svg_pt_t p2 = priv_arg_pt(rel, st->cx, st->cy, args[b], args[b + 1]);
    const svg_pt_t p3 = priv_arg_pt(rel, st->cx, st->cy, args[b + 2], args[b + 3]);
    return priv_emit_cubic(t, p0, p1, p2, p3, st, xs, ys, n);
  }
  return -1;
}

/**
 * @brief Parse a path `d` value into framebuffer-space vertices; return count.
 *
 * @details Handles M/L/H/V/Z exactly (absolute + relative, with implicit-L
 * repeats after M); the cubic `C`/`c`, smooth cubic `S`/`s`, quadratic `Q`/`q`,
 * and smooth quadratic `T`/`t` are flattened into line segments (the smooth
 * forms reflect the previous control point). The elliptical arc `A`/`a` still
 * contributes only its endpoint chord (needs libm trig -- tracked on #141).
 * Multiple subpaths are merged into one polygon.
 */
/** @brief Resolve the next path command at @p d[*i] (explicit letter, or an
 *         implicit repeat of @p *last); advances @p i past a letter; 0 at end. */
static char priv_next_cmd(const uint8_t* d, size_t dlen, size_t* i, char* last)
{
  while ((*i < dlen) && (priv_ws((char)d[*i]) || (d[*i] == ','))) {
    ++(*i);
  }
  if (*i >= dlen) {
    return 0;
  }
  const char c = (char)d[*i];
  if (((c >= 'A') && (c <= 'Z')) || ((c >= 'a') && (c <= 'z'))) {
    *last = c;
    ++(*i);
    return c;
  }
  return *last;
}

static int32_t
priv_parse_path(const uint8_t* d, size_t dlen, const svg_xform_t* t, int32_t* xs, int32_t* ys)
{
  int32_t      n    = 0;
  path_state_t st   = {.cx = 0, .cy = 0, .ctrl = {.x = 0, .y = 0}, .kind = 0};
  char         last = 0;
  size_t       i    = 0U;
  /* Bounded: <= k_svg_poly_max commands; i advances past each command's args. */
  while ((i < dlen) && (n < (int32_t)k_svg_poly_max)) {
    const char    c   = priv_next_cmd(d, dlen, &i, &last);
    const char    u   = priv_lc(c);
    const bool    rel = (c >= 'a') && (c <= 'z');
    const int32_t na  = (c != 0) ? priv_cmd_argc(u) : -1;
    if (na < 0) {
      break; /* unknown / no current command */
    }
    if (u == 'z') {
      continue; /* close: the fill closes implicitly */
    }
    int32_t args[k_svg_path_args] = {};
    for (int32_t a = 0; a < na; ++a) {
      args[a] = priv_num(d, dlen, &i);
    }
    const int32_t cn = priv_path_curve(t, u, rel, args, &st, xs, ys, n);
    if (cn >= 0) {
      n = cn; /* C/S/Q/T flattened into segments */
    } else {
      priv_path_step(u, rel, args, na, &st.cx, &st.cy);
      xs[n] = priv_mx(t, st.cx);
      ys[n] = priv_my(t, st.cy);
      ++n;
      st.kind = 0; /* a non-curve breaks the smooth-reflection chain */
    }
    if (u == 'm') {
      last = rel ? 'l' : 'L'; /* implicit coords after M are line-tos */
    }
  }
  return n;
}

/** @brief Draw one `<path>` (span @p s[0..len)) as a filled polygon. */
static void priv_draw_path(const uint8_t* s, size_t len, const svg_xform_t* t)
{
  const uint32_t fill = priv_attr_paint(s, len, "fill", (uint32_t)k_svg_def_fill);
  size_t         off  = 0U;
  size_t         vl   = 0U;
  if ((fill == (uint32_t)k_svg_no_paint) || !priv_attr(s, len, "d", &off, &vl)) {
    return;
  }
  int32_t       xs[k_svg_poly_max] = {};
  int32_t       ys[k_svg_poly_max] = {};
  const int32_t n                  = priv_parse_path(&s[off], vl, t, xs, ys);
  priv_fill_poly(xs, ys, n, fill);
}

/* ===========================================================================
 * Document parsing + render walk
 * ===========================================================================
 */

/** @brief True iff element @p name begins at @p s[at] with a delimiter after. */
static bool priv_elem_at(const uint8_t* s, size_t len, size_t at, const char* name)
{
  if (!priv_starts_ci(s, len, at, name)) {
    return false;
  }
  const size_t after = at + strlen(name);
  if (after >= len) {
    return true;
  }
  const char c = (char)s[after];
  return priv_ws(c) || (c == '>') || (c == '/');
}

/** @brief Return the span [*open, *close) of the element @p name at/after @p from. */
static bool priv_tag_span(const uint8_t* s,
                          size_t         len,
                          size_t         from,
                          const char*    name,
                          size_t*        open,
                          size_t*        close)
{
  const size_t at = priv_find_ci(s, len, from, name);
  if (at >= len) {
    return false;
  }
  size_t j = at;
  while ((j < len) && (s[j] != '>')) {
    ++j;
  }
  *open  = at;
  *close = j;
  return true;
}

/** @brief Fill @p t's viewBox from the `<svg>` element (default: the box). */
static void priv_read_viewbox(const uint8_t* s, size_t len, svg_xform_t* t)
{
  t->vx        = 0;
  t->vy        = 0;
  t->vw        = t->bw;
  t->vh        = t->bh;
  size_t open  = 0U;
  size_t close = 0U;
  if (!priv_tag_span(s, len, 0U, "<svg", &open, &close)) {
    return;
  }
  const uint8_t* tag  = &s[open];
  const size_t   tlen = close - open;
  size_t         voff = 0U;
  size_t         vlen = 0U;
  if (priv_attr(tag, tlen, "viewBox", &voff, &vlen)) {
    int32_t nums[k_svg_viewbox_n] = {};
    size_t  k                     = 0U;
    for (size_t n = 0U; n < (size_t)k_svg_viewbox_n; ++n) {
      nums[n] = priv_num(&tag[voff], vlen, &k);
    }
    if ((nums[2] > 0) && (nums[3] > 0)) {
      t->vx = nums[0];
      t->vy = nums[1];
      t->vw = nums[2];
      t->vh = nums[3];
    }
  } else {
    const int32_t wv = priv_attr_num(tag, tlen, "width", t->bw);
    const int32_t hv = priv_attr_num(tag, tlen, "height", t->bh);
    if ((wv > 0) && (hv > 0)) {
      t->vw = wv;
      t->vh = hv;
    }
  }
}

/** @brief Dispatch one element span @p s[at..close) to its shape drawer. */
static void
priv_dispatch_shape(const uint8_t* s, size_t len, size_t at, size_t close, const svg_xform_t* t)
{
  const uint8_t* tag  = &s[at];
  const size_t   tlen = (close > at) ? (close - at) : 0U;
  if (priv_elem_at(s, len, at, "<rect")) {
    priv_draw_rect(tag, tlen, t);
  } else if (priv_elem_at(s, len, at, "<circle")) {
    priv_draw_circle(tag, tlen, t);
  } else if (priv_elem_at(s, len, at, "<polygon")) {
    priv_draw_polygon(tag, tlen, t);
  } else if (priv_elem_at(s, len, at, "<polyline")) {
    priv_draw_polyline(tag, tlen, t);
  } else if (priv_elem_at(s, len, at, "<path")) {
    priv_draw_path(tag, tlen, t);
  } else if (priv_elem_at(s, len, at, "<line")) {
    priv_draw_line(tag, tlen, t);
  } else {
    /* Unsupported element -> skip. */
  }
}

/** @brief Walk every element in document order and draw the supported shapes. */
static void priv_draw_shapes(const uint8_t* s, size_t len, const svg_xform_t* t)
{
  size_t i = 0U;
  /* Bounded: each iteration advances past one '<...>' element or breaks at EOF. */
  while (i < len) {
    while ((i < len) && (s[i] != '<')) {
      ++i;
    }
    if (i >= len) {
      break;
    }
    size_t close = i;
    while ((close < len) && (s[close] != '>')) {
      ++close;
    }
    priv_dispatch_shape(s, len, i, close, t);
    i = (close < len) ? (close + 1U) : len;
  }
}

/* ===========================================================================
 * Public API
 * ===========================================================================
 */

bool ra_svg_is_svg(const uint8_t* bytes, size_t len)
{
  if ((bytes == nullptr) || (len == 0U)) {
    return false;
  }
  size_t i = 0U;
  /* Skip a UTF-8 BOM if present. */
  if ((len >= (size_t)k_svg_bom_len) && (bytes[0] == (uint8_t)k_svg_bom0) &&
      (bytes[1] == (uint8_t)k_svg_bom1) && (bytes[2] == (uint8_t)k_svg_bom2)) {
    i = (size_t)k_svg_bom_len;
  }
  while ((i < len) && priv_ws((char)bytes[i])) {
    ++i;
  }
  return priv_starts_ci(bytes, len, i, "<?xml") || priv_starts_ci(bytes, len, i, "<svg");
}

ra_err_t ra_svg_image_href(const uint8_t* svg, size_t len, size_t* out_off, size_t* out_len)
{
  if ((svg == nullptr) || (out_off == nullptr) || (out_len == nullptr)) {
    return k_ra_err_null_ptr;
  }
  size_t open  = 0U;
  size_t close = 0U;
  if (!priv_tag_span(svg, len, 0U, "<image", &open, &close)) {
    return k_ra_err_not_found;
  }
  const uint8_t* tag  = &svg[open];
  const size_t   tlen = close - open;
  size_t         voff = 0U;
  size_t         vlen = 0U;
  if (!priv_attr(tag, tlen, "xlink:href", &voff, &vlen) &&
      !priv_attr(tag, tlen, "href", &voff, &vlen)) {
    return k_ra_err_not_found;
  }
  *out_off = open + voff;
  *out_len = vlen;
  return k_ra_ok;
}

ra_err_t ra_svg_size(const uint8_t* svg, size_t len, int32_t* out_w, int32_t* out_h)
{
  if ((svg == nullptr) || (out_w == nullptr) || (out_h == nullptr)) {
    return k_ra_err_null_ptr;
  }
  size_t open  = 0U;
  size_t close = 0U;
  if (!priv_tag_span(svg, len, 0U, "<svg", &open, &close)) {
    return k_ra_err_not_found;
  }
  const uint8_t* tag  = &svg[open];
  const size_t   tlen = close - open;
  int32_t        w    = priv_attr_num(tag, tlen, "width", 0);
  int32_t        h    = priv_attr_num(tag, tlen, "height", 0);
  if ((w <= 0) || (h <= 0)) {
    size_t voff = 0U;
    size_t vlen = 0U;
    if (priv_attr(tag, tlen, "viewBox", &voff, &vlen)) {
      int32_t nums[k_svg_viewbox_n] = {};
      size_t  k                     = 0U;
      for (size_t n = 0U; n < (size_t)k_svg_viewbox_n; ++n) {
        nums[n] = priv_num(&tag[voff], vlen, &k);
      }
      w = nums[2];
      h = nums[3];
    }
  }
  if ((w <= 0) || (h <= 0)) {
    return k_ra_err_not_found;
  }
  *out_w = w;
  *out_h = h;
  return k_ra_ok;
}

ra_err_t ra_svg_render(const uint8_t* svg, size_t len, int32_t x, int32_t y, int32_t w, int32_t h)
{
  if (svg == nullptr) {
    return k_ra_err_null_ptr;
  }
  if ((w <= 0) || (h <= 0)) {
    return k_ra_err_invalid_arg;
  }
  svg_xform_t t = {.bx = x, .by = y, .bw = w, .bh = h};
  priv_read_viewbox(svg, len, &t);
  priv_draw_shapes(svg, len, &t);
  return k_ra_ok;
}
