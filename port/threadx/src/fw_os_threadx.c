/**
 * @file port/threadx/src/fw_os_threadx.c
 * @brief The Eclipse ThreadX binding of the `fw_os` port contract.
 *
 * @details
 * [Ring 4 / RTOS Port] {World: NS}
 *
 * Implements every MUST call in `libs/if/inc/fw_os.h` on top of ThreadX, so a
 * portable library can stop calling `tx_*` directly. The optional message
 * queue block is declined: `FW_OS_HAS_QUEUE` stays clear, because a binding
 * that leaves an optional surface alone is the point of the capability flag.
 *
 * The three pieces of real logic (millisecond to tick conversion, the
 * priority band map, the status map) live in `fw_os_threadx.h` as pure
 * functions so the host build can test them without a scheduler. What is left
 * here is the part that cannot be tested without hardware or a running
 * kernel: the `tx_*` calls themselves and the placement of a ThreadX control
 * block inside the caller's storage.
 *
 * Nothing here allocates. A caller's ::fw_os_thread_t holds the `TX_THREAD`
 * plus the two fields ThreadX cannot carry for us: the `fw_os` entry function
 * and its `void *` argument, because ThreadX hands a thread a `ULONG` rather
 * than a pointer. A magic word guards against a caller passing storage that
 * was never initialised.
 *
 * @note This file is compiled into the `threadx` static library, so an app
 *       that never references `fw_os_*` does not link it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fw_os.h"
#include "fw_os_threadx.h"
#include "ra8_err.h"
#include "tx_api.h"

/* The mirrored constants in fw_os_threadx.h exist so the pure helpers are
 * host-testable without the vendor tree. This is where the two views are
 * forced to agree: if Eclipse ever renumbers a status or the project retunes
 * TX_MAX_PRIORITIES, the binding stops compiling instead of mapping wrongly. */
static_assert(K_FW_OS_TX_NO_WAIT == TX_NO_WAIT, "TX_NO_WAIT mirror drifted");
static_assert(K_FW_OS_TX_WAIT_FOREVER == TX_WAIT_FOREVER, "TX_WAIT_FOREVER mirror drifted");
static_assert(K_FW_OS_TX_MAX_PRIORITIES == TX_MAX_PRIORITIES, "TX_MAX_PRIORITIES mirror drifted");
static_assert((uint32_t)k_fw_os_tx_success == TX_SUCCESS, "TX_SUCCESS mirror drifted");
static_assert((uint32_t)k_fw_os_tx_deleted == TX_DELETED, "TX_DELETED mirror drifted");
static_assert((uint32_t)k_fw_os_tx_ptr_error == TX_PTR_ERROR, "TX_PTR_ERROR mirror drifted");
static_assert((uint32_t)k_fw_os_tx_wait_error == TX_WAIT_ERROR, "TX_WAIT_ERROR mirror drifted");
static_assert((uint32_t)k_fw_os_tx_size_error == TX_SIZE_ERROR, "TX_SIZE_ERROR mirror drifted");
static_assert((uint32_t)k_fw_os_tx_semaphore_error == TX_SEMAPHORE_ERROR, "TX_SEMAPHORE mirror drifted");
static_assert((uint32_t)k_fw_os_tx_no_instance == TX_NO_INSTANCE, "TX_NO_INSTANCE mirror drifted");
static_assert((uint32_t)k_fw_os_tx_thread_error == TX_THREAD_ERROR, "TX_THREAD_ERROR mirror drifted");
static_assert((uint32_t)k_fw_os_tx_priority_error == TX_PRIORITY_ERROR, "TX_PRIORITY mirror drifted");
static_assert((uint32_t)k_fw_os_tx_delete_error == TX_DELETE_ERROR, "TX_DELETE_ERROR mirror drifted");
static_assert((uint32_t)k_fw_os_tx_caller_error == TX_CALLER_ERROR, "TX_CALLER_ERROR mirror drifted");
static_assert((uint32_t)k_fw_os_tx_wait_aborted == TX_WAIT_ABORTED, "TX_WAIT_ABORTED mirror drifted");
static_assert((uint32_t)k_fw_os_tx_mutex_error == TX_MUTEX_ERROR, "TX_MUTEX_ERROR mirror drifted");
static_assert((uint32_t)k_fw_os_tx_not_available == TX_NOT_AVAILABLE, "TX_NOT_AVAILABLE mirror drifted");
static_assert((uint32_t)k_fw_os_tx_not_owned == TX_NOT_OWNED, "TX_NOT_OWNED mirror drifted");

