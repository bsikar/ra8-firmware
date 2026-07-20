/**
 * @file ra8_mipi_phy_api.h
 * @brief MIPI D-PHY driver -- public function prototypes
 * @ingroup grp_hal_display
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Public entry points for the RA8D2 MIPI PHY block (HUM Ch 64,
 * p 3822-3838). This sub-header declares the lifecycle, status,
 * interrupt-path, power-transition, and operations prototypes. The
 * enums, structs, and callback typedef these prototypes reference live
 * in ``ra8_mipi_phy_types.h``, which this header includes. Both are
 * re-exported by the thin umbrella ``ra8_mipi_phy.h``.
 *
 * # Module-stop note
 *
 * HUM Ch 64.4.2 p 3838: MIPI PHY is gated by MSTPCRC. There is no
 * ``k_ra8_mstp_mipi_phy`` entry in ``libs/ra8_hal/inc/ra8_mstp_regs.h``
 * yet. This driver does NOT add one (touching a shared enum file is
 * forbidden); ``ra8_mipi_phy_init`` performs a direct MSTPCRC write
 * with a TODO marker so a later wave can promote it into ``ra8_mstp_t``.
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
 * 2. Write ``DPHYMDC.MASTEREN`` to select host vs device.
 * 3. Write ``DPHYREFCR.RFREQ`` from ``cfg->pclka_mhz``.
 * 4. Set ``DPHYPWRCR.PWRSEN = 1`` to power the LDO.
 * 5. Spin until ``DPHYSFR.PWRSF`` reads 1 or the spin budget runs
 * out (HUM 64.3.1 step 5).
 * 6. (Host only) write ``DPHYPLFCR`` from ``cfg->pll``.
 * 7. (Host only) write ``DPHYESCCR.ESCDIV``.
 * 8. (Host only) clear ``DPHYPLOCR.PLLSTP`` to release the PLL.
 * 9. (Host only) spin until ``DPHYSFR.PLLSF`` reads 1.
 * 10. Write ``DPHYTIM1..6`` from ``cfg->p_timing``.
 * 11. Set ``DPHYOCR.DPHYEN = 1``.
 *
 * @param[in] cfg Non-NULL configuration block.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok D-PHY enabled, LDO + PLL stable.
 * @retval k_ra8_err_null_ptr ``cfg`` or ``cfg->p_timing`` was NULL.
 * @retval k_ra8_err_invalid_arg ``pclka_mhz`` outside 40..125 (HUM 64.2.1
 * p 3822) or PLL fields out of range.
 * @retval k_ra8_err_not_supported ``lane_count`` > 2.
 * @retval k_ra8_err_hw_timeout LDO or PLL did not stabilise in time.
 *
 * @pre Graphics power domain is on and PCLKA is supplied to the
 * MIPI subsystem (HUM Ch 64.3.1 step 1, p 3837).
 * @pre IRQs masked or single-threaded init context (the driver
 * sequences several register writes that must not be torn).
 *
 * @post ``DPHYOCR.DPHYEN`` reads 1.
 * @post For host mode, ``DPHYSFR.PLLSF`` reads 1.
 *
 * @par State Machine
 * @dot
 * digraph ra8_mipi_phy_api_states {
 *   bgcolor="transparent";
 *   rankdir=LR;
 *   node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
 *         fillcolor="#e8eef7", color="#5a7ca6"];
 *   edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
 *
 *   __start [shape=circle, width=0.18, label="", fillcolor="#5a7ca6", color="#5a7ca6"];
 *
 *   Off [label="Off\\nMSTPCRC bit set, regs\\nunreachable"];
 *   Idle [label="Idle"];
 *   LdoUp [label="LdoUp"];
 *   PllRun [label="PllRun"];
 *   Run [label="Run"];
 *
 *   __start -> Off;
 *   Off -> Idle [label="init step 1 (mstp clear)"];
 *   Idle -> LdoUp [label="PWRSEN=1"];
 *   LdoUp -> PllRun [label="PLLSTP=0 (host)"];
 *   LdoUp -> Run [label="DPHYEN=1 (device)"];
 *   PllRun -> Run [label="DPHYEN=1 (host)"];
 *   Run -> Off [label="deinit"];
 *   Run -> LdoUp [label="pll_stop"];
 * }
 * @enddot
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_mipi_phy_deinit
 * @see HUM Ch 64.3.1 p 3837 "D-PHY Startup Procedure"
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_init(const ra8_mipi_phy_config_t* cfg);

/**
 * @brief Stop the D-PHY per HUM Ch 64.3.2 p 3837 (DPHYEN -> PLLSTP -> PWRSEN).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok All three writes succeeded.
 *
 * @pre ``ra8_mipi_phy_init`` was previously called or the block is
 * already in its post-reset state (in which case the writes
 * are idempotent no-ops).
 * @post ``DPHYOCR.DPHYEN`` and ``DPHYPWRCR.PWRSEN`` both read 0.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_deinit(void);

/**
 * @brief Reset the PHY to its post-MSTPCRC-clear state without re-init.
 *
 * @details
 * Clears DPHYOCR / DPHYPLOCR / DPHYPWRCR and zeros every timing
 * register (DPHYTIM1..6) and DPHYPLFCR. Useful when the higher-level
 * stack hits an unrecoverable error and wants to start over without
 * paying for a full module-stop cycle.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok always.
 *
 * @pre -- (no preconditions).
 * @post DPHYOCR / DPHYPWRCR / DPHYPLOCR / DPHYTIMx / DPHYPLFCR all
 * read their reset values.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_reset(void);

/**
 * @brief Reset and re-arm the PHY after an LDO drop / PLL unlock.
 *
 * @param[in] cfg Same configuration that was passed to the original
 * ``ra8_mipi_phy_init``.
 *
 * @return ``ra8_err_t`` error code (see ``ra8_mipi_phy_init``).
 *
 * @pre ``cfg`` non-NULL.
 * @pre The PHY was previously initialized at least once.
 * @post On success, DPHYEN reads 1 and PLL is locked.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_recover_from_error(const ra8_mipi_phy_config_t* cfg);

/* =============================================================================
 * Status
 * =============================================================================
 */

