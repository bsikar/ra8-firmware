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
#include <string.h>

#include "ra8_attributes.h"
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
 *          its first sector completely (no EoD marker left). One more file
 *          spills into the second sector of an SPC=2 cluster, which is what
 *          the cursor test needs to make the walk cross a sector boundary.
 *
 * @invariant k_fill_subdir_files + 2 == entries per 512-byte dir sector.
 * @invariant k_fill_spc2_sub_files == k_fill_subdir_files + 1.
 * @see create_empty_files()
 */
typedef enum : uint8_t {
  k_fill_subdir_files   = 14U, /**< Files that fill /SUB's first sector. */
  k_fill_spc2_sub_files = 15U, /**< ...plus one in its second sector.    */
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
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_listdir_not_mounted(void)
{
  TEST_BEGIN("listdir: unmounted -> invalid_state (line 123)");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  h->in_use = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_listdir(h, "/", internal_count_cb, nullptr));
  h->in_use = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("listdir: unmounted -> invalid_state (line 123)");
}

/**
 * @test test_listdir_exfat_relative_name
 * @brief exFAT listdir resolves a leading-slash-free path like any other.
 *
 * @details This used to be a `k_ra8_err_not_supported` guard: exFAT listing was
 *          root-only, so anything that was not exactly `"/"` was declined before
 *          a lookup happened. With the namespace no longer flat (#605) a path is
 *          RESOLVED, and a name that is not there is a lookup failure like it
 *          always was on FAT -- the two filesystems no longer disagree about
 *          what "list this path" means.
 *
 * @par MC/DC:
 * No compound decision lies on this path. The vector it contributes is
 * `handle->type == k_ra8_fs_type_exfat`
 * (libs/ra8_fs/src/ra8_fs_fat_dir.c@internal_listdir_locked) -> true, followed by
 * the not-found arm of
 * libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@priv_exfat_resolve_dir.
 *
 * @pre exFAT volume is formatted and mounted.
 * @post Result is k_ra8_err_not_found, not a blanket refusal.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_listdir_exfat_relative_name(void)
{
  TEST_BEGIN("listdir: exFAT relative name -> not_found");
  internal_build_exfat_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_listdir(h, "noslash", internal_count_cb, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("listdir: exFAT relative name -> not_found");
}

/**
 * @test test_listdir_exfat_missing_subdir
 * @brief exFAT listdir of a subdirectory that does not exist reports not_found.
 *
 * @details The companion to test_listdir_exfat_relative_name(): a non-root path
 *          is no longer refused for BEING non-root, so the only thing left to
 *          report is that nothing answers to that name. A volume that does hold
 *          the directory lists it, which `test_ra8_fs_exfat_dirs.c` asserts.
 *
 * @par MC/DC:
 * No compound decision lies on this path. Same dispatch vector as
 * test_listdir_exfat_relative_name, reached with a rooted path instead of a
 * relative one -- both now resolve rather than being refused for their shape.
 *
 * @pre exFAT volume is formatted and mounted.
 * @post Result is k_ra8_err_not_found.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_listdir_exfat_missing_subdir(void)
{
  TEST_BEGIN("listdir: exFAT missing subdirectory -> not_found");
  internal_build_exfat_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_listdir(h, "/sub", internal_count_cb, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("listdir: exFAT missing subdirectory -> not_found");
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
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_listdir_read_error(void)
{
  TEST_BEGIN("listdir: immediate read error -> err (line 149)");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_backend_t saved = h->backend;
  internal_swap_to_inject(h, 0U, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_listdir(h, "/", internal_count_cb, nullptr));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
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
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_listdir_walk_fail(void)
{
  TEST_BEGIN("listdir: full subdir sector, walk fails -> err (line 156)");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SUB"));
  internal_create_empty_files(h, "/SUB", (uint32_t)k_fill_subdir_files);
  /* Remount so the FAT sector cache is cold (#607): creating those files
   * walked the FAT, and a cached sector never reaches the backend, so read 3
   * below would be served from memory and the walk would not fail. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_backend_t saved = h->backend;
  internal_swap_to_inject(h, 2U, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_listdir(h, "/SUB", internal_count_cb, nullptr));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
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
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_listdir_full_no_eod(void)
{
  TEST_BEGIN("listdir: 16-entry root, no EoD -> k_ra8_ok at line 159");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  internal_create_empty_files(h, "/", 16U);
  uint32_t cnt = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", internal_count_cb, &cnt));
  TEST_ASSERT_EQ(16U, cnt);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("listdir: 16-entry root, no EoD -> k_ra8_ok at line 159");
}

/* ===========================================================================
 * Tests: ra8_fs_dir_open / ra8_fs_dir_next cursor walk
 * ===========================================================================
 */

