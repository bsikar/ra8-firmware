/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_reflow_svg_path.c
 * @brief SVG `<path>` `d` mini-language parser + Bezier flatten (#112).
 *
 * @details The `<path>` `d` mini-language: M/L/H/V/Z exact, the cubic 'C'/'S'
 * and quadratic 'Q'/'T' Bezier curves flattened to line segments (smooth forms
 * reflect the previous control point), and the elliptical arc 'A' delegated to
 * the shared arc flatten in ra8_reflow_svg_shape.c. The resulting vertices are
 * filled by the shared scanline polygon fill. No DOM, no heap. See
 * ra8_reflow_svg_internal.h for the shared geometry types and helper contracts.
 *
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 *
 * @since 0.1.0
 */

#include "ra8_attributes.h"
#include "ra8_reflow_svg_internal.h"

/** @brief Cubic Bernstein middle coefficient (3) for the Bezier flatten. */
static const float s_svg_bez3 = 3.0F;
/** @brief Quadratic Bernstein middle coefficient (2) for the Bezier flatten. */
static const float s_svg_bez2 = 2.0F;

/* ===========================================================================
 * Path (`d` mini-language; curves approximated by their endpoint chords)
 * ===========================================================================
 */

/**
 * @brief Return the argument count for a lower-cased SVG path command letter.
 *
 * @details Maps each recognised lower-case path command to the number of
 * numeric arguments it consumes: 'm'/'l'/'t' take 2 (x,y); 'h'/'v' take 1;
 * 'c' takes 6; 's'/'q' take 4; 'a' takes 7; 'z' takes 0. Any unrecognised
 * letter returns -1 so the caller can detect and stop parsing.
 *
 * @param[in] u Lower-cased path command letter to look up.
 *
 * @return int32_t Argument count for @p u, or -1 if unrecognised.
 * @retval 0  'z' (close path, no arguments).
 * @retval 1  'h' or 'v' (horizontal/vertical line-to).
 * @retval 2  'm', 'l', 't' (move-to, line-to, smooth quadratic).
 * @retval 4  's' or 'q' (smooth cubic / quadratic).
 * @retval 6  'c' (cubic Bezier).
 * @retval 7  'a' (elliptical arc).
 * @retval -1 Unrecognised command letter.
 *
 * @pre  @p u is a lower-case ASCII character or any other char value.
 * @pre  The path command alphabet is as defined in the SVG 1.1 specification.
 *
 * @post The return value is one of {-1, 0, 1, 2, 4, 6, 7}.
 * @post @p u is not modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
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

/**
 * @brief Advance the current path point to the endpoint of a line-type command.
 *
 * @details Handles the endpoint-update logic for 'h' (horizontal line-to),
 * 'v' (vertical line-to), and all other commands that terminate with an (x,y)
 * endpoint pair as the last two args. For relative commands (@p rel true) the
 * new position is added to the current point; for absolute commands it replaces
 * it. Curve commands should call @c priv_emit_cubic / @c priv_emit_quad instead
 * as they carry additional control-point state.
 *
 * @param[in]     u    Lower-case command letter determining the update rule.
 * @param[in]     rel  True for relative, false for absolute coordinates.
 * @param[in]     args Array of at least @p na parsed integer arguments; must not be NULL.
 * @param[in]     na   Count of valid entries in @p args.
 * @param[in,out] cx   Current point X; updated by this function.
 * @param[in,out] cy   Current point Y; updated by this function.
 *
 * @pre  @p args is a valid pointer to at least @p na int32_t values.
 * @pre  @p cx and @p cy are valid non-NULL pointers.
 *
 * @post @p *cx and @p *cy reflect the endpoint of the command.
 * @post @p args is not modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
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

/**
 * @brief Evaluate one axis of a cubic Bernstein-Bezier polynomial at parameter @p tt.
 *
 * @details Computes the standard four-point cubic Bezier formula:
 * (1-tt)^3 * c0 + 3*(1-tt)^2*tt * c1 + 3*(1-tt)*tt^2 * c2 + tt^3 * c3.
 * Used by @c priv_flatten_cubic to sample x and y independently at each
 * subdivision parameter value @p tt in (0, 1].
 *
 * @param[in] tt Parameter in [0, 1]; 0 returns @p c0, 1 returns @p c3.
 * @param[in] c0 Start control point value for this axis.
 * @param[in] c1 First interior control point value.
 * @param[in] c2 Second interior control point value.
 * @param[in] c3 End control point value for this axis.
 *
 * @return float The Bezier value at parameter @p tt.
 * @retval c0  When @p tt is exactly 0.0F.
 * @retval c3  When @p tt is exactly 1.0F.
 *
 * @pre  @p tt is a finite float in [0.0F, 1.0F].
 * @pre  @p c0, @p c1, @p c2, @p c3 are finite float values.
 *
 * @post The return value is the cubic Bernstein polynomial evaluated at @p tt.
 * @post No parameters are modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static float priv_bezier1(float tt, float c0, float c1, float c2, float c3)
{
  const float mt = 1.0F - tt;
  return ((mt * mt * mt) * c0) + (s_svg_bez3 * (mt * mt) * tt * c1) +
         (s_svg_bez3 * mt * (tt * tt) * c2) + ((tt * tt * tt) * c3);
}

/**
 * @brief Flatten a cubic Bezier curve (P0..P3, user space) into the vertex array.
 *
 * @details Samples @c k_svg_curve_seg equally-spaced parameter values in (0,1]
 * using @c priv_bezier1 for each axis, maps each sample through the full affine
 * via @c ra8_svgp_map_point, and appends the framebuffer-space point to
 * @p xs[@p *n] / @p ys[@p *n], incrementing @p *n. Stops early when @p *n
 * reaches @c k_svg_poly_max to avoid buffer overflow.
 *
 * @param[in]     t   Active coordinate transform; must not be NULL.
 * @param[in]     p0  Start point in user space.
 * @param[in]     p1  First control point in user space.
 * @param[in]     p2  Second control point in user space.
 * @param[in]     p3  End point in user space.
 * @param[out]    xs  Vertex X array of at least @c k_svg_poly_max elements; must not be NULL.
 * @param[out]    ys  Vertex Y array of at least @c k_svg_poly_max elements; must not be NULL.
 * @param[in,out] n   Current vertex count; updated by the number of samples appended.
 *
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @pre  @p xs, @p ys are valid arrays of at least @c k_svg_poly_max elements.
 *
 * @post @p *n is increased by at most @c k_svg_curve_seg.
 * @post @p *n <= @c k_svg_poly_max on exit.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
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
    ra8_svgp_map_point(t, (int32_t)bx, (int32_t)by, &xs[*n], &ys[*n]);
    ++(*n);
  }
}

/**
 * @brief Resolve an absolute or relative SVG point from an argument pair.
 *
 * @details When @p rel is true, the result is the current point (@p cx, @p cy)
 * plus the argument offsets (@p ax, @p ay). When @p rel is false, the result
 * is simply (@p ax, @p ay) treated as an absolute user-space coordinate.
 * Used throughout the path parser to interpret command arguments uniformly
 * regardless of the command's case (uppercase = absolute, lowercase = relative).
 *
 * @param[in] rel True for relative coordinates; false for absolute.
 * @param[in] cx  Current point X (user space); used only when @p rel is true.
 * @param[in] cy  Current point Y (user space); used only when @p rel is true.
 * @param[in] ax  X argument from the parsed path command.
 * @param[in] ay  Y argument from the parsed path command.
 *
 * @return svg_pt_t The resolved user-space point.
 * @retval {cx+ax, cy+ay}  When @p rel is true.
 * @retval {ax, ay}        When @p rel is false.
 *
 * @pre  @p ax and @p ay are valid int32_t argument values.
 * @pre  When @p rel is true, @p cx and @p cy reflect the current path position.
 *
 * @post The return value is a valid user-space @c svg_pt_t.
 * @post No parameters are modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
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
  int32_t  cx;   /**< Current point X (user space).                             */
  int32_t  cy;   /**< Current point Y (user space).                             */
  svg_pt_t ctrl; /**< Last control (abs): quad ctrl (Q/T) or cubic ctrl2 (C/S). */
  char     kind; /**< 'q' if the last command was Q/T, 'c' if C/S, else 0.      */
} path_state_t;

