/**
 * @file port/nimble/inc/nimble/nimble_npl_os.h
 * @brief ThreadX-backed OS types for the NimBLE Native Porting Layer
 *
 * @par Tag
 * [Ring 4 / PORT] {World: S}
 *
 * @details
 * Upstream ``nimble/nimble_npl.h`` declares the whole NPL function
 * surface and then does ``#include "nimble/nimble_npl_os.h"`` to pick up
 * the OS-specific struct definitions the port supplies. This is that
 * file for the ThreadX port: it is found on the include path because
 * ``port/nimble/inc`` is on it, exactly as upstream's own ports
 * (``porting/npl/freertos/include/nimble/nimble_npl_os.h`` and friends)
 * are found on theirs.
 *
 * Splitting these types out of ``nimble_npl_threadx.h`` is what lets the
 * port include the upstream header instead of hand-copying its
 * declarations. The copies drifted out of the compiler's sight: nothing
 * declared ``ble_npl_mutex_init`` and the other 30-odd NPL entry points
 * in a header the implementation TU could see, so their definitions were
 * never checked against the contract they implement.
 *
 * Mapping:
 *
 *   | NimBLE primitive       | ThreadX primitive  |
 *   |------------------------|--------------------|
 *   | ``ble_npl_mutex``      | ``TX_MUTEX``       |
 *   | ``ble_npl_sem``        | ``TX_SEMAPHORE``   |
 *   | ``ble_npl_eventq``     | ``TX_QUEUE``       |
 *   | ``ble_npl_callout``    | ``TX_TIMER``       |
 *   | ``ble_npl_time_t``     | ``ULONG`` (ticks)  |
 *   | critical section       | TX interrupt mask  |
 *
 * Tick rate matches the project ``tx_user.h``
 * (``TX_TIMER_TICKS_PER_SECOND = 1000``), so 1 NPL tick == 1 ms.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/*
 * Pull in ThreadX so the struct definitions below have ``TX_MUTEX``
 * etc. The host unit-test build (``RA8_SIMULATOR_MODE``) does not
 * cross-link ThreadX -- this file is only compiled into the
 * cross-target build, so the include is unconditional.
 */
#include "tx_api.h"

/**
 * @def BLE_NPL_OS_ALIGNMENT
 * @brief Pool alignment upstream ``os_mempool.h`` sizes ``os_membuf_t`` from.
 * @details Four bytes: the M85 ABI aligns ``uint32_t`` and pointers to 4.
 * @note Read by vendored NimBLE headers, never by first-party code.
 * @since 0.1.0
 */
#define BLE_NPL_OS_ALIGNMENT (4)

/**
 * @def BLE_NPL_TIME_FOREVER
 * @brief Tick value meaning "block indefinitely".
 * @details Maps onto ``TX_WAIT_FOREVER`` inside the NPL implementation.
 * @note Read by vendored NimBLE headers, never by first-party code.
 * @since 0.1.0
 */
#define BLE_NPL_TIME_FOREVER UINT32_MAX

/** @brief Tick-count type used by the NPL. ThreadX ULONG matches. */
typedef uint32_t ble_npl_time_t;

/** @brief Signed tick delta, for timeouts expressed relative to now. */
typedef int32_t ble_npl_stime_t;

/**
 * @struct ble_npl_event
 * @brief NimBLE event opaque -- pairs a callback with an argument.
 *
 * @details Posted into a ``ble_npl_eventq`` by callers and dispatched
 * by ``ble_npl_eventq_get``. Mirrors the FreeRTOS port's struct.
 *
 * @invariant ``queued`` is 1 exactly while the event sits in a queue.
 *
 * @par Example:
 * @code
 * struct ble_npl_event ev;
 * ble_npl_event_init(&ev, on_event, ctx);
 * @endcode
 *
 * @see ble_npl_eventq
 */
struct ble_npl_event {
  uint8_t           queued; /**< 1 while sitting in an evq, 0 otherwise.    */
  ble_npl_event_fn* fn;     /**< Callback invoked by ``ble_npl_event_run``. */
  void*             arg;    /**< Opaque user argument passed to fn().       */
};

/**
 * @struct ble_npl_eventq
 * @brief Event queue mapped onto ``TX_QUEUE``.
 *
 * @details
 * The queue carries pointers (one ``struct ble_npl_event *`` per
 * slot). ThreadX wants the message size in 32-bit words: we pass
 * 1 word and the queue depth comes from ``k_ble_npl_eventq_depth``.
 *
 * @invariant ``storage`` is sized for the configured queue depth.
 *
 * @par Example:
 * @code
 * struct ble_npl_eventq evq;
 * ble_npl_eventq_init(&evq);
 * @endcode
 *
 * @see ble_npl_event
 */
struct ble_npl_eventq {
  TX_QUEUE q;           /**< Backing ThreadX queue.                  */
  uint8_t  storage[64]; /**< Backing storage (16 entries * 4 bytes). */
};

/**
 * @struct ble_npl_callout
 * @brief One-shot timer that posts an event into a queue when it fires.
 *
 * @details Wraps a ``TX_TIMER`` and the ``ble_npl_event`` it should
 * post. ``priv_callout_trampoline`` reads the event pointer out of
 * the timer's user-data field and pushes it via ``ble_npl_eventq_put``.
 *
 * @invariant ``evq`` stays the queue passed to ``ble_npl_callout_init``.
 *
 * @par Example:
 * @code
 * struct ble_npl_callout co;
 * ble_npl_callout_init(&co, &evq, on_timeout, ctx);
 * @endcode
 *
 * @see ble_npl_eventq
 */
struct ble_npl_callout {
  TX_TIMER               handle; /**< Backing ThreadX one-shot timer. */
  struct ble_npl_eventq* evq;    /**< Queue to post the event into.   */
  struct ble_npl_event   ev;     /**< Event posted on each firing.    */
};

/**
 * @struct ble_npl_mutex
 * @brief Mutex mapped onto ``TX_MUTEX``.
 *
 * @details Priority inheritance is left at the ThreadX default chosen by
 * ``ble_npl_mutex_init``.
 *
 * @invariant ``handle`` is a created TX_MUTEX after a successful init.
 *
 * @par Example:
 * @code
 * struct ble_npl_mutex mu;
 * ble_npl_mutex_init(&mu);
 * @endcode
 *
 * @see ble_npl_sem
 */
struct ble_npl_mutex {
  TX_MUTEX handle; /**< Backing ThreadX mutex. */
};

/**
 * @struct ble_npl_sem
 * @brief Counting semaphore mapped onto ``TX_SEMAPHORE``.
 *
 * @details Initial and maximum counts come from the caller and
 * ``k_ble_npl_threadx_sem_max`` respectively.
 *
 * @invariant ``handle`` is a created TX_SEMAPHORE after a successful init.
 *
 * @par Example:
 * @code
 * struct ble_npl_sem sem;
 * ble_npl_sem_init(&sem, 0U);
 * @endcode
 *
 * @see ble_npl_mutex
 */
struct ble_npl_sem {
  TX_SEMAPHORE handle; /**< Backing ThreadX counting semaphore. */
};

#ifdef __cplusplus
}
#endif
