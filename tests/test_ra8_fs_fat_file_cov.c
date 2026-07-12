/**
 * @file test_ra8_fs_fat_file_cov.c
 * @brief Coverage booster for libs/ra8_fs/src/ra8_fs_fat_file.c.
 *
 * @details
 * Dedicated companion test executable that drives the branches in
 * ra8_fs_fat_file.c that no sibling test yet exercises:
 *
 *   - priv_truncate_existing (lines 68-89) -- re-open an existing file for
 *     write triggers the truncation path.
 *   - priv_open_existing error paths (lines 128, 142-145, 148-149) -- file
 *     table exhaustion, write-truncation failure, and append mode.
 *   - priv_write_new_dir_entry (line 167) -- backend read error.
 *   - priv_enter_subdir guards (lines 264, 273, 287) -- name too long,
 *     non-8.3 component, and zero-cluster directory entry.
 *   - priv_resolve_parent depth guard (line 329).
 *   - priv_resolve_dir error and trailing-slash paths (lines 348, 355-356).
 *   - priv_create_new error paths (lines 401, 405, 409).
 *   - ra8_fs_open guards (lines 453, 462, 484).
 *
 * The in-memory block device harness mirrors the pattern from
 * tests/test_ra8_fs_fat.c exactly.  An additional "inject" backend wraps
 * the same byte array but can count-down reads to zero or refuse all
 * writes, allowing deterministic I/O-error injection without SIGALRM or
 * setitimer.
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
#include "unity_minimal.h"

/* ===========================================================================
 * Disk geometry constants
 * ===========================================================================
 */

/**
 * @enum ra8_fs_cov_disk_t
 * @brief Block-count and geometry constants for synthetic block devices.
 */
typedef enum : uint32_t {
  k_cov_block_size   = 512U,        /**< Bytes per logical block.             */
  k_cov_blocks_fat16 = 8U * 1024U,  /**< 4 MiB FAT16 card (matches sibling).  */
  k_cov_blocks_exfat = 65536U,      /**< 32 MiB -- minimum valid exFAT size.  */
  k_cov_reads_inf    = 0xFFFFFFFFU, /**< Sentinel: unlimited reads in inject. */
} ra8_fs_cov_disk_t;

/* ===========================================================================
 * Normal in-memory block device
 * ===========================================================================
 */

/** @brief Flat sector store for the normal backend. */
typedef struct {
  uint8_t* bytes;       /**< Heap-backed sector array. */
  uint32_t block_count; /**< Total 512-byte sectors.   */
  uint32_t byte_count;  /**< Total bytes (block*size). */
} mem_disk_t;

static mem_disk_t s_disk = {};

static ra8_err_t mem_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf, &d->bytes[lba * (uint32_t)k_cov_block_size], count * (uint32_t)k_cov_block_size);
  return k_ra8_ok;
}

static ra8_err_t mem_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[lba * (uint32_t)k_cov_block_size], buf, count * (uint32_t)k_cov_block_size);
  return k_ra8_ok;
}

static ra8_err_t mem_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  *block_count  = d->block_count;
  *block_size   = (uint32_t)k_cov_block_size;
  return k_ra8_ok;
}

static const ra8_fs_backend_t s_backend = {
  .read_block   = mem_read,
  .write_block  = mem_write,
  .get_capacity = mem_capacity,
  .ctx          = &s_disk,
};

/* ===========================================================================
 * Injecting (failure-injecting) backend
 * ===========================================================================
 */

/**
 * @struct inject_disk_t
 * @brief Shared-memory backend with deterministic I/O failure injection.
 *
 * @details Wraps the same byte array as ::mem_disk_t.  When `reads_left`
 *          reaches zero the next read returns ::k_ra8_err_hw_error.  When
 *          `writes_fail` is non-zero every write immediately returns
 *          ::k_ra8_err_hw_error.
 */
typedef struct {
  uint8_t* bytes;       /**< Shared with s_disk.bytes.                   */
  uint32_t block_count; /**< Same geometry as s_disk.                    */
  uint32_t byte_count;  /**< Same geometry as s_disk.                    */
  uint32_t reads_left;  /**< Reads allowed; k_cov_reads_inf = unlimited. */
  uint8_t  writes_fail; /**< Non-zero -> all writes return hw_error.     */
} inject_disk_t;

static inject_disk_t s_inject = {};

