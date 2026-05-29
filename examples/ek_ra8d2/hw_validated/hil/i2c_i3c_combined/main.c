/**
 * @file examples/ek_ra8d2/i2c_i3c_combined/main.c
 * @brief RIIC + I3C(I2C-mode) coexistence self-test for the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up BOTH I2C-capable controllers in one firmware and proves they
 * coexist on real silicon:
 *
 *   - ``ra_i2c`` (RIIC) channel 1 -> the on-board PI4IOE5V6408 expander
 *     (U15) at 0x43 (P512/P511), via the board's validated U15 bring-up.
 *     U15 ACKs, so this is a real controller<->device round-trip.
 *   - ``ra_i3c`` (I3C) channel 0 in I2C-compatibility mode -> the J27
 *     header (P400/P401). No on-board device there, so this is a
 *     controller bring-up self-test (init + a scan attempt).
 *
 * Each second SCI8 prints a single combined banner; the gate matches
 * ``combo: i2c ack=1 i3c initok`` -- emitted only when RIIC ACKed U15
 * AND the I3C controller initialised (init failure panic-halts first).
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
#include "ra_i3c.h"
#include "ra_isr.h"
#include "ra_mpc.h"
#include "ra_mstp.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @brief App-wide tunables. */
typedef enum : uint32_t {
  k_combo_baud        = 115200U,
  k_combo_period_ms   = 1000U,
  k_combo_bus_hz      = 100000U,
  k_combo_sci_channel = 8U,
  k_combo_i2c_channel = 1U, /**< RIIC ch1 -- U15 (P512/P511). */
  k_combo_i3c_channel = 0U, /**< I3C ch0 -- J27 (P400/P401). */
} combo_const_t;

/** @brief Probe address: U15 on RIIC ch1; reused as the I3C scan target. */
typedef enum : uint8_t {
  k_combo_probe_addr = 0x43U,
} combo_byte_t;

/** @brief SCI8 console pins (PD02 TXD8 / PD03 RXD8). */
static const ra_port_pin_t k_combo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_combo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);
/** @brief I3C ch0 pins (P400 SCL0 / P401 SDA0). RIIC ch1 pins are routed
 *         by the board U15 bring-up. */
static const ra_port_pin_t k_combo_pin_i3c_scl =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_0);
static const ra_port_pin_t k_combo_pin_i3c_sda =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_1);

/* The pass banner requires BOTH: RIIC ACKed U15 (real round-trip) and the
 * I3C controller is up (reaching the loop proves ra_i3c_init succeeded). */
static const uint8_t k_combo_msg_ok[]  = "combo: i2c ack=1 i3c initok\r\n";
static const uint8_t k_combo_msg_i2c[] = "combo: i2c ack=0 i3c initok\r\n";
static const uint8_t k_combo_msg_err[] = "combo: i2c ERROR i3c initok\r\n";

/**
 * @brief Park forever after a fatal init failure.
 * @pre Called only after a fatal error in boot.
 * @post CPU is parked; only a debugger or external reset wakes it.
 * @since 0.1.0
 */
static void combo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + SysTick + MSTP up; return PCLKA via @p out_pclka_hz.
 * @param[out] out_pclka_hz Receives the live PCLKA frequency.
 * @pre IRQs masked or single-threaded init context.
 * @post On success ``*out_pclka_hz`` is non-zero and CGC is live.
 * @since 0.1.0
 */
static void combo_clocks_or_halt(uint32_t* out_pclka_hz)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    combo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    combo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, out_pclka_hz) != k_ra_ok) {
    combo_panic_halt();
  }
  if (ra_mstp_init() != k_ra_ok) {
    combo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    combo_panic_halt();
  }
}

/**
 * @brief Route SCI8 console + I3C ch0 SCL0/SDA0 (open-drain) via PFS.
 * @pre ``ra_mstp_init`` already ran so PFS writes land.
 * @post All four pins are in their peripheral PSEL on success.
 * @since 0.1.0
 */
