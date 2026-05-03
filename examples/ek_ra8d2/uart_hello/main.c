/**
 * @file examples/ek_ra8d2/uart_hello/main.c
 * @brief UART "hello world" smoke test for EK-RA8D2 (SCI8 @ 115200)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up via ``ra_cgc_init()`` (XTAL -> PLL1 -> CPUCLK0 =
 * 1 GHz, PCLKA = 125 MHz, SCICLK = PLL1R / 4 = 100 MHz), configures
 * SCI8 TXD8 = PD_02 / RXD8 = PD_03 in async mode at 115200 8N1, and
 * prints ``"hello, ra8d2!\r\n"`` once a second while toggling LED1 as
 * a heartbeat. The CDC channel of the on-board J-Link OB on the
 * EK-RA8D2 surfaces these pins as a virtual serial port on the host;
 * connecting any terminal at 115200 8N1 to that port shows the stream.
 *
 * Sequence:
 *   1. ``ra_cgc_init()`` -- XTAL + PLL1 up, CPUCLK0 = 1 GHz, PCLKA =
 *      125 MHz, SCICLK = PLL1R / 4. The CGC driver also flushes the
 *      MRAM prefetch buffer, sets VSCR.VSCM = 1, programmes MRMS
 *      wait states, and routes SCICLK from PLL1R per HUM 9.2.54 --
 *      no per-app workarounds needed.
 *   2. ``ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz)`` --
 *      SCI_B's BRR is computed against PCLKA (HUM Ch 38 line 1
 *      explicitly says "In this section, PCLK refers to PCLKA").
 *   3. ``ra_pfs_route_peripheral()`` for PD_02 and PD_03 to put them
 *      in SCI async mode (PSEL = ``k_ra_psel_sci_async``).
 *   4. ``ra_sci_init(8, &cfg)`` -- 115200 8N1, no parity, one stop.
 *      At PCLKA = 125 MHz, BRR = 33 yields 114890 baud (0.27 % off,
 *      well within UART tolerance).
 *   5. ``ra_time_init(cpuclk0_hz)`` for the heartbeat delay.
 *   6. ``ra_gpio_output_init(k_ra_pin_led1, low)`` for the visual
 *      heartbeat.
 *   7. Loop: write the greeting, toggle LED1, sleep 1 s.
 *
 * Verification: open the J-Link OB CDC port at 115200 8N1, e.g.
 * ``picocom -b 115200 /dev/cu.usbmodem...`` on macOS or
 * ``minicom -D /dev/ttyACM0 -b 115200`` on Linux. You should see
 * one greeting line per second with LED1 toggling in lock-step.
 *
 * @par Architectural ring
 * [Ring 6 / APP] {World: S} -- application-layer code that runs in
 * the Secure world.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-29
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
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
  k_uart_hello_sci_channel = 8U,
} uart_hello_config_t;

/** @brief Pinout for the on-board J-Link OB CDC channel (SCI8 / PD02 + PD03). */
static const ra_port_pin_t k_uart_hello_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_uart_hello_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

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
 * @brief Route PD_02 / PD_03 to SCI8 TXD / RXD via the PFS PSEL field.
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
 * @post On success PD_02 and PD_03 are in SCI-async (PSEL=0x04) mode.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t uart_hello_pins_init(void)
{
  ra_err_t err =
    ra_pfs_route_peripheral(k_uart_hello_pin_txd, k_ra_psel_sci_async, "uart_hello.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_uart_hello_pin_rxd, k_ra_psel_sci_async, "uart_hello.rxd8");
}

/**
 * @brief Bring CGC + SysTick + SCI8 + LED1 up. Panic-halts on any fail.
 *
 * @details
 * PCLKA comes from ``ra_cgc_get_clock_hz`` so the BRR calculator
 * always sees the real, post-PLL rate (125 MHz) instead of a
 * hardcoded constant. That way the demo keeps working when the
 * CGC driver is retargeted to a different clock tree.
 *
 * @since 0.1.0
 */
static void uart_hello_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    uart_hello_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    uart_hello_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    uart_hello_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    uart_hello_panic_halt();
  }
  if (uart_hello_pins_init() != k_ra_ok) {
    uart_hello_panic_halt();
  }

  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_uart_hello_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_uart_hello_sci_channel, &sci_cfg) != k_ra_ok) {
    uart_hello_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    uart_hello_panic_halt();
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up CGC + SCI8 + LED1 then prints.
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
int32_t main(void)
{
  uart_hello_setup_or_halt();

  ra_isr_globals_enable();

  while (1) {
    if (ra_sci_write_polling((uint8_t)k_uart_hello_sci_channel,
                             k_uart_hello_greeting,
                             (uint32_t)(sizeof(k_uart_hello_greeting) - 1U)) != k_ra_ok) {
      break;
    }
    if (ra_board_led_toggle(k_ra_board_led1) != k_ra_ok) {
      break;
    }
    ra_delay_ms(k_uart_hello_period_ms);
  }

  uart_hello_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
