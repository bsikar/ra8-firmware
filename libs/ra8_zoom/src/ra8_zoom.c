/**
 * @file ra8_zoom.c
 * @brief Viewport state machine for the tap-to-zoom image viewer (#478).
 *
 * @details Implements the non-rendering half of ra8_zoom.h: opening a viewport
 *          onto a source, the integer magnification ladder, anchor clamping in
 *          the magnified plane, discrete pan steps, and the dirty/waveform
 *          bookkeeping the reader turns into a display flush. Pure integer
 *          arithmetic with no MMIO and no allocation, so it behaves identically
 *          on the unit-test host, in ra8_emulator and on silicon. The strip
 *          composite lives in ra8_zoom_render.c.
 *
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_zoom.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_ui.h"
#include "ra8_zoom_internal.h"

/** @brief Component tag for `RA8_CHECK_*` log lines. */
static const char* const s_tag = "ra8_zoom";

/**
 * @enum zoom_step_t
 * @brief Pan-step geometry and the ladder multiplier (no magic numbers).
 *
 * @details One discrete pan step moves the viewport by its own extent less
 *          `extent / k_zoom_overlap_div`, so a strip of the previous view stays
 *          on screen and the reader never loses their place. Expressed as a
 *          divisor rather than a pixel count so it scales with the viewport: a
 *          320 px loupe keeps 40 px, a 1024 px full-screen view keeps 128 px.
 *
 * @invariant k_zoom_overlap_div > 1, so a step always advances.
 *
 * @par Example:
 * @code
 * const int32_t step = v->dst.w - (v->dst.w / (int32_t)k_zoom_overlap_div);
 * @endcode
 *
 * @see ra8_zoom_view_pan_dir
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_zoom_overlap_div = 8U, /**< Viewport fraction retained across a pan step. */
  k_zoom_scale_dbl   = 2U, /**< Ladder multiplier (doubling, not increment).  */
} zoom_step_t;

void ra8_zoom_priv_axis(int32_t          anchor,
                        int32_t          extent,
                        int32_t          scale,
                        int32_t          src_dim,
                        ra8_zoom_axis_t* out)
{
  const int32_t plane = src_dim * scale;
  int32_t       d0    = -anchor;
  if (d0 < 0) {
    d0 = 0;
  }
  int32_t d1 = plane - anchor;
  if (d1 > extent) {
    d1 = extent;
  }
  if (d1 < d0) {
    d1 = d0;
  }
  out->d0 = d0;
  out->d1 = d1;
  out->s0 = 0U;
  /* Beyond this point `anchor + d0 >= 0` by construction, so the source-index
   * divisions never touch a negative numerator. */
  if (d1 <= d0) {
    out->count = 0U;
    return;
  }
  const int32_t s0 = (anchor + d0) / scale;
  const int32_t s1 = (anchor + d1 - 1) / scale;
  /* `span` is a named object rather than the composite `(s1 - s0) + 1` inline,
   * so the widening cast below converts a simple expression: casting the
   * composite would change essential type mid-expression (MISRA 10.8). It is
   * positive by construction -- s1 >= s0 whenever d1 > d0. */
  const int32_t span = (s1 - s0) + 1;
  out->s0            = (uint32_t)s0;
  out->count         = (uint32_t)span;
}

