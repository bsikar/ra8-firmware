/**
 * @file test_ra8_fs_fat_dir_mutate_cov.c
 * @brief Coverage booster for libs/ra8_fs/src/ra8_fs_fat_dir.c -- unlink + rename.
 *
 * @details
 * Dedicated companion test executable that drives the ra8_fs_unlink guards and
 * error paths (unmounted handle, missing parent, bad 8.3 leaf, priv_free_chain
 * FAT-read failure, dir-sector re-read failure) plus the ra8_fs_rename guards
 * and the priv_rename_prepare / priv_fat_rename error paths (null pointers,
 * unmounted handle, missing new-path parent, root-vs-subdir and cross-subdir
 * rejects, bad 8.3 names on either side, existing target, missing source, and
 * the sector re-read failure after the old name is found).
 *
 * The listdir / mkdir half of the suite lives in the split sibling
 * test_ra8_fs_fat_dir_list_cov.c. The shared block-device backends and volume
 * builders live in tests/support/fs_fat_dir_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "support/fs_fat_dir_test_util.h"
#include "unity_minimal.h"

/**
 * @enum dir_mutate_fixture_t
 * @brief Sizes for the small data payloads written by the unlink vectors.
 *
 * @invariant k_payload_len is small enough to fit one cluster.
 * @see test_unlink_free_chain_fail()
 */
typedef enum : uint8_t {
  k_payload_len = 8U, /**< Bytes of file content given to /DATA.TXT. */
} dir_mutate_fixture_t;

/* ===========================================================================
 * Tests: ra8_fs_unlink guards and error paths
 * ===========================================================================
 */

/**
 * @test test_unlink_not_mounted
 * @brief unlink on an unmounted handle returns k_ra8_err_invalid_state (line 395).
 *
 * @par MC/DC:
 * Decision: `if (handle->in_use == 0U)` (line 394, 1 condition).
 * V1: in_use=1 -> false (normal path).
 * V2: in_use=0 -> true -> line 395.
 *
 * @pre FAT16 volume is mounted; in_use forced to 0.
 * @post Result is k_ra8_err_invalid_state.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @details Runs the unlink not mounted vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_unlink_not_mounted(void)
{
  TEST_BEGIN("unlink: unmounted -> invalid_state (line 395)");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  h->in_use = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_unlink(h, "/F.TXT"));
  h->in_use = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("unlink: unmounted -> invalid_state (line 395)");
}

/**
 * @test test_unlink_bad_parent
 * @brief unlink on a path with a non-existent parent returns an error (line 404).
 *
 * @details priv_resolve_parent("/NODIR/F.TXT") tries to enter /NODIR which
 *          does not exist -> returns k_ra8_err_not_found. Line 403-404 is hit.
 *
 * @par MC/DC:
 * Decision: `if (rerr != k_ra8_ok)` at line 403 (1 condition).
 * V1: rerr=k_ra8_ok -> false (parent resolved ok).
 * V2: rerr!=k_ra8_ok -> true -> line 404.
 *
 * @pre FAT16 volume is mounted; /NODIR does not exist.
 * @post Result is k_ra8_err_not_found.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_unlink_bad_parent(void)
{
  TEST_BEGIN("unlink: missing parent dir -> err (line 404)");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_unlink(h, "/NODIR/F.TXT"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("unlink: missing parent dir -> err (line 404)");
}

/**
 * @test test_unlink_bad_83
 * @brief unlink with a name that is not 8.3 now reports not_found, not invalid_arg.
 *
 * @details Deleting never had to pack a name to 8.3 -- it has to FIND one. Since
 *          #600, `"bad name!"` is a name a volume could genuinely hold, so the
 *          honest answer for a volume that does not hold it is
 *          `k_ra8_err_not_found`; `priv_dir_lookup_any()` misses on both the 8.3
 *          and the long-name pass. The old `k_ra8_err_invalid_arg` said "that is
 *          not a name", which stopped being true.
 *
 * @par MC/DC:
 * Decision: `if (err == k_ra8_err_not_found)` in `priv_dir_lookup_any()`
 * (1 condition).
 * V1: the 8.3 pass matched -> false (every other unlink case here).
 * V2: it did not           -> true  -> the long-name pass runs and also misses.
 *
 * @pre FAT16 volume is mounted.
 * @post Result is k_ra8_err_not_found and the volume is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_unlink_bad_83(void)
{
  TEST_BEGIN("unlink: a long name that is absent -> not_found");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_unlink(h, "/bad name!"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("unlink: a long name that is absent -> not_found");
}

/**
 * @test test_unlink_free_chain_fail
 * @brief unlink on a file with data fails at priv_free_chain FAT read (line 421).
 *
 * @details Creates /DATA.TXT with content (first_cluster >= 2). Swaps to
 *          inject backend with reads_left=1. Read 1 is the priv_dir_find scan
 *          of root dir (succeeds). Read 2 is priv_fat_get inside priv_free_chain
 *          to find next cluster (fails) -> line 421.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at line 420 (1 condition).
 * V1: err=k_ra8_ok -> false (chain freed, continues).
 * V2: err!=k_ra8_ok -> true -> line 421.
 *
 * @pre FAT16 volume mounted; /DATA.TXT has data; inject reads_left=1.
 * @post Result is k_ra8_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_unlink_free_chain_fail(void)
{
  TEST_BEGIN("unlink: free_chain FAT read fails -> err (line 421)");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  uint8_t payload[k_payload_len] = {};
  for (uint32_t i = 0; i < (uint32_t)k_payload_len; i++) {
    payload[i] = (uint8_t)(i + 1U);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "/DATA.TXT", payload, sizeof(payload)));
  ra8_fs_backend_t saved = h->backend;
  internal_swap_to_inject(h, 1U, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_unlink(h, "/DATA.TXT"));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("unlink: free_chain FAT read fails -> err (line 421)");
}

/**
 * @test test_unlink_dir_read_fail
 * @brief unlink on an empty file fails at sector re-read for deletion (line 427).
 *
 * @details Creates /EMPTY.TXT with no data (first_cluster=0). Swaps to inject
 *          backend with reads_left=1. Read 1 is the priv_dir_find scan of root
 *          dir (succeeds, finds entry). first_cluster=0 < 2 so priv_free_chain
 *          is skipped. Read 2 is priv_read_sector at line 425 to re-load the
 *          sector for deletion marking (fails) -> line 427.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at line 426 (1 condition).
 * V1: err=k_ra8_ok -> false (sector read, marks 0xE5 and writes).
 * V2: err!=k_ra8_ok -> true -> line 427.
 *
 * @pre FAT16 volume mounted; /EMPTY.TXT is empty; inject reads_left=1.
 * @post Result is k_ra8_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_unlink_dir_read_fail(void)
{
  TEST_BEGIN("unlink: empty file, dir sector re-read fails -> err (line 427)");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "/EMPTY.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  ra8_fs_backend_t saved = h->backend;
  internal_swap_to_inject(h, 1U, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_unlink(h, "/EMPTY.TXT"));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("unlink: empty file, dir sector re-read fails -> err (line 427)");
}

/* ===========================================================================
 * Tests: ra8_fs_rename guards
 * ===========================================================================
 */

