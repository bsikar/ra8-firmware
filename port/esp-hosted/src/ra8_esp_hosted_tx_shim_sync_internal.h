/**
 * @file port/esp-hosted/src/ra8_esp_hosted_tx_shim_sync_internal.h
 * @brief Synchronisation, thread, timer and tick half of the ThreadX model.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * The second half of the host-build ThreadX model described in
 * ``ra8_esp_hosted_tx_shim_internal.h``. That header owns the shared state,
 * the type aliases, the status macros and the byte-pool and queue models;
 * this one owns semaphores, mutexes, threads, timers and the tick clock. The
 * split exists because the two halves together exceed the project's
 * thousand-line file cap, and it falls where the port itself already divides:
 * ``ra8_esp_hosted_rtos_pool.c`` uses only the first half.
 *
 * Including this header is always sufficient on its own -- it pulls in the
 * base header, so no translation unit has to remember the ordering.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include "ra8_esp_hosted_tx_shim_internal.h"

/**
 * @brief Host model of ``tx_semaphore_create``.
 * @details Stores the initial instance count verbatim; ThreadX imposes no
 * ceiling, so neither does the model.
 * @param[out] semaphore_ptr Semaphore control block to initialise.
 * @param[in] name_ptr Diagnostic name; accepted for signature shape.
 * @param[in] initial_count Instance count the semaphore starts with.
 * @return ThreadX status code.
 * @retval TX_SUCCESS The semaphore is ready.
 * @retval TX_SEMAPHORE_ERROR ``semaphore_ptr`` was null.
 * @pre ``semaphore_ptr`` is not already a live semaphore.
 * @pre The caller owns the control block's storage.
 * @post The count equals ``initial_count``.
 * @post The semaphore family call counter has advanced.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline UINT
/* NOLINTNEXTLINE(readability-non-const-parameter) -- ThreadX API pins it non-const. */
tx_semaphore_create(TX_SEMAPHORE* semaphore_ptr, CHAR* name_ptr, ULONG initial_count)
{
  UINT injected = TX_SUCCESS;
  (void)name_ptr;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_semaphore, &injected)) {
    return injected;
  }
  if (semaphore_ptr == nullptr) {
    return TX_SEMAPHORE_ERROR;
  }
  semaphore_ptr->magic     = (uint32_t)k_ra8_esp_hosted_tx_shim_magic;
  semaphore_ptr->count     = initial_count;
  semaphore_ptr->last_wait = 0U;
  return TX_SUCCESS;
}