/**
 * @brief Whether a source dimension is inside the engine's addressable range.
 * @details The single place the engine's addressable range is decided. Both
 *          bounds matter: zero has no pixels to magnify, and a dimension past
 *          the cap could overflow the magnified plane at maximum zoom.
 * @param[in] dim Candidate width or height in pixels.
 * @return True when `dim` is in `[1, k_ra8_zoom_dim_max]`.
 * @retval true  The dimension is usable.
 * @retval false Zero, or large enough that the magnified plane could overflow.
 * @pre  None -- every uint32 value has a defined answer.
 * @pre  @p dim is a pixel count, not a byte count.
 * @post No state is modified (pure).
 * @post A true result guarantees `dim * k_ra8_zoom_scale_max` fits in int32.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool zoom_dim_ok(uint32_t dim)
{
  return (dim != 0U) && (dim <= (uint32_t)k_ra8_zoom_dim_max);
}

/**
 * @brief Saturating signed addition (never wraps at the int32 boundary).
 * @details A pan delta is caller-supplied and may be arbitrary, so the anchor
 *          addition is done in 64 bits and clamped rather than allowed to wrap
 *          -- a wrapped anchor would clamp to the wrong edge.
 * @param[in] a First addend.
 * @param[in] b Second addend.
 * @return The sum, clamped to the int32 range.
 * @retval INT32_MAX The true sum overflowed positive.
 * @retval INT32_MIN The true sum overflowed negative.
 * @pre  @p a and @p b are valid signed integers.
 * @pre  The result is only ever consumed by the anchor clamp.
 * @post No state is mutated.
 * @post The result never wraps around the int32 boundary.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static int32_t zoom_sat_add(int32_t a, int32_t b)
{
  const int64_t sum = (int64_t)a + (int64_t)b;
  if (sum > (int64_t)INT32_MAX) {
    return INT32_MAX;
  }
  if (sum < (int64_t)INT32_MIN) {
    return INT32_MIN;
  }
  return (int32_t)sum;
}

/**
 * @brief Clamp one anchor against the magnified plane and the viewport extent.
 * @details Two regimes, decided by whether the magnified image is bigger than
 *          the viewport. Bigger: the anchor is held inside `[0, plane - extent]`
 *          so the viewport never shows past an edge. Smaller: the image is
 *          centred and the anchor goes negative, which the render reads as a
 *          letterbox rather than as an error.
 * @param[in] anchor Proposed magnified-plane coordinate at viewport offset 0.
 * @param[in] plane  Magnified plane extent on this axis (`src_dim * scale`).
 * @param[in] extent Viewport extent on this axis, destination pixels.
 * @return The clamped anchor.
 * @retval "negative" The image is smaller than the viewport: the centring offset.
 * @retval "[0, plane - extent]" Otherwise, the in-range anchor.
 * @pre  @p extent is positive (validated at open).
 * @pre  @p plane is `src_dim * scale` for the same axis.
 * @post The result satisfies the anchor invariant of ::ra8_zoom_view_t.
 * @post No state is mutated.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static int32_t zoom_clamp_anchor(int32_t anchor, int32_t plane, int32_t extent)
{
  if (plane <= extent) {
    /* Image smaller than the viewport at this zoom: centre it, letting the
     * anchor go negative. The render reads that as a letterbox. */
    return -((extent - plane) / 2);
  }
  if (anchor < 0) {
    return 0;
  }
  if (anchor > (plane - extent)) {
    return plane - extent;
  }
  return anchor;
}

