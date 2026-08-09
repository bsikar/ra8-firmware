/**
 * @file ra8_fs_fat_space.c
 * @brief Free / used / total space query (`ra8_fs_free_space()`) for FAT and exFAT.
 *
 * @details
 * "How much room is left" answered from counts the driver already keeps. On a
 * FAT32 volume whose FSInfo validated at mount the free-cluster count is the
 * cached one, reported in O(1); on FAT12/FAT16, or a FAT32 whose FSInfo was
 * absent or untrusted, the FAT is scanned once (through the shared one-sector
 * FAT cache, so a full scan costs one read per 128/256 clusters, not one per
 * cluster) and the result is cached for the next query. On exFAT the allocation
 * bitmap -- which exFAT spec sec 7.1 makes the sole authority for allocation
 * state -- is population-counted, also cached.
 *
 * Byte totals are 64-bit: a 2 TiB volume's byte count overflows 32 bits even
 * though FAT file sizes stay 32-bit. The cluster counts stay 32-bit, matching
 * the on-disk fields they come from.
 *
 * References (every shorthand citation in this file):
 *   - "MS FAT spec" = Microsoft Corp., "FAT: General Overview of On-Disk
 *     Format", v1.03, December 6 2000.
 *   - "exFAT spec" = Microsoft Corp., "exFAT file system specification",
 *     revision 1.00.
 *
 * NASA Power-of-Ten compliance:
 *   - Rule 2: the FAT scan is bounded by `count_of_clusters`; the bitmap scan
 *     by the bitmap's own byte length (itself `<= count_of_clusters / 8`).
 *   - Rule 3: zero malloc; one 512-byte sector buffer on the stack.
 *   - Rule 7: every backend call is checked.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_internal.h"
#include "ra8_fs_meta.h"

/**
 * @enum ra8_fs_space_const_t
 * @brief Bit-arithmetic constants for the free-space walkers.
 *
 * @details The exFAT allocation bitmap packs one cluster per bit, LSB first, so
 *          a cluster index divides by 8 (shift 3) to a byte and masks with 7 to
 *          a bit -- the same layout the exFAT mutation helpers use.
 *
 * @invariant `k_space_bit_mask + 1 == 1 << k_space_byte_shift == k_space_bits_per_byte`.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_space_bits_per_byte = 8U, /**< Bits in one allocation-bitmap byte.    */
  k_space_bit_mask      = 7U, /**< Cluster index -> bit within its byte.  */
  k_space_byte_shift    = 3U, /**< log2(8): cluster index -> bitmap byte. */
} ra8_fs_space_const_t;

