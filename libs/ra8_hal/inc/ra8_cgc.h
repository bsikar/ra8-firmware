/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_cgc.h
 * @brief High-level Clock Generation Circuit driver
 * @ingroup grp_hal_system
 *
 * @details
 * Minimal clock-init surface for bring-up. The RA8D2 defaults to MOCO
 * (~8 MHz) out of reset, which is enough to blink an LED and exercise
 * the IOPORT block without touching CGC at all. Use the helpers here
 * to move up to HOCO (20 MHz) or the full PLL1 path (CPUCLK0 up to
 * 1 GHz) as the firmware grows.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_cgc_regs.h"
#include "ra8_err.h"

/**
 * @brief Configure the clock tree to a safe default.
 *
 * @details
 * For v0.x bring-up this function is intentionally a no-op: the
 * firmware runs on MOCO (8 MHz) until a real PLL bring-up lands.
 * Returning `k_ra8_ok` lets `main()` keep the pattern:
 *
 * @code{.c}
 * ra8_infrastructure_init();
 * RA8_ERROR_CHECK(ra8_cgc_init());
 * // ... drivers ...
 * @endcode
 *
 * which will automatically start using the real implementation once
 * this function is filled out.
 *
 * @return Always `k_ra8_ok` today. Will return clock-setup errors once
 * a real PLL routine is wired in.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cgc_init(void);

/**
 * @brief Switch the system clock source to HOCO (20 MHz).
 *
 * @details
 * Starts the HOCO if it is stopped, waits for it to stabilise, then
 * writes `k_ra8_cksel_hoco` to SCKSCR. Requires a PRCR unlock around
 * the SCKSCR write.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Clock switched.
 * @retval k_ra8_err_hw_timeout HOCO failed to report ready within the
 * timeout window.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cgc_use_hoco(void);

/**
 * @enum ra8_clock_id_t
 * @brief Identifiers for the clock-tree frequencies queryable at runtime.
 */
typedef enum : uint8_t {
  k_ra8_clock_id_cpuclk0 = 0U, /**< Cortex-M85 CPUCLK0.         */
  k_ra8_clock_id_cpuclk1 = 1U, /**< Cortex-M33 CPUCLK1.         */
  k_ra8_clock_id_iclk    = 2U, /**< System ICLK.                */
  k_ra8_clock_id_pclka   = 3U, /**< PCLKA.                      */
  k_ra8_clock_id_pclkb   = 4U, /**< PCLKB.                      */
  k_ra8_clock_id_pclkc   = 5U, /**< PCLKC.                      */
  k_ra8_clock_id_pclkd   = 6U, /**< PCLKD.                      */
  k_ra8_clock_id_pclke   = 7U, /**< PCLKE.                      */
  k_ra8_clock_id_fclk    = 8U, /**< Flash/MRAM interface clock. */
  k_ra8_clock_id_mriclk  = 9U, /**< MRAM bus clock.             */
} ra8_clock_id_t;

/**
 * @brief Query the current frequency of a clock-tree domain.
 *
 * @details
 * Returns the value last programmed by `ra8_cgc_init()` (or the
 * reset default MOCO value if `ra8_cgc_init()` has not yet run).
 * Drivers that need to compute baud rates or sampling periods
 * should always go through this function rather than hard-coding
 * values from `ra8_time_constants.h`.
 *
 * @param[in] id Clock identifier.
 * @param[out] out_hz On success, current frequency in Hz.
 * @return `k_ra8_ok` on success, `k_ra8_err_invalid_arg` if `out_hz`
 * is NULL or `id` is out of range.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cgc_get_clock_hz(ra8_clock_id_t id, uint32_t* out_hz);

/* =============================================================================
 * runtime reconfigure + oscillation-stop interrupt
 * =============================================================================
 */

/**
 * @typedef ra8_cgc_ostd_fn_t
 * @brief Oscillation-stop-detection callback.
 *
 * @param[in] ctx User-supplied context registered with the callback.
 *
 * @note Invoked from ISR context. Must return quickly. The ISR
 * clears the latched OSTDSR flag before the callback runs.
 */
