/**
 * @file tests/test_ra8_fs_fat_exfat_read_cov.c
 * @brief Coverage-boost tests for `ra8_fs_fat_exfat_read.c`.
 *
 * @details
 * Targets the lines in the exFAT read path that normal API usage cannot reach:
 *
 *   - `priv_ascii_upper`: above-z branch (lines 42-43) and a-z conversion
 *     (line 45).
 *   - `priv_exfat_parse`: bad BytesPerSectorShift rejection (line 98).
 *   - `priv_exfat_next_entry`: cluster-boundary FAT-read failure (131-134),
 *     end-of-chain detection (136-137), chain follow (139-141), and sector-read
 *     failure (147).
 *   - `priv_exfat_name_chunk_eq`: non-ASCII UTF-16 high byte (165) and full
 *     15-char loop completion (171).
 *   - `priv_exfat_match_set`: stream I/O fail (208), wrong stream type (211),
 *     name I/O fail (221), wrong name type (224).
 *   - `priv_exfat_find`: first-read I/O fail (253), match_set I/O error
 *     propagation (266).
 *   - `priv_exfat_open`: write-mode rejection (277), file-table-full (288).
 *
 * A countdown I/O backend injects read failures at exact call positions.
 * Direct byte patching of the RAM sector store creates volume states the
 * public API alone cannot produce.
 *
 * Line 269 of the source (scan-limit exhaustion path in `priv_exfat_find`)
 * carries GCOVR_EXCL_LINE: reaching it requires 65536 consecutive non-EOD
 * directory entries, which no host-side input can synthesise in a bounded test.
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

/** @brief Fill byte for an unformatted disk image. */
typedef enum : uint8_t {
  k_exfat_cov_fill_unformatted = 0xA5U, /**< Matches no boot signature. */
} exfat_cov_fill_t;

/* ---- constants ------------------------------------------------------------- */

/**
 * @enum rc_const_t
 * @brief Sizing, offset, and type constants for the exFAT read coverage tests.
 *
 * @details All numeric literals in this file appear here as named enum values.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_rc_block_size    = 512U,    /**< Bytes per sector.                          */
  k_rc_blocks_exfat  = 131072U, /**< 64 MiB volume in sectors.                  */
  k_rc_entry_bytes   = 32U,     /**< exFAT directory entry size.                */
  k_rc_vbr_bps_off   = 0x6CU,   /**< BytesPerSectorShift offset inside the VBR. */
  k_rc_bps_shift_bad = 8U,      /**< Invalid BPS shift (not 9).                 */
  k_rc_type_bogus    = 0x42U,   /**< Invalid entry type used for disk patching. */
  k_rc_mask_byte     = 0xFFU,   /**< Low-byte mask for 32-bit LE packing.       */
  k_rc_shift8        = 8U,      /**< 8-bit left shift for LE32 pack.            */
  k_rc_shift16       = 16U,     /**< 16-bit left shift for LE32 pack.           */
  k_rc_shift24       = 24U,     /**< 24-bit left shift for LE32 pack.           */
  k_rc_name_off      = 2U,      /**< First character byte offset in Name entry. */
  k_rc_name_per_ent  = 15U,     /**< UTF-16 units per Name entry.               */
  k_rc_root_strm_idx = 4U,      /**< Root dir: first user Stream entry index.   */
  k_rc_root_name_idx = 5U,      /**< Root dir: first user Name entry index.     */
  k_rc_max_files     = 4U,      /**< File table capacity (k_ra8_fs_max_files).  */
} rc_const_t;

/**
 * @enum rc_rd_fail_t
 * @brief Read-countdown seeds for fault injection.
 *
 * @details Negative means never fail; zero means fail the very next read;
 *          N means succeed N times then fail on call N+1.
 *
 * @since 0.1.0
 */
typedef enum : int32_t {
  k_rc_rd_never = -1, /**< Never inject a read error.                         */
  k_rc_rd_at_0  = 0,  /**< Fail on the very first read.                       */
  k_rc_rd_at_4  = 4,  /**< Fail on the 5th read (stream-entry read in match). */
  k_rc_rd_at_5  = 5,  /**< Fail on the 6th read (name-entry read in match).   */
} rc_rd_fail_t;

/* ---- RAM-backed block device with countdown fault injection -------------- */

/**
 * @struct rc_disk_t
 * @brief Memory-backed disk presented to ra8_fs as a block device.
 *
 * @details Sector store is a flat malloc buffer; tests can inspect or corrupt
 *          on-disk state by patching individual bytes after a normal format.
 *
 * @invariant bytes is non-NULL while the volume is alive.
 * @since 0.1.0
 */
