/**
 * @file test_ra8_fs_format_fixture.h
 * @brief RAM-backed block device and helpers shared by the formatter tests.
 *
 * @details
 * The formatter suite is split along the FAT / exFAT seam for the 1000-line
 * file-size cap (see ``tests/test_ra8_fs_format.c`` and
 * ``tests/test_ra8_fs_format_exfat.c``). Both halves format and then mount a
 * real in-memory card, so the disk model, the write-sink backend, the erase
 * backend and the mount/file-cycle helpers live here instead of being copied.
 *
 * Each test binary is linked separately, so the ``static`` definitions below
 * are per-executable and cannot collide. They carry ``[[maybe_unused]]``
 * because neither half exercises every fixture.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "unity_minimal.h"

/** @brief Fill byte for the pre-format disk image. */
typedef enum : uint8_t {
  k_fmt_fill_unformatted = 0xA5U, /**< Format must overwrite every trace. */
} fmt_fill_t;

/**
 * @enum fs_format_fixture_t
 * @brief The payload generators and their seeds.
 */
typedef enum : uint8_t {
  k_fs_pattern_stride =
    31U, /**< Payload stride, `i * 31 + 7`; prime, so the pattern never repeats inside a sector. */
  k_fs_spc_max =
    128U, /**< Largest sectors-per-cluster allowed, so geometry maths is exercised at its limit. */
  /** Its bias, so index 0 is not byte 0. */
  k_fs_pattern_bias = 7U,
} fs_format_fixture_t;

/**
 * @enum fs_format_fixture2_t
 * @brief Out-of-range and malformed inputs the code under test must reject.
 */
typedef enum : uint16_t {
  k_fs_block_size_unsupported =
    1024U, /**< A block size other than 512, which the formatter must reject. */
} fs_format_fixture2_t;

/**
 * @enum ra8_fs_fmt_test_t
 * @brief Synthetic block-device sizes (in 512-byte sectors) per FAT band.
 *
 * @details Each size is picked so the formatter's auto cluster-size sweep lands
 *          the cluster count squarely inside the requested band: a 512 KiB
 *          card for FAT12, a 4 MiB card for FAT16, and a 36 MiB card for FAT32.
 */
typedef enum : uint32_t {
  k_fmt_block_size    = 512U,    /**< Bytes per sector.                    */
  k_fmt_blocks_fat12  = 1024U,   /**< 512 KiB -> FAT12 band.               */
  k_fmt_blocks_fat16  = 8192U,   /**< 4 MiB   -> FAT16 band.               */
  k_fmt_blocks_fat32  = 73728U,  /**< 36 MiB  -> FAT32 band.               */
  k_fmt_blocks_tiny   = 64U,     /**< 32 KiB -- too small for FAT16/FAT32. */
  k_fmt_payload_bytes = 1300U,   /**< Multi-cluster payload (> 1 KiB).     */
  k_fmt_blocks_exfat  = 131072U, /**< 64 MiB -> exFAT (spc-shift 3).       */
} ra8_fs_fmt_test_t;

/**
 * @enum ra8_fs_exfat_tier_t
 * @brief Card sizes (512-byte sectors) and the exFAT SectorsPerClusterShift
 *        each must select (Microsoft default table, MS exFAT spec sec 12.1).
 */
typedef enum : uint32_t {
  k_exfat_de_spc_off = 109U,        /**< VBR byte: SectorsPerClusterShift. */
  k_exfat_blk_64m    = 131072U,     /**< 64 MB  -> spc-shift 3 (4 KB).     */
  k_exfat_blk_1g     = 2097152U,    /**< 1 GB   -> spc-shift 6 (32 KB).    */
  k_exfat_blk_64g    = 134217728U,  /**< 64 GB  -> spc-shift 8 (128 KB).   */
  k_exfat_blk_512g   = 1073741824U, /**< 512 GB -> spc-shift 9 (256 KB).   */
  k_exfat_spc_64m    = 3U,          /**< Expected shift for 64 MB.         */
  k_exfat_spc_1g     = 6U,          /**< Expected shift for 1 GB.          */
  k_exfat_spc_64g    = 8U,          /**< Expected shift for 64 GB.         */
  k_exfat_spc_512g   = 9U,          /**< Expected shift for 512 GB.        */
} ra8_fs_exfat_tier_t;

/** @brief Memory-backed disk handed to ra8_fs as a block device. */
typedef struct {
  uint8_t* bytes;       /**< Flat sector store.         */
  uint32_t block_count; /**< Number of 512-byte blocks. */
} mem_disk_t;

