/**
 * @file ra8_imgdec_scratch.c
 * @brief The shared bump-scratch policy behind the image-decoder seam (#768).
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 3 / Imaging] {World: NS}
 *
 * @details
 * One implementation of the arithmetic the five private decoder shims each
 * wrote out: round up to the block alignment, refuse anything that would run
 * past the capacity, count the live blocks, rewind when the count drains.
 * Every entry point takes the scratch explicitly, so nothing here depends on
 * a file-static slot; a shim fronting a SOUP macro API keeps that slot itself.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_imgdec_scratch.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_arena.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Round @p bytes up to ::k_ra8_imgdec_scratch_align.
 *
 * @param[in]  bytes Byte count to round.
 * @param[out] out   Receives the rounded count on success.
 *
 * @return bool True when the rounded count fits a `size_t`.
 *
 * @post On false @p out is untouched.
 */
RA8_INTERNAL static bool internal_round_up(size_t bytes, size_t* out) {
  const size_t align = (size_t)k_ra8_imgdec_scratch_align;
  const size_t slack = align - 1U;

  if (bytes > (SIZE_MAX - slack)) {
    return false;
  }

  *out = (bytes + slack) & ~slack;
  return true;
}

/**
 * @brief Whether @p scratch is initialised and can be drawn from.
 *
 * @param[in] scratch Scratch to test; NULL reads as unusable.
 *
 * @return bool True when a backing store is bound.
 */
RA8_INTERNAL static bool internal_usable(const ra8_imgdec_scratch_t* scratch) {
  return (scratch != nullptr) && (scratch->base != nullptr) && (scratch->cap != 0U);
}

/**
 * @brief Reserve @p bytes from @p scratch without zeroing them.
 *
 * @param[in,out] scratch Scratch to draw from; assumed usable.
 * @param[in]     bytes   Byte count requested; assumed non-zero.
 *
 * @return void* Aligned block, or NULL when it does not fit.
 *
 * @post On success `offset`, `live` and `high_water` are updated together.
 */
RA8_INTERNAL static void* internal_reserve(ra8_imgdec_scratch_t* scratch, size_t bytes) {
  size_t want = 0U;

  if (!internal_round_up(bytes, &want)) {
    return nullptr;
  }

  if (want > (scratch->cap - scratch->offset)) {
    return nullptr;
  }

  uint8_t* const block = &scratch->base[scratch->offset];

  scratch->offset += want;
  scratch->live += 1U;

  if (scratch->offset > scratch->high_water) {
    scratch->high_water = scratch->offset;
  }

  return block;
}

/**
 * @brief Whether @p align is a usable carve alignment for this contract.
 *
 * @param[in] align Alignment to test; assumed non-zero.
 *
 * @return bool True when @p align is a power of two.
 */
RA8_INTERNAL static bool internal_pow2(uint32_t align) {
  return (align & (align - 1U)) == 0U;
}

/* =============================================================================
 * Public entry points
 * =============================================================================
 */

ra8_err_t ra8_imgdec_scratch_init(ra8_imgdec_scratch_t* scratch, void* base, size_t cap) {
  if ((scratch == nullptr) || (base == nullptr)) {
    return k_ra8_err_invalid_arg;
  }

  if (cap == 0U) {
    return k_ra8_err_invalid_size;
  }

  scratch->base       = (uint8_t*)base;
  scratch->cap        = cap;
  scratch->offset     = 0U;
  scratch->live       = 0U;
  scratch->high_water = 0U;

  return k_ra8_ok;
}

void ra8_imgdec_scratch_reset(ra8_imgdec_scratch_t* scratch) {
  if (scratch == nullptr) {
    return;
  }

  scratch->offset = 0U;
  scratch->live   = 0U;
}

void* ra8_imgdec_scratch_alloc(ra8_imgdec_scratch_t* scratch, size_t bytes) {
  if (!internal_usable(scratch) || (bytes == 0U)) {
    return nullptr;
  }

  return internal_reserve(scratch, bytes);
}

void* ra8_imgdec_scratch_calloc(ra8_imgdec_scratch_t* scratch, size_t count, size_t size) {
  if (!internal_usable(scratch) || (count == 0U) || (size == 0U)) {
    return nullptr;
  }

  if (count > (SIZE_MAX / size)) {
    return nullptr;
  }

  const size_t bytes = count * size;
  void* const  block = internal_reserve(scratch, bytes);

  if (block != nullptr) {
    (void)memset(block, 0, bytes);
  }

  return block;
}

void* ra8_imgdec_scratch_realloc(ra8_imgdec_scratch_t* scratch, void* ptr, size_t old_bytes,
                                 size_t new_bytes) {
  if (!internal_usable(scratch) || (new_bytes == 0U)) {
    return nullptr;
  }

  void* const block = internal_reserve(scratch, new_bytes);

  if (block == nullptr) {
    return nullptr;
  }

  if (ptr != nullptr) {
    const size_t carry = (old_bytes < new_bytes) ? old_bytes : new_bytes;

    if (carry != 0U) {
      (void)memcpy(block, ptr, carry);
    }

    ra8_imgdec_scratch_free(scratch, ptr);
  }

  return block;
}

void ra8_imgdec_scratch_free(ra8_imgdec_scratch_t* scratch, void* ptr) {
  if ((scratch == nullptr) || (ptr == nullptr) || (scratch->live == 0U)) {
    return;
  }

  scratch->live -= 1U;

  if (scratch->live == 0U) {
    scratch->offset = 0U;
  }
}

size_t ra8_imgdec_scratch_high_water(const ra8_imgdec_scratch_t* scratch) {
  if (scratch == nullptr) {
    return 0U;
  }

  return scratch->high_water;
}

ra8_err_t ra8_imgdec_scratch_carve(ra8_imgdec_scratch_t* scratch, ra8_arena_t* arena,
                                   uint32_t bytes, uint32_t align) {
  if ((scratch == nullptr) || (arena == nullptr)) {
    return k_ra8_err_invalid_arg;
  }

  *scratch = (ra8_imgdec_scratch_t){0};

  if (bytes == 0U) {
    return k_ra8_err_invalid_size;
  }

  const uint32_t want = (align == 0U) ? (uint32_t)k_ra8_imgdec_scratch_align : align;

  if (!internal_pow2(want)) {
    return k_ra8_err_invalid_arg;
  }

  if (want > (uint32_t)k_ra8_imgdec_scratch_align) {
    return k_ra8_err_not_supported;
  }

  void*           block = nullptr;
  const ra8_err_t err   = ra8_arena_carve(arena, bytes, want, &block);

  if (err != k_ra8_ok) {
    return err;
  }

  return ra8_imgdec_scratch_init(scratch, block, (size_t)bytes);
}
