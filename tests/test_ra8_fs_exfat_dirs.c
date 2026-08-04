/**
 * @file test_ra8_fs_exfat_dirs.c
 * @brief exFAT directories end to end: create, enter, list, remove (#605).
 *
 * @details
 * exFAT used to be a flat, root-only namespace in `ra8_fs`. `ra8_fs_mkdir()`
 * declined an exFAT mount before doing anything else, `ra8_fs_listdir()`
 * declined any path but `"/"`, and every lookup started at the root cluster --
 * so `"/logs/a.txt"` was not resolved, it was MATCHED, after slash-stripping,
 * as a single flat name. The cards that ship exFAT from the factory are exactly
 * the big ones, so "the volumes with room for thousands of files are the ones
 * where everything must live in one directory" was the state of the platform.
 *
 * These tests pin the behaviour that replaced it: a directory is created,
 * entered, listed and removed; a nested path resolves through several levels; a
 * name collides with a name rather than with a kind; and every scenario ends
 * with a full structural scan of the volume (`exfat_verify()`), which is the
 * pair of questions `fsck.exfat` asks -- does every entry set check out, and do
 * the referenced clusters and the allocation bitmap agree exactly.
 *
 * The error arms, the MC/DC vectors and the scanner's own negative control live
 * in `test_ra8_fs_exfat_dirs_cov.c`, sharing this fixture.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "support/fs_exfat_dir_test_util.h"
#include "unity_minimal.h"

/**
 * @enum dirs_const_t
 * @brief Payload sizes, seeds and expected counts for the directory suite.
 *
 * @details ::k_dirs_payload is comfortably inside one cluster on every geometry
 *          the formatter picks for a 64 MiB volume, so a file inside a
 *          subdirectory costs exactly one cluster and the bitmap census is easy
 *          to reason about.
 *
 * @invariant k_dirs_seed_a != k_dirs_seed_b, so a stale read fails the compare.
 * @see test_mkdir_file_inside_round_trip()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_dirs_payload    = 300U,  /**< Bytes per file.   */
  k_dirs_seed_a     = 0x31U, /**< First seed.       */
  k_dirs_seed_b     = 0x8DU, /**< Second seed.      */
  k_dirs_stride     = 7U,    /**< Fill stride.      */
  k_dirs_one        = 1U,    /**< One entry.        */
  k_dirs_two        = 2U,    /**< Two entries.      */
  k_dirs_none       = 0U,    /**< No entries.       */
  k_dirs_dir_size   = 0U,    /**< Listed dir size.  */
  k_dirs_long_chars = 40U,   /**< Long-name length. */
} dirs_const_t;

/**
 * @brief Fill @p buf with a deterministic, seed-dependent pattern.
 *
 * @details `buf[i] = i * k_dirs_stride + seed`, so two seeds give two byte
 *          streams of the same length and a read that returns the WRONG file's
 *          contents fails the compare instead of passing by luck.
 *
 * @param[out] buf  Destination buffer of at least @p len bytes.
 * @param[in]  len  Number of bytes to write.
 * @param[in]  seed Generator seed.
 *
 * @pre @p buf is non-NULL and addresses @p len writable bytes.
 * @pre @p len is the exact buffer length.
 * @post Every byte of `buf[0..len-1]` is written.
 * @post No other state is modified.
 *
 * @note Trivially thread-safe (writes only through @p buf).
 * @since 0.1.0
 */
static void fill(uint8_t* buf, uint32_t len, uint8_t seed)
{
  for (uint32_t i = 0U; i < len; i++) {
    buf[i] = (uint8_t)((i * (uint32_t)k_dirs_stride) + seed);
  }
}

/**
 * @brief Mount the fixture volume, asserting success.
 *
 * @return The mounted volume.
 * @retval non-NULL The mount handle.
 *
 * @pre `build_exfat_volume()` has run.
 * @pre No other mount is outstanding.
 * @post The returned handle is an exFAT mount.
 * @post No on-disk state is modified.
 *
 * @since 0.1.0
 */
static ra8_fs_mount_t* mount_fixture(void)
{
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_fs_type_exfat, h->type);
  return h;
}

/**
 * @brief Write @p len seeded bytes to @p path, asserting success.
 *
 * @param[in] h    Mounted exFAT volume.
 * @param[in] path File path.
 * @param[in] len  Byte count.
 * @param[in] seed Generator seed.
 *
 * @pre @p h and @p path are non-NULL; @p len > 0.
 * @pre The parent directory of @p path exists.
 * @post @p path resolves to a file of @p len bytes.
 * @post The write succeeded (asserted).
 *
 * @since 0.1.0
 */
