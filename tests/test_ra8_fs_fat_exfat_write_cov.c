/**
 * @file tests/test_ra8_fs_fat_exfat_write_cov.c
 * @brief Coverage-boost tests for `ra8_fs_fat_exfat_write.c`.
 *
 * @details
 * Targets the 32 uncovered lines in the exFAT one-shot write path by
 * using a RAM-backed 64 MiB exFAT volume combined with two techniques:
 *
 *   1. A countdown I/O backend that succeeds for the first N reads (or
 *      writes) and then returns an error, injecting failures at exact
 *      positions in the call sequence.
 *
 *   2. Raw disk patching -- overwriting bytes in the sector store after
 *      a normal format/mount to create volume states that the public API
 *      alone cannot produce (a full root cluster, a FAT chain extension,
 *      a zeroed bitmap).
 *
 * The countdown read sequence for a 1-byte file on a fresh 64 MiB exFAT
 * volume (SPC=8, 4 KiB clusters, 128 entries per cluster) is:
 *
 *   R1  priv_exfat_find_bitmap    reads root cluster sector (entry 0 = 0x81)
 *   R2  priv_exfat_bitmap_scan    reads bitmap cluster sector
 *   W1  priv_exfat_write_data     writes file data sector
 *   R3  priv_exfat_bmp_switch     reads bitmap sector (loaded=UINT32_MAX path)
 *   W2  priv_exfat_bitmap_mark    writes updated bitmap sector
 *   R4-R9   priv_exfat_find_dir_space  reads entries 0-5 of root cluster
 *   R10 priv_exfat_write_dir_set  reads root sector for first dir entry
 *   W3  priv_exfat_write_dir_set  writes root sector (entry 0)
 *   ...
 *
 * For the full-root-cluster tests (entries 3-127 all patched to 0x85):
 *   R4-R131  find_dir_space reads entries 0-127 (128 reads)
 *   R132     priv_fat_get reads the FAT sector
 *
 * @par Target lines
 * 79, 82, 124, 143, 153, 154, 155, 157, 160, 197, 240, 273, 341,
 * 356, 357, 358, 359, 361, 362, 364, 365, 441, 448, 485, 490, 494,
 * 531, 548, 551, 554, 561.
 * (Line 90 carries GCOVR_EXCL_LINE in the source: reaching it requires
 * 65536 non-EOD, non-bitmap root directory entries, which cannot be
 * synthesised by any host-side input.)
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

#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_internal.h"
#include "unity_minimal.h"

/* ---- constants ------------------------------------------------------------- */

/**
 * @enum wc_const_t
 * @brief Sizing and offset constants for the exFAT write coverage tests.
 *
 * @details All numeric literals in this file appear here as named enum values.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_wc_block_size    = 512U,        /**< Bytes per disk sector.                 */
  k_wc_blocks_exfat  = 131072U,     /**< 64 MiB exFAT volume in sectors.        */
  k_wc_entry_bytes   = 32U,         /**< Directory entry size in bytes.         */
  k_wc_per_cluster   = 128U,        /**< Directory entries per 4 KiB cluster.   */
  k_wc_entry_inuse   = 0x85U,       /**< exFAT File entry type (in-use, bit 7). */
  k_wc_entry_eod     = 0x00U,       /**< exFAT end-of-directory type.           */
  k_wc_fat_eoc       = 0xFFFFFFFFU, /**< exFAT end-of-chain FAT value.          */
  k_wc_patch_start   = 3U,          /**< First root entry to mark in-use.       */
  k_wc_chain_offset  = 100U,        /**< Cluster offset for FAT-chain target.   */
  k_wc_shift_byte8   = 8U,          /**< 8-bit shift for LE16/32 pack.          */
  k_wc_shift_byte16  = 16U,         /**< 16-bit shift for LE32 pack.            */
  k_wc_shift_byte24  = 24U,         /**< 24-bit shift for LE32 pack.            */
  k_wc_mask_byte     = 0xFFU,       /**< Low-byte mask.                         */
  k_wc_name_too_long = 65U,         /**< One over k_exfat_name_cap (64).        */
} wc_const_t;

/**
 * @enum wc_rd_fail_t
 * @brief Read-countdown seeds for fault injection.
 *
 * @details Negative means never fail; 0 means fail the very next read;
 *          N means fail after N successful reads.
 *
 * @since 0.1.0
 */
typedef enum : int32_t {
  k_wc_rd_never   = -1,  /**< Never inject a read error.                       */
  k_wc_rd_at_r1   = 0,   /**< Fail R1: find_bitmap read (line 79).             */
  k_wc_rd_at_r2   = 1,   /**< Fail R2: bitmap_scan read (line 124).            */
  k_wc_rd_at_r3   = 2,   /**< Fail R3: bmp_switch read (lines 160, 197).       */
  k_wc_rd_at_r4   = 3,   /**< Fail R4: first read_entry read (273, 341, 531).  */
  k_wc_rd_at_r10  = 9,   /**< Fail R10: write_dir_set read (line 441).         */
  k_wc_rd_fat_get = 131, /**< Fail R132: fat_get in find_dir_space (line 359). */
} wc_rd_fail_t;

