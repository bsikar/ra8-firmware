/**
 * @file test_fw_os.c
 * @brief Conformance vectors for the `fw_os` port against its host binding.
 *
 * @par Tag
 * [Ring 3 / Test] {World: NS}
 *
 * @details
 * The OSAL seam of RA8FW-299 declared a contract and compiled it; nothing proved a
 * binding could satisfy it. These vectors drive the first binding
 * (`tests/support/src/fw_os_host_test.c`) through every MUST declaration and
 * assert the documented answer, including the failure answers, so the contract
 * is specified by a running test rather than by prose alone.
 *
 * Two things are deliberately asserted about the binding's own honesty: a
 * created thread does not run until a test runs it, and a wait that cannot be
 * satisfied reports its failure rather than blocking a single-threaded build
 * forever.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fw_os.h"
#include "fw_os_host_test.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/** @brief Sizes the vectors use for caller-owned thread stacks. */
enum : uint32_t {
  k_test_stack_bytes = 256U,
  k_test_sleep_ms    = 25U,
  k_test_sem_initial = 2U,
};

/** @brief Observable effect of a thread body the vectors run by hand. */
typedef struct {
  uint32_t calls; /* How many times the body ran.  */
  void    *seen;  /* Argument the body was handed. */
} test_body_record_t;

static test_body_record_t s_body;

/**
 * @brief Thread body that records it ran and with what.
 *
 * @param[in] arg Argument the binding recorded at create time.
 *
 * @return Nothing.
 *
 * @pre None.
 * @post The call count and the seen argument are updated.
 * @note Not thread-safe; the host build is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_body(void *arg)
{
  ++s_body.calls;
  s_body.seen = arg;
}

/**
 * @brief A mutex answers lock, relock, unlock and their failures.
 *
 * @return Nothing.
 *
 * @pre None.
 * @post The mutex under test is deinitialised.
 * @note Not thread-safe; the host build is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_mutex(void)
{
  TEST_BEGIN("fw_os mutex: lock, contention, ownership");
  fw_os_mutex_t mutex = {};

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_os_mutex_init(nullptr, false));
  TEST_ASSERT_EQ(k_ra8_ok, fw_os_mutex_init(&mutex, false));

  /* An uninitialised block is not a mutex, and the binding says so. */
  fw_os_mutex_t raw = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_os_mutex_lock(&raw, K_FW_OS_NO_WAIT));

  /* Unlocking what nobody holds is the caller's error, not a silent no-op. */
  TEST_ASSERT_EQ(k_ra8_err_access_denied, fw_os_mutex_unlock(&mutex));

  TEST_ASSERT_EQ(k_ra8_ok, fw_os_mutex_lock(&mutex, K_FW_OS_WAIT_FOREVER));

  /* Held and non-recursive: the bound decides which failure the caller gets. */
  TEST_ASSERT_EQ(k_ra8_err_would_block, fw_os_mutex_lock(&mutex, K_FW_OS_NO_WAIT));
  TEST_ASSERT_EQ(k_ra8_err_timeout, fw_os_mutex_lock(&mutex, k_test_sleep_ms));
  TEST_ASSERT_EQ(k_ra8_err_timeout, fw_os_mutex_lock(&mutex, K_FW_OS_WAIT_FOREVER));

  /* Still held, so releasing the mutex itself has to be refused. */
  TEST_ASSERT_EQ(k_ra8_err_busy, fw_os_mutex_deinit(&mutex));

  TEST_ASSERT_EQ(k_ra8_ok, fw_os_mutex_unlock(&mutex));
  TEST_ASSERT_EQ(k_ra8_ok, fw_os_mutex_deinit(&mutex));
  TEST_END("fw_os mutex: lock, contention, ownership");
}