static ra8_err_t inj_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  inject_disk_t* d = (inject_disk_t*)ctx;
  if (d->reads_left == 0U) {
    return k_ra8_err_hw_error;
  }
  if (d->reads_left != (uint32_t)k_cov_reads_inf) {
    d->reads_left--;
  }
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf, &d->bytes[lba * (uint32_t)k_cov_block_size], count * (uint32_t)k_cov_block_size);
  return k_ra8_ok;
}

static ra8_err_t inj_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  inject_disk_t* d = (inject_disk_t*)ctx;
  if (d->writes_fail != 0U) {
    return k_ra8_err_hw_error;
  }
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[lba * (uint32_t)k_cov_block_size], buf, count * (uint32_t)k_cov_block_size);
  return k_ra8_ok;
}

static ra8_err_t inj_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  inject_disk_t* d = (inject_disk_t*)ctx;
  *block_count     = d->block_count;
  *block_size      = (uint32_t)k_cov_block_size;
  return k_ra8_ok;
}

/* ===========================================================================
 * Volume setup helpers
 * ===========================================================================
 */

static void put16(uint8_t* p, uint32_t off, uint16_t v)
{
  p[off]     = (uint8_t)(v & 0xFFU);
  p[off + 1] = (uint8_t)((v >> 8U) & 0xFFU);
}

/**
 * @brief Build a minimal valid FAT16 BPB at sector 0 of s_disk.
 *
 * @details SPC=1, 2 FATs, 32 FAT sectors, 16 root-directory entries,
 *          512-byte sectors, 8192-sector disk (4 MiB).  Matches the
 *          geometry used by test_ra8_fs_fat.c so shared assumptions hold.
 *
 * @pre s_disk.bytes is nullptr (previous volume freed or first call).
 * @post s_disk holds a calloc-zeroed 4 MiB image with valid BPB.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void build_fat16_volume(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
  s_disk.byte_count  = (uint32_t)k_cov_blocks_fat16 * (uint32_t)k_cov_block_size;
  s_disk.bytes       = (uint8_t*)calloc(1, s_disk.byte_count);
  s_disk.block_count = (uint32_t)k_cov_blocks_fat16;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
  uint8_t* bpb = s_disk.bytes;
  put16(bpb, 11U, (uint16_t)k_cov_block_size);
  bpb[13] = 1U;
  put16(bpb, 14U, 1U);
  bpb[16] = 2U;
  put16(bpb, 17U, 16U);
  put16(bpb, 19U, (uint16_t)k_cov_blocks_fat16);
  put16(bpb, 22U, 32U);
  bpb[510] = 0x55U;
  bpb[511] = 0xAAU;
}

/**
 * @brief Allocate and format an exFAT volume stored in s_disk.
 *
 * @details Allocates 65536 sectors (32 MiB) and calls ra8_fs_format
 *          to produce a mountable exFAT image.
 *
 * @pre s_disk.bytes is nullptr.
 * @post s_disk holds a formatted exFAT image.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void build_exfat_volume(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
  s_disk.byte_count  = (uint32_t)k_cov_blocks_exfat * (uint32_t)k_cov_block_size;
  s_disk.bytes       = (uint8_t*)calloc(1, s_disk.byte_count);
  s_disk.block_count = (uint32_t)k_cov_blocks_exfat;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed for exfat volume");
  }
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_exfat;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
}

/**
 * @brief Free the heap-backed sector store in s_disk.
 *
 * @pre None.
 * @post s_disk.bytes is nullptr.
 *
 * @note Not thread-safe.
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
 * @brief Listdir callback that counts visible entries.
 *
 * @param[in] name Unused.
 * @param[in] attr Unused.
 * @param[in] size Unused.
 * @param[in] ctx  Pointer to a uint32_t counter.
 *
 * @pre ctx is non-NULL and points to a uint32_t.
 * @post Counter at ctx is incremented by one.
 *
 * @note Trivially thread-safe.
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
 * @brief Swap the active backend in *h to the inject backend.
 *
 * @details Copies s_disk geometry into s_inject (sharing the byte array),
 *          then overwrites h->backend with the inject function pointers.
 *          The caller must restore h->backend when done.
 *
 * @param[in,out] h           Mounted FAT volume to redirect.
 * @param[in]     reads_left  Allowed reads before failure; k_cov_reads_inf
 *                            means unlimited.
 * @param[in]     writes_fail Non-zero to make all writes fail immediately.
 *
 * @pre h is non-NULL and mounted.
 * @pre s_disk.bytes is non-NULL.
 * @post h->backend redirects I/O through the inject functions.
 *
 * @note Restore with: h->backend = saved_backend.
 * @since 0.1.0
 */
static void swap_to_inject(ra8_fs_mount_t* h, uint32_t reads_left, uint8_t writes_fail)
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