/* ThreadX hands a thread entry a ULONG, not a pointer, so the binding passes
 * the address of its own block through one. That is only sound where a
 * pointer fits in a ULONG, which is true on every Cortex-M port and is
 * checked rather than assumed. */
static_assert(sizeof(ULONG) >= sizeof(void *), "a ULONG cannot carry a pointer on this port");

/** @brief Marks an ::fw_os_thread_t whose block this binding created. */
static const uint32_t k_thread_magic = 0x74784F53U;
/** @brief Marks an ::fw_os_mutex_t whose block this binding created. */
static const uint32_t k_mutex_magic = 0x74784D58U;
/** @brief Marks an ::fw_os_sem_t whose block this binding created. */
static const uint32_t k_sem_magic = 0x74785345U;

/** @brief Name handed to ThreadX when the caller supplied none. */
static char s_unnamed[] = "fw_os";

/**
 * @struct internal_thread_t
 * @brief What this binding keeps inside a caller's ::fw_os_thread_t storage.
 */
typedef struct {
    TX_THREAD tx;             /**< The ThreadX control block.        */
    void (*entry)(void *arg); /**< The portable entry the caller gave. */
    void *arg;                /**< Its single argument.               */
    uint32_t magic;           /**< ::k_thread_magic once created.     */
} internal_thread_t;

/**
 * @struct internal_mutex_t
 * @brief What this binding keeps inside a caller's ::fw_os_mutex_t storage.
 */
typedef struct {
    TX_MUTEX tx;    /**< The ThreadX control block.    */
    uint32_t magic; /**< ::k_mutex_magic once created. */
} internal_mutex_t;

/**
 * @struct internal_sem_t
 * @brief What this binding keeps inside a caller's ::fw_os_sem_t storage.
 */
typedef struct {
    TX_SEMAPHORE tx; /**< The ThreadX control block.  */
    uint32_t magic;  /**< ::k_sem_magic once created. */
} internal_sem_t;

/* The seam sizes its storage in 64-bit words so a caller can declare one
 * without knowing which RTOS is underneath. These are the checks that say the
 * chosen sizes really do hold a ThreadX block; a binding whose block does not
 * fit raises the constant in its own build rather than overflowing. */
static_assert(sizeof(internal_thread_t) <= sizeof(fw_os_thread_t), "TX_THREAD does not fit");
static_assert(sizeof(internal_mutex_t) <= sizeof(fw_os_mutex_t), "TX_MUTEX does not fit");
static_assert(sizeof(internal_sem_t) <= sizeof(fw_os_sem_t), "TX_SEMAPHORE does not fit");
static_assert(alignof(internal_thread_t) <= alignof(fw_os_thread_t), "thread storage underaligned");
static_assert(alignof(internal_mutex_t) <= alignof(fw_os_mutex_t), "mutex storage underaligned");
static_assert(alignof(internal_sem_t) <= alignof(fw_os_sem_t), "semaphore storage underaligned");

/**
 * @brief The ThreadX entry point that calls the caller's portable entry.
 * @param[in] entry_input The ::internal_thread_t address, passed as a ULONG.
 */
static void internal_trampoline(ULONG entry_input)
{
    internal_thread_t *const self = (internal_thread_t *)(uintptr_t)entry_input;

    if ((self != NULL) && (self->magic == k_thread_magic) && (self->entry != NULL)) {
        self->entry(self->arg);
    }
}

/**
 * @brief Reject a thread configuration ThreadX would refuse anyway.
 * @param[in] cfg The caller's configuration.
 * @return True when every field the binding needs is usable.
 */
static bool internal_cfg_ok(const fw_os_thread_cfg_t *cfg)
{
    return (cfg != NULL) && (cfg->entry != NULL) && (cfg->stack != NULL) &&
           (cfg->stack_bytes >= (size_t)TX_MINIMUM_STACK) &&
           (cfg->stack_bytes <= (size_t)UINT32_MAX) &&
           ((uint32_t)cfg->priority < (uint32_t)TX_MAX_PRIORITIES);
}

/**
 * @brief The name ThreadX should show for a thread or object.
 * @param[in] name The caller's borrowed name, possibly NULL.
 * @return A non-NULL, NUL-terminated name.
 */
static char *internal_name_or_default(const char *name)
{
    /* ThreadX's prototypes take a mutable CHAR*, but the kernel only ever
     * stores and prints the pointer, so a borrowed const name is safe here. */
    return (name == NULL) ? s_unnamed : (char *)(uintptr_t)name;
}

