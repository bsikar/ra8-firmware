/**
 * @file examples/ek_ra8d2/hw_pending/eth_tsn_tas_demo/main.c
 * @brief TSN scheduled-traffic demo: 802.1Qbv time-aware + 802.1Qav credit shaper
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Programs the RA8D2 Ethernet Agent (ETHA) time-sensitive-networking shapers
 * that no other example referenced (recon gap #134).
 *
 * NOTE on the gap: recon #134 named ``ra8_tsn`` as the "time-sensitive
 * networking" driver, but ``libs/ra8_hal/ra8_tsn`` is the on-die **temperature
 * sensor** (already demonstrated by ``adc_diag_tsn_demo``, #183). The real TSN
 * networking surface on this part is the ETHA shaper block: the time-aware
 * shaper (TAS / 802.1Qbv scheduled traffic) and the credit-based shaper (CBS /
 * 802.1Qav), exposed by ``ra8_etha_set_tas_schedule`` / ``ra8_etha_enable_tas``
 * and ``ra8_etha_configure_cbs`` / ``ra8_etha_get_cbs_state`` (HUM Ch 32
 * "Ethernet Agent"). Those are what this example drives.
 *
 * TAS needs the gPTP time base as its schedule reference (the shaper's
 * ``@pre PTP timer (ra8_eth_gptp) provides the time reference``), so the app
 * brings up ``ra8_eth_gptp`` first, then:
 *
 *   1. ``ra8_etha_init`` on port 0 in CONFIG mode (shaper registers are only
 *      writable in CONFIG).
 *   2. ``ra8_etha_set_tas_schedule`` with a 2-entry gate-control list: window 0
 *      opens only the class-7 (PTP / control) gate, window 1 opens the
 *      best-effort classes 0-6. ``ra8_etha_enable_tas`` arms the scheduler.
 *   3. ``ra8_etha_configure_cbs`` on traffic class 2 (AVB class A) with an
 *      illustrative credit increment + upper limit, then
 *      ``ra8_etha_get_cbs_state`` reads the oper-side mirror back.
 *   4. ``ra8_etha_get_status`` reads the TAS cycle-time monitor.
 *
 * The fixed verdict ``"tsn: schedule PASS"`` prints only when every shaper
 * call returned ``k_ra8_ok``. The programmed entry count, CBS enable / gate
 * bits, and TAS cycle monitor are logged for the bench operator.
 *
 * hw_pending: ``tools/ra8_emulator`` has no Ethernet / ETHA peripheral model, and
 * the EK-RA8D2 Ethernet wire is marginal (#21), so this is compile-gated and
 * bench-only -- matching the driver-gap example wave (#182-188). Asserting the
 * egress is actually shaped to the schedule needs a multi-node measurement rig
 * (bench wiring #89).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_eth_gptp.h"
#include "ra8_etha.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/** @brief Demo tunables (illustrative shaper values). */
typedef enum : uint32_t {
  k_tsn_baud          = 115200U, /**< SCI8 console baud.                     */
  k_tsn_period_ms     = 1000U,   /**< Delay between cycles.                  */
  k_tsn_win_units     = 125000U, /**< Per-window bus time (TAS clock units). */
  k_tsn_cycle_units   = 250000U, /**< Full GCL cycle = 2 windows.            */
  k_tsn_cbs_increment = 1500U,   /**< CBS credit increment (20-bit field).   */
  k_tsn_cbs_upper_lim = 65536U,  /**< CBS upper credit limit (31-bit field). */
} tsn_const_t;

/** @brief Formatting + iteration constants. */
typedef enum : uint8_t {
  k_tsn_radix        = 10U, /**< Decimal serialiser radix.          */
  k_tsn_dec_u32_max  = 10U, /**< Max decimal digits for a uint32_t. */
  k_tsn_gate_entries = 2U,  /**< Gate-control-list entry count.     */
} tsn_fmt_t;

/**
 * @brief TAS gate vectors: bit q opens the class-q gate (bit 7 = PTP/control).
 */
