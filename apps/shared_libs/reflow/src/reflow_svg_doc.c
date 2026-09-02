/**
 * @file reflow_svg_doc.c
 * @brief SVG document walk, gradient definitions, and the public render API (#112).
 *
 * @details The document-order element walk (with a bounded nested `<g>`
 * transform stack), the `<linearGradient>` / `<radialGradient>` + `<stop>`
 * gradient-definition scan, and the public ::ra8_svg_is_svg / ::ra8_svg_image_href
 * / ::ra8_svg_size / ::ra8_svg_render entry points. Each shape is dispatched to its
 * rasteriser in the sibling reflow_svg_*.c files. No DOM, no heap. See
 * reflow_svg_internal.h for the shared geometry types and helper contracts.
 *
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <string.h>

#include "ra8_attributes.h"
#include "reflow_svg.h"
#include "reflow_svg_internal.h"

/** @brief Gradient parameter / offset upper clamp. */
static const float s_svg_unit = 1.0F;

/* ===========================================================================
 * Document parsing + render walk
 * ===========================================================================
 */

/**
 * @brief Test whether an element keyword begins at @p s[@p at] with a valid delimiter.
 *
 * @details Uses @c priv_ra8_svgp_starts_ci to match @p name at @p at, then verifies that
 * the character immediately following the keyword (when within @p len) is either
 * XML whitespace, '>', or '/'. A match at end-of-buffer is accepted as a valid
 * (self-closing or run-to-EOF) element. Used to identify element tag kinds
 * during the document walk without allocating strings.
 *
 * @param[in] s    Byte buffer containing the SVG text; must not be NULL.
 * @param[in] len  Total valid bytes in @p s.
 * @param[in] at   Byte offset at which to test for the element name.
 * @param[in] name NUL-terminated element keyword (including '<'); must not be NULL.
 *
 * @return bool Whether @p name begins at @p at with a valid post-name delimiter.
 * @retval true  @p name matches at @p at and is followed by a valid delimiter.
 * @retval false @p name does not match or the following character is not a delimiter.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p name is a valid NUL-terminated ASCII string.
 *
 * @post @p s, @p len, @p at, and @p name are not modified.
 * @post The return value is deterministic given the same inputs.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_elem_at(const uint8_t* s, size_t len, size_t at, const char* name)
{
  if (!priv_ra8_svgp_starts_ci(s, len, at, name)) {
    return false;
  }
  const size_t after = at + strlen(name);
  if (after >= len) {
    return true;
  }
  const char c = (char)s[after];
  return priv_ra8_svgp_ws(c) || (c == '>') || (c == '/');
}

/**
 * @brief Locate the tag span [*open, *close) of element @p name at or after @p from.
 *
 * @details Calls @c priv_ra8_svgp_find_ci to find the first occurrence of @p name in
 * @p s[@p from .. @p len). If found, scans forward for the next '>' character
 * to determine the end of the tag. Sets @p *open to the start of the match and
 * @p *close to the position of '>'. If @p name is not found, returns false.
 *
 * @param[in]  s     Byte buffer containing the SVG text; must not be NULL.
 * @param[in]  len   Total valid bytes in @p s.
 * @param[in]  from  Start offset for the search; must be <= @p len.
 * @param[in]  name  NUL-terminated keyword to find; must not be NULL.
 * @param[out] open  Set to the byte offset of the keyword match when found.
 * @param[out] close Set to the byte offset of the '>' terminator when found.
 *
 * @return bool Whether the element was found.
 * @retval true  @p name found; @p *open and @p *close are valid.
 * @retval false @p name not found in @p s[@p from .. @p len).
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p open and @p close are valid non-NULL output pointers.
 *
 * @post On true: @p *open <= @p *close <= @p len.
 * @post On false: @p *open and @p *close are indeterminate.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_tag_span(const uint8_t* s,
                              size_t         len,
                              size_t         from,
                              const char*    name,
                              size_t*        open,
                              size_t*        close)
{
  const size_t at = priv_ra8_svgp_find_ci(s, len, from, name);
  if (at >= len) {
    return false;
  }
  size_t j = at;
  while ((j < len) && (s[j] != (uint8_t)'>')) {
    ++j;
  }
  *open  = at;
  *close = j;
  return true;
}

/**
 * @brief Fill @p t's viewBox fields from the SVG root element.
 *
 * @details Searches for the first '<svg' tag and attempts to read a 'viewBox'
 * attribute (four integers: min-x, min-y, width, height). If absent, falls
 * back to 'width' and 'height' attributes. If those are also absent, the
 * viewBox defaults to (0, 0, bw, bh) so that the SVG user space equals the
 * target box with no scaling. Values with zero width or height are ignored.
 *
 * @param[in]     s   Byte buffer containing the full SVG document; must not be NULL.
 * @param[in]     len Total valid bytes in @p s.
 * @param[in,out] t   Transform; @c vx, @c vy, @c vw, @c vh are written.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p t is a valid non-NULL pointer with @p t->bw and @p t->bh set.
 *
 * @post @p t->vx, @p t->vy, @p t->vw, @p t->vh reflect the parsed viewBox.
 * @post @p t->vw > 0 and @p t->vh > 0 on exit.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_read_viewbox(const uint8_t* s, size_t len, svg_xform_t* t)
{
  t->vx        = 0;
  t->vy        = 0;
  t->vw        = t->bw;
  t->vh        = t->bh;
  size_t open  = 0U;
  size_t close = 0U;
  if (!internal_tag_span(s, len, 0U, "<svg", &open, &close)) {
    return;
  }
  const uint8_t* tag  = &s[open];
  const size_t   tlen = close - open;
  size_t         voff = 0U;
  size_t         vlen = 0U;
  if (priv_ra8_svgp_attr(tag, tlen, "viewBox", &voff, &vlen)) {
    int32_t nums[k_svg_viewbox_n] = {};
    size_t  k                     = 0U;
    for (size_t n = 0U; n < (size_t)k_svg_viewbox_n; ++n) {
      nums[n] = priv_ra8_svgp_num(&tag[voff], vlen, &k);
    }
    if ((nums[2] > 0) && (nums[3] > 0)) {
      t->vx = nums[0];
      t->vy = nums[1];
      t->vw = nums[2];
      t->vh = nums[3];
    }
  } else {
    const int32_t wv = priv_ra8_svgp_attr_num(tag, tlen, "width", t->bw);
    const int32_t hv = priv_ra8_svgp_attr_num(tag, tlen, "height", t->bh);
    if ((wv > 0) && (hv > 0)) {
      t->vw = wv;
      t->vh = hv;
    }
  }
}

/**
 * @brief Dispatch one element tag span to the appropriate shape-drawing function.
 *
 * @details Slices @p s[@p at .. @p close) as the element tag, composes any
 * element-level 'transform=' onto a local copy of @p t via @c priv_ra8_svgp_apply_xform,
 * then selects the drawing function based on the element name: rect, circle,
 * polygon, polyline, path, or line. Unsupported elements are silently ignored.
 *
 * @param[in] s     Full SVG document byte buffer; must not be NULL.
 * @param[in] len   Total valid bytes in @p s.
 * @param[in] at    Byte offset of the element opening tag in @p s.
 * @param[in] close Byte offset of the '>' terminator in @p s.
 * @param[in] t     Inherited coordinate transform (from group context); must not be NULL.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p at <= @p close <= @p len.
 *
 * @post The appropriate shape is drawn using the locally composed transform.
 * @post @p t is not modified; the local copy absorbs the element transform.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void
internal_dispatch_shape(const uint8_t* s, size_t len, size_t at, size_t close, const svg_xform_t* t)
{
  const uint8_t* tag  = &s[at];
  const size_t   tlen = (close > at) ? (close - at) : 0U;
  /* Compose this element's own `transform=` onto the inherited group transform. */
  svg_xform_t lt = *t;
  priv_ra8_svgp_apply_xform(&lt, tag, tlen);
  if (internal_elem_at(s, len, at, "<rect")) {
    priv_ra8_svgp_draw_rect(tag, tlen, &lt);
  } else if (internal_elem_at(s, len, at, "<circle")) {
    priv_ra8_svgp_draw_circle(tag, tlen, &lt);
  } else if (internal_elem_at(s, len, at, "<polygon")) {
    priv_ra8_svgp_draw_polygon(tag, tlen, &lt);
  } else if (internal_elem_at(s, len, at, "<polyline")) {
    priv_ra8_svgp_draw_polyline(tag, tlen, &lt);
  } else if (internal_elem_at(s, len, at, "<path")) {
    priv_ra8_svgp_draw_path(tag, tlen, &lt);
  } else if (internal_elem_at(s, len, at, "<line")) {
    priv_ra8_svgp_draw_line(tag, tlen, &lt);
  } else {
    /* Unsupported element -> skip. */
  }
}

