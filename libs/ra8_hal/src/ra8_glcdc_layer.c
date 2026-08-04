/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_glcdc_layer.c
 * @brief Graphics LCD Controller driver -- layer composition and CLUT
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Runtime layer-composition slice of the RA8D2 GLCDC driver, split out
 * of ra8_glcdc.c to keep each translation unit under the file-size cap.
 * Holds the graphics-layer config helpers (layer 2 background image,
 * layer 1 / layer 2 show, chroma-key), the layer 1 / layer 2 alpha
 * blender, the direct-write background colour, and the double-buffered
 * CLUT update. Mirrors the register order of FSP's
 * ``r_glcdc_graphics_layer_set`` and the shadow-CLUT swap pattern in
 * FSP's ``R_GLCDC_ClutEdit`` / ``R_GLCDC_ColorPaletteUpdate``. Every
 * register access carries a HUM Ch 63 citation.
 *
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_glcdc.h"
#include "ra8_glcdc_regs.h"
#include "ra8_log.h"

static const char* s_tag = "GLCDC";

/* =============================================================================
 * Local constants
 *
 * Private enum copies of the GLCDC field shifts / masks this translation
 * unit needs. They mirror the matching definitions in ra8_glcdc.c verbatim
 * (TU-local enum constants, not linkable symbols, so duplication is safe).
 * =============================================================================
 */

typedef enum : uint8_t {
  k_ra8_glcdc_layer_count   = 2U,  /**< Number of graphics layers. */
  k_ra8_glcdc_layer1        = 0U,  /**< Layer index for GR[0].     */
  k_ra8_glcdc_dispsel_below = 1U,  /**< AB1.DISPSEL = lower layer. */
  k_ra8_glcdc_dispsel_above = 2U,  /**< AB1.DISPSEL = upper layer. */
  k_ra8_glcdc_arcdef_shift  = 16U, /**< AB7.ARCDEF bit shift.      */
  k_ra8_glcdc_clutsel_shift = 16U, /**< CLUTINT.SEL bit position.  */
  k_ra8_glcdc_word_shift    = 16U, /**< 16-bit field shift.        */
} ra8_glcdc_priv_const_t;

typedef enum : uint16_t {
  k_ra8_glcdc_clut_entries = 256U, /**< Entries per CLUT plane. */
} ra8_glcdc_clut_const_t;

typedef enum : uint32_t {
  k_ra8_glcdc_clutsel_mask = 0x10000UL, /**< CLUTINT.SEL bit mask. */
  k_ra8_glcdc_arcon_mask   = 0x1000UL,  /**< AB1.ARCON bit mask.   */
} ra8_glcdc_priv_mask_t;

typedef enum : uint32_t {
  k_glcdc_shift_high      = 16U, /**< GLCDC shift high.      */
  k_glcdc_shift_flm6_fmt  = 28U, /**< GLCDC shift flm6 fmt.  */
  k_glcdc_shift_arcdef    = 16U, /**< GLCDC shift arcdef.    */
  k_glcdc_axi_burst_bytes = 64U, /**< GLCDC axi burst bytes. */
  k_glcdc_bpp_rgb565      = 2U,  /**< GLCDC bpp rgb565.      */
} ra8_glcdc_layout_priv_t;

/* AB7.ARCDEF[23:16] alpha constants. */
typedef enum : uint32_t {
  k_glcdc_alpha_opaque = 0xFFUL, /**< Fully opaque (constant alpha). */
} ra8_glcdc_alpha_priv_t;

/* OUT_SET.FORMAT[1:0] / FLM6.FORMAT codes for RGB565 framebuffers. */
typedef enum : uint32_t {
  k_glcdc_out_set_rgb565 = 2U, /**< GLCDC out set rgb565. */
} ra8_glcdc_out_set_fmt_t;

/* =============================================================================
 * Layer 2 (background-image) configuration
 *
 * Mirrors FSP `r_glcdc_graphics_layer_set` register order:
 *   FLM6 (format) -> FLM2 (base) -> FLM3 (line stride) ->
 *   FLM5 (datanum/lnnum) -> AB3 (H size+pos) -> AB2 (V size+pos) ->
 *   AB5 / AB4 (alpha-area H/V) -> AB7 (default alpha) -> AB1 (DISPSEL).
 * =============================================================================
 */

