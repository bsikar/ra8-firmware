/**
 * @file examples/ek_ra8d2/hw_validated/hil/wdt_supervisor_demo/main.c
 * @brief WDT supervisor abstraction HIL demo for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Second of three apps under #18. Exercises ``libs/ra8_wdt_supervisor``
 * end-to-end on real hardware: the supervisor refreshes the WWDT only
 * when every registered worker thread has called
 * ``ra8_wdt_supervisor_checkin`` inside its deadline. Two synthetic
 * worker threads (a fast 100 ms one and a slow 250 ms one) check in
 * on disjoint cadences; the supervisor thread (auto-spawned by
 * ``ra8_wdt_supervisor_start``) ticks every 50 ms and decides whether
 * to refresh the underlying WWDT.
 *
 * Liveness counter ``g_wdt_supervisor_demo_tick`` is incremented by
 * worker A each time it checks in; ``hil_jlink_memprobe.sh`` asserts
 * the counter advances over the bench sample window.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_boot_entry.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_wdt.h"
#include "ra8_wdt_supervisor.h"
#include "tx_api.h"

/** @brief Demo tunables. */
typedef enum : uint32_t {
  k_wdt_sup_demo_stack_bytes          = 1024U, /**< Wdt sup demo stack bytes.          */
  k_wdt_sup_demo_priority             = 5U,    /**< Wdt sup demo priority.             */
  k_wdt_sup_demo_worker_a_period_ms   = 100U,  /**< Wdt sup demo worker a period ms.   */
  k_wdt_sup_demo_worker_b_period_ms   = 250U,  /**< Wdt sup demo worker b period ms.   */
  k_wdt_sup_demo_worker_a_deadline_ms = 500U,  /**< Wdt sup demo worker a deadline ms. */
  k_wdt_sup_demo_worker_b_deadline_ms = 750U,  /**< Wdt sup demo worker b deadline ms. */
  k_wdt_sup_demo_sup_period_ms        = 50U,   /**< Wdt sup demo sup period ms.        */
} wdt_sup_demo_const_t;

[[gnu::aligned(8)]] static uint8_t s_thread_a_stack[k_wdt_sup_demo_stack_bytes];
[[gnu::aligned(8)]] static uint8_t s_thread_b_stack[k_wdt_sup_demo_stack_bytes];
[[gnu::aligned(8)]] static uint8_t s_sup_stack[k_wdt_sup_demo_stack_bytes];
static TX_THREAD                   s_thread_a;
static TX_THREAD                   s_thread_b;
static uint8_t                     s_handle_a = 0U;
static uint8_t                     s_handle_b = 0U;

/**
 * @var g_wdt_supervisor_demo_tick
 * @brief Worker-A check-in counter, read via J-Link memprobe.
 *
 * @details Worker A bumps this every iteration after a successful
 * ``ra8_wdt_supervisor_checkin``. The probe asserts the counter
 * advances >= 3 over a 4 s window -- catches "ThreadX wedged",
 * "supervisor failed to spawn", and "worker thread crashed before
 * first iteration".
 *
 * @note Read externally by J-Link; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_wdt_supervisor_demo_tick = 0U;

/**
 * @var g_wdt_supervisor_demo_step
 * @brief Bring-up step tracker, read via J-Link memprobe.
 *
 * @details Stamped at every bring-up checkpoint so a halt in
 * wdt_sup_demo_halt can be traced back to its origin: 1 = wdt_init
 * done, 2 = supervisor_init done, 3 = register_a done, 4 = register_b
 * done, 5 = supervisor_start done, 6 = thread A created, 7 = thread B
 * created. Negative-stamped (with the high bit set) on the failing
 * step.
 *
 * @note Read externally by J-Link.
 * @since 0.1.0
 */
/** @brief Bring-up step codes for ::g_wdt_supervisor_demo_step (memprobe). */
typedef enum : uint32_t {
  k_wdt_sup_step_start               = 0U, /**< Before init.               */
  k_wdt_sup_step_wdt_init            = 1U, /**< WDT initialised.           */
  k_wdt_sup_step_supervisor_init     = 2U, /**< Supervisor initialised.    */
  k_wdt_sup_step_worker_a_registered = 3U, /**< worker_a registered.       */
  k_wdt_sup_step_worker_b_registered = 4U, /**< worker_b registered.       */
  k_wdt_sup_step_supervisor_started  = 5U, /**< Supervisor started (done). */
} wdt_sup_step_t;