/**
 * @test test_rename_null_guards
 * @brief ra8_fs_rename returns k_ra8_err_null_ptr for null handle/old/new (lines 582,585,588).
 *
 * @details Three single-condition checks at lines 581, 584, 587. Each is a
 *          trivial null check; all three must fire to cover lines 582, 585, 588.
 *
 * @par MC/DC:
 * Decisions: three independent `if (X == nullptr)` checks (1 condition each).
 * V1: handle=null -> line 582.
 * V2: handle=ok, old_path=null -> line 585.
 * V3: handle=ok, old_path=ok, new_path=null -> line 588.
 * Three vectors for three conditions, each independently covered.
 *
 * @pre FAT16 volume is mounted.
 * @post All three guard checks return k_ra8_err_null_ptr.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_rename_null_guards(void)
{
  TEST_BEGIN("rename: null handle/old/new -> null_ptr (lines 582,585,588)");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_rename(nullptr, "/A.TXT", "/B.TXT"));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_rename(h, nullptr, "/B.TXT"));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_rename(h, "/A.TXT", nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("rename: null handle/old/new -> null_ptr (lines 582,585,588)");
}

/**
 * @test test_rename_not_mounted
 * @brief rename on unmounted handle returns k_ra8_err_invalid_state (line 591).
 *
 * @par MC/DC:
 * Decision: `if (handle->in_use == 0U)` (line 590, 1 condition).
 * V1: in_use=1 -> false (normal path).
 * V2: in_use=0 -> true -> line 591.
 *
 * @pre FAT16 volume is mounted; in_use forced to 0.
 * @post Result is k_ra8_err_invalid_state.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @details Runs the rename not mounted vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_rename_not_mounted(void)
{
  TEST_BEGIN("rename: unmounted -> invalid_state (line 591)");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  h->in_use = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_rename(h, "/A.TXT", "/B.TXT"));
  h->in_use = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("rename: unmounted -> invalid_state (line 591)");
}

/* ===========================================================================
 * Tests: priv_rename_prepare error paths
 * ===========================================================================
 */

