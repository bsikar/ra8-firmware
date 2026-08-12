/**
 * @file ra8_reflow_svg_xform.c
 * @brief Coordinate transform + 2x3 affine `transform=` parsing for SVG (#112).
 *
 * @details The viewBox->box coordinate map plus the full 2x3 affine
 * `transform=` machinery (translate / scale / rotate / skewX / skewY / matrix,
 * composed and applied per point). Provides the axis-aligned fast-path mappers
 * (::ra8_svgp_mx / ::ra8_svgp_my / ::ra8_svgp_sx) and the general
 * ::ra8_svgp_map_point used by the shape and path stages. No DOM, no heap. See
 * ra8_reflow_svg_internal.h for the shared geometry types and helper contracts.
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
#include "ra8_reflow_svg_internal.h"

/** @brief Pi, for arc degree->radian conversion and sweep wrap. */
static const float s_svg_pi = 3.14159265F;
/** @brief Degrees spanning Pi radians (deg->rad scale). */
static const float s_svg_deg_half = 180.0F;

/* ===========================================================================
 * Coordinate transform + affine `transform=` parsing
 * ===========================================================================
 */

/**
 * @brief Apply the axis-aligned user transform (a/translate) to a user-space x.
 *
 * @details Computes @c (sx * t->ua) + t->ue as a float, then truncates to
 * @c int32_t. Valid only on the axis-aligned fast path where the shear
 * components @c t->ub and @c t->uc are both zero. When the transform has
 * rotation or shear, use @c ra8_svgp_map_point instead to apply the full affine.
 *
 * @param[in] t   Active coordinate transform; must not be NULL.
 * @param[in] sx  User-space X coordinate to transform.
 *
 * @return int32_t User-space X after applying the axis-aligned scale and translate.
 * @retval n  The scaled-and-translated X coordinate, truncated to int32_t.
 *
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @pre  @c t->ub == 0.0F and @c t->uc == 0.0F (axis-aligned fast path).
 *
 * @post The return value is (int32_t)((float)sx * t->ua + t->ue).
 * @post @p t and @p sx are not modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static int32_t priv_ux(const svg_xform_t* t, int32_t sx)
{
  return (int32_t)(((float)sx * t->ua) + t->ue);
}

/**
 * @brief Apply the axis-aligned user transform (d/translate) to a user-space y.
 *
 * @details Computes @c (sy * t->ud) + t->uf as a float, then truncates to
 * @c int32_t. Valid only on the axis-aligned fast path where the shear
 * components @c t->ub and @c t->uc are both zero. When the transform has
 * rotation or shear, use @c ra8_svgp_map_point instead to apply the full affine.
 *
 * @param[in] t   Active coordinate transform; must not be NULL.
 * @param[in] sy  User-space Y coordinate to transform.
 *
 * @return int32_t User-space Y after applying the axis-aligned scale and translate.
 * @retval n  The scaled-and-translated Y coordinate, truncated to int32_t.
 *
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @pre  @c t->ub == 0.0F and @c t->uc == 0.0F (axis-aligned fast path).
 *
 * @post The return value is (int32_t)((float)sy * t->ud + t->uf).
 * @post @p t and @p sy are not modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static int32_t priv_uy(const svg_xform_t* t, int32_t sy)
{
  return (int32_t)(((float)sy * t->ud) + t->uf);
}

/**
 * @brief Test whether the user transform contains rotation or shear.
 *
 * @details Checks whether @c t->ub or @c t->uc is non-zero. When either is
 * non-zero, the axis-aligned fast-path primitives (ra8_gfx_rect, ra8_gfx_circle)
 * cannot represent the shape correctly and callers must fall back to the polygon
 * path via @c ra8_svgp_map_point applied to each vertex.
 *
 * @param[in] t Active coordinate transform; must not be NULL.
 *
 * @return bool Whether the user transform has a rotation or shear component.
 * @retval true  @c t->ub != 0.0F or @c t->uc != 0.0F.
 * @retval false Both @c t->ub and @c t->uc are 0.0F (axis-aligned).
 *
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @pre  @c t->ub and @c t->uc hold the shear terms of the current affine.
 *
 * @post @p t is not modified.
 * @post The return value is deterministic given the same @p t.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
bool ra8_svgp_has_rot(const svg_xform_t* t)
{
  return (t->ub != 0.0F) || (t->uc != 0.0F);
}

/**
 * @brief Map a user-space point through the FULL affine, then viewBox->box.
 *
 * @details Generalized point map for rotated/sheared/gradient shapes. When the
 * affine is axis-aligned (ub==0 && uc==0) the result is byte-identical to
 * @c (ra8_svgp_mx(ux), ra8_svgp_my(uy)), so routing the polygon/path/line emitters
 * through it does not change the untransformed render.
 *
 * @param[in]  t   Active transform; must not be NULL.
 * @param[in]  ux  User-space X coordinate to map.
 * @param[in]  uy  User-space Y coordinate to map.
 * @param[out] fx  Set to the framebuffer X coordinate; must not be NULL.
 * @param[out] fy  Set to the framebuffer Y coordinate; must not be NULL.
 *
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @pre  @p fx and @p fy are valid non-NULL output pointers.
 *
 * @post @p *fx and @p *fy are set to the framebuffer-space coordinates.
 * @post @p t, @p ux, and @p uy are not modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
void ra8_svgp_map_point(const svg_xform_t* t, int32_t ux, int32_t uy, int32_t* fx, int32_t* fy)
{
  const int32_t px = (int32_t)(((float)ux * t->ua) + ((float)uy * t->uc) + t->ue);
  const int32_t py = (int32_t)(((float)ux * t->ub) + ((float)uy * t->ud) + t->uf);
  *fx              = t->bx + (int32_t)(((int64_t)(px - t->vx) * (int64_t)t->bw) / (int64_t)t->vw);
  *fy              = t->by + (int32_t)(((int64_t)(py - t->vy) * (int64_t)t->bh) / (int64_t)t->vh);
}

/**
 * @brief Map a user-space x coordinate to a framebuffer x coordinate.
 *
 * @details Applies the axis-aligned user transform via @c priv_ux, then maps
 * the result through the viewBox-to-box scaling: bx + (ux - vx) * bw / vw.
 * Uses 64-bit intermediate arithmetic to avoid overflow on large coordinates.
 * Valid only when @c t->ub == 0.0F and @c t->uc == 0.0F; for the general
 * case use @c ra8_svgp_map_point.
 *
 * @param[in] t   Active coordinate transform; must not be NULL.
 * @param[in] sx  User-space X coordinate to map.
 *
 * @return int32_t Framebuffer X coordinate corresponding to @p sx.
 * @retval n  The framebuffer pixel column for user-space column @p sx.
 *
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @pre  @c t->vw is non-zero (the viewBox has a positive width).
 *
 * @post The return value is @c t->bx + (int32_t)(((ux - t->vx) * t->bw) / t->vw).
 * @post @p t and @p sx are not modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
int32_t ra8_svgp_mx(const svg_xform_t* t, int32_t sx)
{
  const int32_t ux = priv_ux(t, sx);
  return t->bx + (int32_t)(((int64_t)(ux - t->vx) * (int64_t)t->bw) / (int64_t)t->vw);
}

/**
 * @brief Map a user-space y coordinate to a framebuffer y coordinate.
 *
 * @details Applies the axis-aligned user transform via @c priv_uy, then maps
 * the result through the viewBox-to-box scaling: by + (uy - vy) * bh / vh.
 * Uses 64-bit intermediate arithmetic to avoid overflow on large coordinates.
 * Valid only when @c t->ub == 0.0F and @c t->uc == 0.0F; for the general
 * case use @c ra8_svgp_map_point.
 *
 * @param[in] t   Active coordinate transform; must not be NULL.
 * @param[in] sy  User-space Y coordinate to map.
 *
 * @return int32_t Framebuffer Y coordinate corresponding to @p sy.
 * @retval n  The framebuffer pixel row for user-space row @p sy.
 *
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @pre  @c t->vh is non-zero (the viewBox has a positive height).
 *
 * @post The return value is @c t->by + (int32_t)(((uy - t->vy) * t->bh) / t->vh).
 * @post @p t and @p sy are not modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
int32_t ra8_svgp_my(const svg_xform_t* t, int32_t sy)
{
  const int32_t uy = priv_uy(t, sy);
  return t->by + (int32_t)(((int64_t)(uy - t->vy) * (int64_t)t->bh) / (int64_t)t->vh);
}

/**
 * @brief Scale a user-space length by the user x scale and the viewBox x ratio.
 *
 * @details Applies the user x scale (@c t->ua) to the width @p sw, then scales
 * the result by the viewBox-to-box x ratio (bw / vw). Uses 64-bit intermediate
 * arithmetic to avoid overflow. Returns the framebuffer pixel count corresponding
 * to @p sw user-space units. Valid only on the axis-aligned fast path.
 *
 * @param[in] t   Active coordinate transform; must not be NULL.
 * @param[in] sw  User-space width (or length) to scale.
 *
 * @return int32_t Framebuffer pixel width corresponding to @p sw user units.
 * @retval n  The scaled pixel count, truncated to int32_t.
 *
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @pre  @c t->vw is non-zero (the viewBox has a positive width).
 *
 * @post The return value is (int32_t)((int64_t)((float)sw * t->ua) * t->bw / t->vw).
 * @post @p t and @p sw are not modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
int32_t ra8_svgp_sx(const svg_xform_t* t, int32_t sw)
{
  const int32_t uw = (int32_t)((float)sw * t->ua);
  return (int32_t)(((int64_t)uw * (int64_t)t->bw) / (int64_t)t->vw);
}

/**
 * @brief Parse one SVG floating-point number at @p s[*i], skipping leading separators.
 *
 * @details Advances @p *i past whitespace and comma separators, reads an optional
 * sign ('+'/'-'), then accumulates integer digit characters into @p v. If a decimal
 * point follows, fractional digits are accumulated with decreasing place values.
 * On return, @p *i points one past the last character consumed so subsequent
 * calls parse the next number correctly. Used for all floating-point geometry
 * values including path curve control-point coordinates and arc parameters.
 *
 * @param[in]     s   Byte buffer holding the SVG text; must not be NULL.
 * @param[in]     len Total valid bytes in @p s.
 * @param[in,out] i   Current parse cursor; updated to one past the consumed number.
 *
 * @return float The signed floating-point value parsed from @p s[*i].
 * @retval 0.0F  No digit characters found after the sign (or no sign/digits at all).
 * @retval n     The parsed value, including integer and fractional parts.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p i is not NULL, and @p *i is <= @p len.
 *
 * @post @p *i is advanced past separators, sign, integer digits, and fraction.
 * @post @p *i <= @p len.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
float ra8_svgp_numf(const uint8_t* s, size_t len, size_t* i)
{
  while ((*i < len) && (ra8_svgp_ws((char)s[*i]) || (s[*i] == ','))) {
    ++(*i);
  }
  float sgn = 1.0F;
  if ((*i < len) && ((s[*i] == '-') || (s[*i] == '+'))) {
    sgn = (s[*i] == '-') ? -1.0F : 1.0F;
    ++(*i);
  }
  float v = 0.0F;
  while ((*i < len) && (s[*i] >= '0') && (s[*i] <= '9')) {
    v = (v * (float)k_svg_dec) + (float)(s[*i] - '0');
    ++(*i);
  }
  if ((*i < len) && (s[*i] == '.')) {
    ++(*i);
    float frac = 1.0F;
    while ((*i < len) && (s[*i] >= '0') && (s[*i] <= '9')) {
      frac /= (float)k_svg_dec;
      v += frac * (float)(s[*i] - '0');
      ++(*i);
    }
  }
  return v * sgn;
}

/**
 * @brief Return the 2x3 affine identity transform.
 *
 * @details Constructs and returns an @c svg_utf_t with @c a=1, @c d=1, and
 * all other fields zero. This is the identity element for affine composition:
 * any affine composed with the identity returns the original affine unchanged.
 * Used to initialise the group transform stack and as the starting accumulator
 * in @c priv_parse_xform.
 *
 * @return svg_utf_t The 2x3 identity affine (a=d=1, b=c=e=f=0).
 * @retval svg_utf_t{.a=1,.b=0,.c=0,.d=1,.e=0,.f=0}  Always.
 *
 * @pre  No preconditions; this function has no parameters.
 * @pre  The @c svg_utf_t type is properly defined in this translation unit.
 *
 * @post The returned struct has @c a==1.0F, @c d==1.0F, and all other fields 0.0F.
 * @post No external state is modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static svg_utf_t priv_utf_identity(void)
{
  svg_utf_t id = {.a = 1.0F, .b = 0.0F, .c = 0.0F, .d = 1.0F, .e = 0.0F, .f = 0.0F};
  return id;
}

/**
 * @brief Compose two 2x3 affines: @p a (outer) over @p b (inner).
 *
 * @details Computes the 2x3 matrix product @p a * @p b so that a point is first
 * mapped by @p b and then by @p a, implementing the standard affine composition
 * rule. The translation components are handled correctly: @c e and @c f of the
 * result include the full composite translation. Used to accumulate a series of
 * SVG transform functions (translate, scale, rotate, skew, matrix) into one
 * affine that can be applied per-point in @c ra8_svgp_map_point.
 *
 * @param[in] a Outer (post) affine; applied second.
 * @param[in] b Inner (pre) affine; applied first.
 *
 * @return svg_utf_t The composed affine @p a * @p b.
 * @retval svg_utf_t  The result of composing @p a over @p b as 2x3 matrix product.
 *
 * @pre  @p a and @p b are fully initialised @c svg_utf_t values.
 * @pre  The floating-point components of @p a and @p b are finite.
 *
 * @post The return value correctly maps any point @c p as a(b(p)).
 * @post @p a and @p b are not modified (passed by value).
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static svg_utf_t priv_utf_compose(svg_utf_t a, svg_utf_t b)
{
  svg_utf_t r = {.a = (a.a * b.a) + (a.c * b.b),
                 .b = (a.b * b.a) + (a.d * b.b),
                 .c = (a.a * b.c) + (a.c * b.d),
                 .d = (a.b * b.c) + (a.d * b.d),
                 .e = (a.a * b.e) + (a.c * b.f) + a.e,
                 .f = (a.b * b.e) + (a.d * b.f) + a.f};
  return r;
}

/**
 * @brief Test whether a byte position starts an SVG number literal.
 *
 * @details A byte position is considered the start of a number when it is a
 * decimal digit ('0'-'9'), a sign character ('-' or '+'), or a decimal point
 * ('.'). Returns false immediately when @p at >= @p len to guard against
 * out-of-bounds access. Used by @c priv_xform_read to decide whether to call
 * @c ra8_svgp_numf for the next argument in a transform function's argument list.
 *
 * @param[in] s   Byte buffer containing the transform value; must not be NULL.
 * @param[in] len Total valid bytes in @p s.
 * @param[in] at  Byte offset to test.
 *
 * @return bool Whether @p s[@p at] can begin an SVG number.
 * @retval true  @p at < @p len and @p s[@p at] is a digit, sign, or decimal point.
 * @retval false @p at >= @p len, or @p s[@p at] is not a number-start character.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p at is a size_t value (may equal or exceed @p len).
 *
 * @post @p s, @p len, and @p at are not modified.
 * @post The return value is deterministic given the same inputs.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_is_num_start(const uint8_t* s, size_t len, size_t at)
{
  if (at >= len) {
    return false;
  }
  const char c = (char)s[at];
  return ((c >= '0') && (c <= '9')) || (c == '-') || (c == '+') || (c == '.');
}

/**
 * @enum svg_xf_kind_t
 * @brief `transform=` function kinds recognised by ::priv_parse_xform.
 */
