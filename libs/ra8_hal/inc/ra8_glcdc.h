/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_glcdc.h
 * @brief Graphics LCD Controller driver (two-layer with alpha blending)
 * @ingroup grp_hal_display
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_glcdc_regs.h"

/**
 * @struct ra8_glcdc_timing_t
 * @brief Panel RGB timing the driver programs (accessory parameters).
 *
 * @details
 * Raw horizontal/vertical timing for the attached RGB panel: active size plus
 * front porch, back porch, and sync width. These describe the DISPLAY, not the
 * GLCDC peripheral, so the caller supplies them from the board's BSP and the
 * driver stays panel-agnostic. The driver derives the line/frame totals.
 *
 * @since 0.1.0
 */
typedef struct {
  uint16_t h_active; /**< Active pixels per line.        */
  uint16_t h_front;  /**< Horizontal front porch (px).   */
  uint16_t h_back;   /**< Horizontal back porch (px).    */
  uint16_t h_sync;   /**< HSYNC pulse width (px clocks). */
  uint16_t v_active; /**< Active lines per frame.        */
  uint16_t v_front;  /**< Vertical front porch (lines).  */
  uint16_t v_back;   /**< Vertical back porch (lines).   */
  uint16_t v_sync;   /**< VSYNC pulse width (lines).     */
} ra8_glcdc_timing_t;

/**
 * @struct ra8_glcdc_config_t
 * @brief Minimal GLCDC configuration for a single-layer panel.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra8_glcdc_init`` in
 * ``libs/ra8_hal/src/ra8_glcdc.c``.
 */
typedef struct {
  uint32_t              framebuffer_addr; /**< SRAM or SDRAM address.   */
  uint16_t              width_px;         /**< Visible width.           */
  uint16_t              height_px;        /**< Visible height.          */
  ra8_glcdc_pixel_fmt_t format;           /**< Pixel format code.       */
  ra8_glcdc_timing_t    timing;           /**< Panel timing (from BSP). */
} ra8_glcdc_config_t;

/**
 * @struct ra8_glcdc_layer2_cfg_t
 * @brief Configuration for the background-image (Graphics 2) layer.
 *
 * @details
 * Graphics 2 is the lower layer in the GLCDC blender; Graphics 1
 * (the UI overlay configured by ``ra8_glcdc_init``) is composited
 * over the top of it. Layer 2 has its own framebuffer base, format,
 * line stride, on-panel position, on-panel size, and a constant
 * alpha that gates alpha-blending of the lower layer.
 */
typedef struct {
  uint32_t              framebuffer_addr;  /**< Layer-2 framebuffer base address.     */
  uint32_t              line_stride_bytes; /**< Bytes between successive scan lines.  */
  uint16_t              width_px;          /**< Layer-2 image width  (pixels).        */
  uint16_t              height_px;         /**< Layer-2 image height (lines).         */
  uint16_t              pos_x;             /**< Top-left X within the panel viewport. */
  uint16_t              pos_y;             /**< Top-left Y within the panel viewport. */
  ra8_glcdc_pixel_fmt_t format;            /**< Pixel format code (FLM6.FORMAT).      */
  uint8_t               alpha;             /**< Constant alpha 0..255 (AB7.ARCDEF).   */
} ra8_glcdc_layer2_cfg_t;

/**
 * @enum ra8_glcdc_blend_mode_t
 * @brief Compositing mode between Graphics 1 and Graphics 2.
 */
typedef enum : uint8_t {
  k_ra8_blend_overwrite = 0U, /**< Layer 1 fully covers layer 2 (DISPSEL=01).      */
  k_ra8_blend_normal    = 1U, /**< Layer 1 over layer 2, no alpha (DISPSEL=10).    */
  k_ra8_blend_alpha     = 2U, /**< Per-pixel + global alpha blend (DISPSEL=10+AR). */
} ra8_glcdc_blend_mode_t;

/**
 * @enum ra8_glcdc_dither_mode_t
 * @brief Output-stage dithering mode for 24bpp -> 18/16bpp panels.
 */
typedef enum : uint8_t {
  k_ra8_dither_off      = 0U, /**< No dithering (PDTHA.SEL=0).       */
  k_ra8_dither_truncate = 1U, /**< Round/truncate (PDTHA.SEL=2).     */
  k_ra8_dither_2x2      = 2U, /**< 2x2 ordered dither (PDTHA.SEL=3). */
} ra8_glcdc_dither_mode_t;

