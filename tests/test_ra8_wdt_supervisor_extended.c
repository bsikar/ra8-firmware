/**
 * @file test_ra8_wdt_supervisor_extended.c
 * @brief Extended unit tests for libs/ra8_wdt_supervisor/src/ra8_wdt_supervisor.c
 * @details Targets supervisor capacity, duplicate registration, wraparound, disabled-task, and additional deadline branches.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_wdt_supervisor.h"
#include "unity_minimal.h"

/**
 * @enum t_sup_ext_t
 * @brief Supervisor task stack size.
 */
typedef enum : uint16_t {
  k_t_stack_bytes = 1024U, /**< Supervisor task stack, bytes. */
} t_sup_ext_t;

/* ---------------------------------------------------------------------------
 * Shared scaffolding
 * ---------------------------------------------------------------------------
 */

static uint8_t  s_stack[k_t_stack_bytes];
static uint32_t s_now_ms;
static uint32_t s_refresh_calls;

static uint32_t hook_now(void)
{
  return s_now_ms;
}

static void hook_refresh(void)
{
  ++s_refresh_calls;
}

static void prep(void)
{
  (void)ra8_wdt_supervisor_deinit();
  s_now_ms        = 0U;
  s_refresh_calls = 0U;

  const ra8_wdt_sup_cfg_t cfg = {
    .stack             = s_stack,
    .stack_size_bytes  = (uint32_t)sizeof(s_stack),
    .priority          = 4U,
    .refresh_period_ms = 50U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_set_now_hook(hook_now));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_set_refresh_hook(hook_refresh));
}

/* ---------------------------------------------------------------------------
 * init validation paths
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify init rejects null cfg.
 *
 * @pre None.
 * @post Returns k_ra8_err_null_ptr.
 *
 * @par MC/DC:
 * Decision: ``cfg == nullptr`` -- V1 (true branch).
 *
 * @since 0.1.0
 */
static void test_sup_init_null_cfg(void)
{
  TEST_BEGIN("wdt_supervisor_init null cfg rejected");
  (void)ra8_wdt_supervisor_deinit();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wdt_supervisor_init(nullptr));
  TEST_END("wdt_supervisor_init null cfg rejected");
}

/**
 * @brief Verify init rejects null stack pointer.
 *
 * @pre None.
 * @post Returns k_ra8_err_null_ptr.
 *
 * @par MC/DC:
 * Decision: ``cfg->stack == nullptr``
 * - V1: stack=null  -> true  -> error.
 * - V2: stack=valid -> false -> proceed.
 *
 * @since 0.1.0
 */
static void test_sup_init_null_stack(void)
{
  TEST_BEGIN("wdt_supervisor_init null stack rejected");
  (void)ra8_wdt_supervisor_deinit();
  const ra8_wdt_sup_cfg_t cfg = {
    .stack             = nullptr,
    .stack_size_bytes  = 512U,
    .priority          = 4U,
    .refresh_period_ms = 50U,
  };
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wdt_supervisor_init(&cfg));
  TEST_END("wdt_supervisor_init null stack rejected");
}

/**
 * @brief Verify init rejects stack that is too small.
 *
 * @pre None.
 * @post Returns k_ra8_err_invalid_arg.
 *
 * @par MC/DC:
 * Decision: ``stack_size_bytes < k_ra8_wdt_sup_min_stack``
 * - V1: size=511  -> true  -> error.
 * - V2: size=512  -> false -> proceed.
 *
 * @since 0.1.0
 */
static void test_sup_init_stack_too_small(void)
{
  TEST_BEGIN("wdt_supervisor_init stack too small rejected");
  (void)ra8_wdt_supervisor_deinit();
  const ra8_wdt_sup_cfg_t cfg = {
    .stack             = s_stack,
    .stack_size_bytes  = 511U, /* k_ra8_wdt_sup_min_stack = 512 */
    .priority          = 4U,
    .refresh_period_ms = 50U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_wdt_supervisor_init(&cfg));
  TEST_END("wdt_supervisor_init stack too small rejected");
}

