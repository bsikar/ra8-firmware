/**
 * @file test_ra8_fs_fat_variants.c
 * @brief Focused FAT variant and directory edge coverage for ra8_fs_fat.c.
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
 * @enum fs_fat_variants_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_fs_fat_variants_bpb_55              = 0x55U,
  k_fs_fat_variants_bpb_aa              = 0xAAU,
  k_fs_fat_variants_e_10                = 10U,
  k_fs_fat_variants_fill_short_entry_11 = 11,
  k_fs_fat_variants_fill_short_entry_99 = 99U,
  k_fs_fat_variants_i_0f                = 0x0FU,
  k_fs_fat_variants_put16_11            = 11U,
  k_fs_fat_variants_put16_14            = 14U,
  k_fs_fat_variants_put16_17            = 17U,
  k_fs_fat_variants_put16_19            = 19U,
  k_fs_fat_variants_put16_22            = 22U,
  k_fs_fat_variants_put32_28            = 28U,
  k_fs_fat_variants_put32_36            = 36U,
  k_fs_fat_variants_put32_44            = 44U,
  k_fs_fat_variants_root0_e5            = 0xE5U,
  k_fs_fat_variants_v_24                = 24,
  k_fs_fat_variants_v_ff                = 0xFFU,
  k_fs_fat_variants_val_13              = 13,
  k_fs_fat_variants_val_24              = 24U,
  k_fs_fat_variants_val_a0              = 0xA0U,
} fs_fat_variants_uint8_const_t;

/**
 * @enum fs_fat_variants_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_fs_fat_variants_blocks_ffff = 0xFFFFU,
  k_fs_fat_variants_val_510     = 510,
  k_fs_fat_variants_val_511     = 511,
  k_fs_fat_variants_val_700     = 700,
} fs_fat_variants_uint16_const_t;

/**
 * @enum fs_fat_variants_uint32_const_t
 * @brief Named uint32_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint32_t {
  k_fs_fat_variants_write_fat32_entry_raw_0fffffff = 0x0FFFFFFFU,
  k_fs_fat_variants_write_fat32_entry_raw_a0000000 = 0xA0000000U,
} fs_fat_variants_uint32_const_t;

typedef enum : uint32_t {
  k_block_size       = 512U,   /**< Block size.       */
  k_fat12_blocks     = 1024U,  /**< Fat12 blocks.     */
  k_fat16_blocks     = 8192U,  /**< Fat16 blocks.     */
  k_fat32_blocks     = 70000U, /**< Fat32 blocks.     */
  k_fat12_fat_secs   = 4U,     /**< Fat12 fat secs.   */
  k_fat16_fat_secs   = 32U,    /**< Fat16 fat secs.   */
  k_fat32_fat_secs   = 512U,   /**< Fat32 fat secs.   */
  k_fat_root_entries = 16U,    /**< Fat root entries. */
} fat_variant_disk_t;

typedef struct {
  uint8_t* bytes;       /**< Bytes.       */
  uint32_t block_count; /**< Block count. */
  uint32_t byte_count;  /**< Byte count.  */
} mem_disk_t;

typedef struct {
  uint32_t count;                              /**< Count.      */
  uint32_t total_size;                         /**< Total size. */
  char     last_name[k_ra8_fs_short_name_len]; /**< Last name.  */
} list_ctx_t;

static mem_disk_t s_disk = {};

static void put16(uint8_t* p, uint32_t off, uint16_t v)
{
  p[off]     = (uint8_t)(v & k_fs_fat_variants_v_ff);
  p[off + 1] = (uint8_t)((v >> 8) & k_fs_fat_variants_v_ff);
}

static void put32(uint8_t* p, uint32_t off, uint32_t v)
{
  p[off]     = (uint8_t)(v & k_fs_fat_variants_v_ff);
  p[off + 1] = (uint8_t)((v >> 8) & k_fs_fat_variants_v_ff);
  p[off + 2] = (uint8_t)((v >> 16) & k_fs_fat_variants_v_ff);
  p[off + 3] = (uint8_t)((v >> k_fs_fat_variants_v_24) & k_fs_fat_variants_v_ff);
}

static uint32_t get32(const uint8_t* p, uint32_t off)
{
  return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8U) | ((uint32_t)p[off + 2] << 16U) |
         ((uint32_t)p[off + 3] << k_fs_fat_variants_val_24);
}

static void free_volume(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
}

