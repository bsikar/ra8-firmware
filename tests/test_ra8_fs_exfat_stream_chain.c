/**
 * @file test_ra8_fs_exfat_stream_chain.c
 * @brief exFAT streaming write: the NoFatChain transition and the bitmap (#602).
 *
 * @details
 * The allocation half of the streaming work, and the part the old whole-file
 * writer could not do at all: it asked the bitmap for one CONTIGUOUS run and
 * returned `k_ra8_err_no_mem` when the volume had no such run, however much
 * free space it held. A streaming writer cannot make that bargain -- it does
 * not know how long the file will be when it takes its first cluster -- so it
 * has to be able to fragment, and exFAT spells out exactly how:
 *
 *   - while the clusters are consecutive, `GeneralSecondaryFlags` carries
 *     `NoFatChain` and the FAT holds NOTHING about them (exFAT spec sec 7.4.2);
 *   - the moment the next cluster is not the successor of the last, a real FAT
 *     chain has to be laid over every cluster already allocated, the new one
 *     linked on, and only then may the flag come off.
 *
 * These cases force that transition deterministically -- by parking a second
 * file on the successor cluster the first one wants -- and then check the
 * result three ways: the flag is clear, the FAT chain read straight out of the
 * image visits the right clusters in the right order, and the file still reads
 * back byte for byte. A driver that cleared the flag without writing the chain
 * passes the first check and fails the other two.
 *
 * `stream_dump_image()` runs at the end of each scenario, so
 * `RA8_EXFAT_DUMP_DIR` yields one image per case for `fsck.exfat -n` -- which
 * is the check that matters most here, because a half-written chain is exactly
 * what a host checker reports as a cross-linked or broken cluster chain.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "support/fs_exfat_stream_test_util.h"
#include "unity_minimal.h"

/**
 * @enum xsc_const_t
 * @brief Cluster counts, chain bounds and expectations for the chain tests.
 *
 * @details ::k_xsc_bitmap_span is the file length that carries an allocation
 *          past the first 512-byte bitmap SECTOR on this geometry: 512 bytes
 *          is 4096 cluster bits and a cluster is 4096 bytes, so the crossing
 *          happens 16 MiB in. Nothing below that exercises the bitmap's
 *          sector arithmetic at all.
 *
 * @invariant `k_xsc_bitmap_span` is a whole number of clusters plus a tail.
 *
 * @par Example:
 * @code
 * TEST_ASSERT_EQ(k_xsc_flag_chained, flags_byte);
 * @endcode
 *
 * @see test_stream_fragmentation_forces_chain()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_xsc_flag_chained = 0x01U,       /**< AllocationPossible, NoFatChain CLEAR.     */
  k_xsc_flag_contig  = 0x03U,       /**< AllocationPossible | NoFatChain.          */
  k_xsc_chain_max    = 4096U,       /**< Bound on any chain walk in this file.     */
  k_xsc_eoc_min      = 0xFFFFFFF8U, /**< exFAT end-of-chain threshold.             */
  k_xsc_bitmap_span  = 16785408U,   /**< 4098 clusters: past the first bmp sector. */
  k_xsc_probe_last   = 16785407U,   /**< Last byte of ::k_xsc_bitmap_span.         */
  k_xsc_probe_before = 16773120U,   /**< First byte of cluster index 4095.         */
  k_xsc_probe_after  = 16777216U,   /**< First byte of cluster index 4096.         */
  k_xsc_min_chain    = 4U,          /**< Clusters the fragmented file must own.    */
  k_xsc_fill_step    = 0xFFU,       /**< Modulus of the volume-filling byte ramp.  */
} xsc_const_t;

