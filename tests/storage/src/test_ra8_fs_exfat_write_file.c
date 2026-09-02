/**
 * @file test_ra8_fs_exfat_write_file.c
 * @brief exFAT `ra8_fs_write_file()` replace semantics (#603) + directory guards (#604).
 *
 * @details
 * `priv_exfat_create()` never looked for the name it was about to create, so a
 * second `ra8_fs_write_file()` with the same path wrote a SECOND File/Stream/Name
 * entry set: the volume ended up with two entries for one name, the first file's
 * clusters stayed marked used in the allocation bitmap with nothing referencing
 * them, and which bytes a reader got back depended on directory layout. These
 * tests pin the fixed behaviour -- one entry, the newest contents, and not one
 * bitmap bit more than the new contents need -- across every size relationship
 * between the old and new file.
 *
 * The allocation bitmap is measured directly (`alloc_bitmap_used()`), because it
 * is the only authority on exFAT allocation: a leak is invisible in a directory
 * listing by definition, since the leaked clusters are exactly the ones no
 * directory entry points at any more.
 *
 * The exFAT half of the directory guards lives here too, sharing this fixture.
 * The library has no exFAT `mkdir`, so a directory is presented to the driver by
 * flipping the FileAttributes bit on an entry set it wrote itself
 * (`mark_first_file_as_directory()`); nothing in the exFAT lookup path verifies
 * the set checksum, so the patched set resolves exactly like a real one.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fs_fat_exfat_mutate_test_util.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "unity_minimal.h"

/**
 * @enum ovw_const_t
 * @brief Payload sizes, seeds and expected counts for the replace tests.
 *
 * @details The three payload sizes are chosen so the replacement is smaller,
 *          equal, and larger than what it replaces -- the three cases where a
 *          naive "reuse the old run" implementation would either leak the tail
 *          or overrun. ::k_ovw_large exceeds one cluster on every geometry the
 *          formatter picks for a 64 MiB volume, so the larger case really does
 *          change the cluster count and not just the recorded length.
 *
 * @invariant k_ovw_small < k_ovw_large.
 * @see test_exfat_overwrite_larger()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ovw_small       = 200U,  /**< Comfortably inside one cluster.          */
  k_ovw_large       = 5000U, /**< Spans more than one cluster.             */
  k_ovw_seed_a      = 0x11U, /**< Seed of the first payload.               */
  k_ovw_seed_b      = 0x77U, /**< Seed of the replacement payload.         */
  k_ovw_seed_stride = 5U,    /**< Stride of the fill generator.            */
  k_ovw_repeats     = 5U,    /**< Repeated creates in the no-leak sweep.   */
  k_ovw_one_entry   = 1U,    /**< Entries a replaced name must resolve to. */
  k_ovw_no_entries  = 0U,    /**< Entries after the file is unlinked.      */
} ovw_const_t;

/**
 * @brief Fill @p buf with a deterministic, seed-dependent pattern.
 *
 * @details `buf[i] = i * k_ovw_seed_stride + seed`. Two different seeds produce
 *          two different byte streams of the same length, so a read that returns
 *          the PREVIOUS contents of a replaced file fails the compare instead of
 *          passing by luck.
 *
 * @param[out] buf  Destination buffer of at least @p len bytes.
 * @param[in]  len  Number of bytes to write.
 * @param[in]  seed Generator seed.
 *
 * @pre @p buf is non-NULL and addresses @p len writable bytes.
 * @pre @p len is the exact buffer length.
 * @post Every byte of @p buf[0..len-1] is written.
 * @post No other state is modified.
 *
 * @note Trivially thread-safe (writes only through @p buf).
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_fill(uint8_t* buf, uint32_t len, uint8_t seed)
{
  for (uint32_t i = 0U; i < len; i++) {
    buf[i] = (uint8_t)((i * (uint32_t)k_ovw_seed_stride) + seed);
  }
}

/**
 * @brief Clusters a file of @p len bytes occupies on the mounted volume.
 *
 * @param[in] h   Mounted exFAT volume.
 * @param[in] len File length in bytes.
 *
 * @return Cluster count, rounded up.
 * @retval 1..UINT32_MAX Clusters needed to hold @p len bytes.
 *
 * @pre @p h is non-NULL and mounted; @p len > 0.
 * @pre @p h->sectors_per_cluster is non-zero.
 * @post No state is modified.
 * @post Result depends only on the inputs.
 *
 * @note Pure function.
 * @since 0.1.0 @details Implements the bounded clusters for fixture step using caller-owned state.
 */
