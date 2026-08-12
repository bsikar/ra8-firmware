/**
 * @file test_ra8_fs_exfat_stream_cov.c
 * @brief Backend-failure arms of the exFAT streaming writer (#602).
 *
 * @details
 * A storage driver is mostly error handling, and the arms that carry it are
 * the ones a happy-path test never reaches. Every `if (e != k_ra8_ok) return
 * e;` in the streaming writer is a place where a card that stopped answering
 * has to produce an error rather than a half-written file, so each one is
 * driven here from a real backend failure.
 *
 * Faults are aimed by REGION -- FAT, allocation bitmap, root directory, data
 * heap -- rather than by counting block operations, so a case says what it
 * means ("the bitmap write failed") and survives any reordering of the I/O
 * around it. See `tests/support/fs_exfat_stream_fault_test_util.h`.
 *
 * Two things every case asserts beyond the error code: the call did not
 * SUCCEED quietly, and the mount is still usable afterwards. A driver that
 * leaks a file-table slot or leaves the library wedged on a transient card
 * error has failed differently, not better.
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
#include "support/fs_exfat_stream_fault_test_util.h"
#include "unity_minimal.h"

/**
 * @enum xsf_const_t
 * @brief Offsets, sizes and expectations for the fault-injection cases.
 *
 * @invariant `k_xsf_cross_len` exceeds one 4 KiB cluster.
 *
 * @par Example:
 * @code
 * TEST_ASSERT_EQ(k_xsf_slots, opened);
 * @endcode
 *
 * @see test_grow_bitmap_mark_write_fails()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_xsf_cross_len   = 4200U,       /**< Payload that crosses a cluster edge.       */
  k_xsf_slots       = 4U,          /**< Concurrent handles the file table allows.  */
  k_xsf_root_first  = 3U,          /**< Root slot of the first user File entry.    */
  k_xsf_root_strm   = 4U,          /**< Root slot of its Stream entry.             */
  k_xsf_root_end    = 128U,        /**< Entries in one 4 KiB root cluster.         */
  k_xsf_type_name   = 0xC1U,       /**< Name entry type: in-use, not a File set.   */
  k_xsf_type_eod    = 0x00U,       /**< End-of-directory type byte.                */
  k_xsf_off_clus    = 0x14U,       /**< Stream-ext FirstCluster offset.            */
  k_xsf_off_dlen    = 0x18U,       /**< Stream-ext DataLength offset.              */
  k_xsf_bad_cluster = 1U,          /**< A cluster number below the first data one. */
  k_xsf_fat_eoc     = 0xFFFFFFFFU, /**< exFAT end-of-chain marker.                 */
  k_xsf_byte_mask   = 0xFFU,       /**< Low byte of a packed word.                 */
  k_xsf_shift_byte  = 8U,          /**< Byte position inside a word.               */
  k_xsf_shift_two   = 16U,         /**< Two-byte position inside a word.           */
  k_xsf_shift_three = 24U,         /**< Three-byte position inside a word.         */
  k_xsf_fill_a      = 0x5AU,       /**< Filler byte for the growth cases.          */
  k_xsf_fill_b      = 0x33U,       /**< Filler byte for the transition case.       */
  k_xsf_fill_c      = 0x77U,       /**< Filler byte for the data-sector cases.     */
  k_xsf_fill_d      = 0x21U,       /**< Filler byte for the survey case.           */
  k_xsf_fill_e      = 0x44U,       /**< Filler byte for the chain-walk case.       */
} xsf_const_t;

/**
 * @brief Absolute byte offset of root-directory entry @p idx.
 *
 * @param[in] idx Entry index within the root cluster.
 *
 * @return Byte offset into the fixture's image.
 * @retval >0 The address of that entry's first byte.
 *
 * @pre `flt_bind_geometry()` has run.
 * @pre @p idx is below ::k_xsf_root_end.
 * @post No state is modified.
 * @post The offset lies inside the root cluster.
 *
 * @note Pure with respect to the fixture state.
 * @since 0.1.0
 */
static uint32_t root_at(uint32_t idx)
{
  return (s_flt.dir_lba * (uint32_t)k_flt_block_size) + (idx * (uint32_t)k_flt_entry);
}