/**
 * @brief Host model of ``tx_semaphore_delete``.
 * @details Clears the sentinel so later takes and posts are rejected.
 * @param[in,out] semaphore_ptr Semaphore to delete.
 * @return ThreadX status code.
 * @retval TX_SUCCESS The semaphore was deleted.
 * @retval TX_SEMAPHORE_ERROR ``semaphore_ptr`` was null or never created.
 * @pre ``semaphore_ptr`` came from ``tx_semaphore_create``.
 * @pre Nothing is waiting on the semaphore.
 * @post The semaphore no longer satisfies takes.
 * @post The semaphore family call counter has advanced.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline UINT tx_semaphore_delete(TX_SEMAPHORE* semaphore_ptr)
{
  UINT injected = TX_SUCCESS;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_semaphore, &injected)) {
    return injected;
  }
  if ((semaphore_ptr == nullptr) ||
      (semaphore_ptr->magic != (uint32_t)k_ra8_esp_hosted_tx_shim_magic)) {
    return TX_SEMAPHORE_ERROR;
  }
  (void)memset(semaphore_ptr, 0, sizeof(*semaphore_ptr));
  return TX_SUCCESS;
}

/**
 * @brief Host model of ``tx_semaphore_get``.
 * @details Decrements a non-zero count. A zero count reports
 * ``TX_NO_INSTANCE`` whatever the wait, so both the port's try-take and its
 * blocking-take mappings can be driven.
 * @param[in,out] semaphore_ptr Semaphore to take.
 * @param[in] wait_option Recorded wait option.
 * @return ThreadX status code.
 * @retval TX_SUCCESS An instance was taken.
 * @retval TX_SEMAPHORE_ERROR ``semaphore_ptr`` was null or never created.
 * @retval TX_NO_INSTANCE The count was zero.
 * @pre The semaphore was created.
 * @pre The caller tolerates a non-blocking model.
 * @post On success the count has fallen by one.
 * @post ``last_wait`` records this call's wait option either way.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline UINT tx_semaphore_get(TX_SEMAPHORE* semaphore_ptr, ULONG wait_option)
{
  UINT injected = TX_SUCCESS;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_semaphore, &injected)) {
    return injected;
  }
  if ((semaphore_ptr == nullptr) ||
      (semaphore_ptr->magic != (uint32_t)k_ra8_esp_hosted_tx_shim_magic)) {
    return TX_SEMAPHORE_ERROR;
  }
  semaphore_ptr->last_wait = wait_option;
  if (semaphore_ptr->count == 0U) {
    return TX_NO_INSTANCE;
  }
  semaphore_ptr->count--;
  return TX_SUCCESS;
}

/**
 * @brief Host model of ``tx_semaphore_put``.
 * @details Increments the instance count; ThreadX has no ceiling, so the
 * count may exceed whatever the creator asked for.
 * @param[in,out] semaphore_ptr Semaphore to post.
 * @return ThreadX status code.
 * @retval TX_SUCCESS The count was incremented.
 * @retval TX_SEMAPHORE_ERROR ``semaphore_ptr`` was null or never created.
 * @pre The semaphore was created.
 * @pre The caller may be modelling interrupt context.
 * @post The count has risen by one.
 * @post The semaphore family call counter has advanced.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline UINT tx_semaphore_put(TX_SEMAPHORE* semaphore_ptr)
{
  UINT injected = TX_SUCCESS;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_semaphore, &injected)) {
    return injected;
  }
  if ((semaphore_ptr == nullptr) ||
      (semaphore_ptr->magic != (uint32_t)k_ra8_esp_hosted_tx_shim_magic)) {
    return TX_SEMAPHORE_ERROR;
  }
  semaphore_ptr->count++;
  return TX_SUCCESS;
}

/**
 * @brief Host model of ``tx_mutex_create``.
 * @details Records the priority-inheritance selector so the port's choice can
 * be asserted, and starts the mutex unowned.
 * @param[out] mutex_ptr Mutex control block to initialise.
 * @param[in] name_ptr Diagnostic name; accepted for signature shape.
 * @param[in] inherit Priority-inheritance selector.
 * @return ThreadX status code.
 * @retval TX_SUCCESS The mutex is ready.
 * @retval TX_MUTEX_ERROR ``mutex_ptr`` was null.
 * @pre ``mutex_ptr`` is not already a live mutex.
 * @pre The caller owns the control block's storage.
 * @post The mutex is unowned.
 * @post The mutex family call counter has advanced.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline UINT
/* NOLINTNEXTLINE(readability-non-const-parameter) -- ThreadX API pins it non-const. */
tx_mutex_create(TX_MUTEX* mutex_ptr, CHAR* name_ptr, UINT inherit)
{
  UINT injected = TX_SUCCESS;
  (void)name_ptr;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_mutex, &injected)) {
    return injected;
  }
  if (mutex_ptr == nullptr) {
    return TX_MUTEX_ERROR;
  }
  mutex_ptr->magic   = (uint32_t)k_ra8_esp_hosted_tx_shim_magic;
  mutex_ptr->owned   = 0U;
  mutex_ptr->inherit = inherit;
  return TX_SUCCESS;
}

