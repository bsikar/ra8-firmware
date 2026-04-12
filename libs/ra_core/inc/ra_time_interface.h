/**
 * @file ra_time_interface.h
 * @brief Abstract time-source interface for DI and unit testing
 *
 * @details
 * Modules that need wall-clock time (timeouts, rate limiters, delay
 * loops) should depend on an `ra_time_interface_t` rather than
 * calling `ra_time_ms()` directly. Unit tests can then inject a
 * mock that advances on command, without needing SysTick.
 *
 * Production code points the interface at
 * `g_ra_time_interface_systick` (wraps `ra_time_ms` + `ra_delay_ms`).
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
 * @struct ra_time_interface_t
 * @brief Vtable for a wall-clock time source.
 */
typedef struct {
  /**
   * @brief Return the current time in milliseconds.
   */
  uint32_t (*now_ms)(void* ctx);
  /**
   * @brief Busy-wait for `ms` milliseconds.
   */
  void (*delay_ms)(void* ctx, uint32_t ms);
  /**
   * @brief Opaque context handed to every call.
   */
  void* ctx;
} ra_time_interface_t;

#ifdef __cplusplus
}
#endif
