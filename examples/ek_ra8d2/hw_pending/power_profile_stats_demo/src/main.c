/**
 * @file examples/ek_ra8d2/hw_pending/power_profile_stats_demo/src/main.c
 * @brief First consumer of ra8_power_profile: region accounting end to end.
 *
 * @details
 * `ra8_power_profile` keeps per-region enter/exit accounting behind two
 * caller-supplied hooks, a GPIO edge emitter and a microsecond time base, so
 * the whole library runs without a clock peripheral or a scope on a pin. This
 * app supplies a synthetic clock the test drives by hand and an edge hook that
 * records what it was asked to emit, then checks every documented behaviour:
 *
 *   1. A closed region accumulates exactly the elapsed span. Two disjoint
 *      stays in the active region add up to the sum of their spans.
 *   2. An open region (enter with no exit) reports `is_open`, keeps the
 *      unmatched timestamp in `last_enter_us`, and contributes nothing to
 *      `total_time_us`, as the header promises.
 *   3. The edge hook fires twice per closed region, once entering and once
 *      leaving, and the app checks the polarity it was handed each time.
 *   4. `ra8_power_profile_reset_stats` zeroes the accumulators while the
 *      hooks stay wired, so a fresh enter/exit pair after the reset lands on
 *      a clean slot.
 *
 * Nothing is measured off real silicon: the clock is a counter this file owns
 * and no GPIO is configured, so every leg is deterministic. A board is needed
 * only to confirm the console path.
 *
 * Observable over the SCI8 / J-Link OB VCOM console. A good run prints one
 * verdict per leg and a final `ALL PASS`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_boot_entry.h"
#include "ra8_err.h"
#include "ra8_io_log.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_uart.h"
#include "ra8_log.h"
#include "ra8_power_profile.h"
#include "ra8_sci.h"

/**
 * @enum pp_const_t
 * @brief Console and synthetic-timeline knobs (no magic numbers).
 *
 * @details The synthetic clock starts at ::k_pp_t0_us and is advanced by hand
 *          between marks, so every expected total below is arithmetic on
 *          these constants rather than a measurement.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_pp_uart_chan     = 8U,      /**< SCI8 J-Link OB console.               */
  k_pp_t0_us         = 1000U,   /**< Synthetic clock start, microseconds.  */
  k_pp_active_a_us   = 250U,    /**< First active stay.                    */
  k_pp_gap_us        = 40U,     /**< Idle gap between the two active stays. */
  k_pp_active_b_us   = 150U,    /**< Second active stay.                   */
  k_pp_sleep_us      = 900U,    /**< Closed sleep stay.                    */
  k_pp_standby_us    = 500U,    /**< Time spent inside the OPEN standby.   */
  k_pp_post_reset_us = 75U,     /**< Closed stay taken after the reset.    */
  k_pp_edges_closed  = 2U,      /**< Edge-hook calls per closed region.    */
} pp_const_t;

/**
 * @struct pp_probe_t
 * @brief Synthetic clock plus a record of what the edge hook was handed.
 *
 * @since 0.1.0
 */
typedef struct {
  uint64_t now_us;      /**< Current synthetic time, microseconds.         */
  uint32_t enter_edges; /**< Edge-hook calls that claimed to be entering.  */
  uint32_t exit_edges;  /**< Edge-hook calls that claimed to be leaving.   */
} pp_probe_t;

static pp_probe_t s_probe = {.now_us = k_pp_t0_us}; /**< The fake clock.   */

static ra8_io_stream_t            s_uart;       /**< Console stream.       */
static ra8_io_stream_uart_state_t s_uart_state; /**< Console stream state. */

/**
 * @brief Write a NUL-terminated string to the console stream.
 *
 * @param[in] text Message to queue on SCI8.
 * @return void
 * @pre The console stream was initialised.
 * @post The text was queued on the console sink.
 * @note Errors are ignored: the console reports, it does not act.
 * @since 0.1.0
 */
static void internal_print(const char* text)
{
  (void)ra8_io_stream_puts(&s_uart, text);
}

/**
 * @brief Time-base hook: hand back the synthetic clock.
 *
 * @param[in] ctx Probe state, as ::pp_probe_t.
 * @return uint64_t Current synthetic time in microseconds, 0 without a ctx.
 * @note Monotonic by construction: only ::internal_advance moves it.
 * @since 0.1.0
 */
static uint64_t internal_now_us(void* ctx)
{
  const pp_probe_t* probe = (const pp_probe_t*)ctx;

  return (probe == nullptr) ? 0U : probe->now_us;
}

