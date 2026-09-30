/**
 * @file port/threadx/inc/fw_os_threadx.h
 * @brief Pure mapping arithmetic for the Eclipse ThreadX binding of `fw_os`.
 *
 * @details
 * [Ring 4 / RTOS Port] {World: NS}
 *
 * `port/threadx/src/fw_os_threadx.c` implements the `libs/if/inc/fw_os.h`
 * seam on top of ThreadX. Almost all of that file is a thin forward to a
 * `tx_*` call, but three pieces of it are real logic that can be wrong in
 * ways a compiler cannot catch: converting a millisecond bound into kernel
 * ticks, mapping the four-level portable priority band onto ThreadX's 0..31
 * scale, and turning a `UINT` ThreadX status into an ::ra8_err_t.
 *
 * Those three live here instead, as pure functions over plain integers, so
 * the host unit-test build can prove them without a scheduler, a bench, or
 * even the vendored ThreadX headers. This header deliberately includes no
 * ThreadX header at all: the ThreadX values it has to agree with are restated
 * as named constants below, and the implementation file static_asserts each
 * one against the real `tx_api.h` definition. If the vendor ever renumbers a
 * status code, the binding fails to compile rather than mapping it silently.
 *
 * @note Nothing here touches a control block, a register, or global state.
 *       Every function is a pure value transformation and is safe from any
 *       context, including an interrupt handler.
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

#include "ra8_err.h"

/**
 * @defgroup grp_fw_os_threadx_mirror Mirrored ThreadX values
 * @brief ThreadX constants restated so this header needs no vendor include.
 *
 * @details
 * Each of these is static_asserted against its `tx_api.h` counterpart in
 * `fw_os_threadx.c`. They exist so the arithmetic below is testable on a host
 * that has no ThreadX tree, not so anything may diverge from the vendor.
 * @{
 */

/** @brief Mirrors `TX_NO_WAIT`. */
#define K_FW_OS_TX_NO_WAIT (0UL)
/** @brief Mirrors `TX_WAIT_FOREVER`. */
#define K_FW_OS_TX_WAIT_FOREVER (0xFFFFFFFFUL)
/** @brief Mirrors `TX_MAX_PRIORITIES` from `port/threadx/inc/tx_user.h`. */
#define K_FW_OS_TX_MAX_PRIORITIES (32U)

/**
 * @enum fw_os_threadx_status_t
 * @brief The `tx_api.h` status codes this binding actually distinguishes.
 *
 * @details
 * A deliberately partial list. Every other ThreadX status collapses to
 * ::k_ra8_err_rtos_error, because a binding that invented a specific
 * ra8_err_t for a code it has never reasoned about would be guessing.
 */
typedef enum : uint32_t {
    k_fw_os_tx_success = 0x00U,        /**< `TX_SUCCESS`.        */
    k_fw_os_tx_deleted = 0x01U,        /**< `TX_DELETED`.        */
    k_fw_os_tx_ptr_error = 0x03U,      /**< `TX_PTR_ERROR`.      */
    k_fw_os_tx_wait_error = 0x04U,     /**< `TX_WAIT_ERROR`.     */
    k_fw_os_tx_size_error = 0x05U,     /**< `TX_SIZE_ERROR`.     */
    k_fw_os_tx_semaphore_error = 0x0CU, /**< `TX_SEMAPHORE_ERROR`. */
    k_fw_os_tx_no_instance = 0x0DU,    /**< `TX_NO_INSTANCE`.    */
    k_fw_os_tx_thread_error = 0x0EU,   /**< `TX_THREAD_ERROR`.   */
    k_fw_os_tx_priority_error = 0x0FU, /**< `TX_PRIORITY_ERROR`. */
    k_fw_os_tx_delete_error = 0x11U,   /**< `TX_DELETE_ERROR`.   */
    k_fw_os_tx_caller_error = 0x13U,   /**< `TX_CALLER_ERROR`.   */
    k_fw_os_tx_wait_aborted = 0x1AU,   /**< `TX_WAIT_ABORTED`.   */
    k_fw_os_tx_mutex_error = 0x1CU,    /**< `TX_MUTEX_ERROR`.    */
    k_fw_os_tx_not_available = 0x1DU,  /**< `TX_NOT_AVAILABLE`.  */
    k_fw_os_tx_not_owned = 0x1EU,      /**< `TX_NOT_OWNED`.      */
} fw_os_threadx_status_t;

/** @} */

/**
 * @defgroup grp_fw_os_threadx_map Pure mapping helpers
 * @brief The parts of the binding worth testing.
 * @{
 */

/**
 * @brief Convert an `fw_os` millisecond bound into a ThreadX wait option.
 *
 * @param[in] timeout_ms        `K_FW_OS_NO_WAIT`, `K_FW_OS_WAIT_FOREVER`, or a bound.
 * @param[in] ticks_per_second  The kernel tick rate, `TX_TIMER_TICKS_PER_SECOND`.
 * @return The `wait_option` to hand `tx_mutex_get` / `tx_semaphore_get`.
 *
 * @details
 * Rounds **up**, because `fw_os_mutex_lock` promises to wait at least the
 * bound it was given and a tick-granular kernel cannot do better. A bound that
 * would scale past ::K_FW_OS_TX_WAIT_FOREVER is clamped one tick below it, so
 * an overflowing finite wait can never become an infinite one. A zero tick
 * rate is treated as no-wait rather than dividing by zero.
 */
