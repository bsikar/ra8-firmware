/**
 * @file test_ra8_fs_fat_name_cov.c
 * @brief Coverage booster for libs/ra8_fs/src/ra8_fs_fat_name.c.
 *
 * @details
 * Dedicated companion test executable that drives the branches in
 * ra8_fs_fat_name.c that no sibling test yet exercises:
 *
 *   - priv_path_to_83 null-pointer guard (line 123)
 *   - priv_path_to_83 leading-slash strip (line 126)
 *   - priv_path_to_83 kanji-escape encoding (lines 138-139)
 *   - priv_83_to_str kanji-escape restore (lines 157-158)
 *   - priv_83_to_str extension trailing-space break (line 170)
 *   - priv_dir_walk_next_sector fixed-root EOD (lines 244-246)
 *   - priv_dir_walk_next_sector fixed-root advance (lines 248-251)
 *   - priv_dir_walk_next_sector within-cluster else (line 275)
 *   - priv_dir_walk_next_sector cluster-chain FAT failure (lines 259, 318)
 *   - priv_dir_walk_next_sector cluster-chain EOC (lines 262-263, 316-317, 321)
 *   - priv_dir_walk_next_sector cluster-chain cycle guard (line 269)
 *   - priv_dir_find initial sector read failure (line 296)
 *
 * Two backends are used:
 *   - normal mem backend (heap byte array, unlimited I/O)
 *   - inject backend (reads_left countdown)
 *
 * The internal header ra8_fs_fat_internal.h is included to access dir_walk_t
 * for the synthetic walker calls (fixed-root and within-cluster cases) and
 * the cycle-guard case where count_of_clusters is temporarily zeroed.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_internal.h"
#include "unity_minimal.h"

/**
 * @enum ncov_fixture_t
 * @brief Name-handling fixture: 8.3 buffer sizes, FAT patch offsets, walker state.
 *
 * @details
 * The `k_ncov_walk_*` members are deliberately non-zero values parked in a
 * ::dir_walk_t before a single step, so a step that forgot to advance the LBA
 * or reset the entry index fails an assertion instead of coasting on a zero
 * that happened to be right.
 */
typedef enum : uint16_t {
  k_ncov_sfn_name_len = 11U, /**< A packed 8.3 name: 8 base + 3 extension.       */
  k_ncov_sfn_str_cap  = 13U, /**< Its printable form, "NAME.EXT" plus the NUL.   */
  k_ncov_files_per_sub =
    14U, /**< Files created in the subdirectory: enough for its entries to spill
              past one sector.                                                     */
  k_ncov_fat16_ent2_hi =
    5U, /**< High byte of FAT16 entry 2, which sits at offset 4 within FAT1.       */
  k_ncov_walk_entry_idx = 7U,   /**< Entry index the walker must reset to 0.        */
  k_ncov_walk_lba_sub   = 50U,  /**< Parked LBA for the subdirectory walk.          */
  k_ncov_walk_lba_root  = 100U, /**< Parked LBA for the fixed-root walk.            */
  k_ncov_fat1_byte_off =
    512U, /**< Byte offset of FAT1: it starts at LBA 1 of a 512-byte-sector volume. */
} ncov_fixture_t;

/* ===========================================================================
 * Geometry constants
 * ===========================================================================
 */

/**
 * @enum name_cov_geo_t
 * @brief Block-count and sentinel constants for this test's block devices.
 */
typedef enum : uint32_t {
  k_ncov_blk_sz     = 512U,        /**< Bytes per logical block.             */
  k_ncov_fat16_spc1 = 8U * 1024U,  /**< 4 MiB FAT16 volume, SPC=1.           */
  k_ncov_reads_inf  = 0xFFFFFFFFU, /**< Sentinel: unlimited reads in inject. */
} name_cov_geo_t;

/* ===========================================================================
 * Normal in-memory block device
 * ===========================================================================
 */

/**
 * @struct ncov_mem_disk_t
 * @brief Heap-backed sector store for the normal backend.
 */
typedef struct {
  uint8_t* bytes;       /**< Heap-allocated sector array. */
  uint32_t block_count; /**< Total 512-byte sectors.      */
  uint32_t byte_count;  /**< Total bytes (blocks * size). */
} ncov_mem_disk_t;

/**
 * @var s_ncov_disk
 * @brief Global singleton heap disk; freed by ncov_free_vol().
 * @warning Shared across tests; ncov_free_vol() must be called at test end.
 * @since 0.1.0
 */
static ncov_mem_disk_t s_ncov_disk = {};