/**
 * @brief Absolute byte offset of cluster @p clus's FAT entry.
 *
 * @param[in] clus Cluster number.
 *
 * @return Byte offset into the fixture's image.
 * @retval >0 The address of the 4-byte FAT entry.
 *
 * @pre `flt_bind_geometry()` has run.
 * @pre @p clus addresses a cluster this volume has.
 * @post No state is modified.
 * @post The offset lies inside the FAT.
 *
 * @note Pure with respect to the fixture state.
 * @since 0.1.0
 */
static uint32_t fat_at(uint32_t clus)
{
  return (s_flt.fat_lba * (uint32_t)k_flt_block_size) + (clus * 4U);
}

/**
 * @brief Write a 32-bit little-endian value into the image.
 *
 * @param[in] off Absolute byte offset.
 * @param[in] val Value to store.
 *
 * @return Nothing.
 *
 * @pre The image is allocated and `off + 4` is inside it.
 * @pre The volume is not mid-operation.
 * @post The four bytes at @p off encode @p val.
 * @post No other byte changes.
 *
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0
 */
static void poke32(uint32_t off, uint32_t val)
{
  uint8_t* p = &s_flt.bytes[off];
  p[0]       = (uint8_t)(val & (uint32_t)k_xsf_byte_mask);
  p[1]       = (uint8_t)((val >> (uint32_t)k_xsf_shift_byte) & (uint32_t)k_xsf_byte_mask);
  p[2]       = (uint8_t)((val >> (uint32_t)k_xsf_shift_two) & (uint32_t)k_xsf_byte_mask);
  p[3]       = (uint8_t)((val >> (uint32_t)k_xsf_shift_three) & (uint32_t)k_xsf_byte_mask);
}

/**
 * @brief Read a 32-bit little-endian value out of the image.
 *
 * @param[in] off Absolute byte offset.
 *
 * @return The decoded value.
 * @retval 0..0xFFFFFFFF The stored little-endian word.
 *
 * @pre The image is allocated and `off + 4` is inside it.
 * @pre The volume is not mid-operation.
 * @post No state is modified.
 * @post The result depends only on the image bytes.
 *
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0
 */
static uint32_t peek32(uint32_t off)
{
  const uint8_t* p = &s_flt.bytes[off];
  return (uint32_t)p[0] | ((uint32_t)p[1] << (uint32_t)k_xsf_shift_byte) |
         ((uint32_t)p[2] << (uint32_t)k_xsf_shift_two) |
         ((uint32_t)p[3] << (uint32_t)k_xsf_shift_three);
}

/**
 * @brief Create two adjacent one-cluster files so the first cannot grow in place.
 *
 * @details `A.BIN` takes a cluster; `B.BIN` takes the next one, because the
 *          allocator hands out the lowest free cluster. Appending to `A.BIN`
 *          after this is guaranteed to break contiguity, which is what every
 *          FAT-chain case here needs.
 *
 * @param[in,out] h Mounted exFAT volume with an empty root.
 *
 * @return The cluster `A.BIN` starts at.
 * @retval >=2 The first data cluster of `A.BIN`.
 *
 * @pre @p h is mounted and the root directory is empty.
 * @pre Neither name exists.
 * @post `A.BIN` and `B.BIN` occupy consecutive clusters.
 * @post Both are closed.
 *
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0
 */
static uint32_t make_adjacent_pair(ra8_fs_mount_t* h)
{
  flt_make_file(h, "A.BIN", (uint32_t)k_flt_small);
  flt_make_file(h, "B.BIN", (uint32_t)k_flt_small);
  return peek32(root_at((uint32_t)k_xsf_root_strm) + (uint32_t)k_xsf_off_clus);
}

/**
 * @test test_grow_bitmap_lookup_read_fails
 *
 * @brief A card that cannot be read while the bitmap is being located fails the write.
 *
 * @details The bitmap's location lives in a root-directory entry, so the first
 *          growth of the first file on a fresh mount has to walk the directory
 *          to find it. Faulting a directory read at that moment is the only
 *          route to the error arm in ::priv_exfat_bitmap_lba and the one
 *          above it in the grow path -- every later growth is served from the
 *          cache and never reads anything.
 *
 * @par MC/DC:
 * Decision: `if (e != k_ra8_ok)` after ::priv_exfat_bitmap_lba in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_stream.c@priv_exfat_grow_one`
 * (1 condition). This case is the TRUE vector; every successful write is the
 * FALSE one.
 *
 * @since 0.1.0
 */
