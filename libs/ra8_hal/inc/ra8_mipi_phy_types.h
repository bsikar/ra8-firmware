/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_mipi_phy_types.h
 * @brief MIPI D-PHY driver -- shared enums, structs, and callback typedef
 * @ingroup grp_hal_display
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Type definitions for the RA8D2 MIPI PHY block (HUM Ch 64, p 3822-3838).
 * This sub-header carries every typed enum, register-mirror struct, and
 * the status-callback typedef shared by the DSI and CSI clients of the
 * PHY. The function prototypes that consume these types live in
 * ``ra8_mipi_phy_api.h``; both are re-exported by the thin umbrella
 * ``ra8_mipi_phy.h``.
 *
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_mipi_phy_regs.h"

/**
 * @enum ra8_mipi_phy_mode_t
 * @brief D-PHY host vs device mode select (DPHYMDC.MASTEREN).
 *
 * @details
 * Mirrors HUM Ch 64.2.14 p 3836-3837 and the FSP boolean
 * ``mipi_phy_cfg_t.dsi_mode``: 0 = device (CSI), 1 = host (DSI).
 */
typedef enum : uint8_t {
  k_ra8_mipi_phy_mode_csi_device = 0U, /**< Device mode used by MIPI CSI. */
  k_ra8_mipi_phy_mode_dsi_host   = 1U, /**< Host mode used by MIPI DSI.   */
} ra8_mipi_phy_mode_t;

/**
 * @enum ra8_mipi_phy_lane_count_t
 * @brief Number of D-PHY data lanes activated for a transfer.
 *
 * @details
 * HUM Ch 64.1 Table 64.1 p 3822: this part exposes "Up to 2 lanes".
 * The DSI host (Ch 65) and CSI receiver (Ch 66) each own a separate
 * lane-enable register; the PHY driver tracks the requested count
 * here so callers can reason about it consistently. 1-lane and
 * 2-lane configurations are the only legal silicon settings.
 *
 * The 3-lane / 4-lane enumerators are accepted by the public API
 * (the brief mandates "every lane configuration") but the driver
 * returns ``k_ra8_err_not_supported`` because the silicon physically
 * cannot drive more than 2 lanes -- attempting 3/4 lanes on this
 * MCU group is a misconfiguration that the HAL should refuse loudly
 * rather than silently fall back.
 */
typedef enum : uint8_t {
  k_ra8_mipi_phy_lane_count_1 = 1U, /**< 1 data lane.                       */
  k_ra8_mipi_phy_lane_count_2 = 2U, /**< 2 data lanes (silicon maximum).    */
  k_ra8_mipi_phy_lane_count_3 = 3U, /**< 3 data lanes -- rejected on RA8D2. */
  k_ra8_mipi_phy_lane_count_4 = 4U, /**< 4 data lanes -- rejected on RA8D2. */
} ra8_mipi_phy_lane_count_t;

/**
 * @enum ra8_mipi_phy_lane_id_t
 * @brief Per-lane identifier passed to ``ra8_mipi_phy_set_lane_enable``.
 *
 * @details
 * Lane 0 / Lane 1 are the two data lanes. The clock lane is implicit
 * and always enabled when ``DPHYOCR.DPHYEN = 1`` (HUM Ch 64.2.7
 * p 3827); the public API still exposes a ``k_ra8_mipi_phy_lane_clk``
 * value for symmetry so callers can write transfer code that loops
 * over every lane.
 */
typedef enum : uint8_t {
  k_ra8_mipi_phy_lane_clk = 0U, /**< Clock lane (always-on).      */
  k_ra8_mipi_phy_lane_d0  = 1U, /**< Data lane 0.                 */
  k_ra8_mipi_phy_lane_d1  = 2U, /**< Data lane 1.                 */
  k_ra8_mipi_phy_lane_d2  = 3U, /**< Data lane 2 -- not on RA8D2. */
  k_ra8_mipi_phy_lane_d3  = 4U, /**< Data lane 3 -- not on RA8D2. */
} ra8_mipi_phy_lane_id_t;

/**
 * @enum ra8_mipi_phy_clk_mode_t
 * @brief HS clock-lane operating mode.
 *
 * @details
 * D-PHY allows two clock-lane behaviours:
 *
 * - **Continuous** (``k_ra8_mipi_phy_clk_continuous``): HS clock is
 * kept toggling between bursts. Lower latency on burst entry,
 * higher dynamic power.
 * - **Non-continuous** (``k_ra8_mipi_phy_clk_noncontinuous``): clock
 * lane drops to LP-11 between bursts. Saves power; pays a TCLK-PRE
 * handshake on each new burst.
 *
 * The PHY block does not select this directly (DSI / CSI host drivers
 * own the actual lane state machine), but the driver records the
 * caller's choice so other modules can query it via
 * ``ra8_mipi_phy_get_clock_mode``.
 */