/**
 * @test test_rename_new_parent_fail
 * @brief rename where new_path has a missing parent returns an error (line 474).
 *
 * @details priv_resolve_parent for old_path ("/F.TXT") succeeds (root).
 *          priv_resolve_parent for new_path ("/NODIR/G.TXT") fails because
 *          /NODIR does not exist -> returns k_ra8_err_not_found. Line 473-474 is hit.
 *
 * @par MC/DC:
 * Decision: `if (e2 != k_ra8_ok)` at line 473 (1 condition).
 * V1: e2=k_ra8_ok -> false (new parent resolved).
 * V2: e2!=k_ra8_ok -> true -> line 474.
 *
 * @pre FAT16 volume is mounted; /NODIR does not exist.
 * @post Result is k_ra8_err_not_found.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_rename_new_parent_fail(void)
{
  TEST_BEGIN("rename: new_path missing parent -> err (line 474)");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_rename(h, "/F.TXT", "/NODIR/G.TXT"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("rename: new_path missing parent -> err (line 474)");
}

/**
 * @test test_rename_root_vs_subdir
 * @brief rename across root/subdir boundary returns not_supported (line 480).
 *
 * @details Creates /SUB. rename("/F.TXT", "/SUB/F.TXT"): old_path resolves to
 *          root (is_root=1), new_path resolves to /SUB (is_root=0). The
 *          is_root mismatch check at line 482-483 fires -> line 480 returned.
 *
 * @par MC/DC:
 * Decision: `if (op.is_root != np.is_root)` (line 482, 1 condition).
 * V1: both root -> false (continues).
 * V2: root vs subdir -> true -> line 480 (not_supported).
 *
 * @pre FAT16 volume mounted; /SUB exists.
 * @post Result is k_ra8_err_not_supported.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_rename_root_vs_subdir(void)
{
  TEST_BEGIN("rename: root vs subdir -> not_supported (line 480)");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SUB"));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_fs_rename(h, "/F.TXT", "/SUB/F.TXT"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("rename: root vs subdir -> not_supported (line 480)");
}

/**
 * @test test_rename_cross_subdir
 * @brief rename across two different subdirs returns not_supported (line 487).
 *
 * @details Creates /A and /B (different clusters). rename("/A/F.TXT", "/B/F.TXT"):
 *          both parents are non-root (is_root=0) but their cluster numbers differ.
 *          Line 485-487 fires.
 *
 * @par MC/DC:
 * Decision: `if (op.cluster != np.cluster)` (line 486, 1 condition).
 * V1: same cluster -> false (same-directory rename, not_supported NOT returned).
 * V2: different clusters -> true -> line 487 (not_supported).
 *
 * @pre FAT16 volume mounted; /A and /B both exist.
 * @post Result is k_ra8_err_not_supported.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_rename_cross_subdir(void)
{
  TEST_BEGIN("rename: different subdirs -> not_supported (line 487)");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/A"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/B"));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_fs_rename(h, "/A/F.TXT", "/B/F.TXT"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("rename: different subdirs -> not_supported (line 487)");
}

/**
 * @test test_rename_old_bad_83
 * @brief rename of an absent long name reports not_found, not invalid_arg.
 *
 * @details `priv_rename_prepare()` no longer packs either leaf: since #600 both
 *          sides may be long names, so whether one fits 8.3 is not the rename's
 *          question. `"bad name!"` is a name a volume could hold; this one does
 *          not, so the lookup misses on both passes.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` after the old-name lookup in
 * `priv_fat_rename()` (1 condition).
 * V1: the old name resolved -> false (test_rename_success).
 * V2: it did not            -> true  -> k_ra8_err_not_found.
 *
 * @pre FAT16 volume is mounted.
 * @post Result is k_ra8_err_not_found and the volume is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_rename_old_bad_83(void)
{
  TEST_BEGIN("rename: absent long old leaf -> not_found");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_rename(h, "/bad name!", "/NEW.TXT"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("rename: absent long old leaf -> not_found");
}

/**
 * @test test_rename_new_bad_83
 * @brief rename to a name no encoding can hold returns k_ra8_err_invalid_arg.
 *
 * @details The new leaf `"bad?name"` carries a `?`, illegal in a long name as
 *          well as an 8.3 one, so `priv_dir_reserve()` refuses it. The refusal
 *          happens before anything is written -- proved by the file still being
 *          openable under its original name afterwards.
 *
 * @par MC/DC:
 * Decision: `if (kind == k_name_kind_invalid)` in `priv_dir_reserve()`
 * (1 condition).
 * V1: a storable new name -> false (test_rename_success).
 * V2: `"bad?name"`        -> true  -> k_ra8_err_invalid_arg.
 *
 * @pre FAT16 volume is mounted and holds /OLD.TXT.
 * @post Result is k_ra8_err_invalid_arg and /OLD.TXT still resolves.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_rename_new_bad_83(void)
{
  TEST_BEGIN("rename: unstorable new leaf -> invalid_arg");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "/OLD.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_rename(h, "/OLD.TXT", "/bad?name"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "/OLD.TXT", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("rename: unstorable new leaf -> invalid_arg");
}

/* ===========================================================================
 * Tests: priv_fat_rename error paths
 * ===========================================================================
 */

