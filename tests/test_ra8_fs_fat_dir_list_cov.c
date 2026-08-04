/**
 * @file test_ra8_fs_fat_dir_list_cov.c
 * @brief Coverage booster for libs/ra8_fs/src/ra8_fs_fat_dir.c -- listdir + mkdir.
 *
 * @details
 * Dedicated companion test executable that drives the ra8_fs_listdir guard and
 * error branches (unmounted handle, exFAT path guards, sector-read error, FAT
 * walk failure, full root with no EoD marker) plus the ra8_fs_mkdir guards and
 * the priv_fat_mkdir / priv_dir_cluster_init error paths (bad 8.3 leaf, full
 * directory, cluster allocation failure, cluster-init and dir-entry write
 * failures, and the SPC=2 extra-sector loop).
 *
 * The unlink / rename half of the suite lives in the split sibling
 * test_ra8_fs_fat_dir_mutate_cov.c. The shared block-device backends and
 * volume builders live in tests/support/fs_fat_dir_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "support/fs_fat_dir_test_util.h"
#include "unity_minimal.h"

/**
 * @enum dir_list_fill_t
 * @brief Per-test directory fill counts for the listdir vectors.
 *
 * @details A 512-byte FAT16 directory sector holds 16 32-byte entries; a
 *          subdirectory already carries "." and "..", so 14 extra files pack
 *          its first sector completely (no EoD marker left).
 *
 * @invariant k_fill_subdir_files + 2 == entries per 512-byte dir sector.
 * @see create_empty_files()
 */
typedef enum : uint8_t {
  k_fill_subdir_files = 14U, /**< Files that fill /SUB's first sector. */
} dir_list_fill_t;

/* ===========================================================================
 * Tests: ra8_fs_listdir guards
 * ===========================================================================
 */