/**
 * @brief Read the D-PHY status flags (DPHYSFR).
 *
 * @param[out] out_mask Receives a copy of DPHYSFR (PWRSF | PLLSF).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok ``*out_mask`` updated.
 * @retval k_ra8_err_null_ptr ``out_mask`` was NULL.
 *
 * @pre ``out_mask`` is non-NULL.
 * @post ``*out_mask`` reflects the live register state.
 *
 * @note Thread safety: read-only access, safe under simple races.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_get_status(uint32_t* out_mask);

/**
 * @brief Clear status flags (no-op on this part).
 *
 * @details
 * DPHYSFR is read-only (HUM Ch 64.2.6 p 3826) -- the bits are
 * driven by hardware ready signals and cannot be cleared by
 * software. This entry point exists so ``ra8_mipi_phy`` mirrors
 * the API shape of peer drivers.
 *
 * @param[in] mask Bits the caller would like cleared (ignored).
 *
 * @return ``k_ra8_ok`` always.
 *
 * @pre -- (no requirements; the function ignores its argument).
 * @post Hardware state is unchanged.
 *
 * @note Thread safety: trivially safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_clear_status(uint32_t mask);

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
bool ra8_mipi_phy_is_ldo_stable(void);

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
 * @pre PLL has been programmed via ``ra8_mipi_phy_init``.
 * @post Hardware state is unchanged.
 * @post No DPHYSFR write was issued.
 *
 * @note Thread safety: read-only.
 * @since 0.1.0
 */
