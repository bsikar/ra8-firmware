/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_io_blockdev_cache.c
 * @brief Caching block device -- LRU sector cache + write-through over a backend.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Reads check a caller-owned sector cache and fill it on a miss (evicting the
 * least-recently-used slot); writes go straight through to the wrapped backend
 * and update the cache. Capabilities and sync delegate to the backend. Every
 * predicate is a single condition, so no MC/DC vectors are due.
 */

#include "ra8_io_blockdev_cache.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_internal.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_blockdev_cache";

/**
 * @brief Find the cache slot holding `lba`.
 *
 * @details Linear scan over valid slots; two single-condition checks avoid a
 *          compound decision.
 *
 * @param[in] st  Cache state.
 * @param[in] lba Logical block address to look up.
 *
 * @return uint32_t Slot index, or `st->n_slots` when not cached.
 * @retval st->n_slots The block is not in the cache.
 *
 * @pre `st` is a populated cache state.
 * @pre `st->slots` holds `st->n_slots` entries.
 * @post No state is mutated.
 * @post The return is `st->n_slots` iff `lba` is uncached.
 *
 * @note Not thread-safe with respect to the cache.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t cache_find(const ra8_io_blockdev_cache_state_t* st, uint32_t lba)
{
  for (uint32_t i = 0; i < st->n_slots; ++i) {
    if (!st->slots[i].valid) {
      continue;
    }
    if (st->slots[i].lba == lba) {
      return i;
    }
  }
  return st->n_slots;
}

/**
 * @brief Pick a slot to (re)use: the first free slot, else least-recently-used.
 *
 * @details A free slot wins immediately; otherwise the slot with the smallest
 *          `last_use` stamp is evicted.
 *
 * @param[in] st Cache state.
 *
 * @return uint32_t Index of the slot to use.
 * @retval <st->n_slots Always a valid slot index.
 *
 * @pre `st` has at least one slot.
 * @pre `st->slots` holds `st->n_slots` entries.
 * @post No state is mutated.
 * @post The returned slot is a free slot or the LRU victim.
 *
 * @note Not thread-safe with respect to the cache.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t cache_pick_victim(const ra8_io_blockdev_cache_state_t* st)
{
  uint32_t best = 0;
  for (uint32_t i = 0; i < st->n_slots; ++i) {
    if (!st->slots[i].valid) {
      return i;
    }
    if (st->slots[i].last_use < st->slots[best].last_use) {
      best = i;
    }
  }
  return best;
}

/**
 * @brief Read one block through the cache (hit) or fill it (miss).
 *
 * @details On a hit, copies from the cache and bumps the LRU stamp; on a miss,
 *          reads one block from the backend into the victim slot, records it,
 *          and copies it out. Updates the hit/miss counters.
 *
 * @param[in]  st  Cache state.
 * @param[in]  lba Logical block address.
 * @param[out] dst Destination of one 512-byte block.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    Block delivered into `dst`.
 * @retval k_ra8_err_* Propagated from the backend read.
 *
 * @pre `st` is a populated cache state.
 * @pre `dst` is writable for one block.
 * @post On success `dst` holds the block and the cache reflects it.
 * @post Exactly one of `hits`/`misses` is incremented.
 *
 * @note Not thread-safe with respect to the cache.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t cache_read_block(ra8_io_blockdev_cache_state_t* st, uint32_t lba, uint8_t* dst)
{
  st->clock++;
  const uint32_t hit  = cache_find(st, lba);
  const size_t   span = (size_t)k_ra8_io_block_size_bytes;
  if (hit != st->n_slots) {
    st->hits++;
    (void)memcpy(dst, &st->data[(size_t)hit * span], span);
    st->slots[hit].last_use = st->clock;
    return k_ra8_ok;
  }
  st->misses++;
  const uint32_t v = cache_pick_victim(st);
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_read(st->under, lba, 1, &st->data[(size_t)v * span]),
                      s_tag,
                      "backend read");
  st->slots[v].lba      = lba;
  st->slots[v].valid    = true;
  st->slots[v].last_use = st->clock;
  (void)memcpy(dst, &st->data[(size_t)v * span], span);
  return k_ra8_ok;
}

/**
 * @brief Write one block through to the backend and update the cache.
 *
 * @details Write-through: the backend is written first, then the cache slot for
 *          the block is inserted or refreshed so a following read hits.
 *
 * @param[in] st  Cache state.
 * @param[in] lba Logical block address.
 * @param[in] src Source of one 512-byte block.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    Block committed and cached.
 * @retval k_ra8_err_* Propagated from the backend write.
 *
 * @pre `st` is a populated cache state.
 * @pre `src` is readable for one block.
 * @post On success the backend and the cache both hold the block.
 * @post On a backend error the cache is left unchanged for this block.
 *
 * @note Not thread-safe with respect to the cache.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
cache_write_block(ra8_io_blockdev_cache_state_t* st, uint32_t lba, const uint8_t* src)
{
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_write(st->under, lba, 1, src), s_tag, "backend write");
  st->clock++;
  const size_t span = (size_t)k_ra8_io_block_size_bytes;
  uint32_t     idx  = cache_find(st, lba);
  if (idx == st->n_slots) {
    idx = cache_pick_victim(st);
  }
  (void)memcpy(&st->data[(size_t)idx * span], src, span);
  st->slots[idx].lba      = lba;
  st->slots[idx].valid    = true;
  st->slots[idx].last_use = st->clock;
  return k_ra8_ok;
}

/**
 * @brief Cache vtable: read `count` blocks at `lba` into `buf`.
 *
 * @details Iterates the range one block at a time through ::cache_read_block.
 *
 * @param[in]  ctx   Cache state (as a void cookie).
 * @param[in]  lba   First logical block address.
 * @param[in]  count Number of blocks to read.
 * @param[out] buf   Destination buffer.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Blocks read.
 * @retval k_ra8_err_null_ptr `ctx` or `buf` was NULL.
 * @retval k_ra8_err_*        Propagated from the backend read.
 *
 * @pre `ctx` is a populated cache state.
 * @pre `buf` is writable for `count * 512` bytes.
 * @post On success `buf` holds the requested blocks.
 * @post The cache reflects every block touched.
 *
 * @note Not thread-safe with respect to the cache.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t cache_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  ra8_io_blockdev_cache_state_t* st   = (ra8_io_blockdev_cache_state_t*)ctx;
  const size_t                   span = (size_t)k_ra8_io_block_size_bytes;
  for (uint32_t i = 0; i < count; ++i) {
    RA8_RETURN_ON_ERROR(cache_read_block(st, lba + i, &buf[(size_t)i * span]), s_tag, "read block");
  }
  return k_ra8_ok;
}

/**
 * @brief Cache vtable: write `count` blocks from `buf` at `lba`.
 *
 * @details Iterates the range one block at a time through ::cache_write_block.
 *
 * @param[in] ctx   Cache state (as a void cookie).
 * @param[in] lba   First logical block address.
 * @param[in] count Number of blocks to write.
 * @param[in] buf   Source buffer.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Blocks written through.
 * @retval k_ra8_err_null_ptr `ctx` or `buf` was NULL.
 * @retval k_ra8_err_*        Propagated from the backend write.
 *
 * @pre `ctx` is a populated cache state.
 * @pre `buf` is readable for `count * 512` bytes.
 * @post On success the backend holds the blocks and the cache reflects them.
 * @post On a backend error earlier blocks may already be committed.
 *
 * @note Not thread-safe with respect to the cache.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t cache_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  ra8_io_blockdev_cache_state_t* st   = (ra8_io_blockdev_cache_state_t*)ctx;
  const size_t                   span = (size_t)k_ra8_io_block_size_bytes;
  for (uint32_t i = 0; i < count; ++i) {
    RA8_RETURN_ON_ERROR(cache_write_block(st, lba + i, &buf[(size_t)i * span]),
                        s_tag,
                        "write block");
  }
  return k_ra8_ok;
}

/**
 * @brief Cache vtable: erase through to the backend and drop stale slots.
 *
 * @details Delegates the erase, then invalidates any cached slot whose block
 *          lies in the erased range (three single-condition guards).
 *
 * @param[in] ctx   Cache state (as a void cookie).
 * @param[in] lba   First logical block address.
 * @param[in] count Number of blocks to erase.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Range erased and stale slots dropped.
 * @retval k_ra8_err_null_ptr `ctx` was NULL.
 * @retval k_ra8_err_*        Propagated from the backend erase.
 *
 * @pre `ctx` is a populated cache state.
 * @pre The range fits the backend capacity.
 * @post On success no cached slot covers the erased range.
 * @post On a backend error the cache is left unchanged.
 *
 * @note Not thread-safe with respect to the cache.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t cache_erase(void* ctx, uint32_t lba, uint32_t count)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  ra8_io_blockdev_cache_state_t* st = (ra8_io_blockdev_cache_state_t*)ctx;
  RA8_RETURN_ON_ERROR(ra8_io_blockdev_erase(st->under, lba, count), s_tag, "backend erase");
  const uint32_t end = lba + count;
  for (uint32_t i = 0; i < st->n_slots; ++i) {
    if (!st->slots[i].valid) {
      continue;
    }
    if (st->slots[i].lba < lba) {
      continue;
    }
    if (st->slots[i].lba >= end) {
      continue;
    }
    st->slots[i].valid = false;
  }
  return k_ra8_ok;
}

/**
 * @brief Cache vtable: report the wrapped backend's capabilities.
 *
 * @details The cache does not change the medium's geometry, so it forwards.
 *
 * @param[in]  ctx Cache state (as a const void cookie).
 * @param[out] out Capabilities snapshot.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           `*out` populated from the backend.
 * @retval k_ra8_err_null_ptr `ctx` or `out` was NULL.
 * @retval k_ra8_err_*        Propagated from the backend.
 *
 * @pre `ctx` is a populated cache state.
 * @pre `out` is writable.
 * @post On success `*out` mirrors the wrapped backend.
 * @post No cache state is mutated.
 *
 * @note Thread-safe (pure read of the backend).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t cache_get_caps(const void* ctx, ra8_io_blockdev_caps_t* out)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  const ra8_io_blockdev_cache_state_t* st = (const ra8_io_blockdev_cache_state_t*)ctx;
  return ra8_io_blockdev_get_caps(st->under, out);
}

/**
 * @brief Cache vtable: flush the wrapped backend.
 *
 * @details Writes are write-through so nothing is buffered here; the call
 *          forwards so a backend with its own buffering can commit.
 *
 * @param[in] ctx Cache state (as a void cookie).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Backend flushed.
 * @retval k_ra8_err_null_ptr `ctx` was NULL.
 * @retval k_ra8_err_*        Propagated from the backend sync.
 *
 * @pre `ctx` is a populated cache state.
 * @pre The cache is idle.
 * @post On success the backend reflects every prior write.
 * @post No cache state is mutated.
 *
 * @note Not thread-safe with respect to the cache.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t cache_sync(void* ctx)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  const ra8_io_blockdev_cache_state_t* st = (const ra8_io_blockdev_cache_state_t*)ctx;
  return ra8_io_blockdev_sync(st->under);
}

/** @brief Caching block-device vtable. */
static const ra8_io_blockdev_iface_t k_cache_iface = {
  .read     = cache_read,
  .write    = cache_write,
  .erase    = cache_erase,
  .get_caps = cache_get_caps,
  .sync     = cache_sync,
};