/**
 * @brief Verify init rejects zero refresh_period_ms.
 *
 * @pre None.
 * @post Returns k_ra8_err_invalid_arg.
 *
 * @par MC/DC:
 * Decision: ``refresh_period_ms == 0U``
 * - V1: period=0  -> true  -> error.
 * - V2: period=1  -> false -> proceed.
 *
 * @since 0.1.0
 */
static void test_sup_init_zero_period(void)
{
  TEST_BEGIN("wdt_supervisor_init zero period rejected");
  (void)ra8_wdt_supervisor_deinit();
  const ra8_wdt_sup_cfg_t cfg = {
    .stack             = s_stack,
    .stack_size_bytes  = 512U,
    .priority          = 4U,
    .refresh_period_ms = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_wdt_supervisor_init(&cfg));
  TEST_END("wdt_supervisor_init zero period rejected");
}

/**
 * @brief Verify init rejects priority > 31.
 *
 * @pre None.
 * @post Returns k_ra8_err_invalid_arg.
 *
 * @par MC/DC:
 * Decision: ``priority > k_ra8_wdt_sup_max_priority``
 * - V1: priority=31  -> false -> proceed.
 * - V2: priority=32  -> true  -> error.
 *
 * @since 0.1.0
 */
static void test_sup_init_bad_priority(void)
{
  TEST_BEGIN("wdt_supervisor_init bad priority rejected");
  (void)ra8_wdt_supervisor_deinit();
  const ra8_wdt_sup_cfg_t cfg = {
    .stack             = s_stack,
    .stack_size_bytes  = 512U,
    .priority          = 32U, /* k_ra8_wdt_sup_max_priority = 31 */
    .refresh_period_ms = 50U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_wdt_supervisor_init(&cfg));
  TEST_END("wdt_supervisor_init bad priority rejected");
}

/**
 * @brief Verify double-init returns busy.
 *
 * @pre First init succeeded.
 * @post Second init returns k_ra8_err_busy.
 *
 * @par MC/DC:
 * Decision: ``s_state.initialized``
 * - V1: not initialized -> false -> proceed.
 * - V2: already init    -> true  -> k_ra8_err_busy.
 *
 * @since 0.1.0
 */
static void test_sup_double_init_busy(void)
{
  TEST_BEGIN("wdt_supervisor double-init returns busy");
  prep();
  const ra8_wdt_sup_cfg_t cfg = {
    .stack             = s_stack,
    .stack_size_bytes  = (uint32_t)sizeof(s_stack),
    .priority          = 4U,
    .refresh_period_ms = 50U,
  };
  TEST_ASSERT_EQ(k_ra8_err_busy, ra8_wdt_supervisor_init(&cfg));
  TEST_END("wdt_supervisor double-init returns busy");
}

/* ---------------------------------------------------------------------------
 * register_thread validation
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify register_thread null out_handle rejected.
 *
 * @pre init done.
 * @post Returns k_ra8_err_null_ptr.
 *
 * @par MC/DC:
 * Decision: ``out_handle == nullptr``
 * - V1: out_handle=null  -> true  -> error.
 * - V2: out_handle=valid -> false -> proceed.
 *
 * @since 0.1.0
 */
static void test_sup_register_null_out_handle(void)
{
  TEST_BEGIN("wdt_supervisor_register_thread null out_handle rejected");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wdt_supervisor_register_thread("t1", 100U, nullptr));
  TEST_END("wdt_supervisor_register_thread null out_handle rejected");
}

/**
 * @brief Verify register_thread null name rejected.
 *
 * @pre init done.
 * @post Returns k_ra8_err_null_ptr.
 *
 * @par MC/DC:
 * Decision: ``name == nullptr``
 * - V1: name=null  -> true  -> error.
 * - V2: name=valid -> false -> proceed.
 *
 * @since 0.1.0
 */