/**
 * @brief Re-clamp both anchors against the current source, scale and viewport.
 * @details Called after anything that can invalidate an anchor -- a scale
 *          change, a pan, a rebind onto a differently-sized page -- so the
 *          clamp invariant is re-established in exactly one place instead of
 *          at every mutation site.
 * @param[in,out] v Open view whose anchors may be out of range.
 * @return Nothing.
 * @pre  @p v is non-NULL and open.
 * @pre  `v->scale` is already the scale the anchors should be clamped for.
 * @post Both anchors satisfy the clamp invariant of ::ra8_zoom_view_t.
 * @post Nothing but the anchors is modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void zoom_reclamp(ra8_zoom_view_t* v)
{
  const int32_t scale = (int32_t)v->scale;
  v->anchor_x         = zoom_clamp_anchor(v->anchor_x, (int32_t)v->src.width * scale, v->dst.w);
  v->anchor_y         = zoom_clamp_anchor(v->anchor_y, (int32_t)v->src.height * scale, v->dst.h);
}

/**
 * @brief Record an interactive viewport change and schedule the flush it owes.
 * @details The one place the dirty flag and the waveform class are set, so the
 *          policy is applied uniformly: under the responsive policy an
 *          interactive change owes a fast (bi-level) flush and arms the settle
 *          timer; under the quality policy it owes a full 16-level flush and
 *          arms nothing.
 * @param[in,out] v      Open view.
 * @param[in]     now_ms Current millisecond timestamp.
 * @return Nothing.
 * @pre  @p v is non-NULL and open.
 * @pre  @p now_ms comes from a monotonic millisecond source.
 * @post `v->pending` is true and `v->pending_kind` matches `v->policy`.
 * @post `v->settle_armed` is true only under ::k_ra8_zoom_policy_responsive.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void zoom_touch(ra8_zoom_view_t* v, uint32_t now_ms)
{
  const bool responsive = (v->policy == k_ra8_zoom_policy_responsive);
  v->last_input_ms      = now_ms;
  v->pending            = true;
  v->pending_kind       = responsive ? k_ra8_zoom_refresh_fast : k_ra8_zoom_refresh_quality;
  v->settle_armed       = responsive;
}

/**
 * @brief Validate every pointer a configuration must carry.
 * @details Split out of ::ra8_zoom_view_open so each validation stage stays
 *          inside the project's function-size bar, and so a missing pointer is
 *          rejected before any sizing arithmetic reads through it.
 * @param[in] cfg Candidate configuration (already known non-NULL).
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Every required pointer is present.
 * @retval k_ra8_err_null_ptr One is missing; the log line names it.
 * @pre  @p cfg is non-NULL.
 * @pre  The caller has not yet published any of @p cfg into a view.
 * @post No state is modified.
 * @post On k_ra8_ok the source seam and all three scratch buffers are non-NULL.
 * @note Not thread-safe (logs).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t zoom_cfg_ptrs_ok(const ra8_zoom_view_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg->src.read, s_tag, "source read seam must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->scratch.row, s_tag, "scratch.row must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->scratch.strip, s_tag, "scratch.strip must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->scratch.packed, s_tag, "scratch.packed must not be nullptr");
  return k_ra8_ok;
}

/**
 * @brief Validate the viewport extent and the source extent.
 * @details Runs after the pointer check and before the scratch sizing, which
 *          divides by the viewport width: an empty viewport has to be rejected
 *          first. The source bound is what makes the magnified-plane arithmetic
 *          overflow-free downstream.
 * @param[in] cfg Candidate configuration (already known non-NULL).
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Both extents are usable.
 * @retval k_ra8_err_invalid_arg An extent is empty, or the source is too large.
 * @pre  @p cfg is non-NULL.
 * @pre  ::zoom_cfg_ptrs_ok has already passed for @p cfg.
 * @post No state is modified.
 * @post On k_ra8_ok the magnified plane cannot overflow int32.
 * @note Not thread-safe (logs).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t zoom_cfg_extent_ok(const ra8_zoom_view_cfg_t* cfg)
{
  if ((cfg->dst.w <= 0) || (cfg->dst.h <= 0)) {
    ra8_log_error(s_tag, "viewport must have positive width and height");
    return k_ra8_err_invalid_arg;
  }
  if (!zoom_dim_ok(cfg->src.width) || !zoom_dim_ok(cfg->src.height)) {
    ra8_log_error(s_tag, "source extent is zero or exceeds k_ra8_zoom_dim_max");
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Derive the strip height from the scratch budget and check every capacity.
 * @details Derives the strip height from the strip budget rather than taking it
 *          as a parameter, so the caller tunes the composite by sizing a buffer
 *          and cannot ask for a strip its packed buffer could not hold. The
 *          height is capped by the module maximum and by the viewport itself.
 * @param[in]  cfg            Candidate configuration (already known non-NULL).
 * @param[out] out_strip_rows Destination rows one composite strip can hold.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok         @p out_strip_rows holds a workable strip height.
 * @retval k_ra8_err_no_mem A buffer is too small for this viewport width.
 * @pre  @p cfg passed ::zoom_cfg_extent_ok, so `dst.w > 0`.
 * @pre  @p out_strip_rows addresses writable storage.
 * @post On k_ra8_ok `*out_strip_rows` is in `[1, k_ra8_zoom_strip_rows_max]`.
 * @post On k_ra8_ok the packed buffer can hold one strip of that height.
 * @note Not thread-safe (logs).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t zoom_cfg_scratch_ok(const ra8_zoom_view_cfg_t* cfg, uint16_t* out_strip_rows)
{
  const uint32_t width = (uint32_t)cfg->dst.w;
  if ((cfg->scratch.row_cap < width) || (cfg->scratch.strip_cap < width)) {
    ra8_log_error(s_tag, "scratch row/strip buffer is narrower than the viewport");
    return k_ra8_err_no_mem;
  }
  uint32_t rows = cfg->scratch.strip_cap / width;
  if (rows > (uint32_t)k_ra8_zoom_strip_rows_max) {
    rows = (uint32_t)k_ra8_zoom_strip_rows_max;
  }
  if (rows > (uint32_t)cfg->dst.h) {
    rows = (uint32_t)cfg->dst.h;
  }
  if (cfg->scratch.packed_cap < (((width * rows) + 1U) / 2U)) {
    ra8_log_error(s_tag, "scratch packed buffer cannot hold one dithered strip");
    return k_ra8_err_no_mem;
  }
  *out_strip_rows = (uint16_t)rows;
  return k_ra8_ok;
}

/**
 * @brief Resolve and validate the magnification ladder a configuration asks for.
 * @details Resolves the two "0 means default" fields before validating them,
 *          so an all-zero configuration opens at 1:1 with the engine ceiling
 *          instead of being rejected, while an out-of-range explicit value
 *          still fails closed.
 * @param[in]  cfg         Candidate configuration (already known non-NULL).
 * @param[out] out_scale   Receives the opening magnification.
 * @param[out] out_ceiling Receives the effective ladder ceiling.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Both values are on the engine ladder.
 * @retval k_ra8_err_invalid_arg The ceiling or the opening scale is off it.
 * @pre  @p cfg is non-NULL.
 * @pre  @p out_scale and @p out_ceiling address writable storage.
 * @post On k_ra8_ok `k_ra8_zoom_scale_min <= *out_scale <= *out_ceiling`.
 * @post On k_ra8_ok `*out_ceiling <= k_ra8_zoom_scale_max`.
 * @note Not thread-safe (logs).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
zoom_cfg_ladder_ok(const ra8_zoom_view_cfg_t* cfg, uint8_t* out_scale, uint8_t* out_ceiling)
{
  /* The "0 means default" rewrite happens first and lifts both values to at
   * least k_ra8_zoom_scale_min, so a `< min` half in either check below could
   * never be true. Testing it anyway would be a condition that cannot vary --
   * unreachable by construction, and dead weight in an MC/DC argument. Each
   * check is therefore the one bound that can actually fail. */
  const uint8_t ceiling = (cfg->scale_max == 0U) ? (uint8_t)k_ra8_zoom_scale_max : cfg->scale_max;
  const uint8_t opening = (cfg->scale == 0U) ? (uint8_t)k_ra8_zoom_scale_min : cfg->scale;
  if (ceiling > (uint8_t)k_ra8_zoom_scale_max) {
    ra8_log_error(s_tag, "scale_max is outside the engine ladder");
    return k_ra8_err_invalid_arg;
  }
  if (opening > ceiling) {
    ra8_log_error(s_tag, "opening scale is outside the configured ladder");
    return k_ra8_err_invalid_arg;
  }
  *out_scale   = opening;
  *out_ceiling = ceiling;
  return k_ra8_ok;
}

