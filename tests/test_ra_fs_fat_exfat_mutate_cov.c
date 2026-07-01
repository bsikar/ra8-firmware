/**
 * @file tests/test_ra_fs_fat_exfat_mutate_cov.c
 * @brief Coverage-boost tests for `ra_fs_fat_exfat_mutate.c`.
 *
 * @details
 * Reaches the hard-to-reach branches in `priv_exfat_unlink`,
 * `priv_exfat_rename`, `priv_exfat_free_clusters`, `priv_exfat_listdir`,
 * and `priv_exfat_gather_name` by using a RAM-backed 64 MiB exFAT volume
 * and patching raw directory entries / FAT entries directly in the sector
 * store after the normal filesystem API creates the baseline state.
 *
 * Every uncovered line is handled exactly once: either a positive test
 * exercises it, or a `GCOVR_EXCL_LINE` tag marks it as an I/O-failure /
 * scan-limit path that cannot be injected without a failing backend.
 *
 * @par Target lines
 * 132-133, 146-147, 151-152, 206, 214, 284, 287, 303-306, 309-311,
 * 314-315, 317-318, 332, 420, 423, 429, 442, 445, 489, 530.
 * (Remaining uncovered lines are GCOVR_EXCL in the source file.)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra_err.h"
#include "ra_fs.h"
#include "unity_minimal.h"

/* ---- sizing constants --------------------------------------------------- */

/**
 * @enum mut_cov_const_t
 * @brief Sizing / offset constants for the exFAT mutate coverage tests.
 *
 * @details All magic numbers in this file appear here as named enum values.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_mut_block_size      = 512U,        /**< Bytes per sector.                      */
  k_mut_blocks_exfat    = 131072U,     /**< 64 MiB -- smallest workable exFAT vol. */
  k_mut_entry_bytes     = 32U,         /**< Directory entry size.                  */
  k_mut_strm_off_clus   = 0x14U,       /**< Stream-ext FirstCluster byte offset.   */
  k_mut_strm_off_dlen   = 0x18U,       /**< Stream-ext DataLength byte offset.     */
  k_mut_strm_off_flags  = 1U,          /**< Stream-ext GeneralSecondaryFlags byte. */
  k_mut_file_secnt_off  = 1U,          /**< File entry SecondaryCount byte.        */
  k_mut_payload_two_cls = 5000U,       /**< Payload > 1 cluster -> two clusters.   */
  k_mut_fat_eoc         = 0xFFFFFFFFU, /**< exFAT end-of-chain FAT value.          */
  k_mut_no_fat_bit      = 0x02U,       /**< NoFatChain bit in SecondaryFlags.      */
  k_mut_max_secondary   = 19U,         /**< k_exfat_set_max_entries from source.   */
  k_mut_root_bitmap_idx = 0U,          /**< Root dir: bitmap entry index.          */
  k_mut_root_upcase_idx = 1U,          /**< Root dir: up-case entry index.         */
  k_mut_root_label_idx  = 2U,          /**< Root dir: label entry index.           */
  k_mut_root_file0_idx  = 3U,          /**< Root dir: first user File entry.       */
  k_mut_root_strm0_idx  = 4U,          /**< Root dir: first user Stream entry.     */
  k_mut_root_name0_idx  = 5U,          /**< Root dir: first user Name entry.       */
  k_mut_root_file1_idx  = 6U,          /**< Root dir: second user File entry.      */
  k_mut_shift_byte8     = 8U,          /**< 8-bit left shift for LE16/LE32 packs.  */
  k_mut_shift_byte16    = 16U,         /**< 16-bit left shift.                     */
  k_mut_shift_byte24    = 24U,         /**< 24-bit left shift.                     */
  k_mut_mask_byte       = 0xFFU,       /**< Byte mask.                             */
  k_mut_type_file       = 0x85U,       /**< exFAT File entry type byte.            */
  k_mut_type_stream     = 0xC0U,       /**< exFAT Stream-extension type byte.      */
  k_mut_type_name       = 0xC1U,       /**< exFAT File-name type byte.             */
  k_mut_type_bogus      = 0x42U,       /**< Invalid type used to corrupt entries.  */
} mut_cov_const_t;

/* ---- RAM-backed block device ------------------------------------------- */

/**
 * @struct mut_disk_t
 * @brief Memory-backed disk handed to `ra_fs` as a block device.
 *
 * @details Sector store is a flat `malloc` buffer.  All reads and writes go
 *          straight to this buffer, making it trivial to corrupt directory
 *          entries by patching individual bytes after normal filesystem ops.
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t* bytes;       /**< Flat sector store.         */
  uint32_t block_count; /**< Number of 512-byte blocks. */
} mut_disk_t;

/**
 * @struct mut_list_ctx_t
 * @brief Listdir callback context for asserting entry counts.
 *
 * @since 0.1.0
 */
typedef struct {
  uint32_t count; /**< Number of directory entries reported. */
} mut_list_ctx_t;

/** @var s_disk
 * @brief Module-level RAM disk instance shared across all tests.
 * @warning Modify only through `build_exfat_volume()` / `free_volume()`.
 * @since 0.1.0
 */
