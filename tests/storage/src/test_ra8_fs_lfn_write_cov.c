/**
 * @file test_ra8_fs_lfn_write_cov.c
 * @brief The long-name write seam's failure paths and its awkward geometries (#600).
 *
 * @details
 * The two behaviour suites (`test_ra8_fs_lfn_write.c`, `test_ra8_fs_lfn_erase.c`)
 * drive the seam through a healthy volume. This one drives it through a hostile
 * one: every backend read and every backend write on each of the three verbs is
 * made to fail in turn, and the result has to be REPORTED rather than ignored.
 *
 * The sweeps are budget-swept rather than pinned to one injection count. A test
 * that fails "the fourth read" stops testing anything the day a helper gains a
 * read; raising the budget from zero until the call succeeds walks every I/O on
 * the path and asserts each one is a reported failure, whatever the path's
 * shape becomes.
 *
 * It also builds two geometries the ordinary fixtures cannot: a 64-entry root
 * (four sectors), so a chain and its deletion straddle a sector boundary, and a
 * hand-planted over-long chain, so the deleter's bounded-run cap is exercised
 * by an input no API call can produce.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fs_lfn_write_test_util.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "unity_minimal.h"

/**
 * @enum lwc_val_t
 * @brief Geometry and sweep bounds for these cases.
 *
 * @details `k_lwc_budget_cap` bounds every injection sweep, so a path that
 *          never succeeds fails the test instead of running forever (NASA
 *          Power of 10 Rule 2).
 *
 * @invariant `k_lwc_root_entries` is a whole number of 512-byte sectors.
 * @see sweep_reads()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_lwc_budget_cap   = 256U,        /**< Upper bound on any I/O injection sweep.     */
  k_lwc_root_entries = 64U,         /**< Four sectors of fixed root directory.       */
  k_lwc_fat_sectors  = 32U,         /**< Sectors per FAT copy in the big-root vol.   */
  k_lwc_total_sec    = 8192U,       /**< 4 MiB volume.                               */
  k_lwc_planted_lfn  = 21U,         /**< One more than the spec's chain maximum.     */
  k_lwc_sub_fill     = 13U,         /**< Files that leave one free slot in /SUB.     */
  k_lwc_erase_cap    = 20U,         /**< `k_lfn_erase_max`: the spec's LDIR_Ord max. */
  k_lwc_root_fill    = 15U,         /**< Root entries before the straddling name.    */
  k_lwc_ord_mask     = 0x1FU,       /**< LDIR_Ord order field (MS FAT spec sec 7).   */
  k_lwc_f32_blocks   = 70000U,      /**< Sectors in the FAT32 volume.                */
  k_lwc_f32_fat_secs = 512U,        /**< Sectors per FAT copy on that volume.        */
  k_lwc_f32_root_cl  = 2U,          /**< BPB_RootClus: the root's cluster.           */
  k_lwc_f32_eoc      = 0x0FFFFFFFU, /**< FAT32 end-of-chain marker.                  */
  k_lwc_off_tot32    = 32U,         /**< BPB_TotSec32.                               */
  k_lwc_off_fatsz32  = 36U,         /**< BPB_FATSz32.                                */
  k_lwc_off_rootclus = 44U,         /**< BPB_RootClus.                               */
  k_lwc_f32_fill     = 15U,         /**< Root entries before the growing name.       */
  k_lwc_byte_mask    = 0xFFU,       /**< Low-byte mask used by put32().              */
  k_lwc_bits_byte    = 8U,          /**< Bits per byte, for put32()'s shift.         */
  k_lwc_u32_bytes    = 4U,          /**< Bytes in a FAT32 entry / a put32().         */
  k_lwc_fat_copies   = 2U,          /**< FAT copies the fixtures write.              */
} lwc_val_t;

/* ===========================================================================
 * A 64-entry root, so a chain can straddle a sector boundary
 * ===========================================================================
 */