/**
 * @enum wc_wr_fail_t
 * @brief Write-countdown seeds for fault injection.
 *
 * @since 0.1.0
 */
typedef enum : int32_t {
  k_wc_wr_never = -1, /**< Never inject a write error.                 */
  k_wc_wr_at_w1 = 0,  /**< Fail W1: write_data write (lines 240, 494). */
  k_wc_wr_at_w3 = 2,  /**< Fail W3: write_dir_set write (line 448).    */
} wc_wr_fail_t;

/* ---- RAM-backed block device with countdown fault injection -------------- */

/**
 * @struct wc_disk_t
 * @brief Memory-backed disk presented to `ra8_fs` as a block device.
 *
 * @details Sector store is a flat `malloc` buffer; all I/O goes straight to
 *          this buffer so tests can inspect or corrupt on-disk state by
 *          patching individual bytes.
 *
 * @invariant bytes is non-NULL when the volume is alive.
 * @since 0.1.0
 */
typedef struct {
  uint8_t* bytes;       /**< Flat sector store.          */
  uint32_t block_count; /**< Number of 512-byte sectors. */
} wc_disk_t;

/**
 * @var s_disk
 * @brief Module-level RAM disk shared across all tests.
 * @warning Modify only through `build_exfat_volume()` / `free_volume()`.
 * @since 0.1.0
 */
static wc_disk_t s_disk = {};

/**
 * @var s_rd_remaining
 * @brief Read countdown: negative=never fail, 0=fail next, N=fail after N.
 * @warning Set only through test setup helpers.
 * @since 0.1.0
 */
static int32_t s_rd_remaining = (int32_t)k_wc_rd_never;

/**
 * @var s_wr_remaining
 * @brief Write countdown: negative=never fail, 0=fail next, N=fail after N.
 * @warning Set only through test setup helpers.
 * @since 0.1.0
 */
static int32_t s_wr_remaining = (int32_t)k_wc_wr_never;

/**
 * @brief Countdown-faulting `ra8_fs` read backend.
 *
 * @details Decrements `s_rd_remaining` on each call. When the counter
 *          reaches zero it returns `k_ra8_err_out_of_range` without touching
 *          the disk, simulating an I/O failure at a precise position in the
 *          call sequence.
 *
 * @param[in]  ctx   Pointer to `wc_disk_t`.
 * @param[in]  lba   Start sector.
 * @param[in]  count Sector count.
 * @param[out] buf   Destination buffer.
 *
 * @return k_ra8_ok or k_ra8_err_out_of_range.
 * @retval k_ra8_ok             Sectors read successfully.
 * @retval k_ra8_err_out_of_range Request out of range, or countdown expired.
 *
 * @pre ctx and buf are non-NULL.
 * @post buf holds the requested sectors when k_ra8_ok is returned.
 *
 * @since 0.1.0
 */
static ra8_err_t wc_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  if (s_rd_remaining == 0) {
    return k_ra8_err_out_of_range;
  }
  if (s_rd_remaining > 0) {
    s_rd_remaining--;
  }
  const wc_disk_t* d = (const wc_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf, &d->bytes[lba * (uint32_t)k_wc_block_size], (size_t)count * (size_t)k_wc_block_size);
  return k_ra8_ok;
}

/**
 * @brief Countdown-faulting `ra8_fs` write backend.
 *
 * @param[in] ctx   Pointer to `wc_disk_t`.
 * @param[in] lba   Start sector.
 * @param[in] count Sector count.
 * @param[in] buf   Source buffer.
 *
 * @return k_ra8_ok or k_ra8_err_out_of_range.
 * @retval k_ra8_ok             Sectors written successfully.
 * @retval k_ra8_err_out_of_range Request out of range, or countdown expired.
 *
 * @pre ctx and buf are non-NULL.
 * @post Disk bytes match @p buf on k_ra8_ok.
 *
 * @since 0.1.0
 */
static ra8_err_t wc_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  if (s_wr_remaining == 0) {
    return k_ra8_err_out_of_range;
  }
  if (s_wr_remaining > 0) {
    s_wr_remaining--;
  }
  wc_disk_t* d = (wc_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[lba * (uint32_t)k_wc_block_size], buf, (size_t)count * (size_t)k_wc_block_size);
  return k_ra8_ok;
}

/**
 * @brief Report disk geometry to `ra8_fs`.
 *
 * @param[in]  ctx         Pointer to `wc_disk_t`.
 * @param[out] block_count Receives the sector count.
 * @param[out] block_size  Receives 512.
 *
 * @return k_ra8_ok always.
 * @retval k_ra8_ok Geometry populated.
 *
 * @pre ctx, block_count, block_size are non-NULL.
 * @post *block_count and *block_size reflect disk dimensions.
 *
 * @since 0.1.0
 */
static ra8_err_t wc_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  const wc_disk_t* d = (const wc_disk_t*)ctx;
  *block_count       = d->block_count;
  *block_size        = (uint32_t)k_wc_block_size;
  return k_ra8_ok;
}

/**
 * @var s_ctrl_backend
 * @brief Countdown backend wired to `s_disk`.
 * @since 0.1.0
 */
