/**
 * @file test_ra8_fs_exfat_stream.c
 * @brief exFAT streaming write: create, append, truncate, seek, handles (#602).
 *
 * @details
 * `ra8_fs_open()` answered `k_ra8_err_not_supported` for both writing modes on
 * an exFAT volume, so the largest file this firmware could put on the card
 * every removable device above 32 GB ships with was bounded by SRAM -- and a
 * volume that had seen a few delete/create cycles could refuse a write that
 * fitted, because the one write path needed a single CONTIGUOUS run.
 *
 * This file covers the lifecycle half of the fix: create through the open
 * seam, write in chunks that cross sector and cluster boundaries, append to
 * what is already there, truncate in place, seek and overwrite, and drive four
 * handles at once against a four-slot table. Its siblings cover the allocation
 * half (`test_ra8_fs_exfat_stream_chain.c`) and the `ValidDataLength` /
 * refusal edges (`test_ra8_fs_exfat_stream_vdl.c`).
 *
 * Every assertion that matters is made against BYTES IN THE IMAGE -- the
 * Stream entry's `FirstCluster`, `ValidDataLength`, `DataLength` and
 * `GeneralSecondaryFlags`, and the File entry's `SetChecksum` recomputed from
 * the set -- not against what the handle claims, because a handle that has
 * drifted from the volume is exactly the defect these tests exist to catch.
 *
 * Each scenario ends by dumping the volume through `stream_dump_image()`, so
 * running the suite with `RA8_EXFAT_DUMP_DIR` set produces one image per
 * scenario for `fsck.exfat -n`. See `tests/support/fs_exfat_stream_test_util.h`.
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
#include "support/fs_exfat_stream_test_util.h"
#include "unity_minimal.h"

/**
 * @enum xsm_const_t
 * @brief Handle counts and expected entry counts for the lifecycle tests.
 *
 * @invariant `k_xsm_handles` equals ::k_ra8_fs_max_files.
 *
 * @par Example:
 * @code
 * ra8_fs_file_t* f[k_xsm_handles] = {};
 * @endcode
 *
 * @see test_stream_interleaved_handles()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_xsm_handles     = 4U,    /**< Concurrent open handles the table allows.   */
  k_xsm_one_entry   = 1U,    /**< Directory entries one file must resolve to. */
  k_xsm_two_entries = 2U,    /**< Directory entries for two distinct files.   */
  k_xsm_set_entries = 3U,    /**< File + Stream + one Name entry.             */
  k_xsm_no_clusters = 0U,    /**< FirstCluster of an empty file.              */
  k_xsm_name_cap    = 64U,   /**< Longest exFAT name this adapter accepts.    */
  k_xsm_poison      = 0xEEU, /**< Byte a zero-length read must not overwrite. */
} xsm_const_t;

/**
 * @test test_stream_create_multi_cluster
 *
 * @brief A file written in 333-byte chunks through `open(write)` reads back whole.
 *
 * @details The base case the issue describes: no payload buffer, no contiguous
 *          run reserved up front, just a handle and repeated writes. The chunk
 *          size divides neither 512 nor 4096, so all four positions inside the
 *          write loop occur -- start of a sector, middle of one, across a
 *          sector edge and across a cluster edge -- and the read-back is
 *          compared against the generator so a slice landing at the wrong
 *          offset fails rather than passing on matching bytes.
 *
 *          `ValidDataLength` and `DataLength` must both equal the length: a
 *          sequential write leaves no unwritten prefix, and a driver that
 *          updated only one of them writes a file a host reads short or reads
 *          as garbage.
 *
 * @par MC/DC:
 * Decision: `(no_fat_chain != 0) && (next == tail_cluster + 1)` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_stream.c@priv_exfat_link_cluster`
 * (2 conditions). This case supplies the TRUE vector: a fresh volume hands out
 * successive clusters, so every growth after the first keeps the run
 * contiguous and writes not one FAT entry. The two false vectors live in
 * `test_ra8_fs_exfat_stream_chain.c`.
 *
 * @since 0.1.0
 */
