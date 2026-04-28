/**
 * @file ra_mipi_phy.c
 * @brief MIPI D-PHY driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Full HUM Ch 64 coverage (p 3822-3838). Every documented register
 * field has a corresponding code path; every operating mode is
 * reachable through the public API; every IRQ source is decoded by
 * the dispatcher. The PHY is consumed by ``ra_mipi_dsi`` (master)
 * and ``ra_mipi_csi`` (slave); this driver does NOT touch either of
 * those files.
 *
 * Driver scope:
 *
 *  - Lifecycle: init / deinit / reset / recover_from_error /
 *    enter_stop / exit_stop.
 *  - Power: ldo_enable / ldo_disable / pll_start / pll_stop.
 *  - Mode: switch_mode (DSI <-> CSI), set_clock_mode (continuous /
 *    non-continuous HS clock), set_eotp.
 *  - Lanes: set_lane_count (1/2 supported, 3/4 rejected),
 *    set_lane_enable per lane.
 *  - PLL re-tune: set_lane_speed, validate_pll_band.
 *  - Reference / escape: set_pclka_freq, set_escape_divisor.
 *  - Timing tables: select_timing -- HUM Tables 64.2 (DSI) /
 *    64.3 (CSI) p 3831-3836 are encoded as a static lookup table.
 *  - Status / IRQ: get_status / clear_status / is_pll_locked /
 *    is_ldo_stable / wait_ready / attach_handler / dispatch.
 *
 * The dispatcher tracks the previous DPHYSFR snapshot so it can
 * decode every documented status edge (PWRSF / PLLSF rising and
 * falling) into a typed ``ra_mipi_phy_event_t`` rather than handing
 * the consumer a raw register dump.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_mipi_phy.h"

#include <stdint.h>

#include "ra8d2_mipi_phy_regs.h"
#include "ra8d2_mstp_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

/**
 * @var s_tag
 * @brief Log tag for ``ra_log_*`` calls in this driver.
 */
static const char* s_tag = "MIPI_PHY";

/**
 * @enum ra_mipi_phy_limits_t
 * @brief Validation guard rails for ``ra_mipi_phy_init``.
 *
 * @details
 * HUM Ch 64.2.1 p 3822: RFREQ accepts 0x27 (40 MHz) .. 0x7C
 * (125 MHz). HUM Ch 64.2.4 p 3825: ESCDIV[4:0] is a 5-bit field.
 * HUM Ch 64.2.2 p 3823: NMUL[8:0] valid range is 40 .. 375.
 */
typedef enum : uint16_t {
  k_ra_mipi_phy_pclka_min_mhz = 40U,   /**< Lower bound on RFREQ value. */
  k_ra_mipi_phy_pclka_max_mhz = 125U,  /**< Upper bound on RFREQ value. */
  k_ra_mipi_phy_escdiv_max    = 31U,   /**< 5-bit field width.          */
  k_ra_mipi_phy_nmul_min      = 40U,   /**< NMUL_int floor.             */
  k_ra_mipi_phy_nmul_max      = 375U,  /**< NMUL_int ceiling.           */
  k_ra_mipi_phy_spin_budget   = 4096U, /**< Max poll iterations.        */
} ra_mipi_phy_limits_t;

/**
 * @enum ra_mipi_phy_mstpc_bit_t
 * @brief Direct-write fallback for the MIPI PHY module-stop bit.
 *
 * @details
 * The shared ``ra_mstp_t`` enum in ``libs/ra_hal/inc/ra8d2_mstp_regs.h``
 * does NOT yet have a ``k_ra_mipi_phy`` entry. Per the agent brief
 * we must not extend that file from this driver. As a stop-gap, the
 * driver clears MSTPCRC bit 13 (the MIPI PHY slot in MSTPCRC -- HUM
 * Ch 64.4.2 p 3838 references MSTPCRC for the block) directly.
 *
 * TODO: When ``ra8d2_mstp_regs.h`` gains an explicit ``k_ra_mstp_mipi_phy``
 * value (driven by HUM Ch 11.2.8 "MSTPCRC" p 446-447), replace the
 * direct register write below with ``ra_mstp_enable(k_ra_mstp_mipi_phy)``.
 */
typedef enum : uint8_t {
  k_ra_mipi_phy_mstpc_bit = 13U, /**< Provisional MSTPC slot. */
} ra_mipi_phy_mstpc_bit_t;

/**
 * @enum ra_mipi_phy_lane_bit_t
 * @brief Software-lane bitmap positions for ``s_lane_enable_mask``.
 */
typedef enum : uint8_t {
  k_ra_mipi_phy_lane_bit_clk = 0U, /**< bit 0: clock lane.  */
  k_ra_mipi_phy_lane_bit_d0  = 1U, /**< bit 1: data lane 0. */
  k_ra_mipi_phy_lane_bit_d1  = 2U, /**< bit 2: data lane 1. */
} ra_mipi_phy_lane_bit_t;

/**
 * @enum ra_mipi_phy_lane_mask_t
 * @brief Default lane-enable bitmap (clock + d0 + d1 all on).
 */
typedef enum : uint8_t {
  k_ra_mipi_phy_lane_mask_default = (uint8_t)((1U << (uint8_t)k_ra_mipi_phy_lane_bit_clk) |
                                              (1U << (uint8_t)k_ra_mipi_phy_lane_bit_d0) |
                                              (1U << (uint8_t)k_ra_mipi_phy_lane_bit_d1)),
} ra_mipi_phy_lane_mask_t;