/**
 * @brief Read sectors from the normal in-memory backend.
 *
 * @param[in]  ctx   Pointer to ncov_mem_disk_t.
 * @param[in]  lba   Starting sector.
 * @param[in]  count Sector count.
 * @param[out] buf   Destination buffer.
 * @return Error code.
 * @retval k_ra8_ok           Read succeeded.
 * @retval k_ra8_err_out_of_range LBA + count exceeds block_count.
 * @pre ctx is non-NULL and points to a mounted ncov_mem_disk_t.
 * @pre buf has room for count * 512 bytes.
 * @post buf contains the requested sectors.
 * @post No disk state modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t ncov_mem_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  ncov_mem_disk_t* d = (ncov_mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_ncov_blk_sz],
         (size_t)count * (uint32_t)k_ncov_blk_sz);
  return k_ra8_ok;
}

/**
 * @brief Write sectors to the normal in-memory backend.
 *
 * @param[in] ctx   Pointer to ncov_mem_disk_t.
 * @param[in] lba   Starting sector.
 * @param[in] count Sector count.
 * @param[in] buf   Source buffer.
 * @return Error code.
 * @retval k_ra8_ok           Write succeeded.
 * @retval k_ra8_err_out_of_range LBA + count exceeds block_count.
 * @pre ctx is non-NULL and points to a mounted ncov_mem_disk_t.
 * @pre buf holds at least count * 512 bytes.
 * @post Sectors are written into d->bytes.
 * @post No other state modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t ncov_mem_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  ncov_mem_disk_t* d = (ncov_mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_ncov_blk_sz],
         buf,
         (size_t)count * (uint32_t)k_ncov_blk_sz);
  return k_ra8_ok;
}

/**
 * @brief Query the capacity of the normal in-memory backend.
 *
 * @param[in]  ctx        Pointer to ncov_mem_disk_t.
 * @param[out] block_count Total sectors.
 * @param[out] block_size  Sector size in bytes.
 * @return k_ra8_ok always.
 * @retval k_ra8_ok Capacity fields populated.
 * @pre ctx, block_count, block_size are non-NULL.
 * @post *block_count and *block_size reflect d's geometry.
 * @post No state modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t ncov_mem_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  ncov_mem_disk_t* d = (ncov_mem_disk_t*)ctx;
  *block_count       = d->block_count;
  *block_size        = (uint32_t)k_ncov_blk_sz;
  return k_ra8_ok;
}

/** @brief Normal backend pointing at s_ncov_disk. */
static const ra8_fs_backend_t s_ncov_backend = {
  .read_block   = ncov_mem_read,
  .write_block  = ncov_mem_write,
  .get_capacity = ncov_mem_capacity,
  .ctx          = &s_ncov_disk,
};

/* ===========================================================================
 * Inject (read-countdown) backend
 * ===========================================================================
 */

/**
 * @struct ncov_inject_disk_t
 * @brief Backend with deterministic read-failure injection.
 *
 * @details Shares the same byte array as ncov_mem_disk_t. When reads_left
 *          reaches zero the next read returns k_ra8_err_hw_error. Writes
 *          always succeed (shared bytes are updated).
 */
typedef struct {
  uint8_t* bytes;       /**< Shared with s_ncov_disk.bytes.               */
  uint32_t block_count; /**< Same geometry as s_ncov_disk.                */
  uint32_t byte_count;  /**< Same geometry as s_ncov_disk.                */
  uint32_t reads_left;  /**< Reads allowed; k_ncov_reads_inf = unlimited. */
} ncov_inject_disk_t;

/**
 * @var s_ncov_inject
 * @brief Global inject backend state; populated by ncov_swap_to_inject().
 * @note Restore h->backend after use.
 * @since 0.1.0
 */
static ncov_inject_disk_t s_ncov_inject = {};

