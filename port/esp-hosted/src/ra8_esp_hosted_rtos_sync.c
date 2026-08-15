/**
 * @file port/esp-hosted/src/ra8_esp_hosted_rtos_sync.c
 * @brief Mutex and semaphore vtable slots.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * See `port/esp-hosted/src/ra8_esp_hosted_rtos_internal.h` for the contracts.
 * The RTOS slice is three translation units because one would be several
 * times the project's thousand-line file cap; the split follows the object
 * kinds, which is also how the fixed tables divide. This unit owns the mutex
 * and semaphore tables.
 *
 * There are no mempool-lock rows to fill: ``H_USE_MEMPOOL`` is deliberately
 * ``hosted_osi_funcs_t`` at all. See ``port_esp_hosted_host_config.h`` for
 * why -- the vendored header guards them with ``#ifdef`` rather than ``#if``,
 * so any definition, including zero, would switch them on and make the struct
 * layout depend on include order.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_esp_hosted_port.h"
#include "ra8_esp_hosted_rtos_internal.h"
#include "ra8_log.h"

#ifdef RA8_OFF_TARGET
#include "ra8_esp_hosted_tx_shim_sync_internal.h"
#else
#include "tx_api.h"
#endif

#include "port_esp_hosted_host_os.h"

/**
 * @var s_tag
 * @brief Log tag for the mutex and semaphore half of the port.
 * @details Shared by every diagnostic this translation unit emits.
 * @note Static; do not access outside this TU.
 * @warning Changing it changes log-scraping expectations on the bench.
 * @since 0.1.0
 */
static const char* s_tag = "ESPH_SYNC";

/**
 * @enum ra8_esp_hosted_sync_const_t
 * @brief Numeric constants of the mutex and semaphore half.
 * @details One value, stated here rather than inline so the create-time
 * semaphore count is impossible to change by accident.
 * @invariant ::k_ra8_esp_hosted_sem_initial is exactly one, so the single
 *            drain every vendored call site performs leaves the semaphore
 *            empty.
 * @invariant The value is never larger than any caller's ``max_count``.
 * @par Example:
 * @code
 * static_assert(k_ra8_esp_hosted_sem_initial == 1U, "one drain empties it");
 * @endcode
 * @see priv_ra8_esp_hosted_rtos_bind_sync
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_esp_hosted_sem_initial = 1U, /**< Instances a new semaphore starts with. */
} ra8_esp_hosted_sync_const_t;

/**
 * @struct ra8_esp_hosted_sync_state_t
 * @brief Module state of the mutex and semaphore half -- entirely static.
 * @details Two fixed tables and their occupancy flags. Nothing is allocated;
 * the tables are reserved at link time and exhaustion is a failure, never a
 * growth.
 * @invariant ``ready`` is true exactly while the tables may be used.
 * @invariant A set occupancy flag always names a live ThreadX object.
 * @par Example:
 * @code
 * TEST_ASSERT_NOT_NULL(funcs._h_create_mutex());
 * @endcode
 * @see priv_ra8_esp_hosted_rtos_sync_init
 * @since 0.1.0
 */
typedef struct {
  TX_MUTEX     mutexes[k_ra8_esp_hosted_max_mutexes];     /**< Mutex table.         */
  bool         mutex_used[k_ra8_esp_hosted_max_mutexes];  /**< Mutex occupancy.     */
  TX_SEMAPHORE sems[k_ra8_esp_hosted_max_semaphores];     /**< Semaphore table.     */
  bool         sem_used[k_ra8_esp_hosted_max_semaphores]; /**< Semaphore occupancy. */
  bool         ready;                                     /**< Tables usable.       */
} ra8_esp_hosted_sync_state_t;

/**
 * @var s_sync
 * @brief Singleton state of the mutex and semaphore half.
 * @details Zero-initialised at link time; brought up by
 * ::priv_ra8_esp_hosted_rtos_sync_init.
 * @note Static; do not access outside this TU.
 * @warning Direct modification bypasses every ThreadX consistency check.
 * @since 0.1.0
 */
static ra8_esp_hosted_sync_state_t s_sync;

/* ----------------------------------------------------------------------- */
/* Mutex vtable slots */
/* ----------------------------------------------------------------------- */

