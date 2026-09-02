/**
 * @file test_ra8_fs_fat_truncate.c
 * @brief ra8_fs_truncate on FAT12/16/32: shrink, grow-with-zero-fill, to-zero (#680).
 *
 * @details
 * FAT has one length, so a grow has to put real zeros on disk: the read path
 * serves the bytes straight off the clusters that back them. These cases drive
 * the whole verb through a real, formatted volume and prove the on-disk result
 * three ways -- the API read-back (content preserved, gap reads zero), the
 * free-cluster accounting (a shrink returns space, a grow takes it), and a full
 * unmount / remount (the directory entry persists the new length) -- so a driver
 * that merely adjusts the in-memory size without touching the media fails.
 *
 * The zero-fill is proven against a POISONED card: the whole image is filled
 * with a non-zero byte before formatting, and the payload written before a grow
 * ends mid-sector, so "reads as zero" past the old end cannot pass by accident.
 *
 * `RA8_FAT_DUMP_DIR`, when it names a directory, writes each scenario's image
 * there for a real `fsck.fat -n` pass; unset (as in CI) it does nothing, so the
 * suite carries no dependency on dosfstools.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_fs_meta.h"
#include "ra8_test_file.h"
#include "ra8_test_file_posix.h"
#include "ra8_test_output.h"
#include "unity_minimal.h"

/**
 * @enum fat_trunc_const_t
 * @brief Card geometry, payload sizes and poison byte for the FAT truncate tests.
 *
 * @details ::k_ftr_prefix is deliberately not a multiple of 512, so a grow's old
 *          end lands INSIDE a sector and a driver that zero-filled only whole
 *          sectors would leave residue a coarser test would miss.
 *
 * @invariant `k_ftr_prefix < k_ftr_bytes_per_sector`.
 * @see internal_build_fat_volume()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ftr_block_size       = 512U,   /**< Bytes per logical block.                    */
  k_ftr_blocks_fat32     = 73728U, /**< 36 MiB -> FAT32 band.                       */
  k_ftr_blocks_fat16     = 8192U,  /**< 4 MiB  -> FAT16 band.                       */
  k_ftr_bytes_per_sector = 512U,   /**< One sector.                                 */
  k_ftr_poison           = 0xC7U,  /**< Non-zero pre-format fill.                   */
  k_ftr_prefix           = 300U,   /**< Sub-sector payload before a grow.           */
  k_ftr_seed             = 0x5AU,  /**< Payload generator seed.                     */
  k_ftr_stride           = 7U,     /**< Payload generator stride (coprime w/256).   */
  k_ftr_dump_cap         = 512U,   /**< Dump-path scratch buffer capacity.          */
  k_ftr_fat_entry_bytes  = 4U,     /**< FAT32 FAT entry width, for the boundary.    */
  k_ftr_tiny_old         = 100U,   /**< Sub-sector length before an in-sector grow. */
  k_ftr_tiny_new         = 400U,   /**< In-sector grow target (still in sector 0).  */
  k_ftr_tail_odd         = 7U,     /**< Odd tail added past a cluster boundary.     */
} fat_trunc_const_t;

/** @brief RAM-backed block device shared by one test binary. */
typedef struct {
  uint8_t* bytes;       /**< Flat sector store. */
  uint32_t block_count; /**< 512-byte sectors.  */
} ftr_disk_t;

static ftr_disk_t s_disk = {};

/**
 * @brief Block-device read shim over ::s_disk.
 * @param[in]  ctx   The ::ftr_disk_t backing store.
 * @param[in]  lba   First sector.
 * @param[in]  count Sector count.
 * @param[out] buf   Destination.
 * @return k_ra8_ok, or k_ra8_err_out_of_range past the end.
 * @retval k_ra8_ok Sectors copied.
 * @pre @p buf holds @p count sectors.
 * @pre @p ctx is an ::ftr_disk_t.
 * @post @p buf mirrors the store.
 * @post No state changes.
 * @note Trivially thread-safe for the single-threaded fixture.
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
*/
RA8_INTERNAL static ra8_err_t
internal_ftr_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  const ftr_disk_t* d = (const ftr_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_ftr_block_size],
         (size_t)count * (uint32_t)k_ftr_block_size);
  return k_ra8_ok;
}