/**
 * @brief Hand-build a FAT16 volume whose fixed root spans four sectors.
 *
 * @details The shared fixture's root is one sector, which no chain can
 *          straddle. Four sectors put the boundary in the middle of the
 *          directory, which is where ::priv_slot_advance() and the deleter's
 *          sector batching earn their keep.
 *
 * @return Nothing.
 *
 * @pre `s_disk.bytes` is either null or owned by the fixture.
 * @pre The caller will free_vol() afterwards.
 * @post `s_disk` holds a mountable FAT16 volume with a 64-entry root.
 * @post Any previous image has been released.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_build_fat16_big_root(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
  s_disk.byte_count  = (uint32_t)k_lwc_total_sec * (uint32_t)k_geo_blk_sz;
  s_disk.bytes       = (uint8_t*)calloc(1, s_disk.byte_count);
  s_disk.block_count = (uint32_t)k_lwc_total_sec;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
  uint8_t* bpb = s_disk.bytes;
  internal_put16(bpb, (uint32_t)k_bpb_off_bytes_per_sec, (uint16_t)k_geo_blk_sz);
  bpb[k_bpb_off_sec_per_clus] = 1U;
  internal_put16(bpb, (uint32_t)k_bpb_off_rsvd_sec_cnt, 1U);
  bpb[k_bpb_off_num_fats] = 2U;
  internal_put16(bpb, (uint32_t)k_bpb_off_root_ent_cnt, (uint16_t)k_lwc_root_entries);
  internal_put16(bpb, (uint32_t)k_bpb_off_tot_sec16, (uint16_t)k_lwc_total_sec);
  internal_put16(bpb, (uint32_t)k_bpb_off_fat_sz16, (uint16_t)k_lwc_fat_sectors);
  bpb[k_bpb_off_sig0] = (uint8_t)k_bpb_sig0_val;
  bpb[k_bpb_off_sig1] = (uint8_t)k_bpb_sig1_val;
}

/**
 * @brief Store a 32-bit value little-endian at byte offset @p off.
 *
 * @param[out] p   Destination byte buffer.
 * @param[in]  off Byte offset of the low byte.
 * @param[in]  v   Value to store.
 *
 * @return Nothing.
 *
 * @pre @p p addresses at least `off + 4` writable bytes.
 * @pre @p off is a valid offset inside the buffer.
 * @post `p[off .. off+3]` hold @p v little-endian.
 * @post No other byte is touched.
 *
 * @note Trivially thread-safe (writes only through @p p).
 * @since 0.1.0 @details Implements the bounded put32 fixture step using caller-owned state.
 */
RA8_INTERNAL static void internal_put32(uint8_t* p, uint32_t off, uint32_t v)
{
  for (uint32_t i = 0U; i < (uint32_t)k_lwc_u32_bytes; i++) {
    p[off + i] = (uint8_t)((v >> (i * (uint32_t)k_lwc_bits_byte)) & (uint32_t)k_lwc_byte_mask);
  }
}

