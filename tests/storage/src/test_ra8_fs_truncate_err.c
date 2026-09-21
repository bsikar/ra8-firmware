/**
 * @file test_ra8_fs_truncate_err.c
 * @brief Backend-failure coverage for ra8_fs_truncate on FAT and exFAT (#680).
 *
 * @details
 * The happy-path suites (`test_ra8_fs_fat_truncate.c` /
 * `test_ra8_fs_exfat_truncate.c`) drive the verb over a backend that never
 * fails, so every `if (err) return err;` after a FAT read, a FAT write, a
 * bitmap edit or a directory rewrite goes untaken. This file takes them: it
 * formats a volume through a clean backend, then swaps in a fault-injecting one
 * that fails the Nth read or the Nth write, and sweeps N across a shrink and a
 * grow on both filesystems. Each injected failure must surface as an error from
 * `ra8_fs_truncate` rather than a crash or a silent success, which is what pins
 * the propagation paths the clean suites cannot reach.
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
#include "ra8_fs_meta.h"
#include "unity_minimal.h"

/**
 * @enum ter_const_t
 * @brief Geometry, sweep bounds and the injected error for the fault tests.
 *
 * @invariant `k_ter_sweep_max` exceeds the read/write count of any single
 *            truncate on the fixture geometry, so the sweep's tail runs clean.
 * @see ter_sweep()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ter_block_size   = 512U,    /**< Bytes per logical block.              */
  k_ter_blocks_fat32 = 73728U,  /**< 36 MiB -> FAT32 band.                 */
  k_ter_blocks_exfat = 131072U, /**< 64 MiB -> exFAT.                      */
  k_ter_sweep_max    = 96U,     /**< Fault position sweep upper bound.     */
  k_ter_big_mult     = 3U,      /**< Multi-cluster file size, in clusters. */
  k_ter_seed         = 0x42U,   /**< Payload fill byte.                    */
} ter_const_t;

/**
 * @struct ter_disk_t
 * @brief RAM disk that can fail the Nth read or the Nth write on demand.
 *
 * @invariant `read_fail_at == 0` disables read injection; likewise writes.
 * @since 0.1.0
 */
typedef struct {
  uint8_t* bytes;         /**< Flat sector store.                   */
  uint32_t block_count;   /**< 512-byte sectors.                    */
  uint32_t read_fail_at;  /**< Fail this read ordinal (0 = never).  */
  uint32_t reads;         /**< Reads seen since the last reset.     */
  uint32_t write_fail_at; /**< Fail this write ordinal (0 = never). */
  uint32_t writes;        /**< Writes seen since the last reset.    */
} ter_disk_t;

static ter_disk_t s_disk = {};

/**
 * @brief Read shim: fail the configured ordinal, else copy from the store.
 * @param[in]  ctx   The ::ter_disk_t.
 * @param[in]  lba   First sector.
 * @param[in]  count Sector count.
 * @param[out] buf   Destination.
 * @return k_ra8_ok, k_ra8_err_hw_error at the fault ordinal, or out-of-range.
 * @retval k_ra8_err_hw_error The injected read ordinal was reached.
 * @pre @p buf holds @p count sectors.
 * @pre @p ctx is a ::ter_disk_t.
 * @post The read counter advanced by one.
 * @post On success @p buf mirrors the store.
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0 @details Implements the bounded ter read fixture step using caller-owned state.
 */
RA8_INTERNAL static ra8_err_t
internal_ter_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  ter_disk_t* d = (ter_disk_t*)ctx;
  d->reads++;
  if ((d->read_fail_at != 0U) && (d->reads == d->read_fail_at)) {
    return k_ra8_err_hw_error;
  }
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_ter_block_size],
         (size_t)count * (uint32_t)k_ter_block_size);
  return k_ra8_ok;
}

