/**
 * @file test_ra8_io_vfs.c
 * @brief Unit tests for the ra8_io VFS mount table + path router (issue #158).
 *
 * @details
 * Sets up a FAT16 volume on a RAM block device, registers it under a name, and
 * exercises name-based open / stat / unlink / rename / listdir / mkdir / rmdir,
 * mount-table mechanics (duplicate, full, unmount isolation), and path-parse
 * rejection.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_vfs.h"
#include "unity_minimal.h"

/**
 * @enum io_vfs_fixture_t
 * @brief The byte-level helpers.
 */
typedef enum : uint8_t {
  k_byte_mask = 0xFFU, /**< Truncates a generated or shifted value back into a byte. */
} io_vfs_fixture_t;

/**
 * @enum t_vfs_const_t
 * @brief Fixture sizes.
 */
typedef enum : uint32_t {
  k_t_disk_blocks = 16384, /**< 8 MiB -- comfortably FAT16. */
  k_t_payload     = 100,   /**< Bytes in the seeded file.   */
} t_vfs_const_t;

/** @brief 8 MiB backing buffer. */
static uint8_t s_disk[(size_t)k_t_disk_blocks * (size_t)k_ra8_io_block_size_bytes];
static ra8_io_blockdev_ram_state_t s_bstate;
static ra8_io_blockdev_t           s_bd;
static ra8_fs_backend_t            s_be;
static ra8_fs_mount_t*             s_mnt;

/**
 * @brief Build a fresh FAT16 volume with one seeded file and return its mount.
 */
static ra8_fs_mount_t* setup_volume(void)
{
  if (s_mnt != nullptr) {
    (void)ra8_fs_unmount(s_mnt); /* ra8_fs has only 2 mount slots -- free the prior one */
    s_mnt = nullptr;
  }
  (void)ra8_io_blockdev_ram_init(&s_bd, &s_bstate, s_disk, k_t_disk_blocks, false);
  (void)ra8_io_blockdev_as_fs_backend(&s_bd, &s_be);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;
  opts.label                = "VFS";
  (void)ra8_fs_format(&s_be, &opts);
  s_mnt = nullptr;
  (void)ra8_fs_mount(&s_be, &s_mnt);
  uint8_t data[(size_t)k_t_payload];
  for (uint32_t i = 0; i < (uint32_t)k_t_payload; ++i) {
    data[i] = (uint8_t)((i + 1U) & k_byte_mask);
  }
  (void)ra8_fs_write_file(s_mnt, "HELLO.BIN", data, k_t_payload);
  return s_mnt;
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- mount rejects NULL, bad names, duplicates,
 * and a full table via independent single-condition guards)
 */
static void test_mount_table(void)
{
  TEST_BEGIN("vfs mount table");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  ra8_fs_mount_t* m = setup_volume();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_vfs_mount(nullptr, m));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_vfs_mount("sd", nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_vfs_mount("a:b", m)); /* reserved ':' */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_vfs_mount("", m));    /* empty        */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("sd", m));
  TEST_ASSERT_EQ(k_ra8_err_exists, ra8_io_vfs_mount("sd", m)); /* duplicate */
  /* fill remaining slots, then overflow */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("a", m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("b", m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("c", m));
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_io_vfs_mount("d", m));
  TEST_END("vfs mount table");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- open + read-back over a named mount, and
 * a path with no name: prefix is rejected)
 */
static void test_open_read(void)
{
  TEST_BEGIN("vfs open/read");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("sd", setup_volume()));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_vfs_open("sd:/HELLO.BIN", k_ra8_fs_mode_read, nullptr));
  ra8_fs_file_t*  f = nullptr;
  const ra8_err_t e = ra8_io_vfs_open("noprefix", k_ra8_fs_mode_read, &f);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, e);
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_io_vfs_open("nope:/X", k_ra8_fs_mode_read, &f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_open("sd:/HELLO.BIN", k_ra8_fs_mode_read, &f));
  uint8_t  got[(size_t)k_t_payload] = {};
  uint32_t got_len                  = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, got, k_t_payload, &got_len));
  TEST_ASSERT_EQ(k_t_payload, got_len);
  TEST_ASSERT_EQ(1, got[0]);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_END("vfs open/read");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- stat reports presence/size, a missing
 * file yields exists==false with ok, and a DIRECTORY reports as one: #609, where
 * the open-based implementation returned every folder as a zero-byte file with a
 * hardcoded `archive` attribute)
 */
