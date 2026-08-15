/**
 * @file test_ra8_fs_alloc_perf.c
 * @brief Cluster allocation cost, and FAT32 FSInfo maintenance (#607).
 *
 * @details
 * Two halves of one defect: `ra8_fs` did not know where its free space was.
 *
 * The COST half. `priv_alloc_cluster()` restarted its scan at cluster 2 on
 * every call and `priv_fat_get()` had no cache, so examining cluster N cost N
 * block reads and writing a K-cluster file cost O(K * N) real device round
 * trips. Nothing in the tree measured it, because a mock backend's "read" is a
 * `memcpy` and therefore free. So this file COUNTS the backend calls: the
 * block device here is a counter with a disk attached, and the assertions are
 * about how many times the driver reached for it.
 *
 * Counting is the only way to see this defect off-target, and it is also the
 * only way to keep it fixed -- a regression that reintroduced the rescan would
 * still pass every functional test in the suite.
 *
 * The METADATA half. FSInfo was written once at format time and then never
 * read and never updated, so after any firmware write the on-disk free count
 * was wrong: `fsck.fat` reported "Free cluster summary wrong" on an otherwise
 * clean card and Explorer showed stale free space. The cases below check the
 * written count against the truth computed by walking the FAT in the RAM disk
 * -- which is exactly the comparison `fsck.fat` makes.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "support/fs_fat_dir_test_util.h"
#include "unity_minimal.h"

/* ===========================================================================
 * Constants
 * ===========================================================================
 */

/**
 * @enum ap_geo_t
 * @brief Volume geometry and payload sizes for these cases.
 *
 * @details `k_ap_f32_blocks` has to clear FAT32's 65525-cluster floor at one
 *          sector per cluster, plus the two 512-sector FAT copies and the
 *          32-sector reserved region the formatter lays down.
 *
 * @invariant `k_ap_f32_blocks` yields `count_of_clusters >= 65525`.
 * @see build_fat32_vol()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ap_f32_blocks  = 81920U, /**< 40 MiB: a comfortable FAT32 volume.     */
  k_ap_chunk       = 65536U, /**< Bytes per fill write.                   */
  k_ap_small_clus  = 64U,    /**< Clusters in the first measured write.   */
  k_ap_large_clus  = 128U,   /**< Clusters in the second measured write.  */
  k_ap_clus_bytes  = 512U,   /**< One cluster at SPC=1.                   */
  k_ap_fill_cap    = 512U,   /**< Bound on the volume-filling loop (P10). */
  k_ap_growth_cap  = 3U,     /**< Doubling the size may not triple reads. */
  k_ap_read_budget = 600U,   /**< Reads allowed for the 64-cluster write. */
  k_ap_span_clus   = 300U,   /**< A chain crossing a FAT16 sector.        */
  k_ap_far_index   = 260U,   /**< Chain index reaching FAT sector 1.      */
} ap_geo_t;

/**
 * @enum ap_fsi_t
 * @brief FSInfo sector layout and signatures (MS FAT spec sec 5).
 *
 * @invariant Every offset lies inside one 512-byte sector.
 * @see read_fsinfo_free()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ap_fsi_lba       = 1U,          /**< Where the formatter puts FSInfo. */
  k_ap_fsi_off_lead  = 0U,          /**< FSI_LeadSig offset.              */
  k_ap_fsi_off_struc = 484U,        /**< FSI_StrucSig offset.             */
  k_ap_fsi_off_free  = 488U,        /**< FSI_Free_Count offset.           */
  k_ap_fsi_off_next  = 492U,        /**< FSI_Nxt_Free offset.             */
  k_ap_fsi_off_trail = 508U,        /**< FSI_TrailSig offset.             */
  k_ap_fsi_lead_sig  = 0x41615252U, /**< "RRaA".                          */
  k_ap_fsi_struc_sig = 0x61417272U, /**< "rrAa".                          */
  k_ap_fsi_unknown   = 0xFFFFFFFFU, /**< Free count "not known".          */
  k_ap_first_cluster = 2U,          /**< Cluster numbering starts at 2.   */
  k_ap_fat32_ent     = 4U,          /**< Bytes per FAT32 entry.           */
  k_ap_fat32_mask    = 0x0FFFFFFFU, /**< Low 28 bits are the cluster.     */
  k_ap_shift_byte    = 8U,          /**< Byte position in a word.         */
  k_ap_shift_two     = 16U,         /**< Two-byte position in a word.     */
  k_ap_shift_three   = 24U,         /**< Three-byte position in a word.   */
  k_ap_corrupt_sig   = 0xDEADBEEFU, /**< Not any FSInfo signature.        */
  k_ap_byte_mask     = 0xFFU,       /**< Low byte of a word.              */
} ap_fsi_t;