/**
 * @brief Read sectors from the inject backend (may fail on countdown).
 *
 * @param[in]  ctx   Pointer to ncov_inject_disk_t.
 * @param[in]  lba   Starting sector.
 * @param[in]  count Sector count.
 * @param[out] buf   Destination buffer.
 * @return Error code.
 * @retval k_ra8_ok           Sector read succeeded.
 * @retval k_ra8_err_hw_error reads_left reached zero.
 * @pre ctx is non-NULL and points to a mounted ncov_inject_disk_t.
 * @pre buf has room for count * 512 bytes.
 * @post reads_left decremented (unless at k_ncov_reads_inf or already zero).
 * @post On success, buf contains the requested sectors.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t ncov_inj_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  ncov_inject_disk_t* d = (ncov_inject_disk_t*)ctx;
  if (d->reads_left == 0U) {
    return k_ra8_err_hw_error;
  }
  if (d->reads_left != (uint32_t)k_ncov_reads_inf) {
    d->reads_left--;
  }
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_ncov_blk_sz],
         (size_t)count * (uint32_t)k_ncov_blk_sz);
  return k_ra8_ok;
}

/**
 * @brief Write sectors through the inject backend (always succeeds).
 *
 * @param[in] ctx   Pointer to ncov_inject_disk_t.
 * @param[in] lba   Starting sector.
 * @param[in] count Sector count.
 * @param[in] buf   Source buffer.
 * @return Error code.
 * @retval k_ra8_ok           Write succeeded.
 * @retval k_ra8_err_out_of_range LBA + count exceeds block_count.
 * @pre ctx is non-NULL and points to a mounted ncov_inject_disk_t.
 * @pre buf holds at least count * 512 bytes.
 * @post Sectors are written into d->bytes.
 * @post No other state modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t ncov_inj_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  ncov_inject_disk_t* d = (ncov_inject_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_ncov_blk_sz],
         buf,
         (size_t)count * (uint32_t)k_ncov_blk_sz);
  return k_ra8_ok;
}

/**
 * @brief Query the capacity of the inject backend.
 *
 * @param[in]  ctx        Pointer to ncov_inject_disk_t.
 * @param[out] block_count Total sectors.
 * @param[out] block_size  Sector size in bytes.
 * @return k_ra8_ok always.
 * @retval k_ra8_ok Capacity fields populated.
 * @pre ctx, block_count, block_size are non-NULL.
 * @post *block_count and *block_size reflect d's geometry.
 * @post No state modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t ncov_inj_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  ncov_inject_disk_t* d = (ncov_inject_disk_t*)ctx;
  *block_count          = d->block_count;
  *block_size           = (uint32_t)k_ncov_blk_sz;
  return k_ra8_ok;
}

/* ===========================================================================
 * Volume builder helpers
 * ===========================================================================
 */

/**
 * @brief Write a 16-bit little-endian value into a byte buffer.
 *
 * @param[out] p   Destination buffer.
 * @param[in]  off Byte offset within p.
 * @param[in]  v   Value to write.
 * @pre p is non-NULL and has at least off+2 bytes.
 * @post p[off] and p[off+1] hold the LE encoding of v.
 * @post No state outside p modified.
 * @note Inline helper; not thread-safe.
 * @since 0.1.0
 */
static void ncov_put16(uint8_t* p, uint32_t off, uint16_t v)
{
  p[off]     = (uint8_t)(v & k_byte_mask);
  p[off + 1] = (uint8_t)((v >> 8U) & k_byte_mask);
}

/**
 * @brief Allocate and hand-build a minimal FAT16 BPB in s_ncov_disk (SPC=1).
 *
 * @details 8192 sectors, 2 FATs of 32 sectors each, 16 root entries, SPC=1.
 *          Layout: BPB(1) FAT1(32) FAT2(32) root(1) data(8126).
 *          first_fat_lba=1, first_root_lba=65, first_data_lba=66.
 *
 * @pre s_ncov_disk.bytes is nullptr.
 * @post s_ncov_disk holds a calloc-zeroed 4 MiB image with a valid BPB.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void ncov_build_fat16_vol(void)
{
  if (s_ncov_disk.bytes != nullptr) {
    free(s_ncov_disk.bytes);
    s_ncov_disk.bytes = nullptr;
  }
  s_ncov_disk.byte_count  = (uint32_t)k_ncov_fat16_spc1 * (uint32_t)k_ncov_blk_sz;
  s_ncov_disk.bytes       = (uint8_t*)calloc(1, s_ncov_disk.byte_count);
  s_ncov_disk.block_count = (uint32_t)k_ncov_fat16_spc1;
  if (s_ncov_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
  uint8_t* bpb = s_ncov_disk.bytes;
  ncov_put16(bpb, k_bpb_off_bytes_per_sec, (uint16_t)k_ncov_blk_sz);
  bpb[k_bpb_off_sec_per_clus] = 1U;
  ncov_put16(bpb, k_bpb_off_rsvd_sec_cnt, 1U);
  bpb[16] = 2U;
  ncov_put16(bpb, k_bpb_off_root_ent_cnt, 16U);
  ncov_put16(bpb, k_bpb_off_tot_sec_16, (uint16_t)k_ncov_fat16_spc1);
  ncov_put16(bpb, k_bpb_off_fat_sz_16, 32U);
  bpb[k_bpb_off_signature_lo] = k_bpb_sig_lo;
  bpb[k_bpb_off_signature_hi] = k_bpb_sig_hi;
}

/**
 * @brief Release the heap-backed sector store in s_ncov_disk.
 *
 * @pre None.
 * @post s_ncov_disk.bytes is nullptr.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void ncov_free_vol(void)
{
  if (s_ncov_disk.bytes != nullptr) {
    free(s_ncov_disk.bytes);
    s_ncov_disk.bytes = nullptr;
  }
}

/**
 * @brief Redirect h->backend to the inject (read-countdown) backend.
 *
 * @details Shares s_ncov_disk.bytes with s_ncov_inject; the caller saves
 *          and restores h->backend when done.
 *
 * @param[in,out] h          Mounted volume to redirect.
 * @param[in]     reads_left Reads allowed before failure.
 *
 * @pre h is non-NULL and mounted; s_ncov_disk.bytes is non-NULL.
 * @post h->backend uses ncov_inj_read / ncov_inj_write callbacks.
 *
 * @note Caller must restore h->backend = saved after use.
 * @since 0.1.0
 */