/**
 * @brief Read the FAT entry of @p clus straight out of the volume image.
 *
 * @param[in] h    Mounted exFAT volume.
 * @param[in] clus Cluster number to look up.
 *
 * @return The 32-bit FAT entry.
 * @retval 0..0xFFFFFFFF The stored value, end-of-chain included.
 *
 * @pre @p h is mounted and @p clus addresses a data cluster.
 * @pre The image is present in `s_disk.bytes`.
 * @post No state is modified.
 * @post The value is read whole, with no FAT32 reserved-bit mask applied.
 *
 * @note Reads the image, not the driver -- a driver that masked the value on
 *       the way in could not hide it here.
 * @since 0.1.0
 */
static uint32_t fat_entry(const ra8_fs_mount_t* h, uint32_t clus)
{
  return disk_get_u32le(fat_byte(h, clus));
}

/**
 * @brief Walk a file's FAT chain from @p first and report the clusters visited.
 *
 * @param[in]  h      Mounted exFAT volume.
 * @param[in]  first  Head of the chain.
 * @param[out] out    Receives the visited cluster numbers.
 * @param[in]  cap    Capacity of @p out.
 *
 * @return Number of clusters visited.
 * @retval 1..cap The chain length, stopping at end-of-chain or @p cap.
 *
 * @pre @p h is mounted; @p first is a data cluster; @p out is non-NULL.
 * @pre @p cap is at least 1.
 * @post @p out[0] is @p first.
 * @post No state is modified.
 *
 * @note Bounded by @p cap, so a cyclic chain terminates the walk instead of
 *       the test.
 * @since 0.1.0
 */
static uint32_t chain_walk(const ra8_fs_mount_t* h, uint32_t first, uint32_t* out, uint32_t cap)
{
  uint32_t clus = first;
  uint32_t n    = 0U;
  while (n < cap) {
    out[n] = clus;
    n++;
    const uint32_t next = fat_entry(h, clus);
    if (next >= (uint32_t)k_xsc_eoc_min) {
      break;
    }
    clus = next;
  }
  return n;
}

/**
 * @brief Assert the FAT chain from @p first covers @p total bytes and skips @p taken.
 *
 * @details The three things a materialised chain has to get right, checked
 *          against the image rather than the driver: it is as long as the
 *          file's length requires, it never claims a cluster that belongs to
 *          somebody else, and it ends at an end-of-chain marker. A driver that
 *          cleared `NoFatChain` and wrote no chain at all fails the first of
 *          them immediately.
 *
 * @param[in] h     Mounted exFAT volume.
 * @param[in] first Head of the file's chain.
 * @param[in] total The file's `DataLength` in bytes.
 * @param[in] taken A cluster the chain must NOT visit (the blocker's).
 *
 * @return Nothing; every check is asserted inside.
 *
 * @pre @p h is mounted and @p first is a data cluster.
 * @pre The file is closed, so its entry set is committed.
 * @post No state is modified.
 * @post A pass means the chain is walkable end to end.
 *
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0
 */
static void
expect_chain_over(const ra8_fs_mount_t* h, uint32_t first, uint32_t total, uint32_t taken)
{
  const uint32_t  cbytes = h->sectors_per_cluster * (uint32_t)k_mut_block_size;
  const uint32_t  want   = (total + cbytes - 1U) / cbytes;
  static uint32_t s_seen[k_xsc_chain_max];
  const uint32_t  got = chain_walk(h, first, s_seen, (uint32_t)k_xsc_chain_max);
  TEST_ASSERT_EQ(want, got);
  TEST_ASSERT(got >= (uint32_t)k_xsc_min_chain);
  TEST_ASSERT_EQ(first, s_seen[0]);
  for (uint32_t i = 1U; i < got; i++) {
    if (s_seen[i] == taken) {
      TEST_FAIL_FMT("chain claims cluster %u, which belongs to another file", (unsigned)s_seen[i]);
      break;
    }
  }
  TEST_ASSERT(fat_entry(h, s_seen[got - 1U]) >= (uint32_t)k_xsc_eoc_min);
}