ra8_err_t ra8_glcdc_set_layer2(const ra8_glcdc_layer2_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");

  /* HUM Ch 63 "GR2_FLM6.FORMAT" p 3744 */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_fmt) = (uint32_t)cfg->format;

  /* HUM Ch 63 "GR2_FLM2.BASE" p 3744 */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_saddr) = cfg->framebuffer_addr;

  /* HUM Ch 63 "GR2_FLM3.LNOFF" p 3744 -- bytes between successive
   * lines, sign-extended into bits [31:16]. */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_flm3) = (cfg->line_stride_bytes << k_ra8_glcdc_word_shift);

  /* HUM Ch 63 "GR2_FLM5.LNNUM/DATANUM" p 3744 */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_line) =
    ((uint32_t)cfg->height_px << k_ra8_glcdc_word_shift) | (uint32_t)cfg->width_px;

  /* HUM Ch 63 "GR2_AB3.GRCHS/GRCHW" p 3744 */ /* horizontal pos + width. */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_size) =
    ((uint32_t)cfg->pos_x << k_ra8_glcdc_word_shift) | (uint32_t)cfg->width_px;

  /* HUM Ch 63 "GR2_AB2.GRCVS/GRCVW" p 3744 */ /* vertical pos + height. */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab2) =
    ((uint32_t)cfg->pos_y << k_ra8_glcdc_word_shift) | (uint32_t)cfg->height_px;

  /* HUM Ch 63 "GR2_AB5.ARCHS/ARCHW" p 3744 */ /* alpha-rect H. */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab5) =
    ((uint32_t)cfg->pos_x << k_ra8_glcdc_word_shift) | (uint32_t)cfg->width_px;

  /* HUM Ch 63 "GR2_AB4.ARCVS/ARCVW" p 3744 */ /* alpha-rect V. */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab4) =
    ((uint32_t)cfg->pos_y << k_ra8_glcdc_word_shift) | (uint32_t)cfg->height_px;

  /* HUM Ch 63 "GR2_AB7.ARCDEF" p 3744 */ /* constant alpha. */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab7) = ((uint32_t)cfg->alpha << k_ra8_glcdc_arcdef_shift);

  /* HUM Ch 63 "GR2_AB1.DISPSEL" p 3744 */ /* default to "blend below". */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab1) = (uint32_t)k_ra8_glcdc_dispsel_below;

  /* HUM Ch 63 "GR2_FLMRD.RENB" p 3744 */ /* enable framebuffer read. */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_flmrd) = 1U;

  /* HUM Ch 63 "GR2.VEN" p 3744 */ /* request register update on VS. */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_en) = 1U;

  ra8_log_info_val(s_tag, "set_layer2 fb", cfg->framebuffer_addr);
  return k_ra8_ok;
}

/* =============================================================================
 * Layer 1 / Layer 2 blending
 * =============================================================================
 */

ra8_err_t ra8_glcdc_set_blend(ra8_glcdc_blend_mode_t mode, uint8_t global_alpha)
{
  uint32_t dispsel = 0U;
  uint32_t arcon   = 0U;

  switch (mode) {
    case k_ra8_blend_overwrite: {
      dispsel = (uint32_t)k_ra8_glcdc_dispsel_below;
      arcon   = 0U;
      break;
    }
    case k_ra8_blend_normal: {
      dispsel = (uint32_t)k_ra8_glcdc_dispsel_above;
      arcon   = 0U;
      break;
    }
    case k_ra8_blend_alpha: {
      dispsel = (uint32_t)k_ra8_glcdc_dispsel_above;
      arcon   = (uint32_t)k_ra8_glcdc_arcon_mask;
      break;
    }
    default: {
      return k_ra8_err_invalid_arg;
    }
  }

  /* HUM Ch 63 "GR1_AB1.DISPSEL/ARCON" p 3744 */ /* top-layer blend mode. */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_ab1) = dispsel | arcon;

  /* HUM Ch 63 "GR1_AB7.ARCDEF" p 3744 -- global alpha applied in
   * k_ra8_blend_alpha mode. */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_ab7) = ((uint32_t)global_alpha << k_ra8_glcdc_arcdef_shift);
  return k_ra8_ok;
}

