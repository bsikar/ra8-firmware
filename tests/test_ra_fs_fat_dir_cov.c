/**
 * @file test_ra_fs_fat_dir_cov.c
 * @brief Coverage booster for libs/ra_fs/src/ra_fs_fat_dir.c.
 *
 * @details
 * Dedicated companion test executable that drives the branches in
 * ra_fs_fat_dir.c that no sibling test yet exercises:
 *
 *   - ra_fs_listdir guards (lines 123, 128, 131, 149, 156, 159)
 *   - priv_dir_cluster_init error + SPC>1 loop (lines 233, 237-239, 241)
 *   - priv_fat_mkdir error paths (lines 283, 295, 300, 305-306, 315-316)
 *   - ra_fs_mkdir guards (lines 355, 358)
 *   - ra_fs_unlink guards (lines 395, 404, 408, 421, 427)
 *   - priv_rename_prepare error paths (lines 474, 480, 487, 491, 494)
 *   - priv_fat_rename error paths (lines 545, 552, 557)
 *   - ra_fs_rename guards (lines 582, 585, 588, 591)
 *
 * Three backends are used:
 *   - normal mem backend (heap byte array, unlimited I/O)
 *   - inject backend (reads_left countdown + writes_fail flag)
 *   - wcount backend (writes_left countdown, all reads succeed)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra_err.h"
#include "ra_fs.h"
#include "unity_minimal.h"

/* ===========================================================================
 * Geometry constants
 * ===========================================================================
 */

/**
 * @enum dir_cov_geo_t
 * @brief Block-count constants for the synthetic block devices used here.
 */
typedef enum : uint32_t {
  k_geo_blk_sz     = 512U,        /**< Bytes per logical block.             */
  k_geo_fat16_spc1 = 8U * 1024U,  /**< 4 MiB FAT16 volume, SPC=1.           */
  k_geo_fat16_spc2 = 16U * 1024U, /**< 8 MiB FAT16 volume, SPC=2.           */
  k_geo_exfat      = 65536U,      /**< 32 MiB exFAT volume.                 */
  k_geo_reads_inf  = 0xFFFFFFFFU, /**< Sentinel: unlimited reads in inject. */
} dir_cov_geo_t;

/* ===========================================================================
 * Normal in-memory block device
 * ===========================================================================
 */

/** @brief Heap-backed sector store for the normal backend. */
typedef struct {
  uint8_t* bytes;       /**< Heap-allocated sector array. */
  uint32_t block_count; /**< Total 512-byte sectors.      */
  uint32_t byte_count;  /**< Total bytes (blocks * size). */
} mem_disk_t;

/** @var s_disk
 * @brief Global singleton heap disk; freed by free_vol().
 * @warning Shared across tests; free_vol() must be called at test end.
 * @since 0.1.0
 */
static mem_disk_t s_disk = {};

static ra_err_t mem_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra_err_out_of_range;
  }
  memcpy(buf, &d->bytes[lba * (uint32_t)k_geo_blk_sz], count * (uint32_t)k_geo_blk_sz);
  return k_ra_ok;
}

static ra_err_t mem_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra_err_out_of_range;
  }
  memcpy(&d->bytes[lba * (uint32_t)k_geo_blk_sz], buf, count * (uint32_t)k_geo_blk_sz);
  return k_ra_ok;
}

static ra_err_t mem_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  *block_count  = d->block_count;
  *block_size   = (uint32_t)k_geo_blk_sz;
  return k_ra_ok;
}

/** @brief Normal backend pointing at s_disk. */
static const ra_fs_backend_t s_backend = {
  .read_block   = mem_read,
  .write_block  = mem_write,
  .get_capacity = mem_capacity,
  .ctx          = &s_disk,
};

/* ===========================================================================
 * Inject (read-countdown + write-fail) backend
 * ===========================================================================
 */

/**
 * @struct inject_disk_t
 * @brief Shared-memory backend with deterministic I/O failure injection.
 *
 * @details Wraps the same byte array as mem_disk_t. When reads_left reaches
 *          zero the next read returns k_ra_err_hw_error. When writes_fail is
 *          non-zero every write immediately returns k_ra_err_hw_error.
 */
typedef struct {
  uint8_t* bytes;       /**< Shared with s_disk.bytes.                   */
  uint32_t block_count; /**< Same geometry as s_disk.                    */
  uint32_t byte_count;  /**< Same geometry as s_disk.                    */
  uint32_t reads_left;  /**< Reads allowed; k_geo_reads_inf = unlimited. */
  uint8_t  writes_fail; /**< Non-zero -> all writes return hw_error.     */
} inject_disk_t;

/** @var s_inject
 * @brief Global inject backend state; populated by swap_to_inject().
 * @note Restore h->backend after use.
 * @since 0.1.0
 */
static inject_disk_t s_inject = {};

static ra_err_t inj_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  inject_disk_t* d = (inject_disk_t*)ctx;
  if (d->reads_left == 0U) {
    return k_ra_err_hw_error;
  }
  if (d->reads_left != (uint32_t)k_geo_reads_inf) {
    d->reads_left--;
  }
  if (lba + count > d->block_count) {
    return k_ra_err_out_of_range;
  }
  memcpy(buf, &d->bytes[lba * (uint32_t)k_geo_blk_sz], count * (uint32_t)k_geo_blk_sz);
  return k_ra_ok;
}

static ra_err_t inj_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  inject_disk_t* d = (inject_disk_t*)ctx;
  if (d->writes_fail != 0U) {
    return k_ra_err_hw_error;
  }
  if (lba + count > d->block_count) {
    return k_ra_err_out_of_range;
  }
  memcpy(&d->bytes[lba * (uint32_t)k_geo_blk_sz], buf, count * (uint32_t)k_geo_blk_sz);
  return k_ra_ok;
}

static ra_err_t inj_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  inject_disk_t* d = (inject_disk_t*)ctx;
  *block_count     = d->block_count;
  *block_size      = (uint32_t)k_geo_blk_sz;
  return k_ra_ok;
}

/* ===========================================================================
 * Write-countdown backend
 * ===========================================================================
 */

/**
 * @struct wcount_disk_t
 * @brief Shared-memory backend that counts down writes only.
 *
 * @details All reads succeed unconditionally. Each write_block call
 *          decrements writes_left; when zero the write returns
 *          k_ra_err_hw_error. This allows targeting the Nth write in a
 *          call chain (e.g. FAT copies vs. cluster-init vs. dir-entry).
 */