/**
 * @test test_listdir_not_mounted
 * @brief listdir on an unmounted handle returns k_ra8_err_invalid_state (line 123).
 *
 * @details Mounts a FAT16 volume, clears in_use=0 to simulate an unmounted
 *          handle, then calls ra8_fs_listdir. The guard at line 122-123 fires.
 *
 * @par MC/DC:
 * Decision: `if (handle->in_use == 0U)` (line 122, 1 condition).
 * V1: in_use=1 -> false (normal path, tested by sibling tests).
 * V2: in_use=0 -> true -> line 123.
 * N=1 condition, 1 independent vector sufficient per DO-178C 6.4.4.3.
 *
 * @pre Volume is formatted; h is mounted.
 * @post Result is k_ra8_err_invalid_state.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_listdir_not_mounted(void)
{
  TEST_BEGIN("listdir: unmounted -> invalid_state (line 123)");
  build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  h->in_use = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_listdir(h, "/", count_cb, nullptr));
  h->in_use = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("listdir: unmounted -> invalid_state (line 123)");
}

/**
 * @test test_listdir_exfat_no_slash
 * @brief exFAT listdir with path not starting with '/' returns not_supported (line 128).
 *
 * @details Mounts an exFAT volume and passes a path whose first byte is not
 *          '/'. The guard at line 127-128 fires.
 *
 * @par MC/DC:
 * Decision: `if (path[0] != '/')` (line 127, 1 condition).
 * V1: path[0]='/' -> false (continued, tested at line 131).
 * V2: path[0]!='/' -> true -> line 128.
 *
 * @pre exFAT volume is formatted and mounted.
 * @post Result is k_ra8_err_not_supported.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_listdir_exfat_no_slash(void)
{
  TEST_BEGIN("listdir: exFAT no-slash path -> not_supported (line 128)");
  build_exfat_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_fs_listdir(h, "noslash", count_cb, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("listdir: exFAT no-slash path -> not_supported (line 128)");
}

/**
 * @test test_listdir_exfat_non_root
 * @brief exFAT listdir with path "/subdir" returns not_supported (line 131).
 *
 * @details Mounts an exFAT volume and passes path="/sub" (starts with '/'
 *          but path[1] != NUL). The guard at line 130-131 fires.
 *
 * @par MC/DC:
 * Decision: `if (path[1] != '\\0')` (line 130, 1 condition).
 * V1: path="/\\0" -> false (root path, passes to exfat_listdir).
 * V2: path="/sub" -> true -> line 131.
 *
 * @pre exFAT volume is formatted and mounted.
 * @post Result is k_ra8_err_not_supported.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_listdir_exfat_non_root(void)
{
  TEST_BEGIN("listdir: exFAT non-root path -> not_supported (line 131)");
  build_exfat_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_fs_listdir(h, "/sub", count_cb, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("listdir: exFAT non-root path -> not_supported (line 131)");
}

/**
 * @test test_listdir_read_error
 * @brief listdir fails immediately on first sector read error (line 149).
 *
 * @details Mounts FAT16, swaps to inject backend with reads_left=0.
 *          The first priv_read_sector call in the listdir loop fails.
 *          Line 148-149 is hit.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at line 148 (1 condition).
 * V1: err=k_ra8_ok -> false (sector read ok, continues to visit).
 * V2: err=k_ra8_err_hw_error -> true -> line 149.
 *
 * @pre FAT16 volume is mounted; inject backend has reads_left=0.
 * @post Result is k_ra8_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_listdir_read_error(void)
{
  TEST_BEGIN("listdir: immediate read error -> err (line 149)");
  build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_backend_t saved = h->backend;
  swap_to_inject(h, 0U, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_listdir(h, "/", count_cb, nullptr));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("listdir: immediate read error -> err (line 149)");
}

/**
 * @test test_listdir_walk_fail
 * @brief listdir on a full subdir sector fails at priv_dir_walk_next_sector (line 156).
 *
 * @details Creates /SUB, then fills it with 14 files so its first sector holds
 *          all 16 entries ("." + ".." + 14 files) with no EoD marker. Swaps to
 *          inject backend with reads_left=2 (read 1 = resolve /SUB in root,
 *          read 2 = first subdir sector; read 3 = FAT walk for next cluster fails).
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at line 155 (1 condition).
 * V1: err=k_ra8_ok -> false (walk advanced normally, eod set).
 * V2: err=k_ra8_err_hw_error -> true -> line 156.
 *
 * @pre FAT16 volume is mounted; /SUB contains 14 files (sector full).
 * @post Result is k_ra8_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_listdir_walk_fail(void)
{
  TEST_BEGIN("listdir: full subdir sector, walk fails -> err (line 156)");
  build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SUB"));
  create_empty_files(h, "/SUB", (uint32_t)k_fill_subdir_files);
  /* Remount so the FAT sector cache is cold (#607): creating those files
   * walked the FAT, and a cached sector never reaches the backend, so read 3
   * below would be served from memory and the walk would not fail. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_backend_t saved = h->backend;
  swap_to_inject(h, 2U, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_listdir(h, "/SUB", count_cb, nullptr));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("listdir: full subdir sector, walk fails -> err (line 156)");
}

/**
 * @test test_listdir_full_no_eod
 * @brief listdir with all 16 root slots filled returns k_ra8_ok at line 159.
 *
 * @details Creates 16 empty files in root, filling every dir entry slot so
 *          no EoD (0x00) marker remains. priv_listdir_visit_sector returns 0
 *          (no EoD hit). priv_dir_walk_next_sector then finds no more root
 *          sectors and sets eod=1. The while loop exits and line 159 is hit.
 *
 * @par MC/DC:
 * Decision: `while (eod == 0U)` at line 146 (1 condition).
 * V1: eod=0 -> true (loop body entered, normal case).
 * V2: eod=1 -> false (loop exits) -> line 159.
 * Both arms are exercised in this test (eod transitions 0 -> 1).
 *
 * @pre FAT16 volume is mounted; 16 files in root (no EoD marker in root sector).
 * @post Result is k_ra8_ok; count equals 16.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_listdir_full_no_eod(void)
{
  TEST_BEGIN("listdir: 16-entry root, no EoD -> k_ra8_ok at line 159");
  build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  create_empty_files(h, "/", 16U);
  uint32_t cnt = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", count_cb, &cnt));
  TEST_ASSERT_EQ(16U, cnt);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("listdir: 16-entry root, no EoD -> k_ra8_ok at line 159");
}

/* ===========================================================================
 * Tests: ra8_fs_mkdir public guards
 * ===========================================================================
 */