/**
 * @brief Block-device write shim over ::s_disk.
 * @param[in] ctx   The ::ftr_disk_t backing store.
 * @param[in] lba   First sector.
 * @param[in] count Sector count.
 * @param[in] buf   Source.
 * @return k_ra8_ok, or k_ra8_err_out_of_range past the end.
 * @retval k_ra8_ok Sectors stored.
 * @pre @p buf holds @p count sectors.
 * @pre @p ctx is an ::ftr_disk_t.
 * @post The store mirrors @p buf.
 * @post No other state changes.
 * @note Trivially thread-safe for the single-threaded fixture.
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
*/
RA8_INTERNAL static ra8_err_t
internal_ftr_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
{
  ftr_disk_t* d = (ftr_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_ftr_block_size],
         buf,
         (size_t)count * (uint32_t)k_ftr_block_size);
  return k_ra8_ok;
}

/**
 * @brief Block-device capacity shim over ::s_disk.
 * @param[in]  ctx         The ::ftr_disk_t backing store.
 * @param[out] block_count Receives the sector count.
 * @param[out] block_size  Receives 512.
 * @return k_ra8_ok always.
 * @retval k_ra8_ok Geometry reported.
 * @pre @p ctx is an ::ftr_disk_t.
 * @pre Both out-pointers are non-NULL.
 * @post The out-params carry the geometry.
 * @post No state changes.
 * @note Trivially thread-safe.
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
*/
RA8_INTERNAL static ra8_err_t
internal_ftr_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  const ftr_disk_t* d = (const ftr_disk_t*)ctx;
  *block_count        = d->block_count;
  *block_size         = (uint32_t)k_ftr_block_size;
  return k_ra8_ok;
}

static const ra8_fs_backend_t s_backend = {
  .read_block   = internal_ftr_read,
  .write_block  = internal_ftr_write,
  .get_capacity = internal_ftr_capacity,
  .ctx          = &s_disk,
};

/**
 * @brief The payload byte at file offset @p pos.
 * @param[in] pos File offset.
 * @return `pos * stride + seed`, folded to a byte.
 * @retval 0..255 The generated byte.
 * @pre None.
 * @pre @p pos is a file offset.
 * @post No state changes.
 * @post Depends only on @p pos.
 * @note Pure function.
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
*/
RA8_INTERNAL static uint8_t internal_ftr_byte_at(uint32_t pos)
{
  return (uint8_t)((pos * (uint32_t)k_ftr_stride) + (uint32_t)k_ftr_seed);
}

/**
 * @brief Free the RAM disk.
 * @return Nothing.
 * @pre None.
 * @pre ::s_disk is this binary's disk.
 * @post ::s_disk.bytes is NULL.
 * @post The backing store is released.
 * @note Not thread-safe.
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
*/
RA8_INTERNAL static void internal_free_volume(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
}

/**
 * @brief Allocate a poisoned card of @p blocks sectors and format it as @p type.
 * @param[in] blocks Sector count.
 * @param[in] type   FAT variant to format.
 * @return Nothing; a failure asserts inside.
 * @pre @p type is a FAT12/16/32 variant.
 * @pre @p blocks is large enough for @p type.
 * @post ::s_disk holds a mountable, poison-then-formatted volume.
 * @post Every unwritten data byte reads as ::k_ftr_poison.
 * @note Not thread-safe.
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
*/
RA8_INTERNAL static void internal_build_fat_volume(uint32_t blocks, ra8_fs_type_t type)
{
  internal_free_volume();
  s_disk.bytes       = (uint8_t*)malloc((size_t)blocks * (size_t)k_ftr_block_size);
  s_disk.block_count = blocks;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "malloc failed");
    return;
  }
  memset(s_disk.bytes, (int)k_ftr_poison, (size_t)blocks * (size_t)k_ftr_block_size);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = type;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
}

/**
 * @brief Dump the whole image to `$RA8_FAT_DUMP_DIR/<tag>.img` for `fsck.fat`.
 * @param[in] tag Scenario name; becomes the file basename.
 * @return Nothing. A dump that cannot be written fails the test.
 * @pre ::s_disk holds a formatted volume.
 * @pre @p tag has no path separators.
 * @post With the variable set, the image is on disk.
 * @post With it unset, nothing is written.
 * @note Not thread-safe.
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
*/
RA8_INTERNAL static void internal_dump_image(const char* tag)
{
  const char* dir = getenv("RA8_FAT_DUMP_DIR");
  if ((dir == nullptr) || (s_disk.bytes == nullptr)) {
    return;
  }
  char path[k_ftr_dump_cap] = {};
  (void)snprintf(path, sizeof path, "%s/%s.img", dir, tag);
  const size_t                 total  = (size_t)s_disk.block_count * (size_t)k_ftr_block_size;
  const ra8_test_file_result_t result = internal_test_file_replace(path, s_disk.bytes, total);
  if (result.status != k_ra8_test_file_ok) {
    TEST_FAIL_FMT("short dump %s", path);
  }
}

