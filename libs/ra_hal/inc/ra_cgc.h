/**
 * @file ra_cgc.h
 * @brief High-level Clock Generation Circuit driver
 *
 * @details
 * Minimal clock-init surface for bring-up. The RA8D2 defaults to MOCO
 * (~8 MHz) out of reset, which is enough to blink an LED and exercise
 * the IOPORT block without touching CGC at all. Use the helpers here
 * to move up to HOCO (20 MHz) or the full PLL1 path (CPUCLK0 up to
 * 1 GHz) as the firmware grows.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra_err.h"

/**
 * @brief Configure the clock tree to a safe default.
 *
 * @details
 * For v0.x bring-up this function is intentionally a no-op: the
 * firmware runs on MOCO (8 MHz) until a real PLL bring-up lands.
 * Returning `k_ra_ok` lets `main()` keep the pattern:
 *
 * @code{.c}
 * ra_infrastructure_init();
 * RA_ERROR_CHECK(ra_cgc_init());
 * // ... drivers ...
 * @endcode
 *
 * which will automatically start using the real implementation once
 * this function is filled out.
 *
 * @return Always `k_ra_ok` today. Will return clock-setup errors once
 * a real PLL routine is wired in.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_cgc_init(void);

/**
 * @brief Switch the system clock source to HOCO (20 MHz).
 *
 * @details
 * Starts the HOCO if it is stopped, waits for it to stabilise, then
 * writes `k_ra_cksel_hoco` to SCKSCR. Requires a PRCR unlock around
 * the SCKSCR write.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Clock switched.
 * @retval k_ra_err_hw_timeout HOCO failed to report ready within the
 * timeout window.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_cgc_use_hoco(void);

/**
 * @enum ra_clock_id_t
 * @brief Identifiers for the clock-tree frequencies queryable at runtime.
 */
typedef enum : uint8_t {
  k_ra_clock_id_cpuclk0 = 0U, /**< Cortex-M85 CPUCLK0. */
  k_ra_clock_id_cpuclk1 = 1U, /**< Cortex-M33 CPUCLK1. */
  k_ra_clock_id_iclk    = 2U, /**< System ICLK. */
  k_ra_clock_id_pclka   = 3U, /**< PCLKA. */
  k_ra_clock_id_pclkb   = 4U, /**< PCLKB. */
  k_ra_clock_id_pclkc   = 5U, /**< PCLKC. */
  k_ra_clock_id_pclkd   = 6U, /**< PCLKD. */
  k_ra_clock_id_pclke   = 7U, /**< PCLKE. */
  k_ra_clock_id_fclk    = 8U, /**< Flash/MRAM interface clock. */
  k_ra_clock_id_mriclk  = 9U, /**< MRAM bus clock. */
} ra_clock_id_t;

/**
 * @brief Query the current frequency of a clock-tree domain.
 *
 * @details
 * Returns the value last programmed by `ra_cgc_init()` (or the
 * reset default MOCO value if `ra_cgc_init()` has not yet run).
 * Drivers that need to compute baud rates or sampling periods
 * should always go through this function rather than hard-coding
 * values from `ra_time_constants.h`.
 *
 * @param[in] id Clock identifier.
 * @param[out] out_hz On success, current frequency in Hz.
 * @return `k_ra_ok` on success, `k_ra_err_invalid_arg` if `out_hz`
 * is NULL or `id` is out of range.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_cgc_get_clock_hz(ra_clock_id_t id, uint32_t* out_hz);

/* =============================================================================
 * runtime reconfigure + oscillation-stop interrupt
 * =============================================================================
 */

/**
 * @typedef ra_cgc_ostd_fn_t
 * @brief Oscillation-stop-detection callback.
 *
 * @param[in] ctx User-supplied context registered with the callback.
 *
 * @note Invoked from ISR context. Must return quickly. The ISR
 * clears the latched OSTDSR flag before the callback runs.
 */
typedef void (*ra_cgc_ostd_fn_t)(void* ctx);

