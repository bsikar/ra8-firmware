/**
 * @file test_ra8_fs_exfat_stream_vdl.c
 * @brief exFAT ValidDataLength semantics and the streaming refusals (#602).
 *
 * @details
 * exFAT is the only filesystem this adapter mounts that records TWO lengths
 * per file (exFAT spec sec 7.4.5): `DataLength` is how long the file is, and
 * `ValidDataLength` is how much of it was ever written. The bytes between them
 * were never initialised, and the clusters behind them still hold whatever the
 * previous tenant left -- so an implementation that serves them raw hands a
 * caller somebody else's data, and one that ignores the distinction on write
 * records a prefix length that is a lie.
 *
 * Nothing in this tree could produce that state before, because exFAT had no
 * open-for-write at all; the driver simply ignored `ValidDataLength` on read.
 * These cases construct it the only way a bare-metal test can -- by patching
 * the entry set the driver itself wrote, and repairing the SetChecksum so the
 * result is a set a host would accept -- and then pin both halves of the
 * contract:
 *
 *   - a READ past the prefix returns zeros, not cluster residue;
 *   - a WRITE past the prefix fills the gap with real zeros first, because
 *     `ValidDataLength` is a PREFIX length and a hole in the middle of a file
 *     has no encoding at all.
 *
 * The refusals live here too: an entry set with more Name entries than this
 * adapter can rewrite, and a `DataLength` above 4 GiB that no length in this
 * API can carry. Both are refused at open rather than silently truncated,
 * which is the difference between "not supported" and "corrupted your file".
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
 * @enum xsv_const_t
 * @brief Prefix lengths, patch values and expectations for the VDL tests.
 *
 * @details ::k_xsv_valid_len is deliberately not a multiple of 512, so the
 *          prefix ends INSIDE a sector: a driver that clipped reads to sector
 *          granularity rather than to the prefix would serve the residue in
 *          the rest of that sector and pass a coarser test.
 *
 * @invariant `k_xsv_valid_len < k_xs_multi_cluster`.
 *
 * @par Example:
 * @code
 * disk_set_u32le(strm + k_xs_off_strm_valid, k_xsv_valid_len);
 * @endcode
 *
 * @see test_stream_read_past_valid_is_zero()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_xsv_valid_len   = 5555U, /**< Written prefix; ends mid-sector on purpose. */
  k_xsv_residue     = 0xC7U, /**< Poison written into the unwritten tail.     */
  k_xsv_set_entries = 3U,    /**< File + Stream + one Name entry.             */
  k_xsv_big_secnt   = 8U,    /**< SecondaryCount past what we can rewrite.    */
  k_xsv_probe_span  = 64U,   /**< Bytes probed either side of the prefix.     */
  k_xsv_over_4gib   = 1U,    /**< DataLength high word of a >4 GiB file.      */
  k_xsv_tail_seed   = 0x2BU, /**< Seed of the bytes appended after the gap.   */
  k_xsv_append_len  = 1000U, /**< Bytes appended past the recorded length.    */
  k_xsv_byte_mask   = 0xFFU, /**< Low byte of the packed checksum.            */
  k_xsv_shift_byte  = 8U,    /**< High-byte position of the packed checksum.  */
} xsv_const_t;

/**
 * @brief Repair the first user file's SetChecksum after patching its set.
 *
 * @details A patched entry set is only a useful fixture if it is one a host
 *          would accept, and the checksum covers every byte a patch touches.
 *          Recomputing it here means these tests present the driver with a
 *          VALID file carrying an awkward `ValidDataLength`, not a corrupt one
 *          -- so a failure is about the semantics under test and not about the
 *          fixture.
 *
 * @param[in] h Mounted exFAT volume whose root slot 3 holds a File entry.
 *
 * @return Nothing.
 *
 * @pre @p h is mounted and exactly one user file exists.
 * @pre The set occupies root entries 3..5.
 * @post The stored SetChecksum matches the patched bytes.
 * @post No other byte of the image changes.
 *
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0
 */