static void combo_pfs_or_halt(void)
{
  if (ra_pfs_route_peripheral(k_combo_pin_txd, k_ra_psel_sci_async, "combo.txd8") != k_ra_ok) {
    combo_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_combo_pin_rxd, k_ra_psel_sci_async, "combo.rxd8") != k_ra_ok) {
    combo_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_combo_pin_i3c_scl, k_ra_psel_iic, "combo.scl0") != k_ra_ok) {
    combo_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_combo_pin_i3c_sda, k_ra_psel_iic, "combo.sda0") != k_ra_ok) {
    combo_panic_halt();
  }
  if (ra_mpc_set_open_drain(k_ra_port_4, k_ra_pin_0, true) != k_ra_ok) {
    combo_panic_halt();
  }
  if (ra_mpc_set_open_drain(k_ra_port_4, k_ra_pin_1, true) != k_ra_ok) {
    combo_panic_halt();
  }
}

/**
 * @brief Bring up CGC + console + RIIC(ch1,U15) + I3C(ch0,i2c-mode).
 * @details Panic-halts on any fatal init error. RIIC ch1 comes up via the
 *   board U15 helper (its own pins/pull-ups); the I3C controller via
 *   ra_i3c_init in I2C-compatibility mode.
 * @since 0.1.0
 */
static void combo_setup_or_halt(void)
{
  uint32_t pclka_hz = 0U;
  combo_clocks_or_halt(&pclka_hz);
  combo_pfs_or_halt();

  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_combo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_combo_sci_channel, &sci_cfg) != k_ra_ok) {
    combo_panic_halt();
  }
  /* RIIC ch1 + U15 via the board's validated bring-up. */
  if (ra_board_io_expander_apply_project_sw4_defaults() != k_ra_ok) {
    combo_panic_halt();
  }
  /* I3C ch0 controller in I2C-compatibility mode. */
  const ra_i3c_cfg_t i3c_cfg = {
    .mode     = k_ra_i3c_mode_i2c,
    .bus_hz   = k_combo_bus_hz,
    .pclka_hz = pclka_hz,
  };
  if (ra_i3c_init((uint8_t)k_combo_i3c_channel, &i3c_cfg) != k_ra_ok) {
    combo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    combo_panic_halt();
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up both controllers then scans each.
 * @return Never returns.
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On clean entry the CPU stays in the scan + blink loop forever.
 * @post On any HAL hard error the CPU parks.
 * @since 0.1.0
 */
int32_t main(void)
{
  combo_setup_or_halt();
  ra_isr_globals_enable();

  while (1) {
    /* RIIC ch1 -> U15: a real ACK proves the ra_i2c controller + device. */
    bool           i2c_acked = false;
    const ra_err_t i2c_err =
      ra_i2c_scan((uint8_t)k_combo_i2c_channel, (uint8_t)k_combo_probe_addr, &i2c_acked);

    /* I3C ch0 -> J27 (no device on a bare EVM); the scan result is
     * informational -- reaching here already proves ra_i3c_init worked. */
    bool i3c_acked = false;
    (void)ra_i3c_scan((uint8_t)k_combo_i3c_channel, (uint8_t)k_combo_probe_addr, &i3c_acked);

    const uint8_t* msg     = k_combo_msg_err;
    uint32_t       msg_len = (uint32_t)(sizeof(k_combo_msg_err) - 1U);
    if (i2c_err == k_ra_ok) {
      if (i2c_acked) {
        msg     = k_combo_msg_ok;
        msg_len = (uint32_t)(sizeof(k_combo_msg_ok) - 1U);
      } else {
        msg     = k_combo_msg_i2c;
        msg_len = (uint32_t)(sizeof(k_combo_msg_i2c) - 1U);
      }
    }
    if (ra_sci_write_polling((uint8_t)k_combo_sci_channel, msg, msg_len) != k_ra_ok) {
      break;
    }
    if (ra_board_led_toggle(k_ra_board_led1) != k_ra_ok) {
      break;
    }
    ra_delay_ms(k_combo_period_ms);
  }

  combo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
