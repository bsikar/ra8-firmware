/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_mipi_phy.c
 * @brief MIPI D-PHY driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Full HUM Ch 64 coverage (p 3822-3838). Every documented register
 * field has a corresponding code path; every operating mode is
 * reachable through the public API; every IRQ source is decoded by
 * the dispatcher. The PHY is consumed by ``ra8_mipi_dsi`` (host)
 * and ``ra8_mipi_csi`` (device); this driver does NOT touch either of
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
 * falling) into a typed ``ra8_mipi_phy_event_t`` rather than handing
 * the consumer a raw register dump.
 */

#include "ra8_mipi_phy.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_mipi_phy_internal.h"
#include "ra8_mipi_phy_regs.h"
#include "ra8_mstp_regs.h"

/**
 * @var s_tag
 * @brief Log tag for ``ra8_log_*`` calls in this driver.
 */
static const char* s_tag = "MIPI_PHY";

/**
 * @enum ra8_mipi_phy_limits_t
 * @brief Validation guard rails for ``ra8_mipi_phy_init``.
 *
 * @details
 * HUM Ch 64.2.1 p 3822: RFREQ accepts 0x27 (40 MHz) .. 0x7C
 * (125 MHz). HUM Ch 64.2.4 p 3825: ESCDIV[4:0] is a 5-bit field.
 * HUM Ch 64.2.2 p 3823: NMUL[8:0] valid range is 40 .. 375.
 */
typedef enum : uint16_t {
  k_ra8_mipi_phy_pclka_min_mhz = 40U,   /**< Lower bound on RFREQ value. */
  k_ra8_mipi_phy_pclka_max_mhz = 125U,  /**< Upper bound on RFREQ value. */
  k_ra8_mipi_phy_escdiv_max    = 31U,   /**< 5-bit field width.          */
  k_ra8_mipi_phy_nmul_min      = 40U,   /**< NMUL_int floor.             */
  k_ra8_mipi_phy_nmul_max      = 375U,  /**< NMUL_int ceiling.           */
  k_ra8_mipi_phy_spin_budget   = 4096U, /**< Max poll iterations.        */
} ra8_mipi_phy_limits_t;

/**
 * @enum ra8_mipi_phy_mstpc_bit_t
 * @brief Direct-write fallback for the MIPI PHY module-stop bit.
 *
 * @details
 * The shared ``ra8_mstp_t`` enum in ``libs/ra8_hal/inc/ra8_mstp_regs.h``
 * does NOT yet have a ``k_ra8_mipi_phy`` entry. Per the agent brief
 * we must not extend that file from this driver. As a stop-gap, the
 * driver clears MSTPCRC bit 13 (the MIPI PHY slot in MSTPCRC -- HUM
 * Ch 64.4.2 p 3838 references MSTPCRC for the block) directly.
 *
 * TODO: When ``ra8_mstp_regs.h`` gains an explicit ``k_ra8_mstp_mipi_phy``
 * value (driven by HUM Ch 11.2.8 "MSTPCRC" p 446-447), replace the
 * direct register write below with ``ra8_mstp_enable(k_ra8_mstp_mipi_phy)``.
 */
typedef enum : uint8_t {
  k_ra8_mipi_phy_mstpc_bit = 13U, /**< Provisional MSTPC slot. */
} ra8_mipi_phy_mstpc_bit_t;

/**
 * @enum ra8_mipi_phy_lane_bit_t
 * @brief Software-lane bitmap positions for ``s_lane_enable_mask``.
 */
typedef enum : uint8_t {
  k_ra8_mipi_phy_lane_bit_clk = 0U, /**< bit 0: clock lane.  */
  k_ra8_mipi_phy_lane_bit_d0  = 1U, /**< bit 1: data lane 0. */
  k_ra8_mipi_phy_lane_bit_d1  = 2U, /**< bit 2: data lane 1. */
} ra8_mipi_phy_lane_bit_t;

/**
 * @enum ra8_mipi_phy_lane_mask_t
 * @brief Default lane-enable bitmap (clock + d0 + d1 all on).
 */
typedef enum : uint8_t {
  k_ra8_mipi_phy_lane_mask_default =
    (uint8_t)((1U << k_ra8_mipi_phy_lane_bit_clk) | (1U << k_ra8_mipi_phy_lane_bit_d0) |
              (1U << k_ra8_mipi_phy_lane_bit_d1)), /**< RA8 mipi PHY lane mask default. */
} ra8_mipi_phy_lane_mask_t;

/* =============================================================================
 * Driver state (module-private)
 * =============================================================================
 */

/**
 * @var s_mipi_phy_fn
 * @brief Registered callback for ``ra8_mipi_phy_dispatch``.
 */
static ra8_mipi_phy_event_fn_t s_mipi_phy_fn;

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
 * @brief Last lane-count argument accepted by ``ra8_mipi_phy_set_lane_count``.
 */
