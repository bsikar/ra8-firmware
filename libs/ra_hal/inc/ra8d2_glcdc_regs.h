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
 * ## Block structure (verified against FSP R_GLCDC_Type in
 * R7KA8D2KF_core0.h lines 6980-7045)
 *
 *  | Offset   | Block              | Size   | Purpose                     |
 *  |---------:|--------------------|-------:|-----------------------------|
 *  | 0x0000   | GR1_CLUT0[256]     | 0x0400 | Graphics 1 CLUT plane 0     |
 *  | 0x0400   | GR1_CLUT1[256]     | 0x0400 | Graphics 1 CLUT plane 1     |
 *  | 0x0800   | GR2_CLUT0[256]     | 0x0400 | Graphics 2 CLUT plane 0     |
 *  | 0x0C00   | GR2_CLUT1[256]     | 0x0400 | Graphics 2 CLUT plane 1     |
 *  | 0x1000   | BG                 | 0x001C | Background timing + bgc     |
 *  | 0x1100   | GR[0]              | 0x0100 | Graphics layer 0            |
 *  | 0x1200   | GR[1]              | 0x0100 | Graphics layer 1            |
 *  | 0x1300   | GAM[0..2]          | 0x00C0 | Gamma tables (3 channels)   |
 *  | 0x13C0   | OUT                | 0x0028 | Output control              |
 *  | 0x1400   | TCON               | 0x002C | Timing controller           |
 *  | 0x1440   | SYSCNT             | 0x0014 | System control / ints       |
 *
 * The previous version of this header placed BG at offset 0x000,
 * GR1 at 0x150, GR2 at 0x270, OUT at 0x390, SYSCNT at 0x420 --
 * all of which landed inside GR1/GR2 CLUTs or reserved gaps. Every
 * `k_ra_glcdc_off_*` has been re-derived against FSP.
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
 * @brief Absolute register offsets inside the GLCDC window.
 *
 * @details
 * Names mirror FSP's `R_GLCDC_Type` field names so a reader can
 * cross-reference directly. Offsets are verified byte-for-byte
 * against FSP's sub-type structs (R_GLCDC_BG_Type, R_GLCDC_GR_Type,
 * R_GLCDC_OUT_Type, R_GLCDC_TCON_Type, R_GLCDC_SYSCNT_Type).
 */