/**
 * @brief Copy the base transform @p t into @p out and replace the user affine with @p g.
 *
 * @details Copies all fields of @p t (box, viewBox, gradient set) into @p out,
 * then overwrites the six user-affine fields (@c ua .. @c uf) with the
 * corresponding components of @p g. This produces a transform that has the
 * same viewBox/box mapping as @p t but uses the group's accumulated affine.
 *
 * @param[in]  t   Base transform whose box/viewBox/grads are preserved; must not be NULL.
 * @param[in]  g   Group affine to install in @p out.
 * @param[out] out Transform to write; must not be NULL; may alias @p t.
 *
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 * @pre  @p out is a valid non-NULL pointer to a writable @c svg_xform_t.
 *
 * @post @p out->ua .. @p out->uf equal the fields of @p g.
 * @post All other fields of @p out equal those of @p t.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_xform_with_group(const svg_xform_t* t, svg_utf_t g, svg_xform_t* out)
{
  *out    = *t;
  out->ua = g.a;
  out->ub = g.b;
  out->uc = g.c;
  out->ud = g.d;
  out->ue = g.e;
  out->uf = g.f;
}

/**
 * @brief Handle a 'g' open tag: compose its 'transform=' over the stack top and push.
 *
 * @details Builds a local transform by composing the current stack-top affine
 * with any 'transform=' attribute on the 'g' element via @c priv_ra8_svgp_apply_xform.
 * Self-closing 'g/' elements (detected by '/' before '>') and groups that
 * would exceed @c k_svg_g_depth_max are not pushed; in those cases @p gsp is
 * returned unchanged. Otherwise the new affine is pushed as @p gstk[@p gsp+1]
 * and @p gsp+1 is returned.
 *
 * @param[in] s     Full SVG document byte buffer; must not be NULL.
 * @param[in] i     Byte offset of the '<g' tag start.
 * @param[in] close Byte offset of the '>' terminator for the 'g' tag.
 * @param[in] t     Base coordinate transform (box/viewBox); must not be NULL.
 * @param[in] gstk  Group transform stack of at least @c k_svg_g_depth_max+1 elements; must not be NULL.
 * @param[in] gsp   Current group stack pointer (index of the top entry).
 *
 * @return int32_t The updated stack pointer after this open.
 * @retval gsp    Self-closing 'g' or depth-cap reached; stack unchanged.
 * @retval gsp+1  The new group's affine was pushed; stack pointer advanced.
 *
 * @pre  @p s is a valid pointer to at least @p close bytes.
 * @pre  @p gsp is in [0, k_svg_g_depth_max].
 *
 * @post @p gstk[@p gsp+1] holds the composed affine when the return value is @p gsp+1.
 * @post @p gstk and @p t are not modified when @p gsp is returned.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static int32_t internal_group_open(const uint8_t*     s,
                                   size_t             i,
                                   size_t             close,
                                   const svg_xform_t* t,
                                   svg_utf_t*         gstk,
                                   int32_t            gsp)
{
  svg_xform_t gt = {};
  internal_xform_with_group(t, gstk[gsp], &gt);
  priv_ra8_svgp_apply_xform(&gt, &s[i], (close > i) ? (close - i) : 0U);
  /*
   * internal_group_open is dispatched only for a well-formed element tag whose name
   * occupies at least one byte between '<' (scan position `i`) and '>'
   * (position `close`), so close > i is structurally invariant on every
   * reachable call. The close <= i arm defends against an empty "<>" tag that
   * the upstream element scanner never yields, so the first condition cannot be
   * flipped independently.
   */
  /* mcdc-deactivated: close>i is invariant for a named tag; the empty-"<>" (close<=i) arm is unreachable via public API. */
  const bool self_close = (close > i) && (s[close - 1U] == (uint8_t)'/');
  if (self_close || (gsp >= (int32_t)k_svg_g_depth_max)) {
    return gsp;
  }
  gstk[gsp + 1] =
    (svg_utf_t){.a = gt.ua, .b = gt.ub, .c = gt.uc, .d = gt.ud, .e = gt.ue, .f = gt.uf};
  return gsp + 1;
}