/* =============================================================================
 * Driver state (module-private)
 * =============================================================================
 */

/**
 * @var s_mipi_phy_fn
 * @brief Registered callback for ``ra_mipi_phy_dispatch``.
 */
static ra_mipi_phy_event_fn_t s_mipi_phy_fn;

/**
 * @var s_mipi_phy_ctx
 * @brief Opaque context forwarded to ``s_mipi_phy_fn``.
 */
static void* s_mipi_phy_ctx;

/**
 * @var s_last_sfr
 * @brief Previous DPHYSFR snapshot, used for edge detection in dispatch.
 */
static uint32_t s_last_sfr;

/**
 * @var s_lane_count
 * @brief Last lane-count argument accepted by ``ra_mipi_phy_set_lane_count``.
 */
static ra_mipi_phy_lane_count_t s_lane_count = k_ra_mipi_phy_lane_count_2;

/**
 * @var s_lane_enable_mask
 * @brief Bitmap of enabled lanes (bit 0 = clock, bit N = data lane N-1).
 */
static uint8_t s_lane_enable_mask = (uint8_t)k_ra_mipi_phy_lane_mask_default;

/**
 * @var s_clk_mode
 * @brief HS clock-lane operating mode (continuous vs non-continuous).
 */
static ra_mipi_phy_clk_mode_t s_clk_mode = k_ra_mipi_phy_clk_noncontinuous;

/**
 * @var s_eotp
 * @brief Whether DSI host should append EoTP packets (master mode).
 */
static ra_mipi_phy_eotp_t s_eotp = k_ra_mipi_phy_eotp_disabled;

/* =============================================================================
 * Helpers (file-local)
 * =============================================================================
 */

/**
 * @brief Clear MSTPCRC bit 13, ungating MIPI PHY.
 *
 * @details
 * Stop-gap until ``ra_mstp_t`` learns the MIPI PHY slot. Cites HUM
 * Ch 64.4.2 p 3838 ("MIPI PHY operation can be disabled or enabled
 * using Module Stop Control Register C (MSTPCRC). The MIPI PHY
 * module is initially stopped after reset.").
 */
static void internal_mipi_phy_mstp_unstop(void)
{
  /* HUM Ch 64.4.2 "Module-Stop Function Setting", p 3838 */
  volatile uint32_t* mstpc = &ra_mstp()->MSTPCRC;
  *mstpc                   = *mstpc & ~((uint32_t)1U << (uint32_t)k_ra_mipi_phy_mstpc_bit);
}

/**
 * @brief Spin until ``(reg & mask) == mask`` or the budget runs out.
 */
