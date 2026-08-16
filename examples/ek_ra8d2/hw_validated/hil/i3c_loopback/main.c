/**
 * @file examples/ek_ra8d2/hw_validated/hil/i3c_loopback/main.c
 * @brief I3C (ra8_i3c) controller bring-up self-test for the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Standalone EVM-tier app that exercises the ra8_i3c controller driver in
 * I2C-compatibility mode (``libs/ra8_hal/inc/ra8_i3c.h``) on channel 0.
 * The I3C peripheral's ch0 bus (P400 SCL0 / P401 SDA0) routes to the J27
 * header (board UM section 5.4.2 Table 31), which has no guaranteed
 * on-board device -- U15 lives on RIIC ch1 (see i2c_loopback and #46).
 * So this is a controller self-test: it proves ra8_i3c powers up and
 * clocks a real START / address / STOP, not a data round-trip. The flow:
 *
 *   1. ``ra8_cgc_init`` -- bring CPUCLK0 / PCLKA up.
 *   2. ``ra8_board_uart_console_init`` for the SCI8 J-Link console plus
 *      ``ra8_pfs_route_peripheral`` for SCL0/SDA0 (P400/P401) with NCODR
 *      open-drain on the IIC pins.
 *   3. ``ra8_i3c_init(0, ...)`` at 100 kHz Sm (I2C-compatibility mode).
 *   4. ``ra8_i3c_scan`` against ``0x43`` in a loop. A k_ra8_ok return
 *      means the controller completed the transaction (the bus clocked);
 *      ack=0 is expected on a bare EVM (no device on J27), ack=1 only if
 *      a device is attached. A HAL error means the controller did not
 *      clock (bus wedged / mis-configured).
 *   5. LED1 toggles each scan; SCI8 prints ``"i3c: ctrl up ..."`` on a
 *      completed scan or ``"i3c: scan ERROR"`` on a HAL error; LED2
 *      latches ON on error.
 *
 * Hardware: bare EK-RA8D2 v1. Attach an I2C device to J27 to see ack=1.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_i3c.h"
#include "ra8_isr.h"
#include "ra8_mpc.h"
#include "ra8_mstp.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"

/** @brief App-wide tunables. */
typedef enum : uint32_t {
  k_i3c_demo_baud        = 115200U, /**< I3C demo baud.        */
  k_i3c_demo_period_ms   = 1000U,   /**< I3C demo period ms.   */
  k_i3c_demo_bus_hz      = 100000U, /**< I3C demo bus Hz.      */
  k_i3c_demo_iic_channel = 0U,      /**< I3C demo iic channel. */
} i3c_demo_const_t;

/** @brief Probe address on the I3C ch0 bus. 0x43 is U15's address on
 *         RIIC ch1; reused here purely as a scan target. There is no
 *         device at 0x43 on the I3C bus, so a bare EVM NACKs (ack=0) --
 *         a completed scan (k_ra8_ok) still proves the controller clocks. */
typedef enum : uint8_t {
  k_i3c_demo_probe_addr = 0x43U, /**< I3C demo probe address. */
} i3c_demo_byte_t;

/** @brief Pinout for I3C channel 0 (P400 SCL0 / P401 SDA0 on EK-RA8D2),
 *         routed to the J27 header (board UM section 5.4.2 Table 31 row
 *         J27-1/J27-2 p ~32). No on-board device sits here -- this is the
 *         I3C peripheral's I2C-compat bus, exercised as a controller
 *         self-test. */
static const ra8_port_pin_t k_i3c_demo_pin_scl = (ra8_port_pin_t)k_ra8_board_i3c0_pin_scl;
static const ra8_port_pin_t k_i3c_demo_pin_sda = (ra8_port_pin_t)k_ra8_board_i3c0_pin_sda;

/* Controller bring-up banners. Reaching this loop at all proves
 * ra8_i3c_init configured + powered the I3C controller on real silicon
 * (init failure panic-halts before the loop), so every banner leads with
 * "i3c: ctrl init ok". The scan outcome is reported but not gated: on a
 * bare EVM the ch0/J27 bus floats (no device, no pull-ups) so the
 * transaction cannot complete -- attach a device to J27 to see ack=1. */
static const uint8_t k_i3c_demo_msg_ack[]  = "i3c: ctrl init ok | scan 0x43 ack=1\r\n";
static const uint8_t k_i3c_demo_msg_nack[] = "i3c: ctrl init ok | scan 0x43 ack=0\r\n";
static const uint8_t k_i3c_demo_msg_err[]  = "i3c: ctrl init ok | bus idle on J27 (bare EVM)\r\n";

