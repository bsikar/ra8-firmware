/**
 * @file ra8_mipi_phy_ops.c
 * @brief MIPI D-PHY driver -- the observers, pure helpers and dual-mode shadow
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Third translation unit of the MIPI D-PHY driver. ``ra8_mipi_phy.c``
 * owns the lifecycle / power / lane / status / IRQ path and
 * ``ra8_mipi_phy_timing.c`` owns the HUM Table 64.2 / 64.3 matrix; this
 * file owns the part of ``ra8_mipi_phy_ops.h`` that touches no register
 * on the write side:
 *
 *  - Read-only observers: ``ra8_mipi_phy_get_state``,
 *    ``ra8_mipi_phy_get_active_mode``, ``ra8_mipi_phy_get_status_decoded``.
 *  - Pure arithmetic: ``ra8_mipi_phy_compute_pll_freq``,
 *    ``ra8_mipi_phy_compute_lane_rate_mbps``.
 *  - The dry-run table lookup ``ra8_mipi_phy_lookup_timing``.
 *  - The dual-mode arbitration shadow ``ra8_mipi_phy_set_dual_mode`` /
 *    ``ra8_mipi_phy_get_dual_mode`` / ``ra8_mipi_phy_dual_mode_can_acquire``.
 *  - The Hz-taking convenience wrapper ``ra8_mipi_phy_set_pclka_freq_hz``.
 *
 * ``ra8_mipi_phy_get_state`` and ``ra8_mipi_phy_get_active_mode`` are
 * DERIVED from the live registers rather than from a driver-side cache:
 * the D-PHY publishes its own lifecycle position in MSTPCRC, DPHYSFR and
 * DPHYOCR, and DPHYMDC.MASTEREN is readable, so a shadow could only add
 * a way for the answer to go stale. The doxygen in
 * ``ra8_mipi_phy_ops.h`` says the same.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_mipi_phy.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_mipi_phy_internal.h"
#include "ra8_mipi_phy_regs.h"
#include "ra8_mstp_regs.h"

/**
 * @var s_tag
 * @brief Log tag for ``ra8_log_*`` calls in this translation unit.
 */
static const char* s_tag = "MIPI_PHY";

/**
 * @enum ra8_mipi_phy_ops_scale_t
 * @brief Named scale factors used by the pure helpers in this file.
 *
 * @details
 * HUM Ch 64.2.2 p 3824: "Line rate per lane = PLL output / 2", hence
 * ``k_ra8_mipi_phy_lane_rate_div``. ``k_ra8_mipi_phy_hz_per_mhz`` is the
 * Hz -> MHz divisor used by ``ra8_mipi_phy_set_pclka_freq_hz``, and
 * ``k_ra8_mipi_phy_rfreq_value_max`` is the widest value that survives
 * the narrowing to the ``uint8_t`` parameter of
 * ``ra8_mipi_phy_set_pclka_freq`` (DPHYREFCR.RFREQ is 8 bits, HUM
 * Ch 64.2.1 p 3822).
 */
typedef enum : uint32_t {
  k_ra8_mipi_phy_lane_rate_div     = 2U,       /**< PLL output -> per-lane rate. */
  k_ra8_mipi_phy_hz_per_mhz        = 1000000U, /**< Hz per MHz.                  */
  k_ra8_mipi_phy_rfreq_value_max   = 255U,     /**< Widest 8-bit RFREQ input.    */
} ra8_mipi_phy_ops_scale_t;

/**
 * @var s_dual_mode
 * @brief Arbitration policy recorded by ``ra8_mipi_phy_set_dual_mode``.
 *
 * @details
 * Software-only shadow. The RA8D2 multiplexes one D-PHY between DSI
 * host and CSI device through the single DPHYMDC.MASTEREN bit (HUM
 * Ch 64.2.14 p 3836), so the driver cannot arbitrate in hardware; it
 * records the policy the higher-level stack asked for and answers
 * ``ra8_mipi_phy_dual_mode_can_acquire`` from it.
 */
static ra8_mipi_phy_dual_mode_t s_dual_mode = k_ra8_mipi_phy_dual_off;

/**
 * @brief Whether the MIPI PHY module-stop gate is still closed.
 *
 * @details
 * With MSTPCRC bit 13 set the D-PHY registers are unreachable (HUM
 * Ch 64.4.2 p 3838), so every observer in this file has to ask this
 * first and answer from the reset-state defaults instead of faulting
 * on a gated read.
 *
 * @return ``true`` when the block is still stopped.
 * @retval true  MSTPCRC bit 13 is set; D-PHY registers unreachable.
 * @retval false The module clock is running; registers may be read.
 *
 * @pre -- (safe before ``ra8_mipi_phy_init``).
 * @post No register is modified.
 *
 * @note Internal helper. Read-only; safe under simple races.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_mipi_phy_is_stopped(void)
{
  /* HUM Ch 64.4.2 "Module-Stop Function Setting", p 3838 */
  const uint32_t mstpc = ra8_mstp()->MSTPCRC;
  return (mstpc & ((uint32_t)1U << (uint32_t)k_ra8_mipi_phy_mstpc_bit)) != 0U;
}

