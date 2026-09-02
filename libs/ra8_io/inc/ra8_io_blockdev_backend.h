/**
 * @file ra8_io_blockdev_backend.h
 * @brief Backend implementation contract for the ra8_io block-device fabric.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * This implementer-facing header is the Dependency Inversion seam for storage
 * backends. A library that presents an ::ra8_io_blockdev_t includes this header,
 * defines one const ::ra8_io_blockdev_iface_t, and binds it into the public
 * handle. Application consumers use only `ra8_io_blockdev.h` and never need the
 * concrete callback table.
 *
 * Keeping this contract under `inc/` is intentional: adapters such as `ra8_ftl`
 * are separate libraries and must be able to implement the abstraction without
 * reaching into `libs/ra8_io/src/`. The callback contract remains free of
 * platform and device types, so host, RAM, flash, SD, and future architectures
 * substitute behind the same handle.
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

#ifdef __cplusplus
}
#endif