static void write_seeded(ra8_fs_mount_t* h, const char* path, uint32_t len, uint8_t seed)
{
  uint8_t* buf = (uint8_t*)malloc((size_t)len);
  if (buf == nullptr) {
    TEST_FAIL_FMT("%s", "malloc failed for the payload");
    return;
  }
  fill(buf, len, seed);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, path, buf, len));
  free(buf);
}

/**
 * @brief Read @p path back and assert it holds @p len seeded bytes.
 *
 * @param[in] h    Mounted exFAT volume.
 * @param[in] path File path.
 * @param[in] len  Expected byte count.
 * @param[in] seed Expected generator seed.
 *
 * @pre @p h and @p path are non-NULL; @p len > 0.
 * @pre @p path resolves to a file.
 * @post The file's bytes equal the seeded pattern (asserted).
 * @post No on-disk state is modified.
 *
 * @since 0.1.0
 */
static void expect_seeded(ra8_fs_mount_t* h, const char* path, uint32_t len, uint8_t seed)
{
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, path, k_ra8_fs_mode_read, &f));
  uint32_t sz = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(f, &sz));
  TEST_ASSERT_EQ(len, sz);
  uint8_t* got  = (uint8_t*)malloc((size_t)len);
  uint8_t* want = (uint8_t*)malloc((size_t)len);
  if ((got == nullptr) || (want == nullptr)) {
    TEST_FAIL_FMT("%s", "malloc failed for the read-back buffers");
    return;
  }
  uint32_t n = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, got, len, &n));
  TEST_ASSERT_EQ(len, n);
  fill(want, len, seed);
  TEST_ASSERT_EQ(0, memcmp(got, want, (size_t)len));
  free(got);
  free(want);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
}

/* ---- create / list / remove --------------------------------------------- */

/**
 * @test test_mkdir_lists_as_a_directory
 * @brief `ra8_fs_mkdir()` on exFAT creates something the listing calls a folder.
 *
 * @details Before #605 this call returned `k_ra8_err_not_supported` before
 *          touching the volume. It now writes a File + Stream + Name set with
 *          bit 4 of FileAttributes set, over one zeroed cluster. The listing is
 *          asserted to report the directory bit and a size of zero -- a
 *          directory's exFAT DataLength is its ALLOCATION, and reporting that as
 *          a file size would make every caller think an empty folder held
 *          thousands of bytes.
 *
 * @par MC/DC:
 * No compound decision lies on this path; every one is single-condition. The
 * vectors it contributes: `handle->type == k_ra8_fs_type_exfat`
 * (libs/ra8_fs/src/ra8_fs_fat_dirmk.c@priv_mkdir_locked) -> true, and
 * `(attr & k_exfat_attr_directory) != 0`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_mutate.c@priv_exfat_listdir) -> true, the
 * arm that reports a directory's size as 0 rather than as its allocation.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post The root holds exactly one entry, flagged as a directory.
 * @post The volume passes the structural scan.
 *
 * @since 0.1.0
 */
static void test_mkdir_lists_as_a_directory(void)
{
  TEST_BEGIN("exfat dirs: mkdir creates a listable directory");
  build_exfat_volume();
  ra8_fs_mount_t* h = mount_fixture();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/LOGS"));

  name_ctx_t ctx = {};
  TEST_ASSERT_EQ(k_dirs_one, list_names(h, "/", &ctx));
  TEST_ASSERT_EQ(0, strcmp(ctx.names[0], "LOGS"));
  TEST_ASSERT_EQ(k_dirs_one, has_dir(&ctx, "LOGS"));
  TEST_ASSERT_EQ(k_dirs_dir_size, ctx.sizes[0]);

  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/LOGS", &st));
  TEST_ASSERT_EQ(true, st.is_directory);

  /* A brand new directory is empty, and empty means the listing reports
   * nothing -- exFAT has no "." / ".." entries to filter out. */
  name_ctx_t inner = {};
  TEST_ASSERT_EQ(k_dirs_none, list_names(h, "/LOGS", &inner));

  exfat_verify(h, "mkdir_lists_as_a_directory");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat dirs: mkdir creates a listable directory");
}

