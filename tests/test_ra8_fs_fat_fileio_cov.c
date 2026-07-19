/**
 * @file test_ra8_fs_fat_fileio_cov.c
 * @brief Coverage booster for libs/ra8_fs/src/ra8_fs_fat_fileio.c.
 *
 * @details
 * Dedicated black-box companion executable that drives the error and edge
 * legs in ra8_fs_fat_fileio.c that no sibling test yet exercises. It calls
 * only the public ra8_fs API (ra8_fs_read / ra8_fs_write / ra8_fs_write_file /
 * ra8_fs_seek / ra8_fs_tell / ra8_fs_size), so its coverage merges into the one
 * production ra8_fs_fat_fileio.gcda that gcovr aggregates.
 *
 * Targeted lines (function -> line):
 *   - priv_skip_clusters       -> 59 (FAT-get error), 62/63 (EOC hit)
 *   - priv_read_one_chunk      -> 131 (skip-clusters error propagation)
 *   - ra8_fs_read               -> 188 (file not in use)
 *   - priv_walk_grow           -> 255 (FAT-get error), 265 (FAT-set error)
 *   - priv_write_into_sector   -> 310 (RMW read error)
 *   - priv_write_stream        -> 352 (first-cluster alloc error),
 *                                 374 (sector write error)
 *   - ra8_fs_write              -> 419 (zero-length), 429 (dir-entry read error)
 *   - ra8_fs_write_file         -> 439/442/445 (NULL args), 448 (mount not in
 *                                 use), 456 (open error), 461 (write error)
 *   - ra8_fs_seek               -> 495 (file not in use)
 *   - ra8_fs_tell               -> 533 (file not in use)
 *   - ra8_fs_size               -> 567 (file not in use)
 *
 * The in-memory block-device harness mirrors tests/test_ra8_fs_fat.c exactly.
 * An "inject" backend wraps the same byte array and can fail a specific LBA
 * read or write, fail all writes, or count reads down to a deterministic
 * point -- allowing precise I/O-error injection without SIGALRM or setitimer.
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

/**
 * @enum fat_bpb_field_t
 * @brief Byte offsets of the BPB fields this fixture writes, per the FAT specification.
 *
 * @details
 * The fixture hand-builds a boot sector instead of calling a formatter, so
 * every field is placed at its specified offset. The names are the
 * specification's own, so the layout can be checked against the document
 * without decoding the numbers.
 */
typedef enum : uint16_t {
  k_bpb_off_bytes_per_sec = 11U,  /**< BPB_BytsPerSec: bytes per sector.           */
  k_bpb_off_sec_per_clus  = 13U,  /**< BPB_SecPerClus: sectors per cluster.        */
  k_bpb_off_rsvd_sec_cnt  = 14U,  /**< BPB_RsvdSecCnt: sectors before the 1st FAT. */
  k_bpb_off_root_ent_cnt  = 17U,  /**< BPB_RootEntCnt: root-directory entries.     */
  k_bpb_off_tot_sec16     = 19U,  /**< BPB_TotSec16: total sectors.                */
  k_bpb_off_fat_sz16      = 22U,  /**< BPB_FATSz16: sectors per FAT.               */
  k_bpb_off_sig_lo        = 510U, /**< Low byte of the 0xAA55 boot signature.      */
  k_bpb_off_sig_hi        = 511U, /**< Its high byte.                              */
} fat_bpb_field_t;

/**
 * @enum fio_fixture_t
 * @brief Byte values the file-I/O coverage fixture stimulates the driver with.
 */
typedef enum : uint16_t {
  k_bpb_sig_lo = 0x55U, /**< Boot-signature low byte.  */
  k_bpb_sig_hi = 0xAAU, /**< Boot-signature high byte. */
  k_byte_mask  = 0xFFU, /**< Low-byte mask, shared by the put16 helper and the pattern generator. */
  k_fio_fat16_eoc_byte =
    0xFFU,                /**< Either byte of a FAT16 end-of-chain entry (0xFFFF), planted to cut a
                cluster chain short.                                                */
  k_fio_poison_out = 99U, /**< Poison written into an out-parameter before a call, so a callee that
              leaves it untouched fails the assertion instead of coasting on a
              stale zero.                                                           */
} fio_fixture_t;