typedef struct {
  uint8_t* bytes;       /**< Flat sector store (malloc'd). */
  uint32_t block_count; /**< Number of 512-byte sectors.   */
} rc_disk_t;

/**
 * @var s_disk
 * @brief Module-level RAM disk shared across all tests.
 * @warning Modify only through build_exfat_volume() and free_volume().
 * @since 0.1.0
 */
static rc_disk_t s_disk = {};

/**
 * @var s_rd_remaining
 * @brief Read countdown: negative=never fail, 0=fail next, N=fail after N.
 * @warning Reset to k_rc_rd_never in build_exfat_volume(); set per-test only.
 * @since 0.1.0
 */
static int32_t s_rd_remaining = (int32_t)k_rc_rd_never;

/**
 * @brief Countdown-faulting ra8_fs read backend.
 *
 * @details Decrements s_rd_remaining per call. When it reaches zero the call
 *          returns k_ra8_err_out_of_range without touching the disk, simulating
 *          a read I/O failure at an exact position in the call sequence.
 *
 * @param[in]  ctx   Pointer to rc_disk_t.
 * @param[in]  lba   Start sector.
 * @param[in]  count Sector count.
 * @param[out] buf   Destination buffer.
 *
 * @return Error code.
 * @retval k_ra8_ok              Sectors copied into buf.
 * @retval k_ra8_err_out_of_range Countdown expired or request out of range.
 *
 * @pre ctx and buf are non-NULL.
 * @post buf holds the requested sectors on k_ra8_ok.
 *
 * @since 0.1.0
 */
static ra8_err_t rc_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  if (s_rd_remaining == (int32_t)k_rc_rd_at_0) {
    return k_ra8_err_out_of_range;
  }
  if (s_rd_remaining > (int32_t)k_rc_rd_at_0) {
    s_rd_remaining--;
  }
  const rc_disk_t* d = (const rc_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_rc_block_size],
         (size_t)count * (size_t)k_rc_block_size);
  return k_ra8_ok;
}

/**
 * @brief Plain write backend (no countdown fault injection).
 *
 * @param[in] ctx   Pointer to rc_disk_t.
 * @param[in] lba   Start sector.
 * @param[in] count Sector count.
 * @param[in] buf   Source buffer.
 *
 * @return Error code.
 * @retval k_ra8_ok              Sectors written to the RAM disk.
 * @retval k_ra8_err_out_of_range Request out of range.
 *
 * @pre ctx and buf are non-NULL.
 * @post Disk bytes match buf on k_ra8_ok.
 *
 * @since 0.1.0
 */
static ra8_err_t rc_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  rc_disk_t* d = (rc_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_rc_block_size],
         buf,
         (size_t)count * (size_t)k_rc_block_size);
  return k_ra8_ok;
}

/**
 * @brief Report disk geometry to ra8_fs.
 *
 * @param[in]  ctx         Pointer to rc_disk_t.
 * @param[out] block_count Receives the sector count.
 * @param[out] block_size  Receives 512.
 *
 * @return k_ra8_ok always.
 * @retval k_ra8_ok Geometry populated.
 *
 * @pre ctx, block_count, and block_size are non-NULL.
 * @post *block_count and *block_size reflect the disk dimensions.
 *
 * @since 0.1.0
 */
static ra8_err_t rc_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  const rc_disk_t* d = (const rc_disk_t*)ctx;
  *block_count       = d->block_count;
  *block_size        = (uint32_t)k_rc_block_size;
  return k_ra8_ok;
}

/**
 * @var s_ctrl_backend
 * @brief Countdown-faulting backend wired to s_disk for all tests.
 * @since 0.1.0
 */
static const ra8_fs_backend_t s_ctrl_backend = {
  .read_block   = rc_read,
  .write_block  = rc_write,
  .get_capacity = rc_capacity,
  .ctx          = &s_disk,
};

/* ---- harness helpers ------------------------------------------------------- */

/**
 * @brief Release the heap buffer held by s_disk.
 *
 * @pre None.
 * @post s_disk.bytes is nullptr.
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
 * @details Resets s_rd_remaining to never-fail before the format so it always
 *          succeeds. Pre-fills with 0xA5 to prove the formatter writes the VBR
 *          rather than relying on lucky zeros.
 *
 * @pre None.
 * @post s_disk holds a valid exFAT image; s_rd_remaining == k_rc_rd_never.
 *
 * @since 0.1.0
 */