static ra_err_t internal_mipi_phy_wait_set(volatile uint32_t* reg, uint32_t mask)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ra_mipi_phy_spin_budget; ++i) {
    if ((*reg & mask) == mask) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Pack a validated PLL config into the DPHYPLFCR register layout.
 */
static uint32_t internal_mipi_phy_pack_plfcr(const ra_mipi_phy_pll_t* pll)
{
  uint32_t v = 0U;
  v |= ((uint32_t)pll->idiv & (uint32_t)k_ra_mipi_phy_plfcr_mask_idiv)
       << (uint32_t)k_ra_mipi_phy_plfcr_shift_idiv;
  v |= ((uint32_t)pll->nfmul & (uint32_t)k_ra_mipi_phy_plfcr_mask_nfmul)
       << (uint32_t)k_ra_mipi_phy_plfcr_shift_nfmul;
  v |= ((uint32_t)pll->pmul & (uint32_t)k_ra_mipi_phy_plfcr_mask_pmul)
       << (uint32_t)k_ra_mipi_phy_plfcr_shift_pmul;
  v |= ((uint32_t)pll->nmul_int & (uint32_t)k_ra_mipi_phy_plfcr_mask_nmul)
       << (uint32_t)k_ra_mipi_phy_plfcr_shift_nmul;
  return v;
}

/**
 * @brief Validate a ``ra_mipi_phy_pll_t`` against HUM 64.2.2 p 3823 limits.
 *
 * @details
 * Range-checks NMUL[8:0] only. Use ``ra_mipi_phy_validate_pll_band``
 * for the full PMUL band check.
 */
static ra_err_t internal_mipi_phy_validate_pll(const ra_mipi_phy_pll_t* pll)
{
  if ((pll->nmul_int < (uint16_t)k_ra_mipi_phy_nmul_min) ||
      (pll->nmul_int > (uint16_t)k_ra_mipi_phy_nmul_max)) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/**
 * @brief Write the six DPHYTIMx registers from the timing block.
 */
static void internal_mipi_phy_write_timing(const ra_mipi_phy_timing_t* t)
{
  /* HUM Ch 64.2.8 "DPHYTIM1 : D-PHY Timing Control Register 1", p 3827 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_tim1) =
    (uint32_t)t->tinit & (uint32_t)k_ra_mipi_phy_tim1_tinit_mask;

  /* HUM Ch 64.2.9 "DPHYTIM2 : D-PHY Timing Control Register 2", p 3828 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_tim2) =
    (((uint32_t)t->tclkmiss & (uint32_t)k_ra_mipi_phy_tim_byte_mask)
     << (uint32_t)k_ra_mipi_phy_tim_shift_b2) |
    (((uint32_t)t->tclksett & (uint32_t)k_ra_mipi_phy_tim_byte_mask)
     << (uint32_t)k_ra_mipi_phy_tim_shift_b1) |
    (((uint32_t)t->tclkprep & (uint32_t)k_ra_mipi_phy_tim_byte_mask)
     << (uint32_t)k_ra_mipi_phy_tim_shift_b0);

  /* HUM Ch 64.2.10 "DPHYTIM3 : D-PHY Timing Control Register 3", p 3828 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_tim3) =
    (((uint32_t)t->thssett & (uint32_t)k_ra_mipi_phy_tim_byte_mask)
     << (uint32_t)k_ra_mipi_phy_tim_shift_b1) |
    (((uint32_t)t->thsprep & (uint32_t)k_ra_mipi_phy_tim_byte_mask)
     << (uint32_t)k_ra_mipi_phy_tim_shift_b0);

  /* HUM Ch 64.2.11 "DPHYTIM4 : D-PHY Timing Control Register 4", p 3829 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_tim4) =
    (((uint32_t)t->tclktrl & (uint32_t)k_ra_mipi_phy_tim_byte_mask)
     << (uint32_t)k_ra_mipi_phy_tim_shift_b3) |
    (((uint32_t)t->tclkpost & (uint32_t)k_ra_mipi_phy_tim_byte_mask)
     << (uint32_t)k_ra_mipi_phy_tim_shift_b2) |
    (((uint32_t)t->tclkpre & (uint32_t)k_ra_mipi_phy_tim_byte_mask)
     << (uint32_t)k_ra_mipi_phy_tim_shift_b1) |
    (((uint32_t)t->tclkzero & (uint32_t)k_ra_mipi_phy_tim_byte_mask)
     << (uint32_t)k_ra_mipi_phy_tim_shift_b0);

  /* HUM Ch 64.2.12 "DPHYTIM5 : D-PHY Timing Control Register 5", p 3830 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_tim5) =
    (((uint32_t)t->thsexit & (uint32_t)k_ra_mipi_phy_tim_byte_mask)
     << (uint32_t)k_ra_mipi_phy_tim_shift_b2) |
    (((uint32_t)t->thstrl & (uint32_t)k_ra_mipi_phy_tim_byte_mask)
     << (uint32_t)k_ra_mipi_phy_tim_shift_b1) |
    (((uint32_t)t->thszero & (uint32_t)k_ra_mipi_phy_tim_byte_mask)
     << (uint32_t)k_ra_mipi_phy_tim_shift_b0);

  /* HUM Ch 64.2.13 "DPHYTIM6 : D-PHY Timing Control Register 6", p 3830 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_tim6) =
    (uint32_t)t->tlpx & (uint32_t)k_ra_mipi_phy_tim6_tlpx_mask;
}

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
 * ``ra_mipi_phy_select_timing``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t              mode;     /**< 0 = CSI slave, 1 = DSI master. */
  uint8_t              pclka;    /**< PCLKA in MHz.                  */
  uint16_t             rate_max; /**< Ceiling of the rate column.    */
  ra_mipi_phy_timing_t t;        /**< Timing values for the row.     */
} mipi_phy_table_row_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @enum ra_mipi_phy_table_szlim_t
 * @brief Static lookup-table cardinalities.
 */
typedef enum : uint8_t {
  k_ra_mipi_phy_table_pclka_125 = 125U, /**< PCLKA 125 MHz bucket.  */
  k_ra_mipi_phy_table_pclka_120 = 120U, /**< PCLKA 120 MHz bucket.  */
  k_ra_mipi_phy_table_pclka_100 = 100U, /**< PCLKA 100 MHz bucket.  */
  k_ra_mipi_phy_table_pclka_80  = 80U,  /**< PCLKA 80  MHz bucket.  */
  k_ra_mipi_phy_table_pclka_75  = 75U,  /**< PCLKA 75  MHz bucket.  */
  k_ra_mipi_phy_table_pclka_50  = 50U,  /**< PCLKA 50  MHz bucket.  */
  k_ra_mipi_phy_table_pclka_40  = 40U,  /**< PCLKA 40  MHz bucket.  */
} ra_mipi_phy_table_szlim_t;

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
 * in slave mode (the master side drives them). The struct fields
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
static const ra_mipi_phy_timing_t* internal_mipi_phy_lookup_timing(const mipi_phy_table_row_t* tbl,
                                                                   uint32_t                    n,
                                                                   uint8_t  pclka,
                                                                   uint16_t rate_mbps,
                                                                   uint8_t  mode_flag)
{
  for (uint32_t i = 0U; i < n; ++i) {
    if ((tbl[i].mode == mode_flag) && (tbl[i].pclka == pclka) && (tbl[i].rate_max >= rate_mbps)) {
      return &tbl[i].t;
    }
  }
  return nullptr;
}

/**
 * @brief Compute the PLL output frequency for a (mosc, pll) tuple.
 */
static uint32_t internal_mipi_phy_compute_freq(const ra_mipi_phy_pll_t* pll, uint8_t mosc_mhz)
{
  /* HUM Ch 64.2.2 p 3823 -- f = fMAIN * I * (NF + N) * P. */
  static const uint8_t  s_idiv_div[]   = {1U, 2U, 3U, 4U};
  static const uint8_t  s_pmul_div[]   = {1U, 2U, 4U, 8U};
  static const uint16_t s_nfmul_x100[] = {0U, 33U, 66U, 50U}; /* hundredths */

  const uint32_t idiv    = (uint32_t)s_idiv_div[(uint8_t)pll->idiv & 0x3U];
  const uint32_t pmul    = (uint32_t)s_pmul_div[(uint8_t)pll->pmul & 0x3U];
  const uint32_t nf_x100 = (uint32_t)s_nfmul_x100[(uint8_t)pll->nfmul & 0x3U];
  const uint32_t n_x100  = ((uint32_t)pll->nmul_int * 100U) + nf_x100;
  /* f_mhz = mosc_mhz * (n_x100/100) / idiv / pmul */
  const uint32_t numerator = (uint32_t)mosc_mhz * n_x100;
  const uint32_t denom     = idiv * pmul * 100U;
  return (denom == 0U) ? 0U : (numerator / denom);
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra_err_t ra_mipi_phy_init(const ra_mipi_phy_config_t* cfg)
{
  RA_CHECK_NULL_PTR((const void*)cfg, s_tag, "cfg must not be nullptr");
  RA_CHECK_NULL_PTR((const void*)cfg->p_timing, s_tag, "cfg->p_timing must not be nullptr");

  /* HUM Ch 64.2.1 "DPHYREFCR : D-PHY Reference Clock Setting Register", p 3822 */
  if ((cfg->pclka_mhz < (uint8_t)k_ra_mipi_phy_pclka_min_mhz) ||
      (cfg->pclka_mhz > (uint8_t)k_ra_mipi_phy_pclka_max_mhz)) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 64.2.4 "DPHYESCCR : D-PHY Escape Mode Clock Control Register", p 3825 */
  if (cfg->escdiv > (uint8_t)k_ra_mipi_phy_escdiv_max) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 64.1 "Overview" p 3822 */
  if ((cfg->lane_count != k_ra_mipi_phy_lane_count_1) &&
      (cfg->lane_count != k_ra_mipi_phy_lane_count_2)) {
    return ((cfg->lane_count == k_ra_mipi_phy_lane_count_3) ||
            (cfg->lane_count == k_ra_mipi_phy_lane_count_4))
             ? k_ra_err_not_supported
             : k_ra_err_invalid_arg;
  }

  if (cfg->mode == k_ra_mipi_phy_mode_dsi_master) {
    const ra_err_t pll_err = internal_mipi_phy_validate_pll(&cfg->pll);
    RA_RETURN_ON_ERROR(pll_err, s_tag, "init: pll out of range");
  }

  /* Step 1 -- HUM Ch 64.4.2 "Module-Stop Function Setting", p 3838 */
  internal_mipi_phy_mstp_unstop();

  /* Step 2 -- HUM Ch 64.2.14 "DPHYMDC : D-PHY Mode Control Register", p 3836 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_mdc) =
    (cfg->mode == k_ra_mipi_phy_mode_dsi_master) ? (uint32_t)k_ra_mipi_phy_mdc_masteren : 0U;

  /* Step 3 -- HUM Ch 64.2.1 "DPHYREFCR : D-PHY Reference Clock Setting Register", p 3822 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_refcr) =
    (uint32_t)cfg->pclka_mhz & (uint32_t)k_ra_mipi_phy_refcr_rfreq_mask;

  /* Step 4 -- HUM Ch 64.2.5 "DPHYPWRCR : D-PHY Power Supplying Control Register", p 3826 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_pwrcr) = (uint32_t)k_ra_mipi_phy_pwrcr_pwrsen;

  /* Step 5 -- HUM Ch 64.2.6 "DPHYSFR : D-PHY Status Flag Register", p 3826 */
  const ra_err_t pwr_err = internal_mipi_phy_wait_set(ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr),
                                                      (uint32_t)k_ra_mipi_phy_sfr_pwrsf);
  RA_RETURN_ON_ERROR(pwr_err, s_tag, "init: LDO did not stabilise");

  if (cfg->mode == k_ra_mipi_phy_mode_dsi_master) {
    /* Step 6 -- HUM Ch 64.2.2 "DPHYPLFCR : D-PHY PLL Frequency Control Register", p 3823 */
    *ra_mipi_phy_reg32(k_ra_mipi_phy_off_plfcr) = internal_mipi_phy_pack_plfcr(&cfg->pll);

    /* Step 7 -- HUM Ch 64.2.4 "DPHYESCCR : D-PHY Escape Mode Clock Control Register", p 3825 */
    *ra_mipi_phy_reg32(k_ra_mipi_phy_off_esccr) =
      (uint32_t)cfg->escdiv & (uint32_t)k_ra_mipi_phy_esccr_escdiv_mask;

    /* Step 8 -- HUM Ch 64.2.3 "DPHYPLOCR : D-PHY PLL Operation Control Register", p 3824 */
    *ra_mipi_phy_reg32(k_ra_mipi_phy_off_plocr) = 0U;

    /* Step 9 -- HUM Ch 64.2.6 "DPHYSFR : D-PHY Status Flag Register", p 3826 */
    const ra_err_t pll_err = internal_mipi_phy_wait_set(ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr),
                                                        (uint32_t)k_ra_mipi_phy_sfr_pllsf);
    RA_RETURN_ON_ERROR(pll_err, s_tag, "init: PLL did not lock");
  }

  /* Step 10 -- HUM Ch 64.2.8 "DPHYTIM1 : D-PHY Timing Control Register 1", p 3827 */
  internal_mipi_phy_write_timing(cfg->p_timing);

  /* Step 11 -- HUM Ch 64.2.7 "DPHYOCR : D-PHY Operation Control Register", p 3827 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_ocr) = (uint32_t)k_ra_mipi_phy_ocr_dphyen;

  /* Cache the optional state knobs the driver tracks for the higher
   * level. None of these are stored in PHY registers; the DSI / CSI
   * driver consults them after init. */
  s_lane_count       = cfg->lane_count;
  s_lane_enable_mask = (uint8_t)k_ra_mipi_phy_lane_mask_default;
  s_clk_mode         = cfg->clk_mode;
  s_eotp             = cfg->eotp;
  s_last_sfr         = *ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr);

  ra_log_info(s_tag, "mipi_phy_init done");
  return k_ra_ok;
}