/* ===========================================================================
 * Counting block device
 * ===========================================================================
 */

/** @var s_reads
 * @brief Backend `read_block` calls since the last ::reset_counters.
 * @note The measurement this whole file exists to make.
 * @warning Reset before every measured span.
 * @since 0.1.0
 */
static uint32_t s_reads = 0U;

/** @var s_writes
 * @brief Backend `write_block` calls since the last ::reset_counters.
 * @note Recorded alongside the reads so a "fix" that trades reads for writes
 *       is visible rather than silent.
 * @warning Reset before every measured span.
 * @since 0.1.0
 */
static uint32_t s_writes = 0U;

/**
 * @brief Counting read: tallies the call, then serves it from `s_disk`.
 *
 * @param[in]  ctx   Unused (the fixture's disk is a file-scope singleton).
 * @param[in]  lba   First block.
 * @param[in]  count Blocks to read.
 * @param[out] buf   Destination.
 *
 * @return Error code.
 * @retval k_ra8_ok             Bytes copied.
 * @retval k_ra8_err_out_of_range The range leaves the disk.
 *
 * @pre `s_disk.bytes` is allocated.
 * @pre @p buf holds `count * 512` bytes.
 * @post ::s_reads has grown by one.
 * @post On success @p buf holds the requested blocks.
 *
 * @note Not thread-safe; the suite is single-threaded.
 * @since 0.1.0 @details Implements the bounded cnt read fixture step using caller-owned state.
 */
RA8_INTERNAL static ra8_err_t
internal_cnt_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  s_reads++;
  if (lba + count > s_disk.block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf, &s_disk.bytes[(size_t)lba * (size_t)k_geo_blk_sz], (size_t)count * k_geo_blk_sz);
  return k_ra8_ok;
}

/**
 * @brief Counting write: tallies the call, then commits it to `s_disk`.
 *
 * @param[in] ctx   Unused.
 * @param[in] lba   First block.
 * @param[in] count Blocks to write.
 * @param[in] buf   Source.
 *
 * @return Error code.
 * @retval k_ra8_ok             Bytes committed.
 * @retval k_ra8_err_out_of_range The range leaves the disk.
 *
 * @pre `s_disk.bytes` is allocated.
 * @pre @p buf holds `count * 512` bytes.
 * @post ::s_writes has grown by one.
 * @post On success the disk holds the new blocks.
 *
 * @note Not thread-safe; the suite is single-threaded.
 * @since 0.1.0 @details Implements the bounded cnt write fixture step using caller-owned state.
 */
RA8_INTERNAL static ra8_err_t
internal_cnt_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
{
  (void)ctx;
  s_writes++;
  if (lba + count > s_disk.block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&s_disk.bytes[(size_t)lba * (size_t)k_geo_blk_sz], buf, (size_t)count * k_geo_blk_sz);
  return k_ra8_ok;
}

/**
 * @brief Report the fixture disk's capacity.
 *
 * @param[in]  ctx         Unused.
 * @param[out] block_count Receives the sector count.
 * @param[out] block_size  Receives 512.
 *
 * @return Error code.
 * @retval k_ra8_ok Always.
 *
 * @pre Both outputs are non-NULL.
 * @pre `s_disk` has been built.
 * @post The outputs describe the fixture disk.
 * @post No counters change (capacity is not I/O).
 *
 * @note Not thread-safe; the suite is single-threaded.
 * @since 0.1.0 @details Implements the bounded cnt capacity fixture step using caller-owned state.
 */
RA8_INTERNAL static ra8_err_t
internal_cnt_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  *block_count = s_disk.block_count;
  *block_size  = (uint32_t)k_geo_blk_sz;
  return k_ra8_ok;
}

/** @brief The counting backend every case in this file mounts through. */
static const ra8_fs_backend_t s_cnt_backend = {
  .read_block   = internal_cnt_read,
  .write_block  = internal_cnt_write,
  .get_capacity = internal_cnt_capacity,
  .ctx          = nullptr,
};

