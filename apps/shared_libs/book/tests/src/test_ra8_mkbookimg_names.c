/**
 * @file test_ra8_mkbookimg_names.c
 * @brief Host test: mkbookimg stores each book under its own long basename (#633).
 *
 * @details
 * Before #600 gave `ra8_fs` VFAT long-name write, `tools/mkbookimg` emitted
 * `BOOK01.RBK`, `BOOK02.RBK`, ... and discarded the source's real name. #633
 * removed that workaround: the tool now files each book under its own basename.
 *
 * This test exercises the SAME name derivation the tool ships
 * (`mkbookimg_dest_name`, shared through `tools/mkbookimg/inc/mkbookimg_names.h`)
 * and then proves the derived name round-trips through a mount: a book written
 * to a FAT32 volume under its long basename (the exact `ra8_fs_write_file` call
 * the tool makes) reads back by that same name with identical bytes.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Test] {World: NS}
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mkbookimg_names.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_log.h"
#include "unity_minimal.h"

/** @brief No-op log sink so ra8_fs failure logs never touch host-unmapped ITM. */
static void test_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}

/**
 * @enum mk_disk_t
 * @brief RAM block-device geometry (64 MiB -> FAT32, matching the tool's image).
 */
typedef enum : uint32_t {
  k_disk_block_size = 512U,    /**< Bytes per sector.                       */
  k_disk_blocks     = 131072U, /**< 64 MiB volume (auto-detects FAT32).     */
  k_readback_cap    = 64U,     /**< Read-back buffer bytes (> test bodies). */
} mk_disk_t;

typedef struct {
  uint8_t* bytes;       /**< Bytes.       */
  uint32_t block_count; /**< Block count. */
} mem_disk_t;

static mem_disk_t s_disk = {};

static ra8_err_t mem_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  mem_disk_t* disk = (mem_disk_t*)ctx;
  if ((lba > (uint64_t)disk->block_count) ||
      ((uint64_t)count > ((uint64_t)disk->block_count - lba))) {
    return k_ra8_err_out_of_range;
  }
  (void)memcpy(buf,
               &disk->bytes[(size_t)lba * (size_t)k_disk_block_size],
               (size_t)count * (size_t)k_disk_block_size);
  return k_ra8_ok;
}

static ra8_err_t mem_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* disk = (mem_disk_t*)ctx;
  if ((lba > (uint64_t)disk->block_count) ||
      ((uint64_t)count > ((uint64_t)disk->block_count - lba))) {
    return k_ra8_err_out_of_range;
  }
  (void)memcpy(&disk->bytes[(size_t)lba * (size_t)k_disk_block_size],
               buf,
               (size_t)count * (size_t)k_disk_block_size);
  return k_ra8_ok;
}

static ra8_err_t mem_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  mem_disk_t* disk = (mem_disk_t*)ctx;
  *block_count     = disk->block_count;
  *block_size      = (uint32_t)k_disk_block_size;
  return k_ra8_ok;
}

/** @brief Format a fresh FAT32 RAM volume and return a mounted handle. */
static ra8_fs_mount_t* fresh_volume(void)
{
  static uint8_t                s_disk_bytes[(size_t)k_disk_blocks * (size_t)k_disk_block_size];
  static const ra8_fs_backend_t s_backend = {
    .read_block   = mem_read,
    .write_block  = mem_write,
    .get_capacity = mem_capacity,
    .ctx          = &s_disk,
  };
  (void)memset(s_disk_bytes, 0, sizeof(s_disk_bytes));
  s_disk.block_count = (uint32_t)k_disk_blocks;
  s_disk.bytes       = s_disk_bytes;

  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat32;
  opts.label                = "RABOOKS";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));

  ra8_fs_mount_t* mount = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &mount));
  return mount;
}

static void teardown(ra8_fs_mount_t* mount)
{
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(mount));
  s_disk.bytes = nullptr;
}