static void test_grow_bitmap_lookup_read_fails(void)
{
  TEST_BEGIN("exfat stream cov: bitmap lookup read failure");
  flt_build_volume();
  ra8_fs_mount_t* h = nullptr;
  flt_mount(&h);

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "X.BIN", k_ra8_fs_mode_write, &f));
  const uint8_t one = 'x';
  flt_arm_read(k_flt_region_dir);
  TEST_ASSERT(ra8_fs_write(f, &one, 1U) != k_ra8_ok);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  flt_free_volume();
  TEST_END("exfat stream cov: bitmap lookup read failure");
}

/**
 * @test test_grow_bitmap_probe_read_fails
 *
 * @brief A failed contiguity probe fails the growth rather than guessing.
 *
 * @details The probe that keeps a run contiguous is a bitmap read, and it
 *          happens before anything is allocated. A driver that treated an
 *          unreadable bit as "not free" would silently fragment a file on a
 *          flaky card; this pins the honest answer -- report the read failure.
 *
 * @par MC/DC:
 * Decision: `if (pe != k_ra8_ok)` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_stream.c@priv_exfat_pick_cluster`
 * (1 condition). TRUE here; FALSE on every growth that reads its bit.
 *
 * @since 0.1.0
 */
static void test_grow_bitmap_probe_read_fails(void)
{
  TEST_BEGIN("exfat stream cov: contiguity probe read failure");
  flt_build_volume();
  ra8_fs_mount_t* h = nullptr;
  flt_mount(&h);
  flt_make_file(h, "A.BIN", (uint32_t)k_flt_small);

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_append, &f));
  static uint8_t s_buf[k_flt_payload];
  memset(s_buf, (int)k_xsf_fill_a, sizeof s_buf);
  flt_arm_read(k_flt_region_bitmap);
  TEST_ASSERT(ra8_fs_write(f, s_buf, (uint32_t)k_xsf_cross_len) != k_ra8_ok);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  flt_free_volume();
  TEST_END("exfat stream cov: contiguity probe read failure");
}

/**
 * @test test_grow_bitmap_mark_write_fails
 *
 * @brief A bitmap that cannot be marked fails the growth before the FAT is touched.
 *
 * @details Marking is what makes a cluster owned, and it happens FIRST for
 *          exactly this reason: a failure here leaks a cluster that nothing
 *          references, which a later `fsck` reclaims. The reverse order would
 *          leave a file pointing at a cluster the volume still calls free.
 *
 * @par MC/DC:
 * Decision: `if (e != k_ra8_ok)` after ::priv_exfat_bitmap_mark in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_stream.c@priv_exfat_grow_one`
 * (1 condition). TRUE here; FALSE on every successful growth.
 *
 * @since 0.1.0
 */
static void test_grow_bitmap_mark_write_fails(void)
{
  TEST_BEGIN("exfat stream cov: bitmap mark write failure");
  flt_build_volume();
  ra8_fs_mount_t* h = nullptr;
  flt_mount(&h);
  flt_make_file(h, "A.BIN", (uint32_t)k_flt_small);

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_append, &f));
  static uint8_t s_buf[k_flt_payload];
  memset(s_buf, (int)k_xsf_fill_a, sizeof s_buf);
  flt_arm_write(k_flt_region_bitmap);
  TEST_ASSERT(ra8_fs_write(f, s_buf, (uint32_t)k_xsf_cross_len) != k_ra8_ok);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  flt_free_volume();
  TEST_END("exfat stream cov: bitmap mark write failure");
}

/**
 * @test test_chain_materialize_fat_write_fails
 *
 * @brief A FAT that cannot be written leaves NoFatChain set, not half-cleared.
 *
 * @details The transition writes the chain first and clears the flag second,
 *          so a FAT write failure has to leave the entry set still claiming a
 *          contiguous run -- which it still has, because the failing write was
 *          the one that would have extended it. The flags byte is read back
 *          out of the image to prove the flag really did not move.
 *
 * @par MC/DC:
 * Decision: `if (e != k_ra8_ok)` after ::priv_exfat_materialize_chain in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_stream.c@priv_exfat_link_cluster`
 * (1 condition). TRUE here; FALSE in the fragmentation case that succeeds.
 *
 * @since 0.1.0
 */
