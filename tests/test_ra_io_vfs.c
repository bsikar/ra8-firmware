/**
 * @file test_ra_io_vfs.c
 * @brief Unit tests for the ra_io VFS mount table + path router (issue #158).
 *
 * @details
 * Sets up a FAT16 volume on a RAM block device, registers it under a name, and
 * exercises name-based open / stat / unlink / rename / listdir, mount-table
 * mechanics (duplicate, full, unmount isolation), path-parse rejection, and the
 * not-yet-supported mkdir.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra_err.h"
#include "ra_fs.h"
#include "ra_io_blockdev.h"
#include "ra_io_blockdev_ram.h"
#include "ra_io_vfs.h"
#include "unity_minimal.h"

/**
 * @enum t_vfs_const_t
 * @brief Fixture sizes.
 */
typedef enum : uint32_t {
  k_t_disk_blocks = 16384, /**< 8 MiB -- comfortably FAT16. */
  k_t_payload     = 100,   /**< Bytes in the seeded file.   */
} t_vfs_const_t;

/** @brief 8 MiB backing buffer. */
static uint8_t s_disk[(size_t)k_t_disk_blocks * (size_t)k_ra_io_block_size_bytes];
static ra_io_blockdev_ram_state_t s_bstate;
static ra_io_blockdev_t           s_bd;
static ra_fs_backend_t            s_be;
static ra_fs_mount_t*             s_mnt;

/**
 * @brief Build a fresh FAT16 volume with one seeded file and return its mount.
 */
static ra_fs_mount_t* setup_volume(void)
{
  if (s_mnt != nullptr) {
    (void)ra_fs_unmount(s_mnt); /* ra_fs has only 2 mount slots -- free the prior one */
    s_mnt = nullptr;
  }
  (void)ra_io_blockdev_ram_init(&s_bd, &s_bstate, s_disk, k_t_disk_blocks, false);
  (void)ra_io_blockdev_as_fs_backend(&s_bd, &s_be);
  ra_fs_format_opts_t opts = {};
  opts.type                = k_ra_fs_type_fat16;
  opts.label               = "VFS";
  (void)ra_fs_format(&s_be, &opts);
  s_mnt = nullptr;
  (void)ra_fs_mount(&s_be, &s_mnt);
  uint8_t data[(size_t)k_t_payload];
  for (uint32_t i = 0; i < (uint32_t)k_t_payload; ++i) {
    data[i] = (uint8_t)((i + 1u) & 0xFFu);
  }
  (void)ra_fs_write_file(s_mnt, "HELLO.BIN", data, k_t_payload);
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
  TEST_ASSERT_EQ(k_ra_ok, ra_io_vfs_init());
  ra_fs_mount_t* m = setup_volume();
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_io_vfs_mount(nullptr, m));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_io_vfs_mount("sd", nullptr));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_io_vfs_mount("a:b", m)); /* reserved ':' */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_io_vfs_mount("", m));    /* empty */
  TEST_ASSERT_EQ(k_ra_ok, ra_io_vfs_mount("sd", m));
  TEST_ASSERT_EQ(k_ra_err_exists, ra_io_vfs_mount("sd", m)); /* duplicate */
  /* fill remaining slots, then overflow */
  TEST_ASSERT_EQ(k_ra_ok, ra_io_vfs_mount("a", m));
  TEST_ASSERT_EQ(k_ra_ok, ra_io_vfs_mount("b", m));
  TEST_ASSERT_EQ(k_ra_ok, ra_io_vfs_mount("c", m));
  TEST_ASSERT_EQ(k_ra_err_no_mem, ra_io_vfs_mount("d", m));
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
  TEST_ASSERT_EQ(k_ra_ok, ra_io_vfs_init());
  TEST_ASSERT_EQ(k_ra_ok, ra_io_vfs_mount("sd", setup_volume()));

  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_io_vfs_open("sd:/HELLO.BIN", k_ra_fs_mode_read, nullptr));
  ra_fs_file_t*  f = nullptr;
  const ra_err_t e = ra_io_vfs_open("noprefix", k_ra_fs_mode_read, &f);
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, e);
  TEST_ASSERT_EQ(k_ra_err_not_found, ra_io_vfs_open("nope:/X", k_ra_fs_mode_read, &f));

  TEST_ASSERT_EQ(k_ra_ok, ra_io_vfs_open("sd:/HELLO.BIN", k_ra_fs_mode_read, &f));
  uint8_t  got[(size_t)k_t_payload] = {};
  uint32_t got_len                  = 0;
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_read(f, got, k_t_payload, &got_len));
  TEST_ASSERT_EQ(k_t_payload, got_len);
  TEST_ASSERT_EQ(1, got[0]);
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_close(f));
  TEST_END("vfs open/read");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- stat reports presence/size, a missing
 * file yields exists==false with ok)
 */