/**
 * @brief Create a mutex from the fixed mutex table.
 * @details Priority inheritance is enabled: the bus lock is taken by both the
 * transaction thread and a lower-priority power-save path, which is exactly
 * the inversion ThreadX's inheritance exists to bound.
 * @return Opaque mutex handle, or null on failure.
 * @retval nullptr The substrate is down or the mutex table is full.
 * @retval non-null A handle usable with the other mutex slots.
 * @pre The substrate is initialised.
 * @pre The caller destroys the mutex before teardown.
 * @post On success one table row is occupied and the mutex is unowned.
 * @post On failure no row stays claimed.
 * @note Not thread-safe against a concurrent create.
 * @since 0.1.0
 */
RA8_INTERNAL static void* internal_h_create_mutex(void)
{
  if (!s_sync.ready) {
    return nullptr;
  }
  const uint32_t idx =
    priv_ra8_esp_hosted_rtos_slot_take(s_sync.mutex_used, k_ra8_esp_hosted_max_mutexes);
  if (idx == (uint32_t)k_ra8_esp_hosted_max_mutexes) {
    ra8_log_error(s_tag, "mutex table exhausted");
    return nullptr;
  }
  if (tx_mutex_create(&s_sync.mutexes[idx], nullptr, TX_INHERIT) != TX_SUCCESS) {
    s_sync.mutex_used[idx] = false;
    return nullptr;
  }
  return &s_sync.mutexes[idx];
}

/**
 * @brief Take a mutex, blocking for at most ``timeout_ms``.
 * @details Zero milliseconds is a try-lock; a negative value, which is how
 * ``HOSTED_BLOCK_MAX`` arrives through an ``int``, blocks until the mutex is
 * free.
 * @param[in] mutex_handle Handle from ``_h_create_mutex``.
 * @param[in] timeout_ms Wait in milliseconds; 0 = try, negative = forever.
 * @return ``RET_OK`` on success, a negative ``RET_*`` code otherwise.
 * @retval RET_OK The caller now owns the mutex.
 * @retval RET_INVALID The handle was not usable.
 * @retval RET_FAIL_TIMEOUT The wait expired with the mutex still held.
 * @pre The substrate is initialised.
 * @pre The caller will release the mutex it takes.
 * @post On success the ownership depth has risen by one.
 * @post On failure the ownership depth is unchanged.
 * @note Thread-safe; that is the point. Validation is by return code because
 *       the vtable signature has no error channel.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_h_lock_mutex(void* mutex_handle, int timeout_ms)
{
  const uint32_t idx = priv_ra8_esp_hosted_rtos_slot_index(mutex_handle,
                                                           s_sync.mutexes,
                                                           sizeof(s_sync.mutexes[0]),
                                                           k_ra8_esp_hosted_max_mutexes,
                                                           s_sync.mutex_used);
  if (idx == (uint32_t)k_ra8_esp_hosted_max_mutexes) {
    return RET_INVALID;
  }
  const UINT rc =
    tx_mutex_get(&s_sync.mutexes[idx], (ULONG)priv_ra8_esp_hosted_rtos_ms_to_ticks(timeout_ms));
  return (rc == TX_SUCCESS) ? RET_OK : RET_FAIL_TIMEOUT;
}

/**
 * @brief Release a mutex the caller owns.
 * @details Reports a distinct failure for an unbalanced release rather than
 * swallowing it, because that is always a defect in the caller.
 * @param[in] mutex_handle Handle from ``_h_create_mutex``.
 * @return ``RET_OK`` on success, a negative ``RET_*`` code otherwise.
 * @retval RET_OK The ownership depth fell by one.
 * @retval RET_INVALID The handle was not usable.
 * @retval RET_FAIL The caller did not own the mutex.
 * @pre The substrate is initialised.
 * @pre The caller took the mutex it is releasing.
 * @post On success the ownership depth has fallen by one.
 * @post On failure the ownership depth is unchanged.
 * @note Thread-safe. Validation is by return code because the vtable
 *       signature has no error channel.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_h_unlock_mutex(void* mutex_handle)
{
  const uint32_t idx = priv_ra8_esp_hosted_rtos_slot_index(mutex_handle,
                                                           s_sync.mutexes,
                                                           sizeof(s_sync.mutexes[0]),
                                                           k_ra8_esp_hosted_max_mutexes,
                                                           s_sync.mutex_used);
  if (idx == (uint32_t)k_ra8_esp_hosted_max_mutexes) {
    return RET_INVALID;
  }
  return (tx_mutex_put(&s_sync.mutexes[idx]) == TX_SUCCESS) ? RET_OK : RET_FAIL;
}

/**
 * @brief Delete a mutex and free its table row.
 * @details Frees the row last so a concurrent lookup never sees a
 * half-deleted object.
 * @param[in] mutex_handle Handle from ``_h_create_mutex``.
 * @return ``RET_OK`` on success, a negative ``RET_*`` code otherwise.
 * @retval RET_OK The mutex is gone and its row is free.
 * @retval RET_INVALID The handle was not usable.
 * @retval RET_FAIL ThreadX refused the delete.
 * @pre The substrate is initialised.
 * @pre Nothing owns or is waiting on the mutex.
 * @post The table row is free for reuse.
 * @post A later lock through the stale handle reports ``RET_INVALID``.
 * @note Not thread-safe against a concurrent create. Validation is by return
 *       code because the vtable signature has no error channel.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_h_destroy_mutex(void* mutex_handle)
{
  const uint32_t idx = priv_ra8_esp_hosted_rtos_slot_index(mutex_handle,
                                                           s_sync.mutexes,
                                                           sizeof(s_sync.mutexes[0]),
                                                           k_ra8_esp_hosted_max_mutexes,
                                                           s_sync.mutex_used);
  if (idx == (uint32_t)k_ra8_esp_hosted_max_mutexes) {
    return RET_INVALID;
  }
  const int rc = (tx_mutex_delete(&s_sync.mutexes[idx]) == TX_SUCCESS) ? RET_OK : RET_FAIL;
  s_sync.mutex_used[idx] = false;
  return rc;
}

/* ----------------------------------------------------------------------- */
/* Semaphore vtable slots */
/* ----------------------------------------------------------------------- */