/**
 * @test test_mkdir_file_inside_round_trip
 * @brief A file created inside a directory is listed, opened and read there.
 *
 * @details The whole point of the feature in one scenario: the path is
 *          RESOLVED, so `"/LOGS/A.TXT"` lands inside `LOGS` instead of becoming
 *          a root-level entry literally named `LOGS/A.TXT` that no matcher could
 *          find again. The root's listing is asserted to still hold exactly one
 *          entry, which is what proves the file did not land there.
 *
 * @par MC/DC:
 * No compound decision lies on this path. The vector it contributes is
 * `*end == '\0'`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@priv_exfat_resolve_parent) taken
 * BOTH ways within one call: false for the "LOGS" component, which is
 * descended into, and true for the "A.TXT" leaf, which is handed back. A
 * root-level path only ever takes the true arm.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post The file resolves, lists and reads back inside the directory.
 * @post The volume passes the structural scan.
 *
 * @since 0.1.0
 */
static void test_mkdir_file_inside_round_trip(void)
{
  TEST_BEGIN("exfat dirs: file created inside a directory round-trips");
  build_exfat_volume();
  ra8_fs_mount_t* h = mount_fixture();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/LOGS"));
  write_seeded(h, "/LOGS/A.TXT", (uint32_t)k_dirs_payload, (uint8_t)k_dirs_seed_a);

  name_ctx_t root = {};
  TEST_ASSERT_EQ(k_dirs_one, list_names(h, "/", &root));
  TEST_ASSERT_EQ(0, strcmp(root.names[0], "LOGS"));

  name_ctx_t inner = {};
  TEST_ASSERT_EQ(k_dirs_one, list_names(h, "/LOGS", &inner));
  TEST_ASSERT_EQ(0, strcmp(inner.names[0], "A.TXT"));
  TEST_ASSERT_EQ(k_dirs_payload, inner.sizes[0]);

  expect_seeded(h, "/LOGS/A.TXT", (uint32_t)k_dirs_payload, (uint8_t)k_dirs_seed_a);

  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/LOGS/A.TXT", &st));
  TEST_ASSERT_EQ(false, st.is_directory);
  TEST_ASSERT_EQ(k_dirs_payload, st.size_bytes);

  exfat_verify(h, "mkdir_file_inside_round_trip");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat dirs: file created inside a directory round-trips");
}

/**
 * @test test_nested_three_levels
 * @brief A three-deep path resolves for mkdir, create, list, read and remove.
 *
 * @details Each level is created in turn, a file is written at the bottom, and
 *          every level's listing is asserted to hold exactly its own child. The
 *          removal then walks back up: the deepest directory only becomes
 *          removable once the file inside it is gone, which is the emptiness
 *          rule applied at depth.
 *
 * @par MC/DC:
 * No compound decision lies on this path. It contributes the deep-iteration
 * vectors of the component loop in
 * libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@priv_exfat_resolve_parent (three
 * descents in one call) and `empty == 0`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@priv_exfat_rmdir) taken both ways --
 * true while the level below still exists, false once it is gone.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post Every level resolves while it exists and stops resolving once removed.
 * @post The volume passes the structural scan at both ends.
 *
 * @since 0.1.0
 */
