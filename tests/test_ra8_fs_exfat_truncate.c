/**
 * @file test_ra8_fs_exfat_truncate.c
 * @brief ra8_fs_truncate on exFAT: shrink, sparse grow, and the chain transition (#680).
 *
 * @details
 * exFAT records two lengths (spec sec 7.4.5), and that is what makes its grow
 * different from FAT's: instead of writing zeros, a grow raises `DataLength` to
 * the new size and LEAVES `ValidDataLength` at the written prefix, so the format
 * says "these bytes were never written" and the read path serves them as zero
 * without touching the media. These cases pin all of it against the image the
 * driver wrote: the two lengths and the `NoFatChain` flag are read straight out
 * of the entry set, the read-back proves the gap is zero, and the allocation
 * bitmap census proves a shrink frees clusters and a grow takes them.
 *
 * The transition case is the exFAT-specific one: a blocker parked on the cluster
 * a contiguous file would grow into forces the grow off the fast path, so it
 * materialises a real FAT chain and clears `NoFatChain` -- the same machinery a
 * streaming write uses (#602/#677), reached here through a pre-size rather than
 * a write.
 *
 * `RA8_EXFAT_DUMP_DIR` dumps each scenario's image for a real `fsck.exfat -n`
 * pass; unset (as in CI) it does nothing, so the suite needs no exfatprogs.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_fs_meta.h"
#include "support/fs_exfat_stream_test_util.h"
#include "unity_minimal.h"

/**
 * @enum xt_const_t
 * @brief Payload seeds and the blocker payload for the exFAT truncate tests.
 *
 * @invariant `k_xt_blk_len` fits inside one cluster on the fixture geometry.
 * @see test_exfat_grow_chain_transition()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_xt_seed     = 0x33U, /**< Payload generator seed for the file under test. */
  k_xt_blk_len  = 200U,  /**< Blocker file length (one cluster).              */
  k_xt_blk_fill = 0x5AU, /**< Blocker payload fill byte.                      */
} xt_const_t;

/**
 * @brief Bytes per cluster of the mounted exFAT volume.
 * @param[in] h Mounted volume.
 * @return `sectors_per_cluster * 512`.
 * @retval >0 The allocation-unit size.
 * @pre @p h is mounted as exFAT.
 * @pre @p h describes a formatted volume.
 * @post No state changes.
 * @post Depends only on @p h.
 * @note Pure with respect to the mount.
 * @since 0.1.0
 */
static uint32_t xt_cbytes(const ra8_fs_mount_t* h)
{
  return h->sectors_per_cluster * (uint32_t)k_mut_block_size;
}

/**
 * @brief Read the first user file's Stream `DataLength` from the image.
 * @param[in] h Mounted volume.
 * @return DataLength (low 32 bits).
 * @retval 0..UINT32_MAX The recorded length.
 * @pre @p h is mounted; at least one user file exists.
 * @pre The set sits at the standard root slots.
 * @post No state changes.
 * @post Reads only the image.
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0
 */
static uint32_t xt_dlen(const ra8_fs_mount_t* h)
{
  return disk_get_u32le(stream_strm0_off(h) + (uint32_t)k_xs_off_strm_dlen);
}

/**
 * @brief Read the first user file's Stream `ValidDataLength` from the image.
 * @param[in] h Mounted volume.
 * @return ValidDataLength (low 32 bits).
 * @retval 0..UINT32_MAX The written-prefix length.
 * @pre @p h is mounted; at least one user file exists.
 * @pre The set sits at the standard root slots.
 * @post No state changes.
 * @post Reads only the image.
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0
 */
static uint32_t xt_valid(const ra8_fs_mount_t* h)
{
  return disk_get_u32le(stream_strm0_off(h) + (uint32_t)k_xs_off_strm_valid);
}

/**
 * @brief True when the first user file still carries the `NoFatChain` flag.
 * @param[in] h Mounted volume.
 * @return 1 if contiguous, else 0.
 * @retval 1 The run is contiguous.
 * @retval 0 The run is a FAT chain.
 * @pre @p h is mounted; at least one user file exists.
 * @pre The set sits at the standard root slots.
 * @post No state changes.
 * @post Reads only the image.
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0
 */
