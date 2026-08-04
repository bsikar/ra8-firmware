/**
 * @file ra8_fs_fat_alloc_internal.h
 * @brief Per-mount allocator state: next-free hint, free count, FAT cache.
 * @ingroup grp_storage
 *
 * @details
 * Allocation used to be quadratic. `priv_alloc_cluster()` restarted its scan
 * at cluster 2 on every call, and `priv_fat_get()` had no cache, so examining
 * cluster N cost N block reads and writing a K-cluster file cost O(K*N) real
 * block-device round trips. On a 32 GB FAT32 card the FAT is thousands of
 * sectors and a multi-megabyte write re-read most of it per cluster. It never
 * showed up in a unit test because a mock backend's "read" is a memcpy.
 *
 * Three pieces of state fix it, and they belong together because they are the
 * same fact seen three ways -- where the free space is:
 *
 *   1. a **next-free hint**, so a scan starts where the last one stopped and
 *      wraps exactly once before declaring the volume full;
 *   2. a **one-sector FAT cache**, so the 128 FAT32 entries that share a
 *      sector cost one read between them instead of 128;
 *   3. a **free-cluster count**, seeded from FAT32's FSInfo sector at mount
 *      and written back when a file closes or the volume unmounts, so
 *      `fsck.fat` stops reporting "Free cluster summary wrong" on a card this
 *      firmware wrote and Explorer stops showing stale free space.
 *
 * The state is keyed by mount POINTER in a small module-static table rather
 * than living in ::ra8_fs_mount_t. That is deliberate: `priv_fat_get()` and
 * every chain walker in this adapter take a `const ra8_fs_mount_t*`, and
 * making the mount mutable to carry a cache would have cast that `const` off
 * -- or propagated a non-const mount through a dozen call chains -- to add
 * what is, semantically, a cache and not part of the volume.
 *
 * Every accessor here is total: a mount with no bound slot behaves exactly
 * like the old code (hint at cluster 2, no cache, free count unknown), so a
 * missing binding degrades performance and never correctness.
 *
 * References (every shorthand citation in this file):
 *   - "MS FAT spec" = Microsoft Corp., "FAT: General Overview of On-Disk
 *     Format", v1.03, December 6 2000. FSInfo is sec 5.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_fs.h"

/**
 * @enum ra8_fs_alloc_const_t
 * @brief Sentinels and bounds for the per-mount allocator state.
 *
 * @details `k_fs_cache_empty` doubles as "no sector loaded" and as an LBA no
 *          volume can address, which is why it is `UINT32_MAX` rather than 0
 *          (LBA 0 is the boot sector and a perfectly real address).
 *
 * @invariant `k_fs_alloc_slots` equals ::k_ra8_fs_max_mounts, so a bind can
 *            never fail while a mount slot is available.
 * @see priv_alloc_state_bind()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_fs_cache_empty    = 0xFFFFFFFFU, /**< Cache holds no sector.               */
  k_fs_free_unknown   = 0xFFFFFFFFU, /**< MS FAT spec sec 5: count not known.  */
  k_fs_alloc_slots    = 2U,          /**< One state slot per mount slot.       */
  k_fs_fsinfo_absent  = 0U,          /**< No usable FSInfo sector on this vol. */
  k_fs_bitmap_unknown = 0xFFFFFFFFU, /**< exFAT bitmap LBA not resolved yet.   */
} ra8_fs_alloc_const_t;

