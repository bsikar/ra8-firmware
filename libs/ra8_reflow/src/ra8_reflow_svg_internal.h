/**
 * @file ra8_reflow_svg_internal.h
 * @brief Cross-TU surface for the split minimal-SVG subset (#112).
 * @ingroup grp_ereader
 *
 * @details
 * The SVG subset implementation is split across five translation units that
 * share one set of constants, geometry types, and pure leaf-level helpers:
 *   - ra8_reflow_svg.c        -- scan / attribute / colour-parsing core.
 *   - ra8_reflow_svg_xform.c  -- viewBox + 2x3 affine `transform=` coordinate map.
 *   - ra8_reflow_svg_shape.c  -- rect / circle / line / polygon / polyline draw,
 *                               the scanline polygon fill, gradient evaluation,
 *                               and the elliptical-arc flatten.
 *   - ra8_reflow_svg_path.c   -- the `<path>` `d` mini-language (Bezier curves).
 *   - ra8_reflow_svg_doc.c    -- the document walk, gradient definitions, and the
 *                               public ::ra8_svg_render / ::ra8_svg_size API.
 * Only the constants, geometry types, and helpers referenced by more than one of
 * those TUs are declared here; every TU-private helper stays `static` in its
 * defining file. There is no MMIO and no heap: the only side effect of the whole
 * module is drawing through the bound ra8_gfx framebuffer.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Reflow] {World: NS}
 *
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"

/**
 * @enum priv_svg_consts_t
 * @brief Parse + colour constants for the SVG subset.
 */
typedef enum : uint32_t {
  k_svg_no_paint    = 0xFFFFFFFFU, /**< `none` / absent paint sentinel.          */
  k_svg_def_fill    = 0x000000U,   /**< SVG default fill is black.               */
  k_svg_dec         = 10U,         /**< Decimal base.                            */
  k_svg_hex_base    = 16U,         /**< Hex base / "not a hex digit".            */
  k_svg_hex_a10     = 10U,         /**< Value of hex 'a'/'A'.                    */
  k_svg_hex_nib     = 4U,          /**< Bits per hex nibble.                     */
  k_svg_hex_chan    = 8U,          /**< Bits per colour channel.                 */
  k_svg_hex3        = 3U,          /**< `#rgb` digit count.                      */
  k_svg_hex6        = 6U,          /**< `#rrggbb` digit count.                   */
  k_svg_viewbox_n   = 4U,          /**< `viewBox` number count.                  */
  k_svg_bom0        = 0xEFU,       /**< UTF-8 BOM byte 0.                        */
  k_svg_bom1        = 0xBBU,       /**< UTF-8 BOM byte 1.                        */
  k_svg_bom2        = 0xBFU,       /**< UTF-8 BOM byte 2.                        */
  k_svg_bom_len     = 3U,          /**< UTF-8 BOM length.                        */
  k_svg_poly_max    = 64U,         /**< Max points / scanline crossings.         */
  k_svg_poly_min    = 3U,          /**< Min points for a fillable polygon.       */
  k_svg_path_args   = 7U,          /**< Max args of a path command (`A`).        */
  k_svg_path_ep     = 2U,          /**< Endpoint pair size (x, y); M/L/T.        */
  k_svg_argc_quad   = 4U,          /**< Arg count of `S` / `Q`.                  */
  k_svg_argc_cube   = 6U,          /**< Arg count of `C`.                        */
  k_svg_curve_seg   = 12U,         /**< Segments per cubic-Bezier flatten.       */
  k_svg_cube_ey     = 5U,          /**< `C` endpoint-y argument index.           */
  k_svg_arc_rx      = 0U,          /**< `A` arg index: semi-axis X.              */
  k_svg_arc_ry      = 1U,          /**< `A` arg index: semi-axis Y.              */
  k_svg_arc_rot     = 2U,          /**< `A` arg index: x-axis rotation.          */
  k_svg_arc_large   = 3U,          /**< `A` arg index: large-arc flag.           */
  k_svg_arc_sweep   = 4U,          /**< `A` arg index: sweep flag.               */
  k_svg_arc_ex      = 5U,          /**< `A` arg index: endpoint X.               */
  k_svg_arc_ey      = 6U,          /**< `A` arg index: endpoint Y.               */
  k_svg_arc_seg_max = 24U,         /**< Max segments per arc flatten.            */
  k_svg_g_depth_max = 8U,          /**< Max nested `<g>` transform depth.        */
  k_svg_circle_seg  = 24U,         /**< N-gon segments for a transformed circle. */
  k_svg_rect_pts    = 4U,          /**< Corner count of a rectangle.             */
  k_svg_grad_max    = 8U,          /**< Max gradients defined per document.      */
  k_svg_grad_stops  = 8U,          /**< Max colour stops per gradient.           */
  k_svg_grad_id     = 32U,         /**< Max gradient `id` length, bytes.         */
  k_svg_grad_linear = 0U,          /**< `<linearGradient>` kind.                 */
  k_svg_grad_radial = 1U,          /**< `<radialGradient>` kind.                 */
  k_svg_chan_mask   = 0xFFU,       /**< One 8-bit RGB channel mask.              */
  k_svg_sh_r        = 16U,         /**< Red shift within 0x00RRGGBB.             */
} priv_svg_consts_t;

