/**
 * @file test_ra8_fs_mkdir.c
 * @brief Tests for ra8_fs directory creation + nested path resolution (#158).
 *
 * @details
 * Exercises `ra8_fs_mkdir` and the multi-component path walk that now backs
 * open / write_file / listdir / unlink / rename on FAT12/16/32:
 *   - create a directory and see it in the root listing (with the dir attr),
 *   - create and read back a file inside a subdirectory,
 *   - nested directories (a subdir of a subdir) and a file two levels deep,
 *   - error paths: NULL args, duplicate, missing parent, file-as-directory,
 *   - rename within a subdirectory, cross-directory move rejection, and unlink
 *     of a file inside a subdirectory.
 *
 * The volume is a synthetic in-memory FAT16 disk (hand-built BPB, zeroed data),
 * matching the harness in tests/test_ra8_fs_fat.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
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
 * @enum mkdir_payload_t
 * @brief Payload sizes and fill seeds for the files this fixture creates.
 *
 * @details
 * ::fill stamps `buf[i] = i * k_mkdir_seed_stride + seed`. The seeds are
 * arbitrary; what matters is that the three payloads differ, so a read that
 * returned another file's data fails the compare instead of passing.
 */
typedef enum : uint16_t {
  k_bpb_sig_lo        = 0x55U, /**< Boot-signature low byte.                    */
  k_bpb_sig_hi        = 0xAAU, /**< Boot-signature high byte.                   */
  k_byte_mask         = 0xFFU, /**< Low-byte mask used by the put16 helper.     */
  k_mkdir_seed_small  = 0x11U, /**< Seed for the sub-sector payload.            */
  k_mkdir_seed_medium = 0x55U, /**< Seed for the single-cluster payload.        */
  k_mkdir_seed_large  = 0x7AU, /**< Seed for the multi-cluster payload.         */
  k_mkdir_seed_stride = 5U,    /**< Stride of that generator.                   */
  k_mkdir_bytes_tiny  = 64U,   /**< Payload well under one sector.              */
  k_mkdir_bytes_small = 200U,  /**< Payload that still fits inside one cluster. */
} mkdir_payload_t;

/**
 * @enum ra8_fs_mkdir_disk_t
 * @brief Synthetic block-device sizes.
 */
typedef enum : uint32_t {
  k_disk_block_size   = 512U,       /**< Disk block size.   */
  k_disk_blocks_fat16 = 8U * 1024U, /**< Disk blocks fat16. */
} ra8_fs_mkdir_disk_t;

typedef struct {
  uint8_t* bytes;       /**< Bytes.       */
  uint32_t block_count; /**< Block count. */
  uint32_t byte_count;  /**< Byte count.  */
} mem_disk_t;

static mem_disk_t s_disk = {};

static ra8_err_t mem_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_disk_block_size],
         (size_t)count * (uint32_t)k_disk_block_size);
  return k_ra8_ok;
}

static ra8_err_t mem_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_disk_block_size],
         buf,
         (size_t)count * (uint32_t)k_disk_block_size);
  return k_ra8_ok;
}

static ra8_err_t mem_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  *block_count  = d->block_count;
  *block_size   = (uint32_t)k_disk_block_size;
  return k_ra8_ok;
}

static const ra8_fs_backend_t s_backend = {
  .read_block   = mem_read,
  .write_block  = mem_write,
  .get_capacity = mem_capacity,
  .ctx          = &s_disk,
};

static void put16(uint8_t* p, uint32_t off, uint16_t v)
{
  p[off]     = (uint8_t)(v & k_byte_mask);
  p[off + 1] = (uint8_t)((v >> 8) & k_byte_mask);
}

static void build_fat16_volume(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
  s_disk.byte_count  = (uint32_t)k_disk_blocks_fat16 * (uint32_t)k_disk_block_size;
  s_disk.bytes       = (uint8_t*)calloc(1, s_disk.byte_count);
  s_disk.block_count = (uint32_t)k_disk_blocks_fat16;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
  uint8_t* bpb = &s_disk.bytes[0];
  put16(bpb, k_bpb_off_bytes_per_sec, (uint16_t)k_disk_block_size);
  bpb[k_bpb_off_sec_per_clus] = 1U;
  put16(bpb, k_bpb_off_rsvd_sec_cnt, 1U);
  bpb[16] = 2U;
  put16(bpb, k_bpb_off_root_ent_cnt, 16U);
  put16(bpb, k_bpb_off_tot_sec16, (uint16_t)k_disk_blocks_fat16);
  put16(bpb, k_bpb_off_fat_sz16, 32U);
  bpb[k_bpb_off_sig_lo] = k_bpb_sig_lo;
  bpb[k_bpb_off_sig_hi] = k_bpb_sig_hi;
}

static void free_volume(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
}

/** @brief listdir scan context: counts entries and flags a target name/attr. */
typedef struct {
  const char* want;       /**< Name to look for.                  */
  uint32_t    count;      /**< Total entries reported.            */
  bool        found;      /**< want was reported.                 */
  uint8_t     found_attr; /**< Attribute byte of the found entry. */
} scan_ctx_t;