typedef void (*ra8_cgc_ostd_fn_t)(void* ctx);

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
 * ``ra8_cgc_get_clock_hz`` calls see the new value.
 *
 * @param[in] new_cpuclk_hz Target CPUCLK0 frequency in Hz. Must
 * be a multiple of the crystal divided
 * by 32 (the PLLCCR2 fractional step).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Clock switched to the new target.
 * @retval k_ra8_err_invalid_arg ``new_cpuclk_hz`` is 0.
 * @retval k_ra8_err_hw_timeout PLL lock or MOCO wait timed out.
 *
 * @pre ``ra8_cgc_init`` has been called (PLL1 is the current source).
 * @pre IRQs masked or single-threaded init context.
 *
 * @post On success, SCKSCR == PLL1 and the reported CPUCLK0
 * frequency matches ``new_cpuclk_hz`` to within the PLL2
 * fractional resolution.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cgc_switch_pll1_target(uint32_t new_cpuclk_hz);

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
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Detector armed.
 * @retval k_ra8_err_null_ptr ``handler`` was NULL.
 *
 * @pre ``ra8_cgc_init`` has been called.
 * @post OSTDCR.OSTDE is set.
 * @post Callback will fire on the next detected stop event.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cgc_enable_stop_detection(ra8_cgc_ostd_fn_t handler, void* ctx);

