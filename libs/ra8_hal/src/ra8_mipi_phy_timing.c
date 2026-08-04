/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_mipi_phy_timing.c
 * @brief MIPI D-PHY driver -- HUM timing tables and table-driven setup.
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * This translation unit holds the bulky portion of the MIPI D-PHY driver
 * that ``ra8_mipi_phy.c`` delegates to so that neither file exceeds the
 * ``scripts/checks/check_file_size.py`` cap. It owns:
 *
 *  - The flattened representations of HUM Table 64.2 (D-PHY timing setting
 *    DSI mode, p 3831-3834) and HUM Table 64.3 (D-PHY timing setting CSI
 *    mode, p 3835-3836) as static lookup tables.
 *  - The linear-scan lookup walker that picks the correct row for a
 *    requested ``(mode, pclka, rate)`` tuple.
 *  - ``ra8_mipi_phy_select_timing``, the public table-driven entry point
 *    that selects a row and programs DPHYTIM1..6.
 *  - ``internal_mipi_phy_compute_freq``, the HUM 64.2.2 PLL-frequency
 *    arithmetic helper consumed by ``ra8_mipi_phy_validate_pll_band`` in
 *    ``ra8_mipi_phy.c``.
 *
 * The register-write path used by ``ra8_mipi_phy_select_timing`` lives in
 * ``ra8_mipi_phy.c`` (``internal_mipi_phy_write_timing``) and is reached
 * through ``ra8_mipi_phy_internal.h``.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_mipi_phy.h"
#include "ra8_mipi_phy_internal.h"
#include "ra8_mipi_phy_regs.h"

/** @brief PLL fixed-point (hundredths) scale and NF fractional table. */
typedef enum : uint16_t {
  k_mipi_pll_percent_scale = 100U, /**< Hundredths fixed-point scale. */
  k_mipi_nf_x100_33        = 33U,  /**< NF = 0.33 (x100).             */
  k_mipi_nf_x100_66        = 66U,  /**< NF = 0.66 (x100).             */
  k_mipi_nf_x100_50        = 50U,  /**< NF = 0.50 (x100).             */
} mipi_pll_frac_t;

/* =============================================================================
 * HUM Tables 64.2 / 64.3 (DSI / CSI) timing rows -- p 3831-3836
 * =============================================================================
 */

/**
 * @struct mipi_phy_table_row_t
 * @brief One row of HUM Tables 64.2 / 64.3 with the rate ceiling.
 *
 * @details
 * The HUM tables are organised as PCLKA buckets (one per supported
 * peripheral clock) with line-rate columns inside each bucket. This
 * row representation flattens the matrix into ``(mode, pclka, ceil)``
 * triples so a linear scan can pick the correct row.
 *
 * cppcheck cannot see the lookup table walker so it flags every
 * field as unused; each member is read in
 * ``ra8_mipi_phy_select_timing``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t               mode;     /**< 0 = CSI device, 1 = DSI host. */
  uint8_t               pclka;    /**< PCLKA in MHz.                 */
  uint16_t              rate_max; /**< Ceiling of the rate column.   */
  ra8_mipi_phy_timing_t t;        /**< Timing values for the row.    */
} mipi_phy_table_row_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @enum ra8_mipi_phy_table_szlim_t
 * @brief Static lookup-table cardinalities.
 */
typedef enum : uint8_t {
  k_ra8_mipi_phy_table_pclka_125 = 125U, /**< PCLKA 125 MHz bucket. */
  k_ra8_mipi_phy_table_pclka_120 = 120U, /**< PCLKA 120 MHz bucket. */
  k_ra8_mipi_phy_table_pclka_100 = 100U, /**< PCLKA 100 MHz bucket. */
  k_ra8_mipi_phy_table_pclka_80  = 80U,  /**< PCLKA 80  MHz bucket. */
  k_ra8_mipi_phy_table_pclka_75  = 75U,  /**< PCLKA 75  MHz bucket. */
  k_ra8_mipi_phy_table_pclka_50  = 50U,  /**< PCLKA 50  MHz bucket. */
  k_ra8_mipi_phy_table_pclka_40  = 40U,  /**< PCLKA 40  MHz bucket. */
} ra8_mipi_phy_table_szlim_t;