static void test_nested_three_levels(void)
{
  TEST_BEGIN("exfat dirs: three-level nesting resolves both ways");
  build_exfat_volume();
  ra8_fs_mount_t* h        = mount_fixture();
  const uint32_t  baseline = alloc_bitmap_used(h);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/A"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/A/B"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/A/B/C"));
  write_seeded(h, "/A/B/C/DEEP.TXT", (uint32_t)k_dirs_payload, (uint8_t)k_dirs_seed_b);

  name_ctx_t lvl = {};
  TEST_ASSERT_EQ(k_dirs_one, list_names(h, "/A", &lvl));
  TEST_ASSERT_EQ(k_dirs_one, has_dir(&lvl, "B"));
  TEST_ASSERT_EQ(k_dirs_one, list_names(h, "/A/B", &lvl));
  TEST_ASSERT_EQ(k_dirs_one, has_dir(&lvl, "C"));
  TEST_ASSERT_EQ(k_dirs_one, list_names(h, "/A/B/C", &lvl));
  TEST_ASSERT_EQ(0, strcmp(lvl.names[0], "DEEP.TXT"));
  expect_seeded(h, "/A/B/C/DEEP.TXT", (uint32_t)k_dirs_payload, (uint8_t)k_dirs_seed_b);
  exfat_verify(h, "nested_three_levels_built");

  /* Unwind. Each rmdir refuses until the level below it is gone. */
  TEST_ASSERT_EQ(k_ra8_err_not_empty, ra8_fs_rmdir(h, "/A/B/C"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "/A/B/C/DEEP.TXT"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rmdir(h, "/A/B/C"));
  TEST_ASSERT_EQ(k_ra8_err_not_empty, ra8_fs_rmdir(h, "/A"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rmdir(h, "/A/B"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rmdir(h, "/A"));

  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_stat(h, "/A", &(ra8_fs_stat_t){}));
  name_ctx_t root = {};
  TEST_ASSERT_EQ(k_dirs_none, list_names(h, "/", &root));
  /* Every cluster the tree used came back: the census equals the fresh-volume
   * one, which is the only authority on exFAT allocation. */
  TEST_ASSERT_EQ(baseline, alloc_bitmap_used(h));

  exfat_verify(h, "nested_three_levels_unwound");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat dirs: three-level nesting resolves both ways");
}

/**
 * @test test_rmdir_empty_frees_its_cluster
 * @brief `rmdir` on an empty directory releases exactly one cluster.
 *
 * @details The bitmap census brackets the pair: one `mkdir` takes one cluster,
 *          the matching `rmdir` gives that cluster back, and nothing else moves.
 *          A `rmdir` that retired the entry set without freeing the run would
 *          leave the census high, and the structural scan would call it an
 *          orphan.
 *
 * @par MC/DC:
 * No compound decision lies on this path. The vector it contributes is
 * `e[0] == k_exfat_entry_eod`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@priv_exfat_dir_is_empty) -> true on
 * the FIRST entry: a freshly zeroed cluster is an empty directory because
 * entry type 0x00 is the end-of-directory marker.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post The census returns to its pre-mkdir value.
 * @post The volume passes the structural scan.
 *
 * @since 0.1.0
 */
static void test_rmdir_empty_frees_its_cluster(void)
{
  TEST_BEGIN("exfat dirs: rmdir of an empty directory frees its cluster");
  build_exfat_volume();
  ra8_fs_mount_t* h        = mount_fixture();
  const uint32_t  baseline = alloc_bitmap_used(h);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/TMP"));
  TEST_ASSERT_EQ(baseline + (uint32_t)k_dirs_one, alloc_bitmap_used(h));
  exfat_verify(h, "rmdir_empty_before");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rmdir(h, "/TMP"));
  TEST_ASSERT_EQ(baseline, alloc_bitmap_used(h));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_stat(h, "/TMP", &(ra8_fs_stat_t){}));

  exfat_verify(h, "rmdir_empty_after");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat dirs: rmdir of an empty directory frees its cluster");
}

/**
 * @test test_rmdir_non_empty_changes_nothing
 * @brief A refused `rmdir` leaves the volume byte-for-byte as it found it.
 *
 * @details The emptiness proof runs before anything is written, so the refusal
 *          must be free. Both the census AND the file's contents are checked
 *          afterwards: a `rmdir` that freed the directory's cluster before
 *          discovering the file would strand the file's own clusters, which is
 *          the #604 failure mode one level up.
 *
 * @par MC/DC:
 * No compound decision lies on this path. The vectors it contributes are
 * `(e[0] & k_exfat_inuse_bit) == 0`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@priv_exfat_dir_is_empty) -> false,
 * the live-occupant arm, and `empty == 0`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@priv_exfat_rmdir) -> true, the
 * refusal.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post `rmdir` reports ::k_ra8_err_not_empty.
 * @post The census and the file's contents are unchanged.
 *
 * @since 0.1.0
 */
static void test_rmdir_non_empty_changes_nothing(void)
{
  TEST_BEGIN("exfat dirs: rmdir of a populated directory refuses and costs nothing");
  build_exfat_volume();
  ra8_fs_mount_t* h = mount_fixture();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/DATA"));
  write_seeded(h, "/DATA/F.BIN", (uint32_t)k_dirs_payload, (uint8_t)k_dirs_seed_a);
  const uint32_t before = alloc_bitmap_used(h);

  TEST_ASSERT_EQ(k_ra8_err_not_empty, ra8_fs_rmdir(h, "/DATA"));

  TEST_ASSERT_EQ(before, alloc_bitmap_used(h));
  expect_seeded(h, "/DATA/F.BIN", (uint32_t)k_dirs_payload, (uint8_t)k_dirs_seed_a);
  name_ctx_t inner = {};
  TEST_ASSERT_EQ(k_dirs_one, list_names(h, "/DATA", &inner));

  exfat_verify(h, "rmdir_non_empty_refused");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat dirs: rmdir of a populated directory refuses and costs nothing");
}