/**
 * @brief Bytes per cluster of the mounted volume.
 * @param[in] h Mounted volume.
 * @return `sectors_per_cluster * 512`.
 * @retval >0 The allocation-unit size.
 * @pre @p h is mounted.
 * @pre @p h describes a FAT volume.
 * @post No state changes.
 * @post Depends only on @p h.
 * @note Pure with respect to the mount.
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
*/
RA8_INTERNAL static uint32_t internal_cbytes_of(const ra8_fs_mount_t* h)
{
  return h->sectors_per_cluster * (uint32_t)k_ftr_bytes_per_sector;
}

/**
 * @brief Free clusters currently reported by the mount.
 * @param[in] h Mounted volume.
 * @return The free-cluster count.
 * @retval 0..total The count.
 * @pre @p h is mounted.
 * @pre The volume answers ::ra8_fs_free_space.
 * @post No state changes.
 * @post The result reflects the current allocation.
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
*/
RA8_INTERNAL static uint32_t internal_free_clusters(ra8_fs_mount_t* h)
{
  ra8_fs_space_t sp = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_free_space(h, &sp));
  return sp.free_clusters;
}

/**
 * @brief Create @p name and write @p len generated bytes to it.
 * @param[in,out] h    Mounted volume.
 * @param[in]     name File to create.
 * @param[in]     len  Bytes to write.
 * @return Nothing; every step asserts inside.
 * @pre @p h is mounted; @p name does not exist.
 * @pre @p len fits the volume.
 * @post @p name holds @p len generated bytes.
 * @post No handle is left open.
 * @note Not thread-safe.
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
*/
RA8_INTERNAL static void internal_write_file(ra8_fs_mount_t* h, const char* name, uint32_t len)
{
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, name, k_ra8_fs_mode_write, &f));
  static uint8_t s_buf[k_ftr_block_size];
  uint32_t       done = 0U;
  while (done < len) {
    uint32_t n = len - done;
    if (n > (uint32_t)k_ftr_block_size) {
      n = (uint32_t)k_ftr_block_size;
    }
    for (uint32_t i = 0U; i < n; i++) {
      s_buf[i] = internal_ftr_byte_at(done + i);
    }
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, s_buf, n));
    done += n;
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
}

/**
 * @brief Assert one byte at a known file offset is pattern or zero.
 * @param[in] actual  Byte read back from the driver.
 * @param[in] pos     File offset @p actual corresponds to.
 * @param[in] pat_end File offsets below this must match the generator; the rest zero.
 * @return Nothing; a mismatch fails and terminates the test process.
 * @pre @p pos is a file offset, not a buffer index.
 * @pre @p pat_end bounds which offsets must match the pattern generator.
 * @post No state changes on a match.
 * @post A mismatch is reported through TEST_FAIL_FMT before the process exits.
 * @note Not thread-safe.
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
*/
RA8_INTERNAL static void internal_expect_span_byte(uint8_t actual, uint32_t pos, uint32_t pat_end)
{
  const uint8_t exp = (pos < pat_end) ? internal_ftr_byte_at(pos) : 0U;
  if (actual != exp) {
    TEST_FAIL_FMT("byte %u wrong", (unsigned)pos);
  }
}

/**
 * @brief Assert @p got bytes at file offset @p pos are pattern then zeros.
 * @param[in] buf     Bytes read back from the driver.
 * @param[in] got     Number of bytes in @p buf.
 * @param[in] pos     File offset @p buf[0] corresponds to.
 * @param[in] pat_end File offsets below this must match the generator; the rest zero.
 * @return Nothing; the first mismatch fails and stops.
 * @pre @p buf holds @p got bytes.
 * @pre @p pos is a file offset, not a buffer index.
 * @post No state changes.
 * @post The scan stops at the first mismatch.
 * @note Not thread-safe.
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
*/
RA8_INTERNAL static void
internal_expect_span(const uint8_t* buf, uint32_t got, uint32_t pos, uint32_t pat_end)
{
  for (uint32_t i = 0U; i < got; i++) {
    internal_expect_span_byte(buf[i], pos + i, pat_end);
  }
}