/**
 * @brief Initialise GLCDC with the EK-RA8D2 1024x600 timings and the
 *        supplied graphics layer 1 framebuffer.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_glcdc_init(const ra8_glcdc_config_t* cfg);

/**
 * @brief Start or stop the panel output.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_glcdc_start(bool enable);

/**
 * @typedef ra8_glcdc_event_fn_t
 * @brief GLCDC event callback (VSYNC / line detect / underflow).
 */
typedef void (*ra8_glcdc_event_fn_t)(void* ctx, uint32_t status_mask);

/**
 * @brief Tear down GLCDC (stop + MSTP release).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_glcdc_deinit(void);

/**
 * @brief Read GLCDC status register (VSYNC/HSYNC/underflow).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_glcdc_get_status(uint32_t* out_mask);

/**
 * @brief Clear GLCDC status flags.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_glcdc_clear_status(uint32_t mask);

/**
 * @brief Attach a GLCDC event callback.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_glcdc_attach_handler(ra8_glcdc_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch a GLCDC event -- snapshot + fire callback.
 * @since 0.1.0
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 */
void ra8_glcdc_dispatch(void);

/**
 * @brief Put GLCDC into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_glcdc_enter_stop(void);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_glcdc_exit_stop(void);

/**
 * @brief Configure the Graphics 2 (background-image) layer.
 *
 * @details
 * Programmes the per-layer GR2 register block (FLM2/FLM3/FLM5/FLM6/
 * AB1..AB7) so the layer is read from ``cfg->framebuffer_addr``,
 * placed at ``(cfg->pos_x, cfg->pos_y)`` inside the panel viewport
 * with size ``(cfg->width_px, cfg->height_px)``, and treated as the
 * lower layer in the blender. The constant alpha is loaded into
 * AB7.ARCDEF and gated by AB1.ARCON when the active blend mode is
 * ``k_ra8_blend_alpha``.
 *
 * @param[in] cfg Layer configuration. Must be non-NULL.
 * @return ra8_err_t
 * @retval k_ra8_ok            Layer 2 programmed successfully.
 * @retval k_ra8_err_null_ptr  ``cfg`` was NULL.
 *
 * @pre  GLCDC has been opened via ``ra8_glcdc_init``.
 * @pre  ``cfg->framebuffer_addr`` is valid for the chosen pixel format.
 * @post GR2_FLM2 == ``cfg->framebuffer_addr``.
 * @post GR2 layer is enabled for read (FLMRD.RENB = 1).
 *
 * @note Not thread-safe; caller serialises register access.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_glcdc_set_layer2(const ra8_glcdc_layer2_cfg_t* cfg);

/**
 * @brief Make graphics layer 1 visible by pointing it at a framebuffer.
 *
 * @details
 * Flips GR1 from the default hidden state (DISPSEL=transparent,
 * FLMRD=0, alpha=0) into "show this framebuffer" mode:
 *   - GR1.FLM2.SADDR  = ``fb_addr``      (framebuffer base)
 *   - GR1.AB7.ARCDEF  = 0xFF             (fully opaque)
 *   - GR1.AB1.DISPSEL = non_transparent  (current layer drawn)
 *   - GR1.FLMRD.RENB  = 1                (enable AXI fetch)
 *   - GR1.VEN         = 1                (commit on next VS)
 *
 * Width / height / format / position were already programmed by
 * ``ra8_glcdc_init``; the caller just provides the new framebuffer
 * pointer.  Caller is responsible for filling the framebuffer in
 * cache-coherent fashion (writes to 0x60000000-0x9FFFFFFF SDRAM are
 * non-cacheable by the Cortex-M85 default memory map).
 *
 * @param[in] fb_addr Base address of the layer-1 framebuffer
 *                    (must match the format passed to
 *                    ``ra8_glcdc_init``; alignment is implementation
 *                    defined, 64-byte AXI burst boundary is safe).
 *
 * @return Error code.
 * @retval k_ra8_ok                 Layer 1 enabled.
 * @retval k_ra8_err_invalid_state  GLCDC not in DISPLAYING state.
 *
 * @pre ``ra8_glcdc_init`` and ``ra8_glcdc_start`` have been called.
 * @pre ``fb_addr`` points to a valid framebuffer of the dimensions
 *      passed to ``ra8_glcdc_init``.
 * @post GR1 fetches and composites pixels from ``fb_addr``.
 * @post Layer-1 changes are visible from the next vertical sync.
 *
 * @note Single-threaded; not safe to call from IRQ.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_glcdc_layer1_show(uintptr_t fb_addr);

/**
 * @brief Make graphics layer 2 visible at an arbitrary panel position.
 *
 * @details Sets GR2's framebuffer pointer / stride / dimensions /
 * position registers and pulses VEN to commit on the next vsync.
 * The framebuffer is interpreted as RGB565.
 *
 * @param[in] fb_addr Base address of the layer-2 framebuffer (RGB565).
 * @param[in] panel_x Horizontal position on the panel in GLCDC coords.
 * @param[in] panel_y Vertical   position on the panel in GLCDC coords.
 * @param[in] fb_w    Framebuffer width in pixels.
 * @param[in] fb_h    Framebuffer height in pixels.
 *
 * @return Error code.
 * @retval k_ra8_ok Layer 2 enabled.
 *
 * @pre `ra8_glcdc_init` and `ra8_glcdc_start` have been called.
 * @pre `fb_addr` points to an `fb_w * fb_h * 2`-byte RGB565
 *      framebuffer.
 * @post GR2 fetches and composites pixels from `fb_addr` at
 *       (panel_x, panel_y).
 * @post Layer-2 changes are visible from the next vertical sync.
 *
 * @note Single-threaded; not safe to call from IRQ.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_glcdc_layer2_show(uintptr_t fb_addr,
                                              uint16_t  panel_x,
                                              uint16_t  panel_y,
                                              uint16_t  fb_w,
                                              uint16_t  fb_h);

/**
 * @brief Enable chroma-key transparency on graphics layer 2.
 *
 * @details Pixels in GR2's framebuffer whose RGB888 expansion
 * matches `key_rgb888` are treated as fully transparent at
 * composite time -- the layer below (BG plane) shows through
 * those pixels.  All other pixels remain opaque.  This lets a
 * single RGB565 GR2 layer host multiple opaque shapes with the
 * BG plane visible between them.
 *
 * Caller-provided key is in 0xRRGGBB form (alpha is ignored).
 * For an RGB565 framebuffer, paint matching pixels with the
 * RGB565 word whose 5/6/5-to-8/8/8 expansion equals `key_rgb888`
 * (e.g. magenta: paint 0xF81F into the fb and pass 0xFF00FF here).
 *
 * @param[in] key_rgb888 Chroma-key match colour in 0x00RRGGBB form.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Chroma-key enabled; takes effect on next vsync.
 *
 * @pre `ra8_glcdc_layer2_show` has been called.
 * @post GR2.AB7.CKON = 1, GR2.AB8 = key, GR2.AB9 = transparent.
 * @post GR2.VEN is asserted (auto-clears on next vsync).
 *
 * @note Single-threaded; not safe to call from IRQ.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_glcdc_layer2_chroma_key_enable(uint32_t key_rgb888);

/**
 * @brief Set the layer-1-over-layer-2 blend mode and global alpha.
 *
 * @details
 * Writes layer-1 AB1.DISPSEL / AB1.ARCON and AB7.ARCDEF to select
 * "overwrite", "normal" (top opaque over bottom), or "alpha" (top
 * blended over bottom using per-pixel alpha multiplied by the global
 * alpha argument).
 *
 * @param[in] mode         Compositing mode.
 * @param[in] global_alpha 0..255 constant alpha applied in
 *                         ``k_ra8_blend_alpha`` mode (AB7.ARCDEF).
 * @return ra8_err_t
 * @retval k_ra8_ok               Blend programmed successfully.
 * @retval k_ra8_err_invalid_arg  ``mode`` was out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_glcdc_set_blend(ra8_glcdc_blend_mode_t mode, uint8_t global_alpha);

/**
 * @brief Set the panel-wide background colour shown where neither
 *        layer covers a pixel.
 *
 * @details
 * Writes ``argb`` to BG.BGC. The alpha byte is ignored by hardware
 * but preserved in the register for symmetry with the layer-base
 * colour registers.
 *
 * @param[in] argb 0xAARRGGBB packed colour.
 * @return ra8_err_t
 * @retval k_ra8_ok Always.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_glcdc_set_background_color(uint32_t argb);

/**
 * @brief Update a CLUT plane double-buffered, optionally swapping
 *        on the next vsync.
 *
 * @details
 * GLCDC keeps two CLUT planes per layer (CLUT0/CLUT1) selected by
 * GR[layer].CLUTINT.SEL. This function writes ``entries`` 32-bit
 * ARGB entries into the inactive plane, then either:
 *   - leaves SEL alone (``swap_now == false``): the new table sits
 *     ready for a future swap, mirroring FSP's ``R_GLCDC_ClutEdit``
 *     "shadow" pattern, or
 *   - flips SEL (``swap_now == true``): the hardware will switch to
 *     the freshly-written table at the next vertical sync, mirroring
 *     FSP's ``R_GLCDC_ColorPaletteUpdate``.
 *
 * @param[in] layer    0 = Graphics 1, 1 = Graphics 2.
 * @param[in] clut     Source array of ARGB8888 entries.
 * @param[in] entries  Number of entries to copy (1..256).
 * @param[in] swap_now true to flip CLUTINT.SEL after the write.
 * @return ra8_err_t
 * @retval k_ra8_ok               Programmed successfully.
 * @retval k_ra8_err_null_ptr     ``clut`` was NULL.
 * @retval k_ra8_err_invalid_arg  ``layer >= 2`` or ``entries`` out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_glcdc_set_clut_double_buffered(uint8_t         layer,
                                                           const uint32_t* clut,
                                                           uint32_t        entries,
                                                           bool            swap_now);

/**
 * @brief Programme the output dithering mode (PDTHA.SEL).
 *
 * @details
 * Selects the dithering algorithm applied when the 24bpp internal
 * pipeline drives a narrower (18bpp / 16bpp) panel.
 *
 * @param[in] mode Dither mode.
 * @return ra8_err_t
 * @retval k_ra8_ok               Dither register written.
 * @retval k_ra8_err_invalid_arg  ``mode`` was out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_glcdc_set_dithering(ra8_glcdc_dither_mode_t mode);

/**
 * @brief Programme per-channel output brightness.
 *
 * @details
 * Writes ``g`` to BRIGHT1.BRTG and packs ``b``/``r`` into
 * BRIGHT2.BRTB/BRTR. The values are 8-bit offsets that are added to
 * the 10-bit channel (the low bits of the 10-bit BRTx field are
 * left as 0 -- callers wanting the full range supply pre-scaled
 * numbers in the 0..255 portion).
 *
 * @param[in] r Red brightness offset (0..255).
 * @param[in] g Green brightness offset (0..255).
 * @param[in] b Blue brightness offset (0..255).
 * @return ra8_err_t
 * @retval k_ra8_ok Always.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_glcdc_set_brightness(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Programme per-channel output contrast gain.
 *
 * @details
 * Writes the three 8-bit gains into CONTRAST.CONTG/CONTB/CONTR.
 * 0x80 == x1.000 gain (FSP convention).
 *
 * @param[in] r Red contrast gain   (0..255, 0x80 = 1.0).
 * @param[in] g Green contrast gain (0..255, 0x80 = 1.0).
 * @param[in] b Blue contrast gain  (0..255, 0x80 = 1.0).
 * @return ra8_err_t
 * @retval k_ra8_ok Always.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_glcdc_set_contrast(uint8_t r, uint8_t g, uint8_t b);

/* =============================================================================
 * Gamma correction (piecewise-linear LUT per colour channel)
 * Implemented in libs/ra8_hal/src/ra8_glcdc_gamma.c
 * =============================================================================
 */

