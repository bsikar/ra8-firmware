/**
 * @file main.c
 * @brief IWDT watchdog + reset-cause demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Two-stage demo:
 *
 *   1. On boot, snapshot the reset cause via ``ra_reset_init`` +
 *      ``ra_reset_get_cause`` and log it over SCI8 -- a power-on
 *      reset reads ``power_on``, while a deliberate IWDT trip from
 *      the previous run reads ``iwdt``.
 *   2. Refresh the IWDT counter for ``k_wdt_demo_alive_seconds``
 *      seconds (LED1 toggles each refresh as a heartbeat), then
 *      stop refreshing and let the IWDT underflow trip a hardware
 *      reset. On the next boot the cause flips to ``iwdt`` and the
 *      cycle repeats.
 *
 * The IWDT period itself is configured by the OFS0 option-setting
 * register at flash-write time (the chip cannot be reconfigured at
 * runtime); ``examples/ek_ra8d2/uart_hello/linker_script.ld`` -- the
 * shared template -- sets a multi-second window so the
 * "stop refreshing" stage takes a visible amount of time before the
 * reset fires.
 *
 * Note: in the simulator (host-side test) ``ra_reset_software_reset``
 * returns; on real silicon it never returns. The app's ``while`` loop
 * is the IWDT-stop stage, terminated only by the chip resetting.
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
#include "ra_iwdt.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_reset.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_wdt_demo_baud          = 115200U,
  k_wdt_demo_sci_channel   = 8U,
  k_wdt_demo_refresh_ms    = 100U,
  k_wdt_demo_alive_seconds = 30U,
} wdt_demo_const_t;

static const ra_port_pin_t k_wdt_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_wdt_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief Banner emitted when the cause is ``power_on``. */
static const uint8_t k_wdt_demo_msg_pwr[] = "wdt: boot reason=power_on\r\n";
/** @brief Banner emitted when the cause is ``iwdt`` (we tripped). */
static const uint8_t k_wdt_demo_msg_wdt[] = "wdt: boot reason=iwdt\r\n";
/** @brief Banner emitted for any other cause. */
static const uint8_t k_wdt_demo_msg_other[] = "wdt: boot reason=other\r\n";
/** @brief Logged just before the demo stops feeding the IWDT. */
static const uint8_t k_wdt_demo_msg_stop[] = "wdt: stopping refresh, expect reset\r\n";

static void wdt_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

[[nodiscard]] static ra_err_t wdt_demo_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_wdt_demo_pin_txd, k_ra_psel_sci_async, "watchdog.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_wdt_demo_pin_rxd, k_ra_psel_sci_async, "watchdog.rxd8");
}

/**
 * @brief Pick the right banner string for ``cause``.
 *
 * @par MC/DC:
 * Compound decision: ``cause == power_on || cause == iwdt`` (default).
 * Two atomic conditions x N+1 = 3 vectors -- power_on (this returns
 * pwr msg), iwdt (returns wdt msg), other cause (returns other msg).
 *
 * @since 0.1.0
 */
static const uint8_t* wdt_demo_banner_for(ra_reset_cause_t cause, uint32_t* out_len)
{
  if (cause == k_ra_reset_cause_power_on) {
    *out_len = (uint32_t)(sizeof(k_wdt_demo_msg_pwr) - 1U);
    return k_wdt_demo_msg_pwr;
  }
  if (cause == k_ra_reset_cause_iwdt) {
    *out_len = (uint32_t)(sizeof(k_wdt_demo_msg_wdt) - 1U);
    return k_wdt_demo_msg_wdt;
  }
  *out_len = (uint32_t)(sizeof(k_wdt_demo_msg_other) - 1U);
  return k_wdt_demo_msg_other;
}

static void wdt_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    wdt_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    wdt_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    wdt_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    wdt_demo_panic_halt();
  }
  if (wdt_demo_pins_init() != k_ra_ok) {
    wdt_demo_panic_halt();
  }
  if (ra_reset_init() != k_ra_ok) {
    wdt_demo_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_wdt_demo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_wdt_demo_sci_channel, &sci_cfg) != k_ra_ok) {
    wdt_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    wdt_demo_panic_halt();
  }
  if (ra_iwdt_init() != k_ra_ok) {
    wdt_demo_panic_halt();
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  wdt_demo_setup_or_halt();
  ra_isr_globals_enable();

  ra_reset_cause_t cause = k_ra_reset_cause_unknown;
  (void)ra_reset_get_cause(&cause);
  uint32_t       msg_len = 0U;
  const uint8_t* msg     = wdt_demo_banner_for(cause, &msg_len);
  (void)ra_sci_write_polling((uint8_t)k_wdt_demo_sci_channel, msg, msg_len);

  /* Stage 1: refresh for ``alive_seconds`` seconds. */
  const uint32_t refreshes_per_sec = 1000U / (uint32_t)k_wdt_demo_refresh_ms;
  const uint32_t total_refreshes   = (uint32_t)k_wdt_demo_alive_seconds * refreshes_per_sec;
  for (uint32_t i = 0U; i < total_refreshes; ++i) {
    ra_iwdt_refresh_deferred();
    (void)ra_board_led_toggle(k_ra_board_led1);
    ra_delay_ms((uint32_t)k_wdt_demo_refresh_ms);
  }

  /* Stage 2: stop refreshing and let the IWDT underflow reset us. */
  (void)ra_sci_write_polling((uint8_t)k_wdt_demo_sci_channel,
                             k_wdt_demo_msg_stop,
                             (uint32_t)(sizeof(k_wdt_demo_msg_stop) - 1U));
  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