ra_err_t ra_mipi_phy_deinit(void)
{
  /* Step 1 -- HUM Ch 64.2.7 "DPHYOCR : D-PHY Operation Control Register", p 3827 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_ocr) = 0U;

  /* Step 2 -- HUM Ch 64.2.3 "DPHYPLOCR : D-PHY PLL Operation Control Register", p 3824 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_plocr) = (uint32_t)k_ra_mipi_phy_plocr_pllstp;

  /* Step 3 -- HUM Ch 64.2.5 "DPHYPWRCR : D-PHY Power Supplying Control Register", p 3826 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_pwrcr) = 0U;

  ra_log_info(s_tag, "mipi_phy_deinit done");
  return k_ra_ok;
}

ra_err_t ra_mipi_phy_reset(void)
{
  /* HUM Ch 64.2.7 "DPHYOCR : D-PHY Operation Control Register", p 3827 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_ocr) = 0U;
  /* HUM Ch 64.2.3 "DPHYPLOCR : D-PHY PLL Operation Control Register", p 3824 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_plocr) = (uint32_t)k_ra_mipi_phy_plocr_pllstp;
  /* HUM Ch 64.2.5 "DPHYPWRCR : D-PHY Power Supplying Control Register", p 3826 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_pwrcr) = 0U;
  /* HUM Ch 64.2.2 "DPHYPLFCR : D-PHY PLL Frequency Control Register", p 3823 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_plfcr) = 0U;
  /* HUM Ch 64.2.4 "DPHYESCCR : D-PHY Escape Mode Clock Control Register", p 3825 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_esccr) = 0U;
  /* HUM Ch 64.2.1 "DPHYREFCR : D-PHY Reference Clock Setting Register", p 3822 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_refcr) = 0U;
  /* HUM Ch 64.2.14 "DPHYMDC : D-PHY Mode Control Register", p 3836 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_mdc) = 0U;
  /* HUM Ch 64.2.8 "DPHYTIM1 : D-PHY Timing Control Register 1", p 3827 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_tim1) = 0U;
  /* HUM Ch 64.2.9 "DPHYTIM2 : D-PHY Timing Control Register 2", p 3828 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_tim2) = 0U;
  /* HUM Ch 64.2.10 "DPHYTIM3 : D-PHY Timing Control Register 3", p 3828 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_tim3) = 0U;
  /* HUM Ch 64.2.11 "DPHYTIM4 : D-PHY Timing Control Register 4", p 3829 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_tim4) = 0U;
  /* HUM Ch 64.2.12 "DPHYTIM5 : D-PHY Timing Control Register 5", p 3830 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_tim5) = 0U;
  /* HUM Ch 64.2.13 "DPHYTIM6 : D-PHY Timing Control Register 6", p 3830 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_tim6) = 0U;
  s_last_sfr                                 = 0U;
  return k_ra_ok;
}

ra_err_t ra_mipi_phy_recover_from_error(const ra_mipi_phy_config_t* cfg)
{
  RA_CHECK_NULL_PTR((const void*)cfg, s_tag, "recover: cfg must not be nullptr");
  const ra_err_t r_err = ra_mipi_phy_reset();
  RA_RETURN_ON_ERROR(r_err, s_tag, "recover: reset failed");
  return ra_mipi_phy_init(cfg);
}

ra_err_t ra_mipi_phy_get_status(uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");

  /* HUM Ch 64.2.6 "DPHYSFR : D-PHY Status Flag Register", p 3826 */
  *out_mask = *ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr);
  return k_ra_ok;
}