typedef struct {
  uint8_t* bytes;       /**< Shared with s_disk.bytes.                    */
  uint32_t block_count; /**< Same geometry as s_disk.                     */
  uint32_t byte_count;  /**< Same geometry as s_disk.                     */
  uint32_t writes_left; /**< Writes allowed before failure; 0 = fail now. */
} wcount_disk_t;

/** @var s_wcount
 * @brief Global write-countdown backend state.
 * @note Restore h->backend after use.
 * @since 0.1.0
 */
static wcount_disk_t s_wcount = {};

static ra_err_t wco_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  wcount_disk_t* d = (wcount_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra_err_out_of_range;
  }
  memcpy(buf, &d->bytes[lba * (uint32_t)k_geo_blk_sz], count * (uint32_t)k_geo_blk_sz);
  return k_ra_ok;
}

static ra_err_t wco_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  wcount_disk_t* d = (wcount_disk_t*)ctx;
  if (d->writes_left == 0U) {
    return k_ra_err_hw_error;
  }
  d->writes_left--;
  if (lba + count > d->block_count) {
    return k_ra_err_out_of_range;
  }
  memcpy(&d->bytes[lba * (uint32_t)k_geo_blk_sz], buf, count * (uint32_t)k_geo_blk_sz);
  return k_ra_ok;
}

static ra_err_t wco_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  wcount_disk_t* d = (wcount_disk_t*)ctx;
  *block_count     = d->block_count;
  *block_size      = (uint32_t)k_geo_blk_sz;
  return k_ra_ok;
}

/* ===========================================================================
 * Volume builders
 * ===========================================================================
 */

static void put16(uint8_t* p, uint32_t off, uint16_t v)
{
  p[off]     = (uint8_t)(v & 0xFFU);
  p[off + 1] = (uint8_t)((v >> 8U) & 0xFFU);
}

/**
 * @brief Allocate and hand-build a minimal FAT16 BPB in s_disk (SPC=1).
 *
 * @details 8192 sectors, 2 FATs of 32 sectors each, 16 root entries, SPC=1.
 *          Layout: BPB(1) FAT1(32) FAT2(32) root(1) data(8126).
 *
 * @pre s_disk.bytes is nullptr.
 * @post s_disk holds a calloc-zeroed 4 MiB image with a valid BPB.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void build_fat16_vol(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
  s_disk.byte_count  = (uint32_t)k_geo_fat16_spc1 * (uint32_t)k_geo_blk_sz;
  s_disk.bytes       = (uint8_t*)calloc(1, s_disk.byte_count);
  s_disk.block_count = (uint32_t)k_geo_fat16_spc1;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
  uint8_t* bpb = s_disk.bytes;
  put16(bpb, 11U, (uint16_t)k_geo_blk_sz);
  bpb[13] = 1U;
  put16(bpb, 14U, 1U);
  bpb[16] = 2U;
  put16(bpb, 17U, 16U);
  put16(bpb, 19U, (uint16_t)k_geo_fat16_spc1);
  put16(bpb, 22U, 32U);
  bpb[510] = 0x55U;
  bpb[511] = 0xAAU;
}

/**
 * @brief Allocate and hand-build a minimal FAT16 BPB in s_disk (SPC=2).
 *
 * @details 16384 sectors, 2 FATs of 32 sectors each, 16 root entries, SPC=2.
 *          Layout: BPB(1) FAT1(32) FAT2(32) root(1) data(16318).
 *          Cluster count = 16318/2 = 8159 >= 4085 -> FAT16.
 *
 * @pre s_disk.bytes is nullptr.
 * @post s_disk holds a calloc-zeroed 8 MiB image with a valid SPC=2 BPB.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void build_fat16_spc2_vol(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
  s_disk.byte_count  = (uint32_t)k_geo_fat16_spc2 * (uint32_t)k_geo_blk_sz;
  s_disk.bytes       = (uint8_t*)calloc(1, s_disk.byte_count);
  s_disk.block_count = (uint32_t)k_geo_fat16_spc2;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
  uint8_t* bpb = s_disk.bytes;
  put16(bpb, 11U, (uint16_t)k_geo_blk_sz);
  bpb[13] = 2U;
  put16(bpb, 14U, 1U);
  bpb[16] = 2U;
  put16(bpb, 17U, 16U);
  put16(bpb, 19U, (uint16_t)k_geo_fat16_spc2);
  put16(bpb, 22U, 32U);
  bpb[510] = 0x55U;
  bpb[511] = 0xAAU;
}

/**
 * @brief Allocate a 32 MiB exFAT volume in s_disk via ra_fs_format.
 *
 * @pre s_disk.bytes is nullptr.
 * @post s_disk holds a formatted exFAT image ready to mount.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void build_exfat_vol(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
  s_disk.byte_count  = (uint32_t)k_geo_exfat * (uint32_t)k_geo_blk_sz;
  s_disk.bytes       = (uint8_t*)calloc(1, s_disk.byte_count);
  s_disk.block_count = (uint32_t)k_geo_exfat;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
  ra_fs_format_opts_t opts = {};
  opts.type                = k_ra_fs_type_exfat;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_format(&s_backend, &opts));
}

/**
 * @brief Release the heap-backed sector store in s_disk.
 *
 * @pre None.
 * @post s_disk.bytes is nullptr.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void free_vol(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
}

/* ===========================================================================
 * Shared helpers
 * ===========================================================================
 */

/**
 * @brief Count visible directory entries via listdir callback.
 *
 * @param[in]  name Unused.
 * @param[in]  attr Unused.
 * @param[in]  size Unused.
 * @param[in]  ctx  Pointer to a uint32_t counter.
 *
 * @pre ctx is non-NULL and points to a uint32_t.
 * @post Counter at ctx is incremented by one.
 *
 * @note Trivially thread-safe (no shared state modified by this function).
 * @since 0.1.0
 */
static void count_cb(const char* name, uint8_t attr, uint32_t size, void* ctx)
{
  (void)name;
  (void)attr;
  (void)size;
  if (ctx != nullptr) {
    (*(uint32_t*)ctx)++;
  }
}