static void build_exfat_volume(void)
{
  free_volume();
  s_rd_remaining     = (int32_t)k_rc_rd_never;
  const size_t total = (size_t)k_rc_blocks_exfat * (size_t)k_rc_block_size;
  s_disk.bytes       = (uint8_t*)malloc(total);
  s_disk.block_count = (uint32_t)k_rc_blocks_exfat;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "malloc failed for exFAT volume");
  }
  memset(s_disk.bytes, k_exfat_cov_fill_unformatted, total);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_exfat;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_ctrl_backend, &opts));
}

/**
 * @brief Byte offset in s_disk.bytes for root-dir entry idx.
 *
 * @param[in] h   Mounted exFAT volume.
 * @param[in] idx Entry index (0-based) within the root cluster.
 *
 * @return Absolute byte offset into s_disk.bytes.
 *
 * @pre h is non-NULL and mounted.
 * @post Return value is within the root cluster sector range.
 *
 * @since 0.1.0
 */
static uint32_t root_entry_off(const ra8_fs_mount_t* h, uint32_t idx)
{
  const uint32_t root_lba = h->first_data_lba + ((h->root_cluster - 2U) * h->sectors_per_cluster);
  return (root_lba * (uint32_t)k_rc_block_size) + (idx * (uint32_t)k_rc_entry_bytes);
}

/**
 * @brief Byte offset in s_disk.bytes for the FAT entry of cluster clus.
 *
 * @param[in] h    Mounted exFAT volume.
 * @param[in] clus Cluster number.
 *
 * @return Absolute byte offset of the 4-byte FAT entry for clus.
 *
 * @pre h is non-NULL and mounted.
 * @post Return value addresses a valid 4-byte region in s_disk.bytes.
 *
 * @since 0.1.0
 */
static uint32_t fat_entry_off(const ra8_fs_mount_t* h, uint32_t clus)
{
  return (h->first_fat_lba * (uint32_t)k_rc_block_size) + (clus * 4U);
}

/**
 * @brief Write a 4-byte LE value at byte offset off in s_disk.bytes.
 *
 * @param[in] off Absolute byte offset (must leave room for 4 bytes).
 * @param[in] val 32-bit value to store in little-endian byte order.
 *
 * @pre off + 4 is within the disk image.
 * @post Bytes at off encode val in LE order.
 *
 * @since 0.1.0
 */
static void disk_set_u32le(uint32_t off, uint32_t val)
{
  uint8_t* p = &s_disk.bytes[off];
  p[0]       = (uint8_t)(val & (uint32_t)k_rc_mask_byte);
  p[1]       = (uint8_t)((val >> (uint32_t)k_rc_shift8) & (uint32_t)k_rc_mask_byte);
  p[2]       = (uint8_t)((val >> (uint32_t)k_rc_shift16) & (uint32_t)k_rc_mask_byte);
  p[3]       = (uint8_t)((val >> (uint32_t)k_rc_shift24) & (uint32_t)k_rc_mask_byte);
}

/* ---- tests ----------------------------------------------------------------- */

/**
 * @test test_ascii_upper_above_z
 * @brief priv_ascii_upper returns input unchanged when c > 'z' (lines 42-43).
 *
 * @details '{' (ASCII 123) is above 'z' (122) so the second if-branch fires.
 *
 * @par MC/DC:
 * Decision A: `if (c < 'a')` -- 1 condition.
 * V1: c='{' -> false -> check B.
 * Decision B: `if (c > 'z')` -- 1 condition.
 * V2: c='{' -> true -> return c (this test, lines 42-43).
 * V3: c='a' -> false -> line 45 (test_ascii_upper_a_to_z).
 *
 * @pre None.
 * @post priv_ascii_upper('{') equals '{'.
 *
 * @since 0.1.0
 */
static void test_ascii_upper_above_z(void)
{
  TEST_BEGIN("exfat read cov: ascii_upper '{' -> unchanged (lines 42-43)");
  char result = priv_ascii_upper('{');
  TEST_ASSERT_EQ('{', result);
  TEST_END("exfat read cov: ascii_upper '{' -> unchanged (lines 42-43)");
}

/**
 * @test test_ascii_upper_a_to_z
 * @brief priv_ascii_upper maps 'a'..'z' to 'A'..'Z' (line 45).
 *
 * @par MC/DC:
 * See test_ascii_upper_above_z.
 * V3 (this test): c='a' -> A false, B false -> line 45 return 'A'.
 *
 * @pre None.
 * @post priv_ascii_upper('a') equals 'A'.
 *
 * @since 0.1.0
 */