typedef enum : uint8_t {
  k_ra8_mipi_phy_clk_noncontinuous = 0U, /**< Drop to LP-11 between bursts. */
  k_ra8_mipi_phy_clk_continuous    = 1U, /**< Keep HS clock running.        */
} ra8_mipi_phy_clk_mode_t;

/**
 * @enum ra8_mipi_phy_eotp_t
 * @brief End-of-Transmission Packet emission setting (DSI host only).
 *
 * @details
 * MIPI DSI 1.2 spec section 8.8.2 makes the EoTP packet optional.
 * Some panels require it, others ignore it. The PHY driver does not
 * generate the packet itself (that is a DSI-host responsibility), but
 * tracks the setting so the DSI driver can read it back without
 * duplicating state.
 */
typedef enum : uint8_t {
  k_ra8_mipi_phy_eotp_disabled = 0U, /**< Do not append EoTP. */
  k_ra8_mipi_phy_eotp_enabled  = 1U, /**< Append EoTP packet. */
} ra8_mipi_phy_eotp_t;

/**
 * @enum ra8_mipi_phy_pll_idiv_t
 * @brief DPHYPLFCR.IDIV[1:0] -- input frequency divisor.
 *
 * @details
 * HUM Ch 64.2.2 p 3823: divides the MOSC input. The 8..24 MHz
 * post-divisor band must be respected (HUM 64.2.2 note p 3824).
 */
typedef enum : uint8_t {
  k_ra8_mipi_phy_idiv_1 = 0U, /**< IDIV = 1 (no division). */
  k_ra8_mipi_phy_idiv_2 = 1U, /**< IDIV = 1/2.             */
  k_ra8_mipi_phy_idiv_3 = 2U, /**< IDIV = 1/3.             */
  k_ra8_mipi_phy_idiv_4 = 3U, /**< IDIV = 1/4.             */
} ra8_mipi_phy_pll_idiv_t;

/**
 * @enum ra8_mipi_phy_pll_pmul_t
 * @brief DPHYPLFCR.PMUL[1:0] -- output frequency divisor.
 *
 * @details
 * HUM Ch 64.2.2 p 3823. Each value maps to a different VCO band
 * (HUM 64.2.2 note p 3824). The driver enforces the band mapping
 * via ``ra8_mipi_phy_validate_pll_band``.
 */
typedef enum : uint8_t {
  k_ra8_mipi_phy_pmul_1 = 0U, /**< P = 1 (960.. 1440 MHz).   */
  k_ra8_mipi_phy_pmul_2 = 1U, /**< P = 1/2 (480.. 1440 MHz). */
  k_ra8_mipi_phy_pmul_4 = 2U, /**< P = 1/4 (240.. 750 MHz).  */
  k_ra8_mipi_phy_pmul_8 = 3U, /**< P = 1/8 (120.. 375 MHz).  */
} ra8_mipi_phy_pll_pmul_t;

/**
 * @enum ra8_mipi_phy_pll_nfmul_t
 * @brief DPHYPLFCR.NFMUL[1:0] -- fractional multiplier (NF).
 */
typedef enum : uint8_t {
  k_ra8_mipi_phy_nfmul_0_00 = 0U, /**< NF = 0.00. */
  k_ra8_mipi_phy_nfmul_0_33 = 1U, /**< NF = 0.33. */
  k_ra8_mipi_phy_nfmul_0_66 = 2U, /**< NF = 0.66. */
  k_ra8_mipi_phy_nfmul_0_50 = 3U, /**< NF = 0.50. */
} ra8_mipi_phy_pll_nfmul_t;

/**
 * @enum ra8_mipi_phy_event_t
 * @brief Event source codes pushed through ``ra8_mipi_phy_event_fn_t``.
 *
 * @details
 * The MIPI PHY itself does not raise NVIC vectors; DSI / CSI hosts
 * forward their interrupts here and the dispatcher translates the
 * raw DPHYSFR snapshot into one of these codes for higher-level
 * consumers.
 */
