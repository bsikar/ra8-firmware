/**
 * @file examples/ek_ra8d2/hw_validated/hil/wdt_window_demo/src/main.c
 * @brief WWDT (window watchdog) windowed-refresh demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Companion to ``iwdt_demo`` -- exercises the *separate* Window
 * Watchdog (WWDT) peripheral via ``ra8_wdt.h``. The IWDT is hard-wired
 * to OFS0 option-setting flash and has a fixed period; the WWDT is
 * configured at runtime via ``ra8_wdt_init``, including the lower /
 * upper refresh-window bounds. Without this app, the WWDT driver had
 * no end-to-end bench validation.
 *
 * Bring-up sequence:
 *   1. CGC + SysTick + UART (SCI8) + LED1.
 *   2. ``ra8_wdt_init`` with timeout=16384 cycles, clkdiv=PCLKB/8192,
 *      window_start=75%, window_end=25%, on_expiry=reset.
 *   3. Loop: poll counter; refresh only when the counter sits inside
 *      the legal 25..75% window; print banner; toggle LED1 each
 *      successful refresh.
 *
 * Acceptance: at least 5 ``wdt: window_tick=ok`` banners visible in
 * the boot-timeout window, no fault banners.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"
#include "ra8_wdt.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_wdt_window_demo_baud    = 115200U, /**< Wdt window demo baud.    */
  k_wdt_window_demo_poll_ms = 5U,      /**< Wdt window demo poll ms. */
} wdt_window_demo_const_t;

/**
 * @brief Window bounds in WDT-counter units.
 *
 * @details The WWDT counter counts DOWN from the timeout value
 * (1024 for ``k_ra8_wdt_timeout_1024``). Refresh is legal between
 * 25 % and 75 % of the period:
 *   - 25 % of 1024 = 256 = lower bound
 *   - 75 % of 1024 = 768 = upper bound
 * Outside the window the refresh raises a window violation. Bench
 * measurement: the WWDT counts on a ~40 Hz base clock (chip's WDT
 * clock source -- not PCLKB despite the HUM nomenclature), so the
 * 1024-cycle period runs ~25 s. The 25..75 % window is half of
 * that, ~12.5 s, which fits inside the 15 s HIL gate without
 * missing the window.
 */
typedef enum : uint16_t {
  k_wdt_window_demo_window_low  = 256U, /**< Wdt window demo window low.  */
  k_wdt_window_demo_window_high = 768U, /**< Wdt window demo window high. */
} wdt_window_demo_window_t;

static const uint8_t k_wdt_window_demo_msg_refresh[] = "wdt: window_tick=ok\r\n";
static const uint8_t k_wdt_window_demo_msg_boot[]    = "wdt: window-mode boot\r\n";

static void wdt_window_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

static void wdt_window_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    wdt_window_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    wdt_window_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    wdt_window_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_wdt_window_demo_baud) != k_ra8_ok) {
    wdt_window_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    wdt_window_demo_panic_halt();
  }
}

/**
 * @brief Decide whether the live counter sits inside the legal window.
 *
 * @par MC/DC:
 * Compound decision: ``counter >= low && counter <= high``. Two
 * atomic conditions x 3 vectors -- both true (golden), low fail,
 * high fail (test_app_wdt_window_demo.c).
 */
static bool wdt_window_demo_in_window(uint16_t counter)
{
  return (counter >= (uint16_t)k_wdt_window_demo_window_low) &&
         (counter <= (uint16_t)k_wdt_window_demo_window_high);
}

/**
 * @brief Print a short ASCII banner and never block the caller.
 *
 * @param[in] msg NUL-terminated string literal.
 * @param[in] len Number of bytes to write (excluding any NUL).
 *
 * @pre msg != nullptr.
 * @pre SCI8 console has been brought up by setup_or_halt.
 * @post Bytes queued in the SCI8 TX FIFO best-effort.
 * @post No side effects on failure (errors swallowed).
 *
 * @note Not thread-safe; only main thread calls this.
 * @since 0.1.0
 */