static void test_ascii_upper_a_to_z(void)
{
  TEST_BEGIN("exfat read cov: ascii_upper 'a' -> 'A' (line 45)");
  char result = priv_ascii_upper('a');
  TEST_ASSERT_EQ('A', result);
  TEST_END("exfat read cov: ascii_upper 'a' -> 'A' (line 45)");
}

/**
 * @test test_parse_bad_bps_shift
 * @brief priv_exfat_parse rejects a non-512-byte sector size (line 98).
 *
 * @details Patches VBR byte 0x6C (BytesPerSectorShift) from 9 to 8; the
 *          exFAT signature "EXFAT   " remains intact so priv_exfat_is_volume
 *          returns 1, then priv_exfat_parse hits line 98.
 *
 * @par MC/DC:
 * Decision: `if (buf[k_exfat_off_bps_shift] != k_exfat_bps_shift_512)` -- 1 cond.
 * V1: shift=8 (not 9) -> true -> line 98, not_supported (this test).
 * V2: shift=9 -> false -> continues (every mount on a valid image).
 *
 * @pre None.
 * @post ra8_fs_mount returns an error after BPS shift is patched.
 *
 * @since 0.1.0
 */
static void test_parse_bad_bps_shift(void)
{
  TEST_BEGIN("exfat read cov: bad BPS shift -> error (line 98)");
  build_exfat_volume();
  s_disk.bytes[(uint32_t)k_rc_vbr_bps_off] = (uint8_t)k_rc_bps_shift_bad;
  ra8_fs_mount_t* h                        = nullptr;
  ra8_err_t       e                        = ra8_fs_mount(&s_ctrl_backend, &h);
  TEST_ASSERT(e != k_ra8_ok);
  free_volume();
  TEST_END("exfat read cov: bad BPS shift -> error (line 98)");
}

/**
 * @test test_next_entry_fat_read_fail
 * @brief Cluster boundary: priv_fat_get I/O failure returns error (lines 131-134).
 *
 * @details Sets entry_in_cluster to per_cluster (boundary) and countdown to 0
 *          so the FAT sector read (inside priv_fat_get) fails immediately.
 *
 * @par MC/DC:
 * Decision A: `if (cur->entry_in_cluster >= per_cluster)` -- 1 cond.
 * V1: entry_in_cluster = per_cluster -> true -> boundary block (this + eoc + chain tests).
 * V2: entry_in_cluster = 0 -> false -> skip block (sector-read-fail test).
 *
 * Decision B: `if (e != k_ra8_ok)` after priv_fat_get -- 1 cond.
 * V3: FAT read fails -> true -> line 134 (this test).
 * V4: FAT read ok -> false -> check EOC (eoc + chain tests).
 *
 * @pre Volume is formatted and mounted.
 * @post priv_exfat_next_entry returns != k_ra8_ok.
 *
 * @since 0.1.0
 */
static void test_next_entry_fat_read_fail(void)
{
  TEST_BEGIN("exfat read cov: boundary FAT-read fail (lines 131-134)");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  const uint32_t per_cluster =
    (h->sectors_per_cluster * (uint32_t)k_rc_block_size) / (uint32_t)k_rc_entry_bytes;
  exfat_cursor_t cur = {.cluster = h->root_cluster, .entry_in_cluster = per_cluster, .scanned = 0U};
  uint8_t        buf[k_rc_entry_bytes] = {};

  s_rd_remaining = (int32_t)k_rc_rd_at_0;
  ra8_err_t e    = priv_exfat_next_entry(h, &cur, buf);
  TEST_ASSERT(e != k_ra8_ok);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat read cov: boundary FAT-read fail (lines 131-134)");
}

/**
 * @test test_next_entry_eoc
 * @brief Cluster boundary: EOC in FAT returns not_found (lines 131-132, 136-137).
 *
 * @details Root cluster FAT entry is EOC on a fresh volume; crossing the
 *          per_cluster boundary reads EOC and returns k_ra8_err_not_found.
 *
 * @par MC/DC:
 * Decision: `if (priv_is_eoc(m, next) != 0U)` -- 1 cond.
 * V5: next = EOC -> true -> line 137, not_found (this test).
 * V6: next = valid cluster -> false -> lines 139-141 (chain test).
 *
 * @pre Volume is formatted and mounted.
 * @post priv_exfat_next_entry returns k_ra8_err_not_found.
 *
 * @since 0.1.0
 */