static void test_stat(void)
{
  TEST_BEGIN("vfs stat");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  ra8_fs_mount_t* m = setup_volume();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("sd", m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(m, "/BOOKS"));

  ra8_io_vfs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_stat("sd:/HELLO.BIN", &st));
  TEST_ASSERT(st.exists);
  TEST_ASSERT_EQ(k_t_payload, st.size_bytes);
  TEST_ASSERT(!st.is_directory);
  TEST_ASSERT_EQ(0U, (st.attr & (uint8_t)k_ra8_fs_attr_directory));

  /* The regression this test exists for: a folder must not look like a file. */
  ra8_io_vfs_stat_t dir = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_stat("sd:/BOOKS", &dir));
  TEST_ASSERT(dir.exists);
  TEST_ASSERT(dir.is_directory);
  TEST_ASSERT_EQ(0U, dir.size_bytes);
  TEST_ASSERT_EQ(k_ra8_fs_attr_directory, (dir.attr & (uint8_t)k_ra8_fs_attr_directory));

  /* The mount root is a directory too. */
  ra8_io_vfs_stat_t root = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_stat("sd:/", &root));
  TEST_ASSERT(root.exists);
  TEST_ASSERT(root.is_directory);

  ra8_io_vfs_stat_t miss = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_stat("sd:/GONE.BIN", &miss));
  TEST_ASSERT(!miss.exists);
  TEST_END("vfs stat");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- rename within a mount succeeds; a
 * cross-mount rename is rejected; unlink removes the file)
 */
static void test_rename_unlink(void)
{
  TEST_BEGIN("vfs rename/unlink");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("sd", setup_volume()));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_vfs_rename("sd:/HELLO.BIN", "ram:/BYE.BIN"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_rename("sd:/HELLO.BIN", "sd:/BYE.BIN"));
  ra8_io_vfs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_stat("sd:/BYE.BIN", &st));
  TEST_ASSERT(st.exists);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_unlink("sd:/BYE.BIN"));
  ra8_io_vfs_stat_t gone = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_stat("sd:/BYE.BIN", &gone));
  TEST_ASSERT(!gone.exists);
  TEST_END("vfs rename/unlink");
}

/** @brief listdir callback: count entries. */
static void count_cb(const char* name, uint8_t attr, uint64_t size, void* ctx)
{
  (void)name;
  (void)attr;
  (void)size;
  uint32_t* n = (uint32_t*)ctx;
  (*n)++;
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- listdir visits the seeded file; mkdir
 * creates a real subdirectory that then lists and rejects a duplicate; unmount
 * makes a name stop resolving)
 */
static void test_listdir_mkdir_unmount(void)
{
  TEST_BEGIN("vfs listdir/mkdir/unmount");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("sd", setup_volume()));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("ospi", s_mnt));

  uint32_t n = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_listdir("sd:/", count_cb, &n));
  TEST_ASSERT(n >= 1U);

  /* mkdir now creates a real subdirectory (the #158 ra8_fs mkdir work); a fresh
   * empty subdir lists with zero visible entries ("." / ".." are hidden), and a
   * duplicate create is rejected. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mkdir("sd:/SUB"));
  TEST_ASSERT_EQ(k_ra8_err_exists, ra8_io_vfs_mkdir("sd:/SUB"));
  uint32_t m = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_listdir("sd:/SUB", count_cb, &m));
  TEST_ASSERT_EQ(0U, m);

  /* unmount isolation: dropping "sd" leaves "ospi" working */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_unmount("sd"));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_io_vfs_open("sd:/HELLO.BIN", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_open("ospi:/HELLO.BIN", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_io_vfs_unmount("sd"));
  TEST_END("vfs listdir/mkdir/unmount");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- rmdir delegates 1:1, so each case maps
 * to exactly one already-covered ra8_fs_rmdir outcome: NULL path, a path with
 * no `name:` prefix, an unknown mount name, a directory that still holds a
 * file, and the removal that succeeds once it is emptied)
 */
static void test_rmdir(void)
{
  TEST_BEGIN("vfs rmdir");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("sd", setup_volume()));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_vfs_rmdir(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_io_vfs_rmdir("noprefix"));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_io_vfs_rmdir("nope:/SUB"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mkdir("sd:/SUB"));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_open("sd:/SUB/IN.BIN", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_err_not_empty, ra8_io_vfs_rmdir("sd:/SUB"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_unlink("sd:/SUB/IN.BIN"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_rmdir("sd:/SUB"));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_io_vfs_rmdir("sd:/SUB"));
  TEST_END("vfs rmdir");
}

int32_t main(void)
{
  test_mount_table();
  test_open_read();
  test_stat();
  test_rename_unlink();
  test_listdir_mkdir_unmount();
  test_rmdir();
  (void)fprintf(stderr, "[OK  ] test_ra8_io_vfs.c\n");
  return 0;
}
