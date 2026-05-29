/**
 * @file examples/ek_ra8d2/i2c_loopback/main.c
 * @brief RIIC (ra_i2c) controller self-test against the on-board U15
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Standalone EVM-tier app that exercises the ra_i2c (RIIC) controller
 * driver (``libs/ra_hal/inc/ra_i2c.h``) against the on-board PI4IOE5V6408
 * I2C I/O port expander (U15) at 7-bit address 0x43 -- the only I2C
 * peripheral guaranteed to be populated on a bare EK-RA8D2 v1 (board UM
 * section 4.3.4 "Switch Configuration", p 24). U15 sits on RIIC channel 1
 * (P512 SCL1 / P511 SDA1), per issue #46. The flow:
 *
 *   1. ``ra_cgc_init`` -- bring CPUCLK0 / PCLKA up.
 *   2. ``ra_pfs_route_peripheral`` for the SCI8 console pins only.
 *   3. ``ra_board_io_expander_apply_project_sw4_defaults()`` -- the
 *      board's validated U15 bring-up: bus-recover, P109/P311 pull-ups,
 *      P512/P511 SCL1/SDA1 route + NCODR, ra_i2c_init(ch1) and a U15
 *      write. A k_ra_ok return means U15 ACKed.
 *   4. ``ra_i2c_scan`` against ``0x43`` in a loop -- the U15 expander
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

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_i2c.h"
#include "ra_isr.h"
#include "ra_mpc.h"
#include "ra_mstp.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @brief App-wide tunables. */
typedef enum : uint32_t {
  k_i2c_demo_baud        = 115200U,
  k_i2c_demo_period_ms   = 1000U,
  k_i2c_demo_bus_hz      = 100000U,
  k_i2c_demo_sci_channel = 8U,
  k_i2c_demo_iic_channel = 1U, /* RIIC ch1 (P512 SCL1 / P511 SDA1) -- U15 lives here, per #46 */
} i2c_demo_const_t;

/** @brief Probe target -- on-board PI4IOE5V6408 I/O port expander U15
 *         at 7-bit address 0x43 (board UM section 4.3.4, p 24). This
 *         device is always populated on a bare EK-RA8D2 v1 and ACKs
 *         every address-only probe, so a successful scan proves both
 *         the bus and the controller are alive. */
typedef enum : uint8_t {
  k_i2c_demo_probe_addr = 0x43U,
} i2c_demo_byte_t;

/** @brief Pinout for SCI8 console (PD02 TXD8 / PD03 RXD8). */
static const ra_port_pin_t k_i2c_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_i2c_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);
/* RIIC ch1 SCL1/SDA1 (P512/P511) routing + the P109/P311 pull-ups and
 * NCODR are owned by ra_board_io_expander_apply_project_sw4_defaults(),
 * the same validated bring-up the board library uses for U15. */

static const uint8_t k_i2c_demo_msg_ack[]  = "i2c: scan 0x43 ack=1\r\n";
static const uint8_t k_i2c_demo_msg_nack[] = "i2c: scan 0x43 ack=0\r\n";
static const uint8_t k_i2c_demo_msg_err[]  = "i2c: scan ERROR\r\n";

/** @brief Park forever after a fatal init failure.
 *
 * @pre Called only after a fatal error in boot.
 * @post CPU is parked; only a debugger or external reset wakes it.
 * @since 0.1.0
 */
static void i2c_demo_panic_halt(void)
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
static void i2c_demo_clocks_or_halt(uint32_t* out_pclka_hz)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    i2c_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    i2c_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, out_pclka_hz) != k_ra_ok) {
    i2c_demo_panic_halt();
  }
  if (ra_mstp_init() != k_ra_ok) {
    i2c_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    i2c_demo_panic_halt();
  }
}

/**
 * @brief Route SCI8 console pins + IIC channel-0 SCL/SDA via PFS.
 *
 * @pre ``ra_mstp_init`` already ran so PFS writes land.
 * @post All four pins are in their peripheral PSEL on success.
 *
 * @since 0.1.0
 */
static void i2c_demo_pfs_or_halt(void)
{
  if (ra_pfs_route_peripheral(k_i2c_demo_pin_txd, k_ra_psel_sci_async, "i2c_loopback.txd8") !=
      k_ra_ok) {
    i2c_demo_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_i2c_demo_pin_rxd, k_ra_psel_sci_async, "i2c_loopback.rxd8") !=
      k_ra_ok) {
    i2c_demo_panic_halt();
  }
  /* RIIC ch1 pin routing (P512/P511), pull-ups (P109/P311) and NCODR are
   * done by ra_board_io_expander_apply_project_sw4_defaults() in setup. */
}

/**
 * @brief Bring CGC + SysTick + console + IIC_B up. Panic-halts on any fail.
 *
 * @details
 * Pulls PCLKA via ``ra_cgc_get_clock_hz`` so the IIC_B BRR computation
 * tracks whatever clock tree the CGC driver is set to.
 *
 * @since 0.1.0
 */
static void i2c_demo_setup_or_halt(void)
{
  uint32_t pclka_hz = 0U;
  i2c_demo_clocks_or_halt(&pclka_hz);
  i2c_demo_pfs_or_halt();

  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_i2c_demo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_i2c_demo_sci_channel, &sci_cfg) != k_ra_ok) {
    i2c_demo_panic_halt();
  }
  /* Bring up RIIC ch1 + confirm U15 via the board's validated path
   * (bus-recover + P109/P311 pull-ups + P512/P511 route + ra_i2c_init +
   * a U15 write). k_ra_ok means U15 ACKed the project SW4 byte; the bus
   * is then live for the ra_i2c_scan loop below. */
  if (ra_board_io_expander_apply_project_sw4_defaults() != k_ra_ok) {
    i2c_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    i2c_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    i2c_demo_panic_halt();
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up CGC + IIC_B + console then probes.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On clean entry the CPU stays in the scan + blink loop forever.
 * @post On any HAL hard error LED2 latches ON and the loop exits.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  i2c_demo_setup_or_halt();
  ra_isr_globals_enable();

  while (1) {
    bool           acked = false;
    const ra_err_t err =
      ra_i2c_scan((uint8_t)k_i2c_demo_iic_channel, (uint8_t)k_i2c_demo_probe_addr, &acked);
    const uint8_t* msg     = k_i2c_demo_msg_err;
    uint32_t       msg_len = (uint32_t)(sizeof(k_i2c_demo_msg_err) - 1U);
    if (err == k_ra_ok) {
      if (acked) {
        msg     = k_i2c_demo_msg_ack;
        msg_len = (uint32_t)(sizeof(k_i2c_demo_msg_ack) - 1U);
      } else {
        msg     = k_i2c_demo_msg_nack;
        msg_len = (uint32_t)(sizeof(k_i2c_demo_msg_nack) - 1U);
      }
    } else {
      (void)ra_board_led_on(k_ra_board_led2);
    }
    if (ra_sci_write_polling((uint8_t)k_i2c_demo_sci_channel, msg, msg_len) != k_ra_ok) {
      break;
    }
    if (ra_board_led_toggle(k_ra_board_led1) != k_ra_ok) {
      break;
    }
    ra_delay_ms(k_i2c_demo_period_ms);
  }

  i2c_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
