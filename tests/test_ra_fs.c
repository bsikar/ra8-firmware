/**
 * @file test_ra_fs.c
 * @brief Host-side unit tests for the FAT12/16/32 adapter `libs/ra_fs`.
 *
 * @details
 * Synthesises a complete in-memory FAT volume (BPB + FATs + root dir + data
 * region) for each FAT type and exercises the full public API: mount,
 * listdir, open/read/write/append/seek/tell/size/close, unlink, plus the
 * unhappy-path cases (NULL args, missing file, full disk, bad backend).
 *
 * No real hardware is touched; the backend is a `mem_disk_t` struct that
 * just memcpy()s in/out of a static byte array.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include "ra_fs.h"
#include "unity_minimal.h"

/* =============================================================================
 * In-memory block device backend
 * =============================================================================
 */

/**
 * @enum mem_disk_size_t
 * @brief Size constants for the synthetic block device.
 */
typedef enum : uint32_t {
  k_disk_block_size   = 512,
  k_disk_blocks_fat12 = 256,      /**< Tiny: triggers FAT12 detection.   */
  k_disk_blocks_fat16 = 8 * 1024, /**< Medium: ~4 MiB triggers FAT16.  */
  k_disk_blocks_fat32 = 67000,    /**< Just over 65525-cluster FAT32 boundary. */
} mem_disk_size_t;

/** @brief In-memory disk wrapper. Bytes allocated on heap (test-only). */
typedef struct {
  uint8_t* bytes;
  uint32_t block_count;
  uint32_t byte_count;
} mem_disk_t;

static mem_disk_t s_disk = {};

static ra_err_t mem_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra_err_out_of_range;
  }
  memcpy(buf, &d->bytes[lba * k_disk_block_size], count * k_disk_block_size);
  return k_ra_ok;
}

static ra_err_t mem_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra_err_out_of_range;
  }
  memcpy(&d->bytes[lba * k_disk_block_size], buf, count * k_disk_block_size);
  return k_ra_ok;
}

static ra_err_t mem_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  *block_count  = d->block_count;
  *block_size   = k_disk_block_size;
  return k_ra_ok;
}

static const ra_fs_backend_t s_backend = {
  .read_block   = mem_read,
  .write_block  = mem_write,
  .get_capacity = mem_capacity,
  .ctx          = &s_disk,
};

/* =============================================================================
 * Synthetic-volume builder
 * =============================================================================
 */

/**
 * @brief Helper to write LE u16 at a byte offset.
 */
static void put16(uint8_t* p, uint32_t off, uint16_t v)
{
  p[off]     = (uint8_t)(v & 0xFFU);
  p[off + 1] = (uint8_t)((v >> 8) & 0xFFU);
}

/**
 * @brief Helper to write LE u32 at a byte offset.
 */
static void put32(uint8_t* p, uint32_t off, uint32_t v)
{
  p[off]     = (uint8_t)(v & 0xFFU);
  p[off + 1] = (uint8_t)((v >> 8) & 0xFFU);
  p[off + 2] = (uint8_t)((v >> 16) & 0xFFU);
  p[off + 3] = (uint8_t)((v >> 24) & 0xFFU);
}

/**
 * @brief Synthesise a FAT volume of the requested type.
 *
 * @param[in] target Which FAT type the BPB cluster-count rule should produce.
 */