static void test_chain_materialize_fat_write_fails(void)
{
  TEST_BEGIN("exfat stream cov: FAT write failure during the transition");
  flt_build_volume();
  ra8_fs_mount_t* h = nullptr;
  flt_mount(&h);
  (void)make_adjacent_pair(h);

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_append, &f));
  static uint8_t s_buf[k_flt_payload];
  memset(s_buf, (int)k_xsf_fill_b, sizeof s_buf);
  flt_arm_write(k_flt_region_fat);
  TEST_ASSERT(ra8_fs_write(f, s_buf, (uint32_t)k_xsf_cross_len) != k_ra8_ok);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  flt_free_volume();
  TEST_END("exfat stream cov: FAT write failure during the transition");
}

/**
 * @test test_data_sector_io_fails
 *
 * @brief Both halves of the data read-modify-write report their failure.
 *
 * @details A partial-sector write reads the sector first so the neighbouring
 *          bytes survive, so there are two ways for the media to refuse it and
 *          both have to surface. The file is reopened between the two arms
 *          because each is one-shot.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` twice in
 * `libs/ra8_fs/src/ra8_fs_fat_fileio.c@priv_write_into_sector` (1 condition
 * each). This case supplies both TRUE vectors; every successful write supplies
 * the FALSE ones.
 *
 * @since 0.1.0
 */
static void test_data_sector_io_fails(void)
{
  TEST_BEGIN("exfat stream cov: data sector read and write failures");
  flt_build_volume();
  ra8_fs_mount_t* h = nullptr;
  flt_mount(&h);
  flt_make_file(h, "A.BIN", (uint32_t)k_flt_small);

  static uint8_t s_buf[k_flt_payload];
  memset(s_buf, (int)k_xsf_fill_c, sizeof s_buf);
  ra8_fs_file_t* f = nullptr;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_append, &f));
  flt_arm_read(k_flt_region_data);
  TEST_ASSERT(ra8_fs_write(f, s_buf, (uint32_t)k_flt_small) != k_ra8_ok);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_append, &f));
  flt_arm_write(k_flt_region_data);
  TEST_ASSERT(ra8_fs_write(f, s_buf, (uint32_t)k_flt_small) != k_ra8_ok);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  flt_free_volume();
  TEST_END("exfat stream cov: data sector read and write failures");
}

/**
 * @test test_flush_set_io_fails
 *
 * @brief A directory that cannot be re-read or rewritten fails the flush.
 *
 * @details The flush reads the entry set back before patching it, so both the
 *          gather and the write-back can fail. Either way the bytes are
 *          already on the volume and only the recorded length is behind, which
 *          is the safe direction: a reader sees a shorter file, never a longer
 *          one claiming bytes that were never stored.
 *
 * @par MC/DC:
 * Decisions: `if (e != k_ra8_ok)` after ::priv_exfat_gather_set and after the
 * File-entry write in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_stream.c@priv_exfat_flush_set`
 * (1 condition each). Both TRUE vectors here; the FALSE ones come from every
 * successful write.
 *
 * @since 0.1.0
 */
static void test_flush_set_io_fails(void)
{
  TEST_BEGIN("exfat stream cov: entry-set flush read and write failures");
  flt_build_volume();
  ra8_fs_mount_t* h = nullptr;
  flt_mount(&h);
  flt_make_file(h, "A.BIN", (uint32_t)k_flt_small);

  const uint8_t  one = 'q';
  ra8_fs_file_t* f   = nullptr;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_append, &f));
  flt_arm_read(k_flt_region_dir);
  TEST_ASSERT(ra8_fs_write(f, &one, 1U) != k_ra8_ok);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_append, &f));
  flt_arm_write(k_flt_region_dir);
  TEST_ASSERT(ra8_fs_write(f, &one, 1U) != k_ra8_ok);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  flt_free_volume();
  TEST_END("exfat stream cov: entry-set flush read and write failures");
}