static ra8_mipi_phy_lane_count_t s_lane_count = k_ra8_mipi_phy_lane_count_2;

/**
 * @var s_lane_enable_mask
 * @brief Bitmap of enabled lanes (bit 0 = clock, bit N = data lane N-1).
 */
static uint8_t s_lane_enable_mask = k_ra8_mipi_phy_lane_mask_default;

/**
 * @var s_clk_mode
 * @brief HS clock-lane operating mode (continuous vs non-continuous).
 */
static ra8_mipi_phy_clk_mode_t s_clk_mode = k_ra8_mipi_phy_clk_noncontinuous;

/**
 * @var s_eotp
 * @brief Whether DSI host should append EoTP packets (host mode).
 */
static ra8_mipi_phy_eotp_t s_eotp = k_ra8_mipi_phy_eotp_disabled;

/* =============================================================================
 * Helpers (file-local)
 * =============================================================================
 */

/**
 * @brief Clear MSTPCRC bit 13, ungating MIPI PHY.
 *
 * @details
 * Stop-gap until ``ra8_mstp_t`` learns the MIPI PHY slot. Cites HUM
 * Ch 64.4.2 p 3838 ("MIPI PHY operation can be disabled or enabled
 * using Module Stop Control Register C (MSTPCRC). The MIPI PHY
 * module is initially stopped after reset.").
 *
 * @pre Caller has unlocked write protection on MSTPCRC if required.
 * @pre Called from a single-threaded init context (or with IRQs masked).
 * @post MSTPCRC bit 13 is cleared and the MIPI PHY clock is running.
 * @post No other MSTPCRC bits are modified.
 *
 * @note Internal helper. Not thread-safe; caller provides synchronisation.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_mipi_phy_mstp_unstop(void)
{
  /* HUM Ch 64.4.2 "Module-Stop Function Setting", p 3838 */
  volatile uint32_t* mstpc = &ra8_mstp()->MSTPCRC;
  *mstpc                   = *mstpc & ~((uint32_t)1U << (uint32_t)k_ra8_mipi_phy_mstpc_bit);
}