static void test_stat(void)
{
  TEST_BEGIN("vfs stat");
  TEST_ASSERT_EQ(k_ra_ok, ra_io_vfs_init());
  TEST_ASSERT_EQ(k_ra_ok, ra_io_vfs_mount("sd", setup_volume()));
  ra_io_vfs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_io_vfs_stat("sd:/HELLO.BIN", &st));
  TEST_ASSERT(st.exists);
  TEST_ASSERT_EQ(k_t_payload, st.size_bytes);
  TEST_ASSERT(!st.is_directory);
  ra_io_vfs_stat_t miss = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_io_vfs_stat("sd:/GONE.BIN", &miss));
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
  TEST_ASSERT_EQ(k_ra_ok, ra_io_vfs_init());
  TEST_ASSERT_EQ(k_ra_ok, ra_io_vfs_mount("sd", setup_volume()));

  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_io_vfs_rename("sd:/HELLO.BIN", "ram:/BYE.BIN"));
  TEST_ASSERT_EQ(k_ra_ok, ra_io_vfs_rename("sd:/HELLO.BIN", "sd:/BYE.BIN"));
  ra_io_vfs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_io_vfs_stat("sd:/BYE.BIN", &st));
  TEST_ASSERT(st.exists);

  TEST_ASSERT_EQ(k_ra_ok, ra_io_vfs_unlink("sd:/BYE.BIN"));
  ra_io_vfs_stat_t gone = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_io_vfs_stat("sd:/BYE.BIN", &gone));
  TEST_ASSERT(!gone.exists);
  TEST_END("vfs rename/unlink");
}

/** @brief listdir callback: count entries. */
static void count_cb(const char* name, uint8_t attr, uint32_t size, void* ctx)
{
  (void)name;
  (void)attr;
  (void)size;
  uint32_t* n = (uint32_t*)ctx;
  (*n)++;
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- listdir visits the seeded file; mkdir is
 * not yet supported; unmount makes a name stop resolving)
 */
static void test_listdir_mkdir_unmount(void)
{
  TEST_BEGIN("vfs listdir/mkdir/unmount");
  TEST_ASSERT_EQ(k_ra_ok, ra_io_vfs_init());
  TEST_ASSERT_EQ(k_ra_ok, ra_io_vfs_mount("sd", setup_volume()));
  TEST_ASSERT_EQ(k_ra_ok, ra_io_vfs_mount("ospi", s_mnt));

  uint32_t n = 0;
  TEST_ASSERT_EQ(k_ra_ok, ra_io_vfs_listdir("sd:/", count_cb, &n));
  TEST_ASSERT(n >= 1u);

  TEST_ASSERT_EQ(k_ra_err_not_supported, ra_io_vfs_mkdir("sd:/sub"));

  /* unmount isolation: dropping "sd" leaves "ospi" working */
  TEST_ASSERT_EQ(k_ra_ok, ra_io_vfs_unmount("sd"));
  ra_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra_err_not_found, ra_io_vfs_open("sd:/HELLO.BIN", k_ra_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra_ok, ra_io_vfs_open("ospi:/HELLO.BIN", k_ra_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra_ok, ra_fs_close(f));
  TEST_ASSERT_EQ(k_ra_err_not_found, ra_io_vfs_unmount("sd"));
  TEST_END("vfs listdir/mkdir/unmount");
}

int32_t main(void)
{
  test_mount_table();
  test_open_read();
  test_stat();
  test_rename_unlink();
  test_listdir_mkdir_unmount();
  (void)fprintf(stderr, "[OK  ] test_ra_io_vfs.c\n");
  return 0;
}
