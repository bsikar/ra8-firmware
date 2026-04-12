/**
 * @file ra8d2_lvd_regs.h
 * @brief Low Voltage Detection (LVD) register layout for the Renesas RA8D2
 *
 * @details
 * Three independent low-voltage detectors (LVD0, LVD1, LVD2), each
 * able to fire an IRQ or a reset when VCC drops below a programmable
 * threshold. Control registers live inside the SYSC block.
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
  k_ra_lvd_lvcmpcr_off = 0x417U, /**< LVCMPCR compare control.           */
  k_ra_lvd_lvdlvlr_off = 0x418U, /**< LVDLVLR voltage level.             */
  k_ra_lvd0_cr0_off    = 0x41AU, /**< LVD0 CR0.                          */
  k_ra_lvd0_cr1_off    = 0x0E0U, /**< LVD0 CR1.                          */
  k_ra_lvd0_sr_off     = 0x0E1U, /**< LVD0 Status.                       */
  k_ra_lvd1_cr0_off    = 0x41BU, /**< LVD1 CR0.                          */
  k_ra_lvd1_cr1_off    = 0x0E2U, /**< LVD1 CR1.                          */
  k_ra_lvd1_sr_off     = 0x0E3U, /**< LVD1 Status.                       */
  k_ra_lvd2_cr0_off    = 0x41CU, /**< LVD2 CR0.                          */
  k_ra_lvd2_cr1_off    = 0x0E4U, /**< LVD2 CR1.                          */
  k_ra_lvd2_sr_off     = 0x0E5U, /**< LVD2 Status.                       */
} ra_lvd_off_t;

#ifdef __cplusplus
}
#endif