/**
 * @brief Evaluate one axis of a quadratic Bernstein-Bezier polynomial at parameter @p tt.
 *
 * @details Computes the three-point quadratic Bezier formula:
 * (1-tt)^2 * c0 + 2*(1-tt)*tt * c1 + tt^2 * c2. Used by @c priv_flatten_quad
 * to sample x and y independently at each subdivision parameter.
 *
 * @param[in] tt Parameter in [0, 1]; 0 returns @p c0, 1 returns @p c2.
 * @param[in] c0 Start control point value for this axis.
 * @param[in] c1 Middle control point value.
 * @param[in] c2 End control point value for this axis.
 *
 * @return float The quadratic Bezier value at parameter @p tt.
 * @retval c0  When @p tt is exactly 0.0F.
 * @retval c2  When @p tt is exactly 1.0F.
 *
 * @pre  @p tt is a finite float in [0.0F, 1.0F].
 * @pre  @p c0, @p c1, and @p c2 are finite float values.
 *
 * @post The return value is the quadratic Bernstein polynomial evaluated at @p tt.
 * @post No parameters are modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static float priv_bezier_q1(float tt, float c0, float c1, float c2)
{
  const float mt = 1.0F - tt;
  return ((mt * mt) * c0) + (s_svg_bez2 * mt * tt * c1) + ((tt * tt) * c2);
}

/**
 * @brief Flatten a quadratic Bezier curve (P0..P2, user space) into the vertex array.
 *
 * @details Samples @c k_svg_curve_seg equally-spaced parameter values in (0,1]
 * using @c priv_bezier_q1 for each axis, maps each through the full affine via
 * @c ra8_svgp_map_point, and appends the framebuffer-space point to
 * @p xs[@p *n] / @p ys[@p *n], incrementing @p *n. Stops early when @p *n
 * reaches @c k_svg_poly_max to avoid buffer overflow.
 *
 * @param[in]     t   Active coordinate transform; must not be NULL.
 * @param[in]     p0  Start point in user space.
 * @param[in]     p1  Control point in user space.
 * @param[in]     p2  End point in user space.
 * @param[out]    xs  Vertex X array of at least @c k_svg_poly_max elements; must not be NULL.
 * @param[out]    ys  Vertex Y array of at least @c k_svg_poly_max elements; must not be NULL.
 * @param[in,out] n   Current vertex count; updated by the number of samples appended.
 *
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @pre  @p xs, @p ys are valid arrays of at least @c k_svg_poly_max elements.
 *
 * @post @p *n is increased by at most @c k_svg_curve_seg.
 * @post @p *n <= @c k_svg_poly_max on exit.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
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
    ra8_svgp_map_point(t, (int32_t)bx, (int32_t)by, &xs[*n], &ys[*n]);
    ++(*n);
  }
}

/**
 * @brief Reflect control point @p ctrl through current point @p cur.
 *
 * @details Computes the point that is the reflection of @p ctrl across @p cur:
 * result = (2*cur.x - ctrl.x, 2*cur.y - ctrl.y). This is the SVG rule for
 * deriving the implicit first control point of a smooth cubic ('S'/'s') or
 * smooth quadratic ('T'/'t') curve from the previous command's last control.
 *
 * @param[in] cur  The current path point (pivot of the reflection).
 * @param[in] ctrl The control point to reflect.
 *
 * @return svg_pt_t The reflected control point.
 * @retval {2*cur.x-ctrl.x, 2*cur.y-ctrl.y}  Always.
 *
 * @pre  @p cur and @p ctrl are valid @c svg_pt_t values in user space.
 * @pre  The values fit in int32_t without overflow (SVG coordinates are small).
 *
 * @post The return value is the reflection of @p ctrl through @p cur.
 * @post @p cur and @p ctrl are not modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static svg_pt_t priv_reflect(svg_pt_t cur, svg_pt_t ctrl)
{
  const svg_pt_t r = {.x = (2 * cur.x) - ctrl.x, .y = (2 * cur.y) - ctrl.y};
  return r;
}

/**
 * @brief Derive the first control point for a smooth curve command.
 *
 * @details Returns the reflection of @c st->ctrl through @p p0 when
 * @c st->kind equals @p want (i.e. the previous command was a matching curve
 * type); otherwise returns @p p0 itself (the SVG specification says "assume
 * the control point coincides with the current point"). This implements the
 * smooth commands 'S'/'s' (want='c') and 'T'/'t' (want='q').
 *
 * @param[in] p0   Current path point (used as fallback and pivot for reflection).
 * @param[in] st   Current path cursor state; must not be NULL.
 * @param[in] want Expected previous command kind ('c' for S, 'q' for T).
 *
 * @return svg_pt_t The derived first control point.
 * @retval priv_reflect(p0, st->ctrl)  When @c st->kind == @p want.
 * @retval p0                          Otherwise.
 *
 * @pre  @p st is a valid non-NULL pointer to an initialised @c path_state_t.
 * @pre  @p p0 is the current user-space path position.
 *
 * @post The return value is a valid user-space @c svg_pt_t.
 * @post @p p0, @p st, and @p want are not modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static svg_pt_t priv_smooth_ctrl(svg_pt_t p0, const path_state_t* st, char want)
{
  return (st->kind == want) ? priv_reflect(p0, st->ctrl) : p0;
}

/**
 * @brief Flatten a cubic Bezier (P0..P3) into the vertex list and advance path state.
 *
 * @details Calls @c priv_flatten_cubic to append sampled framebuffer vertices
 * starting at index @p n, records P2 as the new smooth-curve control point in
 * @p st->ctrl, sets @p st->kind to 'c', and advances @p st->cx / @p st->cy
 * to P3. Returns the new vertex count. Used for 'C'/'c' and 'S'/'s' commands.
 *
 * @param[in]     t   Active coordinate transform; must not be NULL.
 * @param[in]     p0  Start point in user space.
 * @param[in]     p1  First control point in user space.
 * @param[in]     p2  Second control point in user space (recorded as ctrl).
 * @param[in]     p3  End point in user space (becomes new current point).
 * @param[in,out] st  Path cursor state; must not be NULL; updated by this call.
 * @param[out]    xs  Vertex X array of at least @c k_svg_poly_max elements; must not be NULL.
 * @param[out]    ys  Vertex Y array of at least @c k_svg_poly_max elements; must not be NULL.
 * @param[in]     n   Vertex count before this call.
 *
 * @return int32_t New vertex count after appending the flattened cubic samples.
 * @retval n..n+k_svg_curve_seg  Depending on how many samples fit before @c k_svg_poly_max.
 *
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @pre  @p st, @p xs, and @p ys are valid non-NULL pointers.
 *
 * @post @p st->ctrl, @p st->kind, @p st->cx, and @p st->cy are updated.
 * @post The return value is in [n, k_svg_poly_max].
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
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

/**
 * @brief Flatten a quadratic Bezier (P0..P2) into the vertex list and advance path state.
 *
 * @details Calls @c priv_flatten_quad to append sampled framebuffer vertices
 * starting at index @p n, records P1 as the new smooth-curve control point in
 * @p st->ctrl, sets @p st->kind to 'q', and advances @p st->cx / @p st->cy
 * to P2. Returns the new vertex count. Used for 'Q'/'q' and 'T'/'t' commands.
 *
 * @param[in]     t   Active coordinate transform; must not be NULL.
 * @param[in]     p0  Start point in user space.
 * @param[in]     p1  Control point in user space (recorded as ctrl).
 * @param[in]     p2  End point in user space (becomes new current point).
 * @param[in,out] st  Path cursor state; must not be NULL; updated by this call.
 * @param[out]    xs  Vertex X array of at least @c k_svg_poly_max elements; must not be NULL.
 * @param[out]    ys  Vertex Y array of at least @c k_svg_poly_max elements; must not be NULL.
 * @param[in]     n   Vertex count before this call.
 *
 * @return int32_t New vertex count after appending the flattened quadratic samples.
 * @retval n..n+k_svg_curve_seg  Depending on how many samples fit before @c k_svg_poly_max.
 *
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @pre  @p st, @p xs, and @p ys are valid non-NULL pointers.
 *
 * @post @p st->ctrl, @p st->kind, @p st->cx, and @p st->cy are updated.
 * @post The return value is in [n, k_svg_poly_max].
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
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
 * @brief Flatten a cubic/quadratic path curve command into the vertex list.
 *
 * @details Handles 'C'/'c', 'S'/'s' (smooth cubic), 'Q'/'q', 'T'/'t' (smooth
 * quadratic), and the elliptical arc 'A'/'a'; 'S'/'T' reflect the previous
 * matching control point via @c priv_smooth_ctrl, and the arc is centre-
 * parametrised and sampled via @c ra8_svgp_flatten_arc. Returns -1 for any other
 * command so the caller falls back to the endpoint-chord path (M/L/H/V/Z).
 *
 * @param[in]     t    Active coordinate transform; must not be NULL.
 * @param[in]     u    Lower-case path command letter.
 * @param[in]     rel  True for relative coordinates.
 * @param[in]     args Parsed integer arguments; must not be NULL.
 * @param[in,out] st   Path cursor state; must not be NULL; updated when curve is consumed.
 * @param[out]    xs   Vertex X array of at least @c k_svg_poly_max elements; must not be NULL.
 * @param[out]    ys   Vertex Y array of at least @c k_svg_poly_max elements; must not be NULL.
 * @param[in]     n    Vertex count before this call.
 *
 * @return int32_t New vertex count, or -1 if @p u is not a supported curve command.
 * @retval -1  @p u is not 'q', 't', 'c', 's', or 'a'.
 * @retval n.. New vertex count after flattening the curve.
 *
 * @pre  @p t, @p args, @p st, @p xs, and @p ys are valid non-NULL pointers.
 * @pre  @p n is in [0, k_svg_poly_max].
 *
 * @post When the return value >= 0, @p st is updated with the new current point.
 * @post The return value is in [-1, k_svg_poly_max].
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
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
  if (u == 'a') {
    const svg_pt_t p_end = priv_arg_pt(rel, st->cx, st->cy, args[k_svg_arc_ex], args[k_svg_arc_ey]);
    int32_t        m     = n;
    ra8_svgp_flatten_arc(t, p0, args, p_end, xs, ys, &m);
    st->kind = 0; /* an arc breaks the smooth-reflection chain */
    st->cx   = p_end.x;
    st->cy   = p_end.y;
    return m;
  }
  return -1;
}