/**
 * @brief Create a counting semaphore from the fixed semaphore table.
 * @details The initial count is **one**, not ``max_count``. Every call site
 * in the vendored tree -- ``bus_init_internal`` in ``spi_drv.c``,
 * ``rpc_platform_init``, ``rpc_core``'s per-request semaphore -- follows the
 * create immediately with one ``_h_get_semaphore(sem, 0)`` whose own comment
 * says it is there to leave the semaphore empty so the waiting task blocks.
 * Starting at ``max_count`` would leave ``max_count - 1`` phantom instances
 * behind that single drain, and ``spi_drv.c`` reads a successful take as "a
 * transmit message is present as per sem" -- so the phantom counts would make
 * it repeatedly believe a frame is queued when none is. Starting at one makes
 * that drain do exactly what its comment claims on every path, binary or
 * counting. ``max_count`` is therefore validated and otherwise unused:
 * ThreadX counting semaphores carry no ceiling.
 * @param[in] max_count Largest count the caller intends to reach; must be
 *                      positive.
 * @return Opaque semaphore handle, or null on failure.
 * @retval nullptr The substrate is down, ``max_count`` was not positive, or
 *         the semaphore table is full.
 * @retval non-null A handle usable with the other semaphore slots.
 * @pre The substrate is initialised.
 * @pre The caller drains the semaphore if it wants it empty.
 * @post On success one table row is occupied and the count is one.
 * @post On failure no row stays claimed.
 * @note Not thread-safe against a concurrent create.
 * @since 0.1.0
 */
RA8_INTERNAL static void* internal_h_create_semaphore(int max_count)
{
  if (!s_sync.ready || (max_count <= 0)) {
    return nullptr;
  }
  const uint32_t idx =
    priv_ra8_esp_hosted_rtos_slot_take(s_sync.sem_used, k_ra8_esp_hosted_max_semaphores);
  if (idx == (uint32_t)k_ra8_esp_hosted_max_semaphores) {
    ra8_log_error(s_tag, "semaphore table exhausted");
    return nullptr;
  }
  if (tx_semaphore_create(&s_sync.sems[idx], nullptr, (ULONG)k_ra8_esp_hosted_sem_initial) !=
      TX_SUCCESS) {
    s_sync.sem_used[idx] = false;
    return nullptr;
  }
  return &s_sync.sems[idx];
}

