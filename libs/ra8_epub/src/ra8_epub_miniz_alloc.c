/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_epub_miniz_alloc.c
 * @brief First-fit free-list allocator over a static pool for miniz.
 *
 * @details
 * Implements ::ra8_epub_miniz_alloc / ::ra8_epub_miniz_free /
 * ::ra8_epub_miniz_realloc against one ``max_align_t``-aligned static buffer.
 * The pool is an implicit list of contiguous blocks, each prefixed by a small
 * header carrying its payload size and a free flag. Allocation is first-fit
 * with split; free coalesces every run of adjacent free blocks, so the pool
 * never fragments permanently and is reusable across unbounded alloc/free
 * cycles (miniz allocates and frees a decompressor per chapter extract).
 */

#include "ra8_epub_miniz_alloc.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"

/**
 * @struct priv_blk_t
 * @brief Header prefixing every pool block.
 *
 * @details
 * Two ``size_t`` fields keep the header a multiple of ``max_align_t`` on both
 * the 32-bit target and the 64-bit host, so the payload that follows is
 * naturally aligned when the pool base is.
 *
 * @invariant ``size`` is a multiple of ``alignof(max_align_t)``.
 * @invariant ``is_free`` is 0 (in use) or 1 (free).
 */
typedef struct {
  size_t size;    /**< Payload bytes that follow this header. */
  size_t is_free; /**< 1 = free, 0 = in use.                  */
} priv_blk_t;

/**
 * @union priv_pool_cell_t
 * @brief One ``max_align_t``-aligned storage cell of the arena.
 *
 * @details
 * Backing the pool with an array of this union (rather than a raw
 * ``uint8_t`` buffer) means a pointer into the pool already carries
 * ``alignof(max_align_t)`` -- which is ``>= alignof(priv_blk_t)``. Casting
 * such a pointer to ``priv_blk_t*`` therefore never increases the required
 * alignment, so it is both well-defined and accepted under ``-Wcast-align``.
 *
 * @invariant ``sizeof(priv_pool_cell_t) == alignof(max_align_t)``.
 */
typedef union {
  max_align_t align; /**< Forces ``alignof(max_align_t)`` on the cell. */
  uint8_t     byte;  /**< Lets the arena be addressed byte-wise.       */
} priv_pool_cell_t;

/** @enum priv_pool_dim_t @brief Arena dimensions in aligned cells. */
typedef enum : size_t {
  /** Cells needed to cover the requested pool, rounded up. */
  k_priv_pool_cells =
    (k_ra8_epub_miniz_pool_bytes + sizeof(priv_pool_cell_t) - 1U) / sizeof(priv_pool_cell_t),
} priv_pool_dim_t;

/**
 * @var s_pool
 * @brief The static arena. Aligned so every split payload stays aligned.
 * @warning Module-private; only the allocator functions touch it.
 */
static priv_pool_cell_t s_pool[k_priv_pool_cells];

/**
 * @brief Byte-pointer to the start of the arena.
 *
 * @details
 * Narrows the aligned cell pointer to ``uint8_t*`` for byte arithmetic. The
 * inverse (::priv_cell_at) re-widens before any ``priv_blk_t*`` cast so the
 * alignment guarantee is never lost across the round trip.
 *
 * @return Pointer to byte 0 of the pool.
 */
RA8_INTERNAL
static uint8_t* priv_base(void)
{
  return &s_pool[0].byte;
}

/**
 * @brief Block header at byte offset @p off from the pool base.
 *
 * @details
 * @p off is always a multiple of ``alignof(max_align_t)`` (headers and
 * payloads are kept aligned), so dividing by the cell size yields the exact
 * aligned cell index. Indexing the cell array re-establishes
 * ``alignof(max_align_t)``, making the ``priv_blk_t*`` cast alignment-safe.
 *
 * @param[in] off Byte offset into the pool; must be a cell-size multiple.
 * @return Header pointer for the block at @p off.
 */
RA8_INTERNAL
static priv_blk_t* priv_cell_at(size_t off)
{
  return (priv_blk_t*)&s_pool[off / sizeof(priv_pool_cell_t)];
}

/**
 * @var s_init
 * @brief False until the pool is laid out as one free block.
 * @warning Module-private.
 */
static bool s_init = false;

