/*
 */
/**
 * @file test_ra8_fs_stat.c
 * @brief Tests for `ra8_fs_stat()` on FAT and exFAT volumes (#609).
 *
 * @details
 * The defect this closes is a wrong answer, not a missing one: `stat` used to
 * be `open(read)` + `size()` + `close()`, and since opening a directory
 * succeeds and a directory's `DIR_FileSize` is 0 by specification, every
 * folder came back as an existing zero-byte FILE. The attribute byte -- the
 * one field that could have told them apart -- was hardcoded to `archive`.
 *
 * So the assertions here are about telling things apart:
 *   - a file reports its real length and no directory bit,
 *   - a directory reports the directory bit and length 0,
 *   - the attribute byte is the entry's own: a read-only file patched on disk
 *     reports read-only, which a hardcoded `archive` cannot do,
 *   - a missing name is not-found rather than an existing empty file,
 *   - the volume root is a directory, on FAT and on exFAT alike,
 *   - `stat` consumes no file-table slot: it still answers with all four file
 *     handles open, which the old open-based implementation could not.
 *
 * The volumes are formatted through the public ``ra8_fs_format()`` over the
 * RAM card in ``tests/test_ra8_fs_format_fixture.h``. Timestamp assertions
 * prove the metadata survives the stat seam instead of stopping at the
 * on-disk writer.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "test_ra8_fs_format_fixture.h"
#include "unity_minimal.h"

/**
 * @enum stat_fixture_t
 * @brief Payload sizes, seeds, and the on-disk offsets the attr probe pokes.
 */
typedef enum : uint32_t {
  k_stat_payload_bytes = 300U,  /**< File length, deliberately not a round sector. */
  k_stat_seed          = 0x21U, /**< Fill seed for that payload.                   */
  k_stat_stride        = 13U,   /**< Fill stride for that payload.                 */
  k_stat_name_bytes    = 11U,   /**< Packed 8.3 name length in a dir entry.        */
  k_stat_attr_off      = 11U,   /**< MS FAT spec sec 6 "DIR_Attr" byte offset.     */
  k_stat_entry_bytes   = 32U,   /**< MS FAT spec sec 6 directory-entry size.       */
  k_stat_open_handles  = 4U,    /**< ::k_ra8_fs_max_files, the whole file table.   */
} stat_fixture_t;

/** @brief Fill @p buf with a deterministic, non-repeating pattern. */
static void fill(uint8_t* buf, uint32_t len)
{
  for (uint32_t i = 0U; i < len; ++i) {
    buf[i] = (uint8_t)((i * (uint32_t)k_stat_stride) + (uint32_t)k_stat_seed);
  }
}

/** @brief Require the legal no-clock fallback and no invented UTC offset. */
static void expect_epoch(const ra8_fs_timestamp_t* stamp)
{
  TEST_ASSERT(stamp->valid);
  TEST_ASSERT(!stamp->utc_offset_valid);
  TEST_ASSERT_EQ(1980U, stamp->value.year);
  TEST_ASSERT_EQ(1U, stamp->value.month);
  TEST_ASSERT_EQ(1U, stamp->value.day);
  TEST_ASSERT_EQ(0U, stamp->value.hour);
  TEST_ASSERT_EQ(0U, stamp->value.minute);
  TEST_ASSERT_EQ(0U, stamp->value.second);
}

/**
 * @brief Set attribute bits directly in the on-disk 8.3 entry named @p name83.
 *
 * @details Walks the RAM card sector by sector looking for the packed 11-byte
 *          name and ORs @p bits into its DIR_Attr. The whole card is walked
 *          because the formatter, not this fixture, decides where the root
 *          directory lands. Poking the image rather
 *          than asking `ra8_fs` to do it is the point: the driver has no
 *          "set attributes" verb, and the question under test is whether
 *          `stat` REPORTS what is on the medium or invents it.
 *
 * @param[in] name83 Packed 11-byte 8.3 name (no dot, space padded).
 * @param[in] bits   Attribute bits to set.
 *
 * @return bool true when the entry was found and patched.
 */
static bool patch_entry_attr(const char* name83, uint8_t bits)
{
  for (uint32_t lba = 0U; lba < s_disk.block_count; ++lba) {
    uint8_t* sec = &s_disk.bytes[(size_t)lba * (size_t)k_fmt_block_size];
    for (uint32_t off = 0U; off < (uint32_t)k_fmt_block_size; off += (uint32_t)k_stat_entry_bytes) {
      if (memcmp(&sec[off], name83, (size_t)k_stat_name_bytes) == 0) {
        sec[off + (uint32_t)k_stat_attr_off] |= bits;
        return true;
      }
    }
  }
  return false;
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a file, a directory and a missing name
 * must be three distinguishable answers, which is exactly what the old
 * open-based stat could not produce)
 */