/**
 * @brief A recursive mutex lets its owner relock and needs a matching unlock.
 *
 * @return Nothing.
 *
 * @pre None.
 * @post The mutex under test is deinitialised.
 * @note Not thread-safe; the host build is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_recursive_mutex(void)
{
  TEST_BEGIN("fw_os mutex: recursive depth");
  fw_os_mutex_t mutex = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_os_mutex_init(&mutex, true));

  TEST_ASSERT_EQ(k_ra8_ok, fw_os_mutex_lock(&mutex, K_FW_OS_NO_WAIT));
  TEST_ASSERT_EQ(k_ra8_ok, fw_os_mutex_lock(&mutex, K_FW_OS_NO_WAIT));

  /* One unlock leaves it held once, so it is still not free to release. */
  TEST_ASSERT_EQ(k_ra8_ok, fw_os_mutex_unlock(&mutex));
  TEST_ASSERT_EQ(k_ra8_err_busy, fw_os_mutex_deinit(&mutex));

  TEST_ASSERT_EQ(k_ra8_ok, fw_os_mutex_unlock(&mutex));
  TEST_ASSERT_EQ(k_ra8_err_access_denied, fw_os_mutex_unlock(&mutex));
  TEST_ASSERT_EQ(k_ra8_ok, fw_os_mutex_deinit(&mutex));
  TEST_END("fw_os mutex: recursive depth");
}

/**
 * @brief A semaphore counts down, refuses an empty take and counts back up.
 *
 * @return Nothing.
 *
 * @pre None.
 * @post The semaphore under test is deinitialised.
 * @note Not thread-safe; the host build is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_semaphore(void)
{
  TEST_BEGIN("fw_os semaphore: counts and empty waits");
  fw_os_sem_t sem = {};

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_os_sem_take(nullptr, K_FW_OS_NO_WAIT));
  TEST_ASSERT_EQ(k_ra8_ok, fw_os_sem_init(&sem, (uint32_t)k_test_sem_initial));

  fw_os_sem_t raw = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_os_sem_take(&raw, K_FW_OS_NO_WAIT));

  TEST_ASSERT_EQ(k_ra8_ok, fw_os_sem_take(&sem, K_FW_OS_NO_WAIT));
  TEST_ASSERT_EQ(k_ra8_ok, fw_os_sem_take(&sem, K_FW_OS_WAIT_FOREVER));

  /* Empty: the bound decides the failure, and neither answer blocks. */
  TEST_ASSERT_EQ(k_ra8_err_would_block, fw_os_sem_take(&sem, K_FW_OS_NO_WAIT));
  TEST_ASSERT_EQ(k_ra8_err_timeout, fw_os_sem_take(&sem, k_test_sleep_ms));
  TEST_ASSERT_EQ(k_ra8_err_timeout, fw_os_sem_take(&sem, K_FW_OS_WAIT_FOREVER));

  TEST_ASSERT_EQ(k_ra8_ok, fw_os_sem_give(&sem));
  TEST_ASSERT_EQ(k_ra8_ok, fw_os_sem_take(&sem, K_FW_OS_NO_WAIT));
  TEST_ASSERT_EQ(k_ra8_ok, fw_os_sem_deinit(&sem));
  TEST_END("fw_os semaphore: counts and empty waits");
}

/** @brief Caller-owned stacks and handles the thread vectors share. */
typedef struct {
  uint64_t stacks[k_fw_os_host_test_max_threads + 1U][k_test_stack_bytes / sizeof(uint64_t)];
  fw_os_thread_t handles[k_fw_os_host_test_max_threads + 1U];
} test_thread_pool_t;

static test_thread_pool_t s_pool;

/**
 * @brief Build a valid thread config pointing at one of the shared stacks.
 *
 * @param[in] slot Index of the stack to hand the thread.
 *
 * @return A config the binding must accept.
 *
 * @pre @p slot is within the pool.
 * @post Nothing is changed.
 * @note Not thread-safe; the host build is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static fw_os_thread_cfg_t internal_cfg_for(uint32_t slot)
{
  return (fw_os_thread_cfg_t){
      .name        = "vector",
      .entry       = internal_body,
      .arg         = &s_body,
      .stack       = &s_pool.stacks[slot][0],
      .stack_bytes = k_test_stack_bytes,
      .priority    = k_fw_os_priority_normal,
  };
}

/**
 * @brief Thread create refuses a malformed request rather than half-doing it.
 *
 * @return Nothing.
 *
 * @pre None.
 * @post No thread exists.
 * @note Not thread-safe; the host build is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_thread_args(void)
{
  TEST_BEGIN("fw_os threads: rejected requests");
  fw_os_host_test_reset();
  s_body                       = (test_body_record_t){};
  const fw_os_thread_cfg_t cfg = internal_cfg_for(0U);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_os_thread_create(nullptr, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_os_thread_create(&s_pool.handles[0], nullptr));

  /* A thread with no body or no stack is not a thread. */
  fw_os_thread_cfg_t broken = cfg;
  broken.entry              = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_os_thread_create(&s_pool.handles[0], &broken));
  broken       = cfg;
  broken.stack = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_os_thread_create(&s_pool.handles[0], &broken));
  broken             = cfg;
  broken.stack_bytes = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_os_thread_create(&s_pool.handles[0], &broken));

  /* Every one of those was refused, so nothing was recorded. */
  TEST_ASSERT_EQ(0, fw_os_host_test_thread_count());
  TEST_END("fw_os threads: rejected requests");
}