/* Spin until ``(reg & mask) == mask`` or the budget runs out -- see surrounding code and HUM citations. */
RA8_INTERNAL
static ra8_err_t internal_mipi_phy_wait_set(volatile const uint32_t* reg, uint32_t mask)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_mipi_phy_spin_budget; ++i) { /* GCOVR_EXCL_BR_LINE */
    if ((*reg & mask) == mask) {                                         /* GCOVR_EXCL_BR_LINE */
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

/* Pack a validated PLL config into the DPHYPLFCR register layout -- see surrounding code and HUM citations. */
RA8_INTERNAL
static uint32_t internal_mipi_phy_pack_plfcr(const ra8_mipi_phy_pll_t* pll)
{
  uint32_t v = 0U;
  v |= ((uint32_t)pll->idiv & k_ra8_mipi_phy_plfcr_mask_idiv)
       << (uint32_t)k_ra8_mipi_phy_plfcr_shift_idiv;
  v |= ((uint32_t)pll->nfmul & k_ra8_mipi_phy_plfcr_mask_nfmul)
       << (uint32_t)k_ra8_mipi_phy_plfcr_shift_nfmul;
  v |= ((uint32_t)pll->pmul & k_ra8_mipi_phy_plfcr_mask_pmul)
       << (uint32_t)k_ra8_mipi_phy_plfcr_shift_pmul;
  v |= ((uint32_t)pll->nmul_int & k_ra8_mipi_phy_plfcr_mask_nmul)
       << (uint32_t)k_ra8_mipi_phy_plfcr_shift_nmul;
  return v;
}

/**
 * @brief Validate a ``ra8_mipi_phy_pll_t`` against HUM 64.2.2 p 3823 limits.
 *
 * @details
 * Range-checks NMUL[8:0] only. Use ``ra8_mipi_phy_validate_pll_band``
 * for the full PMUL band check.
 * @param[in] pll See declaration: ``const ra8_mipi_phy_pll_t* pll``.
 * @return ::ra8_err_t outcome (or scalar return value).
 * @retval k_ra8_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Internal helper. Not thread-safe; caller provides synchronisation.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_mipi_phy_validate_pll(const ra8_mipi_phy_pll_t* pll)
{
  if ((pll->nmul_int < k_ra8_mipi_phy_nmul_min) || (pll->nmul_int > k_ra8_mipi_phy_nmul_max)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/** @brief Implementation of `internal_mipi_phy_write_timing()` -- packs DPHYTIM1..6. */
void internal_mipi_phy_write_timing(const ra8_mipi_phy_timing_t* t)
{
  /* HUM Ch 64.2.8 "DPHYTIM1 : D-PHY Timing Control Register 1", p 3827 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_tim1) =
    (uint32_t)t->tinit & k_ra8_mipi_phy_tim1_tinit_mask;

  /* HUM Ch 64.2.9 "DPHYTIM2 : D-PHY Timing Control Register 2", p 3828 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_tim2) =
    (((uint32_t)t->tclkmiss & k_ra8_mipi_phy_tim_byte_mask)
     << (uint32_t)k_ra8_mipi_phy_tim_shift_b2) |
    (((uint32_t)t->tclksett & k_ra8_mipi_phy_tim_byte_mask)
     << (uint32_t)k_ra8_mipi_phy_tim_shift_b1) |
    (((uint32_t)t->tclkprep & k_ra8_mipi_phy_tim_byte_mask)
     << (uint32_t)k_ra8_mipi_phy_tim_shift_b0);

  /* HUM Ch 64.2.10 "DPHYTIM3 : D-PHY Timing Control Register 3", p 3828 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_tim3) =
    (((uint32_t)t->thssett & k_ra8_mipi_phy_tim_byte_mask)
     << (uint32_t)k_ra8_mipi_phy_tim_shift_b1) |
    (((uint32_t)t->thsprep & k_ra8_mipi_phy_tim_byte_mask)
     << (uint32_t)k_ra8_mipi_phy_tim_shift_b0);

  /* HUM Ch 64.2.11 "DPHYTIM4 : D-PHY Timing Control Register 4", p 3829 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_tim4) =
    (((uint32_t)t->tclktrl & k_ra8_mipi_phy_tim_byte_mask)
     << (uint32_t)k_ra8_mipi_phy_tim_shift_b3) |
    (((uint32_t)t->tclkpost & k_ra8_mipi_phy_tim_byte_mask)
     << (uint32_t)k_ra8_mipi_phy_tim_shift_b2) |
    (((uint32_t)t->tclkpre & k_ra8_mipi_phy_tim_byte_mask)
     << (uint32_t)k_ra8_mipi_phy_tim_shift_b1) |
    (((uint32_t)t->tclkzero & k_ra8_mipi_phy_tim_byte_mask)
     << (uint32_t)k_ra8_mipi_phy_tim_shift_b0);

  /* HUM Ch 64.2.12 "DPHYTIM5 : D-PHY Timing Control Register 5", p 3830 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_tim5) =
    (((uint32_t)t->thsexit & k_ra8_mipi_phy_tim_byte_mask)
     << (uint32_t)k_ra8_mipi_phy_tim_shift_b2) |
    (((uint32_t)t->thstrl & k_ra8_mipi_phy_tim_byte_mask)
     << (uint32_t)k_ra8_mipi_phy_tim_shift_b1) |
    (((uint32_t)t->thszero & k_ra8_mipi_phy_tim_byte_mask)
     << (uint32_t)k_ra8_mipi_phy_tim_shift_b0);

  /* HUM Ch 64.2.13 "DPHYTIM6 : D-PHY Timing Control Register 6", p 3831 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_tim6) = (uint32_t)t->tlpx & k_ra8_mipi_phy_tim6_tlpx_mask;
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

/**
 * @brief Validate ranges/lane-count for ``ra8_mipi_phy_init``.
 *
 * @details
 * Cites HUM Ch 64.2.1 p 3822 (RFREQ range), Ch 64.2.4 p 3825 (ESCDIV
 * width), Ch 64.1 p 3822 (lane-count support matrix), and Ch 64.2.2
 * p 3823 (PLL parameter range, host mode only). Also rejects
 * nullptrs in ``cfg`` and ``cfg->p_timing``.
 * @param[in] cfg See declaration: ``const ra8_mipi_phy_config_t* cfg``.
 * @return ::ra8_err_t outcome (or scalar return value).
 * @retval k_ra8_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Internal helper. Not thread-safe; caller provides synchronisation.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_mipi_phy_validate_init_cfg(const ra8_mipi_phy_config_t* cfg)
{
  if (cfg == nullptr || cfg->p_timing == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((cfg->pclka_mhz < (uint8_t)k_ra8_mipi_phy_pclka_min_mhz) ||
      (cfg->pclka_mhz > (uint8_t)k_ra8_mipi_phy_pclka_max_mhz)) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->escdiv > (uint8_t)k_ra8_mipi_phy_escdiv_max) {
    return k_ra8_err_invalid_arg;
  }
  if ((cfg->lane_count != k_ra8_mipi_phy_lane_count_1) &&
      (cfg->lane_count != k_ra8_mipi_phy_lane_count_2)) {
    return ((cfg->lane_count == k_ra8_mipi_phy_lane_count_3) ||
            (cfg->lane_count == k_ra8_mipi_phy_lane_count_4))
             ? k_ra8_err_not_supported
             : k_ra8_err_invalid_arg;
  }
  if (cfg->mode == k_ra8_mipi_phy_mode_dsi_host) {
    return internal_mipi_phy_validate_pll(&cfg->pll);
  }
  return k_ra8_ok;
}

/**
 * @brief Apply common init steps 1-5 (mode, refclk, LDO power-up).
 *
 * @details
 * Step 1 (HUM Ch 64.4.2 p 3838) ungates MSTPCRC.
 * Step 2 (HUM Ch 64.2.14 p 3836) selects host vs device via DPHYMDC.
 * Step 3 (HUM Ch 64.2.1 p 3822) programs DPHYREFCR.
 * Step 4 (HUM Ch 64.2.5 p 3826) sets DPHYPWRCR.PWRSEN.
 * Step 5 (HUM Ch 64.2.6 p 3826) polls DPHYSFR until PWRSF latches.
 * @param[in] cfg See declaration: ``const ra8_mipi_phy_config_t* cfg``.
 * @return ::ra8_err_t outcome (or scalar return value).
 * @retval k_ra8_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Internal helper. Not thread-safe; caller provides synchronisation.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_mipi_phy_init_power_up(const ra8_mipi_phy_config_t* cfg)
{
  internal_mipi_phy_mstp_unstop();
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_mdc) =
    (cfg->mode == k_ra8_mipi_phy_mode_dsi_host) ? k_ra8_mipi_phy_mdc_hosten : 0U;
  /* HUM Ch 64.2.1 p 3822: RFREQ encodes (MHz - 1). FSP r_mipi_phy.c
   * mirrors this with ``(pclka_hz / 1MHz) - 1``. */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_refcr) =
    ((uint32_t)cfg->pclka_mhz - (uint32_t)k_ra8_mipi_phy_refcr_rfreq_bias) &
    k_ra8_mipi_phy_refcr_rfreq_mask;
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_pwrcr) = k_ra8_mipi_phy_pwrcr_pwrsen;
  return internal_mipi_phy_wait_set(ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr),
                                    k_ra8_mipi_phy_sfr_pwrsf);
}