ra_err_t ra_mipi_phy_clear_status(uint32_t mask)
{
  /* DPHYSFR is read-only on this part (HUM Ch 64.2.6 p 3826) -- nothing to write. */
  (void)mask;
  return k_ra_ok;
}

bool ra_mipi_phy_is_ldo_stable(void)
{
  /* HUM Ch 64.2.6 "DPHYSFR : D-PHY Status Flag Register", p 3826 */
  return (*ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr) & (uint32_t)k_ra_mipi_phy_sfr_pwrsf) != 0U;
}

bool ra_mipi_phy_is_pll_locked(void)
{
  /* HUM Ch 64.2.6 "DPHYSFR : D-PHY Status Flag Register", p 3826 */
  return (*ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr) & (uint32_t)k_ra_mipi_phy_sfr_pllsf) != 0U;
}

ra_err_t ra_mipi_phy_wait_ready(void)
{
  /* HUM Ch 64.2.6 "DPHYSFR : D-PHY Status Flag Register", p 3826 */
  return internal_mipi_phy_wait_set(ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr),
                                    (uint32_t)k_ra_mipi_phy_sfr_ready_mask);
}

ra_err_t ra_mipi_phy_attach_handler(ra_mipi_phy_event_fn_t fn, void* ctx)
{
  s_mipi_phy_fn  = fn;
  s_mipi_phy_ctx = ctx;
  return k_ra_ok;
}