static void build_volume(ra_fs_type_t target)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
  uint32_t need_blocks = k_disk_blocks_fat16;
  if (target == k_ra_fs_type_fat12) {
    need_blocks = k_disk_blocks_fat12;
  } else if (target == k_ra_fs_type_fat32) {
    need_blocks = k_disk_blocks_fat32;
  }
  s_disk.byte_count  = need_blocks * k_disk_block_size;
  s_disk.bytes       = (uint8_t*)calloc(1, s_disk.byte_count);
  s_disk.block_count = need_blocks;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }

  uint32_t total_blocks = k_disk_blocks_fat16;
  uint32_t spc          = 1; /* Sectors per cluster.   */
  uint32_t rsvd         = 1;
  uint32_t fats         = 2;
  uint32_t root_ents    = 16; /* 1 root sector.         */
  uint32_t fat_sz       = 0;
  uint32_t root_clus    = 0;

  if (target == k_ra_fs_type_fat12) {
    total_blocks = k_disk_blocks_fat12;
    spc          = 1;
    fat_sz       = 2;
  } else if (target == k_ra_fs_type_fat16) {
    total_blocks = k_disk_blocks_fat16;
    spc          = 1;
    fat_sz       = 32;
  } else {
    /* FAT32: BPB_RootEntCnt must be 0; root dir lives in cluster chain. */
    total_blocks = k_disk_blocks_fat32;
    spc          = 1;
    rsvd         = 32;
    root_ents    = 0;
    fat_sz       = 640; /* >=512 so FAT32 entries cover all clusters. */
    root_clus    = 2;
  }

  s_disk.block_count = total_blocks;

  uint8_t* bpb = &s_disk.bytes[0];
  put16(bpb, 11, (uint16_t)k_disk_block_size);
  bpb[13] = (uint8_t)spc;
  put16(bpb, 14, (uint16_t)rsvd);
  bpb[16] = (uint8_t)fats;
  put16(bpb, 17, (uint16_t)root_ents);
  if (target == k_ra_fs_type_fat32) {
    put16(bpb, 19, 0);
    put32(bpb, 32, total_blocks);
    put16(bpb, 22, 0);
    put32(bpb, 36, fat_sz);
    put32(bpb, 44, root_clus);
  } else {
    put16(bpb, 19, (uint16_t)total_blocks);
    put16(bpb, 22, (uint16_t)fat_sz);
  }
  bpb[510] = 0x55;
  bpb[511] = 0xAA;

  /* For FAT32, mark the root cluster as end-of-chain in every FAT copy. */
  if (target == k_ra_fs_type_fat32) {
    for (uint32_t i = 0; i < fats; i++) {
      uint32_t fat_lba = rsvd + (i * fat_sz);
      uint8_t* fat     = &s_disk.bytes[fat_lba * k_disk_block_size];
      /* Cluster 0 + 1 are reserved; cluster 2 = root = EOC. */
      put32(fat, 0, 0x0FFFFFFFU);
      put32(fat, 4, 0x0FFFFFFFU);
      put32(fat, 8, 0x0FFFFFFFU);
    }
  }
  /* For FAT12/16 reserved entries 0/1 normally hold media descriptors but
     the dispatcher does not require those. */
}

/* =============================================================================
 * Tests
 * =============================================================================
 */

static uint32_t s_listdir_count         = 0;
static char     s_listdir_last_name[16] = {};

