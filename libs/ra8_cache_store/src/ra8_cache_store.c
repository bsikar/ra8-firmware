/**
 * @file ra8_cache_store.c
 * @brief ra8_cache_store runtime path: put / get / read / evict / pin / checkpoint
 *        (#201).
 *
 * @par Tag
 * [Ring 4 / Storage] {World: NS}
 *
 * @details
 * The steady-state operations on a mounted store. Appends are log-structured
 * (payload sectors first, header last) so a torn put leaves a reclaimable tail,
 * never a visible corrupt entry. Reads stream a sector at a time through the
 * store staging buffer so a cached blob never needs full residency. Eviction
 * only releases sectors -- write-once entries are always re-derivable, so
 * nothing is ever written back. The mount / recovery path lives in
 * `ra8_cache_store_mount.c`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_cache_store.h"

#include <stdint.h>
#include <string.h>

#include "lx_api.h"
#include "ra8_cache_store_internal.h"
#include "ra8_check.h"
#include "ra8_err.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_cache_store";

/**
 * @enum ra8_cs_rt_const_t
 * @brief Runtime sizing limits.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_cs_max_run = 0xFFFFU, /**< Max run length (fits ::ra8_cs_entry_hdr_t sector_count). */
} ra8_cs_rt_const_t;

/* ------------------------------------------------------------------------- */
/* Allocation + append helpers */
/* ------------------------------------------------------------------------- */

/**
 * @brief Count the in-use index slots.
 * @details Linear scan of the caller-owned index array counting the in-use flag.
 * @param[in] store Store to scan.
 * @return Number of live entries (0 when @p store is unusable).
 * @retval 0 No live entries, or @p store / its index is NULL.
 * @pre @p store is the store under test.
 * @pre `store->index` covers `store->index_cap` slots.
 * @post @p store is unmodified.
 * @post The result is <= `store->index_cap`.
 * @note Not thread-safe; the store serialises access.
 * @since 0.1.0
 */
static uint16_t cs_index_used(const ra8_cache_store_t* store)
{
  if (store == nullptr) {
    return 0U;
  }
  if (store->index == nullptr) {
    return 0U;
  }
  uint16_t used = 0U;
  for (uint16_t i = 0U; i < store->index_cap; i++) {
    if ((store->index[i].flags & (uint8_t)k_ra8_cache_store_flag_in_use) != 0U) {
      used++;
    }
  }
  return used;
}

/**
 * @brief True when `[start, start+count)` overlaps no in-use entry's run.
 * @details Nested single-condition overlap test (no compound decision).
 * @param[in] store Store whose live runs are checked.
 * @param[in] start Candidate run start sector.
 * @param[in] count Candidate run length.
 * @return Whether the candidate run is entirely free.
 * @retval true  No live entry overlaps the candidate.
 * @retval false Some live entry overlaps it.
 * @pre @p store and its index are non-NULL.
 * @pre `count >= 1`.
 * @post @p store is unmodified.
 * @post A `true` result means the run may be written.
 * @note Thread-safe with respect to a quiescent store (pure read).
 * @since 0.1.0
 */
static bool cs_run_free(const ra8_cache_store_t* store, uint32_t start, uint32_t count)
{
  if (store == nullptr) {
    return false;
  }
  if (store->index == nullptr) {
    return false;
  }
  uint32_t end = start + count;
  for (uint16_t i = 0U; i < store->index_cap; i++) {
    const ra8_cache_store_entry_t* e = &store->index[i];
    if ((e->flags & (uint8_t)k_ra8_cache_store_flag_in_use) == 0U) {
      continue;
    }
    uint32_t es = e->start_sector;
    uint32_t ee = es + e->sector_count;
    if (start < ee) {
      if (es < end) {
        return false;
      }
    }
  }
  return true;
}

/**
 * @brief First-fit a free contiguous run of @p count sectors in the log region.
 * @details First-fit linear scan from `log_start`; bounded by the logical span (O(sectors * index_cap)).
 * @param[in]  store     Store to allocate within.
 * @param[in]  count     Run length (header + payload sectors).
 * @param[out] out_start Receives the run's first sector.
 * @return Error code.
 * @retval k_ra8_ok               A free run was found.
 * @retval k_ra8_err_null_ptr     `store` or `out_start` NULL.
 * @retval k_ra8_err_invalid_size `count` is zero.
 * @retval k_ra8_err_no_mem       No free run of that length exists.
 * @pre `store->log_start < store->logical_sectors`.
 * @pre @p out_start is writable.
 * @post On `k_ra8_ok`, `[*out_start, *out_start+count)` is free.
 * @post On error @p out_start is untouched.
 * @note Not thread-safe; the store serialises access.
 * @since 0.1.0
 */