/* ===========================================================================
 * Geometry and injection constants
 * ===========================================================================
 */

/**
 * @enum ra8_fio_cov_t
 * @brief Block geometry, buffer sizes, and injection sentinels.
 */
typedef enum : uint32_t {
  k_fio_block_size     = 512U,        /**< Bytes per logical block.              */
  k_fio_blocks_fat16   = 8U * 1024U,  /**< 4 MiB FAT16 card (matches siblings).  */
  k_fio_reads_inf      = 0xFFFFFFFFU, /**< Sentinel: unlimited reads.            */
  k_fio_lba_none       = 0xFFFFFFFFU, /**< Sentinel: no targeted LBA.            */
  k_fio_pat_max        = 2048U,       /**< Seed-pattern scratch size.            */
  k_fio_two_clusters   = 1024U,       /**< Two 512 B clusters at SPC=1.          */
  k_fio_small_write    = 4U,          /**< One partial-sector write.             */
  k_fio_reads_walk_get = 4U,          /**< Reads before walk_grow FAT-get (255). */
  k_fio_reads_walk_set = 9U,          /**< Reads before walk_grow FAT-set (265). */
} ra8_fio_cov_t;

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
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_fio_block_size],
         (size_t)count * (uint32_t)k_fio_block_size);
  return k_ra8_ok;
}

static ra8_err_t mem_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_fio_block_size],
         buf,
         (size_t)count * (uint32_t)k_fio_block_size);
  return k_ra8_ok;
}

static ra8_err_t mem_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  *block_count  = d->block_count;
  *block_size   = (uint32_t)k_fio_block_size;
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
 * @details Wraps the same byte array as ::mem_disk_t. A read fails when its
 *          LBA matches `fail_read_lba`, or when `reads_left` has counted down
 *          to zero. A write fails when `writes_fail` is set or its LBA matches
 *          `fail_write_lba`. Sentinels ::k_fio_lba_none / ::k_fio_reads_inf
 *          disable the respective mechanism.
 */
typedef struct {
  uint8_t* bytes;          /**< Shared with s_disk.bytes.                    */
  uint32_t block_count;    /**< Same geometry as s_disk.                     */
  uint32_t byte_count;     /**< Same geometry as s_disk.                     */
  uint32_t reads_left;     /**< Reads allowed; k_fio_reads_inf = unlimited.  */
  uint32_t fail_read_lba;  /**< Read at this LBA fails; k_fio_lba_none off.  */
  uint32_t fail_write_lba; /**< Write at this LBA fails; k_fio_lba_none off. */
  uint8_t  writes_fail;    /**< Non-zero -> all writes return hw_error.      */
} inject_disk_t;

static inject_disk_t s_inject = {};

static ra8_err_t inj_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  inject_disk_t* d = (inject_disk_t*)ctx;
  if (d->fail_read_lba != (uint32_t)k_fio_lba_none && lba == d->fail_read_lba) {
    return k_ra8_err_hw_error;
  }
  if (d->reads_left == 0U) {
    return k_ra8_err_hw_error;
  }
  if (d->reads_left != (uint32_t)k_fio_reads_inf) {
    d->reads_left--;
  }
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_fio_block_size],
         (size_t)count * (uint32_t)k_fio_block_size);
  return k_ra8_ok;
}

static ra8_err_t inj_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  inject_disk_t* d = (inject_disk_t*)ctx;
  if (d->writes_fail != 0U) {
    return k_ra8_err_hw_error;
  }
  if (d->fail_write_lba != (uint32_t)k_fio_lba_none && lba == d->fail_write_lba) {
    return k_ra8_err_hw_error;
  }
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_fio_block_size],
         buf,
         (size_t)count * (uint32_t)k_fio_block_size);
  return k_ra8_ok;
}

static ra8_err_t inj_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  inject_disk_t* d = (inject_disk_t*)ctx;
  *block_count     = d->block_count;
  *block_size      = (uint32_t)k_fio_block_size;
  return k_ra8_ok;
}

/* ===========================================================================
 * Volume setup helpers
 * ===========================================================================
 */

