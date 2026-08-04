/**
 * @file ra8_nsc_wdt.c
 * @brief NSC veneers: secure-side watchdog arm + refresh
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * The WDT (``ra8_wdt``, HUM Ch 27) is a Secure-owned peripheral -- only the
 * Secure world programmes WDTCR / WDTRR. The Non-Secure e-reader runs the
 * ThreadX watchdog supervisor (``ra8_wdt_supervisor``), which decides in NS
 * whether every worker thread is alive and, when so, must refresh the WDT
 * down-counter. NS cannot touch WDTRR directly, so these two argument-free
 * veneers give it a Secure gateway:
 *
 *  - ``ra8_nsc_wdt_start`` -- arm the counter once, with a fixed Secure-side
 *    configuration, just before the supervisor thread begins refreshing it.
 *  - ``ra8_nsc_wdt_refresh`` -- refresh the down-counter on the supervisor's
 *    cadence. It returns ``void`` so it installs directly as the supervisor's
 *    ``ra8_wdt_sup_refresh_fn_t`` hook.
 *
 * Neither veneer takes a pointer argument, so no ``cmse_check_address_range``
 * is required: nothing crosses the boundary that must be range-checked.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_nsc.h"
#include "ra8_nsc_veneer.h"
#include "ra8_wdt.h"

/**
 * @var s_wdt_cfg
 * @brief Fixed Secure-side WDT configuration used by ::ra8_nsc_wdt_start.
 *
 * @details
 * Mirrors the validated ``wdt_supervisor_demo`` configuration: a fully-open
 * refresh window (``window_start_100`` / ``window_end_0`` -- no upper or lower
 * bound, so any refresh before the timeout is accepted), the counter halted in
 * Sleep, and expiry routed to NMI rather than an internal reset (switching to
 * ``k_ra8_wdt_on_expiry_reset`` is a separate product decision). Living in the
 * Secure ``.rodata`` it is never reachable from NS.
 *
 * @note Access restriction: Secure world only; the veneers copy nothing from NS.
 * @warning Do not point NS code at this object; it is a Secure-side constant.
 * @since 0.1.0
 */
static const ra8_wdt_cfg_t s_wdt_cfg = {
  .timeout       = k_ra8_wdt_timeout_1024,
  .clock_div     = k_ra8_wdt_clkdiv_4,
  .window_start  = k_ra8_wdt_window_start_100,
  .window_end    = k_ra8_wdt_window_end_0,
  .on_expiry     = k_ra8_wdt_on_expiry_nmi,
  .stop_in_sleep = k_ra8_wdt_sleep_stop_count,
};

/**
 * @brief NSC veneer: arm the Secure WDT with the e-reader configuration.
 *
 * @details
 * Forwards to ``ra8_wdt_init`` with the fixed ::s_wdt_cfg. In register-start
 * mode ``ra8_wdt_init`` programmes WDTCR/WDTRCR/WDTCSTPR and issues the
 * unlock sequence that arms the down-counter, so the NS caller should invoke
 * this immediately before starting the supervisor thread that refreshes it --
 * keeping the arm-to-first-refresh gap small (as the flat demo does).
 *
 * @return ``ra8_err_t`` outcome from ``ra8_wdt_init``.
 * @retval k_ra8_ok            WDT configured and armed.
 * @retval k_ra8_err_invalid_arg The fixed configuration was rejected by the driver.
 *
 * @pre The Secure substrate is initialized (clocks running).
 * @pre No prior code has armed the WDT (single arm per boot).
 * @post The WDT down-counter is running and expects a refresh within the timeout.
 * @post WDTCR reflects ::s_wdt_cfg.
 *
 * @note Thread safety: call once, from the NS boot thread, before the supervisor
 *       starts. Not re-entrant.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_wdt_start(void)
{
  return ra8_wdt_init(&s_wdt_cfg);
}

/**
 * @brief NSC veneer: refresh the Secure WDT down-counter.
 *
 * @details
 * Forwards to ``ra8_wdt_refresh_deferred`` (WDTRR heartbeat). Installed as the
 * ThreadX supervisor's ``ra8_wdt_sup_refresh_fn_t`` hook so the supervisor kicks
 * the WDT only when every registered NS thread has checked in on time.
 *
 * @return Nothing.
 * @note This function does not return a value.
 *
 * @pre ::ra8_nsc_wdt_start has armed the WDT.
 * @pre Called from the NS supervisor thread on its refresh cadence.
 * @post The WDT down-counter has been reloaded (refresh issued).
 * @post No NS-visible state is modified.
 *
 * @note Thread safety: the supervisor calls this from one thread only.
 * @since 0.1.0
 */
RA8_NSC_VENEER void ra8_nsc_wdt_refresh(void)
{
  ra8_wdt_refresh_deferred();
}
