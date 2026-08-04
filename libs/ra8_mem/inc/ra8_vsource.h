/**
 * @file ra8_vsource.h
 * @brief Virtual-memory object sources -- the page-cache storage seam (Layer 1, #147).
 * @ingroup grp_ereader
 *
 * @details
 * Layer 1 of the #147 memory hierarchy. ::ra8_vmem (Layer 2) pages objects in
 * through a single loader callback keyed by an `object_id`; a source registry
 * maps each `object_id` to its backing so one cache can serve many objects of
 * two kinds:
 *
 *  - **Paged** -- backed by slow storage (SD/exFAT file, OSPI as data, MRAM,
 *    ...). The registry holds a generic read callback (`offset -> bytes`) so this
 *    layer stays free of any storage dependency; the app wires the callback to an
 *    `ra8_io` block device or `ra8_fs` file. Paged objects are demand-paged through
 *    the Layer-2 cache.
 *  - **XIP** -- backed by an already memory-mapped, execute-in-place region
 *    (OSPI flash). Resident assets (fonts!) cost zero SRAM: ::ra8_vsource_xip_ptr
 *    hands back a real pointer with no copy and no cache frame.
 *
 * ::ra8_vsource_loader is the ::ra8_vmem_loader_fn the cache calls on a miss: it
 * looks up the object, bounds-checks the request, and fills the frame from the
 * object's backing (read callback, or memcpy for an XIP object), zero-padding any
 * tail beyond the object's end.
 *
 * ## Boundary with ra8_io_blockdev
 *
 * This source is read-only and byte-addressed, and lives at [Ring 2 / Core].
 * The `ra8_io` block-device fabric (`ra8_io_blockdev.h`, [Ring 4 / PAL]) is a
 * separate storage seam: read/write/erase over 512-byte LBAs. The two are kept
 * distinct on purpose -- a read-only byte view for the page cache versus a
 * read/write block fabric for filesystems -- and the split is intentional, not
 * drift. ::ra8_vsource must never depend on `ra8_io_blockdev`: that would make
 * Ring 2 depend on Ring 4 and invert ring ordering (see
 * `docs/RING_AND_WORLD.md`). The sanctioned bridge runs the other (allowed) way:
 * the Ring-4 adapter `ra8_io_blockdev_vsource.h` exposes a block device as a
 * ::ra8_vsource_read_fn, doing the LBA<->byte translation so apps can wire a
 * block device straight into ::ra8_vsource_add_paged.
 *
 * Zero allocation (NASA P10 Rule 3): the caller supplies the object array.
 *
 * @note Not thread-safe; the reader serialises access.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @par Tag
 * [Ring 2 / Core] {World: NS}
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @typedef ra8_vsource_read_fn
 * @brief Read `len` bytes at `offset` from a paged object's backing.
 *
 * @details The storage seam: the app binds this to an `ra8_io` block device or an
 *          `ra8_fs` file so ::ra8_vsource stays storage-agnostic.
 *
 * @param[in]  ctx    Opaque backing context registered with the object.
 * @param[in]  offset Absolute byte offset within the backing.
 * @param[out] buf    Destination buffer (`len` writable bytes).
 * @param[in]  len    Number of bytes to read.
 *
 * @return ra8_err_t k_ra8_ok on success; any error aborts the page-in.
 *
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_vsource_read_fn)(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len);

/**
 * @struct ra8_vsource_obj_t
 * @brief One registered object's backing (paged or XIP).
 *
 * @details Treat as private; populated by ::ra8_vsource_add_paged /
 *          ::ra8_vsource_add_xip. `xip != NULL` marks an XIP object.
 *
 * @since 0.1.0
 */
typedef struct {
  ra8_vsource_read_fn read; /**< Paged read callback (NULL for an XIP object). */
  void*               ctx;  /**< Context passed to @c read.                    */
  const uint8_t*      xip;  /**< XIP base pointer (NULL for a paged object).   */
  uint64_t            base; /**< Paged: absolute base offset of the object.    */
  uint64_t            size; /**< Object length in bytes.                       */
} ra8_vsource_obj_t;

/**
 * @struct ra8_vsource_t
 * @brief Object-source registry (caller-owned; treat as private).
 *
 * @invariant `count <= cap`.
 *
 * @since 0.1.0
 */
typedef struct {
  ra8_vsource_obj_t* objs;  /**< `cap` object slots (caller-owned).     */
  uint32_t           cap;   /**< Slot capacity.                         */
  uint32_t           count; /**< Registered objects (next id == count). */
} ra8_vsource_t;