/**
 * @brief Redirect h->backend to the inject (read-countdown) backend.
 *
 * @details Shares s_disk.bytes with s_inject; the caller saves and restores
 *          h->backend when done.
 *
 * @param[in,out] h           Mounted volume to redirect.
 * @param[in]     reads_left  Reads allowed before failure.
 * @param[in]     writes_fail Non-zero to make every write fail immediately.
 *
 * @pre h is non-NULL and mounted; s_disk.bytes is non-NULL.
 * @post h->backend uses inj_read / inj_write callbacks.
 *
 * @note Caller must restore h->backend = saved after use.
 * @since 0.1.0
 */
static void swap_to_inject(ra_fs_mount_t* h, uint32_t reads_left, uint8_t writes_fail)
{
  s_inject.bytes          = s_disk.bytes;
  s_inject.block_count    = s_disk.block_count;
  s_inject.byte_count     = s_disk.byte_count;
  s_inject.reads_left     = reads_left;
  s_inject.writes_fail    = writes_fail;
  h->backend.read_block   = inj_read;
  h->backend.write_block  = inj_write;
  h->backend.get_capacity = inj_capacity;
  h->backend.ctx          = &s_inject;
}

/**
 * @brief Redirect h->backend to the write-countdown backend.
 *
 * @details Shares s_disk.bytes with s_wcount; all reads succeed. Each
 *          write_block call consumes one token from writes_left.
 *
 * @param[in,out] h           Mounted volume to redirect.
 * @param[in]     writes_left Writes allowed before failure.
 *
 * @pre h is non-NULL and mounted; s_disk.bytes is non-NULL.
 * @post h->backend uses wco_read / wco_write callbacks.
 *
 * @note Caller must restore h->backend = saved after use.
 * @since 0.1.0
 */
static void swap_to_wcount(ra_fs_mount_t* h, uint32_t writes_left)
{
  s_wcount.bytes          = s_disk.bytes;
  s_wcount.block_count    = s_disk.block_count;
  s_wcount.byte_count     = s_disk.byte_count;
  s_wcount.writes_left    = writes_left;
  h->backend.read_block   = wco_read;
  h->backend.write_block  = wco_write;
  h->backend.get_capacity = wco_capacity;
  h->backend.ctx          = &s_wcount;
}

/**
 * @brief Create count empty files in the directory at dir_path.
 *
 * @details Each file is opened (write mode, creates dir entry with
 *          first_cluster=0) then immediately closed. The file name is
 *          "Gnn.TXT" where nn is the zero-padded index (00-15).
 *
 * @param[in,out] h        Mounted volume.
 * @param[in]     dir_path Parent directory (e.g. "/" or "/SUB").
 * @param[in]     count    Number of files to create (0..16).
 *
 * @pre h is non-NULL and mounted; dir_path is a valid directory.
 * @pre count <= 16 (maximum root-dir entries).
 * @post count new dir entries exist under dir_path.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void create_empty_files(ra_fs_mount_t* h, const char* dir_path, uint32_t count)
{
  for (uint32_t i = 0; i < count; i++) {
    char name[32] = {};
    if (strcmp(dir_path, "/") == 0) {
      (void)snprintf(name, sizeof(name), "/F%02u.TXT", i);
    } else {
      (void)snprintf(name, sizeof(name), "%s/G%02u.TXT", dir_path, i);
    }
    ra_fs_file_t* f = nullptr;
    TEST_ASSERT_EQ(k_ra_ok, ra_fs_open(h, name, k_ra_fs_mode_write, &f));
    TEST_ASSERT_EQ(k_ra_ok, ra_fs_close(f));
  }
}

/* ===========================================================================
 * Tests: ra_fs_listdir guards
 * ===========================================================================
 */