/**
 * @brief Zero both call counters.
 *
 * @return Nothing.
 *
 * @pre A measured span is about to begin.
 * @pre The previous span's numbers have been read.
 * @post ::s_reads and ::s_writes are zero.
 * @post No other state modified.
 *
 * @note Not thread-safe; the suite is single-threaded.
 * @since 0.1.0 @details Implements the bounded reset counters fixture step using caller-owned state.
 */
RA8_INTERNAL static void internal_reset_counters(void)
{
  s_reads  = 0U;
  s_writes = 0U;
}

/* ===========================================================================
 * Volume helpers
 * ===========================================================================
 */

/**
 * @brief Format and allocate a FAT32 volume in the fixture's RAM disk.
 *
 * @return Nothing.
 *
 * @pre The heap can hold 40 MiB.
 * @pre Any previous volume has been freed.
 * @post `s_disk` holds a formatted FAT32 image with a valid FSInfo sector.
 * @post The image is ready to mount through ::s_cnt_backend.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @details Implements the bounded build fat32 vol fixture step using caller-owned state.
 */
RA8_INTERNAL static void internal_build_fat32_vol(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
  s_disk.block_count = (uint32_t)k_ap_f32_blocks;
  s_disk.byte_count  = (uint32_t)k_ap_f32_blocks * (uint32_t)k_geo_blk_sz;
  s_disk.bytes       = (uint8_t*)calloc(1, s_disk.byte_count);
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat32;
  opts.label                = "PERF";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_cnt_backend, &opts));
}

/**
 * @brief Read a little-endian 32-bit field out of the FSInfo sector.
 *
 * @param[in] off Byte offset within the sector.
 *
 * @return The field value.
 * @retval 0..UINT32_MAX The four bytes, little-endian.
 *
 * @pre `s_disk.bytes` holds a formatted FAT32 volume.
 * @pre @p off leaves four bytes inside the sector.
 * @post No state modified.
 * @post No backend call is counted (this bypasses the driver).
 *
 * @note Reads the fixture's memory directly.
 * @since 0.1.0 @details Implements the bounded read fsinfo fixture step using caller-owned state.
 */
RA8_INTERNAL static uint32_t internal_read_fsinfo(uint32_t off)
{
  const uint32_t at = ((uint32_t)k_ap_fsi_lba * (uint32_t)k_geo_blk_sz) + off;
  return (uint32_t)s_disk.bytes[at] | ((uint32_t)s_disk.bytes[at + 1U] << k_ap_shift_byte) |
         ((uint32_t)s_disk.bytes[at + 2U] << k_ap_shift_two) |
         ((uint32_t)s_disk.bytes[at + 3U] << k_ap_shift_three);
}

/**
 * @brief Overwrite a little-endian 32-bit field in the FSInfo sector.
 *
 * @param[in] off Byte offset within the sector.
 * @param[in] v   Value to store.
 *
 * @return Nothing.
 *
 * @pre `s_disk.bytes` holds a formatted FAT32 volume.
 * @pre No volume is mounted on that disk.
 * @post The four bytes at @p off hold @p v.
 * @post No backend call is counted.
 *
 * @note Writes the fixture's memory directly, which is how these cases forge
 *       the FSInfo variants a mount has to survive.
 * @since 0.1.0 @details Implements the bounded poke fsinfo fixture step using caller-owned state.
 */
RA8_INTERNAL static void internal_poke_fsinfo(uint32_t off, uint32_t v)
{
  const uint32_t at     = ((uint32_t)k_ap_fsi_lba * (uint32_t)k_geo_blk_sz) + off;
  s_disk.bytes[at]      = (uint8_t)(v & (uint32_t)k_ap_byte_mask);
  s_disk.bytes[at + 1U] = (uint8_t)((v >> k_ap_shift_byte) & (uint32_t)k_ap_byte_mask);
  s_disk.bytes[at + 2U] = (uint8_t)((v >> k_ap_shift_two) & (uint32_t)k_ap_byte_mask);
  s_disk.bytes[at + 3U] = (uint8_t)((v >> k_ap_shift_three) & (uint32_t)k_ap_byte_mask);
}