/* ===========================================================================
 * Tests targeting priv_truncate_existing (lines 68-89)
 * ===========================================================================
 */

/**
 * @test test_reopen_write_truncates
 * @brief Re-opening an existing file in write mode calls priv_truncate_existing.
 *
 * @details
 * Creates "DATA.TXT", writes one byte (allocating a cluster), closes it,
 * then re-opens in write mode.  priv_open_existing is invoked with
 * mode==write, which calls priv_truncate_existing to free the old chain and
 * zero the directory-entry size.  The truncated file is verified to be empty.
 *
 * Lines targeted: 68,70,71,72,75,76,77,78,79,80,81,82,83,84,87,88,89
 *                 (priv_truncate_existing body) and 142,143 (write-mode
 *                 branch in priv_open_existing, non-error path).
 *
 * @par MC/DC:
 * Decision: `if (f->first_cluster >= k_cluster_first_data)` (line 70, 1 condition).
 * V1: file has data (first_cluster=2 >= 2) -> TRUE (free_chain called).
 * V2: not exercised here (empty-file truncate is the else-arm; covered by the
 * existing priv_create_new path leaving cluster=0).
 * N=1 condition, 1 independent vector sufficient per DO-178C 6.4.4.3.
 *
 * @pre Volume is formatted and accessible.
 * @post DATA.TXT contains zero bytes after the second open.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_reopen_write_truncates(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: reopen in write mode calls truncate");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f   = nullptr;
  uint8_t        one = (uint8_t)'A';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "DATA.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, &one, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* Re-open in write mode: exercises priv_open_existing -> priv_truncate_existing. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "DATA.TXT", k_ra8_fs_mode_write, &f));
  uint32_t sz = 99U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(f, &sz));
  TEST_ASSERT_EQ(0U, sz); /* truncated to empty */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: reopen in write mode calls truncate");
}

/* ===========================================================================
 * Tests targeting priv_open_existing modes (lines 128, 148, 149)
 * ===========================================================================
 */

/**
 * @test test_open_append_mode
 * @brief Opening an existing file in append mode sets offset to size.
 *
 * @details
 * Writes a 4-byte file, closes, then re-opens in append mode.  On append the
 * offset is advanced to size (line 148).  A subsequent ra8_fs_tell confirms
 * the position.  A read-mode re-open is also done to cover the else branch
 * (line 149) in priv_open_existing.
 *
 * Lines targeted: 148 (append body), 149 (else clause for read mode).
 *
 * @pre Volume is formatted and accessible.
 * @post APND.TXT has 4 bytes; append handle position equals 4.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_open_append_mode(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: open existing in append mode");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f          = nullptr;
  uint8_t        payload[4] = {1U, 2U, 3U, 4U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "APND.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, payload, sizeof(payload)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* Append mode: covers line 148 (f->offset = f->size_bytes). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "APND.TXT", k_ra8_fs_mode_append, &f));
  uint32_t pos = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_tell(f, &pos));
  TEST_ASSERT_EQ(4U, pos);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* Read mode: covers line 149 (else branch in priv_open_existing). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "APND.TXT", k_ra8_fs_mode_read, &f));
  pos = 99U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_tell(f, &pos));
  TEST_ASSERT_EQ(0U, pos);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: open existing in append mode");
}

/**
 * @test test_open_existing_no_mem
 * @brief File-table exhaustion returns k_ra8_err_no_mem from priv_open_existing.
 *
 * @details
 * Creates five files, opens four to fill all four file slots (k_ra8_fs_max_files
 * == 4), then tries to open the fifth existing file.  priv_alloc_file_slot
 * returns nullptr and priv_open_existing returns k_ra8_err_no_mem (line 128).
 *
 * Line targeted: 128.
 *
 * @pre Volume is formatted and accessible.
 * @post All five files were created; the fifth open attempt fails.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_open_existing_no_mem(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: open existing file with full file table");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* Create five files on disk so a fifth one exists to open. */
  ra8_fs_file_t* tmp = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.TXT", k_ra8_fs_mode_write, &tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "B.TXT", k_ra8_fs_mode_write, &tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "C.TXT", k_ra8_fs_mode_write, &tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "D.TXT", k_ra8_fs_mode_write, &tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "E.TXT", k_ra8_fs_mode_write, &tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));

  /* Fill all four file slots. */
  ra8_fs_file_t* fa = nullptr;
  ra8_fs_file_t* fb = nullptr;
  ra8_fs_file_t* fc = nullptr;
  ra8_fs_file_t* fd = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.TXT", k_ra8_fs_mode_read, &fa));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "B.TXT", k_ra8_fs_mode_read, &fb));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "C.TXT", k_ra8_fs_mode_read, &fc));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "D.TXT", k_ra8_fs_mode_read, &fd));

  /* Fifth open: existing file but no slot available -- line 128. */
  ra8_fs_file_t* fe = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_open(h, "E.TXT", k_ra8_fs_mode_read, &fe));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fa));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fb));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fc));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fd));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: open existing file with full file table");
}