/**
 * @enum ra8_glcdc_color_channel_t
 * @brief Selector for the three GLCDC gamma colour channels.
 *
 * @details
 * The GLCDC has three independent gamma correction blocks, one per colour
 * channel.  HUM Ch 63 "GLCDC" p 3789 maps the physical register blocks:
 *   GAM[0] -> Red   (k_ra8_glcdc_ch_r)
 *   GAM[1] -> Green (k_ra8_glcdc_ch_g)
 *   GAM[2] -> Blue  (k_ra8_glcdc_ch_b)
 *
 * @invariant Values are contiguous starting at 0; range is [0, k_ra8_glcdc_ch_count).
 *
 * @code
 * const uint16_t gains[16]      = { ... };
 * const uint16_t thresholds[16] = { ... };
 * ra8_glcdc_set_gamma(k_ra8_glcdc_ch_r, gains, thresholds, 16);
 * @endcode
 *
 * @see ra8_glcdc_set_gamma()
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_glcdc_ch_r     = 0U, /**< Red channel   -> GAM[0].              */
  k_ra8_glcdc_ch_g     = 1U, /**< Green channel -> GAM[1].              */
  k_ra8_glcdc_ch_b     = 2U, /**< Blue channel  -> GAM[2].              */
  k_ra8_glcdc_ch_count = 3U, /**< Number of colour channels (sentinel). */
} ra8_glcdc_color_channel_t;