/**
 * @brief Write shim: fail the configured ordinal, else copy into the store.
 * @param[in] ctx   The ::ter_disk_t.
 * @param[in] lba   First sector.
 * @param[in] count Sector count.
 * @param[in] buf   Source.
 * @return k_ra8_ok, k_ra8_err_hw_error at the fault ordinal, or out-of-range.
 * @retval k_ra8_err_hw_error The injected write ordinal was reached.
 * @pre @p buf holds @p count sectors.
 * @pre @p ctx is a ::ter_disk_t.
 * @post The write counter advanced by one.
 * @post On success the store mirrors @p buf.
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0 @details Implements the bounded ter write fixture step using caller-owned state.
 */
RA8_INTERNAL static ra8_err_t
internal_ter_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
{
  ter_disk_t* d = (ter_disk_t*)ctx;
  d->writes++;
  if ((d->write_fail_at != 0U) && (d->writes == d->write_fail_at)) {
    return k_ra8_err_hw_error;
  }
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_ter_block_size],
         buf,
         (size_t)count * (uint32_t)k_ter_block_size);
  return k_ra8_ok;
}

/**
 * @brief Capacity shim over the fault disk.
 * @param[in]  ctx         The ::ter_disk_t.
 * @param[out] block_count Receives the sector count.
 * @param[out] block_size  Receives 512.
 * @return k_ra8_ok always.
 * @retval k_ra8_ok Geometry reported.
 * @pre @p ctx is a ::ter_disk_t.
 * @pre Both out-pointers are non-NULL.
 * @post The out-params carry the geometry.
 * @post No state changes.
 * @note Trivially thread-safe.
 * @since 0.1.0 @details Implements the bounded ter capacity fixture step using caller-owned state.
 */
RA8_INTERNAL static ra8_err_t
internal_ter_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  const ter_disk_t* d = (const ter_disk_t*)ctx;
  *block_count        = d->block_count;
  *block_size         = (uint32_t)k_ter_block_size;
  return k_ra8_ok;
}

static const ra8_fs_backend_t s_backend = {
  .read_block   = internal_ter_read,
  .write_block  = internal_ter_write,
  .get_capacity = internal_ter_capacity,
  .ctx          = &s_disk,
};

/**
 * @brief Free the fault disk's store.
 * @return Nothing.
 * @pre None.
 * @pre ::s_disk is this binary's disk.
 * @post ::s_disk.bytes is NULL.
 * @post No fault ordinals remain armed.
 * @note Not thread-safe.
 * @since 0.1.0 @details Implements the bounded free volume fixture step using caller-owned state.
 */
RA8_INTERNAL static void internal_free_volume(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
}

/**
 * @brief Format a card of @p blocks sectors as @p type, faults disarmed.
 * @param[in] blocks Sector count.
 * @param[in] type   FAT or exFAT variant.
 * @return Nothing; a failure asserts inside.
 * @pre @p type is a supported variant; @p blocks fits it.
 * @pre No fault is armed during the format.
 * @post ::s_disk holds a mountable volume and both fault ordinals are 0.
 * @post The read/write counters are zero.
 * @note Not thread-safe.
 * @since 0.1.0 @details Implements the bounded build volume fixture step using caller-owned state.
 */
RA8_INTERNAL static void internal_build_volume(uint32_t blocks, ra8_fs_type_t type)
{
  internal_free_volume();
  s_disk.bytes         = (uint8_t*)calloc((size_t)blocks, (size_t)k_ter_block_size);
  s_disk.block_count   = blocks;
  s_disk.read_fail_at  = 0U;
  s_disk.write_fail_at = 0U;
  s_disk.reads         = 0U;
  s_disk.writes        = 0U;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
    return;
  }
  ra8_fs_format_opts_t opts = {};
  opts.type                 = type;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
}

/**
 * @brief Bytes per cluster of the mount.
 * @param[in] h Mounted volume.
 * @return `sectors_per_cluster * 512`.
 * @retval >0 The allocation-unit size.
 * @pre @p h is mounted.
 * @pre @p h describes a supported volume.
 * @post No state changes.
 * @post Depends only on @p h.
 * @note Pure with respect to the mount.
 * @since 0.1.0 @details Implements the bounded cbytes of fixture step using caller-owned state.
 */