RA8_INTERNAL static uint32_t internal_clusters_for(const ra8_fs_mount_t* h, uint32_t len)
{
  const uint32_t cbytes = h->sectors_per_cluster * (uint32_t)k_mut_block_size;
  return (len + cbytes - 1U) / cbytes;
}

/**
 * @brief Count the entries `ra8_fs_listdir()` reports for the exFAT root.
 *
 * @param[in] h Mounted exFAT volume.
 *
 * @return Number of entries reported.
 * @retval 0..UINT32_MAX The entry count.
 *
 * @pre @p h is non-NULL and mounted.
 * @pre The listing succeeds (asserted).
 * @post No on-disk state is modified.
 * @post The returned count reflects only in-use File entries.
 *
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0 @details Implements the bounded root entry count fixture step using caller-owned state.
 */
RA8_INTERNAL static uint32_t internal_root_entry_count(ra8_fs_mount_t* h)
{
  mut_list_ctx_t ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", internal_count_cb, &ctx));
  return ctx.count;
}

/**
 * @brief Read @p len bytes of @p path back and compare them against @p want.
 *
 * @param[in] h    Mounted exFAT volume.
 * @param[in] path File to read.
 * @param[in] want Expected contents.
 * @param[in] len  Expected length.
 *
 * @pre @p h, @p path, @p want are non-NULL; @p len > 0.
 * @pre @p path exists on the volume.
 * @post The file handle opened here is closed again.
 * @post A mismatch in length or contents fails the test.
 *
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0 @details Implements the bounded expect contents fixture step using caller-owned state.
 */
RA8_INTERNAL static void
internal_expect_contents(ra8_fs_mount_t* h, const char* path, const uint8_t* want, uint32_t len)
{
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, path, k_ra8_fs_mode_read, &f));
  uint64_t size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(f, &size));
  TEST_ASSERT_EQ(len, size);
  uint8_t* got = (uint8_t*)malloc(len);
  if (got == nullptr) {
    TEST_FAIL_FMT("%s", "malloc failed for read-back buffer");
  }
  uint32_t got_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, got, len, &got_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(len, got_len);
  TEST_ASSERT_EQ(0, memcmp(want, got, len));
  free(got);
}

/* ---- #603: create / replace --------------------------------------------- */