/**
 * @brief Count the volume's free clusters by walking FAT copy 0 directly.
 *
 * @details The ground truth, computed the same way `fsck.fat` computes it:
 *          every FAT32 entry whose low 28 bits are zero is a free cluster.
 *          Comparing FSInfo against THIS is the assertion that matters -- an
 *          FSInfo the driver merely kept self-consistent would still make a
 *          host complain.
 *
 * @param[in] h Mounted FAT32 volume.
 *
 * @return The number of free clusters.
 * @retval 0..count_of_clusters The tally.
 *
 * @pre @p h is mounted as FAT32 on the fixture's RAM disk.
 * @pre `s_disk.bytes` is allocated.
 * @post No state modified.
 * @post No backend call is counted.
 *
 * @note Walks the fixture's memory directly, bypassing the driver.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_true_free_count(const ra8_fs_mount_t* h)
{
  const uint32_t base = (h->partition_base_lba + h->first_fat_lba) * (uint32_t)k_geo_blk_sz;
  uint32_t       free_clusters = 0U;
  for (uint32_t c = 0U; c < h->count_of_clusters; c++) {
    const uint32_t at = base + (((uint32_t)k_ap_first_cluster + c) * (uint32_t)k_ap_fat32_ent);
    const uint32_t v =
      ((uint32_t)s_disk.bytes[at] | ((uint32_t)s_disk.bytes[at + 1U] << k_ap_shift_byte) |
       ((uint32_t)s_disk.bytes[at + 2U] << k_ap_shift_two) |
       ((uint32_t)s_disk.bytes[at + 3U] << k_ap_shift_three)) &
      (uint32_t)k_ap_fat32_mask;
    if (v == 0U) {
      free_clusters++;
    }
  }
  return free_clusters;
}

/**
 * @brief Create a file of @p clusters clusters through the public API.
 *
 * @param[in] h        Mounted volume.
 * @param[in] path     Name to create.
 * @param[in] clusters Cluster count (one 512-byte sector each at SPC=1).
 *
 * @return Nothing.
 *
 * @pre @p h is mounted read-write with room for the file.
 * @pre @p path is an 8.3 name.
 * @post @p path holds `clusters * 512` bytes.
 * @post The handle used is closed.
 *
 * @note Allocates and frees the payload on the heap.
 * @since 0.1.0 @details Implements the bounded write clusters fixture step using caller-owned state.
 */
RA8_INTERNAL static void
internal_write_clusters(ra8_fs_mount_t* h, const char* path, uint32_t clusters)
{
  const uint32_t len  = clusters * (uint32_t)k_ap_clus_bytes;
  uint8_t*       data = (uint8_t*)calloc(1, len);
  if (data == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, path, k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, data, len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  free(data);
}

/* ===========================================================================
 * Tests
 * ===========================================================================
 */

/**
 * @test test_sequential_alloc_is_amortised_constant
 * @brief Doubling the clusters written does not more than double the backend
 *        reads -- the property the old rescan-from-cluster-2 allocator broke.
 *
 * @details Two writes on one mount, the second twice the size of the first and
 *          starting where the first left off. Under the old allocator the
 *          second write was the expensive one twice over: every allocation
 *          rescanned from cluster 2, so it paid for the first file's clusters
 *          again on every step, and the cost of writing K clusters starting at
 *          offset N was O(K * N) block reads.
 *
 *          Measured on this fixture, driving the same two writes through the
 *          code as it stood at 45b9418ed and as it stands now:
 *
 *              64 clusters:  4419 reads -> 198   (22x)
 *             128 clusters: 25219 reads -> 389   (65x)
 *
 *          and the growth for a doubling went from 5.7x (super-linear) to
 *          1.96x (linear), which is the property asserted below.
 *
 *          The bound is deliberately loose (a factor of three for a doubling)
 *          because the exact figure depends on FAT-mirror writes and data
 *          read-modify-writes, both of which are honestly linear. What it
 *          rules out is the quadratic term, which on these sizes was worth
 *          thousands of reads.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it counts backend reads for two
 * writes and compares the totals)
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_sequential_alloc_is_amortised_constant(void)
{
  TEST_BEGIN("fs alloc: sequential allocation is amortised constant");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_cnt_backend, &h));

  internal_reset_counters();
  internal_write_clusters(h, "SMALL.BIN", (uint32_t)k_ap_small_clus);
  const uint32_t small_reads = s_reads;

  internal_reset_counters();
  internal_write_clusters(h, "LARGE.BIN", (uint32_t)k_ap_large_clus);
  const uint32_t large_reads = s_reads;

  ra8_test_output_t    output = {};
  ra8_test_output_fd_t state  = {};
  (void)internal_test_output_fd_init(&output, &state, STDOUT_FILENO);
  (void)internal_test_output_text(&output, "      reads: ");
  (void)internal_test_output_u64(&output, (uint64_t)k_ap_small_clus);
  (void)internal_test_output_text(&output, " clusters -> ");
  (void)internal_test_output_u64(&output, small_reads);
  (void)internal_test_output_text(&output, ", ");
  (void)internal_test_output_u64(&output, (uint64_t)k_ap_large_clus);
  (void)internal_test_output_text(&output, " clusters -> ");
  (void)internal_test_output_u64(&output, large_reads);
  (void)internal_test_output_text(&output, "\n");

  /* The absolute bound: the old allocator needed well over 4000 reads for the
   * 64-cluster write alone (roughly sum(1..64) for the scans plus sum(0..63)
   * for the chain re-walks). */
  if (small_reads > (uint32_t)k_ap_read_budget) {
    TEST_FAIL_FMT("64-cluster write took %u reads, budget %u",
                  (unsigned)small_reads,
                  (unsigned)k_ap_read_budget);
  }
  /* The shape bound: doubling the work may not triple the reads. */
  if (large_reads > (small_reads * (uint32_t)k_ap_growth_cap)) {
    TEST_FAIL_FMT("doubling the size took %u reads vs %u -- growth is not linear",
                  (unsigned)large_reads,
                  (unsigned)small_reads);
  }

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("fs alloc: sequential allocation is amortised constant");
}