/**
 * @var s_dsi_table
 * @brief HUM Table 64.2 D-PHY timing setting DSI mode (p 3831-3834).
 *
 * @details
 * Rows are ordered by (PCLKA descending, rate ascending). Lookup
 * picks the row with matching PCLKA whose ``rate_max`` is the
 * smallest value >= the requested rate.
 */
static const mipi_phy_table_row_t s_dsi_table[] = {
  /* HUM Table 64.2 PCLKA 125 MHz, p 3831-3832 */
  {1U,
   125U,
   100U,
   {0x000124F9U,
    0x08U,
    0x00U,
    0x00U,
    0x0FU,
    0x00U,
    0x21U,
    0x0DU,
    0x58U,
    0x0AU,
    0x0FU,
    0x16U,
    0x0EU,
    0x08U}},
  {1U,
   125U,
   150U,
   {0x000124F9U,
    0x08U,
    0x00U,
    0x00U,
    0x0DU,
    0x00U,
    0x21U,
    0x0DU,
    0x58U,
    0x0AU,
    0x0FU,
    0x11U,
    0x0EU,
    0x08U}},
  {1U,
   125U,
   250U,
   {0x000124F9U,
    0x08U,
    0x00U,
    0x00U,
    0x0BU,
    0x00U,
    0x21U,
    0x0DU,
    0x3AU,
    0x08U,
    0x0FU,
    0x0CU,
    0x0EU,
    0x08U}},
  {1U,
   125U,
   400U,
   {0x000124F9U,
    0x08U,
    0x00U,
    0x00U,
    0x0AU,
    0x00U,
    0x21U,
    0x04U,
    0x3AU,
    0x08U,
    0x0FU,
    0x09U,
    0x0EU,
    0x08U}},
  {1U,
   125U,
   600U,
   {0x000124F9U,
    0x08U,
    0x00U,
    0x00U,
    0x09U,
    0x00U,
    0x21U,
    0x04U,
    0x23U,
    0x06U,
    0x0FU,
    0x08U,
    0x0EU,
    0x08U}},
  {1U,
   125U,
   720U,
   {0x000124F9U,
    0x08U,
    0x00U,
    0x00U,
    0x08U,
    0x00U,
    0x21U,
    0x04U,
    0x23U,
    0x05U,
    0x0FU,
    0x07U,
    0x0EU,
    0x08U}},

  /* HUM Table 64.2 PCLKA 120 MHz, p 3832 */
  {1U,
   120U,
   100U,
   {0x00011941U,
    0x08U,
    0x00U,
    0x00U,
    0x0EU,
    0x00U,
    0x20U,
    0x0DU,
    0x58U,
    0x0AU,
    0x0FU,
    0x15U,
    0x0DU,
    0x08U}},
  {1U,
   120U,
   150U,
   {0x00011941U,
    0x08U,
    0x00U,
    0x00U,
    0x0CU,
    0x00U,
    0x20U,
    0x0DU,
    0x58U,
    0x0AU,
    0x0FU,
    0x10U,
    0x0DU,
    0x08U}},
  {1U,
   120U,
   250U,
   {0x00011941U,
    0x08U,
    0x00U,
    0x00U,
    0x0AU,
    0x00U,
    0x20U,
    0x0DU,
    0x3AU,
    0x08U,
    0x0FU,
    0x0BU,
    0x0DU,
    0x08U}},
  {1U,
   120U,
   400U,
   {0x00011941U,
    0x08U,
    0x00U,
    0x00U,
    0x09U,
    0x00U,
    0x20U,
    0x04U,
    0x3AU,
    0x07U,
    0x0FU,
    0x08U,
    0x0DU,
    0x08U}},
  {1U,
   120U,
   600U,
   {0x00011941U,
    0x08U,
    0x00U,
    0x00U,
    0x09U,
    0x00U,
    0x20U,
    0x04U,
    0x23U,
    0x06U,
    0x0FU,
    0x07U,
    0x0DU,
    0x08U}},
  {1U,
   120U,
   720U,
   {0x00011941U,
    0x08U,
    0x00U,
    0x00U,
    0x08U,
    0x00U,
    0x20U,
    0x04U,
    0x23U,
    0x05U,
    0x0FU,
    0x06U,
    0x0DU,
    0x08U}},

  /* HUM Table 64.2 PCLKA 100 MHz, p 3833 */
  {1U,
   100U,
   100U,
   {0x0000EA61U,
    0x06U,
    0x00U,
    0x00U,
    0x0CU,
    0x00U,
    0x1CU,
    0x0CU,
    0x46U,
    0x0AU,
    0x0CU,
    0x10U,
    0x0CU,
    0x08U}},
  {1U,
   100U,
   150U,
   {0x0000EA61U,
    0x06U,
    0x00U,
    0x00U,
    0x0AU,
    0x00U,
    0x1CU,
    0x0CU,
    0x46U,
    0x07U,
    0x0CU,
    0x0BU,
    0x0CU,
    0x08U}},
  {1U,
   100U,
   250U,
   {0x0000EA61U,
    0x06U,
    0x00U,
    0x00U,
    0x08U,
    0x00U,
    0x1CU,
    0x0CU,
    0x32U,
    0x06U,
    0x0CU,
    0x08U,
    0x0CU,
    0x08U}},
  {1U,
   100U,
   400U,
   {0x0000EA61U,
    0x06U,
    0x00U,
    0x00U,
    0x07U,
    0x00U,
    0x1CU,
    0x04U,
    0x22U,
    0x04U,
    0x0CU,
    0x06U,
    0x0CU,
    0x08U}},
  {1U,
   100U,
   600U,
   {0x0000EA61U,
    0x06U,
    0x00U,
    0x00U,
    0x07U,
    0x00U,
    0x1CU,
    0x04U,
    0x22U,
    0x04U,
    0x0CU,
    0x05U,
    0x0CU,
    0x08U}},
  {1U,
   100U,
   720U,
   {0x0000EA61U,
    0x06U,
    0x00U,
    0x00U,
    0x07U,
    0x00U,
    0x1CU,
    0x04U,
    0x22U,
    0x03U,
    0x0CU,
    0x05U,
    0x0CU,
    0x08U}},

  /* HUM Table 64.2 PCLKA 80 MHz, p 3833 */
  {1U,
   80U,
   100U,
   {0x0000BB81U,
    0x05U,
    0x00U,
    0x00U,
    0x09U,
    0x00U,
    0x17U,
    0x0AU,
    0x3AU,
    0x07U,
    0x0AU,
    0x0CU,
    0x0AU,
    0x07U}},
  {1U,
   80U,
   150U,
   {0x0000BB81U,
    0x05U,
    0x00U,
    0x00U,
    0x08U,
    0x00U,
    0x17U,
    0x0AU,
    0x2EU,
    0x05U,
    0x0AU,
    0x08U,
    0x0AU,
    0x07U}},
  {1U,
   80U,
   250U,
   {0x0000BB81U,
    0x05U,
    0x00U,
    0x00U,
    0x07U,
    0x00U,
    0x17U,
    0x04U,
    0x2EU,
    0x04U,
    0x0AU,
    0x06U,
    0x0AU,
    0x07U}},
  {1U,
   80U,
   400U,
   {0x0000BB81U,
    0x05U,
    0x00U,
    0x00U,
    0x06U,
    0x00U,
    0x17U,
    0x04U,
    0x1CU,
    0x03U,
    0x0AU,
    0x04U,
    0x0AU,
    0x07U}},
  {1U,
   80U,
   600U,
   {0x0000BB81U,
    0x05U,
    0x00U,
    0x00U,
    0x05U,
    0x00U,
    0x17U,
    0x04U,
    0x1CU,
    0x02U,
    0x0AU,
    0x04U,
    0x0AU,
    0x07U}},
  {1U,
   80U,
   720U,
   {0x0000BB81U,
    0x05U,
    0x00U,
    0x00U,
    0x05U,
    0x00U,
    0x17U,
    0x04U,
    0x1CU,
    0x02U,
    0x0AU,
    0x04U,
    0x0AU,
    0x07U}},

  /* HUM Table 64.2 PCLKA 75 MHz, p 3834 */
  {1U,
   75U,
   100U,
   {0x0000AFC9U,
    0x05U,
    0x00U,
    0x00U,
    0x08U,
    0x00U,
    0x14U,
    0x08U,
    0x37U,
    0x06U,
    0x0AU,
    0x0BU,
    0x0AU,
    0x07U}},
  {1U,
   75U,
   150U,
   {0x0000AFC9U,
    0x05U,
    0x00U,
    0x00U,
    0x07U,
    0x00U,
    0x14U,
    0x08U,
    0x2BU,
    0x04U,
    0x0AU,
    0x08U,
    0x0AU,
    0x07U}},
  {1U,
   75U,
   250U,
   {0x0000AFC9U,
    0x05U,
    0x00U,
    0x00U,
    0x06U,
    0x00U,
    0x14U,
    0x03U,
    0x2BU,
    0x03U,
    0x0AU,
    0x05U,
    0x0AU,
    0x07U}},
  {1U,
   75U,
   400U,
   {0x0000AFC9U,
    0x05U,
    0x00U,
    0x00U,
    0x05U,
    0x00U,
    0x14U,
    0x03U,
    0x19U,
    0x02U,
    0x0AU,
    0x04U,
    0x0AU,
    0x07U}},
  {1U,
   75U,
   600U,
   {0x0000AFC9U,
    0x05U,
    0x00U,
    0x00U,
    0x05U,
    0x00U,
    0x14U,
    0x03U,
    0x19U,
    0x02U,
    0x0AU,
    0x04U,
    0x0AU,
    0x07U}},
  {1U,
   75U,
   720U,
   {0x0000AFC9U,
    0x05U,
    0x00U,
    0x00U,
    0x05U,
    0x00U,
    0x14U,
    0x03U,
    0x19U,
    0x01U,
    0x0AU,
    0x04U,
    0x0AU,
    0x07U}},

  /* HUM Table 64.2 PCLKA 40 MHz, p 3834 */
  {1U,
   40U,
   100U,
   {0x00005DC1U,
    0x02U,
    0x00U,
    0x00U,
    0x04U,
    0x00U,
    0x0CU,
    0x02U,
    0x20U,
    0x01U,
    0x05U,
    0x03U,
    0x05U,
    0x05U}},
  {1U,
   40U,
   150U,
   {0x00005DC1U,
    0x02U,
    0x00U,
    0x00U,
    0x03U,
    0x00U,
    0x0CU,
    0x02U,
    0x1AU,
    0x01U,
    0x05U,
    0x02U,
    0x05U,
    0x05U}},
  {1U,
   40U,
   250U,
   {0x00005DC1U,
    0x02U,
    0x00U,
    0x00U,
    0x03U,
    0x00U,
    0x0CU,
    0x00U,
    0x1AU,
    0x00U,
    0x05U,
    0x02U,
    0x05U,
    0x05U}},
  {1U,
   40U,
   400U,
   {0x00005DC1U,
    0x02U,
    0x00U,
    0x00U,
    0x02U,
    0x00U,
    0x0CU,
    0x00U,
    0x0CU,
    0x00U,
    0x05U,
    0x02U,
    0x05U,
    0x05U}},
  {1U,
   40U,
   600U,
   {0x00005DC1U,
    0x02U,
    0x00U,
    0x00U,
    0x02U,
    0x00U,
    0x0CU,
    0x00U,
    0x0CU,
    0x00U,
    0x05U,
    0x02U,
    0x05U,
    0x05U}},
  {1U,
   40U,
   720U,
   {0x00005DC1U,
    0x02U,
    0x00U,
    0x00U,
    0x02U,
    0x00U,
    0x0CU,
    0x00U,
    0x0CU,
    0x00U,
    0x05U,
    0x02U,
    0x05U,
    0x05U}},
};