/** @brief Listdir tally cookie. */
typedef struct {
  uint32_t count;                         /**< Visible entries seen. */
  char     last[k_ra8_fs_short_name_len]; /**< Last name reported.   */
} list_ctx_t;

[[maybe_unused]] static mem_disk_t s_disk = {};

[[maybe_unused]] static ra8_err_t mem_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_fmt_block_size],
         (size_t)count * (uint32_t)k_fmt_block_size);
  return k_ra8_ok;
}

[[maybe_unused]] static ra8_err_t
mem_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_fmt_block_size],
         buf,
         (size_t)count * (uint32_t)k_fmt_block_size);
  return k_ra8_ok;
}

[[maybe_unused]] static ra8_err_t
mem_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  *block_count  = d->block_count;
  *block_size   = (uint32_t)k_fmt_block_size;
  return k_ra8_ok;
}

[[maybe_unused]] static const ra8_fs_backend_t s_backend = {
  .read_block   = mem_read,
  .write_block  = mem_write,
  .get_capacity = mem_capacity,
  .ctx          = &s_disk,
};

/** @brief LBA the exFAT partition (and its VBR) starts at (mirrors
 * ::k_exfat_fmt_part_lba). The formatter lays an MBR at LBA 0 and the exFAT VBR
 * one aligned megabyte in, so a large-card probe must inspect the VBR here. */
typedef enum : uint32_t {
  k_exfat_test_part_lba = 2048U, /**< exFAT partition start (1 MiB aligned). */
} ra8_fs_exfat_part_t;

/* Multi-GB cards cannot be RAM-backed (a 128 GB malloc is impossible), so the
 * cluster-size-table test uses a write-sink that reports a huge capacity, keeps
 * only sector 0 (the FAT boot sector, or the exFAT MBR) and the exFAT VBR sector
 * (LBA 2048), and black-holes every other write. ra8_fs_format never reads back
 * during a format, so a zero-returning read stub suffices. */
[[maybe_unused]] static uint8_t  s_sink_boot[k_fmt_block_size] = {};
[[maybe_unused]] static uint8_t  s_sink_vbr[k_fmt_block_size]  = {};
[[maybe_unused]] static uint32_t s_sink_blocks                 = 0U;

[[maybe_unused]] static ra8_err_t sink_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  (void)lba;
  memset(buf, 0, (size_t)count * (size_t)k_fmt_block_size);
  return k_ra8_ok;
}

[[maybe_unused]] static ra8_err_t
sink_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  (void)ctx;
  if (lba == 0U && count >= 1U) {
    memcpy(s_sink_boot, buf, (size_t)k_fmt_block_size);
  }
  if (lba == (uint32_t)k_exfat_test_part_lba && count >= 1U) {
    memcpy(s_sink_vbr, buf, (size_t)k_fmt_block_size);
  }
  return k_ra8_ok; /* every other sector is discarded */
}

[[maybe_unused]] static ra8_err_t
sink_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  *block_count = s_sink_blocks;
  *block_size  = (uint32_t)k_fmt_block_size;
  return k_ra8_ok;
}

[[maybe_unused]] static const ra8_fs_backend_t s_sink_backend = {
  .read_block   = sink_read,
  .write_block  = sink_write,
  .get_capacity = sink_capacity,
  .ctx          = nullptr,
};

/**
 * @enum erase_mode_t
 * @brief How the erase-capable mock answers `erase_blocks` (drives clear_region).
 */
typedef enum : uint8_t {
  k_erase_mode_zero        = 0U, /**< Zero the range, return k_ra8_ok (card zeroes). */
  k_erase_mode_unsupported = 1U, /**< Return not_supported -> formatter zero-writes. */
  k_erase_mode_error       = 2U, /**< Return a real error -> format must abort.      */
} erase_mode_t;

[[maybe_unused]] static erase_mode_t s_erase_mode  = k_erase_mode_zero;
[[maybe_unused]] static uint32_t     s_erase_calls = 0U;

[[maybe_unused]] static ra8_err_t mem_erase(void* ctx, uint32_t lba, uint32_t count)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  s_erase_calls++;
  if (s_erase_mode == k_erase_mode_unsupported) {
    return k_ra8_err_not_supported;
  }
  if (s_erase_mode == k_erase_mode_error) {
    return k_ra8_err_out_of_range; /* a real backend error -- must abort the format */
  }
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memset(&d->bytes[(size_t)lba * (uint32_t)k_fmt_block_size],
         0,
         (size_t)count * (uint32_t)k_fmt_block_size);
  return k_ra8_ok;
}

[[maybe_unused]] static const ra8_fs_backend_t s_backend_erase = {
  .read_block   = mem_read,
  .write_block  = mem_write,
  .get_capacity = mem_capacity,
  .erase_blocks = mem_erase,
  .ctx          = &s_disk,
};

