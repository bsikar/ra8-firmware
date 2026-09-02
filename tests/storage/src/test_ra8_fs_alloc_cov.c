/**
 * @file test_ra8_fs_alloc_cov.c
 * @brief The allocator state's fallbacks and its I/O failure legs (#607).
 *
 * @details
 * `test_ra8_fs_alloc_perf.c` proves what the next-free hint, the FAT sector
 * cache and the FSInfo bookkeeping DO. This file proves what they do when
 * something is wrong, which is the half no happy-path test can reach:
 *
 *   - **No bound slot.** Covered by its sibling `test_ra8_fs_alloc_state.c`,
 *     which reaches the accessors directly and therefore cannot share this
 *     file's fixture: the fixture's on-disk-layout enums and the module's own
 *     carry the same names.
 *   - **A hostile FSInfo pointer.** `BPB_FSInfo` arrives from whatever last
 *     formatted the card. A value outside the volume's own reserved region
 *     names something that is not an FSInfo sector, and following it would
 *     read -- and later WRITE -- a sector belonging to the FAT or the data
 *     region.
 *   - **I/O failures.** Every backend call in the FSInfo path is checked, and
 *     a gate that never exercised those returns would not know whether the
 *     checks were real. Each is driven by counting reads down to the exact
 *     call under test.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fs_fat_dir_test_util.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "unity_minimal.h"

/**
 * @enum ac_const_t
 * @brief Volume geometry, FSInfo offsets, and injection counts.
 *
 * @details `k_ac_reads_*` are read budgets, chosen so the countdown lands on
 *          exactly the backend call each case is about. They are asserted
 *          indirectly: every case checks the error code the driver reports,
 *          which is the only thing the injection can produce.
 *
 * @invariant `k_ac_f32_blocks` yields `count_of_clusters >= 65525`.
 * @see build_fat32_vol()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ac_f32_blocks   = 81920U,      /**< 40 MiB: a comfortable FAT32 volume.  */
  k_ac_fsi_lba      = 1U,          /**< Where the formatter puts FSInfo.     */
  k_ac_bpb_off_fsi  = 48U,         /**< BPB_FSInfo (u16) in the boot sector. */
  k_ac_fsi_off_free = 488U,        /**< FSI_Free_Count offset.               */
  k_ac_fsi_bad_lba  = 0xFFFFU,     /**< Far outside the reserved region.     */
  k_ac_payload      = 512U,        /**< Bytes written by the seeded file.    */
  k_ac_reads_locate = 0U,          /**< Fail the FSInfo locate re-read.      */
  k_ac_reads_sector = 1U,          /**< Fail the FSInfo sector read.         */
  k_ac_shift_byte   = 8U,          /**< Byte position in a word.             */
  k_ac_byte_mask    = 0xFFU,       /**< Low byte of a word.                  */
  k_ac_free_unknown = 0xFFFFFFFFU, /**< FSInfo "count not known".            */
} ac_const_t;

/** @brief A standalone backend over ::s_inject, for injecting during mount. */
static const ra8_fs_backend_t s_inject_backend = {
  .read_block   = internal_inj_read,
  .write_block  = internal_inj_write,
  .get_capacity = internal_inj_capacity,
  .ctx          = &s_inject,
};

/**
 * @brief Point ::s_inject at the fixture disk with the given read budget.
 *
 * @param[in] reads_left Reads permitted before the next one fails.
 *
 * @return Nothing.
 *
 * @pre `s_disk` has been built.
 * @pre No mount is currently using ::s_inject.
 * @post ::s_inject_backend serves the fixture disk under that budget.
 * @post Writes are permitted.
 *
 * @note Not thread-safe; the suite is single-threaded.
 * @since 0.1.0 @details Implements the bounded arm inject fixture step using caller-owned state.
 */
RA8_INTERNAL static void internal_arm_inject(uint32_t reads_left)
{
  s_inject.bytes       = s_disk.bytes;
  s_inject.block_count = s_disk.block_count;
  s_inject.byte_count  = s_disk.byte_count;
  s_inject.reads_left  = reads_left;
  s_inject.writes_fail = 0U;
}

/**
 * @brief Format a FAT32 volume into the fixture's RAM disk.
 *
 * @return Nothing.
 *
 * @pre The heap can hold 40 MiB.
 * @pre Any previous volume has been freed.
 * @post `s_disk` holds a FAT32 image with a valid FSInfo sector.
 * @post The image is ready to mount through ::s_backend.
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
  s_disk.block_count = (uint32_t)k_ac_f32_blocks;
  s_disk.byte_count  = (uint32_t)k_ac_f32_blocks * (uint32_t)k_geo_blk_sz;
  s_disk.bytes       = (uint8_t*)calloc(1, s_disk.byte_count);
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat32;
  opts.label                = "ALLOCCOV";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
}

/**
 * @brief Read the FSInfo free-count field straight out of the RAM disk.
 *
 * @return The committed free count.
 * @retval 0..UINT32_MAX The four bytes, little-endian.
 *
 * @pre `s_disk.bytes` holds a formatted FAT32 volume.
 * @pre The FSInfo sector is at ::k_ac_fsi_lba.
 * @post No state modified.
 * @post No backend call is made.
 *
 * @note Reads the fixture's memory directly, bypassing the driver.
 * @since 0.1.0 @details Implements the bounded read fsinfo free fixture step using caller-owned state.
 */