ra8_err_t fw_os_thread_create(fw_os_thread_t *thread, const fw_os_thread_cfg_t *cfg)
{
    if ((thread == NULL) || !internal_cfg_ok(cfg)) {
        return k_ra8_err_invalid_arg;
    }

    internal_thread_t *const self = (internal_thread_t *)(void *)thread;
    self->entry = cfg->entry;
    self->arg = cfg->arg;
    self->magic = k_thread_magic;

    const uint32_t priority = fw_os_threadx_priority_for((uint32_t)cfg->priority);
    const UINT status = tx_thread_create(
        &self->tx, internal_name_or_default(cfg->name), internal_trampoline,
        (ULONG)(uintptr_t)self, cfg->stack, (ULONG)cfg->stack_bytes, (UINT)priority,
        (UINT)priority, TX_NO_TIME_SLICE, TX_AUTO_START);

    if (status != TX_SUCCESS) {
        self->magic = 0U;
        return fw_os_threadx_err_for((uint32_t)status, false);
    }
    return k_ra8_ok;
}

ra8_err_t fw_os_thread_delete(fw_os_thread_t *thread)
{
    if (thread == NULL) {
        return k_ra8_err_invalid_arg;
    }
    internal_thread_t *const self = (internal_thread_t *)(void *)thread;
    if (self->magic != k_thread_magic) {
        return k_ra8_err_invalid_state;
    }

    /* ThreadX will not delete a thread that has not finished, and a thread
     * cannot terminate or delete itself from its own context. Both come back
     * as a caller error, which the seam reports as invalid_state -- exactly
     * what fw_os.h promises for a self-delete a binding cannot do. */
    const UINT terminated = tx_thread_terminate(&self->tx);
    if (terminated != TX_SUCCESS) {
        return fw_os_threadx_err_for((uint32_t)terminated, false);
    }

    const UINT status = tx_thread_delete(&self->tx);
    if (status != TX_SUCCESS) {
        return fw_os_threadx_err_for((uint32_t)status, false);
    }
    self->magic = 0U;
    return k_ra8_ok;
}

void fw_os_thread_sleep_ms(uint32_t ms)
{
    if (ms == 0U) {
        /* fw_os.h says a zero sleep yields. tx_thread_sleep(0) returns at once
         * without giving anyone else a turn, so the yield has to be explicit. */
        tx_thread_relinquish();
        return;
    }
    (void)tx_thread_sleep((ULONG)fw_os_threadx_ticks_for(ms, (uint32_t)TX_TIMER_TICKS_PER_SECOND));
}

void fw_os_thread_yield(void)
{
    tx_thread_relinquish();
}

ra8_err_t fw_os_mutex_init(fw_os_mutex_t *mutex, bool recursive)
{
    if (mutex == NULL) {
        return k_ra8_err_invalid_arg;
    }
    /* ThreadX mutexes always count ownership, so a recursive lock is always
     * available. `recursive == false` therefore means "the caller does not
     * need re-entrancy", not "re-entrancy must trap": this binding cannot
     * offer the trap, and fw_os.h states the contract that way. */
    (void)recursive;

    internal_mutex_t *const self = (internal_mutex_t *)(void *)mutex;
    const UINT status = tx_mutex_create(&self->tx, s_unnamed, TX_INHERIT);
    if (status != TX_SUCCESS) {
        return fw_os_threadx_err_for((uint32_t)status, false);
    }
    self->magic = k_mutex_magic;
    return k_ra8_ok;
}

ra8_err_t fw_os_mutex_deinit(fw_os_mutex_t *mutex)
{
    if (mutex == NULL) {
        return k_ra8_err_invalid_arg;
    }
    internal_mutex_t *const self = (internal_mutex_t *)(void *)mutex;
    if (self->magic != k_mutex_magic) {
        return k_ra8_err_invalid_state;
    }

    /* tx_mutex_delete succeeds on a held mutex and simply resumes whoever was
     * waiting. fw_os.h promises busy instead, so the ownership count is read
     * first rather than letting a held mutex disappear underneath its owner. */
    CHAR *name = NULL;
    ULONG count = 0UL;
    TX_THREAD *owner = NULL;
    TX_THREAD *first_suspended = NULL;
    ULONG suspended = 0UL;
    TX_MUTEX *next = NULL;
    const UINT queried =
        tx_mutex_info_get(&self->tx, &name, &count, &owner, &first_suspended, &suspended, &next);
    if (queried != TX_SUCCESS) {
        return fw_os_threadx_err_for((uint32_t)queried, false);
    }
    if ((count != 0UL) || (suspended != 0UL)) {
        return k_ra8_err_busy;
    }

    const UINT status = tx_mutex_delete(&self->tx);
    if (status != TX_SUCCESS) {
        return fw_os_threadx_err_for((uint32_t)status, false);
    }
    self->magic = 0U;
    return k_ra8_ok;
}

