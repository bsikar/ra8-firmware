/**
 * @file ra8_epaper_geom.c
 * @brief IT8951 geometry, pixel-format and waveform-map helpers
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The pure half of the IT8951 driver, split out of ``ra8_epaper.c`` so
 * neither translation unit exceeds the repository's file-size cap. These
 * functions touch no bus, no GPIO and no driver state -- they are total
 * functions over their arguments, which is why they can be called (and
 * tested) before ``ra8_epaper_init`` has ever run.
 *
 * Three IT8951 facts are encoded here, and each one is a defect the
 * driver previously had:
 *
 *  - **Pixel depth is not the wire code.** The controller numbers its
 *    LD_IMG_AREA formats 2 bpp = 0, 3 bpp = 1, 4 bpp = 2, 8 bpp = 3.
 *    ``ra8_epaper_bits_per_pixel`` and ``ra8_epaper_image_bytes`` deal in
 *    real bit depths; the wire code is applied inside ``ra8_epaper.c``.
 *  - **1 bpp updates must sit on a 32-pixel grid.** The bi-level LUT reads
 *    the update window in 4-byte units, so a misaligned A2 update renders
 *    nothing at all -- the controller accepts the command and the screen
 *    stays blank. ``ra8_epaper_area_is_aligned`` detects that and
 *    ``ra8_epaper_align_area`` repairs it by growing outward.
 *  - **Waveform mode numbers are per-panel.** They come from the
 *    controller's firmware LUT, not from the driver;
 *    ``ra8_epaper_waveform_cfg_for_lut`` derives them from the reported
 *    LUT version string.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_check.h"
#include "ra8_epaper.h"
#include "ra8_err.h"

/**
 * @var s_tag
 * @brief Logging tag used by the null-argument guards in this TU.
 */
static const char* const s_tag = "EPAPER";

/**
 * @enum ra8_epaper_geom_const_t
 * @brief Packing arithmetic constants (no magic numbers).
 */
typedef enum : uint8_t {
  k_ra8_epaper_geom_bits_per_byte = 8U, /**< Row-packing denominator. */
} ra8_epaper_geom_const_t;

/**
 * @enum ra8_epaper_bpp_t
 * @brief Bit depth of each ::ra8_epaper_pixel_format_t.
 */
typedef enum : uint8_t {
  k_ra8_epaper_bpp_1 = 1U, /**< ::k_ra8_epaper_pf_1bpp. */
  k_ra8_epaper_bpp_2 = 2U, /**< ::k_ra8_epaper_pf_2bpp. */
  k_ra8_epaper_bpp_4 = 4U, /**< ::k_ra8_epaper_pf_4bpp. */
  k_ra8_epaper_bpp_8 = 8U, /**< ::k_ra8_epaper_pf_8bpp. */
} ra8_epaper_bpp_t;

/**
 * @enum ra8_epaper_lut_mode_t
 * @brief Waveform LUT mode numbers the vendor documents.
 *
 * @details
 * INIT / DU / GC16 are stable across every Waveshare IT8951 panel. A2 is
 * the one that moves: mode 4 on the ``M641`` LUT (6 inch, 6 inch HD) and
 * mode 6 on the vendor's generic default for the larger panels.
 */
typedef enum : uint8_t {
  k_ra8_epaper_lut_init    = 0U, /**< INIT on every documented LUT. */
  k_ra8_epaper_lut_du      = 1U, /**< DU on every documented LUT.   */
  k_ra8_epaper_lut_gc16    = 2U, /**< GC16 on every documented LUT. */
  k_ra8_epaper_lut_a2_m641 = 4U, /**< A2 on the ``M641`` LUT.       */
  k_ra8_epaper_lut_a2_gen  = 6U, /**< A2 on the vendor generic LUT. */
} ra8_epaper_lut_mode_t;

/**
 * @var s_lut_m641
 * @brief LUT-version token identifying the 6 inch / 6 inch HD waveform set.
 *
 * @details
 * Matched as a prefix because the controller appends a build suffix that
 * varies between firmware revisions.
 *
 * @note Read-only; compared against the reported LUT version.
 * @warning Changing this silently repoints A2 at a different LUT mode.
 * @since 0.1.0
 */
static const char* const s_lut_m641 = "M641";