typedef enum : uint8_t {
  k_svg_xf_none      = 0U, /**< Unrecognised function -> skipped. */
  k_svg_xf_translate = 1U, /**< translate(tx[,ty]).               */
  k_svg_xf_scale     = 2U, /**< scale(sx[,sy]).                   */
  k_svg_xf_rotate    = 3U, /**< rotate(deg[,cx,cy]).              */
  k_svg_xf_skewx     = 4U, /**< skewX(deg).                       */
  k_svg_xf_skewy     = 5U, /**< skewY(deg).                       */
  k_svg_xf_matrix    = 6U, /**< matrix(a,b,c,d,e,f).              */
} svg_xf_kind_t;

/**
 * @enum svg_xf_arg_t
 * @brief Argument indices into a parsed `matrix(a,b,c,d,e,f)` transform list.
 */
typedef enum : uint8_t {
  k_svg_xf_arg_a = 0U, /**< matrix component a (x scale).     */
  k_svg_xf_arg_b = 1U, /**< matrix component b (y shear).     */
  k_svg_xf_arg_c = 2U, /**< matrix component c (x shear).     */
  k_svg_xf_arg_d = 3U, /**< matrix component d (y scale).     */
  k_svg_xf_arg_e = 4U, /**< matrix component e (x translate). */
  k_svg_xf_arg_f = 5U, /**< matrix component f (y translate). */
} svg_xf_arg_t;