static ra8_err_t cs_alloc_run(const ra8_cache_store_t* store, uint32_t count, uint32_t* out_start)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_CHECK_NULL_PTR(out_start, s_tag, "out_start");
  if (count == 0U) {
    return k_ra8_err_invalid_size;
  }
  for (uint32_t start = store->log_start; (start + count) <= store->logical_sectors; start++) {
    if (cs_run_free(store, start, count)) {
      *out_start = start;
      return k_ra8_ok;
    }
  }
  return k_ra8_err_no_mem;
}

/**
 * @brief Write one entry: payload sectors first, then the header (atomic commit).
 * @details Fills the staging buffer per payload sector, then writes the header last so its presence certifies a complete run.
 * @param[in,out] store Store to append into.
 * @param[in]     start Run start (header) sector.
 * @param[in]     seq   Append sequence number.
 * @param[in]     key   Content key.
 * @param[in]     data  Payload bytes.
 * @param[in]     len   Payload length.
 * @param[in]     count Run length (header + payload).
 * @return Error code.
 * @retval k_ra8_ok                Entry fully written.
 * @retval k_ra8_err_null_ptr      `store` or `data` NULL.
 * @retval k_ra8_err_hw_init_failed LevelX write failed mid-run.
 * @pre `[start, start+count)` was allocated free.
 * @pre `store->staging` covers one sector.
 * @post On `k_ra8_ok` the header at `start` certifies a complete run.
 * @post On a mid-run failure no header is written (nothing is findable).
 * @note Not thread-safe; shares the store staging buffer.
 * @since 0.1.0
 */
static ra8_err_t cs_write_entry(ra8_cache_store_t* store,
                                uint32_t           start,
                                uint32_t           seq,
                                uint32_t           key,
                                const uint8_t*     data,
                                uint32_t           len,
                                uint16_t           count)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_CHECK_NULL_PTR(data, s_tag, "data");
  uint16_t       data_sectors = (uint16_t)(count - 1U);
  uint32_t       off          = 0U;
  const uint32_t sec_sz       = (uint32_t)k_ra8_cache_store_sector_bytes;
  for (uint16_t i = 0U; i < data_sectors; i++) {
    uint32_t remain = len - off;
    uint32_t chunk  = (remain < sec_sz) ? remain : sec_sz;
    (void)memset(store->staging, 0, (size_t)sec_sz);
    (void)memcpy(store->staging, &data[off], (size_t)chunk);
    RA8_RETURN_ON_ERROR(ra8_cs_sector_write(store, start + 1U + (uint32_t)i, store->staging),
                        s_tag,
                        "payload");
    off += chunk;
  }
  ra8_cs_entry_hdr_t h = {.magic        = (uint32_t)k_ra8_cs_entry_magic,
                          .seq          = seq,
                          .key          = key,
                          .byte_len     = len,
                          .start_sector = start,
                          .sector_count = count,
                          .flags        = 0U,
                          .hdr_crc      = 0U};
  h.hdr_crc = ra8_cs_crc32((const uint8_t*)&h, (uint32_t)(sizeof(h) - sizeof(h.hdr_crc)));
  (void)memset(store->staging, 0, (size_t)sec_sz);
  (void)memcpy(store->staging, &h, sizeof(h));
  return ra8_cs_sector_write(store, start, store->staging);
}

/**
 * @brief Stamp a dirty superblock before mutating (unless already dirty).
 * @details Guarantees a clean checkpoint is invalidated before any log/dir
 *          mutation, so a crash falls back to a safe log replay.
 * @param[in,out] store Store to mark.
 * @return Error code.
 * @retval k_ra8_ok                Sector 0 now reflects a dirty session.
 * @retval k_ra8_err_null_ptr      `store` NULL.
 * @retval k_ra8_err_not_initialized Store not initialised.
 * @retval k_ra8_err_hw_init_failed LevelX write failed.
 * @pre `store->inited` is true.
 * @pre `store->staging` covers one sector.
 * @post On `k_ra8_ok`, `store->flash_state == dirty`.
 * @post A later mount will replay the log rather than trust a stale checkpoint.
 * @note Not thread-safe; the store serialises access.
 * @since 0.1.0
 */
