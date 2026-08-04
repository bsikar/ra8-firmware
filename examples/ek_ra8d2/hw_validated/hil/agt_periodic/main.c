/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/agt_periodic/main.c
 * @brief AGT 1 Hz tick + LED1 blink demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up CGC + SysTick + the BSP UART console (SCI8 on PD_02 /
 * PD_03 via ra8_board_uart_console_init), starts AGT0
 * in free-running mode with a reload value chosen to underflow at
 * roughly 1 Hz, then sits in the main loop polling AGTCR for the
 * underflow flag. Each underflow toggles board LED1 (BLUE) and emits a
 * one-line tick log over the J-Link OB CDC console.
 *
 * The demo deliberately does **not** wire AGT0 into NVIC / ra8_isr -- it
 * polls the AGTCR status bits via ``ra8_agt_get_status`` to keep the
 * code path identical between the EK-RA8D2 target build and the host
 * unit-test build (``RA8_OFF_TARGET``).
 *
 * Sequence:
 *   1. CGC + SysTick + UART bring-up (panic-halt on any error).
 *   2. ``ra8_board_led_init(LED1)``.
 *   3. ``ra8_agt_start_free_run(0, k_agt_periodic_reload)``.
 *   4. Loop: poll AGTCR, on underflow: clear, toggle LED1, log tick.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_agt.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_agt_periodic_baud    = 115200U, /**< AGT periodic baud.    */
  k_agt_periodic_poll_ms = 10U,     /**< AGT periodic poll ms. */
} agt_periodic_const_t;

/** @brief AGT channel + reload (16-bit, ~1 Hz at PCLKB / 8192 div). */
typedef enum : uint16_t {
  k_agt_periodic_channel = 0U, /**< AGT periodic channel. */
  /* Reload tuned for the fake -- on real silicon AGT prescaler
   * setup happens inside ra8_agt_start_free_run. */
  k_agt_periodic_reload = 0x7FFFU, /**< AGT periodic reload. */
} agt_periodic_timer_t;

/** @brief AGTCR underflow status bit (HUM Ch 24, AGTCR.TUNDF). */
typedef enum : uint8_t {
  k_agt_periodic_undf_bit = 0x20U, /**< AGT periodic undf bit. */
} agt_periodic_status_t;

static const uint8_t k_agt_periodic_log_msg[] = "agt: tick OK\r\n";

static void agt_periodic_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

static void agt_periodic_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    agt_periodic_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    agt_periodic_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    agt_periodic_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_agt_periodic_baud) != k_ra8_ok) {
    agt_periodic_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    agt_periodic_panic_halt();
  }
}

/**
 * @brief Arm AGT0 in free-running mode.
 *
 * @par MC/DC:
 * Compound decision: ``ra8_agt_start_free_run != ok``. One atomic
 * condition x 2 vectors -- ok (golden) and bad-channel reject
 * (covered in test_app_agt_periodic.c).
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t agt_periodic_arm(void)
{
  return ra8_agt_start_free_run((uint8_t)k_agt_periodic_channel, (uint16_t)k_agt_periodic_reload);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  agt_periodic_setup_or_halt();
  ra8_isr_globals_enable();

  if (agt_periodic_arm() != k_ra8_ok) {
    agt_periodic_panic_halt();
  }

  while (1) {
    uint8_t status = 0U;
    if (ra8_agt_get_status((uint8_t)k_agt_periodic_channel, &status) != k_ra8_ok) {
      break;
    }
    if ((status & (uint8_t)k_agt_periodic_undf_bit) != 0U) {
      if (ra8_board_led_toggle(k_ra8_board_led1) != k_ra8_ok) {
        break;
      }
      if (ra8_board_uart_console_write(k_agt_periodic_log_msg,
                                       (size_t)(sizeof(k_agt_periodic_log_msg) - 1U)) != k_ra8_ok) {
        break;
      }
      /* Re-arm: stop + start clears AGTCR.TUNDF on real silicon. */
      if (ra8_agt_stop((uint8_t)k_agt_periodic_channel) != k_ra8_ok) {
        break;
      }
      if (agt_periodic_arm() != k_ra8_ok) {
        break;
      }
    }
    ra8_delay_ms((uint32_t)k_agt_periodic_poll_ms);
  }
  agt_periodic_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