/**
 * @brief Convert an angle from degrees to radians.
 *
 * @details Multiplies @p deg by the constant (pi / 180), stored as
 * @c s_svg_pi / @c s_svg_deg_half, to avoid a magic literal. Used by the
 * rotate, skewX, and skewY transform builders when converting their single
 * degree argument into the radian angle needed by @c cosf and @c sinf.
 *
 * @param[in] deg Angle in degrees (any finite float value).
 *
 * @return float The equivalent angle in radians.
 * @retval n  @p deg * (s_svg_pi / s_svg_deg_half).
 *
 * @pre  @p deg is a finite float value (NaN or infinity produce undefined float arithmetic).
 * @pre  @c s_svg_pi and @c s_svg_deg_half are initialised (guaranteed at module scope).
 *
 * @post The return value equals @p deg * (s_svg_pi / 180.0F).
 * @post @p deg is not modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static float priv_deg2rad(float deg)
{
  return deg * (s_svg_pi / s_svg_deg_half);
}

/**
 * @brief Read up to 6 float arguments from a transform function's '(...)' list.
 *
 * @details Advances @p *j past any leading whitespace/commas, then calls
 * @c ra8_svgp_numf for each number that begins at the current cursor. The loop
 * terminates when @c priv_is_num_start returns false or the
 * @c k_svg_argc_cube limit (6) is reached. On return @p *j points past all
 * consumed arguments. Used by @c priv_parse_xform to populate the argument
 * array passed to @c priv_xform_build.
 *
 * @param[in]     v     Byte buffer containing the transform value text; must not be NULL.
 * @param[in]     vlen  Total valid bytes in @p v.
 * @param[in,out] j     Parse cursor; updated past all consumed argument bytes.
 * @param[out]    args  Array of at least @c k_svg_argc_cube floats; must not be NULL.
 *
 * @return int32_t Count of numbers successfully read into @p args.
 * @retval 0..6  The number of float arguments parsed from @p v[*j].
 *
 * @pre  @p v is a valid pointer to at least @p vlen bytes.
 * @pre  @p j is not NULL and @p *j <= @p vlen.
 *
 * @post @p args[0..return-1] contain the parsed float arguments.
 * @post @p *j is advanced past all consumed whitespace, commas, and digits.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static int32_t priv_xform_read(const uint8_t* v, size_t vlen, size_t* j, float* args)
{
  int32_t na = 0;
  /* Bounded: <= k_svg_argc_cube args; each ra8_svgp_numf advances *j. */
  while (na < (int32_t)k_svg_argc_cube) {
    while ((*j < vlen) && (ra8_svgp_ws((char)v[*j]) || (v[*j] == ','))) {
      ++(*j);
    }
    if (!priv_is_num_start(v, vlen, *j)) {
      break;
    }
    args[na] = ra8_svgp_numf(v, vlen, j);
    ++na;
  }
  return na;
}