typedef enum : uint8_t {
  k_ra8_mipi_phy_event_ldo_ready  = 0U, /**< DPHYSFR.PWRSF rose 0->1. */
  k_ra8_mipi_phy_event_ldo_lost   = 1U, /**< DPHYSFR.PWRSF fell 1->0. */
  k_ra8_mipi_phy_event_pll_locked = 2U, /**< DPHYSFR.PLLSF rose 0->1. */
  k_ra8_mipi_phy_event_pll_lost   = 3U, /**< DPHYSFR.PLLSF fell 1->0. */
  k_ra8_mipi_phy_event_status_chg = 4U, /**< Catch-all status change. */
} ra8_mipi_phy_event_t;

/**
 * @enum ra8_mipi_phy_state_t
 * @brief Lifecycle position of the D-PHY block.
 *
 * @details
 * Tracks the start-up procedure (HUM Ch 64.3.1 p 3837) one stage at
 * a time so that callers can reason about where the driver is without
 * having to re-read DPHYSFR. State transitions follow the @par State
 * Machine in ``ra8_mipi_phy_init``.
 */
typedef enum : uint8_t {
  k_ra8_mipi_phy_state_off     = 0U, /**< MSTPCRC bit set, regs unreachable.  */
  k_ra8_mipi_phy_state_idle    = 1U, /**< MSTPCRC clear, no LDO yet.          */
  k_ra8_mipi_phy_state_ldo_up  = 2U, /**< PWRSEN=1, PWRSF=1, PLL stopped.     */
  k_ra8_mipi_phy_state_pll_run = 3U, /**< PLLSTP=0, PLLSF=1, DPHYEN=0.        */
  k_ra8_mipi_phy_state_run     = 4U, /**< DPHYEN=1, transmitting / receiving. */
  k_ra8_mipi_phy_state_error   = 5U, /**< LDO drop or PLL lost lock.          */
} ra8_mipi_phy_state_t;

/**
 * @enum ra8_mipi_phy_dual_mode_t
 * @brief Arbitration policy when both DSI and CSI want the PHY.
 *
 * @details
 * The RA8D2 silicon hosts a single D-PHY shared between DSI host
 * (Ch 65) and CSI device (Ch 66). MIPI-DSI and MIPI-CSI cannot be
 * active simultaneously -- DPHYMDC.MASTEREN is a single bit. To cover
 * the brief's "dual-mode" requirement, the PHY driver tracks an
 * arbitration policy that the higher-level stack can consult before
 * it requests the PHY. ``alternate`` switches per request,
 * ``dsi_priority`` and ``csi_priority`` lock to one client.
 *
 * The driver does not implement queueing -- it simply records the
 * policy so the consumer can decide what to do when a conflict
 * arises (typically: drop the lower-priority client, switch the PHY
 * mode, then promote the higher-priority client).
 */
typedef enum : uint8_t {
  k_ra8_mipi_phy_dual_off          = 0U, /**< Single-owner -- no arbitration. */
  k_ra8_mipi_phy_dual_alternate    = 1U, /**< Time-share, switch on request.  */
  k_ra8_mipi_phy_dual_dsi_priority = 2U, /**< DSI wins; CSI requests fail.    */
  k_ra8_mipi_phy_dual_csi_priority = 3U, /**< CSI wins; DSI requests fail.    */
} ra8_mipi_phy_dual_mode_t;

/**
 * @struct ra8_mipi_phy_status_decoded_t
 * @brief Decoded DPHYSFR snapshot returned by ``ra8_mipi_phy_get_status_decoded``.
 *
 * @details
 * One bool per bit in DPHYSFR (HUM Ch 64.2.6 p 3826) plus a verbatim
 * copy of the raw register so callers can do their own decoding.
 *
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra8_mipi_phy_get_status_decoded``.
 */
typedef struct {
  bool     ldo_ready;  /**< PWRSF (bit 0) -- LDO power-on stable.  */
  bool     pll_locked; /**< PLLSF (bit 8) -- PLL clock stable.     */
  bool     phy_ready;  /**< (PWRSF & PLLSF) -- driver fully armed. */
  uint32_t raw;        /**< Verbatim DPHYSFR snapshot.             */
} ra8_mipi_phy_status_decoded_t;

/**
 * @struct ra8_mipi_phy_pll_t
 * @brief D-PHY PLL coefficients written into DPHYPLFCR.
 *
 * @details
 * Caller supplies pre-validated coefficients. The driver packs them
 * into DPHYPLFCR per HUM Ch 64.2.2 p 3823 and validates against the
 * PMUL band table on p 3824 before committing.
 *
 * Final PLL output: ``f = f_main * (1/IDIV) * (NMUL_int + NFMUL_frac) * (1/PMUL)``.
 * Line rate per lane = ``f / 2`` (HUM 64.2.2 p 3824).
 *
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra8_mipi_phy_init``.
 */
