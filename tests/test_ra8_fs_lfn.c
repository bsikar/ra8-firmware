/**
 * @file test_ra8_fs_lfn.c
 * @brief VFAT long-filename (LFN) read support for ra8_fs (#101).
 *
 * @details
 * Hand-builds a FAT16 RAM volume whose root directory holds one file written as
 * `mybook.epub` -- i.e. an LFN entry chain (attr 0x0F) immediately before its
 * 8.3 alias `MYBOOK~1.EPU`. Verifies that ra8_fs_open resolves the file by its
 * long name *and* by its 8.3 alias, that a wrong name still misses, and that
 * ra8_fs_listdir reports the long name. The 8.3-only behaviour is unchanged
 * (covered by tests/test_ra8_fs_fat.c).
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

/* --- RAM block device + minimal FAT16 volume (mirrors test_ra8_fs_fat.c) --- */

typedef enum : uint32_t {
  k_block_size    = 512U,                                      /**< Block size.       */
  k_blocks        = 8U * 1024U,                                /**< Blocks.           */
  k_reserved      = 1U,                                        /**< Reserved.         */
  k_num_fats      = 2U,                                        /**< Number fats.      */
  k_sectors_fat   = 32U,                                       /**< Sectors fat.      */
  k_root_lba      = k_reserved + (k_num_fats * k_sectors_fat), /**< Root lba (= 65).  */
  k_dir_entry_len = 32U,                                       /**< Dir entry length. */
} fs_lfn_geom_t;

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
         &d->bytes[(size_t)lba * (uint32_t)k_block_size],
         (size_t)count * (uint32_t)k_block_size);
  return k_ra8_ok;
}

static ra8_err_t mem_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_block_size],
         buf,
         (size_t)count * (uint32_t)k_block_size);
  return k_ra8_ok;
}

static ra8_err_t mem_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  *block_count  = d->block_count;
  *block_size   = (uint32_t)k_block_size;
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
  p[off]     = (uint8_t)(v & 0xFFU);
  p[off + 1] = (uint8_t)((v >> 8) & 0xFFU);
}

/** @brief The 8.3 short-name checksum the LFN chain must carry. */
static uint8_t sfn_checksum(const uint8_t* name83)
{
  uint8_t sum = 0U;
  for (uint32_t i = 0U; i < 11U; i++) {
    sum =
      (uint8_t)((((sum & 1U) != 0U) ? 0x80U : 0U) + (uint32_t)(sum >> 1U) + (uint32_t)name83[i]);
  }
  return sum;
}

/* The packed 8.3 alias for "mybook.epub": base "MYBOOK~1", ext "EPU". */
static const uint8_t k_alias83[11] = {'M', 'Y', 'B', 'O', 'O', 'K', '~', '1', 'E', 'P', 'U'};