/**
 * @test test_rmdir_after_unlink_inside
 * @brief Retired entries do not count as contents.
 *
 * @details exFAT deletes an entry by clearing bit 7 of its type byte, so the
 *          bytes stay where they were. A directory that has held files is
 *          therefore never all-zero again, and an emptiness check that counted
 *          any non-zero entry would make it permanently un-removable -- the
 *          exFAT form of the FAT-side rule that deleted slots and long-name
 *          remnants do not count (#604). This proves the retired sets are
 *          discounted, and that the directory is removable afterwards.
 *
 * @par MC/DC:
 * No compound decision lies on this path. The vector it contributes is
 * `(e[0] & k_exfat_inuse_bit) == 0`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@priv_exfat_dir_is_empty) -> true,
 * the retired-remnant arm, reached only after a real file has been unlinked --
 * the complement of the false arm test_rmdir_non_empty_changes_nothing drives.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post The directory holding only retired entries is removed.
 * @post The census returns to its pre-mkdir value.
 *
 * @since 0.1.0
 */
static void test_rmdir_after_unlink_inside(void)
{
  TEST_BEGIN("exfat dirs: a directory of retired entries is empty");
  build_exfat_volume();
  ra8_fs_mount_t* h        = mount_fixture();
  const uint32_t  baseline = alloc_bitmap_used(h);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SCRATCH"));
  write_seeded(h, "/SCRATCH/ONE.BIN", (uint32_t)k_dirs_payload, (uint8_t)k_dirs_seed_a);
  write_seeded(h, "/SCRATCH/TWO.BIN", (uint32_t)k_dirs_payload, (uint8_t)k_dirs_seed_b);
  name_ctx_t inner = {};
  TEST_ASSERT_EQ(k_dirs_two, list_names(h, "/SCRATCH", &inner));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "/SCRATCH/ONE.BIN"));
  TEST_ASSERT_EQ(k_ra8_err_not_empty, ra8_fs_rmdir(h, "/SCRATCH"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "/SCRATCH/TWO.BIN"));
  TEST_ASSERT_EQ(k_dirs_none, list_names(h, "/SCRATCH", &inner));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rmdir(h, "/SCRATCH"));
  TEST_ASSERT_EQ(baseline, alloc_bitmap_used(h));

  exfat_verify(h, "rmdir_after_unlink_inside");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat dirs: a directory of retired entries is empty");
}

/* ---- names, kinds and collisions ---------------------------------------- */

/**
 * @test test_mkdir_refuses_an_existing_name
 * @brief `mkdir` never replaces -- not a directory, and not a file either.
 *
 * @details `ra8_fs_write_file()` is this library's create-or-REPLACE verb
 *          (#603); `mkdir` is not. Both collisions are checked, because the
 *          lookup that finds them cannot tell the two kinds apart on its own --
 *          a directory answers a name lookup exactly like a file does, which is
 *          the property #604 exists because of.
 *
 * @par MC/DC:
 * No compound decision lies on this path. The vectors it contributes are
 * `fe == k_ra8_ok`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@priv_exfat_mkdir_check) -> true for
 * both an existing directory and an existing file, and
 * `(file_e[k_exfat_off_file_attr] & k_exfat_attr_directory) != 0`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_mutate.c@priv_exfat_unlink_at) -> true,
 * which is what makes write_file over a directory name a refusal.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post Both `mkdir` calls report ::k_ra8_err_exists.
 * @post The census is unchanged by the refusals.
 *
 * @since 0.1.0
 */