void ra_mipi_phy_dispatch(void)
{
  /* HUM Ch 64.2.6 "DPHYSFR : D-PHY Status Flag Register", p 3826 */
  const uint32_t sfr               = *ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr);
  const uint32_t prev              = s_last_sfr;
  s_last_sfr                       = sfr;
  const ra_mipi_phy_event_fn_t fn  = s_mipi_phy_fn;
  void* const                  ctx = s_mipi_phy_ctx;

  if (fn == nullptr) {
    return;
  }

  const uint32_t pwr_now  = sfr & (uint32_t)k_ra_mipi_phy_sfr_pwrsf;
  const uint32_t pwr_prev = prev & (uint32_t)k_ra_mipi_phy_sfr_pwrsf;
  const uint32_t pll_now  = sfr & (uint32_t)k_ra_mipi_phy_sfr_pllsf;
  const uint32_t pll_prev = prev & (uint32_t)k_ra_mipi_phy_sfr_pllsf;
  bool           emitted  = false;

  if ((pwr_now != 0U) && (pwr_prev == 0U)) {
    fn(ctx, k_ra_mipi_phy_event_ldo_ready, sfr);
    emitted = true;
  } else if ((pwr_now == 0U) && (pwr_prev != 0U)) {
    fn(ctx, k_ra_mipi_phy_event_ldo_lost, sfr);
    emitted = true;
  }

  if ((pll_now != 0U) && (pll_prev == 0U)) {
    fn(ctx, k_ra_mipi_phy_event_pll_locked, sfr);
    emitted = true;
  } else if ((pll_now == 0U) && (pll_prev != 0U)) {
    fn(ctx, k_ra_mipi_phy_event_pll_lost, sfr);
    emitted = true;
  }

  if (!emitted) {
    fn(ctx, k_ra_mipi_phy_event_status_chg, sfr);
  }
}

ra_err_t ra_mipi_phy_enter_stop(void)
{
  /* HUM Ch 64.4.1 "Power Gating Control or Software Standby Mode", p 3837 */
  return ra_mipi_phy_deinit();
}

ra_err_t ra_mipi_phy_exit_stop(const ra_mipi_phy_config_t* cfg)
{
  /* HUM Ch 64.4.1 "Power Gating Control or Software Standby Mode", p 3837 */
  return ra_mipi_phy_init(cfg);
}

ra_err_t ra_mipi_phy_ldo_enable(void)
{
  /* HUM Ch 64.2.5 "DPHYPWRCR : D-PHY Power Supplying Control Register", p 3826 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_pwrcr) = (uint32_t)k_ra_mipi_phy_pwrcr_pwrsen;
  /* HUM Ch 64.2.6 "DPHYSFR : D-PHY Status Flag Register", p 3826 */
  return internal_mipi_phy_wait_set(ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr),
                                    (uint32_t)k_ra_mipi_phy_sfr_pwrsf);
}

ra_err_t ra_mipi_phy_ldo_disable(void)
{
  /* HUM Ch 64.2.5 "DPHYPWRCR : D-PHY Power Supplying Control Register", p 3826 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_pwrcr) = 0U;
  return k_ra_ok;
}

ra_err_t ra_mipi_phy_pll_start(void)
{
  /* HUM Ch 64.2.3 "DPHYPLOCR : D-PHY PLL Operation Control Register", p 3824 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_plocr) = 0U;
  /* HUM Ch 64.2.6 "DPHYSFR : D-PHY Status Flag Register", p 3826 */
  return internal_mipi_phy_wait_set(ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr),
                                    (uint32_t)k_ra_mipi_phy_sfr_pllsf);
}

ra_err_t ra_mipi_phy_pll_stop(void)
{
  /* HUM Ch 64.2.3 "DPHYPLOCR : D-PHY PLL Operation Control Register", p 3824 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_plocr) = (uint32_t)k_ra_mipi_phy_plocr_pllstp;
  return k_ra_ok;
}

ra_err_t ra_mipi_phy_set_lane_speed(const ra_mipi_phy_pll_t* pll)
{
  RA_CHECK_NULL_PTR((const void*)pll, s_tag, "pll must not be nullptr");
  const ra_err_t v_err = internal_mipi_phy_validate_pll(pll);
  RA_RETURN_ON_ERROR(v_err, s_tag, "set_lane_speed: pll out of range");

  /* HUM Ch 64.2.2 "DPHYPLFCR : D-PHY PLL Frequency Control Register", p 3824 */
  /* ("DPHYPLFCR must be set while D-PHY PLL operation is stopped".) */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_plocr) = (uint32_t)k_ra_mipi_phy_plocr_pllstp;

  /* HUM Ch 64.2.2 "DPHYPLFCR : D-PHY PLL Frequency Control Register", p 3823 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_plfcr) = internal_mipi_phy_pack_plfcr(pll);

  /* HUM Ch 64.2.3 "DPHYPLOCR : D-PHY PLL Operation Control Register", p 3824 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_plocr) = 0U;

  /* HUM Ch 64.2.6 "DPHYSFR : D-PHY Status Flag Register", p 3826 */
  return internal_mipi_phy_wait_set(ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr),
                                    (uint32_t)k_ra_mipi_phy_sfr_pllsf);
}

