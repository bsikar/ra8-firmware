/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_stbtt_alloc.c
 * @brief Fixed bump arena with refcount auto-reset for stb_truetype.
 *
 * @details
 * See ra8_stbtt_alloc.h for the rationale. The capacity is sized from the
 * measured worst case: the densest printable-ASCII glyph ('@') of the
 * bundled Literata face at the library's maximum font size
 * (`k_ra8_reflow_max_font_px` = 96 px) accumulates ~32 KiB of stb scratch
 * within a single rasterisation. The arena is provisioned at 3x that so a
 * heavier face or a larger glyph still fits with margin.
 *
 *
 * [Ring 4 / Reflow] {World: NS}
 *
 * @since 0.1.0
 */

#include "ra8_stbtt_alloc.h"

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @enum ra8_stbtt_alloc_consts_t
 * @brief Arena sizing knobs (no magic numbers).
 */
typedef enum : uint32_t {
  k_ra8_stbtt_align       = 16U,         /**< Allocation alignment, bytes. */
  k_ra8_stbtt_align_mask  = 15U,         /**< k_ra8_stbtt_align - 1.       */
  k_ra8_stbtt_arena_bytes = 96U * 1024U, /**< 3x the 32 KiB worst case.    */
} ra8_stbtt_alloc_consts_t;

/** Backing store for all stb_truetype scratch (NASA P10 Rule 3). Aligned
 * so every 16-byte-rounded offset yields a 16-byte-aligned pointer. */
alignas(k_ra8_stbtt_align) static uint8_t s_arena[k_ra8_stbtt_arena_bytes];

/** Current bump offset into @ref s_arena, bytes. */
static size_t s_offset = 0U;

/** Count of live (allocated, not yet freed) blocks. */
static size_t s_live = 0U;

/** Peak @ref s_offset observed since boot (diagnostic). */
static size_t s_high_water = 0U;

void* ra8_stbtt_malloc(size_t n)
{
  /* Reject requests too large to ever fit; this also prevents the
   * alignment round-up below from overflowing. */
  if (n > (size_t)k_ra8_stbtt_arena_bytes) {
    return nullptr;
  }
  const size_t aligned = (n + (size_t)k_ra8_stbtt_align_mask) & ~(size_t)k_ra8_stbtt_align_mask;
  if (aligned > (size_t)k_ra8_stbtt_arena_bytes - s_offset) {
    return nullptr;
  }
  void* const block = &s_arena[s_offset];
  s_offset += aligned;
  s_live += 1U;
  if (s_offset > s_high_water) {
    s_high_water = s_offset;
  }
  return block;
}

void ra8_stbtt_free(void* p)
{
  if (p == nullptr) {
    return;
  }
  if (s_live > 0U) {
    s_live -= 1U;
  }
  if (s_live == 0U) {
    s_offset = 0U;
  }
}

size_t ra8_stbtt_alloc_high_water(void)
{
  return s_high_water;
}