/**
 * @brief Open @p name and assert it is @p total bytes: pattern then zeros.
 * @param[in,out] h        Mounted volume.
 * @param[in]     name     File to read.
 * @param[in]     total    Expected length.
 * @param[in]     pat_end  Bytes that must match the generator; the rest are zero.
 * @return Nothing; a mismatch fails at the offset it happened.
 * @pre @p h is mounted; @p name exists at @p total bytes.
 * @pre `pat_end <= total`.
 * @post The file is closed again.
 * @post No on-disk state changes.
 * @note Not thread-safe.
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
*/
RA8_INTERNAL static void internal_expect_pattern_then_zero(ra8_fs_mount_t* h,
                                                           const char*     name,
                                                           uint32_t        total,
                                                           uint32_t        pat_end)
{
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, name, k_ra8_fs_mode_read, &f));
  uint64_t size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(f, &size));
  TEST_ASSERT_EQ(total, size);
  static uint8_t s_buf[k_ftr_block_size];
  uint32_t       pos = 0U;
  while (pos < total) {
    uint32_t want = total - pos;
    if (want > (uint32_t)k_ftr_block_size) {
      want = (uint32_t)k_ftr_block_size;
    }
    uint32_t got = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, s_buf, want, &got));
    TEST_ASSERT_EQ(want, got);
    internal_expect_span(s_buf, got, pos, pat_end);
    pos += got;
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
}

/**
 * @brief Truncate @p name (opened write) to @p size and close it.
 * @param[in,out] h    Mounted volume.
 * @param[in]     name File to truncate.
 * @param[in]     size New length.
 * @return Nothing; every step asserts inside.
 * @pre @p h is mounted; @p name exists.
 * @pre @p size is a valid length for the volume.
 * @post @p name is @p size bytes.
 * @post No handle is left open.
 * @note Not thread-safe.
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
*/
RA8_INTERNAL static void internal_truncate_named(ra8_fs_mount_t* h, const char* name, uint32_t size)
{
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, name, k_ra8_fs_mode_append, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_truncate(f, size));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
}

/**
 * @test internal_test_fat_shrink_keeps_prefix
 * @brief Shrinking a multi-cluster file keeps the surviving prefix and frees the tail.
 *
 * @details A three-cluster file is trimmed to one and a half. The kept bytes
 *          must still read back exactly, the freed clusters must return to the
 *          volume's free count, and a remount must see the new length -- so the
 *          directory entry, not just the handle, was updated.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- the paths it drives are
 * covered elsewhere; the truncate mode/handle guard's MC/DC is owned by
 * internal_test_fat_truncate_rejects.)
 *
 * @since 0.1.0

 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_fat_shrink_keeps_prefix(void)
{
  TEST_BEGIN("fat truncate: shrink keeps the prefix, frees the tail");
  internal_build_fat_volume((uint32_t)k_ftr_blocks_fat32, k_ra8_fs_type_fat32);
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t cb      = internal_cbytes_of(h);
  const uint32_t base    = internal_free_clusters(h);
  const uint32_t old_len = (3U * cb);
  const uint32_t new_len = cb + (cb / 2U);
  internal_write_file(h, "SHRINK.BIN", old_len);
  TEST_ASSERT_EQ(base - 3U, internal_free_clusters(h));

  internal_truncate_named(h, "SHRINK.BIN", new_len);
  TEST_ASSERT_EQ(base - 2U, internal_free_clusters(h)); /* 3 -> 2 clusters */
  internal_expect_pattern_then_zero(h, "SHRINK.BIN", new_len, new_len);
  internal_dump_image("fat_shrink_keeps_prefix");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  internal_expect_pattern_then_zero(h, "SHRINK.BIN", new_len, new_len);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("fat truncate: shrink keeps the prefix, frees the tail");
}