/**
 * @brief Threads fill the pool, refuse when full, run only on request, and free.
 *
 * @return Nothing.
 *
 * @pre None.
 * @post Every thread the vector created has been deleted.
 * @note Not thread-safe; the host build is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_threads(void)
{
  TEST_BEGIN("fw_os threads: capacity, running, delete");
  fw_os_host_test_reset();
  s_body = (test_body_record_t){};

  for (uint32_t slot = 0U; slot < (uint32_t)k_fw_os_host_test_max_threads; ++slot) {
    const fw_os_thread_cfg_t cfg = internal_cfg_for(slot);
    TEST_ASSERT_EQ(k_ra8_ok, fw_os_thread_create(&s_pool.handles[slot], &cfg));
  }
  TEST_ASSERT_EQ((int64_t)k_fw_os_host_test_max_threads, fw_os_host_test_thread_count());

  /* Full, and the binding says so rather than overwriting a live thread. */
  const uint32_t           spare     = (uint32_t)k_fw_os_host_test_max_threads;
  const fw_os_thread_cfg_t spare_cfg = internal_cfg_for(spare);
  TEST_ASSERT_EQ(k_ra8_err_no_mem, fw_os_thread_create(&s_pool.handles[spare], &spare_cfg));

  /* Nothing scheduled any of them: the body has not run. */
  TEST_ASSERT_EQ(0, s_body.calls);
  TEST_ASSERT_EQ(k_ra8_ok, fw_os_host_test_run_thread(0U));
  TEST_ASSERT_EQ(1, s_body.calls);
  TEST_ASSERT(s_body.seen == (void *)&s_body);
  TEST_ASSERT_EQ(k_ra8_err_not_found, fw_os_host_test_run_thread(spare));

  TEST_ASSERT_EQ(k_ra8_ok, fw_os_thread_delete(&s_pool.handles[0]));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_os_thread_delete(&s_pool.handles[0]));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_os_thread_delete(nullptr));

  /* A freed slot is reusable, which is what makes delete worth calling. */
  const fw_os_thread_cfg_t reuse = internal_cfg_for(0U);
  TEST_ASSERT_EQ(k_ra8_ok, fw_os_thread_create(&s_pool.handles[0], &reuse));
  for (uint32_t slot = 0U; slot < (uint32_t)k_fw_os_host_test_max_threads; ++slot) {
    TEST_ASSERT_EQ(k_ra8_ok, fw_os_thread_delete(&s_pool.handles[slot]));
  }
  TEST_ASSERT_EQ(0, fw_os_host_test_thread_count());
  TEST_END("fw_os threads: capacity, running, delete");
}

/**
 * @brief Time reads are monotonic, sleep advances them, yield is observable.
 *
 * @return Nothing.
 *
 * @pre None.
 * @post The binding's clock has advanced by the slept milliseconds.
 * @note Not thread-safe; the host build is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_time(void)
{
  TEST_BEGIN("fw_os time: uptime, sleep, tick rate, yield");
  fw_os_host_test_reset();

  TEST_ASSERT_EQ(0, fw_os_uptime_ms());
  TEST_ASSERT(fw_os_tick_hz() > 0U);

  const uint32_t before = fw_os_uptime_ms();
  fw_os_thread_sleep_ms(k_test_sleep_ms);
  TEST_ASSERT_EQ((int64_t)before + (int64_t)k_test_sleep_ms, fw_os_uptime_ms());

  /* A zero sleep is a yield in the port's words, so it must not lose time. */
  const uint32_t held = fw_os_uptime_ms();
  fw_os_thread_sleep_ms(0U);
  TEST_ASSERT_EQ((int64_t)held, fw_os_uptime_ms());

  TEST_ASSERT_EQ(0, fw_os_host_test_yield_count());
  fw_os_thread_yield();
  TEST_ASSERT_EQ(1, fw_os_host_test_yield_count());
  TEST_END("fw_os time: uptime, sleep, tick rate, yield");
}

