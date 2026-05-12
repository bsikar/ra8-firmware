/**
 * @file ra_mipi_phy.h
 * @brief MIPI D-PHY driver -- physical layer shared by DSI and CSI
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Driver for the RA8D2 MIPI PHY block (HUM Ch 64, p 3822-3838).
 * The PHY is the physical layer underneath both the MIPI DSI host
 * (Ch 65) and the MIPI CSI receiver (Ch 66). Either client may own
 * the PHY at a given time; the driver explicitly tracks the active
 * mode so the PHY can be reprogrammed between DSI and CSI use cases
 * (see ``ra_mipi_phy_switch_mode``).
 *
 * The full surface in this revision covers every documented
 * register field and every documented operating mode:
 *
 * - **Lifecycle** -- ``init``, ``deinit``, ``reset``, ``enter_stop``,
 * ``exit_stop``, ``recover_from_error``.
 * - **Mode control** -- ``switch_mode`` (DSI<->CSI without full
 * re-init), continuous-clock mode, EoTP enable/disable.
 * - **Lane configuration** -- ``set_lane_count`` (1/2 lanes), per-lane
 * enable/disable, ``set_lane_speed`` (PLL re-tune).
 * - **PLL / power** -- ``pll_start``, ``pll_stop``, ``ldo_enable``,
 * ``ldo_disable``, ``set_escape_divisor``, ``set_pclka_freq``.
 * - **Timing tables** -- ``select_timing`` looks up Tables 64.2 / 64.3
 * from HUM p 3831-3836 by (mode, PCLKA, lane rate) and applies the
 * corresponding ``ra_mipi_phy_timing_t`` snapshot.
 * - **Status & IRQ** -- ``get_status``, ``clear_status``,
 * ``attach_handler`` / ``dispatch``, ``is_pll_locked``,
 * ``is_ldo_stable``, ``wait_ready``.
 *
 * # Module-stop note
 *
 * HUM Ch 64.4.2 p 3838: MIPI PHY is gated by MSTPCRC. There is no
 * ``k_ra_mstp_mipi_phy`` entry in ``libs/ra_hal/inc/ra8d2_mstp_regs.h``
 * yet. This driver does NOT add one (touching a shared enum file is
 * forbidden); ``ra_mipi_phy_init`` performs a direct MSTPCRC write
 * with a TODO marker so a later wave can promote it into ``ra_mstp_t``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_mipi_phy_regs.h"
#include "ra_err.h"

/**
 * @enum ra_mipi_phy_mode_t
 * @brief D-PHY master vs slave mode select (DPHYMDC.MASTEREN).
 *
 * @details
 * Mirrors HUM Ch 64.2.14 p 3836-3837 and the FSP boolean
 * ``mipi_phy_cfg_t.dsi_mode``: 0 = slave (CSI), 1 = master (DSI).
 */
typedef enum : uint8_t {
  k_ra_mipi_phy_mode_csi_slave  = 0U, /**< Slave mode used by MIPI CSI. */
  k_ra_mipi_phy_mode_dsi_master = 1U, /**< Master mode used by MIPI DSI. */
} ra_mipi_phy_mode_t;

/**
 * @enum ra_mipi_phy_lane_count_t
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
 * returns ``k_ra_err_not_supported`` because the silicon physically
 * cannot drive more than 2 lanes -- attempting 3/4 lanes on this
 * MCU group is a misconfiguration that the HAL should refuse loudly
 * rather than silently fall back.
 */
typedef enum : uint8_t {
  k_ra_mipi_phy_lane_count_1 = 1U, /**< 1 data lane. */
  k_ra_mipi_phy_lane_count_2 = 2U, /**< 2 data lanes (silicon maximum). */
  k_ra_mipi_phy_lane_count_3 = 3U, /**< 3 data lanes -- rejected on RA8D2. */
  k_ra_mipi_phy_lane_count_4 = 4U, /**< 4 data lanes -- rejected on RA8D2. */
} ra_mipi_phy_lane_count_t;

/**
 * @enum ra_mipi_phy_lane_id_t
 * @brief Per-lane identifier passed to ``ra_mipi_phy_set_lane_enable``.
 *
 * @details
 * Lane 0 / Lane 1 are the two data lanes. The clock lane is implicit
 * and always enabled when ``DPHYOCR.DPHYEN = 1`` (HUM Ch 64.2.7
 * p 3827); the public API still exposes a ``k_ra_mipi_phy_lane_clk``
 * value for symmetry so callers can write transfer code that loops
 * over every lane.
 */
typedef enum : uint8_t {
  k_ra_mipi_phy_lane_clk = 0U, /**< Clock lane (always-on). */
  k_ra_mipi_phy_lane_d0  = 1U, /**< Data lane 0. */
  k_ra_mipi_phy_lane_d1  = 2U, /**< Data lane 1. */
  k_ra_mipi_phy_lane_d2  = 3U, /**< Data lane 2 -- not on RA8D2. */
  k_ra_mipi_phy_lane_d3  = 4U, /**< Data lane 3 -- not on RA8D2. */
} ra_mipi_phy_lane_id_t;

/**
 * @enum ra_mipi_phy_clk_mode_t
 * @brief HS clock-lane operating mode.
 *
 * @details
 * D-PHY allows two clock-lane behaviours:
 *
 * - **Continuous** (``k_ra_mipi_phy_clk_continuous``): HS clock is
 * kept toggling between bursts. Lower latency on burst entry,
 * higher dynamic power.
 * - **Non-continuous** (``k_ra_mipi_phy_clk_noncontinuous``): clock
 * lane drops to LP-11 between bursts. Saves power; pays a TCLK-PRE
 * handshake on each new burst.
 *
 * The PHY block does not select this directly (DSI / CSI host drivers
 * own the actual lane state machine), but the driver records the
 * caller's choice so other modules can query it via
 * ``ra_mipi_phy_get_clock_mode``.
 */
typedef enum : uint8_t {
  k_ra_mipi_phy_clk_noncontinuous = 0U, /**< Drop to LP-11 between bursts. */
  k_ra_mipi_phy_clk_continuous    = 1U, /**< Keep HS clock running. */
} ra_mipi_phy_clk_mode_t;

/**
 * @enum ra_mipi_phy_eotp_t
 * @brief End-of-Transmission Packet emission setting (DSI master only).
 *
 * @details
 * MIPI DSI 1.2 spec section 8.8.2 makes the EoTP packet optional.
 * Some panels require it, others ignore it. The PHY driver does not
 * generate the packet itself (that is a DSI-host responsibility), but
 * tracks the setting so the DSI driver can read it back without
 * duplicating state.
 */
