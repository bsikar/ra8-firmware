/**
 * @file tests/zig_abi_pairs/ra8_box_pair/src/host_log_stub.c
 * @brief Minimal host stub for `ra8_log_emit_error` when linking `libra8_box.a`.
 *
 * @details
 * `libs/ra8_box` emits validation errors through `ra8_log_emit_error`. Host
 * standalone verification executables link this no-op C implementation instead
 * of adding `export fn` stubs to Zig test modules.
 *
 * @author Brighton Sikarskie
 * @date 2026-11-24
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_log.h"

void ra8_log_emit_error(const char* tag, const char* message)
{
  (void)tag;
  (void)message;
}