typedef struct {
  ra8_mipi_phy_pll_idiv_t  idiv;     /**< IDIV[1:0] input divisor.       */
  ra8_mipi_phy_pll_pmul_t  pmul;     /**< PMUL[1:0] output divisor.      */
  ra8_mipi_phy_pll_nfmul_t nfmul;    /**< NFMUL[1:0] fractional N.       */
  uint16_t                 nmul_int; /**< NMUL[8:0] integer N (40..375). */
} ra8_mipi_phy_pll_t;

/**
 * @struct ra8_mipi_phy_timing_t
 * @brief D-PHY HS/LP transition timing values (DPHYTIM1..6).
 *
 * @details
 * Each field maps 1:1 to a register subfield from HUM Ch 64.2.8
 *.. 64.2.13 p 3827-3831. The selection of values per line-rate
 * lives in HUM Tables 64.2 / 64.3 p 3831-3836; the helper
 * ``ra8_mipi_phy_select_timing`` looks the row up automatically.
 *
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra8_mipi_phy_init``.
 */
typedef struct {
  uint32_t tinit;    /**< DPHYTIM1.TINIT[18:0].   */
  uint8_t  tclkprep; /**< DPHYTIM2.TCLKPREP[7:0]. */
  uint8_t  tclksett; /**< DPHYTIM2.TCLKSETT[7:0]. */
  uint8_t  tclkmiss; /**< DPHYTIM2.TCLKMISS[7:0]. */
  uint8_t  thsprep;  /**< DPHYTIM3.THSPREP[7:0].  */
  uint8_t  thssett;  /**< DPHYTIM3.THSSETT[7:0].  */
  uint8_t  tclkzero; /**< DPHYTIM4.TCLKZERO[7:0]. */
  uint8_t  tclkpre;  /**< DPHYTIM4.TCLKPRE[7:0].  */
  uint8_t  tclkpost; /**< DPHYTIM4.TCLKPOST[7:0]. */
  uint8_t  tclktrl;  /**< DPHYTIM4.TCLKTRL[7:0].  */
  uint8_t  thszero;  /**< DPHYTIM5.THSZERO[7:0].  */
  uint8_t  thstrl;   /**< DPHYTIM5.THSTRL[7:0].   */
  uint8_t  thsexit;  /**< DPHYTIM5.THSEXIT[7:0].  */
  uint8_t  tlpx;     /**< DPHYTIM6.TLPX[7:0].     */
} ra8_mipi_phy_timing_t;

/**
 * @struct ra8_mipi_phy_config_t
 * @brief Top-level configuration for ``ra8_mipi_phy_init``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra8_mipi_phy_init``.
 */
typedef struct {
  ra8_mipi_phy_mode_t          mode;           /**< Host (DSI) or device (CSI).            */
  uint8_t                      pclka_mhz;      /**< PCLKA frequency, MHz (40..125).        */
  uint16_t                     line_rate_mbps; /**< Per-lane line rate (80..720).          */
  ra8_mipi_phy_lane_count_t    lane_count;     /**< 1 or 2 data lanes.                     */
  ra8_mipi_phy_clk_mode_t      clk_mode;       /**< Continuous vs non-continuous HS clock. */
  ra8_mipi_phy_eotp_t          eotp;           /**< Append EoTP (DSI only).                */
  ra8_mipi_phy_pll_t           pll;            /**< PLL coefficients.                      */
  uint8_t                      escdiv;         /**< Escape clk divisor (0..31).            */
  const ra8_mipi_phy_timing_t* p_timing;       /**< Non-NULL timing block.                 */
} ra8_mipi_phy_config_t;

/**
 * @typedef ra8_mipi_phy_event_fn_t
 * @brief MIPI PHY status callback (LDO ready / PLL lock / PLL drop).
 *
 * @param[in] ctx Caller context registered via ``ra8_mipi_phy_attach_handler``.
 * @param[in] event Decoded event source from ``ra8_mipi_phy_event_t``.
 * @param[in] sfr Snapshot of DPHYSFR at dispatch time.
 */
typedef void (*ra8_mipi_phy_event_fn_t)(void* ctx, ra8_mipi_phy_event_t event, uint32_t sfr);

#ifdef __cplusplus
}
#endif