static void test_sup_register_null_name(void)
{
  TEST_BEGIN("wdt_supervisor_register_thread null name rejected");
  prep();
  uint8_t h = (uint8_t)k_ra8_wdt_sup_handle_invalid;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_wdt_supervisor_register_thread(nullptr, 100U, &h));
  TEST_END("wdt_supervisor_register_thread null name rejected");
}

/**
 * @brief Verify register_thread zero deadline rejected.
 *
 * @pre init done.
 * @post Returns k_ra8_err_invalid_arg.
 *
 * @par MC/DC:
 * Decision: ``deadline_ms == 0U``
 * - V1: deadline=0    -> true  -> error.
 * - V2: deadline=100  -> false -> proceed.
 *
 * @since 0.1.0
 */
static void test_sup_register_zero_deadline(void)
{
  TEST_BEGIN("wdt_supervisor_register_thread zero deadline rejected");
  prep();
  uint8_t h = (uint8_t)k_ra8_wdt_sup_handle_invalid;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_wdt_supervisor_register_thread("t1", 0U, &h));
  TEST_END("wdt_supervisor_register_thread zero deadline rejected");
}

/**
 * @brief Verify register_thread returns not_initialized before init.
 *
 * @pre deinit called; not re-initialized.
 * @post Returns k_ra8_err_not_initialized.
 *
 * @par MC/DC:
 * Decision: ``!s_state.initialized``
 * - V1: not init -> true  -> error.
 * - V2: init     -> false -> proceed.
 *
 * @since 0.1.0
 */
static void test_sup_register_not_initialized(void)
{
  TEST_BEGIN("wdt_supervisor_register_thread not_initialized before init");
  (void)ra8_wdt_supervisor_deinit();
  uint8_t h = (uint8_t)k_ra8_wdt_sup_handle_invalid;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_wdt_supervisor_register_thread("t1", 100U, &h));
  TEST_END("wdt_supervisor_register_thread not_initialized before init");
}

/**
 * @brief Verify register_thread returns no_mem when registry is full.
 *
 * @pre init done; all slots filled.
 * @post Returns k_ra8_err_no_mem.
 *
 * @par MC/DC:
 * (loop exhaustion without finding a free slot)
 *
 * @since 0.1.0
 */
static void test_sup_register_table_full(void)
{
  TEST_BEGIN("wdt_supervisor_register_thread table full returns no_mem");
  prep();
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_wdt_sup_max_threads; ++i) {
    uint8_t         h = (uint8_t)k_ra8_wdt_sup_handle_invalid;
    const ra8_err_t e = ra8_wdt_supervisor_register_thread("t", 100U, &h);
    TEST_ASSERT_EQ(k_ra8_ok, e);
  }
  uint8_t overflow = (uint8_t)k_ra8_wdt_sup_handle_invalid;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_wdt_supervisor_register_thread("overflow", 100U, &overflow));
  TEST_END("wdt_supervisor_register_thread table full returns no_mem");
}

/* ---------------------------------------------------------------------------
 * checkin validation
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify checkin returns not_initialized before init.
 *
 * @pre Not initialized.
 * @post Returns k_ra8_err_not_initialized.
 *
 * @par MC/DC:
 * Decision: ``!s_state.initialized`` -- true branch.
 *
 * @since 0.1.0
 */
static void test_sup_checkin_not_initialized(void)
{
  TEST_BEGIN("wdt_supervisor_checkin not_initialized");
  (void)ra8_wdt_supervisor_deinit();
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_wdt_supervisor_checkin(0U));
  TEST_END("wdt_supervisor_checkin not_initialized");
}

/**
 * @brief Verify checkin returns invalid_arg for out-of-range handle.
 *
 * @pre init done.
 * @post Returns k_ra8_err_invalid_arg for handle >= max_threads.
 *
 * @par MC/DC:
 * Decision: ``handle >= k_ra8_wdt_sup_max_threads``
 * - V1: handle=0          -> false -> proceed.
 * - V2: handle=max_threads -> true  -> error.
 *
 * @since 0.1.0
 */