static mut_disk_t s_disk = {};

/**
 * @brief `ra_fs` backend: read @p count sectors from the RAM disk.
 *
 * @param[in]  ctx   Pointer to `mut_disk_t`.
 * @param[in]  lba   Start sector.
 * @param[in]  count Sector count.
 * @param[out] buf   Destination buffer.
 *
 * @return k_ra_ok or k_ra_err_out_of_range.
 * @retval k_ra_ok Sectors copied into @p buf.
 * @retval k_ra_err_out_of_range Request exceeds disk size.
 *
 * @pre ctx and buf are non-NULL.
 * @post buf contains the requested sectors on success.
 *
 * @since 0.1.0
 */
static ra_err_t mut_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  const mut_disk_t* d = (const mut_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[lba * (uint32_t)k_mut_block_size],
         (size_t)count * (size_t)k_mut_block_size);
  return k_ra_ok;
}

/**
 * @brief `ra_fs` backend: write @p count sectors to the RAM disk.
 *
 * @param[in] ctx   Pointer to `mut_disk_t`.
 * @param[in] lba   Start sector.
 * @param[in] count Sector count.
 * @param[in] buf   Source buffer.
 *
 * @return k_ra_ok or k_ra_err_out_of_range.
 * @retval k_ra_ok Sectors copied from @p buf.
 * @retval k_ra_err_out_of_range Request exceeds disk size.
 *
 * @pre ctx and buf are non-NULL.
 * @post Sector data in the RAM disk equals @p buf on success.
 *
 * @since 0.1.0
 */
static ra_err_t mut_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  mut_disk_t* d = (mut_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra_err_out_of_range;
  }
  memcpy(&d->bytes[lba * (uint32_t)k_mut_block_size],
         buf,
         (size_t)count * (size_t)k_mut_block_size);
  return k_ra_ok;
}

/**
 * @brief `ra_fs` backend: report disk geometry.
 *
 * @param[in]  ctx         Pointer to `mut_disk_t`.
 * @param[out] block_count Receives the sector count.
 * @param[out] block_size  Receives 512.
 *
 * @return k_ra_ok always.
 * @retval k_ra_ok Geometry populated.
 *
 * @pre ctx, block_count, block_size are non-NULL.
 * @post *block_count and *block_size reflect the disk dimensions.
 *
 * @since 0.1.0
 */
static ra_err_t mut_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  const mut_disk_t* d = (const mut_disk_t*)ctx;
  *block_count        = d->block_count;
  *block_size         = (uint32_t)k_mut_block_size;
  return k_ra_ok;
}

/** @var s_backend
 * @brief Shared backend wired to `s_disk` for all tests.
 * @since 0.1.0
 */
static const ra_fs_backend_t s_backend = {
  .read_block   = mut_read,
  .write_block  = mut_write,
  .get_capacity = mut_capacity,
  .ctx          = &s_disk,
};

/* ---- harness helpers ---------------------------------------------------- */

/**
 * @brief Release the heap buffer held by `s_disk`.
 *
 * @pre None.
 * @post `s_disk.bytes` is `nullptr`.
 *
 * @since 0.1.0
 */
static void free_volume(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
}

/**
 * @brief Allocate a fresh garbage-filled 64 MiB RAM disk and format as exFAT.
 *
 * @details Pre-fills with 0xA5 so a valid format proves the formatter
 *          actually wrote the VBR (not a lucky all-zero read).  No label is
 *          written so the root directory starts with:
 *          [bitmap | upcase | label(empty) | EOD ...].
 *
 * @pre `s_disk.bytes` is `nullptr` (or call `free_volume()` first).
 * @post `s_disk` holds a freshly formatted exFAT image.
 *
 * @since 0.1.0
 */
static void build_exfat_volume(void)
{
  free_volume();
  const size_t total = (size_t)k_mut_blocks_exfat * (size_t)k_mut_block_size;
  s_disk.bytes       = (uint8_t*)malloc(total);
  s_disk.block_count = (uint32_t)k_mut_blocks_exfat;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "malloc failed for exFAT volume");
  }
  memset(s_disk.bytes, 0xA5, total);
  ra_fs_format_opts_t opts = {};
  opts.type                = k_ra_fs_type_exfat;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_format(&s_backend, &opts));
}

/**
 * @brief Listdir callback that increments the count in @p ctx.
 *
 * @param[in] name Unused.
 * @param[in] attr Unused.
 * @param[in] size Unused.
 * @param[in] ctx  Pointer to a `mut_list_ctx_t`.
 *
 * @pre ctx is non-NULL.
 * @post `((mut_list_ctx_t*)ctx)->count` is incremented by one.
 *
 * @since 0.1.0
 */
static void count_cb(const char* name, uint8_t attr, uint32_t size, void* ctx)
{
  (void)name;
  (void)attr;
  (void)size;
  ((mut_list_ctx_t*)ctx)->count++;
}

