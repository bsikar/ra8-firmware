/**
 * @file ra_epub_miniz_alloc.c
 * @brief First-fit free-list allocator over a static pool for miniz.
 *
 * @details
 * Implements ::ra_epub_miniz_alloc / ::ra_epub_miniz_free /
 * ::ra_epub_miniz_realloc against one ``max_align_t``-aligned static buffer.
 * The pool is an implicit list of contiguous blocks, each prefixed by a small
 * header carrying its payload size and a free flag. Allocation is first-fit
 * with split; free coalesces every run of adjacent free blocks, so the pool
 * never fragments permanently and is reusable across unbounded alloc/free
 * cycles (miniz allocates and frees a decompressor per chapter extract).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_epub_miniz_alloc.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

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
 * @var s_pool
 * @brief The static arena. Aligned so every split payload stays aligned.
 * @warning Module-private; only the allocator functions touch it.
 */
alignas(max_align_t) static uint8_t s_pool[k_ra_epub_miniz_pool_bytes];

/**
 * @var s_init
 * @brief False until the pool is laid out as one free block.
 * @warning Module-private.
 */
static bool s_init = false;

/** @enum priv_alloc_const_t @brief Local allocator constants. */
typedef enum : size_t {
  k_priv_hdr_bytes = sizeof(priv_blk_t),       /**< Block-header size.          */
  k_priv_align     = alignof(max_align_t),      /**< Payload alignment.          */
  /** Upper bound on block count (NASA Rule 2 loop bound): every block is at
   *  least a header plus one alignment unit of payload. */
  k_priv_walk_max = k_ra_epub_miniz_pool_bytes / (sizeof(priv_blk_t) + alignof(max_align_t)),
  k_priv_blk_free = 1U, /**< ``is_free`` value for a free block.  */
  k_priv_blk_used = 0U, /**< ``is_free`` value for a used block.  */
} priv_alloc_const_t;

/** @brief Round @p n up to the payload alignment. */
static size_t priv_align_up(size_t n)
{
  const size_t mask = (size_t)k_priv_align - 1U;
  return (n + mask) & ~mask;
}

/** @brief One-past-the-end sentinel of the pool. */
static uint8_t* priv_end(void)
{
  return &s_pool[0] + (size_t)k_ra_epub_miniz_pool_bytes;
}

/** @brief Payload pointer for block @p b. */
static uint8_t* priv_payload(priv_blk_t* b)
{
  return (uint8_t*)b + (size_t)k_priv_hdr_bytes;
}

/** @brief Header for the block whose payload starts at @p p (or NULL). */
static priv_blk_t* priv_header(void* p)
{
  if (p == nullptr) {
    return nullptr;
  }
  return (priv_blk_t*)(void*)((uint8_t*)p - (size_t)k_priv_hdr_bytes);
}

/** @brief Block immediately following @p b in the implicit list. */
static priv_blk_t* priv_next(priv_blk_t* b)
{
  return (priv_blk_t*)(void*)(priv_payload(b) + b->size);
}

/** @brief Lay the whole pool out as a single free block (idempotent). */
static void priv_init(void)
{
  if (s_init) {
    return;
  }
  priv_blk_t* head = (priv_blk_t*)(void*)&s_pool[0];
  head->size       = (size_t)k_ra_epub_miniz_pool_bytes - (size_t)k_priv_hdr_bytes;
  head->is_free    = (size_t)k_priv_blk_free;
  s_init           = true;
}

/** @brief True if @p p points inside this allocator's pool. */
static bool priv_in_pool(const void* p)
{
  const uint8_t* q = (const uint8_t*)p;
  return (q >= &s_pool[0]) && (q < priv_end());
}

/** @brief Merge every run of adjacent free blocks into one. */
static void priv_coalesce(void)
{
  priv_blk_t* b = (priv_blk_t*)(void*)&s_pool[0];
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

/** @brief Split @p b so it holds exactly @p need, trailing a free remainder. */
static void priv_split(priv_blk_t* b, size_t need)
{
  /* Only split when the remainder can hold a header plus a minimum payload. */
  if (b->size >= (need + (size_t)k_priv_hdr_bytes + (size_t)k_priv_align)) {
    priv_blk_t* rem = (priv_blk_t*)(void*)(priv_payload(b) + need);
    rem->size       = b->size - need - (size_t)k_priv_hdr_bytes;
    rem->is_free    = (size_t)k_priv_blk_free;
    b->size         = need;
  }
}

void* ra_epub_miniz_alloc(void* opaque, size_t items, size_t size)
{
  (void)opaque;
  /* Overflow guard (MC/DC): a non-zero element size whose count would wrap. */
  if ((size != 0U) && (items > (SIZE_MAX / size))) {
    return nullptr;
  }
  priv_init();
  size_t need = priv_align_up(items * size);
  if (need == 0U) {
    need = (size_t)k_priv_align; /* never hand back a zero-size block */
  }

  priv_blk_t* b = (priv_blk_t*)(void*)&s_pool[0];
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

void ra_epub_miniz_free(void* opaque, void* address)
{
  (void)opaque;
  if ((address == nullptr) || !priv_in_pool(address)) {
    return;
  }
  priv_blk_t* b = priv_header(address);
  b->is_free    = (size_t)k_priv_blk_free;
  priv_coalesce();
}

void* ra_epub_miniz_realloc(void* opaque, void* address, size_t items, size_t size)
{
  if (address == nullptr) {
    return ra_epub_miniz_alloc(opaque, items, size);
  }
  if ((size != 0U) && (items > (SIZE_MAX / size))) {
    return nullptr;
  }
  const size_t need = priv_align_up(items * size);
  if (need == 0U) {
    ra_epub_miniz_free(opaque, address);
    return nullptr;
  }
  priv_blk_t* b = priv_header(address);
  if (b->size >= need) {
    return address; /* already big enough -- keep in place */
  }
  void* moved = ra_epub_miniz_alloc(opaque, items, size);
  if (moved == nullptr) {
    return nullptr; /* old block still valid */
  }
  (void)memcpy(moved, address, b->size);
  ra_epub_miniz_free(opaque, address);
  return moved;
}
