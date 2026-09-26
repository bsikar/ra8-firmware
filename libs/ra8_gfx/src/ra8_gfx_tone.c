/**
 * @file ra8_gfx_tone.c
 * @brief Per-panel gray-level tone LUT: validate, prepare, quantise (#479).
 *
 * @details
 * Three entry points over one idea: a panel's 16 levels are not evenly spaced,
 * so the dither must know the real tone of each level rather than assume
 * `n * 17`. ::ra8_gfx_tone_lut_validate is the contract gate,
 * ::ra8_gfx_tone_prepare turns a validated 16-knot curve into a per-sample base
 * level plus round-up threshold, and ::ra8_gfx_tone_quantise is the two-lookup
 * hot-loop form ra8_gfx_dither calls. Everything here is pure integer
 * arithmetic over caller-owned memory -- no allocation, no file-scope mutable
 * state -- so host, ra8_emulator and silicon agree byte for byte.
 *
 * The committed ::k_ra8_gfx_tone_lut_nominal is the even palette the tree
 * renders against today; prepared, it reproduces ra8_gfx_dither.c's closed-form
 * rule exactly, which is asserted over all 256 x 256 (sample, threshold) pairs
 * in tests/graphics/src/test_ra8_gfx_tone.c. No measured curve is committed:
 * nothing in this tree has seen a panel.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_gfx_tone.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"

/* The prepared map is indexed by a uint8_t sample, so its domain must be the
 * whole byte range; and the knot count is the panel's level count. Both are
 * assumed by the loops below, so assert them rather than trust the header. */
static_assert((int)k_ra8_gfx_tone_domain == 256, "tone map must cover every gray8 sample");
static_assert((int)k_ra8_gfx_tone_last_knot == (int)k_ra8_gfx_tone_knots - 1,
              "white knot must be the last knot");

const ra8_gfx_tone_lut_t k_ra8_gfx_tone_lut_nominal = {
  /* Even palette: level n renders n * 17, i.e. the (n << 4) | n expansion the
   * rest of the reader uses. This is the uncalibrated default, not a
   * measurement. */
  .level_gray8 = {0,   17,  34,  51,  68,  85,  102, 119,
                  136, 153, 170, 187, 204, 221, 238, 255},
};

/**
 * @brief Highest knot index whose tone is at or below @p gray8.
 *
 * @details Linear walk from the white knot downwards; the curve is strictly
 *          increasing (validated), so the first knot at or below the sample is
 *          the bracketing one. Sixteen knots, so the walk is bounded and runs
 *          once per sample at prepare time, never per pixel.
 *
 * @param[in] lut   Validated curve.
 * @param[in] gray8 Source sample, 0 .. 255.
 * @return Knot index in [0, @ref k_ra8_gfx_tone_last_knot].
 * @retval 0 The sample sits below the second knot.
 * @pre  @p lut satisfies the curve contract (knot 0 is 0, strictly increasing).
 * @post The returned index n satisfies `lut->level_gray8[n] <= gray8`.
 * @post No memory is modified (pure function).
 * @note Thread-safe; reads only its arguments.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t internal_bracket(const ra8_gfx_tone_lut_t* lut, uint8_t gray8)
{
  uint8_t n = (uint8_t)k_ra8_gfx_tone_last_knot;
  while ((n > 0U) && (lut->level_gray8[n] > gray8)) {
    n = (uint8_t)(n - 1U);
  }
  return n;
}

ra8_err_t ra8_gfx_tone_lut_validate(const ra8_gfx_tone_lut_t* lut)
{
  static const char* const k_tag = "ra8_gfx_tone";
  RA8_CHECK_NULL_PTR(lut, k_tag, "lut");

  if (lut->level_gray8[0] != 0U) {
    ra8_log_error(k_tag, "black knot is not 0");
    return k_ra8_err_range_check_failed;
  }
  if (lut->level_gray8[k_ra8_gfx_tone_last_knot] != (uint8_t)k_ra8_gfx_tone_white) {
    ra8_log_error(k_tag, "white knot is not 255");
    return k_ra8_err_range_check_failed;
  }
  for (uint8_t n = 1U; n < (uint8_t)k_ra8_gfx_tone_knots; ++n) {
    if (lut->level_gray8[n] <= lut->level_gray8[n - 1U]) {
      ra8_log_error(k_tag, "curve is not strictly increasing");
      return k_ra8_err_range_check_failed;
    }
  }
  return k_ra8_ok;
}

ra8_err_t ra8_gfx_tone_prepare(const ra8_gfx_tone_lut_t* lut, ra8_gfx_tone_map_t* out)
{
  static const char* const k_tag = "ra8_gfx_tone";
  RA8_CHECK_NULL_PTR(out, k_tag, "out");
  RA8_RETURN_ON_ERROR(ra8_gfx_tone_lut_validate(lut), k_tag, "tone curve rejected");

  for (uint32_t v = 0U; v < (uint32_t)k_ra8_gfx_tone_domain; ++v) {
    const uint8_t n = internal_bracket(lut, (uint8_t)v);
    out->level[v]   = n;
    if (n == (uint8_t)k_ra8_gfx_tone_last_knot) {
      /* On the white knot: no interval above it, so the pixel never rounds up
       * and no 17th level can be produced. */
      out->up_threshold[v] = 0U;
    } else {
      const uint32_t rem  = v - (uint32_t)lut->level_gray8[n];
      const uint32_t span = (uint32_t)lut->level_gray8[n + 1U] - (uint32_t)lut->level_gray8[n];
      /* ceil(rem * 256 / span): the exact integer form of the unbiased test
       * `thr * span < rem * 256`, so `thr < up_threshold` is that comparison. */
      out->up_threshold[v] =
        (uint16_t)(((rem * (uint32_t)k_ra8_gfx_tone_scale) + span - 1U) / span);
    }
  }
  return k_ra8_ok;
}

uint8_t ra8_gfx_tone_quantise(const ra8_gfx_tone_map_t* map, uint8_t gray8, uint8_t thr)
{
  uint8_t level = map->level[gray8];
  if ((uint16_t)thr < map->up_threshold[gray8]) {
    level = (uint8_t)(level + 1U);
  }
  return level;
}