/**
 * @enum ra8_glcdc_gamma_lut_const_t
 * @brief Piecewise-linear LUT depth constants for the GLCDC gamma correction.
 *
 * @details
 * The GLCDC gamma hardware provides 8 LUT registers per channel, each holding
 * two 11-bit gain coefficients, giving 16 gain values total.  The 8 AREA
 * registers each hold two 10-bit boundary thresholds, giving 16 threshold
 * values (HUM Ch 63 "GAMx_LUTn / GAMx_AREAn" p 3790-3793).  Both the gain
 * and threshold arrays passed to `ra8_glcdc_set_gamma()` must have exactly
 * `k_ra8_glcdc_gamma_lut_depth` elements.
 *
 * @see ra8_glcdc_set_gamma()
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_glcdc_gamma_lut_depth = 16U, /**< 16 segments (8 LUT regs x 2 gains). */
} ra8_glcdc_gamma_lut_const_t;

/**
 * @brief Programme the piecewise-linear gamma correction table for one channel.
 *
 * @details
 * Writes the 8 GAMx_LUTn gain-pair registers and the 8 GAMx_AREAn
 * threshold-pair registers for the selected colour channel.  Each LUT register
 * packs two 11-bit gain coefficients (GAIN0 in bits[26:16], GAIN1 in bits[10:0]);
 * each AREA register packs two 10-bit boundary values (TH1 in bits[25:16],
 * TH2 in bits[9:0]).  Call `ra8_glcdc_gamma_enable(true)` after programming all
 * three channels to activate the correction pipeline.
 *
 * @param[in] channel   Colour channel to program (R, G, or B).
 * @param[in] gain      Array of `k_ra8_glcdc_gamma_lut_depth` 11-bit gain values
 *                      (0..2047). gain[0] corresponds to GAIN0 of LUT[0].
 * @param[in] threshold Array of `k_ra8_glcdc_gamma_lut_depth` 10-bit boundary
 *                      values (0..1023). threshold[0] corresponds to TH1 of AREA[0].
 * @param[in] count     Must equal `k_ra8_glcdc_gamma_lut_depth` (16); enforced.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok               LUT and AREA registers written.
 * @retval k_ra8_err_null_ptr     `gain` or `threshold` is NULL.
 * @retval k_ra8_err_invalid_arg  `channel` >= `k_ra8_glcdc_ch_count`, or
 *                               `count` != `k_ra8_glcdc_gamma_lut_depth`.
 *
 * @pre  GLCDC has been initialised via `ra8_glcdc_init()`.
 * @pre  `gain` and `threshold` point to arrays of at least `count` elements.
 * @post All 8 GAMx_LUTn registers hold the packed gain pairs.
 * @post All 8 GAMx_AREAn registers hold the packed threshold pairs.
 *
 * @note Not thread-safe; caller must serialise access to GLCDC registers.
 * @see  ra8_glcdc_gamma_enable()   Activate or deactivate the pipeline.
 * @since 0.1.0
 *
 * @par HUM:
 * Ch 63 "GAMx_LUTn Gamma Correction LUT Register"  p 3790.
 * Ch 63 "GAMx_AREAn Gamma Correction Area Register" p 3793.
 */