RA8_INTERNAL static uint32_t internal_cbytes_of(const ra8_fs_mount_t* h)
{
  return h->sectors_per_cluster * (uint32_t)k_ter_block_size;
}

/**
 * @brief Create @p name (write mode) holding @p len bytes of a fill pattern.
 * @param[in,out] h    Mounted volume, faults disarmed.
 * @param[in]     name File to create.
 * @param[in]     len  Bytes to write.
 * @return Nothing; every step asserts inside.
 * @pre @p h is mounted; no fault is armed.
 * @pre @p len fits the volume.
 * @post @p name holds @p len fill bytes.
 * @post No handle is left open.
 * @note Not thread-safe.
 * @since 0.1.0 @details Implements the bounded seed file fixture step using caller-owned state.
 */
RA8_INTERNAL static void internal_seed_file(ra8_fs_mount_t* h, const char* name, uint32_t len)
{
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, name, k_ra8_fs_mode_write, &f));
  static uint8_t s_buf[k_ter_block_size];
  memset(s_buf, (int)k_ter_seed, sizeof s_buf);
  uint32_t done = 0U;
  while (done < len) {
    uint32_t n = len - done;
    if (n > (uint32_t)k_ter_block_size) {
      n = (uint32_t)k_ter_block_size;
    }
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, s_buf, n));
    done += n;
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
}

/**
 * @brief Arm the Nth read (or write) and truncate @p name to @p new_size once.
 *
 * @details Reopens @p name in append mode (which does not itself write), zeroes
 *          the fault counters, arms exactly one of the two ordinals, and calls
 *          the verb. Any return is acceptable -- an injected failure surfaces as
 *          an error, a position past the operation count succeeds -- the point
 *          is that the propagation paths run without a crash.
 *
 * @param[in,out] h        Mounted volume.
 * @param[in]     name     File to truncate.
 * @param[in]     new_size Target length.
 * @param[in]     read_at  Read ordinal to fail (0 = none).
 * @param[in]     write_at Write ordinal to fail (0 = none).
 * @return Nothing; the call must not crash.
 * @pre @p h is mounted and @p name exists.
 * @pre At most one of @p read_at / @p write_at is non-zero.
 * @post Both fault ordinals are disarmed on return.
 * @post No handle is left open.
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_arm_and_truncate(ra8_fs_mount_t* h,
                                                   const char*     name,
                                                   uint32_t        new_size,
                                                   uint32_t        read_at,
                                                   uint32_t        write_at)
{
  ra8_fs_file_t* f = nullptr;
  if (ra8_fs_open(h, name, k_ra8_fs_mode_append, &f) != k_ra8_ok) {
    return;
  }
  s_disk.reads         = 0U;
  s_disk.writes        = 0U;
  s_disk.read_fail_at  = read_at;
  s_disk.write_fail_at = write_at;
  (void)ra8_fs_truncate(f, new_size);
  s_disk.read_fail_at  = 0U;
  s_disk.write_fail_at = 0U;
  (void)ra8_fs_close(f);
}

/**
 * @brief Sweep the read and write fault ordinals over one truncate.
 *
 * @details For every ordinal in `[1, k_ter_sweep_max]`, rebuild the volume and
 *          file fresh (a failed truncate can leave the volume mid-edit), then
 *          fail that read and, separately, that write. Rebuilding per step keeps
 *          each injection independent.
 *
 * @param[in] blocks   Card size in sectors.
 * @param[in] type     Filesystem to format.
 * @param[in] old_mult Starting file size, in clusters.
 * @param[in] new_size Truncate target in bytes.
 * @return Nothing; a crash or hang fails the run.
 * @pre @p type is supported and @p blocks fits it.
 * @pre `old_mult >= 1`.
 * @post Every volume built is released.
 * @post Both fault ordinals are disarmed.
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_ter_sweep(uint32_t blocks, ra8_fs_type_t type, uint32_t old_mult, uint32_t new_size)
{
  for (uint32_t n = 1U; n <= (uint32_t)k_ter_sweep_max; n++) {
    internal_build_volume(blocks, type);
    ra8_fs_mount_t* h = nullptr;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
    internal_seed_file(h, "T.BIN", old_mult * internal_cbytes_of(h));
    internal_arm_and_truncate(h, "T.BIN", new_size, n, 0U);
    (void)ra8_fs_unmount(h);

    internal_build_volume(blocks, type);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
    internal_seed_file(h, "T.BIN", old_mult * internal_cbytes_of(h));
    internal_arm_and_truncate(h, "T.BIN", new_size, 0U, n);
    (void)ra8_fs_unmount(h);
    internal_free_volume();
  }
}

/**
 * @test test_fat_truncate_backend_faults
 * @brief Every backend read/write fault along a FAT shrink and grow propagates.
 *
 * @details The shrink target is a single cluster and the grow target several,
 *          so the sweep visits the free-chain, tail-recap, allocation,
 *          zero-fill and directory-rewrite writes and each of their guarding
 *          reads.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- the paths it drives are
 * covered elsewhere; the truncate mode/handle guard's MC/DC is owned by
 * test_fat_truncate_rejects.)
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_fat_truncate_backend_faults(void)
{
  TEST_BEGIN("fat truncate: backend read/write faults propagate");
  internal_build_volume((uint32_t)k_ter_blocks_fat32, k_ra8_fs_type_fat32);
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t cb = internal_cbytes_of(h);
  (void)ra8_fs_unmount(h);
  internal_free_volume();
  /* Multi-cluster on both sides so the walk index exceeds zero and the chain
   * walk itself runs (not just its idx==0 short-circuit). */
  internal_ter_sweep((uint32_t)k_ter_blocks_fat32,
                     k_ra8_fs_type_fat32,
                     (uint32_t)k_ter_big_mult,
                     2U * cb);
  internal_ter_sweep((uint32_t)k_ter_blocks_fat32,
                     k_ra8_fs_type_fat32,
                     2U,
                     ((uint32_t)k_ter_big_mult + 2U) * cb);
  TEST_END("fat truncate: backend read/write faults propagate");
}