/**
 * @brief Byte offset in `s_disk.bytes` for root-dir entry @p idx.
 *
 * @param[in] h   Mounted exFAT volume.
 * @param[in] idx Entry index within the root cluster (0-based).
 *
 * @return Absolute byte offset within `s_disk.bytes`.
 *
 * @pre h is non-NULL and mounted.
 * @post Return value is within the root cluster's sector range.
 *
 * @since 0.1.0
 */
static uint32_t root_byte(const ra_fs_mount_t* h, uint32_t idx)
{
  const uint32_t root_lba = h->first_data_lba + (h->root_cluster - 2U) * h->sectors_per_cluster;
  return root_lba * (uint32_t)k_mut_block_size + idx * (uint32_t)k_mut_entry_bytes;
}

/**
 * @brief Byte offset in `s_disk.bytes` for the FAT entry of cluster @p clus.
 *
 * @param[in] h    Mounted exFAT volume.
 * @param[in] clus Cluster number.
 *
 * @return Absolute byte offset of the 4-byte FAT entry for @p clus.
 *
 * @pre h is non-NULL and mounted.
 * @post Return value addresses a valid 4-byte region in `s_disk.bytes`.
 *
 * @since 0.1.0
 */
static uint32_t fat_byte(const ra_fs_mount_t* h, uint32_t clus)
{
  return h->first_fat_lba * (uint32_t)k_mut_block_size + clus * 4U;
}

/**
 * @brief Write a 4-byte LE value at byte offset @p off in `s_disk.bytes`.
 *
 * @param[in] off Absolute byte offset.
 * @param[in] val 32-bit value to write in little-endian order.
 *
 * @pre off + 4 is within the disk image.
 * @post Bytes at @p off encode @p val in LE order.
 *
 * @since 0.1.0
 */
static void disk_set_u32le(uint32_t off, uint32_t val)
{
  uint8_t* p = &s_disk.bytes[off];
  p[0]       = (uint8_t)(val & (uint32_t)k_mut_mask_byte);
  p[1]       = (uint8_t)((val >> (uint32_t)k_mut_shift_byte8) & (uint32_t)k_mut_mask_byte);
  p[2]       = (uint8_t)((val >> (uint32_t)k_mut_shift_byte16) & (uint32_t)k_mut_mask_byte);
  p[3]       = (uint8_t)((val >> (uint32_t)k_mut_shift_byte24) & (uint32_t)k_mut_mask_byte);
}

/**
 * @brief Read a 4-byte LE value from byte offset @p off in `s_disk.bytes`.
 *
 * @param[in] off Absolute byte offset.
 *
 * @return LE uint32 decoded from the four bytes.
 *
 * @pre off + 4 is within the disk image.
 * @post Return value equals the LE uint32 stored at @p off.
 *
 * @since 0.1.0
 */
static uint32_t disk_get_u32le(uint32_t off)
{
  const uint8_t* p = &s_disk.bytes[off];
  return (uint32_t)p[0] | ((uint32_t)p[1] << (uint32_t)k_mut_shift_byte8) |
         ((uint32_t)p[2] << (uint32_t)k_mut_shift_byte16) |
         ((uint32_t)p[3] << (uint32_t)k_mut_shift_byte24);
}

/* ---- tests -------------------------------------------------------------- */

/**
 * @test test_exfat_unlink_not_found
 * @brief `ra_fs_unlink` returns `k_ra_err_not_found` on an empty exFAT volume.
 *
 * @details
 * After formatting with no files, the root directory holds only the three
 * system entries (bitmap, up-case, label) followed by EOD.  Scanning to EOD
 * without finding a matching File entry exercises line 205-206 in
 * `priv_exfat_find_set`: the `if (e[0] == k_exfat_entry_eod)` branch.
 *
 * Lines targeted: 206 (return k_ra_err_not_found on EOD in find_set).
 *
 * @par MC/DC:
 * Decision: `if (e[0] == k_exfat_entry_eod)` -- 1 condition.
 * V1: entry is EOD (0x00) -> T -> not found returned (this test).
 * V2: entry is not EOD (existing tests for the success path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra_fs_unlink returns k_ra_err_not_found.
 *
 * @since 0.1.0
 */
static void test_exfat_unlink_not_found(void)
{
  TEST_BEGIN("exfat mutate cov: unlink nonexistent file -> not_found (line 206)");
  build_exfat_volume();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra_err_not_found, ra_fs_unlink(h, "NOFILE.TXT"));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_volume();
  TEST_END("exfat mutate cov: unlink nonexistent file -> not_found (line 206)");
}

/**
 * @test test_exfat_rename_old_too_long
 * @brief `ra_fs_rename` rejects a 16-char old name immediately.
 *
 * @details
 * The exFAT spec limits a single Name entry to 15 UTF-16 units.  When the
 * caller passes a 16-char old path `priv_exfat_rename` returns
 * `k_ra_err_not_supported` at line 420 before touching the directory.
 *
 * Lines targeted: 420.
 *
 * @par MC/DC:
 * Decision: `if (old_len > k_exfat_name_per_entry)` -- 1 condition.
 * V1: old_len == 16 > 15 -> T -> not_supported (this test).
 * V2: old_len == 5 <= 15 -> F -> proceeds (covered by success-path tests).
 *
 * @pre Volume is formatted and accessible.
 * @post ra_fs_rename returns k_ra_err_not_supported.
 *
 * @since 0.1.0
 */
