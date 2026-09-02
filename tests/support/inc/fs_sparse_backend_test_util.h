/**
 * @file fs_sparse_backend_test_util.h
 * @brief Sparse (windowed) fake block device for huge-media `ra8_fs` tests.
 *
 * @details
 * The simulation rig behind the >4 GiB file (#676) and 4Kn / beyond-2-TiB
 * media (#683) test evidence. A flat RAM store cannot present a 6 GiB -- let
 * alone a 3 TiB -- device, so this backend keeps only the sectors that were
 * ever written NON-ZERO, in a small open-addressed hash table, and serves
 * every other sector as zeros:
 *
 *   - a WRITE whose payload is all zero bytes to a sector that was never
 *     stored is dropped (the sector already reads as zero), which is what
 *     makes formatting a multi-terabyte device affordable -- the formatter's
 *     region clears cost nothing;
 *   - a WRITE with content claims a slot keyed by the 64-bit LBA;
 *   - a READ returns the stored sector or zeros, after the same bounds check
 *     a real device would apply.
 *
 * The store also records the highest LBA ever written and read, so a test can
 * PROVE an access crossed the 32-bit-LBA line instead of assuming it did.
 *
 * A second shape, the WINDOW, presents a slice `[base, base + count)` of a
 * sparse device as a device of its own. Formatting a window puts a complete
 * volume at an arbitrary absolute offset -- how the beyond-2-TiB test plants
 * an exFAT volume past LBA 2^32 without hand-writing every on-disk structure.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"

/**
 * @enum fs_sparse_util_t
 * @brief Geometry and sizing constants for the sparse fake device.
 */
typedef enum : uint32_t {
  k_sp_slots      = 8192U, /**< Hash slots: sectors that can hold content. */
  k_sp_sector_max = 4096U, /**< Largest sector size the store serves.      */
  k_sp_probe_max  = 8192U, /**< Linear-probe bound (the whole table).      */
} fs_sparse_util_t;

/**
 * @struct sparse_disk_t
 * @brief One sparse fake device: geometry, the sector store, and evidence.
 *
 * @details `lba[i]` holds the 64-bit sector address stored in `data[i]`;
 *          `used[i]` distinguishes a live slot from a free one (LBA 0 is a
 *          real address, so it cannot double as the empty marker). The
 *          `max_written` / `max_read` high-water marks are the proof a test
 *          reaches for after driving I/O past the 32-bit line.
 *
 * @invariant Every live slot's `lba[i]` is below `block_count`.
 * @invariant `bps` is a power of two in 512..::k_sp_sector_max.
 */
typedef struct {
  uint64_t block_count;                       /**< Device size in sectors.      */
  uint32_t bps;                               /**< Bytes per sector.            */
  uint32_t stored;                            /**< Live slots (capacity gauge). */
  uint64_t max_written;                       /**< Highest LBA ever written.    */
  uint64_t max_read;                          /**< Highest LBA ever read.       */
  uint64_t lba[k_sp_slots];                   /**< Slot keys.                   */
  uint8_t  used[k_sp_slots];                  /**< 1 = slot holds a sector.     */
  uint8_t  data[k_sp_slots][k_sp_sector_max]; /**< Slot payloads.               */
} sparse_disk_t;

/**
 * @struct sparse_window_t
 * @brief A slice of a sparse device presented as a device of its own.
 *
 * @details LBA `n` of the window is LBA `base + n` of the underlying store, so
 *          formatting the window plants a volume at an arbitrary absolute
 *          offset -- past 2 TiB included.
 *
 * @invariant `base + count` does not exceed the parent's `block_count`.
 */
typedef struct {
  sparse_disk_t* disk;  /**< Underlying sparse store.       */
  uint64_t       base;  /**< First parent LBA of the slice. */
  uint64_t       count; /**< Slice length in sectors.       */
} sparse_window_t;

/** @brief Locate the slot holding @p lba, or the free slot to claim for it. */
[[maybe_unused]] RA8_INTERNAL static uint32_t
internal_sp_slot(const sparse_disk_t* d, uint64_t lba, uint8_t* out_found)
{
  uint32_t i = (uint32_t)(lba % (uint64_t)k_sp_slots);
  for (uint32_t probe = 0U; probe < (uint32_t)k_sp_probe_max; probe++) {
    if (d->used[i] == 0U) {
      *out_found = 0U;
      return i;
    }
    if (d->lba[i] == lba) {
      *out_found = 1U;
      return i;
    }
    i = (i + 1U) % (uint32_t)k_sp_slots;
  }
  *out_found = 0U; /* table full: the test outgrew k_sp_slots -- fail loudly */
  return (uint32_t)k_sp_slots;
}

/** @brief True when @p buf is `n` zero bytes (a write the store may drop). */
[[maybe_unused]] RA8_INTERNAL static uint8_t internal_sp_all_zero(const uint8_t* buf, uint32_t n)
{
  for (uint32_t i = 0U; i < n; i++) {
    if (buf[i] != 0U) {
      return 0U;
    }
  }
  return 1U;
}

/** @brief Reset @p d to an empty device of @p blocks sectors of @p bps bytes. */
[[maybe_unused]] RA8_INTERNAL static void
internal_sp_init(sparse_disk_t* d, uint64_t blocks, uint32_t bps)
{
  memset(d, 0, sizeof(*d));
  d->block_count = blocks;
  d->bps         = bps;
}