/**
 * @test test_stream_fragmentation_forces_chain
 *
 * @brief A blocked successor drops NoFatChain and leaves a real FAT chain.
 *
 * @details Deterministic rather than statistical. `FIRST.BIN` takes one
 *          cluster; `BLOCK.BIN` then takes the very next one, because the
 *          allocator hands out the lowest free cluster. Appending to
 *          `FIRST.BIN` therefore CANNOT stay contiguous -- its successor is
 *          taken -- so the transition is forced with no reliance on fragment
 *          layout.
 *
 *          Three assertions, and all three are needed. The flag must be clear,
 *          or a reader takes the contiguous fast path over clusters that are
 *          not contiguous. The FAT chain must visit exactly the clusters the
 *          file owns, in order, ending at end-of-chain -- read from the image,
 *          so a driver that cleared the flag and wrote no chain fails here.
 *          And the contents must still read back, which is what proves the
 *          chain the driver wrote is the one the reader follows.
 *
 * @par MC/DC:
 * Decision: `(no_fat_chain != 0) && (next == tail_cluster + 1)` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_stream.c@priv_exfat_link_cluster`
 * (2 conditions). This case supplies BOTH false vectors:
 * - the append's first growth has `no_fat_chain = 1` and a non-successor
 *   cluster, varying the SECOND condition -> the transition;
 * - every growth after it has `no_fat_chain = 0`, varying the FIRST condition
 *   -> an ordinary chain extension.
 * `test_ra8_fs_exfat_stream.c@test_stream_create_multi_cluster` supplies the
 * true vector, completing N+1 = 3 for N = 2.
 *
 * @since 0.1.0
 */
static void test_stream_fragmentation_forces_chain(void)
{
  TEST_BEGIN("exfat stream: blocked successor forces the FAT chain");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "FIRST.BIN", k_ra8_fs_mode_write, &f));
  stream_write_pattern(f,
                       (uint32_t)k_xs_sub_sector,
                       (uint32_t)k_xs_chunk,
                       0U,
                       (uint8_t)k_xs_seed_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  const uint32_t strm  = stream_strm0_off(h);
  const uint32_t first = disk_get_u32le(strm + (uint32_t)k_xs_off_strm_clus);
  TEST_ASSERT_EQ(k_xsc_flag_contig, s_disk.bytes[strm + (uint32_t)k_xs_off_strm_flags]);

  /* Park a second file on exactly the cluster FIRST.BIN would grow into. */
  static const uint8_t s_blocker[k_xs_sub_sector] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "BLOCK.BIN", s_blocker, (uint32_t)k_xs_sub_sector));
  const uint32_t blk_strm = root_byte(h, (uint32_t)k_mut_root_strm0_idx + 3U);
  TEST_ASSERT_EQ(first + 1U, disk_get_u32le(blk_strm + (uint32_t)k_xs_off_strm_clus));

  /* Now grow FIRST.BIN over the block. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "FIRST.BIN", k_ra8_fs_mode_append, &f));
  stream_write_pattern(f,
                       (uint32_t)k_xs_multi_cluster,
                       (uint32_t)k_xs_chunk,
                       (uint32_t)k_xs_sub_sector,
                       (uint8_t)k_xs_seed_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  const uint32_t total = (uint32_t)k_xs_sub_sector + (uint32_t)k_xs_multi_cluster;
  TEST_ASSERT_EQ(k_xsc_flag_chained, s_disk.bytes[strm + (uint32_t)k_xs_off_strm_flags]);
  TEST_ASSERT_EQ(total, disk_get_u32le(strm + (uint32_t)k_xs_off_strm_dlen));
  TEST_ASSERT_EQ(first, disk_get_u32le(strm + (uint32_t)k_xs_off_strm_clus));

  expect_chain_over(h, first, total, first + 1U);

  stream_expect_contents(h, "FIRST.BIN", total, (uint8_t)k_xs_seed_a);
  /* The s_blocker must be untouched by the chain that stepped around it. */
  ra8_fs_stat_t blk = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "BLOCK.BIN", &blk));
  TEST_ASSERT_EQ(k_xs_sub_sector, blk.size_bytes);
  TEST_ASSERT_EQ(first + 1U, blk.first_cluster);

  stream_dump_image("stream_fragmentation_forces_chain", h);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat stream: blocked successor forces the FAT chain");
}