/**
 * @test test_forced_failure_is_one_shot
 *
 * @brief An armed failure fires once, on its own call, and then clears.
 */
static void test_forced_failure_is_one_shot(void)
{
  TEST_BEGIN("fw_os host: a forced failure fires once and clears");
  fw_os_host_test_reset();

  fw_os_mutex_t mutex = {0};
  TEST_ASSERT(fw_os_host_test_failure_armed() == false);
  TEST_ASSERT(fw_os_host_test_fail_next(k_fw_os_host_test_call_mutex_init,
                                        k_ra8_err_rtos_mutex) == k_ra8_ok);
  TEST_ASSERT(fw_os_host_test_failure_armed() == true);

  TEST_ASSERT(fw_os_mutex_init(&mutex, false) == k_ra8_err_rtos_mutex);
  TEST_ASSERT(fw_os_host_test_failure_armed() == false);
  TEST_ASSERT(fw_os_mutex_init(&mutex, false) == k_ra8_ok);
  TEST_ASSERT(fw_os_mutex_deinit(&mutex) == k_ra8_ok);

  TEST_END("fw_os host: a forced failure fires once and clears");
}

/**
 * @test test_forced_failure_hits_only_its_own_call
 *
 * @brief Arming one call leaves every other call working.
 */
static void test_forced_failure_hits_only_its_own_call(void)
{
  TEST_BEGIN("fw_os host: a forced failure hits only the call it names");
  fw_os_host_test_reset();

  fw_os_mutex_t mutex = {0};
  fw_os_sem_t   sem   = {0};
  TEST_ASSERT(fw_os_host_test_fail_next(k_fw_os_host_test_call_sem_take,
                                        k_ra8_err_rtos_semaphore) == k_ra8_ok);

  TEST_ASSERT(fw_os_mutex_init(&mutex, false) == k_ra8_ok);
  TEST_ASSERT(fw_os_mutex_lock(&mutex, K_FW_OS_NO_WAIT) == k_ra8_ok);
  TEST_ASSERT(fw_os_mutex_unlock(&mutex) == k_ra8_ok);
  TEST_ASSERT(fw_os_sem_init(&sem, 1U) == k_ra8_ok);
  TEST_ASSERT(fw_os_host_test_failure_armed() == true);

  TEST_ASSERT(fw_os_sem_take(&sem, K_FW_OS_NO_WAIT) == k_ra8_err_rtos_semaphore);
  TEST_ASSERT(fw_os_sem_take(&sem, K_FW_OS_NO_WAIT) == k_ra8_ok);

  TEST_ASSERT(fw_os_sem_deinit(&sem) == k_ra8_ok);
  TEST_ASSERT(fw_os_mutex_deinit(&mutex) == k_ra8_ok);
  TEST_END("fw_os host: a forced failure hits only the call it names");
}

/**
 * @test test_forced_failure_changes_no_state
 *
 * @brief A forced failure returns before the call touches anything.
 */
static void test_forced_failure_changes_no_state(void)
{
  TEST_BEGIN("fw_os host: a forced failure leaves state untouched");
  fw_os_host_test_reset();

  static uint8_t stack[256];
  const fw_os_thread_cfg_t cfg = {
    .name        = "forced",
    .entry       = nullptr,
    .arg         = nullptr,
    .stack       = stack,
    .stack_bytes = sizeof stack,
    .priority    = k_fw_os_priority_normal,
  };
  fw_os_thread_t thread = {0};

  TEST_ASSERT(fw_os_host_test_thread_count() == 0U);
  TEST_ASSERT(fw_os_host_test_fail_next(k_fw_os_host_test_call_thread_create,
                                        k_ra8_err_rtos_thread_create) == k_ra8_ok);
  TEST_ASSERT(fw_os_thread_create(&thread, &cfg) == k_ra8_err_rtos_thread_create);
  TEST_ASSERT(fw_os_host_test_thread_count() == 0U);

  TEST_END("fw_os host: a forced failure leaves state untouched");
}