/**
 * @brief Build the 2x3 affine for an SVG 'rotate(deg[,cx,cy])' transform.
 *
 * @details Converts @p args[0] from degrees to radians and computes @c cosf /
 * @c sinf. When @p na >= 3, @p args[1] and @p args[2] supply the centre of
 * rotation (cx, cy); the translation components e and f are set to keep the
 * centre fixed. When @p na < 3 the rotation is about the origin (e=f=0).
 *
 * @param[in] args Array of at least @p na float arguments; args[0]=degrees,
 *                 args[1]=cx (optional), args[2]=cy (optional).
 * @param[in] na   Count of valid entries in @p args (1 or 3).
 *
 * @return svg_utf_t The 2x3 rotation affine about (cx,cy).
 * @retval svg_utf_t  Rotation matrix; e and f absorb the (cx,cy) re-centring.
 *
 * @pre  @p args is a valid non-NULL pointer to at least @p na float values.
 * @pre  @p na is 1 (rotate about origin) or 3 (rotate about cx,cy).
 *
 * @post The returned affine maps the origin to itself when @p na < 3.
 * @post @p args and @p na are not modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static svg_utf_t priv_xform_rotate(const float* args, int32_t na)
{
  const float co = cosf(priv_deg2rad(args[0]));
  const float si = sinf(priv_deg2rad(args[0]));
  const float cx = (na >= 3) ? args[1] : 0.0F;
  const float cy = (na >= 3) ? args[2] : 0.0F;
  return (svg_utf_t){.a = co,
                     .b = si,
                     .c = -si,
                     .d = co,
                     .e = (cx * (1.0F - co)) + (cy * si),
                     .f = (cy * (1.0F - co)) - (cx * si)};
}

/**
 * @brief Build the 2x3 affine for one parsed SVG transform function.
 *
 * @details Switches on @p kind to construct the appropriate @c svg_utf_t:
 * translate fills @c e/@c f; scale fills @c a/@c d (uniform if na==1);
 * rotate delegates to @c priv_xform_rotate; skewX/skewY fill @c c or @c b
 * via @c tanf; matrix copies all six components directly. An unrecognised
 * @p kind (k_svg_xf_none or default) returns the identity.
 *
 * @param[in] kind Transform function identifier; one of the @c svg_xf_kind_t values.
 * @param[in] args Parsed float arguments; caller guarantees at least @p na valid entries.
 * @param[in] na   Count of valid float arguments in @p args.
 *
 * @return svg_utf_t The 2x3 affine for the specified transform function.
 * @retval svg_utf_t  The computed affine; identity for k_svg_xf_none or unknown kind.
 *
 * @pre  @p args is a valid non-NULL pointer to at least @p na float values.
 * @pre  @p na is consistent with @p kind (e.g. >= 1 for translate/scale).
 *
 * @post The returned affine is a valid 2x3 affine matrix.
 * @post @p args and @p na are not modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static svg_utf_t priv_xform_build(svg_xf_kind_t kind, const float* args, int32_t na)
{
  switch (kind) {
    case k_svg_xf_translate:
      return (svg_utf_t){.a = 1.0F,
                         .b = 0.0F,
                         .c = 0.0F,
                         .d = 1.0F,
                         .e = args[0],
                         .f = (na >= 2) ? args[1] : 0.0F};
    case k_svg_xf_scale:
      return (svg_utf_t){.a = args[0],
                         .b = 0.0F,
                         .c = 0.0F,
                         .d = (na >= 2) ? args[1] : args[0],
                         .e = 0.0F,
                         .f = 0.0F};
    case k_svg_xf_rotate:
      return priv_xform_rotate(args, na);
    case k_svg_xf_skewx:
      return (svg_utf_t){.a = 1.0F,
                         .b = 0.0F,
                         .c = tanf(priv_deg2rad(args[0])),
                         .d = 1.0F,
                         .e = 0.0F,
                         .f = 0.0F};
    case k_svg_xf_skewy:
      return (svg_utf_t){.a = 1.0F,
                         .b = tanf(priv_deg2rad(args[0])),
                         .c = 0.0F,
                         .d = 1.0F,
                         .e = 0.0F,
                         .f = 0.0F};
    case k_svg_xf_matrix:
      return (svg_utf_t){.a = args[k_svg_xf_arg_a],
                         .b = args[k_svg_xf_arg_b],
                         .c = args[k_svg_xf_arg_c],
                         .d = args[k_svg_xf_arg_d],
                         .e = args[k_svg_xf_arg_e],
                         .f = args[k_svg_xf_arg_f]};
    case k_svg_xf_none:
    default:
      return priv_utf_identity();
  }
}

/**
 * @brief Identify the SVG transform function keyword starting at @p v[@p i].
 *
 * @details Tests the byte span at offset @p i against each recognised keyword
 * ("translate(", "scale(", "rotate(", "skewx(", "skewy(", "matrix(") using
 * @c ra8_svgp_starts_ci. Returns the first matching @c svg_xf_kind_t value, or
 * @c k_svg_xf_none when no keyword matches. Case-insensitive to match the SVG
 * specification (though SVG uses lower case by convention).
 *
 * @param[in] v    Byte buffer containing the 'transform=' attribute value; must not be NULL.
 * @param[in] vlen Total valid bytes in @p v.
 * @param[in] i    Byte offset at which the keyword scan begins.
 *
 * @return svg_xf_kind_t The matched transform function kind.
 * @retval k_svg_xf_translate  "translate(" matches at @p v[@p i].
 * @retval k_svg_xf_scale      "scale(" matches at @p v[@p i].
 * @retval k_svg_xf_rotate     "rotate(" matches at @p v[@p i].
 * @retval k_svg_xf_skewx      "skewx(" matches at @p v[@p i].
 * @retval k_svg_xf_skewy      "skewy(" matches at @p v[@p i].
 * @retval k_svg_xf_matrix     "matrix(" matches at @p v[@p i].
 * @retval k_svg_xf_none       No recognised keyword matches.
 *
 * @pre  @p v is a valid pointer to at least @p vlen bytes.
 * @pre  @p i is <= @p vlen.
 *
 * @post @p v, @p vlen, and @p i are not modified.
 * @post The return value is a valid @c svg_xf_kind_t enumerator.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static svg_xf_kind_t priv_xform_kind(const uint8_t* v, size_t vlen, size_t i)
{
  if (ra8_svgp_starts_ci(v, vlen, i, "translate(")) {
    return k_svg_xf_translate;
  }
  if (ra8_svgp_starts_ci(v, vlen, i, "scale(")) {
    return k_svg_xf_scale;
  }
  if (ra8_svgp_starts_ci(v, vlen, i, "rotate(")) {
    return k_svg_xf_rotate;
  }
  if (ra8_svgp_starts_ci(v, vlen, i, "skewx(")) {
    return k_svg_xf_skewx;
  }
  if (ra8_svgp_starts_ci(v, vlen, i, "skewy(")) {
    return k_svg_xf_skewy;
  }
  if (ra8_svgp_starts_ci(v, vlen, i, "matrix(")) {
    return k_svg_xf_matrix;
  }
  return k_svg_xf_none;
}

/**
 * @brief Parse a 'transform=' attribute value into a composed 2x3 affine.
 *
 * @details Recognises translate/scale/rotate/skewX/skewY/matrix functions,
 * composed left-to-right (the leftmost function becomes the outermost
 * in the affine product, i.e. applied last). Unknown or unrecognised function
 * names are skipped. The accumulator starts at the identity and each parsed
 * function is right-composed via @c priv_utf_compose. Bounded: the scan
 * advances past at least one '(' per iteration or breaks at end-of-buffer.
 *
 * @param[in] v    Byte span holding the attribute value text; must not be NULL.
 * @param[in] vlen Total valid bytes in @p v.
 *
 * @return svg_utf_t The composed 2x3 affine for all recognised functions in @p v.
 * @retval priv_utf_identity()  @p v contains no recognised transform functions.
 * @retval svg_utf_t            Composition of all recognised functions, left-to-right.
 *
 * @pre  @p v is a valid pointer to at least @p vlen bytes.
 * @pre  @p vlen accurately reflects the number of valid bytes in @p v.
 *
 * @post The returned affine is the left-to-right composition of all recognised functions.
 * @post @p v is not modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static svg_utf_t priv_parse_xform(const uint8_t* v, size_t vlen)
{
  svg_utf_t acc = priv_utf_identity();
  size_t    i   = 0U;
  /* Bounded: each pass consumes one `name(...)` group or breaks; <= vlen steps. */
  while (i < vlen) {
    while ((i < vlen) && (ra8_svgp_ws((char)v[i]) || (v[i] == ','))) {
      ++i;
    }
    const svg_xf_kind_t kind = priv_xform_kind(v, vlen, i);
    size_t              op   = i;
    while ((op < vlen) && (v[op] != '(')) {
      ++op;
    }
    if (op >= vlen) {
      break;
    }
    size_t j = op + 1U;
    if (kind != k_svg_xf_none) {
      float         args[k_svg_path_args] = {};
      const int32_t na                    = priv_xform_read(v, vlen, &j, args);
      acc                                 = priv_utf_compose(acc, priv_xform_build(kind, args, na));
    }
    while ((j < vlen) && (v[j] != ')')) {
      ++j;
    }
    i = (j < vlen) ? (j + 1U) : vlen;
  }
  return acc;
}