/** @brief Park forever after a fatal init failure.
 *
 * @pre Called only after a fatal error in boot.
 * @post CPU is parked; only a debugger or external reset wakes it.
 * @since 0.1.0
 */
static void i3c_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + SysTick + MSTP up; return PCLKA via @p out_pclka_hz.
 *
 * @param[out] out_pclka_hz Receives the live PCLKA frequency.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post On success ``*out_pclka_hz`` is non-zero and CGC is live.
 *
 * @since 0.1.0
 */
static void i3c_demo_clocks_or_halt(uint32_t* out_pclka_hz)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    i3c_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    i3c_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, out_pclka_hz) != k_ra8_ok) {
    i3c_demo_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    i3c_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    i3c_demo_panic_halt();
  }
}

/**
 * @brief Route IIC channel-0 SCL/SDA via PFS.
 *
 * @pre ``ra8_mstp_init`` already ran so PFS writes land.
 * @post Both IIC pins are in their peripheral PSEL on success.
 *
 * @since 0.1.0
 */
static void i3c_demo_pfs_or_halt(void)
{
  if (ra8_pfs_route_peripheral(k_i3c_demo_pin_scl, k_ra8_psel_iic, "i3c_loopback.scl0") !=
      k_ra8_ok) {
    i3c_demo_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_i3c_demo_pin_sda, k_ra8_psel_iic, "i3c_loopback.sda0") !=
      k_ra8_ok) {
    i3c_demo_panic_halt();
  }

  /* IIC_B requires N-channel open-drain on SCL/SDA so the on-board
   * pullups can win bus arbitration; ra8_pfs_route_peripheral routes
   * the IIC mux but leaves NCODR clear (push-pull). Mirrors the
   * same NCODR write in
   * ra8_board_ek_ra8d2.c::internal_io_expander_route_pins.
   * HUM Ch 20.2.1 "PmnPFS Register" p 855 + HUM Ch 40 "IIC_B" p 2436 */
  if (ra8_mpc_set_open_drain(k_ra8_port_4, k_ra8_pin_0, true) != k_ra8_ok) {
    i3c_demo_panic_halt();
  }
  if (ra8_mpc_set_open_drain(k_ra8_port_4, k_ra8_pin_1, true) != k_ra8_ok) {
    i3c_demo_panic_halt();
  }
}

/**
 * @brief Bring CGC + SysTick + console + IIC_B up. Panic-halts on any fail.
 *
 * @details
 * Pulls PCLKA via ``ra8_cgc_get_clock_hz`` so the IIC_B BRR computation
 * tracks whatever clock tree the CGC driver is set to.
 *
 * @since 0.1.0
 */
static void i3c_demo_setup_or_halt(void)
{
  uint32_t pclka_hz = 0U;
  i3c_demo_clocks_or_halt(&pclka_hz);
  i3c_demo_pfs_or_halt();

  if (ra8_board_uart_console_init((uint32_t)k_i3c_demo_baud) != k_ra8_ok) {
    i3c_demo_panic_halt();
  }
  const ra8_i3c_cfg_t iic_cfg = {
    .mode     = k_ra8_i3c_mode_i2c,
    .bus_hz   = k_i3c_demo_bus_hz,
    .pclka_hz = pclka_hz,
  };
  if (ra8_i3c_init((uint8_t)k_i3c_demo_iic_channel, &iic_cfg) != k_ra8_ok) {
    i3c_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    i3c_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    i3c_demo_panic_halt();
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
  i3c_demo_setup_or_halt();
  ra8_isr_globals_enable();

  while (1) {
    bool            acked = false;
    const ra8_err_t err =
      ra8_i3c_scan((uint8_t)k_i3c_demo_iic_channel, (uint8_t)k_i3c_demo_probe_addr, &acked);
    const uint8_t* msg     = k_i3c_demo_msg_err;
    uint32_t       msg_len = (uint32_t)(sizeof(k_i3c_demo_msg_err) - 1U);
    if (err == k_ra8_ok) {
      if (acked) {
        msg     = k_i3c_demo_msg_ack;
        msg_len = (uint32_t)(sizeof(k_i3c_demo_msg_ack) - 1U);
      } else {
        msg     = k_i3c_demo_msg_nack;
        msg_len = (uint32_t)(sizeof(k_i3c_demo_msg_nack) - 1U);
      }
    }
    if (ra8_board_uart_console_write(msg, (size_t)msg_len) != k_ra8_ok) {
      break;
    }
    if (ra8_board_led_toggle(k_ra8_board_led1) != k_ra8_ok) {
      break;
    }
    ra8_delay_ms(k_i3c_demo_period_ms);
  }

  i3c_demo_panic_halt();
}
