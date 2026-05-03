/**
 * @file main.c
 * @brief IIC_B (I3C-in-I2C-mode) self-test smoke app for the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Standalone EVM-tier app that exercises the IIC_B master driver
 * (``libs/ra_hal/ra_iic_b.h``) without requiring an external slave.
 * The flow:
 *
 *   1. ``ra_cgc_init`` -- bring CPUCLK0 / PCLKA up.
 *   2. ``ra_mstp_init`` + ``ra_pfs_route_peripheral`` for SCL1
 *      (P512) and SDA1 (P511); these are the dedicated I2C pins
 *      wired to the Pmod1 / Arduino / camera I2C buses on the
 *      EK-RA8D2 (UM Table 17 / 20 / 32).
 *   3. ``ra_iic_b_init(0, ...)`` at 100 kHz Sm.
 *   4. ``ra_iic_b_scan`` against an unused 7-bit address (0x77,
 *      typical BME280 default). The bus is unpopulated on a bare
 *      EVM so the slave NACKs -- the call still returns ``k_ra_ok``
 *      with ``out_acked = false``, which is treated as "the bus
 *      controller is alive and clocking SCL".
 *   5. LED1 toggles on each scan and SCI8 prints
 *      ``"iic_b: scan 0x77 ack=0\r\n"`` once a second so a host
 *      terminal can see the heartbeat. LED2 latches ON if the
 *      driver itself returns a hard error (busy / hw_timeout).
 *
 * Hardware: bare EK-RA8D2 only. The dedicated I2C pull-ups on
 * P511 / P512 are populated on the EVM (R5 / R6) so no wiring is
 * required.
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
#include "ra_iic_b.h"
#include "ra_isr.h"
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
  k_i2c_demo_iic_channel = 0U,
} i2c_demo_const_t;

/** @brief Probe target -- BME280 default address; bus is empty so a
 *         NACK is the expected, harmless outcome on a bare EVM. */
typedef enum : uint8_t {
  k_i2c_demo_probe_addr = 0x77U,
} i2c_demo_byte_t;

/** @brief Pinout for SCI8 console (PD02 TXD8 / PD03 RXD8). */
static const ra_port_pin_t k_i2c_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_i2c_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);
/** @brief Pinout for IIC channel 0 (P512 SCL / P511 SDA on EK-RA8D2). */
static const ra_port_pin_t k_i2c_demo_pin_scl =
  (ra_port_pin_t)(((uint16_t)k_ra_port_5 << 8) | (uint16_t)k_ra_pin_12);
static const ra_port_pin_t k_i2c_demo_pin_sda =
  (ra_port_pin_t)(((uint16_t)k_ra_port_5 << 8) | (uint16_t)k_ra_pin_11);

static const uint8_t k_i2c_demo_msg_ack[]  = "iic_b: scan 0x77 ack=1\r\n";
static const uint8_t k_i2c_demo_msg_nack[] = "iic_b: scan 0x77 ack=0\r\n";
static const uint8_t k_i2c_demo_msg_err[]  = "iic_b: scan ERROR\r\n";

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
  if (ra_pfs_route_peripheral(k_i2c_demo_pin_scl, k_ra_psel_iic, "i2c_loopback.scl1") != k_ra_ok) {
    i2c_demo_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_i2c_demo_pin_sda, k_ra_psel_iic, "i2c_loopback.sda1") != k_ra_ok) {
    i2c_demo_panic_halt();
  }
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
  const ra_iic_b_cfg_t iic_cfg = {
    .bus_hz   = k_i2c_demo_bus_hz,
    .pclka_hz = pclka_hz,
  };
  if (ra_iic_b_init((uint8_t)k_i2c_demo_iic_channel, &iic_cfg) != k_ra_ok) {
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
      ra_iic_b_scan((uint8_t)k_i2c_demo_iic_channel, (uint8_t)k_i2c_demo_probe_addr, &acked);
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
