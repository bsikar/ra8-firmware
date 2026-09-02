/**
 * @file ra8_img_arena.c
 * @brief Caller-bound bump arena with refcount auto-reset for stb_image.
 *
 * @details
 * See ra8_img_arena.h for the rationale. Unlike the stb_truetype arena, the
 * backing store is not a file-scope array: the consumer binds a buffer sized
 * for the image at hand (SRAM for thumbnails, SDRAM for full covers) via
 * ra8_img_arena_bind(), and these hooks bump-allocate out of it. The reference
 * count drives an auto-reset so the arena fully drains after each decode.
 *
 *
 * [Ring 4 / Reflow] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_img_arena.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * @enum ra8_img_arena_consts_t
 * @brief Arena alignment knobs (no magic numbers).
 */
typedef enum : uint32_t {
  k_ra8_img_align      = 16U, /**< Allocation alignment, bytes.         */
  k_ra8_img_align_mask = 15U, /**< k_ra8_img_align - 1 (round-up mask). */
} ra8_img_arena_consts_t;

/** Currently-bound arena, or nullptr when no decode is in flight. */
static ra8_img_arena_t* s_arena = nullptr;

void ra8_img_arena_bind(ra8_img_arena_t* arena)
{
  s_arena = arena;
  if (arena != nullptr) {
    arena->offset = 0U;
    arena->live   = 0U;
  }
}

void ra8_img_arena_unbind(void)
{
  s_arena = nullptr;
}

void* ra8_img_arena_malloc(size_t n)
{
  ra8_img_arena_t* const a = s_arena;
  if (a == nullptr) {
    return nullptr;
  }
  /* Reject requests too large to ever fit; this also prevents the alignment
   * round-up below from overflowing. */
  if (n > a->cap) {
    return nullptr;
  }
  const size_t aligned = (n + (size_t)k_ra8_img_align_mask) & ~(size_t)k_ra8_img_align_mask;
  if (aligned > (a->cap - a->offset)) {
    return nullptr;
  }
  void* const block = &a->base[a->offset];
  a->offset += aligned;
  a->live += 1U;
  return block;
}

void ra8_img_arena_free(void* p)
{
  ra8_img_arena_t* const a = s_arena;
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

void* ra8_img_arena_realloc_sized(void* p, size_t oldsz, size_t newsz)
{
  if (p == nullptr) {
    return ra8_img_arena_malloc(newsz);
  }
  /* A bump arena cannot grow in place: allocate fresh, copy, release old. */
  void* const fresh = ra8_img_arena_malloc(newsz);
  if (fresh == nullptr) {
    return nullptr;
  }
  const size_t copy = (oldsz < newsz) ? oldsz : newsz;
  if (copy > 0U) {
    (void)memcpy(fresh, p, copy);
  }
  ra8_img_arena_free(p);
  return fresh;
}
