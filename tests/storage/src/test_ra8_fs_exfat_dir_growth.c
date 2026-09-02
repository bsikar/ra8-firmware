/**
 * @file test_ra8_fs_exfat_dir_growth.c
 * @brief exFAT directories grow past their first cluster (#677).
 *
 * @details
 * Before #677 an exFAT directory was born owning exactly ONE cluster and never
 * grew: `priv_exfat_find_dir_space()` reported `k_ra8_err_no_mem` the moment
 * that cluster's entry sets were full, so a folder on a card with gigabytes free
 * held only as many files as fit a single cluster -- about 42 short-named files
 * on the 4 KiB-cluster geometry the formatter picks for the 64 MiB fixture. This
 * suite pins the behaviour that replaced it.
 *
 * The scenarios, in the order the feature is exercised:
 *
 *  - **The single-cluster ceiling, and the create that used to fail.** A
 *    directory is filled to exactly the pre-#677 ceiling -- proven by the
 *    allocation census showing it still owns one cluster -- and then the NEXT
 *    create is asserted to SUCCEED and to grow the directory to two clusters.
 *    That create is the deliberate negative control: it is the exact call that
 *    returned `k_ra8_err_no_mem` before the fix.
 *  - **A bitmap-sector boundary.** The volume is filled so a directory's growth
 *    allocates clusters whose bitmap bits straddle the 4096-cluster boundary
 *    between two allocation-bitmap sectors, and the whole run is later freed
 *    across that boundary -- exercising the per-sector read-modify-write in the
 *    bitmap mark and clear paths.
 *  - **The NoFatChain -> FAT-chain transition.** A blocker parked on the cluster
 *    adjacent to a directory's tail forces growth off the contiguous fast path
 *    and onto a real FAT chain, then the next growth extends that chain. These
 *    two, plus the stay-contiguous growth in the ceiling test, drive all three
 *    MC/DC vectors of the transition decision in `priv_exfat_dir_link`.
 *  - **`rmdir` frees every cluster.** A multi-cluster directory is emptied and
 *    removed, and the census is asserted back to its pre-`mkdir` value -- no
 *    cluster of a grown directory is left behind.
 *
 * Every scenario ends with `internal_exfat_verify()`, the structural scan that recomputes
 * each entry set's SetChecksum and NameHash and compares the referenced clusters
 * against the allocation bitmap in both directions -- the pair of questions
 * `fsck.exfat` asks. Point `RA8_FS_EXFAT_DUMP` at a directory to also dump each
 * image for a real `fsck.exfat -n` pass.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs_exfat_dir_test_util.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "unity_minimal.h"

/**
 * @enum growth_const_t
 * @brief Sizes, seeds and layout constants for the directory-growth suite.
 *
 * @details ::k_growth_bmp_bits_per_sector is the number of cluster bits one
 *          512-byte allocation-bitmap sector holds (512 * 8), so a directory
 *          whose run straddles that many clusters straddles two bitmap sectors.
 *          ::k_growth_entries_per_file is the entry count of one short-named
 *          file set (File + Stream + one Name), which is what fixes how many
 *          files fit a directory cluster.
 *
 * @invariant k_growth_seed_a != k_growth_seed_b.
 * @see test_fill_past_one_cluster()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_growth_entries_per_file    = 3U,    /**< File + Stream + one Name entry.          */
  k_growth_bmp_bits_per_sector = 4096U, /**< Cluster bits in one 512 B bitmap sector. */
  k_growth_boundary_gap        = 3U,    /**< Clusters below the boundary to start.    */
  k_growth_span_clusters       = 4U,    /**< Directory clusters filled in a run.      */
  k_growth_extra_files         = 5U,    /**< Files past a whole-cluster count.        */
  k_growth_blk_payload         = 64U,   /**< Blocker file byte count (one cluster).   */
  k_growth_seed_a              = 0x53U, /**< Blocker fill seed.                       */
  k_growth_seed_b              = 0x27U, /**< Filler fill seed.                        */
  k_growth_path_cap            = 40U,   /**< Path buffer bytes.                       */
  k_growth_room_margin         = 32U,   /**< Cluster headroom the fixture must have.  */
} growth_const_t;