/**
 * @test internal_test_fat_shrink_to_zero
 * @brief Truncating to zero frees every cluster and clears the first-cluster link.
 *
 * @details The whole chain returns to the volume, the size reads back 0, a read
 *          yields nothing, and a remount sees an empty file -- not a dangling
 *          first-cluster pointer into freed space.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- the paths it drives are
 * covered elsewhere; the truncate mode/handle guard's MC/DC is owned by
 * internal_test_fat_truncate_rejects.)
 *
 * @since 0.1.0

 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_fat_shrink_to_zero(void)
{
  TEST_BEGIN("fat truncate: shrink to zero frees the whole chain");
  internal_build_fat_volume((uint32_t)k_ftr_blocks_fat32, k_ra8_fs_type_fat32);
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t cb   = internal_cbytes_of(h);
  const uint32_t base = internal_free_clusters(h);
  internal_write_file(h, "ZERO.BIN", 2U * cb);
  TEST_ASSERT_EQ(base - 2U, internal_free_clusters(h));

  internal_truncate_named(h, "ZERO.BIN", 0U);
  TEST_ASSERT_EQ(base, internal_free_clusters(h));
  internal_expect_pattern_then_zero(h, "ZERO.BIN", 0U, 0U);
  internal_dump_image("fat_shrink_to_zero");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  internal_expect_pattern_then_zero(h, "ZERO.BIN", 0U, 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("fat truncate: shrink to zero frees the whole chain");
}

/**
 * @test internal_test_fat_grow_zero_fills
 * @brief Growing zero-fills the gap on disk, from a mid-sector old end.
 *
 * @details A sub-sector file is grown to two and a half clusters. The prefix
 *          must survive, and everything past it -- the slack of the file's first
 *          sector, and every fresh cluster -- must read as zero even though the
 *          card was poisoned with 0xC7 before formatting. The free count must
 *          drop by the clusters the grow took, and a remount must agree.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- the paths it drives are
 * covered elsewhere; the truncate mode/handle guard's MC/DC is owned by
 * internal_test_fat_truncate_rejects.)
 *
 * @since 0.1.0

 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_fat_grow_zero_fills(void)
{
  TEST_BEGIN("fat truncate: grow zero-fills the gap on disk");
  internal_build_fat_volume((uint32_t)k_ftr_blocks_fat32, k_ra8_fs_type_fat32);
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t cb      = internal_cbytes_of(h);
  const uint32_t base    = internal_free_clusters(h);
  const uint32_t new_len = (2U * cb) + (cb / 2U);
  internal_write_file(h, "GROW.BIN", (uint32_t)k_ftr_prefix);
  TEST_ASSERT_EQ(base - 1U, internal_free_clusters(h));

  internal_truncate_named(h, "GROW.BIN", new_len);
  TEST_ASSERT_EQ(base - 3U, internal_free_clusters(h)); /* 1 -> 3 clusters */
  internal_expect_pattern_then_zero(h, "GROW.BIN", new_len, (uint32_t)k_ftr_prefix);
  internal_dump_image("fat_grow_zero_fills");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  internal_expect_pattern_then_zero(h, "GROW.BIN", new_len, (uint32_t)k_ftr_prefix);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("fat truncate: grow zero-fills the gap on disk");
}

/**
 * @test internal_test_fat_grow_within_sector
 * @brief A grow that starts and ends inside one sector zero-fills just the gap.
 *
 * @details Both the old and the new end land in the file's first sector, so the
 *          zero-fill is a single partial read-modify-write that must clear only
 *          `[old, new)` and leave the prefix below it and the slack above it
 *          alone. The prefix reads back, the gap reads zero.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- the paths it drives are
 * covered elsewhere; the truncate mode/handle guard's MC/DC is owned by
 * internal_test_fat_truncate_rejects.)
 *
 * @since 0.1.0

 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_fat_grow_within_sector(void)
{
  TEST_BEGIN("fat truncate: a grow inside one sector zero-fills only the gap");
  internal_build_fat_volume((uint32_t)k_ftr_blocks_fat32, k_ra8_fs_type_fat32);
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  internal_write_file(h, "TINY.BIN", (uint32_t)k_ftr_tiny_old);
  internal_truncate_named(h, "TINY.BIN", (uint32_t)k_ftr_tiny_new); /* still inside sector 0 */
  internal_expect_pattern_then_zero(h,
                                    "TINY.BIN",
                                    (uint32_t)k_ftr_tiny_new,
                                    (uint32_t)k_ftr_tiny_old);
  internal_dump_image("fat_grow_within_sector");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("fat truncate: a grow inside one sector zero-fills only the gap");
}

