/**
 * @file ra8_epub_miniz_alloc.c
 * @brief First-fit allocator over one caller-owned miniz workspace.
 *
 * @details Each EPUB or CBZ object embeds its own aligned workspace and arena
 * descriptor. Miniz receives that descriptor through ``m_pAlloc_opaque``.
 * Blocks form an implicit list with in-band headers; allocation is first-fit,
 * free coalesces adjacent blocks, and realloc preserves the original block when
 * a grow cannot be satisfied. No global arena and no host heap fallback exist.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_epub_miniz_alloc.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"

/** @brief Header prefixing every arena block. */
typedef struct {
  size_t size;    /**< Aligned payload bytes after this header. */
  size_t is_free; /**< One for free, zero for live.             */
} priv_blk_t;

/** @brief Aligned cell used to derive the allocator geometry. */
typedef union {
  max_align_t align; /**< Establishes maximum fundamental alignment. */
  uint8_t     byte;  /**< Byte-addressable view.                     */
} priv_pool_cell_t;

/** @brief Private allocator geometry and sentinel values. */
typedef enum : size_t {
  k_priv_hdr_bytes =
    ((sizeof(priv_blk_t) + sizeof(priv_pool_cell_t) - 1U) / sizeof(priv_pool_cell_t)) *
    sizeof(priv_pool_cell_t),
  k_priv_align    = sizeof(priv_pool_cell_t),
  k_priv_walk_max = k_ra8_epub_miniz_pool_bytes / (k_priv_hdr_bytes + sizeof(priv_pool_cell_t)),
  k_priv_blk_free = 1U,
  k_priv_blk_used = 0U,
} priv_alloc_const_t;

static_assert((k_ra8_epub_miniz_pool_bytes % sizeof(priv_pool_cell_t)) == 0U,
              "miniz workspace size must be a whole number of aligned cells");
static_assert(sizeof(ra8_epub_miniz_workspace_t) == k_ra8_epub_miniz_pool_bytes,
              "public miniz workspace must have exact advertised capacity");

/**
 * @brief Validate an arena descriptor before using caller-owned storage.
 * @param[in] arena Descriptor to inspect.
 * @return Whether the descriptor is live and geometrically valid.
 * @post No state is modified.
 */
RA8_INTERNAL
static bool internal_ready(const ra8_epub_miniz_arena_t* arena)
{
  if (arena == nullptr) {
    return false;
  }
  if (arena->initialized != 1U) {
    return false;
  }
  if (arena->base == nullptr) {
    return false;
  }
  if (arena->capacity != (size_t)k_ra8_epub_miniz_pool_bytes) {
    return false;
  }
  return ((uintptr_t)arena->base % alignof(max_align_t)) == 0U;
}

/**
 * @brief Cast byte offset @p off to an aligned block header.
 * @param[in] arena Live arena.
 * @param[in] off Aligned byte offset, at most the arena capacity.
 * @return Header pointer or the one-past sentinel.
 */
RA8_INTERNAL
static priv_blk_t* internal_cell_at(const ra8_epub_miniz_arena_t* arena, size_t off)
{
  void* const cell = &arena->base[off];
  return (priv_blk_t*)cell;
}

/** @brief Return the arena's one-past byte sentinel. */
RA8_INTERNAL
static uint8_t* internal_end(const ra8_epub_miniz_arena_t* arena)
{
  return &arena->base[arena->capacity];
}

/** @brief Return the payload immediately following @p block. */
RA8_INTERNAL
static uint8_t* internal_payload(priv_blk_t* block)
{
  return (uint8_t*)block + (size_t)k_priv_hdr_bytes;
}

/** @brief Return the header for an allocation payload. */
RA8_INTERNAL
static priv_blk_t* internal_header(const ra8_epub_miniz_arena_t* arena, void* address)
{
  const uintptr_t payload = (uintptr_t)address;
  const uintptr_t base    = (uintptr_t)arena->base;
  return internal_cell_at(arena, (size_t)(payload - base) - (size_t)k_priv_hdr_bytes);
}

/** @brief Return the block immediately following @p block. */
RA8_INTERNAL
static priv_blk_t* internal_next(const ra8_epub_miniz_arena_t* arena, priv_blk_t* block)
{
  const uintptr_t end  = (uintptr_t)(internal_payload(block) + block->size);
  const uintptr_t base = (uintptr_t)arena->base;
  return internal_cell_at(arena, (size_t)(end - base));
}

/** @brief Test whether @p address lies in the payload-bearing arena range. */
RA8_INTERNAL
static bool internal_in_pool(const ra8_epub_miniz_arena_t* arena, const void* address)
{
  if (!internal_ready(arena)) {
    return false;
  }
  const uintptr_t value = (uintptr_t)address;
  const uintptr_t first = (uintptr_t)arena->base + (uintptr_t)k_priv_hdr_bytes;
  const uintptr_t end   = (uintptr_t)arena->base + (uintptr_t)arena->capacity;
  return (value >= first) && (value < end);
}

/** @brief Align a pool-bounded request upward to a complete arena cell. */
RA8_INTERNAL
static size_t internal_align_up(size_t bytes)
{
  const size_t mask = (size_t)k_priv_align - 1U;
  return (bytes + mask) & ~mask;
}

/**
 * @brief Validate and multiply a miniz count/size request exactly once.
 * @param[in] items Element count.
 * @param[in] size Element size.
 * @param[out] out_bytes Product on success; unchanged on failure.
 * @return Whether the product is representable and arena-bounded.
 */