/**
 * @brief Mount the fixture volume, asserting it is exFAT.
 *
 * @return The mounted volume.
 * @retval non-NULL The mount handle.
 *
 * @pre `build_exfat_volume()` has run.
 * @pre No other mount is outstanding.
 * @post The returned handle is an exFAT mount.
 * @post No on-disk state is modified.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_fs_mount_t* internal_mount_fixture(void)
{
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_fs_type_exfat, h->type);
  return h;
}

/**
 * @brief Bytes one cluster of the mounted volume holds.
 *
 * @param[in] h Mounted exFAT volume.
 *
 * @return Cluster size in bytes.
 * @retval >0 The cluster byte count.
 *
 * @pre @p h is non-NULL and mounted.
 * @pre `h->sectors_per_cluster` is non-zero.
 * @post No state is modified.
 * @post The result is `sectors_per_cluster * 512`.
 *
 * @since 0.1.0 @details Implements the bounded cluster bytes fixture step using caller-owned state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static uint32_t internal_cluster_bytes(const ra8_fs_mount_t* h)
{
  return h->sectors_per_cluster * (uint32_t)k_mut_block_size;
}

/**
 * @brief Short-named files that fit one directory cluster.
 *
 * @param[in] h Mounted exFAT volume.
 *
 * @return The per-cluster file ceiling.
 * @retval >0 `internal_entries_per_cluster / 3`.
 *
 * @pre @p h is non-NULL and mounted.
 * @pre A short name occupies one Name entry (a 3-entry set).
 * @post No state is modified.
 * @post The result is the pre-#677 single-cluster directory ceiling.
 *
 * @since 0.1.0 @details Implements the bounded files per cluster fixture step using caller-owned state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static uint32_t internal_files_per_cluster(const ra8_fs_mount_t* h)
{
  return internal_entries_per_cluster(h) / (uint32_t)k_growth_entries_per_file;
}

/**
 * @brief Create one empty (zero-length) file at @p path.
 *
 * @details Opens for write and closes without writing, which lays down a
 *          File + Stream + Name set that owns NO data cluster -- the cheapest
 *          way to consume a directory's entry slots without also consuming heap
 *          clusters, so the allocation census reflects directory growth ALONE.
 *
 * @param[in] h    Mounted exFAT volume.
 * @param[in] path File path to create.
 *
 * @pre @p h and @p path are non-NULL; the parent directory exists.
 * @pre @p path does not already exist.
 * @post @p path resolves to a zero-length file owning no clusters.
 * @post The create and close both succeeded (asserted).
 *
 * @since 0.1.0 @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_make_empty_file(ra8_fs_mount_t* h, const char* path)
{
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, path, k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
}

/**
 * @brief Create @p count empty files named `<dir>/F<n>` for n in `[first,...)`.
 *
 * @param[in] h     Mounted exFAT volume.
 * @param[in] dir   Parent directory path (no trailing slash).
 * @param[in] first Index of the first file name.
 * @param[in] count Number of files to create.
 *
 * @pre @p h and @p dir are non-NULL; @p dir exists.
 * @pre The names `<dir>/F<first..first+count-1>` do not already exist.
 * @post @p dir gains @p count empty files.
 * @post Each create succeeded (asserted).
 *
 * @since 0.1.0 @details Implements the bounded fill dir fixture step using caller-owned state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void
internal_fill_dir(ra8_fs_mount_t* h, const char* dir, uint32_t first, uint32_t count)
{
  for (uint32_t i = 0U; i < count; i++) {
    char path[k_growth_path_cap] = {};
    (void)snprintf(path, sizeof(path), "%s/F%05u", dir, first + i);
    internal_make_empty_file(h, path);
  }
}

/**
 * @brief Remove @p count files named `<dir>/F<n>` for n in `[first,...)`.
 *
 * @param[in] h     Mounted exFAT volume.
 * @param[in] dir   Parent directory path (no trailing slash).
 * @param[in] first Index of the first file name.
 * @param[in] count Number of files to remove.
 *
 * @pre @p h and @p dir are non-NULL; the named files exist.
 * @pre The names were created by ::fill_dir.
 * @post The @p count files no longer resolve.
 * @post Each unlink succeeded (asserted).
 *
 * @since 0.1.0 @details Implements the bounded drain dir fixture step using caller-owned state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void
internal_drain_dir(ra8_fs_mount_t* h, const char* dir, uint32_t first, uint32_t count)
{
  for (uint32_t i = 0U; i < count; i++) {
    char path[k_growth_path_cap] = {};
    (void)snprintf(path, sizeof(path), "%s/F%05u", dir, first + i);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, path));
  }
}

/**
 * @brief Write a file that occupies exactly @p clusters contiguous heap clusters.
 *
 * @details Streams @p clusters whole-cluster writes into a fresh file so the
 *          allocator hands out a contiguous run out of the next-free hint. Used
 *          to push the free-cluster frontier to a chosen point so a later
 *          directory grows across an allocation-bitmap sector boundary.
 *
 * @param[in] h        Mounted exFAT volume.
 * @param[in] path     File path to create.
 * @param[in] clusters Number of clusters to consume (> 0).
 *
 * @pre @p h and @p path are non-NULL; the volume has @p clusters free.
 * @pre @p path does not already exist.
 * @post @p path holds `clusters * cluster bytes` bytes.
 * @post Every write and the close succeeded (asserted).
 *
 * @since 0.1.0 @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void
internal_consume_clusters(ra8_fs_mount_t* h, const char* path, uint32_t clusters)
{
  const uint32_t cbytes = internal_cluster_bytes(h);
  uint8_t*       buf    = (uint8_t*)malloc((size_t)cbytes);
  if (buf == nullptr) {
    TEST_FAIL_FMT("%s", "malloc failed for the filler buffer");
    return;
  }
  for (uint32_t i = 0U; i < cbytes; i++) {
    buf[i] = (uint8_t)((i * (uint32_t)k_growth_seed_b) + (uint32_t)k_growth_seed_b);
  }
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, path, k_ra8_fs_mode_write, &f));
  for (uint32_t c = 0U; c < clusters; c++) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, buf, cbytes));
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  free(buf);
}

/**
 * @brief Fill @p buf with a deterministic seed-dependent pattern.
 *
 * @param[out] buf  Destination of at least @p len bytes.
 * @param[in]  len  Byte count.
 * @param[in]  seed Generator seed.
 *
 * @pre @p buf addresses @p len writable bytes.
 * @pre @p len is the exact buffer length.
 * @post Every byte of `buf[0..len-1]` is written.
 * @post No other state is modified.
 *
 * @since 0.1.0 @details Implements the bounded fill pattern fixture step using caller-owned state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_fill_pattern(uint8_t* buf, uint32_t len, uint8_t seed)
{
  for (uint32_t i = 0U; i < len; i++) {
    buf[i] = (uint8_t)((i * (uint32_t)k_growth_entries_per_file) + seed);
  }
}

/**
 * @brief Read the first user directory's Stream flags out of the root image.
 *
 * @details The first directory created in an otherwise-empty root lands at root
 *          entry slot 3 (after bitmap, up-case and label), so its Stream entry
 *          is slot 4. Reading its GeneralSecondaryFlags directly from the image
 *          is how the conversion test proves the NoFatChain bit flipped.
 *
 * @param[in] h Mounted exFAT volume.
 *
 * @return The GeneralSecondaryFlags byte.
 * @retval 0..0xFF The flag byte.
 *
 * @pre @p h is non-NULL and mounted; the first user entry is a directory.
 * @pre `s_disk.bytes` holds the image.
 * @post No state is modified.
 *
 * @since 0.1.0 @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static uint8_t internal_first_dir_flags(const ra8_fs_mount_t* h)
{
  const uint32_t strm_off = internal_root_byte(h, (uint32_t)k_mut_root_strm0_idx);
  return s_disk.bytes[strm_off + (uint32_t)k_mut_strm_off_flags];
}

/**
 * @brief Read the first user directory's DataLength out of the root image.
 *
 * @param[in] h Mounted exFAT volume.
 *
 * @return The DataLength field (the directory's allocation in bytes).
 * @retval 0..UINT32_MAX The recorded length.
 *
 * @pre @p h is non-NULL and mounted; the first user entry is a directory.
 * @pre `s_disk.bytes` holds the image.
 * @post No state is modified.
 *
 * @since 0.1.0 @details Implements the bounded first dir datalen fixture step using caller-owned state. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static uint32_t internal_first_dir_datalen(const ra8_fs_mount_t* h)
{
  const uint32_t strm_off = internal_root_byte(h, (uint32_t)k_mut_root_strm0_idx);
  return internal_disk_get_u32le(strm_off + (uint32_t)k_mut_strm_off_dlen);
}

/* ---- the single-cluster ceiling and the create that used to fail --------- */