static void test_exfat_rename_old_too_long(void)
{
  TEST_BEGIN("exfat mutate cov: rename old name 16 chars -> not_supported (line 420)");
  build_exfat_volume();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  /* 16 chars: one over the 15-char per-entry limit. */
  TEST_ASSERT_EQ(k_ra_err_not_supported, ra_fs_rename(h, "ABCDEFGHIJKLMNOP", "NEWNAME.TXT"));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_volume();
  TEST_END("exfat mutate cov: rename old name 16 chars -> not_supported (line 420)");
}

/**
 * @test test_exfat_rename_new_too_long
 * @brief `ra_fs_rename` rejects a 16-char new name immediately.
 *
 * @details
 * When the old name is valid but the new name is 16 chars,
 * `priv_exfat_rename` returns `k_ra_err_not_supported` at line 423.
 *
 * Lines targeted: 423.
 *
 * @par MC/DC:
 * Decision: `if (new_len > k_exfat_name_per_entry)` -- 1 condition.
 * V1: new_len == 16 > 15 -> T -> not_supported (this test).
 * V2: new_len == 5 <= 15 -> F -> proceeds (success-path tests).
 *
 * @pre Volume is formatted and accessible.
 * @post ra_fs_rename returns k_ra_err_not_supported.
 *
 * @since 0.1.0
 */
static void test_exfat_rename_new_too_long(void)
{
  TEST_BEGIN("exfat mutate cov: rename new name 16 chars -> not_supported (line 423)");
  build_exfat_volume();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  /* Old name is 5 chars (fits), new name is 16 chars (too long). */
  TEST_ASSERT_EQ(k_ra_err_not_supported, ra_fs_rename(h, "A.TXT", "ABCDEFGHIJKLMNOP"));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_volume();
  TEST_END("exfat mutate cov: rename new name 16 chars -> not_supported (line 423)");
}

/**
 * @test test_exfat_rename_new_exists
 * @brief `ra_fs_rename` returns `k_ra_err_exists` when the target exists.
 *
 * @details
 * Creates both "A.TXT" and "B.TXT", then renames A to B.  The existence
 * probe in `priv_exfat_rename` (via `priv_exfat_find`) succeeds and the
 * function returns `k_ra_err_exists` at line 429.
 *
 * Lines targeted: 429.
 *
 * @par MC/DC:
 * Decision: `if (priv_exfat_find(...) == k_ra_ok)` -- 1 condition.
 * V1: new name found -> T -> exists returned (this test).
 * V2: new name not found -> F -> proceeds (rename success-path tests).
 *
 * @pre Volume is formatted and accessible.
 * @post ra_fs_rename returns k_ra_err_exists.
 *
 * @since 0.1.0
 */
static void test_exfat_rename_new_exists(void)
{
  TEST_BEGIN("exfat mutate cov: rename when target exists -> exists (line 429)");
  build_exfat_volume();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));

  const uint8_t dummy = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_write_file(h, "A.TXT", &dummy, 1U));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_write_file(h, "B.TXT", &dummy, 1U));
  TEST_ASSERT_EQ(k_ra_err_exists, ra_fs_rename(h, "A.TXT", "B.TXT"));

  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_volume();
  TEST_END("exfat mutate cov: rename when target exists -> exists (line 429)");
}

/**
 * @test test_exfat_rename_not_found
 * @brief `ra_fs_rename` returns `k_ra_err_not_found` for a missing source.
 *
 * @details
 * Renames a file that does not exist.  `priv_exfat_find_set` scans to EOD
 * and returns `k_ra_err_not_found`, which `priv_exfat_rename` propagates at
 * line 442.
 *
 * Lines targeted: 442 (propagate find_set error in rename).
 *
 * @pre Volume is formatted and accessible.
 * @post ra_fs_rename returns k_ra_err_not_found.
 *
 * @since 0.1.0
 */
static void test_exfat_rename_not_found(void)
{
  TEST_BEGIN("exfat mutate cov: rename nonexistent file -> not_found (line 442)");
  build_exfat_volume();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra_err_not_found, ra_fs_rename(h, "GHOST.TXT", "OTHER.TXT"));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_volume();
  TEST_END("exfat mutate cov: rename nonexistent file -> not_found (line 442)");
}