/**
 * @brief Populate the scalar fields of a fresh cache state.
 *
 * @details Records the caller-owned backend, data buffer, and slot array on
 *          `state` and zeroes the monotonic clock and the hit/miss counters.
 *          The slot array itself is reset separately by ::cache_reset_slots.
 *
 * @param[out] state   Cache state to populate (already null-checked).
 * @param[in]  under   Wrapped backend (must out-live the cache).
 * @param[in]  data    Cache data buffer of `n_slots * 512` bytes.
 * @param[in]  slots   Cache metadata array of `n_slots` entries.
 * @param[in]  n_slots Number of cached sectors (>= 1).
 *
 * @return void
 *
 * @pre `state` is non-NULL and writable.
 * @pre `n_slots` is non-zero.
 * @post `state` references `under`, `data`, and `slots`.
 * @post `state->clock`, `state->hits`, and `state->misses` are zero.
 *
 * @note Not thread-safe with respect to the cache.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void cache_state_init(ra8_io_blockdev_cache_state_t* state,
                             const ra8_io_blockdev_t*       under,
                             uint8_t*                       data,
                             ra8_io_blockdev_cache_slot_t*  slots,
                             uint32_t                       n_slots)
{
  state->under   = under;
  state->data    = data;
  state->slots   = slots;
  state->n_slots = n_slots;
  state->clock   = 0;
  state->hits    = 0;
  state->misses  = 0;
}

/**
 * @brief Reset a slot array to the empty (no sectors cached) state.
 *
 * @details Marks every slot invalid and clears its LBA and LRU stamp so the
 *          first access fills a free slot rather than evicting stale data.
 *
 * @param[out] slots   Cache metadata array of `n_slots` entries.
 * @param[in]  n_slots Number of slots to reset (>= 1).
 *
 * @return void
 *
 * @pre `slots` is non-NULL and holds `n_slots` entries.
 * @pre `n_slots` is non-zero.
 * @post Every slot in `[0, n_slots)` is invalid.
 * @post Every slot's `lba` and `last_use` are zero.
 *
 * @note Not thread-safe with respect to the cache.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void cache_reset_slots(ra8_io_blockdev_cache_slot_t* slots, uint32_t n_slots)
{
  for (uint32_t i = 0; i < n_slots; ++i) {
    slots[i].valid    = false;
    slots[i].lba      = 0;
    slots[i].last_use = 0;
  }
}

ra8_err_t ra8_io_blockdev_cache_init(ra8_io_blockdev_t*             bd,
                                     ra8_io_blockdev_cache_state_t* state,
                                     const ra8_io_blockdev_t*       under,
                                     uint8_t*                       data,
                                     ra8_io_blockdev_cache_slot_t*  slots,
                                     uint32_t                       n_slots)
{
  RA8_CHECK_NULL_PTR(bd, s_tag, "bd must not be nullptr");
  RA8_CHECK_NULL_PTR(state, s_tag, "state must not be nullptr");
  RA8_CHECK_NULL_PTR(under, s_tag, "under must not be nullptr");
  RA8_CHECK_NULL_PTR(data, s_tag, "data must not be nullptr");
  RA8_CHECK_NULL_PTR(slots, s_tag, "slots must not be nullptr");
  if (n_slots == 0U) {
    return k_ra8_err_invalid_size;
  }
  cache_state_init(state, under, data, slots, n_slots);
  cache_reset_slots(slots, n_slots);
  bd->iface = &k_cache_iface;
  bd->ctx   = state;
  return k_ra8_ok;
}

ra8_err_t ra8_io_blockdev_cache_stats(const ra8_io_blockdev_cache_state_t* state,
                                      uint32_t*                            out_hits,
                                      uint32_t*                            out_misses)
{
  RA8_CHECK_NULL_PTR(state, s_tag, "state must not be nullptr");
  if (out_hits != nullptr) {
    *out_hits = state->hits;
  }
  if (out_misses != nullptr) {
    *out_misses = state->misses;
  }
  return k_ra8_ok;
}