/**
 * @test test_fill_past_one_cluster
 * @brief A directory grows past its first cluster; the ceiling+1 create succeeds.
 *
 * @details The census brackets the behaviour change. `mkdir` takes one cluster;
 *          filling the directory to exactly `files_per_cluster` short-named files
 *          leaves it at one cluster -- the pre-#677 ceiling, asserted by the
 *          census. The NEXT create is the deliberate negative control: before
 *          #677 it returned `k_ra8_err_no_mem` on a volume with gigabytes free;
 *          it now succeeds and the census shows the directory grew to two
 *          clusters. Filling well past the ceiling then keeps every create
 *          succeeding and the structural scan clean.
 *
 * @par MC/DC:
 * No compound decision is introduced here. The first growth drives the
 * stay-contiguous arm of the transition decision, whose vectors are recorded on
 * test_convert_forces_fat_chain: the (nofat=T, adjacent=T) control vector for
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_stream.c@internal_exfat_dir_link`, since the
 * cluster after the directory's tail is free and the run stays NoFatChain.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post Every create past the single-cluster ceiling succeeds.
 * @post The census reflects exactly the clusters the directory grew into.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_fill_past_one_cluster(void)
{
  TEST_BEGIN("exfat grow: a directory grows past its first cluster");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h        = internal_mount_fixture();
  const uint32_t  baseline = internal_alloc_bitmap_used(h);
  const uint32_t  fpc      = internal_files_per_cluster(h);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/BIG"));
  TEST_ASSERT_EQ(baseline + 1U, internal_alloc_bitmap_used(h)); /* one cluster, as ever */

  /* Fill to exactly the pre-#677 ceiling: the directory must still own its one
   * cluster, because these sets all fit inside it. */
  internal_fill_dir(h, "/BIG", 0U, fpc);
  TEST_ASSERT_EQ(baseline + 1U, internal_alloc_bitmap_used(h));
  internal_exfat_verify(h, "grow_ceiling_reached");

  /* NEGATIVE CONTROL: this exact create returned k_ra8_err_no_mem before #677.
   * It now succeeds and the directory grows to a second cluster. */
  internal_make_empty_file(h, "/BIG/F00042ceil");
  TEST_ASSERT_EQ(baseline + 2U, internal_alloc_bitmap_used(h));

  /* Keep going well past the ceiling, spanning several clusters. */
  const uint32_t total = (fpc * k_growth_span_clusters) + (uint32_t)k_growth_extra_files;
  internal_fill_dir(h, "/BIG", fpc + 1U, total - (fpc + 1U));

  name_ctx_t ctx = {};
  TEST_ASSERT_EQ(total, internal_list_names(h, "/BIG", &ctx));
  const uint32_t expect_clusters = (total + fpc - 1U) / fpc;
  TEST_ASSERT_EQ(baseline + expect_clusters, internal_alloc_bitmap_used(h));

  /* A sampling round-trips: still files, still zero length, still resolvable. */
  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/BIG/F00000", &st));
  TEST_ASSERT_EQ(false, st.is_directory);
  TEST_ASSERT_EQ(0U, st.size_bytes);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/BIG/F00042ceil", &st));
  TEST_ASSERT_EQ(false, st.is_directory);

  internal_exfat_verify(h, "grow_past_one_cluster");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat grow: a directory grows past its first cluster");
}