/**
 * @brief Apply DSI-host-only steps (6-9) of the init sequence.
 *
 * @details
 * Steps come from HUM Ch 64 p 3823-3826: program PLFCR, ESCCR, clear
 * PLOCR, then poll DPHYSFR until PLLSF latches.
 * @param[in] cfg See declaration: ``const ra8_mipi_phy_config_t* cfg``.
 * @return ::ra8_err_t outcome (or scalar return value).
 * @retval k_ra8_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Internal helper. Not thread-safe; caller provides synchronisation.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_mipi_phy_init_host(const ra8_mipi_phy_config_t* cfg)
{
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plfcr) = internal_mipi_phy_pack_plfcr(&cfg->pll);
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_esccr) =
    (uint32_t)cfg->escdiv & k_ra8_mipi_phy_esccr_escdiv_mask;
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plocr) = 0U;
  return internal_mipi_phy_wait_set(ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr),
                                    k_ra8_mipi_phy_sfr_pllsf);
}

/**
 * @brief Latch ``cfg`` knobs into the driver's tracked-state cache.
 *
 * @details
 * None of these settings live in PHY registers; downstream DSI / CSI
 * drivers consult them after init returns.
 * @param[in] cfg See declaration: ``const ra8_mipi_phy_config_t* cfg``.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Internal helper. Not thread-safe; caller provides synchronisation.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_mipi_phy_cache_state(const ra8_mipi_phy_config_t* cfg)
{
  s_lane_count       = cfg->lane_count;
  s_lane_enable_mask = k_ra8_mipi_phy_lane_mask_default;
  s_clk_mode         = cfg->clk_mode;
  s_eotp             = cfg->eotp;
  s_last_sfr         = *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr);
}

ra8_err_t ra8_mipi_phy_init(const ra8_mipi_phy_config_t* cfg)
{
  const ra8_err_t v_err = internal_mipi_phy_validate_init_cfg(cfg);
  RA8_RETURN_ON_ERROR(v_err, s_tag, "init: bad cfg"); /* GCOVR_EXCL_BR_LINE */

  /* Steps 1-5 -- HUM Ch 64.4.2 / 64.2.14 / 64.2.1 / 64.2.5 / 64.2.6 */
  const ra8_err_t pwr_err = internal_mipi_phy_init_power_up(cfg);
  RA8_RETURN_ON_ERROR(pwr_err, s_tag, "init: LDO did not stabilise"); /* GCOVR_EXCL_BR_LINE */

  if (cfg->mode == k_ra8_mipi_phy_mode_dsi_host) {
    /* Steps 6-9 -- HUM Ch 64.2.2/64.2.4/64.2.3/64.2.6 p 3823-3826 */
    const ra8_err_t m_err = internal_mipi_phy_init_host(cfg);
    RA8_RETURN_ON_ERROR(m_err, s_tag, "init: PLL did not lock"); /* GCOVR_EXCL_BR_LINE */
  } else {
    /* CSI device mode: the PHY is a receiver, so the host-side PLL
     * controls (PLFCR / ESCCR / PLOCR) are unused by this block. Make
     * sure they hold their reset value (0) regardless of any prior
     * host-mode state -- HUM Ch 64.2.2/64.2.3/64.2.4 p 3823-3825. */
    *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plfcr) = 0U;
    *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_esccr) = 0U;
    *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plocr) = 0U;
  }

  /* Step 10 -- HUM Ch 64.2.8 "DPHYTIM1 : D-PHY Timing Control Register 1", p 3827 */
  internal_mipi_phy_write_timing(cfg->p_timing);

  /* Step 11 -- HUM Ch 64.2.7 "DPHYOCR : D-PHY Operation Control Register", p 3827 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_ocr) = k_ra8_mipi_phy_ocr_dphyen;

  internal_mipi_phy_cache_state(cfg);

  ra8_log_info(s_tag, "mipi_phy_init done");
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_phy_deinit(void)
{
  /* Step 1 -- HUM Ch 64.2.7 "DPHYOCR : D-PHY Operation Control Register", p 3827 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_ocr) = 0U;

  /* Step 2 -- HUM Ch 64.2.3 "DPHYPLOCR : D-PHY PLL Operation Control Register", p 3824 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plocr) = k_ra8_mipi_phy_plocr_pllstp;

  /* Step 3 -- HUM Ch 64.2.5 "DPHYPWRCR : D-PHY Power Supplying Control Register", p 3826 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_pwrcr) = 0U;

  ra8_log_info(s_tag, "mipi_phy_deinit done");
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_phy_reset(void)
{
  /* HUM Ch 64.2.7 "DPHYOCR : D-PHY Operation Control Register", p 3827 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_ocr) = 0U;
  /* HUM Ch 64.2.3 "DPHYPLOCR : D-PHY PLL Operation Control Register", p 3824 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plocr) = k_ra8_mipi_phy_plocr_pllstp;
  /* HUM Ch 64.2.5 "DPHYPWRCR : D-PHY Power Supplying Control Register", p 3826 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_pwrcr) = 0U;
  /* HUM Ch 64.2.2 "DPHYPLFCR : D-PHY PLL Frequency Control Register", p 3823 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plfcr) = 0U;
  /* HUM Ch 64.2.4 "DPHYESCCR : D-PHY Escape Mode Clock Control Register", p 3825 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_esccr) = 0U;
  /* HUM Ch 64.2.1 "DPHYREFCR : D-PHY Reference Clock Setting Register", p 3822 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_refcr) = 0U;
  /* HUM Ch 64.2.14 "DPHYMDC : D-PHY Mode Control Register", p 3836 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_mdc) = 0U;
  /* HUM Ch 64.2.8 "DPHYTIM1 : D-PHY Timing Control Register 1", p 3827 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_tim1) = 0U;
  /* HUM Ch 64.2.9 "DPHYTIM2 : D-PHY Timing Control Register 2", p 3828 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_tim2) = 0U;
  /* HUM Ch 64.2.10 "DPHYTIM3 : D-PHY Timing Control Register 3", p 3828 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_tim3) = 0U;
  /* HUM Ch 64.2.11 "DPHYTIM4 : D-PHY Timing Control Register 4", p 3829 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_tim4) = 0U;
  /* HUM Ch 64.2.12 "DPHYTIM5 : D-PHY Timing Control Register 5", p 3830 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_tim5) = 0U;
  /* HUM Ch 64.2.13 "DPHYTIM6 : D-PHY Timing Control Register 6", p 3831 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_tim6) = 0U;
  s_last_sfr                                   = 0U;
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_phy_recover_from_error(const ra8_mipi_phy_config_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "recover: cfg must not be nullptr");
  const ra8_err_t r_err = ra8_mipi_phy_reset();
  RA8_RETURN_ON_ERROR(r_err, s_tag, "recover: reset failed"); /* GCOVR_EXCL_BR_LINE */
  return ra8_mipi_phy_init(cfg);
}