bool ra8_mipi_phy_is_pll_locked(void);

/**
 * @brief Bounded poll until the PHY is fully ready (PWRSF & PLLSF).
 *
 * @return ``k_ra8_ok`` if ready, ``k_ra8_err_hw_timeout`` if not.
 *
 * @pre -- (no preconditions).
 * @post DPHYSFR has been read at least once.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_wait_ready(void);

/* =============================================================================
 * Interrupt path
 * =============================================================================
 */

/**
 * @brief Attach a status callback fired by ``ra8_mipi_phy_dispatch``.
 *
 * @param[in] fn Callback. NULL detaches.
 * @param[in] ctx Opaque value forwarded to the callback.
 *
 * @return ``k_ra8_ok`` always.
 *
 * @pre -- (NULL fn is the documented detach signal).
 * @post Subsequent ``ra8_mipi_phy_dispatch`` calls see ``fn`` / ``ctx``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_attach_handler(ra8_mipi_phy_event_fn_t fn, void* ctx);

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
 * - PWRSF rising -> ``k_ra8_mipi_phy_event_ldo_ready``
 * - PWRSF falling -> ``k_ra8_mipi_phy_event_ldo_lost``
 * - PLLSF rising -> ``k_ra8_mipi_phy_event_pll_locked``
 * - PLLSF falling -> ``k_ra8_mipi_phy_event_pll_lost``
 *
 * If neither bit moved, a single ``k_ra8_mipi_phy_event_status_chg``
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
void ra8_mipi_phy_dispatch(void);

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
 * ``ra8_mipi_phy_deinit``.
 *
 * @return ``ra8_err_t`` error code (see ``ra8_mipi_phy_deinit``).
 *
 * @pre Caller is about to enter graphics-domain-off or standby.
 * @post LDO is off; the PHY draws no current.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_enter_stop(void);

/**
 * @brief Post-standby resume: re-run the start-up procedure.
 *
 * @param[in] cfg Same configuration that was passed to the original
 * ``ra8_mipi_phy_init``.
 *
 * @return ``ra8_err_t`` error code (see ``ra8_mipi_phy_init``).
 *
 * @pre Graphics power domain has been re-enabled and PCLKA is
 * running again (HUM 64.3.1 step 1, p 3837).
 * @post D-PHY is enabled and the PLL is locked.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_exit_stop(const ra8_mipi_phy_config_t* cfg);

/**
 * @brief Enable the D-PHY LDO (DPHYPWRCR.PWRSEN = 1) only.
 *
 * @return ``k_ra8_ok`` after PWRSF stabilises, ``k_ra8_err_hw_timeout``
 * if the LDO never asserts ready.
 *
 * @pre Module-stop is already cleared.
 * @post DPHYPWRCR.PWRSEN reads 1.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_ldo_enable(void);

/**
 * @brief Disable the D-PHY LDO (DPHYPWRCR.PWRSEN = 0) only.
 *
 * @return ``k_ra8_ok`` always.
 *
 * @pre -- (no preconditions; idempotent).
 * @post DPHYPWRCR.PWRSEN reads 0.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_ldo_disable(void);

/**
 * @brief Release the PLL (DPHYPLOCR.PLLSTP = 0) and wait for lock.
 *
 * @return ``k_ra8_ok`` after PLLSF asserts, ``k_ra8_err_hw_timeout`` else.
 *
 * @pre LDO has been brought up (PWRSF = 1).
 * @pre DPHYPLFCR has been programmed.
 * @post DPHYSFR.PLLSF reads 1.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_pll_start(void);

/**
 * @brief Stop the PLL (DPHYPLOCR.PLLSTP = 1).
 *
 * @return ``k_ra8_ok`` always.
 *
 * @pre -- (no preconditions; idempotent).
 * @post DPHYPLOCR.PLLSTP reads 1.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_phy_pll_stop(void);

#ifdef __cplusplus
}
#endif