typedef enum : uint8_t {
  k_ra_mipi_phy_eotp_disabled = 0U, /**< Do not append EoTP. */
  k_ra_mipi_phy_eotp_enabled  = 1U, /**< Append EoTP packet. */
} ra_mipi_phy_eotp_t;

/**
 * @enum ra_mipi_phy_pll_idiv_t
 * @brief DPHYPLFCR.IDIV[1:0] -- input frequency divisor.
 *
 * @details
 * HUM Ch 64.2.2 p 3823: divides the MOSC input. The 8..24 MHz
 * post-divisor band must be respected (HUM 64.2.2 note p 3824).
 */
typedef enum : uint8_t {
  k_ra_mipi_phy_idiv_1 = 0U, /**< IDIV = 1 (no division). */
  k_ra_mipi_phy_idiv_2 = 1U, /**< IDIV = 1/2. */
  k_ra_mipi_phy_idiv_3 = 2U, /**< IDIV = 1/3. */
  k_ra_mipi_phy_idiv_4 = 3U, /**< IDIV = 1/4. */
} ra_mipi_phy_pll_idiv_t;

/**
 * @enum ra_mipi_phy_pll_pmul_t
 * @brief DPHYPLFCR.PMUL[1:0] -- output frequency divisor.
 *
 * @details
 * HUM Ch 64.2.2 p 3823. Each value maps to a different VCO band
 * (HUM 64.2.2 note p 3824). The driver enforces the band mapping
 * via ``ra_mipi_phy_validate_pll_band``.
 */
typedef enum : uint8_t {
  k_ra_mipi_phy_pmul_1 = 0U, /**< P = 1 (960.. 1440 MHz). */
  k_ra_mipi_phy_pmul_2 = 1U, /**< P = 1/2 (480.. 1440 MHz). */
  k_ra_mipi_phy_pmul_4 = 2U, /**< P = 1/4 (240.. 750 MHz). */
  k_ra_mipi_phy_pmul_8 = 3U, /**< P = 1/8 (120.. 375 MHz). */
} ra_mipi_phy_pll_pmul_t;

/**
 * @enum ra_mipi_phy_pll_nfmul_t
 * @brief DPHYPLFCR.NFMUL[1:0] -- fractional multiplier (NF).
 */
typedef enum : uint8_t {
  k_ra_mipi_phy_nfmul_0_00 = 0U, /**< NF = 0.00. */
  k_ra_mipi_phy_nfmul_0_33 = 1U, /**< NF = 0.33. */
  k_ra_mipi_phy_nfmul_0_66 = 2U, /**< NF = 0.66. */
  k_ra_mipi_phy_nfmul_0_50 = 3U, /**< NF = 0.50. */
} ra_mipi_phy_pll_nfmul_t;

/**
 * @enum ra_mipi_phy_event_t
 * @brief Event source codes pushed through ``ra_mipi_phy_event_fn_t``.
 *
 * @details
 * The MIPI PHY itself does not raise NVIC vectors; DSI / CSI hosts
 * forward their interrupts here and the dispatcher translates the
 * raw DPHYSFR snapshot into one of these codes for higher-level
 * consumers.
 */
typedef enum : uint8_t {
  k_ra_mipi_phy_event_ldo_ready  = 0U, /**< DPHYSFR.PWRSF rose 0->1. */
  k_ra_mipi_phy_event_ldo_lost   = 1U, /**< DPHYSFR.PWRSF fell 1->0. */
  k_ra_mipi_phy_event_pll_locked = 2U, /**< DPHYSFR.PLLSF rose 0->1. */
  k_ra_mipi_phy_event_pll_lost   = 3U, /**< DPHYSFR.PLLSF fell 1->0. */
  k_ra_mipi_phy_event_status_chg = 4U, /**< Catch-all status change. */
} ra_mipi_phy_event_t;

/**
 * @enum ra_mipi_phy_state_t
 * @brief Lifecycle position of the D-PHY block.
 *
 * @details
 * Tracks the start-up procedure (HUM Ch 64.3.1 p 3837) one stage at
 * a time so that callers can reason about where the driver is without
 * having to re-read DPHYSFR. State transitions follow the @par State
 * Machine in ``ra_mipi_phy_init``.
 */
typedef enum : uint8_t {
  k_ra_mipi_phy_state_off     = 0U, /**< MSTPCRC bit set, regs unreachable. */
  k_ra_mipi_phy_state_idle    = 1U, /**< MSTPCRC clear, no LDO yet. */
  k_ra_mipi_phy_state_ldo_up  = 2U, /**< PWRSEN=1, PWRSF=1, PLL stopped. */
  k_ra_mipi_phy_state_pll_run = 3U, /**< PLLSTP=0, PLLSF=1, DPHYEN=0. */
  k_ra_mipi_phy_state_run     = 4U, /**< DPHYEN=1, transmitting / receiving. */
  k_ra_mipi_phy_state_error   = 5U, /**< LDO drop or PLL lost lock. */
} ra_mipi_phy_state_t;

/**
 * @enum ra_mipi_phy_dual_mode_t
 * @brief Arbitration policy when both DSI and CSI want the PHY.
 *
 * @details
 * The RA8D2 silicon hosts a single D-PHY shared between DSI master
 * (Ch 65) and CSI slave (Ch 66). MIPI-DSI and MIPI-CSI cannot be
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
  k_ra_mipi_phy_dual_off          = 0U, /**< Single-owner -- no arbitration. */
  k_ra_mipi_phy_dual_alternate    = 1U, /**< Time-share, switch on request. */
  k_ra_mipi_phy_dual_dsi_priority = 2U, /**< DSI wins; CSI requests fail. */
  k_ra_mipi_phy_dual_csi_priority = 3U, /**< CSI wins; DSI requests fail. */
} ra_mipi_phy_dual_mode_t;