static void test_next_entry_eoc(void)
{
  TEST_BEGIN("exfat read cov: boundary EOC -> not_found (lines 131-132, 136-137)");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  const uint32_t per_cluster =
    (h->sectors_per_cluster * (uint32_t)k_rc_block_size) / (uint32_t)k_rc_entry_bytes;
  exfat_cursor_t cur = {.cluster = h->root_cluster, .entry_in_cluster = per_cluster, .scanned = 0U};
  uint8_t        buf[k_rc_entry_bytes] = {};

  s_rd_remaining = (int32_t)k_rc_rd_never;
  ra8_err_t e    = priv_exfat_next_entry(h, &cur, buf);
  TEST_ASSERT_EQ(k_ra8_err_not_found, e);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat read cov: boundary EOC -> not_found (lines 131-132, 136-137)");
}

/**
 * @test test_next_entry_follow_chain
 * @brief Cluster boundary: valid next cluster advances the cursor (lines 131-132, 139-141).
 *
 * @details Patches FAT[root_cluster] to point to root_cluster+1 (not EOC) so
 *          the boundary block follows the chain and reads the first entry of
 *          the next cluster.
 *
 * @par MC/DC:
 * See test_next_entry_eoc for the priv_is_eoc decision.
 * V6 (this test): next = root_cluster+1 -> false -> lines 139-141.
 *
 * @pre Volume is formatted and mounted.
 * @post priv_exfat_next_entry returns k_ra8_ok; cur.cluster == root_cluster+1.
 *
 * @since 0.1.0
 */
static void test_next_entry_follow_chain(void)
{
  TEST_BEGIN("exfat read cov: boundary chain-follow (lines 131-132, 139-141)");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  /* Redirect FAT[root_cluster] -> root_cluster+1 (free, not EOC). */
  const uint32_t next_clus = h->root_cluster + 1U;
  disk_set_u32le(fat_entry_off(h, h->root_cluster), next_clus);

  const uint32_t per_cluster =
    (h->sectors_per_cluster * (uint32_t)k_rc_block_size) / (uint32_t)k_rc_entry_bytes;
  exfat_cursor_t cur = {.cluster = h->root_cluster, .entry_in_cluster = per_cluster, .scanned = 0U};
  uint8_t        buf[k_rc_entry_bytes] = {};

  s_rd_remaining = (int32_t)k_rc_rd_never;
  ra8_err_t e    = priv_exfat_next_entry(h, &cur, buf);
  TEST_ASSERT_EQ(k_ra8_ok, e);
  TEST_ASSERT_EQ(next_clus, cur.cluster);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat read cov: boundary chain-follow (lines 131-132, 139-141)");
}

/**
 * @test test_next_entry_sector_read_fail
 * @brief priv_read_sector failure propagates to the caller (line 147).
 *
 * @details No boundary crossing (entry_in_cluster == 0); countdown == 0 makes
 *          the sector read fail on the first call, hitting line 147.
 *
 * @par MC/DC:
 * Decision: `if (e != k_ra8_ok)` after priv_read_sector -- 1 cond.
 * V7: sector read fails -> true -> line 147 (this test).
 * V8: sector read ok -> false -> copy entry (success-path tests).
 *
 * @pre Volume is formatted and mounted.
 * @post priv_exfat_next_entry returns != k_ra8_ok.
 *
 * @since 0.1.0
 */
static void test_next_entry_sector_read_fail(void)
{
  TEST_BEGIN("exfat read cov: sector read fail (line 147)");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  exfat_cursor_t cur = {.cluster = h->root_cluster, .entry_in_cluster = 0U, .scanned = 0U};
  uint8_t        buf[k_rc_entry_bytes] = {};

  s_rd_remaining = (int32_t)k_rc_rd_at_0;
  ra8_err_t e    = priv_exfat_next_entry(h, &cur, buf);
  TEST_ASSERT(e != k_ra8_ok);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat read cov: sector read fail (line 147)");
}

/**
 * @test test_name_chunk_eq_nonascii
 * @brief Non-ASCII UTF-16 high byte causes early return 0 (line 165).
 *
 * @details Sets entry byte at k_rc_name_off+1 to 1 (high byte non-zero);
 *          the first iteration of the inner loop hits line 165.
 *
 * @par MC/DC:
 * Decision: `if (entry[b+1] != 0U)` -- 1 cond.
 * V1: high_byte = 1 -> true -> return 0 (this test, line 165).
 * V2: high_byte = 0 -> false -> char compare (full-match test).
 *
 * @pre None.
 * @post priv_exfat_name_chunk_eq returns 0.
 *
 * @since 0.1.0
 */