/* ===========================================================================
 * Tests targeting priv_enter_subdir guards (lines 264, 273, 287)
 * ===========================================================================
 */

/**
 * @test test_enter_subdir_name_too_long
 * @brief A path component longer than k_ra8_fs_short_name_len is rejected.
 *
 * @details
 * ra8_fs_open with a path whose first component has 13 characters triggers the
 * `len > k_ra8_fs_short_name_len` check in priv_enter_subdir (line 264).
 *
 * Line targeted: 264.
 *
 * @pre Volume is formatted and accessible.
 * @post Open returns k_ra8_err_invalid_arg.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_enter_subdir_name_too_long(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: subdir component name too long");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* Component "TOOLONGNAME13" is 13 chars > k_ra8_fs_short_name_len (12). */
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_fs_open(h, "/TOOLONGNAME13/FILE.TXT", k_ra8_fs_mode_read, &f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: subdir component name too long");
}

/**
 * @test test_enter_subdir_invalid_83
 * @brief A component that is not an 8.3-representable name is rejected.
 *
 * @details
 * A component that begins with a dot (e.g. ".HIDDEN") cannot be packed into
 * an 8.3 name by priv_path_to_83 (returns 0), so priv_enter_subdir returns
 * k_ra8_err_invalid_arg (line 273).
 *
 * Line targeted: 273.
 *
 * @pre Volume is formatted and accessible.
 * @post Open returns k_ra8_err_invalid_arg.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_enter_subdir_invalid_83(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: subdir component fails 8.3 pack");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* ".HIDDEN" starts with a dot so priv_path_to_83 returns 0. */
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_fs_open(h, "/.HIDDEN/FILE.TXT", k_ra8_fs_mode_read, &f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: subdir component fails 8.3 pack");
}

/**
 * @test test_enter_subdir_zero_cluster
 * @brief A directory entry whose first cluster is < 2 triggers a protocol error.
 *
 * @details
 * Creates directory "CORR" (which lands at root LBA 65, offset 0), then
 * manually zeroes the cluster fields (DIR_FstClusHI at offset 20 and
 * DIR_FstClusLO at offset 26) in the backing byte array.  When a subsequent
 * ra8_fs_open tries to descend into "/CORR/FILE.TXT", priv_enter_subdir finds
 * first_cluster < k_cluster_first_data and returns k_ra8_err_protocol_error
 * (line 287).
 *
 * Line targeted: 287.
 *
 * @par MC/DC:
 * Decision: `if (cl < k_cluster_first_data)` (line 286, 1 condition).
 * V1: cl=0 (zeroed entry) -> TRUE -> k_ra8_err_protocol_error.
 * V2: normal directory -> FALSE -> success (exercised by test_resolve_dir_trailing_slash).
 * N=1 condition, 1 independent vector per DO-178C 6.4.4.3.
 *
 * @pre Volume is formatted and accessible.
 * @post Open returns k_ra8_err_protocol_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_enter_subdir_zero_cluster(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: subdir entry has cluster 0");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* Create the directory -- its dir entry goes at root LBA 65, offset 0. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/CORR"));

  /* Corrupt first-cluster fields in the raw backing store.
   * Root dir starts at sector 65 (reserved=1 + 2*fat_sz=64, hence LBA 65).
   * Entry 0 is at offset 0.  DIR_FstClusHI is at byte 20, DIR_FstClusLO at 26.
   * Zeroing them sets cluster to 0 which is < k_cluster_first_data (2). */
  uint32_t root_lba       = 65U;
  uint32_t off            = root_lba * (uint32_t)k_cov_block_size;
  s_disk.bytes[off + 20U] = 0U;
  s_disk.bytes[off + 21U] = 0U;
  s_disk.bytes[off + 26U] = 0U;
  s_disk.bytes[off + 27U] = 0U;

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_fs_open(h, "/CORR/FILE.TXT", k_ra8_fs_mode_read, &f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: subdir entry has cluster 0");
}

/* ===========================================================================
 * Tests targeting priv_resolve_parent depth (line 329)
 * ===========================================================================
 */