/**
 * @struct ra_mipi_phy_status_decoded_t
 * @brief Decoded DPHYSFR snapshot returned by ``ra_mipi_phy_get_status_decoded``.
 *
 * @details
 * One bool per bit in DPHYSFR (HUM Ch 64.2.6 p 3826) plus a verbatim
 * copy of the raw register so callers can do their own decoding.
 *
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra_mipi_phy_get_status_decoded``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  bool     ldo_ready;  /**< PWRSF (bit 0) -- LDO power-on stable. */
  bool     pll_locked; /**< PLLSF (bit 8) -- PLL clock stable. */
  bool     phy_ready;  /**< (PWRSF & PLLSF) -- driver fully armed. */
  uint32_t raw;        /**< Verbatim DPHYSFR snapshot. */
} ra_mipi_phy_status_decoded_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_mipi_phy_pll_t
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
 * each member is read in ``ra_mipi_phy_init``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_mipi_phy_pll_idiv_t  idiv;     /**< IDIV[1:0] input divisor. */
  ra_mipi_phy_pll_pmul_t  pmul;     /**< PMUL[1:0] output divisor. */
  ra_mipi_phy_pll_nfmul_t nfmul;    /**< NFMUL[1:0] fractional N. */
  uint16_t                nmul_int; /**< NMUL[8:0] integer N (40..375). */
} ra_mipi_phy_pll_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_mipi_phy_timing_t
 * @brief D-PHY HS/LP transition timing values (DPHYTIM1..6).
 *
 * @details
 * Each field maps 1:1 to a register subfield from HUM Ch 64.2.8
 *.. 64.2.13 p 3827-3831. The selection of values per line-rate
 * lives in HUM Tables 64.2 / 64.3 p 3831-3836; the helper
 * ``ra_mipi_phy_select_timing`` looks the row up automatically.
 *
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra_mipi_phy_init``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint32_t tinit;    /**< DPHYTIM1.TINIT[18:0]. */
  uint8_t  tclkprep; /**< DPHYTIM2.TCLKPREP[7:0]. */
  uint8_t  tclksett; /**< DPHYTIM2.TCLKSETT[7:0]. */
  uint8_t  tclkmiss; /**< DPHYTIM2.TCLKMISS[7:0]. */
  uint8_t  thsprep;  /**< DPHYTIM3.THSPREP[7:0]. */
  uint8_t  thssett;  /**< DPHYTIM3.THSSETT[7:0]. */
  uint8_t  tclkzero; /**< DPHYTIM4.TCLKZERO[7:0]. */
  uint8_t  tclkpre;  /**< DPHYTIM4.TCLKPRE[7:0]. */
  uint8_t  tclkpost; /**< DPHYTIM4.TCLKPOST[7:0]. */
  uint8_t  tclktrl;  /**< DPHYTIM4.TCLKTRL[7:0]. */
  uint8_t  thszero;  /**< DPHYTIM5.THSZERO[7:0]. */
  uint8_t  thstrl;   /**< DPHYTIM5.THSTRL[7:0]. */
  uint8_t  thsexit;  /**< DPHYTIM5.THSEXIT[7:0]. */
  uint8_t  tlpx;     /**< DPHYTIM6.TLPX[7:0]. */
} ra_mipi_phy_timing_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_mipi_phy_config_t
 * @brief Top-level configuration for ``ra_mipi_phy_init``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra_mipi_phy_init``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_mipi_phy_mode_t          mode;           /**< Master (DSI) or slave (CSI). */
  uint8_t                     pclka_mhz;      /**< PCLKA frequency, MHz (40..125). */
  uint16_t                    line_rate_mbps; /**< Per-lane line rate (80..720). */
  ra_mipi_phy_lane_count_t    lane_count;     /**< 1 or 2 data lanes. */
  ra_mipi_phy_clk_mode_t      clk_mode;       /**< Continuous vs non-continuous HS clock. */
  ra_mipi_phy_eotp_t          eotp;           /**< Append EoTP (DSI only). */
  ra_mipi_phy_pll_t           pll;            /**< PLL coefficients. */
  uint8_t                     escdiv;         /**< Escape clk divisor (0..31). */
  const ra_mipi_phy_timing_t* p_timing;       /**< Non-NULL timing block. */
} ra_mipi_phy_config_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @typedef ra_mipi_phy_event_fn_t
 * @brief MIPI PHY status callback (LDO ready / PLL lock / PLL drop).
 *
 * @param[in] ctx Caller context registered via ``ra_mipi_phy_attach_handler``.
 * @param[in] event Decoded event source from ``ra_mipi_phy_event_t``.
 * @param[in] sfr Snapshot of DPHYSFR at dispatch time.
 */
typedef void (*ra_mipi_phy_event_fn_t)(void* ctx, ra_mipi_phy_event_t event, uint32_t sfr);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Bring up the D-PHY end-to-end per HUM Ch 64.3.1 p 3837.
 *
 * @details
 * Executes the eleven-step start-up procedure:
 *
 * 1. Ungate the MIPI PHY module (MSTPCRC bit cleared via direct
 * register write -- see file header note about MSTP enum).
 * 2. Write ``DPHYMDC.MASTEREN`` to select master vs slave.
 * 3. Write ``DPHYREFCR.RFREQ`` from ``cfg->pclka_mhz``.
 * 4. Set ``DPHYPWRCR.PWRSEN = 1`` to power the LDO.
 * 5. Spin until ``DPHYSFR.PWRSF`` reads 1 or the spin budget runs
 * out (HUM 64.3.1 step 5).
 * 6. (Master only) write ``DPHYPLFCR`` from ``cfg->pll``.
 * 7. (Master only) write ``DPHYESCCR.ESCDIV``.
 * 8. (Master only) clear ``DPHYPLOCR.PLLSTP`` to release the PLL.
 * 9. (Master only) spin until ``DPHYSFR.PLLSF`` reads 1.
 * 10. Write ``DPHYTIM1..6`` from ``cfg->p_timing``.
 * 11. Set ``DPHYOCR.DPHYEN = 1``.
 *
 * @param[in] cfg Non-NULL configuration block.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok D-PHY enabled, LDO + PLL stable.
 * @retval k_ra_err_null_ptr ``cfg`` or ``cfg->p_timing`` was NULL.
 * @retval k_ra_err_invalid_arg ``pclka_mhz`` outside 40..125 (HUM 64.2.1
 * p 3822) or PLL fields out of range.
 * @retval k_ra_err_not_supported ``lane_count`` > 2.
 * @retval k_ra_err_hw_timeout LDO or PLL did not stabilise in time.
 *
 * @pre Graphics power domain is on and PCLKA is supplied to the
 * MIPI subsystem (HUM Ch 64.3.1 step 1, p 3837).
 * @pre IRQs masked or single-threaded init context (the driver
 * sequences several register writes that must not be torn).
 *
 * @post ``DPHYOCR.DPHYEN`` reads 1.
 * @post For master mode, ``DPHYSFR.PLLSF`` reads 1.
 *
 * @par State Machine
 * @startuml
 * [*] --> Off
 * Off: MSTPCRC bit set, regs unreachable
 * Off --> Idle: init step 1 (mstp clear)
 * Idle --> LdoUp: PWRSEN=1
 * LdoUp --> PllRun: PLLSTP=0 (master)
 * LdoUp --> Run: DPHYEN=1 (slave)
 * PllRun --> Run: DPHYEN=1 (master)
 * Run --> Off: deinit
 * Run --> LdoUp: pll_stop
 * @enduml
 *
 * @note Thread safety: not thread-safe.
 * @see ra_mipi_phy_deinit
 * @see HUM Ch 64.3.1 p 3837 "D-PHY Startup Procedure"
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_init(const ra_mipi_phy_config_t* cfg);