/**
 * @brief Walk every element in document order and draw the supported shapes.
 *
 * @details Maintains a bounded group transform stack of depth @c k_svg_g_depth_max.
 * Entering a 'g' element (via @c internal_group_open) composes its 'transform='
 * onto the current top and pushes. Encountering `</g>` pops. Each shape element
 * inherits the top group transform via @c internal_xform_with_group, then
 * composes its own element-level 'transform=' in @c internal_dispatch_shape.
 * The scan is bounded: each iteration advances past one '<...>' tag or breaks.
 *
 * @param[in] s   Full SVG document byte buffer; must not be NULL.
 * @param[in] len Total valid bytes in @p s.
 * @param[in] t   Base coordinate transform (box, viewBox, gradients); must not be NULL.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p t is a valid non-NULL pointer to an initialised @c svg_xform_t.
 *
 * @post All recognisable shape elements in @p s are drawn to the framebuffer.
 * @post @p t is not modified; local copies absorb all transform composition.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_draw_shapes(const uint8_t* s, size_t len, const svg_xform_t* t)
{
  svg_utf_t gstk[k_svg_g_depth_max + 1U];
  int32_t   gsp = 0;
  gstk[0]  = (svg_utf_t){.a = t->ua, .b = t->ub, .c = t->uc, .d = t->ud, .e = t->ue, .f = t->uf};
  size_t i = 0U;
  /* Bounded: each iteration advances past one '<...>' element or breaks at EOF. */
  while (i < len) {
    while ((i < len) && (s[i] != (uint8_t)'<')) {
      ++i;
    }
    if (i >= len) {
      break;
    }
    size_t close = i;
    while ((close < len) && (s[close] != (uint8_t)'>')) {
      ++close;
    }
    if (internal_elem_at(s, len, i, "</g")) {
      gsp = (gsp > 0) ? (gsp - 1) : 0;
    } else if (internal_elem_at(s, len, i, "<g")) {
      gsp = internal_group_open(s, i, close, t, gstk, gsp);
    } else {
      svg_xform_t ct = {};
      internal_xform_with_group(t, gstk[gsp], &ct);
      internal_dispatch_shape(s, len, i, close, &ct);
    }
    i = (close < len) ? (close + 1U) : len;
  }
}