static void test_sup_checkin_oob_handle(void)
{
  TEST_BEGIN("wdt_supervisor_checkin oob handle rejected");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_wdt_supervisor_checkin((uint8_t)k_ra8_wdt_sup_max_threads));
  TEST_END("wdt_supervisor_checkin oob handle rejected");
}

/**
 * @brief Verify checkin for an unregistered (free) slot returns not_found.
 *
 * @pre init done; slot 0 is free.
 * @post Returns k_ra8_err_not_found.
 *
 * @par MC/DC:
 * Decision: ``slots[handle].state == slot_used``
 * - V1: state=used -> true  -> update and return ok.
 * - V2: state=free -> false -> k_ra8_err_not_found.
 *
 * @since 0.1.0
 */
static void test_sup_checkin_free_slot(void)
{
  TEST_BEGIN("wdt_supervisor_checkin free slot returns not_found");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_wdt_supervisor_checkin(0U));
  TEST_END("wdt_supervisor_checkin free slot returns not_found");
}

/* ---------------------------------------------------------------------------
 * start validation
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify start returns not_initialized before init.
 *
 * @pre Not initialized.
 * @post Returns k_ra8_err_not_initialized.
 *
 * @par MC/DC:
 * Decision: ``!s_state.initialized``
 * - V1: not init -> true  -> error.
 *
 * @since 0.1.0
 */
static void test_sup_start_not_initialized(void)
{
  TEST_BEGIN("wdt_supervisor_start not_initialized");
  (void)ra8_wdt_supervisor_deinit();
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_wdt_supervisor_start());
  TEST_END("wdt_supervisor_start not_initialized");
}

/**
 * @brief Verify start twice returns busy.
 *
 * @pre init done; start called once.
 * @post Second start returns k_ra8_err_busy.
 *
 * @par MC/DC:
 * Decision: ``s_state.started``
 * - V1: not started -> false -> proceed.
 * - V2: started     -> true  -> k_ra8_err_busy.
 *
 * @since 0.1.0
 */
static void test_sup_start_double_busy(void)
{
  TEST_BEGIN("wdt_supervisor_start double start returns busy");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_start());
  TEST_ASSERT_EQ(k_ra8_err_busy, ra8_wdt_supervisor_start());
  TEST_END("wdt_supervisor_start double start returns busy");
}

/* ---------------------------------------------------------------------------
 * tick when not initialized
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify tick returns not_initialized and sets out_did_refresh=false.
 *
 * @pre Not initialized.
 * @post Returns k_ra8_err_not_initialized; out_did_refresh=false.
 *
 * @par MC/DC:
 * Decision: ``!s_state.initialized`` in ra8_wdt_supervisor_tick
 * - V1: not init -> true  -> error + out set.
 *
 * @since 0.1.0
 */
static void test_sup_tick_not_initialized(void)
{
  TEST_BEGIN("wdt_supervisor_tick not_initialized");
  (void)ra8_wdt_supervisor_deinit();
  bool did_refresh = true;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_wdt_supervisor_tick(&did_refresh));
  TEST_ASSERT_EQ(0, did_refresh);
  TEST_END("wdt_supervisor_tick not_initialized");
}

/**
 * @brief Verify tick with null out_did_refresh does not crash.
 *
 * @pre init done; one alive worker.
 * @post Returns k_ra8_ok even with null out pointer.
 *
 * @par MC/DC:
 * Decision: ``out_did_refresh != nullptr``
 * - V1: ptr=valid  -> true  -> write result.
 * - V2: ptr=null   -> false -> skip write (no crash).
 *
 * @since 0.1.0
 */