static uint8_t xt_is_contig(const ra8_fs_mount_t* h)
{
  const uint8_t flags = s_disk.bytes[stream_strm0_off(h) + (uint32_t)k_xs_off_strm_flags];
  return ((flags & (uint8_t)k_mut_no_fat_bit) != 0U) ? 1U : 0U;
}

/**
 * @brief Assert @p got bytes at file offset @p pos are @p seed then zeros.
 * @param[in] buf     Bytes read back from the driver.
 * @param[in] got     Number of bytes in @p buf.
 * @param[in] pos     File offset @p buf[0] corresponds to.
 * @param[in] pat_end File offsets below this must match the generator; the rest zero.
 * @param[in] seed    Generator seed.
 * @return Nothing; the first mismatch fails and stops.
 * @pre @p buf holds @p got bytes.
 * @pre @p pos is a file offset, not a buffer index.
 * @post No state changes.
 * @post The scan stops at the first mismatch.
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0
 */
static void
expect_span_x(const uint8_t* buf, uint32_t got, uint32_t pos, uint32_t pat_end, uint8_t seed)
{
  for (uint32_t i = 0U; i < got; i++) {
    const uint8_t exp = ((pos + i) < pat_end) ? stream_byte_at(pos + i, seed) : 0U;
    if (buf[i] != exp) {
      TEST_FAIL_FMT("byte %u wrong", (unsigned)(pos + i));
      return;
    }
  }
}

/**
 * @brief Read @p name and assert `[0, pat_end)` is @p seed and the rest is zero.
 * @param[in,out] h       Mounted volume.
 * @param[in]     name    File to read.
 * @param[in]     total   Expected length.
 * @param[in]     pat_end Bytes that must match the generator; the rest are zero.
 * @param[in]     seed    Generator seed.
 * @return Nothing; a mismatch fails at the offset it happened.
 * @pre @p h is mounted; @p name exists at @p total bytes.
 * @pre `pat_end <= total`.
 * @post The file is closed again.
 * @post No on-disk state changes.
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0
 */
static void expect_pat_then_zero(ra8_fs_mount_t* h,
                                 const char*     name,
                                 uint32_t        total,
                                 uint32_t        pat_end,
                                 uint8_t         seed)
{
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, name, k_ra8_fs_mode_read, &f));
  uint64_t size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(f, &size));
  TEST_ASSERT_EQ(total, size);
  static uint8_t s_buf[k_xs_big_chunk];
  uint32_t       pos = 0U;
  while (pos < total) {
    uint32_t want = total - pos;
    if (want > (uint32_t)k_xs_big_chunk) {
      want = (uint32_t)k_xs_big_chunk;
    }
    uint32_t got = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, s_buf, want, &got));
    TEST_ASSERT_EQ(want, got);
    expect_span_x(s_buf, got, pos, pat_end, seed);
    pos += got;
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
}

/**
 * @brief Create @p name (append mode) and write @p len generated bytes.
 * @param[in,out] h    Mounted volume.
 * @param[in]     name File to create.
 * @param[in]     len  Bytes to write.
 * @param[in]     seed Generator seed.
 * @return Nothing; every step asserts inside.
 * @pre @p h is mounted; @p name does not exist.
 * @pre @p len fits the volume.
 * @post @p name holds @p len generated bytes, contiguous.
 * @post No handle is left open.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void make_file(ra8_fs_mount_t* h, const char* name, uint32_t len, uint8_t seed)
{
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, name, k_ra8_fs_mode_write, &f));
  stream_write_pattern(f, len, (uint32_t)k_xs_chunk, 0U, seed);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
}

/**
 * @brief Reopen @p name (append), truncate it to @p size, and close it.
 * @param[in,out] h    Mounted volume.
 * @param[in]     name File to truncate.
 * @param[in]     size New length.
 * @return Nothing; every step asserts inside.
 * @pre @p h is mounted; @p name exists.
 * @pre @p size is a valid length for the volume.
 * @post @p name is @p size bytes.
 * @post No handle is left open.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void truncate_named(ra8_fs_mount_t* h, const char* name, uint32_t size)
{
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, name, k_ra8_fs_mode_append, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_truncate(f, size));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
}

/**
 * @test test_exfat_shrink_contiguous
 * @brief Shrinking a contiguous file frees the tail and clamps ValidDataLength.
 *
 * @details A file crossing several clusters is trimmed inside its first. The two
 *          lengths must both come out at the new size, the run must stay
 *          contiguous, the freed clusters must leave the allocation bitmap, and
 *          a read must return the surviving prefix.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- the paths it drives are
 * covered elsewhere; the truncate mode/handle guard's MC/DC is owned by
 * test_fat_truncate_rejects.)
 *
 * @since 0.1.0
 */