/* ===========================================================================
 * Gradient definitions (<linearGradient> / <radialGradient> + <stop>)
 * ===========================================================================
 */

/**
 * @brief Read an SVG attribute as a floating-point number, returning a default when absent.
 *
 * @details Calls @c priv_ra8_svgp_attr to locate the attribute value slice, then forwards
 * that slice to @c priv_ra8_svgp_numf starting at offset zero. Returns @p def when the
 * attribute is absent. Used for reading gradient geometry attributes (x1, y1,
 * x2, y2, cx, cy, r) and stop offsets that are specified as decimal fractions.
 *
 * @param[in] s    Byte buffer containing the SVG tag text; must not be NULL.
 * @param[in] len  Total valid bytes in @p s.
 * @param[in] name NUL-terminated ASCII attribute name; must not be NULL.
 * @param[in] def  Value to return when the attribute is absent.
 *
 * @return float The parsed floating-point value, or @p def when absent.
 * @retval def  Attribute @p name was not found.
 * @retval n    The float value parsed from the attribute.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p name is a valid NUL-terminated ASCII string.
 *
 * @post @p s and @p name are not modified.
 * @post The return value equals @p def when @c priv_ra8_svgp_attr returns false.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static float internal_attr_numf(const uint8_t* s, size_t len, const char* name, float def)
{
  size_t off = 0U;
  size_t vl  = 0U;
  if (!priv_ra8_svgp_attr(s, len, name, &off, &vl)) {
    return def;
  }
  size_t k = 0U;
  return priv_ra8_svgp_numf(&s[off], vl, &k);
}

/**
 * @brief Copy the 'id' attribute value into @p dst as a NUL-terminated string.
 *
 * @details Looks up the "id" attribute via @c priv_ra8_svgp_attr. If absent, writes a
 * NUL byte at @p dst[0] and returns. Otherwise copies at most
 * @c k_svg_grad_id - 1 bytes from the attribute value to @p dst and appends a
 * NUL terminator. Used when scanning gradient definitions to store each
 * gradient's id for later url(#id) resolution.
 *
 * @param[in]  s   Byte buffer containing the SVG tag text; must not be NULL.
 * @param[in]  len Total valid bytes in @p s.
 * @param[out] dst Output buffer of at least @c k_svg_grad_id bytes; must not be NULL.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p dst is a valid pointer to at least @c k_svg_grad_id bytes.
 *
 * @post @p dst[0..n] is NUL-terminated where n <= @c k_svg_grad_id - 1.
 * @post @p s is not modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_copy_attr_id(const uint8_t* s, size_t len, char* dst)
{
  dst[0]     = '\0';
  size_t off = 0U;
  size_t vl  = 0U;
  if (!priv_ra8_svgp_attr(s, len, "id", &off, &vl)) {
    return;
  }
  const size_t n = (vl < (size_t)(k_svg_grad_id - 1U)) ? vl : (size_t)(k_svg_grad_id - 1U);
  for (size_t i = 0U; i < n; ++i) {
    dst[i] = (char)s[off + i];
  }
  dst[n] = '\0';
}

/**
 * @brief Clamp a float value to the unit interval [0, 1].
 *
 * @details Returns 0.0F when @p v < 0.0F, returns @c s_svg_unit (1.0F) when
 * @p v > @c s_svg_unit, and returns @p v unchanged otherwise. Used to clamp
 * parsed gradient stop offsets to a valid range before they are stored in the
 * gradient descriptor.
 *
 * @param[in] v Float value to clamp.
 *
 * @return float The value clamped to [0.0F, 1.0F].
 * @retval 0.0F         When @p v < 0.0F.
 * @retval s_svg_unit   When @p v > @c s_svg_unit.
 * @retval v            When @p v is already in [0.0F, 1.0F].
 *
 * @pre  @p v is a finite float value.
 * @pre  @c s_svg_unit is initialised to 1.0F (module-scope constant).
 *
 * @post The return value is in [0.0F, 1.0F].
 * @post @p v is not modified.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static float internal_clamp01(float v)
{
  if (v < 0.0F) {
    return 0.0F;
  }
  return (v > s_svg_unit) ? s_svg_unit : v;
}

/**
 * @brief Parse a gradient 'stop' element's 'offset' attribute.
 *
 * @details Reads the "offset" attribute from the stop tag via @c priv_ra8_svgp_attr and
 * parses its value as a float via @c priv_ra8_svgp_numf. If a '%' character is found
 * anywhere in the attribute value span, the result is divided by the function-local
 * 100.0F percentage scale to convert from percentage to fraction. Returns 0.0F when the
 * attribute is absent. The caller is responsible for clamping via @c internal_clamp01.
 *
 * @param[in] tag  Byte span of the 'stop' element tag; must not be NULL.
 * @param[in] tlen Total valid bytes in @p tag.
 *
 * @return float The offset as a fraction in [0, 1] (unclamped), or 0.0F if absent.
 * @retval 0.0F  Attribute "offset" is absent.
 * @retval n     Parsed offset; divide by 100 if given as a percentage.
 *
 * @pre  @p tag is a valid pointer to at least @p tlen bytes.
 * @pre  @p tlen > 0 when the tag is expected to contain attributes.
 *
 * @post @p tag is not modified.
 * @post The return value may be outside [0, 1] before the caller clamps it.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static float internal_stop_offset(const uint8_t* tag, size_t tlen)
{
  static const float k_percent = 100.0F;
  size_t             ooff      = 0U;
  size_t             ovl       = 0U;
  if (!priv_ra8_svgp_attr(tag, tlen, "offset", &ooff, &ovl)) {
    return 0.0F;
  }
  size_t      k   = 0U;
  const float off = priv_ra8_svgp_numf(&tag[ooff], ovl, &k);
  for (size_t z = 0U; z < ovl; ++z) {
    if (tag[ooff + z] == (uint8_t)'%') {
      return off / k_percent;
    }
  }
  return off;
}

/**
 * @brief Parse a gradient 'stop' element's 'stop-color' attribute.
 *
 * @details Reads the "stop-color" attribute from the stop tag via @c priv_ra8_svgp_attr
 * and parses the value as a paint via @c priv_ra8_svgp_paint. If the attribute is absent
 * or the paint parses to ::k_svg_no_paint (e.g. "none"), returns the SVG
 * default fill colour ::k_svg_def_fill (black).
 *
 * @param[in] tag  Byte span of the 'stop' element tag; must not be NULL.
 * @param[in] tlen Total valid bytes in @p tag.
 *
 * @return uint32_t The stop colour in 0x00RRGGBB format.
 * @retval (uint32_t)k_svg_def_fill  Attribute absent or parses to no-paint.
 * @retval 0x000000..0xFFFFFF        The parsed solid stop colour.
 *
 * @pre  @p tag is a valid pointer to at least @p tlen bytes.
 * @pre  @p tlen > 0 when the tag is expected to contain attributes.
 *
 * @post @p tag is not modified.
 * @post The return value has bits [31:24] clear.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_stop_color(const uint8_t* tag, size_t tlen)
{
  size_t coff = 0U;
  size_t cvl  = 0U;
  if (!priv_ra8_svgp_attr(tag, tlen, "stop-color", &coff, &cvl)) {
    return (uint32_t)k_svg_def_fill;
  }
  const uint32_t pc = priv_ra8_svgp_paint(&tag[coff], cvl);
  return (pc != (uint32_t)k_svg_no_paint) ? pc : (uint32_t)k_svg_def_fill;
}

/**
 * @brief Parse the 'stop' child elements of a gradient definition into @p g.
 *
 * @details Scans @p s[@p from .. @p end) for '<stop' tags using @c priv_ra8_svgp_find_ci.
 * For each found stop tag, extracts the offset via @c internal_stop_offset (clamped
 * via @c internal_clamp01) and the colour via @c internal_stop_color, stores them in
 * @p g->stops[@p g->nstops], and increments @p g->nstops. Bounded by
 * @c k_svg_grad_stops; stops beyond the limit are silently dropped.
 *
 * @param[in]     s    Full SVG document byte buffer; must not be NULL.
 * @param[in]     from Start offset of the gradient body (after the opening tag).
 * @param[in]     end  End offset (exclusive; position of the closing tag).
 * @param[in,out] g    Gradient to populate; @c nstops and @c stops are updated.
 *
 * @pre  @p s is a valid pointer to at least @p end bytes.
 * @pre  @p g is a valid non-NULL pointer with @p g->nstops <= @c k_svg_grad_stops.
 *
 * @post @p g->stops[0..nstops-1] hold the parsed stop data.
 * @post @p g->nstops <= @c k_svg_grad_stops.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_parse_stops(const uint8_t* s, size_t from, size_t end, svg_grad_t* g)
{
  size_t at = from;
  /* Bounded: <= k_svg_grad_stops stops; `at` advances past each `<stop>`. */
  while (g->nstops < (uint8_t)k_svg_grad_stops) {
    const size_t sp = priv_ra8_svgp_find_ci(s, end, at, "<stop");
    if (sp >= end) {
      break;
    }
    size_t close = sp;
    while ((close < end) && (s[close] != (uint8_t)'>')) {
      ++close;
    }
    const uint8_t* tag      = &s[sp];
    const size_t   tlen     = (close > sp) ? (close - sp) : 0U;
    g->stops[g->nstops].off = internal_clamp01(internal_stop_offset(tag, tlen));
    g->stops[g->nstops].col = internal_stop_color(tag, tlen);
    g->nstops += 1U;
    at = (close < end) ? (close + 1U) : end;
  }
}