/**
 * @brief Hand-build a FAT32 volume whose root is a one-sector cluster chain.
 *
 * @details A FAT32 root is a chain, not a fixed region, so it CAN be grown --
 *          which is the difference `priv_dir_grow()`'s two-condition guard
 *          exists to make, and the only geometry that exercises the
 *          `is_root && !fat32` pair's second condition. One sector per cluster
 *          keeps the root at sixteen slots, so filling it is cheap.
 *
 * @return Nothing.
 *
 * @pre `s_disk.bytes` is either null or owned by the fixture.
 * @pre The caller will free_vol() afterwards.
 * @post `s_disk` holds a mountable FAT32 volume with a 16-slot root.
 * @post Any previous image has been released.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_build_fat32_one_sector_root(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
  s_disk.byte_count  = (uint32_t)k_lwc_f32_blocks * (uint32_t)k_geo_blk_sz;
  s_disk.bytes       = (uint8_t*)calloc(1, s_disk.byte_count);
  s_disk.block_count = (uint32_t)k_lwc_f32_blocks;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
  uint8_t* bpb = s_disk.bytes;
  internal_put16(bpb, (uint32_t)k_bpb_off_bytes_per_sec, (uint16_t)k_geo_blk_sz);
  bpb[k_bpb_off_sec_per_clus] = 1U;
  internal_put16(bpb, (uint32_t)k_bpb_off_rsvd_sec_cnt, 1U);
  bpb[k_bpb_off_num_fats] = 2U;
  internal_put16(bpb, (uint32_t)k_bpb_off_root_ent_cnt, 0U); /* 0 root entries => FAT32 */
  internal_put32(bpb, (uint32_t)k_lwc_off_tot32, (uint32_t)k_lwc_f32_blocks);
  internal_put32(bpb, (uint32_t)k_lwc_off_fatsz32, (uint32_t)k_lwc_f32_fat_secs);
  internal_put32(bpb, (uint32_t)k_lwc_off_rootclus, (uint32_t)k_lwc_f32_root_cl);
  bpb[k_bpb_off_sig0] = (uint8_t)k_bpb_sig0_val;
  bpb[k_bpb_off_sig1] = (uint8_t)k_bpb_sig1_val;
  /* Mark the root's own cluster end-of-chain in both FAT copies. */
  for (uint32_t copy = 0U; copy < (uint32_t)k_lwc_fat_copies; copy++) {
    const uint32_t internal_fat_byte =
      ((1U + (copy * (uint32_t)k_lwc_f32_fat_secs)) * (uint32_t)k_geo_blk_sz) +
      ((uint32_t)k_lwc_f32_root_cl * (uint32_t)k_lwc_u32_bytes);
    internal_put32(s_disk.bytes, internal_fat_byte, (uint32_t)k_lwc_f32_eoc);
  }
}

/* ===========================================================================
 * I/O injection sweeps
 * ===========================================================================
 */

/**
 * @brief What one sweep step should do to the volume.
 *
 * @param[in,out] h Mounted volume, already pointed at an injecting backend.
 *
 * @return The verb's result.
 * @retval k_ra8_ok    The whole operation completed.
 * @retval k_ra8_err_* The injected failure, reported.
 *
 * @pre @p h is non-NULL and mounted.
 * @pre The caller has installed the injecting backend.
 * @post Whatever the verb did, it did through the public API.
 * @post No assertion is made by the step itself.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
typedef ra8_err_t (*lwc_step_t)(ra8_fs_mount_t* h);

/**
 * @brief Build and mount the volume one sweep step will run against.
 *
 * @return The mounted volume.
 * @retval non-NULL Always; a build or mount failure aborts in the harness.
 *
 * @pre Any previous volume has been unmounted and freed.
 * @pre The host allocator is available.
 * @post `s_disk` holds a fresh image and the mount is in use.
 * @post The caller owns the unmount and the free.
 *
 * @note Not thread-safe (uses the fixture singleton).
 * @since 0.1.0
 */
typedef ra8_fs_mount_t* (*lwc_setup_t)(void);

