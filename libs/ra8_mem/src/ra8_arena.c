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
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
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
RA8_INTERNAL static bool internal_is_pow2(uint32_t v)
{
  if (v == 0U) {
    return false;
  }
  return (v & (v - 1U)) == 0U;
}

/**
 * @brief Record a new occupancy if it exceeds the arena's peak.
 *
 * @details Called after every successful bump so the peak survives a rewind.
 *
 * @param[in,out] arena Initialised arena.
 *
 * @return void
 *
 * @pre @p arena is non-NULL.
 * @pre `arena->used` already reflects the completed carve.
 * @post `arena->high_water >= arena->used`.
 * @post No other field is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_note_high_water(ra8_arena_t* arena)
{
  if (arena->used > arena->high_water) {
    arena->high_water = arena->used;
  }
}

/**
 * @brief Validate one slot descriptor without touching the arena.
 *
 * @details Separated from the carve so a whole slot table can be rejected
 *          before the first pointer is published.
 *
 * @param[in] slot Slot descriptor to check.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               The slot is well formed.
 * @retval k_ra8_err_null_ptr     `slot->out_ptr` was NULL.
 * @retval k_ra8_err_invalid_size `slot->bytes` was zero.
 * @retval k_ra8_err_invalid_arg  `slot->align` was zero or not a power of two.
 *
 * @pre @p slot is non-NULL.
 * @pre No arena has been mutated by the caller yet.
 * @post No state is modified.
 * @post The result depends only on @p slot.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_check_slot(const ra8_arena_slot_t* slot)
{
  if (slot->out_ptr == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (slot->bytes == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (!internal_is_pow2(slot->align)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Prove a whole slot table fits, carving from a throw-away copy.
 *
 * @details The copy is what makes ::ra8_arena_carve_all atomic: the real arena
 *          is only advanced once every slot has already succeeded here.
 *
 * @param[in] arena      Initialised arena (read only).
 * @param[in] slots      Array of @p slot_count validated slot descriptors.
 * @param[in] slot_count Number of slots (1 .. ::k_ra8_arena_slot_cap).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok         Every slot fits the remainder in order.
 * @retval k_ra8_err_no_mem Some slot does not fit.
 *
 * @pre @p arena and @p slots are non-NULL.
 * @pre Every slot already passed ::internal_check_slot.
 * @post The caller's arena is not modified.
 * @post No `out_ptr` is written.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_probe_slots(const ra8_arena_t* arena, const ra8_arena_slot_t* slots, uint32_t slot_count)
{
  ra8_arena_t probe = *arena;
  for (uint32_t i = 0U; i < slot_count; ++i) {
    void*           scratch = nullptr;
    const ra8_err_t err     = ra8_arena_carve(&probe, slots[i].bytes, slots[i].align, &scratch);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}


ra8_err_t ra8_arena_init(ra8_arena_t* arena, void* base, uint32_t size)
{
  RA8_CHECK_NULL_PTR(arena, s_tag, "arena must not be nullptr");
  RA8_CHECK_NULL_PTR(base, s_tag, "base must not be nullptr");
  if (size == 0U) {
    return k_ra8_err_invalid_size;
  }
  arena->base       = (uint8_t*)base;
  arena->size       = size;
  arena->used       = 0U;
  arena->high_water = 0U;
  return k_ra8_ok;
}

ra8_err_t ra8_arena_carve(ra8_arena_t* arena, uint32_t bytes, uint32_t align, void** out_ptr)
{
  RA8_CHECK_NULL_PTR(arena, s_tag, "arena must not be nullptr");
  RA8_CHECK_NULL_PTR(out_ptr, s_tag, "out_ptr must not be nullptr");
  if (bytes == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (!internal_is_pow2(align)) {
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
  internal_note_high_water(arena);
  return k_ra8_ok;
}

ra8_err_t ra8_arena_remaining(const ra8_arena_t* arena, uint32_t* out_remaining)
{
  RA8_CHECK_NULL_PTR(arena, s_tag, "arena must not be nullptr");
  RA8_CHECK_NULL_PTR(out_remaining, s_tag, "out_remaining must not be nullptr");
  *out_remaining = arena->size - arena->used;
  return k_ra8_ok;
}

ra8_err_t
ra8_arena_carve_all(ra8_arena_t* arena, const ra8_arena_slot_t* slots, uint32_t slot_count)
{
  RA8_CHECK_NULL_PTR(arena, s_tag, "arena must not be nullptr");
  RA8_CHECK_NULL_PTR(slots, s_tag, "slots must not be nullptr");
  if ((slot_count == 0U) || (slot_count > (uint32_t)k_ra8_arena_slot_cap)) {
    return k_ra8_err_invalid_arg;
  }
  for (uint32_t i = 0U; i < slot_count; ++i) {
    const ra8_err_t err = internal_check_slot(&slots[i]);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  const ra8_err_t fits = internal_probe_slots(arena, slots, slot_count);
  if (fits != k_ra8_ok) {
    return fits;
  }
  for (uint32_t i = 0U; i < slot_count; ++i) {
    (void)ra8_arena_carve(arena, slots[i].bytes, slots[i].align, slots[i].out_ptr);
  }
  return k_ra8_ok;
}

ra8_err_t ra8_arena_high_water(const ra8_arena_t* arena, uint32_t* out_high_water)
{
  RA8_CHECK_NULL_PTR(arena, s_tag, "arena must not be nullptr");
  RA8_CHECK_NULL_PTR(out_high_water, s_tag, "out_high_water must not be nullptr");
  *out_high_water = arena->high_water;
  return k_ra8_ok;
}

ra8_err_t ra8_arena_reset(ra8_arena_t* arena)
{
  RA8_CHECK_NULL_PTR(arena, s_tag, "arena must not be nullptr");
  arena->used = 0U;
  return k_ra8_ok;
}
