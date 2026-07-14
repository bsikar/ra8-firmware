/**
 * @file ra8_mipi_phy_ops.h
 * @brief MIPI D-PHY driver -- runtime operations prototypes
 * @ingroup grp_hal_display
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Runtime operation entry points for the RA8D2 MIPI PHY block (HUM
 * Ch 64, p 3822-3838): lane-speed re-tune, mode switching, lane and
 * clock configuration, EoTP, PCLKA / escape-divisor updates, timing
 * lookup, PLL math helpers, status decode, and dual-mode arbitration.
 * The enums, structs, and callback typedef these prototypes reference
 * live in ``ra8_mipi_phy_types.h``, which this header includes. The
 * lifecycle / status / interrupt / power prototypes live in
 * ``ra8_mipi_phy_api.h``. All three are re-exported by the thin
 * umbrella ``ra8_mipi_phy.h``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
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
#include "ra8_mipi_phy_types.h"

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
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok PLL re-locked.
 * @retval k_ra8_err_null_ptr ``pll`` was NULL.
 * @retval k_ra8_err_invalid_arg ``pll->nmul_int`` outside 40..375
 * (HUM 64.2.2 p 3823) or band mismatch.
 * @retval k_ra8_err_hw_timeout PLL did not relock within the spin
 * budget.
 *
 * @pre ``pll`` is non-NULL.
 * @pre ``ra8_mipi_phy_init`` has already run successfully.
 * @post On success, the PLL output frequency reflects the new
 * coefficients and DPHYSFR.PLLSF reads 1.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_set_lane_speed(const ra8_mipi_phy_pll_t* pll);

/**
 * @brief Switch the PHY between DSI host and CSI device mode.
 *
 * @details
 * HUM Ch 64.2.14 p 3836: DPHYMDC.MASTEREN selects host vs device.
 * Switching modes requires the D-PHY to be disabled first; this
 * helper clears DPHYEN, rewrites DPHYMDC, and lets the caller
 * re-enable the D-PHY (typically via ``ra8_mipi_phy_pll_start`` for
 * host or directly setting DPHYEN for device).
 *
 * @param[in] mode Target mode.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Mode switched.
 * @retval k_ra8_err_invalid_arg ``mode`` outside the enum.
 *
 * @pre -- (DPHYEN may be 0 or 1 -- the helper clears it as needed).
 * @post DPHYMDC.MASTEREN reflects ``mode``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_switch_mode(ra8_mipi_phy_mode_t mode);

/**
 * @brief Configure the active lane count.
 *
 * @details
 * The PHY has no lane-count register of its own (the DSI / CSI host
 * drivers gate individual lanes), so this helper only validates the
 * argument against the silicon limit and stores it for later
 * inspection through ``ra8_mipi_phy_get_lane_count``. The value is
 * also used by ``ra8_mipi_phy_set_lane_enable`` to bound checks.
 *
 * @param[in] count Desired lane count.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Stored.
 * @retval k_ra8_err_invalid_arg ``count`` outside 1..4 enum.
 * @retval k_ra8_err_not_supported ``count`` is 3 or 4 (silicon max 2,
 * HUM Ch 64.1 Table 64.1 p 3822).
 *
 * @pre -- (no preconditions).
 * @post ``ra8_mipi_phy_get_lane_count`` returns ``count`` on success.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_set_lane_count(ra8_mipi_phy_lane_count_t count);

/**
 * @brief Read back the lane count last configured.
 *
 * @details
 * Returns the cached lane-count value last written via
 * ``ra8_mipi_phy_set_lane_count``. The PHY itself has no lane-count
 * register (HUM Ch 64.1 Table 64.1 p 3822); this is a software shadow
 * consulted by ``ra8_mipi_phy_set_lane_enable``.
 *
 * @return Active lane count (defaults to ``k_ra8_mipi_phy_lane_count_2``).
 * @retval k_ra8_mipi_phy_lane_count_1 Single-lane mode.
 * @retval k_ra8_mipi_phy_lane_count_2 Dual-lane (default).
 *
 * @pre -- (no preconditions; safe before ``init``).
 * @pre ``ra8_mipi_phy_set_lane_count`` may or may not have been called.
 * @post Hardware state is unchanged.
 * @post Cached lane-count value is unchanged.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
ra8_mipi_phy_lane_count_t ra8_mipi_phy_get_lane_count(void);

/**
 * @brief Enable or disable a single data lane.
 *
 * @details
 * Mirrors the per-lane gate that the DSI / CSI host drivers expose.
 * The PHY itself has no per-lane register, so this helper only
 * validates the index and stashes the request -- the higher-level
 * driver consults ``ra8_mipi_phy_is_lane_enabled`` when configuring
 * its own lane gate.
 *
 * @param[in] lane Lane identifier.
 * @param[in] enable ``true`` to enable, ``false`` to disable.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Stored.
 * @retval k_ra8_err_invalid_arg ``lane`` outside the enum or beyond
 * the configured ``lane_count``.
 * @retval k_ra8_err_not_supported Lane >= 2 (silicon max).
 *
 * @pre -- (no preconditions).
 * @post ``ra8_mipi_phy_is_lane_enabled`` returns ``enable`` on success.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_set_lane_enable(ra8_mipi_phy_lane_id_t lane, bool enable);

/**
 * @brief Query whether the given lane is currently enabled.
 *
 * @details
 * Returns the cached enable bit for ``lane`` set by
 * ``ra8_mipi_phy_set_lane_enable``. The DSI / CSI host driver consults
 * this when programming its own lane gate.
 *
 * @param[in] lane Lane identifier (clk, d0, d1).
 * @return ``true`` if enabled (default for clk + d0 + d1), else ``false``.
 * @retval true  Lane is software-enabled.
 * @retval false Lane is software-disabled or out of range.
 *
 * @pre -- (no preconditions; safe before ``init``).
 * @pre ``lane`` is a valid ::ra8_mipi_phy_lane_id_t value.
 * @post Hardware state is unchanged.
 * @post Cached lane state is unchanged.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
bool ra8_mipi_phy_is_lane_enabled(ra8_mipi_phy_lane_id_t lane);

/**
 * @brief Set the HS clock-lane mode (continuous vs non-continuous).
 *
 * @param[in] mode Clock-lane mode.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Stored.
 * @retval k_ra8_err_invalid_arg ``mode`` outside the enum.
 *
 * @pre -- (no preconditions).
 * @post ``ra8_mipi_phy_get_clock_mode`` returns ``mode`` on success.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_set_clock_mode(ra8_mipi_phy_clk_mode_t mode);

/**
 * @brief Query the HS clock-lane mode last configured.
 *
 * @details
 * Returns the cached clock-lane mode set by
 * ``ra8_mipi_phy_set_clock_mode``. Per HUM Ch 64.3 p 3837, the PHY
 * does not store the choice itself -- the DSI host gates HS-CLK
 * based on this software shadow.
 *
 * @return Active clock mode (defaults to non-continuous).
 * @retval k_ra8_mipi_phy_clk_mode_continuous     HS clock free-runs.
 * @retval k_ra8_mipi_phy_clk_mode_non_continuous HS clock gated when idle.
 *
 * @pre -- (no preconditions; safe before ``init``).
 * @pre ``ra8_mipi_phy_set_clock_mode`` may or may not have been called.
 * @post Hardware state is unchanged.
 * @post Cached clock-mode value is unchanged.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
ra8_mipi_phy_clk_mode_t ra8_mipi_phy_get_clock_mode(void);

/**
 * @brief Set the EoTP packet emission preference (DSI host only).
 *
 * @param[in] eotp Desired EoTP setting.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Stored.
 * @retval k_ra8_err_invalid_arg ``eotp`` outside the enum.
 *
 * @pre -- (no preconditions).
 * @post ``ra8_mipi_phy_get_eotp`` returns ``eotp`` on success.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_set_eotp(ra8_mipi_phy_eotp_t eotp);

/**
 * @brief Query the EoTP setting last configured.
 *
 * @details
 * Returns the cached End-of-Transmission-Packet preference last set
 * via ``ra8_mipi_phy_set_eotp``. The DSI host honours this when
 * emitting the optional EoTP at the end of HS bursts.
 *
 * @return Active EoTP setting (defaults to disabled).
 * @retval k_ra8_mipi_phy_eotp_disabled EoTP suppression -- legacy peripherals.
 * @retval k_ra8_mipi_phy_eotp_enabled  EoTP appended -- spec-compliant.
 *
 * @pre -- (no preconditions; safe before ``init``).
 * @pre ``ra8_mipi_phy_set_eotp`` may or may not have been called.
 * @post Hardware state is unchanged.
 * @post Cached EoTP value is unchanged.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
ra8_mipi_phy_eotp_t ra8_mipi_phy_get_eotp(void);

/**
 * @brief Update DPHYREFCR.RFREQ at runtime.
 *
 * @param[in] mhz New PCLKA frequency, MHz (40..125).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok RFREQ updated.
 * @retval k_ra8_err_invalid_arg ``mhz`` outside 40..125
 * (HUM Ch 64.2.1 p 3822).
 *
 * @pre Module-stop is cleared.
 * @post DPHYREFCR.RFREQ equals ``mhz`` on success.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_set_pclka_freq(uint8_t mhz);

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
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Divisor updated and PLL re-locked.
 * @retval k_ra8_err_invalid_arg ``escdiv`` > 31.
 * @retval k_ra8_err_hw_timeout PLL did not re-lock within budget.
 *
 * @pre ``ra8_mipi_phy_init`` ran successfully (host mode).
 * @post DPHYESCCR.ESCDIV equals ``escdiv`` on success.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_set_escape_divisor(uint8_t escdiv);

/**
 * @brief Look up an HUM Table 64.2 / 64.3 row and apply the timing.
 *
 * @details
 * Tables 64.2 (DSI mode, p 3831-3834) and 64.3 (CSI mode, p 3835-3836)
 * map ``(PCLKA, lane_rate_mbps)`` to a ready-made set of DPHYTIMx
 * field values. This helper picks the closest row, fills in a
 * ``ra8_mipi_phy_timing_t``, and writes DPHYTIM1..6.
 *
 * @param[in] mode Active mode (DSI vs CSI uses different tables).
 * @param[in] pclka_mhz PCLKA frequency in MHz.
 * @param[in] rate_mbps Per-lane line rate in Mbps.
 * @param[out] out_timing Optional non-NULL pointer to receive the
 * applied timing block (skip with NULL).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Timing applied.
 * @retval k_ra8_err_invalid_arg ``rate_mbps`` outside 80..720 or
 * ``mode`` outside the enum.
 * @retval k_ra8_err_not_supported PCLKA does not match any table row.
 *
 * @pre Module-stop is cleared.
 * @post DPHYTIM1..6 hold the looked-up values.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_select_timing(ra8_mipi_phy_mode_t    mode,
                                                   uint8_t                pclka_mhz,
                                                   uint16_t               rate_mbps,
                                                   ra8_mipi_phy_timing_t* out_timing);

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
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Coefficients are within spec.
 * @retval k_ra8_err_null_ptr ``pll`` was NULL.
 * @retval k_ra8_err_invalid_arg Out-of-spec coefficient.
 *
 * @pre ``pll`` is non-NULL.
 * @pre ``mosc_mhz`` is between 8 and 48.
 * @post Hardware state is unchanged.
 *
 * @note Pure function -- safe to call from any context.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_validate_pll_band(const ra8_mipi_phy_pll_t* pll,
                                                       uint8_t                   mosc_mhz);

/**
 * @brief Compute approximate PLL output frequency for a coefficient block.
 *
 * @details
 * Pure helper -- evaluates the HUM Ch 64.2.2 p 3823 formula
 * ``f = fMAIN * I * (NF + N) * P`` and returns the result in MHz.
 * Hardware state is unchanged. Used by ``ra8_mipi_phy_validate_pll_band``
 * and by the ``ra8_mipi_phy_compute_pll_for_rate`` helper -- exposed
 * publicly so drivers that need to derive a line rate from a PLL
 * snapshot can re-use the computation.
 *
 * @param[in] pll Non-NULL PLL coefficient block.
 * @param[in] mosc_mhz MOSC frequency, MHz (8..48).
 * @param[out] out_mhz Receives the computed output frequency in MHz.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok ``*out_mhz`` updated.
 * @retval k_ra8_err_null_ptr ``pll`` or ``out_mhz`` was NULL.
 * @retval k_ra8_err_invalid_arg ``mosc_mhz`` outside 8..48.
 *
 * @pre ``pll`` is non-NULL.
 * @pre ``out_mhz`` is non-NULL.
 * @post ``*out_mhz`` reflects the formula result (not pinned to band).
 *
 * @note Pure function -- safe from any context.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_mipi_phy_compute_pll_freq(const ra8_mipi_phy_pll_t* pll, uint8_t mosc_mhz, uint32_t* out_mhz);

/**
 * @brief Look up an HUM Table 64.2 / 64.3 row WITHOUT writing the regs.
 *
 * @details
 * Same selection logic as ``ra8_mipi_phy_select_timing`` but stops
 * short of touching DPHYTIM1..6 -- useful for unit tests, dry runs,
 * and callers that want to inspect a candidate timing block before
 * committing.
 *
 * @param[in] mode Active mode.
 * @param[in] pclka_mhz PCLKA frequency, MHz.
 * @param[in] rate_mbps Per-lane line rate, Mbps.
 * @param[out] out_timing Receives the matching timing row.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Row found, ``*out_timing`` filled.
 * @retval k_ra8_err_null_ptr ``out_timing`` was NULL.
 * @retval k_ra8_err_invalid_arg ``rate_mbps`` outside 80..720 or
 * ``mode`` outside the enum.
 * @retval k_ra8_err_not_supported PCLKA does not match any row.
 *
 * @pre ``out_timing`` is non-NULL.
 * @pre ``mode`` is one of the enumerator values.
 * @post Hardware state is unchanged.
 *
 * @note Pure lookup -- thread-safe across the lookup table.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_lookup_timing(ra8_mipi_phy_mode_t    mode,
                                                   uint8_t                pclka_mhz,
                                                   uint16_t               rate_mbps,
                                                   ra8_mipi_phy_timing_t* out_timing);

/**
 * @brief Read DPHYSFR and decode it into a ``ra8_mipi_phy_status_decoded_t``.
 *
 * @param[out] out Non-NULL decoded snapshot.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok ``*out`` populated.
 * @retval k_ra8_err_null_ptr ``out`` was NULL.
 *
 * @pre ``out`` is non-NULL.
 * @post ``*out`` reflects the live DPHYSFR contents at call time.
 *
 * @note Read-only access -- safe under simple races.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_get_status_decoded(ra8_mipi_phy_status_decoded_t* out);

/**
 * @brief Read the cached lifecycle state.
 *
 * @details
 * The state is updated by every public function that performs a
 * register write. Useful for higher-level stacks that need to assert
 * "PHY is in pll_run state before issuing a burst".
 *
 * @return Cached ``ra8_mipi_phy_state_t`` -- defaults to
 * ``k_ra8_mipi_phy_state_off`` after reset / first init.
 * @retval k_ra8_mipi_phy_state_off       PHY powered down.
 * @retval k_ra8_mipi_phy_state_ldo_ready LDO stabilised.
 * @retval k_ra8_mipi_phy_state_pll_run   PLL locked and running.
 *
 * @pre -- (no preconditions; safe before ``init``).
 * @pre Caller has access to driver state (no IRQ guard required).
 * @post Hardware state is unchanged.
 * @post Cached lifecycle state is unchanged.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
ra8_mipi_phy_state_t ra8_mipi_phy_get_state(void);

/**
 * @brief Read the most recently configured operating mode.
 *
 * @details
 * Returns the cached DPHYMDC.MASTEREN choice -- the bit is set during
 * ``ra8_mipi_phy_init`` and cleared when ``ra8_mipi_phy_deinit`` runs.
 *
 * @return Cached ``ra8_mipi_phy_mode_t``. Defaults to
 * ``k_ra8_mipi_phy_mode_csi_device`` (matches DPHYMDC reset
 * value 0 -- HUM Ch 64.2.14 p 3837).
 * @retval k_ra8_mipi_phy_mode_dsi_host DSI host -- TX path.
 * @retval k_ra8_mipi_phy_mode_csi_device  CSI sink -- RX path.
 *
 * @pre -- (no preconditions; safe before ``init``).
 * @pre Caller has access to driver state.
 * @post Hardware state is unchanged.
 * @post Cached active-mode value is unchanged.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
ra8_mipi_phy_mode_t ra8_mipi_phy_get_active_mode(void);

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
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Stored.
 * @retval k_ra8_err_invalid_arg ``mode`` outside the enum.
 *
 * @pre -- (no preconditions).
 * @post ``ra8_mipi_phy_get_dual_mode`` returns ``mode`` on success.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_set_dual_mode(ra8_mipi_phy_dual_mode_t mode);

/**
 * @brief Read back the dual-mode arbitration policy last configured.
 *
 * @details
 * Returns the cached arbitration policy set by
 * ``ra8_mipi_phy_set_dual_mode``. Higher-level stacks consult this when
 * deciding whether to switch the shared D-PHY between DSI and CSI.
 *
 * @return Active dual-mode setting (defaults to
 * ``k_ra8_mipi_phy_dual_off``).
 * @retval k_ra8_mipi_phy_dual_off          No arbitration -- single owner.
 * @retval k_ra8_mipi_phy_dual_alternate    Cooperative time-multiplexing.
 * @retval k_ra8_mipi_phy_dual_dsi_priority DSI wins contention.
 * @retval k_ra8_mipi_phy_dual_csi_priority CSI wins contention.
 *
 * @pre -- (no preconditions; safe before ``init``).
 * @pre Caller has access to driver state.
 * @post Hardware state is unchanged.
 * @post Cached dual-mode policy is unchanged.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
ra8_mipi_phy_dual_mode_t ra8_mipi_phy_get_dual_mode(void);

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
 * @pre ``requestor`` is a valid ::ra8_mipi_phy_mode_t value.
 * @post Hardware state is unchanged.
 * @post Cached dual-mode policy is unchanged.
 *
 * @note Thread safety: read-only over a software shadow.
 * @since 0.1.0
 */
