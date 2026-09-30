/**
 * @file fw_os_host_test.c
 * @brief Single-threaded host binding of the `fw_os` port.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 3 / Test] {World: NS}
 *
 * @details
 * Implements every MUST declaration of `libs/if/inc/fw_os.h` and declines the
 * capability-gated queue block, which is how a binding says it does not carry
 * an optional surface. See `fw_os_host_test.h` for why a wait never blocks
 * here and why a created thread never runs by itself.
 *
 * The control blocks live in the caller's storage, as the port requires, so
 * each one is placed inside the `uint64_t storage[]` the caller owns and the
 * fit is asserted at compile time rather than trusted.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "fw_os_host_test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"

/** @brief Tick rate this binding reports, so a millisecond is one tick. */
enum : uint32_t {
  k_fw_os_host_test_tick_hz = 1000U,
};

/** @brief Sentinels telling an initialised control block from raw storage. */
typedef enum : uint32_t {
  k_fw_os_host_test_mutex_magic = 0x6D7A4F53U, /* "mzOS" */
  k_fw_os_host_test_sem_magic   = 0x736D4F53U, /* "smOS" */
} fw_os_host_test_magic_t;

/** @brief Mutex control block, placed in the caller's storage. */
typedef struct {
  uint32_t magic;     /* Initialised sentinel.             */
  uint32_t depth;     /* Recursive lock depth, 0 = free.   */
  bool     recursive; /* Owner may relock without waiting. */
} fw_os_host_test_mutex_t;

/** @brief Semaphore control block, placed in the caller's storage. */
typedef struct {
  uint32_t magic; /* Initialised sentinel. */
  uint32_t count; /* Available counts.     */
} fw_os_host_test_sem_t;

/** @brief One recorded, never-scheduled thread. */
typedef struct {
  void (*entry)(void *arg); /* Body, run only on request. */
  void *arg;                /* Recorded argument.         */
  bool  live;               /* Slot holds a thread.       */
} fw_os_host_test_thread_t;

/** @brief Binding state a single-threaded build can keep in one place. */
typedef struct {
  fw_os_host_test_thread_t threads[k_fw_os_host_test_max_threads]; /* Recorded threads. */
  uint32_t                 uptime_ms;                              /* Injected clock.   */
  uint32_t                 yields;                                 /* Yield count.      */
  fw_os_host_test_call_t   forced_call;                            /* Armed call.       */
  ra8_err_t                forced_err;                             /* Its error.        */
} fw_os_host_test_state_t;

static fw_os_host_test_state_t s_state;

static_assert(sizeof(fw_os_host_test_mutex_t) <= sizeof(fw_os_mutex_t),
              "the mutex control block must fit the caller-owned storage");
static_assert(sizeof(fw_os_host_test_sem_t) <= sizeof(fw_os_sem_t),
              "the semaphore control block must fit the caller-owned storage");
static_assert(alignof(fw_os_host_test_mutex_t) <= alignof(fw_os_mutex_t),
              "the caller-owned storage must be aligned for the mutex block");
static_assert(alignof(fw_os_host_test_sem_t) <= alignof(fw_os_sem_t),
              "the caller-owned storage must be aligned for the semaphore block");