/**
 * @test test_open_lookup_read_fails
 *
 * @brief A directory read failure at open is reported, not turned into a create.
 *
 * @details ::priv_exfat_open_write treats `k_ra8_err_not_found` as "create it"
 *          and everything else as a failure. Conflating the two would turn an
 *          unreadable card into a second entry set for a name that already
 *          exists -- the duplication #603 fixed, reintroduced through the
 *          error path.
 *
 * @par MC/DC:
 * Decision: `if (e != k_ra8_err_not_found)` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_openw.c@priv_exfat_open_write`
 * (1 condition). TRUE here (a backend error); FALSE when the name is genuinely
 * absent and the create runs.
 *
 * @since 0.1.0
 */
static void test_open_lookup_read_fails(void)
{
  TEST_BEGIN("exfat stream cov: open lookup read failure");
  flt_build_volume();
  ra8_fs_mount_t* h = nullptr;
  flt_mount(&h);
  flt_make_file(h, "A.BIN", (uint32_t)k_flt_small);

  ra8_fs_file_t* f = nullptr;
  flt_arm_read(k_flt_region_dir);
  TEST_ASSERT(ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_write, &f) != k_ra8_ok);

  /* The slot must not have been consumed by the refused open. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  flt_free_volume();
  TEST_END("exfat stream cov: open lookup read failure");
}

/**
 * @test test_truncate_free_write_fails
 *
 * @brief A truncate that cannot free its clusters fails and releases its slot.
 *
 * @details Write mode frees the old allocation before it rewrites the entry
 *          set. When the bitmap refuses the write the open has to fail -- and
 *          give back the file-table slot it claimed, or four such failures
 *          would exhaust a four-slot table with nothing open.
 *
 * @par MC/DC:
 * Decision: `if (e != k_ra8_ok)` after the mode branch in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_openw.c@priv_exfat_open_found`
 * (1 condition). TRUE here; FALSE on every successful truncate or append.
 *
 * @since 0.1.0
 */
static void test_truncate_free_write_fails(void)
{
  TEST_BEGIN("exfat stream cov: truncate bitmap write failure");
  flt_build_volume();
  ra8_fs_mount_t* h = nullptr;
  flt_mount(&h);
  flt_make_file(h, "A.BIN", (uint32_t)k_xsf_cross_len);

  ra8_fs_file_t* f = nullptr;
  flt_arm_write(k_flt_region_bitmap);
  TEST_ASSERT(ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_write, &f) != k_ra8_ok);

  /* Four more opens must still succeed: the refused one released its slot. */
  ra8_fs_file_t* held[k_xsf_slots] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_xsf_slots; i++) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_read, &held[i]));
  }
  for (uint32_t i = 0U; i < (uint32_t)k_xsf_slots; i++) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(held[i]));
  }

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  flt_free_volume();
  TEST_END("exfat stream cov: truncate bitmap write failure");
}

/**
 * @test test_create_file_table_full
 *
 * @brief A create with no free handle leaves the directory untouched.
 *
 * @details The slot is claimed BEFORE the entry set is written, precisely so
 *          this case cannot leave a real, empty file behind on a call that
 *          returned an error. The listing count afterwards is the check that
 *          the ordering is the one described.
 *
 * @par MC/DC:
 * Decision: `if (f == nullptr)` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_openw.c@priv_exfat_open_created`
 * (1 condition). TRUE here; FALSE on every create that has a slot.
 *
 * @since 0.1.0
 */
static void test_create_file_table_full(void)
{
  TEST_BEGIN("exfat stream cov: create with a full file table");
  flt_build_volume();
  ra8_fs_mount_t* h = nullptr;
  flt_mount(&h);
  flt_make_file(h, "A.BIN", (uint32_t)k_flt_small);

  ra8_fs_file_t* held[k_xsf_slots] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_xsf_slots; i++) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_read, &held[i]));
  }
  ra8_fs_file_t* extra = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_open(h, "NEW.BIN", k_ra8_fs_mode_write, &extra));
  for (uint32_t i = 0U; i < (uint32_t)k_xsf_slots; i++) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(held[i]));
  }

  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_stat(h, "NEW.BIN", &st));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  flt_free_volume();
  TEST_END("exfat stream cov: create with a full file table");
}

