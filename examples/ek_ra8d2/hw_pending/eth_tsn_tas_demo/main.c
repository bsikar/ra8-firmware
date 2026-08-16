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
 * starts that counter for real first -- ESWCLK up, ``PTPTIVCt`` derived from
 * the live frequency, timer unit enabled -- then:
 *
 *   1. ``ra8_etha_init`` on port 0 in CONFIG mode (shaper registers are only
 *      writable in CONFIG).
 *   2. ``ra8_etha_tas_ram_reset``, then ``ra8_etha_set_tas_schedule`` with a
 *      2-entry gate list on descriptor queue 7 (PTP / control): one window
 *      open, one shut. Every entry is read back out of the TAS RAM with
 *      ``ra8_etha_read_tas_entry`` and compared. ``ra8_etha_enable_tas``
 *      arms the scheduler.
 *   3. ``ra8_etha_configure_cbs`` on traffic class 2 (AVB class A) with an
 *      illustrative credit increment + upper limit, then
 *      ``ra8_etha_get_cbs_state`` reads the oper-side mirror back.
 *   4. ``ra8_etha_get_status`` reads the TAS cycle-time monitor.
 *
 * @warning **What the verdict does and does not prove.** ``"tsn: schedule
 * PASS"`` requires three things: that the gPTP time base advanced by the
 * measured wall-clock interval (the counter is read twice around a
 * SysTick-timed window), that every TAS entry read back out of the TAS RAM
 * matches what was written to it, and that every shaper call returned
 * ``k_ra8_ok``. The first two are hardware assertions. The third only proves
 * the arguments were accepted: ETHA stays in CONFIG mode here, so no frame is
 * ever transmitted and nothing about *shaped egress* is measured. That needs
 * a multi-node measurement rig (bench wiring #89).
 *
 * The read-back was added with #539, when the driver was found to be writing
 * gate states into ``EATASGL0``, whose field is the TAS RAM entry ADDRESS.
 * Every call returned ``k_ra8_ok`` throughout, which is precisely why a
 * verdict built only out of return codes was worth nothing.
 *
 * hw_pending: ``tools/ra8_emulator`` now models the HUM Ch 35 GPTP timer
 * (``board_periph_gptp.c``), so the gPTP time-base half of this verdict runs
 * under emulation (the counter advances by the SysTick-measured window). The
 * ETHA shaper half does not: the TAS gate RAM learn/read (``EATASGL*`` /
 * ``EATASGR*``, HUM Ch 32) and the CBS state are indirect-RAM registers the
 * emulator leaves to config-reflect, so ``ra8_etha_read_tas_entry`` reads back
 * zero and the entry-match check fails -- modelling that RAM is #539 / #292
 * territory. The EK-RA8D2 Ethernet wire is also marginal (#21). So the whole-app
 * verdict is compile-gated in CI and asserted on the bench, while
 * ``eth_gptp_timestamp_demo`` carries the emulator-gated counter-advance check.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_boot_entry.h"
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
  k_tsn_win_units     = 125000U, /**< Per-window gate time, nanoseconds.     */
  k_tsn_cycle_units   = 250000U, /**< Full GCL cycle = 2 windows, ns.        */
  k_tsn_cbs_increment = 1500U,   /**< CBS credit increment (20-bit field).   */
  k_tsn_cbs_upper_lim = 65536U,  /**< CBS upper credit limit (31-bit field). */
} tsn_const_t;

/** @brief Formatting + iteration constants. */
typedef enum : uint8_t {
  k_tsn_radix        = 10U, /**< Decimal serialiser radix.          */
  k_tsn_dec_u32_max  = 10U, /**< Max decimal digits for a uint32_t. */
  k_tsn_dec_u64_max  = 20U, /**< Max decimal digits for a uint64_t. */
  k_tsn_gate_entries = 2U,  /**< Gate-control-list entry count.     */
} tsn_fmt_t;

/** @brief gPTP time-base check parameters. */
typedef enum : uint32_t {
  k_tsn_gptp_window_ms = 200U,       /**< SysTick-timed measurement window. */
  k_tsn_ns_per_ms      = 1000000U,   /**< Nanoseconds in one millisecond.   */
  k_tsn_ns_per_sec     = 1000000000U /**< Nanoseconds in one second.        */
} tsn_gptp_const_t;

/**
 * @brief Largest seconds value that can be flattened to nanoseconds.
 *
 * @details
 * ``UINT64_MAX / 1e9``: above this, ``sec * 1e9`` wraps.
 */
typedef enum : uint64_t {
  k_tsn_sec_flatten_max = 18446744073ULL, /**< floor(2^64 - 1 / 1e9). */
} tsn_flatten_limit_t;

/** @brief Accuracy band the gPTP advance must fall inside. */
typedef enum : uint8_t {
  k_tsn_tolerance_pct = 10U,  /**< Allowed drift vs SysTick, in percent. */
  k_tsn_pct_full      = 100U, /**< Denominator for the percentage above. */
} tsn_band_t;

/**
 * @brief EATASIGSC initial gate-state bitmap: bit q is queue q's gate.
 */
typedef enum : uint8_t {
  k_tsn_gate_ptp_only = 0x80U, /**< Only queue 7 (PTP / control) starts open. */
} tsn_gate_t;

/* Console line fragments (kept short so each write is one shift-register
 * fill; the periodic log is the only output path). */
static const uint8_t k_tsn_tas_prefix[]   = "tsn: tas_entries=";
static const uint8_t k_tsn_cbs_prefix[]   = "tsn: cbs_en=";
static const uint8_t k_tsn_cbs_gate_sep[] = " gate=";
static const uint8_t k_tsn_cyc_prefix[]   = "tsn: tas_cycle=";
static const uint8_t k_tsn_base_prefix[]  = "tsn: gptp_adv_ns=";
static const uint8_t k_tsn_base_sep[]     = " sys_ms=";
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
 * @brief Log one unsigned 64-bit value as decimal ASCII.
 *
 * @details
 * A standalone serialiser rather than a split-at-10^9 pair of
 * ``tsn_write_u32`` calls: the low half would need zero-padding to nine
 * digits, and an unpadded join silently renders 1000000005 as "15".
 *
 * @param[in] val Value to print.
 *
 * @pre The console has been initialised.
 * @pre ``val`` fits in a uint64_t (always true).
 * @post The decimal digits of ``val`` have been queued to the console.
 * @post Neither scratch buffer outlives the call.
 *
 * @note Not thread-safe; single-threaded demo loop.
 * @since 0.1.0
 */
static void tsn_write_u64(uint64_t val)
{
  uint8_t  buf[k_tsn_dec_u64_max];
  uint8_t  tmp[k_tsn_dec_u64_max];
  uint32_t n = 0U;
  uint64_t v = val;
  if (v == 0U) {
    buf[0] = (uint8_t)'0';
    tsn_write(buf, 1U);
    return;
  }
  while (v != 0U) {
    tmp[n] = (uint8_t)('0' + (uint8_t)(v % (uint64_t)k_tsn_radix));
    v      = v / (uint64_t)k_tsn_radix;
    n++;
  }
  for (uint32_t i = 0U; i < n; i++) {
    buf[i] = tmp[n - 1U - i];
  }
  tsn_write(buf, n);
}

/**
 * @brief Flatten a GPTP sample to nanoseconds.
 *
 * @details
 * The seconds field is 48 bits wide, so ``sec * 1e9`` would wrap a uint64_t
 * above ``sec ~= 1.8e10``. That value comes straight off a hardware register,
 * so an implausible reading is clamped rather than silently wrapped into a
 * small number that could look like a healthy advance.
 *
 * @param[in] sec  Seconds field.
 * @param[in] nsec Nanoseconds field.
 *
 * @return The sample as a nanosecond count, or ``UINT64_MAX`` if ``sec`` is
 *         too large to flatten.
 *
 * @pre ``nsec`` is below one second.
 * @pre ``sec`` came from ``ra8_eth_gptp_get_time`` (48-bit range).
 * @post The result is monotonic in ``sec`` and ``nsec``.
 * @post No hardware was touched.
 *
 * @note Pure and re-entrant.
 * @since 0.1.0
 */
static uint64_t tsn_flatten_ns(uint64_t sec, uint32_t nsec)
{
  if (sec > (uint64_t)k_tsn_sec_flatten_max) {
    return UINT64_MAX;
  }
  return (sec * (uint64_t)k_tsn_ns_per_sec) + (uint64_t)nsec;
}

/**
 * @brief Verify the gPTP time base the TAS scheduler references is running.
 *
 * @details
 * Samples the 78-bit GPTP counter either side of a SysTick-timed window and
 * requires the advance to match to within ``k_tsn_tolerance_pct``. Without
 * this the app would program a gate-control list against a time base that
 * never started, which is exactly the defect issue #498 records.
 *
 * @return True iff the counter advanced by the measured interval.
 * @retval true  The advance matched SysTick inside the tolerance band.
 * @retval false A read failed, or the counter did not track SysTick.
 *
 * @pre ``tsn_arm`` enabled gPTP timer unit 0.
 * @pre The console has been initialised.
 * @post One ``tsn: gptp_adv_ns=`` line has been queued.
 * @post No gPTP or ETHA configuration was changed.
 *
 * @note Not thread-safe; single-threaded demo loop.
 * @since 0.1.0
 */
static bool tsn_check_time_base(void)
{
  uint64_t sec0  = 0U;
  uint32_t nsec0 = 0U;
  uint64_t sec1  = 0U;
  uint32_t nsec1 = 0U;
  bool     ok    = true;

  const uint32_t ms0 = ra8_time_ms();
  if (ra8_eth_gptp_get_time(k_ra8_gptp_timer_0, &sec0, &nsec0) != k_ra8_ok) {
    ok = false;
  }
  ra8_delay_ms((uint32_t)k_tsn_gptp_window_ms);
  if (ra8_eth_gptp_get_time(k_ra8_gptp_timer_0, &sec1, &nsec1) != k_ra8_ok) {
    ok = false;
  }
  const uint32_t elapsed_ms  = ra8_time_ms() - ms0;
  const uint64_t ns0         = tsn_flatten_ns(sec0, nsec0);
  const uint64_t ns1         = tsn_flatten_ns(sec1, nsec1);
  const uint64_t advance_ns  = (ns1 > ns0) ? (ns1 - ns0) : 0ULL;
  const uint64_t expected_ns = (uint64_t)elapsed_ms * (uint64_t)k_tsn_ns_per_ms;
  const uint64_t band_ns = (expected_ns * (uint64_t)k_tsn_tolerance_pct) / (uint64_t)k_tsn_pct_full;

  tsn_write(k_tsn_base_prefix, (uint32_t)(sizeof(k_tsn_base_prefix) - 1U));
  tsn_write_u64(advance_ns);
  tsn_write(k_tsn_base_sep, (uint32_t)(sizeof(k_tsn_base_sep) - 1U));
  tsn_write_u32(elapsed_ms);
  tsn_write(k_tsn_crlf, (uint32_t)(sizeof(k_tsn_crlf) - 1U));

  if (elapsed_ms == 0U) {
    return false;
  }
  if (advance_ns > (expected_ns + band_ns)) {
    ok = false;
  }
  if (advance_ns < (expected_ns - band_ns)) {
    ok = false;
  }
  return ok;
}

/**
 * @brief Read one TAS RAM entry back and compare it with what was programmed.
 *
 * @details
 * The read-back half of the TAS verdict. Split into its own predicate so the
 * caller has a single failure branch: a read error and a value mismatch are
 * the same outcome to the demo, and expressing them as two branches with
 * identical bodies is what clang-tidy's bugprone-branch-clone objects to.
 *
 * @param[in] index TAS RAM entry address to read.
 * @param[in] want  The entry that was programmed at that address.
 *
 * @return True iff the read succeeded and both fields match.
 *
 * @pre ``ra8_etha_set_tas_schedule`` has programmed ``index``.
 * @pre ``want`` points at the entry originally written there.
 * @post No hardware state is changed beyond EATASGR.
 * @since 0.1.0
 */
static bool tsn_tas_entry_matches(uint8_t index, const ra8_etha_tas_entry_t* want)
{
  ra8_etha_tas_entry_t got = {};
  if (ra8_etha_read_tas_entry(k_ra8_etha_port_0, index, &got) != k_ra8_ok) {
    return false;
  }
  if (got.gate_time_ns != want->gate_time_ns) {
    return false;
  }
  return got.gate_open == want->gate_open;
}

/**
 * @brief Program + arm the 802.1Qbv time-aware shaper on port 0.
 *
 * @details
 * Resets the TAS RAM, then gives descriptor queue 7 (PTP / control) a
 * two-entry gate list -- one window open, one shut -- and commits it via
 * ``ra8_etha_set_tas_schedule``. The gate list belongs to a QUEUE rather
 * than being an interleaved per-class vector, because a TAS entry on this
 * silicon carries a single gate-state bit for the queue whose TAS RAM block
 * holds it (HUM Table 32.6 p 1691).
 *
 * Every entry is then read back out of the TAS RAM with
 * ``ra8_etha_read_tas_entry`` and compared against what was programmed.
 * That read-back is the only part of this function that is evidence: the
 * programming calls returned ``k_ra8_ok`` even when the driver was writing
 * gate states into the entry-address register (#539).
 *
 * @return True iff the schedule was committed AND read back byte-identical.
 *
 * @pre ``ra8_etha_init`` left port 0 in CONFIG mode.
 * @pre The gPTP time base is running (``ra8_eth_gptp_init``).
 * @post One ``tsn: tas_entries=`` line has been queued.
 * @post On success EATASC.TASE is set (scheduler armed).
 * @since 0.1.0
 */
static bool tsn_program_tas(void)
{
  bool                              ok                          = true;
  static const ra8_etha_tas_entry_t entries[k_tsn_gate_entries] = {
    {.gate_time_ns = (uint32_t)k_tsn_win_units, .gate_open = true},
    {.gate_time_ns = (uint32_t)k_tsn_win_units, .gate_open = false},
  };
  ra8_etha_tas_queue_t queues[k_ra8_etha_tc_count] = {};
  queues[k_ra8_etha_tc_7].entries                  = entries;
  queues[k_ra8_etha_tc_7].count                    = (uint16_t)k_tsn_gate_entries;

  if (ra8_etha_tas_ram_reset(k_ra8_etha_port_0) != k_ra8_ok) {
    ok = false;
  }
  const ra8_err_t sched_err = ra8_etha_set_tas_schedule(k_ra8_etha_port_0,
                                                        queues,
                                                        (uint8_t)k_tsn_gate_ptp_only,
                                                        (uint32_t)k_tsn_cycle_units,
                                                        0U);
  if (sched_err != k_ra8_ok) {
    ok = false;
  }
  /* Read every entry back: this is what makes the verdict mean something. */
  for (uint8_t index = 0U; index < (uint8_t)k_tsn_gate_entries; ++index) {
    if (!tsn_tas_entry_matches(index, &entries[index])) {
      ok = false;
    }
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
 * @brief Run one cycle: check the time base, program TAS + CBS, read status.
 *
 * @return True iff the gPTP counter advanced and every shaper call returned
 *         ``k_ra8_ok``.
 *
 * @pre ``tsn_arm`` started gPTP and left ETHA port 0 in CONFIG mode.
 * @pre The console has been initialised.
 * @post One log block (time base + TAS + CBS + status) has been queued.
 * @post The shaper schedule is (re)programmed and armed.
 * @since 0.1.0
 */
static bool tsn_run_cycle(void)
{
  bool ok = true;
  if (!tsn_check_time_base()) {
    ok = false;
  }
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
  if (ra8_cgc_eswclk_init() != k_ra8_ok) {
    tsn_panic_halt();
  }
}

/**
 * @brief Start the gPTP time base the TAS scheduler references.
 *
 * @details
 * Reads the live ESWCLK frequency, hands it to ``ra8_eth_gptp_init`` (which
 * derives ``PTPTIVCt`` from it), then enables the timer unit so the counter
 * actually advances.
 *
 * @return ``ra8_err_t`` error code from the first failing step.
 * @retval k_ra8_ok The gPTP counter is programmed and running.
 * @retval Other    Forwarded from the failing driver call.
 *
 * @pre ``tsn_setup_or_halt`` has started ESWCLK and ungated MSTP.
 * @pre IRQs are masked or this is single-threaded init.
 * @post On ``k_ra8_ok`` timer unit 0 is counting.
 * @post On failure no partial gPTP state is relied upon by the caller.
 *
 * @note Boot-time only; not re-entrant.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t tsn_arm_time_base(void)
{
  uint32_t        eswclk_hz = 0U;
  const ra8_err_t hz_err    = ra8_cgc_eswclk_hz(&eswclk_hz);
  if (hz_err != k_ra8_ok) {
    return hz_err;
  }
  const ra8_eth_gptp_cfg_t cfg      = {.clk_hz = eswclk_hz};
  const ra8_err_t          init_err = ra8_eth_gptp_init(&cfg);
  if (init_err != k_ra8_ok) {
    return init_err;
  }
  return ra8_eth_gptp_timer_enable(k_ra8_gptp_timer_0);
}

/**
 * @brief Bring up the gPTP time base and ETHA port 0 in CONFIG mode.
 *
 * @details
 * ``tsn_arm_time_base`` starts the GPTP counter that the TAS scheduler uses
 * as its cycle reference; ``ra8_etha_init`` powers ETHA port 0 and enters
 * CONFIG mode (initial_mode = ``k_ra8_etha_opc_config``), which is the only
 * mode in which the TAS / CBS shaper registers are writable.
 *
 * @return ``ra8_err_t`` error code from the first failing step.
 * @retval k_ra8_ok gPTP timer running and port 0 is in CONFIG mode.
 * @retval Other   Forwarded from ``tsn_arm_time_base`` / ``ra8_etha_init``.
 *
 * @pre ``tsn_setup_or_halt`` has ungated MSTP.
 * @pre IRQs are masked or this is single-threaded init.
 * @post On ``k_ra8_ok`` the shaper registers on port 0 are writable.
 * @post On failure no partial ETHA state is relied upon by the caller.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t tsn_arm(void)
{
  const ra8_err_t gptp_err = tsn_arm_time_base();
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

void main(void)
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
}