/* ---- a bitmap-sector boundary -------------------------------------------- */

/**
 * @test test_growth_across_bitmap_sector_boundary
 * @brief Directory growth allocates and frees across a bitmap-sector boundary.
 *
 * @details One allocation-bitmap sector maps 4096 clusters, so a directory whose
 *          run straddles cluster index 4096 straddles two bitmap sectors. A
 *          filler file pushes the free-cluster frontier to just below that
 *          boundary; a directory created there then grows across it, and is
 *          later freed across it. The structural scan afterwards would flag any
 *          bit the growth set in the wrong sector -- as an orphan (a set bit no
 *          set references) or a dangling reference -- and the final census
 *          proves every cluster came back.
 *
 * @par MC/DC:
 * No compound decision is introduced here. The growth exercises the bitmap
 * read-modify-write across a sector edge in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_write.c@priv_exfat_bitmap_mark` and the
 * clear across it in `priv_exfat_bitmap_clear`; both are single-condition
 * sector-switch guards driven here for the first time at a non-zero sector.
 *
 * @pre A freshly formatted 64 MiB exFAT volume with room past the boundary.
 * @post Every create across the boundary succeeds and the scan is clean.
 * @post `rmdir` and the filler removal return the census to baseline.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_growth_across_bitmap_sector_boundary(void)
{
  TEST_BEGIN("exfat grow: growth spans an allocation-bitmap sector boundary");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h        = internal_mount_fixture();
  const uint32_t  baseline = internal_alloc_bitmap_used(h);
  const uint32_t  fpc      = internal_files_per_cluster(h);

  /* The fixture must reach past the first bitmap sector for this to mean
   * anything; the 64 MiB / 4 KiB geometry has ~16000 clusters. */
  TEST_ASSERT_EQ(1U,
                 (h->count_of_clusters >
                  ((uint32_t)k_growth_bmp_bits_per_sector + (uint32_t)k_growth_room_margin))
                   ? 1U
                   : 0U);

  /* Push the frontier to a few clusters below the boundary. The used clusters
   * of a freshly formatted volume are its first `baseline` ones, so the frontier
   * index equals the census. */
  const uint32_t target  = (uint32_t)k_growth_bmp_bits_per_sector - (uint32_t)k_growth_boundary_gap;
  const uint32_t to_fill = target - baseline;
  internal_consume_clusters(h, "/FILL.BIN", to_fill);
  TEST_ASSERT_EQ(target, internal_alloc_bitmap_used(h));

  /* A directory created here starts just below the boundary; filling it across
   * several clusters walks its run over the bitmap-sector edge. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/D"));
  const uint32_t nfiles = fpc * k_growth_span_clusters;
  internal_fill_dir(h, "/D", 0U, nfiles);
  name_ctx_t ctx = {};
  TEST_ASSERT_EQ(nfiles, internal_list_names(h, "/D", &ctx));
  internal_exfat_verify(h, "grow_bitmap_boundary_built");

  /* Free the whole run back across the boundary and confirm nothing leaks. */
  internal_drain_dir(h, "/D", 0U, nfiles);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rmdir(h, "/D"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "/FILL.BIN"));
  TEST_ASSERT_EQ(baseline, internal_alloc_bitmap_used(h));

  internal_exfat_verify(h, "grow_bitmap_boundary_freed");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat grow: growth spans an allocation-bitmap sector boundary");
}