/**
 * @brief Host model of ``tx_mutex_delete``.
 * @details Clears the sentinel so later locks are rejected.
 * @param[in,out] mutex_ptr Mutex to delete.
 * @return ThreadX status code.
 * @retval TX_SUCCESS The mutex was deleted.
 * @retval TX_MUTEX_ERROR ``mutex_ptr`` was null or never created.
 * @pre ``mutex_ptr`` came from ``tx_mutex_create``.
 * @pre Nothing owns the mutex.
 * @post The mutex no longer accepts locks.
 * @post The mutex family call counter has advanced.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline UINT tx_mutex_delete(TX_MUTEX* mutex_ptr)
{
  UINT injected = TX_SUCCESS;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_mutex, &injected)) {
    return injected;
  }
  if ((mutex_ptr == nullptr) || (mutex_ptr->magic != (uint32_t)k_ra8_esp_hosted_tx_shim_magic)) {
    return TX_MUTEX_ERROR;
  }
  (void)memset(mutex_ptr, 0, sizeof(*mutex_ptr));
  return TX_SUCCESS;
}

/**
 * @brief Host model of ``tx_mutex_get``.
 * @details Single-threaded host code cannot contend, so the lock always
 * succeeds and only deepens the ownership count.
 * @param[in,out] mutex_ptr Mutex to lock.
 * @param[in] wait_option Ignored; the model never suspends.
 * @return ThreadX status code.
 * @retval TX_SUCCESS The mutex is owned by the caller.
 * @retval TX_MUTEX_ERROR ``mutex_ptr`` was null or never created.
 * @pre The mutex was created.
 * @pre The caller will balance this with a release.
 * @post The ownership depth has risen by one.
 * @post The mutex family call counter has advanced.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline UINT tx_mutex_get(TX_MUTEX* mutex_ptr, ULONG wait_option)
{
  UINT injected = TX_SUCCESS;
  (void)wait_option;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_mutex, &injected)) {
    return injected;
  }
  if ((mutex_ptr == nullptr) || (mutex_ptr->magic != (uint32_t)k_ra8_esp_hosted_tx_shim_magic)) {
    return TX_MUTEX_ERROR;
  }
  mutex_ptr->owned++;
  return TX_SUCCESS;
}

/**
 * @brief Host model of ``tx_mutex_put``.
 * @details Reports ``TX_NOT_OWNED`` for an unbalanced release, the one mutex
 * failure the port can provoke on a single-threaded host.
 * @param[in,out] mutex_ptr Mutex to release.
 * @return ThreadX status code.
 * @retval TX_SUCCESS The ownership depth fell by one.
 * @retval TX_MUTEX_ERROR ``mutex_ptr`` was null or never created.
 * @retval TX_NOT_OWNED The mutex was not owned.
 * @pre The mutex was created.
 * @pre A matching lock was taken.
 * @post The ownership depth never goes below zero.
 * @post The mutex family call counter has advanced.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline UINT tx_mutex_put(TX_MUTEX* mutex_ptr)
{
  UINT injected = TX_SUCCESS;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_mutex, &injected)) {
    return injected;
  }
  if ((mutex_ptr == nullptr) || (mutex_ptr->magic != (uint32_t)k_ra8_esp_hosted_tx_shim_magic)) {
    return TX_MUTEX_ERROR;
  }
  if (mutex_ptr->owned == 0U) {
    return TX_NOT_OWNED;
  }
  mutex_ptr->owned--;
  return TX_SUCCESS;
}

/**
 * @brief Host model of ``tx_thread_create``.
 * @details Records entry point, argument, priority and stack size; the body is
 * never scheduled, so a test calls it directly when it wants it to run.
 * @param[out] thread_ptr Thread control block to initialise.
 * @param[in] name_ptr Diagnostic name; accepted for signature shape.
 * @param[in] entry_function Thread body.
 * @param[in] entry_input Argument handed to the body.
 * @param[in] stack_start Stack storage; recorded only.
 * @param[in] stack_size Stack length in bytes.
 * @param[in] priority ThreadX priority.
 * @param[in] preempt_threshold Preemption threshold; accepted for shape.
 * @param[in] time_slice Time slice; accepted for shape.
 * @param[in] auto_start Auto-start selector; accepted for shape.
 * @return ThreadX status code.
 * @retval TX_SUCCESS The thread was recorded.
 * @retval TX_THREAD_ERROR ``thread_ptr``, the entry or the stack was null.
 * @pre ``stack_size`` is at least ::TX_MINIMUM_STACK.
 * @pre ``entry_function`` is non-null.
 * @post The recorded entry point matches the argument.
 * @post The thread family call counter has advanced.
 * @note Single-threaded host use only; nothing is scheduled.
 * @since 0.1.0
 */
static inline UINT tx_thread_create(
  TX_THREAD* thread_ptr,
  /* NOLINTNEXTLINE(readability-non-const-parameter) -- ThreadX API pins it non-const. */
  CHAR* name_ptr,
  void (*entry_function)(ULONG),
  ULONG entry_input,
  void* stack_start,
  ULONG stack_size,
  UINT  priority,
  UINT  preempt_threshold,
  ULONG time_slice,
  UINT  auto_start)
{
  UINT injected = TX_SUCCESS;
  (void)name_ptr;
  (void)preempt_threshold;
  (void)time_slice;
  (void)auto_start;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_thread, &injected)) {
    return injected;
  }
  if ((thread_ptr == nullptr) || (entry_function == nullptr) || (stack_start == nullptr)) {
    return TX_THREAD_ERROR;
  }
  thread_ptr->magic      = (uint32_t)k_ra8_esp_hosted_tx_shim_magic;
  thread_ptr->entry      = entry_function;
  thread_ptr->input      = entry_input;
  thread_ptr->priority   = priority;
  thread_ptr->stack_size = stack_size;
  thread_ptr->terminated = false;
  return TX_SUCCESS;
}

