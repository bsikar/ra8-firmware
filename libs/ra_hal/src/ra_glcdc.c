/**
 * @file ra_glcdc.c
 * @brief Graphics LCD Controller driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Driver for the RA8D2 GLCDC block. Programmes the background
 * stage, both graphics layers (UI overlay over background image),
 * the alpha blender, output-stage brightness/contrast/dithering,
 * and double-buffered CLUTs. Closely mirrors the init order in
 * FSP's ``r_glcdc_graphics_layer_set`` and the shadow-CLUT swap
 * pattern in FSP's ``R_GLCDC_ClutEdit`` /
 * ``R_GLCDC_ColorPaletteUpdate``. Every register access carries a
 * HUM Ch 53 citation.
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

/* =============================================================================
 * Local constants
 * =============================================================================
 */

typedef enum : uint8_t {
  k_ra_glcdc_layer_count   = 2U,  /**< Number of graphics layers.  */
  k_ra_glcdc_layer1        = 0U,  /**< Layer index for GR[0].      */
  k_ra_glcdc_dispsel_below = 1U,  /**< AB1.DISPSEL = lower layer.  */
  k_ra_glcdc_dispsel_above = 2U,  /**< AB1.DISPSEL = upper layer.  */
  k_ra_glcdc_arcdef_shift  = 16U, /**< AB7.ARCDEF bit shift.       */
  k_ra_glcdc_clutsel_shift = 16U, /**< CLUTINT.SEL bit position.   */
  k_ra_glcdc_pdtha_shift   = 8U,  /**< PDTHA.SEL bit shift.        */
  k_ra_glcdc_word_shift    = 16U, /**< 16-bit field shift.         */
  k_ra_glcdc_g_shift       = 16U, /**< Green-channel byte shift.   */
  k_ra_glcdc_b_shift       = 8U,  /**< Blue-channel byte shift.    */
} ra_glcdc_priv_const_t;

typedef enum : uint16_t {
  k_ra_glcdc_clut_entries = 256U, /**< Entries per CLUT plane.      */
} ra_glcdc_clut_const_t;

typedef enum : uint32_t {
  k_ra_glcdc_clutsel_mask = 0x10000UL, /**< CLUTINT.SEL bit mask.   */
  k_ra_glcdc_arcon_mask   = 0x1000UL,  /**< AB1.ARCON bit mask.     */
} ra_glcdc_priv_mask_t;

/* =============================================================================
 * Initialisation
 * =============================================================================
 */

/* HUM Ch 53 "Graphics LCD Controller (GLCDC)" p 3744 */
ra_err_t ra_glcdc_init(const ra_glcdc_config_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_glcdc);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "glcdc_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 53 "BG_HSIZE / BG_VSIZE / BG_BGC" p 3744 */
  /* Background stage: H/V size + colour. */
  *ra_glcdc_reg32(k_ra_glcdc_off_bg_hsize) = (uint32_t)cfg->width_px;
  *ra_glcdc_reg32(k_ra_glcdc_off_bg_vsize) = (uint32_t)cfg->height_px;
  *ra_glcdc_reg32(k_ra_glcdc_off_bg_bgc)   = 0U;

  /* HUM Ch 53 "GR1_FLM6 / GR1_FLM2 / GR1_FLM5 / GR1_AB3" p 3744 */
  /* Graphics layer 1: format + framebuffer base + line stride + size. */
  *ra_glcdc_reg32(k_ra_glcdc_off_gr1_fmt)   = (uint32_t)cfg->format;
  *ra_glcdc_reg32(k_ra_glcdc_off_gr1_saddr) = cfg->framebuffer_addr;
  *ra_glcdc_reg32(k_ra_glcdc_off_gr1_line)  = (uint32_t)cfg->width_px;
  *ra_glcdc_reg32(k_ra_glcdc_off_gr1_size) =
    ((uint32_t)cfg->height_px << 16U) | (uint32_t)cfg->width_px;

  /* HUM Ch 53 "PANEL_CLK" p 3744 */
  /* Panel clock from the EK-RA8D2 panel timing constant. */
  *ra_glcdc_reg32(k_ra_glcdc_off_panel_clk) = k_ra_glcdc_ek_pixel_clk_hz;

  ra_log_info_val(s_tag, "glcdc_init fb", cfg->framebuffer_addr);
  return k_ra_ok;
}