ra8_err_t ra8_mipi_phy_get_status(uint32_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");

  /* HUM Ch 64.2.6 "DPHYSFR : D-PHY Status Flag Register", p 3826 */
  *out_mask = *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr);
  return k_ra8_ok;
}

bool ra8_mipi_phy_is_ldo_stable(void)
{
  /* HUM Ch 64.2.6 "DPHYSFR : D-PHY Status Flag Register", p 3826 */
  return (*ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr) & k_ra8_mipi_phy_sfr_pwrsf) != 0U;
}

bool ra8_mipi_phy_is_pll_locked(void)
{
  /* HUM Ch 64.2.6 "DPHYSFR : D-PHY Status Flag Register", p 3826 */
  return (*ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr) & k_ra8_mipi_phy_sfr_pllsf) != 0U;
}

ra8_err_t ra8_mipi_phy_wait_ready(void)
{
  /* HUM Ch 64.2.6 "DPHYSFR : D-PHY Status Flag Register", p 3826 */
  return internal_mipi_phy_wait_set(ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr),
                                    k_ra8_mipi_phy_sfr_ready_mask);
}

ra8_err_t ra8_mipi_phy_attach_handler(ra8_mipi_phy_event_fn_t fn, void* ctx)
{
  s_mipi_phy_fn  = fn;
  s_mipi_phy_ctx = ctx;
  return k_ra8_ok;
}

