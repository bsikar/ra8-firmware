/**
 * @file main.c
 * @brief UART "hello world" smoke test for EK-RA8D2 (SCI0 @ 115200)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up on HOCO + PLL via ``ra_cgc_init()``, configures
 * SCI0 TXD0=P1_01 / RXD0=P1_02 in async mode at 115200 8N1, and
 * prints ``"hello, ra8d2!\r\n"`` once a second while toggling LED1
 * as a heartbeat. The CDC channel of the on-board J-Link OB on the
 * EK-RA8D2 surfaces SCI0 as a virtual serial port on the host, so
 * connecting a terminal at 115200 8N1 to that port should show the
 * stream.
 *
 * Sequence:
 *   1. ``ra_cgc_init()`` -- HOCO + PLL up, CPUCLK0 / PCLKB at their
 *      rated rates. Required for an accurate baud-rate divisor.
 *   2. ``ra_cgc_get_clock_hz(k_ra_clock_id_pclkb, &pclkb_hz)`` --
 *      the SCI BRR is computed against PCLKB.
 *   3. ``ra_pfs_route_peripheral()`` for P1_01 and P1_02 to put
 *      them in SCI async mode (PSEL = ``k_ra_psel_sci_async``).
 *   4. ``ra_sci_init(0, &cfg)`` -- 115200 8N1, no parity, one stop.
 *   5. ``ra_time_init(cpuclk0_hz)`` for the heartbeat delay.
 *   6. ``ra_gpio_output_init(k_ra_pin_led1, low)`` for the visual
 *      heartbeat.
 *   7. Loop: write the greeting, toggle LED1, sleep 1 s.
 *
 * Verification: open the J-Link OB CDC port at 115200 8N1 (e.g.
 * ``screen /dev/cu.usbmodem<...> 115200`` on macOS or
 * ``minicom -D /dev/ttyACM0 -b 115200`` on Linux). You should see
 * one greeting line per second and LED1 toggling in lock-step.
 *
 * If you see garbled bytes, the baud divisor is wrong -- almost
 * always because the CGC didn't bring PCLKB up where you expected.
 *
 * @par Architectural ring
 * [Ring 6 / APP] {World: S} -- application-layer code that runs in
 * the Secure world.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-28
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @brief Compile-time settings for the demo. */
typedef enum : uint32_t {
  k_uart_hello_baud        = 115200U,
  k_uart_hello_period_ms   = 1000U,
  k_uart_hello_sci_channel = 0U,
} uart_hello_config_t;

/** @brief Pinout for the on-board J-Link OB CDC channel (SCI0). */
static const ra_port_pin_t k_uart_hello_pin_txd0 =
  (ra_port_pin_t)(((uint16_t)k_ra_port_1 << 8) | (uint16_t)k_ra_pin_1);
static const ra_port_pin_t k_uart_hello_pin_rxd0 =
  (ra_port_pin_t)(((uint16_t)k_ra_port_1 << 8) | (uint16_t)k_ra_pin_2);

/** @brief Greeting string sent every period. Must remain ASCII. */
static const uint8_t k_uart_hello_greeting[] = "hello, ra8d2!\r\n";

/**
 * @brief Halt forever in WFI -- used as a panic stop on init failure.
 *
 * @pre Called only after a fatal error in boot.
 *
 * @post CPU is parked; only a debugger or external reset wakes it.
 *
 * @since 0.1.0
 */
static void uart_hello_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Route P1_01 / P1_02 to SCI0 TXD / RXD via the PFS PSEL field.
 *
 * @return Error code from the first failing route call, or k_ra_ok.
 *
 * @retval k_ra_ok                       Both pins are SCI-routed.
 * @retval k_ra_err_gpio_invalid_port    PFS port index out of range.
 * @retval k_ra_err_gpio_invalid_pin     PFS pin index out of range.
 * @retval k_ra_err_gpio_conflict        Pin already claimed by another owner.
 *
 * @pre IOPORT module is reachable.
 * @pre Caller is single-threaded init context.
 *
 * @post On success P1_01 and P1_02 are in SCI-async (PSEL=0x04) mode.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t uart_hello_pins_init(void)
{
  ra_err_t err =
    ra_pfs_route_peripheral(k_uart_hello_pin_txd0, k_ra_psel_sci_async, "uart_hello.txd0");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_uart_hello_pin_rxd0, k_ra_psel_sci_async, "uart_hello.rxd0");
}

/**
 * @brief Application entry. Brings up CGC + SCI0 + LED1 then prints.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On clean entry the CPU stays in the print + blink loop forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @since 0.1.0
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  if (ra_cgc_init() != k_ra_ok) {
    uart_hello_panic_halt();
  }

  uint32_t cpuclk0_hz = 0U;
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    uart_hello_panic_halt();
  }

  uint32_t pclkb_hz = 0U;
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclkb, &pclkb_hz) != k_ra_ok) {
    uart_hello_panic_halt();
  }

  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    uart_hello_panic_halt();
  }

  if (uart_hello_pins_init() != k_ra_ok) {
    uart_hello_panic_halt();
  }

  const ra_sci_cfg_t sci_cfg = {
    .baud      = (uint32_t)k_uart_hello_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclkb_hz,
  };
  if (ra_sci_init((uint8_t)k_uart_hello_sci_channel, &sci_cfg) != k_ra_ok) {
    uart_hello_panic_halt();
  }

  if (ra_gpio_output_init(k_ra_pin_led1, k_ra_level_low) != k_ra_ok) {
    uart_hello_panic_halt();
  }

  ra_isr_globals_enable();

  while (1) {
    if (ra_sci_write_polling((uint8_t)k_uart_hello_sci_channel,
                             k_uart_hello_greeting,
                             (uint32_t)(sizeof(k_uart_hello_greeting) - 1U)) != k_ra_ok) {
      break;
    }
    if (ra_gpio_toggle(k_ra_pin_led1) != k_ra_ok) {
      break;
    }
    ra_delay_ms((uint32_t)k_uart_hello_period_ms);
  }

  uart_hello_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