static void alloc_volume(uint32_t blocks)
{
  free_volume();
  s_disk.byte_count  = blocks * (uint32_t)k_block_size;
  s_disk.bytes       = (uint8_t*)calloc(1, s_disk.byte_count);
  s_disk.block_count = blocks;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
}

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

static void build_common_bpb(uint32_t blocks, uint32_t fat_secs, uint32_t root_entries)
{
  uint8_t* bpb = &s_disk.bytes[0];
  put16(bpb, k_fs_fat_variants_put16_11, (uint16_t)k_block_size);
  bpb[k_fs_fat_variants_val_13] = 1U;
  put16(bpb, k_fs_fat_variants_put16_14, 1U);
  bpb[16] = 2U;
  put16(bpb, k_fs_fat_variants_put16_17, (uint16_t)root_entries);
  if (blocks <= k_fs_fat_variants_blocks_ffff) {
    put16(bpb, k_fs_fat_variants_put16_19, (uint16_t)blocks);
  } else {
    put32(bpb, 32U, blocks);
  }
  if (fat_secs <= k_fs_fat_variants_blocks_ffff) {
    put16(bpb, k_fs_fat_variants_put16_22, (uint16_t)fat_secs);
  } else {
    put32(bpb, k_fs_fat_variants_put32_36, fat_secs);
  }
  bpb[k_fs_fat_variants_val_510] = k_fs_fat_variants_bpb_55;
  bpb[k_fs_fat_variants_val_511] = k_fs_fat_variants_bpb_aa;
}

static void write_fat32_entry_raw(uint32_t cluster, uint32_t value)
{
  const uint32_t off = cluster * 4U;
  for (uint32_t i = 0; i < 2U; i++) {
    uint8_t* fat =
      &s_disk.bytes[((1U + (i * (uint32_t)k_fat32_fat_secs)) * (uint32_t)k_block_size) + off];
    put32(fat, 0U, value);
  }
}

static void build_fat12_volume(void)
{
  alloc_volume((uint32_t)k_fat12_blocks);
  build_common_bpb((uint32_t)k_fat12_blocks,
                   (uint32_t)k_fat12_fat_secs,
                   (uint32_t)k_fat_root_entries);
}

static void build_fat16_volume(void)
{
  alloc_volume((uint32_t)k_fat16_blocks);
  build_common_bpb((uint32_t)k_fat16_blocks,
                   (uint32_t)k_fat16_fat_secs,
                   (uint32_t)k_fat_root_entries);
}

static void build_fat32_volume(void)
{
  alloc_volume((uint32_t)k_fat32_blocks);
  build_common_bpb((uint32_t)k_fat32_blocks, 0U, 0U);
  put32(s_disk.bytes, k_fs_fat_variants_put32_36, (uint32_t)k_fat32_fat_secs);
  put32(s_disk.bytes, k_fs_fat_variants_put32_44, 2U);
  write_fat32_entry_raw(2U, k_fs_fat_variants_write_fat32_entry_raw_0fffffff);
}