typedef enum : uint8_t {
  k_tsn_gate_ptp_only   = 0x80U, /**< Only class 7 (PTP / control) open. */
  k_tsn_gate_besteffort = 0x7FU, /**< Classes 0-6 (best-effort) open.    */
} tsn_gate_t;

/* Console line fragments (kept short so each write is one shift-register
 * fill; the periodic log is the only output path). */
static const uint8_t k_tsn_tas_prefix[]   = "tsn: tas_entries=";
static const uint8_t k_tsn_cbs_prefix[]   = "tsn: cbs_en=";
static const uint8_t k_tsn_cbs_gate_sep[] = " gate=";
static const uint8_t k_tsn_cyc_prefix[]   = "tsn: tas_cycle=";
static const uint8_t k_tsn_crlf[]         = "\r\n";
static const uint8_t k_tsn_verdict_pass[] = "tsn: schedule PASS\r\n";
static const uint8_t k_tsn_verdict_fail[] = "tsn: schedule FAIL\r\n";

/**
 * @brief Park forever after a fatal init error.
 *
 * @pre Called only after an unrecoverable bring-up failure.
 * @post CPU is parked; only a debugger or reset wakes it.
 * @since 0.1.0
 */
static void tsn_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Write a byte span to the SCI8 console, discarding the status.
 *
 * @param[in] data Non-NULL byte span to transmit.
 * @param[in] len  Byte count (0 is a no-op).
 *
 * @pre ``ra8_board_uart_console_init`` has succeeded.
 * @pre ``data`` points at ``len`` readable bytes.
 * @post ``len`` bytes have been queued to the console UART.
 * @since 0.1.0
 */
static void tsn_write(const uint8_t* data, uint32_t len)
{
  (void)ra8_board_uart_console_write(data, (size_t)len);
}

/**
 * @brief Serialise an unsigned 32-bit value into decimal ASCII.
 *
 * @param[out] buf Destination, at least ``k_tsn_dec_u32_max`` bytes.
 * @param[in]  val Value to serialise.
 *
 * @return Number of digits written (1..10).
 *
 * @pre ``buf`` is non-NULL and sized for the widest uint32_t.
 * @pre No trailing NUL is required by the caller.
 * @post ``buf`` holds the most-significant digit first.
 * @post The return value is in ``[1, k_tsn_dec_u32_max]``.
 * @since 0.1.0
 */
static uint32_t tsn_u32_to_dec(uint8_t* buf, uint32_t val)
{
  if (val == 0U) {
    buf[0] = (uint8_t)'0';
    return 1U;
  }
  uint8_t  tmp[k_tsn_dec_u32_max];
  uint32_t n = 0U;
  uint32_t v = val;
  while (v != 0U) {
    tmp[n] = (uint8_t)('0' + (uint8_t)(v % (uint32_t)k_tsn_radix));
    v      = v / (uint32_t)k_tsn_radix;
    n++;
  }
  for (uint32_t i = 0U; i < n; i++) {
    buf[i] = tmp[n - 1U - i];
  }
  return n;
}

/**
 * @brief Log one unsigned 32-bit value as decimal ASCII.
 *
 * @param[in] val Value to print.
 *
 * @pre The console has been initialised.
 * @pre ``val`` fits in a uint32_t (always true).
 * @post The decimal digits of ``val`` have been queued to the console.
 * @since 0.1.0
 */
static void tsn_write_u32(uint32_t val)
{
  uint8_t        buf[k_tsn_dec_u32_max];
  const uint32_t n = tsn_u32_to_dec(buf, val);
  tsn_write(buf, n);
}

/**
 * @brief Program + arm the 802.1Qbv time-aware shaper on port 0.
 *
 * @details
 * Builds a 2-entry gate-control list (window 0: class-7 gate only; window 1:
 * best-effort classes 0-6), commits it via ``ra8_etha_set_tas_schedule``, and
 * arms the scheduler with ``ra8_etha_enable_tas``. The programmed entry count
 * is logged.
 *
 * @return True iff both TAS calls returned ``k_ra8_ok``.
 *
 * @pre ``ra8_etha_init`` left port 0 in CONFIG mode.
 * @pre The gPTP time base is running (``ra8_eth_gptp_init``).
 * @post One ``tsn: tas_entries=`` line has been queued.
 * @post On success EATASC.TASE is set (scheduler armed).
 * @since 0.1.0
 */