/**
 * @test test_listdir_not_mounted
 * @brief listdir on an unmounted handle returns k_ra_err_invalid_state (line 123).
 *
 * @details Mounts a FAT16 volume, clears in_use=0 to simulate an unmounted
 *          handle, then calls ra_fs_listdir. The guard at line 122-123 fires.
 *
 * @par MC/DC:
 * Decision: `if (handle->in_use == 0U)` (line 122, 1 condition).
 * V1: in_use=1 -> false (normal path, tested by sibling tests).
 * V2: in_use=0 -> true -> line 123.
 * N=1 condition, 1 independent vector sufficient per DO-178C 6.4.4.3.
 *
 * @pre Volume is formatted; h is mounted.
 * @post Result is k_ra_err_invalid_state.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_listdir_not_mounted(void)
{
  TEST_BEGIN("listdir: unmounted -> invalid_state (line 123)");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  h->in_use = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_fs_listdir(h, "/", count_cb, nullptr));
  h->in_use = 1U;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("listdir: unmounted -> invalid_state (line 123)");
}

/**
 * @test test_listdir_exfat_no_slash
 * @brief exFAT listdir with path not starting with '/' returns not_supported (line 128).
 *
 * @details Mounts an exFAT volume and passes a path whose first byte is not
 *          '/'. The guard at line 127-128 fires.
 *
 * @par MC/DC:
 * Decision: `if (path[0] != '/')` (line 127, 1 condition).
 * V1: path[0]='/' -> false (continued, tested at line 131).
 * V2: path[0]!='/' -> true -> line 128.
 *
 * @pre exFAT volume is formatted and mounted.
 * @post Result is k_ra_err_not_supported.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_listdir_exfat_no_slash(void)
{
  TEST_BEGIN("listdir: exFAT no-slash path -> not_supported (line 128)");
  build_exfat_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra_err_not_supported, ra_fs_listdir(h, "noslash", count_cb, nullptr));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("listdir: exFAT no-slash path -> not_supported (line 128)");
}

/**
 * @test test_listdir_exfat_non_root
 * @brief exFAT listdir with path "/subdir" returns not_supported (line 131).
 *
 * @details Mounts an exFAT volume and passes path="/sub" (starts with '/'
 *          but path[1] != NUL). The guard at line 130-131 fires.
 *
 * @par MC/DC:
 * Decision: `if (path[1] != '\\0')` (line 130, 1 condition).
 * V1: path="/\\0" -> false (root path, passes to exfat_listdir).
 * V2: path="/sub" -> true -> line 131.
 *
 * @pre exFAT volume is formatted and mounted.
 * @post Result is k_ra_err_not_supported.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_listdir_exfat_non_root(void)
{
  TEST_BEGIN("listdir: exFAT non-root path -> not_supported (line 131)");
  build_exfat_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra_err_not_supported, ra_fs_listdir(h, "/sub", count_cb, nullptr));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("listdir: exFAT non-root path -> not_supported (line 131)");
}

/**
 * @test test_listdir_read_error
 * @brief listdir fails immediately on first sector read error (line 149).
 *
 * @details Mounts FAT16, swaps to inject backend with reads_left=0.
 *          The first priv_read_sector call in the listdir loop fails.
 *          Line 148-149 is hit.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra_ok)` at line 148 (1 condition).
 * V1: err=k_ra_ok -> false (sector read ok, continues to visit).
 * V2: err=k_ra_err_hw_error -> true -> line 149.
 *
 * @pre FAT16 volume is mounted; inject backend has reads_left=0.
 * @post Result is k_ra_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_listdir_read_error(void)
{
  TEST_BEGIN("listdir: immediate read error -> err (line 149)");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  ra_fs_backend_t saved = h->backend;
  swap_to_inject(h, 0U, 0U);
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_fs_listdir(h, "/", count_cb, nullptr));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("listdir: immediate read error -> err (line 149)");
}

/**
 * @test test_listdir_walk_fail
 * @brief listdir on a full subdir sector fails at priv_dir_walk_next_sector (line 156).
 *
 * @details Creates /SUB, then fills it with 14 files so its first sector holds
 *          all 16 entries ("." + ".." + 14 files) with no EoD marker. Swaps to
 *          inject backend with reads_left=2 (read 1 = resolve /SUB in root,
 *          read 2 = first subdir sector; read 3 = FAT walk for next cluster fails).
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra_ok)` at line 155 (1 condition).
 * V1: err=k_ra_ok -> false (walk advanced normally, eod set).
 * V2: err=k_ra_err_hw_error -> true -> line 156.
 *
 * @pre FAT16 volume is mounted; /SUB contains 14 files (sector full).
 * @post Result is k_ra_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_listdir_walk_fail(void)
{
  TEST_BEGIN("listdir: full subdir sector, walk fails -> err (line 156)");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mkdir(h, "/SUB"));
  create_empty_files(h, "/SUB", 14U);
  ra_fs_backend_t saved = h->backend;
  swap_to_inject(h, 2U, 0U);
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_fs_listdir(h, "/SUB", count_cb, nullptr));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("listdir: full subdir sector, walk fails -> err (line 156)");
}

/**
 * @test test_listdir_full_no_eod
 * @brief listdir with all 16 root slots filled returns k_ra_ok at line 159.
 *
 * @details Creates 16 empty files in root, filling every dir entry slot so
 *          no EoD (0x00) marker remains. priv_listdir_visit_sector returns 0
 *          (no EoD hit). priv_dir_walk_next_sector then finds no more root
 *          sectors and sets eod=1. The while loop exits and line 159 is hit.
 *
 * @par MC/DC:
 * Decision: `while (eod == 0U)` at line 146 (1 condition).
 * V1: eod=0 -> true (loop body entered, normal case).
 * V2: eod=1 -> false (loop exits) -> line 159.
 * Both arms are exercised in this test (eod transitions 0 -> 1).
 *
 * @pre FAT16 volume is mounted; 16 files in root (no EoD marker in root sector).
 * @post Result is k_ra_ok; count equals 16.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_listdir_full_no_eod(void)
{
  TEST_BEGIN("listdir: 16-entry root, no EoD -> k_ra_ok at line 159");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  create_empty_files(h, "/", 16U);
  uint32_t cnt = 0;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_listdir(h, "/", count_cb, &cnt));
  TEST_ASSERT_EQ(16U, cnt);
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("listdir: 16-entry root, no EoD -> k_ra_ok at line 159");
}

/* ===========================================================================
 * Tests: ra_fs_mkdir public guards
 * ===========================================================================
 */

/**
 * @test test_mkdir_not_mounted
 * @brief mkdir on an unmounted handle returns k_ra_err_invalid_state (line 355).
 *
 * @par MC/DC:
 * Decision: `if (handle->in_use == 0U)` (line 354, 1 condition).
 * V1: in_use=1 -> false (normal path).
 * V2: in_use=0 -> true -> line 355.
 *
 * @pre FAT16 volume is mounted; in_use forced to 0.
 * @post Result is k_ra_err_invalid_state.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_mkdir_not_mounted(void)
{
  TEST_BEGIN("mkdir: unmounted -> invalid_state (line 355)");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  h->in_use = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_fs_mkdir(h, "/NEWDIR"));
  h->in_use = 1U;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("mkdir: unmounted -> invalid_state (line 355)");
}

/**
 * @test test_mkdir_exfat
 * @brief mkdir on an exFAT volume returns k_ra_err_not_supported (line 358).
 *
 * @par MC/DC:
 * Decision: `if (handle->type == k_ra_fs_type_exfat)` (line 357, 1 condition).
 * V1: type!=exfat -> false (FAT path, tested by sibling tests).
 * V2: type==exfat -> true -> line 358.
 *
 * @pre exFAT volume is formatted and mounted.
 * @post Result is k_ra_err_not_supported.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_mkdir_exfat(void)
{
  TEST_BEGIN("mkdir: exFAT volume -> not_supported (line 358)");
  build_exfat_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra_err_not_supported, ra_fs_mkdir(h, "/DIR"));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("mkdir: exFAT volume -> not_supported (line 358)");
}

/* ===========================================================================
 * Tests: priv_fat_mkdir internal error paths
 * ===========================================================================
 */