ra_err_t ra_mipi_phy_switch_mode(ra_mipi_phy_mode_t mode)
{
  if ((mode != k_ra_mipi_phy_mode_dsi_master) && (mode != k_ra_mipi_phy_mode_csi_slave)) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 64.2.7 "DPHYOCR : D-PHY Operation Control Register", p 3827 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_ocr) = 0U;
  /* HUM Ch 64.2.14 "DPHYMDC : D-PHY Mode Control Register", p 3836 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_mdc) =
    (mode == k_ra_mipi_phy_mode_dsi_master) ? (uint32_t)k_ra_mipi_phy_mdc_masteren : 0U;
  return k_ra_ok;
}

ra_err_t ra_mipi_phy_set_lane_count(ra_mipi_phy_lane_count_t count)
{
  if ((count == k_ra_mipi_phy_lane_count_3) || (count == k_ra_mipi_phy_lane_count_4)) {
    /* HUM Ch 64.1 "Overview" p 3822 */
    return k_ra_err_not_supported;
  }
  if ((count != k_ra_mipi_phy_lane_count_1) && (count != k_ra_mipi_phy_lane_count_2)) {
    return k_ra_err_invalid_arg;
  }
  s_lane_count = count;
  /* When the count drops to 1, force lane 1 off so callers don't see
   * a stale "enabled" answer for an inactive lane. */
  if (count == k_ra_mipi_phy_lane_count_1) {
    s_lane_enable_mask &= (uint8_t)~((uint8_t)1U << (uint8_t)k_ra_mipi_phy_lane_bit_d1);
  } else {
    s_lane_enable_mask |= (uint8_t)((uint8_t)1U << (uint8_t)k_ra_mipi_phy_lane_bit_d1);
  }
  return k_ra_ok;
}

ra_mipi_phy_lane_count_t ra_mipi_phy_get_lane_count(void)
{
  return s_lane_count;
}

ra_err_t ra_mipi_phy_set_lane_enable(ra_mipi_phy_lane_id_t lane, bool enable)
{
  if ((lane == k_ra_mipi_phy_lane_d2) || (lane == k_ra_mipi_phy_lane_d3)) {
    /* HUM Ch 64.1 "Overview" p 3822 */
    return k_ra_err_not_supported;
  }
  uint8_t bit = 0U;
  switch (lane) {
    case k_ra_mipi_phy_lane_clk:
      bit = (uint8_t)k_ra_mipi_phy_lane_bit_clk;
      break;
    case k_ra_mipi_phy_lane_d0:
      bit = (uint8_t)k_ra_mipi_phy_lane_bit_d0;
      break;
    case k_ra_mipi_phy_lane_d1:
      bit = (uint8_t)k_ra_mipi_phy_lane_bit_d1;
      /* Cannot enable lane 1 when lane_count = 1. */
      if (enable && (s_lane_count == k_ra_mipi_phy_lane_count_1)) {
        return k_ra_err_invalid_arg;
      }
      break;
    default:
      return k_ra_err_invalid_arg;
  }
  if (enable) {
    s_lane_enable_mask |= (uint8_t)((uint8_t)1U << bit);
  } else {
    s_lane_enable_mask &= (uint8_t)~((uint8_t)1U << bit);
  }
  return k_ra_ok;
}

bool ra_mipi_phy_is_lane_enabled(ra_mipi_phy_lane_id_t lane)
{
  uint8_t bit = 0U;
  switch (lane) {
    case k_ra_mipi_phy_lane_clk:
      bit = (uint8_t)k_ra_mipi_phy_lane_bit_clk;
      break;
    case k_ra_mipi_phy_lane_d0:
      bit = (uint8_t)k_ra_mipi_phy_lane_bit_d0;
      break;
    case k_ra_mipi_phy_lane_d1:
      bit = (uint8_t)k_ra_mipi_phy_lane_bit_d1;
      break;
    default:
      return false;
  }
  return (s_lane_enable_mask & (uint8_t)((uint8_t)1U << bit)) != 0U;
}

ra_err_t ra_mipi_phy_set_clock_mode(ra_mipi_phy_clk_mode_t mode)
{
  if ((mode != k_ra_mipi_phy_clk_continuous) && (mode != k_ra_mipi_phy_clk_noncontinuous)) {
    return k_ra_err_invalid_arg;
  }
  s_clk_mode = mode;
  return k_ra_ok;
}

ra_mipi_phy_clk_mode_t ra_mipi_phy_get_clock_mode(void)
{
  return s_clk_mode;
}

ra_err_t ra_mipi_phy_set_eotp(ra_mipi_phy_eotp_t eotp)
{
  if ((eotp != k_ra_mipi_phy_eotp_enabled) && (eotp != k_ra_mipi_phy_eotp_disabled)) {
    return k_ra_err_invalid_arg;
  }
  s_eotp = eotp;
  return k_ra_ok;
}