static void scan_cb(const char* name, uint8_t attr, uint32_t size, void* ctx)
{
  (void)size;
  scan_ctx_t* s = (scan_ctx_t*)ctx;
  s->count++;
  if (strcmp(name, s->want) == 0) {
    s->found      = true;
    s->found_attr = attr;
  }
}

/** @brief Fill `buf` with a deterministic pattern. */
static void fill(uint8_t* buf, uint32_t len, uint8_t seed)
{
  for (uint32_t i = 0; i < len; ++i) {
    buf[i] = (uint8_t)((i * k_mkdir_seed_stride) + seed);
  }
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- mkdir creates a directory, the duplicate
 * attempt is rejected, and the root listing reports it with the directory attr)
 */
static void test_mkdir_basic(void)
{
  TEST_BEGIN("mkdir basic + root listing");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/BOOKS"));
  TEST_ASSERT_EQ(k_ra8_err_exists, ra8_fs_mkdir(h, "/BOOKS"));

  scan_ctx_t sc = {.want = "BOOKS"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", scan_cb, &sc));
  TEST_ASSERT(sc.found);
  TEST_ASSERT((sc.found_attr & (uint8_t)k_ra8_fs_attr_directory) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("mkdir basic + root listing");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a file written through a subdirectory
 * path reads back byte-identical and appears in the subdirectory listing)
 */
static void test_file_in_subdir(void)
{
  TEST_BEGIN("file inside a subdirectory");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/BOOKS"));

  uint8_t data[k_mkdir_bytes_small];
  fill(data, sizeof(data), k_mkdir_seed_small);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "/BOOKS/A.TXT", data, (uint32_t)sizeof(data)));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "/BOOKS/A.TXT", k_ra8_fs_mode_read, &f));
  uint8_t  got[k_mkdir_bytes_small] = {};
  uint32_t got_len                  = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, got, (uint32_t)sizeof(got), &got_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(sizeof(data), got_len);
  TEST_ASSERT_EQ(0, memcmp(data, got, sizeof(data)));

  scan_ctx_t sc = {.want = "A.TXT"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/BOOKS", scan_cb, &sc));
  TEST_ASSERT(sc.found);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("file inside a subdirectory");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a directory nested two levels deep holds
 * a file that reads back byte-identical)
 */
static void test_nested_dirs(void)
{
  TEST_BEGIN("nested directories two levels deep");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/BOOKS"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/BOOKS/SCIFI"));

  uint8_t data[k_mkdir_bytes_tiny];
  fill(data, sizeof(data), k_mkdir_seed_medium);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_write_file(h, "/BOOKS/SCIFI/X.TXT", data, (uint32_t)sizeof(data)));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "/BOOKS/SCIFI/X.TXT", k_ra8_fs_mode_read, &f));
  uint8_t  got[k_mkdir_bytes_tiny] = {};
  uint32_t got_len                 = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, got, (uint32_t)sizeof(got), &got_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(sizeof(data), got_len);
  TEST_ASSERT_EQ(0, memcmp(data, got, sizeof(data)));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("nested directories two levels deep");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- each guard is an independent
 * single-condition check: NULL handle/path, missing parent, file-as-directory)
 */
static void test_mkdir_errors(void)
{
  TEST_BEGIN("mkdir error paths");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_mkdir(nullptr, "/X"));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_mkdir(h, nullptr));
  /* missing intermediate component */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_mkdir(h, "/NOPE/X"));
  /* an intermediate component that is a regular file, not a directory */
  uint8_t data[8] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "/F.TXT", data, (uint32_t)sizeof(data)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_mkdir(h, "/F.TXT/X"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("mkdir error paths");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- rename within a subdirectory succeeds,
 * a cross-directory move is rejected, and unlink inside a subdirectory removes
 * the file)
 */
static void test_subdir_rename_unlink(void)
{
  TEST_BEGIN("rename + unlink inside a subdirectory");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/BOOKS"));
  uint8_t data[16] = {};
  fill(data, sizeof(data), k_mkdir_seed_large);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "/BOOKS/A.TXT", data, (uint32_t)sizeof(data)));

  /* same-directory rename */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rename(h, "/BOOKS/A.TXT", "/BOOKS/B.TXT"));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "/BOOKS/B.TXT", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_open(h, "/BOOKS/A.TXT", k_ra8_fs_mode_read, &f));

  /* cross-directory move is not supported by the in-place rename */
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_fs_rename(h, "/BOOKS/B.TXT", "/C.TXT"));

  /* unlink inside the subdirectory */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "/BOOKS/B.TXT"));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_open(h, "/BOOKS/B.TXT", k_ra8_fs_mode_read, &f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("rename + unlink inside a subdirectory");
}

int32_t main(void)
{
  test_mkdir_basic();
  test_file_in_subdir();
  test_nested_dirs();
  test_mkdir_errors();
  test_subdir_rename_unlink();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_mkdir.c\n");
  return 0;
}