/**
 * @test test_mkdir_bad_leaf
 * @brief mkdir with a non-8.3 leaf name returns k_ra_err_invalid_arg (line 283).
 *
 * @details The leaf "bad name!" contains a space and '!' which are not valid
 *          8.3 characters. priv_path_to_83 returns 0 -> line 283.
 *
 * @par MC/DC:
 * Decision: `if (priv_path_to_83(leaf, name83) == 0U)` (line 282, 1 condition).
 * V1: valid 8.3 -> false (continues to dir_find).
 * V2: invalid 8.3 -> true -> line 283.
 *
 * @pre FAT16 volume is mounted.
 * @post Result is k_ra_err_invalid_arg.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_mkdir_bad_leaf(void)
{
  TEST_BEGIN("mkdir: non-8.3 leaf -> invalid_arg (line 283)");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_fs_mkdir(h, "/bad name!"));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("mkdir: non-8.3 leaf -> invalid_arg (line 283)");
}

/**
 * @test test_mkdir_dir_full
 * @brief mkdir when root dir is full returns the error from priv_dir_find_free (line 295).
 *
 * @details Fills all 16 root dir entry slots with empty files. A subsequent
 *          mkdir has no free slot -> priv_dir_find_free returns k_ra_err_no_mem
 *          -> line 295.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra_ok)` at line 294 (1 condition).
 * V1: err=k_ra_ok -> false (free slot found, continues).
 * V2: err!=k_ra_ok -> true -> line 295.
 *
 * @pre FAT16 volume is mounted; 16 empty files fill all root dir slots.
 * @post Result is k_ra_err_no_mem.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_mkdir_dir_full(void)
{
  TEST_BEGIN("mkdir: root dir full -> no_mem from dir_find_free (line 295)");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  create_empty_files(h, "/", 16U);
  TEST_ASSERT_EQ(k_ra_err_no_mem, ra_fs_mkdir(h, "/NOROOM"));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("mkdir: root dir full -> no_mem from dir_find_free (line 295)");
}

/**
 * @test test_mkdir_alloc_fail
 * @brief mkdir fails at priv_alloc_eoc_cluster when first FAT write fails (line 300).
 *
 * @details Uses the wcount backend with writes_left=0 so the first write
 *          (FAT1 update inside priv_alloc_eoc_cluster) fails immediately.
 *          Line 299-300 is hit.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra_ok)` at line 299 (1 condition).
 * V1: err=k_ra_ok -> false (cluster allocated, continues to cluster_init).
 * V2: err!=k_ra_ok -> true -> line 300.
 *
 * @pre FAT16 volume is mounted; wcount writes_left=0.
 * @post Result is k_ra_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_mkdir_alloc_fail(void)
{
  TEST_BEGIN("mkdir: alloc_eoc_cluster write fails -> err (line 300)");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  ra_fs_backend_t saved = h->backend;
  swap_to_wcount(h, 0U);
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_fs_mkdir(h, "/NEWDIR"));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("mkdir: alloc_eoc_cluster write fails -> err (line 300)");
}

/**
 * @test test_mkdir_cluster_init_fail
 * @brief mkdir fails at priv_dir_cluster_init sector-0 write, triggers priv_free_chain
 *        (lines 233, 305-306).
 *
 * @details Uses wcount writes_left=2 on a SPC=1 volume. The two FAT writes
 *          inside priv_alloc_eoc_cluster succeed (writes 1 and 2). The third
 *          write (sector 0 of the new dir cluster inside priv_dir_cluster_init)
 *          fails. Line 233 propagates the error back to priv_fat_mkdir which
 *          then executes priv_free_chain (line 305) and returns (line 306).
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra_ok)` at line 232 in priv_dir_cluster_init (1 condition).
 * V1: err=k_ra_ok -> false (sector 0 written ok, continues to loop).
 * V2: err!=k_ra_ok -> true -> line 233.
 *
 * @pre FAT16 SPC=1 volume is mounted; wcount writes_left=2.
 * @post Result is k_ra_err_hw_error (cluster is freed via line 305).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_mkdir_cluster_init_fail(void)
{
  TEST_BEGIN("mkdir: cluster_init sector-0 write fails -> free+return (lines 233,305-306)");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  ra_fs_backend_t saved = h->backend;
  swap_to_wcount(h, 2U);
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_fs_mkdir(h, "/NEWDIR"));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("mkdir: cluster_init sector-0 write fails -> free+return (lines 233,305-306)");
}

/**
 * @test test_mkdir_write_entry_fail
 * @brief mkdir fails at priv_write_new_dir_entry, triggers priv_free_chain (lines 315-316).
 *
 * @details Uses wcount writes_left=3 on a SPC=1 volume. Writes 1-2 are FAT
 *          updates (priv_alloc_eoc_cluster). Write 3 is cluster sector-0
 *          (priv_dir_cluster_init, succeeds). Write 4 (priv_write_new_dir_entry
 *          to root dir sector) fails. Lines 314-316 are hit.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra_ok)` at line 314 (1 condition).
 * V1: err=k_ra_ok -> false (dir entry written, returns k_ra_ok).
 * V2: err!=k_ra_ok -> true -> lines 315-316.
 *
 * @pre FAT16 SPC=1 volume is mounted; wcount writes_left=3.
 * @post Result is k_ra_err_hw_error (cluster freed via line 315).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_mkdir_write_entry_fail(void)
{
  TEST_BEGIN("mkdir: write_new_dir_entry fails -> free+return (lines 315-316)");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  ra_fs_backend_t saved = h->backend;
  swap_to_wcount(h, 3U);
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_fs_mkdir(h, "/NEWDIR"));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("mkdir: write_new_dir_entry fails -> free+return (lines 315-316)");
}

/**
 * @test test_mkdir_spc2_ok
 * @brief mkdir on a SPC=2 volume succeeds, covering the extra-sector loop (lines 237-238, 241).
 *
 * @details On a SPC=2 volume, priv_dir_cluster_init writes sector 0 (dot
 *          entries) then enters the loop for s=1 (line 237: write sector 1),
 *          checks result (line 238: condition false), and the loop ends
 *          (line 241: closing brace). The directory is created successfully.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra_ok)` at line 238 inside the SPC loop (1 condition).
 * V1: err=k_ra_ok -> false (loop iteration ok) -> line 241 (covered here).
 * V2: err!=k_ra_ok -> true -> line 239 (covered by test_mkdir_spc2_second_fail).
 *
 * @pre FAT16 SPC=2 volume is mounted.
 * @post Result is k_ra_ok; /SUB2 is visible in root listing.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_mkdir_spc2_ok(void)
{
  TEST_BEGIN("mkdir SPC=2: extra-sector loop succeeds (lines 237-238, 241)");
  build_fat16_spc2_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mkdir(h, "/SUB2"));
  uint32_t cnt = 0;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_listdir(h, "/", count_cb, &cnt));
  TEST_ASSERT_EQ(1U, cnt);
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("mkdir SPC=2: extra-sector loop succeeds (lines 237-238, 241)");
}

/**
 * @test test_mkdir_spc2_second_fail
 * @brief mkdir on SPC=2 volume fails at the extra sector write in the loop (line 239).
 *
 * @details Uses wcount writes_left=3. Writes 1-2 are FAT updates (alloc_eoc).
 *          Write 3 is sector 0 of the cluster (priv_dir_cluster_init, ok).
 *          Write 4 is sector 1 of the cluster (the loop body at line 237, SPC=2);
 *          this fails -> line 239.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra_ok)` at line 238 inside the SPC loop (1 condition).
 * V1: err=k_ra_ok -> false -> covered by test_mkdir_spc2_ok.
 * V2: err!=k_ra_ok -> true -> line 239 (covered here).
 *
 * @pre FAT16 SPC=2 volume is mounted; wcount writes_left=3.
 * @post Result is k_ra_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_mkdir_spc2_second_fail(void)
{
  TEST_BEGIN("mkdir SPC=2: extra-sector write fails -> err (line 239)");
  build_fat16_spc2_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  ra_fs_backend_t saved = h->backend;
  swap_to_wcount(h, 3U);
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_fs_mkdir(h, "/NEWDIR"));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("mkdir SPC=2: extra-sector write fails -> err (line 239)");
}

/* ===========================================================================
 * Tests: ra_fs_unlink guards and error paths
 * ===========================================================================
 */