/**
 * @test test_mkdir_not_mounted
 * @brief mkdir on an unmounted handle returns k_ra8_err_invalid_state (line 355).
 *
 * @par MC/DC:
 * Decision: `if (handle->in_use == 0U)` (line 354, 1 condition).
 * V1: in_use=1 -> false (normal path).
 * V2: in_use=0 -> true -> line 355.
 *
 * @pre FAT16 volume is mounted; in_use forced to 0.
 * @post Result is k_ra8_err_invalid_state.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_mkdir_not_mounted(void)
{
  TEST_BEGIN("mkdir: unmounted -> invalid_state (line 355)");
  build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  h->in_use = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_mkdir(h, "/NEWDIR"));
  h->in_use = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("mkdir: unmounted -> invalid_state (line 355)");
}

/**
 * @test test_mkdir_exfat
 * @brief mkdir on an exFAT volume returns k_ra8_err_not_supported (line 358).
 *
 * @par MC/DC:
 * Decision: `if (handle->type == k_ra8_fs_type_exfat)` (line 357, 1 condition).
 * V1: type!=exfat -> false (FAT path, tested by sibling tests).
 * V2: type==exfat -> true -> line 358.
 *
 * @pre exFAT volume is formatted and mounted.
 * @post Result is k_ra8_err_not_supported.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_mkdir_exfat(void)
{
  TEST_BEGIN("mkdir: exFAT volume -> not_supported (line 358)");
  build_exfat_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_fs_mkdir(h, "/DIR"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("mkdir: exFAT volume -> not_supported (line 358)");
}

/* ===========================================================================
 * Tests: priv_fat_mkdir internal error paths
 * ===========================================================================
 */

