/**
 * @file fw_os.h
 * @brief Architecture-neutral OS port: threads, mutexes, semaphores, time.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details
 * The OSAL seam of RA8FW-299, child (c) of epic RA8FW-298. Portable libraries state what
 * they need of an operating system here; a binding chosen by the composition
 * root supplies it. Nothing above this header names ThreadX.
 *
 * ## What this surface is derived from
 *
 * Not from what an RTOS offers, from what this tree actually calls. Every
 * declaration below backs a ThreadX call some first-party file makes today, and
 * the counts behind that claim are measured in `libs/if/README.md` and re-run
 * by `scripts/checks/check_measured_counts.py`. Two facts from that measurement
 * shaped the design:
 *
 *   - The coupling is almost entirely in `examples/`. Of the first-party files
 *     that call `tx_*`, only five sit in `libs/`: `ra8_wdt_supervisor`,
 *     `ra8_modem_at`, `ra8_time`, and the `ra8_fs` seam header. The seam's job
 *     is therefore to free those five and give the examples one thing to call,
 *     not to wrap ThreadX completely.
 *   - The surface is small. Threads, mutexes, semaphores and a clock read cover
 *     nearly all of it; queues appear in two files and byte pools in four.
 *
 * So queues and pools are OPTIONAL, capability-gated the way `arch/arch.h`
 * gates its optional surface, and a binding that declines one owes nothing for
 * it. Everything ungated is a MUST: a binding that cannot supply it is not a
 * binding.
 *
 * ## What is deliberately NOT here
 *
 * ThreadX's preemption threshold and time slice, its trace hooks, and its FPU
 * enable/disable pair. They are real ThreadX features with no portable
 * meaning, and a seam that carries them is a ThreadX header with a new prefix.
 * A binding that wants them exposes them in its own binding header, which the
 * composition root may name because the composition root already knows which
 * RTOS it picked.
 *
 * ## Time is milliseconds, not ticks
 *
 * `tx_time_get` returns ticks, and the tick rate is an RTOS build constant, so
 * a tick count means nothing to a portable caller without a second fact. This
 * port states durations and instants in milliseconds and offers
 * ::fw_os_tick_hz for the callers that genuinely need the underlying
 * resolution. A binding converts; the conversion is not the caller's problem.
 *
 * Handles and stacks are caller-owned. This interface performs no allocation
 * and contains no operating-system or device header.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @name Binding-supplied sizes and capabilities
 *
 * @details
 * `arch/arch.h` reads its capabilities from a real core's `caps.h`, because
 * two cores exist. No OS binding exists yet, so the contract carries defaults
 * a binding overrides from its own build instead of including a header that is
 * not there. The defaults are deliberately generous: a binding whose control
 * block does not fit says so by raising the constant, and the conformance
 * translation unit checks the storage really is large enough.
 * @{
 */

#ifndef K_FW_OS_THREAD_STORAGE_WORDS
/** @brief 64-bit words of caller-owned storage one thread needs. */
#define K_FW_OS_THREAD_STORAGE_WORDS (32U)
#endif
#ifndef K_FW_OS_MUTEX_STORAGE_WORDS
/** @brief 64-bit words of caller-owned storage one mutex needs. */
#define K_FW_OS_MUTEX_STORAGE_WORDS (16U)
#endif
#ifndef K_FW_OS_SEM_STORAGE_WORDS
/** @brief 64-bit words of caller-owned storage one semaphore needs. */
#define K_FW_OS_SEM_STORAGE_WORDS (16U)
#endif
#ifndef K_FW_OS_QUEUE_STORAGE_WORDS
/** @brief 64-bit words of caller-owned storage one queue needs. */
#define K_FW_OS_QUEUE_STORAGE_WORDS (16U)
#endif
#ifndef FW_OS_HAS_QUEUE
/**
 * @brief Whether the binding supplies message queues.
 *
 * @details
 * Declined by default. Two first-party files call `tx_queue_*`, so a binding
 * that has no native queue should leave this clear rather than emulate one.
 */
#define FW_OS_HAS_QUEUE (0)
#endif

/** @} */

/**
 * @defgroup grp_fw_os fw_os port
 * @brief What a portable library needs of an operating system.
 * @{
 */

