/**
 * @file ra8d2_glcdc_regs.h
 * @brief Graphics LCD Controller (GLCDC) register layout for the RA8D2
 *
 * @details
 * GLCDC at `0x40342000` drives up to two RGB graphics layers with
 * alpha blending, chroma key, dither, gamma correction, and
 * parallel-RGB or MIPI-DSI output. The EK-RA8D2's 1024x600 7"
 * parallel TFT connects here via the GLCDC expansion header.
 *
 * ## Block structure
 *
 * The GLCDC register map is organised as several sub-blocks at
 * different offsets inside the 4 KiB window:
 *
 *  - **Background (BG)** at 0x000: horizontal / vertical timing,
 *    background colour.
 *  - **Graphics 1 (GR1)** at 0x150: base address, frame size,
 *    scaling, alpha, chromakey.
 *  - **Graphics 2 (GR2)** at 0x270: same as GR1 for the second
 *    layer.
 *  - **Output control (OUT)** at 0x390: dither, clipping, serial
 *    mode, phase clock.
 *  - **System control** at 0x420: enable, interrupts, status.
 *
 * This header models the offsets and a handful of the most-touched
 * field enums; a full driver will grow per-block structs.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum : uintptr_t {
  k_ra_glcdc_base_addr    = 0x40342000UL,
  k_ra_mipi_dsi_base_addr = 0x40346000UL,
  k_ra_mipi_phy_base_addr = 0x40346C00UL,
  k_ra_mipi_csi_base_addr = 0x40347000UL,
  k_ra_ceu_base_addr      = 0x40348000UL,
} ra_display_addr_t;

/**
 * @enum ra_glcdc_block_off_t
 * @brief GLCDC sub-block offsets within the 4 KiB window.
 */
typedef enum : uint16_t {
  k_ra_glcdc_off_bg_en      = 0x000U, /**< BG enable.                   */
  k_ra_glcdc_off_bg_per     = 0x004U, /**< BG period (H/V).             */
  k_ra_glcdc_off_bg_sync    = 0x008U, /**< BG sync width.               */
  k_ra_glcdc_off_bg_vsize   = 0x00CU, /**< BG vertical size.            */
  k_ra_glcdc_off_bg_hsize   = 0x010U, /**< BG horizontal size.          */
  k_ra_glcdc_off_bg_bgc     = 0x014U, /**< BG background colour.        */
  k_ra_glcdc_off_bg_int     = 0x024U, /**< BG interrupt status.         */
  k_ra_glcdc_off_gr1_en     = 0x150U, /**< GR1 enable.                  */
  k_ra_glcdc_off_gr1_fmt    = 0x154U, /**< GR1 pixel format.            */
  k_ra_glcdc_off_gr1_saddr  = 0x158U, /**< GR1 start address (SRAM).    */
  k_ra_glcdc_off_gr1_line   = 0x15CU, /**< GR1 line length in pixels.   */
  k_ra_glcdc_off_gr1_size   = 0x160U, /**< GR1 frame size.              */
  k_ra_glcdc_off_gr2_en     = 0x270U, /**< GR2 enable.                  */
  k_ra_glcdc_off_gr2_fmt    = 0x274U, /**< GR2 pixel format.            */
  k_ra_glcdc_off_gr2_saddr  = 0x278U, /**< GR2 start address (SRAM).    */
  k_ra_glcdc_off_gr2_line   = 0x27CU, /**< GR2 line length in pixels.   */
  k_ra_glcdc_off_gr2_size   = 0x280U, /**< GR2 frame size.              */
  k_ra_glcdc_off_out_set    = 0x390U, /**< Output control set.          */
  k_ra_glcdc_off_out_int    = 0x3A0U, /**< Output interrupt status.     */
  k_ra_glcdc_off_panel_dtha = 0x3B0U, /**< Panel dither control.      */
  k_ra_glcdc_off_panel_clk  = 0x3C0U, /**< Panel clock control.        */
  k_ra_glcdc_off_sys_cfg    = 0x420U, /**< System config (enable).      */
  k_ra_glcdc_off_sys_stat   = 0x424U, /**< System status.               */
  k_ra_glcdc_off_sys_intr   = 0x428U, /**< System IRQ enable.           */
} ra_glcdc_block_off_t;

/**
 * @enum ra_glcdc_pixel_fmt_t
 * @brief Pixel format codes written to `GRnFMT.FORMAT[2:0]`.
 */
typedef enum : uint8_t {
  k_ra_glcdc_fmt_argb8888 = 0x0U, /**< 32-bit ARGB.                 */
  k_ra_glcdc_fmt_rgb888   = 0x1U, /**< 24-bit RGB (packed).         */
  k_ra_glcdc_fmt_rgb565   = 0x2U, /**< 16-bit RGB565.               */
  k_ra_glcdc_fmt_argb1555 = 0x3U, /**< 16-bit ARGB1555.             */
  k_ra_glcdc_fmt_argb4444 = 0x4U, /**< 16-bit ARGB4444.             */
  k_ra_glcdc_fmt_clut8    = 0x5U, /**< 8-bit CLUT.                  */
  k_ra_glcdc_fmt_clut4    = 0x6U, /**< 4-bit CLUT.                  */
  k_ra_glcdc_fmt_clut1    = 0x7U, /**< 1-bit CLUT.                  */
} ra_glcdc_pixel_fmt_t;

/**
 * @brief Pointer to a 32-bit register inside the GLCDC block.
 * @param[in] offset One of the `k_ra_glcdc_off_*` values.
 * @return Volatile pointer to the register.
 */
static inline volatile uint32_t* ra_glcdc_reg32(ra_glcdc_block_off_t offset)
{
  return (volatile uint32_t*)(k_ra_glcdc_base_addr + (uint16_t)offset);
}

/* =============================================================================
 * EK-RA8D2 panel timings (1024 x 600, 7-inch parallel TFT)
 * =============================================================================
 *
 * Values taken from the FSP `board_ra8d2_ek.c` panel descriptor and
 * the panel datasheet in `docs/reference/`. These are the nominal
 * values; a real driver should read the selected panel descriptor
 * at runtime.
 */

typedef enum : uint16_t {
  k_ra_glcdc_ek_h_active = 1024U,
  k_ra_glcdc_ek_v_active = 600U,
  k_ra_glcdc_ek_h_front  = 160U,
  k_ra_glcdc_ek_h_back   = 140U,
  k_ra_glcdc_ek_h_sync   = 20U,
  k_ra_glcdc_ek_v_front  = 12U,
  k_ra_glcdc_ek_v_back   = 20U,
  k_ra_glcdc_ek_v_sync   = 3U,
} ra_glcdc_ek_timing_t;

typedef enum : uint32_t {
  k_ra_glcdc_ek_pixel_clk_hz = 51200000UL, /**< ~51.2 MHz pixel clock. */
} ra_glcdc_ek_clk_t;

#ifdef __cplusplus
}
#endif