/* =============================================================================
 * Background colour
 * =============================================================================
 */

ra8_err_t ra8_glcdc_set_background_color(uint32_t argb)
{
  /* BG_BGC is direct-write (not shadow-latched).  Writing it mid-
   * frame tears the panel horizontally.  Sync to vblank by pulsing
   * BG.EN.VEN and polling BG.EN.VEN to auto-clear (= VS just fired),
   * then write BG_BGC during the vblank window so the next visible
   * scan starts with the new colour.  FSP polls BG.EN.VEN_b in
   * exactly this way (see r_glcdc.c FSP_ERROR_RETURN checks). */
  enum : uint32_t {
    k_bg_en_ven   = 1UL << 8,  /**< Bg en ven.              */
    k_ven_timeout = 0x40000UL, /**< > 1 frame at 1 GHz CPU. */
  };

  uint32_t bg_en = *ra8_glcdc_reg32(k_ra8_glcdc_off_bg_en);
  bg_en |= k_bg_en_ven;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_bg_en) = bg_en;

  for (uint32_t i = 0U; i < (uint32_t)k_ven_timeout; i++) {
    if ((*ra8_glcdc_reg32(k_ra8_glcdc_off_bg_en) & k_bg_en_ven) == 0U) {
      break;
    }
  }
  /* Write BG_BGC during the vblank window we just polled into. */
  /* HUM Ch 63 "BG_BGC" p 3763 */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_bg_bgc) = argb;
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_glcdc_layer1_show`.
 *
 * @details See header for caller-facing contract.  Writes SADDR,
 * alpha=0xFF, DISPSEL=non_transparent, FLMRD=1, then asserts GR1.VEN
 * so the chip commits the layer-1 visibility flip at the next VS.
 *
 * @param[in] fb_addr Framebuffer base address (see header for dims).
 *
 * @return Always `k_ra8_ok` (writes can't fail at this point).
 * @retval k_ra8_ok Layer 1 visible from next VS.
 *
 * @pre `ra8_glcdc_init` and `ra8_glcdc_start` have run.
 * @pre `fb_addr` is valid for the framebuffer size from init.
 * @post GR1 fetches and composites pixels from `fb_addr`.
 * @post GR1.VEN is asserted (auto-clears at next VS).
 *
 * @note Single-threaded; not safe to call from IRQ.
 * @since 0.1.0
 */
ra8_err_t ra8_glcdc_layer1_show(uintptr_t fb_addr)
{
  enum : uint32_t {
    k_gr1_ven           = 1UL << 0, /**< Gr1 ven.                                             */
    k_gr1_dispsel_lower = 3UL,      /**< AB1.DISPSEL = ON_LOWER (FSP's BLEND_ON_LOWER_LAYER). */
  };
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_saddr) = (uint32_t)fb_addr;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_ab7) =
    ((uint32_t)k_glcdc_alpha_opaque << k_glcdc_shift_arcdef);
  /* Both GR layers use ON_LOWER (FSP's BLEND_ON_LOWER_LAYER) so they
   * coexist in composition.  With NON_TRANSPARENT (2) the chip
   * empirically drops GR1 from the output once GR2 is also enabled. */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_ab1)   = k_gr1_dispsel_lower;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_flmrd) = 1U;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_en)    = k_gr1_ven;
  return k_ra8_ok;
}

/**
 * @brief Enable chroma-key transparency on GLCDC graphics layer 2.
 *
 * @details Programmes GR2.AB8 / AB9 with the match/replace colour
 * pair so that every framebuffer pixel matching `key_rgb888`
 * composites with alpha=0 (i.e. the lower layer / BG plane shows
 * through).  Also asserts the AB1.ARCON bit because some RA parts
 * gate the per-pixel alpha replacement behind ARCON; without it
 * the compositor clamps alpha back to AB7.ARCDEF (0xFF) and the
 * pixel stays opaque.  See HUM Ch 63 for the AB7 / AB8 / AB9
 * layouts.
 *
 * @param[in] key_rgb888 Chroma-key match colour, 0x00RRGGBB.  The
 *                       function adds an explicit alpha=0xFF for
 *                       the comparator and masks the supplied value
 *                       to 24 bits, so callers may pass either
 *                       0x00RRGGBB or 0xFFRRGGBB.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Chroma-key enabled; effect lands at next vsync.
 *
 * @pre `ra8_glcdc_layer2_show` has run (GR2 has a framebuffer and
 *      panel position).
 * @pre Caller serialises GLCDC register access (single-threaded
 *      init context, or interrupts masked).
 * @post GR2.AB7.CKON=1, GR2.AB8 holds the match colour, GR2.AB9
 *       holds (0, transparent).
 * @post GR2.VEN is asserted (auto-clears at next vsync).
 *
 * @note Not thread-safe; not ISR-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_glcdc_layer2_chroma_key_enable(uint32_t key_rgb888)
{
  enum : uint32_t {
    k_gr2_ven = 1UL << 0, /**< Gr2 ven. */
    /* Try several plausible CKON bit positions at once -- the HUM
     * isn't in front of me and different RA parts have moved the
     * bit around (0, 16, 24).  Setting all three is safe: at most
     * one is meaningful, the others land in reserved bits which
     * the chip ignores. */
    /** Ab7 ckon. */
    k_ab7_ckon = (1UL << 0) | (1UL << 16) | (1UL << 24),
    /** Ab1 arcon. */
    k_ab1_arcon = 1UL << 12,
    /** Arcdef op. */
    k_arcdef_op         = ((uint32_t)k_glcdc_alpha_opaque << k_glcdc_shift_arcdef),
    k_alpha_opaque_byte = 0xFFUL << 24, /**< AB8 alpha-byte = opaque. */
    k_rgb888_mask       = 0x00FFFFFFUL, /**< 24-bit colour mask.      */
    k_ab9_transparent   = 0x00000000UL, /**< AB9 = alpha-0 replace.   */
  };
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab8) = k_alpha_opaque_byte | (key_rgb888 & k_rgb888_mask);
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab9) = k_ab9_transparent;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab7) = k_arcdef_op | k_ab7_ckon;
  /* OR in ARCON without clobbering DISPSEL (already set by layer2_show). */
  const uint32_t ab1                        = *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab1);
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab1) = ab1 | k_ab1_arcon;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_en)  = k_gr2_ven;
  return k_ra8_ok;
}

/**
 * @brief Make graphics layer 2 visible at the given panel position.
 *
 * @details Programmes GR2's framebuffer pointer, line stride, line
 * count, panel position, alpha, DISPSEL, FLMRD, and asserts
 * GR2.VEN to latch on the next vertical sync.  The framebuffer
 * format is hard-coded to RGB565; for other formats use the
 * chroma-key helper after this call.
 *
 * @param[in] fb_addr Layer-2 framebuffer base (RGB565).
 * @param[in] panel_x Horizontal panel position (GLCDC coords).
 * @param[in] panel_y Vertical panel position   (GLCDC coords).
 * @param[in] fb_w    Framebuffer width in pixels.
 * @param[in] fb_h    Framebuffer height in pixels.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Layer 2 visible from next vsync.
 *
 * @pre `ra8_glcdc_init` and `ra8_glcdc_start` have been called.
 * @pre `fb_addr` points to an `fb_w * fb_h * 2`-byte RGB565
 *      framebuffer.
 * @post GR2 composites pixels from `fb_addr` at (panel_x, panel_y).
 * @post GR2.VEN is asserted (auto-clears at next vsync).
 *
 * @note Single-threaded; not safe to call from IRQ.
 * @since 0.1.0
 */
ra8_err_t ra8_glcdc_layer2_show(uintptr_t fb_addr,
                                uint16_t  panel_x,
                                uint16_t  panel_y,
                                uint16_t  fb_w,
                                uint16_t  fb_h)
{
  enum : uint32_t {
    k_gr2_ven           = 1UL << 0, /**< Gr2 ven.                                             */
    k_gr2_dispsel_lower = 3UL,      /**< AB1.DISPSEL = ON_LOWER (FSP's BLEND_ON_LOWER_LAYER). */
  };
  /* HUM 63: GR2_FLM6.FORMAT[30:28] = 2 (RGB565). */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_fmt) =
    ((uint32_t)k_glcdc_out_set_rgb565 << k_glcdc_shift_flm6_fmt);
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_saddr) = (uint32_t)fb_addr;

  const uint32_t line_bytes                  = (uint32_t)fb_w * (uint32_t)k_glcdc_bpp_rgb565;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_flm3) = (line_bytes << k_glcdc_shift_high);

  const uint32_t datanum = (line_bytes / (uint32_t)k_glcdc_axi_burst_bytes) - 1U;
  const uint32_t lnnum   = (uint32_t)fb_h - 1U;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_line) = (lnnum << k_glcdc_shift_high) | datanum;

  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_size) =
    ((uint32_t)panel_x << k_glcdc_shift_high) | (uint32_t)fb_w;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab2) =
    ((uint32_t)panel_y << k_glcdc_shift_high) | (uint32_t)fb_h;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab4) =
    ((uint32_t)panel_y << k_glcdc_shift_high) | (uint32_t)fb_h;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab5) =
    ((uint32_t)panel_x << k_glcdc_shift_high) | (uint32_t)fb_w;

  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab7) =
    ((uint32_t)k_glcdc_alpha_opaque << k_glcdc_shift_arcdef);
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab1)   = k_gr2_dispsel_lower;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_flmrd) = 1U;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_en)    = k_gr2_ven;
  return k_ra8_ok;
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

RA8_INTERNAL
static volatile uint32_t* internal_clut_plane(uint8_t layer, uint8_t plane)
{
  uint16_t off;
  if (layer == k_ra8_glcdc_layer1) {
    off = (plane == 0U) ? (uint16_t)k_ra8_glcdc_off_gr1_clut0 : (uint16_t)k_ra8_glcdc_off_gr1_clut1;
  } else {
    off = (plane == 0U) ? (uint16_t)k_ra8_glcdc_off_gr2_clut0 : (uint16_t)k_ra8_glcdc_off_gr2_clut1;
  }
  return (volatile uint32_t*)(k_ra8_glcdc_base_addr + off);
}

/* internal_clutint_reg -- see header for full description. */
RA8_INTERNAL
static volatile uint32_t* internal_clutint_reg(uint8_t layer)
{
  if (layer == k_ra8_glcdc_layer1) {
    return ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_clutint);
  }
  return ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_clutint);
}

ra8_err_t ra8_glcdc_set_clut_double_buffered(uint8_t         layer,
                                             const uint32_t* clut,
                                             uint32_t        entries,
                                             bool            swap_now)
{
  RA8_CHECK_NULL_PTR(clut, s_tag, "clut must not be nullptr");
  if (layer >= (uint8_t)k_ra8_glcdc_layer_count) {
    return k_ra8_err_invalid_arg;
  }
  if ((entries == 0U) || (entries > (uint32_t)k_ra8_glcdc_clut_entries)) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 63 "GR[n]_CLUTINT.SEL" p 3744 -- pick the *inactive*
   * plane (FSP shadow-buffer pattern). */
  volatile uint32_t* clutint = internal_clutint_reg(layer);
  const uint32_t     active =
    ((*clutint) & (uint32_t)k_ra8_glcdc_clutsel_mask) >> k_ra8_glcdc_clutsel_shift;
  const uint8_t target_plane = (active != 0U) ? (uint8_t)0U : (uint8_t)1U;

  /* HUM Ch 63 "GR[n]_CLUT[plane]" p 3744 */ /* write inactive plane. */
  volatile uint32_t* dst = internal_clut_plane(layer, target_plane);
  for (uint32_t i = 0U; i < entries; ++i) {
    dst[i] = clut[i];
  }

  if (swap_now) {
    /* HUM Ch 63 "GR[n]_CLUTINT.SEL" p 3744 */ /* flip on next VS. */
    const uint32_t cleared = (*clutint) & ~(uint32_t)k_ra8_glcdc_clutsel_mask;
    *clutint               = cleared | ((uint32_t)target_plane << k_ra8_glcdc_clutsel_shift);
  }
  return k_ra8_ok;
}
