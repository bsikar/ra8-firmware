/**
 * @file ra8_c6link_arena.c
 * @brief The fixed decode arena that lets a protobuf codec run with no heap.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * `rpc__unpack()` allocates: one block for the message, one per nested message,
 * one per repeated field and one per binary field. This firmware has no heap --
 * `_sbrk` is a strong symbol that reports a fatal error -- so the codec is
 * handed an allocator over a caller-supplied buffer instead.
 *
 * A bump allocator is the right shape for it. Every allocation a decode makes
 * is released by the matching `rpc__free_unpacked()`, and the RPC layer empties
 * the arena immediately afterwards, so the peak requirement is one message
 * rather than one run and no fragmentation can accumulate. That is what keeps
 * the whole control plane inside NASA Power of 10 Rule 3: the only storage
 * decision is the caller's single `= {}` array, made before the link opens.
 *
 * The `free` row is not a no-op. protobuf-c unwinds its own partial work when a
 * decode fails part-way, and those releases are strictly newest-first, so
 * rolling the bump offset back when the freed block is the newest one reclaims
 * exactly the space a failed decode would otherwise strand.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_c6link.h"
#include "ra8_c6link_internal.h"

/**
 * @enum ra8_c6link_arena_t
 * @brief Alignment the arena hands out blocks at.
 *
 * @details
 * protobuf-c stores `uint64_t` fields (`WifiInitConfig::feature_caps` is one)
 * inside blocks it gets from this allocator, so eight-byte alignment is a
 * correctness requirement on the target and not a performance choice.
 *
 * @invariant ::k_ra8_c6link_arena_align is a power of two, so the rounding
 *            mask below is exact.
 * @invariant Every returned block is aligned to it.
 *
 * @par Example:
 * @code
 * const uint32_t rounded = (want + k_ra8_c6link_arena_align - 1U) & ~mask;
 * @endcode
 *
 * @see priv_c6link_arena_alloc
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_c6link_arena_align = 8U,
  /**< Alignment, in bytes, of every block the arena hands out. */
  k_ra8_c6link_arena_mask = 7U,
  /**< ::k_ra8_c6link_arena_align minus one, as a rounding mask. */
} ra8_c6link_arena_t;

RA8_PRIV void* priv_c6link_arena_alloc(void* ctx, size_t size)
{
  ra8_c6link_t* link = (ra8_c6link_t*)ctx;
  if ((link == nullptr) || (link->arena == nullptr)) {
    return nullptr;
  }

  /* `arena_used` never exceeds `arena_bytes`, so this cannot underflow, and
     every later comparison stays inside the remaining space -- which is what
     lets the rounding below be checked without an overflow test that no
     reachable input could ever make true. */
  const uint32_t avail = link->arena_bytes - link->arena_used;
  if (size > (size_t)avail) {
    return nullptr;
  }
  const uint32_t want = (uint32_t)size;
  const uint32_t pad  = (uint32_t)((0U - want) & (uint32_t)k_ra8_c6link_arena_mask);
  if (pad > (avail - want)) {
    return nullptr;
  }

  const uint32_t at = link->arena_used;
  link->arena_used  = at + want + pad;
  link->arena_last  = at + 1U;
  return &link->arena[at];
}

RA8_PRIV void priv_c6link_arena_free(void* ctx, void* pointer)
{
  ra8_c6link_t* link = (ra8_c6link_t*)ctx;
  if ((link == nullptr) || (pointer == nullptr) || (link->arena_last == 0U)) {
    return;
  }
  const uint32_t at = link->arena_last - 1U;
  if (pointer == &link->arena[at]) {
    link->arena_used = at;
    link->arena_last = 0U;
  }
}

RA8_PRIV void priv_c6link_arena_reset(ra8_c6link_t* link)
{
  if (link == nullptr) {
    return;
  }
  link->arena_used = 0U;
  link->arena_last = 0U;
}

RA8_PRIV void priv_c6link_arena_bind(ProtobufCAllocator* out, ra8_c6link_t* link)
{
  if ((out == nullptr) || (link == nullptr)) {
    return;
  }
  out->alloc          = priv_c6link_arena_alloc;
  out->free           = priv_c6link_arena_free;
  out->allocator_data = link;
}