/* Fault-injection backend over the same RAM disk (for the metadata _cov tests):
 * `s_fault_read_at` fails the Nth read after ::fault_reset (0 = never), and
 * `s_fault_write_all` fails every write. A volume is FORMATTED through the plain
 * backend, then MOUNTED through this one so an operation's I/O can be made to
 * fail deterministically. */
[[maybe_unused]] static uint32_t s_fault_read_at   = 0U;
[[maybe_unused]] static uint32_t s_fault_read_seen = 0U;
[[maybe_unused]] static bool     s_fault_write_all = false;

[[maybe_unused]] static void fault_reset(void)
{
  s_fault_read_at   = 0U;
  s_fault_read_seen = 0U;
  s_fault_write_all = false;
}

[[maybe_unused]] static ra8_err_t fault_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  s_fault_read_seen++;
  if ((s_fault_read_at != 0U) && (s_fault_read_seen >= s_fault_read_at)) {
    return k_ra8_err_hw_error;
  }
  return mem_read(ctx, lba, count, buf);
}

[[maybe_unused]] static ra8_err_t
fault_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  if (s_fault_write_all) {
    return k_ra8_err_hw_error;
  }
  return mem_write(ctx, lba, count, buf);
}

[[maybe_unused]] static const ra8_fs_backend_t s_fault_backend = {
  .read_block   = fault_read,
  .write_block  = fault_write,
  .get_capacity = mem_capacity,
  .ctx          = &s_disk,
};

[[maybe_unused]] static void free_volume(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
}

/**
 * @brief Allocate a fresh, garbage-filled card of @p blocks sectors.
 *
 * @details The store is pre-filled with 0xA5 so a successful format proves the
 *          formatter actually wrote a valid BPB (rather than a zeroed buffer
 *          accidentally satisfying a reader).
 */
[[maybe_unused]] static void alloc_garbage_card(uint32_t blocks)
{
  free_volume();
  s_disk.bytes       = (uint8_t*)malloc((size_t)blocks * (size_t)k_fmt_block_size);
  s_disk.block_count = blocks;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "malloc failed");
  }
  memset(s_disk.bytes, k_fmt_fill_unformatted, (size_t)blocks * (size_t)k_fmt_block_size);
}

[[maybe_unused]] static void count_cb(const char* name, uint8_t attr, uint32_t size, void* ctx)
{
  (void)attr;
  (void)size;
  list_ctx_t* c = (list_ctx_t*)ctx;
  c->count++;
  (void)snprintf(c->last, sizeof(c->last), "%s", name);
}

/**
 * @brief Mount @p be, assert the detected type, and run a full file cycle.
 *
 * @details Mount-side half of the round-trip, factored so both the RAM-backed
 *          happy-path tests and the erase-capable backend test can reuse it:
 *          create + write a multi-cluster payload, read it back and byte-compare,
 *          list (expect the one file), unlink, list again (empty), unmount.
 */
[[maybe_unused]] static void verify_mount_file_cycle(const ra8_fs_backend_t* be, ra8_fs_type_t type)
{
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(be, &h));
  TEST_ASSERT_EQ(type, h->type);

  static uint8_t s_wr[k_fmt_payload_bytes];
  static uint8_t s_rd[k_fmt_payload_bytes];
  for (uint32_t i = 0U; i < (uint32_t)k_fmt_payload_bytes; i++) {
    s_wr[i] = (uint8_t)((i * k_fs_pattern_stride) + k_fs_pattern_bias);
    s_rd[i] = 0U;
  }
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "HELLO.BIN", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, s_wr, (uint32_t)k_fmt_payload_bytes));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "HELLO.BIN", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, s_rd, (uint32_t)k_fmt_payload_bytes, &got));
  TEST_ASSERT_EQ(k_fmt_payload_bytes, got);
  TEST_ASSERT_EQ(0, memcmp(s_wr, s_rd, (size_t)k_fmt_payload_bytes));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  list_ctx_t ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", count_cb, &ctx));
  TEST_ASSERT_EQ(1, ctx.count);
  TEST_ASSERT_EQ(0, strcmp(ctx.last, "HELLO.BIN"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "HELLO.BIN"));
  ctx.count = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", count_cb, &ctx));
  TEST_ASSERT_EQ(0, ctx.count);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
}

[[maybe_unused]] static void
format_mount_cycle(uint32_t blocks, ra8_fs_type_t type, const char* label)
{
  alloc_garbage_card(blocks);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = type;
  opts.label                = label;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  verify_mount_file_cycle(&s_backend, type);
  free_volume();
}
