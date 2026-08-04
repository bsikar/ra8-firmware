/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_wdt_sup_tx_shim_internal.h
 * @brief ThreadX shim for ra8_wdt_supervisor (host unit-test build only)
 * @ingroup grp_system
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details
 * On the cross-compiled target, the supervisor pulls in the real
 * ``tx_api.h``. On the host unit-test build (``RA8_OFF_TARGET``)
 * ThreadX is not linked, so this header provides degenerate stand-ins
 * for the handful of TX symbols the supervisor uses. The stubs let
 * the implementation file compile and link unchanged; tests drive the
 * supervisor synchronously through ``ra8_wdt_supervisor_tick``.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* The five ThreadX type aliases (ULONG, UINT, CHAR, TX_MUTEX, TX_THREAD)
 * intentionally violate the project's lower_case typedef rule because
 * they MUST mirror the real ThreadX ``tx_api.h`` spelling: this header
 * stands in for the vendor header on the host build, and the supervisor
 * source uses the upper-case names the vendor API mandates. */

/**
 * @typedef ULONG
 * @brief ThreadX-compatible unsigned long (host stub).
 */
typedef unsigned long ULONG; /* NOLINT(readability-identifier-naming) -- ThreadX name. */

/**
 * @typedef UINT
 * @brief ThreadX-compatible unsigned int (host stub).
 */
typedef unsigned int UINT; /* NOLINT(readability-identifier-naming) -- ThreadX name. */

/**
 * @typedef CHAR
 * @brief ThreadX-compatible CHAR (host stub).
 */
typedef char CHAR; /* NOLINT(readability-identifier-naming) -- ThreadX name. */

/** @def TX_SUCCESS Successful ThreadX status code (host stub). */
#define TX_SUCCESS (0U)
/** @def TX_NO_INHERIT Mutex priority-inherit disable flag (host stub). */
#define TX_NO_INHERIT (0U)
/** @def TX_WAIT_FOREVER Block-forever wait option (host stub). */
#define TX_WAIT_FOREVER (0xFFFFFFFFUL)
/** @def TX_AUTO_START Auto-start flag (host stub). */
#define TX_AUTO_START (1U)
/** @def TX_NO_TIME_SLICE Disable time-slicing flag (host stub). */
#define TX_NO_TIME_SLICE (0U)

/**
 * @enum ra8_wdt_sup_tx_shim_canary_t
 * @brief Canary patterns stamped into host-stub TX_MUTEX / TX_THREAD blocks.
 *
 * @details
 * The host shim does not implement real ThreadX semantics; it only needs a
 * way to mark a control block as "created" so unit tests that introspect
 * the structures can tell the difference. Two distinct canaries are used
 * so a stray pointer pun between mutex and thread is detectable. Per
 * CLAUDE.md no integer literals are allowed in source files -- these are
 * named typed enums.
 */
typedef enum : uint32_t {
  k_ra8_wdt_sup_tx_shim_mutex_canary = 0xA5A5A5A5U, /**< Stamped into TX_MUTEX::magic on create. */
  k_ra8_wdt_sup_tx_shim_thread_canary =
    0x5A5A5A5AU, /**< Stamped into TX_THREAD::magic on create. */
} ra8_wdt_sup_tx_shim_canary_t;

/**
 * @struct TX_MUTEX
 * @brief Opaque mutex stand-in for the host build.
 */
typedef struct {
  uint32_t magic; /**< Sentinel for "created".                              */
} TX_MUTEX;       /* NOLINT(readability-identifier-naming) -- ThreadX name. */

/**
 * @struct TX_THREAD
 * @brief Opaque thread stand-in for the host build.
 */
typedef struct {
  uint32_t magic; /**< Sentinel for "created".                              */
} TX_THREAD;      /* NOLINT(readability-identifier-naming) -- ThreadX name. */

/* NOLINTBEGIN(readability-non-const-parameter) -- ThreadX pins CHAR* name non-const. */
/**
 * @brief Host stub for tx_mutex_create.
 *
 * @details See implementation.
 * @param[in] m See implementation.
 * @param[in] name See implementation.
 * @param[in] inherit See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline UINT tx_mutex_create(TX_MUTEX* m, CHAR* name, UINT inherit)
{
  (void)name;
  (void)inherit;
  if (m != ((void*)0)) {
    m->magic = (uint32_t)k_ra8_wdt_sup_tx_shim_mutex_canary;
  }
  return TX_SUCCESS;
}
/* NOLINTEND(readability-non-const-parameter) */

/**
 * @brief Host stub for tx_mutex_get.
 *
 * @details See implementation.
 * @param[in] m See implementation.
 * @param[in] wait See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline UINT tx_mutex_get(TX_MUTEX* m, ULONG wait)
{
  (void)m;
  (void)wait;
  return TX_SUCCESS;
}

/**
 * @brief Host stub for tx_mutex_put.
 *
 * @details See implementation.
 * @param[in] m See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline UINT tx_mutex_put(TX_MUTEX* m)
{
  (void)m;
  return TX_SUCCESS;
}

/**
 * @brief Host stub for tx_mutex_delete.
 *
 * @details See implementation.
 * @param[in] m See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline UINT tx_mutex_delete(TX_MUTEX* m)
{
  (void)m;
  return TX_SUCCESS;
}

/* NOLINTBEGIN(readability-non-const-parameter) -- ThreadX pins CHAR* name non-const. */
/**
 * @brief Host stub for tx_thread_create.
 *
 * @details See implementation.
 * @param[in] t See implementation.
 * @param[in] name See implementation.
 * @param[in] entry See implementation.
 * @param[in] arg See implementation.
 * @param[in] stack See implementation.
 * @param[in] stack_size See implementation.
 * @param[in] prio See implementation.
 * @param[in] preempt_thresh See implementation.
 * @param[in] slice See implementation.
 * @param[in] autostart See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline UINT tx_thread_create(TX_THREAD* t,
                                    CHAR*      name,
                                    void (*entry)(ULONG),
                                    ULONG arg,
                                    void* stack,
                                    ULONG stack_size,
                                    UINT  prio,
                                    UINT  preempt_thresh,
                                    UINT  slice,
                                    UINT  autostart)
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
    t->magic = (uint32_t)k_ra8_wdt_sup_tx_shim_thread_canary;
  }
  return TX_SUCCESS;
}
/* NOLINTEND(readability-non-const-parameter) */

/**
 * @brief Host stub for tx_thread_terminate.
 *
 * @details See implementation.
 * @param[in] t See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline UINT tx_thread_terminate(TX_THREAD* t)
{
  (void)t;
  return TX_SUCCESS;
}

/**
 * @brief Host stub for tx_thread_delete.
 *
 * @details See implementation.
 * @param[in] t See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline UINT tx_thread_delete(TX_THREAD* t)
{
  (void)t;
  return TX_SUCCESS;
}

/**
 * @brief Host stub for tx_thread_sleep.
 *
 * @details See implementation.
 * @param[in] ticks See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline UINT tx_thread_sleep(ULONG ticks)
{
  (void)ticks;
  return TX_SUCCESS;
}

/**
 * @brief Host stub for tx_time_get.
 *
 * @details See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline ULONG tx_time_get(void)
{
  return 0UL;
}

#ifdef __cplusplus
}
#endif