static bool tsn_program_tas(void)
{
  bool                      ok                        = true;
  const ra8_etha_tas_gate_t gates[k_tsn_gate_entries] = {
    {.gate_state  = (uint8_t)k_tsn_gate_ptp_only,
     .time_units  = (uint32_t)k_tsn_win_units,
     .cut_through = 0U},
    {.gate_state  = (uint8_t)k_tsn_gate_besteffort,
     .time_units  = (uint32_t)k_tsn_win_units,
     .cut_through = 0U},
  };
  const ra8_err_t sched_err = ra8_etha_set_tas_schedule(k_ra8_etha_port_0,
                                                        gates,
                                                        (uint16_t)k_tsn_gate_entries,
                                                        (uint32_t)k_tsn_cycle_units,
                                                        0U);
  if (sched_err != k_ra8_ok) {
    ok = false;
  }
  if (ra8_etha_enable_tas(k_ra8_etha_port_0, 1U) != k_ra8_ok) {
    ok = false;
  }
  tsn_write(k_tsn_tas_prefix, (uint32_t)(sizeof(k_tsn_tas_prefix) - 1U));
  tsn_write_u32((uint32_t)k_tsn_gate_entries);
  tsn_write(k_tsn_crlf, (uint32_t)(sizeof(k_tsn_crlf) - 1U));
  return ok;
}

/**
 * @brief Configure the 802.1Qav credit-based shaper on traffic class 2.
 *
 * @details
 * Programs an illustrative credit increment + upper limit on class 2 (AVB
 * class A) via ``ra8_etha_configure_cbs``, then reads the oper-side mirror
 * back with ``ra8_etha_get_cbs_state`` and logs the enable + gate-open bits.
 *
 * @return True iff both CBS calls returned ``k_ra8_ok``.
 *
 * @pre ``ra8_etha_init`` left port 0 in CONFIG mode.
 * @pre The console has been initialised.
 * @post One ``tsn: cbs_en=.. gate=..`` line has been queued.
 * @post Class-2 CBS admin registers reflect the programmed parameters.
 * @since 0.1.0
 */
static bool tsn_program_cbs(void)
{
  bool                       ok    = true;
  const ra8_etha_cbs_param_t param = {
    .increment = (uint32_t)k_tsn_cbs_increment,
    .upper_lim = (uint32_t)k_tsn_cbs_upper_lim,
  };
  if (ra8_etha_configure_cbs(k_ra8_etha_port_0, k_ra8_etha_tc_2, 1U, &param) != k_ra8_ok) {
    ok = false;
  }
  uint8_t              enabled   = 0U;
  uint8_t              gate_open = 0U;
  ra8_etha_cbs_param_t oper      = {};
  const ra8_err_t      st_err =
    ra8_etha_get_cbs_state(k_ra8_etha_port_0, k_ra8_etha_tc_2, &enabled, &gate_open, &oper);
  if (st_err != k_ra8_ok) {
    ok = false;
  }
  tsn_write(k_tsn_cbs_prefix, (uint32_t)(sizeof(k_tsn_cbs_prefix) - 1U));
  tsn_write_u32((uint32_t)enabled);
  tsn_write(k_tsn_cbs_gate_sep, (uint32_t)(sizeof(k_tsn_cbs_gate_sep) - 1U));
  tsn_write_u32((uint32_t)gate_open);
  tsn_write(k_tsn_crlf, (uint32_t)(sizeof(k_tsn_crlf) - 1U));
  return ok;
}

/**
 * @brief Read + log the ETHA port-0 status (TAS cycle-time monitor).
 *
 * @return True iff ``ra8_etha_get_status`` returned ``k_ra8_ok``.
 *
 * @pre ``ra8_etha_init`` brought up port 0.
 * @pre The console has been initialised.
 * @post One ``tsn: tas_cycle=`` line has been queued.
 * @post No ETHA register state was mutated (status read is passive).
 * @since 0.1.0
 */
