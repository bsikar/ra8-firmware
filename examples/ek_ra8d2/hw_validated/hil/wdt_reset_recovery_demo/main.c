/**
 * @file examples/ek_ra8d2/wdt_reset_recovery_demo/main.c
 * @brief WWDT reset + recovery HIL demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Third of three apps under #18. Companion to ``watchdog_demo``
 * (which covers IWDT) -- this one specifically exercises the
 * Window Watchdog Timer (WDT0 / WWDT) reset path: arm WWDT,
 * deliberately stop refreshing, let the WWDT underflow trigger an
 * internal reset, then on the next boot read the reset cause via
 * ``ra_reset_get_cause`` and confirm ``k_ra_reset_cause_wdt0``.
 *
 * Stage A (first boot, power-on cause): print
 * ``wdt_rr: boot reason=power_on``, init WWDT with on_expiry=reset,
 * refresh for a few seconds, then stop refreshing -> WWDT trips
 * an internal reset.
 *
 * Stage B (post-reset boot, wdt0 cause): print
 * ``wdt: reset_by=watchdog``. The HIL probe asserts THIS banner
 * appears inside the boot timeout, which only happens once the
 * stage-A reset has actually fired and the chip has rebooted.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_reset.h"
#include "ra_sci.h"
#include "ra_time.h"
#include "ra_wdt.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_wdt_rr_baud        = 115200U,
  k_wdt_rr_sci_channel = 8U,
  k_wdt_rr_refresh_ms  = 50U,
  k_wdt_rr_arm_iters   = 30U, /**< 30 * 50 ms = 1.5 s alive before stop. */
} wdt_rr_const_t;

static const ra_port_pin_t k_wdt_rr_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_wdt_rr_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

static const uint8_t k_wdt_rr_msg_pwr[]       = "wdt_rr: boot reason=power_on\r\n";
static const uint8_t k_wdt_rr_msg_recovered[] = "wdt: reset_by=watchdog\r\n";
static const uint8_t k_wdt_rr_msg_other[]     = "wdt_rr: boot reason=other\r\n";
static const uint8_t k_wdt_rr_msg_arming[]    = "wdt_rr: arming WWDT for reset\r\n";

static void wdt_rr_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

[[nodiscard]] static ra_err_t wdt_rr_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_wdt_rr_pin_txd, k_ra_psel_sci_async, "wdt_rr.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_wdt_rr_pin_rxd, k_ra_psel_sci_async, "wdt_rr.rxd8");
}

/**
 * @brief Pick the right banner string for ``cause``.
 *
 * @par MC/DC:
 * Compound decision: ``cause == power_on || cause == wdt0`` (default).
 * Two atomic conditions x N+1 = 3 vectors -- power_on (returns pwr msg),
 * wdt0 (returns recovered msg), other (returns other msg).
 */
static const uint8_t* wdt_rr_banner_for(ra_reset_cause_t cause, uint32_t* out_len)
{
  if (cause == k_ra_reset_cause_power_on) {
    *out_len = (uint32_t)(sizeof(k_wdt_rr_msg_pwr) - 1U);
    return k_wdt_rr_msg_pwr;
  }
  if (cause == k_ra_reset_cause_wdt0) {
    *out_len = (uint32_t)(sizeof(k_wdt_rr_msg_recovered) - 1U);
    return k_wdt_rr_msg_recovered;
  }
  *out_len = (uint32_t)(sizeof(k_wdt_rr_msg_other) - 1U);
  return k_wdt_rr_msg_other;
}

static void wdt_rr_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    wdt_rr_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    wdt_rr_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    wdt_rr_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    wdt_rr_panic_halt();
  }
  if (wdt_rr_pins_init() != k_ra_ok) {
    wdt_rr_panic_halt();
  }
  if (ra_reset_init() != k_ra_ok) {
    wdt_rr_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_wdt_rr_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_wdt_rr_sci_channel, &sci_cfg) != k_ra_ok) {
    wdt_rr_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    wdt_rr_panic_halt();
  }
}

/**
 * @brief Stage A: arm WWDT, refresh briefly, then stop and wait for reset.
 *
 * @pre wdt_rr_setup_or_halt has succeeded.
 * @pre WWDT not yet running.
 * @post WWDT is armed and will reset the chip after the refresh budget.
 * @post Function never returns from real silicon (the WWDT underflow
 *       resets the chip mid-WFI).
 *
 * @note Single-threaded; main() is the only caller.
 * @since 0.1.0
 */
static void wdt_rr_arm_and_wait_for_reset(void)
{
  /* on_expiry = reset (RSTIRQS=1) so the WWDT underflow drops us
   * straight into a hardware reset instead of NMI. */
  const ra_wdt_cfg_t cfg = {
    .timeout       = k_ra_wdt_timeout_1024,
    .clock_div     = k_ra_wdt_clkdiv_4,
    .window_start  = k_ra_wdt_window_start_100,
    .window_end    = k_ra_wdt_window_end_0,
    .on_expiry     = k_ra_wdt_on_expiry_reset,
    .stop_in_sleep = k_ra_wdt_sleep_stop_count,
  };
  if (ra_wdt_init(&cfg) != k_ra_ok) {
    wdt_rr_panic_halt();
  }
  (void)ra_sci_write_polling((uint8_t)k_wdt_rr_sci_channel,
                             k_wdt_rr_msg_arming,
                             (uint32_t)(sizeof(k_wdt_rr_msg_arming) - 1U));

  for (uint32_t i = 0U; i < (uint32_t)k_wdt_rr_arm_iters; ++i) {
    (void)ra_wdt_refresh_for(k_ra_wdt0);
    (void)ra_board_led_toggle(k_ra_board_led1);
    ra_delay_ms((uint32_t)k_wdt_rr_refresh_ms);
  }
  /* Stop refreshing -- the WWDT counter will underflow and the chip
   * will reset. The HIL probe scrapes the reset banner on the next
   * boot. */
  while (1) {
    __asm__ volatile("wfi");
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  wdt_rr_setup_or_halt();
  ra_isr_globals_enable();

  ra_reset_cause_t cause = k_ra_reset_cause_unknown;
  (void)ra_reset_get_cause(&cause);
  /* Bench-truth: RSTSRn flags are sticky across resets (HUM Ch 6.2 -
   * the registers note "Only 0 can be written. To clear ..."). With
   * IWDTRF + WDTRF + SWRF all latched from prior bench cycles the
   * decoder's IWDTRF-first priority order would mask out the real
   * cause. Clear every flag so the next reset surfaces cleanly. */
  (void)ra_reset_clear_cause(0x7FFFFFFFU);
  uint32_t       msg_len = 0U;
  const uint8_t* msg     = wdt_rr_banner_for(cause, &msg_len);
  (void)ra_sci_write_polling((uint8_t)k_wdt_rr_sci_channel, msg, msg_len);

  if (cause == k_ra_reset_cause_wdt0) {
    /* Stage B: just recovered from a WWDT reset. Sit here so the
     * HIL probe scrapes the recovery banner. The chip will not
     * reset again until power-cycled or the demo is re-flashed. */
    while (1) {
      __asm__ volatile("wfi");
    }
  }

  /* Stage A: power-on (or any non-WWDT cause). Arm WWDT, refresh
   * briefly, then stop refreshing to trigger the reset. */
  wdt_rr_arm_and_wait_for_reset();
  return 0;
}
#pragma GCC diagnostic pop