static void ncov_swap_to_inject(ra8_fs_mount_t* h, uint32_t reads_left)
{
  s_ncov_inject.bytes       = s_ncov_disk.bytes;
  s_ncov_inject.block_count = s_ncov_disk.block_count;
  s_ncov_inject.byte_count  = s_ncov_disk.byte_count;
  s_ncov_inject.reads_left  = reads_left;
  h->backend.read_block     = ncov_inj_read;
  h->backend.write_block    = ncov_inj_write;
  h->backend.get_capacity   = ncov_inj_capacity;
  h->backend.ctx            = &s_ncov_inject;
}

/**
 * @brief Create count empty files inside /SUB on the mounted volume.
 *
 * @details Each file is opened (write mode) then immediately closed with
 *          no data written. Names are "/SUB/G00.TXT" through "/SUB/Gnn.TXT".
 *
 * @param[in,out] h     Mounted FAT16 volume.
 * @param[in]     count Number of files to create (0..14).
 *
 * @pre h is non-NULL and mounted; /SUB directory already exists.
 * @pre count <= 14 (. and .. already occupy 2 of the 16 slots).
 * @post count new 8.3 dir entries exist inside /SUB.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void ncov_create_files_in_sub(ra8_fs_mount_t* h, uint32_t count)
{
  for (uint32_t i = 0; i < count; i++) {
    char name[32] = {};
    (void)snprintf(name, sizeof(name), "/SUB/G%02u.TXT", i);
    ra8_fs_file_t* f = nullptr;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, name, k_ra8_fs_mode_write, &f));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  }
}

/* ===========================================================================
 * Tests: priv_path_to_83 guards
 * ===========================================================================
 */

/**
 * @test test_ncov_path_to_83_null_guards
 * @brief priv_path_to_83 returns 0 when path or out11 is null (line 123).
 *
 * @details Passes nullptr for path and nullptr for out11 in separate calls;
 *          both must return 0 immediately via the guard at line 122-123.
 *
 * @par MC/DC:
 * Decision: `if (path == nullptr || out11 == nullptr)` (2 conditions).
 * V1: path=nullptr, out11=valid  -> true  (path null independently true).
 * V2: path=valid,   out11=nullptr -> true  (out11 null independently true).
 * Sibling tests supply the all-false control vector (normal path).
 *
 * @pre None.
 * @post Returns 0 for each null argument.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_ncov_path_to_83_null_guards(void)
{
  TEST_BEGIN("priv_path_to_83: null-pointer guards (line 123)");
  uint8_t buf[k_ncov_sfn_name_len] = {};
  TEST_ASSERT_EQ(0, priv_path_to_83(nullptr, buf));
  TEST_ASSERT_EQ(0, priv_path_to_83("FILE.TXT", nullptr));
  TEST_END("priv_path_to_83: null-pointer guards (line 123)");
}

/**
 * @test test_ncov_path_to_83_leading_slash
 * @brief priv_path_to_83 strips one or more leading slashes (line 126).
 *
 * @details Passes "/FILE.TXT" and verifies the packed result equals that
 *          of "FILE.TXT". The while loop at line 125 must execute at least
 *          once, covering the path++ at line 126.
 *
 * @par MC/DC:
 * Decision: `while (*path == '/')` (1 condition).
 * V1: *path='/'  -> true  -> line 126 executed (path incremented).
 * V2: *path!='/' -> false -> loop exits (after the increment in V1).
 * Both arms exercised sequentially.
 *
 * @pre None.
 * @post Packed result for "/FILE.TXT" equals that for "FILE.TXT".
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_ncov_path_to_83_leading_slash(void)
{
  TEST_BEGIN("priv_path_to_83: leading-slash strip (line 126)");
  uint8_t ref11[k_ncov_sfn_name_len] = {};
  uint8_t out11[k_ncov_sfn_name_len] = {};
  TEST_ASSERT_EQ(1, priv_path_to_83("FILE.TXT", ref11));
  TEST_ASSERT_EQ(1, priv_path_to_83("/FILE.TXT", out11));
  for (uint32_t i = 0; i < k_ncov_sfn_name_len; i++) {
    TEST_ASSERT_EQ(ref11[i], out11[i]);
  }
  TEST_END("priv_path_to_83: leading-slash strip (line 126)");
}

/**
 * @test test_ncov_path_to_83_kanji_escape
 * @brief priv_path_to_83 replaces 0xE5 first byte with 0x05 (lines 138-139).
 *
 * @details Passes a path whose first byte is 0xE5 (k_dir_marker_free_used).
 *          After packing the base, out11[0] == 0xE5 triggers the guard at
 *          line 137-138 and is replaced with k_dir_marker_kanji_e5 (0x05).
 *
 * @par MC/DC:
 * Decision: `if (out11[0] == k_dir_marker_free_used)` (1 condition).
 * V1: out11[0]=0xE5 -> true -> line 138 (replacement) executed.
 * The false arm (normal name) is covered by sibling tests.
 *
 * @pre None.
 * @post out11[0] == k_dir_marker_kanji_e5 (0x05).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_ncov_path_to_83_kanji_escape(void)
{
  TEST_BEGIN("priv_path_to_83: kanji-escape replacement (lines 138-139)");
  uint8_t out11[k_ncov_sfn_name_len] = {};
  /* '\xe5' is byte 0xE5 -- the k_dir_marker_free_used value. */
  TEST_ASSERT_EQ(1, priv_path_to_83("\xe5NAME.TXT", out11));
  TEST_ASSERT_EQ(k_dir_marker_kanji_e5, out11[0]);
  TEST_END("priv_path_to_83: kanji-escape replacement (lines 138-139)");
}