static void list_cb(const char* name, uint8_t attr, uint32_t size, void* ctx)
{
  list_ctx_t* c = (list_ctx_t*)ctx;
  (void)attr;
  c->count++;
  c->total_size += size;
  (void)snprintf(c->last_name, sizeof(c->last_name), "%s", name);
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_fat12_large_chain_hits_12bit_dispatch_and_straddle(void)
{
  TEST_BEGIN("ra8_fs FAT12: long chain hits 12-bit get/set and straddle");
  build_fat12_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_fs_type_fat12, h->type);

  const uint32_t len = 175000U;
  uint8_t*       wr  = (uint8_t*)malloc(len);
  uint8_t*       rd  = (uint8_t*)malloc(len);
  TEST_ASSERT_NOT_NULL(wr);
  TEST_ASSERT_NOT_NULL(rd);
  for (uint32_t i = 0; i < len; i++) {
    wr[i] = (uint8_t)(i & k_fs_fat_variants_v_ff);
    rd[i] = 0U;
  }

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "CHAIN.BIN", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, wr, len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "CHAIN.BIN", k_ra8_fs_mode_read, &f));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, rd, len, &got));
  TEST_ASSERT_EQ(len, got);
  TEST_ASSERT_EQ(0, memcmp(wr, rd, len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "CHAIN.BIN"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free(wr);
  free(rd);
  free_volume();
  TEST_END("ra8_fs FAT12: long chain hits 12-bit get/set and straddle");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_fat32_write_preserves_reserved_nibble_and_round_trips(void)
{
  TEST_BEGIN("ra8_fs FAT32: write/read preserves high reserved FAT nibble");
  build_fat32_volume();
  write_fat32_entry_raw(3U, k_fs_fat_variants_write_fat32_entry_raw_a0000000);
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_fs_type_fat32, h->type);

  uint8_t payload[k_fs_fat_variants_val_700] = {};
  for (uint32_t i = 0; i < sizeof(payload); i++) {
    payload[i] = (uint8_t)(k_fs_fat_variants_val_a0 + (i & k_fs_fat_variants_i_0f));
  }
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "F32.BIN", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, payload, sizeof(payload)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  const uint32_t fat_entry3 = get32(s_disk.bytes, ((1U * (uint32_t)k_block_size) + (3U * 4U)));
  TEST_ASSERT_EQ(0xA0000000U, (fat_entry3 & 0xF0000000U));
  TEST_ASSERT_EQ(4U, (fat_entry3 & 0x0FFFFFFFU));

  uint8_t  rd[k_fs_fat_variants_val_700] = {};
  uint32_t got                           = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "F32.BIN", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, rd, sizeof(rd), &got));
  TEST_ASSERT_EQ(sizeof(rd), got);
  TEST_ASSERT_EQ(0, memcmp(payload, rd, sizeof(rd)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs FAT32: write/read preserves high reserved FAT nibble");
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_short_name_boundary_cases(void)
{
  TEST_BEGIN("ra8_fs FAT16: short-name packing boundaries");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "/ABCDEFGH.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "NOEXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_open(h, "ABCDEFGHI.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_open(h, "A.TOOL", k_ra8_fs_mode_write, &f));

  list_ctx_t ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", list_cb, &ctx));
  TEST_ASSERT_EQ(2, ctx.count);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs FAT16: short-name packing boundaries");
}

static void fill_short_entry(uint8_t*   ent,
                             const char raw11[k_fs_fat_variants_fill_short_entry_11],
                             uint8_t    attr,
                             uint32_t   size)
{
  memset(ent, 0, k_ra8_fs_dir_entry_bytes);
  memcpy(ent, raw11, 11U);
  ent[k_fs_fat_variants_fill_short_entry_11] = attr;
  put32(ent, k_fs_fat_variants_put32_28, size);
}
/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */

static void test_fat32_root_walker_skips_and_follows_chain(void)
{
  TEST_BEGIN("ra8_fs FAT32: root walker skips deleted/LFN and follows chain");
  build_fat32_volume();
  write_fat32_entry_raw(2U, 4U);
  write_fat32_entry_raw(4U, k_fs_fat_variants_write_fat32_entry_raw_0fffffff);

  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  uint8_t* root0 = &s_disk.bytes[(size_t)h->first_data_lba * (uint32_t)k_block_size];
  for (uint32_t e = 0; e < 16U; e++) {
    char raw[k_fs_fat_variants_fill_short_entry_11] = {
      'E',
      'N',
      'T',
      (char)('0' + (char)(e % k_fs_fat_variants_e_10)),
      ' ',
      ' ',
      ' ',
      ' ',
      'B',
      'I',
      'N'};
    fill_short_entry(&root0[(size_t)e * (uint32_t)k_ra8_fs_dir_entry_bytes],
                     raw,
                     k_ra8_fs_attr_archive,
                     e);
  }
  root0[0]                                = k_fs_fat_variants_root0_e5;
  root0[32U + k_fs_fat_variants_put16_11] = k_ra8_fs_attr_lfn;

  uint8_t* root1 = &s_disk.bytes[(size_t)(h->first_data_lba + 2U) * (uint32_t)k_block_size];
  fill_short_entry(root1,
                   "TAIL    TXT",
                   k_ra8_fs_attr_archive,
                   k_fs_fat_variants_fill_short_entry_99);

  list_ctx_t ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", list_cb, &ctx));
  TEST_ASSERT_EQ(15, ctx.count);
  TEST_ASSERT_EQ(0, strcmp(ctx.last_name, "TAIL.TXT"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs FAT32: root walker skips deleted/LFN and follows chain");
}

int32_t main(void)
{
  test_fat12_large_chain_hits_12bit_dispatch_and_straddle();
  test_fat32_write_preserves_reserved_nibble_and_round_trips();
  test_short_name_boundary_cases();
  test_fat32_root_walker_skips_and_follows_chain();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_fat_variants.c\n");
  return 0;
}