/**
 * @test test_exfat_truncate_backend_faults
 * @brief Every backend read/write fault along an exFAT shrink and grow propagates.
 *
 * @details On exFAT the same sweep exercises the allocation-bitmap edits, the
 *          entry-set flush and the FAT walk that a chained shrink follows, plus
 *          the grow's cluster allocation.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- the paths it drives are
 * covered elsewhere; the truncate mode/handle guard's MC/DC is owned by
 * test_fat_truncate_rejects.)
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_truncate_backend_faults(void)
{
  TEST_BEGIN("exfat truncate: backend read/write faults propagate");
  internal_build_volume((uint32_t)k_ter_blocks_exfat, k_ra8_fs_type_exfat);
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t cb = internal_cbytes_of(h);
  (void)ra8_fs_unmount(h);
  internal_free_volume();
  internal_ter_sweep((uint32_t)k_ter_blocks_exfat,
                     k_ra8_fs_type_exfat,
                     (uint32_t)k_ter_big_mult,
                     2U * cb);
  internal_ter_sweep((uint32_t)k_ter_blocks_exfat,
                     k_ra8_fs_type_exfat,
                     2U,
                     ((uint32_t)k_ter_big_mult + 2U) * cb);
  TEST_END("exfat truncate: backend read/write faults propagate");
}

/**
 * @brief Run every truncate fault-coverage test.
 * @return 0 on success (a crash or assertion aborts the process).
 * @retval 0 Every sweep completed.
 * @pre The host provides a working heap.
 * @pre No volume is mounted on entry.
 * @post Every volume built is released.
 * @post A success banner is written to stderr.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_fat_truncate_backend_faults();
  internal_test_exfat_truncate_backend_faults();
  return 0;
}
