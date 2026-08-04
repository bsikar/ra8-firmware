/**
 * @file examples/ek_ra8d2/hw_validated/hil/kint_demo/main.c
 * @brief Key-interrupt (KINT / IRQ pin) input demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up the on-board user switch SW1 (P009 -> IRQ13-DS, EK-RA8D2
 * UM Table 25 p 32) as a falling-edge external interrupt source via
 * ``ra8_icu_configure_irq_pin``. Each press logs a one-line message
 * over the J-Link OB CDC console (SCI8). The implementation polls the
 * ICU status flag rather than wiring the NVIC -- the goal is to
 * exercise the IRQCRb register programming path on a stock board with
 * no expansion modules required.
 *
 * Sequence:
 *   1. CGC + SysTick + UART (SCI8) bring-up.
 *   2. ``ra8_icu_init()``.
 *   3. ``ra8_board_sw_init(SW1)``.
 *   4. ``ra8_icu_configure_irq_pin(13, falling-edge + filter)``.
 *   5. Loop: poll ``ra8_board_sw_read``; log on press, debounce, repeat.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
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
  k_kint_demo_baud        = 115200U, /**< Kint demo baud.        */
  k_kint_demo_poll_ms     = 20U,     /**< Kint demo poll ms.     */
  k_kint_demo_debounce_ms = 50U,     /**< Kint demo debounce ms. */
} kint_demo_const_t;

/** @brief KIN0 / IRQ channel selection (SW1 -> IRQ13-DS). */
typedef enum : uint8_t {
  k_kint_demo_irq_channel = 13U, /**< k_ra8_board_sw1_irq. */
} kint_demo_irq_t;

static const uint8_t k_kint_demo_log_msg[]  = "kint: SW1 press\r\n";
static const uint8_t k_kint_demo_boot_msg[] = "kint_demo: boot\r\n";

static void kint_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

static void kint_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    kint_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    kint_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    kint_demo_panic_halt();
  }
  if (ra8_icu_init() != k_ra8_ok) {
    kint_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_kint_demo_baud) != k_ra8_ok) {
    kint_demo_panic_halt();
  }
}

/**
 * @brief Configure SW1 as a KIN0-style falling-edge IRQ source.
 *
 * @par MC/DC:
 * Compound decision: ``icu_init != ok || sw_init != ok ||
 * configure_irq_pin != ok``. Three atomic conditions x N+1 = 4 vectors
 * -- exercised in test_app_kint_demo.c.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t kint_demo_arm(void)
{
  /* ICU bring-up is now performed in kint_demo_setup_or_halt so the
   * canonical CGC -> MSTP -> IOPORT -> TIME -> ICU -> peripheral order
   * is preserved (see audit_init_order). */
  ra8_err_t err = ra8_board_sw_init(k_ra8_board_sw1);
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_icu_irq_cfg_t cfg = {
    .sense      = k_ra8_icu_irqmd_falling,
    .filter_div = k_ra8_icu_fclksel_pclkb_64,
    .filter_en  = true,
  };
  return ra8_icu_configure_irq_pin((uint8_t)k_kint_demo_irq_channel, &cfg);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  kint_demo_setup_or_halt();
  ra8_isr_globals_enable();

  if (kint_demo_arm() != k_ra8_ok) {
    kint_demo_panic_halt();
  }

  /* HIL boot banner -- scraped by scripts/hil/run_direct.sh to confirm
   * the CGC + SCI + ICU bring-up reached the poll loop. */
  (void)ra8_board_uart_console_write(k_kint_demo_boot_msg,
                                     (size_t)(sizeof(k_kint_demo_boot_msg) - 1U));

  ra8_board_sw_state_t prev = k_ra8_board_sw_released;
  while (1) {
    ra8_board_sw_state_t now = k_ra8_board_sw_released;
    if (ra8_board_sw_read(k_ra8_board_sw1, &now) != k_ra8_ok) {
      break;
    }
    if ((prev == k_ra8_board_sw_released) && (now == k_ra8_board_sw_pressed)) {
      if (ra8_board_uart_console_write(k_kint_demo_log_msg,
                                       (size_t)(sizeof(k_kint_demo_log_msg) - 1U)) != k_ra8_ok) {
        break;
      }
      ra8_delay_ms((uint32_t)k_kint_demo_debounce_ms);
    }
    prev = now;
    ra8_delay_ms((uint32_t)k_kint_demo_poll_ms);
  }
  kint_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