/**
 * @test test_create_no_directory_space
 *
 * @brief A root directory with no room GROWS to hold the new create (#677).
 *
 * @details Every slot in the single root cluster is filled with an in-use Name
 *          entry, which occupies the slot without pretending to be a File set,
 *          so the lookup still reports the name absent and the create still has
 *          to find three consecutive free entries. There are none in the first
 *          cluster, so ::priv_exfat_link grows the root and lays the set down in
 *          the fresh one -- before #677 this refused with `k_ra8_err_no_mem`, its
 *          FAT chain being end-of-chain with nowhere to extend to.
 *
 * @par MC/DC:
 * Decision: `if (e != k_ra8_ok)` after ::priv_exfat_link in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_openw.c@priv_exfat_open_created`
 * (1 condition). FALSE here -- the grow succeeds; the TRUE arm (link fails
 * because the VOLUME is full) is driven by
 * tests/test_ra8_fs_exfat_dir_growth.c@test_volume_full_reports_no_mem.
 *
 * @since 0.1.0
 */
static void test_create_no_directory_space(void)
{
  TEST_BEGIN("exfat stream cov: a full root grows to hold a create");
  flt_build_volume();
  ra8_fs_mount_t* h = nullptr;
  flt_mount(&h);

  for (uint32_t i = (uint32_t)k_xsf_root_first; i < (uint32_t)k_xsf_root_end; i++) {
    s_flt.bytes[root_at(i)] = (uint8_t)k_xsf_type_name;
  }

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "NEW.BIN", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  flt_free_volume();
  TEST_END("exfat stream cov: a full root grows to hold a create");
}

/**
 * @test test_survey_rejects_broken_chain
 *
 * @brief A chain pointing below the first data cluster is refused at open.
 *
 * @details The survey walks the FAT to find a chained file's tail, and a
 *          corrupt entry pointing at cluster 0 or 1 -- neither of which exists
 *          -- would otherwise be followed straight into the FAT's own reserved
 *          words. It is reported as a protocol error instead.
 *
 * @par MC/DC:
 * Decision: `if (next < k_cluster_first_data)` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_openw.c@priv_exfat_survey_alloc`
 * (1 condition). TRUE here; FALSE on every well-formed chain.
 *
 * @since 0.1.0
 */
static void test_survey_rejects_broken_chain(void)
{
  TEST_BEGIN("exfat stream cov: survey refuses a chain below cluster 2");
  flt_build_volume();
  ra8_fs_mount_t* h = nullptr;
  flt_mount(&h);
  const uint32_t first = make_adjacent_pair(h);

  /* Force A.BIN to fragment, so it becomes FAT-chained and the survey walks. */
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_append, &f));
  static uint8_t s_buf[k_flt_payload];
  memset(s_buf, (int)k_xsf_fill_d, sizeof s_buf);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, s_buf, (uint32_t)k_xsf_cross_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* Poke the FAT under an UNMOUNTED volume: the driver reads the FAT through a
   * one-sector write-through cache, so a poke made behind a live mount would
   * simply not be seen. Remounting drops the cache with the mount. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  poke32(fat_at(first), (uint32_t)k_xsf_bad_cluster);
  flt_mount(&h);
  TEST_ASSERT_EQ(k_ra8_err_protocol_error, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_append, &f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  flt_free_volume();
  TEST_END("exfat stream cov: survey refuses a chain below cluster 2");
}

/**
 * @test test_survey_zero_length_with_cluster
 *
 * @brief A zero-length file still holding a cluster number is surveyed as empty.
 *
 * @details exFAT spec sec 7.4.4 forbids the combination, and another
 *          implementation can still leave it. Without the length half of the
 *          survey's guard the contiguous branch computes a cluster count of 0
 *          and a tail ONE BELOW the first cluster -- and the next growth
 *          probes, and can take, a cluster belonging to somebody else.
 *
 * @par MC/DC:
 * Decision: `(first_cluster < k_cluster_first_data) || (size_bytes == 0)` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_openw.c@priv_exfat_survey_alloc`
 * (2 conditions). This case varies the SECOND: a real cluster with a zero
 * length. The first is varied by an ordinary empty file, and both false by
 * every append to a real one.
 *
 * @since 0.1.0
 */