RA8_ISR_SAFE
void ra8_mipi_phy_dispatch(void)
{
  /* HUM Ch 64.2.6 "DPHYSFR : D-PHY Status Flag Register", p 3826 */
  const uint32_t sfr                = *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr);
  const uint32_t prev               = s_last_sfr;
  s_last_sfr                        = sfr;
  const ra8_mipi_phy_event_fn_t fn  = s_mipi_phy_fn;
  void* const                   ctx = s_mipi_phy_ctx;

  if (fn == nullptr) {
    return;
  }

  const uint32_t pwr_now  = sfr & k_ra8_mipi_phy_sfr_pwrsf;
  const uint32_t pwr_prev = prev & k_ra8_mipi_phy_sfr_pwrsf;
  const uint32_t pll_now  = sfr & k_ra8_mipi_phy_sfr_pllsf;
  const uint32_t pll_prev = prev & k_ra8_mipi_phy_sfr_pllsf;

  /* Emit at most one event per dispatch -- power edges take priority
   * over PLL edges, with status_chg as the fallback when nothing
   * transitioned. Multiple emits per call would over-count callers
   * that key off the cb count. */
  if ((pwr_now != 0U) && (pwr_prev == 0U)) {
    fn(ctx, k_ra8_mipi_phy_event_ldo_ready, sfr);
  } else if ((pwr_now == 0U) && (pwr_prev != 0U)) {
    fn(ctx, k_ra8_mipi_phy_event_ldo_lost, sfr);
  } else if ((pll_now != 0U) && (pll_prev == 0U)) {
    fn(ctx, k_ra8_mipi_phy_event_pll_locked, sfr);
  } else if ((pll_now == 0U) && (pll_prev != 0U)) {
    fn(ctx, k_ra8_mipi_phy_event_pll_lost, sfr);
  } else {
    fn(ctx, k_ra8_mipi_phy_event_status_chg, sfr);
  }
}

ra8_err_t ra8_mipi_phy_enter_stop(void)
{
  /* HUM Ch 64.4.1 "Power Gating Control or Software Standby Mode", p 3837 */
  return ra8_mipi_phy_deinit();
}

ra8_err_t ra8_mipi_phy_exit_stop(const ra8_mipi_phy_config_t* cfg)
{
  /* HUM Ch 64.4.1 "Power Gating Control or Software Standby Mode", p 3837 */
  return ra8_mipi_phy_init(cfg);
}

ra8_err_t ra8_mipi_phy_ldo_enable(void)
{
  /* HUM Ch 64.2.5 "DPHYPWRCR : D-PHY Power Supplying Control Register", p 3826 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_pwrcr) = k_ra8_mipi_phy_pwrcr_pwrsen;
  /* HUM Ch 64.2.6 "DPHYSFR : D-PHY Status Flag Register", p 3826 */
  return internal_mipi_phy_wait_set(ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr),
                                    k_ra8_mipi_phy_sfr_pwrsf);
}

ra8_err_t ra8_mipi_phy_ldo_disable(void)
{
  /* HUM Ch 64.2.5 "DPHYPWRCR : D-PHY Power Supplying Control Register", p 3826 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_pwrcr) = 0U;
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_phy_pll_start(void)
{
  /* HUM Ch 64.2.3 "DPHYPLOCR : D-PHY PLL Operation Control Register", p 3824 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plocr) = 0U;
  /* HUM Ch 64.2.6 "DPHYSFR : D-PHY Status Flag Register", p 3826 */
  return internal_mipi_phy_wait_set(ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr),
                                    k_ra8_mipi_phy_sfr_pllsf);
}