/**
 * @test test_rename_exists
 * @brief rename where new_path already exists returns k_ra8_err_exists (line 545).
 *
 * @details Creates A.TXT and B.TXT. rename("/A.TXT", "/B.TXT"): priv_dir_find
 *          for the new name (B.TXT) returns k_ra8_ok (found) -> line 545.
 *
 * @par MC/DC:
 * Decision: `if (priv_dir_find(..., new83) == k_ra8_ok)` (line 544, 1 condition).
 * V1: new name not found -> false (continues to find old name).
 * V2: new name found -> true -> line 545.
 *
 * @pre FAT16 volume mounted; A.TXT and B.TXT both exist.
 * @post Result is k_ra8_err_exists.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_rename_exists(void)
{
  TEST_BEGIN("rename: new name already exists -> exists (line 545)");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "/A.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "/B.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_err_exists, ra8_fs_rename(h, "/A.TXT", "/B.TXT"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("rename: new name already exists -> exists (line 545)");
}

/**
 * @test test_rename_old_not_found
 * @brief rename where old_path does not exist returns k_ra8_err_not_found (line 552).
 *
 * @details On a fresh volume, neither NOEXIST.TXT nor NEW.TXT exist.
 *          priv_dir_find(new83): not found (no k_ra8_err_exists branch). Then
 *          priv_dir_find(old83 "NOEXIST.TXT"): not found -> line 551-552.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at line 551 (1 condition).
 * V1: err=k_ra8_ok -> false (old name found, reads sector for rewrite).
 * V2: err!=k_ra8_ok -> true -> line 552.
 *
 * @pre FAT16 volume is freshly mounted (empty); no files exist.
 * @post Result is k_ra8_err_not_found.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_rename_old_not_found(void)
{
  TEST_BEGIN("rename: old name not found -> not_found (line 552)");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_rename(h, "/NOEXIST.TXT", "/NEW.TXT"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("rename: old name not found -> not_found (line 552)");
}

/**
 * @test test_rename_read_fail
 * @brief rename fails at sector re-read after finding old name (line 557).
 *
 * @details Creates F.TXT. Swaps to inject backend with reads_left=2.
 *          Read 1: priv_dir_find(new83 "G.TXT") -> not found (G doesn't exist).
 *          Read 2: priv_dir_find(old83 "F.TXT") -> found (F exists).
 *          Read 3: priv_read_sector(lba, sec) at line 555 -> fails -> line 557.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at line 556 (1 condition).
 * V1: err=k_ra8_ok -> false (sector read ok, copies new name and writes).
 * V2: err!=k_ra8_ok -> true -> line 557.
 *
 * @pre FAT16 volume mounted; F.TXT exists; G.TXT does not; inject reads_left=2.
 * @post Result is k_ra8_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_rename_read_fail(void)
{
  TEST_BEGIN("rename: sector re-read fails after finding old name -> err (line 557)");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "/F.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  ra8_fs_backend_t saved = h->backend;
  internal_swap_to_inject(h, 2U, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_rename(h, "/F.TXT", "/G.TXT"));
  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("rename: sector re-read fails after finding old name -> err (line 557)");
}
/* ===========================================================================
 * Entry point
 * ===========================================================================
 */

/**
 * @brief Test executable entry point.
 *
 * @details Runs the unlink + rename coverage tests in sequence. Each test is
 *          self-contained: it builds the volume, mounts, exercises the target
 *          branches, unmounts, and frees the disk. Failure exits via
 *          TEST_FAIL_FMT (exit(1)).
 *
 * @return 0 on success (all tests passed).
 *
 * @pre Host environment provides calloc/free and stderr.
 * @post The targeted unlink/rename branches in ra8_fs_fat_dir.c are exercised.
 *
 * @note Not thread-safe (single-threaded test runner).
 * @since 0.1.0
 */
int main(void)
{
  internal_test_unlink_not_mounted();
  internal_test_unlink_bad_parent();
  internal_test_unlink_bad_83();
  internal_test_unlink_free_chain_fail();
  internal_test_unlink_dir_read_fail();

  internal_test_rename_null_guards();
  internal_test_rename_not_mounted();
  internal_test_rename_new_parent_fail();
  internal_test_rename_root_vs_subdir();
  internal_test_rename_cross_subdir();
  internal_test_rename_old_bad_83();
  internal_test_rename_new_bad_83();
  internal_test_rename_exists();
  internal_test_rename_old_not_found();
  internal_test_rename_read_fail();

  return 0;
}
