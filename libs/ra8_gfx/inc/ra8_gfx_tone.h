/**
 * @file ra8_gfx_tone.h
 * @brief Per-panel gray-level tone LUT for the 16-level e-ink dither path (#479).
 * @ingroup grp_ereader
 *
 * @details
 * The panel's 16 gray levels are not perceptually evenly spaced, and the tone
 * each level actually renders is a property of the glass, not of the encoder.
 * ra8_gfx_dither.h quantises against an *assumed* even palette (level `n`
 * renders as `n * 17`); this module makes that assumption a replaceable curve,
 * so a measured panel response can be mapped without touching the dither rule.
 *
 * Two objects, deliberately split:
 *  - ::ra8_gfx_tone_lut_t is the CURVE -- 16 bytes, the gray8 tone each panel
 *    level renders, in level order. That is the shape a bench measurement
 *    produces and the shape a stored per-device record would carry.
 *  - ::ra8_gfx_tone_map_t is the curve PREPARED for rendering -- a per-source
 *    value base level plus its round-up threshold, so the hot loop does two
 *    table reads and one compare, no search and no divide.
 *
 * @par What is nominal and what is measured
 * ::k_ra8_gfx_tone_lut_nominal is the even palette the tree renders against
 * today (`n * 17`), committed so the renderer works with no calibration data at
 * all. Prepared from it, ::ra8_gfx_tone_quantise reproduces the closed-form rule
 * in ra8_gfx_dither.c exactly, for every one of the 256 x 256 (sample,
 * threshold) pairs -- that equality is asserted in
 * `tests/graphics/src/test_ra8_gfx_tone.c`, and it is what keeps the committed
 * dither goldens valid while the seam exists. A MEASURED curve is a bench
 * artefact: nothing in-tree has seen a panel, so no measured LUT is committed
 * and none is inferred.
 *
 * @par Curve contract
 * A LUT is strictly increasing with `level_gray8[0] == 0` and
 * `level_gray8[15] == 255`. Strict monotonicity is what makes every level
 * distinguishable and every interval width non-zero; pinning the endpoints is
 * what keeps the curve a map of the whole 0..255 source domain. A measured
 * response therefore has to be NORMALISED to the panel's own black and white
 * before it is stored -- see the open questions in the issue, that
 * normalisation is a bench decision, not one this module makes.
 *
 * @note Every entry point is a pure integer transform over caller-owned memory:
 *       no global state, no allocation, ISR-safe, and identical on host,
 *       ra8_emulator and silicon (the EIL==HIL rule).
 * @see ra8_gfx_dither.h        The blue-noise quantiser these curves feed.
 * @see ra8_gfx_dither_gray4_level_tone  Single-pixel dither through a curve.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_gfx_tone_const_t
 * @brief Curve knot count and source-domain size.
 *
 * @details
 * The knot count is the panel's level count (4 bpp, 16 levels) and the domain is
 * the gray8 source range, so a prepared map holds one entry per possible sample.
 *
 * @invariant k_ra8_gfx_tone_knots == k_ra8_gfx_dither_levels.
 * @invariant k_ra8_gfx_tone_last_knot == k_ra8_gfx_tone_knots - 1.
 * @see ra8_gfx_tone_lut_t
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_gfx_tone_knots     = 16,  /**< Curve knots (one per panel level).       */
  k_ra8_gfx_tone_last_knot = 15,  /**< Index of the white knot.                 */
  k_ra8_gfx_tone_domain    = 256, /**< gray8 source values a map covers.        */
  k_ra8_gfx_tone_white     = 255, /**< Required tone of the white knot.         */
  k_ra8_gfx_tone_scale     = 256, /**< Threshold-texture depth (mask range +1). */
} ra8_gfx_tone_const_t;

/**
 * @struct ra8_gfx_tone_lut_t
 * @brief A panel's tone response: the gray8 each of the 16 levels renders.
 *
 * @details
 * Index is the 4-bit panel level, value is the tone that level produces on the
 * glass. Strictly increasing, `[0] == 0`, `[15] == 255`
 * (::ra8_gfx_tone_lut_validate enforces exactly that). 16 bytes, so it copies
 * into a stored device record without packing.
 *
 * @see k_ra8_gfx_tone_lut_nominal  The committed uncalibrated curve.
 * @see ra8_gfx_tone_lut_validate   The contract, enforced.
 * @since 0.1.0
 */
typedef struct {
  uint8_t level_gray8[k_ra8_gfx_tone_knots]; /**< Rendered tone per panel level. */
} ra8_gfx_tone_lut_t;

/**
 * @struct ra8_gfx_tone_map_t
 * @brief A tone curve prepared for the dither hot loop (caller-owned, 768 B).
 *
 * @details
 * For each gray8 sample `v`: `level[v]` is the panel level immediately at or
 * below `v` on the curve, and `up_threshold[v]` is the exclusive blue-noise
 * threshold below which the pixel rounds up to the next level -- i.e. the pixel
 * takes `level[v] + 1` when `thr < up_threshold[v]`. Both come out of
 * ::ra8_gfx_tone_prepare; the struct is plain data with no back-pointer to the
 * curve, so it can live in a `static` at file scope or on a caller's stack.
 *
 * @invariant level[v] is in [0, 15] and non-decreasing in v.
 * @invariant up_threshold[v] is in [0, 256]; it is 0 at v == 255 (never rounds up).
 * @see ra8_gfx_tone_prepare
 * @see ra8_gfx_tone_quantise
 * @since 0.1.0
 */