static ra8_err_t cs_mark_dirty(ra8_cache_store_t* store)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_VALIDATE_INIT(store->inited, s_tag, "store");
  if (store->flash_state == (uint8_t)k_ra8_cs_dirty) {
    return k_ra8_ok;
  }
  RA8_RETURN_ON_ERROR(ra8_cs_super_write(store, (uint32_t)k_ra8_cs_dirty), s_tag, "mark dirty");
  store->flash_state = (uint8_t)k_ra8_cs_dirty;
  return k_ra8_ok;
}

/**
 * @brief Save the directory + a clean superblock (the checkpoint commit).
 * @details Marks the superblock dirty first so the directory is only ever rewritten while a crash would safely replay, then writes the directory and a clean superblock.
 * @param[in,out] store Store to checkpoint.
 * @return Error code.
 * @retval k_ra8_ok                Checkpoint committed clean.
 * @retval k_ra8_err_null_ptr      `store` NULL.
 * @retval k_ra8_err_not_initialized Store not initialised.
 * @retval k_ra8_err_hw_init_failed LevelX write failed.
 * @pre `store->inited` is true.
 * @pre The index reflects the intended live set.
 * @post On `k_ra8_ok`, `store->flash_state == clean` and the directory is current.
 * @post The dirty marker precedes the directory rewrite (crash-safe ordering).
 * @note Not thread-safe; the store serialises access.
 * @since 0.1.0
 */
static ra8_err_t cs_checkpoint(ra8_cache_store_t* store)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_VALIDATE_INIT(store->inited, s_tag, "store");
  RA8_RETURN_ON_ERROR(cs_mark_dirty(store), s_tag, "pre-dirty");
  uint32_t count = 0U;
  RA8_RETURN_ON_ERROR(ra8_cs_dir_save(store, &count), s_tag, "dir save");
  RA8_RETURN_ON_ERROR(ra8_cs_super_write(store, (uint32_t)k_ra8_cs_clean), s_tag, "clean super");
  store->flash_state = (uint8_t)k_ra8_cs_clean;
  return k_ra8_ok;
}

/**
 * @brief Validate a put request and compute its run length.
 * @details Runs every put precondition (args, init, size, duplicate key, index
 *          and budget capacity) and returns the total run length so the caller
 *          keeps a small, single-purpose body.
 * @param[in]  store     Store to put into.
 * @param[in]  key       Content key.
 * @param[in]  data      Payload bytes.
 * @param[in]  len       Payload length.
 * @param[out] out_count Receives the run length (header + payload sectors).
 * @return Error code.
 * @retval k_ra8_ok                 Request is valid; `*out_count` set.
 * @retval k_ra8_err_null_ptr       `store` or `data` NULL.
 * @retval k_ra8_err_not_initialized Store not initialised.
 * @retval k_ra8_err_invalid_size   `len` zero or run longer than the header field.
 * @retval k_ra8_err_exists         `key` already present (write-once).
 * @retval k_ra8_err_no_mem         Index full or budget exhausted.
 * @pre `store->inited` is true.
 * @pre @p out_count is writable.
 * @post On `k_ra8_ok`, `*out_count >= 2` (header + at least one payload sector).
 * @post No store state is modified.
 * @note Not thread-safe; the store serialises access.
 * @since 0.1.0
 */
static ra8_err_t cs_put_check(ra8_cache_store_t* store,
                              uint32_t           key,
                              const uint8_t*     data,
                              uint32_t           len,
                              uint32_t*          out_count)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_CHECK_NULL_PTR(data, s_tag, "data");
  RA8_VALIDATE_INIT(store->inited, s_tag, "store");
  if (len == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (ra8_cs_index_find(store, key) >= 0) {
    return k_ra8_err_exists;
  }
  if (cs_index_used(store) >= store->index_cap) {
    return k_ra8_err_no_mem;
  }
  const uint32_t sec_sz = (uint32_t)k_ra8_cache_store_sector_bytes;
  uint32_t       count  = 1U + ((len + (sec_sz - 1U)) / sec_sz);
  if (count > (uint32_t)k_ra8_cs_max_run) {
    return k_ra8_err_invalid_size;
  }
  if ((store->live_sectors + count) > store->data_capacity) {
    return k_ra8_err_no_mem;
  }
  *out_count = count;
  return k_ra8_ok;
}

