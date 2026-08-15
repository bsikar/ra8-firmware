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
  k_icu_extint_demo_baud    = 115200U, /**< Icu extint demo baud.    */
  k_icu_extint_demo_poll_ms = 20U,     /**< Icu extint demo poll ms. */
} icu_extint_demo_const_t;

/** @brief SW1 -> IRQ13-DS (EK-RA8D2 UM Table 25 p 32). */
typedef enum : uint8_t {
  k_icu_extint_demo_irq_num   = 13U,   /**< Icu extint demo IRQ number.      */
  k_icu_extint_demo_irqf_mask = 0x40U, /**< IRQCRi.IRQ_DETECT (HUM 14.2.12). */
} icu_extint_demo_irq_t;

/** @brief Fixed UART diagnostics for boot and an observed SW1 IRQ edge. */
static const uint8_t s_icu_extint_demo_msg_press[] = "icu: irq13 press\r\n";
static const uint8_t s_icu_extint_demo_msg_boot[]  = "icu_extint_demo: boot\r\n";

/**
 * @brief Park the processor after an unrecoverable ICU demo failure.
 *
 * @details Repeats wait-for-interrupt forever, preserving IRQ detection, LED,
 *          and console state for an attached debugger.
 *
 * @return None.
 *
 * @pre The caller has determined external-interrupt validation cannot continue.
 * @pre Any required UART diagnostic has already completed.
 * @post The function never returns to its caller.
 * @post No more IRQ flag polls or LED updates are performed.
 *
 * @note Interrupt wakeups return immediately to the permanent loop.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_icu_extint_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Initialize clocks, ICU, console, LED1, and the SW1 input.
 *
 * @details Starts the time base and ICU block, opens the UART console, and
 *          claims the board switch and indicator LED before IRQ13 is armed.
 *          Any failed dependency enters the permanent panic halt.
 *
 * @return None.
 *
 * @pre Reset-time initialization configured the core and C runtime.
 * @pre SW1, LED1, and the SCI8 console are available to this image.
 * @post On success every dependency required to configure IRQ13 is ready.
 * @post On failure the function never returns to its caller.
 *
 * @note Single-shot boot helper; it is not reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_icu_extint_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_icu_extint_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_icu_extint_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_icu_extint_demo_panic_halt();
  }
  if (ra8_icu_init() != k_ra8_ok) {
    internal_icu_extint_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_icu_extint_demo_baud) != k_ra8_ok) {
    internal_icu_extint_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_icu_extint_demo_panic_halt();
  }
  if (ra8_board_sw_init(k_ra8_board_sw1) != k_ra8_ok) {
    internal_icu_extint_demo_panic_halt();
  }
}

/**
 * @brief Configure SW1's IRQ for falling-edge detection w/ digital filter.
 *
 * @details Applies IRQ13 falling-edge sensing and enables the PCLKB/64 digital
 *          filter used to reject switch bounce before the poll loop reads the
 *          detection flag.
 *
 * @par MC/DC:
 * Decision: ``ra8_icu_configure_irq_pin != ok``. One atomic condition
 * x 2 vectors -- golden (this) + null cfg / out-of-range channel
 * (test_app_icu_extint_demo.c).
 *
 * @return ra8_err_t Status from configuring the board switch IRQ input.
 * @retval k_ra8_ok IRQ13 accepted the falling-edge and filter configuration.
 * @retval (other)  The ICU driver rejected or could not apply the configuration.
 *
 * @pre ::internal_icu_extint_demo_setup_or_halt completed successfully.
 * @pre IRQ13 is not configured by another owner.
 * @post On success falling edges on SW1 set the filtered IRQ13 detection flag.
 * @post On failure no valid IRQ13 configuration is promised.
 *
 * @note The main loop polls the detection flag; this image installs no NVIC handler.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_icu_extint_demo_arm(void)
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
  internal_icu_extint_demo_setup_or_halt();
  ra8_isr_globals_enable();

  if (internal_icu_extint_demo_arm() != k_ra8_ok) {
    internal_icu_extint_demo_panic_halt();
  }

  /* HIL boot banner -- scraped by scripts/hil/run_direct.sh to confirm
   * the CGC + SCI + ICU bring-up reached the main poll loop. */
  (void)ra8_board_uart_console_write(s_icu_extint_demo_msg_boot,
                                     (size_t)(sizeof(s_icu_extint_demo_msg_boot) - 1U));

  while (1) {
    uint8_t irqcr = 0U;
    if (ra8_icu_read_irqcr((uint8_t)k_icu_extint_demo_irq_num, &irqcr) != k_ra8_ok) {
      break;
    }
    if ((irqcr & (uint8_t)k_icu_extint_demo_irqf_mask) != 0U) {
      if (ra8_board_led_toggle(k_ra8_board_led1) != k_ra8_ok) {
        break;
      }
      if (ra8_board_uart_console_write(s_icu_extint_demo_msg_press,
                                       (size_t)(sizeof(s_icu_extint_demo_msg_press) - 1U)) !=
          k_ra8_ok) {
        break;
      }
    }
    ra8_delay_ms((uint32_t)k_icu_extint_demo_poll_ms);
  }
  internal_icu_extint_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