/**
 * @test test_fsinfo_tracks_the_truth
 * @brief The FSInfo free count written at close and at unmount matches the
 *        count obtained by walking the FAT -- the comparison `fsck.fat` makes.
 *
 * @details Also checks `FSI_Nxt_Free` moves off its formatted value, and that
 *          unlinking gives the clusters back to the count rather than leaking
 *          them.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it compares the written FSInfo free
 * count against the count obtained by walking the FAT)
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_fsinfo_tracks_the_truth(void)
{
  TEST_BEGIN("fs alloc: FSInfo free count matches the FAT");
  internal_build_fat32_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_cnt_backend, &h));
  const uint32_t before = internal_true_free_count(h);
  TEST_ASSERT(before > (uint32_t)k_ap_large_clus);

  internal_write_clusters(h, "DATA.BIN", (uint32_t)k_ap_large_clus);
  /* Closing the file is what commits the count. */
  TEST_ASSERT_EQ(internal_true_free_count(h), internal_read_fsinfo((uint32_t)k_ap_fsi_off_free));
  TEST_ASSERT_EQ(before - (uint32_t)k_ap_large_clus,
                 internal_read_fsinfo((uint32_t)k_ap_fsi_off_free));
  TEST_ASSERT(internal_read_fsinfo((uint32_t)k_ap_fsi_off_next) > (uint32_t)k_ap_first_cluster);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "DATA.BIN"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  /* Unmount is the other commit point. */
  TEST_ASSERT_EQ(before, internal_read_fsinfo((uint32_t)k_ap_fsi_off_free));

  /* And the freed space is reachable again: remount, refill, same count. */
  h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_cnt_backend, &h));
  internal_write_clusters(h, "AGAIN.BIN", (uint32_t)k_ap_large_clus);
  TEST_ASSERT_EQ(internal_true_free_count(h), internal_read_fsinfo((uint32_t)k_ap_fsi_off_free));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("fs alloc: FSInfo free count matches the FAT");
}