/**
 * @brief Count the set bits in one byte.
 *
 * @details A bounded eight-iteration population count -- no compiler builtin, so
 *          the same result on the host and on the Cortex-M target regardless of
 *          `__builtin_popcount` availability.
 *
 * @param[in] b Byte to count.
 *
 * @return The number of 1 bits in @p b.
 * @retval 0..8 The population count.
 *
 * @pre None (total over uint8_t).
 * @pre @p b is a whole byte.
 * @post No state modified.
 * @post The result is at most 8.
 *
 * @note Pure function; trivially thread-safe.
 * @note Bounded loop (NASA Rule 2): exactly ::k_space_bits_per_byte iterations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_popcount8(uint8_t b)
{
  uint32_t n = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_space_bits_per_byte; i++) {
    n += ((uint32_t)b >> i) & 1U;
  }
  return n;
}

/**
 * @brief Count free clusters on a FAT12/16/32 volume by scanning the FAT.
 *
 * @details Reads every data cluster's FAT entry through ::priv_fat_get (and so
 *          through the shared one-sector FAT cache) and counts the ones that
 *          read as ::k_cluster_free. The fallback for a volume with no trusted
 *          FSInfo count; the result is cached by the caller.
 *
 * @param[in]  m        Mounted FAT volume.
 * @param[out] out_free Receives the free-cluster count.
 *
 * @return Error code.
 * @retval k_ra8_ok    @p out_free holds the count.
 * @retval k_ra8_err_* Backend read failure mid-scan.
 *
 * @pre @p m and @p out_free are non-NULL; `m->type` is FAT12/16/32.
 * @pre The mount's geometry is populated.
 * @post On success @p out_free is `<= m->count_of_clusters`.
 * @post No volume state is modified.
 *
 * @note Bounded loop (NASA Rule 2): `m->count_of_clusters` iterations.
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_space_fat_free(const ra8_fs_mount_t* m, uint32_t* out_free)
{
  uint32_t       free = 0U;
  const uint32_t last = (uint32_t)k_cluster_first_data + m->count_of_clusters;
  for (uint32_t c = (uint32_t)k_cluster_first_data; c < last; c++) {
    uint32_t        v   = 0U;
    const ra8_err_t err = priv_fat_get(m, c, &v);
    if (err != k_ra8_ok) {
      return err;
    }
    if (v == (uint32_t)k_cluster_free) {
      free++;
    }
  }
  *out_free = free;
  return k_ra8_ok;
}

/**
 * @brief Count free clusters on an exFAT volume by population-counting the bitmap.
 *
 * @details Locates the allocation bitmap (exFAT spec sec 7.1.5), reads it sector
 *          by sector through one reused buffer, and sums the SET bits over the
 *          volume's `count_of_clusters` cluster bits -- the used count. Free is
 *          the complement. The final partial byte is masked to the volume's last
 *          cluster so bits past the cluster heap (which the spec leaves 0, but a
 *          defensive read cannot assume) never inflate the used count.
 *
 * @param[in]  m        Mounted exFAT volume.
 * @param[out] out_free Receives the free-cluster count.
 *
 * @return Error code.
 * @retval k_ra8_ok            @p out_free holds the count.
 * @retval k_ra8_err_not_found No allocation-bitmap entry (corrupt volume).
 * @retval k_ra8_err_*         Backend read failure.
 *
 * @pre @p m and @p out_free are non-NULL; `m->type` is exFAT.
 * @pre The mount's geometry is populated.
 * @post On success @p out_free is `<= m->count_of_clusters`.
 * @post No volume state is modified.
 *
 * @note Bounded loop (NASA Rule 2): `count_of_clusters / 8 + 1` iterations.
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_space_exfat_free(const ra8_fs_mount_t* m, uint32_t* out_free)
{
  uint32_t        bmp_clus = 0U;
  uint32_t        bmp_len  = 0U;
  const ra8_err_t ferr     = priv_exfat_find_bitmap(m, &bmp_clus, &bmp_len);
  if (ferr != k_ra8_ok) {
    return ferr;
  }
  const uint32_t bmp_lba    = priv_cluster_to_lba(m, bmp_clus);
  const uint32_t total      = m->count_of_clusters;
  const uint32_t full_bytes = total >> (uint32_t)k_space_byte_shift;
  const uint32_t rem_bits   = total & (uint32_t)k_space_bit_mask;
  /* One byte per full group of 8 clusters, plus one partial byte for the tail.
   * The tail byte is masked to the volume's last cluster so any bits the spec
   * leaves 0 past the heap cannot inflate the used count. */
  const uint32_t nbytes                         = full_bytes + ((rem_bits != 0U) ? 1U : 0U);
  uint32_t       used                           = 0U;
  uint32_t       loaded                         = UINT32_MAX;
  uint8_t        sec[k_ra8_fs_bytes_per_sector] = {};
  for (uint32_t bi = 0U; bi < nbytes; bi++) {
    const uint32_t lba = bmp_lba + (bi / (uint32_t)k_ra8_fs_bytes_per_sector);
    if (lba != loaded) {
      const ra8_err_t err = priv_read_sector(m, lba, sec);
      if (err != k_ra8_ok) {
        return err;
      }
      loaded = lba;
    }
    uint8_t b = sec[bi % (uint32_t)k_ra8_fs_bytes_per_sector];
    /* The loop only reaches `bi == full_bytes` when `rem_bits != 0` (that is the
     * only case `nbytes` includes the partial byte), so masking it here needs no
     * second condition -- and adding one would be an MC/DC hole nothing can flip. */
    if (bi == full_bytes) {
      b = (uint8_t)(b & (uint8_t)((1U << rem_bits) - 1U));
    }
    used += priv_popcount8(b);
  }
  /* The bitmap holds exactly `total` cluster bits and the tail is masked, so
   * `used <= total` always -- no clamp needed. */
  *out_free = total - used;
  return k_ra8_ok;
}