/**
 * @test test_mkdir_bad_leaf
 * @brief mkdir with a name no encoding can hold returns k_ra8_err_invalid_arg.
 *
 * @details `"bad?name"` carries a `?`, which is illegal in a long name as well
 *          as in an 8.3 one, so ::priv_name_classify() reports
 *          `k_name_kind_invalid` and `priv_dir_reserve()` refuses. The sibling
 *          case `"bad name!"` -- illegal in 8.3 only, because of the space --
 *          is created as a long name since #600 and is covered in
 *          test_ra8_fs_lfn_write.c.
 *
 * @par MC/DC:
 * Decision: `if (kind == k_name_kind_invalid)` in `priv_dir_reserve()`
 * (1 condition).
 * V1: a storable name -> false (every other mkdir case here).
 * V2: `"bad?name"`     -> true  -> k_ra8_err_invalid_arg.
 *
 * @pre FAT16 volume is mounted.
 * @post Result is k_ra8_err_invalid_arg.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_mkdir_bad_leaf(void)
{
  TEST_BEGIN("mkdir: unstorable leaf -> invalid_arg");
  build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_mkdir(h, "/bad?name"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("mkdir: unstorable leaf -> invalid_arg");
}

/**
 * @test test_mkdir_dir_full
 * @brief mkdir when root dir is full returns the error from priv_dir_find_free (line 295).
 *
 * @details Fills all 16 root dir entry slots with empty files. A subsequent
 *          mkdir has no free slot -> priv_dir_find_free returns k_ra8_err_no_mem
 *          -> line 295.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at line 294 (1 condition).
 * V1: err=k_ra8_ok -> false (free slot found, continues).
 * V2: err!=k_ra8_ok -> true -> line 295.
 *
 * @pre FAT16 volume is mounted; 16 empty files fill all root dir slots.
 * @post Result is k_ra8_err_no_mem.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_mkdir_dir_full(void)
{
  TEST_BEGIN("mkdir: root dir full -> no_mem from dir_find_free (line 295)");
  build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  create_empty_files(h, "/", 16U);
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_mkdir(h, "/NOROOM"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("mkdir: root dir full -> no_mem from dir_find_free (line 295)");
}

/**
 * @test test_mkdir_alloc_fail
 * @brief mkdir fails at priv_alloc_eoc_cluster when first FAT write fails (line 300).
 *
 * @details Uses the wcount backend with writes_left=0 so the first write
 *          (FAT1 update inside priv_alloc_eoc_cluster) fails immediately.
 *          Line 299-300 is hit.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at line 299 (1 condition).
 * V1: err=k_ra8_ok -> false (cluster allocated, continues to cluster_init).
 * V2: err!=k_ra8_ok -> true -> line 300.
 *
 * @pre FAT16 volume is mounted; wcount writes_left=0.
 * @post Result is k_ra8_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_mkdir_alloc_fail(void)
{
  TEST_BEGIN("mkdir: alloc_eoc_cluster write fails -> err (line 300)");
  build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_backend_t saved = h->backend;
  swap_to_wcount(h, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_mkdir(h, "/NEWDIR"));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("mkdir: alloc_eoc_cluster write fails -> err (line 300)");
}

/**
 * @test test_mkdir_cluster_init_fail
 * @brief mkdir fails at priv_dir_cluster_init sector-0 write, triggers priv_free_chain
 *        (lines 233, 305-306).
 *
 * @details Uses wcount writes_left=2 on a SPC=1 volume. The two FAT writes
 *          inside priv_alloc_eoc_cluster succeed (writes 1 and 2). The third
 *          write (sector 0 of the new dir cluster inside priv_dir_cluster_init)
 *          fails. Line 233 propagates the error back to priv_fat_mkdir which
 *          then executes priv_free_chain (line 305) and returns (line 306).
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at line 232 in priv_dir_cluster_init (1 condition).
 * V1: err=k_ra8_ok -> false (sector 0 written ok, continues to loop).
 * V2: err!=k_ra8_ok -> true -> line 233.
 *
 * @pre FAT16 SPC=1 volume is mounted; wcount writes_left=2.
 * @post Result is k_ra8_err_hw_error (cluster is freed via line 305).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_mkdir_cluster_init_fail(void)
{
  TEST_BEGIN("mkdir: cluster_init sector-0 write fails -> free+return (lines 233,305-306)");
  build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_backend_t saved = h->backend;
  swap_to_wcount(h, 2U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_mkdir(h, "/NEWDIR"));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("mkdir: cluster_init sector-0 write fails -> free+return (lines 233,305-306)");
}

/**
 * @test test_mkdir_write_entry_fail
 * @brief mkdir fails at priv_write_new_dir_entry, triggers priv_free_chain (lines 315-316).
 *
 * @details Uses wcount writes_left=3 on a SPC=1 volume. Writes 1-2 are FAT
 *          updates (priv_alloc_eoc_cluster). Write 3 is cluster sector-0
 *          (priv_dir_cluster_init, succeeds). Write 4 (priv_write_new_dir_entry
 *          to root dir sector) fails. Lines 314-316 are hit.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at line 314 (1 condition).
 * V1: err=k_ra8_ok -> false (dir entry written, returns k_ra8_ok).
 * V2: err!=k_ra8_ok -> true -> lines 315-316.
 *
 * @pre FAT16 SPC=1 volume is mounted; wcount writes_left=3.
 * @post Result is k_ra8_err_hw_error (cluster freed via line 315).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_mkdir_write_entry_fail(void)
{
  TEST_BEGIN("mkdir: write_new_dir_entry fails -> free+return (lines 315-316)");
  build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_backend_t saved = h->backend;
  swap_to_wcount(h, 3U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_mkdir(h, "/NEWDIR"));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("mkdir: write_new_dir_entry fails -> free+return (lines 315-316)");
}

/**
 * @test test_mkdir_spc2_ok
 * @brief mkdir on a SPC=2 volume succeeds, covering the extra-sector loop (lines 237-238, 241).
 *
 * @details On a SPC=2 volume, priv_dir_cluster_init writes sector 0 (dot
 *          entries) then enters the loop for s=1 (line 237: write sector 1),
 *          checks result (line 238: condition false), and the loop ends
 *          (line 241: closing brace). The directory is created successfully.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at line 238 inside the SPC loop (1 condition).
 * V1: err=k_ra8_ok -> false (loop iteration ok) -> line 241 (covered here).
 * V2: err!=k_ra8_ok -> true -> line 239 (covered by test_mkdir_spc2_second_fail).
 *
 * @pre FAT16 SPC=2 volume is mounted.
 * @post Result is k_ra8_ok; /SUB2 is visible in root listing.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_mkdir_spc2_ok(void)
{
  TEST_BEGIN("mkdir SPC=2: extra-sector loop succeeds (lines 237-238, 241)");
  build_fat16_spc2_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SUB2"));
  uint32_t cnt = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", count_cb, &cnt));
  TEST_ASSERT_EQ(1U, cnt);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("mkdir SPC=2: extra-sector loop succeeds (lines 237-238, 241)");
}

/**
 * @test test_mkdir_spc2_second_fail
 * @brief mkdir on SPC=2 volume fails at the extra sector write in the loop (line 239).
 *
 * @details Uses wcount writes_left=3. Writes 1-2 are FAT updates (alloc_eoc).
 *          Write 3 is sector 0 of the cluster (priv_dir_cluster_init, ok).
 *          Write 4 is sector 1 of the cluster (the loop body at line 237, SPC=2);
 *          this fails -> line 239.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at line 238 inside the SPC loop (1 condition).
 * V1: err=k_ra8_ok -> false -> covered by test_mkdir_spc2_ok.
 * V2: err!=k_ra8_ok -> true -> line 239 (covered here).
 *
 * @pre FAT16 SPC=2 volume is mounted; wcount writes_left=3.
 * @post Result is k_ra8_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_mkdir_spc2_second_fail(void)
{
  TEST_BEGIN("mkdir SPC=2: extra-sector write fails -> err (line 239)");
  build_fat16_spc2_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_backend_t saved = h->backend;
  swap_to_wcount(h, 3U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_mkdir(h, "/NEWDIR"));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_vol();
  TEST_END("mkdir SPC=2: extra-sector write fails -> err (line 239)");
}
/* ===========================================================================
 * Entry point
 * ===========================================================================
 */

/**
 * @brief Test executable entry point.
 *
 * @details Runs the listdir + mkdir coverage tests in sequence. Each test is
 *          self-contained: it builds the volume, mounts, exercises the target
 *          branches, unmounts, and frees the disk. Failure exits via
 *          TEST_FAIL_FMT (exit(1)).
 *
 * @return 0 on success (all tests passed).
 *
 * @pre Host environment provides calloc/free and stderr.
 * @post The targeted listdir/mkdir branches in ra8_fs_fat_dir.c are exercised.
 *
 * @note Not thread-safe (single-threaded test runner).
 * @since 0.1.0
 */
int main(void)
{
  test_listdir_not_mounted();
  test_listdir_exfat_no_slash();
  test_listdir_exfat_non_root();
  test_listdir_read_error();
  test_listdir_walk_fail();
  test_listdir_full_no_eod();

  test_mkdir_not_mounted();
  test_mkdir_exfat();
  test_mkdir_bad_leaf();
  test_mkdir_dir_full();
  test_mkdir_alloc_fail();
  test_mkdir_cluster_init_fail();
  test_mkdir_write_entry_fail();
  test_mkdir_spc2_ok();
  test_mkdir_spc2_second_fail();

  return 0;
}