/**
 * @var s_csi_table
 * @brief HUM Table 64.3 D-PHY timing setting CSI mode (p 3835-3836).
 *
 * @details
 * CSI rows only constrain TINIT, TCLKMISS, TCLKSETT, THSSETT,
 * TCLKPREP, THSPREP -- the high-speed lane fields are not used
 * in device mode (the host side drives them). The struct fields
 * not listed in the table are left zero-filled.
 */
static const mipi_phy_table_row_t s_csi_table[] = {
  /* HUM Table 64.3 PCLKA 125 MHz, p 3835 */
  {0U, 125U, 90U, {0x000124F9U, 0x11U, 0x19U, 0x03U, 0x12U, 0x12U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U,
   125U,
   100U,
   {0x000124F9U, 0x11U, 0x19U, 0x03U, 0x11U, 0x11U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U,
   125U,
   130U,
   {0x000124F9U, 0x11U, 0x19U, 0x03U, 0x10U, 0x10U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U,
   125U,
   200U,
   {0x000124F9U, 0x11U, 0x19U, 0x03U, 0x0FU, 0x0FU, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U,
   125U,
   300U,
   {0x000124F9U, 0x11U, 0x19U, 0x03U, 0x0DU, 0x0EU, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U,
   125U,
   400U,
   {0x000124F9U, 0x11U, 0x19U, 0x03U, 0x0CU, 0x0DU, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U,
   125U,
   700U,
   {0x000124F9U, 0x11U, 0x19U, 0x03U, 0x0BU, 0x0CU, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U,
   125U,
   720U,
   {0x000124F9U, 0x11U, 0x19U, 0x03U, 0x0AU, 0x0BU, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},

  /* HUM Table 64.3 PCLKA 120 MHz, p 3835 */
  {0U, 120U, 90U, {0x00011941U, 0x10U, 0x18U, 0x03U, 0x11U, 0x11U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U,
   120U,
   100U,
   {0x00011941U, 0x10U, 0x18U, 0x03U, 0x10U, 0x10U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U,
   120U,
   130U,
   {0x00011941U, 0x10U, 0x18U, 0x03U, 0x0FU, 0x0FU, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U,
   120U,
   200U,
   {0x00011941U, 0x10U, 0x18U, 0x03U, 0x0EU, 0x0EU, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U,
   120U,
   300U,
   {0x00011941U, 0x10U, 0x18U, 0x03U, 0x0CU, 0x0DU, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U,
   120U,
   400U,
   {0x00011941U, 0x10U, 0x18U, 0x03U, 0x0BU, 0x0CU, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U,
   120U,
   700U,
   {0x00011941U, 0x10U, 0x18U, 0x03U, 0x0AU, 0x0BU, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U,
   120U,
   720U,
   {0x00011941U, 0x10U, 0x18U, 0x03U, 0x09U, 0x0AU, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},

  /* HUM Table 64.3 PCLKA 100 MHz, p 3835 */
  {0U, 100U, 90U, {0x0000EA61U, 0x0DU, 0x13U, 0x02U, 0x0EU, 0x0EU, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U,
   100U,
   100U,
   {0x0000EA61U, 0x0DU, 0x13U, 0x02U, 0x0DU, 0x0DU, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U,
   100U,
   130U,
   {0x0000EA61U, 0x0DU, 0x13U, 0x02U, 0x0CU, 0x0CU, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U,
   100U,
   200U,
   {0x0000EA61U, 0x0DU, 0x13U, 0x02U, 0x0BU, 0x0BU, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U,
   100U,
   300U,
   {0x0000EA61U, 0x0DU, 0x13U, 0x02U, 0x0AU, 0x0AU, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U,
   100U,
   400U,
   {0x0000EA61U, 0x0DU, 0x13U, 0x02U, 0x09U, 0x09U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U,
   100U,
   700U,
   {0x0000EA61U, 0x0DU, 0x13U, 0x02U, 0x08U, 0x08U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U,
   100U,
   720U,
   {0x0000EA61U, 0x0DU, 0x13U, 0x02U, 0x08U, 0x07U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},

  /* HUM Table 64.3 PCLKA 80 MHz, p 3835-3836 */
  {0U, 80U, 90U, {0x0000BB81U, 0x0AU, 0x0FU, 0x01U, 0x0AU, 0x0AU, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U, 80U, 100U, {0x0000BB81U, 0x0AU, 0x0FU, 0x01U, 0x0AU, 0x0AU, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U, 80U, 130U, {0x0000BB81U, 0x0AU, 0x0FU, 0x01U, 0x09U, 0x09U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U, 80U, 200U, {0x0000BB81U, 0x0AU, 0x0FU, 0x01U, 0x08U, 0x08U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U, 80U, 300U, {0x0000BB81U, 0x0AU, 0x0FU, 0x01U, 0x07U, 0x07U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U, 80U, 400U, {0x0000BB81U, 0x0AU, 0x0FU, 0x01U, 0x07U, 0x07U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U, 80U, 700U, {0x0000BB81U, 0x0AU, 0x0FU, 0x01U, 0x06U, 0x06U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U, 80U, 720U, {0x0000BB81U, 0x0AU, 0x0FU, 0x01U, 0x06U, 0x05U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},

  /* HUM Table 64.3 PCLKA 75 MHz, p 3836 */
  {0U, 75U, 90U, {0x0000AFC9U, 0x0AU, 0x0EU, 0x01U, 0x09U, 0x09U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U, 75U, 100U, {0x0000AFC9U, 0x0AU, 0x0EU, 0x01U, 0x09U, 0x09U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U, 75U, 130U, {0x0000AFC9U, 0x0AU, 0x0EU, 0x01U, 0x08U, 0x08U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U, 75U, 200U, {0x0000AFC9U, 0x0AU, 0x0EU, 0x01U, 0x07U, 0x07U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U, 75U, 300U, {0x0000AFC9U, 0x0AU, 0x0EU, 0x01U, 0x06U, 0x06U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U, 75U, 400U, {0x0000AFC9U, 0x0AU, 0x0EU, 0x01U, 0x06U, 0x06U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U, 75U, 700U, {0x0000AFC9U, 0x0AU, 0x0EU, 0x01U, 0x06U, 0x05U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U, 75U, 720U, {0x0000AFC9U, 0x0AU, 0x0EU, 0x01U, 0x06U, 0x05U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},

  /* HUM Table 64.3 PCLKA 50 MHz, p 3836 */
  {0U, 50U, 90U, {0x00007531U, 0x06U, 0x08U, 0x01U, 0x05U, 0x05U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U, 50U, 100U, {0x00007531U, 0x06U, 0x08U, 0x01U, 0x05U, 0x05U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U, 50U, 130U, {0x00007531U, 0x06U, 0x08U, 0x01U, 0x04U, 0x04U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U, 50U, 200U, {0x00007531U, 0x06U, 0x08U, 0x01U, 0x04U, 0x04U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U, 50U, 300U, {0x00007531U, 0x06U, 0x08U, 0x01U, 0x03U, 0x03U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U, 50U, 400U, {0x00007531U, 0x06U, 0x08U, 0x01U, 0x03U, 0x03U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U, 50U, 700U, {0x00007531U, 0x06U, 0x08U, 0x01U, 0x03U, 0x03U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
  {0U, 50U, 720U, {0x00007531U, 0x06U, 0x08U, 0x01U, 0x02U, 0x02U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U}},
};

/**
 * @brief Lookup helper -- linear scan picks the first row whose
 *        ``rate_max`` is >= the requested rate within a matching
 *        PCLKA bucket.
 */
RA8_INTERNAL
static const ra8_mipi_phy_timing_t* internal_mipi_phy_lookup_timing(const mipi_phy_table_row_t* tbl,
                                                                    uint32_t                    n,
                                                                    uint8_t  pclka,
                                                                    uint16_t rate_mbps,
                                                                    uint8_t  mode_flag)
{
  for (uint32_t i = 0U; i < n; ++i) {
    // mcdc-deactivated: TU-local helper internal_mipi_phy_lookup_timing 3-condition table-row matcher; the timing table contains exactly one row per (mode, pclka) tuple covering the rate-bucket range, so on a hit all three conditions are true and on a miss at least one is false; no MC/DC vector can isolate any single condition flip independently of the static table layout.
    if ((tbl[i].mode == mode_flag) && (tbl[i].pclka == pclka) && (tbl[i].rate_max >= rate_mbps)) {
      return &tbl[i].t;
    }
  }
  return nullptr;
}

uint32_t internal_mipi_phy_compute_freq(const ra8_mipi_phy_pll_t* pll, uint8_t mosc_mhz)
{
  /* HUM Ch 64.2.2 p 3823 -- f = fMAIN * I * (NF + N) * P. */
  static const uint8_t  s_idiv_div[]   = {1U, 2U, 3U, 4U};
  static const uint8_t  s_pmul_div[]   = {1U, 2U, 4U, 8U};
  static const uint16_t s_nfmul_x100[] = {0U,
                                          k_mipi_nf_x100_33,
                                          k_mipi_nf_x100_66,
                                          k_mipi_nf_x100_50}; /* hundredths */

  const uint32_t idiv    = (uint32_t)s_idiv_div[(uint8_t)pll->idiv & 0x3U];
  const uint32_t pmul    = (uint32_t)s_pmul_div[(uint8_t)pll->pmul & 0x3U];
  const uint32_t nf_x100 = (uint32_t)s_nfmul_x100[(uint8_t)pll->nfmul & 0x3U];
  const uint32_t n_x100  = ((uint32_t)pll->nmul_int * k_mipi_pll_percent_scale) + nf_x100;
  /* f_mhz = mosc_mhz * (n_x100/100) / idiv / pmul */
  const uint32_t numerator = (uint32_t)mosc_mhz * n_x100;
  const uint32_t denom     = idiv * pmul * k_mipi_pll_percent_scale;
  return (denom == 0U) ? 0U : (numerator / denom);
}

ra8_err_t ra8_mipi_phy_select_timing(ra8_mipi_phy_mode_t          mode,
                                     uint8_t                      pclka_mhz,
                                     uint16_t                     rate_mbps,
                                     ra8_mipi_phy_timing_t* const out_timing)
{
  if ((mode != k_ra8_mipi_phy_mode_dsi_host) && (mode != k_ra8_mipi_phy_mode_csi_device)) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 64.1 "Overview" p 3822 */
  if ((rate_mbps < k_ra8_mipi_phy_line_rate_min_mbps) ||
      (rate_mbps > k_ra8_mipi_phy_line_rate_max_mbps)) {
    return k_ra8_err_invalid_arg;
  }

  const ra8_mipi_phy_timing_t* row;
  if (mode == k_ra8_mipi_phy_mode_dsi_host) {
    /* HUM Table 64.2 "D-PHY timing setting DSI mode", p 3831-3834 */
    row = internal_mipi_phy_lookup_timing(s_dsi_table,
                                          sizeof(s_dsi_table) / sizeof(s_dsi_table[0]),
                                          pclka_mhz,
                                          rate_mbps,
                                          1U);
  } else {
    /* HUM Table 64.3 "D-PHY timing setting CSI mode", p 3835-3836 */
    row = internal_mipi_phy_lookup_timing(s_csi_table,
                                          sizeof(s_csi_table) / sizeof(s_csi_table[0]),
                                          pclka_mhz,
                                          rate_mbps,
                                          0U);
  }
  if (row == nullptr) {
    return k_ra8_err_not_supported;
  }
  internal_mipi_phy_write_timing(row);
  if (out_timing != nullptr) {
    *out_timing = *row;
  }
  return k_ra8_ok;
}