/**
 * @test test_resolve_parent_too_deep
 * @brief A path with 32 real nested directories hits the depth guard.
 *
 * @details
 * Creates 32 nested directories (/D00, /D00/D01, ...) so that every component
 * resolves successfully.  A final ra8_fs_open with the 33-component path causes
 * the depth loop in priv_resolve_parent to exhaust its bound (k_path_max_depth
 * == 32) and return k_ra8_err_invalid_arg (line 329).
 *
 * Line targeted: 329.
 *
 * @pre Volume is formatted and accessible.
 * @post Open returns k_ra8_err_invalid_arg.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_resolve_parent_too_deep(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: path depth exceeds k_path_max_depth");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* Build 32 nested directories.  Each component is "Dnn" (3 chars). */
  char     mkdir_path[200] = {};
  uint32_t base_len        = 0U;

  for (uint32_t i = 0U; i < 32U; i++) {
    char seg[8] = {};
    /* Format component as /Dnn -- avoids leading digits in 8.3 names. */
    seg[0]           = '/';
    seg[1]           = 'D';
    seg[2]           = (char)('0' + (i / 10U));
    seg[3]           = (char)('0' + (i % 10U));
    seg[4]           = '\0';
    uint32_t seg_len = 4U;
    if (base_len + seg_len >= (uint32_t)sizeof(mkdir_path)) {
      TEST_FAIL_FMT("%s", "path buffer overflow");
    }
    for (uint32_t k = 0U; k < seg_len; k++) {
      mkdir_path[base_len + k] = seg[k];
    }
    base_len += seg_len;
    mkdir_path[base_len] = '\0';
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, mkdir_path));
  }

  /* Append leaf "/LEAF.TXT" to form a 33-component path. */
  const char* leaf     = "/LEAF.TXT";
  uint32_t    leaf_len = 9U;
  if (base_len + leaf_len >= (uint32_t)sizeof(mkdir_path)) {
    TEST_FAIL_FMT("%s", "path buffer overflow for leaf");
  }
  for (uint32_t k = 0U; k < leaf_len; k++) {
    mkdir_path[base_len + k] = leaf[k];
  }
  mkdir_path[base_len + leaf_len] = '\0';

  /* 32 intermediate components all exist, so the depth loop runs out. */
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_open(h, mkdir_path, k_ra8_fs_mode_read, &f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: path depth exceeds k_path_max_depth");
}

/* ===========================================================================
 * Tests targeting priv_resolve_dir paths (lines 348, 355-356)
 * ===========================================================================
 */

/**
 * @test test_resolve_dir_error_path
 * @brief priv_resolve_dir propagates a not-found error from priv_resolve_parent.
 *
 * @details
 * ra8_fs_listdir with a multi-component path whose first component does not
 * exist causes priv_resolve_parent -> priv_enter_subdir to return
 * k_ra8_err_not_found, which priv_resolve_dir then propagates at line 348.
 *
 * Line targeted: 348.
 *
 * @pre Volume is formatted and accessible.
 * @post listdir returns k_ra8_err_not_found.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_resolve_dir_error_path(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: resolve_dir propagates not-found error");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  uint32_t count = 0U;
  /* "/NOMATCH/SUB" -- NOMATCH does not exist so priv_enter_subdir returns
   * not_found, propagated by priv_resolve_dir (line 348). */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_listdir(h, "/NOMATCH/SUB", count_cb, &count));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: resolve_dir propagates not-found error");
}

/**
 * @test test_resolve_dir_trailing_slash
 * @brief A trailing slash in a listdir path resolves to the parent directory.
 *
 * @details
 * Creates subdirectory "SUB", then calls ra8_fs_listdir with "/SUB/".  In
 * priv_resolve_dir, priv_resolve_parent enters "SUB" and returns with an
 * empty leaf pointer (the path ended in '/').  The `len == 0` branch at
 * line 354 is taken, setting *out = parent (line 355) and returning k_ra8_ok
 * (line 356).
 *
 * Lines targeted: 355, 356.
 *
 * @par MC/DC:
 * Decision: `if (len == 0U)` (line 354, 1 condition).
 * V1: path="/SUB/" -> trailing slash -> leaf="" -> len=0 -> TRUE (lines 355-356).
 * V2: path="/SUB"  -> leaf="SUB"   -> len=3  -> FALSE (priv_enter_subdir called).
 * N=1 condition, 1 independent vector per DO-178C 6.4.4.3.
 *
 * @pre Volume is formatted and accessible.
 * @post listdir on the trailing-slash path succeeds with an empty directory.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_resolve_dir_trailing_slash(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: trailing slash resolves to parent dir");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SUB"));

  uint32_t count = 0U;
  /* "/SUB/" -> trailing slash -> resolve to the SUB directory itself. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/SUB/", count_cb, &count));
  TEST_ASSERT_EQ(0U, count);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: trailing slash resolves to parent dir");
}

/* ===========================================================================
 * Tests targeting priv_create_new error paths (lines 401, 405)
 * ===========================================================================
 */