static bool tsn_log_status(void)
{
  bool              ok  = true;
  ra8_etha_status_t sts = {};
  const ra8_err_t   err = ra8_etha_get_status(k_ra8_etha_port_0, &sts);
  if (err != k_ra8_ok) {
    ok = false;
  }
  tsn_write(k_tsn_cyc_prefix, (uint32_t)(sizeof(k_tsn_cyc_prefix) - 1U));
  tsn_write_u32(sts.tas_cycle);
  tsn_write(k_tsn_crlf, (uint32_t)(sizeof(k_tsn_crlf) - 1U));
  return ok;
}

/**
 * @brief Run one cycle: program TAS + CBS, then read status.
 *
 * @return True iff every shaper call in the cycle returned ``k_ra8_ok``.
 *
 * @pre ``tsn_arm`` brought up gPTP and ETHA port 0 in CONFIG mode.
 * @pre The console has been initialised.
 * @post One log block (TAS + CBS + status) has been queued.
 * @post The shaper schedule is (re)programmed and armed.
 * @since 0.1.0
 */
static bool tsn_run_cycle(void)
{
  bool ok = true;
  if (!tsn_program_tas()) {
    ok = false;
  }
  if (!tsn_program_cbs()) {
    ok = false;
  }
  if (!tsn_log_status()) {
    ok = false;
  }
  return ok;
}

/**
 * @brief Core bring-up: CGC -> MSTP -> TIME -> console + LED.
 *
 * @pre Reset defaults are in force (single-threaded boot context).
 * @pre No peripheral has been claimed yet.
 * @post On return every core clock + console + LED is live.
 * @post Any failure parks the CPU via ``tsn_panic_halt``.
 * @since 0.1.0
 */
static void tsn_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    tsn_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    tsn_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    tsn_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    tsn_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_tsn_baud) != k_ra8_ok) {
    tsn_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    tsn_panic_halt();
  }
}

/**
 * @brief Bring up the gPTP time base and ETHA port 0 in CONFIG mode.
 *
 * @details
 * ``ra8_eth_gptp_init`` starts the shared IEEE 1588 timer that the TAS
 * scheduler uses as its cycle reference; ``ra8_etha_init`` powers ETHA port 0
 * and enters CONFIG mode (initial_mode = ``k_ra8_etha_opc_config``), which is
 * the only mode in which the TAS / CBS shaper registers are writable.
 *
 * @return ``ra8_err_t`` error code from the first failing step.
 * @retval k_ra8_ok gPTP timer up and port 0 is in CONFIG mode.
 * @retval Other   Forwarded from ``ra8_eth_gptp_init`` / ``ra8_etha_init``.
 *
 * @pre ``tsn_setup_or_halt`` has ungated MSTP.
 * @pre IRQs are masked or this is single-threaded init.
 * @post On ``k_ra8_ok`` the shaper registers on port 0 are writable.
 * @post On failure no partial ETHA state is relied upon by the caller.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t tsn_arm(void)
{
  const ra8_err_t gptp_err = ra8_eth_gptp_init();
  if (gptp_err != k_ra8_ok) {
    return gptp_err;
  }
  const ra8_etha_config_t cfg = {
    .initial_mode = k_ra8_etha_opc_config,
    .eaeie0_mask  = 0U,
    .eaeie1_mask  = 0U,
    .eaeie2_mask  = 0U,
  };
  return ra8_etha_init(k_ra8_etha_port_0, &cfg);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  tsn_setup_or_halt();
  ra8_isr_globals_enable();

  if (tsn_arm() != k_ra8_ok) {
    tsn_panic_halt();
  }

  while (1) {
    const bool healthy = tsn_run_cycle();
    if (healthy) {
      tsn_write(k_tsn_verdict_pass, (uint32_t)(sizeof(k_tsn_verdict_pass) - 1U));
    } else {
      tsn_write(k_tsn_verdict_fail, (uint32_t)(sizeof(k_tsn_verdict_fail) - 1U));
    }
    if (ra8_board_led_toggle(k_ra8_board_led1) != k_ra8_ok) {
      break;
    }
    ra8_delay_ms((uint32_t)k_tsn_period_ms);
  }
  tsn_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