static void put16(uint8_t* p, uint32_t off, uint16_t v)
{
  p[off]     = (uint8_t)(v & k_byte_mask);
  p[off + 1] = (uint8_t)((v >> 8U) & k_byte_mask);
}

/**
 * @brief Build a minimal valid FAT16 BPB at sector 0 of s_disk.
 *
 * @details SPC=1, 2 FATs, 32 FAT sectors, 16 root-directory entries,
 *          512-byte sectors, 8192-sector disk (4 MiB). Matches the geometry
 *          used by test_ra8_fs_fat.c so shared assumptions hold: first FAT at
 *          LBA 1, root dir at LBA 65, first data sector (cluster 2) at LBA 66.
 *
 * @pre s_disk.bytes is nullptr (previous volume freed or first call).
 * @post s_disk holds a calloc-zeroed 4 MiB image with a valid BPB.
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
  s_disk.byte_count  = (uint32_t)k_fio_blocks_fat16 * (uint32_t)k_fio_block_size;
  s_disk.bytes       = (uint8_t*)calloc(1, s_disk.byte_count);
  s_disk.block_count = (uint32_t)k_fio_blocks_fat16;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
  uint8_t* bpb = s_disk.bytes;
  put16(bpb, k_bpb_off_bytes_per_sec, (uint16_t)k_fio_block_size);
  bpb[k_bpb_off_sec_per_clus] = 1U;
  put16(bpb, k_bpb_off_rsvd_sec_cnt, 1U);
  bpb[16] = 2U;
  put16(bpb, k_bpb_off_root_ent_cnt, 16U);
  put16(bpb, k_bpb_off_tot_sec16, (uint16_t)k_fio_blocks_fat16);
  put16(bpb, k_bpb_off_fat_sz16, 32U);
  bpb[k_bpb_off_sig_lo] = k_bpb_sig_lo;
  bpb[k_bpb_off_sig_hi] = k_bpb_sig_hi;
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
 * @brief Redirect a mounted volume's I/O through the inject backend.
 *
 * @details Copies s_disk geometry into s_inject (sharing the byte array),
 *          programs the requested failure controls, then overwrites
 *          h->backend with the inject function pointers. Restore the saved
 *          backend before unmount.
 *
 * @param[in,out] h              Mounted FAT volume to redirect.
 * @param[in]     reads_left     Reads allowed before failure; k_fio_reads_inf
 *                               means unlimited.
 * @param[in]     fail_read_lba  Read LBA to fail; k_fio_lba_none disables.
 * @param[in]     fail_write_lba Write LBA to fail; k_fio_lba_none disables.
 * @param[in]     writes_fail    Non-zero fails every write.
 *
 * @pre h is non-NULL and mounted; s_disk.bytes is non-NULL.
 * @post h->backend routes through the inject functions.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void use_inject(ra8_fs_mount_t* h,
                       uint32_t        reads_left,
                       uint32_t        fail_read_lba,
                       uint32_t        fail_write_lba,
                       uint8_t         writes_fail)
{
  s_inject.bytes          = s_disk.bytes;
  s_inject.block_count    = s_disk.block_count;
  s_inject.byte_count     = s_disk.byte_count;
  s_inject.reads_left     = reads_left;
  s_inject.fail_read_lba  = fail_read_lba;
  s_inject.fail_write_lba = fail_write_lba;
  s_inject.writes_fail    = writes_fail;
  h->backend.read_block   = inj_read;
  h->backend.write_block  = inj_write;
  h->backend.get_capacity = inj_capacity;
  h->backend.ctx          = &s_inject;
}

/**
 * @brief Create a file of @p len bytes with a deterministic byte pattern.
 *
 * @param[in,out] h    Mounted volume (normal backend).
 * @param[in]     name 8.3 file name.
 * @param[in]     len  Number of bytes to write (<= k_fio_pat_max).
 *
 * @pre h is mounted with the normal backend; len <= k_fio_pat_max.
 * @post The named file exists on the volume with @p len bytes.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void seed_file(ra8_fs_mount_t* h, const char* name, uint32_t len)
{
  static uint8_t s_pat[k_fio_pat_max] = {};
  for (uint32_t i = 0; i < len && i < (uint32_t)k_fio_pat_max; i++) {
    s_pat[i] = (uint8_t)(i & k_byte_mask);
  }
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, name, k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, s_pat, len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
}

/* ===========================================================================
 * ra8_fs_read / seek / tell / size: file-not-in-use legs
 * ===========================================================================
 */