static void test_exfat_shrink_contiguous(void)
{
  TEST_BEGIN("exfat truncate: contiguous shrink frees the tail");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t cb   = xt_cbytes(h);
  const uint32_t base = alloc_bitmap_used(h);
  make_file(h, "SH.BIN", 3U * cb, (uint8_t)k_xt_seed);
  TEST_ASSERT_EQ(base + 3U, alloc_bitmap_used(h));

  const uint32_t new_len = cb / 2U;
  truncate_named(h, "SH.BIN", new_len);
  TEST_ASSERT_EQ(base + 1U, alloc_bitmap_used(h)); /* 3 -> 1 cluster */
  TEST_ASSERT_EQ(new_len, xt_dlen(h));
  TEST_ASSERT_EQ(new_len, xt_valid(h));
  TEST_ASSERT_EQ(1U, xt_is_contig(h));
  expect_pat_then_zero(h, "SH.BIN", new_len, new_len, (uint8_t)k_xt_seed);

  stream_dump_image("exfat_shrink_contiguous");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat truncate: contiguous shrink frees the tail");
}

/**
 * @test test_exfat_shrink_to_zero
 * @brief Truncating to zero frees the whole allocation and empties the entry set.
 *
 * @details Every cluster returns to the bitmap, both lengths read zero, and a
 *          read yields nothing -- the entry set describes an empty file, kept in
 *          place so the name and its creation stamp survive.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- the paths it drives are
 * covered elsewhere; the truncate mode/handle guard's MC/DC is owned by
 * test_fat_truncate_rejects.)
 *
 * @since 0.1.0
 */
static void test_exfat_shrink_to_zero(void)
{
  TEST_BEGIN("exfat truncate: shrink to zero frees the allocation");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t cb   = xt_cbytes(h);
  const uint32_t base = alloc_bitmap_used(h);
  make_file(h, "Z.BIN", 2U * cb, (uint8_t)k_xt_seed);
  TEST_ASSERT_EQ(base + 2U, alloc_bitmap_used(h));

  truncate_named(h, "Z.BIN", 0U);
  TEST_ASSERT_EQ(base, alloc_bitmap_used(h));
  TEST_ASSERT_EQ(0U, xt_dlen(h));
  TEST_ASSERT_EQ(0U, xt_valid(h));
  expect_pat_then_zero(h, "Z.BIN", 0U, 0U, (uint8_t)k_xt_seed);

  stream_dump_image("exfat_shrink_to_zero");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat truncate: shrink to zero frees the allocation");
}

/**
 * @test test_exfat_grow_sparse
 * @brief Growing raises DataLength alone; the gap reads zero without being written.
 *
 * @details The write leaves `ValidDataLength == DataLength`; the grow must raise
 *          `DataLength` to the new size and leave `ValidDataLength` at the old
 *          one, which is the format's way of recording an unwritten suffix. A
 *          read must then return the prefix followed by zeros, and the bitmap
 *          must show only the extra clusters the length now needs.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- the paths it drives are
 * covered elsewhere; the truncate mode/handle guard's MC/DC is owned by
 * test_fat_truncate_rejects.)
 *
 * @since 0.1.0
 */