/* ===========================================================================
 * Tests: priv_83_to_str guards
 * ===========================================================================
 */

/**
 * @test test_ncov_83_to_str_kanji_restore
 * @brief priv_83_to_str restores 0x05 first byte to 0xE5 (lines 157-158).
 *
 * @details Constructs an in11 buffer with in11[0] = k_dir_marker_kanji_e5
 *          (0x05) and the rest set to spaces. The base copy loop emits the
 *          0x05 byte into out12[0], then the three-way AND condition at
 *          lines 156-158 (j>0, out12[0]==0x05, in11[0]==0x05) fires, setting
 *          out12[0] back to k_dir_marker_free_used (0xE5).
 *
 * @par MC/DC:
 * Decision: three-condition AND at line 156 (mcdc-deactivated in source;
 * this test covers the true arm for line coverage).
 * V1: all three conditions true -> out12[0] = 0xE5 (lines 157-158 hit).
 *
 * @pre None.
 * @post (uint8_t)out12[0] == k_dir_marker_free_used (0xE5).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_ncov_83_to_str_kanji_restore(void)
{
  TEST_BEGIN("priv_83_to_str: kanji-escape restore (lines 157-158)");
  uint8_t in11[k_ncov_sfn_name_len] = {};
  in11[0]                           = k_dir_marker_kanji_e5;
  for (uint32_t i = 1U; i < k_ncov_sfn_name_len; i++) {
    in11[i] = ' ';
  }
  char out12[k_ncov_sfn_str_cap] = {};
  priv_83_to_str(in11, out12);
  /* out12[0] is a signed char; read the restored 0xE5 marker in the
   * unsigned byte domain so it compares as 229, not -27. */
  const uint8_t out12_marker = (uint8_t)out12[0];
  TEST_ASSERT_EQ(k_dir_marker_free_used, out12_marker);
  TEST_END("priv_83_to_str: kanji-escape restore (lines 157-158)");
}

/**
 * @test test_ncov_83_to_str_ext_trailing_space
 * @brief priv_83_to_str breaks early on a trailing space in the extension (line 170).
 *
 * @details Constructs an in11 with a two-character extension "TX " (trailing
 *          space). The extension copy loop processes 'T', then 'X', then
 *          encounters the space at in11[10] and breaks at line 170.
 *          The resulting string must be "AA.TX" (no trailing space).
 *
 * @par MC/DC:
 * Decision: `if (in11[k_filename_base_len + i] == ' ')` inside ext loop (1 condition).
 * V1: byte=space  -> true  -> break at line 170 (this test).
 * V2: byte!= space -> false -> character copied (covered for 'T' and 'X' earlier).
 *
 * @pre None.
 * @post strcmp(out12, "AA.TX") == 0.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_ncov_83_to_str_ext_trailing_space(void)
{
  TEST_BEGIN("priv_83_to_str: extension trailing-space break (line 170)");
  /* "AA      TX " packed: base='A','A',' '...' '; ext='T','X',' ' */
  const uint8_t in11[11]                  = {'A', 'A', ' ', ' ', ' ', ' ', ' ', ' ', 'T', 'X', ' '};
  char          out12[k_ncov_sfn_str_cap] = {};
  priv_83_to_str(in11, out12);
  TEST_ASSERT_EQ(0, strcmp(out12, "AA.TX"));
  TEST_END("priv_83_to_str: extension trailing-space break (line 170)");
}

/* ===========================================================================
 * Tests: priv_dir_walk_next_sector (fixed-root and within-cluster)
 * ===========================================================================
 */