/** @enum priv_alloc_const_t @brief Local allocator constants. */
typedef enum : size_t {
  /** Block-header stride, rounded up to a whole number of arena cells.
   *  ::priv_cell_at indexes ``s_pool`` by ``off / sizeof(priv_pool_cell_t)``,
   *  which truncates any offset that is not a cell multiple. Keeping the
   *  header a whole number of cells means a payload that starts ``hdr`` bytes
   *  after a cell-aligned header is itself cell-aligned, so every header and
   *  payload offset divides exactly and no block is ever aliased onto a
   *  neighbour. ``sizeof(priv_blk_t)`` alone is not enough: on a target whose
   *  ``priv_blk_t`` is smaller than a cell, the bare size would leave payloads
   *  on sub-cell offsets that ::priv_cell_at then collapses. */
  k_priv_hdr_bytes =
    ((sizeof(priv_blk_t) + sizeof(priv_pool_cell_t) - 1U) / sizeof(priv_pool_cell_t)) *
    sizeof(priv_pool_cell_t),
  /** Payload alignment == one arena cell. This is ``>= alignof(max_align_t)``
   *  (the cell embeds a ``max_align_t``), so rounding requests up to it both
   *  satisfies the documented ``max_align_t`` guarantee and keeps every split
   *  offset cell-aligned for ::priv_cell_at. */
  k_priv_align = sizeof(priv_pool_cell_t),
  /** Upper bound on block count (NASA Rule 2 loop bound): every block is at
   *  least a header plus one alignment unit of payload. */
  k_priv_walk_max = k_ra8_epub_miniz_pool_bytes / (k_priv_hdr_bytes + sizeof(priv_pool_cell_t)),
  k_priv_blk_free = 1U, /**< ``is_free`` value for a free block. */
  k_priv_blk_used = 0U, /**< ``is_free`` value for a used block. */
} priv_alloc_const_t;

/**
 * @brief Round @p n up to the payload alignment boundary.
 *
 * @details
 * Computes the smallest multiple of ``k_priv_align`` that is greater than or
 * equal to @p n using the standard bitmask idiom:
 * ``(n + mask) & ~mask`` where ``mask = k_priv_align - 1``.  Because
 * ``k_priv_align`` equals ``sizeof(priv_pool_cell_t)`` (a power of two), the
 * bitmask is exact and no division is required.  The result ensures that every
 * payload size stored in a block header is a cell-size multiple, which
 * guarantees that ``priv_cell_at`` can index ``s_pool`` without truncation.
 *
 * @param[in] n Raw byte count to round up; may be zero.
 *
 * @return Aligned byte count; always a multiple of ``k_priv_align`` and
 *         always greater than or equal to @p n.
 * @retval 0             When @p n is 0 (zero rounds to zero).
 * @retval k_priv_align  Minimum non-zero aligned size when @p n is in
 *                       ``(0, k_priv_align]``.
 *
 * @pre  ``k_priv_align`` is a power of two and greater than zero.
 * @pre  @p n plus ``k_priv_align - 1`` does not overflow ``size_t``
 *       (caller is responsible; pool sizes are bounded by the pool constant).
 *
 * @post Return value is a multiple of ``k_priv_align``.
 * @post Return value is greater than or equal to @p n.
 *
 * @note Not thread-safe; pure computation with no shared state.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static size_t priv_align_up(size_t n)
{
  const size_t mask = (size_t)k_priv_align - 1U;
  return (n + mask) & ~mask;
}

/** @brief One-past-the-end sentinel of the pool. */
RA8_INTERNAL
static uint8_t* priv_end(void)
{
  return priv_base() + (size_t)k_ra8_epub_miniz_pool_bytes;
}

/** @brief Payload pointer for block @p b. */
RA8_INTERNAL
static uint8_t* priv_payload(priv_blk_t* b)
{
  return (uint8_t*)b + (size_t)k_priv_hdr_bytes;
}

/** @brief Header for the block whose payload starts at @p p (or NULL). */
RA8_INTERNAL
static priv_blk_t* priv_header(void* p)
{
  if (p == nullptr) {
    return nullptr;
  }
  const size_t payload_off = (size_t)((uint8_t*)p - priv_base());
  return priv_cell_at(payload_off - (size_t)k_priv_hdr_bytes);
}

/** @brief Block immediately following @p b in the implicit list. */
RA8_INTERNAL
static priv_blk_t* priv_next(priv_blk_t* b)
{
  /* uintptr_t byte-diff (both operands are uint8_t* into s_pool): identical
   * value to pointer subtraction, but avoids the analyzer's cross-array
   * pointer-subtraction UB false positive. Bind to locals first so the casts
   * are not applied to a bare function-call result (-Wbad-function-cast). */
  const uint8_t* const blk_end = priv_payload(b) + b->size;
  const uint8_t* const pool    = priv_base();
  const size_t         off     = (size_t)((uintptr_t)blk_end - (uintptr_t)pool);
  return priv_cell_at(off);
}