[[nodiscard]] uint8_t ra8_epaper_bits_per_pixel(ra8_epaper_pixel_format_t pf)
{
  switch (pf) {
    case k_ra8_epaper_pf_1bpp:
      return (uint8_t)k_ra8_epaper_bpp_1;
    case k_ra8_epaper_pf_2bpp:
      return (uint8_t)k_ra8_epaper_bpp_2;
    case k_ra8_epaper_pf_4bpp:
      return (uint8_t)k_ra8_epaper_bpp_4;
    case k_ra8_epaper_pf_8bpp:
    default:
      return (uint8_t)k_ra8_epaper_bpp_8;
  }
}

[[nodiscard]] ra8_err_t ra8_epaper_image_bytes(const ra8_epaper_area_t*  area,
                                               ra8_epaper_pixel_format_t pf,
                                               size_t*                   out_bytes)
{
  RA8_CHECK_NULL_PTR(area, s_tag, "image_bytes: area null");
  RA8_CHECK_NULL_PTR(out_bytes, s_tag, "image_bytes: out null");
  if ((area->width == 0U) || (area->height == 0U)) {
    return k_ra8_err_invalid_size;
  }
  const size_t bpp           = (size_t)ra8_epaper_bits_per_pixel(pf);
  const size_t bits_per_byte = (size_t)k_ra8_epaper_geom_bits_per_byte;
  /* Rows are packed independently and each starts on a byte boundary. */
  const size_t row_bytes = (((size_t)area->width * bpp) + (bits_per_byte - 1U)) / bits_per_byte;
  *out_bytes             = row_bytes * (size_t)area->height;
  return k_ra8_ok;
}

[[nodiscard]] bool ra8_epaper_area_is_aligned(const ra8_epaper_area_t*  area,
                                              ra8_epaper_pixel_format_t pf)
{
  if (area == nullptr) {
    return false;
  }
  if (pf != k_ra8_epaper_pf_1bpp) {
    return true;
  }
  const uint16_t grid = (uint16_t)k_ra8_epaper_align_1bpp_px;
  return ((area->x % grid) == 0U) && ((area->width % grid) == 0U);
}

[[nodiscard]] ra8_err_t
ra8_epaper_align_area(ra8_epaper_area_t* area, ra8_epaper_pixel_format_t pf, uint16_t panel_width)
{
  RA8_CHECK_NULL_PTR(area, s_tag, "align_area: area null");
  if (panel_width == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (((uint32_t)area->x + (uint32_t)area->width) > (uint32_t)panel_width) {
    return k_ra8_err_invalid_arg;
  }
  if (pf != k_ra8_epaper_pf_1bpp) {
    return k_ra8_ok;
  }
  const uint32_t grid  = (uint32_t)k_ra8_epaper_align_1bpp_px;
  const uint32_t right = (uint32_t)area->x + (uint32_t)area->width;
  const uint32_t x_lo  = ((uint32_t)area->x / grid) * grid;
  /* Grow outward so the caller's rectangle stays wholly covered. */
  uint32_t x_hi = ((right + grid - 1U) / grid) * grid;
  if (x_hi > (uint32_t)panel_width) {
    x_hi = (uint32_t)panel_width;
  }
  if ((x_hi <= x_lo) || (((x_hi - x_lo) % grid) != 0U)) {
    /* The clamp cut the window off the grid: the panel width is itself not
     * a multiple of the 1 bpp grid, so no aligned superset exists here. */
    return k_ra8_err_invalid_arg;
  }
  area->x     = (uint16_t)x_lo;
  area->width = (uint16_t)(x_hi - x_lo);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_epaper_waveform_cfg_for_lut(const char*                lut_version,
                                                        ra8_epaper_waveform_cfg_t* out_cfg)
{
  RA8_CHECK_NULL_PTR(lut_version, s_tag, "waveform_cfg: lut null");
  RA8_CHECK_NULL_PTR(out_cfg, s_tag, "waveform_cfg: out null");
  bool is_m641 = true;
  for (uint32_t i = 0U; s_lut_m641[i] != '\0'; i++) {
    if (lut_version[i] != s_lut_m641[i]) {
      is_m641 = false;
      break;
    }
  }
  out_cfg->init = (uint8_t)k_ra8_epaper_lut_init;
  out_cfg->du   = (uint8_t)k_ra8_epaper_lut_du;
  out_cfg->gc16 = (uint8_t)k_ra8_epaper_lut_gc16;
  out_cfg->a2   = is_m641 ? (uint8_t)k_ra8_epaper_lut_a2_m641 : (uint8_t)k_ra8_epaper_lut_a2_gen;
  return k_ra8_ok;
}