RA8_INTERNAL
static bool internal_request_bytes(size_t items, size_t size, size_t* out_bytes)
{
  if ((size != 0U) && (items > (SIZE_MAX / size))) {
    return false;
  }
  const size_t bytes = items * size;
  if (bytes > (size_t)k_ra8_epub_miniz_pool_bytes) {
    return false;
  }
  *out_bytes = bytes;
  return true;
}

/** @brief Split @p block if its remainder can hold another allocation. */
RA8_INTERNAL
static void internal_split(const ra8_epub_miniz_arena_t* arena, priv_blk_t* block, size_t need)
{
  const size_t remainder_min = (size_t)k_priv_hdr_bytes + (size_t)k_priv_align;
  if ((block->size - need) < remainder_min) {
    return;
  }
  const uint8_t* const payload   = internal_payload(block);
  const uintptr_t      rem_addr  = (uintptr_t)payload + need;
  const uintptr_t      base      = (uintptr_t)arena->base;
  priv_blk_t* const    remainder = internal_cell_at(arena, (size_t)(rem_addr - base));
  remainder->size                = block->size - need - (size_t)k_priv_hdr_bytes;
  remainder->is_free             = (size_t)k_priv_blk_free;
  block->size                    = need;
}

/** @brief Merge all adjacent free blocks in @p arena. */
RA8_INTERNAL
static void internal_coalesce(const ra8_epub_miniz_arena_t* arena)
{
  priv_blk_t* block = internal_cell_at(arena, 0U);
  for (size_t guard = 0U; guard < (size_t)k_priv_walk_max; ++guard) {
    if ((uint8_t*)block >= internal_end(arena)) {
      break;
    }
    if (block->is_free == (size_t)k_priv_blk_free) {
      priv_blk_t* next = internal_next(arena, block);
      while (((uint8_t*)next < internal_end(arena)) && (next->is_free == (size_t)k_priv_blk_free)) {
        block->size += (size_t)k_priv_hdr_bytes + next->size;
        next = internal_next(arena, block);
      }
    }
    block = internal_next(arena, block);
  }
}

ra8_err_t
ra8_epub_miniz_arena_init(ra8_epub_miniz_arena_t* arena, void* workspace, size_t workspace_bytes)
{
  if ((arena == nullptr) || (workspace == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (workspace_bytes < (size_t)k_ra8_epub_miniz_pool_bytes) {
    return k_ra8_err_invalid_size;
  }
  if (((uintptr_t)workspace % alignof(max_align_t)) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_epub_miniz_arena_t next = {
    .base        = (uint8_t*)workspace,
    .capacity    = (size_t)k_ra8_epub_miniz_pool_bytes,
    .initialized = 1U,
  };
  priv_blk_t* const head = internal_cell_at(&next, 0U);
  head->size             = next.capacity - (size_t)k_priv_hdr_bytes;
  head->is_free          = (size_t)k_priv_blk_free;
  *arena                 = next;
  return k_ra8_ok;
}

void ra8_epub_miniz_arena_deinit(ra8_epub_miniz_arena_t* arena)
{
  if (arena != nullptr) {
    *arena = (ra8_epub_miniz_arena_t){};
  }
}

void* ra8_epub_miniz_alloc(void* opaque, size_t items, size_t size)
{
  ra8_epub_miniz_arena_t* const arena = (ra8_epub_miniz_arena_t*)opaque;
  if (!internal_ready(arena)) {
    return nullptr;
  }
  size_t bytes = 0U;
  if (!internal_request_bytes(items, size, &bytes)) {
    return nullptr;
  }
  size_t need = internal_align_up(bytes);
  if (need == 0U) {
    need = (size_t)k_priv_align;
  }
  priv_blk_t* block = internal_cell_at(arena, 0U);
  for (size_t guard = 0U; guard < (size_t)k_priv_walk_max; ++guard) {
    if ((uint8_t*)block >= internal_end(arena)) {
      break;
    }
    if ((block->is_free == (size_t)k_priv_blk_free) && (block->size >= need)) {
      internal_split(arena, block, need);
      block->is_free = (size_t)k_priv_blk_used;
      return internal_payload(block);
    }
    block = internal_next(arena, block);
  }
  return nullptr;
}

void ra8_epub_miniz_free(void* opaque, void* address)
{
  ra8_epub_miniz_arena_t* const arena = (ra8_epub_miniz_arena_t*)opaque;
  if ((address == nullptr) || !internal_in_pool(arena, address)) {
    return;
  }
  priv_blk_t* const block = internal_header(arena, address);
  block->is_free          = (size_t)k_priv_blk_free;
  internal_coalesce(arena);
}

void* ra8_epub_miniz_realloc(void* opaque, void* address, size_t items, size_t size)
{
  if (address == nullptr) {
    return ra8_epub_miniz_alloc(opaque, items, size);
  }
  ra8_epub_miniz_arena_t* const arena = (ra8_epub_miniz_arena_t*)opaque;
  if (!internal_in_pool(arena, address)) {
    return nullptr;
  }
  size_t bytes = 0U;
  if (!internal_request_bytes(items, size, &bytes)) {
    return nullptr;
  }
  const size_t need = internal_align_up(bytes);
  if (need == 0U) {
    ra8_epub_miniz_free(arena, address);
    return nullptr;
  }
  priv_blk_t* const block = internal_header(arena, address);
  if (block->size >= need) {
    return address;
  }
  void* const moved = ra8_epub_miniz_alloc(arena, items, size);
  if (moved == nullptr) {
    return nullptr;
  }
  (void)memcpy(moved, address, block->size);
  ra8_epub_miniz_free(arena, address);
  return moved;
}