/**
 * @struct svg_pt_t
 * @brief A 2-D point in SVG user space (an x/y pair).
 */
typedef struct {
  int32_t x; /**< X coordinate. */
  int32_t y; /**< Y coordinate. */
} svg_pt_t;

/**
 * @struct svg_utf_t
 * @brief A user `transform=` as a full 2x3 affine matrix.
 *
 * @details A point @c (x,y) maps to @c (a*x+c*y+e, b*x+d*y+f). `translate`,
 * `scale`, `rotate`, `skewX/Y`, and `matrix` are all representable and closed
 * under composition (::priv_utf_compose), so a transform list and nested `<g>`
 * groups collapse to one of these. The axis-aligned subset (b==0 && c==0)
 * reproduces the legacy scale+translate render byte-for-byte.
 */
typedef struct {
  float a; /**< Row0 col0 (x scale). */
  float b; /**< Row1 col0 (y shear). */
  float c; /**< Row0 col1 (x shear). */
  float d; /**< Row1 col1 (y scale). */
  float e; /**< X translate.         */
  float f; /**< Y translate.         */
} svg_utf_t;

/** @brief Forward decl: the document's gradient set (defined after svg_xform_t). */
typedef struct svg_grads svg_grads_t;

/**
 * @struct svg_xform_t
 * @brief SVG-user-space -> framebuffer-box coordinate transform.
 */
typedef struct {
  int32_t bx; /**< Box left.       */
  int32_t by; /**< Box top.        */
  int32_t bw; /**< Box width.      */
  int32_t bh; /**< Box height.     */
  int32_t vx; /**< viewBox min-x.  */
  int32_t vy; /**< viewBox min-y.  */
  int32_t vw; /**< viewBox width.  */
  int32_t vh; /**< viewBox height. */
  /* User-space `transform=` as a 2x3 affine, applied to a point BEFORE the
   * viewBox->box map: (x,y) -> (ua*x+uc*y+ue, ub*x+ud*y+uf). Identity
   * (ua=ud=1, others 0) reproduces the untransformed render byte-for-byte; the
   * axis-aligned case (ub==0 && uc==0) keeps the fast-path primitives. */
  float ua; /**< Affine a (x scale; 1 = identity).     */
  float ub; /**< Affine b (y shear; 0 = axis-aligned). */
  float uc; /**< Affine c (x shear; 0 = axis-aligned). */
  float ud; /**< Affine d (y scale; 1 = identity).     */
  float ue; /**< Affine e (x translate, user units).   */
  float uf; /**< Affine f (y translate, user units).   */
  /* Gradients defined in the document, scanned once before the render walk.
   * NULL keeps the solid-fill render byte-for-byte unchanged. A shape whose
   * `fill="url(#id)"` resolves here is filled per-pixel via ra8_svgp_fill_poly_grad. */
  const svg_grads_t* grads; /**< Document gradient set, or NULL. */
} svg_xform_t;

/**
 * @struct svg_stop_t
 * @brief One gradient colour stop: an offset in [0,1] and an 0x00RRGGBB colour.
 */
typedef struct {
  float    off; /**< Stop offset, clamped to [0,1]. */
  uint32_t col; /**< Stop colour, 0x00RRGGBB.       */
} svg_stop_t;

/**
 * @struct svg_grad_t
 * @brief A parsed linear/radial gradient in objectBoundingBox units.
 *
 * @details Coordinates are bbox-relative ([0,1]). Linear uses the vector
 * @c (x1,y1)->(x2,y2); radial uses centre @c (x1,y1) and radius @c x2. Only
 * @c gradientUnits="objectBoundingBox" (the default) is modelled; other units
 * are treated as objectBoundingBox (a documented limitation).
 */
