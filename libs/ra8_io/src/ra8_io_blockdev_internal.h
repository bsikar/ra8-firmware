/**
 * @file ra8_io_blockdev_internal.h
 * @brief Internal vtable type for the ra8_io block-device fabric.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Not part of the public API. Only the block-device dispatcher and the backend
 * translation units include this file. Tests under `tests/` MAY include it to
 * drive MC/DC vectors against internal helpers; application code never should.
 *
 * Defines ::ra8_io_blockdev_iface -- the function-pointer table each backend
 * fills in and binds into a caller-owned ::ra8_io_blockdev_t. It is forward
 * declared opaque in `ra8_io_blockdev.h`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_io_blockdev.h"

/**
 * @struct ra8_io_blockdev_iface
 * @brief One vtable per storage backend. Each callback receives the backend's
 *        opaque `ctx` so backends keep their state private.
 *
 * @details
 * `read`, `write`, and `get_caps` are mandatory. `erase` and `sync` may be NULL
 * for media that have no erase concept or no write buffering; the dispatcher
 * maps a NULL `erase` to ::k_ra8_err_not_supported and a NULL `sync` to success.
 *
 * @invariant `read`, `write`, and `get_caps` are non-NULL.
 *
 * @since 0.1.0
 */
/* cppcheck-suppress-begin [unusedStructMember] */
struct ra8_io_blockdev_iface {
  /** @brief Read `count` blocks at `lba` into `buf`. */
  ra8_err_t (*read)(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf);

  /** @brief Write `count` blocks from `buf` at `lba`. */
  ra8_err_t (*write)(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf);

  /** @brief Erase `count` blocks at `lba`. May be NULL (no erase concept). */
  ra8_err_t (*erase)(void* ctx, uint32_t lba, uint32_t count);

  /** @brief Fill `*out` with the medium capabilities. */
  ra8_err_t (*get_caps)(const void* ctx, ra8_io_blockdev_caps_t* out);

  /** @brief Commit any write buffering. May be NULL (nothing buffered). */
  ra8_err_t (*sync)(void* ctx);
};
/* cppcheck-suppress-end [unusedStructMember] */

#ifdef __cplusplus
}
#endif