static void test_exfat_grow_sparse(void)
{
  TEST_BEGIN("exfat truncate: grow raises DataLength, leaves ValidDataLength");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t cb      = xt_cbytes(h);
  const uint32_t old_len = cb + (cb / 2U);   /* 2 clusters */
  const uint32_t new_len = (3U * cb) + 100U; /* 4 clusters */
  const uint32_t base    = alloc_bitmap_used(h);
  make_file(h, "GR.BIN", old_len, (uint8_t)k_xt_seed);
  TEST_ASSERT_EQ(base + 2U, alloc_bitmap_used(h));

  truncate_named(h, "GR.BIN", new_len);
  TEST_ASSERT_EQ(base + 4U, alloc_bitmap_used(h)); /* 2 -> 4 clusters */
  TEST_ASSERT_EQ(new_len, xt_dlen(h));
  TEST_ASSERT_EQ(old_len, xt_valid(h)); /* the gap stays unwritten */
  TEST_ASSERT_EQ(1U, xt_is_contig(h));
  expect_pat_then_zero(h, "GR.BIN", new_len, old_len, (uint8_t)k_xt_seed);

  stream_dump_image("exfat_grow_sparse");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat truncate: grow raises DataLength, leaves ValidDataLength");
}

/**
 * @test test_exfat_grow_chain_transition
 * @brief A grow blocked from staying contiguous converts the run to a FAT chain.
 *
 * @details A one-cluster file is opened, a blocker file is parked on the cluster
 *          adjacent to its tail (the next-free hint makes that deterministic),
 *          and the file is then grown by truncation. Unable to take its tail's
 *          successor, the grow materialises a real FAT chain and clears
 *          `NoFatChain` -- proven by reading the flag back out of the entry set.
 *          The grown gap still reads zero, the recorded lengths are right, and
 *          the blocker is untouched.
 *
 * @par MC/DC:
 * Decision: `(file->no_fat_chain != 0) && (next == file->tail_cluster + 1)` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_stream.c@priv_exfat_link_cluster`. The
 * convert-to-chain vector (nofat=1, next!=tail+1) is driven here; the
 * stay-contiguous vector is driven by ::test_exfat_grow_sparse and the
 * already-a-chain vector by the streaming suite. This case reaches the decision
 * through a truncate rather than a write.
 *
 * @since 0.1.0
 */
static void test_exfat_grow_chain_transition(void)
{
  TEST_BEGIN("exfat truncate: a blocked grow converts the run to a FAT chain");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t cb = xt_cbytes(h);

  ra8_fs_file_t* fa = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_write, &fa));
  stream_write_pattern(fa, cb, (uint32_t)k_xs_chunk, 0U, (uint8_t)k_xt_seed);
  TEST_ASSERT_EQ(1U, xt_is_contig(h));

  /* Park a blocker on A's tail + 1: the write above left the next-free hint one
   * past A's only cluster, so this file takes exactly that cluster. */
  static uint8_t s_blk[k_xt_blk_len];
  memset(s_blk, (int)k_xt_blk_fill, sizeof s_blk);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "BLK.BIN", s_blk, (uint32_t)k_xt_blk_len));

  /* Grow A: it cannot stay contiguous, so it becomes a real FAT chain. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_truncate(fa, (uint64_t)2U * cb));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fa));

  TEST_ASSERT_EQ(0U, xt_is_contig(h)); /* NoFatChain cleared */
  TEST_ASSERT_EQ(2U * cb, xt_dlen(h));
  TEST_ASSERT_EQ(cb, xt_valid(h)); /* the written prefix */
  expect_pat_then_zero(h, "A.BIN", 2U * cb, cb, (uint8_t)k_xt_seed);

  /* The conversion touched only A. */
  ra8_fs_file_t* fb = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "BLK.BIN", k_ra8_fs_mode_read, &fb));
  static uint8_t s_got[k_xt_blk_len];
  uint32_t       n = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(fb, s_got, (uint32_t)k_xt_blk_len, &n));
  TEST_ASSERT_EQ(k_xt_blk_len, n);
  TEST_ASSERT_EQ(0, memcmp(s_got, s_blk, sizeof s_blk));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fb));

  stream_dump_image("exfat_grow_chain_transition");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat truncate: a blocked grow converts the run to a FAT chain");
}

