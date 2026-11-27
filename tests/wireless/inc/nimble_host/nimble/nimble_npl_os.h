/**
 * @file tests/wireless/inc/nimble_host/nimble/nimble_npl_os.h
 * @brief Deterministic single-thread host OS types for NimBLE CTests.
 *
 * @details
 * Shadows ``port/nimble/inc/nimble/nimble_npl_os.h`` ONLY inside host
 * integration test executables so upstream NimBLE compiles cleanly without
 * ThreadX while sharing the exact ``syscfg/syscfg.h`` and
 * ``nimble/nimble_npl_os_log.h`` headers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.2.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define BLE_NPL_OS_ALIGNMENT (4)
#define BLE_NPL_TIME_FOREVER UINT32_MAX

typedef uint32_t ble_npl_time_t;
typedef int32_t  ble_npl_stime_t;

typedef enum : uint16_t {
  k_nimble_host_evq_cap = 64U,
} nimble_host_limits_t;

struct ble_npl_event {
  uint8_t           queued;
  ble_npl_event_fn* fn;
  void*             arg;
};

struct ble_npl_eventq {
  struct ble_npl_event* ring[k_nimble_host_evq_cap];
  uint16_t              head;
  uint16_t              tail;
  uint16_t              count;
};

struct ble_npl_callout {
  struct ble_npl_eventq* evq;
  struct ble_npl_event   ev;
  ble_npl_time_t         ticks;
  bool                   active;
};

struct ble_npl_mutex {
  uint32_t depth;
};

struct ble_npl_sem {
  uint16_t count;
};

/** @brief Optional deterministic pump invoked when `ble_npl_sem_pend` waits on zero. */
typedef void (*nimble_npl_host_pump_fn_t)(void* ctx);

/** @brief Register a pump callback invoked during blocking host semaphore waits. */
void nimble_npl_host_set_pump(nimble_npl_host_pump_fn_t pump_cb, void* ctx);

/** @brief Advance deterministic host time (ms) and fire expired active callouts. */
void nimble_npl_host_tick(uint32_t delta_ms);

/** @brief Reset synthetic host clock and critical counter between test cases. */
void nimble_npl_host_reset(void);

#ifdef __cplusplus
}
#endif