/**
 * @brief Disable oscillation-stop detection.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Detector disabled.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post OSTDCR.OSTDE is clear.
 * @post Previously-registered callback is cleared.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cgc_disable_stop_detection(void);

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
 *    silicon; plain RAM on the host fake).
 * 3. Re-lock PRCR.
 *
 * After this returns ::k_ra8_ok the caller is free to invoke
 * ::ra8_mstp_enable for ::k_ra8_mstp_usbhs (typically routed through
 * ::ra8_usb_device_init / ::ra8_usb_host_init) and the controller's
 * SYSCFG.USBE bit will see a clocked PHY underneath it.
 *
 * @return ::ra8_err_t error code.
 * @retval k_ra8_ok USBHS clock subsystem armed.
 * @retval k_ra8_err_hw_timeout Main XTAL never reported stable.
 *
 * @pre  ::ra8_cgc_init has been called (main XTAL has been started).
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
[[nodiscard]] ra8_err_t ra8_cgc_usbhs_pll_enable(void);

/**
 * @brief Bring up the Ethernet Switch Module (ESWM / RMAC) clock.
 *
 * @details
 * On RA8D2 the RMAC PHY-station-management hardware (MPSM/MPIC) is
 * fed by ESWCLK, a dedicated peripheral clock with its own source-
 * select (ESWCKCR) and divider (ESWCKDIVCR) registers. After power-on
 * reset ESWCKCR reads as 0 (ESWCKSEL = HOCO ~20 MHz) which is fine for
 * MDC but the source-select handshake (CKSREQ -> CKSRDY) must still be
 * exercised for the divider write to take effect. Symptom on a chip
 * where this routine has not run: ``ra8_rmac_mdio_c22_read`` returns
 * ``k_ra8_err_hw_timeout`` for every PHY address even though the wire is
 * physically connected and the PHY is out of reset.
 *
 * Sequence per FSP ``bsp_peripheral_clock_set`` and the HUM Ch 9
 * "Clock selection switching procedure":
 *   1. PRCR-CGC unlock window.
 *   2. ESWCKCR.CKSREQ = 1, wait CKSRDY = 1 (clock gated, switchable).
 *   3. Write ESWCKDIVCR.
 *   4. Write ESWCKCR = ``source`` | CKSREQ.
 *   5. Write ESWCKCR = ``source`` (drop the request bit).
 *   6. Wait CKSRDY = 0 (new clock running).
 *   7. PRCR re-lock.
 *
 * This implementation picks PLL1P / 8 = 125 MHz as the ESWCLK source,
 * matching what every FSP example for the EK-RA8D2 ships with. The
 * resulting 125 MHz fans out to:
 *   - ESWM's per-port descriptor-ring logic.
 *   - The MPIC.PSMCS divider chain (-> 1 MHz MDC after ra8_rmac_init).
 *
 * The same eswclk_hz value is what the caller passes via
 * ::ra8_rmac_config_t::eswclk_hz so ``internal_calc_psmcs`` can
 * compute the right MPIC.PSMCS divider code.
 *
 * @return ::ra8_err_t error code.
 * @retval k_ra8_ok            ESWCLK selected and running.
 * @retval k_ra8_err_hw_timeout CKSRDY handshake never completed.
 *
 * @pre  ::ra8_cgc_init has been called (PLL1 locked).
 * @pre  Single-threaded init context.
 *
 * @post ESWCKCR.CKSEL = ::k_ra8_eswcksel_pll1p, ESWCKDIVCR = /1.
 * @post Subsequent ``ra8_rmac_init`` calls will see a live ESWCLK.
 * @post PRCR is re-locked.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cgc_eswclk_init(void);

/**
 * @brief Query the current ESWCLK (Ethernet Switch clock) frequency.
 *
 * @details
 * Computed from the most recent ::ra8_cgc_eswclk_init call. Returns 0
 * if the ESWCLK has not been configured. Used by ::ra8_rmac_init
 * (callers fill ``ra8_rmac_config_t.eswclk_hz`` from this value) to
 * compute the MPIC.PSMCS divider code.
 *
 * @param[out] out_hz On success, the live ESWCLK frequency in Hz.
 *
 * @return ::ra8_err_t error code.
 * @retval k_ra8_ok           ESWCLK frequency written to *out_hz.
 * @retval k_ra8_err_null_ptr out_hz is nullptr.
 *
 * @pre  out_hz is a valid 32-bit pointer.
 * @post On k_ra8_ok, *out_hz holds the live ESWCLK frequency
 *       (0 if ::ra8_cgc_eswclk_init has not yet succeeded).
 *
 * @note Thread-safe (read-only access to a published value).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cgc_eswclk_hz(uint32_t* out_hz);

/**
 * @brief Bring up PLL2 with caller-supplied multiplier and P divider.
 *
 * @details
 * Sequence (mirrors FSP `bsp_clocks.c` PLL2 path and HUM Ch 9.2.10..12):
 *   1. PRCR-CGC unlock window.
 *   2. Stop PLL2 (PLL2CR.PLL2STP = 1) and wait OSCSF.PLL2SF = 0.
 *   3. Programme PLL2CCR (input divider, source, multiplier) and
 *      PLL2CCR2 (output dividers).
 *   4. Start PLL2 (PLL2CR.PLL2STP = 0) and wait OSCSF.PLL2SF = 1.
 *   5. PRCR re-lock.
 *
 * The multiplier is encoded the same way PLL1 encodes it: the integer
 * part goes in `PLL2MUL[15:8]` and the quarter-step fractional part in
 * `PLL2MULNF[7:6]`. Caller passes the *integer* multiplier (1..255) and
 * the quarter-steps (0..3) separately.
 *
 * Output dividers use the standard ::ra8_plodiv_t code-to-ratio map
 * (e.g. /4 = 3, /5 = 4, /6 = 5).
 *
 * VCO frequency MUST land in the 960..2400 MHz range required by the
 * RA8D2 silicon (per `BSP_FEATURE_CGC_PLLCCR_VCO_*`). PLL2 output
 * frequency MUST land in 60..1200 MHz (`BSP_FEATURE_CGC_PLL2_OUT_*`).
 * The driver does not enforce this; the caller is responsible.
 *
 * Input source is fixed to the main XTAL on EK-RA8D2 (PL2SRCSEL = 0)
 * and input divider is fixed to /2 so the PLL pre-scale lands at
 * 12 MHz, matching what the FSP quickstart does for the 24 MHz crystal.
 *
 * @param[in] mul_int   Integer multiplier (1..255). Effective multiplier
 *                      is ``mul_int + (mul_quarters / 4)``.
 * @param[in] mul_quarters Fractional quarter-steps (0..3).
 * @param[in] p_div_code Output divider P code from ::ra8_plodiv_t
 *                      (e.g. ::k_ra8_plodiv_div4 for /4).
 *
 * @return ::ra8_err_t error code.
 * @retval k_ra8_ok            PLL2 locked.
 * @retval k_ra8_err_invalid_arg ``mul_int`` is 0 or ``mul_quarters > 3``.
 * @retval k_ra8_err_hw_timeout PLL2SF stop or start handshake exhausted
 *                              its spin budget.
 *
 * @pre  ::ra8_cgc_init has been called (main XTAL stable).
 * @pre  Single-threaded init context.
 * @pre  CPU is not currently sourced from PLL2.
 *
 * @post On k_ra8_ok return, PLL2 is locked and ready to be selected as a
 *       USBCKCR source.
 * @post PRCR is re-locked.
 *
 * @note Not thread-safe.
 *
 * @par Example:
 * @code
 * // 24 MHz / 2 (fixed) * 80 = 960 MHz VCO; /4 -> 240 MHz PLL2P.
 * ra8_cgc_pll2_enable(80U, 0U, k_ra8_plodiv_div4);
 * @endcode
 *
 * @see ra8_cgc_usbfs_clock_enable
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_cgc_pll2_enable(uint8_t mul_int, uint8_t mul_quarters, ra8_plodiv_t p_div_code);

/**
 * @brief Bring up the USB-FS module clock (USBCKCR / USBCKDIVCR).
 *
 * @details
 * On RA8D2 the USB-FS controller is not clocked merely by ungating
 * MSTPCRB.MSTPB11 -- the System Control block has a dedicated
 * USBCKCR / USBCKDIVCR pair (HUM Ch 9, USBCKCR p 365) that selects
 * the source clock for the controller's 48 MHz reference. After
 * power-on reset USBCKCR reads as 0 (USBCKSEL = HOCO, ~20 MHz)
 * which is **not** a valid USB-FS reference, and the SIE will never
 * bus-reset a connected device. Symptom on EK-RA8D2: DVSTCTR0 stuck
 * at 0x0002 (Powered, never Default), INTSTS0.CTRT = 0, no SETUP
 * packet ever delivered.
 *
 * Sequence per FSP `bsp_clocks.c` and HUM Ch 9 "Clock
 * selection switching procedure" (canonical example: SPICKCR p 370):
 *   0. Force MSTPCRB.MSTPB11 = 1 (USBFS module-stopped). HUM step 1
 *      requires this whenever USBCKDIVCR is moved off 1/1; we are
 *      programming /5 below, and a caller that has already released
 *      MSTPB11 (e.g. by invoking ::ra8_usb_device_init first) will
 *      otherwise hang the SREQ -> SRDY handshake silently.
 *   1. PRCR-CGC unlock window.
 *   2. USBCKCR.USBCKSREQ = 1, wait USBCKSRDY = 1 (clock gated).
 *   3. Write USBCKDIVCR.
 *   4. Write USBCKCR = src | USBCKSREQ.
 *   5. Write USBCKCR = src (drop the request bit).
 *   6. Wait USBCKSRDY = 0 (new clock running).
 *   7. PRCR re-lock.
 *
 * Source picked here is PLL2P with USBCKDIVCR = /5 codepoint.
 * PLL2 is brought up via ::ra8_cgc_pll2_enable to land on
 * VCO = 24 MHz / 2 * 80 = 960 MHz, then PL2ODIVP = /4 -> PLL2P =
 * 240 MHz. USBCKDIVCR = /5 then yields exactly 48.000 MHz, which is
 * within the USB-FS spec (48 MHz +/- 2500 ppm = +/- 0.25 %).
 *
 * @return ::ra8_err_t error code.
 * @retval k_ra8_ok USB-FS module clock running.
 * @retval k_ra8_err_hw_timeout USBCKSRDY handshake never completed.
 *
 * @pre  ::ra8_cgc_init has been called (PLL1 locked).
 * @pre  Single-threaded init context.
 * @pre  Should run BEFORE any caller releases MSTPB11 (USBFS) -- the
 *       routine forces MSTPB11 = 1 internally for safety, but invoking
 *       this from main() before any USB driver init is the documented
 *       call order.
 *
 * @post USBCKCR.USBCKSEL = ::k_ra8_usbcksel_pll2p.
 * @post USBCKDIVCR programmed for /5 (codepoint 6).
 * @post MSTPCRB.MSTPB11 = 1 (USBFS module-stopped); the USB driver
 *       must subsequently release it via ::ra8_mstp_enable / its
 *       ::ra8_usb_device_init wrapper.
 * @post PRCR is re-locked.
 *
 * @note Not thread-safe.
 *
 * @see ra8_cgc_usbhs_pll_enable
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cgc_usbfs_clock_enable(void);

/**
 * @brief Test helper: invoke the registered OSTD callback.
 *
 * @details
 * Unit tests call this to simulate a CGC oscillation-stop
 * interrupt firing, verifying that the callback + context are
 * correctly stored and that OSTDSR is cleared before the
 * handler runs. Target builds use the real ICU / NVIC path
 * plumbed through ``ra8_isr_dispatch`` instead.
 *
 * @pre ``ra8_cgc_enable_stop_detection`` has been called.
 * @post OSTDSR is cleared.
 * @post The registered callback ran exactly once.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 *
 * @pre Module state is consistent.
 */
void ra8_cgc_fake_trigger_stop_detection(void);

#ifdef __cplusplus
}
#endif