static void repair_checksum(const ra8_fs_mount_t* h)
{
  const uint16_t cs =
    stream_set_checksum(h, (uint32_t)k_mut_root_file0_idx, (uint32_t)k_xsv_set_entries);
  const uint32_t file_off = root_byte(h, (uint32_t)k_mut_root_file0_idx);
  s_disk.bytes[file_off + (uint32_t)k_xs_off_file_csum] = (uint8_t)(cs & (uint16_t)k_xsv_byte_mask);
  s_disk.bytes[file_off + (uint32_t)k_xs_off_file_csum + 1U] =
    (uint8_t)((cs >> (uint16_t)k_xsv_shift_byte) & (uint16_t)k_xsv_byte_mask);
}

/**
 * @brief Build a file whose written prefix is shorter than its recorded length.
 *
 * @details Writes a real multi-cluster file, poisons the whole of it on the
 *          media so "unwritten" bytes are visibly NOT zero, then lowers
 *          `ValidDataLength` to ::k_xsv_valid_len and repairs the checksum. The
 *          result is exactly what another implementation leaves behind when it
 *          pre-allocates: `DataLength` clusters owned, only a prefix ever
 *          written.
 *
 * @param[in] h Mounted exFAT volume.
 *
 * @return Nothing; failures assert inside.
 *
 * @pre @p h is mounted with an empty root directory.
 * @pre The fixture's RAM disk is present.
 * @post `VDL.BIN` exists with `ValidDataLength < DataLength`.
 * @post Every byte of its allocation reads as ::k_xsv_residue on the media.
 *
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0
 */
static void make_partly_valid_file(ra8_fs_mount_t* h)
{
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "VDL.BIN", k_ra8_fs_mode_write, &f));
  static uint8_t s_poison[k_xs_multi_cluster];
  memset(s_poison, (int)k_xsv_residue, sizeof s_poison);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, s_poison, (uint32_t)k_xs_multi_cluster));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  const uint32_t strm = stream_strm0_off(h);
  disk_set_u32le(strm + (uint32_t)k_xs_off_strm_valid, (uint32_t)k_xsv_valid_len);
  repair_checksum(h);
}

/**
 * @test test_stream_read_past_valid_is_zero
 *
 * @brief Bytes past `ValidDataLength` read as zero, not as cluster residue.
 *
 * @details The whole allocation is poisoned with a non-zero byte, so "reads as
 *          zero" cannot pass by accident: every byte the driver serves from the
 *          media is 0xC7 and every byte it is required to synthesise is 0x00.
 *          The probe straddles the prefix boundary, which is mid-sector, so a
 *          driver that clipped to sector granularity fails here even though it
 *          would pass a probe at a sector edge. The file's LENGTH must still be
 *          `DataLength` -- the tail is unwritten, not absent.
 *
 * @par MC/DC:
 * Decision: `file->offset >= valid` in
 * `libs/ra8_fs/src/ra8_fs_fat_fileio.c@priv_read_span` (1 condition).
 * - offset below the prefix -> false -> the media is read (bytes are 0xC7)
 * - offset at or past it    -> true  -> zeros are synthesised
 * The paired decision `want > cap` in the same function is driven by the same
 * read: the span that starts below the prefix and would cross it is clipped to
 * the prefix, so the next span takes the zero arm.
 *
 * @since 0.1.0
 */
static void test_stream_read_past_valid_is_zero(void)
{
  TEST_BEGIN("exfat stream: reads past ValidDataLength are zeros");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  make_partly_valid_file(h);

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "VDL.BIN", k_ra8_fs_mode_read, &f));
  uint64_t size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(f, &size));
  TEST_ASSERT_EQ(k_xs_multi_cluster, size);

  static uint8_t s_whole[k_xs_multi_cluster];
  uint32_t       got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, s_whole, (uint32_t)k_xs_multi_cluster, &got));
  TEST_ASSERT_EQ(k_xs_multi_cluster, got);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  for (uint32_t i = 0U; i < (uint32_t)k_xs_multi_cluster; i++) {
    const uint8_t want = (i < (uint32_t)k_xsv_valid_len) ? (uint8_t)k_xsv_residue : 0U;
    if (s_whole[i] != want) {
      TEST_FAIL_FMT("byte %u should be 0x%02X", (unsigned)i, (unsigned)want);
      break;
    }
  }

  stream_dump_image("stream_read_past_valid");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat stream: reads past ValidDataLength are zeros");
}

