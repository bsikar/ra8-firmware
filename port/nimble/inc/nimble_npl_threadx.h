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
 * definition in ``nimble_npl_threadx.c``. ``nimble/nimble_port.h``
 * supplies ``nimble_port_init``, ``nimble_port_run``, and
 * ``nimble_port_get_dflt_eventq`` from the compiled NimBLE porting TU.
 */
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"

/* =============================================================================
 * NPL constants
 * =============================================================================
 */

/**
 * @enum ble_npl_threadx_limits_t
 * @brief Sizing constants for the ThreadX-backed NPL.
 */
typedef enum : uint16_t {
  /** Legacy 16-bit sentinel kept only for internal sizing bounds. */
  k_ble_npl_threadx_wait_forever = 0xFFFFU,
  /** Default eventq depth (number of pointer slots). */
  k_ble_npl_threadx_eventq_depth = 64U,
  /** Bytes of storage per eventq slot (one pointer on M85). */
  k_ble_npl_threadx_eventq_slot_bytes = 4U,
  /** Default sem max value -- effectively unbounded for our use case. */
  k_ble_npl_threadx_sem_max = 0xFFFFU,
} ble_npl_threadx_limits_t;

#ifdef __cplusplus
}
#endif