RA8_INTERNAL static uint32_t internal_read_fsinfo_free(void)
{
  const uint32_t at =
    ((uint32_t)k_ac_fsi_lba * (uint32_t)k_geo_blk_sz) + (uint32_t)k_ac_fsi_off_free;
  return (uint32_t)s_disk.bytes[at] | ((uint32_t)s_disk.bytes[at + 1U] << k_ac_shift_byte) |
         ((uint32_t)s_disk.bytes[at + 2U] << (2U * (uint32_t)k_ac_shift_byte)) |
         ((uint32_t)s_disk.bytes[at + 3U] << (3U * (uint32_t)k_ac_shift_byte));
}

/**
 * @brief Create one small file on @p h.
 *
 * @param[in] h Mounted volume.
 *
 * @return Nothing.
 *
 * @pre @p h is mounted read-write with room for the file.
 * @pre The clock binding is irrelevant here.
 * @post `DATA.BIN` holds ::k_ac_payload bytes.
 * @post The volume's tracked free count has dropped.
 *
 * @note Uses the public seam only.
 * @since 0.1.0 @details Implements the bounded seed file fixture step using caller-owned state.
 */
RA8_INTERNAL static void internal_seed_file(ra8_fs_mount_t* h)
{
  uint8_t payload[k_ac_payload] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "DATA.BIN", payload, (uint32_t)k_ac_payload));
}

/**
 * @test test_fsinfo_pointer_out_of_range
 * @brief A `BPB_FSInfo` naming a sector outside the reserved region is treated
 *        as "no FSInfo", and nothing is ever written there.
 *
 * @details The pointer arrives from whatever last formatted the card.
 *          Following one that leaves the reserved region would have the
 *          driver read -- and at unmount WRITE -- a sector belonging to the
 *          FAT or the data region, corrupting a volume it was only asked to
 *          keep tidy.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- `lba >= m->reserved_sectors` in
 * priv_fsinfo_locate is a single condition, driven true here and false by
 * every other FAT32 case in the suite)
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_fsinfo_pointer_out_of_range(void)
{
  TEST_BEGIN("fs alloc: an out-of-range BPB_FSInfo is treated as absent");
  internal_build_fat32_vol();
  const uint32_t before          = internal_read_fsinfo_free();
  s_disk.bytes[k_ac_bpb_off_fsi] = (uint8_t)((uint32_t)k_ac_fsi_bad_lba & (uint32_t)k_ac_byte_mask);
  s_disk.bytes[k_ac_bpb_off_fsi + 1U] =
    (uint8_t)(((uint32_t)k_ac_fsi_bad_lba >> k_ac_shift_byte) & (uint32_t)k_ac_byte_mask);

  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  internal_seed_file(h);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  /* The real FSInfo sector was never consulted, so it was never rewritten. */
  TEST_ASSERT_EQ(before, internal_read_fsinfo_free());
  internal_free_vol();
  TEST_END("fs alloc: an out-of-range BPB_FSInfo is treated as absent");
}

/**
 * @test test_fsinfo_seed_read_failures
 * @brief A backend that cannot read the boot sector or the FSInfo sector
 *        fails the mount instead of quietly mounting with invented state.
 *
 * @details A sector inside the volume's own reserved region that will not read
 *          is a sick card, and reporting the mount as healthy would hand the
 *          caller a volume whose free-space bookkeeping is a guess.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- both legs are single-condition
 * `if (err != k_ra8_ok)` propagations, driven by a read countdown)
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_fsinfo_seed_read_failures(void)
{
  TEST_BEGIN("fs alloc: FSInfo seed reports its read failures");
  internal_build_fat32_vol();

  /* The seed re-reads the boot sector to find BPB_FSInfo. Fail that read. */
  internal_arm_inject((uint32_t)k_ac_reads_locate);
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_mount(&s_inject_backend, &h));

  /* Then let the locate through and fail the FSInfo sector read itself. */
  internal_arm_inject((uint32_t)k_ac_reads_sector);
  h = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_mount(&s_inject_backend, &h));

  /* The failed mounts released their slots, so a healthy mount still works. */
  h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("fs alloc: FSInfo seed reports its read failures");
}

/**
 * @test test_fsinfo_flush_io_failures
 * @brief A writeback that cannot read or cannot write the FSInfo sector is
 *        reported through `ra8_fs_unmount()`, and the slot is released anyway.
 *
 * @details Swallowing the error would leave a card whose reported free space
 *          is wrong with nothing to say so; refusing the unmount would strand
 *          the mount slot. The contract is "report it, release it", and both
 *          halves are asserted here.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the read and write legs of
 * priv_fsinfo_flush are single-condition error propagations)
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_fsinfo_flush_io_failures(void)
{
  TEST_BEGIN("fs alloc: FSInfo writeback reports its I/O failures");
  internal_build_fat32_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  internal_seed_file(h);
  /* Closing the file already flushed, so give the volume something to say:
   * unlinking returns clusters and marks the count dirty again. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "DATA.BIN"));

  /* Read leg: no reads left at all, so the FSInfo read-modify-write fails. */
  const ra8_fs_backend_t saved = h->backend;
  internal_swap_to_inject(h, 0U, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_unmount(h));
  h->backend = saved;

  /* Write leg: reads succeed, every write fails. */
  h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  internal_seed_file(h);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "DATA.BIN"));
  internal_swap_to_inject(h, (uint32_t)k_geo_reads_inf, 1U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_unmount(h));
  h->backend = saved;

  /* Both slots were released despite the failures, so mounting still works. */
  h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT(internal_read_fsinfo_free() != (uint32_t)k_ac_free_unknown);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("fs alloc: FSInfo writeback reports its I/O failures");
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
 * @post No mount slot is left in use.
 *
 * @note Single-threaded by construction.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_fsinfo_pointer_out_of_range();
  internal_test_fsinfo_seed_read_failures();
  internal_test_fsinfo_flush_io_failures();
  return 0;
}