/**
 * @test internal_test_fat_grow_from_empty
 * @brief Growing a brand-new empty file allocates a zero-filled extent.
 *
 * @details Open-for-write leaves a zero-length file; truncating it up must
 *          allocate its first cluster and every following one and zero them all,
 *          so a read returns nothing but zeros.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- the paths it drives are
 * covered elsewhere; the truncate mode/handle guard's MC/DC is owned by
 * internal_test_fat_truncate_rejects.)
 *
 * @since 0.1.0

 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_fat_grow_from_empty(void)
{
  TEST_BEGIN("fat truncate: grow from an empty file zero-fills");
  internal_build_fat_volume((uint32_t)k_ftr_blocks_fat32, k_ra8_fs_type_fat32);
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t cb      = internal_cbytes_of(h);
  const uint32_t new_len = cb + 1U;

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "EMPTY.BIN", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_truncate(f, new_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  internal_expect_pattern_then_zero(h, "EMPTY.BIN", new_len, 0U);
  internal_dump_image("fat_grow_from_empty");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("fat truncate: grow from an empty file zero-fills");
}

/**
 * @test internal_test_fat_boundary_grow_shrink
 * @brief Grow past a FAT-sector boundary, then shrink back across it.
 *
 * @details One FAT32 FAT sector maps 128 cluster entries, so a file grown past
 *          128 clusters has its chain links straddle two FAT sectors -- the walk,
 *          the allocation and the free all cross the boundary. The grow is proven
 *          zero, and the shrink returns exactly the clusters it crossed.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- the paths it drives are
 * covered elsewhere; the truncate mode/handle guard's MC/DC is owned by
 * internal_test_fat_truncate_rejects.)
 *
 * @since 0.1.0

 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_fat_boundary_grow_shrink(void)
{
  TEST_BEGIN("fat truncate: grow and shrink across a FAT-sector boundary");
  internal_build_fat_volume((uint32_t)k_ftr_blocks_fat32, k_ra8_fs_type_fat32);
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t cb            = internal_cbytes_of(h);
  const uint32_t per_fat_sec   = (uint32_t)k_ftr_bytes_per_sector / (uint32_t)k_ftr_fat_entry_bytes;
  const uint32_t span_clusters = per_fat_sec + 4U; /* just past one FAT sector */
  const uint32_t base          = internal_free_clusters(h);
  const uint32_t big           = span_clusters * cb;

  internal_write_file(h, "BND.BIN", (uint32_t)k_ftr_prefix);
  internal_truncate_named(h, "BND.BIN", big);
  TEST_ASSERT_EQ(base - span_clusters, internal_free_clusters(h));
  internal_expect_pattern_then_zero(h, "BND.BIN", big, (uint32_t)k_ftr_prefix);
  internal_dump_image("fat_boundary_grow");

  internal_truncate_named(h, "BND.BIN", cb);
  TEST_ASSERT_EQ(base - 1U, internal_free_clusters(h));
  internal_expect_pattern_then_zero(h, "BND.BIN", cb, (uint32_t)k_ftr_prefix);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("fat truncate: grow and shrink across a FAT-sector boundary");
}

/**
 * @test internal_test_fat_truncate_offset_follows
 * @brief The offset is left in place on a grow and pulled down by a shrink.
 *
 * @details `ftruncate()` does not move the cursor, except that a shrink below it
 *          cannot leave it dangling past the end. A grow past the cursor leaves
 *          it exactly where it was.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- the paths it drives are
 * covered elsewhere; the truncate mode/handle guard's MC/DC is owned by
 * internal_test_fat_truncate_rejects.)
 *
 * @since 0.1.0

 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_fat_truncate_offset_follows(void)
{
  TEST_BEGIN("fat truncate: offset stays on grow, clamps on shrink");
  internal_build_fat_volume((uint32_t)k_ftr_blocks_fat32, k_ra8_fs_type_fat32);
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t cb = internal_cbytes_of(h);
  internal_write_file(h, "OFF.BIN", 3U * cb);

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "OFF.BIN", k_ra8_fs_mode_append, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_seek(f, cb + 10U));
  uint64_t off = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_truncate(f, (uint64_t)5U * cb)); /* grow past the cursor */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_tell(f, &off));
  TEST_ASSERT_EQ(cb + 10U, off);                         /* offset unmoved  */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_truncate(f, cb / 2U)); /* shrink below it */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_tell(f, &off));
  TEST_ASSERT_EQ(cb / 2U, off); /* offset clamped to size */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("fat truncate: offset stays on grow, clamps on shrink");
}