/**
 * @brief Stop the D-PHY per HUM Ch 64.3.2 p 3837 (DPHYEN -> PLLSTP -> PWRSEN).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok All three writes succeeded.
 *
 * @pre ``ra_mipi_phy_init`` was previously called or the block is
 * already in its post-reset state (in which case the writes
 * are idempotent no-ops).
 * @post ``DPHYOCR.DPHYEN`` and ``DPHYPWRCR.PWRSEN`` both read 0.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_deinit(void);

/**
 * @brief Reset the PHY to its post-MSTPCRC-clear state without re-init.
 *
 * @details
 * Clears DPHYOCR / DPHYPLOCR / DPHYPWRCR and zeros every timing
 * register (DPHYTIM1..6) and DPHYPLFCR. Useful when the higher-level
 * stack hits an unrecoverable error and wants to start over without
 * paying for a full module-stop cycle.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok always.
 *
 * @pre -- (no preconditions).
 * @post DPHYOCR / DPHYPWRCR / DPHYPLOCR / DPHYTIMx / DPHYPLFCR all
 * read their reset values.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_reset(void);

/**
 * @brief Reset and re-arm the PHY after an LDO drop / PLL unlock.
 *
 * @param[in] cfg Same configuration that was passed to the original
 * ``ra_mipi_phy_init``.
 *
 * @return ``ra_err_t`` error code (see ``ra_mipi_phy_init``).
 *
 * @pre ``cfg`` non-NULL.
 * @pre The PHY was previously initialized at least once.
 * @post On success, DPHYEN reads 1 and PLL is locked.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_recover_from_error(const ra_mipi_phy_config_t* cfg);

/* =============================================================================
 * Status
 * =============================================================================
 */

/**
 * @brief Read the D-PHY status flags (DPHYSFR).
 *
 * @param[out] out_mask Receives a copy of DPHYSFR (PWRSF | PLLSF).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok ``*out_mask`` updated.
 * @retval k_ra_err_null_ptr ``out_mask`` was NULL.
 *
 * @pre ``out_mask`` is non-NULL.
 * @post ``*out_mask`` reflects the live register state.
 *
 * @note Thread safety: read-only access, safe under simple races.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_get_status(uint32_t* out_mask);

/**
 * @brief Clear status flags (no-op on this part).
 *
 * @details
 * DPHYSFR is read-only (HUM Ch 64.2.6 p 3826) -- the bits are
 * driven by hardware ready signals and cannot be cleared by
 * software. This entry point exists so ``ra_mipi_phy`` mirrors
 * the API shape of peer drivers.
 *
 * @param[in] mask Bits the caller would like cleared (ignored).
 *
 * @return ``k_ra_ok`` always.
 *
 * @pre -- (no requirements; the function ignores its argument).
 * @post Hardware state is unchanged.
 *
 * @note Thread safety: trivially safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_clear_status(uint32_t mask);

/**
 * @brief Test whether the LDO has stabilised (DPHYSFR.PWRSF = 1).
 *
 * @details
 * Reads the DPHYSFR status register (HUM Ch 64.2 "D-PHY Status",
 * p 3835) and returns the PWRSF bit so callers can poll without
 * unpacking the register layout.
 *
 * @return ``true`` if PWRSF is set, ``false`` otherwise.
 * @retval true  PWRSF asserted -- LDO output is in regulation.
 * @retval false PWRSF de-asserted -- LDO still settling or off.
 *
 * @pre -- (no preconditions; safe before ``init``).
 * @pre LDO power supply present at MCU pin VL_DPHY.
 * @post Hardware state is unchanged.
 * @post No DPHYSFR write was issued.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
bool ra_mipi_phy_is_ldo_stable(void);

/**
 * @brief Test whether the PLL is locked (DPHYSFR.PLLSF = 1).
 *
 * @details
 * Reads DPHYSFR (HUM Ch 64.2 "D-PHY Status", p 3835) and returns
 * the PLLSF bit so callers can poll without unpacking the register
 * layout.
 *
 * @return ``true`` if PLLSF is set, ``false`` otherwise.
 * @retval true  PLLSF asserted -- D-PHY PLL is locked.
 * @retval false PLLSF de-asserted -- PLL not yet locked or unlocked.
 *
 * @pre -- (no preconditions; safe before ``init``).
 * @pre PLL has been programmed via ``ra_mipi_phy_init``.
 * @post Hardware state is unchanged.
 * @post No DPHYSFR write was issued.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
bool ra_mipi_phy_is_pll_locked(void);

/**
 * @brief Bounded poll until the PHY is fully ready (PWRSF & PLLSF).
 *
 * @return ``k_ra_ok`` if ready, ``k_ra_err_hw_timeout`` if not.
 *
 * @pre -- (no preconditions).
 * @post DPHYSFR has been read at least once.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_wait_ready(void);

/* =============================================================================
 * Interrupt path
 * =============================================================================
 */