/**
 * @brief Scan the document for gradient definitions and populate @p gs.
 *
 * @details Searches the document for '<linearGradient' and '<radialGradient'
 * opening tags in document order. For each found gradient, reads the 'id',
 * 'x1', 'y1', 'x2', 'y2' (linear) or 'cx', 'cy', 'r' (radial) attributes
 * and populates a new entry in @p gs->g. Then locates the matching closing
 * tag and calls @c internal_parse_stops to fill the stop list. Bounded by
 * @c k_svg_grad_max; additional gradients are silently dropped.
 *
 * @param[in]  s   Full SVG document byte buffer; must not be NULL.
 * @param[in]  len Total valid bytes in @p s.
 * @param[out] gs  Gradient set to populate; @c n and @c g are written.
 *
 * @pre  @p s is a valid pointer to at least @p len bytes.
 * @pre  @p gs is a valid non-NULL pointer to a writable @c svg_grads_t.
 *
 * @post @p gs->n holds the count of parsed gradients (<= @c k_svg_grad_max).
 * @post @p gs->g[0..n-1] are valid, fully-populated @c svg_grad_t entries.
 *
 * @note Not thread-safe in isolation; callers in this module are
 *       single-threaded during SVG render.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_scan_grads(const uint8_t* s, size_t len, svg_grads_t* gs)
{
  static const float k_half = 0.5F;
  gs->n                     = 0;
  size_t from               = 0U;
  /* Bounded: <= k_svg_grad_max gradients; `from` strictly advances each pass. */
  while (gs->n < (int32_t)k_svg_grad_max) {
    const size_t lin    = priv_ra8_svgp_find_ci(s, len, from, "<linearGradient");
    const size_t rad    = priv_ra8_svgp_find_ci(s, len, from, "<radialGradient");
    const bool   is_rad = (rad < lin);
    const size_t at     = is_rad ? rad : lin;
    if (at >= len) {
      break;
    }
    size_t close = at;
    while ((close < len) && (s[close] != (uint8_t)'>')) {
      ++close;
    }
    const uint8_t* tag  = &s[at];
    const size_t   tlen = (close > at) ? (close - at) : 0U;
    svg_grad_t*    g    = &gs->g[gs->n];
    *g                  = (svg_grad_t){};
    g->kind             = is_rad ? (uint8_t)k_svg_grad_radial : (uint8_t)k_svg_grad_linear;
    internal_copy_attr_id(tag, tlen, g->id);
    if (is_rad) {
      g->x1 = internal_attr_numf(tag, tlen, "cx", k_half);
      g->y1 = internal_attr_numf(tag, tlen, "cy", k_half);
      g->x2 = internal_attr_numf(tag, tlen, "r", k_half);
    } else {
      g->x1 = internal_attr_numf(tag, tlen, "x1", 0.0F);
      g->y1 = internal_attr_numf(tag, tlen, "y1", 0.0F);
      g->x2 = internal_attr_numf(tag, tlen, "x2", s_svg_unit);
      g->y2 = internal_attr_numf(tag, tlen, "y2", 0.0F);
    }
    const char*  endtag  = is_rad ? "</radialGradient" : "</linearGradient";
    const size_t endpos  = priv_ra8_svgp_find_ci(s, len, close, endtag);
    const size_t stopend = (endpos < len) ? endpos : len;
    internal_parse_stops(s, close, stopend, g);
    if (g->nstops > 0U) {
      gs->n += 1;
    }
    from = (endpos < len) ? (endpos + 1U) : len;
  }
}