/**
 * @test test_ncov_walk_fixed_root_eod
 * @brief priv_dir_walk_next_sector sets eod=1 when the fixed root is exhausted
 *        (lines 244-246).
 *
 * @details Directly constructs a dir_walk_t with is_root_fixed=1 and
 *          fixed_remaining=1 (the last remaining sector). The backend is never
 *          accessed in the fixed-root path, so a zero-initialized mount suffices.
 *          The condition at line 244 is true, setting eod=1 and returning k_ra8_ok.
 *
 * @par MC/DC:
 * Decision: `if (w->fixed_remaining <= 1U)` at line 244 (1 condition).
 * V1: fixed_remaining=1 <= 1 -> true -> lines 245-246 (this test).
 * V2: fixed_remaining=3 > 1  -> false -> lines 248-251 (next test).
 *
 * @pre None.
 * @post eod == 1 and return value is k_ra8_ok.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_ncov_walk_fixed_root_eod(void)
{
  TEST_BEGIN("priv_dir_walk_next_sector: fixed-root EOD (lines 244-246)");
  ra8_fs_mount_t m = {};
  dir_walk_t     w = {};
  uint8_t        eod;
  w.is_root_fixed   = 1;
  w.fixed_remaining = 1;
  w.cur_lba         = k_ncov_walk_lba_root;
  eod               = 0;
  TEST_ASSERT_EQ(k_ra8_ok, priv_dir_walk_next_sector(&m, &w, &eod));
  TEST_ASSERT_EQ(1, eod);
  TEST_END("priv_dir_walk_next_sector: fixed-root EOD (lines 244-246)");
}

/**
 * @test test_ncov_walk_fixed_root_advance
 * @brief priv_dir_walk_next_sector advances the LBA when more root sectors remain
 *        (lines 248-251).
 *
 * @details Directly constructs a dir_walk_t with is_root_fixed=1 and
 *          fixed_remaining=3 (> 1). The condition at line 244 is false;
 *          lines 248-251 decrement fixed_remaining, increment cur_lba,
 *          and reset entry_idx.
 *
 * @par MC/DC:
 * See test_ncov_walk_fixed_root_eod for the shared decision.
 *
 * @pre None.
 * @post eod == 0, fixed_remaining decremented, cur_lba incremented, entry_idx == 0.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_ncov_walk_fixed_root_advance(void)
{
  TEST_BEGIN("priv_dir_walk_next_sector: fixed-root advance (lines 248-251)");
  ra8_fs_mount_t m = {};
  dir_walk_t     w = {};
  uint8_t        eod;
  w.is_root_fixed   = 1;
  w.fixed_remaining = 3;
  w.cur_lba         = k_ncov_walk_lba_root;
  w.entry_idx       = k_ncov_walk_entry_idx;
  eod               = 0;
  TEST_ASSERT_EQ(k_ra8_ok, priv_dir_walk_next_sector(&m, &w, &eod));
  TEST_ASSERT_EQ(0, eod);
  TEST_ASSERT_EQ(2, w.fixed_remaining);
  TEST_ASSERT_EQ(101, w.cur_lba);
  TEST_ASSERT_EQ(0, w.entry_idx);
  TEST_END("priv_dir_walk_next_sector: fixed-root advance (lines 248-251)");
}

/**
 * @test test_ncov_walk_within_cluster
 * @brief priv_dir_walk_next_sector increments cur_lba without a FAT read when
 *        the sector is still within the current cluster (line 275).
 *
 * @details Directly constructs a dir_walk_t with is_root_fixed=0,
 *          sector_in_cluster=0, and a mount with sectors_per_cluster=2.
 *          After sector_in_cluster is incremented to 1 (still < 2), the
 *          else branch at line 275 is taken, incrementing cur_lba.
 *          No backend reads occur.
 *
 * @par MC/DC:
 * Decision: `if (w->sector_in_cluster >= m->sectors_per_cluster)` at line 255
 *           (1 condition).
 * V1: sector_in_cluster=1 >= SPC=2 -> false -> line 275 (this test).
 * V2: sector_in_cluster=1 >= SPC=1 -> true  -> FAT read path (covered by
 *     tests that mount a real FAT16 volume with SPC=1).
 *
 * @pre None.
 * @post eod == 0, sector_in_cluster == 1, cur_lba incremented, entry_idx == 0.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_ncov_walk_within_cluster(void)
{
  TEST_BEGIN("priv_dir_walk_next_sector: within-cluster else (line 275)");
  ra8_fs_mount_t m      = {};
  m.sectors_per_cluster = 2;
  dir_walk_t w          = {};
  uint8_t    eod;
  w.is_root_fixed     = 0;
  w.sector_in_cluster = 0;
  w.cur_lba           = k_ncov_walk_lba_sub;
  w.entry_idx         = k_ncov_walk_entry_idx;
  eod                 = 0;
  TEST_ASSERT_EQ(k_ra8_ok, priv_dir_walk_next_sector(&m, &w, &eod));
  TEST_ASSERT_EQ(0, eod);
  TEST_ASSERT_EQ(1, w.sector_in_cluster);
  TEST_ASSERT_EQ(51, w.cur_lba);
  TEST_ASSERT_EQ(0, w.entry_idx);
  TEST_END("priv_dir_walk_next_sector: within-cluster else (line 275)");
}

/* ===========================================================================
 * Tests: priv_dir_walk_next_sector and priv_dir_find (cluster-chain paths)
 * ===========================================================================
 */