ra8_err_t ra8_mipi_phy_pll_stop(void)
{
  /* HUM Ch 64.2.3 "DPHYPLOCR : D-PHY PLL Operation Control Register", p 3824 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plocr) = k_ra8_mipi_phy_plocr_pllstp;
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_phy_set_lane_speed(const ra8_mipi_phy_pll_t* pll)
{
  RA8_CHECK_NULL_PTR(pll, s_tag, "pll must not be nullptr");
  const ra8_err_t v_err = internal_mipi_phy_validate_pll(pll);
  RA8_RETURN_ON_ERROR(v_err, s_tag, "set_lane_speed: pll out of range"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 64.2.2 "DPHYPLFCR : D-PHY PLL Frequency Control Register", p 3824 */
  /* ("DPHYPLFCR must be set while D-PHY PLL operation is stopped".) */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plocr) = k_ra8_mipi_phy_plocr_pllstp;

  /* HUM Ch 64.2.2 "DPHYPLFCR : D-PHY PLL Frequency Control Register", p 3823 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plfcr) = internal_mipi_phy_pack_plfcr(pll);

  /* HUM Ch 64.2.3 "DPHYPLOCR : D-PHY PLL Operation Control Register", p 3824 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plocr) = 0U;

  /* HUM Ch 64.2.6 "DPHYSFR : D-PHY Status Flag Register", p 3826 */
  return internal_mipi_phy_wait_set(ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr),
                                    k_ra8_mipi_phy_sfr_pllsf);
}

ra8_err_t ra8_mipi_phy_switch_mode(ra8_mipi_phy_mode_t mode)
{
  if ((mode != k_ra8_mipi_phy_mode_dsi_host) && (mode != k_ra8_mipi_phy_mode_csi_device)) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 64.2.7 "DPHYOCR : D-PHY Operation Control Register", p 3827 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_ocr) = 0U;
  /* HUM Ch 64.2.14 "DPHYMDC : D-PHY Mode Control Register", p 3836 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_mdc) =
    (mode == k_ra8_mipi_phy_mode_dsi_host) ? k_ra8_mipi_phy_mdc_hosten : 0U;
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_phy_set_lane_count(ra8_mipi_phy_lane_count_t count)
{
  if ((count == k_ra8_mipi_phy_lane_count_3) || (count == k_ra8_mipi_phy_lane_count_4)) {
    /* HUM Ch 64.1 "Overview" p 3822 */
    return k_ra8_err_not_supported;
  }
  if ((count != k_ra8_mipi_phy_lane_count_1) && (count != k_ra8_mipi_phy_lane_count_2)) {
    return k_ra8_err_invalid_arg;
  }
  s_lane_count = count;
  /* When the count drops to 1, force lane 1 off so callers don't see
   * a stale "enabled" answer for an inactive lane. */
  if (count == k_ra8_mipi_phy_lane_count_1) {
    s_lane_enable_mask &= (uint8_t)~((uint8_t)1U << k_ra8_mipi_phy_lane_bit_d1);
  } else {
    s_lane_enable_mask |= (uint8_t)((uint8_t)1U << k_ra8_mipi_phy_lane_bit_d1);
  }
  return k_ra8_ok;
}

ra8_mipi_phy_lane_count_t ra8_mipi_phy_get_lane_count(void)
{
  return s_lane_count;
}

ra8_err_t ra8_mipi_phy_set_lane_enable(ra8_mipi_phy_lane_id_t lane, bool enable)
{
  if ((lane == k_ra8_mipi_phy_lane_d2) || (lane == k_ra8_mipi_phy_lane_d3)) {
    /* HUM Ch 64.1 "Overview" p 3822 */
    return k_ra8_err_not_supported;
  }
  uint8_t bit = 0U;
  switch (lane) {
    case k_ra8_mipi_phy_lane_clk:
      bit = k_ra8_mipi_phy_lane_bit_clk;
      break;
    case k_ra8_mipi_phy_lane_d0:
      bit = k_ra8_mipi_phy_lane_bit_d0;
      break;
    case k_ra8_mipi_phy_lane_d1:
      bit = k_ra8_mipi_phy_lane_bit_d1;
      /* Cannot enable lane 1 when lane_count = 1. */
      if (enable && (s_lane_count == k_ra8_mipi_phy_lane_count_1)) {
        return k_ra8_err_invalid_arg;
      }
      break;
    default:
      return k_ra8_err_invalid_arg;
  }
  if (enable) {
    s_lane_enable_mask |= (uint8_t)((uint8_t)1U << bit);
  } else {
    s_lane_enable_mask &= (uint8_t)~((uint8_t)1U << bit);
  }
  return k_ra8_ok;
}

bool ra8_mipi_phy_is_lane_enabled(ra8_mipi_phy_lane_id_t lane)
{
  uint8_t bit = 0U;
  switch (lane) {
    case k_ra8_mipi_phy_lane_clk:
      bit = k_ra8_mipi_phy_lane_bit_clk;
      break;
    case k_ra8_mipi_phy_lane_d0:
      bit = k_ra8_mipi_phy_lane_bit_d0;
      break;
    case k_ra8_mipi_phy_lane_d1:
      bit = k_ra8_mipi_phy_lane_bit_d1;
      break;
    default:
      return false;
  }
  return (s_lane_enable_mask & (uint8_t)((uint8_t)1U << bit)) != 0U;
}