/**
 * @test test_stream_chained_file_reads_and_frees
 *
 * @brief A chained file round-trips through unlink and gives every cluster back.
 *
 * @details The transition is only half the contract: once a file is chained,
 *          the code that FREES it has to walk the FAT rather than assume a run,
 *          or a fragmented file leaves most of its clusters marked used with
 *          nothing pointing at them -- unreclaimable without a reformat. The
 *          bitmap census before and after is the direct measurement.
 *
 * @par MC/DC:
 * Decision: `nofat == 1` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_mutate.c@priv_exfat_free_clusters`
 * (1 condition). This case supplies the FALSE vector -- walk the chain --
 * which nothing could reach before exFAT could produce a chained file at all;
 * `test_ra8_fs_exfat_stream.c@test_stream_truncate_in_place` supplies the true
 * vector.
 *
 * @since 0.1.0
 */
static void test_stream_chained_file_reads_and_frees(void)
{
  TEST_BEGIN("exfat stream: a chained file frees every cluster");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t empty_used = alloc_bitmap_used(h);

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "FRAG.BIN", k_ra8_fs_mode_write, &f));
  stream_write_pattern(f,
                       (uint32_t)k_xs_sub_sector,
                       (uint32_t)k_xs_chunk,
                       0U,
                       (uint8_t)k_xs_seed_b);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  static const uint8_t s_blocker[k_xs_sub_sector] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "PIN.BIN", s_blocker, (uint32_t)k_xs_sub_sector));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "FRAG.BIN", k_ra8_fs_mode_append, &f));
  stream_write_pattern(f,
                       (uint32_t)k_xs_multi_cluster,
                       (uint32_t)k_xs_chunk,
                       (uint32_t)k_xs_sub_sector,
                       (uint8_t)k_xs_seed_b);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  const uint32_t total = (uint32_t)k_xs_sub_sector + (uint32_t)k_xs_multi_cluster;
  stream_expect_contents(h, "FRAG.BIN", total, (uint8_t)k_xs_seed_b);
  const uint32_t strm = stream_strm0_off(h);
  TEST_ASSERT_EQ(k_xsc_flag_chained, s_disk.bytes[strm + (uint32_t)k_xs_off_strm_flags]);

  stream_dump_image("stream_chained_before_unlink", h);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "FRAG.BIN"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "PIN.BIN"));
  TEST_ASSERT_EQ(empty_used, alloc_bitmap_used(h));

  stream_dump_image("stream_chained_after_unlink", h);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat stream: a chained file frees every cluster");
}

/**
 * @test test_stream_multi_cluster_run_materializes
 *
 * @brief A run of SEVERAL clusters gets a chain written over all of it.
 *
 * @details The transition in `test_stream_fragmentation_forces_chain` happens
 *          when the file owns exactly one cluster, so the loop that links a
 *          run to its successors never executes a single iteration. Here the
 *          file owns four before it is blocked, so the loop runs three times
 *          and the chain it leaves has to visit every one of them in order --
 *          which the walk below checks against the image. A driver that only
 *          linked the tail would leave the first three clusters unreachable
 *          and the file's own contents unreadable past cluster 0.
 *
 * @par MC/DC:
 * Decision: `(i + 1) < file->alloc_clusters` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_stream.c@priv_exfat_materialize_chain`
 * (1 condition). This case supplies the TRUE vector -- the loop body runs --
 * while the single-cluster transition supplies the FALSE one.
 *
 * @since 0.1.0
 */