static void wdt_window_demo_banner(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

static const uint8_t k_wdt_window_demo_msg_init_failed[]    = "wdt: init failed\r\n";
static const uint8_t k_wdt_window_demo_msg_counter_failed[] = "wdt: get_counter failed\r\n";
static const uint8_t k_wdt_window_demo_msg_refresh_failed[] = "wdt: refresh_for failed\r\n";
static const uint8_t k_wdt_window_demo_msg_clear_failed[]   = "wdt: clear_status failed\r\n";

/**
 * @brief Inner loop body -- one polling iteration.
 *
 * @return true to continue, false to break out and panic-halt.
 *
 * @pre WWDT has been ra8_wdt_init'd.
 * @pre SCI8 console up.
 * @post On true return, no driver error occurred this iteration.
 * @post On false return, an SCI error banner has been emitted.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool wdt_window_demo_iter(void)
{
  uint16_t counter = 0U;
  if (ra8_wdt_get_counter(&counter) != k_ra8_ok) {
    wdt_window_demo_banner(k_wdt_window_demo_msg_counter_failed,
                           (uint32_t)(sizeof(k_wdt_window_demo_msg_counter_failed) - 1U));
    return false;
  }
  if (!wdt_window_demo_in_window(counter)) {
    return true;
  }
  if (ra8_wdt_refresh_for(k_ra8_wdt0) != k_ra8_ok) {
    wdt_window_demo_banner(k_wdt_window_demo_msg_refresh_failed,
                           (uint32_t)(sizeof(k_wdt_window_demo_msg_refresh_failed) - 1U));
    return false;
  }
  if (ra8_wdt_clear_status() != k_ra8_ok) {
    wdt_window_demo_banner(k_wdt_window_demo_msg_clear_failed,
                           (uint32_t)(sizeof(k_wdt_window_demo_msg_clear_failed) - 1U));
    return false;
  }
  (void)ra8_board_led_toggle(k_ra8_board_led1);
  wdt_window_demo_banner(k_wdt_window_demo_msg_refresh,
                         (uint32_t)(sizeof(k_wdt_window_demo_msg_refresh) - 1U));
  return true;
}

void main(void)
{
  wdt_window_demo_setup_or_halt();
  ra8_isr_globals_enable();

  /* Boot banner -- emit before ra8_wdt_init so the HIL host can see
   * the firmware booted even if the WDT bring-up fails. */
  (void)ra8_board_uart_console_write(k_wdt_window_demo_msg_boot,
                                     (size_t)(sizeof(k_wdt_window_demo_msg_boot) - 1U));

  /* on_expiry=k_ra8_wdt_on_expiry_nmi (RSTIRQS=0) routes a window
   * violation through NMI/IRQ instead of a silent reset, so a bug
   * in the demo's window math shows up as a banner-loss rather than
   * a reset-loop that confuses HIL bench parsing. */
  const ra8_wdt_cfg_t cfg = {
    .timeout       = k_ra8_wdt_timeout_1024,
    .clock_div     = k_ra8_wdt_clkdiv_4,
    .window_start  = k_ra8_wdt_window_start_75,
    .window_end    = k_ra8_wdt_window_end_25,
    .on_expiry     = k_ra8_wdt_on_expiry_nmi,
    .stop_in_sleep = k_ra8_wdt_sleep_stop_count,
  };
  if (ra8_wdt_init(&cfg) != k_ra8_ok) {
    wdt_window_demo_banner(k_wdt_window_demo_msg_init_failed,
                           (uint32_t)(sizeof(k_wdt_window_demo_msg_init_failed) - 1U));
    wdt_window_demo_panic_halt();
  }

  while (wdt_window_demo_iter()) {
    ra8_delay_ms((uint32_t)k_wdt_window_demo_poll_ms);
  }
  wdt_window_demo_panic_halt();
}