/* ===========================================================================
 * Public API
 * ===========================================================================
 */

bool ra8_svg_is_svg(const uint8_t* bytes, size_t len)
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
  while ((i < len) && priv_ra8_svgp_ws((char)bytes[i])) {
    ++i;
  }
  return priv_ra8_svgp_starts_ci(bytes, len, i, "<?xml") ||
         priv_ra8_svgp_starts_ci(bytes, len, i, "<svg");
}

ra8_err_t ra8_svg_image_href(const uint8_t* svg, size_t len, size_t* out_off, size_t* out_len)
{
  if ((svg == nullptr) || (out_off == nullptr) || (out_len == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  size_t open  = 0U;
  size_t close = 0U;
  if (!internal_tag_span(svg, len, 0U, "<image", &open, &close)) {
    return k_ra8_err_not_found;
  }
  const uint8_t* tag  = &svg[open];
  const size_t   tlen = close - open;
  size_t         voff = 0U;
  size_t         vlen = 0U;
  if (!priv_ra8_svgp_attr(tag, tlen, "xlink:href", &voff, &vlen) &&
      !priv_ra8_svgp_attr(tag, tlen, "href", &voff, &vlen)) {
    return k_ra8_err_not_found;
  }
  *out_off = open + voff;
  *out_len = vlen;
  return k_ra8_ok;
}

ra8_err_t ra8_svg_size(const uint8_t* svg, size_t len, int32_t* out_w, int32_t* out_h)
{
  if ((svg == nullptr) || (out_w == nullptr) || (out_h == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  size_t open  = 0U;
  size_t close = 0U;
  if (!internal_tag_span(svg, len, 0U, "<svg", &open, &close)) {
    return k_ra8_err_not_found;
  }
  const uint8_t* tag  = &svg[open];
  const size_t   tlen = close - open;
  int32_t        w    = priv_ra8_svgp_attr_num(tag, tlen, "width", 0);
  int32_t        h    = priv_ra8_svgp_attr_num(tag, tlen, "height", 0);
  if ((w <= 0) || (h <= 0)) {
    size_t voff = 0U;
    size_t vlen = 0U;
    if (priv_ra8_svgp_attr(tag, tlen, "viewBox", &voff, &vlen)) {
      int32_t nums[k_svg_viewbox_n] = {};
      size_t  k                     = 0U;
      for (size_t n = 0U; n < (size_t)k_svg_viewbox_n; ++n) {
        nums[n] = priv_ra8_svgp_num(&tag[voff], vlen, &k);
      }
      w = nums[2];
      h = nums[3];
    }
  }
  if ((w <= 0) || (h <= 0)) {
    return k_ra8_err_not_found;
  }
  *out_w = w;
  *out_h = h;
  return k_ra8_ok;
}

ra8_err_t ra8_svg_render(const uint8_t* svg, size_t len, int32_t x, int32_t y, int32_t w, int32_t h)
{
  if (svg == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((w <= 0) || (h <= 0)) {
    return k_ra8_err_invalid_arg;
  }
  svg_xform_t t     = {.bx = x,
                       .by = y,
                       .bw = w,
                       .bh = h,
                       .ua = 1.0F,
                       .ub = 0.0F,
                       .uc = 0.0F,
                       .ud = 1.0F,
                       .ue = 0.0F,
                       .uf = 0.0F};
  svg_grads_t grads = {};
  internal_scan_grads(svg, len, &grads);
  t.grads = &grads;
  internal_read_viewbox(svg, len, &t);
  internal_draw_shapes(svg, len, &t);
  return k_ra8_ok;
}