static void test_stream_create_multi_cluster(void)
{
  TEST_BEGIN("exfat stream: chunked create across clusters");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "BIG.BIN", k_ra8_fs_mode_write, &f));
  stream_write_pattern(f,
                       (uint32_t)k_xs_multi_cluster,
                       (uint32_t)k_xs_chunk,
                       0U,
                       (uint8_t)k_xs_seed_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  stream_expect_contents(h, "BIG.BIN", (uint32_t)k_xs_multi_cluster, (uint8_t)k_xs_seed_a);

  const uint32_t strm = stream_strm0_off(h);
  TEST_ASSERT_EQ(k_xs_multi_cluster, disk_get_u32le(strm + (uint32_t)k_xs_off_strm_dlen));
  TEST_ASSERT_EQ(k_xs_multi_cluster, disk_get_u32le(strm + (uint32_t)k_xs_off_strm_valid));
  TEST_ASSERT_EQ(0U, disk_get_u32le(strm + (uint32_t)k_xs_off_dlen_hi));
  TEST_ASSERT_EQ(k_xs_flag_contig, s_disk.bytes[strm + (uint32_t)k_xs_off_strm_flags]);

  /* The set the driver left behind must checksum to what it recorded, or a
   * host `fsck` rejects the entry the moment it reads it. */
  const uint32_t file_off = root_byte(h, (uint32_t)k_mut_root_file0_idx);
  const uint16_t stored =
    (uint16_t)((uint16_t)s_disk.bytes[file_off + k_xs_off_file_csum] |
               (uint16_t)((uint16_t)s_disk.bytes[file_off + k_xs_off_file_csum + 1U] << 8U));
  TEST_ASSERT_EQ(
    stream_set_checksum(h, (uint32_t)k_mut_root_file0_idx, (uint32_t)k_xsm_set_entries),
    stored);

  stream_dump_image("stream_create_multi_cluster");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat stream: chunked create across clusters");
}

/**
 * @test test_stream_append_extends
 *
 * @brief `open(append)` resumes at `DataLength` and grows the same file.
 *
 * @details Writes a sub-sector file, closes it, reopens in append mode and
 *          writes enough to cross several clusters. The interesting part is
 *          the FIRST appended byte: it lands mid-sector, in a cluster the file
 *          already owns and has already written, so a driver that positions an
 *          append at a cluster boundary -- or that re-allocates instead of
 *          reusing -- corrupts the join. The comparator checks the whole file
 *          against one generator stream, so a discontinuity at the join fails
 *          at the exact offset.
 *
 * @par MC/DC:
 * Decision: `mode == k_ra8_fs_mode_write` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_openw.c@priv_exfat_open_found`
 * (1 condition). This case is the FALSE vector -- survey the allocation and
 * park at `DataLength`. `test_stream_truncate_in_place()` is the TRUE vector.
 *
 * @since 0.1.0
 */
static void test_stream_append_extends(void)
{
  TEST_BEGIN("exfat stream: append resumes at DataLength");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "LOG.TXT", k_ra8_fs_mode_write, &f));
  stream_write_pattern(f,
                       (uint32_t)k_xs_sub_sector,
                       (uint32_t)k_xs_chunk,
                       0U,
                       (uint8_t)k_xs_seed_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "LOG.TXT", k_ra8_fs_mode_append, &f));
  uint32_t at = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_tell(f, &at));
  TEST_ASSERT_EQ(k_xs_sub_sector, at);
  stream_write_pattern(f,
                       (uint32_t)k_xs_multi_cluster,
                       (uint32_t)k_xs_chunk,
                       (uint32_t)k_xs_sub_sector,
                       (uint8_t)k_xs_seed_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  const uint32_t total = (uint32_t)k_xs_sub_sector + (uint32_t)k_xs_multi_cluster;
  stream_expect_contents(h, "LOG.TXT", total, (uint8_t)k_xs_seed_a);

  mut_list_ctx_t ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", count_cb, &ctx));
  TEST_ASSERT_EQ(k_xsm_one_entry, ctx.count);

  stream_dump_image("stream_append_extends");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat stream: append resumes at DataLength");
}

