/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/icu_extint_demo/main.c
 * @brief ICU external-interrupt-on-user-button demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Routes the SW1 user push-button (P009 -> IRQ13-DS) through the ICU,
 * configures the IRQCR for falling-edge detection with the digital
 * filter enabled, then polls IRQCR to detect press events. Each press
 * toggles LED1 and emits a short banner over SCI8.
 *
 * Bring-up sequence:
 *   1. CGC + SysTick + UART (SCI8 on PD_02 / PD_03) + LED1 + SW1.
 *   2. ``ra8_icu_init`` to clear all IRQCR/NMI/WUPEN.
 *   3. ``ra8_icu_configure_irq_pin(13, falling_edge + filter)``.
 *   4. Loop: read IRQCR via ``ra8_icu_read_irqcr``; on rising bit log
 *      "icu: irq13\r\n" and toggle LED1.
 *
 * Bare EK-RA8D2; no expansion board.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_icu.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_icu_extint_demo_baud    = 115200U, /**< Icu extint demo baud.    */
  k_icu_extint_demo_poll_ms = 20U,     /**< Icu extint demo poll ms. */
} icu_extint_demo_const_t;

/** @brief SW1 -> IRQ13-DS (EK-RA8D2 UM Table 25 p 32). */
typedef enum : uint8_t {
  k_icu_extint_demo_irq_num   = 13U,   /**< Icu extint demo IRQ number.      */
  k_icu_extint_demo_irqf_mask = 0x40U, /**< IRQCRi.IRQ_DETECT (HUM 14.2.12). */
} icu_extint_demo_irq_t;

static const uint8_t k_icu_extint_demo_msg_press[] = "icu: irq13 press\r\n";
static const uint8_t k_icu_extint_demo_msg_boot[]  = "icu_extint_demo: boot\r\n";

static void icu_extint_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

static void icu_extint_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    icu_extint_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    icu_extint_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    icu_extint_demo_panic_halt();
  }
  if (ra8_icu_init() != k_ra8_ok) {
    icu_extint_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_icu_extint_demo_baud) != k_ra8_ok) {
    icu_extint_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    icu_extint_demo_panic_halt();
  }
  if (ra8_board_sw_init(k_ra8_board_sw1) != k_ra8_ok) {
    icu_extint_demo_panic_halt();
  }
}

/**
 * @brief Configure SW1's IRQ for falling-edge detection w/ digital filter.
 *
 * @par MC/DC:
 * Decision: ``ra8_icu_configure_irq_pin != ok``. One atomic condition
 * x 2 vectors -- golden (this) + null cfg / out-of-range channel
 * (test_app_icu_extint_demo.c).
 */
[[nodiscard]] static ra8_err_t icu_extint_demo_arm(void)
{
  const ra8_icu_irq_cfg_t cfg = {
    .sense      = k_ra8_icu_irqmd_falling,
    .filter_div = k_ra8_icu_fclksel_pclkb_64,
    .filter_en  = true,
  };
  return ra8_icu_configure_irq_pin((uint8_t)k_icu_extint_demo_irq_num, &cfg);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  icu_extint_demo_setup_or_halt();
  ra8_isr_globals_enable();

  if (icu_extint_demo_arm() != k_ra8_ok) {
    icu_extint_demo_panic_halt();
  }

  /* HIL boot banner -- scraped by scripts/hil/run_direct.sh to confirm
   * the CGC + SCI + ICU bring-up reached the main poll loop. */
  (void)ra8_board_uart_console_write(k_icu_extint_demo_msg_boot,
                                     (size_t)(sizeof(k_icu_extint_demo_msg_boot) - 1U));

  while (1) {
    uint8_t irqcr = 0U;
    if (ra8_icu_read_irqcr((uint8_t)k_icu_extint_demo_irq_num, &irqcr) != k_ra8_ok) {
      break;
    }
    if ((irqcr & (uint8_t)k_icu_extint_demo_irqf_mask) != 0U) {
      if (ra8_board_led_toggle(k_ra8_board_led1) != k_ra8_ok) {
        break;
      }
      if (ra8_board_uart_console_write(k_icu_extint_demo_msg_press,
                                       (size_t)(sizeof(k_icu_extint_demo_msg_press) - 1U)) !=
          k_ra8_ok) {
        break;
      }
    }
    ra8_delay_ms((uint32_t)k_icu_extint_demo_poll_ms);
  }
  icu_extint_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
