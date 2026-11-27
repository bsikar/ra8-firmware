/**
 * @file port/nimble/inc/nimble_port_alloc.h
 * @brief Fixed-SRAM static allocation redirect for freestanding NimBLE builds.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: S}
 *
 * @details
 * Upstream Apache NimBLE's ``ble_gatts.c`` and ``ble_att_svr.c`` call
 * ``malloc``, ``realloc``, and ``free`` during stack bring-up to size the
 * local GATT attribute and CCCD tables. Freestanding RA8 images intentionally
 * carry no libc heap (``_sbrk`` traps unconditionally). Compiling
 * ``nimble_objs`` with ``-include .../nimble_port_alloc.h`` on
 * ``Generic`` targets includes ``<stdlib.h>`` once cleanly first and then
 * redirects NimBLE's allocation calls onto the static SRAM pool in
 * ``port/nimble/src/nimble_port_alloc.c``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.2.0
 */

#pragma once

#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocate @p size bytes from the static NimBLE SRAM pool.
 * @param[in] size Requested byte count (0 allocates minimum aligned block).
 * @return 8-byte aligned pointer within static pool, or NULL when exhausted.
 * @pre Module static state is valid (self-initializes on first call).
 * @post Pool metadata invariants remain intact.
 * @since 0.2.0
 */
void* nimble_port_malloc(size_t size);

/**
 * @brief Resize an allocation from ::nimble_port_malloc.
 * @param[in] ptr  Existing block pointer or NULL.
 * @param[in] size Requested new byte count (0 frees @p ptr).
 * @return Resized 8-byte aligned block pointer, or NULL on exhaustion/free.
 * @pre @p ptr is NULL or was returned by ::nimble_port_malloc / ::nimble_port_realloc.
 * @post Existing prefix bytes up to min(old, new) are preserved when non-NULL.
 * @since 0.2.0
 */
void* nimble_port_realloc(void* ptr, size_t size);

/**
 * @brief Return a block to the static NimBLE SRAM pool and coalesce neighbors.
 * @param[in] ptr Block pointer returned by ::nimble_port_malloc, or NULL.
 * @pre @p ptr is NULL or belongs to the static NimBLE SRAM pool.
 * @post Adjacent free regions are coalesced to avoid fragmentation.
 * @since 0.2.0
 */
void nimble_port_free(void* ptr);

#define malloc  nimble_port_malloc
#define realloc nimble_port_realloc
#define free    nimble_port_free

#ifdef __cplusplus
}
#endif