/**
 * @brief Copy the payload slice that starts at @p byte_pos, within one sector.
 * @details Reads the covering logical sector into staging and copies at most the
 *          bytes remaining in that sector, so the caller loops sector by sector.
 * @param[in]  store      Store whose flash is read.
 * @param[in]  data_start First payload logical sector of the entry.
 * @param[in]  byte_pos   Byte offset within the payload.
 * @param[out] dst        Destination for the copied bytes.
 * @param[in]  max        Upper bound on bytes to copy this call.
 * @param[out] out_copied Receives the number of bytes copied (>= 1).
 * @return Error code.
 * @retval k_ra8_ok                 Slice copied.
 * @retval k_ra8_err_null_ptr       `store` or `dst` NULL.
 * @retval k_ra8_err_hw_init_failed LevelX read failed.
 * @pre @p max >= 1 and @p dst covers @p max bytes.
 * @pre `store->staging` covers one sector.
 * @post On `k_ra8_ok`, `1 <= *out_copied <= max`.
 * @post On error @p dst content is unspecified.
 * @note Not thread-safe; shares the store staging buffer.
 * @since 0.1.0
 */
static ra8_err_t cs_read_at(const ra8_cache_store_t* store,
                            uint32_t                 data_start,
                            uint64_t                 byte_pos,
                            uint8_t*                 dst,
                            uint32_t                 max,
                            uint32_t*                out_copied)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_CHECK_NULL_PTR(dst, s_tag, "dst");
  const uint32_t sec_sz = (uint32_t)k_ra8_cache_store_sector_bytes;
  uint32_t       sector = data_start + (uint32_t)(byte_pos / sec_sz);
  uint32_t       soff   = (uint32_t)(byte_pos % sec_sz);
  uint32_t       space  = sec_sz - soff;
  uint32_t       chunk  = (max < space) ? max : space;
  RA8_RETURN_ON_ERROR(ra8_cs_sector_read(store, sector, store->staging), s_tag, "read");
  (void)memcpy(dst, &store->staging[soff], (size_t)chunk);
  *out_copied = chunk;
  return k_ra8_ok;
}

/**
 * @brief Release every logical sector of a run back to the free pool.
 * @details Loops `[start, start+count)` through ::ra8_cs_sector_release so eviction
 *          reclaims the whole entry with no write-back.
 * @param[in,out] store Store whose flash is released.
 * @param[in]     start First sector of the run.
 * @param[in]     count Run length.
 * @return Error code.
 * @retval k_ra8_ok                 Run released.
 * @retval k_ra8_err_null_ptr       `store` NULL.
 * @retval k_ra8_err_not_initialized Store not initialised.
 * @retval k_ra8_err_hw_init_failed LevelX release failed.
 * @pre `store->inited` is true.
 * @pre `[start, start+count)` is the entry's run.
 * @post On `k_ra8_ok` every sector of the run is free.
 * @post On error some sectors may remain mapped (caller retries).
 * @note Not thread-safe; the store serialises access.
 * @since 0.1.0
 */
static ra8_err_t cs_release_run(ra8_cache_store_t* store, uint32_t start, uint16_t count)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_VALIDATE_INIT(store->inited, s_tag, "store");
  for (uint16_t i = 0U; i < count; i++) {
    RA8_RETURN_ON_ERROR(ra8_cs_sector_release(store, start + (uint32_t)i), s_tag, "release");
  }
  return k_ra8_ok;
}

/* ------------------------------------------------------------------------- */
/* Public API */
/* ------------------------------------------------------------------------- */

ra8_err_t
ra8_cache_store_put(ra8_cache_store_t* store, uint32_t key, const uint8_t* data, uint32_t len)
{
  uint32_t count = 0U;
  RA8_RETURN_ON_ERROR(cs_put_check(store, key, data, len, &count), s_tag, "check");
  RA8_RETURN_ON_ERROR(cs_mark_dirty(store), s_tag, "dirty");
  uint32_t start = 0U;
  RA8_RETURN_ON_ERROR(cs_alloc_run(store, count, &start), s_tag, "alloc");
  RA8_RETURN_ON_ERROR(
    cs_write_entry(store, start, store->next_seq, key, data, len, (uint16_t)count),
    s_tag,
    "write");
  (void)ra8_cs_index_add(store, key, start, (uint16_t)count, len, false);
  store->live_sectors += count;
  store->next_seq += 1U;
  return k_ra8_ok;
}

ra8_err_t ra8_cache_store_get(const ra8_cache_store_t*  store,
                              uint32_t                  key,
                              ra8_cache_store_reader_t* out_reader)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_CHECK_NULL_PTR(out_reader, s_tag, "out_reader");
  RA8_VALIDATE_INIT(store->inited, s_tag, "store");
  int32_t slot = ra8_cs_index_find(store, key);
  if (slot < 0) {
    return k_ra8_err_not_found;
  }
  const ra8_cache_store_entry_t* e = &store->index[slot];
  *out_reader = (ra8_cache_store_reader_t){.store        = store,
                                           .data_start   = e->start_sector + 1U,
                                           .data_sectors = (uint32_t)e->sector_count - 1U,
                                           .byte_len     = e->byte_len};
  return k_ra8_ok;
}