/**
 * @test test_ncov_walk_cluster_chain_eoc
 * @brief priv_dir_walk_next_sector follows the FAT chain to EOC in a full
 *        subdirectory, causing priv_dir_find to return not_found (lines 262-263,
 *        316-317, 321).
 *
 * @details Mounts a FAT16 volume, creates /SUB, then creates 14 empty files
 *          in /SUB so all 16 directory slots are occupied (. + .. + 14 files).
 *          Opening a non-existent file via the public API triggers priv_dir_find
 *          in SUB: all 16 entries are scanned with no match and no 0x00 marker,
 *          so priv_dir_walk_next_sector is called (line 316). The FAT entry for
 *          SUB's cluster is EOC (set by ra8_fs_mkdir), so eod is set to 1
 *          (lines 262-263) and priv_dir_find returns k_ra8_err_not_found (line 321).
 *
 * @par MC/DC:
 * Decision: `if (priv_is_eoc(m, next) != 0U)` at line 261 (1 condition).
 * V1: next=EOC   -> true  -> lines 262-263 (this test).
 * V2: next!=EOC  -> false -> follow chain (covered by cycle-guard test).
 *
 * Decision: `if (err != k_ra8_ok)` at line 317 (1 condition).
 * V1: err=k_ra8_ok -> false -> continue while (this test).
 * V2: err!=k_ra8_ok -> true -> return (FAT-failure test).
 *
 * @pre None (volume built internally).
 * @post ra8_fs_open returns k_ra8_err_not_found.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_ncov_walk_cluster_chain_eoc(void)
{
  TEST_BEGIN("priv_dir_find: cluster-chain EOC -> not_found (lines 262-263, 316-317, 321)");
  ncov_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ncov_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SUB"));
  /* 14 files + . + .. = 16 entries, filling the single-sector cluster */
  ncov_create_files_in_sub(h, k_ncov_files_per_sub);
  ra8_fs_file_t* f = nullptr;
  /* 8.3 and LFN searches both exhaust SUB, hitting lines 262-263 then 321 */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_open(h, "/SUB/NOTHERE.TXT", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  ncov_free_vol();
  TEST_END("priv_dir_find: cluster-chain EOC -> not_found (lines 262-263, 316-317, 321)");
}

/**
 * @test test_ncov_walk_fat_read_fail
 * @brief priv_dir_walk_next_sector propagates a FAT read failure (lines 259, 318).
 *
 * @details Same volume and subdirectory setup as test_ncov_walk_cluster_chain_eoc.
 *          After mounting and filling SUB, the backend is replaced with the inject
 *          backend configured to allow exactly 2 reads:
 *            - Read 1: root directory sector (to locate /SUB).
 *            - Read 2: SUB directory sector (16 entries scanned, no match).
 *            - Read 3: FAT sector inside priv_dir_walk_next_sector -> FAILS.
 *          priv_fat_get returns k_ra8_err_hw_error (line 259); priv_dir_find
 *          propagates it at line 318.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at line 258 (1 condition).
 * V1: err=hw_error != k_ra8_ok -> true -> line 259 returned (this test).
 * V2: err=k_ra8_ok             -> false -> follow chain (cycle-guard test).
 *
 * Decision: `if (err != k_ra8_ok)` at line 317 (1 condition).
 * V1: err!=k_ra8_ok -> true -> line 318 (this test).
 * V2: err=k_ra8_ok  -> false (EOC test above).
 *
 * @pre None (volume built internally).
 * @post ra8_fs_open returns k_ra8_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_ncov_walk_fat_read_fail(void)
{
  TEST_BEGIN("priv_dir_walk_next_sector: FAT read failure (lines 259, 318)");
  ncov_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ncov_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SUB"));
  ncov_create_files_in_sub(h, k_ncov_files_per_sub);
  /* reads_left=2: root-dir read and SUB-dir read succeed;
     the FAT read for the cluster chain (read 3) fails. */
  ra8_fs_backend_t saved = h->backend;
  ncov_swap_to_inject(h, 2U);
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_open(h, "/SUB/NOTHERE.TXT", k_ra8_fs_mode_read, &f));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  ncov_free_vol();
  TEST_END("priv_dir_walk_next_sector: FAT read failure (lines 259, 318)");
}