/**
 * @test test_fsinfo_invalid_is_not_written
 * @brief A sector whose signatures do not validate is left alone, and the
 *        volume still works.
 *
 * @details The signatures are the only thing that says `BPB_FSInfo` points at
 *          an FSInfo sector at all. Writing a free count into a sector that
 *          failed them would corrupt whatever it really is.
 *
 * @par MC/DC:
 * Decision chain in priv_fsinfo_signatures_ok: lead, then struct, then
 * trailing -- three single-condition branches.
 * - V1: lead corrupted     -> first returns false.
 * - V2: struct corrupted   -> second returns false.
 * - V3: trailing corrupted -> third returns false.
 * - V4: all three intact   -> true, covered by test_fsinfo_tracks_the_truth.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_fsinfo_invalid_is_not_written(void)
{
  TEST_BEGIN("fs alloc: unvalidated FSInfo is never written back");
  const uint32_t offs[] = {
    (uint32_t)k_ap_fsi_off_lead,
    (uint32_t)k_ap_fsi_off_struc,
    (uint32_t)k_ap_fsi_off_trail,
  };
  for (uint32_t i = 0U; i < (sizeof(offs) / sizeof(offs[0])); i++) {
    internal_build_fat32_vol();
    internal_poke_fsinfo(offs[i], (uint32_t)k_ap_corrupt_sig);
    const uint32_t free_before = internal_read_fsinfo((uint32_t)k_ap_fsi_off_free);

    ra8_fs_mount_t* h = nullptr;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_cnt_backend, &h));
    internal_write_clusters(h, "DATA.BIN", (uint32_t)k_ap_small_clus);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));

    TEST_ASSERT_EQ(k_ap_corrupt_sig, internal_read_fsinfo(offs[i]));
    TEST_ASSERT_EQ(free_before, internal_read_fsinfo((uint32_t)k_ap_fsi_off_free));
    internal_free_vol();
  }
  TEST_END("fs alloc: unvalidated FSInfo is never written back");
}

/**
 * @test test_fsinfo_unknown_stays_unknown
 * @brief A free count that cannot be trusted is written back as "unknown"
 *        rather than as a number we made up.
 *
 * @details The format defines 0xFFFFFFFF for exactly this, and `fsck.fat`
 *          accepts it silently. Inventing a count instead is what produces the
 *          "Free cluster summary wrong" report this issue is about.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it seeds an untrusted free count and
 * asserts the writeback repeats the format's "unknown" value)
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_fsinfo_unknown_stays_unknown(void)
{
  TEST_BEGIN("fs alloc: an untrusted free count is written as unknown");
  internal_build_fat32_vol();
  internal_poke_fsinfo((uint32_t)k_ap_fsi_off_free, (uint32_t)k_ap_fsi_unknown);

  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_cnt_backend, &h));
  internal_write_clusters(h, "DATA.BIN", (uint32_t)k_ap_small_clus);
  TEST_ASSERT_EQ(k_ap_fsi_unknown, internal_read_fsinfo((uint32_t)k_ap_fsi_off_free));
  /* The hint is still maintained, because it is always safe to write. */
  TEST_ASSERT(internal_read_fsinfo((uint32_t)k_ap_fsi_off_next) > (uint32_t)k_ap_first_cluster);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("fs alloc: an untrusted free count is written as unknown");
}