/** @brief Write @p body under @p name, then read it back and assert it matches. */
static void assert_name_roundtrips(ra8_fs_mount_t* mount, const char* name, const char* body)
{
  const uint32_t len = (uint32_t)strlen(body);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(mount, name, (const uint8_t*)body, len));

  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, name, k_ra8_fs_mode_read, &file));
  uint64_t size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(file, &size));
  TEST_ASSERT_EQ(len, size);
  uint8_t  got_buf[k_readback_cap];
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, got_buf, (uint32_t)sizeof(got_buf), &got));
  TEST_ASSERT_EQ(len, got);
  for (uint32_t i = 0U; i < len; ++i) {
    TEST_ASSERT_EQ((uint8_t)body[i], got_buf[i]);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));
}

/**
 * @test test_dest_name_derivation
 * @brief The shared derivation keeps the basename and rejects the unusable.
 *
 * @par MC/DC:
 * `mkbookimg_dest_name`'s reject guards are short-circuit `||` chains
 * (`path == NULL || out == NULL || cap == 0` and `n == 0 || n >= cap`). Each
 * condition is driven true in isolation -- NULL path, NULL out, empty basename,
 * over-cap name -- against the all-false accept case, so each independently
 * flips the accept/reject outcome.
 */
static void test_dest_name_derivation(void)
{
  TEST_BEGIN("mkbookimg: destination name = source basename");
  char name[k_mkbookimg_name_cap] = {};

  TEST_ASSERT(mkbookimg_dest_name("Meditations.rabook", name, sizeof name));
  TEST_ASSERT_EQ(0, strcmp(name, "Meditations.rabook"));

  /* A directory prefix is stripped to the basename. */
  TEST_ASSERT(mkbookimg_dest_name("content/library/Pride and Prejudice.epub", name, sizeof name));
  TEST_ASSERT_EQ(0, strcmp(name, "Pride and Prejudice.epub"));

  /* A bare name with no directory is kept verbatim. */
  TEST_ASSERT(mkbookimg_dest_name("plain.rabook", name, sizeof name));
  TEST_ASSERT_EQ(0, strcmp(name, "plain.rabook"));

  /* Empty basename (path ends in '/') and NULLs are refused, not truncated. */
  TEST_ASSERT(!mkbookimg_dest_name("dir/", name, sizeof name));
  TEST_ASSERT(!mkbookimg_dest_name("", name, sizeof name));
  TEST_ASSERT(!mkbookimg_dest_name(nullptr, name, sizeof name));
  TEST_ASSERT(!mkbookimg_dest_name("ok.rabook", nullptr, sizeof name));
  TEST_ASSERT(!mkbookimg_dest_name("ok.rabook", name, 0U));

  /* A name that does not fit the buffer is refused rather than truncated. */
  TEST_ASSERT(!mkbookimg_dest_name("Meditations.rabook", name, 4U));

  TEST_END("mkbookimg: destination name = source basename");
}

/**
 * @test test_longname_roundtrips_through_mount
 * @brief Books written under their long basenames read back by the same name.
 *
 * @par MC/DC:
 * No compound decision under test; this is an end-to-end FS round-trip of a
 * derived long name (write -> mount -> open -> size + byte compare).
 */
static void test_longname_roundtrips_through_mount(void)
{
  TEST_BEGIN("mkbookimg: long book names round-trip through a mount");
  ra8_fs_mount_t* mount = fresh_volume();

  /* Two human-readable long names -- exactly what the tool now files. */
  char name_a[k_mkbookimg_name_cap] = {};
  char name_b[k_mkbookimg_name_cap] = {};
  TEST_ASSERT(mkbookimg_dest_name("out/Pride and Prejudice.epub", name_a, sizeof name_a));
  TEST_ASSERT(mkbookimg_dest_name("Meditations.rabook", name_b, sizeof name_b));
  assert_name_roundtrips(mount, name_a, "epub-body-a");
  assert_name_roundtrips(mount, name_b, "RABOOK1-body-b");

  /* An 8.3 name written by the old tool still resolves after the change. */
  assert_name_roundtrips(mount, "BOOK01.RBK", "legacy-8.3-body");

  teardown(mount);
  TEST_END("mkbookimg: long book names round-trip through a mount");
}

int main(void)
{
  ra8_log_set_byte_sink(test_log_sink, nullptr);
  test_dest_name_derivation();
  test_longname_roundtrips_through_mount();
  return 0;
}