volatile uint32_t g_wdt_supervisor_demo_step = k_wdt_sup_step_start;

/**
 * @var g_wdt_supervisor_demo_last_err
 * @brief Captures the most recent ra8_err_t / TX_SUCCESS value.
 *
 * @details Stamped right after every bring-up call, so JLink can
 * read the actual error code returned on a halt.
 *
 * @note Read externally by J-Link.
 * @since 0.1.0
 */
/** @brief Sentinel for ::g_wdt_supervisor_demo_last_err ("none yet"). */
typedef enum : uint32_t {
  k_wdt_sup_err_none = 0xFFFFFFFFU, /**< Wdt sup error none. */
} wdt_sup_err_sentinel_t;

volatile uint32_t g_wdt_supervisor_demo_last_err = k_wdt_sup_err_none;

static void wdt_sup_demo_bring_up(void);

/**
 * @brief Worker A entry. 100 ms loop, checks in each iteration.
 *
 * @param[in] arg Unused.
 *
 * @pre Supervisor registered ``s_handle_a``.
 * @pre ThreadX scheduler is running.
 * @post Loops forever; never returns.
 * @post ``g_wdt_supervisor_demo_tick`` advances 1:1 with check-ins.
 *
 * @note Single thread.
 * @since 0.1.0
 */
static void worker_a_entry(ULONG arg)
{
  (void)arg;
  /* Supervisor bring-up runs from worker A's first action -- the
   * supervisor's register / start APIs need a real thread context
   * (tx_mutex_get returns NOT_SYSTEM from tx_application_define). */
  wdt_sup_demo_bring_up();
  while (1) {
    if (ra8_wdt_supervisor_checkin(s_handle_a) == k_ra8_ok) {
      g_wdt_supervisor_demo_tick += 1U;
    }
    (void)tx_thread_sleep((ULONG)k_wdt_sup_demo_worker_a_period_ms);
  }
}

/**
 * @brief Worker B entry. 250 ms loop, checks in each iteration.
 *
 * @param[in] arg Unused.
 *
 * @pre Supervisor registered ``s_handle_b``.
 * @pre ThreadX scheduler is running.
 * @post Loops forever; never returns.
 * @post Supervisor sees B alive each cycle.
 *
 * @note Single thread.
 * @since 0.1.0
 */
static void worker_b_entry(ULONG arg)
{
  (void)arg;
  /* Wait briefly for worker A to finish supervisor bring-up before
   * we start spamming checkin calls (which would just return
   * k_ra8_err_not_initialized until A is done). */
  (void)tx_thread_sleep((ULONG)k_wdt_sup_demo_worker_b_period_ms);
  while (1) {
    (void)ra8_wdt_supervisor_checkin(s_handle_b);
    (void)tx_thread_sleep((ULONG)k_wdt_sup_demo_worker_b_period_ms);
  }
}

static void wdt_sup_demo_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring up WWDT, supervisor, and two registered worker threads.
 *
 * @pre tx_application_define context (scheduler not yet running).
 * @pre WWDT not yet running.
 * @post Two worker threads registered + auto-started.
 * @post Supervisor thread running.
 *
 * @note Only ever called once.
 * @since 0.1.0
 */