/**
 * @test test_exfat_stream_type_mismatch
 * @brief `priv_exfat_take_set` clears `matched` when the first secondary is
 *        not a Stream-extension entry (lines 131-133).
 *
 * @details
 * Writes "A.TXT", then corrupts the on-disk Stream entry (root index 4) by
 * overwriting its type byte with `k_mut_type_bogus` (0x42, which is not
 * `k_exfat_entry_stream` 0xC0).  Calling `ra_fs_unlink("A.TXT")` causes
 * `priv_exfat_take_set` to enter the `k==0` branch, detect the wrong type,
 * set `matched=0`, and continue.  The outer loop then sees EOD and returns
 * `k_ra_err_not_found`.
 *
 * Lines targeted: 131-133 (stream type mismatch sets matched=0 + continue).
 *
 * @par MC/DC:
 * Decision: `if (se[0] != k_exfat_entry_stream)` -- 1 condition.
 * V1: type is 0x42 != 0xC0 -> T -> matched cleared (this test).
 * V2: type is 0xC0 -> F -> stream processing proceeds (normal unlink path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra_fs_unlink returns k_ra_err_not_found.
 *
 * @since 0.1.0
 */
static void test_exfat_stream_type_mismatch(void)
{
  TEST_BEGIN("exfat mutate cov: stream type mismatch -> matched=0 (lines 131-133)");
  build_exfat_volume();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));

  const uint8_t dummy = (uint8_t)'A';
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_write_file(h, "A.TXT", &dummy, 1U));

  /* Corrupt the Stream entry type byte (root index 4, byte 0). */
  const uint32_t strm_off = root_byte(h, (uint32_t)k_mut_root_strm0_idx);
  s_disk.bytes[strm_off]  = (uint8_t)k_mut_type_bogus;

  TEST_ASSERT_EQ(k_ra_err_not_found, ra_fs_unlink(h, "A.TXT"));

  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_volume();
  TEST_END("exfat mutate cov: stream type mismatch -> matched=0 (lines 131-133)");
}

/**
 * @test test_exfat_name_entry_type_mismatch
 * @brief `priv_exfat_take_set` clears `matched` when a secondary is not a
 *        Name entry (lines 145-147).
 *
 * @details
 * Writes "A.TXT", then overwrites the Name entry's type byte (root index 5,
 * byte 0) with 0x00.  When `priv_exfat_take_set` reads this entry for
 * `k == 1`, the type is 0x00 != 0xC1, so `matched` is cleared and the loop
 * continues.  The outer scan hits EOD and returns `k_ra_err_not_found`.
 *
 * Lines targeted: 145-147 (name type mismatch).
 *
 * @par MC/DC:
 * Decision: `if (se[0] != k_exfat_entry_name)` -- 1 condition.
 * V1: type is 0x00 != 0xC1 -> T -> matched cleared (this test).
 * V2: type is 0xC1 -> F -> name processing proceeds (normal path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra_fs_unlink returns k_ra_err_not_found.
 *
 * @since 0.1.0
 */
static void test_exfat_name_entry_type_mismatch(void)
{
  TEST_BEGIN("exfat mutate cov: name entry type mismatch -> matched=0 (lines 145-147)");
  build_exfat_volume();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));

  const uint8_t dummy = (uint8_t)'A';
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_write_file(h, "A.TXT", &dummy, 1U));

  /* Corrupt the Name entry type byte (root index 5, byte 0). */
  const uint32_t name_off = root_byte(h, (uint32_t)k_mut_root_name0_idx);
  s_disk.bytes[name_off]  = 0x00U;

  TEST_ASSERT_EQ(k_ra_err_not_found, ra_fs_unlink(h, "A.TXT"));

  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_volume();
  TEST_END("exfat mutate cov: name entry type mismatch -> matched=0 (lines 145-147)");
}

/**
 * @test test_exfat_name_chunk_mismatch
 * @brief `priv_exfat_take_set` clears `matched` when the name chunk does not
 *        match the target (lines 150-152).
 *
 * @details
 * Writes "A.TXT" then "B.TXT" (same length -- 5 chars each).  When the
 * outer scan in `priv_exfat_find_set` finds the File entry for "A.TXT" and
 * calls `priv_exfat_take_set` looking for "B.TXT", the Name chunk comparison
 * (`priv_exfat_name_chunk_eq`) returns 0 because the stored name is "A.TXT",
 * not "B.TXT".  This clears `matched` at lines 150-152.
 * The scan then continues to the "B.TXT" set and succeeds, causing unlink to
 * return `k_ra_ok`.
 *
 * Lines targeted: 150-152 (name chunk mismatch clears matched).
 *
 * @par MC/DC:
 * Decision: `if (priv_exfat_name_chunk_eq(...) == 0U)` -- 1 condition.
 * V1: names differ -> T -> matched cleared (this test).
 * V2: names match  -> F -> matched unchanged at 1 (normal success path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra_fs_unlink("B.TXT") returns k_ra_ok.
 *
 * @since 0.1.0
 */
static void test_exfat_name_chunk_mismatch(void)
{
  TEST_BEGIN("exfat mutate cov: name chunk mismatch (lines 150-152)");
  build_exfat_volume();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));

  /* Both names are 5 chars so the NameLength check passes on both; the name
   * bytes differ, triggering the mismatch on the A.TXT set. */
  const uint8_t dummy = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_write_file(h, "A.TXT", &dummy, 1U));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_write_file(h, "B.TXT", &dummy, 1U));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unlink(h, "B.TXT"));

  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_volume();
  TEST_END("exfat mutate cov: name chunk mismatch (lines 150-152)");
}