/**
 * @brief Resolve the next path command at @p d[*i].
 *
 * @details Skips leading whitespace and commas, then reads the next character.
 * If it is an ASCII letter ('A'-'Z' or 'a'-'z'), that letter is stored in
 * @p *last, @p *i is advanced past it, and the letter is returned. If the
 * next character is not a letter (implicit argument repeat), @p *last is
 * returned unchanged (the previous command letter). Returns 0 at end-of-buffer.
 *
 * @param[in]     d     Path 'd' attribute byte span; must not be NULL.
 * @param[in]     dlen  Total valid bytes in @p d.
 * @param[in,out] i     Parse cursor; advanced past the command letter when found.
 * @param[in,out] last  Last explicit command letter seen; updated when a new letter is consumed.
 *
 * @return char The current path command letter, or 0 at end-of-buffer.
 * @retval 'A'..'Z' or 'a'..'z'  The command letter (new or repeated from @p *last).
 * @retval 0                      @p *i >= @p dlen (end of buffer).
 *
 * @pre  @p d is a valid pointer to at least @p dlen bytes.
 * @pre  @p i and @p last are valid non-NULL pointers.
 *
 * @post @p *i is advanced past the command letter when an explicit letter is consumed.
 * @post @p *last holds the most recently seen explicit command letter.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static char priv_next_cmd(const uint8_t* d, size_t dlen, size_t* i, char* last)
{
  while ((*i < dlen) && (ra8_svgp_ws((char)d[*i]) || (d[*i] == ','))) {
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

/**
 * @brief Parse a path 'd' value into framebuffer-space vertices; return count.
 *
 * @details Handles M/L/H/V/Z exactly (absolute + relative, with implicit-L
 * repeats after M); the cubic 'C'/'c', smooth cubic 'S'/'s', quadratic 'Q'/'q',
 * smooth quadratic 'T'/'t', and elliptical arc 'A'/'a' are flattened into
 * the vertex array (smooth forms reflect the previous control point, arcs are
 * centre-parametrised). Multiple subpaths are merged into one polygon. 'Z'
 * is silently accepted and the implicit close is handled by the polygon fill.
 *
 * @param[in]  d    Byte span holding the 'd' attribute value; must not be NULL.
 * @param[in]  dlen Total valid bytes in @p d.
 * @param[in]  t    Active coordinate transform; must not be NULL.
 * @param[out] xs   Vertex X array of at least @c k_svg_poly_max elements; must not be NULL.
 * @param[out] ys   Vertex Y array of at least @c k_svg_poly_max elements; must not be NULL.
 *
 * @return int32_t Number of vertices filled in @p xs / @p ys.
 * @retval 0             No valid path commands produced any vertices.
 * @retval 1..k_svg_poly_max  Count of framebuffer-space vertices.
 *
 * @pre  @p d is a valid pointer to at least @p dlen bytes.
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
priv_parse_path(const uint8_t* d, size_t dlen, const svg_xform_t* t, int32_t* xs, int32_t* ys)
{
  int32_t      n    = 0;
  path_state_t st   = {.cx = 0, .cy = 0, .ctrl = {.x = 0, .y = 0}, .kind = 0};
  char         last = 0;
  size_t       i    = 0U;
  /* Bounded: <= k_svg_poly_max commands. Every non-'z' command emits at least
   * one vertex (n advances) or hits the vertex cap, and the 'z' arm advances i
   * on a malformed implicit repeat, so the loop always makes progress and
   * cannot spin on untrusted input (NASA P10 Rule 2). */
  while ((i < dlen) && (n < (int32_t)k_svg_poly_max)) {
    const size_t i_before = i;
    const char   c        = priv_next_cmd(d, dlen, &i, &last);
    const char   u        = ra8_svgp_lc(c);
    /*
     * priv_next_cmd() only ever yields c in {0} U ['A'..'Z'] U ['a'..'z'] (an
     * ASCII path-command letter or the 0 end-marker). Whenever c >= 'a' holds,
     * c is a lowercase letter (0x61..0x7A) and therefore always <= 'z'; no
     * yielded value can make (c >= 'a') true while (c <= 'z') is false. The
     * (c <= 'z') condition thus cannot be flipped independently.
     */
    /* mcdc-deactivated: c is a command letter or 0; (c<='z') is always true once (c>='a') holds. */
    const bool    rel = (c >= 'a') && (c <= 'z');
    const int32_t na  = (c != 0) ? priv_cmd_argc(u) : -1;
    if (na < 0) {
      break; /* unknown / no current command */
    }
    if (u == 'z') {
      /* Close: the fill closes implicitly. 'z' takes no args, so an implicit
       * repeat after a non-command byte (e.g. the digit in d="Z2") leaves the
       * cursor unmoved and would spin forever -- skip the byte to guarantee
       * forward progress. */
      if (i == i_before) {
        ++i;
      }
      continue;
    }
    int32_t args[k_svg_path_args] = {};
    for (int32_t a = 0; a < na; ++a) {
      args[a] = ra8_svgp_num(d, dlen, &i);
    }
    const int32_t cn = priv_path_curve(t, u, rel, args, &st, xs, ys, n);
    if (cn >= 0) {
      n = cn; /* C/S/Q/T flattened into segments */
    } else {
      priv_path_step(u, rel, args, na, &st.cx, &st.cy);
      ra8_svgp_map_point(t, st.cx, st.cy, &xs[n], &ys[n]);
      ++n;
      st.kind = 0; /* a non-curve breaks the smooth-reflection chain */
    }
    if (u == 'm') {
      last = rel ? 'l' : 'L'; /* implicit coords after M are line-tos */
    }
  }
  return n;
}

