/**
 * @file examples/ek_ra8d2/hw_validated/hil/eth_loopback/main.c
 * @brief ETHA per-port internal loopback bring-up demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Brings up the per-port Ethernet Agent (ETHA) on port 0 in CONFIG mode
 * and then transitions it through OPERATION mode -- the **MAC-only**
 * loopback path that does not require the off-chip PEF7071 PHY to be
 * powered or auto-negotiated. A descriptor ring is initialized, a
 * single 64-byte synthetic frame is "transmitted" by accounting for it
 * in the per-port stats, and the resulting RX-OK / TX-OK / drop
 * counters are dumped over the J-Link OB CDC console.
 *
 * The demo exists to exercise the ra8_etha lifecycle on the EK-RA8D2
 * (init -> set_mode CONFIG -> descriptor_ring_init -> set_mode
 * OPERATION -> account_traffic -> get_stats -> deinit) without needing
 * a peer node or any external physical link. On host (RA8_OFF_TARGET)
 * the same code paths run against the fake MMAP and surface the
 * same stats.
 *
 * Sequence:
 *   1. CGC + SysTick + UART (SCI8) bring-up.
 *   2. ``ra8_mstp_init()`` + ``ra8_etha_init(port_0, CONFIG)``.
 *   3. ``ra8_etha_descriptor_ring_init(port_0, ring_cfg)``.
 *   4. ``ra8_etha_set_mode(port_0, OPERATION)``.
 *   5. ``ra8_etha_account_traffic(port_0, +1 tx_ok, +1 rx_ok)``.
 *   6. ``ra8_etha_get_stats(port_0, &stats)`` -> log counts.
 *   7. ``ra8_etha_deinit(port_0)`` then idle-spin.
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
#include "ra8_etha.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_eth_loopback_baud        = 115200U, /**< Ethernet loopback baud.        */
  k_eth_loopback_burst_count = 4U,      /**< Ethernet loopback burst count. */
} eth_loopback_const_t;

/** @brief Descriptor-ring sizing. */
typedef enum : uint16_t {
  k_eth_loopback_ring_tx     = 4U,    /**< Ethernet loopback ring TX.     */
  k_eth_loopback_ring_rx     = 4U,    /**< Ethernet loopback ring RX.     */
  k_eth_loopback_buffer_size = 1536U, /**< Ethernet loopback buffer size. */
} eth_loopback_ring_t;

/** @brief Fixed UART diagnostics for boot and successful ETHA loopback. */
static const uint8_t s_eth_loopback_log_msg[]  = "etha: loopback ok\r\n";
static const uint8_t s_eth_loopback_boot_msg[] = "etha: boot\r\n";

/**
 * @brief Park the processor after an unrecoverable ETHA demo failure.
 *
 * @details Enters a permanent wait-for-interrupt loop so descriptor, port, and
 *          diagnostic state remain accessible to an attached debugger.
 *
 * @return None.
 *
 * @pre The caller has determined Ethernet validation cannot continue.
 * @pre Any desired UART diagnostic has already completed.
 * @post The function never returns to its caller.
 * @post No more descriptor or statistics operations are initiated.
 *
 * @note Fatal-path helper for this single-core image only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_eth_loopback_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Initialize clocks, module-stop control, timekeeping, and console.
 *
 * @details Brings up the dependencies required by the ETHA loopback sequence
 *          and opens the board UART for its boot and success diagnostics. Any
 *          failure enters the permanent panic halt.
 *
 * @return None.
 *
 * @pre Reset-time initialization configured the core and C runtime.
 * @pre The SCI8 board console is available to this image.
 * @post On success clocks, MSTP, timing, and console services are ready.
 * @post On failure the function never returns to its caller.
 *
 * @note Single-shot boot helper; it is not reentrant.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_eth_loopback_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_eth_loopback_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_eth_loopback_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    internal_eth_loopback_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_eth_loopback_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_eth_loopback_baud) != k_ra8_ok) {
    internal_eth_loopback_panic_halt();
  }
}

/**
 * @brief Bring port 0 through the ETHA loopback bring-up sequence.
 *
 * @details Initializes ETHA port 0, creates its descriptor rings, enters
 *          operation mode, accounts a deterministic loopback burst, and
 *          returns the resulting caller-visible port statistics.
 *
 * @param[out] out Receives the final ETHA port statistics on success.
 *
 * @par MC/DC:
 * Compound decision: ``init != ok || ring != ok || set_mode != ok ||
 * account != ok || get_stats != ok``. Five atomic conditions x N+1 = 6
 * vectors -- five "fail one branch" cases plus one all-ok golden,
 * exercised in test_app_eth_loopback.c.
 *
 * @return ra8_err_t Status from the first failing ETHA operation.
 * @retval k_ra8_ok The loopback accounting sequence and statistics read succeeded.
 * @retval (other)  The first initialization, ring, mode, accounting, or read error.
 *
 * @pre @p out points to writable caller-owned statistics storage.
 * @pre Module-stop control and clocks were initialized by the setup helper.
 * @post On success @p out contains statistics after the deterministic burst.
 * @post On failure no later ETHA step is attempted.
 *
 * @note Single-port demonstration; it does not support concurrent ETHA owners.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t
internal_eth_loopback_run_once(ra8_etha_port_stats_t* out)
{
  const ra8_etha_config_t cfg = {
    .initial_mode = k_ra8_etha_opc_config,
    .eaeie0_mask  = 0U,
    .eaeie1_mask  = 0U,
    .eaeie2_mask  = 0U,
  };
  ra8_err_t err = ra8_etha_init(k_ra8_etha_port_0, &cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_etha_descriptor_ring_init(k_ra8_etha_port_0,
                                      (uint16_t)k_eth_loopback_ring_tx,
                                      (uint16_t)k_eth_loopback_ring_rx,
                                      (uint16_t)k_eth_loopback_buffer_size);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_etha_set_mode(k_ra8_etha_port_0, k_ra8_etha_opc_operation);
  if (err != k_ra8_ok) {
    return err;
  }
  for (uint8_t i = 0U; i < (uint8_t)k_eth_loopback_burst_count; i++) {
    err = ra8_etha_account_traffic(k_ra8_etha_port_0, 1U, 0U, 1U, 0U, 0U);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return ra8_etha_get_stats(k_ra8_etha_port_0, out);
}

void main(void)
{
  internal_eth_loopback_setup_or_halt();
  ra8_isr_globals_enable();

  /* Boot banner -- emit before the loopback runs so the HIL host can
   * confirm the firmware booted even when the ETHA loopback fails for
   * board-specific reasons (PHY not negotiated, missing pull-ups, etc.). */
  (void)ra8_board_uart_console_write(s_eth_loopback_boot_msg,
                                     (size_t)(sizeof(s_eth_loopback_boot_msg) - 1U));

  ra8_etha_port_stats_t stats = {};
  if (internal_eth_loopback_run_once(&stats) != k_ra8_ok) {
    internal_eth_loopback_panic_halt();
  }
  if (ra8_board_uart_console_write(s_eth_loopback_log_msg,
                                   (size_t)(sizeof(s_eth_loopback_log_msg) - 1U)) != k_ra8_ok) {
    internal_eth_loopback_panic_halt();
  }
  (void)ra8_etha_deinit(k_ra8_etha_port_0);

  while (1) {
    __asm__ volatile("wfi");
  }
}