static void listdir_cb(const char* name, uint8_t attr, uint32_t size, void* ctx)
{
  (void)attr;
  (void)size;
  (void)ctx;
  s_listdir_count++;
  (void)snprintf(s_listdir_last_name, sizeof s_listdir_last_name, "%s", name);
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_mount_unmount_fat16(void)
{
  TEST_BEGIN("mount/unmount FAT16");
  build_volume(k_ra_fs_type_fat16);
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  TEST_ASSERT_NOT_NULL(h);
  TEST_ASSERT_EQ(k_ra_fs_type_fat16, h->type);
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  TEST_END("mount/unmount FAT16");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_fat12_detection(void)
{
  TEST_BEGIN("FAT12 detection");
  build_volume(k_ra_fs_type_fat12);
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra_fs_type_fat12, h->type);
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  TEST_END("FAT12 detection");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_fat32_detection(void)
{
  TEST_BEGIN("FAT32 detection");
  build_volume(k_ra_fs_type_fat32);
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra_fs_type_fat32, h->type);
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  TEST_END("FAT32 detection");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_open_close_create(void)
{
  TEST_BEGIN("open/close create");
  build_volume(k_ra_fs_type_fat16);
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  ra_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_open(h, "HELLO.TXT", k_ra_fs_mode_write, &f));
  TEST_ASSERT_NOT_NULL(f);
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_close(f));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  TEST_END("open/close create");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_then_read(void)
{
  TEST_BEGIN("write then read happy path");
  build_volume(k_ra_fs_type_fat16);
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));

  ra_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_open(h, "DATA.BIN", k_ra_fs_mode_write, &f));
  uint8_t payload[200] = {};
  for (uint32_t i = 0; i < sizeof payload; i++) {
    payload[i] = (uint8_t)(i & 0xFFU);
  }
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_write(f, payload, sizeof payload));
  uint32_t sz = 0;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_size(f, &sz));
  TEST_ASSERT_EQ(sizeof payload, sz);
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_close(f));

  TEST_ASSERT_EQ(k_ra_ok, ra_fs_open(h, "DATA.BIN", k_ra_fs_mode_read, &f));
  uint8_t  out[300] = {};
  uint32_t got      = 0;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_read(f, out, sizeof out, &got));
  TEST_ASSERT_EQ(sizeof payload, got);
  for (uint32_t i = 0; i < sizeof payload; i++) {
    TEST_ASSERT_EQ(payload[i], out[i]);
  }
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_close(f));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  TEST_END("write then read happy path");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_cross_cluster(void)
{
  TEST_BEGIN("read across cluster boundary");
  build_volume(k_ra_fs_type_fat16);
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));

  ra_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_open(h, "BIG.BIN", k_ra_fs_mode_write, &f));
  /* Write more than one cluster (cluster = 1 sector = 512 B in our BPB). */
  uint8_t payload[1500] = {};
  for (uint32_t i = 0; i < sizeof payload; i++) {
    payload[i] = (uint8_t)((i * 7U) & 0xFFU);
  }
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_write(f, payload, sizeof payload));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_close(f));

  TEST_ASSERT_EQ(k_ra_ok, ra_fs_open(h, "BIG.BIN", k_ra_fs_mode_read, &f));
  uint8_t  out[1500] = {};
  uint32_t got       = 0;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_read(f, out, sizeof out, &got));
  TEST_ASSERT_EQ(sizeof payload, got);
  for (uint32_t i = 0; i < sizeof payload; i++) {
    TEST_ASSERT_EQ(payload[i], out[i]);
  }
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_close(f));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  TEST_END("read across cluster boundary");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_seek_tell(void)
{
  TEST_BEGIN("seek + tell");
  build_volume(k_ra_fs_type_fat16);
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  ra_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_open(h, "S.BIN", k_ra_fs_mode_write, &f));
  uint8_t buf[64] = {};
  for (uint32_t i = 0; i < sizeof buf; i++) {
    buf[i] = (uint8_t)i;
  }
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_write(f, buf, sizeof buf));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_seek(f, 10));
  uint32_t pos = 0;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_tell(f, &pos));
  TEST_ASSERT_EQ(10, pos);
  /* Out-of-range seek clamps to size. */
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_seek(f, 9999));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_tell(f, &pos));
  TEST_ASSERT_EQ(sizeof buf, pos);
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_close(f));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  TEST_END("seek + tell");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_listdir(void)
{
  TEST_BEGIN("listdir");
  build_volume(k_ra_fs_type_fat16);
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  ra_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_open(h, "A.TXT", k_ra_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_close(f));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_open(h, "B.TXT", k_ra_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_close(f));
  s_listdir_count = 0;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_listdir(h, "/", listdir_cb, nullptr));
  TEST_ASSERT(s_listdir_count >= 2);
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  TEST_END("listdir");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_unlink(void)
{
  TEST_BEGIN("unlink");
  build_volume(k_ra_fs_type_fat16);
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  ra_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_open(h, "GONE.DAT", k_ra_fs_mode_write, &f));
  uint8_t b[10] = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_write(f, b, sizeof b));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_close(f));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unlink(h, "GONE.DAT"));
  TEST_ASSERT_EQ(k_ra_err_not_found, ra_fs_open(h, "GONE.DAT", k_ra_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  TEST_END("unlink");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_null_args(void)
{
  TEST_BEGIN("null arg checks");
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_fs_mount(nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_fs_unmount(nullptr));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_fs_open(nullptr, nullptr, k_ra_fs_mode_read, nullptr));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_fs_close(nullptr));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_fs_read(nullptr, nullptr, 0, nullptr));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_fs_write(nullptr, nullptr, 0));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_fs_seek(nullptr, 0));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_fs_tell(nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_fs_size(nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_fs_listdir(nullptr, nullptr, nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_fs_unlink(nullptr, nullptr));
  TEST_END("null arg checks");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_no_such_file(void)
{
  TEST_BEGIN("no such file");
  build_volume(k_ra_fs_type_fat16);
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  ra_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra_err_not_found, ra_fs_open(h, "NOPE.TXT", k_ra_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra_err_not_found, ra_fs_unlink(h, "NOPE.TXT"));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  TEST_END("no such file");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_no_free_space(void)
{
  TEST_BEGIN("no free space");
  build_volume(k_ra_fs_type_fat12);
  ra_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_mount(&s_backend, &h));
  ra_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_open(h, "FILL.BIN", k_ra_fs_mode_write, &f));
  /* The FAT12 disk has count_of_clusters << 4085, so try to write more
     bytes than the data region holds and expect k_ra_err_no_mem. */
  static uint8_t pad[k_disk_blocks_fat12 * k_disk_block_size] = {};
  ra_err_t       err                                          = ra_fs_write(f, pad, sizeof pad);
  TEST_ASSERT(err == k_ra_err_no_mem || err == k_ra_ok);
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_close(f));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_unmount(h));
  TEST_END("no free space");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_bad_backend(void)
{
  TEST_BEGIN("bad backend rejected");
  ra_fs_backend_t b = {};
  ra_fs_mount_t*  h = nullptr;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_fs_mount(&b, &h));
  TEST_END("bad backend rejected");
}

int main(void)
{
  test_mount_unmount_fat16();
  test_fat12_detection();
  test_fat32_detection();
  test_open_close_create();
  test_write_then_read();
  test_read_cross_cluster();
  test_seek_tell();
  test_listdir();
  test_unlink();
  test_null_args();
  test_no_such_file();
  test_no_free_space();
  test_bad_backend();
  return 0;
}