/**
 * @brief View caller-owned mutex storage as this binding's control block.
 *
 * @param[in] mutex Caller-owned storage, never NULL.
 *
 * @return Pointer to the control block inside @p mutex.
 *
 * @pre @p mutex is non-NULL.
 * @post Storage is unchanged.
 * @note Not thread-safe; the host build is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static fw_os_host_test_mutex_t *internal_mutex_block(fw_os_mutex_t *mutex)
{
  return (fw_os_host_test_mutex_t *)(void *)&mutex->storage[0];
}

/**
 * @brief View caller-owned semaphore storage as this binding's control block.
 *
 * @param[in] sem Caller-owned storage, never NULL.
 *
 * @return Pointer to the control block inside @p sem.
 *
 * @pre @p sem is non-NULL.
 * @post Storage is unchanged.
 * @note Not thread-safe; the host build is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static fw_os_host_test_sem_t *internal_sem_block(fw_os_sem_t *sem)
{
  return (fw_os_host_test_sem_t *)(void *)&sem->storage[0];
}

/**
 * @brief Consume the armed failure when it names @p call.
 *
 * @details
 * One-shot. Called first in every seam call that can fail, before the call
 * touches any state, so a forced failure leaves the binding as it was.
 *
 * @param[in]  call    The seam call asking.
 * @param[out] out_err Set to the forced error when this returns true.
 *
 * @return True when @p call was armed and the arming has been consumed.
 *
 * @pre @p out_err is non-NULL.
 * @post The arming is cleared when it matched @p call.
 * @note Not thread-safe; the host build is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_forced(fw_os_host_test_call_t call, ra8_err_t *out_err)
{
  if (s_state.forced_call != call) {
    return false;
  }
  s_state.forced_call = k_fw_os_host_test_call_none;
  *out_err            = s_state.forced_err;
  return true;
}

/**
 * @brief Report the failure a wait that cannot be satisfied deserves.
 *
 * @details
 * Nothing else can run here, so a wait that fails now fails for good. The
 * caller's bound only decides which honest answer it gets.
 *
 * @param[in] timeout_ms The caller's bound.
 *
 * @return Result code.
 * @retval k_ra8_err_would_block ::K_FW_OS_NO_WAIT was asked for.
 * @retval k_ra8_err_timeout Any other bound, including wait-forever.
 *
 * @pre None.
 * @post Binding state is unchanged.
 * @note Not thread-safe; the host build is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_wait_failed(uint32_t timeout_ms)
{
  return (timeout_ms == K_FW_OS_NO_WAIT) ? k_ra8_err_would_block : k_ra8_err_timeout;
}

void fw_os_host_test_reset(void)
{
  s_state = (fw_os_host_test_state_t){};
}

ra8_err_t fw_os_host_test_fail_next(fw_os_host_test_call_t call, ra8_err_t err)
{
  if (err == k_ra8_ok) {
    return k_ra8_err_invalid_arg;
  }
  if (call > k_fw_os_host_test_call_sem_give) {
    return k_ra8_err_invalid_arg;
  }
  s_state.forced_call = call;
  s_state.forced_err  = err;
  return k_ra8_ok;
}

bool fw_os_host_test_failure_armed(void)
{
  return s_state.forced_call != k_fw_os_host_test_call_none;
}

ra8_err_t fw_os_host_test_run_thread(uint32_t index)
{
  if ((index >= (uint32_t)k_fw_os_host_test_max_threads) || !s_state.threads[index].live) {
    return k_ra8_err_not_found;
  }
  s_state.threads[index].entry(s_state.threads[index].arg);
  return k_ra8_ok;
}

uint32_t fw_os_host_test_thread_count(void)
{
  uint32_t live = 0U;
  for (uint32_t slot = 0U; slot < (uint32_t)k_fw_os_host_test_max_threads; ++slot) {
    if (s_state.threads[slot].live) {
      ++live;
    }
  }
  return live;
}

uint32_t fw_os_host_test_yield_count(void)
{
  return s_state.yields;
}

ra8_err_t fw_os_thread_create(fw_os_thread_t *thread, const fw_os_thread_cfg_t *cfg)
{
  ra8_err_t forced = k_ra8_ok;
  if (internal_forced(k_fw_os_host_test_call_thread_create, &forced)) {
    return forced;
  }

  if ((thread == nullptr) || (cfg == nullptr) || (cfg->entry == nullptr) ||
      (cfg->stack == nullptr) || (cfg->stack_bytes == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  for (uint32_t slot = 0U; slot < (uint32_t)k_fw_os_host_test_max_threads; ++slot) {
    if (s_state.threads[slot].live) {
      continue;
    }
    s_state.threads[slot].entry = cfg->entry;
    s_state.threads[slot].arg   = cfg->arg;
    s_state.threads[slot].live  = true;
    thread->storage[0]          = (uint64_t)slot + 1ULL;
    return k_ra8_ok;
  }
  return k_ra8_err_no_mem;
}

ra8_err_t fw_os_thread_delete(fw_os_thread_t *thread)
{
  ra8_err_t forced = k_ra8_ok;
  if (internal_forced(k_fw_os_host_test_call_thread_delete, &forced)) {
    return forced;
  }

  if (thread == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  const uint64_t handle = thread->storage[0];
  if ((handle == 0ULL) || (handle > (uint64_t)k_fw_os_host_test_max_threads)) {
    return k_ra8_err_invalid_state;
  }
  const uint32_t slot = (uint32_t)(handle - 1ULL);
  if (!s_state.threads[slot].live) {
    return k_ra8_err_invalid_state;
  }
  s_state.threads[slot] = (fw_os_host_test_thread_t){};
  thread->storage[0]    = 0ULL;
  return k_ra8_ok;
}

void fw_os_thread_sleep_ms(uint32_t ms)
{
  s_state.uptime_ms += ms;
}

void fw_os_thread_yield(void)
{
  ++s_state.yields;
}

ra8_err_t fw_os_mutex_init(fw_os_mutex_t *mutex, bool recursive)
{
  ra8_err_t forced = k_ra8_ok;
  if (internal_forced(k_fw_os_host_test_call_mutex_init, &forced)) {
    return forced;
  }

  if (mutex == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  fw_os_host_test_mutex_t *block = internal_mutex_block(mutex);
  *block                         = (fw_os_host_test_mutex_t){
                             .magic     = (uint32_t)k_fw_os_host_test_mutex_magic,
                             .depth     = 0U,
                             .recursive = recursive,
  };
  return k_ra8_ok;
}

ra8_err_t fw_os_mutex_deinit(fw_os_mutex_t *mutex)
{
  ra8_err_t forced = k_ra8_ok;
  if (internal_forced(k_fw_os_host_test_call_mutex_deinit, &forced)) {
    return forced;
  }

  if (mutex == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  fw_os_host_test_mutex_t *block = internal_mutex_block(mutex);
  if (block->magic != (uint32_t)k_fw_os_host_test_mutex_magic) {
    return k_ra8_err_invalid_state;
  }
  if (block->depth != 0U) {
    return k_ra8_err_busy;
  }
  *block = (fw_os_host_test_mutex_t){};
  return k_ra8_ok;
}

ra8_err_t fw_os_mutex_lock(fw_os_mutex_t *mutex, uint32_t timeout_ms)
{
  ra8_err_t forced = k_ra8_ok;
  if (internal_forced(k_fw_os_host_test_call_mutex_lock, &forced)) {
    return forced;
  }

  if (mutex == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  fw_os_host_test_mutex_t *block = internal_mutex_block(mutex);
  if (block->magic != (uint32_t)k_fw_os_host_test_mutex_magic) {
    return k_ra8_err_invalid_state;
  }
  if ((block->depth != 0U) && !block->recursive) {
    return internal_wait_failed(timeout_ms);
  }
  ++block->depth;
  return k_ra8_ok;
}

ra8_err_t fw_os_mutex_unlock(fw_os_mutex_t *mutex)
{
  ra8_err_t forced = k_ra8_ok;
  if (internal_forced(k_fw_os_host_test_call_mutex_unlock, &forced)) {
    return forced;
  }

  if (mutex == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  fw_os_host_test_mutex_t *block = internal_mutex_block(mutex);
  if (block->magic != (uint32_t)k_fw_os_host_test_mutex_magic) {
    return k_ra8_err_invalid_state;
  }
  if (block->depth == 0U) {
    return k_ra8_err_access_denied;
  }
  --block->depth;
  return k_ra8_ok;
}

ra8_err_t fw_os_sem_init(fw_os_sem_t *sem, uint32_t initial_count)
{
  ra8_err_t forced = k_ra8_ok;
  if (internal_forced(k_fw_os_host_test_call_sem_init, &forced)) {
    return forced;
  }

  if (sem == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  fw_os_host_test_sem_t *block = internal_sem_block(sem);
  *block                       = (fw_os_host_test_sem_t){
                            .magic = (uint32_t)k_fw_os_host_test_sem_magic,
                            .count = initial_count,
  };
  return k_ra8_ok;
}

ra8_err_t fw_os_sem_deinit(fw_os_sem_t *sem)
{
  ra8_err_t forced = k_ra8_ok;
  if (internal_forced(k_fw_os_host_test_call_sem_deinit, &forced)) {
    return forced;
  }

  if (sem == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  fw_os_host_test_sem_t *block = internal_sem_block(sem);
  if (block->magic != (uint32_t)k_fw_os_host_test_sem_magic) {
    return k_ra8_err_invalid_state;
  }
  *block = (fw_os_host_test_sem_t){};
  return k_ra8_ok;
}

ra8_err_t fw_os_sem_take(fw_os_sem_t *sem, uint32_t timeout_ms)
{
  ra8_err_t forced = k_ra8_ok;
  if (internal_forced(k_fw_os_host_test_call_sem_take, &forced)) {
    return forced;
  }

  if (sem == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  fw_os_host_test_sem_t *block = internal_sem_block(sem);
  if (block->magic != (uint32_t)k_fw_os_host_test_sem_magic) {
    return k_ra8_err_invalid_state;
  }
  if (block->count == 0U) {
    return internal_wait_failed(timeout_ms);
  }
  --block->count;
  return k_ra8_ok;
}

ra8_err_t fw_os_sem_give(fw_os_sem_t *sem)
{
  ra8_err_t forced = k_ra8_ok;
  if (internal_forced(k_fw_os_host_test_call_sem_give, &forced)) {
    return forced;
  }

  if (sem == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  fw_os_host_test_sem_t *block = internal_sem_block(sem);
  if (block->magic != (uint32_t)k_fw_os_host_test_sem_magic) {
    return k_ra8_err_invalid_state;
  }
  ++block->count;
  return k_ra8_ok;
}

uint32_t fw_os_uptime_ms(void)
{
  return s_state.uptime_ms;
}

uint32_t fw_os_tick_hz(void)
{
  return (uint32_t)k_fw_os_host_test_tick_hz;
}