typedef enum : uint16_t {
  /* ---- Graphics 1 CLUT planes ------------------------------------------ */
  k_ra_glcdc_off_gr1_clut0 = 0x0000U, /**< GR1_CLUT0[256] @ 0x0000.          */
  k_ra_glcdc_off_gr1_clut1 = 0x0400U, /**< GR1_CLUT1[256] @ 0x0400.          */
  k_ra_glcdc_off_gr2_clut0 = 0x0800U, /**< GR2_CLUT0[256] @ 0x0800.          */
  k_ra_glcdc_off_gr2_clut1 = 0x0C00U, /**< GR2_CLUT1[256] @ 0x0C00.          */

  /* ---- Background block ------------------------------------------------ */
  k_ra_glcdc_off_bg_en    = 0x1000U, /**< BG.EN: module enable.               */
  k_ra_glcdc_off_bg_per   = 0x1004U, /**< BG.PERI: free-running period.       */
  k_ra_glcdc_off_bg_sync  = 0x1008U, /**< BG.SYNC: sync position.             */
  k_ra_glcdc_off_bg_vsize = 0x100CU, /**< BG.VSIZE: vertical valid size.      */
  k_ra_glcdc_off_bg_hsize = 0x1010U, /**< BG.HSIZE: horizontal valid size.    */
  k_ra_glcdc_off_bg_bgc   = 0x1014U, /**< BG.BGC: background colour.          */
  k_ra_glcdc_off_bg_mon   = 0x1018U, /**< BG.MON: status monitor.             */

  /* ---- Graphics layer 0 (GR[0]) ---------------------------------------- */
  k_ra_glcdc_off_gr1_en      = 0x1100U, /**< GR[0].VEN: register update ctrl.   */
  k_ra_glcdc_off_gr1_flmrd   = 0x1104U, /**< GR[0].FLMRD: frame buffer read en. */
  k_ra_glcdc_off_gr1_flm1    = 0x1108U, /**< GR[0].FLM1: burst mode.            */
  k_ra_glcdc_off_gr1_saddr   = 0x110CU, /**< GR[0].FLM2.BASE: framebuffer addr. */
  k_ra_glcdc_off_gr1_flm3    = 0x1110U, /**< GR[0].FLM3: line offset bytes.     */
  k_ra_glcdc_off_gr1_line    = 0x1118U, /**< GR[0].FLM5: datanum + lnnum.       */
  k_ra_glcdc_off_gr1_fmt     = 0x111CU, /**< GR[0].FLM6.FORMAT: pixel format.   */
  k_ra_glcdc_off_gr1_ab1     = 0x1120U, /**< GR[0].AB1: alpha blend ctrl 1.     */
  k_ra_glcdc_off_gr1_ab2     = 0x1124U, /**< GR[0].AB2: graphics V size + pos.  */
  k_ra_glcdc_off_gr1_size    = 0x1128U, /**< GR[0].AB3: graphics H size + pos.  */
  k_ra_glcdc_off_gr1_ab4     = 0x112CU, /**< GR[0].AB4: alpha rect V size+pos.  */
  k_ra_glcdc_off_gr1_ab5     = 0x1130U, /**< GR[0].AB5: alpha rect H size+pos.  */
  k_ra_glcdc_off_gr1_ab6     = 0x1134U, /**< GR[0].AB6: fade rate / coef.       */
  k_ra_glcdc_off_gr1_ab7     = 0x1138U, /**< GR[0].AB7: chroma-key on, def alf. */
  k_ra_glcdc_off_gr1_ab8     = 0x113CU, /**< GR[0].AB8: chroma-key key colour.  */
  k_ra_glcdc_off_gr1_ab9     = 0x1140U, /**< GR[0].AB9: chroma-key replace col. */
  k_ra_glcdc_off_gr1_base    = 0x114CU, /**< GR[0].BASE: layer bg colour.       */
  k_ra_glcdc_off_gr1_clutint = 0x1150U, /**< GR[0].CLUTINT: CLUT swap + line.   */
  k_ra_glcdc_off_gr1_mon     = 0x1154U, /**< GR[0].MON: status monitor.         */

  /* ---- Graphics layer 1 (GR[1]) ---------------------------------------- */
  k_ra_glcdc_off_gr2_en      = 0x1200U, /**< GR[1].VEN.                         */
  k_ra_glcdc_off_gr2_flmrd   = 0x1204U, /**< GR[1].FLMRD.                       */
  k_ra_glcdc_off_gr2_flm1    = 0x1208U, /**< GR[1].FLM1.                        */
  k_ra_glcdc_off_gr2_saddr   = 0x120CU, /**< GR[1].FLM2.BASE.                   */
  k_ra_glcdc_off_gr2_flm3    = 0x1210U, /**< GR[1].FLM3.                        */
  k_ra_glcdc_off_gr2_line    = 0x1218U, /**< GR[1].FLM5.                        */
  k_ra_glcdc_off_gr2_fmt     = 0x121CU, /**< GR[1].FLM6.FORMAT.                 */
  k_ra_glcdc_off_gr2_ab1     = 0x1220U, /**< GR[1].AB1.                         */
  k_ra_glcdc_off_gr2_ab2     = 0x1224U, /**< GR[1].AB2: graphics V size + pos.  */
  k_ra_glcdc_off_gr2_size    = 0x1228U, /**< GR[1].AB3: graphics H size + pos.  */
  k_ra_glcdc_off_gr2_ab4     = 0x122CU, /**< GR[1].AB4: alpha rect V size+pos.  */
  k_ra_glcdc_off_gr2_ab5     = 0x1230U, /**< GR[1].AB5: alpha rect H size+pos.  */
  k_ra_glcdc_off_gr2_ab6     = 0x1234U, /**< GR[1].AB6: fade rate / coef.       */
  k_ra_glcdc_off_gr2_ab7     = 0x1238U, /**< GR[1].AB7: chroma-key on, def alf. */
  k_ra_glcdc_off_gr2_ab8     = 0x123CU, /**< GR[1].AB8: chroma-key key colour.  */
  k_ra_glcdc_off_gr2_ab9     = 0x1240U, /**< GR[1].AB9: chroma-key replace col. */
  k_ra_glcdc_off_gr2_base    = 0x124CU, /**< GR[1].BASE.                        */
  k_ra_glcdc_off_gr2_clutint = 0x1250U, /**< GR[1].CLUTINT.                     */
  k_ra_glcdc_off_gr2_mon     = 0x1254U, /**< GR[1].MON.                         */

  /* ---- Gamma correction (GAM[0..2]) ------------------------------------ */
  k_ra_glcdc_off_gam0 = 0x1300U, /**< GAM[0] block base.                       */
  k_ra_glcdc_off_gam1 = 0x1340U, /**< GAM[1] block base.                       */
  k_ra_glcdc_off_gam2 = 0x1380U, /**< GAM[2] block base.                       */

  /* ---- Output control block (OUT) -------------------------------------- */
  k_ra_glcdc_off_out_vlatch   = 0x13C0U, /**< OUT.VLATCH.                       */
  k_ra_glcdc_off_out_set      = 0x13C4U, /**< OUT.SET: output interface.        */
  k_ra_glcdc_off_out_bright1  = 0x13C8U, /**< OUT.BRIGHT1.                       */
  k_ra_glcdc_off_out_bright2  = 0x13CCU, /**< OUT.BRIGHT2.                       */
  k_ra_glcdc_off_out_contrast = 0x13D0U, /**< OUT.CONTRAST.                      */
  k_ra_glcdc_off_panel_dtha   = 0x13D4U, /**< OUT.PDTHA: panel dither.           */
  k_ra_glcdc_off_out_clkphase = 0x13E4U, /**< OUT.CLKPHASE.                      */

  /* ---- Timing control block (TCON) ------------------------------------- */
  k_ra_glcdc_off_tcon_tim   = 0x1404U, /**< TCON.TIM: reference timing.         */
  k_ra_glcdc_off_tcon_stva1 = 0x1408U, /**< TCON.STVA1.                         */
  k_ra_glcdc_off_tcon_stva2 = 0x140CU, /**< TCON.STVA2.                         */
  k_ra_glcdc_off_tcon_stvb1 = 0x1410U, /**< TCON.STVB1.                         */
  k_ra_glcdc_off_tcon_stvb2 = 0x1414U, /**< TCON.STVB2.                         */
  k_ra_glcdc_off_tcon_stha1 = 0x1418U, /**< TCON.STHA1.                         */
  k_ra_glcdc_off_tcon_stha2 = 0x141CU, /**< TCON.STHA2.                         */
  k_ra_glcdc_off_tcon_sthb1 = 0x1420U, /**< TCON.STHB1.                         */
  k_ra_glcdc_off_tcon_sthb2 = 0x1424U, /**< TCON.STHB2.                         */
  k_ra_glcdc_off_tcon_de    = 0x1428U, /**< TCON.DE.                            */

  /* ---- System control block (SYSCNT) ----------------------------------- */
  k_ra_glcdc_off_sys_cfg   = 0x1440U, /**< SYSCNT.DTCTEN (state detection).    */
  k_ra_glcdc_off_sys_intr  = 0x1444U, /**< SYSCNT.INTEN.                       */
  k_ra_glcdc_off_sys_clr   = 0x1448U, /**< SYSCNT.STCLR.                       */
  k_ra_glcdc_off_sys_stat  = 0x144CU, /**< SYSCNT.STMON.                       */
  k_ra_glcdc_off_panel_clk = 0x1450U, /**< SYSCNT.PANEL_CLK.                   */
} ra_glcdc_block_off_t;

/**
 * @enum ra_glcdc_pixel_fmt_t
 * @brief Pixel format codes written to `GRn_FLM6.FORMAT[30:28]`.
 *
 * @details
 * Verified against HUM Ch 63 "GLCDC" Table 63.11 "Graphics data
 * format" and FSP `r_glcdc.h` `glcdc_gr_format_t`.
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