/**
 * @brief Retune PLL1 to a new CPUCLK0 target without a full deinit.
 *
 * @details
 * Walks through the safe transition sequence from HUM Ch 9.2
 * (p 317):
 *
 * 1. Switch SCKSCR to MOCO so the CPU runs on a simple source
 * while PLL1 is being reprogrammed.
 * 2. Stop PLL1 via PLLCR.PLLSTP.
 * 3. Compute the new PLLCCR / PLLCCR2 integer + fractional
 * multipliers from ``new_cpuclk_hz``.
 * 4. Start PLL1 and wait for OSCSF.PLL1SF.
 * 5. Switch SCKSCR back to PLL1.
 * 6. Republish the clock-tree frequencies so subsequent
 * ``ra_cgc_get_clock_hz`` calls see the new value.
 *
 * @param[in] new_cpuclk_hz Target CPUCLK0 frequency in Hz. Must
 * be a multiple of the crystal divided
 * by 32 (the PLLCCR2 fractional step).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Clock switched to the new target.
 * @retval k_ra_err_invalid_arg ``new_cpuclk_hz`` is 0.
 * @retval k_ra_err_hw_timeout PLL lock or MOCO wait timed out.
 *
 * @pre ``ra_cgc_init`` has been called (PLL1 is the current source).
 * @pre IRQs masked or single-threaded init context.
 *
 * @post On success, SCKSCR == PLL1 and the reported CPUCLK0
 * frequency matches ``new_cpuclk_hz`` to within the PLL2
 * fractional resolution.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_cgc_switch_pll1_target(uint32_t new_cpuclk_hz);

/**
 * @brief Enable oscillation-stop detection on the main crystal.
 *
 * @details
 * Programmes OSTDCR to enable the OST detector and installs a
 * callback that fires when the OSTDSR flag is set (oscillator
 * has stopped unexpectedly). The callback runs from the CGC
 * interrupt handler after the driver clears the latched flag.
 *
 * @param[in] handler Callback invoked on oscillation-stop event.
 * Must not be NULL.
 * @param[in] ctx Stored context passed to the callback.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Detector armed.
 * @retval k_ra_err_null_ptr ``handler`` was NULL.
 *
 * @pre ``ra_cgc_init`` has been called.
 * @post OSTDCR.OSTDE is set.
 * @post Callback will fire on the next detected stop event.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_cgc_enable_stop_detection(ra_cgc_ostd_fn_t handler, void* ctx);

/**
 * @brief Disable oscillation-stop detection.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Detector disabled.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post OSTDCR.OSTDE is clear.
 * @post Previously-registered callback is cleared.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_cgc_disable_stop_detection(void);

/**
 * @brief Bring up the USBHS PHY reference clock + module clock.
 *
 * @details
 * The RA8D2 USB 2.0 High-Speed controller drives an internal PHY whose
 * 480 MHz CDR PLL needs a stable 12 MHz reference derived from the
 * main XTAL (24 MHz / 2). The CGC-side gating for that path lives in
 * the System Control block; the procedure (per HUM Ch 9 "Clock
 * Generation Circuit", USBCKCR / USBCKDIVCR description, p 365) is:
 *
 * 1. Verify the main oscillator is stable (OSCSF.MOSCSF = 1). The PHY
 *    cannot lock without a stable XTAL reference.
 * 2. Inside a PRCR-CGC unlock window, the USBHS module clock select
 *    is committed (USBCKCR.USBCKSREQ / USBCKSRDY handshake on
 *    silicon; plain RAM on the host simulator).
 * 3. Re-lock PRCR.
 *
 * After this returns ::k_ra_ok the caller is free to invoke
 * ::ra_mstp_enable for ::k_ra_mstp_usbhs (typically routed through
 * ::ra_usb_device_init / ::ra_usb_host_init) and the controller's
 * SYSCFG.USBE bit will see a clocked PHY underneath it.
 *
 * @return ::ra_err_t error code.
 * @retval k_ra_ok USBHS clock subsystem armed.
 * @retval k_ra_err_hw_timeout Main XTAL never reported stable.
 *
 * @pre  ::ra_cgc_init has been called (main XTAL has been started).
 * @pre  Caller is single-threaded init context.
 *
 * @post OSCSF.MOSCSF = 1 (main XTAL still running).
 * @post USBHS clock subsystem is selected and ready for the MSTP
 *       ungate that follows.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_cgc_usbhs_pll_enable(void);

/**
 * @brief Test helper: invoke the registered OSTD callback.
 *
 * @details
 * Unit tests call this to simulate a CGC oscillation-stop
 * interrupt firing, verifying that the callback + context are
 * correctly stored and that OSTDSR is cleared before the
 * handler runs. Target builds use the real ICU / NVIC path
 * plumbed through ``ra_isr_dispatch`` instead.
 *
 * @pre ``ra_cgc_enable_stop_detection`` has been called.
 * @post OSTDSR is cleared.
 * @post The registered callback ran exactly once.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 *
 * @pre Module state is consistent.
 */
void ra_cgc_sim_trigger_stop_detection(void);

#ifdef __cplusplus
}
#endif
