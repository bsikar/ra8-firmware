/**
 * @file port/nimble/inc/nimble_npl_threadx.h
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
 * ``threadx_fs_demo``.
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
 * @warning UNVALIDATED SCAFFOLD (issue #286): this NimBLE port and its
 * ThreadX Native Porting Layer link and pass the static gates, but have
 * NEVER been hardware-validated and are NOT emulator-gated -- ra8_emulator models
 * no RA8D2 BLE controller / HCI mailbox, and the underlying ra8_ble
 * transport is itself unproven on this board (see #86, #91). Treat every
 * symbol here as a link-only stub, not a working BLE stack. Consumers stay
 * under ``examples/_unsupported/`` until a NimBLE app is driven to real
 * hardware validation and promoted out of that tier.
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
 * Upstream declares the entire NPL function surface and pulls the
 * OS-specific structs in through ``nimble/nimble_npl_os.h``, which this
 * port supplies (``port/nimble/inc/nimble/nimble_npl_os.h``). Including
 * it here is what puts a prototype in front of every ``ble_npl_*``
 * definition in ``nimble_npl_threadx.c``; the port used to restate the
 * types by hand and restate none of the functions, so nothing checked
 * those definitions against the contract they implement.
 */
#include "nimble/nimble_npl.h"

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

/* =============================================================================
 * NPL constants
 * =============================================================================
 */

/**
 * @enum ble_npl_threadx_limits_t
 * @brief Sizing constants for the ThreadX-backed NPL.
 */
typedef enum : uint16_t {
  /** Wait forever -- maps onto ``TX_WAIT_FOREVER``. */
  k_ble_npl_threadx_wait_forever = 0xFFFFU,
  /** Default eventq depth (number of pointer slots). */
  k_ble_npl_threadx_eventq_depth = 16U,
  /** Bytes of storage per eventq slot (one pointer on M85). */
  k_ble_npl_threadx_eventq_slot_bytes = 4U,
  /** Default sem max value -- effectively unbounded for our use case. */
  k_ble_npl_threadx_sem_max = 0xFFFFU,
} ble_npl_threadx_limits_t;

#ifdef __cplusplus
}
#endif