/**
 * @brief Lay the whole pool out as a single free block (idempotent).
 *
 * @details
 * On the first call, writes a single ``priv_blk_t`` header at the start of
 * ``s_pool`` whose ``size`` spans the entire arena minus the header itself and
 * marks it free, then sets ``s_init`` to prevent re-initialisation.
 * Subsequent calls return immediately without touching the pool, making the
 * function safe to call at the top of every public allocator entry-point
 * without performance cost after startup.  The pool header size and free-block
 * sentinel values are taken from ``priv_alloc_const_t`` so no magic numbers
 * appear in this function.
 *
 * @pre  ``s_pool`` is a statically allocated array of ``priv_pool_cell_t``
 *       with at least ``k_priv_hdr_bytes + k_priv_align`` bytes of capacity.
 * @pre  No concurrent call to any allocator function is in progress (single-
 *       threaded initialisation context assumed).
 *
 * @post When ``s_init`` was false on entry, ``s_pool`` contains exactly one
 *       free block spanning ``k_ra8_epub_miniz_pool_bytes - k_priv_hdr_bytes``
 *       payload bytes.
 * @post ``s_init`` is true after the call.
 *
 * @return Nothing.
 *
 * @note Not thread-safe; must be called before any concurrent allocator use.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_init(void)
{
  if (s_init) {
    return;
  }
  priv_blk_t* head = priv_cell_at(0U);
  head->size       = (size_t)k_ra8_epub_miniz_pool_bytes - (size_t)k_priv_hdr_bytes;
  head->is_free    = (size_t)k_priv_blk_free;
  s_init           = true;
}

/**
 * @brief True if @p p points inside this allocator's pool.
 *
 * @details
 * Compares the byte address of @p p against the half-open interval
 * ``[priv_base(), priv_end())``.  Both bounds are derived from ``s_pool``
 * at call time, so the check remains correct after any future resize of
 * the pool constant.  The function is used by ``ra8_epub_miniz_free`` and
 * ``ra8_epub_miniz_realloc`` to silently ignore pointers that did not
 * originate from this allocator (e.g. a null or a foreign heap pointer),
 * which is the behaviour expected by the miniz callback contract.
 *
 * @param[in] p Pointer to test; may be null or point anywhere in the
 *              address space.
 *
 * @return Boolean membership result.
 * @retval true   @p p lies in ``[priv_base(), priv_end())``.
 * @retval false  @p p is null or lies outside the pool range.
 *
 * @pre  ``s_pool`` has been declared (static storage; always true).
 * @pre  ``priv_base()`` and ``priv_end()`` return consistent pointers into
 *       the same array (guaranteed by the module-private design).
 *
 * @post The pool state is not modified.
 * @post The return value is deterministic for any given @p p and pool layout.
 *
 * @note Not thread-safe; pure read of static storage with no locking.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_in_pool(const void* p)
{
  const uint8_t* q = (const uint8_t*)p;
  return (q >= priv_base()) && (q < priv_end());
}

/**
 * @brief Merge every run of adjacent free blocks into one.
 *
 * @details
 * Walks the implicit free list from the start of the pool to its end.
 * Whenever a free block is found, its successor is examined in an inner loop:
 * if the successor is also free its payload and header bytes are absorbed into
 * the current block (expanding ``b->size`` by ``k_priv_hdr_bytes + n->size``)
 * and the check repeats with the new successor.  Once the successor is either
 * in-use or past the pool end, the outer walk advances to the next block via
 * ``priv_next``.  Both loops are bounded by ``k_priv_walk_max``, satisfying
 * NASA Power of 10 Rule 2.  After coalescing, no two adjacent free blocks
 * remain in the pool, so subsequent first-fit searches see the maximum
 * available contiguous free space.
 *
 * @pre  ``priv_init`` has been called at least once (``s_init`` is true).
 * @pre  All block headers reachable from the pool base are well-formed
 *       (``size`` is a cell multiple; ``is_free`` is 0 or 1).
 *
 * @post No two adjacent blocks in the pool are both free.
 * @post The total number of payload bytes owned by free blocks is unchanged.
 *
 * @return Nothing.
 *
 * @note Not thread-safe; must be called with exclusive access to the pool.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_coalesce(void)
{
  priv_blk_t* b = priv_cell_at(0U);
  for (size_t guard = 0U; guard < (size_t)k_priv_walk_max; guard++) {
    if ((uint8_t*)b >= priv_end()) {
      break;
    }
    if (b->is_free == (size_t)k_priv_blk_free) {
      priv_blk_t* n = priv_next(b);
      /* Decision (MC/DC): swallow the successor while it exists and is free. */
      while (((uint8_t*)n < priv_end()) && (n->is_free == (size_t)k_priv_blk_free)) {
        b->size += (size_t)k_priv_hdr_bytes + n->size;
        n = priv_next(b);
      }
    }
    b = priv_next(b);
  }
}