/**
 * @test test_stream_truncate_in_place
 *
 * @brief `open(write)` on an existing name empties it and returns its clusters.
 *
 * @details Writes a four-cluster file, reopens it for writing and puts a
 *          300-byte file there instead. Three things have to be true and only
 *          one of them is visible from the API: the contents are the new ones,
 *          the directory still holds ONE entry for the name (a create that
 *          appended a second set was the #603 defect), and the allocation
 *          bitmap has given back every cluster the old contents held. The
 *          bitmap is measured directly, because leaked clusters are by
 *          definition the ones no directory entry points at.
 *
 * @par MC/DC:
 * Decision: `mode == k_ra8_fs_mode_write` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_openw.c@priv_exfat_open_found`
 * (1 condition) -- the TRUE vector, paired with the append case above.
 *
 * @since 0.1.0
 */
static void test_stream_truncate_in_place(void)
{
  TEST_BEGIN("exfat stream: write mode truncates in place");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t empty_used = alloc_bitmap_used(h);

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "DATA.BIN", k_ra8_fs_mode_write, &f));
  stream_write_pattern(f,
                       (uint32_t)k_xs_multi_cluster,
                       (uint32_t)k_xs_big_chunk,
                       0U,
                       (uint8_t)k_xs_seed_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  const uint32_t big_used = alloc_bitmap_used(h);
  TEST_ASSERT(big_used > empty_used);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "DATA.BIN", k_ra8_fs_mode_write, &f));
  uint32_t size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(f, &size));
  TEST_ASSERT_EQ(0U, size);
  stream_write_pattern(f,
                       (uint32_t)k_xs_sub_sector,
                       (uint32_t)k_xs_chunk,
                       0U,
                       (uint8_t)k_xs_seed_b);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  stream_expect_contents(h, "DATA.BIN", (uint32_t)k_xs_sub_sector, (uint8_t)k_xs_seed_b);
  TEST_ASSERT_EQ(empty_used + 1U, alloc_bitmap_used(h));

  mut_list_ctx_t ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", count_cb, &ctx));
  TEST_ASSERT_EQ(k_xsm_one_entry, ctx.count);

  stream_dump_image("stream_truncate_in_place");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat stream: write mode truncates in place");
}

/**
 * @brief Read `PATCH.BIN` whole and assert the three regions came from the
 *        streams that wrote them.
 *
 * @details Before the patch and after it must come from the first generator;
 *          the patched span from the second. Split out of the test so neither
 *          it nor this stays over the statement budget.
 *
 * @param[in,out] h    Mounted exFAT volume.
 * @param[in]     at   File offset the patch starts at.
 * @param[in]     len  Length of the patch.
 *
 * @return Nothing; every check is asserted inside.
 *
 * @pre @p h is mounted and `PATCH.BIN` exists at its full length.
 * @pre No handle is open on it.
 * @post The file is closed again.
 * @post No on-disk state is modified.
 *
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0
 */
static void expect_patched(ra8_fs_mount_t* h, uint32_t at, uint32_t len)
{
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "PATCH.BIN", k_ra8_fs_mode_read, &f));
  static uint8_t s_back[k_xs_multi_cluster];
  uint32_t       got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, s_back, (uint32_t)k_xs_multi_cluster, &got));
  TEST_ASSERT_EQ(k_xs_multi_cluster, got);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  stream_expect_span(s_back, at, 0U, (uint8_t)k_xs_seed_a);
  stream_expect_span(&s_back[at], len, at, (uint8_t)k_xs_seed_b);
  stream_expect_span(&s_back[at + len],
                     (uint32_t)k_xs_multi_cluster - (at + len),
                     at + len,
                     (uint8_t)k_xs_seed_a);
}

/**
 * @test test_stream_seek_overwrite
 *
 * @brief Seeking back into a written file overwrites without changing its length.
 *
 * @details Writes one stream, seeks to a byte that is neither sector- nor
 *          cluster-aligned, and overwrites a span from the OTHER generator.
 *          `DataLength` must not move -- an overwrite is not a growth -- and
 *          the three regions (before, over, after) must each read back from
 *          the stream that wrote them, which a driver that mistook the seek
 *          for a truncate or an append would fail.
 *
 * @par MC/DC:
 * Decision: `file->offset > file->size_bytes` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_stream.c@priv_exfat_write_stream`
 * (1 condition). The overwrite supplies the FALSE vector -- bytes land below
 * the recorded length and it must stay put -- while every appending case above
 * supplies the TRUE one.
 *
 * @since 0.1.0
 */