/**
 * @test test_forced_failure_rejects_a_useless_arming
 *
 * @brief Arming success, or a call outside the enum, is refused.
 */
static void test_forced_failure_rejects_a_useless_arming(void)
{
  TEST_BEGIN("fw_os host: arming refuses k_ra8_ok and an unknown call");
  fw_os_host_test_reset();

  TEST_ASSERT(fw_os_host_test_fail_next(k_fw_os_host_test_call_mutex_lock,
                                        k_ra8_ok) == k_ra8_err_invalid_arg);
  TEST_ASSERT(fw_os_host_test_failure_armed() == false);

  const fw_os_host_test_call_t past_end =
    (fw_os_host_test_call_t)((uint32_t)k_fw_os_host_test_call_sem_give + 1U);
  TEST_ASSERT(fw_os_host_test_fail_next(past_end, k_ra8_err_rtos_error) ==
              k_ra8_err_invalid_arg);
  TEST_ASSERT(fw_os_host_test_failure_armed() == false);

  TEST_END("fw_os host: arming refuses k_ra8_ok and an unknown call");
}

/**
 * @test test_reset_disarms_a_forced_failure
 *
 * @brief Reset clears an arming, so no case leaks into the next.
 */
static void test_reset_disarms_a_forced_failure(void)
{
  TEST_BEGIN("fw_os host: reset disarms a forced failure");
  fw_os_host_test_reset();

  TEST_ASSERT(fw_os_host_test_fail_next(k_fw_os_host_test_call_mutex_init,
                                        k_ra8_err_rtos_mutex) == k_ra8_ok);
  fw_os_host_test_reset();
  TEST_ASSERT(fw_os_host_test_failure_armed() == false);

  fw_os_mutex_t mutex = {0};
  TEST_ASSERT(fw_os_mutex_init(&mutex, false) == k_ra8_ok);
  TEST_ASSERT(fw_os_mutex_deinit(&mutex) == k_ra8_ok);

  TEST_END("fw_os host: reset disarms a forced failure");
}

/**
 * @brief A reset returns the binding to its just-started state.
 *
 * @return Nothing.
 *
 * @pre None.
 * @post The binding holds no thread and reports zero uptime.
 * @note Not thread-safe; the host build is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_reset(void)
{
  TEST_BEGIN("fw_os host binding: reset");
  static uint64_t    stack[k_test_stack_bytes / sizeof(uint64_t)];
  fw_os_thread_t     thread = {};
  fw_os_thread_cfg_t cfg    = {
       .name        = "reset",
       .entry       = internal_body,
       .arg         = nullptr,
       .stack       = &stack[0],
       .stack_bytes = k_test_stack_bytes,
       .priority    = k_fw_os_priority_low,
  };
  TEST_ASSERT_EQ(k_ra8_ok, fw_os_thread_create(&thread, &cfg));
  fw_os_thread_sleep_ms(k_test_sleep_ms);
  fw_os_thread_yield();

  fw_os_host_test_reset();
  TEST_ASSERT_EQ(0, fw_os_host_test_thread_count());
  TEST_ASSERT_EQ(0, fw_os_uptime_ms());
  TEST_ASSERT_EQ(0, fw_os_host_test_yield_count());
  TEST_END("fw_os host binding: reset");
}

int main(void)
{
  internal_check_mutex();
  internal_check_recursive_mutex();
  internal_check_semaphore();
  internal_check_thread_args();
  internal_check_threads();
  internal_check_time();
  internal_check_reset();
  test_forced_failure_is_one_shot();
  test_forced_failure_hits_only_its_own_call();
  test_forced_failure_changes_no_state();
  test_forced_failure_rejects_a_useless_arming();
  test_reset_disarms_a_forced_failure();
  return 0;
}