static const ra8_fs_backend_t s_ctrl_backend = {
  .read_block   = wc_read,
  .write_block  = wc_write,
  .get_capacity = wc_capacity,
  .ctx          = &s_disk,
};

/* ---- harness helpers ------------------------------------------------------- */

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
 * @brief Allocate a fresh 64 MiB RAM disk and format it as exFAT.
 *
 * @details Resets the countdown faults to -1 (never fail) before format so
 *          the format itself always succeeds. Pre-fills with 0xA5 to prove
 *          the formatter writes the VBR rather than leaving a lucky zero
 *          pattern.
 *
 * @pre None.
 * @post `s_disk` holds a valid exFAT image; countdown faults are at -1.
 *
 * @since 0.1.0
 */
static void build_exfat_volume(void)
{
  free_volume();
  s_rd_remaining     = (int32_t)k_wc_rd_never;
  s_wr_remaining     = (int32_t)k_wc_wr_never;
  const size_t total = (size_t)k_wc_blocks_exfat * (size_t)k_wc_block_size;
  s_disk.bytes       = (uint8_t*)malloc(total);
  s_disk.block_count = (uint32_t)k_wc_blocks_exfat;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "malloc failed for exFAT volume");
  }
  memset(s_disk.bytes, 0xA5, total);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_exfat;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_ctrl_backend, &opts));
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
static uint32_t root_entry_off(const ra8_fs_mount_t* h, uint32_t idx)
{
  const uint32_t root_lba = h->first_data_lba + (h->root_cluster - 2U) * h->sectors_per_cluster;
  return root_lba * (uint32_t)k_wc_block_size + idx * (uint32_t)k_wc_entry_bytes;
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
static uint32_t fat_entry_off(const ra8_fs_mount_t* h, uint32_t clus)
{
  return h->first_fat_lba * (uint32_t)k_wc_block_size + clus * 4U;
}

/**
 * @brief Write a 4-byte LE value at byte offset @p off in `s_disk.bytes`.
 *
 * @param[in] off Absolute byte offset.
 * @param[in] val 32-bit value to store in little-endian order.
 *
 * @pre off + 4 is within the disk image.
 * @post Bytes at @p off encode @p val in LE order.
 *
 * @since 0.1.0
 */
static void disk_set_u32le(uint32_t off, uint32_t val)
{
  uint8_t* p = &s_disk.bytes[off];
  p[0]       = (uint8_t)(val & (uint32_t)k_wc_mask_byte);
  p[1]       = (uint8_t)((val >> (uint32_t)k_wc_shift_byte8) & (uint32_t)k_wc_mask_byte);
  p[2]       = (uint8_t)((val >> (uint32_t)k_wc_shift_byte16) & (uint32_t)k_wc_mask_byte);
  p[3]       = (uint8_t)((val >> (uint32_t)k_wc_shift_byte24) & (uint32_t)k_wc_mask_byte);
}

/**
 * @brief Patch root-cluster entries @p start..127 to the in-use type 0x85.
 *
 * @details Forces `priv_exfat_find_dir_space` to scan the entire root cluster
 *          without finding a free run, triggering the FAT chain walk.  Entries
 *          0-2 (system entries written by the formatter) and entries 16-127
 *          (pre-fill 0xA5, bit 7 set) are already treated as in-use by
 *          `priv_exfat_slot_free`; only entries 3-15 (zeroed by the formatter)
 *          need explicit patching.  For safety this function patches every
 *          entry from @p start to 127.
 *
 * @param[in] h     Mounted exFAT volume.
 * @param[in] start First entry index to patch (typically 3).
 *
 * @pre h is non-NULL.
 * @post Entries @p start-127 have type byte 0x85.
 *
 * @since 0.1.0
 */
static void patch_root_full(const ra8_fs_mount_t* h, uint32_t start)
{
  for (uint32_t i = start; i < (uint32_t)k_wc_per_cluster; i++) {
    s_disk.bytes[root_entry_off(h, i)] = (uint8_t)k_wc_entry_inuse;
  }
}

/* ---- forward declaration of the internal function under test ------------- */

/*
 * `priv_exfat_bmp_switch` is cross-TU external linkage (non-static) in
 * `ra8_fs_fat_exfat_write.c`.  Its full declaration is in the umbrella header
 * `ra8_fs_fat_internal.h` (included above), which is on the include path for
 * tests per CLAUDE.md "Test access to internal symbols".
 */

/* ---- tests ----------------------------------------------------------------- */

/**
 * @test test_create_empty_path
 * @brief `priv_exfat_create` rejects an empty path (line 548).
 *
 * @details Calling `ra8_fs_write_file` with "/" as the path causes
 *          `priv_exfat_create` to strip the leading slash, leaving an
 *          empty name of length 0, and return `k_ra8_err_invalid_arg`.
 *
 * Lines targeted: 548.
 *
 * @par MC/DC:
 * Decision: `if (nlen == 0U)` -- 1 condition.
 * V1: path "/" -> stripped to "" -> nlen=0 -> T -> invalid_arg (this test).
 * V2: path "X.TXT" -> nlen=5 -> F -> proceeds (all success-path tests).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns k_ra8_err_invalid_arg.
 *
 * @since 0.1.0
 */
static void test_create_empty_path(void)
{
  TEST_BEGIN("exfat write cov: empty path -> invalid_arg (line 548)");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  const uint8_t dummy = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_write_file(h, "/", &dummy, 1U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat write cov: empty path -> invalid_arg (line 548)");
}

/**
 * @test test_create_path_too_long
 * @brief `priv_exfat_create` rejects a name longer than 64 characters (line 551).
 *
 * @details A 65-character name exceeds `k_exfat_name_cap` (64), so
 *          `priv_exfat_create` returns `k_ra8_err_invalid_arg` at line 551.
 *
 * Lines targeted: 551.
 *
 * @par MC/DC:
 * Decision: `if (nlen > k_exfat_name_cap)` -- 1 condition.
 * V1: nlen=65 > 64 -> T -> invalid_arg (this test).
 * V2: nlen=5 <= 64 -> F -> proceeds (success-path tests).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns k_ra8_err_invalid_arg.
 *
 * @since 0.1.0
 */
static void test_create_path_too_long(void)
{
  TEST_BEGIN("exfat write cov: 65-char name -> invalid_arg (line 551)");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  /* Exactly 65 'A' characters: one over the 64-char limit. */
  static const char k_long_name[] =
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
  const uint8_t dummy = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_write_file(h, k_long_name, &dummy, 1U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat write cov: 65-char name -> invalid_arg (line 551)");
}

/**
 * @test test_create_zero_len
 * @brief `priv_exfat_create` rejects a zero-byte file (line 554).
 *
 * @details `ra8_fs_write_file` passes `len=0` to `priv_exfat_create`, which
 *          returns `k_ra8_err_invalid_arg` at line 554.
 *
 * Lines targeted: 554.
 *
 * @par MC/DC:
 * Decision: `if (len == 0U)` -- 1 condition.
 * V1: len=0 -> T -> invalid_arg (this test).
 * V2: len=1 -> F -> proceeds (all success-path tests).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns k_ra8_err_invalid_arg.
 *
 * @since 0.1.0
 */
static void test_create_zero_len(void)
{
  TEST_BEGIN("exfat write cov: len=0 -> invalid_arg (line 554)");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  const uint8_t dummy = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_write_file(h, "X.TXT", &dummy, 0U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat write cov: len=0 -> invalid_arg (line 554)");
}

/**
 * @test test_find_bitmap_eod
 * @brief `priv_exfat_find_bitmap` returns `k_ra8_err_not_found` at the
 *        end-of-directory marker (lines 82, 485, 561).
 *
 * @details Overwrites the first root-cluster entry's type byte with 0x00
 *          (EOD) after format, hiding the bitmap entry.  `priv_exfat_find_bitmap`
 *          sees EOD at entry 0 and returns `k_ra8_err_not_found` at line 82.
 *          This propagates through `priv_exfat_alloc_write` (line 485) and
 *          `priv_exfat_create` (line 561).
 *
 * Lines targeted: 82, 485, 561.
 *
 * @par MC/DC:
 * Decision: `if (e[0] == k_exfat_entry_eod)` in find_bitmap -- 1 condition.
 * V1: e[0]=0x00 (EOD) -> T -> not_found returned (this test).
 * V2: e[0]=0x81 (bitmap) -> F -> bitmap found (normal write_file path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns k_ra8_err_not_found.
 *
 * @since 0.1.0
 */
static void test_find_bitmap_eod(void)
{
  TEST_BEGIN("exfat write cov: EOD hides bitmap -> not_found (lines 82,485,561)");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  /* Overwrite the bitmap entry's type byte with EOD. */
  s_disk.bytes[root_entry_off(h, 0U)] = (uint8_t)k_wc_entry_eod;

  const uint8_t dummy = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_write_file(h, "X.TXT", &dummy, 1U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat write cov: EOD hides bitmap -> not_found (lines 82,485,561)");
}

/**
 * @test test_find_bitmap_read_fail
 * @brief Read failure on R1 propagates through `priv_exfat_find_bitmap`
 *        (lines 79, 485, 561).
 *
 * @details With `s_rd_remaining=0` the very first `priv_read_sector` call --
 *          inside `priv_exfat_next_entry` called by `priv_exfat_find_bitmap` --
 *          fails.  The error propagates at line 79 (find_bitmap), line 485
 *          (alloc_write), and line 561 (create).
 *
 * Lines targeted: 79, 485, 561.
 *
 * @par MC/DC:
 * Decision: `if (r != k_ra8_ok)` in find_bitmap -- 1 condition.
 * V1: r=k_ra8_err_out_of_range -> T -> error returned (this test).
 * V2: r=k_ra8_ok -> F -> entry examined (normal path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns non-ok.
 *
 * @since 0.1.0
 */
static void test_find_bitmap_read_fail(void)
{
  TEST_BEGIN("exfat write cov: R1 read fail -> error at line 79");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  s_rd_remaining = (int32_t)k_wc_rd_at_r1;

  const uint8_t   dummy = (uint8_t)'X';
  const ra8_err_t r     = ra8_fs_write_file(h, "X.TXT", &dummy, 1U);
  TEST_ASSERT(r != k_ra8_ok);

  s_rd_remaining = (int32_t)k_wc_rd_never;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat write cov: R1 read fail -> error at line 79");
}

/**
 * @test test_bitmap_scan_read_fail
 * @brief Read failure on R2 propagates through `priv_exfat_bitmap_scan`
 *        (lines 124, 490, 561).
 *
 * @details After find_bitmap reads the root cluster sector (R1, succeeds),
 *          the second read is inside `priv_exfat_bitmap_scan`, which reads the
 *          bitmap sector.  Failing it exercises line 124 in bitmap_scan, line
 *          490 in alloc_write, and line 561 in create.
 *
 * Lines targeted: 124, 490, 561.
 *
 * @par MC/DC:
 * Decision: `if (e != k_ra8_ok)` in bitmap_scan -- 1 condition.
 * V1: e=error -> T -> early return (this test).
 * V2: e=k_ra8_ok -> F -> process bitmap byte (normal scan path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns non-ok.
 *
 * @since 0.1.0
 */
static void test_bitmap_scan_read_fail(void)
{
  TEST_BEGIN("exfat write cov: R2 read fail -> error at line 124");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  s_rd_remaining = (int32_t)k_wc_rd_at_r2;

  const uint8_t   dummy = (uint8_t)'X';
  const ra8_err_t r     = ra8_fs_write_file(h, "X.TXT", &dummy, 1U);
  TEST_ASSERT(r != k_ra8_ok);

  s_rd_remaining = (int32_t)k_wc_rd_never;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat write cov: R2 read fail -> error at line 124");
}

/**
 * @test test_bitmap_scan_full_volume
 * @brief `priv_exfat_bitmap_scan` returns `k_ra8_err_no_mem` when every cluster
 *        bit is set (lines 143, 490, 561).
 *
 * @details Fills the entire bitmap cluster (8 sectors) with 0xFF after format,
 *          marking every cluster as allocated.  `priv_exfat_bitmap_scan`
 *          iterates all `count_of_clusters` entries without finding a free bit
 *          and returns `k_ra8_err_no_mem` at line 143.
 *
 * Lines targeted: 143, 490, 561.
 *
 * @par MC/DC:
 * Decision: loop exhausted without match in bitmap_scan -- 1 outcome.
 * V1: all bits 1 -> loop exhausted -> no_mem (this test).
 * V2: free bit found -> k_ra8_ok (normal write_file path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns k_ra8_err_no_mem.
 *
 * @since 0.1.0
 */
static void test_bitmap_scan_full_volume(void)
{
  TEST_BEGIN("exfat write cov: all clusters used -> no_mem at line 143");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  /* Bitmap cluster is always at cluster 2 = first_data_lba for a fresh volume. */
  const uint32_t bmp_lba = h->first_data_lba;
  for (uint32_t s = 0U; s < h->sectors_per_cluster; s++) {
    memset(&s_disk.bytes[(bmp_lba + s) * (uint32_t)k_wc_block_size],
           0xFF,
           (uint32_t)k_wc_block_size);
  }

  const uint8_t dummy = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_write_file(h, "X.TXT", &dummy, 1U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat write cov: all clusters used -> no_mem at line 143");
}

/**
 * @test test_bmp_switch_write_fails
 * @brief `priv_exfat_bmp_switch` returns the write error when flushing the
 *        old sector fails (lines 153, 154, 155).
 *
 * @details Calls `priv_exfat_bmp_switch` directly with `*loaded` set to a
 *          valid LBA (not UINT32_MAX) and a different target LBA, so the
 *          function enters the flush block.  With `s_wr_remaining=0`, the
 *          `priv_write_sector` call fails, and the function returns the write
 *          error at line 155.
 *
 * Lines targeted: 153, 154, 155.
 *
 * @par MC/DC:
 * Decision: `if (we != k_ra8_ok)` in bmp_switch -- 1 condition.
 * V1: we=error -> T -> return write error (this test).
 * V2: we=k_ra8_ok -> F -> proceed to read (test_bmp_switch_sector_change).
 *
 * @pre Volume is formatted and accessible.
 * @post priv_exfat_bmp_switch returns non-ok.
 *
 * @since 0.1.0
 */
static void test_bmp_switch_write_fails(void)
{
  TEST_BEGIN("exfat write cov: bmp_switch old-sector write fails (lines 153-155)");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  /* Arm: fail the very next write. */
  s_wr_remaining = (int32_t)k_wc_wr_at_w1;

  uint32_t        loaded               = h->first_fat_lba;
  uint8_t         sec[k_wc_block_size] = {};
  const ra8_err_t r = priv_exfat_bmp_switch(h, h->first_fat_lba + 1U, &loaded, sec);
  TEST_ASSERT(r != k_ra8_ok);

  s_wr_remaining = (int32_t)k_wc_wr_never;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat write cov: bmp_switch old-sector write fails (lines 153-155)");
}

/**
 * @test test_bmp_switch_sector_change
 * @brief `priv_exfat_bmp_switch` flushes the old sector and loads the new one
 *        when both I/O ops succeed (lines 153, 154, 157).
 *
 * @details Calls `priv_exfat_bmp_switch` with `*loaded` set to a valid LBA
 *          (not UINT32_MAX) and a different target LBA.  With no faults armed,
 *          the write of the old sector succeeds (line 153, we=k_ra8_ok) and the
 *          read of the new sector succeeds (line 157), returning k_ra8_ok.
 *
 * Lines targeted: 153, 154, 157.
 *
 * @par MC/DC:
 * Decision: `if (*loaded != UINT32_MAX)` in bmp_switch -- 1 condition.
 * V1: loaded=valid_lba -> T -> enters flush block (this test and write-fails
 *     test).
 * V2: loaded=UINT32_MAX -> F -> skips flush (normal bitmap_mark first call).
 *
 * @pre Volume is formatted and accessible.
 * @post priv_exfat_bmp_switch returns k_ra8_ok.
 *
 * @since 0.1.0
 */
static void test_bmp_switch_sector_change(void)
{
  TEST_BEGIN("exfat write cov: bmp_switch flush+load -> k_ra8_ok (lines 153,154,157)");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  /* No faults -- both write (flush old) and read (load new) must succeed. */
  uint32_t loaded               = h->first_fat_lba;
  uint8_t  sec[k_wc_block_size] = {};
  TEST_ASSERT_EQ(k_ra8_ok, priv_exfat_bmp_switch(h, h->first_fat_lba + 1U, &loaded, sec));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat write cov: bmp_switch flush+load -> k_ra8_ok (lines 153,154,157)");
}

/**
 * @test test_bmp_switch_read_fail
 * @brief Read failure on R3 propagates through `priv_exfat_bmp_switch`
 *        (lines 160, 197, 561).
 *
 * @details R1 (find_bitmap) and R2 (bitmap_scan) succeed; W1 (write_data)
 *          succeeds; R3 is the `priv_read_sector` call inside
 *          `priv_exfat_bmp_switch` (loaded=UINT32_MAX path, lines 158-160).
 *          The error propagates through `priv_exfat_bitmap_mark` (line 197)
 *          and `priv_exfat_create` (line 561).
 *
 * Lines targeted: 160, 197, 561.
 *
 * @par MC/DC:
 * Decision: `if (e != k_ra8_ok)` in bmp_switch read path -- 1 condition.
 * V1: e=error -> T -> return error (this test).
 * V2: e=k_ra8_ok -> F -> loaded updated, k_ra8_ok returned (normal path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns non-ok.
 *
 * @since 0.1.0
 */
static void test_bmp_switch_read_fail(void)
{
  TEST_BEGIN("exfat write cov: R3 read fail in bmp_switch (lines 160,197,561)");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  s_rd_remaining = (int32_t)k_wc_rd_at_r3;

  const uint8_t   dummy = (uint8_t)'X';
  const ra8_err_t r     = ra8_fs_write_file(h, "X.TXT", &dummy, 1U);
  TEST_ASSERT(r != k_ra8_ok);

  s_rd_remaining = (int32_t)k_wc_rd_never;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat write cov: R3 read fail in bmp_switch (lines 160,197,561)");
}

/**
 * @test test_write_data_fail
 * @brief Write failure on W1 propagates through `priv_exfat_write_data`
 *        (lines 240, 494, 561).
 *
 * @details R1 (find_bitmap) and R2 (bitmap_scan) succeed; W1 is the
 *          `priv_write_sector` inside `priv_exfat_write_data`.  Failing it
 *          exercises line 240 in write_data, line 494 in alloc_write, and
 *          line 561 in create.
 *
 * Lines targeted: 240, 494, 561.
 *
 * @par MC/DC:
 * Decision: `if (e != k_ra8_ok)` in write_data -- 1 condition.
 * V1: e=error -> T -> early return (this test).
 * V2: e=k_ra8_ok -> F -> continue sector loop (normal write path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns non-ok.
 *
 * @since 0.1.0
 */
static void test_write_data_fail(void)
{
  TEST_BEGIN("exfat write cov: W1 write fail in write_data (lines 240,494,561)");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  s_wr_remaining = (int32_t)k_wc_wr_at_w1;

  const uint8_t   dummy = (uint8_t)'X';
  const ra8_err_t r     = ra8_fs_write_file(h, "X.TXT", &dummy, 1U);
  TEST_ASSERT(r != k_ra8_ok);

  s_wr_remaining = (int32_t)k_wc_wr_never;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat write cov: W1 write fail in write_data (lines 240,494,561)");
}

/**
 * @test test_read_entry_fail
 * @brief Read failure on R4 (first `priv_exfat_read_entry` in
 *        `priv_exfat_find_dir_space`) propagates (lines 273, 341, 531).
 *
 * @details R1-R3 succeed (find_bitmap, bitmap_scan, bmp_switch); W1-W2
 *          succeed (write_data, bitmap_mark); R4 is the first
 *          `priv_read_sector` inside `priv_exfat_read_entry` for entry 0 of
 *          the root cluster.  Failing it exercises line 273 in read_entry,
 *          line 341 in find_dir_space, and line 531 in link.
 *
 * Lines targeted: 273, 341, 531.
 *
 * @par MC/DC:
 * Decision: `if (r != k_ra8_ok)` in find_dir_space -- 1 condition.
 * V1: r=error -> T -> return r (this test).
 * V2: r=k_ra8_ok -> F -> examine slot (normal scan path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns non-ok.
 *
 * @since 0.1.0
 */
static void test_read_entry_fail(void)
{
  TEST_BEGIN("exfat write cov: R4 read fail -> lines 273,341,531");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  s_rd_remaining = (int32_t)k_wc_rd_at_r4;

  const uint8_t   dummy = (uint8_t)'X';
  const ra8_err_t r     = ra8_fs_write_file(h, "X.TXT", &dummy, 1U);
  TEST_ASSERT(r != k_ra8_ok);

  s_rd_remaining = (int32_t)k_wc_rd_never;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat write cov: R4 read fail -> lines 273,341,531");
}

/**
 * @test test_dir_space_full_root_eoc
 * @brief `priv_exfat_find_dir_space` reaches the FAT chain check and returns
 *        `k_ra8_err_no_mem` when FAT[root] is EOC (lines 356-358, 361, 362, 531).
 *
 * @details Patches root entries 3-127 to type 0x85 (in-use).  Entries 0-2
 *          are system entries (also in-use); entries 16-127 are already 0xA5
 *          (bit 7 set) from the pre-fill.  With all 128 entries in-use,
 *          `priv_exfat_find_dir_space` scans the entire cluster, then calls
 *          `priv_fat_get` (lines 356-357).  The FAT entry for the root cluster
 *          is EOC on a fresh volume, so `priv_is_eoc` returns true and the
 *          function returns `k_ra8_err_no_mem` at line 362.  `priv_exfat_link`
 *          propagates at line 531.
 *
 * Lines targeted: 356, 357, 358, 361, 362, 531.
 *
 * @par MC/DC:
 * Decision: `if (priv_is_eoc(m, next) != 0U)` -- 1 condition.
 * V1: next=EOC -> T -> return no_mem (this test).
 * V2: next=valid_cluster -> F -> follow chain (test_dir_space_chain).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns k_ra8_err_no_mem.
 *
 * @since 0.1.0
 */
static void test_dir_space_full_root_eoc(void)
{
  TEST_BEGIN("exfat write cov: full root + FAT EOC -> no_mem (lines 356-362,531)");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  patch_root_full(h, (uint32_t)k_wc_patch_start);

  const uint8_t dummy = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_write_file(h, "X.TXT", &dummy, 1U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat write cov: full root + FAT EOC -> no_mem (lines 356-362,531)");
}

/**
 * @test test_dir_space_fat_get_fail
 * @brief Read failure on R132 causes `priv_fat_get` to fail inside
 *        `priv_exfat_find_dir_space` (line 359).
 *
 * @details With entries 3-127 patched to 0x85 (in-use), the inner for loop
 *          runs all 128 iterations (R4-R131).  R132 is the `priv_read_sector`
 *          inside `priv_fat_get` (lines 356-357).  Failing it makes `fe !=
 *          k_ra8_ok` true and exercises line 359.
 *
 * Read sequence (3 alloc_write + 128 find_dir_space inner loop = 131
 * successes; R132 = fat_get):
 *   R1  find_bitmap, R2  bitmap_scan, R3  bmp_switch,
 *   R4-R131 read_entry(cluster, 0..127),
 *   R132 priv_fat_get -> FAIL.
 *
 * Lines targeted: 359.
 *
 * @par MC/DC:
 * Decision: `if (fe != k_ra8_ok)` in find_dir_space FAT chain -- 1 condition.
 * V1: fe=error -> T -> return fe (this test).
 * V2: fe=k_ra8_ok -> F -> is_eoc check (test_dir_space_full_root_eoc).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns non-ok.
 *
 * @since 0.1.0
 */
static void test_dir_space_fat_get_fail(void)
{
  TEST_BEGIN("exfat write cov: R132 fat_get fail -> line 359");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  patch_root_full(h, (uint32_t)k_wc_patch_start);
  s_rd_remaining = (int32_t)k_wc_rd_fat_get;

  const uint8_t   dummy = (uint8_t)'X';
  const ra8_err_t r     = ra8_fs_write_file(h, "X.TXT", &dummy, 1U);
  TEST_ASSERT(r != k_ra8_ok);

  s_rd_remaining = (int32_t)k_wc_rd_never;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat write cov: R132 fat_get fail -> line 359");
}

/**
 * @test test_dir_space_chain
 * @brief `priv_exfat_find_dir_space` follows a FAT chain to a second cluster
 *        that has free entries (lines 364, 365).
 *
 * @details Patches:
 *   - Root entries 3-127 to 0x85 (all in-use).
 *   - FAT[root_cluster] = G (root_cluster + 100), so the FAT chain extends.
 *   - FAT[G] = EOC (0xFFFFFFFF) to terminate the chain at G.
 *   - All sectors of cluster G zeroed so entries appear as EOD (free).
 *
 * `priv_exfat_find_dir_space` scans the root cluster (no free run), calls
 * `priv_fat_get` (succeeds, returns G, not EOC), sets `cluster=G` at line 364
 * and increments `guard` at line 365.  It then scans G and finds the first
 * 3 entries free, returning k_ra8_ok.  The file is created successfully.
 *
 * Lines targeted: 364, 365.
 *
 * @par MC/DC:
 * Decision: `if (priv_is_eoc(m, next) != 0U)` -- 1 condition.
 * V1: next=EOC -> T -> no_mem (test_dir_space_full_root_eoc).
 * V2: next=G (not EOC) -> F -> cluster=G, guard++ (this test).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns k_ra8_ok.
 *
 * @since 0.1.0
 */
static void test_dir_space_chain(void)
{
  TEST_BEGIN("exfat write cov: FAT chain to cluster G -> lines 364,365");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  patch_root_full(h, (uint32_t)k_wc_patch_start);

  const uint32_t G     = h->root_cluster + (uint32_t)k_wc_chain_offset;
  const uint32_t G_lba = h->first_data_lba + (G - 2U) * h->sectors_per_cluster;

  /* Zero cluster G so all entries appear as EOD (free). */
  for (uint32_t s = 0U; s < h->sectors_per_cluster; s++) {
    memset(&s_disk.bytes[(G_lba + s) * (uint32_t)k_wc_block_size], 0x00, (uint32_t)k_wc_block_size);
  }

  /* Build FAT chain: root_cluster -> G -> EOC. */
  disk_set_u32le(fat_entry_off(h, h->root_cluster), G);
  disk_set_u32le(fat_entry_off(h, G), (uint32_t)k_wc_fat_eoc);

  const uint8_t dummy = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "X.TXT", &dummy, 1U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat write cov: FAT chain to cluster G -> lines 364,365");
}

/**
 * @test test_write_dir_set_read_fail
 * @brief Read failure on R10 (first read inside `priv_exfat_write_dir_set`)
 *        propagates (line 441).
 *
 * @details R1-R9 succeed (3 in alloc_write + 6 in find_dir_space for entries
 *          0-5 of the root cluster); W1-W2 succeed (write_data, bitmap_mark).
 *          R10 is the `priv_read_sector` inside `priv_exfat_write_dir_set`
 *          for the first directory entry (index 3).  Failing it exercises
 *          line 441.
 *
 * Lines targeted: 441.
 *
 * @par MC/DC:
 * Decision: `if (e != k_ra8_ok)` in write_dir_set read -- 1 condition.
 * V1: e=error -> T -> return e (this test).
 * V2: e=k_ra8_ok -> F -> patch and write (normal write_dir_set path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns non-ok.
 *
 * @since 0.1.0
 */
static void test_write_dir_set_read_fail(void)
{
  TEST_BEGIN("exfat write cov: R10 write_dir_set read fail -> line 441");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  s_rd_remaining = (int32_t)k_wc_rd_at_r10;

  const uint8_t   dummy = (uint8_t)'X';
  const ra8_err_t r     = ra8_fs_write_file(h, "X.TXT", &dummy, 1U);
  TEST_ASSERT(r != k_ra8_ok);

  s_rd_remaining = (int32_t)k_wc_rd_never;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat write cov: R10 write_dir_set read fail -> line 441");
}

/**
 * @test test_write_dir_set_write_fail
 * @brief Write failure on W3 (first write inside `priv_exfat_write_dir_set`)
 *        propagates (line 448).
 *
 * @details W1 (write_data) and W2 (bitmap_mark) succeed; W3 is the
 *          `priv_write_sector` inside `priv_exfat_write_dir_set` for the
 *          patched sector of directory entry 3.  Failing it exercises line 448.
 *
 * Lines targeted: 448.
 *
 * @par MC/DC:
 * Decision: `if (e != k_ra8_ok)` in write_dir_set write -- 1 condition.
 * V1: e=error -> T -> return e (this test).
 * V2: e=k_ra8_ok -> F -> next entry (normal write_dir_set path).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_write_file returns non-ok.
 *
 * @since 0.1.0
 */
static void test_write_dir_set_write_fail(void)
{
  TEST_BEGIN("exfat write cov: W3 write_dir_set write fail -> line 448");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  s_wr_remaining = (int32_t)k_wc_wr_at_w3;

  const uint8_t   dummy = (uint8_t)'X';
  const ra8_err_t r     = ra8_fs_write_file(h, "X.TXT", &dummy, 1U);
  TEST_ASSERT(r != k_ra8_ok);

  s_wr_remaining = (int32_t)k_wc_wr_never;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat write cov: W3 write_dir_set write fail -> line 448");
}

/* ---- entry point ----------------------------------------------------------- */

int32_t main(void)
{
  test_create_empty_path();
  test_create_path_too_long();
  test_create_zero_len();
  test_find_bitmap_eod();
  test_find_bitmap_read_fail();
  test_bitmap_scan_read_fail();
  test_bitmap_scan_full_volume();
  test_bmp_switch_write_fails();
  test_bmp_switch_sector_change();
  test_bmp_switch_read_fail();
  test_write_data_fail();
  test_read_entry_fail();
  test_dir_space_full_root_eoc();
  test_dir_space_fat_get_fail();
  test_dir_space_chain();
  test_write_dir_set_read_fail();
  test_write_dir_set_write_fail();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_fat_exfat_write_cov.c\n");
  return 0;
}
