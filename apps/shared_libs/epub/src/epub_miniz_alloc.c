/**
 * @file epub_miniz_alloc.c
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

#include "epub_miniz_alloc.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"

/** @brief Header prefixing every arena block. */
typedef struct {
  size_t size;    /**< Aligned payload bytes after this header. */
  size_t is_free; /**< One for free, zero for live.             */
} priv_blk_t;

/** @brief Private allocator geometry and sentinel values. */
typedef enum : size_t {
  k_priv_hdr_bytes = ((sizeof(priv_blk_t) + alignof(max_align_t) - 1U) / alignof(max_align_t)) *
                     alignof(max_align_t), /**< Aligned in-band header bytes. */
  k_priv_align     = alignof(max_align_t), /**< Maximum-alignment cell size.  */
  k_priv_walk_max =
    k_epub_miniz_pool_bytes / (k_priv_hdr_bytes + alignof(max_align_t)), /**< Block-walk guard.  */
  k_priv_blk_free = 1U,                                                  /**< Free-block marker. */
  k_priv_blk_used = 0U,                                                  /**< Live-block marker. */
} priv_alloc_const_t;

static_assert((k_epub_miniz_pool_bytes % alignof(max_align_t)) == 0U,
              "miniz workspace size must be a whole number of aligned cells");
static_assert(sizeof(epub_miniz_workspace_t) == k_epub_miniz_pool_bytes,
              "public miniz workspace must have exact advertised capacity");

/**
 * @brief Copy a pointer representation into an integer address.
 * @details Uses a bytewise copy so the conversion does not violate pointer
 *          aliasing rules checked by the static analyzer.
 * @param[in] pointer Object pointer whose representation is copied.
 * @return Integer address carrying the same representation.
 * @retval 0 The null pointer representation when it is all-zero on this target.
 * @pre @p pointer may carry any object-pointer representation.
 * @pre uintptr_t and object pointers have identical representation widths.
 * @post No pointed-to byte is read or modified.
 * @post The returned bytes equal the pointer representation on entry.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static uintptr_t internal_pointer_address(const void* pointer)
{
  uintptr_t address = 0U;
  static_assert(sizeof(address) == sizeof(pointer), "uintptr_t must represent object pointers");
  (void)memcpy((void*)&address, (const void*)&pointer, sizeof(address));
  return address;
}

/**
 * @brief Validate an arena descriptor before using caller-owned storage.
 * @details Checks lifecycle, exact capacity, backing pointer, and alignment.
 * @param[in] arena Descriptor to inspect.
 * @return Whether the descriptor is live and geometrically valid.
 * @retval true Arena is initialized with exact aligned backing storage.
 * @retval false Arena is null, inactive, malformed, or misaligned.
 * @pre @p arena is null or points to a readable descriptor.
 * @pre Descriptor fields may contain any representable values.
 * @post No state is modified.
 * @post No backing byte is inspected.
 * @note Pure and thread-safe for immutable input.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_ready(const epub_miniz_arena_t* arena)
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
  if (arena->capacity != (size_t)k_epub_miniz_pool_bytes) {
    return false;
  }
  return (internal_pointer_address(arena->base) % alignof(max_align_t)) == 0U;
}

/**
 * @brief Cast byte offset @p off to an aligned block header.
 * @param[in] arena Live arena.
 * @param[in] off Aligned byte offset, at most the arena capacity.
 * @return Header pointer or the one-past sentinel.
 */
RA8_INTERNAL
static priv_blk_t* internal_cell_at(const epub_miniz_arena_t* arena, size_t off)
{
  void* const cell = &arena->base[off];
  return (priv_blk_t*)cell;
}

/** @brief Return the arena's one-past byte sentinel. */
RA8_INTERNAL
static uint8_t* internal_end(const epub_miniz_arena_t* arena)
{
  return &arena->base[arena->capacity];
}

/** @brief Return the payload immediately following @p block. */
RA8_INTERNAL
static uint8_t* internal_payload(priv_blk_t* block)
{
  return &((uint8_t*)block)[(size_t)k_priv_hdr_bytes];
}