ra_mipi_phy_eotp_t ra_mipi_phy_get_eotp(void)
{
  return s_eotp;
}

ra_err_t ra_mipi_phy_set_pclka_freq(uint8_t mhz)
{
  /* HUM Ch 64.2.1 "DPHYREFCR : D-PHY Reference Clock Setting Register", p 3822 */
  if ((mhz < (uint8_t)k_ra_mipi_phy_pclka_min_mhz) ||
      (mhz > (uint8_t)k_ra_mipi_phy_pclka_max_mhz)) {
    return k_ra_err_invalid_arg;
  }
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_refcr) =
    (uint32_t)mhz & (uint32_t)k_ra_mipi_phy_refcr_rfreq_mask;
  return k_ra_ok;
}

ra_err_t ra_mipi_phy_set_escape_divisor(uint8_t escdiv)
{
  /* HUM Ch 64.2.4 "DPHYESCCR : D-PHY Escape Mode Clock Control Register", p 3825 */
  if (escdiv > (uint8_t)k_ra_mipi_phy_escdiv_max) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 64.2.4 "DPHYESCCR : D-PHY Escape Mode Clock Control Register" p 3825 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_plocr) = (uint32_t)k_ra_mipi_phy_plocr_pllstp;
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_esccr) =
    (uint32_t)escdiv & (uint32_t)k_ra_mipi_phy_esccr_escdiv_mask;
  /* HUM Ch 64.2.3 "DPHYPLOCR : D-PHY PLL Operation Control Register", p 3824 */
  *ra_mipi_phy_reg32(k_ra_mipi_phy_off_plocr) = 0U;
  /* HUM Ch 64.2.6 "DPHYSFR : D-PHY Status Flag Register", p 3826 */
  return internal_mipi_phy_wait_set(ra_mipi_phy_reg32(k_ra_mipi_phy_off_sfr),
                                    (uint32_t)k_ra_mipi_phy_sfr_pllsf);
}

ra_err_t ra_mipi_phy_select_timing(ra_mipi_phy_mode_t          mode,
                                   uint8_t                     pclka_mhz,
                                   uint16_t                    rate_mbps,
                                   ra_mipi_phy_timing_t* const out_timing)
{
  if ((mode != k_ra_mipi_phy_mode_dsi_master) && (mode != k_ra_mipi_phy_mode_csi_slave)) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 64.1 "Overview" p 3822 */
  if ((rate_mbps < (uint16_t)k_ra_mipi_phy_line_rate_min_mbps) ||
      (rate_mbps > (uint16_t)k_ra_mipi_phy_line_rate_max_mbps)) {
    return k_ra_err_invalid_arg;
  }

  const ra_mipi_phy_timing_t* row;
  if (mode == k_ra_mipi_phy_mode_dsi_master) {
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
    return k_ra_err_not_supported;
  }
  internal_mipi_phy_write_timing(row);
  if (out_timing != nullptr) {
    *out_timing = *row;
  }
  return k_ra_ok;
}

ra_err_t ra_mipi_phy_validate_pll_band(const ra_mipi_phy_pll_t* pll, uint8_t mosc_mhz)
{
  RA_CHECK_NULL_PTR((const void*)pll, s_tag, "validate_pll_band: pll must not be nullptr");
  if ((mosc_mhz < (uint8_t)k_ra_mipi_phy_mosc_min_mhz) ||
      (mosc_mhz > (uint8_t)k_ra_mipi_phy_mosc_max_mhz)) {
    /* HUM Ch 64.1 "Overview" p 3822 */
    return k_ra_err_invalid_arg;
  }
  const ra_err_t n_err = internal_mipi_phy_validate_pll(pll);
  RA_RETURN_ON_ERROR(n_err, s_tag, "validate_pll_band: NMUL out of range");

  /* HUM Ch 64.2.2 "DPHYPLFCR : D-PHY PLL Frequency Control Register" p 3824 */
  const uint32_t f_mhz = internal_mipi_phy_compute_freq(pll, mosc_mhz);
  uint16_t       lo    = 0U;
  uint16_t       hi    = 0U;
  switch (pll->pmul) {
    case k_ra_mipi_phy_pmul_1:
      lo = (uint16_t)k_ra_mipi_phy_pll_p1_min;
      hi = (uint16_t)k_ra_mipi_phy_pll_p1_max;
      break;
    case k_ra_mipi_phy_pmul_2:
      lo = (uint16_t)k_ra_mipi_phy_pll_p2_min;
      hi = (uint16_t)k_ra_mipi_phy_pll_p2_max;
      break;
    case k_ra_mipi_phy_pmul_4:
      lo = (uint16_t)k_ra_mipi_phy_pll_p4_min;
      hi = (uint16_t)k_ra_mipi_phy_pll_p4_max;
      break;
    case k_ra_mipi_phy_pmul_8:
      lo = (uint16_t)k_ra_mipi_phy_pll_p8_min;
      hi = (uint16_t)k_ra_mipi_phy_pll_p8_max;
      break;
    default:
      return k_ra_err_invalid_arg;
  }
  if ((f_mhz < (uint32_t)lo) || (f_mhz > (uint32_t)hi)) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}