/** @brief Wait with no timeout: block until the operation can proceed. */
#define K_FW_OS_WAIT_FOREVER (UINT32_MAX)
/** @brief Do not wait: fail with ::k_ra8_err_would_block instead of blocking. */
#define K_FW_OS_NO_WAIT (0UL)

/**
 * @brief Portable thread priority band.
 *
 * @details
 * RTOS priority scales disagree on direction and on width, so the port states a
 * band rather than a number and a binding maps it onto whatever its scheduler
 * uses. Four levels is what this tree's threads actually distinguish; a caller
 * that needs finer control than this is expressing an RTOS-specific scheduling
 * policy and belongs in the composition root, not in a portable library.
 */
typedef enum : uint8_t {
    k_fw_os_priority_idle = 0U,     /**< Runs only when nothing else can.      */
    k_fw_os_priority_low = 1U,      /**< Background work, no deadline.         */
    k_fw_os_priority_normal = 2U,   /**< The default for application threads.  */
    k_fw_os_priority_high = 3U,     /**< Latency-sensitive, still preemptible. */
} fw_os_priority_t;

/**
 * @struct fw_os_thread_t
 * @brief Caller-owned storage for one thread.
 *
 * @details
 * The binding's control block is opaque and its size is a binding fact, so the
 * caller supplies aligned storage and the binding places its block inside.
 * ::K_FW_OS_THREAD_STORAGE_WORDS comes from the selected binding's
 * `fw_os_caps.h`, which is why that header is included above rather than the
 * size being guessed here.
 */
typedef struct fw_os_thread_s {
    uint64_t storage[K_FW_OS_THREAD_STORAGE_WORDS]; /**< Binding-private, 8-byte aligned. */
} fw_os_thread_t;

/** @brief Caller-owned storage for one mutex. */
typedef struct fw_os_mutex_s {
    uint64_t storage[K_FW_OS_MUTEX_STORAGE_WORDS]; /**< Binding-private. */
} fw_os_mutex_t;

/** @brief Caller-owned storage for one counting semaphore. */
typedef struct fw_os_sem_s {
    uint64_t storage[K_FW_OS_SEM_STORAGE_WORDS]; /**< Binding-private. */
} fw_os_sem_t;

/**
 * @struct fw_os_thread_cfg_t
 * @brief Everything the port needs to start a thread.
 *
 * @details
 * `stack` is caller-owned and stays valid for the thread's whole life. `name`
 * is borrowed, not copied, for the same reason: this interface allocates
 * nothing.
 */
typedef struct fw_os_thread_cfg_s {
    const char *name;          /**< Borrowed, NUL-terminated, may be NULL. */
    void (*entry)(void *arg);  /**< Thread body.                           */
    void *arg;                 /**< Single argument handed to `entry`.     */
    void *stack;               /**< Caller-owned stack, 8-byte aligned.    */
    size_t stack_bytes;        /**< Size of `stack`.                       */
    fw_os_priority_t priority; /**< Portable band, mapped by the binding.  */
} fw_os_thread_cfg_t;

/**
 * @name MUST: threads
 *
 * 67 first-party files call `tx_thread_*` today, overwhelmingly
 * `tx_thread_create` and `tx_thread_sleep`.
 * @{
 */

/**
 * @brief Create and start a thread.
 *
 * @param[out] thread Caller-owned storage, uninitialised on entry.
 * @param[in]  cfg    Thread description. Not retained beyond this call, except
 *                    `stack` and `name`, which are borrowed for the thread's life.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for a malformed `cfg`, or
 *         ::k_ra8_err_no_mem when the binding has no thread slot left.
 *
 * @details
 * The thread is runnable when this returns. A binding that can only create
 * threads before the scheduler starts returns ::k_ra8_err_invalid_state rather
 * than deferring, so the caller learns the restriction at the call site.
 */
ra8_err_t fw_os_thread_create(fw_os_thread_t *thread, const fw_os_thread_cfg_t *cfg);

/**
 * @brief Stop and release a thread created by ::fw_os_thread_create.
 * @param[in,out] thread The thread to release.
 * @return ::k_ra8_ok, or ::k_ra8_err_invalid_state when a thread tries to
 *         delete itself and the binding cannot.
 */
ra8_err_t fw_os_thread_delete(fw_os_thread_t *thread);