static void test_mkdir_refuses_an_existing_name(void)
{
  TEST_BEGIN("exfat dirs: mkdir over an existing name refuses");
  build_exfat_volume();
  ra8_fs_mount_t* h = mount_fixture();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/BOOKS"));
  write_seeded(h, "/NOTES.TXT", (uint32_t)k_dirs_payload, (uint8_t)k_dirs_seed_a);
  const uint32_t before = alloc_bitmap_used(h);

  TEST_ASSERT_EQ(k_ra8_err_exists, ra8_fs_mkdir(h, "/BOOKS"));
  TEST_ASSERT_EQ(k_ra8_err_exists, ra8_fs_mkdir(h, "/NOTES.TXT"));
  TEST_ASSERT_EQ(before, alloc_bitmap_used(h));

  /* The reverse collision: write_file over a DIRECTORY name is refused rather
   * than replacing it, because unlink refuses a directory (#604). */
  uint8_t payload[k_dirs_payload] = {};
  fill(payload, (uint32_t)k_dirs_payload, (uint8_t)k_dirs_seed_b);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_fs_write_file(h, "/BOOKS", payload, (uint32_t)k_dirs_payload));
  TEST_ASSERT_EQ(before, alloc_bitmap_used(h));

  exfat_verify(h, "mkdir_refuses_existing_name");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat dirs: mkdir over an existing name refuses");
}

/**
 * @test test_wrong_verb_for_the_kind
 * @brief `unlink` refuses a real directory; `rmdir` refuses a file.
 *
 * @details The #604 guards were written against a directory forged by flipping
 *          an attribute bit on a set the driver had written, because there was
 *          no way to make a real one. This is the same pair of assertions
 *          against a directory `mkdir` actually created -- and, crucially,
 *          against one that HOLDS something, so a guard that had regressed would
 *          strand the child's clusters and the structural scan would say so.
 *
 * @par MC/DC:
 * No compound decision lies on this path. It contributes the two mirror-image
 * kind guards, each on its true arm:
 * `(file_e[k_exfat_off_file_attr] & k_exfat_attr_directory) != 0`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_mutate.c@priv_exfat_unlink_at) and
 * `(file_e[k_exfat_off_file_attr] & k_exfat_attr_directory) == 0`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@priv_exfat_rmdir_locate), plus
 * `(attr & k_exfat_attr_directory) != 0`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_read.c@priv_exfat_open) -> true.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post `unlink` on the directory reports ::k_ra8_err_invalid_arg.
 * @post `rmdir` on the file reports ::k_ra8_err_invalid_arg.
 *
 * @since 0.1.0
 */
static void test_wrong_verb_for_the_kind(void)
{
  TEST_BEGIN("exfat dirs: unlink refuses a directory, rmdir refuses a file");
  build_exfat_volume();
  ra8_fs_mount_t* h = mount_fixture();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/KEEP"));
  write_seeded(h, "/KEEP/CHILD.BIN", (uint32_t)k_dirs_payload, (uint8_t)k_dirs_seed_a);
  write_seeded(h, "/PLAIN.BIN", (uint32_t)k_dirs_payload, (uint8_t)k_dirs_seed_b);
  const uint32_t before = alloc_bitmap_used(h);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_unlink(h, "/KEEP"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_rmdir(h, "/PLAIN.BIN"));
  TEST_ASSERT_EQ(before, alloc_bitmap_used(h));

  /* Opening a directory as a file is refused too, so the child is reachable. */
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_open(h, "/KEEP", k_ra8_fs_mode_read, &f));
  expect_seeded(h, "/KEEP/CHILD.BIN", (uint32_t)k_dirs_payload, (uint8_t)k_dirs_seed_a);

  exfat_verify(h, "wrong_verb_for_the_kind");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat dirs: unlink refuses a directory, rmdir refuses a file");
}

/**
 * @test test_same_name_in_two_directories
 * @brief Two directories may each hold a different file of the same name.
 *
 * @details The flat namespace could not express this at all: both would have
 *          been one root-level name, and the second create would have REPLACED
 *          the first (#603). Different contents are written to each so a lookup
 *          that still resolved by leaf name alone would return the wrong bytes
 *          rather than merely the wrong entry.
 *
 * @par MC/DC:
 * No compound decision lies on this path. The vector it contributes is the
 * per-directory scope of the name match in
 * libs/ra8_fs/src/ra8_fs_fat_exfat_read.c@priv_exfat_find: the same leaf name
 * resolves to two different entry sets depending only on the ::exfat_dir_t it
 * is given, which a flat scan starting at the root cannot express.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post Both files exist and read back their own contents.
 * @post Removing one leaves the other untouched.
 *
 * @since 0.1.0
 */