static inline uint32_t fw_os_threadx_ticks_for(uint32_t timeout_ms, uint32_t ticks_per_second)
{
    if (timeout_ms == 0U) {
        return (uint32_t)K_FW_OS_TX_NO_WAIT;
    }
    if (timeout_ms == UINT32_MAX) {
        return (uint32_t)K_FW_OS_TX_WAIT_FOREVER;
    }
    if (ticks_per_second == 0U) {
        return (uint32_t)K_FW_OS_TX_NO_WAIT;
    }

    const uint64_t scaled = ((uint64_t)timeout_ms * (uint64_t)ticks_per_second);
    const uint64_t rounded_up = (scaled + 999ULL) / 1000ULL;
    const uint64_t at_least_one = (rounded_up == 0ULL) ? 1ULL : rounded_up;
    const uint64_t ceiling = (uint64_t)K_FW_OS_TX_WAIT_FOREVER - 1ULL;

    return (at_least_one > ceiling) ? (uint32_t)ceiling : (uint32_t)at_least_one;
}

/**
 * @brief Convert a ThreadX tick count into milliseconds.
 *
 * @param[in] ticks             A `tx_time_get` reading.
 * @param[in] ticks_per_second  The kernel tick rate.
 * @return Milliseconds, truncated to 32 bits exactly as ::fw_os_uptime_ms
 *         documents. A zero tick rate yields zero.
 */
static inline uint32_t fw_os_threadx_ms_for(uint32_t ticks, uint32_t ticks_per_second)
{
    if (ticks_per_second == 0U) {
        return 0U;
    }
    const uint64_t ms = ((uint64_t)ticks * 1000ULL) / (uint64_t)ticks_per_second;
    return (uint32_t)(ms & 0xFFFFFFFFULL);
}

/**
 * @brief Map the portable priority band onto a ThreadX priority number.
 *
 * @param[in] band A ::fw_os_priority_t value, 0 (idle) through 3 (high).
 * @return A ThreadX priority in `[0, K_FW_OS_TX_MAX_PRIORITIES)`.
 *
 * @details
 * ThreadX counts **down**: 0 is the most urgent. The four bands are spread
 * across the scale rather than packed at one end, and the most urgent band
 * lands at 8 rather than 0 on purpose. Levels 0..7 stay reserved for the
 * composition root, so an application's own latency-critical thread can still
 * outrank anything a portable library starts. An out-of-range band is treated
 * as ::k_fw_os_priority_normal; the caller has already been rejected by
 * ::fw_os_thread_create's argument check, and this function must still return
 * a legal priority.
 */
static inline uint32_t fw_os_threadx_priority_for(uint32_t band)
{
    switch (band) {
        case 0U:
            return 31U;
        case 1U:
            return 24U;
        case 3U:
            return 8U;
        default:
            return 16U;
    }
}

/**
 * @brief Map a ThreadX status onto an ::ra8_err_t.
 *
 * @param[in] status  A `UINT` returned by a `tx_*` call.
 * @param[in] waited  True when the caller asked to wait at all, i.e. the
 *                    requested timeout was not `K_FW_OS_NO_WAIT`.
 * @return The ::ra8_err_t the `fw_os` contract promises for that outcome.
 *
 * @details
 * `waited` exists because ThreadX cannot tell the two apart: both "the mutex
 * was held and you said do not wait" and "you waited and the bound passed"
 * come back as `TX_NOT_AVAILABLE`. The seam distinguishes them, so the binding
 * recovers the difference from what the caller asked for.
 */
static inline ra8_err_t fw_os_threadx_err_for(uint32_t status, bool waited)
{
    switch (status) {
        case (uint32_t)k_fw_os_tx_success:
            return k_ra8_ok;
        case (uint32_t)k_fw_os_tx_not_available:
        case (uint32_t)k_fw_os_tx_no_instance:
            return waited ? k_ra8_err_timeout : k_ra8_err_would_block;
        case (uint32_t)k_fw_os_tx_wait_aborted:
        case (uint32_t)k_fw_os_tx_deleted:
            return k_ra8_err_timeout;
        case (uint32_t)k_fw_os_tx_not_owned:
            return k_ra8_err_access_denied;
        case (uint32_t)k_fw_os_tx_delete_error:
            return k_ra8_err_busy;
        case (uint32_t)k_fw_os_tx_caller_error:
            return k_ra8_err_invalid_state;
        case (uint32_t)k_fw_os_tx_ptr_error:
        case (uint32_t)k_fw_os_tx_wait_error:
        case (uint32_t)k_fw_os_tx_size_error:
        case (uint32_t)k_fw_os_tx_semaphore_error:
        case (uint32_t)k_fw_os_tx_thread_error:
        case (uint32_t)k_fw_os_tx_priority_error:
        case (uint32_t)k_fw_os_tx_mutex_error:
            return k_ra8_err_invalid_arg;
        default:
            return k_ra8_err_rtos_error;
    }
}

/** @} */

#ifdef __cplusplus
}
#endif