/**
 * @brief Claim and reset the allocator state slot for a freshly mounted volume.
 *
 * @details Called once from `ra8_fs_mount()` after the geometry is known. The
 *          reset is the load-bearing half: the mount table is a fixed array,
 *          so the same ::ra8_fs_mount_t address is handed out again after an
 *          unmount, and a slot carrying the PREVIOUS volume's next-free hint
 *          would hand the new volume another disk's answers. The shared FAT
 *          sector cache is NOT dropped here -- ::priv_alloc_state_release owns
 *          that, and a mount slot cannot be handed out again without passing
 *          through it, so a second drop would be an unreachable branch
 *          pretending to be a safety net.
 *
 * @param[in] m Mount that has just been parsed.
 *
 * @return Nothing.
 *
 * @pre @p m is non-NULL and its geometry fields are populated.
 * @pre No allocator accessor is running concurrently.
 * @post @p m owns a slot whose hint is cluster 2 and whose cache is empty.
 * @post The free count is ::k_fs_free_unknown until ::priv_fsinfo_seed runs.
 *
 * @note Not thread-safe; callers serialise mount operations.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_alloc_state_bind(const ra8_fs_mount_t* m);

/**
 * @brief Release the allocator state slot held by @p m.
 *
 * @details Called from `ra8_fs_unmount()` after any FSInfo writeback. Also
 *          drops the shared FAT sector cache when it belongs to @p m, so a
 *          later mount of a different volume cannot be served stale bytes.
 *
 * @param[in] m Mount being unmounted.
 *
 * @return Nothing.
 *
 * @pre @p m is non-NULL.
 * @pre Any pending FSInfo writeback has already been flushed.
 * @post @p m owns no slot; a later bind starts from a clean state.
 * @post The shared FAT sector cache holds nothing belonging to @p m.
 *
 * @note Not thread-safe; callers serialise mount operations.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_alloc_state_release(const ra8_fs_mount_t* m);

/**
 * @brief Read one FAT sector, through the shared one-sector cache.
 *
 * @details The whole point of the exercise: a FAT32 sector holds 128 entries
 *          and a FAT16 sector 256, so a free-cluster scan that reads through
 *          here issues one backend read per 128 (or 256) clusters examined
 *          instead of one per cluster.
 *
 *          Only sectors inside the FIRST FAT copy are cached. Reads never
 *          touch the other copies, and a `priv_fat_set()` mirroring an entry
 *          into copy 1 would otherwise evict the very sector the scan is
 *          walking -- turning the cache off exactly during the allocation
 *          storm it exists for. An LBA outside that window is passed straight
 *          to the backend.
 *
 * @param[in]  m   Mount providing the backend and the FAT geometry.
 * @param[in]  lba Volume-relative sector to read.
 * @param[out] buf Destination of at least ::k_ra8_fs_bytes_per_sector bytes.
 *
 * @return Error code.
 * @retval k_ra8_ok    @p buf holds the sector (from the cache or the backend).
 * @retval k_ra8_err_* Backend read failure; @p buf is unspecified.
 *
 * @pre @p m and @p buf are non-NULL; the mount's backend is bound.
 * @pre @p buf addresses a whole writable sector.
 * @post On success @p buf equals the on-disk sector at @p lba.
 * @post A cacheable sector that missed is now the cached one.
 *
 * @note Not thread-safe; the cache is module-static like `s_scratch`.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_fat_sector_read(const ra8_fs_mount_t* m, uint32_t lba, uint8_t* buf);

/**
 * @brief Tell the cache that @p lba has just been written with @p buf.
 *
 * @details Write-through: the caller has already committed the sector, so the
 *          cache is refreshed rather than invalidated and the next read of the
 *          same sector still costs nothing. Called after every FAT sector
 *          write, including the ones that land in a mirror copy -- those are
 *          outside the cached window and are simply ignored.
 *
 * @param[in] m   Mount whose FAT geometry decides whether @p lba is cacheable.
 * @param[in] buf The bytes that were written.
 * @param[in] lba Volume-relative sector that was written.
 *
 * @return Nothing.
 *
 * @pre @p m and @p buf are non-NULL.
 * @pre The write of @p buf to @p lba already succeeded.
 * @post If @p lba is cacheable, the cache holds @p buf for @p m.
 * @post No backend I/O is issued.
 *
 * @note Not thread-safe; the cache is module-static like `s_scratch`.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_fat_sector_wrote(const ra8_fs_mount_t* m, const uint8_t* buf, uint32_t lba);

/**
 * @brief Report the cluster a free-space scan should start from.
 *
 * @details Seeded from FAT32's `FSI_Nxt_Free` when the FSInfo sector
 *          validates, otherwise cluster 2. Always returns a value the caller
 *          must still range-check: a hint is an optimisation, not a promise,
 *          and an on-disk FSInfo written by some other implementation is not
 *          trusted to be in range.
 *
 * @param[in] m Mount to query.
 *
 * @return The cluster number to begin scanning at.
 * @retval 2..UINT32_MAX The hint, or 2 when @p m has no bound slot.
 *
 * @pre @p m is non-NULL.
 * @pre The mount's geometry is populated.
 * @post No state modified.
 * @post The result is at least ::k_cluster_first_data.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_PRIV
uint32_t priv_alloc_hint_get(const ra8_fs_mount_t* m);

/**
 * @brief Move the next-free hint forward to @p cluster.
 *
 * @details Called after a successful allocation with the cluster AFTER the one
 *          taken, so a run of allocations walks forward instead of re-reading
 *          the entries it has just filled in.
 *
 * @param[in] m       Mount to update.
 * @param[in] cluster Cluster the next scan should start at.
 *
 * @return Nothing.
 *
 * @pre @p m is non-NULL.
 * @pre @p cluster is the cluster after the one just allocated.
 * @post A later ::priv_alloc_hint_get returns @p cluster.
 * @post No backend I/O is issued.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_alloc_hint_set(const ra8_fs_mount_t* m, uint32_t cluster);

/**
 * @brief Pull the next-free hint back to @p cluster if it is further on.
 *
 * @details Called for every cluster returned to the volume. Without it a
 *          delete followed by a create would step over the space it just
 *          released and only find it again after a full wrap -- correct, but
 *          it would make freed clusters look unreusable to any test (and any
 *          user) watching where the next file lands.
 *
 * @param[in] m       Mount to update.
 * @param[in] cluster Cluster that has just become free.
 *
 * @return Nothing.
 *
 * @pre @p m is non-NULL.
 * @pre @p cluster's FAT entry (or bitmap bit) already reads as free.
 * @post The hint is at most @p cluster.
 * @post No backend I/O is issued.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_alloc_hint_lower(const ra8_fs_mount_t* m, uint32_t cluster);

/**
 * @brief Account @p n clusters as taken out of the volume's free space.
 *
 * @details A no-op while the count is ::k_fs_free_unknown: a number we are not
 *          sure of is worse than no number, because FSInfo's "unknown" value
 *          is honest and a wrong count is what makes `fsck.fat` complain in
 *          the first place.
 *
 * @param[in] m Mount to update.
 * @param[in] n Clusters allocated.
 *
 * @return Nothing.
 *
 * @pre @p m is non-NULL.
 * @pre @p n clusters have actually been marked used on disk.
 * @post The tracked free count drops by @p n, or stays unknown.
 * @post The volume is flagged for FSInfo writeback.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_free_count_took(const ra8_fs_mount_t* m, uint32_t n);

/**
 * @brief Account @p n clusters as returned to the volume's free space.
 *
 * @details The mirror of ::priv_free_count_took, and equally a no-op while the
 *          count is unknown. Clamped at the volume's cluster count so a
 *          double-free on a corrupt chain cannot inflate the reported free
 *          space past the size of the disk.
 *
 * @param[in] m Mount to update.
 * @param[in] n Clusters freed.
 *
 * @return Nothing.
 *
 * @pre @p m is non-NULL.
 * @pre @p n clusters have actually been marked free on disk.
 * @post The tracked free count rises by @p n but never past `count_of_clusters`.
 * @post The volume is flagged for FSInfo writeback.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_PRIV
void priv_free_count_gave(const ra8_fs_mount_t* m, uint32_t n);

/**
 * @brief Validate FAT32's FSInfo sector and seed the hint and free count.
 *
 * @details MS FAT spec sec 5. All three signatures must match -- lead
 *          `0x41615252`, struct `0x61417272`, trailing `0xAA550000` -- before
 *          a single field is believed, and each field is then range-checked
 *          independently: `FSI_Nxt_Free` must name a real cluster and
 *          `FSI_Free_Count` must not exceed the volume's cluster count. Either
 *          one failing leaves that half at its default without discarding the
 *          other.
 *
 *          A volume with no FSInfo (FAT12, FAT16, exFAT, or a FAT32 BPB whose
 *          `BPB_FSInfo` points outside the reserved region) is not an error:
 *          the mount keeps the cluster-2 hint and an unknown free count, and
 *          nothing is ever written back.
 *
 * @param[in] m Freshly parsed mount with a bound allocator slot.
 *
 * @return Error code.
 * @retval k_ra8_ok    Seeded, or the volume legitimately has no FSInfo.
 * @retval k_ra8_err_* The backend could not read a sector inside the volume's
 *                     own reserved region.
 *
 * @pre @p m is non-NULL and ::priv_alloc_state_bind has run for it.
 * @pre @p m's geometry fields are populated.
 * @post On success the hint and free count reflect a validated FSInfo, or
 *       their defaults.
 * @post No FSInfo bytes are modified.
 *
 * @note Not thread-safe; callers serialise mount operations.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_fsinfo_seed(const ra8_fs_mount_t* m);

/**
 * @brief Write the tracked free count and next-free hint back into FSInfo.
 *
 * @details Read-modify-write of the FSInfo sector, so the signatures and every
 *          reserved byte survive untouched. Does nothing when the volume has
 *          no FSInfo or when nothing has been allocated or freed since the
 *          last flush, which keeps a read-only workload from writing to the
 *          card at all.
 *
 *          An unknown free count is written as ::k_fs_free_unknown rather than
 *          a guess: the format defines that value for exactly this case, and
 *          `fsck.fat` accepts it silently instead of reporting a wrong summary.
 *
 * @param[in] m Mount to flush.
 *
 * @return Error code.
 * @retval k_ra8_ok    Written, or there was nothing to write.
 * @retval k_ra8_err_* Backend read/write failure on the FSInfo sector.
 *
 * @pre @p m is non-NULL.
 * @pre Every cluster allocation and release has already been committed.
 * @post On success the on-disk FSInfo matches the tracked state.
 * @post The dirty flag is clear, so a second flush is a no-op.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_fsinfo_flush(const ra8_fs_mount_t* m);

/**
 * @brief Resolve the exFAT allocation bitmap's first LBA, once per mount.
 *
 * @details The bitmap's location lives in a system directory entry in the root,
 *          so finding it means walking the root directory -- fine once, ruinous
 *          per cluster of a streaming write, which is exactly how often the
 *          grow path needs it. The answer cannot change while a volume is
 *          mounted (the bitmap is not relocatable), so the first walk is cached
 *          in the mount's allocator state and every later call is a load.
 *
 *          A mount with no bound allocator slot still gets a correct answer,
 *          just an uncached one: the accessor is total, like the rest of this
 *          header's.
 *
 * @param[in]  m       Mounted exFAT volume.
 * @param[out] out_lba Receives the volume-relative first LBA of the bitmap.
 *
 * @return Error code.
 * @retval k_ra8_ok            The bitmap was located (cached or freshly found).
 * @retval k_ra8_err_not_found The root directory carries no bitmap entry.
 * @retval k_ra8_err_*         Backend read failure during the directory walk.
 *
 * @pre @p m and @p out_lba are non-NULL and `m->type` is exFAT.
 * @pre The volume is mounted; the root directory is readable.
 * @post On success `*out_lba` addresses the bitmap's first sector.
 * @post On failure `*out_lba` is unmodified.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_exfat_bitmap_lba(const ra8_fs_mount_t* m, uint32_t* out_lba);