ra8_err_t ra8_zoom_source_init(ra8_zoom_source_t* out,
                               ra8_zoom_read_fn   read,
                               void*              ctx,
                               uint32_t           width,
                               uint32_t           height)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "source out must not be nullptr");
  RA8_CHECK_NULL_PTR(read, s_tag, "source read seam must not be nullptr");
  if (!zoom_dim_ok(width) || !zoom_dim_ok(height)) {
    ra8_log_error(s_tag, "source extent is zero or exceeds k_ra8_zoom_dim_max");
    return k_ra8_err_invalid_arg;
  }
  out->read   = read;
  out->ctx    = ctx;
  out->width  = width;
  out->height = height;
  return k_ra8_ok;
}

/**
 * @brief Run every configuration check, in the order they depend on each other.
 *
 * @details The order is not stylistic. Pointers first, because the extent check
 *          reads through them; extents second, because the scratch sizing
 *          divides by the viewport width; the ladder last, because it is the
 *          only check with no dependency on the others. Split out of
 *          ::ra8_zoom_view_open so that function is "check, then adopt".
 *
 * @param[in]  cfg            Candidate configuration (already known non-NULL).
 * @param[out] out_strip_rows Receives the resolved composite strip height.
 * @param[out] out_scale      Receives the resolved opening magnification.
 * @param[out] out_ceiling    Receives the resolved ladder ceiling.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    Every check passed; all three outputs are resolved.
 * @retval k_ra8_err_* The first failing check's code, verbatim.
 *
 * @pre  @p cfg is non-NULL.
 * @pre  All three out-parameters address writable storage.
 * @post No state outside the out-parameters is modified.
 * @post On any error the out-parameters hold no meaningful value.
 *
 * @note Not thread-safe (logs).
 * @see ra8_zoom_view_open
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t zoom_cfg_ok(const ra8_zoom_view_cfg_t* cfg,
                             uint16_t*                  out_strip_rows,
                             uint8_t*                   out_scale,
                             uint8_t*                   out_ceiling)
{
  RA8_RETURN_ON_ERROR(zoom_cfg_ptrs_ok(cfg), s_tag, "cfg pointer check");
  RA8_RETURN_ON_ERROR(zoom_cfg_extent_ok(cfg), s_tag, "cfg extent check");
  RA8_RETURN_ON_ERROR(zoom_cfg_scratch_ok(cfg, out_strip_rows), s_tag, "cfg scratch check");
  return zoom_cfg_ladder_ok(cfg, out_scale, out_ceiling);
}

/**
 * @brief Publish a validated configuration into a view and open it.
 *
 * @details The last step of ::ra8_zoom_view_open, split out so the open path
 *          reads as "validate, validate, validate, adopt" rather than as one
 *          long function. Nothing here can fail: every value has already been
 *          checked, so this is assignment plus the initial clamp. The anchor is
 *          derived from the focus point in SOURCE pixels -- scaled into the
 *          magnified plane, then backed off by half a viewport so the focus
 *          lands in the middle rather than the corner.
 *
 * @param[out] v          View to populate (its previous contents are discarded).
 * @param[in]  cfg        The validated configuration.
 * @param[in]  opening    Resolved opening magnification.
 * @param[in]  ceiling    Resolved ladder ceiling.
 * @param[in]  strip_rows Resolved composite strip height.
 *
 * @return Nothing.
 *
 * @pre  @p cfg passed every ::zoom_cfg_* check.
 * @pre  @p opening, @p ceiling and @p strip_rows are their resolved values.
 * @post `v->active` is true and a quality flush is owed.
 * @post Both anchors satisfy the clamp invariant of ::ra8_zoom_view_t.
 *
 * @note Not thread-safe.
 * @see ra8_zoom_view_open
 * @since 0.1.0
 */
