/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_esp_hosted_rtos.c
 * @brief Host tests for the esp-hosted RTOS abstraction slice.
 *
 * @par Tag
 * [Test / Host] {World: N/A}
 *
 * @details
 * Drives ``ra8_esp_hosted_rtos.c`` -- the thread, sleep, timer and clock rows,
 * the millisecond-to-tick and spin-sizing arithmetic, the shared table
 * helpers and the lifecycle that brings the whole slice up -- through the
 * recording ThreadX model in ``ra8_esp_hosted_tx_shim_internal.h``. The
 * allocator, queue, mutex and semaphore rows are covered by the sibling
 * ``test_ra8_esp_hosted_rtos_pool.c``; the two files exist separately because
 * one would exceed the project's thousand-line cap.
 *
 * The model is what makes the error branches reachable: a test can arm one
 * ThreadX API family to fail once, so pool exhaustion, a refused queue create
 * and a refused delete are exercised as first-class outcomes rather than
 * being assumed unreachable. Everything else is driven through the bound
 * vtable, which is how the vendored core will reach these functions.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_hosted_os_abstraction.h"
#include "port_esp_hosted_host_os.h"
#include "ra8_esp_hosted_port.h"
#include "ra8_esp_hosted_rtos_internal.h"
#include "ra8_esp_hosted_tx_shim_sync_internal.h"
#include "unity_minimal.h"

/**
 * @enum t_rtos_const_t
 * @brief Fixed vectors this translation unit feeds the timer and clock rows.
 *
 * @details
 * ::k_t_rtos_exhaustion_period_ms is an arbitrary but uniform period, used so
 * every timer the table-exhaustion test claims is identical and the only thing
 * varying across its vectors is how many rows are already taken. The two tick
 * readings sit just below the 32-bit `tx_time_get` wrap, far enough apart that
 * the second is provably later than the first while both still precede the
 * discontinuity the epoch extension has to survive.
 *
 * @invariant ::k_t_rtos_ticks_pre_wrap is less than
 *            ::k_t_rtos_ticks_late_pre_wrap and both are below `UINT32_MAX`,
 *            so the rollover vector that follows them really is a wrap and not
 *            merely a smaller reading.
 *
 * @par Example:
 * @code
 * ra8_esp_hosted_tx_shim_set_ticks(k_t_rtos_ticks_pre_wrap);
 * @endcode
 *
 * @see ra8_esp_hosted_tx_shim_set_ticks
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_t_rtos_exhaustion_period_ms = 10U,         /**< Period every claimed timer runs at.   */
  k_t_rtos_ticks_pre_wrap       = 0xFFFFFF00U, /**< Tick reading shortly before the wrap. */
  k_t_rtos_ticks_late_pre_wrap  = 0xFFFFFFF0U, /**< A later reading, still before it.     */
} t_rtos_const_t;

/** Vtable every test drives the port through. */
static hosted_osi_funcs_t s_funcs;

/** Set by the timer callback so a test can prove the expiry ran. */
static uint32_t s_timer_hits;

/** Set by the thread body so a test can prove the entry shim ran. */
static const void* s_thread_arg_seen;