/**
 * @brief Take a semaphore, blocking for at most ``timeout_ms``.
 * @details A zero timeout is a non-blocking try-take that returns ``RET_OK``
 * on success -- ``spi_drv.c`` spells it ``if (!_h_get_semaphore(sem, 0))``
 * and treats the zero as "an item is queued", so the mapping is load-bearing.
 * @param[in] semaphore_handle Handle from ``_h_create_semaphore``.
 * @param[in] timeout_ms Wait in milliseconds; 0 = try, negative = forever.
 * @return ``RET_OK`` on success, a negative ``RET_*`` code otherwise.
 * @retval RET_OK An instance was taken.
 * @retval RET_INVALID The handle was not usable.
 * @retval RET_FAIL_TIMEOUT The count stayed zero for the whole wait.
 * @pre The substrate is initialised.
 * @pre The caller is a thread when the timeout is non-zero.
 * @post On success the count has fallen by one.
 * @post On failure the count is unchanged.
 * @note Thread context only when blocking. Validation is by return code
 *       because the vtable signature has no error channel.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_h_get_semaphore(void* semaphore_handle, int timeout_ms)
{
  const uint32_t idx = priv_ra8_esp_hosted_rtos_slot_index(semaphore_handle,
                                                           s_sync.sems,
                                                           sizeof(s_sync.sems[0]),
                                                           k_ra8_esp_hosted_max_semaphores,
                                                           s_sync.sem_used);
  if (idx == (uint32_t)k_ra8_esp_hosted_max_semaphores) {
    return RET_INVALID;
  }
  const UINT rc =
    tx_semaphore_get(&s_sync.sems[idx], (ULONG)priv_ra8_esp_hosted_rtos_ms_to_ticks(timeout_ms));
  return (rc == TX_SUCCESS) ? RET_OK : RET_FAIL_TIMEOUT;
}

/**
 * @brief Post a semaphore from thread context.
 * @details Increments the count and, if a thread is blocked on it, releases
 * that thread. ThreadX imposes no ceiling, so a post always succeeds on a
 * live semaphore.
 * @param[in] semaphore_handle Handle from ``_h_create_semaphore``.
 * @return ``RET_OK`` on success, a negative ``RET_*`` code otherwise.
 * @retval RET_OK The count was incremented.
 * @retval RET_INVALID The handle was not usable.
 * @retval RET_FAIL ThreadX refused the post.
 * @pre The substrate is initialised.
 * @pre The caller is not an interrupt handler; use the ISR variant there.
 * @post On success the count has risen by one.
 * @post On failure the count is unchanged.
 * @note Thread-safe. Validation is by return code because the vtable
 *       signature has no error channel.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_h_post_semaphore(void* semaphore_handle)
{
  const uint32_t idx = priv_ra8_esp_hosted_rtos_slot_index(semaphore_handle,
                                                           s_sync.sems,
                                                           sizeof(s_sync.sems[0]),
                                                           k_ra8_esp_hosted_max_semaphores,
                                                           s_sync.sem_used);
  if (idx == (uint32_t)k_ra8_esp_hosted_max_semaphores) {
    return RET_INVALID;
  }
  return (tx_semaphore_put(&s_sync.sems[idx]) == TX_SUCCESS) ? RET_OK : RET_FAIL;
}

/**
 * @brief Post a semaphore from an interrupt handler.
 * @details Called from the HANDSHAKE and DATA_READY edge handlers. It does
 * exactly what the thread-context post does, because ``tx_semaphore_put`` is
 * itself ISR-safe -- and deliberately nothing else: no logging, no blocking,
 * no allocation, so the handler stays bounded.
 * @param[in] semaphore_handle Handle from ``_h_create_semaphore``.
 * @return ``RET_OK`` on success, a negative ``RET_*`` code otherwise.
 * @retval RET_OK The count was incremented.
 * @retval RET_INVALID The handle was not usable.
 * @retval RET_FAIL ThreadX refused the post.
 * @pre The substrate is initialised.
 * @pre The caller is an interrupt handler or a thread; either is fine.
 * @post On success the count has risen by one.
 * @post No log line is emitted on any path.
 * @note Safe from interrupt context; that is this slot's whole purpose.
 * @since 0.1.0
 */