/**
 * @test test_read_seek_tell_size_not_in_use
 * @brief Drive the `file->in_use == 0` guards of read/seek/tell/size
 *        (lines 188, 495, 533, 567).
 *
 * @details Opens a valid file, snapshots the handle, clears in_use on the
 *          copy, and confirms every accessor rejects the closed handle with
 *          k_ra8_err_invalid_state. No compound decision under test (each guard
 *          is a single condition on the copy path).
 */
static void test_read_seek_tell_size_not_in_use(void)
{
  TEST_BEGIN("ra8_fs_fileio: read/seek/tell/size not-in-use");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "NIU.TXT", k_ra8_fs_mode_write, &f));
  uint8_t payload = (uint8_t)'Q';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, &payload, 1U));

  ra8_fs_file_t closed = *f;
  closed.in_use        = 0U;
  uint8_t  buf[8]      = {};
  uint32_t got         = k_fio_poison_out;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_read(&closed, buf, sizeof(buf), &got)); /* 188 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_seek(&closed, 0U));                     /* 495 */
  uint32_t pos = k_fio_poison_out;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_tell(&closed, &pos)); /* 533 */
  uint32_t sz = k_fio_poison_out;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_size(&closed, &sz)); /* 567 */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fileio: read/seek/tell/size not-in-use");
}

/* ===========================================================================
 * ra8_fs_write: zero-length early return (line 419)
 * ===========================================================================
 */

/**
 * @test test_write_zero_length
 * @brief A zero-length write returns success without touching the backend
 *        (line 419).
 */
static void test_write_zero_length(void)
{
  TEST_BEGIN("ra8_fs_fileio: write zero length");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "ZERO.TXT", k_ra8_fs_mode_write, &f));
  uint8_t byte = (uint8_t)'0';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, &byte, 0U)); /* 419 */
  uint32_t sz = k_fio_poison_out;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(f, &sz));
  TEST_ASSERT_EQ(0, sz);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fileio: write zero length");
}

/* ===========================================================================
 * ra8_fs_write_file: argument, state, open, and write legs
 * ===========================================================================
 */

/**
 * @test test_write_file_guards
 * @brief Drive ra8_fs_write_file NULL-argument guards (439/442/445), the
 *        mount-not-in-use guard (448), and the open-error leg (456).
 *
 * @par MC/DC:
 * The three NULL guards are independent single-condition `if` statements, not
 * one compound decision, so each is exercised by one dedicated vector:
 * - handle=NULL             -> 439.
 * - path=NULL               -> 442.
 * - data=NULL               -> 445.
 * - mount->in_use == 0      -> 448 (snapshot copy with in_use cleared).
 * - open fails (bad 8.3)    -> 456 (leading-dot path rejected by open).
 */
static void test_write_file_guards(void)
{
  TEST_BEGIN("ra8_fs_fileio: write_file guards");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint8_t data[k_fio_small_write] = {1U, 2U, 3U, 4U};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_fs_write_file(nullptr, "F.TXT", data, sizeof(data)));               /* 439 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_write_file(h, nullptr, data, sizeof(data))); /* 442 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_fs_write_file(h, "F.TXT", nullptr, sizeof(data))); /* 445 */

  ra8_fs_mount_t closed_mount = *h;
  closed_mount.in_use         = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_fs_write_file(&closed_mount, "F.TXT", data, sizeof(data))); /* 448 */

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_write_file(h, ".TXT", data, sizeof(data))); /* 456 */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fileio: write_file guards");
}

/**
 * @test test_write_file_write_error
 * @brief ra8_fs_write_file where the internal write fails but open and close
 *        succeed (line 461).
 *
 * @details Fails only the first data sector's write. The open (which touches
 *          the root-directory sector only) succeeds, the FAT allocation writes
 *          succeed, and the read-modify-write read succeeds, so the failure is
 *          the data-sector write inside ra8_fs_write. ra8_fs_close performs no
 *          I/O, so the write error is returned at line 461.
 */