/**
 * @brief Stream @p len payload bytes at @p offset, sector by sector, into @p buf.
 * @details Loops ::cs_read_at until @p len bytes are copied; the iteration count
 *          is bounded by the entry's payload-sector span plus one.
 * @param[in]  store        Store whose flash is read.
 * @param[in]  data_start   First payload logical sector of the entry.
 * @param[in]  data_sectors Payload sector count (the loop bound).
 * @param[in]  offset       Byte offset within the payload.
 * @param[out] buf          Destination (`len` writable bytes).
 * @param[in]  len          Bytes to copy.
 * @return Error code.
 * @retval k_ra8_ok                 All bytes copied.
 * @retval k_ra8_err_null_ptr       `store` or `buf` NULL.
 * @retval k_ra8_err_hw_init_failed LevelX read failed.
 * @pre `offset + len` is within the entry payload (checked by the caller).
 * @pre `store->staging` covers one sector.
 * @post On `k_ra8_ok`, `buf[0..len)` holds the payload slice.
 * @post On error `buf` content is unspecified.
 * @note Not thread-safe; shares the store staging buffer.
 * @since 0.1.0
 */
static ra8_err_t cs_read_stream(const ra8_cache_store_t* store,
                                uint32_t                 data_start,
                                uint32_t                 data_sectors,
                                uint64_t                 offset,
                                uint8_t*                 buf,
                                uint32_t                 len)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf");
  uint32_t done     = 0U;
  uint32_t max_iter = data_sectors + 1U;
  for (uint32_t g = 0U; g <= max_iter; g++) {
    if (done >= len) {
      break;
    }
    uint32_t copied = 0U;
    RA8_RETURN_ON_ERROR(
      cs_read_at(store, data_start, offset + (uint64_t)done, &buf[done], len - done, &copied),
      s_tag,
      "at");
    done += copied;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_cache_store_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf");
  const ra8_cache_store_reader_t* r     = (const ra8_cache_store_reader_t*)ctx;
  const ra8_cache_store_t*        store = r->store;
  RA8_CHECK_NULL_PTR(store, s_tag, "reader store");
  if ((offset + (uint64_t)len) > (uint64_t)r->byte_len) {
    return k_ra8_err_out_of_range;
  }
  return cs_read_stream(store, r->data_start, r->data_sectors, offset, buf, len);
}

ra8_err_t ra8_cache_store_evict(ra8_cache_store_t* store, uint32_t key)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_VALIDATE_INIT(store->inited, s_tag, "store");
  int32_t slot = ra8_cs_index_find(store, key);
  if (slot < 0) {
    return k_ra8_err_not_found;
  }
  ra8_cache_store_entry_t* e = &store->index[slot];
  if ((e->flags & (uint8_t)k_ra8_cache_store_flag_pinned) != 0U) {
    return k_ra8_err_busy;
  }
  RA8_RETURN_ON_ERROR(cs_mark_dirty(store), s_tag, "dirty");
  RA8_RETURN_ON_ERROR(cs_release_run(store, e->start_sector, e->sector_count), s_tag, "release");
  store->live_sectors -= e->sector_count;
  *e = (ra8_cache_store_entry_t){};
  return k_ra8_ok;
}

ra8_err_t ra8_cache_store_pin(ra8_cache_store_t* store, uint32_t key, bool pin)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_VALIDATE_INIT(store->inited, s_tag, "store");
  int32_t slot = ra8_cs_index_find(store, key);
  if (slot < 0) {
    return k_ra8_err_not_found;
  }
  RA8_RETURN_ON_ERROR(cs_mark_dirty(store), s_tag, "dirty");
  ra8_cache_store_entry_t* e = &store->index[slot];
  if (pin) {
    e->flags |= (uint8_t)k_ra8_cache_store_flag_pinned;
  } else {
    e->flags &= (uint8_t)~(uint8_t)k_ra8_cache_store_flag_pinned;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_cache_store_sync(ra8_cache_store_t* store)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_VALIDATE_INIT(store->inited, s_tag, "store");
  return cs_checkpoint(store);
}

ra8_err_t ra8_cache_store_close(ra8_cache_store_t* store)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_VALIDATE_INIT(store->inited, s_tag, "store");
  RA8_RETURN_ON_ERROR(cs_checkpoint(store), s_tag, "checkpoint");
  (void)lx_nor_flash_close(store->flash);
  store->inited = false;
  return k_ra8_ok;
}