/**
 * @brief Block the calling thread for at least `ms` milliseconds.
 *
 * @param[in] ms Milliseconds to sleep. Zero yields to any runnable thread of
 *               equal priority without blocking.
 *
 * @details
 * At least, never at most: the binding rounds up to its tick, so a sleep
 * shorter than one tick still costs one. A caller that needs a bound tighter
 * than ::fw_os_tick_hz allows is asking for a timer, not a sleep.
 */
void fw_os_thread_sleep_ms(uint32_t ms);

/**
 * @brief Yield the processor to any runnable thread of equal priority.
 */
void fw_os_thread_yield(void);

/** @} */

/**
 * @name MUST: mutual exclusion
 *
 * Five first-party files call `tx_mutex_*`.
 * @{
 */

/**
 * @brief Initialise a mutex.
 * @param[out] mutex     Caller-owned storage.
 * @param[in]  recursive True when the owner may lock it again without blocking.
 * @return ::k_ra8_ok, or ::k_ra8_err_not_supported when the binding has
 *         no recursive mode and one was asked for.
 */
ra8_err_t fw_os_mutex_init(fw_os_mutex_t *mutex, bool recursive);

/**
 * @brief Release a mutex initialised by ::fw_os_mutex_init.
 * @param[in,out] mutex The mutex to release.
 * @return ::k_ra8_ok, or ::k_ra8_err_busy when it is still held.
 */
ra8_err_t fw_os_mutex_deinit(fw_os_mutex_t *mutex);

/**
 * @brief Acquire a mutex.
 * @param[in,out] mutex      The mutex to acquire.
 * @param[in]     timeout_ms ::K_FW_OS_NO_WAIT, ::K_FW_OS_WAIT_FOREVER, or a bound.
 * @return ::k_ra8_ok, ::k_ra8_err_timeout, or ::k_ra8_err_would_block
 *         when ::K_FW_OS_NO_WAIT was given and the mutex was held.
 */
ra8_err_t fw_os_mutex_lock(fw_os_mutex_t *mutex, uint32_t timeout_ms);

/**
 * @brief Release a mutex the calling thread holds.
 * @param[in,out] mutex The mutex to release.
 * @return ::k_ra8_ok, or ::k_ra8_err_access_denied when the caller is not
 *         the owner.
 */
ra8_err_t fw_os_mutex_unlock(fw_os_mutex_t *mutex);

/** @} */

/**
 * @name MUST: counting semaphores
 *
 * Eight first-party files call `tx_semaphore_*`.
 * @{
 */

/**
 * @brief Initialise a counting semaphore.
 * @param[out] sem           Caller-owned storage.
 * @param[in]  initial_count Starting count.
 * @return ::k_ra8_ok or ::k_ra8_err_invalid_arg.
 */
ra8_err_t fw_os_sem_init(fw_os_sem_t *sem, uint32_t initial_count);

/**
 * @brief Release a semaphore initialised by ::fw_os_sem_init.
 * @param[in,out] sem The semaphore to release.
 * @return ::k_ra8_ok, or ::k_ra8_err_busy when a thread is waiting on it.
 */
ra8_err_t fw_os_sem_deinit(fw_os_sem_t *sem);

/**
 * @brief Take one count, blocking until one is available or the bound passes.
 * @param[in,out] sem        The semaphore.
 * @param[in]     timeout_ms ::K_FW_OS_NO_WAIT, ::K_FW_OS_WAIT_FOREVER, or a bound.
 * @return ::k_ra8_ok, ::k_ra8_err_timeout, or ::k_ra8_err_would_block.
 */
ra8_err_t fw_os_sem_take(fw_os_sem_t *sem, uint32_t timeout_ms);

/**
 * @brief Add one count, waking a waiter if there is one.
 * @param[in,out] sem The semaphore.
 * @return ::k_ra8_ok. Safe from an interrupt handler on every binding;
 *         this is the only call in this header for which that is promised.
 */
ra8_err_t fw_os_sem_give(fw_os_sem_t *sem);

/** @} */

/**
 * @name MUST: time
 *
 * `tx_time_get` plus the direct SysTick reach-ins this seam exists to remove:
 * six first-party files include `ra8_systick.h` today, and `ra8_time.c` is one
 * of them.
 * @{
 */