/**
 * @brief Host model of ``tx_thread_terminate``.
 * @details Marks the recorded thread terminated without running anything.
 * @param[in,out] thread_ptr Thread to terminate.
 * @return ThreadX status code.
 * @retval TX_SUCCESS The thread is marked terminated.
 * @retval TX_THREAD_ERROR ``thread_ptr`` was null or never created.
 * @pre ``thread_ptr`` came from ``tx_thread_create``.
 * @pre The caller will delete the thread afterwards.
 * @post The thread reads as terminated.
 * @post The thread family call counter has advanced.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline UINT tx_thread_terminate(TX_THREAD* thread_ptr)
{
  UINT injected = TX_SUCCESS;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_thread, &injected)) {
    return injected;
  }
  if ((thread_ptr == nullptr) || (thread_ptr->magic != (uint32_t)k_ra8_esp_hosted_tx_shim_magic)) {
    return TX_THREAD_ERROR;
  }
  thread_ptr->terminated = true;
  return TX_SUCCESS;
}

/**
 * @brief Host model of ``tx_thread_delete``.
 * @details Clears the control block so a later terminate is rejected.
 * @param[in,out] thread_ptr Thread to delete.
 * @return ThreadX status code.
 * @retval TX_SUCCESS The thread was deleted.
 * @retval TX_THREAD_ERROR ``thread_ptr`` was null or never created.
 * @pre ``thread_ptr`` was terminated first.
 * @pre No other object references the control block.
 * @post The control block reads as uncreated.
 * @post The thread family call counter has advanced.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline UINT tx_thread_delete(TX_THREAD* thread_ptr)
{
  UINT injected = TX_SUCCESS;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_thread, &injected)) {
    return injected;
  }
  if ((thread_ptr == nullptr) || (thread_ptr->magic != (uint32_t)k_ra8_esp_hosted_tx_shim_magic)) {
    return TX_THREAD_ERROR;
  }
  (void)memset(thread_ptr, 0, sizeof(*thread_ptr));
  return TX_SUCCESS;
}

/**
 * @brief Host model of ``tx_thread_sleep``.
 * @details Does not block; it advances the simulated tick counter instead,
 * which is what makes an elapsed-time assertion possible without a clock.
 * @param[in] timer_ticks Ticks to sleep.
 * @return ThreadX status code.
 * @retval TX_SUCCESS The simulated clock advanced.
 * @pre The shim has been reset at least once.
 * @pre The caller is not modelling interrupt context.
 * @post The tick counter has advanced by ``timer_ticks``.
 * @post The sleep counter has risen by one.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline UINT tx_thread_sleep(ULONG timer_ticks)
{
  UINT injected = TX_SUCCESS;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_thread, &injected)) {
    return injected;
  }
  g_ra8_esp_hosted_tx_shim.sleeps++;
  g_ra8_esp_hosted_tx_shim.last_sleep = timer_ticks;
  g_ra8_esp_hosted_tx_shim.ticks += timer_ticks;
  return TX_SUCCESS;
}

/**
 * @brief Host model of ``tx_thread_relinquish``.
 * @details Counts the yield; there is nothing to yield to on the host.
 * @pre The shim has been reset at least once.
 * @pre The caller is not modelling interrupt context.
 * @post The relinquish counter has risen by one.
 * @post No other model state changes.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline void tx_thread_relinquish(void)
{
  g_ra8_esp_hosted_tx_shim.relinquishes++;
}

/**
 * @brief Host model of ``tx_timer_create``.
 * @details Records both tick counts so one-shot and periodic timers stay
 * distinguishable, and honours the auto-activate selector.
 * @param[out] timer_ptr Timer control block to initialise.
 * @param[in] name_ptr Diagnostic name; accepted for signature shape.
 * @param[in] expiration_function Function run on expiry.
 * @param[in] expiration_input Argument handed to the expiry function.
 * @param[in] initial_ticks Ticks before the first expiry.
 * @param[in] reschedule_ticks Ticks between later expiries; 0 = one-shot.
 * @param[in] auto_activate Whether the timer starts running.
 * @return ThreadX status code.
 * @retval TX_SUCCESS The timer was recorded.
 * @retval TX_TIMER_ERROR ``timer_ptr`` or the expiry function was null.
 * @retval TX_TICK_ERROR ``initial_ticks`` was zero.
 * @pre ``initial_ticks`` is non-zero.
 * @pre ``expiration_function`` is non-null.
 * @post The recorded reschedule count matches the argument.
 * @post The timer family call counter has advanced.
 * @note Single-threaded host use only; nothing fires by itself.
 * @since 0.1.0
 */
