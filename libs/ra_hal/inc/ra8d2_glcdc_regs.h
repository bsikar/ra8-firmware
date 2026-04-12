/**
 * @file ra8d2_glcdc_regs.h
 * @brief GLCDC (Graphics LCD Controller) register base for the Renesas RA8D2
 *
 * @details
 * GLCDC at `0x40342000` drives up to two RGB layers with alpha
 * blending to a parallel RGB or MIPI-DSI panel. The EK-RA8D2's
 * 1024x600 7" parallel TFT connects here via the GLCDC expansion
 * header. Related blocks: MIPI-DSI at `0x40346000`, MIPI-PHY at
 * `0x40346C00`, CEU at `0x40348000`, MIPI-CSI at `0x40347000`.
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

#ifdef __cplusplus
}
#endif