typedef struct {
  uint8_t  level[k_ra8_gfx_tone_domain];        /**< Base panel level per sample.  */
  uint16_t up_threshold[k_ra8_gfx_tone_domain]; /**< Exclusive round-up threshold. */
} ra8_gfx_tone_map_t;

/**
 * @brief The committed nominal (uncalibrated) curve: level `n` renders `n * 17`.
 *
 * @details
 * The even palette the whole reader assumes today, so the renderer works out of
 * the box with no calibration record. Prepared and run through
 * ::ra8_gfx_tone_quantise it reproduces ra8_gfx_dither.c's closed-form rule
 * bit-for-bit over the entire (sample, threshold) product, which is what makes
 * the committed dither goldens unaffected by the existence of this seam.
 *
 * @see ra8_gfx_tone_prepare
 * @since 0.1.0
 */
extern const ra8_gfx_tone_lut_t k_ra8_gfx_tone_lut_nominal;

/**
 * @brief Range-check a tone curve against the curve contract.
 *
 * @details
 * Rejects anything a renderer cannot use: a null pointer, a black knot that is
 * not 0, a white knot that is not 255, or any knot not strictly greater than its
 * predecessor (which would make a level unreachable or an interval zero-width).
 * This is the single gate a stored or bench-supplied curve passes through, and
 * ::ra8_gfx_tone_prepare calls it, so no unvalidated curve can reach the hot
 * loop.
 *
 * @param[in] lut Curve to check.
 *
 * @return Error code.
 * @retval k_ra8_ok                    The curve satisfies the contract.
 * @retval k_ra8_err_null_ptr          @p lut is NULL.
 * @retval k_ra8_err_range_check_failed A knot is out of contract (endpoint or order).
 *
 * @pre  None.
 * @post No memory is modified (pure function).
 *
 * @note Thread-safe; reads only its argument.
 * @see ra8_gfx_tone_prepare
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gfx_tone_lut_validate(const ra8_gfx_tone_lut_t* lut);

/**
 * @brief Prepare a validated curve into the per-sample form the dither consumes.
 *
 * @details
 * Walks the 256 gray8 samples once. For sample `v` it takes the highest knot
 * `n` with `level_gray8[n] <= v`, and writes `level[v] = n` plus the round-up
 * threshold `ceil(rem * 256 / span)` where `rem = v - level_gray8[n]` and
 * `span = level_gray8[n + 1] - level_gray8[n]`. That ceiling is the exact
 * integer form of the unbiased comparison `thr * span < rem * 256`, so the
 * round-up probability over a uniform mask is exactly `rem / span` and a flat
 * field reconstructs its source tone -- the same unbiasedness the even-palette
 * rule has, now measured against the real interval widths. At `v == 255` the
 * sample sits on the white knot: level 15, threshold 0, no round-up, so no
 * 17th level can ever be produced.
 *
 * @param[in]  lut Curve to prepare; must satisfy ::ra8_gfx_tone_lut_validate.
 * @param[out] out Prepared map, fully overwritten on success.
 *
 * @return Error code.
 * @retval k_ra8_ok                    Map written.
 * @retval k_ra8_err_null_ptr          @p lut or @p out is NULL.
 * @retval k_ra8_err_range_check_failed @p lut violates the curve contract.
 *
 * @pre  @p out points to writable ::ra8_gfx_tone_map_t storage.
 * @post On k_ra8_ok every level is in [0, 15] and non-decreasing in the sample.
 * @post On any error return @p out is unmodified.
 *
 * @note Thread-safe in that it writes only @p out; holds no shared state.
 *
 * @par Example:
 * @code
 * static ra8_gfx_tone_map_t s_map;
 * RA8_RETURN_ON_ERROR(ra8_gfx_tone_prepare(&k_ra8_gfx_tone_lut_nominal, &s_map),
 *                     "app", "tone prepare");
 * @endcode
 *
 * @see ra8_gfx_tone_quantise
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_gfx_tone_prepare(const ra8_gfx_tone_lut_t* lut,
                                             ra8_gfx_tone_map_t*       out);

/**
 * @brief Quantise one gray8 sample to a panel level through a prepared curve.
 *
 * @details
 * Two table reads and one compare: `level[gray8]`, rounded up when the pixel's
 * blue-noise threshold @p thr falls below `up_threshold[gray8]`. The threshold
 * itself comes from the caller (ra8_gfx_dither owns the mask and its phase), so
 * this function stays pure and the mask stays in one place.
 *
 * @param[in] map   Prepared map from ::ra8_gfx_tone_prepare (non-NULL).
 * @param[in] gray8 Source luminance sample, 0 (black) .. 255 (white).
 * @param[in] thr   Blue-noise threshold for this pixel, 0 .. 255.
 *
 * @return The dithered 4-bit level.
 * @retval 0  The pixel quantised to black.
 * @retval 15 The pixel quantised to white (the maximum level).
 *
 * @pre  @p map was written by a successful ::ra8_gfx_tone_prepare.
 * @post The result is in [0, 15].
 * @post No memory is modified (pure function).
 *
 * @note Thread-safe; reads only its arguments. ISR-safe.
 * @see ra8_gfx_dither_gray4_level_tone  The same rule with the mask applied.
 * @since 0.1.0
 */
uint8_t ra8_gfx_tone_quantise(const ra8_gfx_tone_map_t* map, uint8_t gray8, uint8_t thr);