ra8_err_t fw_os_mutex_lock(fw_os_mutex_t *mutex, uint32_t timeout_ms)
{
    if (mutex == NULL) {
        return k_ra8_err_invalid_arg;
    }
    internal_mutex_t *const self = (internal_mutex_t *)(void *)mutex;
    if (self->magic != k_mutex_magic) {
        return k_ra8_err_invalid_state;
    }

    const uint32_t wait = fw_os_threadx_ticks_for(timeout_ms, (uint32_t)TX_TIMER_TICKS_PER_SECOND);
    const UINT status = tx_mutex_get(&self->tx, (ULONG)wait);
    return fw_os_threadx_err_for((uint32_t)status, timeout_ms != 0U);
}

ra8_err_t fw_os_mutex_unlock(fw_os_mutex_t *mutex)
{
    if (mutex == NULL) {
        return k_ra8_err_invalid_arg;
    }
    internal_mutex_t *const self = (internal_mutex_t *)(void *)mutex;
    if (self->magic != k_mutex_magic) {
        return k_ra8_err_invalid_state;
    }
    return fw_os_threadx_err_for((uint32_t)tx_mutex_put(&self->tx), false);
}

ra8_err_t fw_os_sem_init(fw_os_sem_t *sem, uint32_t initial_count)
{
    if (sem == NULL) {
        return k_ra8_err_invalid_arg;
    }
    internal_sem_t *const self = (internal_sem_t *)(void *)sem;
    const UINT status = tx_semaphore_create(&self->tx, s_unnamed, (ULONG)initial_count);
    if (status != TX_SUCCESS) {
        return fw_os_threadx_err_for((uint32_t)status, false);
    }
    self->magic = k_sem_magic;
    return k_ra8_ok;
}

ra8_err_t fw_os_sem_deinit(fw_os_sem_t *sem)
{
    if (sem == NULL) {
        return k_ra8_err_invalid_arg;
    }
    internal_sem_t *const self = (internal_sem_t *)(void *)sem;
    if (self->magic != k_sem_magic) {
        return k_ra8_err_invalid_state;
    }

    /* Same reason as the mutex: deleting a semaphore with a waiter on it
     * succeeds in ThreadX and wakes the waiter with TX_DELETED. fw_os.h
     * promises busy, so the suspension count decides. */
    CHAR *name = NULL;
    ULONG current = 0UL;
    TX_THREAD *first_suspended = NULL;
    ULONG suspended = 0UL;
    TX_SEMAPHORE *next = NULL;
    const UINT queried =
        tx_semaphore_info_get(&self->tx, &name, &current, &first_suspended, &suspended, &next);
    if (queried != TX_SUCCESS) {
        return fw_os_threadx_err_for((uint32_t)queried, false);
    }
    if (suspended != 0UL) {
        return k_ra8_err_busy;
    }

    const UINT status = tx_semaphore_delete(&self->tx);
    if (status != TX_SUCCESS) {
        return fw_os_threadx_err_for((uint32_t)status, false);
    }
    self->magic = 0U;
    return k_ra8_ok;
}

ra8_err_t fw_os_sem_take(fw_os_sem_t *sem, uint32_t timeout_ms)
{
    if (sem == NULL) {
        return k_ra8_err_invalid_arg;
    }
    internal_sem_t *const self = (internal_sem_t *)(void *)sem;
    if (self->magic != k_sem_magic) {
        return k_ra8_err_invalid_state;
    }

    const uint32_t wait = fw_os_threadx_ticks_for(timeout_ms, (uint32_t)TX_TIMER_TICKS_PER_SECOND);
    const UINT status = tx_semaphore_get(&self->tx, (ULONG)wait);
    return fw_os_threadx_err_for((uint32_t)status, timeout_ms != 0U);
}

ra8_err_t fw_os_sem_give(fw_os_sem_t *sem)
{
    if (sem == NULL) {
        return k_ra8_err_invalid_arg;
    }
    internal_sem_t *const self = (internal_sem_t *)(void *)sem;
    if (self->magic != k_sem_magic) {
        return k_ra8_err_invalid_state;
    }
    return fw_os_threadx_err_for((uint32_t)tx_semaphore_put(&self->tx), false);
}

uint32_t fw_os_uptime_ms(void)
{
    return fw_os_threadx_ms_for((uint32_t)tx_time_get(), (uint32_t)TX_TIMER_TICKS_PER_SECOND);
}

uint32_t fw_os_tick_hz(void)
{
    return (uint32_t)TX_TIMER_TICKS_PER_SECOND;
}