/**
 * @test test_root_dir_full
 * @brief Attempting to create a file in a full root directory returns no_mem.
 *
 * @details
 * The FAT16 volume has 16 root-directory entries.  After creating 16 files
 * a seventeenth creation attempt causes priv_dir_find_free to return
 * k_ra8_err_no_mem, which priv_create_new propagates at line 401.
 *
 * Line targeted: 401.
 *
 * @pre Volume is formatted and accessible.
 * @post The seventeenth ra8_fs_open returns k_ra8_err_no_mem.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_root_dir_full(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: root directory full returns no_mem");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* Create 16 files to fill all root directory slots. */
  ra8_fs_file_t* tmp       = nullptr;
  const char*    names[16] = {
    "F00.TXT",
    "F01.TXT",
    "F02.TXT",
    "F03.TXT",
    "F04.TXT",
    "F05.TXT",
    "F06.TXT",
    "F07.TXT",
    "F08.TXT",
    "F09.TXT",
    "F10.TXT",
    "F11.TXT",
    "F12.TXT",
    "F13.TXT",
    "F14.TXT",
    "F15.TXT",
  };
  for (uint32_t i = 0U; i < 16U; i++) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, names[i], k_ra8_fs_mode_write, &tmp));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));
  }

  /* Seventeenth file: root directory is full, priv_dir_find_free fails. */
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_open(h, "F16.TXT", k_ra8_fs_mode_write, &tmp));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: root directory full returns no_mem");
}

/**
 * @test test_create_no_file_slot
 * @brief File-table exhaustion during file creation returns no_mem.
 *
 * @details
 * Pre-creates four files and opens all four to fill the static file-slot
 * table.  A fifth open for a new (non-existing) file goes through
 * priv_create_new; priv_dir_find_free succeeds but priv_alloc_file_slot
 * returns nullptr and priv_create_new returns k_ra8_err_no_mem (line 405).
 *
 * Line targeted: 405.
 *
 * @pre Volume is formatted and accessible.
 * @post The fifth open returns k_ra8_err_no_mem.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_create_no_file_slot(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: create new file with full slot table");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* Pre-create four files, then open them all to consume all slots. */
  ra8_fs_file_t* tmp = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "PA.TXT", k_ra8_fs_mode_write, &tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "PB.TXT", k_ra8_fs_mode_write, &tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "PC.TXT", k_ra8_fs_mode_write, &tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "PD.TXT", k_ra8_fs_mode_write, &tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));

  ra8_fs_file_t* fa = nullptr;
  ra8_fs_file_t* fb = nullptr;
  ra8_fs_file_t* fc = nullptr;
  ra8_fs_file_t* fd = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "PA.TXT", k_ra8_fs_mode_read, &fa));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "PB.TXT", k_ra8_fs_mode_read, &fb));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "PC.TXT", k_ra8_fs_mode_read, &fc));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "PD.TXT", k_ra8_fs_mode_read, &fd));

  /* NEW.TXT does not exist; create path -> dir slot free but no file slot. */
  ra8_fs_file_t* fn = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_open(h, "NEW.TXT", k_ra8_fs_mode_write, &fn));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fa));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fb));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fc));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fd));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: create new file with full slot table");
}

/* ===========================================================================
 * Tests targeting ra8_fs_open guards (lines 453, 462)
 * ===========================================================================
 */

/**
 * @test test_open_not_in_use
 * @brief ra8_fs_open on an unmounted handle returns k_ra8_err_invalid_state.
 *
 * @details
 * Mounts, stores the handle, unmounts (setting in_use=0), then calls
 * ra8_fs_open on the stale handle.  Line 453 returns k_ra8_err_invalid_state.
 *
 * Line targeted: 453.
 *
 * @par MC/DC:
 * Decision: `if (handle->in_use == 0U)` (line 452, 1 condition).
 * V1: in_use=0 (unmounted) -> TRUE -> k_ra8_err_invalid_state (line 453).
 * V2: in_use=1 (mounted)   -> FALSE -> continue (all other tests).
 * N=1 condition, 1 independent vector per DO-178C 6.4.4.3.
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_open returns k_ra8_err_invalid_state.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_open_not_in_use(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: ra8_fs_open on unmounted handle");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));

  /* h is a pointer into the static pool; unmount sets in_use=0. */
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_open(h, "DEAD.TXT", k_ra8_fs_mode_read, &f));

  free_volume();
  TEST_END("ra8_fs_fat_file cov: ra8_fs_open on unmounted handle");
}

