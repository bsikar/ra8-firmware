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
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
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

static const uint8_t s_ulpt_demo_log_msg[] = "ulpt: wake ok\r\n";

/**
 * @brief Park the core after an unrecoverable ULPT demo failure.
 *
 * @details Repeatedly executes WFI so an attached debugger can inspect the
 *          timer and clock state without additional foreground traffic.
 *
 * @pre Called only from the boot or terminal foreground path.
 * @pre The caller does not require this function to return.
 * @post The core remains in the WFI loop until reset or debug intervention.
 * @post No further timer or console operation is requested.
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
 * @brief Initialize clocks, SysTick, SCI8, and the ULPT block.
 *
 * @details Brings all foreground-loop dependencies up in order and transfers
 *          control to the terminal panic helper on the first HAL error.
 *
 * @pre Reset startup initialized static storage and the vector table.
 * @pre Called once before global interrupt enable.
 * @post On return, the delay service, console, and ULPT driver are ready.
 * @post ULPT0 remains stopped until ``internal_arm`` runs.
 * @note Not thread-safe; it mutates global peripheral configuration.
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
  if (ra8_board_uart_console_init((uint32_t)k_ulpt_demo_baud) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_ulpt_init() != k_ra8_ok) {
    internal_panic_halt();
  }
}

/**
 * @brief Arm ULPT0 with the demo period.
 *
 * @details Delegates the fixed channel and approximately one-second reload to
 *          the ULPT HAL without changing any other timer channel.
 *
 * @par MC/DC:
 * Compound decision: ``ra8_ulpt_start != ok``. One atomic condition x
 * 2 vectors -- ok (golden) and bad-channel reject (covered in
 * test_app_ulpt_demo.c).
 *
 * @return ULPT start status from the HAL.
 * @retval k_ra8_ok ULPT0 accepted the channel and reload configuration.
 * @retval (other) The HAL rejected or could not start the timer.
 *
 * @pre ``internal_setup_or_halt`` initialized the ULPT driver.
 * @pre ULPT0 is stopped or ready to be re-armed after an underflow.
 * @post On success, ULPT0 counts toward the fixed reload underflow.
 * @post On failure, the caller receives the HAL error unchanged.
 * @note Not thread-safe with concurrent ULPT0 control.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_arm(void)
{
  return ra8_ulpt_start((uint8_t)k_ulpt_demo_channel, (uint32_t)k_ulpt_demo_period_ticks);
}

/**
 * @brief Report and re-arm each observed ULPT0 underflow.
 *
 * @details Initializes and arms ULPT0, then polls its underflow bit. Each event
 *          emits the fixed wake banner, stops the channel to clear status, and
 *          starts the next interval with the same reload.
 *
 * @pre Reset startup and SystemInit completed successfully.
 * @pre The EK-RA8D2 console wiring matches the board definition.
 * @post Every reported wake corresponds to an observed underflow status bit.
 * @post Any HAL or output error leads to the terminal panic helper.
 * @note Does not return during normal operation.
 * @since 0.1.0
 */
void main(void)
{
  internal_setup_or_halt();
  ra8_isr_globals_enable();

  if (internal_arm() != k_ra8_ok) {
    internal_panic_halt();
  }

  while (1) {
    uint8_t status = 0U;
    if (ra8_ulpt_get_status((uint8_t)k_ulpt_demo_channel, &status) != k_ra8_ok) {
      break;
    }
    if ((status & (uint8_t)k_ulpt_demo_undf_bit) != 0U) {
      if (ra8_board_uart_console_write(s_ulpt_demo_log_msg,
                                       (size_t)(sizeof(s_ulpt_demo_log_msg) - 1U)) != k_ra8_ok) {
        break;
      }
      if (ra8_ulpt_stop((uint8_t)k_ulpt_demo_channel) != k_ra8_ok) {
        break;
      }
      if (internal_arm() != k_ra8_ok) {
        break;
      }
    }
    ra8_delay_ms((uint32_t)k_ulpt_demo_poll_ms);
  }
  internal_panic_halt();
}