/** @brief Build a FAT16 volume with the LFN chain + 8.3 entry for mybook.epub. */
static void build_volume_with_lfn(void)
{
  free(s_disk.bytes);
  s_disk.byte_count  = (uint32_t)k_blocks * (uint32_t)k_block_size;
  s_disk.bytes       = (uint8_t*)calloc(1U, s_disk.byte_count);
  s_disk.block_count = (uint32_t)k_blocks;
  TEST_ASSERT(s_disk.bytes != nullptr);

  uint8_t* bpb = &s_disk.bytes[0];
  put16(bpb, 11U, (uint16_t)k_block_size);
  bpb[13] = 1U;                          /* sectors/cluster  */
  put16(bpb, 14U, (uint16_t)k_reserved); /* reserved sectors */
  bpb[16] = (uint8_t)k_num_fats;
  put16(bpb, 17U, 16U);                /* root dir entries */
  put16(bpb, 19U, (uint16_t)k_blocks); /* total sectors    */
  put16(bpb, 22U, (uint16_t)k_sectors_fat);
  bpb[510] = 0x55U;
  bpb[511] = 0xAAU;

  /* Root directory at LBA 65: slot 0 = LFN("mybook.epub"), slot 1 = 8.3 entry. */
  uint8_t* root = &s_disk.bytes[(size_t)(uint32_t)k_root_lba * (uint32_t)k_block_size];

  /* --- slot 0: single LFN entry (the name is 11 chars, fits one entry) --- */
  uint8_t* lfn = &root[0];
  lfn[0]       = 0x41U; /* order 1 | last-logical (0x40) */
  lfn[11]      = 0x0FU; /* LFN attribute                 */
  lfn[12]      = 0x00U;
  lfn[13]      = sfn_checksum(k_alias83);
  put16(lfn, 26U, 0U);
  const char* const lname = "mybook.epub"; /* 11 chars */
  /* UTF-16LE char slot byte-offsets within a 32-byte LFN entry. */
  static const uint8_t k_off[13] = {1U, 3U, 5U, 7U, 9U, 14U, 16U, 18U, 20U, 22U, 24U, 28U, 30U};
  for (uint32_t i = 0U; i < 13U; i++) {
    if (i < strlen(lname)) {
      put16(lfn, (uint32_t)k_off[i], (uint16_t)(uint8_t)lname[i]);
    } else if (i == strlen(lname)) {
      put16(lfn, (uint32_t)k_off[i], 0x0000U); /* name terminator */
    } else {
      put16(lfn, (uint32_t)k_off[i], 0xFFFFU); /* padding */
    }
  }

  /* --- slot 1: the 8.3 short entry (zero-length file is enough to open) --- */
  uint8_t* sfn = &root[(uint32_t)k_dir_entry_len];
  memcpy(sfn, k_alias83, 11U);
  sfn[11] = 0x20U;     /* ARCHIVE                            */
  put16(sfn, 26U, 0U); /* first cluster low = 0 (empty file) */
  put16(sfn, 20U, 0U); /* first cluster high                 */
  sfn[28] = 0U;        /* file size = 0                      */
  sfn[29] = 0U;
  sfn[30] = 0U;
  sfn[31] = 0U;
  /* slot 2 stays 0x00 -> end-of-directory. */
}

static void free_volume(void)
{
  free(s_disk.bytes);
  s_disk.bytes = nullptr;
}

/* --- listdir capture --- */

typedef struct {
  char    last[64]; /**< Last.  */
  uint8_t count;    /**< Count. */
} listdir_capture_t;

static void capture_cb(const char* name, uint8_t attr, uint32_t size, void* ctx)
{
  (void)attr;
  (void)size;
  listdir_capture_t* c = (listdir_capture_t*)ctx;
  (void)snprintf(c->last, sizeof(c->last), "%s", name);
  c->count++;
}

/**
 * @test test_lfn_open_by_long_and_short
 * @brief A file written as mybook.epub opens by its long name AND by its 8.3
 *        alias; a wrong long name still misses.
 *
 * @par MC/DC: not applicable -- name-resolution acceptance path; the open guard
 *      decisions are covered in tests/test_ra8_fs_fat.c.
 */
static void test_lfn_open_by_long_and_short(void)
{
  TEST_BEGIN("ra8_fs LFN: open by long name + 8.3 alias");
  build_volume_with_lfn();
  ra8_fs_mount_t* mount = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &mount));

  ra8_fs_file_t* f = nullptr;
  /* Long name (4-char extension -- unreachable via 8.3) resolves now. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "mybook.epub", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  /* Case-insensitive long-name match. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "MyBook.EPUB", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  /* The 8.3 alias still opens the same entry. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "MYBOOK~1.EPU", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  /* A different long name misses. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_open(mount, "other.epub", k_ra8_fs_mode_read, &f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(mount));
  free_volume();
  TEST_END("ra8_fs LFN: open by long name + 8.3 alias");
}

/**
 * @test test_lfn_listdir_reports_long_name
 * @brief ra8_fs_listdir reports the reassembled long name, not the 8.3 alias.
 *
 * @par MC/DC: not applicable -- enumeration acceptance path.
 */
static void test_lfn_listdir_reports_long_name(void)
{
  TEST_BEGIN("ra8_fs LFN: listdir reports the long name");
  build_volume_with_lfn();
  ra8_fs_mount_t* mount = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &mount));

  listdir_capture_t cap = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(mount, "/", capture_cb, &cap));
  TEST_ASSERT_EQ(1U, cap.count);
  TEST_ASSERT_EQ(0, strcmp(cap.last, "mybook.epub"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(mount));
  free_volume();
  TEST_END("ra8_fs LFN: listdir reports the long name");
}

int32_t main(void)
{
  test_lfn_open_by_long_and_short();
  test_lfn_listdir_reports_long_name();
  return 0;
}
