/**
 * @file ra_glcdc.c
 * @brief GLCDC driver framework
 *
 * @details
 * Writes the minimum subset of GLCDC registers documented in
 * `ra8d2_glcdc_regs.h` to light up graphics layer 1 against the
 * EK-RA8D2 1024x600 panel timings. Blending, chroma-key, dither,
 * and dual-layer support will be added incrementally.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_glcdc.h"

#include <stdint.h>

#include "ra8d2_glcdc_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "GLCDC";

ra_err_t ra_glcdc_init(const ra_glcdc_config_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_glcdc);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "glcdc_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  /* Background stage: size, sync, and colour. */
  *ra_glcdc_reg32(k_ra_glcdc_off_bg_hsize) = (uint32_t)cfg->width_px;
  *ra_glcdc_reg32(k_ra_glcdc_off_bg_vsize) = (uint32_t)cfg->height_px;
  *ra_glcdc_reg32(k_ra_glcdc_off_bg_bgc)   = 0U;

  /* Graphics layer 1. */
  *ra_glcdc_reg32(k_ra_glcdc_off_gr1_fmt)   = (uint32_t)cfg->format;
  *ra_glcdc_reg32(k_ra_glcdc_off_gr1_saddr) = cfg->framebuffer_addr;
  *ra_glcdc_reg32(k_ra_glcdc_off_gr1_line)  = (uint32_t)cfg->width_px;
  *ra_glcdc_reg32(k_ra_glcdc_off_gr1_size) =
    ((uint32_t)cfg->height_px << 16U) | (uint32_t)cfg->width_px;

  /* Panel clock + output control programmed from the EK-RA8D2 panel
   * timings in the register header. */
  *ra_glcdc_reg32(k_ra_glcdc_off_panel_clk) = (uint32_t)k_ra_glcdc_ek_pixel_clk_hz;

  ra_log_info_val(s_tag, "glcdc_init fb", cfg->framebuffer_addr);
  return k_ra_ok;
}

ra_err_t ra_glcdc_start(bool enable)
{
  uint32_t en = 0UL;
  if (enable) {
    en = 1UL;
  }
  *ra_glcdc_reg32(k_ra_glcdc_off_sys_cfg) = en;
  *ra_glcdc_reg32(k_ra_glcdc_off_bg_en)   = en;
  *ra_glcdc_reg32(k_ra_glcdc_off_gr1_en)  = en;
  return k_ra_ok;
}