static void test_write_file_write_error(void)
{
  TEST_BEGIN("ra8_fs_fileio: write_file write error");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const ra8_fs_backend_t saved                   = h->backend;
  const uint8_t          data[k_fio_small_write] = {5U, 6U, 7U, 8U};
  use_inject(h, k_fio_reads_inf, k_fio_lba_none, h->first_data_lba, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 ra8_fs_write_file(h, "WERR.BIN", data, sizeof(data))); /* 461 */
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fileio: write_file write error");
}

/* ===========================================================================
 * ra8_fs_read: cluster-walk error and end-of-chain legs
 * ===========================================================================
 */

/**
 * @test test_read_walk_fat_get_error
 * @brief A FAT read failure during the read cluster-walk propagates out
 *        (priv_skip_clusters line 59, priv_read_one_chunk line 131).
 *
 * @details Seeds a two-cluster file, re-opens it read-only, then fails reads
 *          of the first FAT sector. The first chunk (cluster index 0, no walk)
 *          reads a data sector and succeeds; advancing into cluster index 1
 *          forces priv_skip_clusters to read the FAT sector, which fails.
 */
static void test_read_walk_fat_get_error(void)
{
  TEST_BEGIN("ra8_fs_fileio: read walk FAT-get error");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  seed_file(h, "RWALK.BIN", k_fio_two_clusters);
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "RWALK.BIN", k_ra8_fs_mode_read, &f));

  const ra8_fs_backend_t saved = h->backend;
  use_inject(h, k_fio_reads_inf, h->first_fat_lba, k_fio_lba_none, 0U);
  uint8_t  buf[k_fio_two_clusters] = {};
  uint32_t got                     = k_fio_poison_out;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_read(f, buf, sizeof(buf), &got)); /* 59, 131 */
  h->backend = saved;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fileio: read walk FAT-get error");
}

/**
 * @test test_read_walk_hits_eoc
 * @brief The read cluster-walk hitting end-of-chain before the target cluster
 *        returns invalid_state (priv_skip_clusters lines 62/63 -> line 131).
 *
 * @details Seeds a two-cluster file, then corrupts the first cluster's FAT
 *          entry to an end-of-chain marker. Seeking into cluster index 1 and
 *          reading walks one cluster, finds the premature EOC, and reports it.
 */
static void test_read_walk_hits_eoc(void)
{
  TEST_BEGIN("ra8_fs_fileio: read walk hits EOC");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  seed_file(h, "REOC.BIN", k_fio_two_clusters);
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "REOC.BIN", k_ra8_fs_mode_read, &f));

  /* Corrupt FAT copy 0 entry for the first cluster to a FAT16 EOC marker. */
  const uint32_t fat_byte =
    (h->first_fat_lba * (uint32_t)k_fio_block_size) + (f->first_cluster * 2U);
  s_disk.bytes[fat_byte]      = k_fio_fat16_eoc_byte;
  s_disk.bytes[fat_byte + 1U] = k_fio_fat16_eoc_byte;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_seek(f, (uint32_t)k_fio_block_size));
  uint8_t  buf[k_fio_block_size] = {};
  uint32_t got                   = k_fio_poison_out;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_read(f, buf, sizeof(buf), &got)); /* 62,63,131 */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fileio: read walk hits EOC");
}

/* ===========================================================================
 * ra8_fs_write: streaming error legs
 * ===========================================================================
 */

/**
 * @test test_write_stream_alloc_error
 * @brief The first-cluster allocation failing inside the write stream
 *        propagates out (priv_write_stream line 352).
 *
 * @details Opens a fresh file (first_cluster unallocated), then fails reads of
 *          the FAT sector. The first thing ra8_fs_write does is allocate the
 *          head cluster, which scans the FAT; that read fails.
 */