/**
 * @brief Split @p b so it holds exactly @p need bytes, leaving a free
 *        remainder block after it.
 *
 * @details
 * Examines whether the block is large enough to be split: a remainder block
 * requires at least ``k_priv_hdr_bytes + k_priv_align`` bytes beyond @p need.
 * When the condition is met, a new ``priv_blk_t`` header is placed immediately
 * after the first @p need aligned payload bytes of @p b.  The remainder
 * header's ``size`` is set to absorb all leftover bytes (``b->size - need -
 * k_priv_hdr_bytes``) and is marked free.  Block @p b is then trimmed to
 * exactly @p need bytes.  If the block is too small to split, it is left
 * unchanged and the caller receives a slightly oversized allocation, which is
 * correct because the block's original size was already at least @p need.
 *
 * @param[in,out] b    Block to split; must be a valid free block inside the
 *                     pool with ``b->size >= need``.
 * @param[in]     need Aligned payload size the caller requires; must be a
 *                     multiple of ``k_priv_align`` and greater than zero.
 *
 * @pre  @p b is non-null and its header lies within ``s_pool``.
 * @pre  @p need is a positive multiple of ``k_priv_align`` and does not
 *       exceed ``b->size``.
 *
 * @post When split occurred: @p b has ``size == need`` and the remainder block
 *       is marked free with size equal to the leftover payload bytes.
 * @post When no split occurred: @p b is unchanged.
 *
 * @return Nothing.
 *
 * @note Not thread-safe; must be called with exclusive access to the pool.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_split(priv_blk_t* b, size_t need)
{
  /* Only split when the remainder can hold a header plus a minimum payload. */
  if (b->size >= (need + (size_t)k_priv_hdr_bytes + (size_t)k_priv_align)) {
    const uint8_t* const rem_ptr = priv_payload(b) + need;
    const uint8_t* const pool    = priv_base();
    const size_t         rem_off = (size_t)((uintptr_t)rem_ptr - (uintptr_t)pool);
    priv_blk_t*          rem     = priv_cell_at(rem_off);
    rem->size                    = b->size - need - (size_t)k_priv_hdr_bytes;
    rem->is_free                 = (size_t)k_priv_blk_free;
    b->size                      = need;
  }
}

void* ra8_epub_miniz_alloc(void* opaque, size_t items, size_t size)
{
  (void)opaque;
  /* Overflow guard (MC/DC): a non-zero element size whose count would wrap. */
  if ((size != 0U) && (items > (SIZE_MAX / size))) {
    return nullptr;
  }
  /* Oversize guard (MC/DC): a request larger than the whole pool can never be
   * satisfied, and rounding it up in priv_align_up would overflow
   * (bytes + align - 1) into a tiny under-allocation -- reject it first. */
  if ((items * size) > (size_t)k_ra8_epub_miniz_pool_bytes) {
    return nullptr;
  }
  priv_init();
  size_t need = priv_align_up(items * size);
  if (need == 0U) {
    need = (size_t)k_priv_align; /* never hand back a zero-size block */
  }

  priv_blk_t* b = priv_cell_at(0U);
  for (size_t guard = 0U; guard < (size_t)k_priv_walk_max; guard++) {
    if ((uint8_t*)b >= priv_end()) {
      break;
    }
    /* First-fit decision (MC/DC): a free block big enough for the request. */
    if ((b->is_free == (size_t)k_priv_blk_free) && (b->size >= need)) {
      priv_split(b, need);
      b->is_free = (size_t)k_priv_blk_used;
      return priv_payload(b);
    }
    b = priv_next(b);
  }
  return nullptr;
}

void ra8_epub_miniz_free(void* opaque, void* address)
{
  (void)opaque;
  if ((address == nullptr) || !priv_in_pool(address)) {
    return;
  }
  priv_blk_t* b = priv_header(address);
  b->is_free    = (size_t)k_priv_blk_free;
  priv_coalesce();
}

void* ra8_epub_miniz_realloc(void* opaque, void* address, size_t items, size_t size)
{
  if (address == nullptr) {
    return ra8_epub_miniz_alloc(opaque, items, size);
  }
  if ((size != 0U) && (items > (SIZE_MAX / size))) {
    return nullptr;
  }
  /* Oversize guard (MC/DC): see ra8_epub_miniz_alloc -- reject a larger-than-pool
   * request before priv_align_up can overflow it; the old block stays valid. */
  if ((items * size) > (size_t)k_ra8_epub_miniz_pool_bytes) {
    return nullptr;
  }
  const size_t need = priv_align_up(items * size);
  if (need == 0U) {
    ra8_epub_miniz_free(opaque, address);
    return nullptr;
  }
  priv_blk_t* b = priv_header(address);
  if (b->size >= need) {
    return address; /* already big enough -- keep in place */
  }
  void* moved = ra8_epub_miniz_alloc(opaque, items, size);
  if (moved == nullptr) {
    return nullptr; /* old block still valid */
  }
  (void)memcpy(moved, address, b->size);
  ra8_epub_miniz_free(opaque, address);
  return moved;
}