/** Bring the port down (if up), clear the ThreadX model, bring it back up. */
static void reset_port(void)
{
  if (ra8_esp_hosted_rtos_is_ready()) {
    (void)ra8_esp_hosted_rtos_deinit();
  }
  ra8_esp_hosted_tx_shim_reset();
  (void)memset(&s_funcs, 0, sizeof(s_funcs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_rtos_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_rtos_bind(&s_funcs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_rtos_bind_pool(&s_funcs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_rtos_bind_sync(&s_funcs));
  s_timer_hits      = 0U;
  s_thread_arg_seen = NULL;
}

/** Timer expiry callback used by the timer tests. */
static void timer_cb(void* arg)
{
  (void)arg;
  s_timer_hits++;
}

/** Thread body used by the thread tests; never actually scheduled. */
static void thread_body(void const* arg)
{
  s_thread_arg_seen = arg;
}

/* ---------------------------------------------------------------------------
 * Millisecond-to-tick mapping
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_ms_to_ticks_maps_every_class
 *
 * @brief Zero, positive, -1 and HOSTED_BLOCK_MAX each map as documented.
 *
 * @details
 * The kernel runs at 1 kHz so a positive millisecond count passes through
 * unchanged; zero must become TX_NO_WAIT and every negative value must become
 * TX_WAIT_FOREVER. HOSTED_BLOCK_MAX reaches the vtable as -1 after its
 * conversion to int, so it is checked in that spelling too.
 *
 * @par MC/DC:
 * Two single-condition decisions, `timeout_ms < 0` then `timeout_ms == 0`;
 * neither is compound, so independence is trivial. Four vectors cover every
 * outcome: -1 (first true), (int)HOSTED_BLOCK_MAX (first true, the real
 * caller spelling), 0 (first false, second true), 250 (both false).
 *
 * @pre The port need not be initialised; the mapping is a pure function.
 * @post No module state is mutated.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_ms_to_ticks_maps_every_class(void)
{
  TEST_BEGIN("ms_to_ticks maps 0 / positive / negative / HOSTED_BLOCK_MAX");
  TEST_ASSERT_EQ(0U, ra8_esp_hosted_rtos_ms_to_ticks(0));
  TEST_ASSERT_EQ(250U, ra8_esp_hosted_rtos_ms_to_ticks(250));
  TEST_ASSERT_EQ(0xFFFFFFFFU, ra8_esp_hosted_rtos_ms_to_ticks(-1));
  TEST_ASSERT_EQ(0xFFFFFFFFU, ra8_esp_hosted_rtos_ms_to_ticks((int)HOSTED_BLOCK_MAX));
  TEST_ASSERT_EQ(0xFFFFFFFFU, ra8_esp_hosted_rtos_ms_to_ticks(-100000));
  TEST_END("ms_to_ticks maps 0 / positive / negative / HOSTED_BLOCK_MAX");
}

/* ---------------------------------------------------------------------------
 * Queue word rounding
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_queue_words_rounds_up_and_rejects_oversize
 *
 * @brief Element sizes round up to whole words; over sixteen words is refused.
 *
 * @details
 * 28 bytes (sizeof(interface_buffer_handle_t) on this ABI) is exactly seven
 * words; 29 must round to eight rather than truncate to seven, which would
 * corrupt every message. 64 bytes is the largest size ThreadX can express and
 * 65 must be refused rather than truncated.
 *
 * @par MC/DC:
 * Decision `(item_bytes == 0U) || (words > k_queue_words_max)` in
 * `port/esp-hosted/src/ra8_esp_hosted_rtos_pool.c@ra8_esp_hosted_rtos_queue_words`
 * (2 conditions).
 * - Vector 1: 28 bytes -> false, false -> returns 7 (control).
 * - Vector 2: 0 bytes  -> true (short-circuit) -> returns 0 (varies emptiness).
 * - Vector 3: 68 bytes -> false, true -> returns 0 (varies the word cap).
 * Vectors 1+2 prove the emptiness condition independently drives the outcome;
 * 1+3 prove the same for the cap. N+1 = 3 vectors for N=2: minimal MC/DC.
 *
 * @pre The port need not be initialised; the rounding is a pure function.
 * @post No module state is mutated.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_queue_words_rounds_up_and_rejects_oversize(void)
{
  TEST_BEGIN("queue_words rounds up and rejects over sixteen words");
  TEST_ASSERT_EQ(7U, ra8_esp_hosted_rtos_queue_words(28U));
  TEST_ASSERT_EQ(8U, ra8_esp_hosted_rtos_queue_words(29U));
  TEST_ASSERT_EQ(1U, ra8_esp_hosted_rtos_queue_words(1U));
  TEST_ASSERT_EQ(16U, ra8_esp_hosted_rtos_queue_words(64U));
  TEST_ASSERT_EQ(0U, ra8_esp_hosted_rtos_queue_words(0U));
  TEST_ASSERT_EQ(0U, ra8_esp_hosted_rtos_queue_words(65U));
  TEST_ASSERT_EQ(0U, ra8_esp_hosted_rtos_queue_words(68U));
  TEST_END("queue_words rounds up and rejects over sixteen words");
}

/* ---------------------------------------------------------------------------
 * Microsecond spin sizing
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_us_spin_iters_guards_and_clamps
 *
 * @brief An unknown rate or a zero duration spins not at all; long ones clamp.
 *
 * @details
 * At 1 GHz and four cycles per iteration, one microsecond is 250 iterations,
 * so the arithmetic is checked against a hand-computed value rather than
 * against itself. The clamp is what makes the loop statically bounded.
 *
 * @par MC/DC:
 * Decision `(cpu_hz == 0U) || (usec == 0U)` in
 * `port/esp-hosted/src/ra8_esp_hosted_rtos.c@ra8_esp_hosted_rtos_us_spin_iters`
 * (2 conditions).
 * - Vector 1: cpu_hz=1e9, usec=1 -> false, false -> returns 250 (control).
 * - Vector 2: cpu_hz=0,   usec=1 -> true (short-circuit) -> 0 (varies rate).
 * - Vector 3: cpu_hz=1e9, usec=0 -> false, true -> 0 (varies duration).
 * Vectors 1+2 prove the rate condition independently drives the outcome; 1+3
 * prove the same for the duration. N+1 = 3 vectors for N=2: minimal MC/DC.
 *
 * @pre The port need not be initialised; the sizing is a pure function.
 * @post No module state is mutated.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_us_spin_iters_guards_and_clamps(void)
{
  TEST_BEGIN("us_spin_iters guards zero inputs and clamps long spins");
  TEST_ASSERT_EQ(250U, ra8_esp_hosted_rtos_us_spin_iters(1000000000U, 1U));
  TEST_ASSERT_EQ(0U, ra8_esp_hosted_rtos_us_spin_iters(0U, 1U));
  TEST_ASSERT_EQ(0U, ra8_esp_hosted_rtos_us_spin_iters(1000000000U, 0U));
  /* A rate below 1 MHz truncates to zero cycles per microsecond; the floor
   * keeps the caller spinning at least once rather than not at all. */
  TEST_ASSERT_EQ(1U, ra8_esp_hosted_rtos_us_spin_iters(100000U, 1U));
  /* 999 us at 1 GHz is 249750 iterations, just inside the 250000 clamp. */
  TEST_ASSERT_EQ(249750U, ra8_esp_hosted_rtos_us_spin_iters(1000000000U, 999U));
  TEST_ASSERT_EQ(250000U, ra8_esp_hosted_rtos_us_spin_iters(1000000000U, 5000U));
  TEST_END("us_spin_iters guards zero inputs and clamps long spins");
}

/* ---------------------------------------------------------------------------
 * Table helpers
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_slot_take_claims_then_reports_full
 *
 * @brief The bitmap hands out each free row once, then reports the table full.
 *
 * @details
 * Returning the row count on failure is what lets a caller use one comparison
 * for both the bound and the failure, so the "full" answer is asserted to be
 * exactly the count and not merely "some large value".
 *
 * @par MC/DC:
 * Two single-condition decisions, `used == nullptr` and `!used[i]`. Vectors:
 * a null bitmap (first true), a bitmap with a free row (first false, second
 * true), and a fully occupied bitmap (first false, second false throughout).
 *
 * @pre The port need not be initialised.
 * @post The local bitmap is fully occupied.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_slot_take_claims_then_reports_full(void)
{
  TEST_BEGIN("slot_take claims free rows then reports the table full");
  bool used[3] = {};
  TEST_ASSERT_EQ(3U, ra8_esp_hosted_rtos_slot_take(NULL, 3U));
  TEST_ASSERT_EQ(0U, ra8_esp_hosted_rtos_slot_take(used, 3U));
  TEST_ASSERT_EQ(1U, ra8_esp_hosted_rtos_slot_take(used, 3U));
  TEST_ASSERT_EQ(2U, ra8_esp_hosted_rtos_slot_take(used, 3U));
  TEST_ASSERT_EQ(3U, ra8_esp_hosted_rtos_slot_take(used, 3U));
  TEST_END("slot_take claims free rows then reports the table full");
}

/**
 * @test test_slot_index_rejects_null_foreign_and_freed
 *
 * @brief Only a live row of the named table resolves; everything else fails.
 *
 * @details
 * The lookup is the port's whole defence against a stale or foreign handle,
 * so each way of being wrong is checked separately: a null handle, a null
 * table, a null bitmap, an address outside the table, and an address inside
 * it whose occupancy flag has been cleared.
 *
 * @par MC/DC:
 * Both decisions live in
 * `port/esp-hosted/src/ra8_esp_hosted_rtos.c@ra8_esp_hosted_rtos_slot_index`.
 * Decision A, `(handle == nullptr) || (base == nullptr) || (used == nullptr)`
 * (3 conditions):
 * - Vector 1: all three non-null -> false,false,false (control).
 * - Vector 2: handle null        -> true (varies handle).
 * - Vector 3: base null          -> false,true (varies base).
 * - Vector 4: used null          -> false,false,true (varies used).
 * Pairs 1+2, 1+3 and 1+4 prove each condition's independent influence.
 * N+1 = 4 vectors for N=3: minimal MC/DC.
 * Decision B, `(row == handle) && used[i]` (2 conditions):
 * - Vector 1: matching row, flag set   -> true,true  -> found (control).
 * - Vector 2: foreign address, flag set -> false     -> not found.
 * - Vector 3: matching row, flag clear  -> true,false -> not found.
 * Vectors 1+2 prove the address comparison's independent influence; 1+3 prove
 * the same for the occupancy flag. N+1 = 3 vectors for N=2: minimal MC/DC.
 *
 * @pre The port need not be initialised.
 * @post The local table is untouched.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_slot_index_rejects_null_foreign_and_freed(void)
{
  TEST_BEGIN("slot_index resolves only live rows of the named table");
  uint64_t rows[2]   = {};
  bool     used[2]   = {true, true};
  uint32_t elsewhere = 0U;

  TEST_ASSERT_EQ(1U, ra8_esp_hosted_rtos_slot_index(&rows[1], rows, sizeof(rows[0]), 2U, used));
  TEST_ASSERT_EQ(2U, ra8_esp_hosted_rtos_slot_index(NULL, rows, sizeof(rows[0]), 2U, used));
  TEST_ASSERT_EQ(2U, ra8_esp_hosted_rtos_slot_index(&rows[1], NULL, sizeof(rows[0]), 2U, used));
  TEST_ASSERT_EQ(2U, ra8_esp_hosted_rtos_slot_index(&rows[1], rows, sizeof(rows[0]), 2U, NULL));
  TEST_ASSERT_EQ(2U, ra8_esp_hosted_rtos_slot_index(&elsewhere, rows, sizeof(rows[0]), 2U, used));
  used[1] = false;
  TEST_ASSERT_EQ(2U, ra8_esp_hosted_rtos_slot_index(&rows[1], rows, sizeof(rows[0]), 2U, used));
  TEST_END("slot_index resolves only live rows of the named table");
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_init_rejects_second_call_and_deinit_needs_init
 *
 * @brief Init is not idempotent and deinit refuses an uninitialised port.
 *
 * @details
 * A silent second success would hand the caller pools whose contents the
 * first caller still owns, so the state machine is asserted in both
 * directions rather than only on the happy path.
 *
 * @par MC/DC:
 * Two single-condition decisions, `s_rtos.ready` in init and `!s_rtos.ready`
 * in deinit; no compound condition. Vectors: init from down (false), init
 * from up (true), deinit from up (false), deinit from down (true).
 *
 * @pre The ThreadX model has been reset.
 * @post The port is left initialised for the tests that follow.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_init_rejects_second_call_and_deinit_needs_init(void)
{
  TEST_BEGIN("init rejects a second call; deinit rejects an uninitialised port");
  if (ra8_esp_hosted_rtos_is_ready()) {
    (void)ra8_esp_hosted_rtos_deinit();
  }
  ra8_esp_hosted_tx_shim_reset();
  TEST_ASSERT_EQ(false, ra8_esp_hosted_rtos_is_ready());
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_esp_hosted_rtos_deinit());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_rtos_init());
  TEST_ASSERT_EQ(true, ra8_esp_hosted_rtos_is_ready());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_esp_hosted_rtos_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_rtos_deinit());
  TEST_ASSERT_EQ(false, ra8_esp_hosted_rtos_is_ready());
  TEST_END("init rejects a second call; deinit rejects an uninitialised port");
}

/**
 * @test test_init_reports_a_refused_byte_pool
 *
 * @brief A ThreadX pool refusal is reported, not swallowed.
 *
 * @details
 * Arms the modelled byte-pool family to fail its next call, which is the
 * transport pool create inside the port's init. The port must report
 * k_ra8_err_rtos_error and stay down rather than come up with no allocator.
 *
 * @par MC/DC:
 * Single-condition decision `tx_byte_pool_create(...) != TX_SUCCESS`; vectors
 * are the armed failure here and the ordinary success in every other test.
 *
 * @pre The ThreadX model has been reset.
 * @post The port is left down; the caller re-initialises.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_init_reports_a_refused_byte_pool(void)
{
  TEST_BEGIN("init reports a refused byte pool");
  if (ra8_esp_hosted_rtos_is_ready()) {
    (void)ra8_esp_hosted_rtos_deinit();
  }
  ra8_esp_hosted_tx_shim_reset();
  ra8_esp_hosted_tx_shim_arm(k_ra8_esp_hosted_tx_shim_family_pool, TX_NO_MEMORY);
  TEST_ASSERT_EQ(k_ra8_err_rtos_error, ra8_esp_hosted_rtos_init());
  TEST_ASSERT_EQ(false, ra8_esp_hosted_rtos_is_ready());
  TEST_END("init reports a refused byte pool");
}

/**
 * @test test_bind_fills_its_rows_and_rejects_null
 *
 * @brief Each binder fills exactly its own rows and refuses a null table.
 *
 * @details
 * The three binders are separate because each translation unit can only see
 * its own implementations; the test therefore checks that a row is still null
 * after the binder that does not own it has run, and non-null after the one
 * that does.
 *
 * @par MC/DC:
 * Single-condition decision `out == nullptr` in each binder (the
 * RA8_CHECK_NULL_PTR guard). Vectors: a null table and a real table, for all
 * three binders.
 *
 * @pre The ThreadX model has been reset.
 * @post The local vtable holds every RTOS row.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_bind_fills_its_rows_and_rejects_null(void)
{
  TEST_BEGIN("each binder fills its own rows and rejects a null table");
  hosted_osi_funcs_t table = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_esp_hosted_rtos_bind(NULL));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_esp_hosted_rtos_bind_pool(NULL));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_esp_hosted_rtos_bind_sync(NULL));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_rtos_bind(&table));
  TEST_ASSERT_NOT_NULL(table._h_thread_create);
  TEST_ASSERT_NOT_NULL(table._h_get_time_ms);
  TEST_ASSERT_NULL(table._h_malloc);
  TEST_ASSERT_NULL(table._h_create_mutex);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_rtos_bind_pool(&table));
  TEST_ASSERT_NOT_NULL(table._h_malloc);
  TEST_ASSERT_NOT_NULL(table._h_create_queue);
  TEST_ASSERT_NULL(table._h_create_mutex);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_rtos_bind_sync(&table));
  TEST_ASSERT_NOT_NULL(table._h_create_mutex);
  TEST_ASSERT_NOT_NULL(table._h_post_semaphore_from_isr);
  TEST_END("each binder fills its own rows and rejects a null table");
}

/* ---------------------------------------------------------------------------
 * Threads, timers, sleeps and the extended clock
 * ---------------------------------------------------------------------------
 */

/**
 * @test test_thread_create_bounds_cancel_and_table
 *
 * @brief Stack bounds are enforced, the entry shim runs, the table is fixed.
 *
 * @details
 * The port reserves one fixed stack region per table row, so a request larger
 * than a region must be refused rather than quietly given a smaller stack --
 * the failure mode that produces a stack overflow weeks later. The recorded
 * entry point is invoked directly (nothing is scheduled on the host) to prove
 * the ULONG-index trampoline reaches the core's `void const*` body.
 *
 * @par MC/DC:
 * Creating a thread reaches three functions, so this case covers the
 * decisions of all three:
 * `port/esp-hosted/src/ra8_esp_hosted_rtos.c@internal_h_thread_create` (its
 * two entry guards),
 * `port/esp-hosted/src/ra8_esp_hosted_rtos.c@internal_copy_name` (the bounded
 * name copy) and
 * `port/esp-hosted/src/ra8_esp_hosted_rtos.c@internal_thread_entry` (the
 * ULONG-index trampoline).
 * Decision `!s_rtos.ready || (start_routine == nullptr)` in the create
 * (2 conditions):
 * - Vector 1: ready, real body -> false,false -> creates (control).
 * - Vector 2: not ready        -> true        -> refused (varies readiness).
 * - Vector 3: ready, null body -> false,true  -> refused (varies the body).
 * Decision `(tstack_size < TX_MINIMUM_STACK) || (tstack_size > k_stack_bytes)`
 * in the same function (2 conditions) is covered by 4096 (control), 64
 * (first true) and 8192 (first false, second true).
 *
 * The bounded name copy `while ((n < limit) && (src[n] != '\0'))`
 * (2 conditions):
 * - Vector 1: "spi_trans", shorter than the buffer -> both true for nine
 *   passes, then the terminator ends it (varies the character test)
 * - Vector 2: "an_extremely_long_thread_name" -> the character test stays
 *   true and the bound ends it, which is the NASA Rule 2 path and the reason
 *   an over-long name truncates instead of being refused (varies the bound)
 * The `(dst == nullptr) || (cap == 0U)` guard above it takes only its false
 * arm: both call sites pass `slot->name` and `sizeof(slot->name)`, so neither
 * condition can be true without the table row itself being malformed. Its
 * ``src == nullptr`` sibling is a separate single-condition test and is
 * driven by the null-name creation below.
 *
 * The entry shim's guard
 * `if (s_rtos.thread_used[idx] && (s_rtos.threads[idx].entry != nullptr))`
 * takes its true-true control when the trampoline is invoked below. Neither
 * false arm
 * is reachable here: nothing schedules on the host, so the shim is only ever
 * entered by hand with a live row, and driving it after a cancel is not
 * possible because ``_h_thread_cancel`` deletes the ThreadX control block the
 * entry pointer is read from. The guard is there for the hardware case, where
 * a terminated thread can still be dispatched once before deletion lands.
 *
 * @pre The port is initialised.
 * @post Every thread created here is cancelled.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_thread_create_bounds_cancel_and_table(void)
{
  TEST_BEGIN("thread create bounds, entry shim, cancel, fixed table");
  reset_port();
  int marker = 0;
  TEST_ASSERT_NULL(s_funcs._h_thread_create("t", DFLT_TASK_PRIO, 4096U, NULL, &marker));
  TEST_ASSERT_NULL(s_funcs._h_thread_create("t", DFLT_TASK_PRIO, 64U, thread_body, &marker));
  TEST_ASSERT_NULL(s_funcs._h_thread_create("t", DFLT_TASK_PRIO, 8192U, thread_body, &marker));

  void* const t = s_funcs._h_thread_create("spi_trans",
                                           DFLT_TASK_PRIO,
                                           DFLT_TASK_STACK_SIZE,
                                           thread_body,
                                           &marker);
  TEST_ASSERT_NOT_NULL(t);
  const TX_THREAD* const cb = (const TX_THREAD*)t;
  TEST_ASSERT_EQ(DFLT_TASK_PRIO, cb->priority);
  TEST_ASSERT_EQ(DFLT_TASK_STACK_SIZE, cb->stack_size);
  /* Run the trampoline the way ThreadX would; it must reach thread_body. */
  cb->entry(cb->input);
  TEST_ASSERT(s_thread_arg_seen == &marker);

  /* A name longer than the fixed buffer is truncated, not refused. */
  void* const t2 = s_funcs._h_thread_create("an_extremely_long_thread_name",
                                            DFLT_TASK_PRIO,
                                            DFLT_TASK_STACK_SIZE,
                                            thread_body,
                                            NULL);
  TEST_ASSERT_NOT_NULL(t2);
  void* const t3 =
    s_funcs._h_thread_create(NULL, DFLT_TASK_PRIO, DFLT_TASK_STACK_SIZE, thread_body, NULL);
  TEST_ASSERT_NOT_NULL(t3);
  void* const t4 =
    s_funcs._h_thread_create("d", DFLT_TASK_PRIO, DFLT_TASK_STACK_SIZE, thread_body, NULL);
  TEST_ASSERT_NOT_NULL(t4);
  TEST_ASSERT_NULL(
    s_funcs._h_thread_create("e", DFLT_TASK_PRIO, DFLT_TASK_STACK_SIZE, thread_body, NULL));

  TEST_ASSERT_EQ(RET_INVALID, s_funcs._h_thread_cancel(NULL));
  TEST_ASSERT_EQ(RET_OK, s_funcs._h_thread_cancel(t));
  TEST_ASSERT_EQ(RET_INVALID, s_funcs._h_thread_cancel(t));
  TEST_ASSERT_EQ(RET_OK, s_funcs._h_thread_cancel(t2));
  TEST_ASSERT_EQ(RET_OK, s_funcs._h_thread_cancel(t3));
  TEST_ASSERT_EQ(RET_OK, s_funcs._h_thread_cancel(t4));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_rtos_deinit());
  TEST_ASSERT_NULL(
    s_funcs._h_thread_create("t", DFLT_TASK_PRIO, DFLT_TASK_STACK_SIZE, thread_body, NULL));
  TEST_END("thread create bounds, entry shim, cancel, fixed table");
}