/**
 * @brief Milliseconds since the scheduler started.
 *
 * @return A count that wraps at 2^32 ms, roughly every 49.7 days. Callers
 *         compare differences rather than absolute values, which stays correct
 *         across the wrap for intervals under the period.
 */
uint32_t fw_os_uptime_ms(void);

/**
 * @brief The binding's underlying tick rate, in hertz.
 *
 * @return Ticks per second. A caller needs this only to reason about the
 *         resolution of ::fw_os_thread_sleep_ms and the timeouts above; the
 *         port itself never states a duration in ticks.
 */
uint32_t fw_os_tick_hz(void);

/** @} */

#if FW_OS_HAS_QUEUE

/**
 * @name OPTIONAL: message queues
 *
 * Declared when the binding's `fw_os_caps.h` sets ::FW_OS_HAS_QUEUE. Two
 * first-party files call `tx_queue_*`, which is why this is optional: a binding
 * without native queues should decline rather than emulate one badly.
 * @{
 */

/** @brief Caller-owned storage for one queue. */
typedef struct fw_os_queue_s {
    uint64_t storage[K_FW_OS_QUEUE_STORAGE_WORDS]; /**< Binding-private. */
} fw_os_queue_t;

/**
 * @brief Initialise a fixed-size message queue over caller-owned memory.
 * @param[out] queue        Caller-owned control storage.
 * @param[in]  buffer       Caller-owned backing memory, valid for the queue's life.
 * @param[in]  buffer_bytes Size of `buffer`.
 * @param[in]  msg_bytes    Size of one message; every send and receive uses it.
 * @return ::k_ra8_ok, or ::k_ra8_err_invalid_size when `buffer_bytes` is
 *         not a whole number of messages.
 */
ra8_err_t fw_os_queue_init(fw_os_queue_t *queue, void *buffer, size_t buffer_bytes,
                           size_t msg_bytes);

/**
 * @brief Release a queue initialised by ::fw_os_queue_init.
 * @param[in,out] queue The queue to release.
 * @return ::k_ra8_ok, or ::k_ra8_err_busy when a thread is waiting on it.
 */
ra8_err_t fw_os_queue_deinit(fw_os_queue_t *queue);

/**
 * @brief Copy one message into the queue.
 * @param[in,out] queue      The queue.
 * @param[in]     msg        Message to copy, `msg_bytes` long.
 * @param[in]     timeout_ms ::K_FW_OS_NO_WAIT, ::K_FW_OS_WAIT_FOREVER, or a bound.
 * @return ::k_ra8_ok, ::k_ra8_err_timeout, or ::k_ra8_err_would_block.
 */
ra8_err_t fw_os_queue_send(fw_os_queue_t *queue, const void *msg, uint32_t timeout_ms);

/**
 * @brief Copy one message out of the queue.
 * @param[in,out] queue      The queue.
 * @param[out]    msg        Receives `msg_bytes`.
 * @param[in]     timeout_ms ::K_FW_OS_NO_WAIT, ::K_FW_OS_WAIT_FOREVER, or a bound.
 * @return ::k_ra8_ok, ::k_ra8_err_timeout, or ::k_ra8_err_would_block.
 */
ra8_err_t fw_os_queue_receive(fw_os_queue_t *queue, void *msg, uint32_t timeout_ms);

/** @} */

#endif /* FW_OS_HAS_QUEUE */

/*
 * The port's own invariants, checked against whichever binding's fw_os_caps.h
 * the build selected. A binding whose caps contradict the contract fails to
 * build rather than producing a port nobody can call.
 */
static_assert(K_FW_OS_NO_WAIT != K_FW_OS_WAIT_FOREVER,
              "the no-wait and wait-forever sentinels must be distinguishable");
static_assert(K_FW_OS_THREAD_STORAGE_WORDS > 0U,
              "fw_os_caps.h: a thread control block cannot be zero-sized");
static_assert(K_FW_OS_MUTEX_STORAGE_WORDS > 0U,
              "fw_os_caps.h: a mutex control block cannot be zero-sized");
static_assert(K_FW_OS_SEM_STORAGE_WORDS > 0U,
              "fw_os_caps.h: a semaphore control block cannot be zero-sized");

/** @} */

#ifdef __cplusplus
}
#endif