static void test_survey_zero_length_with_cluster(void)
{
  TEST_BEGIN("exfat stream cov: zero length with a cluster still recorded");
  flt_build_volume();
  ra8_fs_mount_t* h = nullptr;
  flt_mount(&h);
  flt_make_file(h, "A.BIN", (uint32_t)k_flt_small);

  const uint32_t strm = root_at((uint32_t)k_xsf_root_strm);
  poke32(strm + (uint32_t)k_xsf_off_dlen, 0U);
  TEST_ASSERT(peek32(strm + (uint32_t)k_xsf_off_clus) >= 2U);

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_append, &f));
  uint64_t at = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_tell(f, &at));
  TEST_ASSERT_EQ(0U, at);
  const uint8_t one = 'z';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, &one, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* A fresh cluster was taken, and the recorded one is now that cluster. */
  TEST_ASSERT_EQ(1U, peek32(strm + (uint32_t)k_xsf_off_dlen));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  flt_free_volume();
  TEST_END("exfat stream cov: zero length with a cluster still recorded");
}

/**
 * @brief Move the shared FAT sector cache onto @p other, off the mount under test.
 *
 * @details The FAT is read through one write-through sector cache keyed on the
 *          MOUNT, so a fault injected -- or a byte poked -- behind a live mount
 *          that already read that sector is simply never seen. Reading `A.BIN`
 *          past its first cluster through a SECOND mount of the same volume
 *          walks the chain under a different owner, which is what displaces the
 *          entry.
 *
 * @param[in,out] other Second mount of the same volume.
 * @param[in]     deep  A byte offset inside the file's second cluster.
 *
 * @return Nothing; every step is asserted inside.
 *
 * @pre @p other is mounted and `A.BIN` exists with at least two clusters.
 * @pre @p deep is below the file's length.
 * @post The cached FAT sector belongs to @p other.
 * @post No on-disk state is modified.
 *
 * @note Not thread-safe; the fixture is single-threaded.
 * @since 0.1.0
 */
static void drop_fat_cache(ra8_fs_mount_t* other, uint32_t deep)
{
  ra8_fs_file_t* ef  = nullptr;
  uint8_t        one = 0U;
  uint32_t       got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(other, "A.BIN", k_ra8_fs_mode_read, &ef));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_seek(ef, deep));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(ef, &one, 1U, &got));
  TEST_ASSERT_EQ(1U, got);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(ef));
}

/**
 * @test test_survey_length_without_cluster
 *
 * @brief A length with no first cluster is surveyed as empty, not walked.
 *
 * @details The mirror of ::test_survey_zero_length_with_cluster, and the other
 *          half of the same guard. exFAT spec sec 7.4.4 pairs `FirstCluster` 0
 *          with `DataLength` 0; another implementation can leave a length
 *          behind on a file whose allocation was released, and following that
 *          into cluster 0 would walk the FAT's own reserved words.
 *
 * @par MC/DC:
 * Decision: `(first_cluster < k_cluster_first_data) || (size_bytes == 0)` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_openw.c@priv_exfat_survey_alloc`
 * (2 conditions). This case varies the FIRST: no cluster, but a non-zero
 * length. ::test_survey_zero_length_with_cluster varies the second, and every
 * append to a real file drives both false -- N+1 = 3 vectors for N = 2.
 *
 * @since 0.1.0
 */
static void test_survey_length_without_cluster(void)
{
  TEST_BEGIN("exfat stream cov: a length with no cluster is surveyed empty");
  flt_build_volume();
  ra8_fs_mount_t* h = nullptr;
  flt_mount(&h);

  const uint8_t nothing = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "E.BIN", &nothing, 0U));

  const uint32_t strm = root_at((uint32_t)k_xsf_root_strm);
  TEST_ASSERT_EQ(0U, peek32(strm + (uint32_t)k_xsf_off_clus));
  poke32(strm + (uint32_t)k_xsf_off_dlen, (uint32_t)k_flt_small);

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "E.BIN", k_ra8_fs_mode_append, &f));
  uint64_t at = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_tell(f, &at));
  TEST_ASSERT_EQ(k_flt_small, at);
  const uint8_t one = 'y';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, &one, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* The write allocated from scratch and the recorded length now covers the
   * zero-filled gap the append had to write plus the byte itself. */
  TEST_ASSERT_EQ(k_flt_small + 1U, peek32(strm + (uint32_t)k_xsf_off_dlen));
  TEST_ASSERT(peek32(strm + (uint32_t)k_xsf_off_clus) >= 2U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  flt_free_volume();
  TEST_END("exfat stream cov: a length with no cluster is surveyed empty");
}

