/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_error_interface.h
 * @brief Abstract error-reporter interface for DI
 * @ingroup grp_core
 *
 * @details
 * Drivers that want to report non-fatal errors (retry loops,
 * degraded sensors, CRC mismatches) should push them into an
 * injectable sink rather than hard-coding
 * `internal_ra8_fatal_error()`. Production code points the sink at
 * the log backend; tests point it at a ring buffer they can assert
 * over.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_err.h"

/**
 * @struct ra8_error_interface_t
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
  void (*report)(void* ctx, const char* tag, const char* msg, ra8_err_t err);
  /**
   * @brief Opaque context.
   */
  void* ctx;
} ra8_error_interface_t;

/**
 * @brief Production `ra8_error_interface_t` that pushes reports into
 *        the standard `ra8_log_error_val` backend.
 */
extern const ra8_error_interface_t g_ra8_error_sink_log;

#ifdef __cplusplus
}
#endif