static void test_stream_multi_cluster_run_materializes(void)
{
  TEST_BEGIN("exfat stream: a multi-cluster run is chained end to end");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "RUN.BIN", k_ra8_fs_mode_write, &f));
  stream_write_pattern(f,
                       (uint32_t)k_xs_multi_cluster,
                       (uint32_t)k_xs_big_chunk,
                       0U,
                       (uint8_t)k_xs_seed_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  const uint32_t strm   = stream_strm0_off(h);
  const uint32_t first  = disk_get_u32le(strm + (uint32_t)k_xs_off_strm_clus);
  const uint32_t cbytes = h->sectors_per_cluster * (uint32_t)k_mut_block_size;
  const uint32_t owned  = ((uint32_t)k_xs_multi_cluster + cbytes - 1U) / cbytes;
  TEST_ASSERT(owned >= (uint32_t)k_xsc_min_chain);

  static const uint8_t s_blocker[k_xs_sub_sector] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "PIN.BIN", s_blocker, (uint32_t)k_xs_sub_sector));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "RUN.BIN", k_ra8_fs_mode_append, &f));
  stream_write_pattern(f,
                       (uint32_t)k_xs_one_cluster,
                       (uint32_t)k_xs_chunk,
                       (uint32_t)k_xs_multi_cluster,
                       (uint8_t)k_xs_seed_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  const uint32_t total = (uint32_t)k_xs_multi_cluster + (uint32_t)k_xs_one_cluster;
  TEST_ASSERT_EQ(k_xsc_flag_chained, s_disk.bytes[strm + (uint32_t)k_xs_off_strm_flags]);
  expect_chain_over(h, first, total, first + owned);
  stream_expect_contents(h, "RUN.BIN", total, (uint8_t)k_xs_seed_a);

  stream_dump_image("stream_multi_cluster_run_materializes", h);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat stream: a multi-cluster run is chained end to end");
}

/**
 * @brief Read `SEEK.BIN` whole and assert the overwritten head and the tail.
 *
 * @details The first chunk came from the second generator (the backward-seek
 *          overwrite), everything after it from the first. Split out of the
 *          test so neither stays over the statement budget.
 *
 * @param[in,out] h     Mounted exFAT volume.
 * @param[in]     total The file's full length.
 *
 * @return Nothing; every check is asserted inside.
 *
 * @pre @p h is mounted and `SEEK.BIN` exists at @p total bytes.
 * @pre No handle is open on it.
 * @post The file is closed again.
 * @post No on-disk state is modified.
 *
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0
 */
static void expect_patched_head(ra8_fs_mount_t* h, uint32_t total)
{
  ra8_fs_file_t* r = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "SEEK.BIN", k_ra8_fs_mode_read, &r));
  static uint8_t s_back[k_xs_sub_sector + k_xs_multi_cluster];
  uint32_t       got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(r, s_back, total, &got));
  TEST_ASSERT_EQ(total, got);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(r));
  stream_expect_span(s_back, (uint32_t)k_xs_chunk, 0U, (uint8_t)k_xs_seed_b);
  stream_expect_span(&s_back[k_xs_chunk],
                     total - (uint32_t)k_xs_chunk,
                     (uint32_t)k_xs_chunk,
                     (uint8_t)k_xs_seed_a);
}

/**
 * @test test_stream_chained_backward_seek
 *
 * @brief A backward seek on a chained file re-walks from the head.
 *
 * @details The write path keeps a forward waypoint so appending stays linear
 *          in the bytes written. A seek BACKWARDS invalidates it -- the
 *          waypoint is ahead of the target -- and the walk has to start over
 *          from the chain head instead of following the FAT from a cluster
 *          that is already past where it needs to be. Overwriting a byte in
 *          the first cluster and reading the whole file back is what catches a
 *          walk that resumed from the wrong place.
 *
 * @par MC/DC:
 * Decision: `idx >= file->walk_cache_idx` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_stream.c@priv_exfat_cluster_at`
 * (1 condition). This case supplies the FALSE vector; every sequential write
 * supplies the TRUE one.
 *
 * @since 0.1.0
 */