/**
 * @brief Attach a status callback fired by ``ra_mipi_phy_dispatch``.
 *
 * @param[in] fn Callback. NULL detaches.
 * @param[in] ctx Opaque value forwarded to the callback.
 *
 * @return ``k_ra_ok`` always.
 *
 * @pre -- (NULL fn is the documented detach signal).
 * @post Subsequent ``ra_mipi_phy_dispatch`` calls see ``fn`` / ``ctx``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_attach_handler(ra_mipi_phy_event_fn_t fn, void* ctx);

/**
 * @brief Read DPHYSFR, snapshot it, decode events, and fire the callback.
 *
 * @details
 * The PHY itself does not raise an NVIC vector -- DSI / CSI do.
 * The DSI / CSI driver may forward its IRQ here so any consumer
 * can observe LDO / PLL transitions through one common slot. This
 * dispatcher compares the current DPHYSFR against the previous
 * snapshot and emits one event per detected edge:
 *
 * - PWRSF rising -> ``k_ra_mipi_phy_event_ldo_ready``
 * - PWRSF falling -> ``k_ra_mipi_phy_event_ldo_lost``
 * - PLLSF rising -> ``k_ra_mipi_phy_event_pll_locked``
 * - PLLSF falling -> ``k_ra_mipi_phy_event_pll_lost``
 *
 * If neither bit moved, a single ``k_ra_mipi_phy_event_status_chg``
 * is emitted so the consumer at least knows the PHY was polled.
 *
 * @pre -- (no preconditions; tolerates the callback being NULL).
 * @pre Called from ISR context, host-test driver or polling task.
 * @post DPHYSFR has been read once; no register has been written.
 * @post Internal status snapshot is updated for the next edge compare.
 *
 * @note Thread safety: not thread-safe. See HUM Ch 64.2 p 3835.
 * @since 0.1.0
 */
void ra_mipi_phy_dispatch(void);

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @brief Pre-standby tear-down: disable D-PHY then drop the LDO.
 *
 * @details
 * HUM Ch 64.4.1 p 3837: "Before transitioning to graphics power
 * domain off or Software Standby mode, stop the operation as
 * described in section 64.3.2." This is a thin wrapper that calls
 * ``ra_mipi_phy_deinit``.
 *
 * @return ``ra_err_t`` error code (see ``ra_mipi_phy_deinit``).
 *
 * @pre Caller is about to enter graphics-domain-off or standby.
 * @post LDO is off; the PHY draws no current.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_enter_stop(void);

/**
 * @brief Post-standby resume: re-run the start-up procedure.
 *
 * @param[in] cfg Same configuration that was passed to the original
 * ``ra_mipi_phy_init``.
 *
 * @return ``ra_err_t`` error code (see ``ra_mipi_phy_init``).
 *
 * @pre Graphics power domain has been re-enabled and PCLKA is
 * running again (HUM 64.3.1 step 1, p 3837).
 * @post D-PHY is enabled and the PLL is locked.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_exit_stop(const ra_mipi_phy_config_t* cfg);

/**
 * @brief Enable the D-PHY LDO (DPHYPWRCR.PWRSEN = 1) only.
 *
 * @return ``k_ra_ok`` after PWRSF stabilises, ``k_ra_err_hw_timeout``
 * if the LDO never asserts ready.
 *
 * @pre Module-stop is already cleared.
 * @post DPHYPWRCR.PWRSEN reads 1.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_ldo_enable(void);

/**
 * @brief Disable the D-PHY LDO (DPHYPWRCR.PWRSEN = 0) only.
 *
 * @return ``k_ra_ok`` always.
 *
 * @pre -- (no preconditions; idempotent).
 * @post DPHYPWRCR.PWRSEN reads 0.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_ldo_disable(void);

/**
 * @brief Release the PLL (DPHYPLOCR.PLLSTP = 0) and wait for lock.
 *
 * @return ``k_ra_ok`` after PLLSF asserts, ``k_ra_err_hw_timeout`` else.
 *
 * @pre LDO has been brought up (PWRSF = 1).
 * @pre DPHYPLFCR has been programmed.
 * @post DPHYSFR.PLLSF reads 1.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_pll_start(void);

/**
 * @brief Stop the PLL (DPHYPLOCR.PLLSTP = 1).
 *
 * @return ``k_ra_ok`` always.
 *
 * @pre -- (no preconditions; idempotent).
 * @post DPHYPLOCR.PLLSTP reads 1.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_pll_stop(void);

/* =============================================================================
 * Operations
 * =============================================================================
 */