/**
 * @test test_exfat_shrink_chained
 * @brief A FAT-chained exFAT file shrinks by re-terminating and freeing the tail.
 *
 * @details A file forced onto a real FAT chain (blocker parked, grown to three
 *          clusters) is then trimmed to two. Because it is no longer contiguous,
 *          the shrink walks the chain to the new tail, caps it with an
 *          end-of-chain marker and frees the remainder off the allocation bitmap
 *          -- the chained arm the contiguous cases never reach. The result stays
 *          a chain, reads back the written prefix then zeros, and is
 *          `fsck.exfat`-clean.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- the paths it drives are
 * covered elsewhere; the truncate mode/handle guard's MC/DC is owned by
 * test_fat_truncate_rejects.)
 *
 * @since 0.1.0
 */
static void test_exfat_shrink_chained(void)
{
  TEST_BEGIN("exfat truncate: a chained file shrinks via the FAT-walk arm");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t cb   = xt_cbytes(h);
  const uint32_t base = alloc_bitmap_used(h);

  ra8_fs_file_t* fa = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "C.BIN", k_ra8_fs_mode_write, &fa));
  stream_write_pattern(fa, cb, (uint32_t)k_xs_chunk, 0U, (uint8_t)k_xt_seed);

  /* Park a blocker on the tail's successor, then grow to three clusters: the
   * first growth converts to a chain, the second extends it. */
  static uint8_t s_blk[k_xt_blk_len];
  memset(s_blk, (int)k_xt_blk_fill, sizeof s_blk);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "BLK.BIN", s_blk, (uint32_t)k_xt_blk_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_truncate(fa, (uint64_t)3U * cb));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fa));
  TEST_ASSERT_EQ(0U, xt_is_contig(h)); /* now a FAT chain */
  const uint32_t used_before = alloc_bitmap_used(h);

  /* Shrink the chain from three clusters to two: the FAT-walk shrink arm. */
  truncate_named(h, "C.BIN", 2U * cb);
  TEST_ASSERT_EQ(used_before - 1U, alloc_bitmap_used(h));
  TEST_ASSERT_EQ(0U, xt_is_contig(h)); /* still a chain */
  TEST_ASSERT_EQ(2U * cb, xt_dlen(h));
  TEST_ASSERT_EQ(cb, xt_valid(h));
  expect_pat_then_zero(h, "C.BIN", 2U * cb, cb, (uint8_t)k_xt_seed);
  (void)base;

  stream_dump_image("exfat_shrink_chained");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat truncate: a chained file shrinks via the FAT-walk arm");
}

/**
 * @test test_exfat_truncate_rejects_readonly
 * @brief A read-only exFAT handle cannot be truncated, and the file is untouched.
 *
 * @details The shared mode guard rejects a read open on exFAT exactly as on FAT,
 *          and a read handle carries no entry-set coordinates to flush, so the
 *          refusal must land before any of that is reached.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- the paths it drives are
 * covered elsewhere; the truncate mode/handle guard's MC/DC is owned by
 * test_fat_truncate_rejects.)
 *
 * @since 0.1.0
 */
static void test_exfat_truncate_rejects_readonly(void)
{
  TEST_BEGIN("exfat truncate: a read-only handle is refused");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t cb = xt_cbytes(h);
  make_file(h, "RO.BIN", cb, (uint8_t)k_xt_seed);

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "RO.BIN", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_truncate(f, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(cb, xt_dlen(h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat truncate: a read-only handle is refused");
}

/**
 * @brief Run every exFAT truncate test.
 * @return 0 on success (a failure aborts inside the assertion macros).
 * @retval 0 Every test passed.
 * @pre The host provides a working heap.
 * @pre No volume is mounted on entry.
 * @post Every test built and released its own volume.
 * @post A success banner is written to stderr.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_exfat_shrink_contiguous();
  test_exfat_shrink_to_zero();
  test_exfat_grow_sparse();
  test_exfat_grow_chain_transition();
  test_exfat_shrink_chained();
  test_exfat_truncate_rejects_readonly();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_exfat_truncate.c\n");
  return 0;
}