static void test_same_name_in_two_directories(void)
{
  TEST_BEGIN("exfat dirs: one name, two directories, two files");
  build_exfat_volume();
  ra8_fs_mount_t* h = mount_fixture();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/LEFT"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/RIGHT"));
  write_seeded(h, "/LEFT/SAME.BIN", (uint32_t)k_dirs_payload, (uint8_t)k_dirs_seed_a);
  write_seeded(h, "/RIGHT/SAME.BIN", (uint32_t)k_dirs_payload, (uint8_t)k_dirs_seed_b);

  expect_seeded(h, "/LEFT/SAME.BIN", (uint32_t)k_dirs_payload, (uint8_t)k_dirs_seed_a);
  expect_seeded(h, "/RIGHT/SAME.BIN", (uint32_t)k_dirs_payload, (uint8_t)k_dirs_seed_b);
  name_ctx_t root = {};
  TEST_ASSERT_EQ(k_dirs_two, list_names(h, "/", &root));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "/LEFT/SAME.BIN"));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_stat(h, "/LEFT/SAME.BIN", &(ra8_fs_stat_t){}));
  expect_seeded(h, "/RIGHT/SAME.BIN", (uint32_t)k_dirs_payload, (uint8_t)k_dirs_seed_b);

  exfat_verify(h, "same_name_in_two_directories");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat dirs: one name, two directories, two files");
}

/**
 * @test test_long_directory_name
 * @brief A directory name spanning several Name entries round-trips.
 *
 * @details exFAT stores a name in 15-character chunks, so a 40-character name
 *          occupies three Name entries and a five-entry set. That is the shape
 *          the SetChecksum, the NameHash and the free-slot reservation all have
 *          to agree about, and the structural scan recomputes both hashes from
 *          the image rather than trusting the writer.
 *
 * @par MC/DC:
 * No compound decision lies on this path. The vector it contributes is
 * `pos < nlen`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_write.c@priv_exfat_build_set) taken both
 * ways in the same call: true for the 40 stored characters and false for the
 * 5 padding slots of the third Name entry.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post The long-named directory resolves, lists and is removable.
 * @post The volume passes the structural scan.
 *
 * @since 0.1.0
 */
static void test_long_directory_name(void)
{
  TEST_BEGIN("exfat dirs: a 40-character directory name round-trips");
  build_exfat_volume();
  ra8_fs_mount_t* h = mount_fixture();

  char name[(uint32_t)k_dirs_long_chars + 1U] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_dirs_long_chars; i++) {
    name[i] = (char)('A' + (char)(i % ('Z' - 'A' + 1)));
  }
  char path[(uint32_t)k_dirs_long_chars + 2U] = {};
  (void)snprintf(path, sizeof(path), "/%s", name);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, path));
  name_ctx_t root = {};
  TEST_ASSERT_EQ(k_dirs_one, list_names(h, "/", &root));
  TEST_ASSERT_EQ(0, strcmp(root.names[0], name));
  TEST_ASSERT_EQ(k_dirs_one, has_dir(&root, name));
  exfat_verify(h, "long_directory_name");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rmdir(h, path));
  TEST_ASSERT_EQ(k_dirs_none, list_names(h, "/", &root));

  exfat_verify(h, "long_directory_name_removed");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat dirs: a 40-character directory name round-trips");
}

/**
 * @test test_replace_inside_a_subdirectory
 * @brief The #603 replace semantics hold at depth, and leak nothing there.
 *
 * @details `ra8_fs_write_file()` over an existing name releases the old entry
 *          set and its clusters before writing the new one. That used to be a
 *          root-only claim because there was nowhere else to write; this asserts
 *          it inside a subdirectory, with the census bracketing the repeat so a
 *          leak would show as a rising count.
 *
 * @par MC/DC:
 * Decision: `(ue != k_ra8_ok) && (ue != k_ra8_err_not_found)`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c is not its home --
 * libs/ra8_fs/src/ra8_fs_fat_exfat_write.c@priv_exfat_create, 2 conditions).
 * - V1: ue = k_ra8_err_not_found (the first write, fresh name) -> T,F -> false
 *       -> proceed. Varies the second condition against V3.
 * - V2: ue = k_ra8_ok (the second write, name replaced) -> F -> false ->
 *       proceed. Varies the first condition against V3.
 * - V3: ue = a backend error -> T,T -> true -> propagate, driven by the
 *       read-fault vectors in tests/test_ra8_fs_fat_exfat_write_dir_cov.c.
 * V1+V3 prove the second condition independently affects the outcome; V2+V3
 * prove the same for the first. N+1 = 3 vectors for N=2: minimal MC/DC.
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post The directory holds exactly one entry with the newest contents.
 * @post The census after the repeat equals the census after the first write.
 *
 * @since 0.1.0
 */