/**
 * @brief Compose the 'transform=' attribute from @p tag onto @p t's user affine.
 *
 * @details Looks up the "transform" attribute in @p tag[0..tlen) using
 * @c ra8_svgp_attr. If absent, returns without modification. If found, parses
 * the value via @c priv_parse_xform and composes the result over the current
 * user affine in @p t using @c priv_utf_compose, then writes the composed
 * components back to @p t->ua .. @p t->uf. The viewBox and box fields of
 * @p t are not touched.
 *
 * @param[in,out] t    Transform whose user affine is updated in place; must not be NULL.
 * @param[in]     tag  Byte span of the element tag to search; must not be NULL.
 * @param[in]     tlen Total valid bytes in @p tag.
 *
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @pre  @p tag is a valid pointer to at least @p tlen bytes.
 *
 * @post When a 'transform=' attribute is found, @p t->ua .. @p t->uf reflect
 *       the newly composed affine.
 * @post When no 'transform=' attribute is found, @p t is unchanged.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
void ra8_svgp_apply_xform(svg_xform_t* t, const uint8_t* tag, size_t tlen)
{
  size_t off = 0U;
  size_t vl  = 0U;
  if (!ra8_svgp_attr(tag, tlen, "transform", &off, &vl)) {
    return;
  }
  const svg_utf_t cur = {.a = t->ua, .b = t->ub, .c = t->uc, .d = t->ud, .e = t->ue, .f = t->uf};
  const svg_utf_t out = priv_utf_compose(cur, priv_parse_xform(&tag[off], vl));
  t->ua               = out.a;
  t->ub               = out.b;
  t->uc               = out.c;
  t->ud               = out.d;
  t->ue               = out.e;
  t->uf               = out.f;
}