/**
 * @test test_mcdc_dir_next_sector_exhausted
 * @brief The cursor crosses a sector that yields no entry and no end marker.
 *
 * @details On an SPC=2 volume /SUB's single cluster spans two 512-byte sectors
 *          (32 entries). "." and ".." take the first two slots and 14 files
 *          fill the rest of sector 0; the 15th lands as the first entry of
 *          sector 1. Unlinking those 14 leaves sector 0 holding nothing but
 *          two dot slots and fourteen 0xE5 deleted slots, and no 0x00
 *          end-of-directory marker, so the scan runs the sector out without
 *          copying an entry and without reaching the end of the directory --
 *          the only state in which the cursor advances to the next sector.
 *
 * @par MC/DC:
 * Decision: `if (*out_entry || state->finished)` in
 * `libs/ra8_fs/src/ra8_fs_fat_dir.c@internal_fat_dir_next` (2 conditions).
 * - V1: out_entry=false, finished=false -> F,F -> dec F: sector 0 is exhausted
 *       by the dot and deleted slots, so the walk advances to sector 1.
 * - V2: out_entry=true (finished not evaluated) -> C1=T -> dec T: sector 1
 *       holds the surviving file, which this same first `dir_next` returns.
 * - V3: out_entry=false, finished=true -> C1=F, C2=T -> dec T: the second
 *       `dir_next` meets the 0x00 end marker behind that file and reports EOF.
 * V1+V2 prove `*out_entry` independently drives the outcome; V1+V3 prove the
 * same for `state->finished`. N+1 = 3 vectors for N=2 conditions: minimal
 * MC/DC. The decision's third condition disappeared with the dead `ra8_err_t`
 * return of internal_fat_dir_scan_sector(), which had no failing path at all.
 *
 * @pre A FAT16 SPC=2 volume is built and mounted.
 * @pre /SUB holds one surviving file in its second sector, behind a first
 *      sector consumed entirely by dot and deleted slots.
 * @post Both `dir_next` calls return k_ra8_ok.
 * @post The first reports the surviving file and the second reports clean EOF.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_mcdc_dir_next_sector_exhausted(void)
{
  TEST_BEGIN("dir cursor MC/DC: sector runs out with no entry and no end marker");
  internal_build_fat16_spc2_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SUB"));
  internal_create_empty_files(h, "/SUB", (uint32_t)k_fill_spc2_sub_files);
  internal_unlink_empty_files(h, "/SUB", (uint32_t)k_fill_subdir_files);

  ra8_fs_dir_t    directory = {};
  ra8_fs_dirent_t entry     = {};
  bool            present   = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_dir_open(h, "/SUB", &directory));
  /* V1 then V2 inside one call: sector 0 is exhausted by two dot slots and
   * fourteen deleted ones, so the walk advances and finds the survivor. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_dir_next(&directory, &entry, &present));
  TEST_ASSERT(present);
  TEST_ASSERT(strcmp(entry.name, "G14.TXT") == 0);
  /* V3: the 0x00 end marker behind that entry ends the walk. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_dir_next(&directory, &entry, &present));
  TEST_ASSERT(!present);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_dir_close(&directory));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("dir cursor MC/DC: sector runs out with no entry and no end marker");
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
 * @since 0.1.0 @details Runs the mkdir not mounted vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_mkdir_not_mounted(void)
{
  TEST_BEGIN("mkdir: unmounted -> invalid_state (line 355)");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  h->in_use = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_mkdir(h, "/NEWDIR"));
  h->in_use = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("mkdir: unmounted -> invalid_state (line 355)");
}

/**
 * @test test_mkdir_exfat_dispatches
 * @brief mkdir on an exFAT volume reaches the exFAT creator, not a refusal.
 *
 * @details The dispatch vector for `priv_mkdir_locked`: this used to answer
 *          `k_ra8_err_not_supported` before touching the volume, and now takes
 *          the exFAT branch and creates a directory (#605). The behaviour of
 *          that creator is exercised in `test_ra8_fs_exfat_dirs.c`; what this
 *          asserts is that the exFAT arm is TAKEN.
 *
 * @par MC/DC:
 * Decision: `if (handle->type == k_ra8_fs_type_exfat)`
 * (libs/ra8_fs/src/ra8_fs_fat_dirmk.c@priv_mkdir_locked, 1 condition).
 * V1: type != exfat -> false -> priv_fat_mkdir (the sibling tests here).
 * V2: type == exfat -> true  -> priv_exfat_mkdir (this test).
 *
 * @pre exFAT volume is formatted and mounted.
 * @post The directory exists and reports itself as one.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_mkdir_exfat_dispatches(void)
{
  TEST_BEGIN("mkdir: exFAT volume -> the exFAT creator");
  internal_build_exfat_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/DIR"));
  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/DIR", &st));
  TEST_ASSERT_EQ(true, st.is_directory);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("mkdir: exFAT volume -> the exFAT creator");
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
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_mkdir_bad_leaf(void)
{
  TEST_BEGIN("mkdir: unstorable leaf -> invalid_arg");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_mkdir(h, "/bad?name"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
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
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_mkdir_dir_full(void)
{
  TEST_BEGIN("mkdir: root dir full -> no_mem from dir_find_free (line 295)");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  internal_create_empty_files(h, "/", 16U);
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_mkdir(h, "/NOROOM"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
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
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_mkdir_alloc_fail(void)
{
  TEST_BEGIN("mkdir: alloc_eoc_cluster write fails -> err (line 300)");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_backend_t saved = h->backend;
  internal_swap_to_wcount(h, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_mkdir(h, "/NEWDIR"));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
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
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_mkdir_cluster_init_fail(void)
{
  TEST_BEGIN("mkdir: cluster_init sector-0 write fails -> free+return (lines 233,305-306)");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_backend_t saved = h->backend;
  internal_swap_to_wcount(h, 2U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_mkdir(h, "/NEWDIR"));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
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
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_mkdir_write_entry_fail(void)
{
  TEST_BEGIN("mkdir: write_new_dir_entry fails -> free+return (lines 315-316)");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_backend_t saved = h->backend;
  internal_swap_to_wcount(h, 3U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_mkdir(h, "/NEWDIR"));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
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
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_mkdir_spc2_ok(void)
{
  TEST_BEGIN("mkdir SPC=2: extra-sector loop succeeds (lines 237-238, 241)");
  internal_build_fat16_spc2_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SUB2"));
  uint32_t cnt = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", internal_count_cb, &cnt));
  TEST_ASSERT_EQ(1U, cnt);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
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
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_mkdir_spc2_second_fail(void)
{
  TEST_BEGIN("mkdir SPC=2: extra-sector write fails -> err (line 239)");
  internal_build_fat16_spc2_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_backend_t saved = h->backend;
  internal_swap_to_wcount(h, 3U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_mkdir(h, "/NEWDIR"));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("mkdir SPC=2: extra-sector write fails -> err (line 239)");
}
/* ===========================================================================
 * Entry point
 * ===========================================================================
 */

/**
 * @brief Test executable entry point.
 *
 * @details Runs the listdir + cursor + mkdir coverage tests in sequence. Each test is
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
  internal_test_listdir_not_mounted();
  internal_test_listdir_exfat_relative_name();
  internal_test_listdir_exfat_missing_subdir();
  internal_test_listdir_read_error();
  internal_test_listdir_walk_fail();
  internal_test_listdir_full_no_eod();

  internal_test_mcdc_dir_next_sector_exhausted();

  internal_test_mkdir_not_mounted();
  internal_test_mkdir_exfat_dispatches();
  internal_test_mkdir_bad_leaf();
  internal_test_mkdir_dir_full();
  internal_test_mkdir_alloc_fail();
  internal_test_mkdir_cluster_init_fail();
  internal_test_mkdir_write_entry_fail();
  internal_test_mkdir_spc2_ok();
  internal_test_mkdir_spc2_second_fail();

  return 0;
}