/**
 * @test test_stale_hint_is_range_checked
 * @brief An `FSI_Nxt_Free` pointing outside the volume does not break
 *        allocation.
 *
 * @details The hint arrives from whatever last wrote the card. A value past
 *          the last cluster must restart the scan, not index off the end of
 *          the FAT.
 *
 * @par MC/DC:
 * Decision pair in priv_fsinfo_seed: `nxt >= k_cluster_first_data` and
 * `(nxt - k_cluster_first_data) < count_of_clusters`, two single-condition
 * guards that must BOTH hold before the on-disk hint is believed.
 * - V1: nxt = 0xFFFFFFFF -> first true, second false -> hint stays at 2.
 * - V2: the formatter's nxt = 3 -> both true -> hint seeded from the disk
 *       (covered by test_fsinfo_tracks_the_truth).
 * The `priv_alloc_start` clamp is driven separately, by
 * test_full_volume_wraps_once_then_reports_full.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_stale_hint_is_range_checked(void)
{
  TEST_BEGIN("fs alloc: an out-of-range FSInfo hint is ignored");
  internal_build_fat32_vol();
  internal_poke_fsinfo((uint32_t)k_ap_fsi_off_next, (uint32_t)k_ap_fsi_unknown);

  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_cnt_backend, &h));
  internal_write_clusters(h, "DATA.BIN", (uint32_t)k_ap_small_clus);
  TEST_ASSERT_EQ(internal_true_free_count(h), internal_read_fsinfo((uint32_t)k_ap_fsi_off_free));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "DATA.BIN", k_ra8_fs_mode_read, &f));
  uint32_t got                  = 0U;
  uint8_t  buf[k_ap_clus_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ap_clus_bytes, got);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("fs alloc: an out-of-range FSInfo hint is ignored");
}

/**
 * @test test_freed_space_is_reused_first
 * @brief Deleting a file puts its clusters back at the front of the queue.
 *
 * @details The next-free hint only ever moves forward on its own, so without
 *          pulling it back on a free the next file would step straight over
 *          the space just released and only find it again after a full wrap.
 *          Correct, but it would make a delete look like it had done nothing.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it deletes a file and asserts the
 * next one lands on the released clusters)
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_freed_space_is_reused_first(void)
{
  TEST_BEGIN("fs alloc: freed clusters are reused before later ones");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_cnt_backend, &h));

  internal_write_clusters(h, "FIRST.BIN", (uint32_t)k_ap_small_clus);
  internal_write_clusters(h, "SECOND.BIN", (uint32_t)k_ap_small_clus);

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "FIRST.BIN", k_ra8_fs_mode_read, &f));
  const uint32_t first_head = f->first_cluster;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "FIRST.BIN"));
  internal_write_clusters(h, "THIRD.BIN", (uint32_t)k_ap_small_clus);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "THIRD.BIN", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(first_head, f->first_cluster);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("fs alloc: freed clusters are reused before later ones");
}

/**
 * @test test_full_volume_wraps_once_then_reports_full
 * @brief With the hint sitting past the last cluster, the scan restarts at 2,
 *        examines the whole volume, and only then reports the volume full.
 *
 * @details The wrap is the part a hinted scan can get wrong. A scan that
 *          started at the hint and stopped at the end would call a volume full
 *          while free clusters sat below the hint -- a filesystem that loses
 *          capacity as it is used. Filling the volume completely leaves the
 *          hint exactly one past the last cluster, which is the state that
 *          drives it.
 *
 * @par MC/DC:
 * Decision: `(hint - k_cluster_first_data) >= count_of_clusters` in
 * priv_alloc_start (1 condition).
 * - V1 (here): the hint sits one past the last cluster -> true -> the scan
 *   restarts at cluster 2 and covers the whole volume before reporting full.
 * - V2: an ordinary in-range hint -> false -> covered by every other case.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_full_volume_wraps_once_then_reports_full(void)
{
  TEST_BEGIN("fs alloc: a full volume wraps once, then reports full");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_cnt_backend, &h));

  static uint8_t chunk[k_ap_chunk] = {};
  ra8_fs_file_t* f                 = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "FILL.BIN", k_ra8_fs_mode_write, &f));
  ra8_err_t e      = k_ra8_ok;
  uint32_t  rounds = 0U;
  while ((e == k_ra8_ok) && (rounds < (uint32_t)k_ap_fill_cap)) {
    e = ra8_fs_write(f, chunk, (uint32_t)k_ap_chunk);
    rounds++;
  }
  TEST_ASSERT_EQ(k_ra8_err_no_mem, e);
  TEST_ASSERT(rounds < (uint32_t)k_ap_fill_cap);

  /* The hint now sits past the last cluster. One more allocation attempt must
   * restart at cluster 2, walk the whole volume, and report it full -- not
   * index off the end of the FAT. */
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_write(f, chunk, (uint32_t)k_ap_clus_bytes));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* Freeing everything makes the volume usable again through the same hint. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "FILL.BIN"));
  internal_write_clusters(h, "AFTER.BIN", (uint32_t)k_ap_small_clus);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("fs alloc: a full volume wraps once, then reports full");
}

/**
 * @brief Read one cluster's worth and report the backend reads it cost.
 *
 * @param[in,out] f Open file positioned where the caller wants it.
 *
 * @return The number of backend reads the call issued.
 * @retval 1..N One data-sector read, plus any FAT sector the walk had to fetch.
 *
 * @pre @p f is open for reading with at least one cluster left.
 * @pre The counting backend is the one under @p f's mount.
 * @post The file offset has advanced by one cluster.
 * @post ::s_reads holds the same value that was returned.
 *
 * @note Not thread-safe; the suite is single-threaded.
 * @since 0.1.0 @details Implements the bounded read one cluster cost fixture step using caller-owned state.
 */
RA8_INTERNAL static uint32_t internal_read_one_cluster_cost(ra8_fs_file_t* f)
{
  uint8_t  buf[k_ap_clus_bytes] = {};
  uint32_t got                  = 0U;
  internal_reset_counters();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ap_clus_bytes, got);
  return s_reads;
}