/**
 * @test test_timer_oneshot_and_periodic
 *
 * @brief One-shot timers stop after one expiry; periodic ones keep going.
 *
 * @details
 * The distinction is carried entirely by the ThreadX reschedule count, so the
 * test fires each timer through the model and checks both the callback count
 * and whether the timer is still active afterwards.
 *
 * @par MC/DC:
 * Starting and firing a timer reaches two functions, so this case covers
 * both: `port/esp-hosted/src/ra8_esp_hosted_rtos.c@internal_h_timer_start`
 * (its two entry guards) and
 * `port/esp-hosted/src/ra8_esp_hosted_rtos.c@internal_timer_expiry` (the
 * ULONG-index expiry shim).
 * Decision `!s_rtos.ready || (timeout_handler == nullptr) || (duration_ms <= 0)`
 * in the start (3 conditions):
 * - Vector 1: ready, real handler, 50 ms -> false,false,false (control).
 * - Vector 2: not ready                  -> true (varies readiness).
 * - Vector 3: ready, null handler        -> false,true (varies the handler).
 * - Vector 4: ready, real handler, 0 ms  -> false,false,true (varies duration).
 * Pairs 1+2, 1+3 and 1+4 prove each condition's independent influence.
 * N+1 = 4 vectors for N=3: minimal MC/DC.
 * Decision `(type != ONESHOT) && (type != PERIODIC)` in the same function
 * (2 conditions) is covered by ONESHOT (false), PERIODIC (true,false) and 99
 * (true,true).
 *
 * Firing a timer drives the expiry shim's guard
 * `if (s_rtos.timer_used[idx] && (s_rtos.timers[idx].cb_fn != nullptr))`
 * through its true-true control, which is what the callback counts below
 * observe.
 * Neither false arm is reachable: the model only dispatches a row it still
 * holds active, and a live row always has a callback because the start
 * refuses a null one. The guard is there for the hardware case, where a
 * stopped timer can still have one expiry in flight.
 *
 * @pre The port is initialised.
 * @post Every timer created here is stopped.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_timer_oneshot_and_periodic(void)
{
  TEST_BEGIN("one-shot timers stop after one expiry; periodic ones repeat");
  reset_port();
  TEST_ASSERT_NULL(s_funcs._h_timer_start("t", 50, H_TIMER_TYPE_ONESHOT, NULL, NULL));
  TEST_ASSERT_NULL(s_funcs._h_timer_start("t", 0, H_TIMER_TYPE_ONESHOT, timer_cb, NULL));
  TEST_ASSERT_NULL(s_funcs._h_timer_start("t", 50, 99, timer_cb, NULL));

  void* const one = s_funcs._h_timer_start("once", 50, H_TIMER_TYPE_ONESHOT, timer_cb, NULL);
  TEST_ASSERT_NOT_NULL(one);
  TEST_ASSERT_EQ(0U, ((const TX_TIMER*)one)->reschedule);
  TEST_ASSERT_EQ(true, ra8_esp_hosted_tx_shim_fire_timer((TX_TIMER*)one));
  TEST_ASSERT_EQ(1U, s_timer_hits);
  TEST_ASSERT_EQ(false, ((const TX_TIMER*)one)->active);
  TEST_ASSERT_EQ(false, ra8_esp_hosted_tx_shim_fire_timer((TX_TIMER*)one));

  void* const rep = s_funcs._h_timer_start("rep", 20, H_TIMER_TYPE_PERIODIC, timer_cb, NULL);
  TEST_ASSERT_NOT_NULL(rep);
  TEST_ASSERT_EQ(20U, ((const TX_TIMER*)rep)->reschedule);
  TEST_ASSERT_EQ(true, ra8_esp_hosted_tx_shim_fire_timer((TX_TIMER*)rep));
  TEST_ASSERT_EQ(true, ra8_esp_hosted_tx_shim_fire_timer((TX_TIMER*)rep));
  TEST_ASSERT_EQ(3U, s_timer_hits);
  TEST_ASSERT_EQ(true, ((const TX_TIMER*)rep)->active);

  TEST_ASSERT_EQ(RET_INVALID, s_funcs._h_timer_stop(NULL));
  TEST_ASSERT_EQ(RET_OK, s_funcs._h_timer_stop(one));
  TEST_ASSERT_EQ(RET_OK, s_funcs._h_timer_stop(rep));
  TEST_ASSERT_EQ(RET_INVALID, s_funcs._h_timer_stop(rep));
  TEST_END("one-shot timers stop after one expiry; periodic ones repeat");
}

/**
 * @test test_timer_table_exhaustion
 *
 * @brief The timer table refuses beyond its fixed budget and recovers.
 *
 * @details
 * Four rows is the budget; the fifth must fail, and stopping one must free
 * the row so a later start succeeds. That second half is what proves the row
 * is released rather than merely marked.
 *
 * @par MC/DC:
 * Single-condition decision `idx == k_max_timers` after the table claim.
 * Vectors: the four successful claims, the fifth refusal, and the successful
 * claim after a stop.
 *
 * @pre The port is initialised.
 * @post Every timer created here is stopped.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_timer_table_exhaustion(void)
{
  TEST_BEGIN("timer table refuses beyond its budget and recovers on stop");
  reset_port();
  void* timers[k_ra8_esp_hosted_max_timers] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_esp_hosted_max_timers; ++i) {
    timers[i] = s_funcs._h_timer_start("t",
                                       (int)k_t_rtos_exhaustion_period_ms,
                                       H_TIMER_TYPE_PERIODIC,
                                       timer_cb,
                                       NULL);
    TEST_ASSERT_NOT_NULL(timers[i]);
  }
  TEST_ASSERT_NULL(s_funcs._h_timer_start("t",
                                          (int)k_t_rtos_exhaustion_period_ms,
                                          H_TIMER_TYPE_PERIODIC,
                                          timer_cb,
                                          NULL));
  TEST_ASSERT_EQ(RET_OK, s_funcs._h_timer_stop(timers[0]));
  timers[0] = s_funcs._h_timer_start("t",
                                     (int)k_t_rtos_exhaustion_period_ms,
                                     H_TIMER_TYPE_PERIODIC,
                                     timer_cb,
                                     NULL);
  TEST_ASSERT_NOT_NULL(timers[0]);
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_esp_hosted_max_timers; ++i) {
    TEST_ASSERT_EQ(RET_OK, s_funcs._h_timer_stop(timers[i]));
  }
  TEST_END("timer table refuses beyond its budget and recovers on stop");
}

/**
 * @test test_get_time_ms_extends_across_a_32_bit_rollover
 *
 * @brief The millisecond clock keeps rising when the tick counter wraps.
 *
 * @details
 * `tx_time_get` wraps after about 49.7 days at 1 kHz. The model lets the tick
 * counter be placed just below the wrap and then moved past it, so the port's
 * 64-bit extension is driven over the exact discontinuity rather than
 * inspected by reading the source.
 *
 * @par MC/DC:
 * Two single-condition decisions: `!s_rtos.ready` and `now < s_rtos.last_ticks`.
 * Vectors: uninitialised (first true), a rising reading (first false, second
 * false), and a reading that has wrapped (first false, second true).
 *
 * @pre The port is initialised.
 * @post The port is left initialised with a non-zero epoch.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_get_time_ms_extends_across_a_32_bit_rollover(void)
{
  TEST_BEGIN("get_time_ms extends across a 32-bit tick rollover");
  reset_port();
  ra8_esp_hosted_tx_shim_set_ticks(k_t_rtos_ticks_pre_wrap);
  const uint64_t before = s_funcs._h_get_time_ms();
  TEST_ASSERT_EQ(k_t_rtos_ticks_pre_wrap, before);

  /* One more reading below the wrap: time must move forward, not epoch. */
  ra8_esp_hosted_tx_shim_set_ticks(k_t_rtos_ticks_late_pre_wrap);
  const uint64_t later = s_funcs._h_get_time_ms();
  TEST_ASSERT_EQ(k_t_rtos_ticks_late_pre_wrap, later);

  /* Now past the wrap: the reading is smaller but the reported time is not. */
  ra8_esp_hosted_tx_shim_set_ticks(0x00000010U);
  const uint64_t wrapped = s_funcs._h_get_time_ms();
  TEST_ASSERT_EQ(0x100000010ULL, wrapped);
  TEST_ASSERT(wrapped > later);

  /* And it keeps rising within the new epoch. */
  ra8_esp_hosted_tx_shim_set_ticks(0x00000020U);
  TEST_ASSERT_EQ(0x100000020ULL, s_funcs._h_get_time_ms());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_rtos_deinit());
  TEST_ASSERT_EQ(0ULL, s_funcs._h_get_time_ms());
  TEST_END("get_time_ms extends across a 32-bit tick rollover");
}