/**
 * @brief Reprogram the PLL coefficients (DPHYPLFCR) at runtime.
 *
 * @details
 * HUM Ch 64.2.2 p 3824: DPHYPLFCR may only be written while
 * DPHYPLOCR.PLLSTP = 1. This helper sets PLLSTP, writes DPHYPLFCR
 * from ``pll``, clears PLLSTP, and spins until DPHYSFR.PLLSF
 * re-asserts. The D-PHY enable bit (DPHYOCR.DPHYEN) is left as it
 * was on entry; callers transmitting active video should disable
 * the lane outputs first.
 *
 * @param[in] pll Non-NULL PLL coefficient block.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok PLL re-locked.
 * @retval k_ra_err_null_ptr ``pll`` was NULL.
 * @retval k_ra_err_invalid_arg ``pll->nmul_int`` outside 40..375
 * (HUM 64.2.2 p 3823) or band mismatch.
 * @retval k_ra_err_hw_timeout PLL did not relock within the spin
 * budget.
 *
 * @pre ``pll`` is non-NULL.
 * @pre ``ra_mipi_phy_init`` has already run successfully.
 * @post On success, the PLL output frequency reflects the new
 * coefficients and DPHYSFR.PLLSF reads 1.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_set_lane_speed(const ra_mipi_phy_pll_t* pll);

/**
 * @brief Switch the PHY between DSI master and CSI slave mode.
 *
 * @details
 * HUM Ch 64.2.14 p 3836: DPHYMDC.MASTEREN selects master vs slave.
 * Switching modes requires the D-PHY to be disabled first; this
 * helper clears DPHYEN, rewrites DPHYMDC, and lets the caller
 * re-enable the D-PHY (typically via ``ra_mipi_phy_pll_start`` for
 * master or directly setting DPHYEN for slave).
 *
 * @param[in] mode Target mode.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Mode switched.
 * @retval k_ra_err_invalid_arg ``mode`` outside the enum.
 *
 * @pre -- (DPHYEN may be 0 or 1 -- the helper clears it as needed).
 * @post DPHYMDC.MASTEREN reflects ``mode``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_switch_mode(ra_mipi_phy_mode_t mode);

/**
 * @brief Configure the active lane count.
 *
 * @details
 * The PHY has no lane-count register of its own (the DSI / CSI host
 * drivers gate individual lanes), so this helper only validates the
 * argument against the silicon limit and stores it for later
 * inspection through ``ra_mipi_phy_get_lane_count``. The value is
 * also used by ``ra_mipi_phy_set_lane_enable`` to bound checks.
 *
 * @param[in] count Desired lane count.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Stored.
 * @retval k_ra_err_invalid_arg ``count`` outside 1..4 enum.
 * @retval k_ra_err_not_supported ``count`` is 3 or 4 (silicon max 2,
 * HUM Ch 64.1 Table 64.1 p 3822).
 *
 * @pre -- (no preconditions).
 * @post ``ra_mipi_phy_get_lane_count`` returns ``count`` on success.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_set_lane_count(ra_mipi_phy_lane_count_t count);

/**
 * @brief Read back the lane count last configured.
 *
 * @details
 * Returns the cached lane-count value last written via
 * ``ra_mipi_phy_set_lane_count``. The PHY itself has no lane-count
 * register (HUM Ch 64.1 Table 64.1 p 3822); this is a software shadow
 * consulted by ``ra_mipi_phy_set_lane_enable``.
 *
 * @return Active lane count (defaults to ``k_ra_mipi_phy_lane_count_2``).
 * @retval k_ra_mipi_phy_lane_count_1 Single-lane mode.
 * @retval k_ra_mipi_phy_lane_count_2 Dual-lane (default).
 *
 * @pre -- (no preconditions; safe before ``init``).
 * @pre ``ra_mipi_phy_set_lane_count`` may or may not have been called.
 * @post Hardware state is unchanged.
 * @post Cached lane-count value is unchanged.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
ra_mipi_phy_lane_count_t ra_mipi_phy_get_lane_count(void);

/**
 * @brief Enable or disable a single data lane.
 *
 * @details
 * Mirrors the per-lane gate that the DSI / CSI host drivers expose.
 * The PHY itself has no per-lane register, so this helper only
 * validates the index and stashes the request -- the higher-level
 * driver consults ``ra_mipi_phy_is_lane_enabled`` when configuring
 * its own lane gate.
 *
 * @param[in] lane Lane identifier.
 * @param[in] enable ``true`` to enable, ``false`` to disable.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Stored.
 * @retval k_ra_err_invalid_arg ``lane`` outside the enum or beyond
 * the configured ``lane_count``.
 * @retval k_ra_err_not_supported Lane >= 2 (silicon max).
 *
 * @pre -- (no preconditions).
 * @post ``ra_mipi_phy_is_lane_enabled`` returns ``enable`` on success.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_set_lane_enable(ra_mipi_phy_lane_id_t lane, bool enable);

/**
 * @brief Query whether the given lane is currently enabled.
 *
 * @details
 * Returns the cached enable bit for ``lane`` set by
 * ``ra_mipi_phy_set_lane_enable``. The DSI / CSI host driver consults
 * this when programming its own lane gate.
 *
 * @param[in] lane Lane identifier (clk, d0, d1).
 * @return ``true`` if enabled (default for clk + d0 + d1), else ``false``.
 * @retval true  Lane is software-enabled.
 * @retval false Lane is software-disabled or out of range.
 *
 * @pre -- (no preconditions; safe before ``init``).
 * @pre ``lane`` is a valid ::ra_mipi_phy_lane_id_t value.
 * @post Hardware state is unchanged.
 * @post Cached lane state is unchanged.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
bool ra_mipi_phy_is_lane_enabled(ra_mipi_phy_lane_id_t lane);

/**
 * @brief Set the HS clock-lane mode (continuous vs non-continuous).
 *
 * @param[in] mode Clock-lane mode.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Stored.
 * @retval k_ra_err_invalid_arg ``mode`` outside the enum.
 *
 * @pre -- (no preconditions).
 * @post ``ra_mipi_phy_get_clock_mode`` returns ``mode`` on success.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_set_clock_mode(ra_mipi_phy_clk_mode_t mode);

/**
 * @brief Query the HS clock-lane mode last configured.
 *
 * @details
 * Returns the cached clock-lane mode set by
 * ``ra_mipi_phy_set_clock_mode``. Per HUM Ch 64.3 p 3837, the PHY
 * does not store the choice itself -- the DSI host gates HS-CLK
 * based on this software shadow.
 *
 * @return Active clock mode (defaults to non-continuous).
 * @retval k_ra_mipi_phy_clk_mode_continuous     HS clock free-runs.
 * @retval k_ra_mipi_phy_clk_mode_non_continuous HS clock gated when idle.
 *
 * @pre -- (no preconditions; safe before ``init``).
 * @pre ``ra_mipi_phy_set_clock_mode`` may or may not have been called.
 * @post Hardware state is unchanged.
 * @post Cached clock-mode value is unchanged.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
ra_mipi_phy_clk_mode_t ra_mipi_phy_get_clock_mode(void);

/**
 * @brief Set the EoTP packet emission preference (DSI master only).
 *
 * @param[in] eotp Desired EoTP setting.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Stored.
 * @retval k_ra_err_invalid_arg ``eotp`` outside the enum.
 *
 * @pre -- (no preconditions).
 * @post ``ra_mipi_phy_get_eotp`` returns ``eotp`` on success.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_set_eotp(ra_mipi_phy_eotp_t eotp);

/**
 * @brief Query the EoTP setting last configured.
 *
 * @details
 * Returns the cached End-of-Transmission-Packet preference last set
 * via ``ra_mipi_phy_set_eotp``. The DSI master honours this when
 * emitting the optional EoTP at the end of HS bursts.
 *
 * @return Active EoTP setting (defaults to disabled).
 * @retval k_ra_mipi_phy_eotp_disabled EoTP suppression -- legacy peripherals.
 * @retval k_ra_mipi_phy_eotp_enabled  EoTP appended -- spec-compliant.
 *
 * @pre -- (no preconditions; safe before ``init``).
 * @pre ``ra_mipi_phy_set_eotp`` may or may not have been called.
 * @post Hardware state is unchanged.
 * @post Cached EoTP value is unchanged.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
ra_mipi_phy_eotp_t ra_mipi_phy_get_eotp(void);

/**
 * @brief Update DPHYREFCR.RFREQ at runtime.
 *
 * @param[in] mhz New PCLKA frequency, MHz (40..125).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok RFREQ updated.
 * @retval k_ra_err_invalid_arg ``mhz`` outside 40..125
 * (HUM Ch 64.2.1 p 3822).
 *
 * @pre Module-stop is cleared.
 * @post DPHYREFCR.RFREQ equals ``mhz`` on success.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_set_pclka_freq(uint8_t mhz);

/**
 * @brief Update DPHYESCCR.ESCDIV at runtime.
 *
 * @details
 * HUM Ch 64.2.4 p 3825: ESCDIV[4:0] must be written while
 * DPHYPLOCR.PLLSTP = 1. This helper enforces that by stopping the
 * PLL, writing the divisor, and restarting the PLL.
 *
 * @param[in] escdiv New escape divisor (0..31).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Divisor updated and PLL re-locked.
 * @retval k_ra_err_invalid_arg ``escdiv`` > 31.
 * @retval k_ra_err_hw_timeout PLL did not re-lock within budget.
 *
 * @pre ``ra_mipi_phy_init`` ran successfully (master mode).
 * @post DPHYESCCR.ESCDIV equals ``escdiv`` on success.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_set_escape_divisor(uint8_t escdiv);

/**
 * @brief Look up an HUM Table 64.2 / 64.3 row and apply the timing.
 *
 * @details
 * Tables 64.2 (DSI mode, p 3831-3834) and 64.3 (CSI mode, p 3835-3836)
 * map ``(PCLKA, lane_rate_mbps)`` to a ready-made set of DPHYTIMx
 * field values. This helper picks the closest row, fills in a
 * ``ra_mipi_phy_timing_t``, and writes DPHYTIM1..6.
 *
 * @param[in] mode Active mode (DSI vs CSI uses different tables).
 * @param[in] pclka_mhz PCLKA frequency in MHz.
 * @param[in] rate_mbps Per-lane line rate in Mbps.
 * @param[out] out_timing Optional non-NULL pointer to receive the
 * applied timing block (skip with NULL).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Timing applied.
 * @retval k_ra_err_invalid_arg ``rate_mbps`` outside 80..720 or
 * ``mode`` outside the enum.
 * @retval k_ra_err_not_supported PCLKA does not match any table row.
 *
 * @pre Module-stop is cleared.
 * @post DPHYTIM1..6 hold the looked-up values.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_select_timing(ra_mipi_phy_mode_t    mode,
                                                 uint8_t               pclka_mhz,
                                                 uint16_t              rate_mbps,
                                                 ra_mipi_phy_timing_t* out_timing);

/**
 * @brief Validate a PLL coefficient block against HUM 64.2.2 p 3823-3824.
 *
 * @details
 * Checks the integer N range (40..375), then verifies the resulting
 * PLL output frequency lies within the band selected by PMUL[1:0]
 * (P=1: 960..1440, P=1/2: 480..1440, P=1/4: 240..750, P=1/8: 120..375).
 * The MOSC frequency is taken from the parameter ``mosc_mhz`` so the
 * check works without consulting the live CGC state.
 *
 * @param[in] pll Non-NULL PLL coefficient block.
 * @param[in] mosc_mhz MOSC frequency, MHz (8..48).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Coefficients are within spec.
 * @retval k_ra_err_null_ptr ``pll`` was NULL.
 * @retval k_ra_err_invalid_arg Out-of-spec coefficient.
 *
 * @pre ``pll`` is non-NULL.
 * @pre ``mosc_mhz`` is between 8 and 48.
 * @post Hardware state is unchanged.
 *
 * @note Pure function -- safe to call from any context.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_validate_pll_band(const ra_mipi_phy_pll_t* pll,
                                                     uint8_t                  mosc_mhz);

/**
 * @brief Compute approximate PLL output frequency for a coefficient block.
 *
 * @details
 * Pure helper -- evaluates the HUM Ch 64.2.2 p 3823 formula
 * ``f = fMAIN * I * (NF + N) * P`` and returns the result in MHz.
 * Hardware state is unchanged. Used by ``ra_mipi_phy_validate_pll_band``
 * and by the ``ra_mipi_phy_compute_pll_for_rate`` helper -- exposed
 * publicly so drivers that need to derive a line rate from a PLL
 * snapshot can re-use the computation.
 *
 * @param[in] pll Non-NULL PLL coefficient block.
 * @param[in] mosc_mhz MOSC frequency, MHz (8..48).
 * @param[out] out_mhz Receives the computed output frequency in MHz.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok ``*out_mhz`` updated.
 * @retval k_ra_err_null_ptr ``pll`` or ``out_mhz`` was NULL.
 * @retval k_ra_err_invalid_arg ``mosc_mhz`` outside 8..48.
 *
 * @pre ``pll`` is non-NULL.
 * @pre ``out_mhz`` is non-NULL.
 * @post ``*out_mhz`` reflects the formula result (not pinned to band).
 *
 * @note Pure function -- safe from any context.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_mipi_phy_compute_pll_freq(const ra_mipi_phy_pll_t* pll, uint8_t mosc_mhz, uint32_t* out_mhz);

/**
 * @brief Look up an HUM Table 64.2 / 64.3 row WITHOUT writing the regs.
 *
 * @details
 * Same selection logic as ``ra_mipi_phy_select_timing`` but stops
 * short of touching DPHYTIM1..6 -- useful for unit tests, dry runs,
 * and callers that want to inspect a candidate timing block before
 * committing.
 *
 * @param[in] mode Active mode.
 * @param[in] pclka_mhz PCLKA frequency, MHz.
 * @param[in] rate_mbps Per-lane line rate, Mbps.
 * @param[out] out_timing Receives the matching timing row.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Row found, ``*out_timing`` filled.
 * @retval k_ra_err_null_ptr ``out_timing`` was NULL.
 * @retval k_ra_err_invalid_arg ``rate_mbps`` outside 80..720 or
 * ``mode`` outside the enum.
 * @retval k_ra_err_not_supported PCLKA does not match any row.
 *
 * @pre ``out_timing`` is non-NULL.
 * @pre ``mode`` is one of the enumerator values.
 * @post Hardware state is unchanged.
 *
 * @note Pure lookup -- thread-safe across the lookup table.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_lookup_timing(ra_mipi_phy_mode_t    mode,
                                                 uint8_t               pclka_mhz,
                                                 uint16_t              rate_mbps,
                                                 ra_mipi_phy_timing_t* out_timing);

/**
 * @brief Read DPHYSFR and decode it into a ``ra_mipi_phy_status_decoded_t``.
 *
 * @param[out] out Non-NULL decoded snapshot.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok ``*out`` populated.
 * @retval k_ra_err_null_ptr ``out`` was NULL.
 *
 * @pre ``out`` is non-NULL.
 * @post ``*out`` reflects the live DPHYSFR contents at call time.
 *
 * @note Read-only access -- safe under simple races.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_get_status_decoded(ra_mipi_phy_status_decoded_t* out);

/**
 * @brief Read the cached lifecycle state.
 *
 * @details
 * The state is updated by every public function that performs a
 * register write. Useful for higher-level stacks that need to assert
 * "PHY is in pll_run state before issuing a burst".
 *
 * @return Cached ``ra_mipi_phy_state_t`` -- defaults to
 * ``k_ra_mipi_phy_state_off`` after reset / first init.
 * @retval k_ra_mipi_phy_state_off       PHY powered down.
 * @retval k_ra_mipi_phy_state_ldo_ready LDO stabilised.
 * @retval k_ra_mipi_phy_state_pll_run   PLL locked and running.
 *
 * @pre -- (no preconditions; safe before ``init``).
 * @pre Caller has access to driver state (no IRQ guard required).
 * @post Hardware state is unchanged.
 * @post Cached lifecycle state is unchanged.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
ra_mipi_phy_state_t ra_mipi_phy_get_state(void);

/**
 * @brief Read the most recently configured operating mode.
 *
 * @details
 * Returns the cached DPHYMDC.MASTEREN choice -- the bit is set during
 * ``ra_mipi_phy_init`` and cleared when ``ra_mipi_phy_deinit`` runs.
 *
 * @return Cached ``ra_mipi_phy_mode_t``. Defaults to
 * ``k_ra_mipi_phy_mode_csi_slave`` (matches DPHYMDC reset
 * value 0 -- HUM Ch 64.2.14 p 3837).
 * @retval k_ra_mipi_phy_mode_dsi_master DSI host -- TX path.
 * @retval k_ra_mipi_phy_mode_csi_slave  CSI sink -- RX path.
 *
 * @pre -- (no preconditions; safe before ``init``).
 * @pre Caller has access to driver state.
 * @post Hardware state is unchanged.
 * @post Cached active-mode value is unchanged.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
ra_mipi_phy_mode_t ra_mipi_phy_get_active_mode(void);

/**
 * @brief Set the dual-mode arbitration policy.
 *
 * @details
 * RA8D2 silicon physically multiplexes one D-PHY between DSI and CSI
 * (HUM Ch 64.2.14 p 3837 -- DPHYMDC.MASTEREN is a single bit). This
 * helper records the caller's preferred policy so the higher-level
 * stack can decide arbitration without re-reading the spec.
 *
 * @param[in] mode New arbitration policy.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Stored.
 * @retval k_ra_err_invalid_arg ``mode`` outside the enum.
 *
 * @pre -- (no preconditions).
 * @post ``ra_mipi_phy_get_dual_mode`` returns ``mode`` on success.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_set_dual_mode(ra_mipi_phy_dual_mode_t mode);

/**
 * @brief Read back the dual-mode arbitration policy last configured.
 *
 * @details
 * Returns the cached arbitration policy set by
 * ``ra_mipi_phy_set_dual_mode``. Higher-level stacks consult this when
 * deciding whether to switch the shared D-PHY between DSI and CSI.
 *
 * @return Active dual-mode setting (defaults to
 * ``k_ra_mipi_phy_dual_off``).
 * @retval k_ra_mipi_phy_dual_off          No arbitration -- single owner.
 * @retval k_ra_mipi_phy_dual_alternate    Cooperative time-multiplexing.
 * @retval k_ra_mipi_phy_dual_dsi_priority DSI wins contention.
 * @retval k_ra_mipi_phy_dual_csi_priority CSI wins contention.
 *
 * @pre -- (no preconditions; safe before ``init``).
 * @pre Caller has access to driver state.
 * @post Hardware state is unchanged.
 * @post Cached dual-mode policy is unchanged.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
ra_mipi_phy_dual_mode_t ra_mipi_phy_get_dual_mode(void);

/**
 * @brief Test whether a (mode, policy) request is currently allowed.
 *
 * @details
 * Implements the policy table:
 *
 * - ``dual_off`` -- accept any request.
 * - ``dual_alternate`` -- accept any request (caller switches modes).
 * - ``dual_dsi_priority`` -- accept DSI; reject CSI when mode != CSI.
 * - ``dual_csi_priority`` -- accept CSI; reject DSI when mode != DSI.
 *
 * @param[in] requestor Mode the caller wants to enter.
 *
 * @return ``true`` if the request is permitted, ``false`` if blocked.
 * @retval true  Policy permits ``requestor`` to acquire the PHY.
 * @retval false Policy blocks ``requestor`` (priority owner is active).
 *
 * @pre Driver init is not required; the test is purely software policy.
 * @pre ``requestor`` is a valid ::ra_mipi_phy_mode_t value.
 * @post Hardware state is unchanged.
 * @post Cached dual-mode policy is unchanged.
 *
 * @note Thread safety: read-only over a software shadow.
 * @since 0.1.0
 */