/**
 * @test test_unlink_not_mounted
 * @brief unlink on an unmounted handle returns k_ra_err_invalid_state (line 395).
 *
 * @par MC/DC:
 * Decision: `if (handle->in_use == 0U)` (line 394, 1 condition).
 * V1: in_use=1 -> false (normal path).
 * V2: in_use=0 -> true -> line 395.
 *
 * @pre FAT16 volume is mounted; in_use forced to 0.
 * @post Result is k_ra_err_invalid_state.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_unlink_not_mounted(void)
{
  TEST_BEGIN("unlink: unmounted -> invalid_state (line 395)");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  h->in_use = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_fs_unlink(h, "/F.TXT"));
  h->in_use = 1U;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("unlink: unmounted -> invalid_state (line 395)");
}

/**
 * @test test_unlink_bad_parent
 * @brief unlink on a path with a non-existent parent returns an error (line 404).
 *
 * @details priv_resolve_parent("/NODIR/F.TXT") tries to enter /NODIR which
 *          does not exist -> returns k_ra_err_not_found. Line 403-404 is hit.
 *
 * @par MC/DC:
 * Decision: `if (rerr != k_ra_ok)` at line 403 (1 condition).
 * V1: rerr=k_ra_ok -> false (parent resolved ok).
 * V2: rerr!=k_ra_ok -> true -> line 404.
 *
 * @pre FAT16 volume is mounted; /NODIR does not exist.
 * @post Result is k_ra_err_not_found.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_unlink_bad_parent(void)
{
  TEST_BEGIN("unlink: missing parent dir -> err (line 404)");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra_err_not_found, ra_fs_unlink(h, "/NODIR/F.TXT"));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("unlink: missing parent dir -> err (line 404)");
}

/**
 * @test test_unlink_bad_83
 * @brief unlink with a non-8.3 leaf name returns k_ra_err_invalid_arg (line 408).
 *
 * @details priv_path_to_83("bad name!") returns 0 -> line 408.
 *
 * @par MC/DC:
 * Decision: `if (priv_path_to_83(leaf, name83) == 0U)` (line 407, 1 condition).
 * V1: valid 8.3 -> false (continues to dir_find).
 * V2: invalid 8.3 -> true -> line 408.
 *
 * @pre FAT16 volume is mounted.
 * @post Result is k_ra_err_invalid_arg.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_unlink_bad_83(void)
{
  TEST_BEGIN("unlink: non-8.3 leaf -> invalid_arg (line 408)");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_fs_unlink(h, "/bad name!"));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("unlink: non-8.3 leaf -> invalid_arg (line 408)");
}

/**
 * @test test_unlink_free_chain_fail
 * @brief unlink on a file with data fails at priv_free_chain FAT read (line 421).
 *
 * @details Creates /DATA.TXT with content (first_cluster >= 2). Swaps to
 *          inject backend with reads_left=1. Read 1 is the priv_dir_find scan
 *          of root dir (succeeds). Read 2 is priv_fat_get inside priv_free_chain
 *          to find next cluster (fails) -> line 421.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra_ok)` at line 420 (1 condition).
 * V1: err=k_ra_ok -> false (chain freed, continues).
 * V2: err!=k_ra_ok -> true -> line 421.
 *
 * @pre FAT16 volume mounted; /DATA.TXT has data; inject reads_left=1.
 * @post Result is k_ra_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_unlink_free_chain_fail(void)
{
  TEST_BEGIN("unlink: free_chain FAT read fails -> err (line 421)");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_write_file(h, "/DATA.TXT", payload, sizeof(payload)));
  ra_fs_backend_t saved = h->backend;
  swap_to_inject(h, 1U, 0U);
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_fs_unlink(h, "/DATA.TXT"));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("unlink: free_chain FAT read fails -> err (line 421)");
}

/**
 * @test test_unlink_dir_read_fail
 * @brief unlink on an empty file fails at sector re-read for deletion (line 427).
 *
 * @details Creates /EMPTY.TXT with no data (first_cluster=0). Swaps to inject
 *          backend with reads_left=1. Read 1 is the priv_dir_find scan of root
 *          dir (succeeds, finds entry). first_cluster=0 < 2 so priv_free_chain
 *          is skipped. Read 2 is priv_read_sector at line 425 to re-load the
 *          sector for deletion marking (fails) -> line 427.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra_ok)` at line 426 (1 condition).
 * V1: err=k_ra_ok -> false (sector read, marks 0xE5 and writes).
 * V2: err!=k_ra_ok -> true -> line 427.
 *
 * @pre FAT16 volume mounted; /EMPTY.TXT is empty; inject reads_left=1.
 * @post Result is k_ra_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_unlink_dir_read_fail(void)
{
  TEST_BEGIN("unlink: empty file, dir sector re-read fails -> err (line 427)");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  ra_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_open(h, "/EMPTY.TXT", k_ra_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_close(f));
  ra_fs_backend_t saved = h->backend;
  swap_to_inject(h, 1U, 0U);
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_fs_unlink(h, "/EMPTY.TXT"));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("unlink: empty file, dir sector re-read fails -> err (line 427)");
}

/* ===========================================================================
 * Tests: ra_fs_rename guards
 * ===========================================================================
 */