typedef struct {
  uint8_t    kind;                    /**< k_svg_grad_linear / k_svg_grad_radial.  */
  uint8_t    nstops;                  /**< Number of valid stops.                  */
  char       id[k_svg_grad_id];       /**< NUL-terminated gradient id (for url()). */
  float      x1;                      /**< Linear start x / radial centre x.       */
  float      y1;                      /**< Linear start y / radial centre y.       */
  float      x2;                      /**< Linear end x / radial radius.           */
  float      y2;                      /**< Linear end y (unused for radial).       */
  svg_stop_t stops[k_svg_grad_stops]; /**< Colour stops, in document order.        */
} svg_grad_t;

/**
 * @struct svg_grads
 * @brief Bounded set of gradients scanned from the document before the render.
 */
struct svg_grads {
  int32_t    n;                 /**< Number of valid gradients (<= k_svg_grad_max). */
  svg_grad_t g[k_svg_grad_max]; /**< The gradient table.                            */
};

/* ===========================================================================
 * Cross-TU helper prototypes (defined `static`-free in one TU, called in others)
 * ===========================================================================
 */

/**
 * @brief Test whether a character is XML whitespace.
 *
 * @details Classifies @p c as space (0x20), tab (0x09), line feed (0x0A),
 * carriage return (0x0D), or form feed (0x0C). Defined in ra8_reflow_svg.c.
 *
 * @param[in] c Character to test.
 *
 * @return bool Classification result.
 * @retval true  @p c is one of the five XML whitespace characters.
 * @retval false @p c is not an XML whitespace character.
 *
 * @pre  @p c is any value representable by @c char.
 * @pre  No state is required before calling this function.
 * @post The return value is one of exactly {true, false}.
 * @post No external state is modified.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV bool ra8_svgp_ws(char c);

/**
 * @brief ASCII-fold a character to lower case.
 *
 * @details Converts 'A'-'Z' to lower case; all other bytes pass through.
 * Defined in ra8_reflow_svg.c.
 *
 * @param[in] c Character to fold.
 *
 * @return char The lowercase equivalent of @p c, or @p c unchanged.
 * @retval 'a'..'z' When @p c was an uppercase ASCII letter.
 * @retval c        Otherwise.
 *
 * @pre  @p c is any value representable by @c char.
 * @pre  No module state is required before calling this function.
 * @post The result is lower case when @p c was an uppercase letter.
 * @post No external state is modified.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV char ra8_svgp_lc(char c);

/**
 * @brief Case-insensitive prefix test of @p lit at @p at within @p s[0..len).
 *
 * @details Matches @p lit against @p s starting at @p at after ASCII folding,
 * returning false when the remaining span is shorter than @p lit. Defined in
 * ra8_reflow_svg.c.
 *
 * @param[in] s   Byte buffer to search; must not be NULL.
 * @param[in] len Total number of valid bytes in @p s.
 * @param[in] at  Byte offset at which to start the comparison.
 * @param[in] lit NUL-terminated ASCII literal to match; must not be NULL.
 *
 * @return bool Result of the case-insensitive prefix test.
 * @retval true  @p s[at .. at+strlen(lit)) matches @p lit case-insensitively.
 * @retval false @p at + strlen(@p lit) > @p len, or any character differs.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p lit is a valid NUL-terminated ASCII string.
 * @post @p s and @p lit are not modified.
 * @post @p at and @p len are not modified.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV bool ra8_svgp_starts_ci(const uint8_t* s, size_t len, size_t at, const char* lit);

/**
 * @brief Find the first case-insensitive occurrence of @p lit in @p s[from..len).
 *
 * @details Scans for the first position where @c ra8_svgp_starts_ci matches; an
 * empty @p lit returns @p len. Defined in ra8_reflow_svg.c.
 *
 * @param[in] s    Byte buffer to search; must not be NULL.
 * @param[in] len  Total valid bytes in @p s.
 * @param[in] from Starting byte offset; must be <= @p len.
 * @param[in] lit  NUL-terminated ASCII literal to find; must not be NULL.
 *
 * @return size_t Byte offset of the first match, or @p len if not found.
 * @retval [from..len) Offset of the first case-insensitive occurrence.
 * @retval len         @p lit was not found, or @p lit is empty.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p from is <= @p len.
 * @post @p s and @p lit are not modified.
 * @post The return value is always in the closed range [@p from, @p len].
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV size_t ra8_svgp_find_ci(const uint8_t* s, size_t len, size_t from, const char* lit);

/**
 * @brief Parse the integer part of one SVG number at @p s[*i]; truncate fractions.
 *
 * @details Skips leading whitespace/comma separators, reads an optional sign and
 * decimal digits, and discards any fractional part. Defined in ra8_reflow_svg.c.
 *
 * @param[in]     s   Byte buffer holding the SVG text; must not be NULL.
 * @param[in]     len Total valid bytes in @p s.
 * @param[in,out] i   Current parse cursor; advanced past the consumed number.
 *
 * @return int32_t The signed integer value parsed from the current cursor.
 * @retval 0  No digit characters were found.
 * @retval n  The value of the decimal integer scanned from @p s[*i].
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p i is not NULL, and @p *i is <= @p len.
 * @post @p *i is advanced past whitespace, sign, integer digits, and fraction.
 * @post @p *i <= @p len.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV int32_t ra8_svgp_num(const uint8_t* s, size_t len, size_t* i);

/**
 * @brief Find attribute @p name in tag span @p s[0..len); return its value slice.
 *
 * @details Locates the word-boundary-delimited @p name, then the quoted value;
 * sets @p *voff / @p *vlen to the value content. Defined in ra8_reflow_svg.c.
 *
 * @param[in]  s    Byte buffer containing the tag text; must not be NULL.
 * @param[in]  len  Total valid bytes in @p s.
 * @param[in]  name NUL-terminated ASCII attribute name; must not be NULL.
 * @param[out] voff Set to the byte offset of the first value character when found.
 * @param[out] vlen Set to the byte length of the value when found.
 *
 * @return bool Whether the attribute was found and parsed.
 * @retval true  Attribute @p name found; @p *voff and @p *vlen are valid.
 * @retval false Attribute not found, missing '=', or missing opening quote.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p voff and @p vlen are valid non-NULL output pointers.
 * @post On true: @p *voff < @p len and @p *voff + @p *vlen <= @p len.
 * @post On false: @p *voff and @p *vlen are indeterminate.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV bool
ra8_svgp_attr(const uint8_t* s, size_t len, const char* name, size_t* voff, size_t* vlen);

/**
 * @brief Read an SVG attribute as a signed integer, returning @p def when absent.
 *
 * @details Locates the value via @c ra8_svgp_attr and parses it via @c ra8_svgp_num.
 * Defined in ra8_reflow_svg.c.
 *
 * @param[in] s    Byte buffer containing the SVG tag text; must not be NULL.
 * @param[in] len  Total valid bytes in @p s.
 * @param[in] name NUL-terminated ASCII attribute name; must not be NULL.
 * @param[in] def  Value to return when the attribute is absent.
 *
 * @return int32_t The parsed integer value, or @p def when absent.
 * @retval def Attribute @p name was not found in @p s.
 * @retval n   The signed integer parsed from the attribute value.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p name is a valid NUL-terminated ASCII string.
 * @post @p s and @p name are not modified.
 * @post The return value equals @p def when the attribute is absent.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV int32_t ra8_svgp_attr_num(const uint8_t* s, size_t len, const char* name, int32_t def);

/**
 * @brief Parse an SVG paint value ('#rgb'/'#rrggbb'/name/'none') to 0x00RRGGBB.
 *
 * @details Routes the span to the hex or named-colour parser, returning the
 * no-paint sentinel for empty/"none"/unknown. Defined in ra8_reflow_svg.c.
 *
 * @param[in] s   Byte span containing the paint value; must not be NULL.
 * @param[in] len Number of valid bytes in @p s.
 *
 * @return uint32_t The 24-bit RGB colour, or ::k_svg_no_paint on failure.
 * @retval 0x000000..0xFFFFFF  Successfully parsed solid colour.
 * @retval (uint32_t)k_svg_no_paint  Empty span, "none", or unrecognised value.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p len is the exact byte length of the attribute value to parse.
 * @post @p s is not modified.
 * @post The return value has bits [31:24] clear.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV uint32_t ra8_svgp_paint(const uint8_t* s, size_t len);

/**
 * @brief Read an SVG attribute as a paint colour, returning @p def when absent.
 *
 * @details Locates the value via @c ra8_svgp_attr and parses it via @c ra8_svgp_paint.
 * Defined in ra8_reflow_svg.c.
 *
 * @param[in] s    Byte buffer containing the SVG tag text; must not be NULL.
 * @param[in] len  Total valid bytes in @p s.
 * @param[in] name NUL-terminated ASCII attribute name; must not be NULL.
 * @param[in] def  Default paint value returned when the attribute is absent.
 *
 * @return uint32_t The parsed paint colour, or @p def when absent.
 * @retval def                       Attribute @p name was not found in @p s.
 * @retval 0x000000..0xFFFFFF        Successfully parsed solid paint colour.
 * @retval (uint32_t)k_svg_no_paint  Attribute present but parses to no-paint.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p name is a valid NUL-terminated ASCII string.
 * @post @p s and @p name are not modified.
 * @post The return value equals @p def when the attribute is absent.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV uint32_t ra8_svgp_attr_paint(const uint8_t* s, size_t len, const char* name, uint32_t def);

/**
 * @brief Resolve a shape's 'fill': a solid colour, or a gradient index via @p gi.
 *
 * @details Sets @p *gi to the matched gradient index for a 'fill="url(#id)"' that
 * resolves in @p t->grads (returning ::k_svg_no_paint), else returns the parsed
 * solid colour or @p def. Defined in ra8_reflow_svg.c.
 *
 * @param[in]  s    Byte buffer containing the SVG tag text; must not be NULL.
 * @param[in]  len  Total valid bytes in @p s.
 * @param[in]  t    Active coordinate transform carrying the gradient set pointer.
 * @param[in]  def  Default solid colour when the 'fill' attribute is absent.
 * @param[out] gi   Set to the matched gradient index, or -1 when not a gradient.
 *
 * @return uint32_t The resolved solid colour, or ::k_svg_no_paint for gradient fills.
 * @retval def                       'fill' attribute is absent.
 * @retval (uint32_t)k_svg_no_paint  'fill' is a 'url(#id)' reference (gradient).
 * @retval 0x000000..0xFFFFFF        Solid colour from the 'fill' attribute.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p gi is a valid non-NULL output pointer.
 * @post @p *gi is always written before return.
 * @post When @p *gi >= 0, the return value is ::k_svg_no_paint.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV uint32_t ra8_svgp_resolve_fill(const uint8_t*     s,
                                        size_t             len,
                                        const svg_xform_t* t,
                                        uint32_t           def,
                                        int32_t*           gi);

/**
 * @brief Parse one SVG floating-point number at @p s[*i], skipping separators.
 *
 * @details Skips whitespace/commas, reads an optional sign, integer and
 * fractional digits, and advances @p *i past the number. Defined in
 * ra8_reflow_svg_xform.c.
 *
 * @param[in]     s   Byte buffer holding the SVG text; must not be NULL.
 * @param[in]     len Total valid bytes in @p s.
 * @param[in,out] i   Current parse cursor; advanced past the consumed number.
 *
 * @return float The signed floating-point value parsed from @p s[*i].
 * @retval 0.0F  No digit characters were found.
 * @retval n     The parsed value, including integer and fractional parts.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p i is not NULL, and @p *i is <= @p len.
 * @post @p *i is advanced past separators, sign, integer digits, and fraction.
 * @post @p *i <= @p len.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV float ra8_svgp_numf(const uint8_t* s, size_t len, size_t* i);

/**
 * @brief Test whether the user transform contains rotation or shear.
 *
 * @details True when @c t->ub or @c t->uc is non-zero. Defined in
 * ra8_reflow_svg_xform.c.
 *
 * @param[in] t Active coordinate transform; must not be NULL.
 *
 * @return bool Whether the user transform has a rotation or shear component.
 * @retval true  @c t->ub != 0.0F or @c t->uc != 0.0F.
 * @retval false Both @c t->ub and @c t->uc are 0.0F (axis-aligned).
 *
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @pre  @c t->ub and @c t->uc hold the shear terms of the current affine.
 * @post @p t is not modified.
 * @post The return value is deterministic given the same @p t.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV bool ra8_svgp_has_rot(const svg_xform_t* t);

/**
 * @brief Map a user-space point through the FULL affine, then viewBox->box.
 *
 * @details Generalised point map for rotated/sheared/gradient shapes; reduces to
 * the axis-aligned map when the affine is axis-aligned. Defined in
 * ra8_reflow_svg_xform.c.
 *
 * @param[in]  t   Active transform; must not be NULL.
 * @param[in]  ux  User-space X coordinate to map.
 * @param[in]  uy  User-space Y coordinate to map.
 * @param[out] fx  Set to the framebuffer X coordinate; must not be NULL.
 * @param[out] fy  Set to the framebuffer Y coordinate; must not be NULL.
 *
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @pre  @p fx and @p fy are valid non-NULL output pointers.
 * @post @p *fx and @p *fy are set to the framebuffer-space coordinates.
 * @post @p t, @p ux, and @p uy are not modified.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV void
ra8_svgp_map_point(const svg_xform_t* t, int32_t ux, int32_t uy, int32_t* fx, int32_t* fy);

/**
 * @brief Map a user-space x coordinate to a framebuffer x coordinate.
 *
 * @details Applies the axis-aligned user transform then the viewBox->box scale.
 * Valid only on the axis-aligned fast path. Defined in ra8_reflow_svg_xform.c.
 *
 * @param[in] t   Active coordinate transform; must not be NULL.
 * @param[in] sx  User-space X coordinate to map.
 *
 * @return int32_t Framebuffer X coordinate corresponding to @p sx.
 * @retval n  The framebuffer pixel column for user-space column @p sx.
 *
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @pre  @c t->vw is non-zero (the viewBox has a positive width).
 * @post The return value is @c t->bx + (int32_t)(((ux - t->vx) * t->bw) / t->vw).
 * @post @p t and @p sx are not modified.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV int32_t ra8_svgp_mx(const svg_xform_t* t, int32_t sx);

/**
 * @brief Map a user-space y coordinate to a framebuffer y coordinate.
 *
 * @details Applies the axis-aligned user transform then the viewBox->box scale.
 * Valid only on the axis-aligned fast path. Defined in ra8_reflow_svg_xform.c.
 *
 * @param[in] t   Active coordinate transform; must not be NULL.
 * @param[in] sy  User-space Y coordinate to map.
 *
 * @return int32_t Framebuffer Y coordinate corresponding to @p sy.
 * @retval n  The framebuffer pixel row for user-space row @p sy.
 *
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @pre  @c t->vh is non-zero (the viewBox has a positive height).
 * @post The return value is @c t->by + (int32_t)(((uy - t->vy) * t->bh) / t->vh).
 * @post @p t and @p sy are not modified.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV int32_t ra8_svgp_my(const svg_xform_t* t, int32_t sy);

/**
 * @brief Scale a user-space length by the user x scale and the viewBox x ratio.
 *
 * @details Applies @c t->ua then the viewBox->box x ratio. Valid only on the
 * axis-aligned fast path. Defined in ra8_reflow_svg_xform.c.
 *
 * @param[in] t   Active coordinate transform; must not be NULL.
 * @param[in] sw  User-space width (or length) to scale.
 *
 * @return int32_t Framebuffer pixel width corresponding to @p sw user units.
 * @retval n  The scaled pixel count, truncated to int32_t.
 *
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @pre  @c t->vw is non-zero (the viewBox has a positive width).
 * @post The return value is (int32_t)((int64_t)((float)sw * t->ua) * t->bw / t->vw).
 * @post @p t and @p sw are not modified.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV int32_t ra8_svgp_sx(const svg_xform_t* t, int32_t sw);

/**
 * @brief Compose the 'transform=' attribute from @p tag onto @p t's user affine.
 *
 * @details Looks up "transform", parses it, and composes it over the current
 * user affine in @p t. Defined in ra8_reflow_svg_xform.c.
 *
 * @param[in,out] t    Transform whose user affine is updated in place; must not be NULL.
 * @param[in]     tag  Byte span of the element tag to search; must not be NULL.
 * @param[in]     tlen Total valid bytes in @p tag.
 *
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @pre  @p tag is a valid pointer to at least @p tlen bytes.
 * @post When a 'transform=' attribute is found, @p t->ua .. @p t->uf reflect
 *       the newly composed affine.
 * @post When no 'transform=' attribute is found, @p t is unchanged.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV void ra8_svgp_apply_xform(svg_xform_t* t, const uint8_t* tag, size_t tlen);

/**
 * @brief Even-odd scanline fill of polygon (xs,ys)[0..n) with a solid colour.
 *
 * @details Computes the bounding Y range and fills spans via ra8_gfx. Defined in
 * ra8_reflow_svg_shape.c.
 *
 * @param[in] xs    Array of @p n framebuffer X coordinates; must not be NULL.
 * @param[in] ys    Array of @p n framebuffer Y coordinates; must not be NULL.
 * @param[in] n     Number of polygon vertices.
 * @param[in] color Fill colour in 0x00RRGGBB format.
 *
 * @pre  @p xs and @p ys are valid pointers to at least @p n elements.
 * @pre  @p n <= @c k_svg_poly_max.
 * @post All pixels inside the even-odd polygon boundary are painted @p color.
 * @post No drawing occurs when @p n < @c k_svg_poly_min.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV void ra8_svgp_fill_poly(const int32_t* xs, const int32_t* ys, int32_t n, uint32_t color);

/**
 * @brief Per-pixel gradient scanline fill of polygon (xs,ys)[0..n).
 *
 * @details Evaluates @p g at each pixel's bounding-box-relative position and
 * writes it with ra8_gfx_pixel. Defined in ra8_reflow_svg_shape.c.
 *
 * @param[in] xs Array of @p n framebuffer X coordinates; must not be NULL.
 * @param[in] ys Array of @p n framebuffer Y coordinates; must not be NULL.
 * @param[in] n  Number of polygon vertices.
 * @param[in] g  Gradient descriptor to evaluate per pixel; must not be NULL.
 *
 * @pre  @p xs and @p ys are valid pointers to at least @p n elements.
 * @pre  @p g is a valid non-NULL pointer with @p g->nstops >= 1.
 * @post All pixels inside the even-odd polygon boundary are painted with the
 *       gradient colour evaluated at their bounding-box-relative position.
 * @post No drawing occurs when @p n < @c k_svg_poly_min.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV void
ra8_svgp_fill_poly_grad(const int32_t* xs, const int32_t* ys, int32_t n, const svg_grad_t* g);

/**
 * @brief Match a 'url(#id)' paint value to a gradient index in the document set.
 *
 * @details Verifies that @p val begins with "url(#" via @c ra8_svgp_starts_ci,
 * extracts the id substring between '#' and ')', and linear-searches
 * @p grads->g[0..n) comparing each gradient's @c id with @c memcmp. Returns -1
 * immediately when @p grads is NULL or the value is not a url() reference.
 * Defined in ra8_reflow_svg.c. TU-external for MC/DC test access: the sole
 * production caller invokes it only with a bound gradient set and a value that
 * already passed the identical url(#) prefix check, so the guard's two
 * conditions are reachable independently only through a direct test call.
 *
 * @param[in] grads Gradient set to search; may be NULL (returns -1).
 * @param[in] val   Byte span holding the raw attribute value; must not be NULL.
 * @param[in] vlen  Number of valid bytes in @p val.
 *
 * @return int32_t Index into @p grads->g, or -1 on no match.
 * @retval 0..grads->n-1  The gradient whose id matches the url(#id) reference.
 * @retval -1             @p grads is NULL, @p val is not "url(#...)", or no id match.
 *
 * @pre  @p val is a valid pointer to at least @p vlen bytes.
 * @pre  When @p grads is non-NULL, @p grads->n <= @c k_svg_grad_max.
 * @post @p val and @p grads are not modified.
 * @post The return value is always in [-1, grads->n-1].
 *
 * @note Test-access only. Not thread-safe in isolation; callers are
 *       single-threaded during SVG render.
 *
 * @par MC/DC:
 * Decision: `(grads == nullptr) || !ra8_svgp_starts_ci(val, vlen, 0U, "url(#")`
 * (2 conditions); N+1 = 3 vectors:
 *  - V1: grads set,  val "url(#g)"  -> C1 F, C2 F -> match (control).
 *  - V2: grads NULL, val "url(#g)"  -> C1 T        -> -1 (varies grads).
 *  - V3: grads set,  val "solid"    -> C1 F, C2 T  -> -1 (varies prefix).
 *
 * @since 0.1.0
 */