static void test_replace_inside_a_subdirectory(void)
{
  TEST_BEGIN("exfat dirs: repeated write_file inside a directory replaces");
  build_exfat_volume();
  ra8_fs_mount_t* h = mount_fixture();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/CACHE"));
  write_seeded(h, "/CACHE/E.BIN", (uint32_t)k_dirs_payload, (uint8_t)k_dirs_seed_a);
  const uint32_t after_first = alloc_bitmap_used(h);

  write_seeded(h, "/CACHE/E.BIN", (uint32_t)k_dirs_payload, (uint8_t)k_dirs_seed_b);
  TEST_ASSERT_EQ(after_first, alloc_bitmap_used(h));

  name_ctx_t inner = {};
  TEST_ASSERT_EQ(k_dirs_one, list_names(h, "/CACHE", &inner));
  expect_seeded(h, "/CACHE/E.BIN", (uint32_t)k_dirs_payload, (uint8_t)k_dirs_seed_b);

  exfat_verify(h, "replace_inside_a_subdirectory");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat dirs: repeated write_file inside a directory replaces");
}

/**
 * @test test_rename_within_a_subdirectory
 * @brief `rename` works inside a directory and refuses to cross one.
 *
 * @details This rename rewrites an entry set where it lies, so it cannot move
 *          one between directories -- the two paths must resolve to the same
 *          parent. Both halves are asserted: the in-place rename succeeds and
 *          carries the contents across, and the cross-directory attempt reports
 *          ::k_ra8_err_not_supported without disturbing either directory.
 *
 * @par MC/DC:
 * No compound decision lies on this path. The vector it contributes is
 * `op.cluster != np.cluster`
 * (libs/ra8_fs/src/ra8_fs_fat_exfat_mutate.c@priv_exfat_rename_prepare) taken
 * both ways: false for the in-place rename inside "/SRC", true for the
 * attempt to move the entry into "/DST".
 *
 * @pre A freshly formatted 64 MiB exFAT volume.
 * @post The renamed file resolves under its new name in the same directory.
 * @post The cross-directory attempt changes nothing.
 *
 * @since 0.1.0
 */
static void test_rename_within_a_subdirectory(void)
{
  TEST_BEGIN("exfat dirs: rename inside a directory, never across one");
  build_exfat_volume();
  ra8_fs_mount_t* h = mount_fixture();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SRC"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/DST"));
  write_seeded(h, "/SRC/OLD.BIN", (uint32_t)k_dirs_payload, (uint8_t)k_dirs_seed_a);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rename(h, "/SRC/OLD.BIN", "/SRC/NEW.BIN"));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_stat(h, "/SRC/OLD.BIN", &(ra8_fs_stat_t){}));
  expect_seeded(h, "/SRC/NEW.BIN", (uint32_t)k_dirs_payload, (uint8_t)k_dirs_seed_a);

  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_fs_rename(h, "/SRC/NEW.BIN", "/DST/NEW.BIN"));
  expect_seeded(h, "/SRC/NEW.BIN", (uint32_t)k_dirs_payload, (uint8_t)k_dirs_seed_a);
  name_ctx_t dst = {};
  TEST_ASSERT_EQ(k_dirs_none, list_names(h, "/DST", &dst));

  exfat_verify(h, "rename_within_a_subdirectory");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat dirs: rename inside a directory, never across one");
}

/**
 * @brief Run every exFAT directory round-trip test.
 *
 * @return Process exit status.
 * @retval 0 Every test passed (a failure aborts before returning).
 *
 * @pre The host provides malloc for the 64 MiB volume.
 * @pre No other test binary shares this process.
 * @post Every fixture volume is freed.
 * @post The pass banner has been printed.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  test_mkdir_lists_as_a_directory();
  test_mkdir_file_inside_round_trip();
  test_nested_three_levels();
  test_rmdir_empty_frees_its_cluster();
  test_rmdir_non_empty_changes_nothing();
  test_rmdir_after_unlink_inside();
  test_mkdir_refuses_an_existing_name();
  test_wrong_verb_for_the_kind();
  test_same_name_in_two_directories();
  test_long_directory_name();
  test_replace_inside_a_subdirectory();
  test_rename_within_a_subdirectory();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_exfat_dirs.c\n");
  return 0;
}