/**
 * @brief Edge hook: record the polarity the profiler asked for.
 *
 * @param[in] ctx       Probe state, as ::pp_probe_t.
 * @param[in] region_id Region being entered or left.
 * @param[in] entering  True for the entering edge, false for the leaving one.
 * @return void
 * @post The matching edge counter is incremented.
 * @note No GPIO is configured; this app only counts the calls.
 * @since 0.1.0
 */
static void internal_pulse(void* ctx, ra8_power_profile_region_id_t region_id, bool entering)
{
  pp_probe_t* probe = (pp_probe_t*)ctx;

  (void)region_id;

  if (probe == nullptr) {
    return;
  }

  if (entering) {
    probe->enter_edges += 1U;
  } else {
    probe->exit_edges += 1U;
  }
}

/**
 * @brief Move the synthetic clock forward.
 *
 * @param[in] delta_us Microseconds to add.
 * @return void
 * @post The clock reads @p delta_us later than before.
 * @since 0.1.0
 */
static void internal_advance(uint32_t delta_us)
{
  s_probe.now_us += (uint64_t)delta_us;
}

/**
 * @brief Mark a closed region that spans @p span_us on the synthetic clock.
 *
 * @param[in] region  Region to enter and leave.
 * @param[in] span_us Microseconds to spend inside it.
 * @return ra8_err_t Error code from the profiler, ::k_ra8_ok on success.
 * @post The clock has advanced by @p span_us.
 * @since 0.1.0
 */
static ra8_err_t internal_closed_stay(ra8_power_profile_region_id_t region, uint32_t span_us)
{
  ra8_err_t err = ra8_power_profile_mark_enter(region);
  if (err != k_ra8_ok) {
    return err;
  }

  internal_advance(span_us);
  err = ra8_power_profile_mark_exit(region);
  return err;
}

/**
 * @brief Wire the hooks and walk the synthetic timeline.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Every mark was accepted.
 * @post Active holds two closed stays, sleep one, standby one still open.
 * @since 0.1.0
 */
static ra8_err_t internal_walk_timeline(void)
{
  const ra8_power_profile_config_t cfg = {
      .pulse         = internal_pulse,
      .now_us        = internal_now_us,
      .user_ctx_gpio = &s_probe,
      .user_ctx_time = &s_probe,
  };

  ra8_err_t err = ra8_power_profile_init(&cfg);
  if (err != k_ra8_ok) {
    return err;
  }

  err = internal_closed_stay(k_ra8_power_profile_region_active, (uint32_t)k_pp_active_a_us);
  if (err != k_ra8_ok) {
    return err;
  }

  internal_advance((uint32_t)k_pp_gap_us);

  err = internal_closed_stay(k_ra8_power_profile_region_active, (uint32_t)k_pp_active_b_us);
  if (err != k_ra8_ok) {
    return err;
  }

  err = internal_closed_stay(k_ra8_power_profile_region_sleep, (uint32_t)k_pp_sleep_us);
  if (err != k_ra8_ok) {
    return err;
  }

  /* Deliberately left open: no matching exit, so it must not accrue time. */
  err = ra8_power_profile_mark_enter(k_ra8_power_profile_region_software_standby);
  if (err != k_ra8_ok) {
    return err;
  }

  internal_advance((uint32_t)k_pp_standby_us);
  return k_ra8_ok;
}

/**
 * @brief Check the snapshot against the synthetic timeline.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Totals, counts, and open state all agree.
 * @retval k_ra8_err_invalid_arg A total or a flag disagreed.
 * @since 0.1.0
 */
static ra8_err_t internal_check_stats(void)
{
  ra8_power_profile_stats_t stats = {0};

  const ra8_err_t err = ra8_power_profile_get_stats(&stats);
  if (err != k_ra8_ok) {
    return err;
  }

  const ra8_power_profile_region_stats_t* active
      = &stats.regions[k_ra8_power_profile_region_active];
  const ra8_power_profile_region_stats_t* sleep
      = &stats.regions[k_ra8_power_profile_region_sleep];
  const ra8_power_profile_region_stats_t* standby
      = &stats.regions[k_ra8_power_profile_region_software_standby];

  const uint64_t want_active = (uint64_t)k_pp_active_a_us + (uint64_t)k_pp_active_b_us;
  const uint64_t want_open_at
      = (uint64_t)k_pp_t0_us + want_active + (uint64_t)k_pp_gap_us + (uint64_t)k_pp_sleep_us;

  const bool active_ok = (active->entries == 2U) && (active->exits == 2U)
                         && (active->total_time_us == want_active) && !active->is_open;
  const bool sleep_ok = (sleep->entries == 1U) && (sleep->exits == 1U)
                        && (sleep->total_time_us == (uint64_t)k_pp_sleep_us) && !sleep->is_open;
  const bool standby_ok = (standby->entries == 1U) && (standby->exits == 0U)
                          && (standby->total_time_us == 0U) && standby->is_open
                          && (standby->last_enter_us == want_open_at);

  return (active_ok && sleep_ok && standby_ok) ? k_ra8_ok : k_ra8_err_invalid_arg;
}

