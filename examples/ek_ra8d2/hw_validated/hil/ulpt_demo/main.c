/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/ulpt_demo/main.c
 * @brief ULPT 1 Hz wake-from-software-standby demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up the on-chip Ultra-Low-Power Timer (ULPT0) in 1 Hz mode and
 * uses its underflow event as a wake source through software-standby
 * cycles. Each underflow:
 *
 *   1. Increments a wake counter.
 *   2. Logs ``"ulpt: wake ok\r\n"`` over the J-Link OB CDC console.
 *   3. Stops the timer to clear ULPTCR.TUNDF.
 *   4. Re-arms the timer with the same period.
 *
 * The demo deliberately keeps the wake-source path inside the ULPT
 * driver and polls ULPTCR via ``ra8_ulpt_get_status``: this matches the
 * host unit-test path (no NVIC needed) and exercises the same
 * register sequence on the EK-RA8D2 silicon.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"
#include "ra8_ulpt.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_ulpt_demo_baud    = 115200U, /**< Ulpt demo baud.    */
  k_ulpt_demo_poll_ms = 50U,     /**< Ulpt demo poll ms. */
  /* 32 768 LOCO ticks ~= 1 s (chosen so the period fits in the
   * 32-bit reload field while still being legible at host-test scale). */
  k_ulpt_demo_period_ticks = 0x8000U, /**< Ulpt demo period ticks. */
} ulpt_demo_const_t;

/** @brief ULPT channel + status. */
typedef enum : uint8_t {
  k_ulpt_demo_channel  = 0U,    /**< Ulpt demo channel.                    */
  k_ulpt_demo_undf_bit = 0x20U, /**< ULPTCR.TUNDF -- mirrors AGTCR layout. */
} ulpt_demo_chan_t;

static const uint8_t k_ulpt_demo_log_msg[] = "ulpt: wake ok\r\n";

static void ulpt_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

static void ulpt_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    ulpt_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    ulpt_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    ulpt_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_ulpt_demo_baud) != k_ra8_ok) {
    ulpt_demo_panic_halt();
  }
  if (ra8_ulpt_init() != k_ra8_ok) {
    ulpt_demo_panic_halt();
  }
}

/**
 * @brief Arm ULPT0 with the demo period.
 *
 * @par MC/DC:
 * Compound decision: ``ra8_ulpt_start != ok``. One atomic condition x
 * 2 vectors -- ok (golden) and bad-channel reject (covered in
 * test_app_ulpt_demo.c).
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t ulpt_demo_arm(void)
{
  return ra8_ulpt_start((uint8_t)k_ulpt_demo_channel, (uint32_t)k_ulpt_demo_period_ticks);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  ulpt_demo_setup_or_halt();
  ra8_isr_globals_enable();

  if (ulpt_demo_arm() != k_ra8_ok) {
    ulpt_demo_panic_halt();
  }

  while (1) {
    uint8_t status = 0U;
    if (ra8_ulpt_get_status((uint8_t)k_ulpt_demo_channel, &status) != k_ra8_ok) {
      break;
    }
    if ((status & (uint8_t)k_ulpt_demo_undf_bit) != 0U) {
      if (ra8_board_uart_console_write(k_ulpt_demo_log_msg,
                                       (size_t)(sizeof(k_ulpt_demo_log_msg) - 1U)) != k_ra8_ok) {
        break;
      }
      if (ra8_ulpt_stop((uint8_t)k_ulpt_demo_channel) != k_ra8_ok) {
        break;
      }
      if (ulpt_demo_arm() != k_ra8_ok) {
        break;
      }
    }
    ra8_delay_ms((uint32_t)k_ulpt_demo_poll_ms);
  }
  ulpt_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