static void test_stream_seek_overwrite(void)
{
  TEST_BEGIN("exfat stream: seek + overwrite keeps DataLength");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "PATCH.BIN", k_ra8_fs_mode_write, &f));
  stream_write_pattern(f,
                       (uint32_t)k_xs_multi_cluster,
                       (uint32_t)k_xs_big_chunk,
                       0U,
                       (uint8_t)k_xs_seed_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  const uint32_t patch_at  = (uint32_t)k_xs_one_cluster + (uint32_t)k_xs_chunk;
  const uint32_t patch_len = (uint32_t)k_xs_chunk;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "PATCH.BIN", k_ra8_fs_mode_append, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_seek(f, patch_at));
  static uint8_t s_patch[k_xs_chunk];
  stream_fill_at(s_patch, patch_len, patch_at, (uint8_t)k_xs_seed_b);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, s_patch, patch_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  const uint32_t strm = stream_strm0_off(h);
  TEST_ASSERT_EQ(k_xs_multi_cluster, disk_get_u32le(strm + (uint32_t)k_xs_off_strm_dlen));

  expect_patched(h, patch_at, patch_len);

  stream_dump_image("stream_seek_overwrite");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat stream: seek + overwrite keeps DataLength");
}

/**
 * @test test_stream_interleaved_handles
 *
 * @brief Two handles writing two files at once keep their own entry sets straight.
 *
 * @details The file table is shared, the scratch sector is shared, and the
 *          per-mount next-free hint is shared, so two open streams are the
 *          case where a driver that kept ANY of a file's streaming state
 *          outside its handle produces a mess -- clusters attributed to the
 *          wrong file, or one file's `DataLength` written into the other's
 *          entry set. Writes are alternated in chunks so both files grow
 *          across cluster boundaries in an interleaved order, and both are
 *          verified independently afterwards.
 *
 *          The table's capacity is asserted at the same time: the fifth open
 *          must be refused rather than silently reusing a slot.
 *
 * @par MC/DC:
 * (no compound decision is uniquely touched -- this exercises the handle-state
 * separation end to end; the file-table-full guard it also drives is a single
 * condition whose false arm every other case here supplies)
 *
 * @since 0.1.0
 */
static void test_stream_interleaved_handles(void)
{
  TEST_BEGIN("exfat stream: interleaved handles vs the file table");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* fa = nullptr;
  ra8_fs_file_t* fb = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_write, &fa));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "B.BIN", k_ra8_fs_mode_write, &fb));

  static uint8_t s_buf[k_xs_chunk];
  uint32_t       done = 0U;
  while (done < (uint32_t)k_xs_multi_cluster) {
    uint32_t n = (uint32_t)k_xs_multi_cluster - done;
    if (n > (uint32_t)k_xs_chunk) {
      n = (uint32_t)k_xs_chunk;
    }
    stream_fill_at(s_buf, n, done, (uint8_t)k_xs_seed_a);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(fa, s_buf, n));
    stream_fill_at(s_buf, n, done, (uint8_t)k_xs_seed_b);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(fb, s_buf, n));
    done += n;
  }

  /* Four slots exist and two are taken; the fifth open must be refused. */
  ra8_fs_file_t* extra[k_xsm_handles] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_read, &extra[0]));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "B.BIN", k_ra8_fs_mode_read, &extra[1]));
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_read, &extra[2]));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(extra[0]));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(extra[1]));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fa));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fb));

  stream_expect_contents(h, "A.BIN", (uint32_t)k_xs_multi_cluster, (uint8_t)k_xs_seed_a);
  stream_expect_contents(h, "B.BIN", (uint32_t)k_xs_multi_cluster, (uint8_t)k_xs_seed_b);

  mut_list_ctx_t ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", count_cb, &ctx));
  TEST_ASSERT_EQ(k_xsm_two_entries, ctx.count);

  stream_dump_image("stream_interleaved_handles");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat stream: interleaved handles vs the file table");
}