/**
 * @test internal_test_fat_truncate_rejects
 * @brief Truncate refuses a NULL, a closed, and a read-only handle.
 *
 * @details The guard drives all three vectors of its compound decision, and a
 *          read-only file's length must be untouched by the refused call.
 *
 * @par MC/DC:
 * Decision: `file->in_use == 0 || file->mode == k_ra8_fs_mode_read` in
 * `libs/ra8_fs/src/ra8_fs_fat_truncate.c@internal_truncate_locked` (2 conditions).
 * - Vector 1: in_use=1, mode=write -> false (control: the earlier tests all
 *   truncate a writable open handle).
 * - Vector 2: in_use=0, mode=write -> true (the closed handle here).
 * - Vector 3: in_use=1, mode=read  -> true (the read handle here).
 * Vectors 1+2 prove in_use independently affects the outcome; 1+3 prove the
 * same for mode. N+1 = 3 vectors for N=2: minimal MC/DC. The NULL handle drives
 * the separate first guard.
 *
 * @since 0.1.0

 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_fat_truncate_rejects(void)
{
  TEST_BEGIN("fat truncate: refuses null, closed, and read-only handles");
  internal_build_fat_volume((uint32_t)k_ftr_blocks_fat32, k_ra8_fs_type_fat32);
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t cb = internal_cbytes_of(h);
  internal_write_file(h, "RO.BIN", cb);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_truncate(nullptr, 0U));

  /* Append mode so closing it does not truncate RO.BIN -- the closed handle is
   * the in_use=0 vector, and RO.BIN must survive to the read-only vector. */
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "RO.BIN", k_ra8_fs_mode_append, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_truncate(f, 0U)); /* closed handle */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "RO.BIN", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_truncate(f, 0U)); /* read-only */
  uint64_t size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(f, &size));
  TEST_ASSERT_EQ(cb, size); /* the refused call changed nothing */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("fat truncate: refuses null, closed, and read-only handles");
}

/**
 * @test internal_test_fat16_shrink_grow_roundtrip
 * @brief The verb works on a FAT16 volume, not just FAT32.
 *
 * @details FAT16 uses a different FAT entry width and end-of-chain marker, so a
 *          separate variant proves the shrink re-cap and the grow allocation use
 *          the mount's type rather than assuming FAT32.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- the paths it drives are
 * covered elsewhere; the truncate mode/handle guard's MC/DC is owned by
 * internal_test_fat_truncate_rejects.)
 *
 * @since 0.1.0

 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_fat16_shrink_grow_roundtrip(void)
{
  TEST_BEGIN("fat truncate: shrink and grow on a FAT16 volume");
  internal_build_fat_volume((uint32_t)k_ftr_blocks_fat16, k_ra8_fs_type_fat16);
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_fs_type_fat16, h->type);
  const uint32_t cb = internal_cbytes_of(h);
  internal_write_file(h, "F16.BIN", 3U * cb);
  internal_truncate_named(h, "F16.BIN", cb);
  internal_expect_pattern_then_zero(h, "F16.BIN", cb, cb);
  internal_truncate_named(h, "F16.BIN", (2U * cb) + (uint32_t)k_ftr_tail_odd);
  internal_expect_pattern_then_zero(h, "F16.BIN", (2U * cb) + (uint32_t)k_ftr_tail_odd, cb);
  internal_dump_image("fat16_shrink_grow");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("fat truncate: shrink and grow on a FAT16 volume");
}

/**
 * @brief Run every FAT truncate test.
 * @return 0 on success (a failure aborts inside the assertion macros).
 * @retval 0 Every test passed.
 * @pre The host provides a working heap.
 * @pre No volume is mounted on entry.
 * @post Every test built and released its own volume.
 * @post A success banner is written to the diagnostic descriptor.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_fat_shrink_keeps_prefix();
  internal_test_fat_shrink_to_zero();
  internal_test_fat_grow_zero_fills();
  internal_test_fat_grow_within_sector();
  internal_test_fat_grow_from_empty();
  internal_test_fat_boundary_grow_shrink();
  internal_test_fat_truncate_offset_follows();
  internal_test_fat_truncate_rejects();
  internal_test_fat16_shrink_grow_roundtrip();
  TEST_ASSERT_EQ(
    k_ra8_test_output_ok,
    internal_test_output_fd_text(STDERR_FILENO, "[OK  ] test_ra8_fs_fat_truncate.c\n"));
  return 0;
}