ra_err_t ra_glcdc_start(bool enable)
{
  uint32_t en = 0UL;
  if (enable) {
    en = 1UL;
  }
  /* HUM Ch 53 "SYSCNT.DTCTEN / BG.EN / GR1.VEN" p 3744 */
  /* Enable / disable the system + background + graphics-layer engines. */
  *ra_glcdc_reg32(k_ra_glcdc_off_sys_cfg) = en;
  *ra_glcdc_reg32(k_ra_glcdc_off_bg_en)   = en;
  *ra_glcdc_reg32(k_ra_glcdc_off_gr1_en)  = en;
  return k_ra_ok;
}

/* =============================================================================
 * Layer 2 (background-image) configuration
 *
 * Mirrors FSP `r_glcdc_graphics_layer_set` register order:
 *   FLM6 (format) -> FLM2 (base) -> FLM3 (line stride) ->
 *   FLM5 (datanum/lnnum) -> AB3 (H size+pos) -> AB2 (V size+pos) ->
 *   AB5 / AB4 (alpha-area H/V) -> AB7 (default alpha) -> AB1 (DISPSEL).
 * =============================================================================
 */

ra_err_t ra_glcdc_set_layer2(const ra_glcdc_layer2_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");

  /* HUM Ch 53 "GR2_FLM6.FORMAT" p 3744 */
  *ra_glcdc_reg32(k_ra_glcdc_off_gr2_fmt) = (uint32_t)cfg->format;

  /* HUM Ch 53 "GR2_FLM2.BASE" p 3744 */
  *ra_glcdc_reg32(k_ra_glcdc_off_gr2_saddr) = cfg->framebuffer_addr;

  /* HUM Ch 53 "GR2_FLM3.LNOFF" p 3744 -- bytes between successive
   * lines, sign-extended into bits [31:16]. */
  *ra_glcdc_reg32(k_ra_glcdc_off_gr2_flm3) = (cfg->line_stride_bytes << k_ra_glcdc_word_shift);

  /* HUM Ch 53 "GR2_FLM5.LNNUM/DATANUM" p 3744 */
  *ra_glcdc_reg32(k_ra_glcdc_off_gr2_line) =
    ((uint32_t)cfg->height_px << k_ra_glcdc_word_shift) | (uint32_t)cfg->width_px;

  /* HUM Ch 53 "GR2_AB3.GRCHS/GRCHW" p 3744 -- horizontal pos + width. */
  *ra_glcdc_reg32(k_ra_glcdc_off_gr2_size) =
    ((uint32_t)cfg->pos_x << k_ra_glcdc_word_shift) | (uint32_t)cfg->width_px;

  /* HUM Ch 53 "GR2_AB2.GRCVS/GRCVW" p 3744 -- vertical pos + height. */
  *ra_glcdc_reg32(k_ra_glcdc_off_gr2_ab2) =
    ((uint32_t)cfg->pos_y << k_ra_glcdc_word_shift) | (uint32_t)cfg->height_px;

  /* HUM Ch 53 "GR2_AB5.ARCHS/ARCHW" p 3744 -- alpha-rect H. */
  *ra_glcdc_reg32(k_ra_glcdc_off_gr2_ab5) =
    ((uint32_t)cfg->pos_x << k_ra_glcdc_word_shift) | (uint32_t)cfg->width_px;

  /* HUM Ch 53 "GR2_AB4.ARCVS/ARCVW" p 3744 -- alpha-rect V. */
  *ra_glcdc_reg32(k_ra_glcdc_off_gr2_ab4) =
    ((uint32_t)cfg->pos_y << k_ra_glcdc_word_shift) | (uint32_t)cfg->height_px;

  /* HUM Ch 53 "GR2_AB7.ARCDEF" p 3744 -- constant alpha. */
  *ra_glcdc_reg32(k_ra_glcdc_off_gr2_ab7) = ((uint32_t)cfg->alpha << k_ra_glcdc_arcdef_shift);

  /* HUM Ch 53 "GR2_AB1.DISPSEL" p 3744 -- default to "blend below". */
  *ra_glcdc_reg32(k_ra_glcdc_off_gr2_ab1) = (uint32_t)k_ra_glcdc_dispsel_below;

  /* HUM Ch 53 "GR2_FLMRD.RENB" p 3744 -- enable framebuffer read. */
  *ra_glcdc_reg32(k_ra_glcdc_off_gr2_flmrd) = 1U;

  /* HUM Ch 53 "GR2.VEN" p 3744 -- request register update on VS. */
  *ra_glcdc_reg32(k_ra_glcdc_off_gr2_en) = 1U;

  ra_log_info_val(s_tag, "set_layer2 fb", cfg->framebuffer_addr);
  return k_ra_ok;
}