static void test_stream_chained_backward_seek(void)
{
  TEST_BEGIN("exfat stream: backward seek on a chained file");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "SEEK.BIN", k_ra8_fs_mode_write, &f));
  stream_write_pattern(f,
                       (uint32_t)k_xs_sub_sector,
                       (uint32_t)k_xs_chunk,
                       0U,
                       (uint8_t)k_xs_seed_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  static const uint8_t s_blocker[k_xs_sub_sector] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "PIN.BIN", s_blocker, (uint32_t)k_xs_sub_sector));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "SEEK.BIN", k_ra8_fs_mode_append, &f));
  stream_write_pattern(f,
                       (uint32_t)k_xs_multi_cluster,
                       (uint32_t)k_xs_chunk,
                       (uint32_t)k_xs_sub_sector,
                       (uint8_t)k_xs_seed_a);
  /* The waypoint now sits in the last cluster; write below it. */
  static uint8_t s_patch[k_xs_chunk];
  stream_fill_at(s_patch, (uint32_t)k_xs_chunk, 0U, (uint8_t)k_xs_seed_b);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_seek(f, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, s_patch, (uint32_t)k_xs_chunk));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  const uint32_t total = (uint32_t)k_xs_sub_sector + (uint32_t)k_xs_multi_cluster;
  const uint32_t strm  = stream_strm0_off(h);
  TEST_ASSERT_EQ(k_xsc_flag_chained, s_disk.bytes[strm + (uint32_t)k_xs_off_strm_flags]);
  TEST_ASSERT_EQ(total, disk_get_u32le(strm + (uint32_t)k_xs_off_strm_dlen));

  expect_patched_head(h, total);

  stream_dump_image("stream_chained_backward_seek", h);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat stream: backward seek on a chained file");
}

/**
 * @test test_stream_crosses_bitmap_sector
 *
 * @brief An allocation past cluster 4096 crosses into the bitmap's second sector.
 *
 * @details 512 bitmap bytes cover 4096 clusters and a cluster is 4096 bytes on
 *          this geometry, so nothing under 16 MiB makes the bitmap code
 *          compute a second sector at all. This case writes just past that
 *          line, which exercises the LBA arithmetic in the free-bit probe, the
 *          marker and the scan window together -- an off-by-one in any of them
 *          corrupts a bitmap byte belonging to a different 4096-cluster block,
 *          and the census afterwards is what catches it.
 *
 * @par MC/DC:
 * Decision: `lba != loaded` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_write.c@priv_exfat_bitmap_window`
 * (1 condition). Every smaller case supplies the FALSE vector, where the whole
 * scan is served from one loaded sector; this one supplies the TRUE vector by
 * walking the window past the sector edge.
 *
 * @since 0.1.0
 */
static void test_stream_crosses_bitmap_sector(void)
{
  TEST_BEGIN("exfat stream: allocation crosses a bitmap sector");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t empty_used = alloc_bitmap_used(h);

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "HUGE.BIN", k_ra8_fs_mode_write, &f));
  stream_write_pattern(f,
                       (uint32_t)k_xsc_bitmap_span,
                       (uint32_t)k_xs_big_chunk,
                       0U,
                       (uint8_t)k_xs_seed_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  const uint32_t cbytes = h->sectors_per_cluster * (uint32_t)k_mut_block_size;
  const uint32_t want   = ((uint32_t)k_xsc_bitmap_span + cbytes - 1U) / cbytes;
  TEST_ASSERT_EQ(empty_used + want, alloc_bitmap_used(h));

  const uint32_t strm = stream_strm0_off(h);
  TEST_ASSERT_EQ(k_xsc_bitmap_span, disk_get_u32le(strm + (uint32_t)k_xs_off_strm_dlen));
  TEST_ASSERT_EQ(k_xsc_flag_contig, s_disk.bytes[strm + (uint32_t)k_xs_off_strm_flags]);

  /* Read the two ends and the byte either side of the crossing, rather than
   * 16 MiB of comparisons: a bitmap arithmetic fault shows up as a wrong
   * cluster, and a wrong cluster shows up at any offset inside it. */
  ra8_fs_file_t* r = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "HUGE.BIN", k_ra8_fs_mode_read, &r));
  static const uint32_t s_k_probe[] = {0U,
                                       (uint32_t)k_xsc_probe_before,
                                       (uint32_t)k_xsc_probe_after,
                                       (uint32_t)k_xsc_probe_last};
  for (uint32_t i = 0U; i < (uint32_t)(sizeof s_k_probe / sizeof s_k_probe[0]); i++) {
    uint8_t  one = 0U;
    uint32_t got = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_seek(r, s_k_probe[i]));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(r, &one, 1U, &got));
    TEST_ASSERT_EQ(1U, got);
    TEST_ASSERT_EQ(stream_byte_at(s_k_probe[i], (uint8_t)k_xs_seed_a), one);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(r));

  stream_dump_image("stream_crosses_bitmap_sector", h);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat stream: allocation crosses a bitmap sector");
}