/**
 * @test test_exfat_too_many_secondary
 * @brief `priv_exfat_find_set` returns `k_ra_err_no_mem` when `total > max_pos`
 *        (line 213-214).
 *
 * @details
 * Writes "A.TXT" (SecondaryCount = 2), then patches the File entry's
 * SecondaryCount byte (byte 1 of the File entry at root index 3) to 19.
 * This makes `total = 1 + 19 = 20 > k_exfat_set_max_entries = 19`, so
 * `priv_exfat_find_set` returns `k_ra_err_no_mem` at line 214.
 * `priv_exfat_unlink` propagates that error at line 332.
 *
 * Lines targeted: 214 (no_mem in find_set), 332 (propagate in unlink).
 *
 * @pre Volume is formatted and accessible.
 * @post ra_fs_unlink returns k_ra_err_no_mem.
 *
 * @since 0.1.0
 */
static void test_exfat_too_many_secondary(void)
{
  TEST_BEGIN("exfat mutate cov: sec-count 19 -> no_mem (lines 214,332)");
  build_exfat_volume();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));

  const uint8_t dummy = (uint8_t)'A';
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_write_file(h, "A.TXT", &dummy, 1U));

  /* Patch SecondaryCount to 19: total = 20 > k_exfat_set_max_entries. */
  const uint32_t file_off = root_byte(h, (uint32_t)k_mut_root_file0_idx);
  s_disk.bytes[file_off + (uint32_t)k_mut_file_secnt_off] = (uint8_t)k_mut_max_secondary;

  TEST_ASSERT_EQ(k_ra_err_no_mem, ra_fs_unlink(h, "A.TXT"));

  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_volume();
  TEST_END("exfat mutate cov: sec-count 19 -> no_mem (lines 214,332)");
}

/**
 * @test test_exfat_free_clusters_no_first
 * @brief `priv_exfat_free_clusters` returns early when `first < 2` (line 283-284).
 *
 * @details
 * Writes "A.TXT", then patches the Stream entry's FirstCluster field
 * (bytes 20-23 of the stream entry) to 0.  A cluster number below
 * `k_cluster_first_data` (2) is invalid; `priv_exfat_free_clusters`
 * returns `k_ra_ok` immediately at line 284 without touching the bitmap.
 *
 * Lines targeted: 283-284 (first < k_cluster_first_data guard).
 *
 * @par MC/DC:
 * Decision: `if (first < k_cluster_first_data)` -- 1 condition.
 * V1: first == 0 < 2 -> T -> early return (this test).
 * V2: first == 5 >= 2 -> F -> proceeds to free (normal unlink path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra_fs_unlink returns k_ra_ok (file entry deleted; bitmap unchanged).
 *
 * @since 0.1.0
 */
static void test_exfat_free_clusters_no_first(void)
{
  TEST_BEGIN("exfat mutate cov: first_cluster=0 -> early return (lines 283-284)");
  build_exfat_volume();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));

  const uint8_t dummy = (uint8_t)'A';
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_write_file(h, "A.TXT", &dummy, 1U));

  /* Zero the FirstCluster field in the Stream entry. */
  const uint32_t strm_off = root_byte(h, (uint32_t)k_mut_root_strm0_idx);
  disk_set_u32le(strm_off + (uint32_t)k_mut_strm_off_clus, 0U);

  /* Unlink must not crash and must return ok. */
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unlink(h, "A.TXT"));

  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_volume();
  TEST_END("exfat mutate cov: first_cluster=0 -> early return (lines 283-284)");
}

/**
 * @test test_exfat_free_clusters_zero_size
 * @brief `priv_exfat_free_clusters` returns early when `size == 0` (line 286-287).
 *
 * @details
 * Writes "A.TXT", then patches the Stream entry's DataLength field
 * (bytes 24-27) to 0.  A zero data length means no clusters were used;
 * `priv_exfat_free_clusters` returns `k_ra_ok` immediately at line 287
 * without finding or clearing the bitmap.
 *
 * Lines targeted: 286-287 (size == 0 guard).
 *
 * @par MC/DC:
 * Decision: `if (size == 0U)` -- 1 condition.
 * V1: size == 0 -> T -> early return (this test).
 * V2: size != 0 -> F -> proceeds (normal unlink path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra_fs_unlink returns k_ra_ok.
 *
 * @since 0.1.0
 */
static void test_exfat_free_clusters_zero_size(void)
{
  TEST_BEGIN("exfat mutate cov: size=0 -> early return (lines 286-287)");
  build_exfat_volume();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));

  const uint8_t dummy = (uint8_t)'A';
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_write_file(h, "A.TXT", &dummy, 1U));

  /* Zero the DataLength field in the Stream entry. */
  const uint32_t strm_off = root_byte(h, (uint32_t)k_mut_root_strm0_idx);
  disk_set_u32le(strm_off + (uint32_t)k_mut_strm_off_dlen, 0U);

  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unlink(h, "A.TXT"));

  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_volume();
  TEST_END("exfat mutate cov: size=0 -> early return (lines 286-287)");
}