ra8_err_t ra8_mipi_phy_compute_pll_freq(const ra8_mipi_phy_pll_t* pll,
                                       uint8_t                   mosc_mhz,
                                       uint32_t*                 out_mhz)
{
  RA8_CHECK_NULL_PTR(pll, s_tag, "pll must not be nullptr");
  RA8_CHECK_NULL_PTR(out_mhz, s_tag, "out_mhz must not be nullptr");
  /* HUM Ch 64.2.2 "DPHYPLFCR", p 3823 -- MOSC input window. */
  if (mosc_mhz < (uint8_t)k_ra8_mipi_phy_mosc_min_mhz) {
    return k_ra8_err_invalid_arg;
  }
  if (mosc_mhz > (uint8_t)k_ra8_mipi_phy_mosc_max_mhz) {
    return k_ra8_err_invalid_arg;
  }
  *out_mhz = priv_mipi_phy_compute_freq(pll, mosc_mhz);
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_phy_compute_lane_rate_mbps(const ra8_mipi_phy_pll_t* pll,
                                              uint8_t                   mosc_mhz,
                                              uint32_t*                 out_mbps)
{
  uint32_t        freq_mhz = 0U;
  const ra8_err_t err      = ra8_mipi_phy_compute_pll_freq(pll, mosc_mhz, &freq_mhz);
  if (err != k_ra8_ok) {
    return err;
  }
  /* HUM Ch 64.2.2 p 3824 -- "Line rate per lane = PLL output / 2". */
  *out_mbps = freq_mhz / (uint32_t)k_ra8_mipi_phy_lane_rate_div;
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_phy_lookup_timing(ra8_mipi_phy_mode_t          mode,
                                     uint8_t                      pclka_mhz,
                                     uint16_t                     rate_mbps,
                                     ra8_mipi_phy_timing_t* const out_timing)
{
  RA8_CHECK_NULL_PTR(out_timing, s_tag, "out_timing must not be nullptr");
  return priv_mipi_phy_find_timing(mode, pclka_mhz, rate_mbps, out_timing);
}

ra8_err_t ra8_mipi_phy_get_status_decoded(ra8_mipi_phy_status_decoded_t* const out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");

  /* HUM Ch 64.2.6 "DPHYSFR : D-PHY Status Flag Register", p 3826 */
  const uint32_t sfr = *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr);
  out->raw           = sfr;
  out->ldo_ready     = (sfr & k_ra8_mipi_phy_sfr_pwrsf) != 0U;
  out->pll_locked    = (sfr & k_ra8_mipi_phy_sfr_pllsf) != 0U;
  out->phy_ready     = (sfr & k_ra8_mipi_phy_sfr_ready_mask) == k_ra8_mipi_phy_sfr_ready_mask;
  return k_ra8_ok;
}

ra8_mipi_phy_state_t ra8_mipi_phy_get_state(void)
{
  if (internal_mipi_phy_is_stopped()) {
    return k_ra8_mipi_phy_state_off;
  }
  /* HUM Ch 64.3.1 "Start-up procedure", p 3837 -- the flags rise in order. */
  const uint32_t sfr = *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr);
  if ((sfr & k_ra8_mipi_phy_sfr_pwrsf) == 0U) {
    return k_ra8_mipi_phy_state_idle;
  }
  if ((sfr & k_ra8_mipi_phy_sfr_pllsf) == 0U) {
    return k_ra8_mipi_phy_state_ldo_up;
  }
  /* HUM Ch 64.2.7 "DPHYOCR : D-PHY Operation Control Register", p 3827 */
  const uint32_t ocr = *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_ocr);
  if ((ocr & k_ra8_mipi_phy_ocr_dphyen) != 0U) {
    return k_ra8_mipi_phy_state_run;
  }
  return k_ra8_mipi_phy_state_pll_run;
}

ra8_mipi_phy_mode_t ra8_mipi_phy_get_active_mode(void)
{
  if (internal_mipi_phy_is_stopped()) {
    /* DPHYMDC reset value is 0 = CSI device (HUM Ch 64.2.14 p 3837). */
    return k_ra8_mipi_phy_mode_csi_device;
  }
  /* HUM Ch 64.2.14 "DPHYMDC : D-PHY Mode Control Register", p 3836 */
  const uint32_t mdc = *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_mdc);
  if ((mdc & k_ra8_mipi_phy_mdc_hosten) != 0U) {
    return k_ra8_mipi_phy_mode_dsi_host;
  }
  return k_ra8_mipi_phy_mode_csi_device;
}

ra8_err_t ra8_mipi_phy_set_dual_mode(ra8_mipi_phy_dual_mode_t mode)
{
  switch (mode) {
    case k_ra8_mipi_phy_dual_off:
    case k_ra8_mipi_phy_dual_alternate:
    case k_ra8_mipi_phy_dual_dsi_priority:
    case k_ra8_mipi_phy_dual_csi_priority:
      s_dual_mode = mode;
      return k_ra8_ok;
    default:
      return k_ra8_err_invalid_arg;
  }
}

ra8_mipi_phy_dual_mode_t ra8_mipi_phy_get_dual_mode(void)
{
  return s_dual_mode;
}

bool ra8_mipi_phy_dual_mode_can_acquire(ra8_mipi_phy_mode_t requestor)
{
  if (requestor == k_ra8_mipi_phy_mode_dsi_host) {
    return s_dual_mode != k_ra8_mipi_phy_dual_csi_priority;
  }
  if (requestor == k_ra8_mipi_phy_mode_csi_device) {
    return s_dual_mode != k_ra8_mipi_phy_dual_dsi_priority;
  }
  return false;
}

ra8_err_t ra8_mipi_phy_set_pclka_freq_hz(uint32_t hz)
{
  const uint32_t mhz = hz / (uint32_t)k_ra8_mipi_phy_hz_per_mhz;
  if (mhz > (uint32_t)k_ra8_mipi_phy_rfreq_value_max) {
    return k_ra8_err_invalid_arg;
  }
  /* Range enforcement (40..125 MHz) stays in the one place that owns it. */
  return ra8_mipi_phy_set_pclka_freq((uint8_t)mhz);
}