/**
 * @test test_exfat_create_new
 * @brief A first `write_file()` creates one entry and claims exactly its clusters.
 *
 * @details The baseline case the replace path must not disturb: the name does
 *          not exist, `priv_exfat_unlink()` reports not-found, and the create
 *          proceeds unchanged.
 *
 * @par MC/DC:
 * Decision: `if (ue != k_ra8_ok && ue != k_ra8_err_not_found)` in
 * `priv_exfat_create()` -- 2 conditions.
 * - V1: ue = k_ra8_ok           -> C1 F (C2 masked) -> F: proceed (replace case,
 *       test_exfat_overwrite_same_size).
 * - V2: ue = k_ra8_err_not_found -> C1 T, C2 F      -> F: proceed (THIS test).
 * - V3: ue = k_ra8_err_invalid_arg -> C1 T, C2 T    -> T: propagate
 *       (test_exfat_write_file_over_directory_refused).
 * V1+V3 vary C1 alone and flip the outcome; V2+V3 vary C2 alone and flip it.
 * N+1 = 3 vectors for N=2: minimal MC/DC.
 *
 * @pre A freshly formatted exFAT volume is mounted.
 * @post The root lists exactly one entry and the bitmap grew by one cluster.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_create_new(void)
{
  TEST_BEGIN("exfat write_file: first create claims exactly its clusters");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t base = internal_alloc_bitmap_used(h);

  uint8_t data[k_ovw_small] = {};
  internal_fill(data, (uint32_t)k_ovw_small, (uint8_t)k_ovw_seed_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", data, (uint32_t)k_ovw_small));

  TEST_ASSERT_EQ(k_ovw_one_entry, internal_root_entry_count(h));
  internal_expect_contents(h, "A.TXT", data, (uint32_t)k_ovw_small);
  TEST_ASSERT_EQ(base + internal_clusters_for(h, (uint32_t)k_ovw_small),
                 internal_alloc_bitmap_used(h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat write_file: first create claims exactly its clusters");
}

/**
 * @test test_exfat_overwrite_same_size
 * @brief Rewriting a name at the same length replaces it rather than duplicating it.
 *
 * @details This is the exact call sequence that used to corrupt the volume:
 *          provision a file, provision it again (a re-run, a retry after a power
 *          cut, an idempotent install step). Before the fix the root held two
 *          entry sets for one name and the first file's cluster stayed allocated
 *          forever. The bitmap census is what proves the second half.
 *
 * @par MC/DC:
 * Decision: `if (ue != k_ra8_ok && ue != k_ra8_err_not_found)` -- 2 conditions.
 * - V1: ue = k_ra8_ok -> C1 F (C2 masked) -> F: proceed with the replace (THIS
 *   test; see test_exfat_create_new() for the full vector set).
 *
 * @pre A freshly formatted exFAT volume is mounted.
 * @post One entry, the second payload, and no growth in allocated clusters.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_overwrite_same_size(void)
{
  TEST_BEGIN("exfat write_file: same-size rewrite replaces, does not duplicate");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  uint8_t first[k_ovw_small] = {};
  internal_fill(first, (uint32_t)k_ovw_small, (uint8_t)k_ovw_seed_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", first, (uint32_t)k_ovw_small));
  const uint32_t after_first = internal_alloc_bitmap_used(h);

  uint8_t second[k_ovw_small] = {};
  internal_fill(second, (uint32_t)k_ovw_small, (uint8_t)k_ovw_seed_b);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", second, (uint32_t)k_ovw_small));

  TEST_ASSERT_EQ(k_ovw_one_entry, internal_root_entry_count(h));
  internal_expect_contents(h, "A.TXT", second, (uint32_t)k_ovw_small);
  TEST_ASSERT_EQ(after_first, internal_alloc_bitmap_used(h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat write_file: same-size rewrite replaces, does not duplicate");
}

/**
 * @test test_exfat_overwrite_larger
 * @brief A replacement that needs more clusters than its predecessor.
 *
 * @details The old run is released before the new one is scanned for, so the
 *          multi-cluster replacement is free to reuse the space the small file
 *          occupied. The assertion is on the absolute count, not on growth, so
 *          a leak of the old single cluster would fail here.
 *
 * @par MC/DC:
 * (no compound decision under test -- the replace branch is the same single
 * `ue == k_ra8_ok` vector documented in test_exfat_create_new(); this test
 * varies the SIZE relationship, not the decision)
 *
 * @pre A freshly formatted exFAT volume is mounted.
 * @post One entry of the larger length, holding the larger payload.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_overwrite_larger(void)
{
  TEST_BEGIN("exfat write_file: replacement larger than the file it replaces");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t base = internal_alloc_bitmap_used(h);

  uint8_t small[k_ovw_small] = {};
  internal_fill(small, (uint32_t)k_ovw_small, (uint8_t)k_ovw_seed_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", small, (uint32_t)k_ovw_small));

  uint8_t large[k_ovw_large] = {};
  internal_fill(large, (uint32_t)k_ovw_large, (uint8_t)k_ovw_seed_b);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", large, (uint32_t)k_ovw_large));

  TEST_ASSERT_EQ(k_ovw_one_entry, internal_root_entry_count(h));
  internal_expect_contents(h, "A.TXT", large, (uint32_t)k_ovw_large);
  TEST_ASSERT_EQ(base + internal_clusters_for(h, (uint32_t)k_ovw_large),
                 internal_alloc_bitmap_used(h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat write_file: replacement larger than the file it replaces");
}

/**
 * @test test_exfat_overwrite_smaller
 * @brief A replacement that needs fewer clusters returns the surplus.
 *
 * @details The direction a "reuse the existing run" shortcut gets wrong: the
 *          tail clusters of the old multi-cluster file must come back to the
 *          bitmap, not stay marked used behind a shorter DataLength.
 *
 * @par MC/DC:
 * (no compound decision under test -- see test_exfat_create_new() for the
 * decision's vector set; this test varies the SIZE relationship)
 *
 * @pre A freshly formatted exFAT volume is mounted.
 * @post Allocated clusters equal what the smaller file alone requires.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_overwrite_smaller(void)
{
  TEST_BEGIN("exfat write_file: replacement smaller than the file it replaces");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t base = internal_alloc_bitmap_used(h);

  uint8_t large[k_ovw_large] = {};
  internal_fill(large, (uint32_t)k_ovw_large, (uint8_t)k_ovw_seed_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", large, (uint32_t)k_ovw_large));

  uint8_t small[k_ovw_small] = {};
  internal_fill(small, (uint32_t)k_ovw_small, (uint8_t)k_ovw_seed_b);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", small, (uint32_t)k_ovw_small));

  TEST_ASSERT_EQ(k_ovw_one_entry, internal_root_entry_count(h));
  internal_expect_contents(h, "A.TXT", small, (uint32_t)k_ovw_small);
  TEST_ASSERT_EQ(base + internal_clusters_for(h, (uint32_t)k_ovw_small),
                 internal_alloc_bitmap_used(h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat write_file: replacement smaller than the file it replaces");
}

/**
 * @test test_exfat_repeat_create_no_leak
 * @brief Repeated creates of one name never grow the allocated-cluster count.
 *
 * @details The no-leak assertion the issue asks for, taken over several rounds
 *          rather than one: before the fix each round added a cluster and an
 *          entry set that nothing could reach, so the bitmap climbed
 *          monotonically. Unlinking at the end must return the volume to its
 *          post-format census exactly.
 *
 * @par MC/DC:
 * (no compound decision under test -- this is a repetition sweep over the
 * replace branch documented in test_exfat_create_new())
 *
 * @pre A freshly formatted exFAT volume is mounted.
 * @post Allocated clusters after unlink equal the post-format baseline.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_repeat_create_no_leak(void)
{
  TEST_BEGIN("exfat write_file: repeated creates leak neither clusters nor entries");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t base = internal_alloc_bitmap_used(h);

  for (uint32_t round = 0U; round < (uint32_t)k_ovw_repeats; round++) {
    uint8_t data[k_ovw_small] = {};
    internal_fill(data, (uint32_t)k_ovw_small, (uint8_t)((uint32_t)k_ovw_seed_a + round));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", data, (uint32_t)k_ovw_small));
    TEST_ASSERT_EQ(k_ovw_one_entry, internal_root_entry_count(h));
    internal_expect_contents(h, "A.TXT", data, (uint32_t)k_ovw_small);
    TEST_ASSERT_EQ(base + internal_clusters_for(h, (uint32_t)k_ovw_small),
                   internal_alloc_bitmap_used(h));
  }

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "A.TXT"));
  TEST_ASSERT_EQ(k_ovw_no_entries, internal_root_entry_count(h));
  TEST_ASSERT_EQ(base, internal_alloc_bitmap_used(h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat write_file: repeated creates leak neither clusters nor entries");
}

/**
 * @test test_mcdc_exfat_create_unlink_verdict
 * @brief MC/DC vectors for the replace-or-propagate decision in the create path.
 *
 * @details Drives all three outcomes of the guard that turned a duplicating
 *          create into a replacing one, in one function and against one volume,
 *          so the three vectors differ only in what the preceding unlink found.
 *
 * @par MC/DC:
 * Decision: `if ((ue != k_ra8_ok) && (ue != k_ra8_err_not_found))` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_openw.c@priv_exfat_open_write` (2 conditions),
 * where `ue` is what `priv_exfat_unlink()` reported for the name about to be
 * created.
 * - V1 (control, C1=F, C2 masked -> F): the name exists as a file, unlink
 *   succeeds -> `ue == k_ra8_ok` -> the create proceeds and REPLACES it.
 * - V2 (C1=T, C2=F -> F): the name does not exist -> `ue ==
 *   k_ra8_err_not_found` -> the create proceeds and creates it.
 * - V3 (C1=T, C2=T -> T): the name exists as a DIRECTORY, so unlink refuses ->
 *   `ue == k_ra8_err_invalid_arg` -> the decision is true and the error is
 *   propagated instead of the directory being overwritten.
 * V1 and V3 differ only in C1 and flip the outcome (C2 is masked in V1); V2 and
 * V3 hold C1 true and differ only in C2, and flip the outcome. N+1 = 3 vectors
 * for N=2 conditions: minimal MC/DC.
 *
 * @pre A freshly formatted exFAT volume is mounted.
 * @post Each vector produced its documented return code.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_mcdc_exfat_create_unlink_verdict(void)
{
  TEST_BEGIN("exfat create: MC/DC over the replace-or-propagate verdict");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* V2: nothing to unlink -- unlink reports not_found and the create runs. */
  uint8_t first[k_ovw_small] = {};
  internal_fill(first, (uint32_t)k_ovw_small, (uint8_t)k_ovw_seed_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", first, (uint32_t)k_ovw_small));
  TEST_ASSERT_EQ(k_ovw_one_entry, internal_root_entry_count(h));

  /* V1: the name is now a file -- unlink succeeds and the create replaces it. */
  uint8_t second[k_ovw_small] = {};
  internal_fill(second, (uint32_t)k_ovw_small, (uint8_t)k_ovw_seed_b);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", second, (uint32_t)k_ovw_small));
  TEST_ASSERT_EQ(k_ovw_one_entry, internal_root_entry_count(h));
  internal_expect_contents(h, "A.TXT", second, (uint32_t)k_ovw_small);

  /* V3: the same name now reads as a directory -- unlink refuses, and the
   * create must propagate that refusal rather than swallow it. */
  internal_mark_first_file_as_directory(h);
  const uint32_t before = internal_alloc_bitmap_used(h);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_fs_write_file(h, "A.TXT", first, (uint32_t)k_ovw_small));
  TEST_ASSERT_EQ(before, internal_alloc_bitmap_used(h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat create: MC/DC over the replace-or-propagate verdict");
}

/* ---- #604: exFAT directory guards --------------------------------------- */

/**
 * @test test_exfat_unlink_directory_refused
 * @brief `ra8_fs_unlink()` refuses an exFAT entry set flagged as a directory.
 *
 * @details A directory's entry set is indistinguishable from a file's to the
 *          name matcher, so without the attribute check the chain holding every
 *          child would be handed to the bitmap-clearing path. The census after
 *          the refusal proves nothing was freed.
 *
 * @par MC/DC:
 * Decision: `if ((file_e[k_exfat_off_file_attr] & k_exfat_attr_directory) != 0U)`
 * in `priv_exfat_unlink()` -- 1 condition.
 * - V1: attr = 0x10 -> T -> k_ra8_err_invalid_arg (THIS test).
 * - V2: attr = 0x20 -> F -> the unlink proceeds (test_exfat_repeat_create_no_leak).
 *
 * @pre A file exists and its entry has been flagged as a directory.
 * @post The call returns k_ra8_err_invalid_arg and the bitmap is unchanged.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_unlink_directory_refused(void)
{
  TEST_BEGIN("exfat unlink: a directory is refused, not freed");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  uint8_t data[k_ovw_small] = {};
  internal_fill(data, (uint32_t)k_ovw_small, (uint8_t)k_ovw_seed_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "LOGS", data, (uint32_t)k_ovw_small));
  internal_mark_first_file_as_directory(h);
  const uint32_t before = internal_alloc_bitmap_used(h);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_unlink(h, "LOGS"));
  TEST_ASSERT_EQ(before, internal_alloc_bitmap_used(h));
  TEST_ASSERT_EQ(k_ovw_one_entry, internal_root_entry_count(h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat unlink: a directory is refused, not freed");
}

/**
 * @test test_exfat_open_directory_refused
 * @brief `ra8_fs_open()` in read mode refuses an exFAT directory.
 *
 * @details A directory reports DataLength 0, so an unguarded open handed back a
 *          valid, zero-byte file handle -- which is what made a directory look
 *          like an existing empty file to everything layered above.
 *
 * @par MC/DC:
 * Decision: `if ((attr & k_exfat_attr_directory) != 0U)` in `priv_exfat_open()`
 * -- 1 condition.
 * - V1: attr = 0x10 -> T -> k_ra8_err_invalid_arg (THIS test).
 * - V2: attr = 0x20 -> F -> the handle is populated (every read-back above).
 *
 * @pre A file exists and its entry has been flagged as a directory.
 * @post The open returns k_ra8_err_invalid_arg and yields no handle.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_open_directory_refused(void)
{
  TEST_BEGIN("exfat open: a directory is refused, not opened as an empty file");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  uint8_t data[k_ovw_small] = {};
  internal_fill(data, (uint32_t)k_ovw_small, (uint8_t)k_ovw_seed_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "LOGS", data, (uint32_t)k_ovw_small));
  internal_mark_first_file_as_directory(h);

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_open(h, "LOGS", k_ra8_fs_mode_read, &f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat open: a directory is refused, not opened as an empty file");
}

/**
 * @test test_exfat_write_file_over_directory_refused
 * @brief `ra8_fs_write_file()` over a directory name reports the refusal.
 *
 * @details The replace path delegates its "does this already exist" question to
 *          `priv_exfat_unlink()`, so the directory guard reaches the create path
 *          for free -- but only if the refusal is PROPAGATED rather than
 *          swallowed alongside the not-found case. This is the third MC/DC
 *          vector of that decision.
 *
 * @par MC/DC:
 * Decision: `if (ue != k_ra8_ok && ue != k_ra8_err_not_found)` -- 2 conditions.
 * - V3: ue = k_ra8_err_invalid_arg -> C1 T, C2 T -> T -> propagate (THIS test).
 * See test_exfat_create_new() for V1 and V2 and the independence argument.
 *
 * @pre A file exists and its entry has been flagged as a directory.
 * @post The call returns k_ra8_err_invalid_arg and the volume is unchanged.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_write_file_over_directory_refused(void)
{
  TEST_BEGIN("exfat write_file: writing over a directory name is refused");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  uint8_t data[k_ovw_small] = {};
  internal_fill(data, (uint32_t)k_ovw_small, (uint8_t)k_ovw_seed_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "LOGS", data, (uint32_t)k_ovw_small));
  internal_mark_first_file_as_directory(h);
  const uint32_t before = internal_alloc_bitmap_used(h);

  uint8_t other[k_ovw_small] = {};
  internal_fill(other, (uint32_t)k_ovw_small, (uint8_t)k_ovw_seed_b);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_write_file(h, "LOGS", other, (uint32_t)k_ovw_small));
  TEST_ASSERT_EQ(before, internal_alloc_bitmap_used(h));
  TEST_ASSERT_EQ(k_ovw_one_entry, internal_root_entry_count(h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat write_file: writing over a directory name is refused");
}

/**
 * @test test_exfat_rmdir_dispatches
 * @brief `ra8_fs_rmdir()` reaches the exFAT remover, guards running first.
 *
 * @details This used to report `k_ra8_err_not_supported`: with no exFAT
 *          directory-CREATION path, removal had no reachable subject and
 *          declining was the only honest answer. Both halves landed together
 *          (#605), so the dispatch now reaches `priv_exfat_rmdir` and a name
 *          that is not there reports not_found. Argument validation still runs
 *          first, so a NULL path is a null_ptr rather than a lookup.
 *
 * @par MC/DC:
 * Decision: `if (handle->type == k_ra8_fs_type_exfat)`
 * (libs/ra8_fs/src/ra8_fs_fat_dirmk.c@internal_rmdir_locked, 1 condition).
 * - V1: type = exFAT -> T -> priv_exfat_rmdir (THIS test).
 * - V2: type = FAT16 -> F -> priv_fat_rmdir (test_ra8_fs_rmdir.c).
 *
 * @pre A freshly formatted exFAT volume is mounted.
 * @post A NULL path reports null_ptr; a missing name reports not_found.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_rmdir_dispatches(void)
{
  TEST_BEGIN("exfat rmdir: dispatches to the exFAT remover");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_rmdir(h, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_rmdir(h, "/LOGS"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat rmdir: dispatches to the exFAT remover");
}

/**
 * @brief Run every exFAT write_file / directory-guard test.
 *
 * @return Process exit status.
 * @retval 0 Every test passed (a failure aborts inside the assertion macros).
 *
 * @pre The host provides a working heap.
 * @pre No volume is mounted on entry.
 * @post Every test built and released its own volume.
 * @post A success banner is written to stderr.
 *
 * @since 0.1.0
 */
int main(void)
{
  internal_test_exfat_create_new();
  internal_test_exfat_overwrite_same_size();
  internal_test_exfat_overwrite_larger();
  internal_test_exfat_overwrite_smaller();
  internal_test_exfat_repeat_create_no_leak();
  internal_test_mcdc_exfat_create_unlink_verdict();
  internal_test_exfat_unlink_directory_refused();
  internal_test_exfat_open_directory_refused();
  internal_test_exfat_write_file_over_directory_refused();
  internal_test_exfat_rmdir_dispatches();
  return 0;
}