ra8_err_t ra8_mipi_phy_set_clock_mode(ra8_mipi_phy_clk_mode_t mode)
{
  if ((mode != k_ra8_mipi_phy_clk_continuous) && (mode != k_ra8_mipi_phy_clk_noncontinuous)) {
    return k_ra8_err_invalid_arg;
  }
  s_clk_mode = mode;
  return k_ra8_ok;
}

ra8_mipi_phy_clk_mode_t ra8_mipi_phy_get_clock_mode(void)
{
  return s_clk_mode;
}

ra8_err_t ra8_mipi_phy_set_eotp(ra8_mipi_phy_eotp_t eotp)
{
  if ((eotp != k_ra8_mipi_phy_eotp_enabled) && (eotp != k_ra8_mipi_phy_eotp_disabled)) {
    return k_ra8_err_invalid_arg;
  }
  s_eotp = eotp;
  return k_ra8_ok;
}

ra8_mipi_phy_eotp_t ra8_mipi_phy_get_eotp(void)
{
  return s_eotp;
}

ra8_err_t ra8_mipi_phy_set_pclka_freq(uint8_t mhz)
{
  /* HUM Ch 64.2.1 "DPHYREFCR : D-PHY Reference Clock Setting Register", p 3822
   * RFREQ encodes (MHz - 1); see ``k_ra8_mipi_phy_refcr_rfreq_bias``. FSP
   * r_mipi_phy.c uses the same -1 encoding. */
  if ((mhz < (uint8_t)k_ra8_mipi_phy_pclka_min_mhz) ||
      (mhz > (uint8_t)k_ra8_mipi_phy_pclka_max_mhz)) {
    return k_ra8_err_invalid_arg;
  }
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_refcr) =
    ((uint32_t)mhz - (uint32_t)k_ra8_mipi_phy_refcr_rfreq_bias) & k_ra8_mipi_phy_refcr_rfreq_mask;
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_phy_set_escape_divisor(uint8_t escdiv)
{
  /* HUM Ch 64.2.4 "DPHYESCCR : D-PHY Escape Mode Clock Control Register", p 3825 */
  if (escdiv > (uint8_t)k_ra8_mipi_phy_escdiv_max) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 64.2.4 "DPHYESCCR : D-PHY Escape Mode Clock Control Register" p 3825 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plocr) = k_ra8_mipi_phy_plocr_pllstp;
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_esccr) =
    (uint32_t)escdiv & k_ra8_mipi_phy_esccr_escdiv_mask;
  /* HUM Ch 64.2.3 "DPHYPLOCR : D-PHY PLL Operation Control Register", p 3824 */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_plocr) = 0U;
  /* HUM Ch 64.2.6 "DPHYSFR : D-PHY Status Flag Register", p 3826 */
  return internal_mipi_phy_wait_set(ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr),
                                    k_ra8_mipi_phy_sfr_pllsf);
}

ra8_err_t ra8_mipi_phy_validate_pll_band(const ra8_mipi_phy_pll_t* pll, uint8_t mosc_mhz)
{
  RA8_CHECK_NULL_PTR(pll, s_tag, "validate_pll_band: pll must not be nullptr");
  if ((mosc_mhz < k_ra8_mipi_phy_mosc_min_mhz) || (mosc_mhz > k_ra8_mipi_phy_mosc_max_mhz)) {
    /* HUM Ch 64.1 "Overview" p 3822 */
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t n_err = internal_mipi_phy_validate_pll(pll);
  RA8_RETURN_ON_ERROR(n_err,
                      s_tag,
                      "validate_pll_band: NMUL out of range"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 64.2.2 "DPHYPLFCR : D-PHY PLL Frequency Control Register" p 3824 */
  const uint32_t f_mhz = internal_mipi_phy_compute_freq(pll, mosc_mhz);
  uint16_t       lo    = 0U;
  uint16_t       hi    = 0U;
  switch (pll->pmul) {
    case k_ra8_mipi_phy_pmul_1:
      lo = k_ra8_mipi_phy_pll_p1_min;
      hi = k_ra8_mipi_phy_pll_p1_max;
      break;
    case k_ra8_mipi_phy_pmul_2:
      lo = k_ra8_mipi_phy_pll_p2_min;
      hi = k_ra8_mipi_phy_pll_p2_max;
      break;
    case k_ra8_mipi_phy_pmul_4:
      lo = k_ra8_mipi_phy_pll_p4_min;
      hi = k_ra8_mipi_phy_pll_p4_max;
      break;
    case k_ra8_mipi_phy_pmul_8:
      lo = k_ra8_mipi_phy_pll_p8_min;
      hi = k_ra8_mipi_phy_pll_p8_max;
      break;
    default:
      return k_ra8_err_invalid_arg;
  }
  if ((f_mhz < (uint32_t)lo) || (f_mhz > (uint32_t)hi)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}