static void test_name_chunk_eq_nonascii(void)
{
  TEST_BEGIN("exfat read cov: name_chunk_eq non-ASCII high byte (line 165)");
  uint8_t entry[(uint32_t)k_rc_entry_bytes] = {};
  entry[(uint32_t)k_rc_name_off]            = (uint8_t)'A'; /* low byte       */
  entry[(uint32_t)k_rc_name_off + 1U]       = 1U;           /* high byte != 0 */
  uint8_t result                            = priv_exfat_name_chunk_eq(entry, "A", 0U, 1U);
  TEST_ASSERT_EQ(0U, result);
  TEST_END("exfat read cov: name_chunk_eq non-ASCII high byte (line 165)");
}

/**
 * @test test_name_chunk_eq_full_match
 * @brief Loop runs all 15 iterations without early exit (line 171).
 *
 * @details Fills all 15 UTF-16 units with 'A' (high byte zero) and searches
 *          for a 15-char path "AAAAAAAAAAAAAAA"; every char matches and the
 *          loop exhausts all k_rc_name_per_ent iterations before returning 1.
 *
 * @par MC/DC:
 * Decision: `if ((pos+i) >= nlen)` -- 1 cond.
 * V3: pos=0, i in 0..14, nlen=15 -> always false (this test, line 171 reached).
 * V4: (pos+i) >= nlen -> true -> line 161 early return (short-name tests).
 *
 * @pre None.
 * @post priv_exfat_name_chunk_eq returns 1.
 *
 * @since 0.1.0
 */
static void test_name_chunk_eq_full_match(void)
{
  TEST_BEGIN("exfat read cov: name_chunk_eq full 15-char match (line 171)");
  uint8_t entry[(uint32_t)k_rc_entry_bytes] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_rc_name_per_ent; i++) {
    entry[(uint32_t)k_rc_name_off + (i * 2U)] = (uint8_t)'A'; /* low byte */
    /* high byte stays 0 from zero-init */
  }
  uint8_t result =
    priv_exfat_name_chunk_eq(entry, "AAAAAAAAAAAAAAA", 0U, (uint32_t)k_rc_name_per_ent);
  TEST_ASSERT_EQ(1U, result);
  TEST_END("exfat read cov: name_chunk_eq full 15-char match (line 171)");
}

/**
 * @test test_match_set_stream_io_fail
 * @brief priv_exfat_match_set stream-entry read failure propagates (lines 208, 266).
 *
 * @details Creates a file at root entry 3; countdown == 4 lets the four
 *          directory entries before the file (bitmap, upcase, label, file)
 *          succeed, then fails on the stream-entry read inside match_set.
 *          match_set returns the I/O error (line 208); priv_exfat_find sees
 *          e != k_ra8_err_not_found and returns it (line 266).
 *
 * @par MC/DC:
 * Decision A (priv_exfat_match_set line 207): `if (e != k_ra8_ok)` -- 1 cond.
 * V1: stream read fails -> true -> line 208 (this test).
 * V2: stream read ok -> false -> check type (other tests).
 *
 * Decision B (priv_exfat_find line 265): `if (e != k_ra8_err_not_found)` -- 1 cond.
 * V3: e = io-error -> true -> line 266 (this test).
 * V4: e = not_found -> false -> continue loop (wrong-type tests).
 *
 * @pre None.
 * @post ra8_fs_open returns != k_ra8_ok.
 *
 * @since 0.1.0
 */
static void test_match_set_stream_io_fail(void)
{
  TEST_BEGIN("exfat read cov: match_set stream I/O fail (lines 208, 266)");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  const uint8_t data = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", &data, 1U));

  /* 4 reads succeed (entries 0-3), 5th (stream entry) fails. */
  s_rd_remaining   = (int32_t)k_rc_rd_at_4;
  ra8_fs_file_t* f = nullptr;
  ra8_err_t      e = ra8_fs_open(h, "A.TXT", k_ra8_fs_mode_read, &f);
  TEST_ASSERT(e != k_ra8_ok);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat read cov: match_set stream I/O fail (lines 208, 266)");
}

/**
 * @test test_match_set_wrong_stream_type
 * @brief Wrong stream entry type causes not_found (line 211).
 *
 * @details Patches the stream entry (root index 4) to type 0x42 after
 *          creating the file; priv_exfat_match_set reads the wrong type and
 *          returns k_ra8_err_not_found at line 211.
 *
 * @par MC/DC:
 * Decision: `if (strm[0] != k_exfat_entry_stream)` -- 1 cond.
 * V1: strm[0] = 0x42 -> true -> line 211, not_found (this test).
 * V2: strm[0] = 0xC0 -> false -> check nlen (success-path tests).
 *
 * @pre None.
 * @post ra8_fs_open returns k_ra8_err_not_found.
 *
 * @since 0.1.0
 */