bool ra8_mipi_phy_dual_mode_can_acquire(ra8_mipi_phy_mode_t requestor);

/**
 * @brief Update DPHYREFCR.RFREQ from a PCLKA frequency in Hz.
 *
 * @details
 * Convenience wrapper around ``ra8_mipi_phy_set_pclka_freq`` that
 * accepts the rounded MHz value of an arbitrary Hz input -- mirrors
 * the FSP code path which calls ``R_FSP_SystemClockHzGet(PCLKA)``
 * and divides by 1_000_000.
 *
 * @param[in] hz PCLKA frequency in Hz (40_000_000.. 125_000_000).
 *
 * @return ``ra8_err_t`` error code (see ``ra8_mipi_phy_set_pclka_freq``).
 *
 * @pre ``hz`` is at least 1_000_000 (so the floor doesn't underflow).
 * @post DPHYREFCR.RFREQ holds floor(hz / 1_000_000) on success.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_set_pclka_freq_hz(uint32_t hz);

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
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok ``*out_mbps`` updated.
 * @retval k_ra8_err_null_ptr Either pointer was NULL.
 * @retval k_ra8_err_invalid_arg ``mosc_mhz`` outside 8..48.
 *
 * @pre ``pll`` and ``out_mbps`` are non-NULL.
 * @pre ``mosc_mhz`` is between 8 and 48.
 * @post Hardware state is unchanged.
 *
 * @note Pure helper -- safe from any context.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_compute_lane_rate_mbps(const ra8_mipi_phy_pll_t* pll,
                                                            uint8_t                   mosc_mhz,
                                                            uint32_t*                 out_mbps);

#ifdef __cplusplus
}
#endif