RA8_PRIV int32_t priv_match_grad(const svg_grads_t* grads, const uint8_t* val, size_t vlen);

/**
 * @brief Flatten an SVG elliptical arc into the polygon vertex array.
 *
 * @details Converts the endpoint-parametrised arc to centre form and samples it,
 * appending mapped framebuffer points; appends a line-to on a degenerate arc.
 * Defined in ra8_reflow_svg_shape.c.
 *
 * @param[in]     t      Active coordinate transform; must not be NULL.
 * @param[in]     p0     Arc start point (current path position, user space).
 * @param[in]     args   Arc argument array indexed by @c k_svg_arc_* constants; must not be NULL.
 * @param[in]     p_end  Arc end point (absolute, user space).
 * @param[out]    xs     Vertex X array of at least @c k_svg_poly_max elements; must not be NULL.
 * @param[out]    ys     Vertex Y array of at least @c k_svg_poly_max elements; must not be NULL.
 * @param[in,out] n      Current vertex count; updated by this function.
 *
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @pre  @p args has at least @c k_svg_path_args (7) valid int32_t entries.
 * @post @p *n is increased by the number of arc sample points appended.
 * @post @p *n <= @c k_svg_poly_max on exit.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV void ra8_svgp_flatten_arc(const svg_xform_t* t,
                                   svg_pt_t           p0,
                                   const int32_t*     args,
                                   svg_pt_t           p_end,
                                   int32_t*           xs,
                                   int32_t*           ys,
                                   int32_t*           n);

/**
 * @brief Draw one SVG 'rect' element using its fill colour or gradient.
 *
 * @details Axis-aligned solid rects use the ra8_gfx_rect fast-path; rotated rects
 * are scanline-filled. Defined in ra8_reflow_svg_shape.c.
 *
 * @param[in] s   Byte span of the element tag; must not be NULL.
 * @param[in] len Total valid bytes in @p s (the tag span length).
 * @param[in] t   Active coordinate transform; must not be NULL.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @post All pixels inside the rect boundary are painted with the fill colour.
 * @post No pixels outside the rect boundary are modified.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV void ra8_svgp_draw_rect(const uint8_t* s, size_t len, const svg_xform_t* t);

/**
 * @brief Draw one SVG 'circle' element using its fill colour or gradient.
 *
 * @details Axis-aligned solid circles use the ra8_gfx_circle fast-path; rotated
 * circles are approximated by an N-gon and scanline-filled. Defined in
 * ra8_reflow_svg_shape.c.
 *
 * @param[in] s   Byte span of the element tag; must not be NULL.
 * @param[in] len Total valid bytes in @p s (the tag span length).
 * @param[in] t   Active coordinate transform; must not be NULL.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @post All pixels inside the circle boundary are painted with the fill colour.
 * @post No pixels outside the circle boundary are modified.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV void ra8_svgp_draw_circle(const uint8_t* s, size_t len, const svg_xform_t* t);

/**
 * @brief Draw one SVG 'line' element using its stroke colour.
 *
 * @details Maps both endpoints through the affine and draws with ra8_gfx_line;
 * 'fill' is ignored for lines. Defined in ra8_reflow_svg_shape.c.
 *
 * @param[in] s   Byte span of the element tag; must not be NULL.
 * @param[in] len Total valid bytes in @p s (the tag span length).
 * @param[in] t   Active coordinate transform; must not be NULL.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @post The line from (x1,y1) to (x2,y2) is drawn in the stroke colour.
 * @post No drawing occurs when the stroke colour is ::k_svg_no_paint.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV void ra8_svgp_draw_line(const uint8_t* s, size_t len, const svg_xform_t* t);

/**
 * @brief Draw one SVG 'polygon' element as a filled polygon.
 *
 * @details Parses 'points', maps vertices, and fills (solid or gradient).
 * Defined in ra8_reflow_svg_shape.c.
 *
 * @param[in] s   Byte span of the element tag; must not be NULL.
 * @param[in] len Total valid bytes in @p s.
 * @param[in] t   Active coordinate transform; must not be NULL.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @post All pixels inside the polygon boundary are painted with the fill colour.
 * @post No drawing occurs when fill is absent or 'points' is missing.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV void ra8_svgp_draw_polygon(const uint8_t* s, size_t len, const svg_xform_t* t);

/**
 * @brief Draw one SVG 'polyline' element as connected stroke line segments.
 *
 * @details Parses 'points', maps vertices, and connects them with ra8_gfx_line.
 * Defined in ra8_reflow_svg_shape.c.
 *
 * @param[in] s   Byte span of the element tag; must not be NULL.
 * @param[in] len Total valid bytes in @p s.
 * @param[in] t   Active coordinate transform; must not be NULL.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @post All (n-1) line segments between adjacent vertices are drawn in the stroke colour.
 * @post No drawing occurs when stroke is absent or 'points' is missing.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV void ra8_svgp_draw_polyline(const uint8_t* s, size_t len, const svg_xform_t* t);

/**
 * @brief Draw one SVG 'path' element as a filled polygon.
 *
 * @details Parses the 'd' mini-language into framebuffer vertices and fills them
 * (solid or gradient). Defined in ra8_reflow_svg_path.c.
 *
 * @param[in] s   Byte span of the element tag; must not be NULL.
 * @param[in] len Total valid bytes in @p s.
 * @param[in] t   Active coordinate transform; must not be NULL.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @post All pixels inside the path's filled boundary are painted with the fill colour.
 * @post No drawing occurs when fill is absent or the 'd' attribute is missing.
 *
 * @note Not thread-safe in isolation; callers are single-threaded during render.
 * @since 0.1.0
 */
RA8_PRIV void ra8_svgp_draw_path(const uint8_t* s, size_t len, const svg_xform_t* t);