/**
 * @brief Draw one SVG 'path' element as a filled polygon.
 *
 * @details Resolves the fill (solid or gradient via @c ra8_svgp_resolve_fill),
 * reads the 'd' attribute, parses it into framebuffer-space vertices via
 * @c priv_parse_path, and fills the resulting polygon with @c ra8_svgp_fill_poly
 * or @c ra8_svgp_fill_poly_grad. Returns without drawing when fill is absent/none
 * or the 'd' attribute is missing.
 *
 * @param[in] s   Byte span of the element tag; must not be NULL.
 * @param[in] len Total valid bytes in @p s.
 * @param[in] t   Active coordinate transform; must not be NULL.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 *
 * @post All pixels inside the path's filled boundary are painted with the fill colour.
 * @post No drawing occurs when fill is absent or the 'd' attribute is missing.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
void ra8_svgp_draw_path(const uint8_t* s, size_t len, const svg_xform_t* t)
{
  int32_t        gi   = -1;
  const uint32_t fill = ra8_svgp_resolve_fill(s, len, t, (uint32_t)k_svg_def_fill, &gi);
  size_t         off  = 0U;
  size_t         vl   = 0U;
  if (((gi < 0) && (fill == (uint32_t)k_svg_no_paint)) || !ra8_svgp_attr(s, len, "d", &off, &vl)) {
    return;
  }
  int32_t       xs[k_svg_poly_max] = {};
  int32_t       ys[k_svg_poly_max] = {};
  const int32_t n                  = priv_parse_path(&s[off], vl, t, xs, ys);
  if (gi >= 0) {
    ra8_svgp_fill_poly_grad(xs, ys, n, &t->grads->g[gi]);
  } else {
    ra8_svgp_fill_poly(xs, ys, n, fill);
  }
}