/* =============================================================================
 * Layer 1 / Layer 2 blending
 * =============================================================================
 */

ra_err_t ra_glcdc_set_blend(ra_glcdc_blend_mode_t mode, uint8_t global_alpha)
{
  uint32_t dispsel = 0U;
  uint32_t arcon   = 0U;

  switch (mode) {
    case k_ra_blend_overwrite: {
      dispsel = (uint32_t)k_ra_glcdc_dispsel_below;
      arcon   = 0U;
      break;
    }
    case k_ra_blend_normal: {
      dispsel = (uint32_t)k_ra_glcdc_dispsel_above;
      arcon   = 0U;
      break;
    }
    case k_ra_blend_alpha: {
      dispsel = (uint32_t)k_ra_glcdc_dispsel_above;
      arcon   = (uint32_t)k_ra_glcdc_arcon_mask;
      break;
    }
    default: {
      return k_ra_err_invalid_arg;
    }
  }

  /* HUM Ch 53 "GR1_AB1.DISPSEL/ARCON" p 3744 -- top-layer blend mode. */
  *ra_glcdc_reg32(k_ra_glcdc_off_gr1_ab1) = dispsel | arcon;

  /* HUM Ch 53 "GR1_AB7.ARCDEF" p 3744 -- global alpha applied in
   * k_ra_blend_alpha mode. */
  *ra_glcdc_reg32(k_ra_glcdc_off_gr1_ab7) = ((uint32_t)global_alpha << k_ra_glcdc_arcdef_shift);
  return k_ra_ok;
}

/* =============================================================================
 * Background colour
 * =============================================================================
 */

ra_err_t ra_glcdc_set_background_color(uint32_t argb)
{
  /* HUM Ch 53 "BG_BGC" p 3744 -- panel-wide fallback colour. */
  *ra_glcdc_reg32(k_ra_glcdc_off_bg_bgc) = argb;
  return k_ra_ok;
}

/* =============================================================================
 * Double-buffered CLUT update
 *
 * GR[layer].CLUTINT.SEL selects the active plane (0 or 1). A new
 * table is written into the *inactive* plane, then SEL is flipped
 * so that hardware switches at the next vertical sync. Mirrors
 * FSP `R_GLCDC_ClutEdit` / `R_GLCDC_ColorPaletteUpdate`.
 * =============================================================================
 */

static volatile uint32_t* internal_clut_plane(uint8_t layer, uint8_t plane)
{
  uint16_t off;
  if (layer == k_ra_glcdc_layer1) {
    off = (plane == 0U) ? (uint16_t)k_ra_glcdc_off_gr1_clut0 : (uint16_t)k_ra_glcdc_off_gr1_clut1;
  } else {
    off = (plane == 0U) ? (uint16_t)k_ra_glcdc_off_gr2_clut0 : (uint16_t)k_ra_glcdc_off_gr2_clut1;
  }
  return (volatile uint32_t*)(k_ra_glcdc_base_addr + off);
}

static volatile uint32_t* internal_clutint_reg(uint8_t layer)
{
  if (layer == k_ra_glcdc_layer1) {
    return ra_glcdc_reg32(k_ra_glcdc_off_gr1_clutint);
  }
  return ra_glcdc_reg32(k_ra_glcdc_off_gr2_clutint);
}

ra_err_t ra_glcdc_set_clut_double_buffered(uint8_t         layer,
                                           const uint32_t* clut,
                                           uint32_t        entries,
                                           bool            swap_now)
{
  RA_CHECK_NULL_PTR(clut, s_tag, "clut must not be nullptr");
  if (layer >= (uint8_t)k_ra_glcdc_layer_count) {
    return k_ra_err_invalid_arg;
  }
  if ((entries == 0U) || (entries > (uint32_t)k_ra_glcdc_clut_entries)) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 53 "GR[n]_CLUTINT.SEL" p 3744 -- pick the *inactive*
   * plane (FSP shadow-buffer pattern). */
  volatile uint32_t* clutint = internal_clutint_reg(layer);
  const uint32_t     active =
    ((*clutint) & (uint32_t)k_ra_glcdc_clutsel_mask) >> k_ra_glcdc_clutsel_shift;
  const uint8_t target_plane = (active != 0U) ? (uint8_t)0U : (uint8_t)1U;

  /* HUM Ch 53 "GR[n]_CLUT[plane]" p 3744 -- write inactive plane. */
  volatile uint32_t* dst = internal_clut_plane(layer, target_plane);
  for (uint32_t i = 0U; i < entries; ++i) {
    dst[i] = clut[i];
  }

  if (swap_now) {
    /* HUM Ch 53 "GR[n]_CLUTINT.SEL" p 3744 -- flip on next VS. */
    const uint32_t cleared = (*clutint) & ~(uint32_t)k_ra_glcdc_clutsel_mask;
    *clutint               = cleared | ((uint32_t)target_plane << k_ra_glcdc_clutsel_shift);
  }
  return k_ra_ok;
}