/**
 * @brief Read `VDL.BIN` whole and assert its three regions.
 *
 * @details Residue below the old prefix, real zeros across the gap the append
 *          had to fill, and the appended stream past the old length. Split out
 *          of the test so neither stays over the statement budget.
 *
 * @param[in,out] h     Mounted exFAT volume.
 * @param[in]     total The file's length after the append.
 *
 * @return Nothing; every check is asserted inside.
 *
 * @pre @p h is mounted and `VDL.BIN` exists at @p total bytes.
 * @pre No handle is open on it.
 * @post The file is closed again.
 * @post No on-disk state is modified.
 *
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0
 */
static void expect_three_regions(ra8_fs_mount_t* h, uint32_t total)
{
  ra8_fs_file_t* r = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "VDL.BIN", k_ra8_fs_mode_read, &r));
  static uint8_t s_whole[k_xs_multi_cluster + k_xsv_append_len];
  uint32_t       got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(r, s_whole, total, &got));
  TEST_ASSERT_EQ(total, got);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(r));
  for (uint32_t i = 0U; i < total; i++) {
    uint8_t want = 0U; /* the gap the append had to fill */
    if (i < (uint32_t)k_xsv_valid_len) {
      want = (uint8_t)k_xsv_residue;
    } else if (i >= (uint32_t)k_xs_multi_cluster) {
      want = stream_byte_at(i, (uint8_t)k_xsv_tail_seed);
    }
    if (s_whole[i] != want) {
      TEST_FAIL_FMT("byte %u should be 0x%02X", (unsigned)i, (unsigned)want);
      break;
    }
  }
}

/**
 * @test test_stream_append_fills_the_gap
 *
 * @brief Appending past the prefix writes real zeros over the gap first.
 *
 * @details An append parks at `DataLength`, which is past `ValidDataLength`, so
 *          the moment a byte lands there the gap below it can no longer be
 *          described: the format records unwritten data only as a PREFIX. The
 *          driver therefore has to write the zeros a reader is entitled to see,
 *          and the two lengths have to come out equal afterwards.
 *
 *          The check is made twice over: once through the API (the file reads
 *          back as residue, then zeros, then the appended stream) and once
 *          against the image (`ValidDataLength == DataLength`). A driver that
 *          only raised `ValidDataLength` without writing anything passes the
 *          second and fails the first, because the media still holds 0xC7.
 *
 * @par MC/DC:
 * Decision: `file->valid_bytes < file->offset` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_stream.c@priv_exfat_close_gap`
 * (1 condition).
 * - append onto a partly-valid file -> true  -> the gap is zero-filled
 * - every ordinary sequential write -> false -> the loop does nothing at all
 * The second vector is supplied by every other streaming case; this one is the
 * only route to the first.
 *
 * @since 0.1.0
 */
static void test_stream_append_fills_the_gap(void)
{
  TEST_BEGIN("exfat stream: append past the prefix zero-fills the gap");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  make_partly_valid_file(h);

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "VDL.BIN", k_ra8_fs_mode_append, &f));
  stream_write_pattern(f,
                       (uint32_t)k_xsv_append_len,
                       (uint32_t)k_xs_chunk,
                       (uint32_t)k_xs_multi_cluster,
                       (uint8_t)k_xsv_tail_seed);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  const uint32_t total = (uint32_t)k_xs_multi_cluster + (uint32_t)k_xsv_append_len;
  const uint32_t strm  = stream_strm0_off(h);
  TEST_ASSERT_EQ(total, disk_get_u32le(strm + (uint32_t)k_xs_off_strm_dlen));
  TEST_ASSERT_EQ(total, disk_get_u32le(strm + (uint32_t)k_xs_off_strm_valid));

  expect_three_regions(h, total);

  stream_dump_image("stream_append_fills_the_gap");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat stream: append past the prefix zero-fills the gap");
}

/**
 * @test test_stream_inverted_lengths_clamped
 *
 * @brief A `ValidDataLength` longer than the file is clamped, not written back.
 *
 * @details `ValidDataLength` is a PREFIX of `DataLength` (exFAT spec sec
 *          7.4.5), so a set claiming more valid bytes than the file has is not
 *          describing anything. Believing it would have the next flush write
 *          the inversion straight back to the card, where a host checker sees
 *          it -- and would let a read serve cluster residue past the end of a
 *          file as though it had been written. The open clamps instead, and
 *          the flush that follows leaves the two lengths equal.
 *
 * @par MC/DC:
 * Decision: `file->valid_bytes > file->size_bytes` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_openw.c@priv_exfat_open_found`
 * (1 condition). This case is the TRUE vector; every well-formed set is the
 * FALSE one.
 *
 * @since 0.1.0
 */