/**
 * @brief Check the edge hook fired twice per closed region and nothing else.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Edge counts match the timeline.
 * @retval k_ra8_err_invalid_arg An edge was missing or spurious.
 * @since 0.1.0
 */
static ra8_err_t internal_check_edges(void)
{
  const uint32_t closed_regions = 3U; /* active twice plus sleep once. */
  const uint32_t want_exits     = closed_regions;
  const uint32_t want_enters    = closed_regions + 1U; /* the open standby. */

  const bool counted = (s_probe.enter_edges == want_enters)
                       && (s_probe.exit_edges == want_exits)
                       && ((want_enters + want_exits)
                           == ((closed_regions * (uint32_t)k_pp_edges_closed) + 1U));

  return counted ? k_ra8_ok : k_ra8_err_invalid_arg;
}

/**
 * @brief Reset the accumulators and confirm a fresh pair lands clean.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              The reset cleared history, hooks still live.
 * @retval k_ra8_err_invalid_arg Stale history survived the reset.
 * @since 0.1.0
 */
static ra8_err_t internal_check_reset(void)
{
  ra8_err_t err = ra8_power_profile_reset_stats();
  if (err != k_ra8_ok) {
    return err;
  }

  ra8_power_profile_stats_t cleared = {0};
  err                               = ra8_power_profile_get_stats(&cleared);
  if (err != k_ra8_ok) {
    return err;
  }

  const ra8_power_profile_region_stats_t* was_active
      = &cleared.regions[k_ra8_power_profile_region_active];
  if ((was_active->entries != 0U) || (was_active->total_time_us != 0U)) {
    return k_ra8_err_invalid_arg;
  }

  err = internal_closed_stay(k_ra8_power_profile_region_active, (uint32_t)k_pp_post_reset_us);
  if (err != k_ra8_ok) {
    return err;
  }

  ra8_power_profile_stats_t again = {0};
  err                             = ra8_power_profile_get_stats(&again);
  if (err != k_ra8_ok) {
    return err;
  }

  const ra8_power_profile_region_stats_t* fresh
      = &again.regions[k_ra8_power_profile_region_active];
  const bool clean = (fresh->entries == 1U) && (fresh->exits == 1U)
                     && (fresh->total_time_us == (uint64_t)k_pp_post_reset_us);

  return clean ? k_ra8_ok : k_ra8_err_invalid_arg;
}

/**
 * @brief Entry point: walk the timeline and print one verdict per leg.
 *
 * @return void
 * @pre SystemInit configured VTOR / FPU / priority grouping.
 * @post A verdict per leg and a final summary are queued on SCI8.
 * @post Control parks in an infinite loop; the function never returns.
 * @note Single-threaded; runs to the park loop on the main stack.
 * @since 0.1.0
 */
void main(void)
{
  ra8_log_init();
  (void)ra8_io_stream_uart_init(&s_uart, &s_uart_state, (uint8_t)k_pp_uart_chan);
  (void)ra8_io_log_attach(&s_uart);
  internal_print("power_profile_stats_demo: boot\r\n");

  bool pass = true;

  if (internal_walk_timeline() == k_ra8_ok) {
    internal_print("power_profile_stats_demo: timeline PASS\r\n");
  } else {
    internal_print("power_profile_stats_demo: timeline FAIL\r\n");
    pass = false;
  }

  if (internal_check_stats() == k_ra8_ok) {
    internal_print("power_profile_stats_demo: accounting PASS\r\n");
  } else {
    internal_print("power_profile_stats_demo: accounting FAIL\r\n");
    pass = false;
  }

  if (internal_check_edges() == k_ra8_ok) {
    internal_print("power_profile_stats_demo: edges PASS\r\n");
  } else {
    internal_print("power_profile_stats_demo: edges FAIL\r\n");
    pass = false;
  }

  if (internal_check_reset() == k_ra8_ok) {
    internal_print("power_profile_stats_demo: reset PASS\r\n");
  } else {
    internal_print("power_profile_stats_demo: reset FAIL\r\n");
    pass = false;
  }

  internal_print(pass ? "power_profile_stats_demo: ALL PASS\r\n"
                      : "power_profile_stats_demo: ALL FAIL\r\n");

  (void)ra8_sci_flush((uint8_t)k_pp_uart_chan);
  while (true) {
  }
}