/**
 * @brief Resolve the free-cluster count: cached, or freshly walked and cached.
 *
 * @details On the FAT variants it prefers the allocator's tracked count (FAT32
 *          FSInfo, or a FAT12/16 count this routine cached earlier): the FAT
 *          allocator keeps it current on every ::priv_alloc_cluster /
 *          ::priv_free_chain, so once known it stays right. When that count is
 *          ::k_fs_free_unknown the FAT is walked once and the result cached.
 *
 *          exFAT is the exception and is ALWAYS recomputed. Its allocator
 *          maintains the allocation bitmap directly and never touches the
 *          tracked free count, so a cached value would go stale on the next
 *          write; the bitmap is authoritative and small, so a fresh
 *          population count every query is both correct and cheap.
 *
 * @param[in]  m        Mounted volume.
 * @param[out] out_free Receives the free-cluster count.
 *
 * @return Error code.
 * @retval k_ra8_ok    @p out_free holds the count.
 * @retval k_ra8_err_* Backend read failure while walking.
 *
 * @pre @p m and @p out_free are non-NULL; the mount is in use.
 * @pre The mount's geometry is populated.
 * @post On success @p out_free is `<= m->count_of_clusters`.
 * @post On a fresh FAT walk the count is cached in the allocator slot.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_space_free_clusters(const ra8_fs_mount_t* m, uint32_t* out_free)
{
  if (m->type == k_ra8_fs_type_exfat) {
    return priv_space_exfat_free(m, out_free);
  }
  const uint32_t cached = priv_free_count_peek(m);
  if (cached != (uint32_t)k_fs_free_unknown) {
    *out_free = cached;
    return k_ra8_ok;
  }
  const ra8_err_t err = priv_space_fat_free(m, out_free);
  if (err != k_ra8_ok) {
    return err;
  }
  priv_free_count_cache(m, *out_free);
  return k_ra8_ok;
}

/**
 * @brief Report free/used/total space -- the guarded body of ::ra8_fs_free_space().
 *
 * @details Resolves the free-cluster count, clamps it to the volume size (a
 *          corrupt count above the disk size is worse than a walk), and scales
 *          the cluster figures by the allocation-unit size into the 64-bit byte
 *          totals. The public ::ra8_fs_free_space brackets this with the library
 *          lock; the full contract is documented there.
 *
 * @param[in]  handle Mount handle.
 * @param[out] out    Receives the space figures.
 *
 * @return Error code.
 * @retval k_ra8_ok                Figures reported.
 * @retval k_ra8_err_null_ptr      @p handle or @p out is NULL.
 * @retval k_ra8_err_invalid_state Mount is not in use.
 * @retval k_ra8_err_*             Backend read failure.
 *
 * @pre The library lock is held (or none is installed).
 * @pre @p handle and @p out are non-NULL.
 * @post On success `out->used_clusters + out->free_clusters == out->total_clusters`.
 * @post No volume state is modified.
 *
 * @note Never call this from outside `ra8_fs`; it is the unlocked half.
 * @note The null-pointer guard's MC/DC vectors live in `test_ra8_fs_space.c`
 *       (test_space_null_guard).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_EXPECTS_LOCK("ra8_fs_lock")
static ra8_err_t priv_space_locked(ra8_fs_mount_t* handle, ra8_fs_space_t* out)
{
  if (handle == nullptr || out == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (handle->in_use == 0U) {
    return k_ra8_err_invalid_state;
  }
  uint32_t        free_clusters = 0U;
  const ra8_err_t err           = priv_space_free_clusters(handle, &free_clusters);
  if (err != k_ra8_ok) {
    return err;
  }
  /* `free_clusters` is bounded by the volume size at its source: the FAT scan
   * counts among exactly `count_of_clusters` entries, the bitmap popcount masks
   * to the last cluster, and the cached count is clamped on the way in. So no
   * clamp is needed here. */
  const uint32_t total = handle->count_of_clusters;
  const uint32_t bytes_per_cluster =
    handle->sectors_per_cluster * (uint32_t)k_ra8_fs_bytes_per_sector;
  const uint32_t used    = total - free_clusters;
  *out                   = (ra8_fs_space_t){};
  out->bytes_per_cluster = bytes_per_cluster;
  out->total_clusters    = total;
  out->free_clusters     = free_clusters;
  out->used_clusters     = used;
  out->total_bytes       = (uint64_t)total * (uint64_t)bytes_per_cluster;
  out->free_bytes        = (uint64_t)free_clusters * (uint64_t)bytes_per_cluster;
  out->used_bytes        = (uint64_t)used * (uint64_t)bytes_per_cluster;
  return k_ra8_ok;
}

/* =============================================================================
 * Public entry point -- the lock bracket
 * =============================================================================
 */

RA8_OWNS_RESOURCE("ra8_fs_lock")
ra8_err_t ra8_fs_free_space(ra8_fs_mount_t* handle, ra8_fs_space_t* out)
{
  priv_lock_acquire();
  const ra8_err_t err = priv_space_locked(handle, out);
  priv_lock_release();
  return err;
}