static void test_write_stream_alloc_error(void)
{
  TEST_BEGIN("ra8_fs_fileio: write stream alloc error");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "WALLOC.BIN", k_ra8_fs_mode_write, &f));

  const ra8_fs_backend_t saved                   = h->backend;
  const uint8_t          data[k_fio_small_write] = {9U, 10U, 11U, 12U};
  use_inject(h, k_fio_reads_inf, h->first_fat_lba, k_fio_lba_none, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_write(f, data, sizeof(data))); /* 352 */
  h->backend = saved;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fileio: write stream alloc error");
}

/**
 * @test test_write_into_sector_read_error
 * @brief The read-modify-write read failing propagates out
 *        (priv_write_into_sector line 310).
 *
 * @details Fails reads of the first data sector. FAT allocation reads/writes
 *          succeed; the read-modify-write read of the target data sector then
 *          fails.
 */
static void test_write_into_sector_read_error(void)
{
  TEST_BEGIN("ra8_fs_fileio: write_into_sector read error");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "WRMW.BIN", k_ra8_fs_mode_write, &f));

  const ra8_fs_backend_t saved                   = h->backend;
  const uint8_t          data[k_fio_small_write] = {13U, 14U, 15U, 16U};
  use_inject(h, k_fio_reads_inf, h->first_data_lba, k_fio_lba_none, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_write(f, data, sizeof(data))); /* 310 */
  h->backend = saved;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fileio: write_into_sector read error");
}

/**
 * @test test_write_into_sector_write_error
 * @brief The sector write failing propagates out of the write stream
 *        (priv_write_stream line 374 via priv_write_into_sector).
 *
 * @details Fails only the first data sector's write. FAT writes and the
 *          read-modify-write read succeed; the merged sector write fails.
 */
static void test_write_into_sector_write_error(void)
{
  TEST_BEGIN("ra8_fs_fileio: write_into_sector write error");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "WSEC.BIN", k_ra8_fs_mode_write, &f));

  const ra8_fs_backend_t saved                   = h->backend;
  const uint8_t          data[k_fio_small_write] = {17U, 18U, 19U, 20U};
  use_inject(h, k_fio_reads_inf, k_fio_lba_none, h->first_data_lba, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_write(f, data, sizeof(data))); /* 374 */
  h->backend = saved;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fileio: write_into_sector write error");
}

/**
 * @test test_write_dir_entry_read_error
 * @brief The directory-entry read after a successful stream write fails
 *        (ra8_fs_write line 429).
 *
 * @details Fails reads of the file's directory-entry sector. The stream write
 *          (FAT + data sectors) completes; the follow-up read of the directory
 *          entry to patch cluster and size then fails.
 */
static void test_write_dir_entry_read_error(void)
{
  TEST_BEGIN("ra8_fs_fileio: write dir-entry read error");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "WDIR.BIN", k_ra8_fs_mode_write, &f));

  const ra8_fs_backend_t saved                   = h->backend;
  const uint8_t          data[k_fio_small_write] = {21U, 22U, 23U, 24U};
  use_inject(h, k_fio_reads_inf, f->dir_entry_lba, k_fio_lba_none, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_write(f, data, sizeof(data))); /* 429 */
  h->backend = saved;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fileio: write dir-entry read error");
}

/* ===========================================================================
 * priv_walk_grow internal error legs (lines 255, 265)
 * ===========================================================================
 */

/**
 * @test test_walk_grow_fat_get_error
 * @brief A FAT read failing inside priv_walk_grow propagates out (line 255).
 *
 * @details Writes two clusters to a fresh file in one call. The first cluster
 *          (index 0) allocates and writes with four backend reads (FAT scan,
 *          two FAT-copy read-modify-writes, and the data read-modify-write).
 *          Advancing into cluster index 1 makes priv_walk_grow issue the fifth
 *          read (the FAT-get for the chain head); failing after four reads
 *          exercises the FAT-get error leg. Asserting on the error code (not a
 *          line) keeps the test robust to exact read ordering.
 */
static void test_walk_grow_fat_get_error(void)
{
  TEST_BEGIN("ra8_fs_fileio: walk_grow FAT-get error");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "WGGET.BIN", k_ra8_fs_mode_write, &f));

  static uint8_t         s_data[k_fio_two_clusters] = {};
  const ra8_fs_backend_t saved                      = h->backend;
  use_inject(h, k_fio_reads_walk_get, k_fio_lba_none, k_fio_lba_none, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_write(f, s_data, sizeof(s_data))); /* 255 */
  h->backend = saved;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fileio: walk_grow FAT-get error");
}