/**
 * @brief Initialise an empty source registry over a caller-owned object array.
 *
 * @param[out] vs   Registry to populate (zero-initialised by the caller).
 * @param[in]  objs Object slot array (out-lives the registry).
 * @param[in]  cap  Number of slots in @p objs (>= 1).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Registry ready (empty).
 * @retval k_ra8_err_null_ptr     `vs` or `objs` was NULL.
 * @retval k_ra8_err_invalid_size `cap` was zero.
 *
 * @pre `objs` covers `cap` entries and out-lives the registry.
 * @pre `vs` does not alias `objs`.
 * @post On success `vs->count == 0`.
 * @post On any non-ok return `vs` is left unbound.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vsource_init(ra8_vsource_t* vs, ra8_vsource_obj_t* objs, uint32_t cap);

/**
 * @brief Register a storage-paged object; returns its `object_id`.
 *
 * @param[in]  vs     Initialised registry.
 * @param[in]  read   Backing read callback.
 * @param[in]  ctx    Context for @p read.
 * @param[in]  base   Absolute base offset of the object within the backing.
 * @param[in]  size   Object length in bytes (> 0).
 * @param[out] out_id Receives the assigned `object_id`.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Registered; `*out_id` set.
 * @retval k_ra8_err_null_ptr     `vs`, `read`, or `out_id` was NULL.
 * @retval k_ra8_err_invalid_size `size` was zero.
 * @retval k_ra8_err_no_mem       The registry is full.
 *
 * @pre `read` reads the object's bytes for offsets in `[base, base + size)`.
 * @pre `vs` was populated by ::ra8_vsource_init.
 * @post On success the object is addressable via ::ra8_vsource_loader.
 * @post On any non-ok return the registry is unchanged.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vsource_add_paged(ra8_vsource_t*      vs,
                                              ra8_vsource_read_fn read,
                                              void*               ctx,
                                              uint64_t            base,
                                              uint64_t            size,
                                              uint32_t*           out_id);

/**
 * @brief Register an execute-in-place (memory-mapped) object; returns its id.
 *
 * @param[in]  vs       Initialised registry.
 * @param[in]  xip_base Base pointer of the memory-mapped object.
 * @param[in]  size     Object length in bytes (> 0).
 * @param[out] out_id   Receives the assigned `object_id`.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Registered; `*out_id` set.
 * @retval k_ra8_err_null_ptr     `vs`, `xip_base`, or `out_id` was NULL.
 * @retval k_ra8_err_invalid_size `size` was zero.
 * @retval k_ra8_err_no_mem       The registry is full.
 *
 * @pre `xip_base` is a readable region of at least `size` bytes.
 * @pre `vs` was populated by ::ra8_vsource_init.
 * @post On success ::ra8_vsource_xip_ptr returns pointers into the region.
 * @post On any non-ok return the registry is unchanged.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_vsource_add_xip(ra8_vsource_t* vs, const uint8_t* xip_base, uint64_t size, uint32_t* out_id);

/**
 * @brief Fill a page frame from an object -- the ::ra8_vmem_loader_fn adapter.
 *
 * @details Looks up @p object_id, bounds-checks against the object size, and
 *          fills @p frame from the object's backing (paged read callback, or
 *          memcpy for an XIP object). Any tail beyond the object end is zeroed.
 *          The signature matches `ra8_vmem_loader_fn`, so pass `vs` as the cache's
 *          `loader_ctx` and this function as its `loader`.
 *
 * @param[in]  ctx         The ::ra8_vsource_t registry (as a void cookie).
 * @param[in]  object_id   Object to page in.
 * @param[in]  offset      Frame-aligned byte offset within the object.
 * @param[out] frame       Destination frame (`frame_bytes` writable).
 * @param[in]  frame_bytes Frame size in bytes.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Frame filled (zero-padded past the object end).
 * @retval k_ra8_err_null_ptr     `ctx` or `frame` was NULL.
 * @retval k_ra8_err_out_of_range `object_id` is not registered, or `offset` is at
 *                               or beyond the object size.
 * @retval k_ra8_err_*            The read callback's error (paged objects).
 *
 * @pre `ctx` is a populated ::ra8_vsource_t.
 * @pre `frame` is writable for `frame_bytes`.
 * @post On success `frame` holds the object's bytes at `offset` (tail zeroed).
 * @post On any non-ok return `frame` content is unspecified.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vsource_loader(void*    ctx,
                                           uint32_t object_id,
                                           uint64_t offset,
                                           uint8_t* frame,
                                           uint32_t frame_bytes);

/**
 * @brief Get a direct pointer into an XIP object (no copy, no cache).
 *
 * @param[in]  vs        Initialised registry.
 * @param[in]  object_id An XIP object id.
 * @param[in]  offset    Byte offset within the object.
 * @param[in]  len       Number of bytes the caller will read.
 * @param[out] out_ptr   Receives the pointer to `object[offset]`.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Pointer returned.
 * @retval k_ra8_err_null_ptr      `vs` or `out_ptr` was NULL.
 * @retval k_ra8_err_out_of_range  Unknown id, or `offset + len` exceeds the size.
 * @retval k_ra8_err_not_supported The object is paged, not XIP.
 *
 * @pre `vs` was populated by ::ra8_vsource_init.
 * @pre `object_id` was registered via ::ra8_vsource_add_xip.
 * @post On success `*out_ptr` is valid for `len` bytes.
 * @post No registry state is mutated.
 *
 * @note Thread-safe with respect to a quiescent registry (pure read).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vsource_xip_ptr(const ra8_vsource_t* vs,
                                            uint32_t             object_id,
                                            uint64_t             offset,
                                            uint32_t             len,
                                            const uint8_t**      out_ptr);

#ifdef __cplusplus
}
#endif