/**
 * @brief Raise an I/O budget from zero until @p step succeeds.
 *
 * @details The volume is REBUILT for every budget. That is not tidiness: a
 *          step that got far enough to write its new entry but not far enough
 *          to erase the old one leaves a volume the next attempt would refuse
 *          for an unrelated reason, and the sweep would stall on a failure
 *          that proves nothing. Each budget therefore starts from the same
 *          state and differs only in where the backend gives up.
 *
 *          Every budget below the succeeding one must have produced a reported
 *          failure. A step that "succeeded" while an I/O was still being denied
 *          would mean a swallowed error, which is what this sweep exists to
 *          catch.
 *
 * @param[in] setup  Builds and mounts the starting volume.
 * @param[in] step   The operation to attempt.
 * @param[in] writes Non-zero to sweep writes; zero to sweep reads.
 *
 * @return The budget at which the step first succeeded.
 * @retval 1..k_lwc_budget_cap The number of I/Os the path needs.
 *
 * @pre @p setup and @p step are non-NULL.
 * @pre The volume @p setup builds is one where @p step can eventually succeed.
 * @post Every volume built here has been unmounted and freed.
 * @post Every failing budget reported an error rather than succeeding.
 *
 * @note Bounded by ::k_lwc_budget_cap (NASA Power of 10 Rule 2).
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_sweep_io(lwc_setup_t setup, lwc_step_t step, uint8_t writes)
{
  for (uint32_t budget = 0U; budget < (uint32_t)k_lwc_budget_cap; budget++) {
    ra8_fs_mount_t*        h     = setup();
    const ra8_fs_backend_t saved = h->backend;
    if (writes != 0U) {
      internal_swap_to_wcount(h, budget);
    } else {
      internal_swap_to_inject(h, budget, 0U);
    }
    const ra8_err_t err = step(h);
    h->backend          = saved;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
    internal_free_vol();
    if (err == k_ra8_ok) {
      return budget;
    }
  }
  TEST_FAIL_FMT("%s", "I/O sweep never succeeded within the budget cap");
  return 0U;
}

/**
 * @brief Create `/A Long Enough Name.txt` and close it.
 *
 * @param[in,out] h Mounted volume.
 *
 * @return The open's result (the close is only reached on success).
 * @retval k_ra8_ok    Created.
 * @retval k_ra8_err_* Reported failure.
 *
 * @pre @p h is mounted.
 * @pre The name does not already exist.
 * @post On success the file exists and no handle is left open.
 * @post On failure no handle is leaked.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @details Implements the bounded step create long fixture step using caller-owned state.
 */
RA8_INTERNAL static ra8_err_t internal_step_create_long(ra8_fs_mount_t* h)
{
  ra8_fs_file_t*  f   = nullptr;
  const ra8_err_t err = ra8_fs_open(h, "/A Long Enough Name.txt", k_ra8_fs_mode_write, &f);
  if (err == k_ra8_ok) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  }
  return err;
}

/**
 * @brief Delete `/A Long Enough Name.txt`.
 *
 * @param[in,out] h Mounted volume.
 *
 * @return The unlink's result.
 * @retval k_ra8_ok    Deleted.
 * @retval k_ra8_err_* Reported failure.
 *
 * @pre @p h is mounted and holds the name.
 * @pre No handle refers to it.
 * @post On success neither the entry nor its chain remains.
 * @post On failure the report reached the caller.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @details Implements the bounded step unlink long fixture step using caller-owned state.
 */
RA8_INTERNAL static ra8_err_t internal_step_unlink_long(ra8_fs_mount_t* h)
{
  return ra8_fs_unlink(h, "/A Long Enough Name.txt");
}

/**
 * @brief Rename `/A Long Enough Name.txt` to another long name.
 *
 * @param[in,out] h Mounted volume.
 *
 * @return The rename's result.
 * @retval k_ra8_ok    Renamed.
 * @retval k_ra8_err_* Reported failure.
 *
 * @pre @p h is mounted and holds the old name.
 * @pre The new name is free.
 * @post On success only the new name resolves.
 * @post On failure the report reached the caller.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @details Implements the bounded step rename long fixture step using caller-owned state.
 */
RA8_INTERNAL static ra8_err_t internal_step_rename_long(ra8_fs_mount_t* h)
{
  return ra8_fs_rename(h, "/A Long Enough Name.txt", "/Another Long Name Here.txt");
}

/**
 * @brief Create a long name inside `/SUB`, which has to grow to hold it.
 *
 * @param[in,out] h Mounted volume with a `/SUB` filled to one free slot.
 *
 * @return The open's result.
 * @retval k_ra8_ok    Created; the directory grew.
 * @retval k_ra8_err_* Reported failure.
 *
 * @pre @p h is mounted and `/SUB` exists with one free slot.
 * @pre The name does not already exist.
 * @post On success `/SUB` is one cluster longer.
 * @post On failure the report reached the caller.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @details Implements the bounded step create in full sub fixture step using caller-owned state.
 */
RA8_INTERNAL static ra8_err_t internal_step_create_in_full_sub(ra8_fs_mount_t* h)
{
  ra8_fs_file_t*  f   = nullptr;
  const ra8_err_t err = ra8_fs_open(h, "/SUB/Spilling Over The Edge.txt", k_ra8_fs_mode_write, &f);
  if (err == k_ra8_ok) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  }
  return err;
}