RA8_INTERNAL RA8_ISR_SAFE static int internal_h_post_semaphore_from_isr(void* semaphore_handle)
{
  const uint32_t idx = priv_ra8_esp_hosted_rtos_slot_index(semaphore_handle,
                                                           s_sync.sems,
                                                           sizeof(s_sync.sems[0]),
                                                           k_ra8_esp_hosted_max_semaphores,
                                                           s_sync.sem_used);
  if (idx == (uint32_t)k_ra8_esp_hosted_max_semaphores) {
    return RET_INVALID;
  }
  return (tx_semaphore_put(&s_sync.sems[idx]) == TX_SUCCESS) ? RET_OK : RET_FAIL;
}

/**
 * @brief Delete a semaphore and free its table row.
 * @details Frees the row last so a concurrent lookup never sees a
 * half-deleted object.
 * @param[in] semaphore_handle Handle from ``_h_create_semaphore``.
 * @return ``RET_OK`` on success, a negative ``RET_*`` code otherwise.
 * @retval RET_OK The semaphore is gone and its row is free.
 * @retval RET_INVALID The handle was not usable.
 * @retval RET_FAIL ThreadX refused the delete.
 * @pre The substrate is initialised.
 * @pre No thread is blocked on the semaphore.
 * @post The table row is free for reuse.
 * @post A later take through the stale handle reports ``RET_INVALID``.
 * @note Not thread-safe against a concurrent create. Validation is by return
 *       code because the vtable signature has no error channel.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_h_destroy_semaphore(void* semaphore_handle)
{
  const uint32_t idx = priv_ra8_esp_hosted_rtos_slot_index(semaphore_handle,
                                                           s_sync.sems,
                                                           sizeof(s_sync.sems[0]),
                                                           k_ra8_esp_hosted_max_semaphores,
                                                           s_sync.sem_used);
  if (idx == (uint32_t)k_ra8_esp_hosted_max_semaphores) {
    return RET_INVALID;
  }
  const int rc         = (tx_semaphore_delete(&s_sync.sems[idx]) == TX_SUCCESS) ? RET_OK : RET_FAIL;
  s_sync.sem_used[idx] = false;
  return rc;
}

ra8_err_t priv_ra8_esp_hosted_rtos_sync_init(void)
{
  if (s_sync.ready) {
    ra8_log_error(s_tag, "sync tables already initialised");
    return k_ra8_err_invalid_state;
  }
  (void)memset(&s_sync, 0, sizeof(s_sync));
  s_sync.ready = true;
  return k_ra8_ok;
}

ra8_err_t priv_ra8_esp_hosted_rtos_sync_deinit(void)
{
  if (!s_sync.ready) {
    ra8_log_error(s_tag, "sync tables not initialised");
    return k_ra8_err_not_initialized;
  }
  ra8_err_t worst = k_ra8_ok;
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_esp_hosted_max_semaphores; ++i) {
    if (s_sync.sem_used[i] && (internal_h_destroy_semaphore(&s_sync.sems[i]) != RET_OK)) {
      worst = k_ra8_err_rtos_error;
    }
  }
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_esp_hosted_max_mutexes; ++i) {
    if (s_sync.mutex_used[i] && (internal_h_destroy_mutex(&s_sync.mutexes[i]) != RET_OK)) {
      worst = k_ra8_err_rtos_error;
    }
  }
  (void)memset(&s_sync, 0, sizeof(s_sync));
  return worst;
}

ra8_err_t priv_ra8_esp_hosted_rtos_bind_sync(hosted_osi_funcs_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "vtable is NULL");
  out->_h_create_mutex            = internal_h_create_mutex;
  out->_h_lock_mutex              = internal_h_lock_mutex;
  out->_h_unlock_mutex            = internal_h_unlock_mutex;
  out->_h_destroy_mutex           = internal_h_destroy_mutex;
  out->_h_create_semaphore        = internal_h_create_semaphore;
  out->_h_get_semaphore           = internal_h_get_semaphore;
  out->_h_post_semaphore          = internal_h_post_semaphore;
  out->_h_post_semaphore_from_isr = internal_h_post_semaphore_from_isr;
  out->_h_destroy_semaphore       = internal_h_destroy_semaphore;
  return k_ra8_ok;
}
