/**
 * @file port/nimble/inc/nimble/nimble_npl_os_log.h
 * @brief NPL logging hook for the ThreadX port (compiled out)
 *
 * @par Tag
 * [Ring 4 / PORT] {World: S}
 *
 * @details
 * Upstream ``nimble/nimble_npl_log.h`` chains to this per-port header to
 * pick up ``BLE_NPL_LOG_IMPL``, which defines the ``BLE_NPL_LOG_DEBUG``
 * and friends for one log module. This port has no NimBLE log sink -- the
 * project logs through ``ra8_log`` -- so the macro emits empty static
 * inline functions and every NPL log call compiles away.
 *
 * It exists so the port supplies the complete pair of headers upstream
 * expects. The build previously borrowed the upstream "dummy" port for
 * this file, and that dropped the dummy's ``nimble_npl_os.h`` into the
 * same include path, giving translation units a ``struct ble_npl_event``
 * of ``{ int dummy; }`` while the port's own TUs saw the real
 * ThreadX-backed layout.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

/**
 * @def BLE_NPL_LOG_IMPL
 * @brief Define one no-op NPL log entry point for the given level.
 *
 * @details Expands to an empty ``static inline`` function named after the
 * enclosing ``BLE_NPL_LOG_MODULE``, matching the signature upstream call
 * sites use so the calls type-check and then optimise away.
 *
 * @param lvl Log level token (``DEBUG``, ``INFO``, ``WARN``, ``ERROR``,
 *            ``CRITICAL``) as passed by ``nimble_npl_log.h``.
 *
 * @note Vendored NimBLE headers use this; first-party code logs through
 *       ``ra8_log`` instead.
 *
 * @par Example:
 * @code
 * BLE_NPL_LOG_IMPL(DEBUG);
 * @endcode
 *
 * @since 0.1.0
 */
#define BLE_NPL_LOG_IMPL(lvl)                                                                      \
  static inline void _BLE_NPL_LOG_CAT(BLE_NPL_LOG_MODULE,                                          \
                                      _BLE_NPL_LOG_CAT(_, lvl))(const char* fmt, ...)              \
  {                                                                                                \
    (void)fmt;                                                                                     \
  }