/* ---- the NoFatChain -> FAT-chain transition ------------------------------ */

/**
 * @test test_convert_forces_fat_chain
 * @brief A blocked adjacent cluster forces growth onto a real FAT chain.
 *
 * @details A directory's run is contiguous and carries NoFatChain while it can.
 *          Parking a file on the cluster ADJACENT to the directory's tail makes
 *          the first growth unable to stay contiguous, so it materialises a real
 *          FAT chain and clears the flag -- proven by reading the directory's own
 *          Stream entry back out of the image. The second growth then extends
 *          that chain. The blocker's contents are re-read at the end to prove the
 *          conversion touched only the directory, and `rmdir` frees the chain via
 *          the FAT-walk path.
 *
 * @par MC/DC:
 * Decision: `(*nofat != 0) && (next == tail + 1)`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_stream.c@internal_exfat_dir_link, 2 conditions).
 * - Vector 1: nofat=1, next=tail+1 -> true (stay contiguous). Driven by the
 *   first growth in test_fill_past_one_cluster, where the adjacent cluster is
 *   free. [control]
 * - Vector 2: nofat=0, next=tail+1 -> false (already a chain, extend it).
 *   Driven by the SECOND growth here: the converted directory's tail successor
 *   is free, so `next == tail + 1` yet the run is no longer contiguous. Varies
 *   the first condition against Vector 1.
 * - Vector 3: nofat=1, next!=tail+1 -> false (convert to a chain). Driven by the
 *   FIRST growth here: the adjacent cluster is the parked blocker, so the run
 *   fragments. Varies the second condition against Vector 1.
 * Vectors 1+2 prove the first condition independently affects the outcome;
 * 1+3 prove the same for the second. N+1 = 3 vectors for N=2: minimal MC/DC.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post The directory is a FAT chain after the first growth, and grows further.
 * @post The blocker is intact and `rmdir` frees every cluster of the chain.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_convert_forces_fat_chain(void)
{
  TEST_BEGIN("exfat grow: a blocked adjacent cluster converts the run to a chain");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h        = internal_mount_fixture();
  const uint32_t  baseline = internal_alloc_bitmap_used(h);
  const uint32_t  fpc      = internal_files_per_cluster(h);
  const uint32_t  cbytes   = internal_cluster_bytes(h);

  /* First user entry -> root slot 3, so its Stream is slot 4: readable directly. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/D"));
  TEST_ASSERT_EQ(cbytes, internal_first_dir_datalen(h)); /* one cluster */
  TEST_ASSERT_EQ(1U, ((internal_first_dir_flags(h) & (uint8_t)k_mut_no_fat_bit) != 0U) ? 1U : 0U);

  /* Park a blocker on the cluster the directory would grow into: mkdir left the
   * next-free hint one past the directory's cluster, so this file's data cluster
   * is exactly the directory's tail + 1. */
  uint8_t payload[k_growth_blk_payload] = {};
  internal_fill_pattern(payload, (uint32_t)k_growth_blk_payload, (uint8_t)k_growth_seed_a);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_write_file(h, "/BLK.BIN", payload, (uint32_t)k_growth_blk_payload));

  /* Fill the directory past two clusters: the first growth converts (adjacent
   * cluster blocked), the second extends the fresh chain. */
  const uint32_t nfiles = (fpc * 2U) + 2U;
  internal_fill_dir(h, "/D", 0U, nfiles);

  /* The run is now a real FAT chain: NoFatChain clear, DataLength three clusters. */
  TEST_ASSERT_EQ(0U, ((internal_first_dir_flags(h) & (uint8_t)k_mut_no_fat_bit) != 0U) ? 1U : 0U);
  TEST_ASSERT_EQ(cbytes * 3U, internal_first_dir_datalen(h));

  name_ctx_t ctx = {};
  TEST_ASSERT_EQ(nfiles, internal_list_names(h, "/D", &ctx));

  /* The conversion touched only the directory. */
  ra8_fs_file_t* bf = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "/BLK.BIN", k_ra8_fs_mode_read, &bf));
  uint8_t  got[k_growth_blk_payload] = {};
  uint32_t n                         = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(bf, got, (uint32_t)k_growth_blk_payload, &n));
  TEST_ASSERT_EQ(k_growth_blk_payload, n);
  TEST_ASSERT_EQ(0, memcmp(got, payload, (size_t)k_growth_blk_payload));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(bf));
  internal_exfat_verify(h, "grow_converted_to_chain");

  /* rmdir frees a chained multi-cluster directory through the FAT-walk path. */
  internal_drain_dir(h, "/D", 0U, nfiles);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rmdir(h, "/D"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "/BLK.BIN"));
  TEST_ASSERT_EQ(baseline, internal_alloc_bitmap_used(h));

  internal_exfat_verify(h, "grow_chain_freed");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat grow: a blocked adjacent cluster converts the run to a chain");
}