static void test_stat_fat_file_dir_missing(void)
{
  TEST_BEGIN("stat on FAT: file vs directory vs missing");
  alloc_garbage_card((uint32_t)k_fmt_blocks_fat16);
  ra8_fs_format_opts_t opts = {.type = k_ra8_fs_type_fat16, .label = "STAT"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  uint8_t payload[k_stat_payload_bytes] = {};
  fill(payload, (uint32_t)k_stat_payload_bytes);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_write_file(h, "/DATA.BIN", payload, (uint32_t)k_stat_payload_bytes));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/BOOKS"));

  /* A file: real length, archive attribute, not a directory. */
  ra8_fs_stat_t file = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/DATA.BIN", &file));
  TEST_ASSERT(!file.is_directory);
  TEST_ASSERT_EQ(k_stat_payload_bytes, file.size_bytes);
  TEST_ASSERT_EQ(k_ra8_fs_attr_archive, (file.attr & (uint8_t)k_ra8_fs_attr_archive));
  TEST_ASSERT_EQ(0U, (file.attr & (uint8_t)k_ra8_fs_attr_directory));
  expect_epoch(&file.created);
  expect_epoch(&file.modified);
  expect_epoch(&file.accessed);

  /* A directory: the bit that says so, and length 0 -- the case that used to
   * come back indistinguishable from an empty file. */
  ra8_fs_stat_t dir = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/BOOKS", &dir));
  TEST_ASSERT(dir.is_directory);
  TEST_ASSERT_EQ(0U, dir.size_bytes);
  TEST_ASSERT_EQ(k_ra8_fs_attr_directory, (dir.attr & (uint8_t)k_ra8_fs_attr_directory));
  TEST_ASSERT(dir.first_cluster >= 2U);

  /* Missing: not-found, not an existing zero-byte file. */
  ra8_fs_stat_t gone = {};
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_stat(h, "/NOPE.BIN", &gone));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("stat on FAT: file vs directory vs missing");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the reported attribute byte must be the
 * entry's own, which a hardcoded `archive` cannot be)
 */
static void test_stat_reports_the_real_attribute_byte(void)
{
  TEST_BEGIN("stat on FAT: the attribute byte is the entry's, not a constant");
  alloc_garbage_card((uint32_t)k_fmt_blocks_fat16);
  ra8_fs_format_opts_t opts = {.type = k_ra8_fs_type_fat16, .label = "ATTR"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  uint8_t payload[k_stat_payload_bytes] = {};
  fill(payload, (uint32_t)k_stat_payload_bytes);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_write_file(h, "/RO.BIN", payload, (uint32_t)k_stat_payload_bytes));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));

  const uint8_t extra = (uint8_t)((uint8_t)k_ra8_fs_attr_read_only | (uint8_t)k_ra8_fs_attr_hidden |
                                  (uint8_t)k_ra8_fs_attr_system);
  TEST_ASSERT(patch_entry_attr("RO      BIN", extra));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/RO.BIN", &st));
  TEST_ASSERT_EQ(extra, (st.attr & extra));
  TEST_ASSERT(!st.is_directory);
  TEST_ASSERT_EQ(k_stat_payload_bytes, st.size_bytes);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("stat on FAT: the attribute byte is the entry's, not a constant");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the root has no directory entry to read,
 * so it is answered from mount geometry, and nested paths resolve like open's)
 */
static void test_stat_root_and_nested(void)
{
  TEST_BEGIN("stat on FAT: the root, and a path two components deep");
  alloc_garbage_card((uint32_t)k_fmt_blocks_fat16);
  ra8_fs_format_opts_t opts = {.type = k_ra8_fs_type_fat16, .label = "ROOT"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_stat_t root = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/", &root));
  TEST_ASSERT(root.is_directory);
  TEST_ASSERT_EQ(0U, root.size_bytes);
  TEST_ASSERT(!root.created.valid);
  TEST_ASSERT(!root.modified.valid);
  TEST_ASSERT(!root.accessed.valid);

  ra8_fs_stat_t empty_path = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "", &empty_path));
  TEST_ASSERT(empty_path.is_directory);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/BOOKS"));
  uint8_t payload[k_stat_payload_bytes] = {};
  fill(payload, (uint32_t)k_stat_payload_bytes);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_write_file(h, "/BOOKS/A.BIN", payload, (uint32_t)k_stat_payload_bytes));

  ra8_fs_stat_t nested = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/BOOKS/A.BIN", &nested));
  TEST_ASSERT(!nested.is_directory);
  TEST_ASSERT_EQ(k_stat_payload_bytes, nested.size_bytes);

  ra8_fs_stat_t missing_parent = {};
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_stat(h, "/GONE/A.BIN", &missing_parent));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("stat on FAT: the root, and a path two components deep");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a metadata query must not spend one of
 * the four file slots, which is what the open-based implementation did)
 */
