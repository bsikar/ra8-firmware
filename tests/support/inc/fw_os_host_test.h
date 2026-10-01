/**
 * @file fw_os_host_test.h
 * @brief Host-build binding of the `fw_os` port, plus its test control surface.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 3 / Test] {World: NS}
 *
 * @details
 * The first binding of the OSAL seam declared in `libs/if/inc/fw_os.h` (RA8FW-299).
 * It is owned by the test tree rather than by `port/`, because the host build
 * is the only place the seam can be exercised without a bench or a linked
 * RTOS, and a production port with no consumer is a gap rather than progress.
 *
 * The binding is single-threaded and deterministic, which decides its two
 * interesting semantics honestly rather than by pretending:
 *
 *   - No thread this binding creates ever runs on its own, because nothing
 *     here schedules. ::fw_os_thread_create records the thread and reports
 *     success; a test runs the body when it wants one, through
 *     ::fw_os_host_test_run_thread. A binding that silently ran the entry on
 *     the caller's stack would be reporting concurrency it does not have.
 *   - A wait that cannot be satisfied cannot be satisfied later either, since
 *     no other thread can run to release the mutex or give the count. So a
 *     bounded wait reports ::k_ra8_err_timeout immediately and
 *     ::K_FW_OS_NO_WAIT reports ::k_ra8_err_would_block, and neither ever
 *     blocks. ::K_FW_OS_WAIT_FOREVER would deadlock a real scheduler here, so
 *     it reports ::k_ra8_err_timeout rather than spinning.
 *
 * Time is injected, not read from the host clock: ::fw_os_thread_sleep_ms
 * advances the binding's own millisecond counter, so a test that sleeps gets a
 * reproducible ::fw_os_uptime_ms instead of whatever the machine was doing.
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
#include <stdint.h>

#include "fw_os.h"
#include "ra8_err.h"

/**
 * @brief How many live threads this binding can record at once.
 *
 * @details
 * Small on purpose: the capacity exists so a test can reach the
 * ::k_ra8_err_no_mem branch of ::fw_os_thread_create without allocating a
 * realistic thread table.
 */
enum : uint32_t {
  k_fw_os_host_test_max_threads = 4U,
};

/**
 * @enum fw_os_host_test_call_t
 * @brief The seam calls this binding can be told to fail.
 *
 * @details
 * A caller with an RTOS-error branch cannot reach it on the host unless the
 * binding under it can be made to fail on demand. Without this, every such
 * branch is dead code in the host build, and the usual workaround is a
 * per-caller shim that re-declares the RTOS API, which is exactly the
 * coupling ::fw_os exists to remove.
 */
typedef enum : uint32_t {
  k_fw_os_host_test_call_none = 0U,     /**< Nothing armed.         */
  k_fw_os_host_test_call_thread_create, /**< ::fw_os_thread_create. */
  k_fw_os_host_test_call_thread_delete, /**< ::fw_os_thread_delete. */
  k_fw_os_host_test_call_mutex_init,    /**< ::fw_os_mutex_init.    */
  k_fw_os_host_test_call_mutex_deinit,  /**< ::fw_os_mutex_deinit.  */
  k_fw_os_host_test_call_mutex_lock,    /**< ::fw_os_mutex_lock.    */
  k_fw_os_host_test_call_mutex_unlock,  /**< ::fw_os_mutex_unlock.  */
  k_fw_os_host_test_call_sem_init,      /**< ::fw_os_sem_init.      */
  k_fw_os_host_test_call_sem_deinit,    /**< ::fw_os_sem_deinit.    */
  k_fw_os_host_test_call_sem_take,      /**< ::fw_os_sem_take.      */
  k_fw_os_host_test_call_sem_give,      /**< ::fw_os_sem_give.      */
} fw_os_host_test_call_t;

/**
 * @brief Make the next @p call fail once, with @p err.
 *
 * @details
 * One-shot: the arming is consumed by the first matching call, so a test
 * cannot leak a forced failure into the next case. Arming
 * ::k_fw_os_host_test_call_none disarms. The failure is injected before the
 * call does anything, so no state changes on a forced failure.
 *
 * @param[in] call Seam call whose next invocation must fail.
 * @param[in] err  Error the forced call returns. Must not be ::k_ra8_ok.
 *
 * @return ::k_ra8_ok when armed.
 * @retval k_ra8_err_invalid_arg @p err is ::k_ra8_ok, or @p call is out of
 *         range.
 *
 * @pre None.
 * @post The next matching seam call returns @p err and changes nothing.
 * @note Not thread-safe; the host build is single-threaded.
 * @since 0.1.0
 */
ra8_err_t fw_os_host_test_fail_next(fw_os_host_test_call_t call, ra8_err_t err);

/**
 * @brief Report whether an armed failure is still waiting to be consumed.
 *
 * @return True when a forced failure is armed.
 *
 * @pre None.
 * @post No state is mutated.
 * @note Not thread-safe; the host build is single-threaded.
 * @since 0.1.0
 */
bool fw_os_host_test_failure_armed(void);

/**
 * @brief Return this binding to its just-started state.
 *
 * @details
 * Forgets every recorded thread and resets the millisecond counter and the
 * yield count, and disarms any forced failure. Mutexes and semaphores live in
 * caller-owned storage, so they are unaffected: a test re-initialises those
 * itself.
 *
 * @return Nothing.
 *
 * @pre None.
 * @post No thread is recorded, the uptime is zero, no yield is counted and
 *       no forced failure is armed.
 * @note Not thread-safe; the host build is single-threaded.
 * @since 0.1.0
 */
void fw_os_host_test_reset(void);

/**
 * @brief Run one recorded thread body on the caller's stack.
 *
 * @details
 * The binding never runs a thread by itself, so this is how a test drives
 * one. The body runs to completion synchronously and the thread stays
 * recorded, so a test may run it again.
 *
 * @param[in] index Slot of the thread to run, in creation order.
 *
 * @return Result code.
 * @retval k_ra8_ok The body ran to completion.
 * @retval k_ra8_err_not_found No thread is recorded in @p index.
 *
 * @pre @p index names a thread created and not yet deleted.
 * @post The thread's entry has been called once with its recorded argument.
 * @note Not thread-safe; the host build is single-threaded.
 * @since 0.1.0
 */
ra8_err_t fw_os_host_test_run_thread(uint32_t index);

/**
 * @brief How many threads this binding currently holds.
 *
 * @return Count of recorded threads, never more than
 *         ::k_fw_os_host_test_max_threads.
 *
 * @pre None.
 * @post Binding state is unchanged.
 * @note Not thread-safe; the host build is single-threaded.
 * @since 0.1.0
 */
uint32_t fw_os_host_test_thread_count(void);

/**
 * @brief How many times ::fw_os_thread_yield has been called since the reset.
 *
 * @details
 * A yield has nothing to yield to here, so the count is the only observable
 * effect and the only thing a test can assert about it.
 *
 * @return Yield count since the last ::fw_os_host_test_reset.
 *
 * @pre None.
 * @post Binding state is unchanged.
 * @note Not thread-safe; the host build is single-threaded.
 * @since 0.1.0
 */
uint32_t fw_os_host_test_yield_count(void);

#ifdef __cplusplus
}
#endif