/**
 * @test test_ncov_walk_cycle_guard
 * @brief priv_dir_walk_next_sector returns k_ra8_err_protocol_error when
 *        cluster_hops exceeds count_of_clusters (line 269).
 *
 * @details Builds a FAT16 volume and writes FAT[2] = 3 directly into
 *          s_ncov_disk.bytes (byte offset 512+4 = FAT1 sector, cluster-2
 *          entry). After mounting, count_of_clusters is temporarily set to 0
 *          so any non-zero hop count exceeds it. A synthetic dir_walk_t is
 *          constructed with is_root_fixed=0, cluster=2, sector_in_cluster=0.
 *          priv_dir_walk_next_sector increments sector_in_cluster to 1 (== SPC),
 *          reads FAT[2] = 3 (not EOC for FAT16), increments cluster_hops to 1,
 *          and finds 1 > 0 (count_of_clusters) -> returns k_ra8_err_protocol_error
 *          at line 269.
 *
 * @par MC/DC:
 * Decision: `if (w->cluster_hops > m->count_of_clusters)` at line 268 (1 condition).
 * V1: cluster_hops=1 > count_of_clusters=0 -> true -> line 269 (this test).
 * V2: cluster_hops=0 > count_of_clusters=N -> false (never triggered in normal walks).
 *
 * @pre None (volume built internally).
 * @post priv_dir_walk_next_sector returns k_ra8_err_protocol_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_ncov_walk_cycle_guard(void)
{
  TEST_BEGIN("priv_dir_walk_next_sector: cycle guard (line 269)");
  ncov_build_fat16_vol();
  /* FAT1 is at LBA 1 = byte offset 512 in s_ncov_disk.bytes.
     FAT16 entry 2 is at byte offset 4 within FAT1 (cluster * 2 = 4). */
  s_ncov_disk.bytes[k_ncov_fat1_byte_off + 4U]                   = 0x03U;
  s_ncov_disk.bytes[k_ncov_fat1_byte_off + k_ncov_fat16_ent2_hi] = 0x00U;
  ra8_fs_mount_t* h                                              = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ncov_backend, &h));
  uint32_t saved_coc   = h->count_of_clusters;
  h->count_of_clusters = 0U;
  dir_walk_t w         = {};
  w.is_root_fixed      = 0;
  w.cluster            = 2;
  w.sector_in_cluster  = 0;
  w.cluster_hops       = 0;
  uint8_t eod          = 0;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, priv_dir_walk_next_sector(h, &w, &eod));
  h->count_of_clusters = saved_coc;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  ncov_free_vol();
  TEST_END("priv_dir_walk_next_sector: cycle guard (line 269)");
}

/**
 * @test test_ncov_dir_find_initial_read_fail
 * @brief priv_dir_find returns the sector-read error immediately (line 296).
 *
 * @details Mounts a fresh FAT16 volume, then replaces the backend with the
 *          inject backend configured for 0 reads. The first ra8_fs_open triggers
 *          priv_dir_find in the root; priv_read_sector fails immediately at
 *          line 294, and priv_dir_find returns the error at line 296.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at line 294 (1 condition).
 * V1: err=hw_error != k_ra8_ok -> true -> line 296 returned (this test).
 * V2: err=k_ra8_ok             -> false -> scan entries (normal path).
 *
 * @pre None (volume built internally).
 * @post ra8_fs_open returns k_ra8_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_ncov_dir_find_initial_read_fail(void)
{
  TEST_BEGIN("priv_dir_find: initial read failure (line 296)");
  ncov_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_ncov_backend, &h));
  ra8_fs_backend_t saved = h->backend;
  /* reads_left=0: the first priv_read_sector call inside priv_dir_find fails */
  ncov_swap_to_inject(h, 0U);
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_open(h, "/ANYFILE.TXT", k_ra8_fs_mode_read, &f));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  ncov_free_vol();
  TEST_END("priv_dir_find: initial read failure (line 296)");
}

/* ===========================================================================
 * Test entry point
 * ===========================================================================
 */

/**
 * @brief Test executable entry point.
 *
 * @details Runs all coverage-booster tests for ra8_fs_fat_name.c in sequence.
 *          Each test prints [RUN] and [PASS] banners via the unity_minimal
 *          macros; any assertion failure calls exit(1).
 *
 * @return 0 on success.
 *
 * @pre None.
 * @post All targeted lines in ra8_fs_fat_name.c have been executed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_ncov_path_to_83_null_guards();
  test_ncov_path_to_83_leading_slash();
  test_ncov_path_to_83_kanji_escape();
  test_ncov_83_to_str_kanji_restore();
  test_ncov_83_to_str_ext_trailing_space();
  test_ncov_walk_fixed_root_eod();
  test_ncov_walk_fixed_root_advance();
  test_ncov_walk_within_cluster();
  test_ncov_walk_cluster_chain_eoc();
  test_ncov_walk_fat_read_fail();
  test_ncov_walk_cycle_guard();
  test_ncov_dir_find_initial_read_fail();
  return 0;
}