/**
 * @test test_rename_null_guards
 * @brief ra_fs_rename returns k_ra_err_null_ptr for null handle/old/new (lines 582,585,588).
 *
 * @details Three single-condition checks at lines 581, 584, 587. Each is a
 *          trivial null check; all three must fire to cover lines 582, 585, 588.
 *
 * @par MC/DC:
 * Decisions: three independent `if (X == nullptr)` checks (1 condition each).
 * V1: handle=null -> line 582.
 * V2: handle=ok, old_path=null -> line 585.
 * V3: handle=ok, old_path=ok, new_path=null -> line 588.
 * Three vectors for three conditions, each independently covered.
 *
 * @pre FAT16 volume is mounted.
 * @post All three guard checks return k_ra_err_null_ptr.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_rename_null_guards(void)
{
  TEST_BEGIN("rename: null handle/old/new -> null_ptr (lines 582,585,588)");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_fs_rename(nullptr, "/A.TXT", "/B.TXT"));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_fs_rename(h, nullptr, "/B.TXT"));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_fs_rename(h, "/A.TXT", nullptr));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("rename: null handle/old/new -> null_ptr (lines 582,585,588)");
}

/**
 * @test test_rename_not_mounted
 * @brief rename on unmounted handle returns k_ra_err_invalid_state (line 591).
 *
 * @par MC/DC:
 * Decision: `if (handle->in_use == 0U)` (line 590, 1 condition).
 * V1: in_use=1 -> false (normal path).
 * V2: in_use=0 -> true -> line 591.
 *
 * @pre FAT16 volume is mounted; in_use forced to 0.
 * @post Result is k_ra_err_invalid_state.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_rename_not_mounted(void)
{
  TEST_BEGIN("rename: unmounted -> invalid_state (line 591)");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  h->in_use = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_fs_rename(h, "/A.TXT", "/B.TXT"));
  h->in_use = 1U;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("rename: unmounted -> invalid_state (line 591)");
}

/* ===========================================================================
 * Tests: priv_rename_prepare error paths
 * ===========================================================================
 */

/**
 * @test test_rename_new_parent_fail
 * @brief rename where new_path has a missing parent returns an error (line 474).
 *
 * @details priv_resolve_parent for old_path ("/F.TXT") succeeds (root).
 *          priv_resolve_parent for new_path ("/NODIR/G.TXT") fails because
 *          /NODIR does not exist -> returns k_ra_err_not_found. Line 473-474 is hit.
 *
 * @par MC/DC:
 * Decision: `if (e2 != k_ra_ok)` at line 473 (1 condition).
 * V1: e2=k_ra_ok -> false (new parent resolved).
 * V2: e2!=k_ra_ok -> true -> line 474.
 *
 * @pre FAT16 volume is mounted; /NODIR does not exist.
 * @post Result is k_ra_err_not_found.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_rename_new_parent_fail(void)
{
  TEST_BEGIN("rename: new_path missing parent -> err (line 474)");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra_err_not_found, ra_fs_rename(h, "/F.TXT", "/NODIR/G.TXT"));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("rename: new_path missing parent -> err (line 474)");
}

/**
 * @test test_rename_root_vs_subdir
 * @brief rename across root/subdir boundary returns not_supported (line 480).
 *
 * @details Creates /SUB. rename("/F.TXT", "/SUB/F.TXT"): old_path resolves to
 *          root (is_root=1), new_path resolves to /SUB (is_root=0). The
 *          is_root mismatch check at line 482-483 fires -> line 480 returned.
 *
 * @par MC/DC:
 * Decision: `if (op.is_root != np.is_root)` (line 482, 1 condition).
 * V1: both root -> false (continues).
 * V2: root vs subdir -> true -> line 480 (not_supported).
 *
 * @pre FAT16 volume mounted; /SUB exists.
 * @post Result is k_ra_err_not_supported.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_rename_root_vs_subdir(void)
{
  TEST_BEGIN("rename: root vs subdir -> not_supported (line 480)");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mkdir(h, "/SUB"));
  TEST_ASSERT_EQ(k_ra_err_not_supported, ra_fs_rename(h, "/F.TXT", "/SUB/F.TXT"));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("rename: root vs subdir -> not_supported (line 480)");
}

/**
 * @test test_rename_cross_subdir
 * @brief rename across two different subdirs returns not_supported (line 487).
 *
 * @details Creates /A and /B (different clusters). rename("/A/F.TXT", "/B/F.TXT"):
 *          both parents are non-root (is_root=0) but their cluster numbers differ.
 *          Line 485-487 fires.
 *
 * @par MC/DC:
 * Decision: `if (op.cluster != np.cluster)` (line 486, 1 condition).
 * V1: same cluster -> false (same-directory rename, not_supported NOT returned).
 * V2: different clusters -> true -> line 487 (not_supported).
 *
 * @pre FAT16 volume mounted; /A and /B both exist.
 * @post Result is k_ra_err_not_supported.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_rename_cross_subdir(void)
{
  TEST_BEGIN("rename: different subdirs -> not_supported (line 487)");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mkdir(h, "/A"));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mkdir(h, "/B"));
  TEST_ASSERT_EQ(k_ra_err_not_supported, ra_fs_rename(h, "/A/F.TXT", "/B/F.TXT"));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("rename: different subdirs -> not_supported (line 487)");
}

/**
 * @test test_rename_old_bad_83
 * @brief rename where old_path leaf is not 8.3 returns k_ra_err_invalid_arg (line 491).
 *
 * @details priv_rename_prepare: both parents resolve to root (ok). priv_path_to_83
 *          for the old leaf "bad name!" fails (contains spaces/punctuation) -> line 491.
 *
 * @par MC/DC:
 * Decision: `if (priv_path_to_83(ol, old83) == 0U)` (line 490, 1 condition).
 * V1: valid 8.3 old name -> false (continues to new name check).
 * V2: invalid 8.3 old name -> true -> line 491.
 *
 * @pre FAT16 volume is mounted.
 * @post Result is k_ra_err_invalid_arg.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_rename_old_bad_83(void)
{
  TEST_BEGIN("rename: old leaf not 8.3 -> invalid_arg (line 491)");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_fs_rename(h, "/bad name!", "/NEW.TXT"));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("rename: old leaf not 8.3 -> invalid_arg (line 491)");
}

/**
 * @test test_rename_new_bad_83
 * @brief rename where new_path leaf is not 8.3 returns k_ra_err_invalid_arg (line 494).
 *
 * @details priv_rename_prepare: both parents resolve to root (ok). Old leaf
 *          "OLD.TXT" packs to 8.3 successfully. New leaf "bad name!" fails -> line 494.
 *
 * @par MC/DC:
 * Decision: `if (priv_path_to_83(nl, new83) == 0U)` (line 493, 1 condition).
 * V1: valid 8.3 new name -> false (parent set, returns k_ra_ok).
 * V2: invalid 8.3 new name -> true -> line 494.
 *
 * @pre FAT16 volume is mounted.
 * @post Result is k_ra_err_invalid_arg.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_rename_new_bad_83(void)
{
  TEST_BEGIN("rename: new leaf not 8.3 -> invalid_arg (line 494)");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_fs_rename(h, "/OLD.TXT", "/bad name!"));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("rename: new leaf not 8.3 -> invalid_arg (line 494)");
}

/* ===========================================================================
 * Tests: priv_fat_rename error paths
 * ===========================================================================
 */