static void test_match_set_wrong_stream_type(void)
{
  TEST_BEGIN("exfat read cov: match_set wrong stream type (line 211)");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  const uint8_t data = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", &data, 1U));
  /* Corrupt the stream entry type byte. */
  s_disk.bytes[root_entry_off(h, (uint32_t)k_rc_root_strm_idx)] = (uint8_t)k_rc_type_bogus;

  s_rd_remaining   = (int32_t)k_rc_rd_never;
  ra8_fs_file_t* f = nullptr;
  ra8_err_t      e = ra8_fs_open(h, "A.TXT", k_ra8_fs_mode_read, &f);
  TEST_ASSERT_EQ(k_ra8_err_not_found, e);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat read cov: match_set wrong stream type (line 211)");
}

/**
 * @test test_match_set_name_io_fail
 * @brief priv_exfat_match_set name-entry read failure propagates (lines 221, 266).
 *
 * @details Creates a file; countdown == 5 lets entries 0-4 (bitmap, upcase,
 *          label, file, stream) succeed, then fails on the name-entry read.
 *          match_set returns the I/O error (line 221); priv_exfat_find
 *          propagates it (line 266).
 *
 * @par MC/DC:
 * Decision: `if (e != k_ra8_ok)` after name priv_exfat_next_entry -- 1 cond.
 * V1: name read fails -> true -> line 221 (this test).
 * V2: name read ok -> false -> check type (wrong-name-type test).
 *
 * @pre None.
 * @post ra8_fs_open returns != k_ra8_ok.
 *
 * @since 0.1.0
 */
static void test_match_set_name_io_fail(void)
{
  TEST_BEGIN("exfat read cov: match_set name I/O fail (lines 221, 266)");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  const uint8_t data = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", &data, 1U));

  /* 5 reads succeed (entries 0-4), 6th (name entry) fails. */
  s_rd_remaining   = (int32_t)k_rc_rd_at_5;
  ra8_fs_file_t* f = nullptr;
  ra8_err_t      e = ra8_fs_open(h, "A.TXT", k_ra8_fs_mode_read, &f);
  TEST_ASSERT(e != k_ra8_ok);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat read cov: match_set name I/O fail (lines 221, 266)");
}

/**
 * @test test_match_set_wrong_name_type
 * @brief Wrong name entry type causes not_found (line 224).
 *
 * @details Patches the name entry (root index 5) to type 0x42 after creating
 *          the file; the stream entry is intact so match_set reads it and
 *          proceeds, then sees nm[0] != 0xC1 and returns k_ra8_err_not_found
 *          at line 224.
 *
 * @par MC/DC:
 * Decision: `if (nm[0] != k_exfat_entry_name)` -- 1 cond.
 * V1: nm[0] = 0x42 -> true -> line 224, not_found (this test).
 * V2: nm[0] = 0xC1 -> false -> chunk_eq (success-path tests).
 *
 * @pre None.
 * @post ra8_fs_open returns k_ra8_err_not_found.
 *
 * @since 0.1.0
 */
static void test_match_set_wrong_name_type(void)
{
  TEST_BEGIN("exfat read cov: match_set wrong name type (line 224)");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  const uint8_t data = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", &data, 1U));
  /* Corrupt the name entry type byte only; leave stream intact. */
  s_disk.bytes[root_entry_off(h, (uint32_t)k_rc_root_name_idx)] = (uint8_t)k_rc_type_bogus;

  s_rd_remaining   = (int32_t)k_rc_rd_never;
  ra8_fs_file_t* f = nullptr;
  ra8_err_t      e = ra8_fs_open(h, "A.TXT", k_ra8_fs_mode_read, &f);
  TEST_ASSERT_EQ(k_ra8_err_not_found, e);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat read cov: match_set wrong name type (line 224)");
}

/**
 * @test test_find_first_read_fail
 * @brief priv_exfat_find first entry read failure propagates (line 253).
 *
 * @details Countdown == 0 makes the very first priv_exfat_next_entry call
 *          inside priv_exfat_find fail, hitting the `return e;` at line 253.
 *
 * @par MC/DC:
 * Decision (priv_exfat_find line 252): `if (e != k_ra8_ok)` -- 1 cond.
 * V1: first read fails -> true -> line 253 (this test).
 * V2: first read ok -> false -> check entry type (all other find tests).
 *
 * @pre Volume is formatted and mounted.
 * @post ra8_fs_open returns != k_ra8_ok.
 *
 * @since 0.1.0
 */
