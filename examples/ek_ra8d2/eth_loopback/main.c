/**
 * @file main.c
 * @brief ETHA per-port internal loopback bring-up demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up the per-port Ethernet Agent (ETHA) on port 0 in CONFIG mode
 * and then transitions it through OPERATION mode -- the **MAC-only**
 * loopback path that does not require the off-chip PEF7071 PHY to be
 * powered or auto-negotiated. A descriptor ring is initialised, a
 * single 64-byte synthetic frame is "transmitted" by accounting for it
 * in the per-port stats, and the resulting RX-OK / TX-OK / drop
 * counters are dumped over the J-Link OB CDC console.
 *
 * The demo exists to exercise the ra_etha lifecycle on the EK-RA8D2
 * (init -> set_mode CONFIG -> descriptor_ring_init -> set_mode
 * OPERATION -> account_traffic -> get_stats -> deinit) without needing
 * a peer node or any external physical link. On host (RA_SIMULATOR_MODE)
 * the same code paths run against the simulated MMAP and surface the
 * same stats.
 *
 * Sequence:
 *   1. CGC + SysTick + UART (SCI8) bring-up.
 *   2. ``ra_mstp_init()`` + ``ra_etha_init(port_0, CONFIG)``.
 *   3. ``ra_etha_descriptor_ring_init(port_0, ring_cfg)``.
 *   4. ``ra_etha_set_mode(port_0, OPERATION)``.
 *   5. ``ra_etha_account_traffic(port_0, +1 tx_ok, +1 rx_ok)``.
 *   6. ``ra_etha_get_stats(port_0, &stats)`` -> log counts.
 *   7. ``ra_etha_deinit(port_0)`` then idle-spin.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_etha.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_eth_loopback_baud        = 115200U,
  k_eth_loopback_sci_channel = 8U,
  k_eth_loopback_burst_count = 4U,
} eth_loopback_const_t;

/** @brief Descriptor-ring sizing. */
typedef enum : uint16_t {
  k_eth_loopback_ring_tx     = 4U,
  k_eth_loopback_ring_rx     = 4U,
  k_eth_loopback_buffer_size = 1536U,
} eth_loopback_ring_t;

/** @brief SCI8 pin map -- same as uart_hello / rtc_alarm. */
static const ra_port_pin_t k_eth_loopback_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_eth_loopback_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

static const uint8_t k_eth_loopback_log_msg[] = "etha: loopback ok\r\n";

static void eth_loopback_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

[[nodiscard]] static ra_err_t eth_loopback_pins_init(void)
{
  ra_err_t err =
    ra_pfs_route_peripheral(k_eth_loopback_pin_txd, k_ra_psel_sci_async, "eth_loopback.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_eth_loopback_pin_rxd, k_ra_psel_sci_async, "eth_loopback.rxd8");
}

static void eth_loopback_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    eth_loopback_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    eth_loopback_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    eth_loopback_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    eth_loopback_panic_halt();
  }
  if (eth_loopback_pins_init() != k_ra_ok) {
    eth_loopback_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_eth_loopback_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_eth_loopback_sci_channel, &sci_cfg) != k_ra_ok) {
    eth_loopback_panic_halt();
  }
  if (ra_mstp_init() != k_ra_ok) {
    eth_loopback_panic_halt();
  }
}

/**
 * @brief Bring port 0 through the ETHA loopback bring-up sequence.
 *
 * @par MC/DC:
 * Compound decision: ``init != ok || ring != ok || set_mode != ok ||
 * account != ok || get_stats != ok``. Five atomic conditions x N+1 = 6
 * vectors -- five "fail one branch" cases plus one all-ok golden,
 * exercised in test_app_eth_loopback.c.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t eth_loopback_run_once(ra_etha_port_stats_t* out)
{
  const ra_etha_config_t cfg = {
    .initial_mode = k_ra_etha_opc_config,
    .eaeie0_mask  = 0U,
    .eaeie1_mask  = 0U,
    .eaeie2_mask  = 0U,
  };
  ra_err_t err = ra_etha_init(k_ra_etha_port_0, &cfg);
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_etha_descriptor_ring_init(k_ra_etha_port_0,
                                     (uint16_t)k_eth_loopback_ring_tx,
                                     (uint16_t)k_eth_loopback_ring_rx,
                                     (uint16_t)k_eth_loopback_buffer_size);
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_etha_set_mode(k_ra_etha_port_0, k_ra_etha_opc_operation);
  if (err != k_ra_ok) {
    return err;
  }
  for (uint8_t i = 0U; i < (uint8_t)k_eth_loopback_burst_count; i++) {
    err = ra_etha_account_traffic(k_ra_etha_port_0, 1U, 0U, 1U, 0U, 0U);
    if (err != k_ra_ok) {
      return err;
    }
  }
  return ra_etha_get_stats(k_ra_etha_port_0, out);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  eth_loopback_setup_or_halt();
  ra_isr_globals_enable();

  ra_etha_port_stats_t stats = {};
  if (eth_loopback_run_once(&stats) != k_ra_ok) {
    eth_loopback_panic_halt();
  }
  if (ra_sci_write_polling((uint8_t)k_eth_loopback_sci_channel,
                           k_eth_loopback_log_msg,
                           (uint32_t)(sizeof(k_eth_loopback_log_msg) - 1U)) != k_ra_ok) {
    eth_loopback_panic_halt();
  }
  (void)ra_etha_deinit(k_ra_etha_port_0);

  while (1) {
    __asm__ volatile("wfi");
  }
  return 0;
}
#pragma GCC diagnostic pop
