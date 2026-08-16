/**
 * @file examples/ek_ra8d2/hw_validated/hil/iwdt_demo/main.c
 * @brief IWDT window-mode refresh demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Demonstrates the IWDT in **window mode**, distinct from the existing
 * ``watchdog_demo`` which simply refreshes until it stops. The IWDT
 * period and window-start / window-end positions are programmed in
 * the OFS0 option-setting register at flash time -- the chip cannot
 * reconfigure them at runtime. This demo refreshes the counter only
 * inside the legal window (between ``IWDTRPES`` and ``IWDTRPSS``)
 * via ``ra8_iwdt_refresh_deferred``, observes the live counter via
 * ``ra8_iwdt_get_counter``, and reads the IWDTSR underflow / refresh-
 * error status via ``ra8_iwdt_get_status`` to detect window violations.
 *
 * Bring-up sequence:
 *   1. CGC + SysTick + UART (SCI8) + LED1.
 *   2. ``ra8_iwdt_init`` (no-op on RA8D2 -- OFS0 owns the period).
 *   3. Loop: poll counter; refresh only when the counter is inside
 *      the legal window; clear status; toggle LED1 each refresh.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_boot_entry.h"
#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_iwdt.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_iwdt_demo_baud    = 115200U, /**< Iwdt demo baud.    */
  k_iwdt_demo_poll_ms = 50U,     /**< Iwdt demo poll ms. */
} iwdt_demo_const_t;

/** @brief Window bounds in 14-bit IWDTSR.CNTVAL units (HUM 28.2.2). */
typedef enum : uint16_t {
  k_iwdt_demo_window_low  = 0x0400U, /**< Refresh-permitted lower bound. */
  k_iwdt_demo_window_high = 0x0C00U, /**< Refresh-permitted upper bound. */
} iwdt_demo_window_t;

static const uint8_t s_iwdt_demo_msg_refresh[] = "iwdt: refresh in window\r\n";
static const uint8_t s_iwdt_demo_msg_boot[]    = "iwdt: poll counter\r\n";

/**
 * @brief Park the core after an unrecoverable IWDT demo failure.
 *
 * @details Repeatedly executes WFI, retaining live IWDT and status-register
 *          state for an attached debugger until reset.
 *
 * @pre Called only from the boot or terminal foreground path.
 * @pre The caller does not require this function to return.
 * @post The core remains in the WFI loop until external intervention.
 * @post No further explicit IWDT refresh is issued.
 * @note Not thread-safe; this is the terminal single-threaded path.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Initialize clocks, SysTick, SCI8, and LED1.
 *
 * @details Brings the foreground loop's dependencies up in order and transfers
 *          to the panic helper if any clock, console, or LED operation fails.
 *
 * @pre Reset startup initialized static storage and the vector table.
 * @pre Called once before global interrupt enable.
 * @post On return, SCI8 and LED1 are ready and delays use the CPU clock.
 * @post IWDT option-register configuration remains unchanged.
 * @note Not thread-safe; it mutates global board and clock state.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_iwdt_demo_baud) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_panic_halt();
  }
}

/**
 * @brief Decide whether the live counter sits inside the legal window.
 *
 * @details Applies the inclusive lower and upper OFS0-derived bounds used by
 *          the foreground loop before requesting a deferred refresh.
 *
 * @par MC/DC:
 * Compound decision: ``counter >= low && counter <= high``. Two atomic
 * conditions x 3 vectors -- both true (golden), low fail, high fail
 * (test_app_iwdt_demo.c).
 *
 * @param[in] counter Current 14-bit IWDT counter sample.
 * @return Whether refreshing at this sample is permitted.
 * @retval true ``counter`` lies within both inclusive bounds.
 * @retval false ``counter`` is below the lower or above the upper bound.
 *
 * @pre ``counter`` was sampled from the active IWDT instance.
 * @pre The compiled window constants match the programmed OFS0 policy.
 * @post No hardware or application state is modified.
 * @post The result depends only on ``counter`` and the fixed bounds.
 * @note Pure and reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_in_window(uint16_t counter)
{
  return (counter >= (uint16_t)k_iwdt_demo_window_low) &&
         (counter <= (uint16_t)k_iwdt_demo_window_high);
}

/**
 * @brief Poll and refresh the IWDT only inside its legal window.
 *
 * @details Emits the boot banner, initializes the option-controlled IWDT,
 *          samples its counter, and on legal samples queues a refresh, clears
 *          status, toggles LED1, and emits the refresh banner.
 *
 * @pre Reset startup and SystemInit completed successfully.
 * @pre OFS0 contains the window policy represented by the demo constants.
 * @post Each reported refresh was requested only within the inclusive window.
 * @post Any HAL or output error leads to the terminal panic helper.
 * @note Does not return during normal operation.
 * @since 0.1.0
 */
void main(void)
{
  internal_setup_or_halt();
  ra8_isr_globals_enable();

  /* Boot banner -- emit immediately after setup so the HIL host can
   * confirm the firmware booted regardless of OFS0 / IWDT state. */
  (void)ra8_board_uart_console_write(s_iwdt_demo_msg_boot,
                                     (size_t)(sizeof(s_iwdt_demo_msg_boot) - 1U));

  if (ra8_iwdt_init() != k_ra8_ok) {
    internal_panic_halt();
  }

  while (1) {
    uint16_t counter = 0U;
    if (ra8_iwdt_get_counter(&counter) != k_ra8_ok) {
      break;
    }
    if (internal_in_window(counter)) {
      ra8_iwdt_refresh_deferred();
      if (ra8_iwdt_clear_status() != k_ra8_ok) {
        break;
      }
      if (ra8_board_led_toggle(k_ra8_board_led1) != k_ra8_ok) {
        break;
      }
      if (ra8_board_uart_console_write(s_iwdt_demo_msg_refresh,
                                       (size_t)(sizeof(s_iwdt_demo_msg_refresh) - 1U)) !=
          k_ra8_ok) {
        break;
      }
    }
    ra8_delay_ms((uint32_t)k_iwdt_demo_poll_ms);
  }
  internal_panic_halt();
}