static void test_stream_inverted_lengths_clamped(void)
{
  TEST_BEGIN("exfat stream: an inverted ValidDataLength is clamped");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "INV.BIN", k_ra8_fs_mode_write, &f));
  stream_write_pattern(f, k_xs_sub_sector, k_xs_chunk, 0U, k_xs_seed_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  const uint32_t strm = stream_strm0_off(h);
  disk_set_u32le(strm + (uint32_t)k_xs_off_strm_valid, (uint32_t)k_xs_multi_cluster);
  repair_checksum(h);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "INV.BIN", k_ra8_fs_mode_append, &f));
  const uint8_t one = 'c';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, &one, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  const uint32_t total = (uint32_t)k_xs_sub_sector + 1U;
  TEST_ASSERT_EQ(total, disk_get_u32le(strm + (uint32_t)k_xs_off_strm_dlen));
  TEST_ASSERT_EQ(total, disk_get_u32le(strm + (uint32_t)k_xs_off_strm_valid));

  stream_dump_image("stream_inverted_lengths_clamped");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat stream: an inverted ValidDataLength is clamped");
}

/**
 * @test test_stream_oversized_set_refused
 *
 * @brief An entry set larger than this adapter rewrites is refused, not mangled.
 *
 * @details The flush rewrites a set through a fixed buffer, so a set carrying
 *          more Name entries than the adapter's name cap needs cannot be
 *          checksummed correctly -- the checksum covers a count of entries the
 *          buffer cannot hold. Refusing at open is the honest answer;
 *          rewriting the part that fits would leave a set a host rejects. A
 *          READ of the same file is unaffected and must still work, because
 *          nothing about reading needs the set rewritten.
 *
 * @par MC/DC:
 * Decision: `count > k_exfat_set_writable` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_openw.c@priv_exfat_writable_set`
 * (1 condition). This case is the TRUE vector; every other streaming case is
 * the FALSE one.
 *
 * @since 0.1.0
 */
static void test_stream_oversized_set_refused(void)
{
  TEST_BEGIN("exfat stream: an unrewritable entry set is refused");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "WIDE.BIN", k_ra8_fs_mode_write, &f));
  stream_write_pattern(f,
                       (uint32_t)k_xs_sub_sector,
                       (uint32_t)k_xs_chunk,
                       0U,
                       (uint8_t)k_xs_seed_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* Dump BEFORE the patch: what follows is a hand-built fixture, and only the
   * driver's own output belongs in the `fsck.exfat -n` evidence. */
  stream_dump_image("stream_oversized_set_refused");

  /* Widen the set: raise SecondaryCount and fill the extra slots with real
   * Name entries. A Name entry whose characters all sit past NameLength still
   * matches -- the comparator stops at the name's end -- so this is a set the
   * lookup ACCEPTS and the writable-set guard is the only thing standing
   * between it and a rewrite through a buffer too small to hold it. */
  const uint32_t file_off = root_byte(h, (uint32_t)k_mut_root_file0_idx);
  s_disk.bytes[file_off + (uint32_t)k_mut_file_secnt_off] = (uint8_t)k_xsv_big_secnt;
  for (uint32_t i = (uint32_t)k_mut_root_name0_idx + 1U;
       i <= ((uint32_t)k_mut_root_file0_idx + (uint32_t)k_xsv_big_secnt);
       i++) {
    s_disk.bytes[root_byte(h, i)] = (uint8_t)k_mut_type_name;
  }

  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_fs_open(h, "WIDE.BIN", k_ra8_fs_mode_append, &f));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_fs_open(h, "WIDE.BIN", k_ra8_fs_mode_write, &f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat stream: an unrewritable entry set is refused");
}

/**
 * @test test_stream_over_4gib_accepted
 *
 * @brief A file above 4 GiB opens for writing -- the length model is 64-bit.
 *
 * @details The old adapter refused a `DataLength` whose high word was non-zero
 *          because every handle length was 32 bits and the first flush would
 *          have written the LOW word back. The length model is 64-bit now
 *          (#676): the same fixture opens, reports its real size, and a
 *          truncate DOWN through the 4 GiB boundary lands exactly where it was
 *          told to. The volume behind the fixture is far smaller than the
 *          claimed length, so nothing is read or written through the fictional
 *          clusters -- only the metadata paths run, which is precisely what
 *          this case pins.
 *
 * @par MC/DC:
 * (no compound decision -- the old one-condition guard in
 * `priv_exfat_writable_set` is deleted; this is the behavioral pin that the
 * high word round-trips instead of being refused)
 *
 * @since 0.1.0
 */