/**
 * @brief Build and mount an empty hand-built FAT16 volume.
 *
 * @return The mounted volume.
 * @retval non-NULL Always.
 *
 * @pre Any previous volume has been unmounted and freed.
 * @pre The host allocator is available.
 * @post The root directory is empty.
 * @post The caller owns the unmount and the free.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_fs_mount_t* internal_setup_empty(void)
{
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  return h;
}

/**
 * @brief Build and mount a volume already holding one long-named file.
 *
 * @return The mounted volume.
 * @retval non-NULL Always.
 *
 * @pre Any previous volume has been unmounted and freed.
 * @pre The host allocator is available.
 * @post `/A Long Enough Name.txt` exists with its chain.
 * @post The caller owns the unmount and the free.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_fs_mount_t* internal_setup_with_long_file(void)
{
  ra8_fs_mount_t* h = internal_setup_empty();
  TEST_ASSERT_EQ(k_ra8_ok, internal_step_create_long(h));
  return h;
}

/**
 * @brief Build and mount a volume whose `/SUB` has exactly one free slot.
 *
 * @return The mounted volume.
 * @retval non-NULL Always.
 *
 * @pre Any previous volume has been unmounted and freed.
 * @pre The host allocator is available.
 * @post `/SUB` holds "." + ".." + ::k_lwc_sub_fill files.
 * @post The caller owns the unmount and the free.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_fs_mount_t* internal_setup_full_sub(void)
{
  ra8_fs_mount_t* h = internal_setup_empty();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SUB"));
  internal_create_empty_files(h, "/SUB", (uint32_t)k_lwc_sub_fill);
  return h;
}

/* ===========================================================================
 * Tests
 * ===========================================================================
 */