/**
 * @test test_stream_empty_file
 *
 * @brief An empty exFAT file is legal, and this is what one looks like.
 *
 * @details `ra8_fs_write_file(len = 0)` used to be refused on exFAT and
 *          accepted on FAT, which is the inconsistency #602 is about: the same
 *          call meant two things depending on a volume format the caller was
 *          supposed to be abstracted from. It now creates the file on both.
 *
 *          exFAT spec sec 7.4.4 is specific about the shape: `FirstCluster`
 *          must be 0 and `NoFatChain` must be clear when `DataLength` is 0. A
 *          driver that left the flag set over an allocation of nothing writes
 *          an entry a host checker rejects, so the flags byte is asserted
 *          exactly, not merely tested for the length.
 *
 * @par MC/DC:
 * Decision: `(alloc_clusters > 0) && (no_fat_chain != 0)` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_stream.c@priv_exfat_patch_stream`
 * (2 conditions). This case supplies the vector that varies the FIRST
 * condition -- no clusters at all, so flags 0x01 and `FirstCluster` 0 -- while
 * `test_stream_create_multi_cluster()` supplies the true vector and
 * `test_ra8_fs_exfat_stream_chain.c` the one that varies the second.
 *
 * @since 0.1.0
 */
static void test_stream_empty_file(void)
{
  TEST_BEGIN("exfat stream: zero-length file is created, not refused");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t empty_used = alloc_bitmap_used(h);

  const uint8_t nothing = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "EMPTY.TXT", &nothing, 0U));

  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "EMPTY.TXT", &st));
  TEST_ASSERT_EQ(0U, st.size_bytes);
  TEST_ASSERT(!st.is_directory);

  const uint32_t strm = stream_strm0_off(h);
  TEST_ASSERT_EQ(k_xsm_no_clusters, disk_get_u32le(strm + (uint32_t)k_xs_off_strm_clus));
  TEST_ASSERT_EQ(0U, disk_get_u32le(strm + (uint32_t)k_xs_off_strm_dlen));
  TEST_ASSERT_EQ(0U, disk_get_u32le(strm + (uint32_t)k_xs_off_strm_valid));
  TEST_ASSERT_EQ(k_xs_flag_poss, s_disk.bytes[strm + (uint32_t)k_xs_off_strm_flags]);
  TEST_ASSERT_EQ(empty_used, alloc_bitmap_used(h));

  /* A read of an empty file is an immediate EOF, not an error. */
  ra8_fs_file_t* f   = nullptr;
  uint8_t        one = (uint8_t)k_xsm_poison;
  uint32_t       got = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "EMPTY.TXT", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, &one, 1U, &got));
  TEST_ASSERT_EQ(0U, got);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  stream_dump_image("stream_empty_file");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat stream: zero-length file is created, not refused");
}

/**
 * @test test_stream_name_guards
 *
 * @brief An empty or over-long exFAT name is refused before anything is written.
 *
 * @details The two argument guards `priv_exfat_open_write()` applies. They used
 *          to live in the whole-file creator, and they have to survive its
 *          removal or a caller that hands in "/" or a 65-character name gets a
 *          directory entry it can never look up again. The directory listing is
 *          asserted empty afterwards: a guard that refuses AFTER laying down a
 *          set is not a guard.
 *
 * @par MC/DC:
 * Two single-condition guards, checked in order: `nlen == 0` and
 * `nlen > k_exfat_name_cap` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_openw.c@priv_exfat_open_write`. This case
 * drives both true; every other case in this file drives both false.
 *
 * @since 0.1.0
 */
static void test_stream_name_guards(void)
{
  TEST_BEGIN("exfat stream: empty and over-long names refused");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_open(h, "/", k_ra8_fs_mode_write, &f));

  char toolong[k_xs_sub_sector] = {};
  memset(toolong, 'A', (size_t)k_xsm_name_cap + 1U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_open(h, toolong, k_ra8_fs_mode_append, &f));

  mut_list_ctx_t ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", count_cb, &ctx));
  TEST_ASSERT_EQ(0U, ctx.count);

  stream_dump_image("stream_name_guards");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat stream: empty and over-long names refused");
}

/**
 * @brief Run every exFAT streaming lifecycle test.
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
int32_t main(void)
{
  test_stream_create_multi_cluster();
  test_stream_append_extends();
  test_stream_truncate_in_place();
  test_stream_seek_overwrite();
  test_stream_interleaved_handles();
  test_stream_empty_file();
  test_stream_name_guards();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_exfat_stream.c\n");
  return 0;
}
