/**
 * @file examples/ek_ra8d2/hw_validated/hil/i2c_loopback/main.c
 * @brief RIIC (ra8_i2c) controller self-test against the on-board U15
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Standalone EVM-tier app that exercises the ra8_i2c (RIIC) controller
 * driver (``libs/ra8_hal/inc/ra8_i2c.h``) against the on-board PI4IOE5V6408
 * I2C I/O port expander (U15) at 7-bit address 0x43 -- the only I2C
 * peripheral guaranteed to be populated on a bare EK-RA8D2 v1 (board UM
 * section 4.3.4 "Switch Configuration", p 24). U15 sits on RIIC channel 1
 * (P512 SCL1 / P511 SDA1), per issue #46. The flow:
 *
 *   1. ``ra8_cgc_init`` -- bring CPUCLK0 up.
 *   2. ``ra8_board_uart_console_init`` -- BSP SCI8 console (PD02 TXD /
 *      PD03 RXD) bring-up.
 *   3. ``ra8_board_io_expander_apply_project_sw4_defaults()`` -- the
 *      board's validated U15 bring-up: bus-recover, P109/P311 pull-ups,
 *      P512/P511 SCL1/SDA1 route + NCODR, ra8_i2c_init(ch1) and a U15
 *      write. A k_ra8_ok return means U15 ACKed.
 *   4. ``ra8_i2c_scan`` against ``0x43`` in a loop -- the U15 expander
 *      ACKs every address-only probe, proving the bus and controller.
 *   5. LED1 toggles each scan; SCI8 prints ``"i2c: scan 0x43 ack=1\r\n"``
 *      once a second so a host terminal sees the heartbeat. LED2 latches
 *      ON if the driver returns a hard error or U15 fails to ACK.
 *
 * Hardware: bare EK-RA8D2 v1 only -- U15 is on-board, no jumpers needed.
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
#include "ra8_i2c.h"
#include "ra8_isr.h"
#include "ra8_mpc.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/** @brief App-wide tunables. */
typedef enum : uint32_t {
  k_i2c_demo_baud        = 115200U, /**< I2C demo baud.                                         */
  k_i2c_demo_period_ms   = 1000U,   /**< I2C demo period ms.                                    */
  k_i2c_demo_bus_hz      = 100000U, /**< I2C demo bus Hz.                                       */
  k_i2c_demo_iic_channel = 1U,      /**< RIIC ch1 (P512 SCL1 / P511 SDA1) -- U15 here, per #46. */
} i2c_demo_const_t;

/** @brief Probe target -- on-board PI4IOE5V6408 I/O port expander U15
 *         at 7-bit address 0x43 (board UM section 4.3.4, p 24). This
 *         device is always populated on a bare EK-RA8D2 v1 and ACKs
 *         every address-only probe, so a successful scan proves both
 *         the bus and the controller are alive. */
typedef enum : uint8_t {
  k_i2c_demo_probe_addr = 0x43U, /**< I2C demo probe address. */
} i2c_demo_byte_t;

/* SCI8 console (PD02 TXD / PD03 RXD) pin routing + baud are owned by the
 * BSP via ra8_board_uart_console_init(). RIIC ch1 SCL1/SDA1 (P512/P511)
 * routing + the P109/P311 pull-ups and NCODR are owned by
 * ra8_board_io_expander_apply_project_sw4_defaults(), the same validated
 * bring-up the board library uses for U15. */

static const uint8_t s_i2c_demo_msg_ack[]  = "i2c: scan 0x43 ack=1\r\n";
static const uint8_t s_i2c_demo_msg_nack[] = "i2c: scan 0x43 ack=0\r\n";
static const uint8_t s_i2c_demo_msg_err[]  = "i2c: scan ERROR\r\n";

/**
 * @brief Park forever after a fatal initialization failure.
 * @details Repeatedly executes WFI so a debugger can inspect the failed bus or
 *          clock state without additional I2C traffic.
 *
 * @pre Called only after a fatal error in boot.
 * @pre The caller does not require recovery without reset.
 * @post CPU is parked; only a debugger or external reset wakes it.
 * @post No further I2C or console operation is requested.
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
 * @brief Bring CGC + SysTick + MSTP up.
 * @details Initializes the canonical clock tree, samples CPUCLK0, ungates
 *          required peripheral modules, and starts the millisecond timebase.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Reset startup initialized static storage and the vector table.
 * @post On success CGC is live and SysTick is ticking.
 * @post Any failed dependency transfers to ``internal_panic_halt``.
 * @note Not thread-safe; it mutates global clock and module-stop state.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_clocks_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_panic_halt();
  }
}

/**
 * @brief Bring CGC + SysTick + console + IIC_B up. Panic-halts on any fail.
 *
 * @details
 * The BSP ``ra8_board_uart_console_init`` owns the SCI8 console (PD02 TXD /
 * PD03 RXD pin routing + baud), so the app no longer hand-rolls the SCI
 * bring-up.
 *
 * @pre ``internal_clocks_or_halt`` may safely own global clock setup.
 * @pre U15 is populated at its fixed EK-RA8D2 address.
 * @post On return, SCI8, RIIC1/U15, and both status LEDs are ready.
 * @post Any initialization failure has parked the core.
 * @note Not thread-safe; it owns the demo's peripheral setup.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_setup_or_halt(void)
{
  internal_clocks_or_halt();

  if (ra8_board_uart_console_init((uint32_t)k_i2c_demo_baud) != k_ra8_ok) {
    internal_panic_halt();
  }
  /* Bring up RIIC ch1 + confirm U15 via the board's validated path
   * (bus-recover + P109/P311 pull-ups + P512/P511 route + ra8_i2c_init +
   * a U15 write). k_ra8_ok means U15 ACKed the project SW4 byte; the bus
   * is then live for the ra8_i2c_scan loop below. */
  if (ra8_board_io_expander_apply_project_sw4_defaults() != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    internal_panic_halt();
  }
}

/**
 * @brief Application entry. Brings up CGC + IIC_B + console then probes.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On clean entry the CPU stays in the scan + blink loop forever.
 * @post On any HAL hard error LED2 latches ON and the loop exits.
 *
 * @since 0.1.0
 */
void main(void)
{
  internal_setup_or_halt();
  ra8_isr_globals_enable();

  while (1) {
    bool            acked = false;
    const ra8_err_t err =
      ra8_i2c_scan((uint8_t)k_i2c_demo_iic_channel, (uint8_t)k_i2c_demo_probe_addr, &acked);
    const uint8_t* msg     = s_i2c_demo_msg_err;
    uint32_t       msg_len = (uint32_t)(sizeof(s_i2c_demo_msg_err) - 1U);
    if (err == k_ra8_ok) {
      if (acked) {
        msg     = s_i2c_demo_msg_ack;
        msg_len = (uint32_t)(sizeof(s_i2c_demo_msg_ack) - 1U);
      } else {
        msg     = s_i2c_demo_msg_nack;
        msg_len = (uint32_t)(sizeof(s_i2c_demo_msg_nack) - 1U);
      }
    } else {
      (void)ra8_board_led_on(k_ra8_board_led2);
    }
    if (ra8_board_uart_console_write(msg, (size_t)msg_len) != k_ra8_ok) {
      break;
    }
    if (ra8_board_led_toggle(k_ra8_board_led1) != k_ra8_ok) {
      break;
    }
    ra8_delay_ms(k_i2c_demo_period_ms);
  }

  internal_panic_halt();
}