/**
 * @test test_stream_volume_full
 *
 * @brief A stream that runs the volume dry reports no_mem and keeps what it wrote.
 *
 * @details Writes until the allocation bitmap has nothing left. The failure has
 *          to be `k_ra8_err_no_mem` and not a crash or a silent short write,
 *          and -- the part that is easy to get wrong -- the file has to survive
 *          it: `DataLength` must describe the bytes that really landed, so a
 *          reader gets a truncated file rather than a file claiming bytes the
 *          volume never stored. The read-back after the failure is what proves
 *          it.
 *
 * @par MC/DC:
 * Decision: `run >= need` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_write.c@priv_exfat_bitmap_window`
 * (1 condition). Every successful growth supplies the TRUE vector; this case
 * supplies the FALSE one for the whole window, twice -- the hinted pass and
 * the full rescan behind it -- which is what turns into `k_ra8_err_no_mem`.
 *
 * @since 0.1.0
 */
static void test_stream_volume_full(void)
{
  TEST_BEGIN("exfat stream: a full volume reports no_mem, file stays honest");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "FILL.BIN", k_ra8_fs_mode_write, &f));
  static uint8_t s_buf[k_xs_big_chunk];
  ra8_err_t      last  = k_ra8_ok;
  uint32_t       wrote = 0U;
  const uint32_t cap   = h->count_of_clusters + 2U;
  for (uint32_t i = 0U; i < cap; i++) {
    memset(s_buf, (int)(i & (uint32_t)k_xsc_fill_step), sizeof s_buf);
    last = ra8_fs_write(f, s_buf, (uint32_t)k_xs_big_chunk);
    if (last != k_ra8_ok) {
      break;
    }
    wrote += (uint32_t)k_xs_big_chunk;
  }
  TEST_ASSERT_EQ(k_ra8_err_no_mem, last);
  TEST_ASSERT(wrote > 0U);

  uint64_t size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(f, &size));
  TEST_ASSERT(size >= wrote);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "FILL.BIN", &st));
  TEST_ASSERT_EQ(size, st.size_bytes);

  ra8_fs_file_t* r   = nullptr;
  uint32_t       got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "FILL.BIN", k_ra8_fs_mode_read, &r));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(r, s_buf, (uint32_t)k_xs_big_chunk, &got));
  TEST_ASSERT_EQ(k_xs_big_chunk, got);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(r));

  stream_dump_image("stream_volume_full", h);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat stream: a full volume reports no_mem, file stays honest");
}

/**
 * @brief Run every exFAT chain / bitmap streaming test.
 *
 * @return Process exit status.
 * @retval 0 Every test passed (a failure aborts inside the assertion macros).
 *
 * @pre The host provides a working heap.
 * @pre No volume is mounted on entry.
 * @post Every test built and released its own volume.
 * @post A success banner is written to stderr.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  test_stream_fragmentation_forces_chain();
  test_stream_chained_file_reads_and_frees();
  test_stream_multi_cluster_run_materializes();
  test_stream_chained_backward_seek();
  test_stream_crosses_bitmap_sector();
  test_stream_volume_full();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_exfat_stream_chain.c\n");
  return 0;
}