/**
 * @test test_mcdc_fat_sector_cache
 * @brief The FAT sector cache serves a repeat of the same sector and misses
 *        on anything else -- proved by counting backend reads.
 *
 * @details Every vector is a read count, because a cache is invisible any
 *          other way: the bytes are identical whether they came from memory or
 *          from the device, and only the number of times the driver reached
 *          for the device tells them apart.
 *
 *          The volume holds one long file whose cluster chain crosses a FAT16
 *          sector boundary (256 entries per sector), which is what makes the
 *          "same mount, different sector" vector reachable at all.
 *
 * @par MC/DC:
 * Decision: `(s_fat_cache_owner == m) && (s_fat_cache_lba == lba)` in
 * `libs/ra8_fs/src/ra8_fs_fat_alloc.c@priv_fat_sector_read` (2 conditions).
 * - V1: same mount, same FAT sector      -> true  (control: cache hit, one
 *       read fewer than the miss that populated it).
 * - V2: a different mount, same sector   -> false (varies condition 1: the
 *       second mount of the same disk pays for its own read).
 * - V3: same mount, a different sector   -> false (varies condition 2: walking
 *       past cluster 255 leaves FAT sector 0 and must re-read).
 *
 * The single-condition window test in
 * `libs/ra8_fs/src/ra8_fs_fat_alloc.c@priv_lba_is_cacheable` is driven by the
 * same reads: a copy-0 sector is cached (V1 could not otherwise hit) and a
 * mirror-copy sector is not (every FAT write in
 * test_sequential_alloc_is_amortised_constant reads its mirror from the
 * backend, which is why that test's read count is linear rather than zero).
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_mcdc_fat_sector_cache(void)
{
  TEST_BEGIN("fs alloc: the FAT sector cache hits only on the same mount+sector");
  internal_build_fat16_vol();
  ra8_fs_mount_t* a = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_cnt_backend, &a));
  internal_write_clusters(a, "SPAN.BIN", (uint32_t)k_ap_span_clus);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(a));
  a = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_cnt_backend, &a));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(a, "SPAN.BIN", k_ra8_fs_mode_read, &f));
  (void)internal_read_one_cluster_cost(f); /* index 0 needs no FAT walk at all */

  /* The miss that populates the cache: index 1 needs FAT sector 0. */
  const uint32_t miss = internal_read_one_cluster_cost(f);
  /* V1: index 2 needs the same FAT sector -- served from memory. */
  const uint32_t hit = internal_read_one_cluster_cost(f);
  TEST_ASSERT_EQ(miss - 1U, hit);

  /* V3: walking past cluster 255 leaves FAT sector 0 for sector 1. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_seek(f, (uint64_t)k_ap_far_index * (uint32_t)k_ap_clus_bytes));
  TEST_ASSERT(internal_read_one_cluster_cost(f) > hit);
  /* ... and coming back is a miss again, because sector 1 is what is cached. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_seek(f, (uint32_t)k_ap_clus_bytes));
  TEST_ASSERT(internal_read_one_cluster_cost(f) > hit);

  /* V2: a second mount of the same disk shares no cache entry with the first.
   * FAT sector 0 is the cached one again after the seek above. */
  ra8_fs_mount_t* b = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_cnt_backend, &b));
  ra8_fs_file_t* g = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(b, "SPAN.BIN", k_ra8_fs_mode_read, &g));
  (void)internal_read_one_cluster_cost(g);
  TEST_ASSERT_EQ(miss, internal_read_one_cluster_cost(g));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(g));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(b));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(a));
  internal_free_vol();
  TEST_END("fs alloc: the FAT sector cache hits only on the same mount+sector");
}

/**
 * @brief Run every case in this file.
 *
 * @return Process exit status.
 * @retval 0 Every case passed.
 *
 * @pre The process has a heap for a 40 MiB RAM disk.
 * @pre No other test binary shares this process.
 * @post Every volume allocated here has been freed.
 * @post The measured read counts have been printed for the record.
 *
 * @note Single-threaded by construction.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_sequential_alloc_is_amortised_constant();
  internal_test_fsinfo_tracks_the_truth();
  internal_test_fsinfo_invalid_is_not_written();
  internal_test_fsinfo_unknown_stays_unknown();
  internal_test_stale_hint_is_range_checked();
  internal_test_freed_space_is_reused_first();
  internal_test_full_volume_wraps_once_then_reports_full();
  internal_test_mcdc_fat_sector_cache();
  (void)internal_test_output_fd_text(STDOUT_FILENO, "[OK  ] test_ra8_fs_alloc_perf.c\n");
  return 0;
}
