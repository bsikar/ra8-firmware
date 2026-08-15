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

#include "ra8_attributes.h"
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

/** @brief Fixed UART diagnostics for KINT demo boot and SW1 detection. */
static const uint8_t s_kint_demo_log_msg[]  = "kint: SW1 press\r\n";
static const uint8_t s_kint_demo_boot_msg[] = "kint_demo: boot\r\n";

/**
 * @brief Park the processor after an unrecoverable KINT demo failure.
 *
 * @details Preserves ICU, switch, LED, and console state in a permanent
 *          wait-for-interrupt loop for debugger inspection.
 *
 * @return None.
 *
 * @pre The caller has determined key-interrupt validation cannot continue.
 * @pre Any required UART diagnostic has completed.
 * @post The function never returns to its caller.
 * @post No further key flag polls or LED updates occur.
 *
 * @note Interrupt wakeups return immediately to the permanent loop.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_kint_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Initialize clocks, ICU, console, and the KINT status LED.
 *
 * @details Brings up the clock, module-stop, I/O, and time services in canonical
 *          order, then initializes the ICU, UART console, and LED1. Any failed
 *          dependency enters the permanent panic halt.
 *
 * @return None.
 *
 * @pre Reset-time initialization configured the core and C runtime.
 * @pre The board console, LED1, and SW1 are available to this image.
 * @post On success every dependency needed to arm the switch IRQ is ready.
 * @post On failure the function never returns to its caller.
 *
 * @note Single-shot boot helper; it is not reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_kint_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_kint_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_kint_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_kint_demo_panic_halt();
  }
  if (ra8_icu_init() != k_ra8_ok) {
    internal_kint_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_kint_demo_baud) != k_ra8_ok) {
    internal_kint_demo_panic_halt();
  }
}

/**
 * @brief Configure SW1 as a KIN0-style falling-edge IRQ source.
 *
 * @details Initializes the board switch, then configures the selected ICU
 *          channel for filtered falling-edge detection.
 *
 * @par MC/DC:
 * Compound decision: ``icu_init != ok || sw_init != ok ||
 * configure_irq_pin != ok``. Three atomic conditions x N+1 = 4 vectors
 * -- exercised in test_app_kint_demo.c.
 *
 * @return ra8_err_t Status from switch initialization or IRQ configuration.
 * @retval k_ra8_ok SW1 and its filtered falling-edge IRQ were configured.
 * @retval (other)  The first board or ICU error.
 *
 * @pre ::internal_kint_demo_setup_or_halt completed successfully.
 * @pre The selected ICU channel is not owned by another context.
 * @post On success SW1 falling edges set the selected detection flag.
 * @post On switch initialization failure IRQ configuration is not attempted.
 *
 * @note The application polls the detection flag rather than installing an ISR.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_kint_demo_arm(void)
{
  /* ICU bring-up is now performed in internal_kint_demo_setup_or_halt so the
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
  internal_kint_demo_setup_or_halt();
  ra8_isr_globals_enable();

  if (internal_kint_demo_arm() != k_ra8_ok) {
    internal_kint_demo_panic_halt();
  }

  /* HIL boot banner -- scraped by scripts/hil/run_direct.sh to confirm
   * the CGC + SCI + ICU bring-up reached the poll loop. */
  (void)ra8_board_uart_console_write(s_kint_demo_boot_msg,
                                     (size_t)(sizeof(s_kint_demo_boot_msg) - 1U));

  ra8_board_sw_state_t prev = k_ra8_board_sw_released;
  while (1) {
    ra8_board_sw_state_t now = k_ra8_board_sw_released;
    if (ra8_board_sw_read(k_ra8_board_sw1, &now) != k_ra8_ok) {
      break;
    }
    if ((prev == k_ra8_board_sw_released) && (now == k_ra8_board_sw_pressed)) {
      if (ra8_board_uart_console_write(s_kint_demo_log_msg,
                                       (size_t)(sizeof(s_kint_demo_log_msg) - 1U)) != k_ra8_ok) {
        break;
      }
      ra8_delay_ms((uint32_t)k_kint_demo_debounce_ms);
    }
    prev = now;
    ra8_delay_ms((uint32_t)k_kint_demo_poll_ms);
  }
  internal_kint_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