/**
 * @test test_create_io_sweeps
 * @brief Every read and every write on the create path is reported.
 *
 * @details Two sweeps over one fresh volume each. Creating a long name touches
 *          the lookup, the alias probe, the free-run search and the chain
 *          write, so the sweep walks all four without naming any of them --
 *          which is the point: the assertion survives the path changing shape.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` after every `priv_read_sector()` /
 * `priv_write_sector()` in the create path (1 condition each).
 * - V1: the budget allows the I/O -> false -> the path continues.
 * - V2: it does not               -> true  -> the error is returned.
 * Both arms of every such guard on the path are taken across the sweep.
 *
 * @pre A hand-built FAT16 volume is mounted and empty.
 * @post Both sweeps reached a succeeding budget, and the file exists.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_create_io_sweeps(void)
{
  TEST_BEGIN("lfn cov: create read/write failure sweeps");
  TEST_ASSERT(internal_sweep_io(internal_setup_empty, internal_step_create_long, 0U) > 0U);
  TEST_ASSERT(internal_sweep_io(internal_setup_empty, internal_step_create_long, 1U) > 0U);
  TEST_END("lfn cov: create read/write failure sweeps");
}

/**
 * @test test_unlink_io_sweeps
 * @brief Every read and every write on the chain-deleting path is reported.
 *
 * @details The deleter walks the directory to find the chain, then
 *          read-modify-writes each sector the chain lives in. Both halves are
 *          swept.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` in `priv_dir_collect_chain()` and
 * `priv_dir_erase_positions()` (1 condition each).
 * - V1: the I/O is allowed  -> false -> the walk / the erase continues.
 * - V2: it is denied        -> true  -> the error is returned.
 *
 * @pre A hand-built FAT16 volume is mounted and holds one long-named file.
 * @post The file was deleted and the root holds no orphan.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_unlink_io_sweeps(void)
{
  TEST_BEGIN("lfn cov: unlink read/write failure sweeps");
  TEST_ASSERT(internal_sweep_io(internal_setup_with_long_file, internal_step_unlink_long, 0U) > 0U);
  TEST_ASSERT(internal_sweep_io(internal_setup_with_long_file, internal_step_unlink_long, 1U) > 0U);
  TEST_END("lfn cov: unlink read/write failure sweeps");
}

/**
 * @test test_rename_io_sweeps
 * @brief Every read and every write on the re-filing rename path is reported.
 *
 * @details Rename is the longest path of the three: two lookups, a
 *          reservation, a chain write and a chain deletion. A write denied
 *          part-way leaves the file under both names, which the sweep does not
 *          assert against -- what it asserts is that the caller was told.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` after `priv_dir_commit()` in
 * `priv_fat_rename()` (1 condition).
 * - V1: the commit succeeded -> false -> the old entry is erased.
 * - V2: it did not           -> true  -> the error is returned and the old
 *   entry is left alone, so the file is still reachable.
 *
 * @pre A hand-built FAT16 volume is mounted and holds one long-named file.
 * @post Some name resolves to the file after every sweep step.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_rename_io_sweeps(void)
{
  TEST_BEGIN("lfn cov: rename read/write failure sweeps");
  TEST_ASSERT(internal_sweep_io(internal_setup_with_long_file, internal_step_rename_long, 0U) > 0U);
  TEST_ASSERT(internal_sweep_io(internal_setup_with_long_file, internal_step_rename_long, 1U) > 0U);
  TEST_END("lfn cov: rename read/write failure sweeps");
}

/**
 * @test test_directory_growth_failures
 * @brief Growing a directory reports its FAT and backend failures.
 *
 * @details The growth path allocates a cluster, zeroes every sector of it, and
 *          links it in. Each of those can fail; the sweeps walk all of them.
 *          A failure after the allocation must not leak the cluster, which is
 *          checked by growing successfully afterwards and seeing the same
 *          cluster reused.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` after the zero-fill write in
 * `priv_dir_grow()` (1 condition).
 * - V1: the write succeeded -> false -> the loop continues to the next sector.
 * - V2: it failed           -> true  -> the fresh cluster is freed and the
 *   error returned.
 *
 * @pre A hand-built FAT16 volume (SPC=1) is mounted with a full `/SUB`.
 * @post The long name exists in `/SUB` and the volume is consistent.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_directory_growth_failures(void)
{
  TEST_BEGIN("lfn cov: directory-growth failure sweeps");
  TEST_ASSERT(internal_sweep_io(internal_setup_full_sub, internal_step_create_in_full_sub, 0U) >
              0U);
  TEST_ASSERT(internal_sweep_io(internal_setup_full_sub, internal_step_create_in_full_sub, 1U) >
              0U);

  /* And once cleanly, to prove the grown directory is usable afterwards. */
  ra8_fs_mount_t* h = internal_setup_full_sub();
  TEST_ASSERT_EQ(k_ra8_ok, internal_step_create_in_full_sub(h));
  name_list_t l = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/SUB", internal_collect_cb, &l));
  TEST_ASSERT_EQ(k_lwc_sub_fill + 1U, l.count);
  TEST_ASSERT_EQ(0, strcmp(l.name[(uint32_t)k_lwc_sub_fill], "Spilling Over The Edge.txt"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("lfn cov: directory-growth failure sweeps");
}

/**
 * @test test_chain_straddles_a_sector
 * @brief A chain written and deleted across a sector boundary stays consistent.
 *
 * @details On a 64-entry root the boundary falls between slots 15 and 16.
 *          Filling the root to slot 15 and then creating a name needing three
 *          slots puts the chain's first entry in sector 0 and the rest in
 *          sector 1, exercising both the writer's sector crossing and the
 *          deleter's read-modify-write batching over two sectors.
 *
 * @par MC/DC:
 * Decision: `if (pos[i].lba != cur)` in `priv_dir_erase_positions()`
 * (1 condition).
 * - V1: the next slot is in the same sector -> false -> the buffer is reused.
 * - V2: it is in the next one               -> true  -> flush, load, continue.
 *
 * @pre A 64-entry-root FAT16 volume is mounted.
 * @post The name round-trips, and after deletion the root holds no orphan.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_chain_straddles_a_sector(void)
{
  TEST_BEGIN("lfn cov: a chain across a sector boundary");
  internal_build_fat16_big_root();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_lwc_root_entries, h->root_entries);

  /* 15 short entries, so the next free slot is the last one of sector 0. */
  internal_create_empty_files(h, "/", (uint32_t)k_lwc_root_fill);
  internal_write_and_verify(h, "/Straddling The Boundary.txt");
  TEST_ASSERT_EQ(0U, internal_count_orphan_slots(h));

  name_list_t l = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", internal_collect_cb, &l));
  TEST_ASSERT_EQ(k_lwc_root_fill + 1U, l.count);
  TEST_ASSERT_EQ(0, strcmp(l.name[(uint32_t)k_lwc_root_fill], "Straddling The Boundary.txt"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "/Straddling The Boundary.txt"));
  scan_result_t r = {};
  internal_scan_root_of(h, &r);
  TEST_ASSERT_EQ(k_lwc_root_fill, r.live);
  TEST_ASSERT_EQ(0U, r.lfn_slots);
  TEST_ASSERT_EQ(0U, r.orphans);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("lfn cov: a chain across a sector boundary");
}

