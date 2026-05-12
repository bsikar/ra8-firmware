/**
 * @file nimble_npl_threadx.h
 * @brief NimBLE Native Porting Layer mapping onto Eclipse ThreadX
 *
 * @par Tag
 * [Ring 4 / PORT] {World: S}
 *
 * @details
 * Apache NimBLE expects a small set of OS primitives -- mutex,
 * semaphore, event-queue, callout (one-shot timer that posts an
 * event), tick counter, critical section -- collectively the
 * "Native Porting Layer" (NPL). Upstream ships canned ports for
 * FreeRTOS, Mynewt, Linux, NuttX, RIOT and "dummy". We provide a
 * ThreadX-backed implementation here so NimBLE's host stack runs
 * on the same kernel that powers ``threadx_blink`` and
 * ``threadx_filex_demo``.
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
 * etc. The host unit-test build (``RA_SIMULATOR_MODE``) does not
 * cross-link ThreadX -- this file is only compiled into the
 * cross-target build, so the include is unconditional.
 */
#include "tx_api.h"

/* =============================================================================
 * Forward decls (NimBLE NPL)
 * =============================================================================
 *
 * These match libs/third_party/nimble/nimble/include/nimble/nimble_npl.h
 * verbatim. We re-declare here so this header can stand alone for
 * build systems that compile the .c without first walking the
 * upstream NPL header.
 */

struct ble_npl_event;
typedef void ble_npl_event_fn(struct ble_npl_event* ev);

/** @brief Tick-count type used by the NPL. ThreadX ULONG matches. */
typedef uint32_t ble_npl_time_t;

/**
 * @struct ble_npl_event
 * @brief NimBLE event opaque -- pairs a callback with an argument.
 *
 * @details Posted into a ``ble_npl_eventq`` by callers and dispatched
 * by ``ble_npl_eventq_get``. Mirrors the FreeRTOS port's struct.
 */
struct ble_npl_event {
  uint8_t           queued; /**< 1 while sitting in an evq, 0 otherwise. */
  ble_npl_event_fn* fn;     /**< Callback invoked by ``ble_npl_event_run``. */
  void*             arg;    /**< Opaque user argument passed to fn().      */
};

/**
 * @struct ble_npl_eventq
 * @brief Event queue mapped onto ``TX_QUEUE``.
 *
 * @details
 * The queue carries pointers (one ``struct ble_npl_event *`` per
 * slot). ThreadX wants the message size in 32-bit words: we pass
 * 1 word and the queue depth comes from ``k_ble_npl_eventq_depth``.
 */
struct ble_npl_eventq {
  TX_QUEUE q;           /**< Backing ThreadX queue.                       */
  uint8_t  storage[64]; /**< Backing storage (16 entries * 4 bytes). */
};

/**
 * @struct ble_npl_callout
 * @brief One-shot timer that posts an event into a queue when it fires.
 *
 * @details Wraps a ``TX_TIMER`` and the ``ble_npl_event`` it should
 * post. ``priv_callout_trampoline`` reads the event pointer out of
 * the timer's user-data field and pushes it via ``ble_npl_eventq_put``.
 */
struct ble_npl_callout {
  TX_TIMER               handle; /**< Backing ThreadX one-shot timer. */
  struct ble_npl_eventq* evq;    /**< Queue to post the event into.    */
  struct ble_npl_event   ev;     /**< Event posted on each firing.     */
};

/** @brief Mutex mapped onto ``TX_MUTEX``. */
struct ble_npl_mutex {
  TX_MUTEX handle; /**< Backing ThreadX mutex.                          */
};

/* =============================================================================
 * NimBLE port API (matches porting/nimble/include/nimble/nimble_port.h)
 * =============================================================================
 */

/**
 * @brief Bring the NimBLE host port up.
 *
 * @details
 * Initialises the default eventq used by the host stack. The
 * adapter implementation lives in ``nimble_npl_threadx.c`` and is
 * marked weak so the upstream NimBLE host TUs can supersede it
 * once they are wired into the build.
 *
 * @pre ThreadX kernel is running (``tx_kernel_enter`` returned).
 * @post ``nimble_port_get_dflt_eventq`` returns a usable queue.
 *
 * @since 0.1.0
 *
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
void nimble_port_init(void);

/**
 * @brief Pump the NimBLE host event loop forever.
 *
 * @details Pulls events out of the default eventq and calls
 * ``ble_npl_event_run`` on each. Never returns.
 *
 * @pre ``nimble_port_init`` has been called.
 * @post Calling thread runs the host loop until shutdown.
 *
 * @since 0.1.0
 *
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
void nimble_port_run(void);

/**
 * @brief Return the host's default eventq pointer.
 *
 * @return Pointer to the default ``struct ble_npl_eventq`` (never NULL
 *         after ``nimble_port_init``).
 *
 * @pre ``nimble_port_init`` has been called.
 *
 * @since 0.1.0
 */
struct ble_npl_eventq* nimble_port_get_dflt_eventq(void);

/** @brief Counting semaphore mapped onto ``TX_SEMAPHORE``. */
struct ble_npl_sem {
  TX_SEMAPHORE handle; /**< Backing ThreadX counting semaphore.        */
};

/* =============================================================================
 * NPL constants
 * =============================================================================
 */

/**
 * @enum ble_npl_threadx_limits_t
 * @brief Sizing constants for the ThreadX-backed NPL.
 */
typedef enum : uint16_t {
  /** Wait forever -- maps onto ``TX_WAIT_FOREVER``.                    */
  k_ble_npl_threadx_wait_forever = 0xFFFFU,
  /** Default eventq depth (number of pointer slots).                   */
  k_ble_npl_threadx_eventq_depth = 16U,
  /** Bytes of storage per eventq slot (one pointer on M85).            */
  k_ble_npl_threadx_eventq_slot_bytes = 4U,
  /** Default sem max value -- effectively unbounded for our use case.  */
  k_ble_npl_threadx_sem_max = 0xFFFFU,
} ble_npl_threadx_limits_t;

#ifdef __cplusplus
}
#endif