static void test_stat_consumes_no_file_slot(void)
{
  TEST_BEGIN("stat with every file handle already open");
  alloc_garbage_card((uint32_t)k_fmt_blocks_fat16);
  ra8_fs_format_opts_t opts = {.type = k_ra8_fs_type_fat16, .label = "SLOTS"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  uint8_t payload[k_stat_payload_bytes] = {};
  fill(payload, (uint32_t)k_stat_payload_bytes);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_write_file(h, "/DATA.BIN", payload, (uint32_t)k_stat_payload_bytes));

  /* Fill the file table. */
  ra8_fs_file_t*           held[k_stat_open_handles] = {};
  static const char* const names[]                   = {"/H0.BIN", "/H1.BIN", "/H2.BIN", "/H3.BIN"};
  for (uint32_t i = 0U; i < (uint32_t)k_stat_open_handles; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, names[i], k_ra8_fs_mode_write, &held[i]));
  }
  ra8_fs_file_t* overflow = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_open(h, "/DATA.BIN", k_ra8_fs_mode_read, &overflow));

  /* ...and stat still answers. */
  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/DATA.BIN", &st));
  TEST_ASSERT_EQ(k_stat_payload_bytes, st.size_bytes);

  for (uint32_t i = 0U; i < (uint32_t)k_stat_open_handles; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(held[i]));
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("stat with every file handle already open");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the argument guards and the
 * not-in-use mount guard are single-condition each)
 */
static void test_stat_guards(void)
{
  TEST_BEGIN("stat argument and mount-state guards");
  alloc_garbage_card((uint32_t)k_fmt_blocks_fat16);
  ra8_fs_format_opts_t opts = {.type = k_ra8_fs_type_fat16, .label = "GUARD"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_stat(nullptr, "/", &st));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_stat(h, nullptr, &st));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_stat(h, "/", nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_stat(h, "/", &st));
  free_volume();
  TEST_END("stat argument and mount-state guards");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the exFAT lookup must answer file,
 * missing and root the same way the FAT one does)
 */
static void test_stat_exfat(void)
{
  TEST_BEGIN("stat on exFAT: file, missing, root");
  alloc_garbage_card((uint32_t)k_fmt_blocks_exfat);
  ra8_fs_format_opts_t opts = {.type = k_ra8_fs_type_exfat, .label = "EXSTAT"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_fs_type_exfat, h->type);

  uint8_t payload[k_stat_payload_bytes] = {};
  fill(payload, (uint32_t)k_stat_payload_bytes);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_write_file(h, "STORY.TXT", payload, (uint32_t)k_stat_payload_bytes));

  ra8_fs_stat_t file = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/STORY.TXT", &file));
  TEST_ASSERT(!file.is_directory);
  TEST_ASSERT_EQ(k_stat_payload_bytes, file.size_bytes);
  TEST_ASSERT_EQ(k_ra8_fs_attr_archive, (file.attr & (uint8_t)k_ra8_fs_attr_archive));
  TEST_ASSERT(file.first_cluster >= 2U);
  expect_epoch(&file.created);
  expect_epoch(&file.modified);
  expect_epoch(&file.accessed);

  /* The leading slash is optional on exFAT, exactly as it is for open (#93). */
  ra8_fs_stat_t no_slash = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "STORY.TXT", &no_slash));
  TEST_ASSERT_EQ(file.size_bytes, no_slash.size_bytes);

  ra8_fs_stat_t gone = {};
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_stat(h, "/NOPE.TXT", &gone));

  ra8_fs_stat_t root = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/", &root));
  TEST_ASSERT(root.is_directory);
  TEST_ASSERT_EQ(0U, root.size_bytes);
  TEST_ASSERT_EQ(h->root_cluster, root.first_cluster);
  TEST_ASSERT(!root.created.valid);
  TEST_ASSERT(!root.modified.valid);
  TEST_ASSERT(!root.accessed.valid);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("stat on exFAT: file, missing, root");
}

int32_t main(void)
{
  test_stat_fat_file_dir_missing();
  test_stat_reports_the_real_attribute_byte();
  test_stat_root_and_nested();
  test_stat_consumes_no_file_slot();
  test_stat_guards();
  test_stat_exfat();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_stat.c\n");
  return 0;
}