/**
 * @test test_walk_grow_fat_set_error
 * @brief A FAT write failing inside priv_walk_grow after growing the chain
 *        propagates out (line 265).
 *
 * @details Same two-cluster write as the FAT-get case, but reads are allowed
 *          to run until the chain-link FAT-set for the newly grown cluster,
 *          whose read-modify-write read is failed. Asserting on the error code
 *          keeps the test robust to exact read ordering.
 */
static void test_walk_grow_fat_set_error(void)
{
  TEST_BEGIN("ra8_fs_fileio: walk_grow FAT-set error");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "WGSET.BIN", k_ra8_fs_mode_write, &f));

  static uint8_t         s_data[k_fio_two_clusters] = {};
  const ra8_fs_backend_t saved                      = h->backend;
  use_inject(h, k_fio_reads_walk_set, k_fio_lba_none, k_fio_lba_none, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_write(f, s_data, sizeof(s_data))); /* 265 */
  h->backend = saved;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fileio: walk_grow FAT-set error");
}

/**
 * @test test_read_cache_unset_after_write
 * @brief The read accelerator's "cache unset" leg: a read right after a write on
 *        the same handle walks from the chain head (line 124, first condition
 *        false).
 *
 * @details A write resets `file->walk_cache_cluster` to 0 (below the first data
 *          cluster), so the next read chunk cannot resume from a cached waypoint
 *          and must walk from `first_cluster`. Writing two clusters then seeking
 *          to 0 and reading on the SAME handle drives that leg. A fresh
 *          read-mode open would instead seed the cache to `first_cluster` (first
 *          condition true), which the sibling sequential-read tests cover.
 *
 * @par MC/DC:
 * Decision (in `priv_read_one_chunk`): `(walk_cache_cluster >=
 * k_cluster_first_data) && (cluster_idx_now >= walk_cache_idx)` (2 conditions).
 * - V1 (here): walk_cache_cluster == 0 -> C1 false (short-circuit) -> walk from
 *   the chain head.
 * - V2: cache set, target at/ahead -> both true (resume) -- sibling sequential
 *   read.
 * - V3: cache set, target behind   -> C1 true, C2 false -- sibling backward seek.
 * Pair (V1,V2) proves the cache-set operand independently moves the outcome.
 */
static void test_read_cache_unset_after_write(void)
{
  TEST_BEGIN("ra8_fs_fileio: read after write walks from head (cache unset)");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "RDCACHE.BIN", k_ra8_fs_mode_write, &f));

  static uint8_t s_wr[k_fio_two_clusters] = {};
  for (uint32_t i = 0; i < (uint32_t)k_fio_two_clusters; i++) {
    s_wr[i] = (uint8_t)(i & k_byte_mask);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, s_wr, sizeof(s_wr))); /* resets walk cache to 0 */

  /* Same handle: seek to the start and read back. The first read chunk sees
   * walk_cache_cluster == 0, so the cache-set condition (C1) is false. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_seek(f, 0U));
  static uint8_t s_rd[k_fio_two_clusters] = {};
  uint32_t       got                      = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, s_rd, sizeof(s_rd), &got));
  TEST_ASSERT_EQ(k_fio_two_clusters, got);
  TEST_ASSERT_EQ(0, memcmp(s_wr, s_rd, sizeof(s_wr)));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fileio: read after write walks from head (cache unset)");
}

int32_t main(void)
{
  test_read_seek_tell_size_not_in_use();
  test_read_cache_unset_after_write();
  test_write_zero_length();
  test_write_file_guards();
  test_write_file_write_error();
  test_read_walk_fat_get_error();
  test_read_walk_hits_eoc();
  test_write_stream_alloc_error();
  test_write_into_sector_read_error();
  test_write_into_sector_write_error();
  test_write_dir_entry_read_error();
  test_walk_grow_fat_get_error();
  test_walk_grow_fat_set_error();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_fat_fileio_cov.c\n");
  return 0;
}
