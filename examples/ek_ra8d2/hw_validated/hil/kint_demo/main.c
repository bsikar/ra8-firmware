/**
 * @file examples/ek_ra8d2/kint_demo/main.c
 * @brief Key-interrupt (KINT / IRQ pin) input demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up the on-board user switch SW1 (P009 -> IRQ13-DS, EK-RA8D2
 * UM Table 25 p 32) as a falling-edge external interrupt source via
 * ``ra_icu_configure_irq_pin``. Each press logs a one-line message
 * over the J-Link OB CDC console (SCI8). The implementation polls the
 * ICU status flag rather than wiring the NVIC -- the goal is to
 * exercise the IRQCRb register programming path on a stock board with
 * no expansion modules required.
 *
 * Sequence:
 *   1. CGC + SysTick + UART (SCI8) bring-up.
 *   2. ``ra_icu_init()``.
 *   3. ``ra_board_sw_init(SW1)``.
 *   4. ``ra_icu_configure_irq_pin(13, falling-edge + filter)``.
 *   5. Loop: poll ``ra_board_sw_read``; log on press, debounce, repeat.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_icu.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_kint_demo_baud        = 115200U,
  k_kint_demo_sci_channel = 8U,
  k_kint_demo_poll_ms     = 20U,
  k_kint_demo_debounce_ms = 50U,
} kint_demo_const_t;

/** @brief KIN0 / IRQ channel selection (SW1 -> IRQ13-DS). */
typedef enum : uint8_t {
  k_kint_demo_irq_channel = 13U, /**< k_ra_board_sw1_irq. */
} kint_demo_irq_t;

/** @brief SCI8 pin map -- same as uart_hello / rtc_alarm. */
static const ra_port_pin_t k_kint_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_kint_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

static const uint8_t k_kint_demo_log_msg[]  = "kint: SW1 press\r\n";
static const uint8_t k_kint_demo_boot_msg[] = "kint_demo: boot\r\n";

static void kint_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

[[nodiscard]] static ra_err_t kint_demo_pins_init(void)
{
  ra_err_t err =
    ra_pfs_route_peripheral(k_kint_demo_pin_txd, k_ra_psel_sci_async, "kint_demo.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_kint_demo_pin_rxd, k_ra_psel_sci_async, "kint_demo.rxd8");
}

static void kint_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    kint_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    kint_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    kint_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    kint_demo_panic_halt();
  }
  if (ra_icu_init() != k_ra_ok) {
    kint_demo_panic_halt();
  }
  if (kint_demo_pins_init() != k_ra_ok) {
    kint_demo_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_kint_demo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_kint_demo_sci_channel, &sci_cfg) != k_ra_ok) {
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
[[nodiscard]] static ra_err_t kint_demo_arm(void)
{
  /* ICU bring-up is now performed in kint_demo_setup_or_halt so the
   * canonical CGC -> MSTP -> IOPORT -> TIME -> ICU -> peripheral order
   * is preserved (see audit_init_order). */
  ra_err_t err = ra_board_sw_init(k_ra_board_sw1);
  if (err != k_ra_ok) {
    return err;
  }
  const ra_icu_irq_cfg_t cfg = {
    .sense      = k_ra_icu_irqmd_falling,
    .filter_div = k_ra_icu_fclksel_pclkb_64,
    .filter_en  = true,
  };
  return ra_icu_configure_irq_pin((uint8_t)k_kint_demo_irq_channel, &cfg);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  kint_demo_setup_or_halt();
  ra_isr_globals_enable();

  if (kint_demo_arm() != k_ra_ok) {
    kint_demo_panic_halt();
  }

  /* HIL boot banner -- scraped by scripts/hil_run_direct.sh to confirm
   * the CGC + SCI + ICU bring-up reached the poll loop. */
  (void)ra_sci_write_polling((uint8_t)k_kint_demo_sci_channel,
                             k_kint_demo_boot_msg,
                             (uint32_t)(sizeof(k_kint_demo_boot_msg) - 1U));

  ra_board_sw_state_t prev = k_ra_board_sw_released;
  while (1) {
    ra_board_sw_state_t now = k_ra_board_sw_released;
    if (ra_board_sw_read(k_ra_board_sw1, &now) != k_ra_ok) {
      break;
    }
    if ((prev == k_ra_board_sw_released) && (now == k_ra_board_sw_pressed)) {
      if (ra_sci_write_polling((uint8_t)k_kint_demo_sci_channel,
                               k_kint_demo_log_msg,
                               (uint32_t)(sizeof(k_kint_demo_log_msg) - 1U)) != k_ra_ok) {
        break;
      }
      ra_delay_ms((uint32_t)k_kint_demo_debounce_ms);
    }
    prev = now;
    ra_delay_ms((uint32_t)k_kint_demo_poll_ms);
  }
  kint_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
