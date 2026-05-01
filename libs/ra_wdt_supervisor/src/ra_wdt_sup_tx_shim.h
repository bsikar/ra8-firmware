/**
 * @file ra_wdt_sup_tx_shim.h
 * @brief ThreadX shim for ra_wdt_supervisor (host unit-test build only)
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details
 * On the cross-compiled target, the supervisor pulls in the real
 * ``tx_api.h``. On the host unit-test build (``RA_SIMULATOR_MODE``)
 * ThreadX is not linked, so this header provides degenerate stand-ins
 * for the handful of TX symbols the supervisor uses. The stubs let
 * the implementation file compile and link unchanged; tests drive the
 * supervisor synchronously through ``ra_wdt_supervisor_tick``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @typedef ULONG
 * @brief ThreadX-compatible unsigned long (host stub).
 */
typedef unsigned long ULONG;

/**
 * @typedef UINT
 * @brief ThreadX-compatible unsigned int (host stub).
 */
typedef unsigned int UINT;

/**
 * @typedef CHAR
 * @brief ThreadX-compatible CHAR (host stub).
 */
typedef char CHAR;

/** @def TX_SUCCESS Successful ThreadX status code (host stub). */
#define TX_SUCCESS 0U
/** @def TX_NO_INHERIT Mutex priority-inherit disable flag (host stub). */
#define TX_NO_INHERIT 0U
/** @def TX_WAIT_FOREVER Block-forever wait option (host stub). */
#define TX_WAIT_FOREVER 0xFFFFFFFFUL
/** @def TX_AUTO_START Auto-start flag (host stub). */
#define TX_AUTO_START 1U
/** @def TX_NO_TIME_SLICE Disable time-slicing flag (host stub). */
#define TX_NO_TIME_SLICE 0U

/**
 * @struct TX_MUTEX
 * @brief Opaque mutex stand-in for the host build.
 */
typedef struct {
  uint32_t magic; /**< Sentinel for "created". */
} TX_MUTEX;

/**
 * @struct TX_THREAD
 * @brief Opaque thread stand-in for the host build.
 */
typedef struct {
  uint32_t magic; /**< Sentinel for "created". */
} TX_THREAD;

/** @brief Host stub for tx_mutex_create. */
static inline UINT tx_mutex_create(TX_MUTEX* m, CHAR* name, UINT inherit)
{
  (void)name;
  (void)inherit;
  if (m != ((void*)0)) {
    m->magic = 0xA5A5A5A5U;
  }
  return TX_SUCCESS;
}

/** @brief Host stub for tx_mutex_get. */
static inline UINT tx_mutex_get(TX_MUTEX* m, ULONG wait)
{
  (void)m;
  (void)wait;
  return TX_SUCCESS;
}

/** @brief Host stub for tx_mutex_put. */
static inline UINT tx_mutex_put(TX_MUTEX* m)
{
  (void)m;
  return TX_SUCCESS;
}

/** @brief Host stub for tx_mutex_delete. */
static inline UINT tx_mutex_delete(TX_MUTEX* m)
{
  (void)m;
  return TX_SUCCESS;
}

/** @brief Host stub for tx_thread_create. */
static inline UINT tx_thread_create(TX_THREAD* t,
                                    CHAR*      name,
                                    void (*entry)(ULONG),
                                    ULONG      arg,
                                    void*      stack,
                                    ULONG      stack_size,
                                    UINT       prio,
                                    UINT       preempt_thresh,
                                    UINT       slice,
                                    UINT       autostart)
{
  (void)name;
  (void)entry;
  (void)arg;
  (void)stack;
  (void)stack_size;
  (void)prio;
  (void)preempt_thresh;
  (void)slice;
  (void)autostart;
  if (t != ((void*)0)) {
    t->magic = 0x5A5A5A5AU;
  }
  return TX_SUCCESS;
}

/** @brief Host stub for tx_thread_terminate. */
static inline UINT tx_thread_terminate(TX_THREAD* t)
{
  (void)t;
  return TX_SUCCESS;
}

/** @brief Host stub for tx_thread_delete. */
static inline UINT tx_thread_delete(TX_THREAD* t)
{
  (void)t;
  return TX_SUCCESS;
}

/** @brief Host stub for tx_thread_sleep. */
static inline UINT tx_thread_sleep(ULONG ticks)
{
  (void)ticks;
  return TX_SUCCESS;
}

/** @brief Host stub for tx_time_get. */
static inline ULONG tx_time_get(void)
{
  return 0UL;
}

#ifdef __cplusplus
}
#endif