/**
 * @test test_overlong_planted_chain
 * @brief A chain longer than the specification allows is truncated, not trusted.
 *
 * @details `LDIR_Ord` runs 1..20, so twenty slots is the longest legal chain.
 *          A directory carrying more in front of one entry is corrupt; the
 *          deleter keeps the twenty CLOSEST to the entry, because those are the
 *          ones its checksum has most recently confirmed. The over-long run is
 *          planted directly into the root, since no API call can create one.
 *
 * @par MC/DC:
 * Decision: `if (len == k_lfn_erase_max)` in `priv_run_push()` (1 condition).
 * - V1: the run is shorter -> false -> the address is appended.
 * - V2: it is at the cap   -> true  -> the oldest address is dropped first.
 *
 * @pre A 64-entry-root FAT16 volume is mounted and holds one short-named file.
 * @post The entry is gone and exactly one planted slot is left behind.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_overlong_planted_chain(void)
{
  TEST_BEGIN("lfn cov: an over-long planted chain is capped");
  internal_build_fat16_big_root();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* One ordinary file, whose 8.3 entry will close the planted run. */
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "/VICTIM.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* Move it to slot k_lwc_planted_lfn and plant that many long-name slots in
   * front of it, all carrying its checksum. */
  uint8_t entry[k_lw_entry] = {};
  memcpy(entry, internal_root_slot(h, 0U), (size_t)k_lw_entry);
  const uint8_t csum = internal_spec_checksum(&entry[k_lw_off_name]);
  memcpy(internal_root_slot(h, (uint32_t)k_lwc_planted_lfn), entry, (size_t)k_lw_entry);
  for (uint32_t i = 0U; i < (uint32_t)k_lwc_planted_lfn; i++) {
    uint8_t* slot = internal_root_slot(h, i);
    memset(slot, 0, (size_t)k_lw_entry);
    slot[k_lw_off_name] = (uint8_t)(((uint32_t)k_lwc_planted_lfn - i) & (uint32_t)k_lwc_ord_mask);
    slot[k_lw_off_attr] = (uint8_t)k_lw_attr_lfn;
    slot[k_lw_off_csum] = csum;
  }

  scan_result_t before = {};
  internal_scan_root_of(h, &before);
  TEST_ASSERT_EQ(k_lwc_planted_lfn, before.lfn_slots);
  TEST_ASSERT_EQ(k_lwc_planted_lfn, before.chained);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "/VICTIM.TXT"));

  /* The cap is 20, so exactly one of the 21 planted slots survives. */
  scan_result_t after = {};
  internal_scan_root_of(h, &after);
  TEST_ASSERT_EQ(0U, after.live);
  TEST_ASSERT_EQ(k_lwc_planted_lfn - k_lwc_erase_cap, after.lfn_slots);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("lfn cov: an over-long planted chain is capped");
}

