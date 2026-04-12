/**
 * @file ra_error_interface.h
 * @brief Abstract error-reporter interface for DI
 *
 * @details
 * Drivers that want to report non-fatal errors (retry loops,
 * degraded sensors, CRC mismatches) should push them into an
 * injectable sink rather than hard-coding
 * `internal_ra_fatal_error()`. Production code points the sink at
 * the log backend; tests point it at a ring buffer they can assert
 * over.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra_err.h"

/**
 * @struct ra_error_interface_t
 * @brief Vtable for an error sink.
 */
typedef struct {
  /**
   * @brief Record a non-fatal error.
   * @param[in] ctx  Opaque context.
   * @param[in] tag  Source component tag.
   * @param[in] msg  Human-readable message.
   * @param[in] err  Error code.
   */
  void (*report)(void* ctx, const char* tag, const char* msg, ra_err_t err);
  /**
   * @brief Opaque context.
   */
  void* ctx;
} ra_error_interface_t;

#ifdef __cplusplus
}
#endif