/**
 * @test test_sleeps_yield_and_delays
 *
 * @brief Sleeps advance the kernel clock; usleep splits; delays never sleep.
 *
 * @details
 * The model advances its tick counter by whatever a sleep asked for, so the
 * millisecond and second sleeps can be checked by arithmetic. `_h_usleep` is
 * the interesting one: a sub-millisecond request must not sleep at all (and
 * must not silently do nothing either -- it spins), while a request of a
 * millisecond or more sleeps the whole-millisecond part.
 *
 * @par MC/DC:
 * Two single-condition decisions in `_h_usleep`, `useconds == 0U` and
 * `whole_ms > 0U`. Vectors: 0 us (first true), 250 us (first false, second
 * false: spins only), 2500 us (first false, second true: sleeps 2 ms then
 * spins 500 us). `_h_blocking_delay`'s clamp select is covered by a small
 * count and a count above the bound. Beneath both of those sits a
 * doubly-bounded loop condition -- `for (i = 0; (i < iters) &&
 * (i < k_spin_iters_max); ++i)` in
 * `port/esp-hosted/src/ra8_esp_hosted_rtos.c@internal_spin_us`, and the same
 * shape against `k_delay_iters_max` in
 * `port/esp-hosted/src/ra8_esp_hosted_rtos.c@internal_h_blocking_delay`.
 * The first condition takes both outcomes:
 * - Vector 1: a non-zero request within the bound -> true while iterating,
 *   then false, which is what ends the loop (0 us for the delay's small
 *   count, 250 us and 2500 us for the spin)
 * - Vector 2: a zero request -> false on the first pass; the body never runs
 *
 * The second condition is deactivated rather than untested, and deliberately
 * so: `iters` is clamped to the very constant the condition restates --
 * ::ra8_esp_hosted_rtos_us_spin_iters clamps one, the select above the loop
 * clamps the other -- so it can never be false while the first is true, and
 * no vector can show it independently affecting the outcome. It is present
 * because NASA Power of 10 Rule 2 asks for a bound provable from the loop
 * itself, and the ``RA8_BOUNDED_LOOP`` annotation on each function names the
 * constant that makes it provable. Removing it would move the proof into a
 * caller and leave the loop unbounded on its face. The clamp it restates is
 * what ::test_us_spin_iters_guards_and_clamps covers, with an above-the-bound
 * duration.
 *
 * @pre The port is initialised.
 * @post The fake kernel clock has advanced.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_sleeps_yield_and_delays(void)
{
  TEST_BEGIN("sleeps advance the kernel clock; usleep splits; delays spin");
  reset_port();
  ra8_esp_hosted_tx_shim_set_ticks(0U);

  TEST_ASSERT_EQ(7U, s_funcs._h_msleep(7U));
  TEST_ASSERT_EQ(7U, g_ra8_esp_hosted_tx_shim.ticks);
  TEST_ASSERT_EQ(2U, s_funcs._h_sleep(2U));
  TEST_ASSERT_EQ(2007U, g_ra8_esp_hosted_tx_shim.ticks);

  /* Sub-millisecond: spins, never sleeps. */
  const uint32_t sleeps_before = g_ra8_esp_hosted_tx_shim.sleeps;
  TEST_ASSERT_EQ(0U, s_funcs._h_usleep(0U));
  TEST_ASSERT_EQ(250U, s_funcs._h_usleep(250U));
  TEST_ASSERT_EQ(sleeps_before, g_ra8_esp_hosted_tx_shim.sleeps);
  TEST_ASSERT_EQ(2007U, g_ra8_esp_hosted_tx_shim.ticks);

  /* Two and a half milliseconds: sleeps 2 ms, spins the remaining 500 us. */
  TEST_ASSERT_EQ(2500U, s_funcs._h_usleep(2500U));
  TEST_ASSERT_EQ(sleeps_before + 1U, g_ra8_esp_hosted_tx_shim.sleeps);
  TEST_ASSERT_EQ(2009U, g_ra8_esp_hosted_tx_shim.ticks);

  TEST_ASSERT_EQ(0U, g_ra8_esp_hosted_tx_shim.relinquishes);
  s_funcs._h_thread_yield();
  TEST_ASSERT_EQ(1U, g_ra8_esp_hosted_tx_shim.relinquishes);

  TEST_ASSERT_EQ(100U, s_funcs._h_blocking_delay(100U));
  TEST_ASSERT_EQ(2000000U, s_funcs._h_blocking_delay(2000000U));
  TEST_ASSERT_EQ(2009U, g_ra8_esp_hosted_tx_shim.ticks);

  /* Down: a sleep must not touch the kernel, and yield must not relinquish. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_rtos_deinit());
  TEST_ASSERT_EQ(5U, s_funcs._h_msleep(5U));
  TEST_ASSERT_EQ(2009U, g_ra8_esp_hosted_tx_shim.ticks);
  s_funcs._h_thread_yield();
  TEST_ASSERT_EQ(1U, g_ra8_esp_hosted_tx_shim.relinquishes);
  TEST_END("sleeps advance the kernel clock; usleep splits; delays spin");
}

/**
 * @test test_deinit_releases_every_outstanding_object
 *
 * @brief Teardown deletes whatever the core still holds and empties the tables.
 *
 * @details
 * The vendored core is not required to destroy its objects before the port is
 * torn down, so deinit has to walk every table itself. The test leaves one of
 * each kind outstanding, tears down, and then proves the tables are empty by
 * re-initialising and allocating a full set again.
 *
 * @par MC/DC:
 * Each teardown walk pairs its occupancy flag with the release call in one
 * compound decision, so a refusal is recorded only for a row that was
 * actually held:
 * `if (s_rtos.timer_used[i] && (internal_h_timer_stop(...) != RET_OK))` and
 * the matching thread walk in
 * `port/esp-hosted/src/ra8_esp_hosted_rtos.c@ra8_esp_hosted_rtos_deinit`, and
 * the semaphore and mutex walks in
 * `port/esp-hosted/src/ra8_esp_hosted_rtos_sync.c@ra8_esp_hosted_rtos_sync_deinit`
 * (2 conditions each).
 * - Vector 1: used=T, the release refuses -> true; the worst-error is raised.
 *   Driven by ::test_deinit_reports_a_refused_delete, which arms the model to
 *   refuse (the control vector for the second condition).
 * - Vector 2: used=F -> false; the empty rows of every table are walked here
 *   on every teardown and must record nothing (varies the occupancy flag).
 * - Vector 3: used=T, the release succeeds -> false; the occupied row this
 *   case leaves in each table is released cleanly and the teardown reports
 *   success (varies the release result).
 * Vectors 1+2 prove the occupancy flag independently affects the outcome;
 * 1+3 prove the same for the release result. N+1 = 3 vectors for N=2:
 * minimal MC/DC per walk.
 *
 * @pre The port is initialised.
 * @post The port is left down.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_deinit_releases_every_outstanding_object(void)
{
  TEST_BEGIN("deinit releases every outstanding object");
  reset_port();
  TEST_ASSERT_NOT_NULL(s_funcs._h_create_queue(4U, 28U));
  TEST_ASSERT_NOT_NULL(s_funcs._h_create_mutex());
  TEST_ASSERT_NOT_NULL(s_funcs._h_create_semaphore(1));
  TEST_ASSERT_NOT_NULL(s_funcs._h_timer_start("t", 10, H_TIMER_TYPE_PERIODIC, timer_cb, NULL));
  TEST_ASSERT_NOT_NULL(
    s_funcs._h_thread_create("t", DFLT_TASK_PRIO, DFLT_TASK_STACK_SIZE, thread_body, NULL));
  TEST_ASSERT_NOT_NULL(s_funcs._h_malloc(64U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_rtos_deinit());
  TEST_ASSERT_EQ(false, ra8_esp_hosted_rtos_is_ready());

  /* Every table must be empty again: a full set of each kind must fit. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_rtos_init());
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_esp_hosted_max_mutexes; ++i) {
    TEST_ASSERT_NOT_NULL(s_funcs._h_create_mutex());
  }
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_esp_hosted_max_timers; ++i) {
    TEST_ASSERT_NOT_NULL(s_funcs._h_timer_start("t", 10, H_TIMER_TYPE_PERIODIC, timer_cb, NULL));
  }
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_esp_hosted_max_threads; ++i) {
    TEST_ASSERT_NOT_NULL(
      s_funcs._h_thread_create("t", DFLT_TASK_PRIO, DFLT_TASK_STACK_SIZE, thread_body, NULL));
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_esp_hosted_rtos_deinit());
  TEST_END("deinit releases every outstanding object");
}

/**
 * @test test_deinit_reports_a_refused_delete
 *
 * @brief A ThreadX delete refusal during teardown is reported, not swallowed.
 *
 * @details
 * Teardown must attempt every object even when one refuses, so the worst code
 * seen is returned rather than the first. Each family is armed to fail once,
 * with exactly one live object of that kind, so the refusal lands inside that
 * family's walk and nowhere else.
 *
 * @par MC/DC:
 * Single-condition decision `tx_queue_delete(...) != TX_SUCCESS` inside the
 * pool teardown walk. Vectors: the armed refusal here, and the ordinary
 * success in ::test_deinit_releases_every_outstanding_object.
 *
 * The four object walks that pair an occupancy flag with a release result --
 * the timer and thread walks in
 * `port/esp-hosted/src/ra8_esp_hosted_rtos.c@ra8_esp_hosted_rtos_deinit`, and
 * the semaphore and mutex walks in
 * `port/esp-hosted/src/ra8_esp_hosted_rtos_sync.c@ra8_esp_hosted_rtos_sync_deinit`
 * -- take their "occupied row, release refuses" vector here, one family at a
 * time. It is the vector that proves the release result independently affects
 * the outcome; the other two vectors of each walk are in
 * ::test_deinit_releases_every_outstanding_object.
 *
 * @pre The port is initialised.
 * @post The port is left down.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_deinit_reports_a_refused_delete(void)
{
  TEST_BEGIN("deinit reports a refused delete");
  reset_port();
  TEST_ASSERT_NOT_NULL(s_funcs._h_create_queue(4U, 28U));
  ra8_esp_hosted_tx_shim_arm(k_ra8_esp_hosted_tx_shim_family_queue, TX_QUEUE_ERROR);
  TEST_ASSERT_EQ(k_ra8_err_rtos_error, ra8_esp_hosted_rtos_deinit());
  TEST_ASSERT_EQ(false, ra8_esp_hosted_rtos_is_ready());

  /* A live timer whose deactivate is refused: the timer walk must record it. */
  reset_port();
  TEST_ASSERT_NOT_NULL(s_funcs._h_timer_start("t", 10, H_TIMER_TYPE_PERIODIC, timer_cb, NULL));
  ra8_esp_hosted_tx_shim_arm(k_ra8_esp_hosted_tx_shim_family_timer, TX_TIMER_ERROR);
  TEST_ASSERT_EQ(k_ra8_err_rtos_error, ra8_esp_hosted_rtos_deinit());

  /* A live thread whose terminate is refused: the thread walk must record it. */
  reset_port();
  TEST_ASSERT_NOT_NULL(
    s_funcs._h_thread_create("t", DFLT_TASK_PRIO, DFLT_TASK_STACK_SIZE, thread_body, NULL));
  ra8_esp_hosted_tx_shim_arm(k_ra8_esp_hosted_tx_shim_family_thread, TX_THREAD_ERROR);
  TEST_ASSERT_EQ(k_ra8_err_rtos_error, ra8_esp_hosted_rtos_deinit());

  /* A live semaphore whose delete is refused: the sync walk must record it. */
  reset_port();
  TEST_ASSERT_NOT_NULL(s_funcs._h_create_semaphore(1));
  ra8_esp_hosted_tx_shim_arm(k_ra8_esp_hosted_tx_shim_family_semaphore, TX_SEMAPHORE_ERROR);
  TEST_ASSERT_EQ(k_ra8_err_rtos_error, ra8_esp_hosted_rtos_deinit());

  /* A live mutex whose delete is refused: the sync walk must record that too. */
  reset_port();
  TEST_ASSERT_NOT_NULL(s_funcs._h_create_mutex());
  ra8_esp_hosted_tx_shim_arm(k_ra8_esp_hosted_tx_shim_family_mutex, TX_MUTEX_ERROR);
  TEST_ASSERT_EQ(k_ra8_err_rtos_error, ra8_esp_hosted_rtos_deinit());
  TEST_ASSERT_EQ(false, ra8_esp_hosted_rtos_is_ready());
  TEST_END("deinit reports a refused delete");
}

/* ---------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------------
 */

int32_t main(void)
{
  test_ms_to_ticks_maps_every_class();
  test_queue_words_rounds_up_and_rejects_oversize();
  test_us_spin_iters_guards_and_clamps();
  test_slot_take_claims_then_reports_full();
  test_slot_index_rejects_null_foreign_and_freed();
  test_init_rejects_second_call_and_deinit_needs_init();
  test_init_reports_a_refused_byte_pool();
  test_bind_fills_its_rows_and_rejects_null();
  test_thread_create_bounds_cancel_and_table();
  test_timer_oneshot_and_periodic();
  test_timer_table_exhaustion();
  test_get_time_ms_extends_across_a_32_bit_rollover();
  test_sleeps_yield_and_delays();
  test_deinit_releases_every_outstanding_object();
  test_deinit_reports_a_refused_delete();
  if (ra8_esp_hosted_rtos_is_ready()) {
    (void)ra8_esp_hosted_rtos_deinit();
  }
  (void)fprintf(stderr, "[OK  ] test_ra8_esp_hosted_rtos.c\n");
  return 0;
}