RA8_INTERNAL
static void zoom_view_adopt(ra8_zoom_view_t*           v,
                            const ra8_zoom_view_cfg_t* cfg,
                            uint8_t                    opening,
                            uint8_t                    ceiling,
                            uint16_t                   strip_rows)
{
  v->src        = cfg->src;
  v->scratch    = cfg->scratch;
  v->dst        = cfg->dst;
  v->scale      = opening;
  v->scale_max  = ceiling;
  v->policy     = cfg->policy;
  v->settle_ms  = (cfg->settle_ms == 0U) ? (uint16_t)k_ra8_zoom_settle_ms_default : cfg->settle_ms;
  v->strip_rows = strip_rows;
  v->anchor_x   = (cfg->focus_x * (int32_t)opening) - (cfg->dst.w / 2);
  v->anchor_y   = (cfg->focus_y * (int32_t)opening) - (cfg->dst.h / 2);
  v->last_input_ms = 0U;
  v->settle_armed  = false;
  v->pending       = true;
  v->pending_kind  = k_ra8_zoom_refresh_quality;
  v->active        = true;
  zoom_reclamp(v);
}

ra8_err_t ra8_zoom_view_open(ra8_zoom_view_t* v, const ra8_zoom_view_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(v, s_tag, "view must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  v->active           = false;
  uint16_t strip_rows = 0U;
  uint8_t  opening    = 0U;
  uint8_t  ceiling    = 0U;
  RA8_RETURN_ON_ERROR(zoom_cfg_ok(cfg, &strip_rows, &opening, &ceiling), s_tag, "cfg check");
  zoom_view_adopt(v, cfg, opening, ceiling, strip_rows);
  return k_ra8_ok;
}

ra8_err_t ra8_zoom_view_rebind(ra8_zoom_view_t* v, const ra8_zoom_source_t* src, uint32_t now_ms)
{
  RA8_CHECK_NULL_PTR(v, s_tag, "view must not be nullptr");
  RA8_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA8_CHECK_NULL_PTR(src->read, s_tag, "source read seam must not be nullptr");
  if (!v->active) {
    ra8_log_error(s_tag, "rebind on a closed view");
    return k_ra8_err_invalid_state;
  }
  if (!zoom_dim_ok(src->width) || !zoom_dim_ok(src->height)) {
    ra8_log_error(s_tag, "source extent is zero or exceeds k_ra8_zoom_dim_max");
    return k_ra8_err_invalid_arg;
  }
  v->src = *src;
  zoom_reclamp(v);
  zoom_touch(v, now_ms);
  return k_ra8_ok;
}

ra8_err_t ra8_zoom_view_invalidate(ra8_zoom_view_t* v, uint32_t now_ms)
{
  RA8_CHECK_NULL_PTR(v, s_tag, "view must not be nullptr");
  if (!v->active) {
    ra8_log_error(s_tag, "invalidate on a closed view");
    return k_ra8_err_invalid_state;
  }
  zoom_touch(v, now_ms);
  return k_ra8_ok;
}

ra8_err_t ra8_zoom_view_close(ra8_zoom_view_t* v)
{
  RA8_CHECK_NULL_PTR(v, s_tag, "view must not be nullptr");
  v->active       = false;
  v->pending      = false;
  v->settle_armed = false;
  return k_ra8_ok;
}

bool ra8_zoom_view_active(const ra8_zoom_view_t* v)
{
  return (v != nullptr) && v->active;
}

uint8_t ra8_zoom_scale_cycle(uint8_t scale, uint8_t min, uint8_t max)
{
  const uint8_t floor_v   = (min == 0U) ? (uint8_t)k_ra8_zoom_scale_min : min;
  const uint8_t ceiling_v = (max < floor_v) ? floor_v : max;
  if (scale >= ceiling_v) {
    return floor_v;
  }
  const uint16_t doubled = (uint16_t)((uint16_t)scale * (uint16_t)k_zoom_scale_dbl);
  if (doubled >= (uint16_t)ceiling_v) {
    return ceiling_v;
  }
  if (doubled < (uint16_t)floor_v) {
    return floor_v;
  }
  return (uint8_t)doubled;
}

ra8_err_t ra8_zoom_view_set_scale(ra8_zoom_view_t* v,
                                  uint8_t          scale,
                                  int32_t          focus_x,
                                  int32_t          focus_y,
                                  uint32_t         now_ms)
{
  RA8_CHECK_NULL_PTR(v, s_tag, "view must not be nullptr");
  if (!v->active) {
    ra8_log_error(s_tag, "set_scale on a closed view");
    return k_ra8_err_invalid_state;
  }
  if ((scale < (uint8_t)k_ra8_zoom_scale_min) || (scale > v->scale_max)) {
    ra8_log_error(s_tag, "requested scale is off the ladder");
    return k_ra8_err_out_of_range;
  }
  if (scale == v->scale) {
    return k_ra8_ok;
  }
  /* Keep the plane point under (focus_x, focus_y) fixed: rescale that plane
   * coordinate by new/old, then re-derive the anchor from it. The intermediate
   * product is 64-bit (a plane coordinate times the new scale can exceed int32
   * before the divide brings it back), and each narrowing cast takes a NAMED
   * 64-bit object rather than a composite expression -- casting the quotient
   * in place would change essential type mid-expression (MISRA 10.8). */
  const int32_t off_x    = focus_x - v->dst.x;
  const int32_t off_y    = focus_y - v->dst.y;
  const int64_t old      = (int64_t)v->scale;
  const int64_t plane_x  = (int64_t)(v->anchor_x + off_x) * (int64_t)scale;
  const int64_t plane_y  = (int64_t)(v->anchor_y + off_y) * (int64_t)scale;
  const int64_t scaled_x = plane_x / old;
  const int64_t scaled_y = plane_y / old;
  v->anchor_x            = (int32_t)scaled_x - off_x;
  v->anchor_y            = (int32_t)scaled_y - off_y;
  v->scale               = scale;
  zoom_reclamp(v);
  zoom_touch(v, now_ms);
  return k_ra8_ok;
}

ra8_err_t ra8_zoom_view_pan(ra8_zoom_view_t* v, int32_t dx, int32_t dy, uint32_t now_ms)
{
  RA8_CHECK_NULL_PTR(v, s_tag, "view must not be nullptr");
  if (!v->active) {
    ra8_log_error(s_tag, "pan on a closed view");
    return k_ra8_err_invalid_state;
  }
  const int32_t was_x = v->anchor_x;
  const int32_t was_y = v->anchor_y;
  v->anchor_x         = zoom_sat_add(v->anchor_x, dx);
  v->anchor_y         = zoom_sat_add(v->anchor_y, dy);
  zoom_reclamp(v);
  /* Decision: a pan that clamped to no movement owes the panel nothing. */
  if ((v->anchor_x != was_x) || (v->anchor_y != was_y)) {
    zoom_touch(v, now_ms);
  }
  return k_ra8_ok;
}

ra8_err_t ra8_zoom_view_pan_dir(ra8_zoom_view_t* v, ra8_zoom_pan_t dir, uint32_t now_ms)
{
  RA8_CHECK_NULL_PTR(v, s_tag, "view must not be nullptr");
  if (!v->active) {
    ra8_log_error(s_tag, "pan_dir on a closed view");
    return k_ra8_err_invalid_state;
  }
  const int32_t step_x = v->dst.w - (v->dst.w / (int32_t)k_zoom_overlap_div);
  const int32_t step_y = v->dst.h - (v->dst.h / (int32_t)k_zoom_overlap_div);
  int32_t       dx     = 0;
  int32_t       dy     = 0;
  switch (dir) {
    case k_ra8_zoom_pan_left:
      dx = -step_x;
      break;
    case k_ra8_zoom_pan_right:
      dx = step_x;
      break;
    case k_ra8_zoom_pan_up:
      dy = -step_y;
      break;
    case k_ra8_zoom_pan_down:
      dy = step_y;
      break;
    case k_ra8_zoom_pan_none:
    default:
      break;
  }
  return ra8_zoom_view_pan(v, dx, dy, now_ms);
}

ra8_err_t ra8_zoom_view_window(const ra8_zoom_view_t* v, ra8_ui_rect_t* out)
{
  RA8_CHECK_NULL_PTR(v, s_tag, "view must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "out rect must not be nullptr");
  if (!v->active) {
    ra8_log_error(s_tag, "window query on a closed view");
    return k_ra8_err_invalid_state;
  }
  ra8_zoom_axis_t ax = {};
  ra8_zoom_axis_t ay = {};
  ra8_zoom_priv_axis(v->anchor_x, v->dst.w, (int32_t)v->scale, (int32_t)v->src.width, &ax);
  ra8_zoom_priv_axis(v->anchor_y, v->dst.h, (int32_t)v->scale, (int32_t)v->src.height, &ay);
  out->x = (int32_t)ax.s0;
  out->w = (int32_t)ax.count;
  out->y = (int32_t)ay.s0;
  out->h = (int32_t)ay.count;
  return k_ra8_ok;
}

bool ra8_zoom_view_tick(ra8_zoom_view_t* v, uint32_t now_ms)
{
  if (v == nullptr) {
    return false;
  }
  /* Decision: only an open view with an armed settle can promote to quality. */
  if (!v->active || !v->settle_armed) {
    return false;
  }
  /* Unsigned difference, so a wrapping millisecond counter settles early
   * rather than never. */
  if ((now_ms - v->last_input_ms) < (uint32_t)v->settle_ms) {
    return false;
  }
  v->settle_armed = false;
  v->pending      = true;
  v->pending_kind = k_ra8_zoom_refresh_quality;
  return true;
}

ra8_err_t ra8_zoom_view_present(ra8_zoom_view_t* v, ra8_zoom_present_t* out)
{
  RA8_CHECK_NULL_PTR(v, s_tag, "view must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "present out must not be nullptr");
  if (!v->active) {
    ra8_log_error(s_tag, "present on a closed view");
    return k_ra8_err_invalid_state;
  }
  out->rect    = v->dst;
  out->refresh = v->pending_kind;
  out->present = v->pending;
  v->pending   = false;
  return k_ra8_ok;
}