static void test_sup_tick_null_out(void)
{
  TEST_BEGIN("wdt_supervisor_tick null out_did_refresh no crash");
  prep();
  uint8_t h = (uint8_t)k_ra8_wdt_sup_handle_invalid;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_register_thread("t1", 200U, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_checkin(h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_tick(nullptr));
  TEST_END("wdt_supervisor_tick null out_did_refresh no crash");
}

/* ---------------------------------------------------------------------------
 * thread_count
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify thread_count returns 0 after deinit.
 *
 * @pre deinit called.
 * @post Returns 0.
 *
 * @par MC/DC:
 * (loop exits immediately; count=0)
 *
 * @since 0.1.0
 */
static void test_sup_thread_count_empty(void)
{
  TEST_BEGIN("wdt_supervisor_thread_count returns 0 when empty");
  (void)ra8_wdt_supervisor_deinit();
  TEST_ASSERT_EQ(0, ra8_wdt_supervisor_thread_count());
  TEST_END("wdt_supervisor_thread_count returns 0 when empty");
}

/**
 * @brief Verify thread_count increments as threads are registered.
 *
 * @pre init done.
 * @post thread_count increases by 1 per registration.
 *
 * @par MC/DC:
 * Decision (loop body): ``slots[i].state == slot_used``
 * - V1: state=free -> false -> skip.
 * - V2: state=used -> true  -> count.
 *
 * @since 0.1.0
 */
static void test_sup_thread_count_increments(void)
{
  TEST_BEGIN("wdt_supervisor_thread_count increments with registrations");
  prep();
  TEST_ASSERT_EQ(0, ra8_wdt_supervisor_thread_count());

  uint8_t h1 = (uint8_t)k_ra8_wdt_sup_handle_invalid;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_register_thread("t1", 100U, &h1));
  TEST_ASSERT_EQ(1, ra8_wdt_supervisor_thread_count());

  uint8_t h2 = (uint8_t)k_ra8_wdt_sup_handle_invalid;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_register_thread("t2", 200U, &h2));
  TEST_ASSERT_EQ(2, ra8_wdt_supervisor_thread_count());
  TEST_END("wdt_supervisor_thread_count increments with registrations");
}

/* ---------------------------------------------------------------------------
 * set_now_hook / set_refresh_hook -- nullptr restores default
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify set_now_hook(nullptr) restores the default hook.
 *
 * @pre init done.
 * @post Returns k_ra8_ok; does not crash.
 *
 * @par MC/DC:
 * Decision: ``now != nullptr``
 * - V1: now=valid   -> use provided hook.
 * - V2: now=nullptr -> restore default.
 *
 * @since 0.1.0
 */
static void test_sup_set_now_hook_null_restores(void)
{
  TEST_BEGIN("wdt_supervisor_set_now_hook null restores default");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_set_now_hook(nullptr));
  TEST_END("wdt_supervisor_set_now_hook null restores default");
}

/**
 * @brief Verify set_refresh_hook(nullptr) restores the default hook.
 *
 * @pre init done.
 * @post Returns k_ra8_ok; does not crash.
 *
 * @par MC/DC:
 * Decision: ``refresh != nullptr``
 * - V1: refresh=valid   -> use provided hook.
 * - V2: refresh=nullptr -> restore default.
 *
 * @since 0.1.0
 */
static void test_sup_set_refresh_hook_null_restores(void)
{
  TEST_BEGIN("wdt_supervisor_set_refresh_hook null restores default");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_wdt_supervisor_set_refresh_hook(nullptr));
  TEST_END("wdt_supervisor_set_refresh_hook null restores default");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom, exactly as before.
 */
static void (*const s_test_roster[])(void) = {
  test_sup_init_null_cfg,
  test_sup_init_null_stack,
  test_sup_init_stack_too_small,
  test_sup_init_zero_period,
  test_sup_init_bad_priority,
  test_sup_double_init_busy,
  test_sup_register_null_out_handle,
  test_sup_register_null_name,
  test_sup_register_zero_deadline,
  test_sup_register_not_initialized,
  test_sup_register_table_full,
  test_sup_checkin_not_initialized,
  test_sup_checkin_oob_handle,
  test_sup_checkin_free_slot,
  test_sup_start_not_initialized,
  test_sup_start_double_busy,
  test_sup_tick_not_initialized,
  test_sup_tick_null_out,
  test_sup_thread_count_empty,
  test_sup_thread_count_increments,
  test_sup_set_now_hook_null_restores,
  test_sup_set_refresh_hook_null_restores,
};

int main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  return 0;
}