/* ---- rmdir frees every cluster ------------------------------------------- */

/**
 * @test test_rmdir_multicluster_frees_all
 * @brief `rmdir` of a grown contiguous directory releases every cluster.
 *
 * @details The complement of the chained-`rmdir` in test_convert_forces_fat_chain:
 *          a directory that grew CONTIGUOUSLY across several clusters is emptied
 *          and removed, and the census is asserted back to its pre-`mkdir` value.
 *          A `rmdir` that freed only the first cluster of the run -- or that read
 *          the stale one-cluster DataLength #677 left before it taught growth to
 *          update the Stream entry -- would leave the census high, and the
 *          structural scan would call the rest orphaned space.
 *
 * @par MC/DC:
 * No compound decision lies on this path. It contributes the contiguous arm of
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_mutate.c@priv_exfat_free_clusters`
 * (`nofat == 1`) over a run of more than one cluster -- the arm that clears
 * `ceil(DataLength / cluster bytes)` bits rather than walking a FAT chain.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post The multi-cluster directory and all its clusters are gone.
 * @post The census equals its pre-`mkdir` value.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_rmdir_multicluster_frees_all(void)
{
  TEST_BEGIN("exfat grow: rmdir of a grown contiguous directory frees every cluster");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h        = internal_mount_fixture();
  const uint32_t  baseline = internal_alloc_bitmap_used(h);
  const uint32_t  fpc      = internal_files_per_cluster(h);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/M"));
  const uint32_t nfiles = (fpc * k_growth_span_clusters) + 2U;
  internal_fill_dir(h, "/M", 0U, nfiles);
  const uint32_t clusters = (nfiles + fpc - 1U) / fpc;
  TEST_ASSERT_EQ(baseline + clusters, internal_alloc_bitmap_used(h));
  internal_exfat_verify(h, "grow_multicluster_before_rmdir");

  internal_drain_dir(h, "/M", 0U, nfiles);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rmdir(h, "/M"));
  TEST_ASSERT_EQ(baseline, internal_alloc_bitmap_used(h)); /* every cluster of the run came back */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_stat(h, "/M", &(ra8_fs_stat_t){}));

  internal_exfat_verify(h, "grow_multicluster_after_rmdir");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat grow: rmdir of a grown contiguous directory frees every cluster");
}