/**
 * @test test_rename_exists
 * @brief rename where new_path already exists returns k_ra_err_exists (line 545).
 *
 * @details Creates A.TXT and B.TXT. rename("/A.TXT", "/B.TXT"): priv_dir_find
 *          for the new name (B.TXT) returns k_ra_ok (found) -> line 545.
 *
 * @par MC/DC:
 * Decision: `if (priv_dir_find(..., new83) == k_ra_ok)` (line 544, 1 condition).
 * V1: new name not found -> false (continues to find old name).
 * V2: new name found -> true -> line 545.
 *
 * @pre FAT16 volume mounted; A.TXT and B.TXT both exist.
 * @post Result is k_ra_err_exists.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_rename_exists(void)
{
  TEST_BEGIN("rename: new name already exists -> exists (line 545)");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  ra_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_open(h, "/A.TXT", k_ra_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_close(f));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_open(h, "/B.TXT", k_ra_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_close(f));
  TEST_ASSERT_EQ(k_ra_err_exists, ra_fs_rename(h, "/A.TXT", "/B.TXT"));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("rename: new name already exists -> exists (line 545)");
}

/**
 * @test test_rename_old_not_found
 * @brief rename where old_path does not exist returns k_ra_err_not_found (line 552).
 *
 * @details On a fresh volume, neither NOEXIST.TXT nor NEW.TXT exist.
 *          priv_dir_find(new83): not found (no k_ra_err_exists branch). Then
 *          priv_dir_find(old83 "NOEXIST.TXT"): not found -> line 551-552.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra_ok)` at line 551 (1 condition).
 * V1: err=k_ra_ok -> false (old name found, reads sector for rewrite).
 * V2: err!=k_ra_ok -> true -> line 552.
 *
 * @pre FAT16 volume is freshly mounted (empty); no files exist.
 * @post Result is k_ra_err_not_found.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_rename_old_not_found(void)
{
  TEST_BEGIN("rename: old name not found -> not_found (line 552)");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra_err_not_found, ra_fs_rename(h, "/NOEXIST.TXT", "/NEW.TXT"));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("rename: old name not found -> not_found (line 552)");
}

/**
 * @test test_rename_read_fail
 * @brief rename fails at sector re-read after finding old name (line 557).
 *
 * @details Creates F.TXT. Swaps to inject backend with reads_left=2.
 *          Read 1: priv_dir_find(new83 "G.TXT") -> not found (G doesn't exist).
 *          Read 2: priv_dir_find(old83 "F.TXT") -> found (F exists).
 *          Read 3: priv_read_sector(lba, sec) at line 555 -> fails -> line 557.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra_ok)` at line 556 (1 condition).
 * V1: err=k_ra_ok -> false (sector read ok, copies new name and writes).
 * V2: err!=k_ra_ok -> true -> line 557.
 *
 * @pre FAT16 volume mounted; F.TXT exists; G.TXT does not; inject reads_left=2.
 * @post Result is k_ra_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_rename_read_fail(void)
{
  TEST_BEGIN("rename: sector re-read fails after finding old name -> err (line 557)");
  build_fat16_vol();
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  ra_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_open(h, "/F.TXT", k_ra_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_close(f));
  ra_fs_backend_t saved = h->backend;
  swap_to_inject(h, 2U, 0U);
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_fs_rename(h, "/F.TXT", "/G.TXT"));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  free_vol();
  TEST_END("rename: sector re-read fails after finding old name -> err (line 557)");
}

/* ===========================================================================
 * Entry point
 * ===========================================================================
 */

/**
 * @brief Test executable entry point.
 *
 * @details Runs all coverage-booster tests in sequence. Each test is
 *          self-contained: it builds the volume, mounts, exercises the
 *          target lines, unmounts, and frees the disk. Failure exits via
 *          TEST_FAIL_FMT (exit(1)).
 *
 * @return 0 on success (all tests passed).
 *
 * @pre Host environment provides calloc/free and stderr.
 * @post All targeted lines in ra_fs_fat_dir.c are exercised.
 *
 * @note Not thread-safe (single-threaded test runner).
 * @since 0.1.0
 */
int main(void)
{
  test_listdir_not_mounted();
  test_listdir_exfat_no_slash();
  test_listdir_exfat_non_root();
  test_listdir_read_error();
  test_listdir_walk_fail();
  test_listdir_full_no_eod();

  test_mkdir_not_mounted();
  test_mkdir_exfat();
  test_mkdir_bad_leaf();
  test_mkdir_dir_full();
  test_mkdir_alloc_fail();
  test_mkdir_cluster_init_fail();
  test_mkdir_write_entry_fail();
  test_mkdir_spc2_ok();
  test_mkdir_spc2_second_fail();

  test_unlink_not_mounted();
  test_unlink_bad_parent();
  test_unlink_bad_83();
  test_unlink_free_chain_fail();
  test_unlink_dir_read_fail();

  test_rename_null_guards();
  test_rename_not_mounted();
  test_rename_new_parent_fail();
  test_rename_root_vs_subdir();
  test_rename_cross_subdir();
  test_rename_old_bad_83();
  test_rename_new_bad_83();
  test_rename_exists();
  test_rename_old_not_found();
  test_rename_read_fail();

  return 0;
}