static void wdt_sup_demo_bring_up(void)
{
  /* Bring up the underlying WWDT first -- supervisor's refresh hook
   * calls ra8_wdt_refresh_deferred. NMI on expiry so a supervisor
   * bug surfaces as a missing-tick rather than a reset loop. The
   * 25..75 % window restriction is intentionally disabled for the
   * supervisor demo (window_start=100, window_end=0) -- the
   * supervisor's refresh cadence is on the 50 ms supervisor tick,
   * not synchronized to the WWDT counter, so an artificial window
   * would just look like a flaky test. */
  const ra8_wdt_cfg_t wdt_cfg = {
    .timeout       = k_ra8_wdt_timeout_1024,
    .clock_div     = k_ra8_wdt_clkdiv_4,
    .window_start  = k_ra8_wdt_window_start_100,
    .window_end    = k_ra8_wdt_window_end_0,
    .on_expiry     = k_ra8_wdt_on_expiry_nmi,
    .stop_in_sleep = k_ra8_wdt_sleep_stop_count,
  };
  ra8_err_t err                  = ra8_wdt_init(&wdt_cfg);
  g_wdt_supervisor_demo_last_err = (uint32_t)err;
  if (err != k_ra8_ok) {
    wdt_sup_demo_halt();
  }
  g_wdt_supervisor_demo_step      = k_wdt_sup_step_wdt_init;
  const ra8_wdt_sup_cfg_t sup_cfg = {
    .stack             = s_sup_stack,
    .stack_size_bytes  = (uint32_t)k_wdt_sup_demo_stack_bytes,
    .priority          = (uint32_t)k_wdt_sup_demo_priority + 1U,
    .refresh_period_ms = (uint32_t)k_wdt_sup_demo_sup_period_ms,
  };
  err                            = ra8_wdt_supervisor_init(&sup_cfg);
  g_wdt_supervisor_demo_last_err = (uint32_t)err;
  if (err != k_ra8_ok) {
    wdt_sup_demo_halt();
  }
  g_wdt_supervisor_demo_step = k_wdt_sup_step_supervisor_init;
  err = ra8_wdt_supervisor_register_thread("worker_a",
                                           (uint32_t)k_wdt_sup_demo_worker_a_deadline_ms,
                                           &s_handle_a);
  g_wdt_supervisor_demo_last_err = (uint32_t)err;
  if (err != k_ra8_ok) {
    wdt_sup_demo_halt();
  }
  g_wdt_supervisor_demo_step = k_wdt_sup_step_worker_a_registered;
  err = ra8_wdt_supervisor_register_thread("worker_b",
                                           (uint32_t)k_wdt_sup_demo_worker_b_deadline_ms,
                                           &s_handle_b);
  g_wdt_supervisor_demo_last_err = (uint32_t)err;
  if (err != k_ra8_ok) {
    wdt_sup_demo_halt();
  }
  g_wdt_supervisor_demo_step     = k_wdt_sup_step_worker_b_registered;
  err                            = ra8_wdt_supervisor_start();
  g_wdt_supervisor_demo_last_err = (uint32_t)err;
  if (err != k_ra8_ok) {
    wdt_sup_demo_halt();
  }
  g_wdt_supervisor_demo_step = k_wdt_sup_step_supervisor_started;
}

/**
 * @brief Spawn the two worker threads after the supervisor is up.
 *
 * @pre tx_application_define context.
 * @pre wdt_sup_demo_bring_up has succeeded.
 * @post s_thread_a and s_thread_b are auto-started.
 * @post Halts on any thread-create failure.
 *
 * @note Only ever called once.
 * @since 0.1.0
 */
static void wdt_sup_demo_spawn_workers(void)
{
  UINT err = tx_thread_create(&s_thread_a,
                              (CHAR*)"wdt_sup_a",
                              worker_a_entry,
                              0U,
                              s_thread_a_stack,
                              (ULONG)k_wdt_sup_demo_stack_bytes,
                              (UINT)k_wdt_sup_demo_priority,
                              (UINT)k_wdt_sup_demo_priority,
                              TX_NO_TIME_SLICE,
                              TX_AUTO_START);
  if (err != TX_SUCCESS) {
    wdt_sup_demo_halt();
  }
  err = tx_thread_create(&s_thread_b,
                         (CHAR*)"wdt_sup_b",
                         worker_b_entry,
                         0U,
                         s_thread_b_stack,
                         (ULONG)k_wdt_sup_demo_stack_bytes,
                         (UINT)k_wdt_sup_demo_priority,
                         (UINT)k_wdt_sup_demo_priority,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
  if (err != TX_SUCCESS) {
    wdt_sup_demo_halt();
  }
}

/* NOLINTNEXTLINE(misc-use-internal-linkage) -- ThreadX-required exported symbol. */
void tx_application_define(void* first_unused)
{
  (void)first_unused;
  /* Supervisor bring-up moved into worker A's first action so the
   * register / start APIs run with a valid thread context. */
  wdt_sup_demo_spawn_workers();
}

void main(void)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    wdt_sup_demo_halt();
  }
  tx_kernel_enter();
  wdt_sup_demo_halt();
}