/**
 * @test test_open_exfat_dispatch
 * @brief ra8_fs_open on an exFAT volume dispatches to priv_exfat_open.
 *
 * @details
 * Formats and mounts an exFAT volume, then calls ra8_fs_open in read mode for
 * a non-existent file.  Line 462 (`return priv_exfat_open(...)`) is executed;
 * priv_exfat_open returns k_ra8_err_not_found since the file does not exist.
 *
 * Line targeted: 462.
 *
 * @pre s_disk has space for 65536 sectors (32 MiB).
 * @post ra8_fs_open returns k_ra8_err_not_found.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_open_exfat_dispatch(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: ra8_fs_open dispatches to exFAT");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_open(h, "EFILE.TXT", k_ra8_fs_mode_read, &f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: ra8_fs_open dispatches to exFAT");
}

/* ===========================================================================
 * Tests requiring I/O error injection (lines 73, 85, 143-145, 167, 409, 484)
 * ===========================================================================
 */

/**
 * @test test_free_chain_write_error
 * @brief A backend write failure in priv_free_chain surfaces through truncate.
 *
 * @details
 * Creates "WFAIL.TXT" with one byte of data (one cluster allocated), closes
 * it, swaps the mount's backend to the inject backend with writes_fail=1, then
 * re-opens in write mode.  priv_truncate_existing calls priv_free_chain which
 * attempts to write the freed FAT entry, fails, and returns k_ra8_err_hw_error.
 * priv_truncate_existing propagates it (lines 72-73) and priv_open_existing
 * sets in_use=0 and returns (lines 143-145).
 *
 * Lines targeted: 73, 143, 144, 145.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at line 72 (1 condition, truncate free chain).
 * V1: priv_free_chain fails (write error) -> TRUE  -> line 73 (return err).
 * V2: priv_free_chain succeeds            -> FALSE -> continue (test_reopen_write_truncates).
 *
 * Decision: `if (err != k_ra8_ok)` at line 143 (1 condition, truncate result in open_existing).
 * V1: truncate fails  -> TRUE  -> lines 144-145.
 * V2: truncate OK     -> FALSE -> continue (test_reopen_write_truncates).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_open returns k_ra8_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_free_chain_write_error(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: free_chain write error propagates through truncate");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f   = nullptr;
  uint8_t        one = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "WFAIL.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, &one, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* Save backend, install write-failing inject backend. */
  ra8_fs_backend_t saved = h->backend;
  swap_to_inject(h, (uint32_t)k_cov_reads_inf, 1U);

  /* Re-open in write mode: free_chain write fails -> lines 72-73 -> 143-145. */
  ra8_fs_file_t* f2 = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_open(h, "WFAIL.TXT", k_ra8_fs_mode_write, &f2));

  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: free_chain write error propagates through truncate");
}

/**
 * @test test_truncate_dir_read_error
 * @brief A backend read error during directory re-read in priv_truncate_existing.
 *
 * @details
 * Creates "RFAIL.TXT" with one byte, closes, swaps to the inject backend with
 * reads_left=2.  The re-open sequence consumes:
 *   Read 1 -- priv_dir_find reads the root-directory sector (LBA 65).
 *   Read 2 -- priv_free_chain reads the FAT sector (LBA 1).
 *   Read 3 -- priv_truncate_existing: priv_read_sector(dir LBA) -> FAIL.
 * Lines 84-85 in priv_truncate_existing are hit, then propagated through
 * priv_open_existing (lines 143-145).
 *
 * Lines targeted: 85, 143, 144, 145.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at line 84 (1 condition, read-sector in truncate).
 * V1: read fails  -> TRUE  -> line 85 (return err).
 * V2: read OK     -> FALSE -> continue (test_reopen_write_truncates).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_open returns k_ra8_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_truncate_dir_read_error(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: directory read error inside priv_truncate_existing");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f   = nullptr;
  uint8_t        one = (uint8_t)'Y';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "RFAIL.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, &one, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* Save backend, install 2-reads-then-fail inject backend (writes OK). */
  ra8_fs_backend_t saved = h->backend;
  swap_to_inject(h, 2U, 0U);

  /*
   * Read 1: priv_dir_find reads root dir sector (LBA 65) -> OK.
   * Read 2: priv_free_chain reads FAT sector (LBA 1)     -> OK.
   * Read 3: priv_truncate_existing reads LBA 65          -> FAIL.
   */
  ra8_fs_file_t* f2 = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_open(h, "RFAIL.TXT", k_ra8_fs_mode_write, &f2));

  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: directory read error inside priv_truncate_existing");
}