/** @brief Return the header for an allocation payload. */
RA8_INTERNAL
static priv_blk_t* internal_header(const epub_miniz_arena_t* arena, void* address)
{
  const uintptr_t payload = internal_pointer_address(address);
  const uintptr_t base    = internal_pointer_address(arena->base);
  return internal_cell_at(arena, (size_t)(payload - base) - (size_t)k_priv_hdr_bytes);
}

/** @brief Return the block immediately following @p block. */
RA8_INTERNAL
static priv_blk_t* internal_next(const epub_miniz_arena_t* arena, priv_blk_t* block)
{
  const uintptr_t end  = internal_pointer_address(&internal_payload(block)[block->size]);
  const uintptr_t base = internal_pointer_address(arena->base);
  return internal_cell_at(arena, (size_t)(end - base));
}

/**
 * @brief Test whether an address lies in the payload-bearing arena range.
 * @details Rejects inactive arenas and excludes the header-only prefix and
 * one-past byte without dereferencing the candidate address.
 * @param[in] arena Arena descriptor to validate and bound the comparison.
 * @param[in] address Candidate allocation payload address.
 * @return Whether the numeric address is inside the payload-bearing range.
 * @retval true Address is at or after the first payload byte and before end.
 * @retval false Arena is invalid or address lies outside the numeric range.
 * @pre @p arena is null or points to a readable descriptor.
 * @pre @p address may be null or any non-dereferenced pointer value.
 * @post No arena or candidate memory is changed.
 * @post The candidate address is never dereferenced.
 * @note Range membership alone does not prove the address starts a live block.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_in_pool(const epub_miniz_arena_t* arena, const void* address)
{
  if (!internal_ready(arena)) {
    return false;
  }
  const uintptr_t value = internal_pointer_address(address);
  const uintptr_t base  = internal_pointer_address(arena->base);
  const uintptr_t first = base + (uintptr_t)k_priv_hdr_bytes;
  const uintptr_t end   = base + (uintptr_t)arena->capacity;
  return (value >= first) && (value < end);
}

/**
 * @brief Align a pool-bounded request upward to a complete arena cell.
 * @details Applies the power-of-two maximum-alignment mask after request bounds
 * have already excluded arithmetic overflow.
 * @param[in] bytes Unaligned request no larger than the arena capacity.
 * @return Smallest aligned cell count covering @p bytes.
 * @retval 0 Returned when @p bytes is zero.
 * @retval aligned A multiple of ::k_priv_align not smaller than @p bytes.
 * @pre @p bytes passed ::internal_request_bytes.
 * @pre ::k_priv_align is a non-zero power of two.
 * @post Return value is arena-bounded and aligned.
 * @post No state is modified.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
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
 * @details Performs multiplication only after its overflow guard and rejects
 * requests larger than the complete caller-owned pool.
 * @retval true Product is representable, bounded, and stored in @p out_bytes.
 * @retval false Multiplication overflows or the product exceeds pool capacity.
 * @pre @p out_bytes is non-null and writable.
 * @pre @p items and @p size may be any representable `size_t` values.
 * @post Success initializes @p out_bytes exactly once.
 * @post Failure leaves @p out_bytes unchanged.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_request_bytes(size_t items, size_t size, size_t* out_bytes)
{
  if ((size != 0U) && (items > (SIZE_MAX / size))) {
    return false;
  }
  const size_t bytes = items * size;
  if (bytes > (size_t)k_epub_miniz_pool_bytes) {
    return false;
  }
  *out_bytes = bytes;
  return true;
}

/**
 * @brief Split a block if its remainder can hold another allocation.
 * @details Leaves unusably small tails inside the live block; otherwise emits
 * one aligned free block with its own in-band header.
 * @param[in] arena Live arena owning @p block.
 * @param[in,out] block Free block selected for allocation.
 * @param[in] need Aligned payload bytes required by the allocation.
 * @pre @p arena is ready and @p block is a valid free arena block.
 * @pre @p need is aligned, non-zero, and no greater than `block->size`.
 * @post The original block retains at least @p need payload bytes.
 * @post Any emitted remainder is aligned, free, and wholly inside the arena.
 * @note Not thread-safe through one arena.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_split(const epub_miniz_arena_t* arena, priv_blk_t* block, size_t need)
{
  const size_t remainder_min = (size_t)k_priv_hdr_bytes + (size_t)k_priv_align;
  if ((block->size - need) < remainder_min) {
    return;
  }
  const uint8_t* const payload   = internal_payload(block);
  const uintptr_t      rem_addr  = internal_pointer_address(payload) + need;
  const uintptr_t      base      = internal_pointer_address(arena->base);
  priv_blk_t* const    remainder = internal_cell_at(arena, (size_t)(rem_addr - base));
  remainder->size                = block->size - need - (size_t)k_priv_hdr_bytes;
  remainder->is_free             = (size_t)k_priv_blk_free;
  block->size                    = need;
}

/**
 * @brief Merge all adjacent free blocks in an arena.
 * @details Walks the implicit bounded block list and folds each contiguous free
 * run into its first header without moving live payload bytes.
 * @param[in,out] arena Live arena whose in-band headers may be updated.
 * @pre @p arena is ready and its block chain is geometrically valid.
 * @pre No concurrent allocation operation uses the same arena.
 * @post No two adjacent blocks are both marked free.
 * @post Live block addresses and payload bytes are unchanged.
 * @note The fixed walk guard bounds behavior if metadata is corrupted.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_coalesce(const epub_miniz_arena_t* arena)
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

ra8_err_t epub_miniz_arena_init(epub_miniz_arena_t* arena, void* workspace, size_t workspace_bytes)
{
  if ((arena == nullptr) || (workspace == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (workspace_bytes < (size_t)k_epub_miniz_pool_bytes) {
    return k_ra8_err_invalid_size;
  }
  if ((internal_pointer_address(workspace) % alignof(max_align_t)) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  const epub_miniz_arena_t next = {
    .base        = (uint8_t*)workspace,
    .capacity    = (size_t)k_epub_miniz_pool_bytes,
    .initialized = 1U,
  };
  priv_blk_t* const head = internal_cell_at(&next, 0U);
  head->size             = next.capacity - (size_t)k_priv_hdr_bytes;
  head->is_free          = (size_t)k_priv_blk_free;
  *arena                 = next;
  return k_ra8_ok;
}

void epub_miniz_arena_deinit(epub_miniz_arena_t* arena)
{
  if (arena != nullptr) {
    *arena = (epub_miniz_arena_t){};
  }
}

void* epub_miniz_alloc(void* opaque, size_t items, size_t size)
{
  epub_miniz_arena_t* const arena = (epub_miniz_arena_t*)opaque;
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

void epub_miniz_free(void* opaque, void* address)
{
  epub_miniz_arena_t* const arena = (epub_miniz_arena_t*)opaque;
  if ((address == nullptr) || !internal_in_pool(arena, address)) {
    return;
  }
  priv_blk_t* const block = internal_header(arena, address);
  block->is_free          = (size_t)k_priv_blk_free;
  internal_coalesce(arena);
}

void* epub_miniz_realloc(void* opaque, void* address, size_t items, size_t size)
{
  if (address == nullptr) {
    return epub_miniz_alloc(opaque, items, size);
  }
  epub_miniz_arena_t* const arena = (epub_miniz_arena_t*)opaque;
  if (!internal_in_pool(arena, address)) {
    return nullptr;
  }
  size_t bytes = 0U;
  if (!internal_request_bytes(items, size, &bytes)) {
    return nullptr;
  }
  const size_t need = internal_align_up(bytes);
  if (need == 0U) {
    epub_miniz_free(arena, address);
    return nullptr;
  }
  priv_blk_t* const block = internal_header(arena, address);
  if (block->size >= need) {
    return address;
  }
  void* const moved = epub_miniz_alloc(arena, items, size);
  if (moved == nullptr) {
    return nullptr;
  }
  (void)memcpy(moved, address, block->size);
  epub_miniz_free(arena, address);
  return moved;
}