/**
 * @test test_exfat_fat_chain_free
 * @brief The FAT-chain walk in `priv_exfat_free_clusters` frees a two-cluster
 *        chain (lines 303-306, 309-311, 314-315, 317-318).
 *
 * @details
 * Writes a 5000-byte file that spans two 4 KiB clusters (clusters 5 and 6
 * on a fresh 64 MiB volume).  By default the formatter uses the NoFatChain
 * mode (GeneralSecondaryFlags bit 1 set).  This test:
 *   1. Clears the NoFatChain bit in the Stream entry to force FAT-chain mode.
 *   2. Writes FAT[first_cluster] = first_cluster + 1 (link to second cluster).
 *   3. Writes FAT[first_cluster + 1] = EOC (0xFFFFFFFF).
 *   4. Calls ra_fs_unlink, which enters the FAT-chain walk loop.
 *
 * The walk iterates twice:
 *   - Iteration 1: bitmap_clear(cluster5), fat_get->cluster6, is_eoc=false,
 *     clus=cluster6 (lines 317-318).
 *   - Iteration 2: bitmap_clear(cluster6), fat_get->EOC, is_eoc=true,
 *     return ok (line 315).
 *
 * Lines targeted:
 *   303 (clus=first), 304 (for loop header), 305 (bitmap_clear call),
 *   306 (if e!=ok), 309 (next=0), 310 (fat_get call), 311 (if e!=ok),
 *   314 (is_eoc check), 315 (return ok on EOC), 317 (clus=next),
 *   318 (loop body close).
 *
 * @par MC/DC:
 * Decision: `if (priv_is_eoc(m, next) != 0U)` -- 1 condition.
 * V1: next is EOC -> T -> return ok (iteration 2).
 * V2: next is a valid cluster -> F -> clus=next, continue (iteration 1).
 *
 * @pre Volume is formatted and accessible.
 * @post ra_fs_unlink returns k_ra_ok; bitmap bits for both clusters cleared.
 *
 * @since 0.1.0
 */
static void test_exfat_fat_chain_free(void)
{
  TEST_BEGIN("exfat mutate cov: FAT-chain 2-cluster free (lines 303-318)");
  build_exfat_volume();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));

  /* Write a 5000-byte file -> two 4 KiB clusters allocated. */
  static uint8_t s_payload[k_mut_payload_two_cls];
  for (uint32_t i = 0U; i < (uint32_t)k_mut_payload_two_cls; i++) {
    s_payload[i] = (uint8_t)(i & (uint32_t)k_mut_mask_byte);
  }
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_write_file(h, "A.TXT", s_payload, (uint32_t)k_mut_payload_two_cls));

  /* Read first_cluster from the Stream entry FirstCluster field. */
  const uint32_t strm_off    = root_byte(h, (uint32_t)k_mut_root_strm0_idx);
  const uint32_t first_clus  = disk_get_u32le(strm_off + (uint32_t)k_mut_strm_off_clus);
  const uint32_t second_clus = first_clus + 1U;

  /* Clear the NoFatChain bit so free_clusters uses the FAT walk. */
  s_disk.bytes[strm_off + (uint32_t)k_mut_strm_off_flags] &= (uint8_t)~(uint8_t)k_mut_no_fat_bit;

  /* Build a two-entry FAT chain: first -> second -> EOC. */
  disk_set_u32le(fat_byte(h, first_clus), second_clus);
  disk_set_u32le(fat_byte(h, second_clus), (uint32_t)k_mut_fat_eoc);

  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unlink(h, "A.TXT"));

  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_volume();
  TEST_END("exfat mutate cov: FAT-chain 2-cluster free (lines 303-318)");
}

/**
 * @test test_exfat_rename_count_not_three
 * @brief `priv_exfat_rename` returns `k_ra_err_not_supported` when the found
 *        set does not have exactly 3 entries (line 444-445).
 *
 * @details
 * Writes "A.TXT" (SecondaryCount = 2, set size = 3) then patches the
 * SecondaryCount byte to 1 (set size = 2).  After `priv_exfat_find_set`
 * reports count = 2, the check `count != 3` fires and rename returns
 * `k_ra_err_not_supported` at line 445.
 *
 * Lines targeted: 444-445.
 *
 * @pre Volume is formatted and accessible.
 * @post ra_fs_rename returns k_ra_err_not_supported.
 *
 * @since 0.1.0
 */
static void test_exfat_rename_count_not_three(void)
{
  TEST_BEGIN("exfat mutate cov: entry count != 3 -> not_supported (lines 444-445)");
  build_exfat_volume();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));

  const uint8_t dummy = (uint8_t)'A';
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_write_file(h, "A.TXT", &dummy, 1U));

  /* Patch SecondaryCount to 1 -> set size becomes 2 (File + Stream only). */
  const uint32_t file_off = root_byte(h, (uint32_t)k_mut_root_file0_idx);
  s_disk.bytes[file_off + (uint32_t)k_mut_file_secnt_off] = 1U;

  TEST_ASSERT_EQ(k_ra_err_not_supported, ra_fs_rename(h, "A.TXT", "C.TXT"));

  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_volume();
  TEST_END("exfat mutate cov: entry count != 3 -> not_supported (lines 444-445)");
}