/**
 * @test test_write_dir_entry_read_error
 * @brief A backend read error in priv_write_new_dir_entry propagates to line 167.
 *
 * @details
 * Uses a fresh (empty) volume and swaps to the inject backend with reads_left=3.
 * The ra8_fs_open for a new file (write mode) consumes:
 *   Read 1 -- priv_dir_find reads root dir sector (LBA 65) -> not found.
 *   Read 2 -- priv_dir_find_long reads root dir sector     -> not found.
 *   Read 3 -- priv_dir_find_free reads root dir sector     -> finds free slot.
 *   Read 4 -- priv_write_new_dir_entry: priv_read_sector(free_lba) -> FAIL.
 * Line 167 (`return err`) in priv_write_new_dir_entry is hit, and the error
 * propagates to priv_create_new line 409 (`return err`).
 *
 * Lines targeted: 167, 409.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at line 166 (1 condition, read in write_new_dir_entry).
 * V1: read fails  -> TRUE  -> line 167 (return err).
 * V2: read OK     -> FALSE -> continue (all file-creation tests).
 *
 * Decision: `if (err != k_ra8_ok)` at line 408 (1 condition, write_new_dir_entry result).
 * V1: write_new_dir_entry fails -> TRUE  -> line 409.
 * V2: write_new_dir_entry OK    -> FALSE -> continue (all file-creation tests).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_open returns k_ra8_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_write_dir_entry_read_error(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: read error in priv_write_new_dir_entry");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* Swap to 3-reads-then-fail backend on an empty volume. */
  ra8_fs_backend_t saved = h->backend;
  swap_to_inject(h, 3U, 0U);

  /*
   * Read 1: priv_dir_find reads LBA 65         -> not found.
   * Read 2: priv_dir_find_long reads LBA 65    -> not found.
   * Read 3: priv_dir_find_free reads LBA 65    -> free slot found.
   * Read 4: priv_write_new_dir_entry(LBA 65)   -> FAIL -> line 167 -> line 409.
   */
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_open(h, "NDIR.TXT", k_ra8_fs_mode_write, &f));

  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: read error in priv_write_new_dir_entry");
}

/**
 * @test test_dir_find_io_error
 * @brief A backend read failure during priv_dir_find surfaces at ra8_fs_open line 484.
 *
 * @details
 * Swaps to the inject backend with reads_left=0 (fail immediately).  The
 * first read inside priv_dir_find fails, returning k_ra8_err_hw_error (which is
 * neither k_ra8_ok nor k_ra8_err_not_found).  ra8_fs_open propagates it at
 * line 484 (`return err`).
 *
 * Line targeted: 484.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_err_not_found)` at line 483 (1 condition).
 * V1: err=k_ra8_err_hw_error -> TRUE  -> line 484.
 * V2: err=k_ra8_err_not_found -> FALSE -> continue to creation path (all create tests).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_open returns k_ra8_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_dir_find_io_error(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: dir_find I/O error propagated at ra8_fs_open line 484");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* Swap to a backend that fails the very first read. */
  ra8_fs_backend_t saved = h->backend;
  swap_to_inject(h, 0U, 0U);

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_open(h, "FIND.TXT", k_ra8_fs_mode_read, &f));

  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: dir_find I/O error propagated at ra8_fs_open line 484");
}

/* ===========================================================================
 * Main
 * ===========================================================================
 */

int32_t main(void)
{
  test_reopen_write_truncates();
  test_open_append_mode();
  test_open_existing_no_mem();
  test_enter_subdir_name_too_long();
  test_enter_subdir_invalid_83();
  test_enter_subdir_zero_cluster();
  test_resolve_parent_too_deep();
  test_resolve_dir_error_path();
  test_resolve_dir_trailing_slash();
  test_root_dir_full();
  test_create_no_file_slot();
  test_open_not_in_use();
  test_open_exfat_dispatch();
  test_free_chain_write_error();
  test_truncate_dir_read_error();
  test_write_dir_entry_read_error();
  test_dir_find_io_error();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_fat_file_cov.c\n");
  return 0;
}
