/**
 * @file examples/ek_ra8d2/hw_validated/hil/uart_hello/main.c
 * @brief UART "hello world" HIL test for EK-RA8D2 (SCI8 @ 115200)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings the chip up via ``ra8_cgc_init()`` (XTAL -> PLL1 -> CPUCLK0 =
 * 1 GHz, PCLKA = 125 MHz, SCICLK = PLL1R / 4 = 100 MHz), brings up the
 * SCI8 J-Link OB console (TXD8 = PD_02 / RXD8 = PD_03 in async mode at
 * 115200 8N1) through the board-support-package console API, and prints
 * ``"hello, ra8d2!\r\n"`` once a second while toggling LED1 as a
 * heartbeat. The CDC channel of the on-board J-Link OB on the
 * EK-RA8D2 surfaces these pins as a virtual serial port on the host;
 * connecting any terminal at 115200 8N1 to that port shows the stream.
 *
 * Sequence:
 *   1. ``ra8_cgc_init()`` -- XTAL + PLL1 up, CPUCLK0 = 1 GHz, PCLKA =
 *      125 MHz, SCICLK = PLL1R / 4. The CGC driver also flushes the
 *      MRAM prefetch buffer, sets VSCR.VSCM = 1, programmes MRMS
 *      wait states, and routes SCICLK from PLL1R per HUM 9.2.54 --
 *      no per-app workarounds needed.
 *   2. ``ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz)`` --
 *      the heartbeat delay is calibrated against CPUCLK0.
 *   3. ``ra8_time_init(cpuclk0_hz)`` for the heartbeat delay.
 *   4. ``ra8_board_uart_console_init(115200)`` -- the BSP routes PD_02
 *      / PD_03 to SCI8 async (PSEL) and brings up SCI8 at 115200 8N1,
 *      computing BRR against PCLKA internally.
 *   5. ``ra8_board_led_init(k_ra8_board_led1)`` for the visual heartbeat.
 *   6. Loop: write the greeting, toggle LED1, sleep 1 s.
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

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/** @brief Compile-time settings for the demo. */
typedef enum : uint32_t {
  k_uart_hello_baud      = 115200U, /**< UART hello baud.      */
  k_uart_hello_period_ms = 1000U,   /**< UART hello period ms. */
} uart_hello_config_t;

/** @brief Greeting string sent every period. Must remain ASCII. */
static const uint8_t s_uart_hello_greeting[] = "hello, ra8d2!\r\n";

/**
 * @brief Halt forever in WFI -- used as a panic stop on init failure.
 *
 * @details Preserves the failed boot state in a low-activity loop that remains
 *          accessible to an attached debugger.
 *
 * @return None.
 *
 * @pre Called only after a fatal error in boot.
 * @pre The caller has no remaining recovery or diagnostic write to perform.
 *
 * @post CPU is parked; only a debugger or external reset wakes it.
 * @post The demo emits no further UART bytes or LED transitions.
 *
 * @note Interrupt wakeups return immediately to the permanent loop.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_uart_hello_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + SysTick + SCI8 + LED1 up. Panic-halts on any fail.
 *
 * @details
 * The SCI8 J-Link OB console (TXD8 = PD_02 / RXD8 = PD_03 @ 115200 8N1)
 * is brought up through the board-support-package console API, which
 * owns the PFS pin routing, the BRR calculation against PCLKA, and the
 * SCI8 init. That keeps the demo correct when the CGC driver is
 * retargeted to a different clock tree.
 *
 * @return None.
 *
 * @pre Reset-time initialization configured the core and vector table.
 * @pre The EK-RA8D2 SCI8 console pins and LED1 are available to this app.
 * @post On success the millisecond time base and 115200-baud console are ready.
 * @post On success LED1 is initialized; on failure the function never returns.
 *
 * @note Single-shot boot helper; it is not reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_uart_hello_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    internal_uart_hello_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_uart_hello_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_uart_hello_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_uart_hello_baud) != k_ra8_ok) {
    internal_uart_hello_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_uart_hello_panic_halt();
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
  internal_uart_hello_setup_or_halt();

  ra8_isr_globals_enable();

  while (1) {
    (void)ra8_board_uart_console_write(s_uart_hello_greeting,
                                       (size_t)(sizeof(s_uart_hello_greeting) - 1U));
    if (ra8_board_led_toggle(k_ra8_board_led1) != k_ra8_ok) {
      break;
    }
    ra8_delay_ms(k_uart_hello_period_ms);
  }

  internal_uart_hello_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
