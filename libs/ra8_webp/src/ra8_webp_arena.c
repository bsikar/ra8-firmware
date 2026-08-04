/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_webp_arena.c
 * @brief Caller-bound bump arena with refcount auto-reset for libwebp.
 *
 * @details
 * See ra8_webp_arena.h for the rationale. The backing store is caller-owned:
 * the consumer binds a buffer sized for the WebP at hand via
 * ra8_webp_arena_bind(), and these hooks bump-allocate out of it. The reference
 * count drives an auto-reset so the arena fully drains after each decode. This
 * mirrors libs/ra8_reflow/src/ra8_img_arena.c (the stb_image arena) but adds a
 * zeroing calloc that libwebp's `WebPSafeCalloc` requires.
 *
 *
 * [Ring 4 / WebP] {World: NS}
 *
 * @since 0.1.0
 */

#include "ra8_webp_arena.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * @enum ra8_webp_arena_consts_t
 * @brief Arena alignment knobs (no magic numbers).
 */
typedef enum : uint32_t {
  k_ra8_webp_align      = 16U, /**< Allocation alignment, bytes.          */
  k_ra8_webp_align_mask = 15U, /**< k_ra8_webp_align - 1 (round-up mask). */
} ra8_webp_arena_consts_t;

/** Currently-bound arena, or nullptr when no decode is in flight. */
static ra8_webp_arena_t* s_arena = nullptr;

void ra8_webp_arena_bind(ra8_webp_arena_t* arena)
{
  s_arena = arena;
  if (arena != nullptr) {
    arena->offset = 0U;
    arena->live   = 0U;
  }
}

void ra8_webp_arena_unbind(void)
{
  s_arena = nullptr;
}

void* ra8_webp_arena_malloc(size_t n)
{
  ra8_webp_arena_t* const a = s_arena;
  if (a == nullptr) {
    return nullptr;
  }
  /* Reject requests too large to ever fit; this also prevents the alignment
   * round-up below from overflowing. */
  if (n > a->cap) {
    return nullptr;
  }
  const size_t aligned = (n + (size_t)k_ra8_webp_align_mask) & ~(size_t)k_ra8_webp_align_mask;
  if (aligned > (a->cap - a->offset)) {
    return nullptr;
  }
  void* const block = &a->base[a->offset];
  a->offset += aligned;
  a->live += 1U;
  return block;
}

void* ra8_webp_arena_calloc(size_t nmemb, size_t size)
{
  /* Guard the product against size_t overflow before allocating (libwebp
   * checks this too, but the arena must be safe on its own). */
  if ((size != 0U) && (nmemb > (SIZE_MAX / size))) {
    return nullptr;
  }
  const size_t total = nmemb * size;
  void* const  block = ra8_webp_arena_malloc(total);
  if (block == nullptr) {
    return nullptr;
  }
  if (total > 0U) {
    memset(block, 0, total);
  }
  return block;
}

void ra8_webp_arena_free(void* p)
{
  ra8_webp_arena_t* const a = s_arena;
  if ((p == nullptr) || (a == nullptr)) {
    return;
  }
  if (a->live > 0U) {
    a->live -= 1U;
  }
  if (a->live == 0U) {
    a->offset = 0U;
  }
}