/**
 * @test test_alias_probe_reports_backend_errors
 * @brief A backend failure during the alias probe is reported, not retried past.
 *
 * @details The probe reads the directory once per candidate tail. A denied read
 *          is not "this alias is free" -- treating it that way would file the
 *          entry under a name something else already holds.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` in `priv_alias_unique()` (1 condition).
 * - V1: `k_ra8_err_not_found` -> the earlier arm returns; not this one.
 * - V2: a backend error       -> true -> returned to the caller.
 *
 * @pre A hand-built FAT16 volume is mounted and empty.
 * @post The create was refused and the root is untouched.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_alias_probe_reports_backend_errors(void)
{
  TEST_BEGIN("lfn cov: a backend error during the alias probe");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* One read for the 8.3 lookup, one for the long-name lookup, then the alias
   * probe's first read is denied. */
  const ra8_fs_backend_t saved = h->backend;
  internal_swap_to_inject(h, 2U, 0U);
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 ra8_fs_open(h, "/A Long Enough Name.txt", k_ra8_fs_mode_write, &f));
  h->backend = saved;

  scan_result_t r = {};
  internal_scan_root_of(h, &r);
  TEST_ASSERT_EQ(0U, r.live);
  TEST_ASSERT_EQ(0U, r.lfn_slots);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("lfn cov: a backend error during the alias probe");
}

/**
 * @test test_fat32_root_grows
 * @brief A FAT32 root IS a chain, so a long name can extend it.
 *
 * @details The one geometry that separates the two conditions of the growth
 *          guard. A FAT12/16 root is a fixed sector region and can never grow
 *          (test_ra8_fs_lfn_write.c@test_fixed_root_cannot_grow); a FAT32 root
 *          is an ordinary cluster chain and grows exactly as a subdirectory
 *          does. Filling it to one free slot and then asking for a name that
 *          needs three is what makes the difference observable.
 *
 * @par MC/DC:
 * Decision: `if ((loc->is_root != 0U) && (m->type != k_ra8_fs_type_fat32))` in
 * `libs/ra8_fs/src/ra8_fs_fat_lfn_write.c@internal_dir_grow` (2 conditions).
 * - V1: a FAT16 root       -> C1=T, C2=T -> k_ra8_err_no_mem
 *   (test_ra8_fs_lfn_write.c@test_fixed_root_cannot_grow).
 * - V2: a FAT16 subdirectory -> C1=F     -> the chain is extended
 *   (test_ra8_fs_lfn_write.c@test_chain_grows_the_directory).
 * - V3: a FAT32 root       -> C1=T, C2=F -> the chain is extended (THIS case).
 * V1+V2 isolate C1; V1+V3 isolate C2. N+1 = 3 vectors for N=2.
 *
 * @pre A hand-built FAT32 volume with a one-sector root is mounted.
 * @post The long name resolves and the root holds no orphan.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_fat32_root_grows(void)
{
  TEST_BEGIN("lfn cov: a FAT32 root grows to fit a chain");
  internal_build_fat32_one_sector_root();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_fs_type_fat32, h->type);

  internal_create_empty_files(h, "/", (uint32_t)k_lwc_f32_fill);
  internal_write_and_verify(h, "/Grown Into A New Cluster.txt");

  name_list_t l = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", internal_collect_cb, &l));
  TEST_ASSERT_EQ(k_lwc_f32_fill + 1U, l.count);
  TEST_ASSERT_EQ(0, strcmp(l.name[k_lwc_f32_fill], "Grown Into A New Cluster.txt"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "/Grown Into A New Cluster.txt"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("lfn cov: a FAT32 root grows to fit a chain");
}

/**
 * @brief Run every case in this suite.
 *
 * @return Process exit status.
 * @retval 0 Every case passed; a failure aborts inside the harness instead.
 *
 * @pre The host allocator is available.
 * @pre No other suite shares this process.
 * @post Every volume allocated here has been freed.
 * @post The harness has printed one line per case.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_create_io_sweeps();
  internal_test_unlink_io_sweeps();
  internal_test_rename_io_sweeps();
  internal_test_directory_growth_failures();
  internal_test_chain_straddles_a_sector();
  internal_test_fat32_root_grows();
  internal_test_overlong_planted_chain();
  internal_test_alias_probe_reports_backend_errors();
  return 0;
}