/** @brief Read one stored-or-zero sector of @p d into @p out. */
[[maybe_unused]] RA8_INTERNAL static void
internal_sp_peek(sparse_disk_t* d, uint64_t lba, uint8_t* out)
{
  uint8_t        found = 0U;
  const uint32_t i     = internal_sp_slot(d, lba, &found);
  if (found != 0U) {
    memcpy(out, d->data[i], d->bps);
  } else {
    memset(out, 0, d->bps);
  }
}

/** @brief Store one sector of @p d from @p src (claims a slot when needed). */
[[maybe_unused]] RA8_INTERNAL static void
internal_sp_poke(sparse_disk_t* d, uint64_t lba, const uint8_t* src)
{
  uint8_t        found = 0U;
  const uint32_t i     = internal_sp_slot(d, lba, &found);
  if (i >= (uint32_t)k_sp_slots) {
    return; /* table full; the bounds assert in the test will catch it */
  }
  if (found == 0U) {
    if (internal_sp_all_zero(src, d->bps) != 0U) {
      return; /* an unstored sector already reads as zeros */
    }
    d->used[i] = 1U;
    d->lba[i]  = lba;
    d->stored++;
  }
  memcpy(d->data[i], src, d->bps);
}

/** @brief `ra8_fs_backend_t.read_block` over a ::sparse_disk_t cookie. */
[[maybe_unused]] RA8_INTERNAL static ra8_err_t
internal_sp_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  sparse_disk_t* d = (sparse_disk_t*)ctx;
  if ((lba >= d->block_count) || (count > (d->block_count - lba))) {
    return k_ra8_err_out_of_range;
  }
  for (uint32_t k = 0U; k < count; k++) {
    internal_sp_peek(d, lba + k, &buf[(size_t)k * d->bps]);
  }
  if ((count != 0U) && ((lba + count - 1U) > d->max_read)) {
    d->max_read = lba + count - 1U;
  }
  return k_ra8_ok;
}

/** @brief `ra8_fs_backend_t.write_block` over a ::sparse_disk_t cookie. */
[[maybe_unused]] RA8_INTERNAL static ra8_err_t
internal_sp_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
{
  sparse_disk_t* d = (sparse_disk_t*)ctx;
  if ((lba >= d->block_count) || (count > (d->block_count - lba))) {
    return k_ra8_err_out_of_range;
  }
  for (uint32_t k = 0U; k < count; k++) {
    internal_sp_poke(d, lba + k, &buf[(size_t)k * d->bps]);
  }
  if ((count != 0U) && ((lba + count - 1U) > d->max_written)) {
    d->max_written = lba + count - 1U;
  }
  return k_ra8_ok;
}

/** @brief `ra8_fs_backend_t.get_capacity` over a ::sparse_disk_t cookie. */
[[maybe_unused]] RA8_INTERNAL static ra8_err_t
internal_sp_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  const sparse_disk_t* d = (const sparse_disk_t*)ctx;
  *block_count           = d->block_count;
  *block_size            = d->bps;
  return k_ra8_ok;
}

/** @brief Bind @p d as a full `ra8_fs` backend. */
[[maybe_unused]] RA8_INTERNAL static ra8_fs_backend_t internal_sp_backend(sparse_disk_t* d)
{
  const ra8_fs_backend_t be = {
    .read_block   = internal_sp_read,
    .write_block  = internal_sp_write,
    .get_capacity = internal_sp_capacity,
    .erase_blocks = nullptr,
    .ctx          = d,
  };
  return be;
}

/** @brief `read_block` over a ::sparse_window_t cookie (base-shifted). */
[[maybe_unused]] RA8_INTERNAL static ra8_err_t
internal_spw_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  sparse_window_t* w = (sparse_window_t*)ctx;
  if ((lba >= w->count) || (count > (w->count - lba))) {
    return k_ra8_err_out_of_range;
  }
  return internal_sp_read(w->disk, w->base + lba, count, buf);
}

/** @brief `write_block` over a ::sparse_window_t cookie (base-shifted). */
[[maybe_unused]] RA8_INTERNAL static ra8_err_t
internal_spw_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
{
  sparse_window_t* w = (sparse_window_t*)ctx;
  if ((lba >= w->count) || (count > (w->count - lba))) {
    return k_ra8_err_out_of_range;
  }
  return internal_sp_write(w->disk, w->base + lba, count, buf);
}

/** @brief `get_capacity` over a ::sparse_window_t cookie. */
[[maybe_unused]] RA8_INTERNAL static ra8_err_t
internal_spw_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  const sparse_window_t* w = (const sparse_window_t*)ctx;
  *block_count             = w->count;
  *block_size              = w->disk->bps;
  return k_ra8_ok;
}

/** @brief Bind window @p w as a full `ra8_fs` backend. */
[[maybe_unused]] RA8_INTERNAL static ra8_fs_backend_t internal_spw_backend(sparse_window_t* w)
{
  const ra8_fs_backend_t be = {
    .read_block   = internal_spw_read,
    .write_block  = internal_spw_write,
    .get_capacity = internal_spw_capacity,
    .erase_blocks = nullptr,
    .ctx          = w,
  };
  return be;
}