static void test_find_first_read_fail(void)
{
  TEST_BEGIN("exfat read cov: find first read fail (line 253)");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  /* No file needed; the first directory read itself fails. */
  s_rd_remaining   = (int32_t)k_rc_rd_at_0;
  ra8_fs_file_t* f = nullptr;
  ra8_err_t      e = ra8_fs_open(h, "X.TXT", k_ra8_fs_mode_read, &f);
  TEST_ASSERT(e != k_ra8_ok);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat read cov: find first read fail (line 253)");
}

/**
 * @test test_open_write_mode_rejected
 * @brief priv_exfat_open rejects write mode (line 277).
 *
 * @details ra8_fs_open with k_ra8_fs_mode_write on an exFAT mount dispatches to
 *          priv_exfat_open, which returns k_ra8_err_not_supported at line 277.
 *
 * @par MC/DC:
 * Decision: `if (mode != k_ra8_fs_mode_read)` -- 1 cond.
 * V1: mode = write -> true -> line 277, not_supported (this test).
 * V2: mode = read -> false -> priv_exfat_find (success-path tests).
 *
 * @pre Volume is formatted and mounted.
 * @post ra8_fs_open returns k_ra8_err_not_supported.
 *
 * @since 0.1.0
 */
static void test_open_write_mode_rejected(void)
{
  TEST_BEGIN("exfat read cov: open write mode rejected (line 277)");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  s_rd_remaining   = (int32_t)k_rc_rd_never;
  ra8_fs_file_t* f = nullptr;
  ra8_err_t      e = ra8_fs_open(h, "X.TXT", k_ra8_fs_mode_write, &f);
  TEST_ASSERT_EQ(k_ra8_err_not_supported, e);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat read cov: open write mode rejected (line 277)");
}

/**
 * @test test_open_file_table_full
 * @brief priv_exfat_open returns no_mem when the file table is full (line 288).
 *
 * @details Opens the same file four times to fill all k_rc_max_files slots,
 *          then opens it a fifth time to trigger the nullptr path at line 287-288.
 *
 * @par MC/DC:
 * Decision: `if (f == nullptr)` -- 1 cond.
 * V1: priv_alloc_file_slot() returns nullptr -> true -> line 288 (this test).
 * V2: priv_alloc_file_slot() returns valid -> false -> populate handle (other tests).
 *
 * @pre Volume is formatted; no other files are open.
 * @post The 5th ra8_fs_open returns k_ra8_err_no_mem.
 *
 * @since 0.1.0
 */
static void test_open_file_table_full(void)
{
  TEST_BEGIN("exfat read cov: file table full -> no_mem (line 288)");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ctrl_backend, &h));

  const uint8_t data = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "F.TXT", &data, 1U));

  s_rd_remaining = (int32_t)k_rc_rd_never;

  /* Fill all four file slots. */
  ra8_fs_file_t* slots[(uint32_t)k_rc_max_files] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_rc_max_files; i++) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "F.TXT", k_ra8_fs_mode_read, &slots[i]));
  }

  /* Fifth open must fail with no_mem. */
  ra8_fs_file_t* extra = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_open(h, "F.TXT", k_ra8_fs_mode_read, &extra));

  /* Release all open slots before unmount. */
  for (uint32_t i = 0U; i < (uint32_t)k_rc_max_files; i++) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(slots[i]));
  }

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat read cov: file table full -> no_mem (line 288)");
}

/* ---- entry point ----------------------------------------------------------- */

/**
 * @brief Test executable entry point.
 *
 * @return 0 on success, 1 if any assertion failed.
 * @retval 0 All tests passed.
 * @retval 1 At least one assertion failed.
 *
 * @pre None.
 * @post All allocated resources freed; s_disk.bytes is nullptr.
 *
 * @since 0.1.0
 */
int main(void)
{
  test_ascii_upper_above_z();
  test_ascii_upper_a_to_z();
  test_parse_bad_bps_shift();
  test_next_entry_fat_read_fail();
  test_next_entry_eoc();
  test_next_entry_follow_chain();
  test_next_entry_sector_read_fail();
  test_name_chunk_eq_nonascii();
  test_name_chunk_eq_full_match();
  test_match_set_stream_io_fail();
  test_match_set_wrong_stream_type();
  test_match_set_name_io_fail();
  test_match_set_wrong_name_type();
  test_find_first_read_fail();
  test_open_write_mode_rejected();
  test_open_file_table_full();

  printf("[OK  ] test_ra8_fs_fat_exfat_read_cov.c\n");
  return 0;
}