/**
 * @test test_cluster_at_chain_ends_early
 *
 * @brief A chain that ends before the file does is an invalid state, not a guess.
 *
 * @details The entry set and the FAT can disagree -- a card that lost a FAT
 *          sector says so this way -- and the writer must not carry on into
 *          whatever cluster the walk stopped at, nor read past a FAT it cannot
 *          load. Both arms of the walk are driven: an unreadable FAT sector,
 *          and a chain that terminates early.
 *
 *          Breaking the chain takes some care, because the FAT is read through
 *          a one-sector write-through cache keyed on the MOUNT: a poke made
 *          behind the open handle would never be seen. A second mount of the
 *          same volume evicts the entry -- the cache is one sector shared by
 *          every mount -- so the next walk reads the poked FAT off the media.
 *
 * @par MC/DC:
 * Two single-condition decisions in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_stream.c@priv_exfat_cluster_at`:
 * `if (e != k_ra8_ok)` after the FAT read, and `if (priv_is_eoc(m, next) != 0)`.
 * Both TRUE here; both FALSE on every walk that reaches its index.
 *
 * @since 0.1.0
 */
static void test_cluster_at_chain_ends_early(void)
{
  TEST_BEGIN("exfat stream cov: the chain ends before the file does");
  flt_build_volume();
  ra8_fs_mount_t* h = nullptr;
  flt_mount(&h);
  const uint32_t first = make_adjacent_pair(h);

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_append, &f));
  static uint8_t s_buf[k_flt_payload];
  memset(s_buf, (int)k_xsf_fill_e, sizeof s_buf);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, s_buf, (uint32_t)k_xsf_cross_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* Both arms overwrite a byte in the SECOND cluster, so the walk really has a
   * hop to take: an overwrite inside the first cluster reads no FAT at all.
   * The second mount exists to move the shared one-sector FAT cache off `h`
   * before each arm -- without that the walk is served from memory and neither
   * an injected read failure nor a poked FAT entry is ever seen. */
  const uint8_t   one   = 'e';
  const uint32_t  deep  = (uint32_t)k_xsf_cross_len;
  ra8_fs_mount_t* evict = nullptr;
  flt_mount(&evict);

  /* Arm 1: the FAT sector cannot be read at all. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_append, &f));
  drop_fat_cache(evict, deep);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_seek(f, deep));
  flt_arm_read(k_flt_region_fat);
  TEST_ASSERT(ra8_fs_write(f, &one, 1U) != k_ra8_ok);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* Arm 2: the chain terminates before the offset the entry set claims. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.BIN", k_ra8_fs_mode_append, &f));
  drop_fat_cache(evict, deep);
  poke32(fat_at(first), (uint32_t)k_xsf_fat_eoc);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_seek(f, deep));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_write(f, &one, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(evict));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  flt_free_volume();
  TEST_END("exfat stream cov: the chain ends before the file does");
}

/**
 * @brief Run every exFAT streaming fault-injection test.
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
  test_grow_bitmap_lookup_read_fails();
  test_grow_bitmap_probe_read_fails();
  test_grow_bitmap_mark_write_fails();
  test_chain_materialize_fat_write_fails();
  test_data_sector_io_fails();
  test_flush_set_io_fails();
  test_open_lookup_read_fails();
  test_truncate_free_write_fails();
  test_create_file_table_full();
  test_create_no_directory_space();
  test_survey_rejects_broken_chain();
  test_survey_zero_length_with_cluster();
  test_survey_length_without_cluster();
  test_cluster_at_chain_ends_early();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_exfat_stream_cov.c\n");
  return 0;
}