bool ra_mipi_phy_dual_mode_can_acquire(ra_mipi_phy_mode_t requestor);

/**
 * @brief Update DPHYREFCR.RFREQ from a PCLKA frequency in Hz.
 *
 * @details
 * Convenience wrapper around ``ra_mipi_phy_set_pclka_freq`` that
 * accepts the rounded MHz value of an arbitrary Hz input -- mirrors
 * the FSP code path which calls ``R_FSP_SystemClockHzGet(PCLKA)``
 * and divides by 1_000_000.
 *
 * @param[in] hz PCLKA frequency in Hz (40_000_000.. 125_000_000).
 *
 * @return ``ra_err_t`` error code (see ``ra_mipi_phy_set_pclka_freq``).
 *
 * @pre ``hz`` is at least 1_000_000 (so the floor doesn't underflow).
 * @post DPHYREFCR.RFREQ holds floor(hz / 1_000_000) on success.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_set_pclka_freq_hz(uint32_t hz);

/**
 * @brief Compute (mosc, pll) -> approximate per-lane line rate.
 *
 * @details
 * Helper for callers that want to verify a PLL coefficient set
 * matches a target lane rate. Returns ``f_pll / 2`` per HUM Ch 64.2.2
 * p 3824 ("Line rate per lane = PLL output / 2").
 *
 * @param[in] pll Non-NULL PLL coefficients.
 * @param[in] mosc_mhz MOSC frequency in MHz.
 * @param[out] out_mbps Receives the per-lane line rate in Mbps.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok ``*out_mbps`` updated.
 * @retval k_ra_err_null_ptr Either pointer was NULL.
 * @retval k_ra_err_invalid_arg ``mosc_mhz`` outside 8..48.
 *
 * @pre ``pll`` and ``out_mbps`` are non-NULL.
 * @pre ``mosc_mhz`` is between 8 and 48.
 * @post Hardware state is unchanged.
 *
 * @note Pure helper -- safe from any context.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_mipi_phy_compute_lane_rate_mbps(const ra_mipi_phy_pll_t* pll,
                                                          uint8_t                  mosc_mhz,
                                                          uint32_t*                out_mbps);

#ifdef __cplusplus
}
#endif
