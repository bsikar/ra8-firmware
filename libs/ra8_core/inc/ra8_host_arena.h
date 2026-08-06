/**
 * @file ra8_host_arena.h
 * @brief A generic bump/arena allocator for device-portable tool code.
 * @ingroup grp_core
 *
 * @par Tag
 * [Ring 1 / Core]
 *
 * @details
 * Provides a simple bump allocator that takes a caller-supplied buffer and
 * hands out aligned blocks from it. The allocator performs no dynamic
 * allocation: the only storage decision is the caller's initial buffer,
 * making it compatible with NASA Power-of-10 Rule 3.
 *
 * @par Usage
 * @code
 * // Host side: arena backed by a heap buffer
 * uint8_t* buf = (uint8_t*)malloc(64U * 1024U);
 * ra8_arena_t arena;
 * ra8_arena_init(&arena, buf, 64U * 1024U);
 *
 * // Device side: arena backed by a static array
 * static uint8_t s_buf[64U * 1024U];
 * ra8_arena_t arena;
 * ra8_arena_init(&arena, s_buf, (uint32_t)sizeof s_buf);
 *
 * // Both paths use the same allocation API:
 * void* p = ra8_arena_alloc(&arena, 256U, k_ra8_arena_align);
 * @endcode
 *
 * Follows the same design as the `ra8_c6link` decode arena but is
 * decoupled from protobuf-c and usable by any library.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 *
 *

 */

#ifndef RA8_HOST_ARENA_H
#define RA8_HOST_ARENA_H

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @brief Alignment the arena hands out blocks at (8-byte for uint64_t safety).
 * @since 0.1.0
 */
#define k_ra8_arena_align ((uint32_t)8U)
#define k_ra8_arena_mask  ((uint32_t)7U)

/**
 * @brief A bump allocator over a caller-supplied buffer.
 *
 * @details
 * The arena does not own its buffer. The caller is responsible for the
 * lifetime of the backing storage. Call ::ra8_arena_reset to reclaim all
 * allocations without releasing the underlying buffer.
 * @since 0.1.0
 */
typedef struct {
  uint8_t* buf;  /**< Backing buffer (caller-owned). */
  uint32_t cap;  /**< Total capacity in bytes.       */
  uint32_t used; /**< Current bump offset.           */
} ra8_arena_t;

/**
 * @brief Initialise an arena over a caller-supplied buffer.
 *
 * @param[out] a     Arena to initialise.
 * @param[in]  buf   Backing buffer (must outlive the arena).
 * @param[in]  bytes Size of @p buf in bytes.
 *
 * @retval k_ra8_ok       Success.
 * @retval k_ra8_err_null_ptr @p a or @p buf is null.
 * @since 0.1.0
 */
static inline ra8_err_t ra8_arena_init(ra8_arena_t* a, uint8_t* buf, uint32_t bytes)
{
  if ((a == nullptr) || (buf == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  a->buf  = buf;
  a->cap  = bytes;
  a->used = 0U;
  return k_ra8_ok;
}

/**
 * @brief Allocate @p bytes from the arena, returning an aligned pointer.
 *
 * @param[in,out] a     Arena to allocate from.
 * @param[in]     bytes Number of bytes requested.
 *
 * @return Aligned pointer to the allocated block, or @c nullptr if the
 *         arena has insufficient space.
 * @since 0.1.0
 */
static inline void* ra8_arena_alloc(ra8_arena_t* a, uint32_t bytes, uint32_t align)
{
  if ((a == nullptr) || (a->buf == nullptr)) {
    return nullptr;
  }

  const uint32_t mask  = (align > 0U) ? (align - 1U) : k_ra8_arena_mask;
  const uint32_t avail = a->cap - a->used;
  if (bytes > avail) {
    return nullptr;
  }

  const uint32_t pad = (0U - bytes) & mask;
  if (pad > (avail - bytes)) {
    return nullptr;
  }

  const uint32_t at = a->used;
  a->used           = at + bytes + pad;
  return &a->buf[at];
}

/**
 * @brief Allocate @p count * @p size bytes, zero-initialised.
 *
 * @param[in,out] a     Arena to allocate from.
 * @param[in]     count Number of elements.
 * @param[in]     size  Size of each element in bytes.
 *
 * @return Aligned, zeroed pointer, or @c nullptr on OOM.
 * @since 0.1.0
 */
static inline void* ra8_arena_calloc(ra8_arena_t* a, uint32_t count, uint32_t size)
{
  const uint32_t total = count * size;
  void*          p     = ra8_arena_alloc(a, total, k_ra8_arena_align);
  if (p != nullptr) {
    uint8_t* dst = (uint8_t*)p;
    for (uint32_t i = 0U; i < total; ++i) {
      dst[i] = 0U;
    }
  }
  return p;
}

/**
 * @brief Reset the arena, reclaiming all allocations.
 *
 * @details
 * Does not release the backing buffer. All previously returned pointers
 * become invalid.
 *
 * @param[in,out] a Arena to reset.
 * @since 0.1.0
 */
static inline void ra8_arena_reset(ra8_arena_t* a)
{
  if (a != nullptr) {
    a->used = 0U;
  }
}

/**
 * @brief Query remaining capacity.
 *
 * @param[in] a Arena to query.
 * @return Bytes remaining, or 0 if @p a is null.
 * @since 0.1.0
 */
static inline uint32_t ra8_arena_remaining(const ra8_arena_t* a)
{
  if (a == nullptr) {
    return 0U;
  }
  return a->cap - a->used;
}

/**
 * @brief Save the current bump offset for later restore.
 *
 * @param[in] a Arena to query.
 * @return Current offset, or 0 if @p a is null.
 * @since 0.1.0
 */
static inline uint32_t ra8_arena_save(ra8_arena_t* a)
{
  uint32_t mark = 0U;
  if (a != nullptr) {
    mark = a->used;
  }
  return mark;
}

/**
 * @brief Restore the bump offset to a previously saved mark.
 *
 * @param[in,out] a    Arena to restore.
 * @param[in]     mark Saved offset from ::ra8_arena_save.
 * @since 0.1.0
 */
static inline void ra8_arena_restore(ra8_arena_t* a, uint32_t mark)
{
  if (a != nullptr) {
    a->used = mark;
  }
}

#endif /* RA8_HOST_ARENA_H */
