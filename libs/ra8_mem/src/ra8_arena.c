/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_arena.c
 * @brief Init-time bump-arena implementation (Layer 0, #147).
 *
 * @par Tag
 * [Ring 2 / Core] {World: NS}
 *
 * @details
 * Bump-only allocation with power-of-two alignment. Fit checks use `uintptr_t`
 * subtraction against the region end so they never overflow.
 */

#include "ra8_arena.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_arena";

/**
 * @brief Test whether @p v is a power of two.
 *
 * @details A power of two has exactly one set bit, so `v & (v - 1)` is zero;
 *          zero is rejected by the leading `v != 0` term.
 *
 * @param[in] v Value to test.
 *
 * @return true if @p v is a non-zero power of two, else false.
 * @retval true  @p v is a non-zero power of two.
 * @retval false @p v is zero or has more than one set bit.
 *
 * @pre None.
 * @pre @p v fits in a uint32_t.
 * @post No state is modified.
 * @post The result depends only on @p v.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_is_pow2(uint32_t v)
{
  if (v == 0U) {
    return false;
  }
  return (v & (v - 1U)) == 0U;
}

ra8_err_t ra8_arena_init(ra8_arena_t* arena, void* base, uint32_t size)
{
  RA8_CHECK_NULL_PTR(arena, s_tag, "arena must not be nullptr");
  RA8_CHECK_NULL_PTR(base, s_tag, "base must not be nullptr");
  if (size == 0U) {
    return k_ra8_err_invalid_size;
  }
  arena->base = (uint8_t*)base;
  arena->size = size;
  arena->used = 0U;
  return k_ra8_ok;
}

ra8_err_t ra8_arena_carve(ra8_arena_t* arena, uint32_t bytes, uint32_t align, void** out_ptr)
{
  RA8_CHECK_NULL_PTR(arena, s_tag, "arena must not be nullptr");
  RA8_CHECK_NULL_PTR(out_ptr, s_tag, "out_ptr must not be nullptr");
  if (bytes == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (!priv_is_pow2(align)) {
    return k_ra8_err_invalid_arg;
  }
  const uintptr_t cur     = (uintptr_t)arena->base + (uintptr_t)arena->used;
  const uintptr_t mask    = (uintptr_t)align - 1U;
  const uintptr_t aligned = (cur + mask) & ~mask;
  const uintptr_t end     = (uintptr_t)arena->base + (uintptr_t)arena->size;
  if (aligned > end) {
    return k_ra8_err_no_mem;
  }
  if ((uintptr_t)bytes > (end - aligned)) {
    return k_ra8_err_no_mem;
  }
  *out_ptr    = (void*)aligned;
  arena->used = (uint32_t)((aligned + (uintptr_t)bytes) - (uintptr_t)arena->base);
  return k_ra8_ok;
}

ra8_err_t ra8_arena_remaining(const ra8_arena_t* arena, uint32_t* out_remaining)
{
  RA8_CHECK_NULL_PTR(arena, s_tag, "arena must not be nullptr");
  RA8_CHECK_NULL_PTR(out_remaining, s_tag, "out_remaining must not be nullptr");
  *out_remaining = arena->size - arena->used;
  return k_ra8_ok;
}