/* ---- the volume is full: growth has nowhere to go ------------------------ */

/**
 * @test test_volume_full_reports_no_mem
 * @brief When the volume has no free cluster, a full directory cannot grow.
 *
 * @details The honest floor of the feature: growth extends a directory only
 *          while the VOLUME has a cluster to spare. A directory is filled to its
 *          single-cluster ceiling, the allocation bitmap is then marked entirely
 *          used, and the next create -- of a file and of a subdirectory -- is
 *          asserted to report ::k_ra8_err_no_mem, because the grow it now
 *          attempts finds nowhere to allocate. This is the negative half of the
 *          behaviour the rest of the suite proves: `no_mem` no longer means "the
 *          directory is full", it means "the disk is".
 *
 *          The structural scan is deliberately NOT run here -- the bitmap was
 *          forced inconsistent to stage a full volume, so it would (correctly)
 *          report the clusters it claims but nothing references.
 *
 * @par MC/DC:
 * No compound decision lies on this path. It drives the TRUE arm of
 * `e != k_ra8_ok` after ::priv_exfat_link
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_openw.c@internal_exfat_open_created), the
 * volume-full return of
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_write.c@priv_exfat_find_dir_space`, and the
 * failed cluster pick inside
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_stream.c@priv_exfat_grow_dir`.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post Both the file create and the mkdir report ::k_ra8_err_no_mem.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_volume_full_reports_no_mem(void)
{
  TEST_BEGIN("exfat grow: a full volume leaves a full directory unable to grow");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h   = internal_mount_fixture();
  const uint32_t  fpc = internal_files_per_cluster(h);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/D"));
  internal_fill_dir(h, "/D", 0U, fpc); /* fills the one cluster exactly, no growth yet */

  /* No free cluster anywhere: the next create must grow and cannot. */
  internal_alloc_bitmap_fill(h);

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_open(h, "/D/OVERFLOW", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_mkdir(h, "/D/SUBDIR"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat grow: a full volume leaves a full directory unable to grow");
}

/**
 * @brief Run every exFAT directory-growth test.
 *
 * @return Process exit status.
 * @retval 0 Every test passed (a failure aborts before returning).
 *
 * @pre The host provides malloc for the 64 MiB volume and the filler buffer.
 * @pre No other test binary shares this process.
 * @post Every fixture volume is freed.
 * @post The pass banner has been printed.
 *
 * @since 0.1.0
 */
int main(void)
{
  internal_test_fill_past_one_cluster();
  internal_test_growth_across_bitmap_sector_boundary();
  internal_test_convert_forces_fat_chain();
  internal_test_rmdir_multicluster_frees_all();
  internal_test_volume_full_reports_no_mem();
  return 0;
}