/* =============================================================================
 * Output stage: dithering / brightness / contrast
 * =============================================================================
 */

ra_err_t ra_glcdc_set_dithering(ra_glcdc_dither_mode_t mode)
{
  uint32_t sel;
  switch (mode) {
    case k_ra_dither_off: {
      sel = 0U;
      break;
    }
    case k_ra_dither_truncate: {
      sel = 2U;
      break;
    }
    case k_ra_dither_2x2: {
      sel = 3U;
      break;
    }
    default: {
      return k_ra_err_invalid_arg;
    }
  }
  /* HUM Ch 53 "OUT_PDTHA.SEL" p 3744 -- panel dither selector. */
  *ra_glcdc_reg32(k_ra_glcdc_off_panel_dtha) = (sel << k_ra_glcdc_pdtha_shift);
  return k_ra_ok;
}

ra_err_t ra_glcdc_set_brightness(uint8_t r, uint8_t g, uint8_t b)
{
  /* HUM Ch 53 "OUT_BRIGHT1.BRTG" p 3744 -- green offset. */
  *ra_glcdc_reg32(k_ra_glcdc_off_out_bright1) = (uint32_t)g;

  /* HUM Ch 53 "OUT_BRIGHT2.BRTB/BRTR" p 3744 -- blue + red offsets. */
  *ra_glcdc_reg32(k_ra_glcdc_off_out_bright2) =
    ((uint32_t)b << k_ra_glcdc_word_shift) | (uint32_t)r;
  return k_ra_ok;
}

ra_err_t ra_glcdc_set_contrast(uint8_t r, uint8_t g, uint8_t b)
{
  /* HUM Ch 53 "OUT_CONTRAST.CONTG/CONTB/CONTR" p 3744 */
  *ra_glcdc_reg32(k_ra_glcdc_off_out_contrast) =
    ((uint32_t)g << k_ra_glcdc_g_shift) | ((uint32_t)b << k_ra_glcdc_b_shift) | (uint32_t)r;
  return k_ra_ok;
}

/* =============================================================================
 * lifecycle + IRQ + power transition
 * =============================================================================
 */

static ra_glcdc_event_fn_t s_glcdc_fn;
static void*               s_glcdc_ctx;

ra_err_t ra_glcdc_deinit(void)
{
  *ra_glcdc_reg32(k_ra_glcdc_off_sys_cfg) = 0U;
  *ra_glcdc_reg32(k_ra_glcdc_off_bg_en)   = 0U;
  *ra_glcdc_reg32(k_ra_glcdc_off_gr1_en)  = 0U;
  s_glcdc_fn                              = nullptr;
  s_glcdc_ctx                             = nullptr;
  return ra_mstp_disable(k_ra_mstp_glcdc);
}

ra_err_t ra_glcdc_get_status(uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  *out_mask = *ra_glcdc_reg32(k_ra_glcdc_off_sys_stat);
  return k_ra_ok;
}

ra_err_t ra_glcdc_clear_status(uint32_t mask)
{
  volatile uint32_t* reg = ra_glcdc_reg32(k_ra_glcdc_off_sys_stat);
  *reg                   = *reg & ~mask;
  return k_ra_ok;
}

ra_err_t ra_glcdc_attach_handler(ra_glcdc_event_fn_t fn, void* ctx)
{
  s_glcdc_fn  = fn;
  s_glcdc_ctx = ctx;
  return k_ra_ok;
}

void ra_glcdc_dispatch(void)
{
  volatile uint32_t*        reg  = ra_glcdc_reg32(k_ra_glcdc_off_sys_stat);
  const uint32_t            mask = *reg;
  const ra_glcdc_event_fn_t fn   = s_glcdc_fn;
  void* const               ctx  = s_glcdc_ctx;
  *reg                           = 0U;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

ra_err_t ra_glcdc_enter_stop(void)
{
  *ra_glcdc_reg32(k_ra_glcdc_off_sys_cfg) = 0U;
  return ra_mstp_disable(k_ra_mstp_glcdc);
}

ra_err_t ra_glcdc_exit_stop(void)
{
  return ra_mstp_enable(k_ra_mstp_glcdc);
}