static void test_stream_over_4gib_accepted(void)
{
  TEST_BEGIN("exfat stream: a file above 4 GiB opens and reports its real size");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "HUGE.BIN", k_ra8_fs_mode_write, &f));
  stream_write_pattern(f,
                       (uint32_t)k_xs_sub_sector,
                       (uint32_t)k_xs_chunk,
                       0U,
                       (uint8_t)k_xs_seed_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  stream_dump_image("stream_over_4gib_accepted");

  /* A 4 GiB+ length as another implementation would record it. */
  const uint32_t strm = stream_strm0_off(h);
  disk_set_u32le(strm + (uint32_t)k_xs_off_dlen_hi, (uint32_t)k_xsv_over_4gib);
  repair_checksum(h);

  /* Read-open: the full 64-bit DataLength comes back. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "HUGE.BIN", k_ra8_fs_mode_read, &f));
  uint64_t size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(f, &size));
  TEST_ASSERT_EQ(((uint64_t)k_xsv_over_4gib << 32U) | (uint64_t)k_xs_sub_sector, size);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* Append-open (the survey runs); then truncate back below 4 GiB, through
   * the boundary, and confirm the recorded length followed. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "HUGE.BIN", k_ra8_fs_mode_append, &f));
  uint64_t pos = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_tell(f, &pos));
  TEST_ASSERT_EQ(size, pos);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_truncate(f, (uint64_t)k_xs_sub_sector));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(f, &size));
  TEST_ASSERT_EQ(k_xs_sub_sector, size);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat stream: a file above 4 GiB opens and reports its real size");
}

/**
 * @test test_stream_write_over_directory_refused
 *
 * @brief Opening a directory for writing is refused before the truncate.
 *
 * @details A directory answers an exFAT name lookup exactly like a file, and
 *          its Stream entry describes the cluster chain holding its CONTENTS.
 *          Truncating it would hand those clusters back to the volume and
 *          orphan every file inside (#604). The guard runs before a file slot
 *          is even claimed, and the directory's entry must be untouched
 *          afterwards.
 *
 * @par MC/DC:
 * Decision: `(file_e[attr] & k_exfat_attr_directory) != 0` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_openw.c@priv_exfat_open_found`
 * (1 condition). This case is the TRUE vector; every other streaming case that
 * reopens a real file is the FALSE one.
 *
 * @since 0.1.0
 */
static void test_stream_write_over_directory_refused(void)
{
  TEST_BEGIN("exfat stream: open(write) on a directory is refused");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "FOLDER", k_ra8_fs_mode_write, &f));
  stream_write_pattern(f,
                       (uint32_t)k_xs_sub_sector,
                       (uint32_t)k_xs_chunk,
                       0U,
                       (uint8_t)k_xs_seed_a);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  stream_dump_image("stream_write_over_directory_refused");
  /* There is no exFAT mkdir, so a directory is presented by flipping the
   * attribute bit on a set the driver wrote -- a fixture, hence the dump above
   * rather than below. */
  mark_first_file_as_directory(h);

  const uint32_t strm   = stream_strm0_off(h);
  const uint32_t before = disk_get_u32le(strm + (uint32_t)k_xs_off_strm_dlen);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_open(h, "FOLDER", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_open(h, "FOLDER", k_ra8_fs_mode_append, &f));
  TEST_ASSERT_EQ(before, disk_get_u32le(strm + (uint32_t)k_xs_off_strm_dlen));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat stream: open(write) on a directory is refused");
}

/**
 * @brief Run every exFAT ValidDataLength / refusal test.
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
  test_stream_read_past_valid_is_zero();
  test_stream_append_fills_the_gap();
  test_stream_inverted_lengths_clamped();
  test_stream_oversized_set_refused();
  test_stream_over_4gib_accepted();
  test_stream_write_over_directory_refused();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_exfat_stream_vdl.c\n");
  return 0;
}