[[nodiscard]] ra8_err_t ra8_glcdc_set_gamma(ra8_glcdc_color_channel_t channel,
                                            const uint16_t*           gain,
                                            const uint16_t*           threshold,
                                            uint8_t                   count);

/**
 * @brief Enable or disable the GLCDC gamma correction pipeline.
 *
 * @details
 * Writes the GAMSW.GAMON bit (bit 0) in the output-control block, which gates
 * the piecewise-linear correction circuit for all three colour channels.  Call
 * after programming all three channel tables with `ra8_glcdc_set_gamma()`.
 *
 * @param[in] enable `true` to activate gamma correction; `false` to bypass it.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok            GAMSW register updated.
 * @retval k_ra8_err_null_ptr  GAMSW register address could not be resolved
 *                            (should not occur in normal operation).
 *
 * @pre  GLCDC has been initialised via `ra8_glcdc_init()`.
 * @pre  All three channel tables have been written via `ra8_glcdc_set_gamma()`
 *       when enabling.
 * @post OUT.GAMSW.GAMON == (enable ? 1 : 0).
 * @post The gamma pipeline state is visible on the next active pixel.
 *
 * @note Not thread-safe; caller must serialise access to GLCDC registers.
 * @see  ra8_glcdc_set_gamma()   Programme individual channel tables.
 * @since 0.1.0
 *
 * @par HUM:
 * Ch 63 "OUT_GAMSW Gamma Correction Block Switch Register" p 3789.
 */
[[nodiscard]] ra8_err_t ra8_glcdc_gamma_enable(bool enable);

#ifdef __cplusplus
}
#endif