/**
 * @test test_exfat_listdir_skip_non_stream
 * @brief `priv_exfat_listdir` skips a file set whose first secondary is not a
 *        Stream-extension entry (line 529-530).
 *
 * @details
 * Writes "A.TXT", then patches the Stream entry type byte from 0xC0 to
 * `k_mut_type_bogus` (0x42).  When `priv_exfat_listdir` reads this second
 * entry after the File entry it finds type != 0xC0 and executes `continue`
 * at line 530, never invoking the callback.  Listdir then reads the Name
 * entry (not a File entry, skipped) and EOD, and returns `k_ra_ok`.
 * The callback count remains 0.
 *
 * Lines targeted: 529-530.
 *
 * @par MC/DC:
 * Decision: `if (strm[0] != k_exfat_entry_stream)` -- 1 condition.
 * V1: type is 0x42 != 0xC0 -> T -> continue (this test).
 * V2: type is 0xC0 -> F -> proceeds to gather_name (normal listdir path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra_fs_listdir returns k_ra_ok with count == 0.
 *
 * @since 0.1.0
 */
static void test_exfat_listdir_skip_non_stream(void)
{
  TEST_BEGIN("exfat mutate cov: bad stream type -> listdir skip (lines 529-530)");
  build_exfat_volume();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));

  const uint8_t dummy = (uint8_t)'A';
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_write_file(h, "A.TXT", &dummy, 1U));

  /* Corrupt the Stream entry type byte. */
  const uint32_t strm_off = root_byte(h, (uint32_t)k_mut_root_strm0_idx);
  s_disk.bytes[strm_off]  = (uint8_t)k_mut_type_bogus;

  mut_list_ctx_t ctx = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_listdir(h, "/", count_cb, &ctx));
  TEST_ASSERT_EQ(0U, ctx.count);

  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_volume();
  TEST_END("exfat mutate cov: bad stream type -> listdir skip (lines 529-530)");
}

/**
 * @test test_exfat_gather_name_skip_non_name
 * @brief `priv_exfat_gather_name` skips secondaries whose type is not
 *        `k_exfat_entry_name` (line 488-489).
 *
 * @details
 * Writes "A.TXT", then patches the Name entry type byte (root index 5,
 * byte 0) from 0xC1 to `k_mut_type_bogus` (0x42).  The Stream entry is
 * left intact (type 0xC0) so `priv_exfat_listdir` reaches `gather_name`.
 * Inside `gather_name` the patched entry fails the `ne[0] == 0xC1` check
 * and `continue` fires at line 489.  No name characters are copied, the
 * callback is still invoked with an empty name, and listdir returns
 * `k_ra_ok` with count == 1.
 *
 * Lines targeted: 488-489 (non-name secondary skipped in gather_name).
 *
 * @par MC/DC:
 * Decision: `if (ne[0] != k_exfat_entry_name)` -- 1 condition.
 * V1: type != 0xC1 -> T -> continue (this test).
 * V2: type == 0xC1 -> F -> copy name chars (normal gather_name path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra_fs_listdir returns k_ra_ok with count == 1.
 *
 * @since 0.1.0
 */
static void test_exfat_gather_name_skip_non_name(void)
{
  TEST_BEGIN("exfat mutate cov: non-name secondary skipped in gather_name (lines 488-489)");
  build_exfat_volume();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));

  const uint8_t dummy = (uint8_t)'A';
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_write_file(h, "A.TXT", &dummy, 1U));

  /* Patch the Name entry type byte; leave Stream entry intact. */
  const uint32_t name_off = root_byte(h, (uint32_t)k_mut_root_name0_idx);
  s_disk.bytes[name_off]  = (uint8_t)k_mut_type_bogus;

  /* listdir must still fire the callback once (empty name), not error. */
  mut_list_ctx_t ctx = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_listdir(h, "/", count_cb, &ctx));
  TEST_ASSERT_EQ(1U, ctx.count);

  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_volume();
  TEST_END("exfat mutate cov: non-name secondary skipped in gather_name (lines 488-489)");
}

/* ---- entry point -------------------------------------------------------- */

int32_t main(void)
{
  test_exfat_unlink_not_found();
  test_exfat_rename_old_too_long();
  test_exfat_rename_new_too_long();
  test_exfat_rename_new_exists();
  test_exfat_rename_not_found();
  test_exfat_stream_type_mismatch();
  test_exfat_name_entry_type_mismatch();
  test_exfat_name_chunk_mismatch();
  test_exfat_free_clusters_no_first();
  test_exfat_free_clusters_zero_size();
  test_exfat_fat_chain_free();
  test_exfat_too_many_secondary();
  test_exfat_rename_count_not_three();
  test_exfat_listdir_skip_non_stream();
  test_exfat_gather_name_skip_non_name();
  (void)fprintf(stderr, "[OK  ] test_ra_fs_fat_exfat_mutate_cov.c\n");
  return 0;
}