static inline UINT tx_timer_create(
  TX_TIMER* timer_ptr,
  /* NOLINTNEXTLINE(readability-non-const-parameter) -- ThreadX API pins it non-const. */
  CHAR* name_ptr,
  void (*expiration_function)(ULONG),
  ULONG expiration_input,
  ULONG initial_ticks,
  ULONG reschedule_ticks,
  UINT  auto_activate)
{
  UINT injected = TX_SUCCESS;
  (void)name_ptr;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_timer, &injected)) {
    return injected;
  }
  if ((timer_ptr == nullptr) || (expiration_function == nullptr)) {
    return TX_TIMER_ERROR;
  }
  if (initial_ticks == 0U) {
    return TX_TICK_ERROR;
  }
  timer_ptr->magic      = (uint32_t)k_ra8_esp_hosted_tx_shim_magic;
  timer_ptr->expiry     = expiration_function;
  timer_ptr->input      = expiration_input;
  timer_ptr->initial    = initial_ticks;
  timer_ptr->reschedule = reschedule_ticks;
  timer_ptr->active     = (auto_activate == TX_AUTO_ACTIVATE);
  return TX_SUCCESS;
}

/**
 * @brief Host model of ``tx_timer_deactivate``.
 * @details Marks the timer stopped; a later fire request is refused.
 * @param[in,out] timer_ptr Timer to stop.
 * @return ThreadX status code.
 * @retval TX_SUCCESS The timer is stopped.
 * @retval TX_TIMER_ERROR ``timer_ptr`` was null or never created.
 * @pre ``timer_ptr`` came from ``tx_timer_create``.
 * @pre The caller will delete or restart the timer.
 * @post The timer reads as inactive.
 * @post The timer family call counter has advanced.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline UINT tx_timer_deactivate(TX_TIMER* timer_ptr)
{
  UINT injected = TX_SUCCESS;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_timer, &injected)) {
    return injected;
  }
  if ((timer_ptr == nullptr) || (timer_ptr->magic != (uint32_t)k_ra8_esp_hosted_tx_shim_magic)) {
    return TX_TIMER_ERROR;
  }
  timer_ptr->active = false;
  return TX_SUCCESS;
}

/**
 * @brief Host model of ``tx_timer_delete``.
 * @details Clears the control block so a later fire request is rejected.
 * @param[in,out] timer_ptr Timer to delete.
 * @return ThreadX status code.
 * @retval TX_SUCCESS The timer was deleted.
 * @retval TX_TIMER_ERROR ``timer_ptr`` was null or never created.
 * @pre ``timer_ptr`` was deactivated first.
 * @pre No expiry is in progress.
 * @post The control block reads as uncreated.
 * @post The timer family call counter has advanced.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline UINT tx_timer_delete(TX_TIMER* timer_ptr)
{
  UINT injected = TX_SUCCESS;
  if (ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_timer, &injected)) {
    return injected;
  }
  if ((timer_ptr == nullptr) || (timer_ptr->magic != (uint32_t)k_ra8_esp_hosted_tx_shim_magic)) {
    return TX_TIMER_ERROR;
  }
  (void)memset(timer_ptr, 0, sizeof(*timer_ptr));
  return TX_SUCCESS;
}

/**
 * @brief Host model of ``tx_time_get``.
 * @details Returns the simulated 32-bit tick counter, which a test moves with
 * ::ra8_esp_hosted_tx_shim_set_ticks or by sleeping.
 * @return Current simulated tick count.
 * @retval 0 The shim was just reset.
 * @pre The shim has been reset at least once.
 * @pre The caller treats the value as wrapping at 2^32.
 * @post No model state changes beyond the call counter.
 * @post The value equals whatever was last set or slept to.
 * @note Single-threaded host use only.
 * @since 0.1.0
 */
static inline ULONG tx_time_get(void)
{
  UINT injected = TX_SUCCESS;
  (void)ra8_esp_hosted_tx_shim_enter(k_ra8_esp_hosted_tx_shim_family_time, &injected);
  return g_ra8_esp_hosted_tx_shim.ticks;
}
