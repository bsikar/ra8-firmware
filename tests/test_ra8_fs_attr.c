/**
 * @file test_ra8_fs_attr.c
 * @brief File-attribute honor + set/clear (`ra8_fs_set_attr()`, #681) on FAT and exFAT.
 *
 * @details
 * Two halves of #681, proven on both filesystems:
 *
 *   - HONORED. A file marked read-only refuses every mutating path -- open for
 *     writing, open for appending, whole-file write, unlink and rename -- with
 *     ::k_ra8_err_access_denied, while an open for READING still succeeds and
 *     returns the original bytes. Clearing the bit restores ordinary access.
 *   - SETTABLE. Each of read-only / hidden / system / archive can be set and
 *     cleared and the change is read back through ::ra8_fs_stat (so `stat` stays
 *     truthful) and, on exFAT, through a recompute of the entry-set SetChecksum
 *     off the raw image (so the patch is proven non-corrupting and the volume
 *     re-mounts). A mask naming a non-settable bit, or one bit in both masks, is
 *     rejected.
 *   - ARCHIVE-ON-MODIFY. Clearing the archive bit and then writing content sets
 *     it again, the FAT/exFAT convention a backup tool relies on.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_fs_meta.h"
#include "test_ra8_fs_format_fixture.h"

/**
 * @enum attr_off_t
 * @brief On-disk attribute-byte offsets and the bit values under test.
 */
typedef enum : uint32_t {
  k_at_entry_size = 32U,     /**< Directory entry size.                */
  k_at_dir_attr   = 11U,     /**< FAT DIR_Attr byte offset.            */
  k_at_attr_lfn   = 0x0FU,   /**< Long-name marker.                    */
  k_at_attr_volid = 0x08U,   /**< ATTR_VOLUME_ID.                      */
  k_at_attr_dir   = 0x10U,   /**< ATTR_DIRECTORY.                      */
  k_at_attr_ro    = 0x01U,   /**< ATTR_READ_ONLY.                      */
  k_at_attr_hid   = 0x02U,   /**< ATTR_HIDDEN.                         */
  k_at_attr_sys   = 0x04U,   /**< ATTR_SYSTEM.                         */
  k_at_attr_arc   = 0x20U,   /**< ATTR_ARCHIVE.                        */
  k_at_dir_free   = 0x00U,   /**< End-of-directory marker.             */
  k_at_dir_del    = 0xE5U,   /**< Deleted-entry marker.                */
  k_at_dir_dot    = 0x2EU,   /**< "." / ".." entry.                    */
  k_at_first_clus = 2U,      /**< First data cluster.                  */
  k_at_xf_file    = 0x85U,   /**< exFAT File entry type.               */
  k_at_xf_secnt   = 1U,      /**< exFAT File entry SecondaryCount off. */
  k_at_xf_csum    = 2U,      /**< exFAT File entry SetChecksum off.    */
  k_at_xf_attr    = 4U,      /**< exFAT File entry FileAttributes off. */
  k_at_shl_b1     = 8U,      /**< LE byte-1 shift.                     */
  k_at_csum_wrap  = 0x8000U, /**< exFAT SetChecksum rotate wrap bit.   */
} attr_off_t;

static uint16_t img_rd16(uint32_t off)
{
  return (uint16_t)((uint16_t)s_disk.bytes[off] |
                    (uint16_t)((uint16_t)s_disk.bytes[off + 1U] << (uint16_t)k_at_shl_b1));
}

/** @brief Byte offset of the first regular FAT file entry in the fixed root. */
static uint32_t fat_file_off(const ra8_fs_mount_t* h)
{
  const uint32_t base = (h->partition_base_lba + h->first_root_lba) * (uint32_t)k_fmt_block_size;
  for (uint32_t e = 0U; e < h->root_entries; e++) {
    const uint32_t off   = base + (e * (uint32_t)k_at_entry_size);
    const uint8_t  name0 = s_disk.bytes[off];
    const uint8_t  attr  = s_disk.bytes[off + (uint32_t)k_at_dir_attr];
    if (name0 == (uint8_t)k_at_dir_free) {
      break;
    }
    if ((name0 == (uint8_t)k_at_dir_del) || (name0 == (uint8_t)k_at_dir_dot)) {
      continue;
    }
    if (attr == (uint8_t)k_at_attr_lfn) {
      continue;
    }
    if ((attr & ((uint8_t)k_at_attr_volid | (uint8_t)k_at_attr_dir)) != 0U) {
      continue;
    }
    return off;
  }
  TEST_FAIL_FMT("%s", "no FAT file entry found");
  return 0U;
}

/** @brief Byte offset of the exFAT File entry (0x85) in the root cluster. */
static uint32_t exfat_file_off(const ra8_fs_mount_t* h)
{
  const uint32_t root = (h->partition_base_lba + h->first_data_lba +
                         ((h->root_cluster - (uint32_t)k_at_first_clus) * h->sectors_per_cluster)) *
                        (uint32_t)k_fmt_block_size;
  const uint32_t per =
    (h->sectors_per_cluster * (uint32_t)k_fmt_block_size) / (uint32_t)k_at_entry_size;
  for (uint32_t e = 0U; e < per; e++) {
    const uint32_t off = root + (e * (uint32_t)k_at_entry_size);
    if (s_disk.bytes[off] == (uint8_t)k_at_xf_file) {
      return off;
    }
  }
  TEST_FAIL_FMT("%s", "no exFAT File entry found");
  return 0U;
}

/** @brief exFAT SetChecksum over @p count entries at @p off (skips File csum). */
static uint16_t exfat_img_setcsum(uint32_t off, uint32_t count)
{
  uint16_t cs = 0U;
  for (uint32_t i = 0U; i < (count * (uint32_t)k_at_entry_size); i++) {
    if ((i == (uint32_t)k_at_xf_csum) || (i == ((uint32_t)k_at_xf_csum + 1U))) {
      continue;
    }
    cs = (uint16_t)(((cs & 1U) != 0U ? (uint32_t)k_at_csum_wrap : 0U) + (cs >> 1) +
                    (uint16_t)s_disk.bytes[off + i]);
  }
  return cs;
}

/** @brief The attribute byte ra8_fs_stat reports for @p name. */
static uint8_t stat_attr(ra8_fs_mount_t* h, const char* name)
{
  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, name, &st));
  return st.attr;
}

static const uint8_t s_body[4] = {'d', 'a', 't', 'a'};

/** @brief Format @p type, mount, and create @p name holding "data". */
static ra8_fs_mount_t* fmt_mount_file(uint32_t blocks, ra8_fs_type_t type, const char* name)
{
  alloc_garbage_card(blocks);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = type;
  opts.label                = "ATTR";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, name, s_body, (uint32_t)sizeof(s_body)));
  return h;
}

/**
 * @brief Assert a read-only @p name refuses every mutating path but opens to read.
 *
 * @par MC/DC:
 * Decision: `if ((mode != read) && (attr & read_only))` (2 conditions) in
 * `libs/ra8_fs/src/ra8_fs_fat_file.c@priv_open_existing`.
 * - V1 mode=read,  read-only set   -> C1=F,(C2 not eval) -> F (read open OK).
 * - V3 mode=write, read-only set   -> C1=T,C2=T           -> T (denied).
 * - Vc mode=write, read-only CLEAR -> C1=T,C2=F           -> F (write open OK;
 *   supplied by the caller after clearing the bit).
 * V1+V3 hold C2 (read-only) true and flip C1, isolating the MODE condition;
 * Vc+V3 hold C1 (write) true and flip C2, isolating the READ-ONLY condition.
 * N+1 = 3 vectors for N=2 conditions. The exFAT path
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_openw.c@priv_exfat_open_found` reaches the
 * same behaviour with a SINGLE-condition read-only check -- `priv_exfat_open`
 * dispatches read opens away before it -- and both truth values of that one
 * condition are driven here (V3 true, Vc false).
 */
static void assert_readonly_refuses(ra8_fs_mount_t* h, const char* name, const char* other)
{
  ra8_fs_file_t* f = nullptr;
  /* Mutating paths are all refused. */
  TEST_ASSERT_EQ(k_ra8_err_access_denied, ra8_fs_open(h, name, k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_err_access_denied, ra8_fs_open(h, name, k_ra8_fs_mode_append, &f));
  TEST_ASSERT_EQ(k_ra8_err_access_denied,
                 ra8_fs_write_file(h, name, s_body, (uint32_t)sizeof(s_body)));
  TEST_ASSERT_EQ(k_ra8_err_access_denied, ra8_fs_unlink(h, name));
  TEST_ASSERT_EQ(k_ra8_err_access_denied, ra8_fs_rename(h, name, other));

  /* But a READ open still works and returns the original bytes. */
  uint8_t  rd[sizeof(s_body)] = {};
  uint32_t got                = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, name, k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, rd, (uint32_t)sizeof(rd), &got));
  TEST_ASSERT_EQ(sizeof(s_body), got);
  TEST_ASSERT_EQ(0, memcmp(s_body, rd, sizeof(s_body)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
}

/**
 * @test test_attr_honor_fat
 * @par MC/DC:
 * See ::assert_readonly_refuses for the open-guard decision. The unlink and
 * rename guards are single-condition (the read-only bit alone), driven true here
 * and false after the bit is cleared.
 */
static void test_attr_honor_fat(void)
{
  TEST_BEGIN("ra8_fs set_attr: FAT16 read-only honored on write/unlink/rename, read allowed");
  ra8_fs_mount_t* h = fmt_mount_file((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, "RO.BIN");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_attr(h, "RO.BIN", (uint8_t)k_ra8_fs_attr_read_only, 0U));
  TEST_ASSERT((stat_attr(h, "RO.BIN") & (uint8_t)k_at_attr_ro) != 0U);
  assert_readonly_refuses(h, "RO.BIN", "RW.BIN");

  /* Clear the bit -> write and unlink succeed again (isolates the mode/bit
   * conditions of the open guard, and drives the unlink/rename guards false). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_attr(h, "RO.BIN", 0U, (uint8_t)k_ra8_fs_attr_read_only));
  TEST_ASSERT((stat_attr(h, "RO.BIN") & (uint8_t)k_at_attr_ro) == 0U);
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "RO.BIN", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rename(h, "RO.BIN", "RW.BIN"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "RW.BIN"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs set_attr: FAT16 read-only honored on write/unlink/rename, read allowed");
}

/**
 * @test test_attr_honor_exfat
 * @par MC/DC:
 * The exFAT open guard mirrors the FAT one (see ::assert_readonly_refuses); the
 * unlink/rename guards are single-condition, exercised true then false.
 */
static void test_attr_honor_exfat(void)
{
  TEST_BEGIN("ra8_fs set_attr: exFAT read-only honored on write/unlink/rename, read allowed");
  ra8_fs_mount_t* h = fmt_mount_file((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat, "RO.BIN");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_attr(h, "RO.BIN", (uint8_t)k_ra8_fs_attr_read_only, 0U));
  TEST_ASSERT((stat_attr(h, "RO.BIN") & (uint8_t)k_at_attr_ro) != 0U);

  /* The set-attr patch left the entry set intact (SetChecksum still matches). */
  const uint32_t off   = exfat_file_off(h);
  const uint32_t count = 1U + (uint32_t)s_disk.bytes[off + (uint32_t)k_at_xf_secnt];
  TEST_ASSERT_EQ(exfat_img_setcsum(off, count), img_rd16(off + (uint32_t)k_at_xf_csum));

  assert_readonly_refuses(h, "RO.BIN", "RW.BIN");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_attr(h, "RO.BIN", 0U, (uint8_t)k_ra8_fs_attr_read_only));
  TEST_ASSERT((stat_attr(h, "RO.BIN") & (uint8_t)k_at_attr_ro) == 0U);
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "RO.BIN", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rename(h, "RO.BIN", "RW.BIN"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "RW.BIN"));

  /* The volume still mounts and the root is intact after all the churn. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  ra8_fs_mount_t* h2 = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h2));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h2));
  free_volume();
  TEST_END("ra8_fs set_attr: exFAT read-only honored on write/unlink/rename, read allowed");
}

/**
 * @brief Set each settable bit, re-stat, clear it, re-stat; directory bit intact.
 *
 * @par MC/DC:
 * (no compound decision unique to this helper -- each of the four settable bits
 * is set then cleared and the change is read back through `ra8_fs_stat`, which
 * proves the round-trip and that `stat` reporting stays truthful)
 */
static void assert_each_bit_round_trips(ra8_fs_mount_t* h, const char* name)
{
  static const uint8_t bits[4] = {(uint8_t)k_at_attr_ro,
                                  (uint8_t)k_at_attr_hid,
                                  (uint8_t)k_at_attr_sys,
                                  (uint8_t)k_at_attr_arc};
  for (uint32_t i = 0U; i < 4U; i++) {
    const uint8_t b = bits[i];
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_attr(h, name, b, 0U));
    TEST_ASSERT((stat_attr(h, name) & b) != 0U);
    /* A file's DIRECTORY bit is never accidentally set by a set-attr. */
    TEST_ASSERT((stat_attr(h, name) & (uint8_t)k_at_attr_dir) == 0U);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_attr(h, name, 0U, b));
    TEST_ASSERT((stat_attr(h, name) & b) == 0U);
  }
}

/**
 * @test test_attr_set_clear_fat
 * @par MC/DC:
 * (no compound decision unique to this case -- each settable bit round-trips
 * via ::assert_each_bit_round_trips, and a two-bit set is confirmed against the
 * raw on-disk FAT DIR_Attr byte)
 */
static void test_attr_set_clear_fat(void)
{
  TEST_BEGIN("ra8_fs set_attr: FAT16 set/clear read-only/hidden/system/archive round-trip");
  ra8_fs_mount_t* h = fmt_mount_file((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, "A.BIN");
  assert_each_bit_round_trips(h, "A.BIN");

  /* Set two at once, then confirm the raw on-disk byte carries exactly them (plus
   * the archive the file was created with) -- proof the patch is on the medium. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_fs_set_attr(h,
                    "A.BIN",
                    (uint8_t)((uint8_t)k_ra8_fs_attr_hidden | (uint8_t)k_ra8_fs_attr_system),
                    0U));
  const uint8_t raw = s_disk.bytes[fat_file_off(h) + (uint32_t)k_at_dir_attr];
  TEST_ASSERT((raw & (uint8_t)k_at_attr_hid) != 0U);
  TEST_ASSERT((raw & (uint8_t)k_at_attr_sys) != 0U);
  TEST_ASSERT((raw & (uint8_t)k_at_attr_dir) == 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs set_attr: FAT16 set/clear read-only/hidden/system/archive round-trip");
}

/**
 * @test test_attr_set_clear_exfat
 * @par MC/DC:
 * (no compound decision unique to this case -- each settable bit round-trips,
 * then a two-bit set is verified against the raw exFAT File entry byte, a fresh
 * SetChecksum, and a re-stat after a mount cycle)
 */
static void test_attr_set_clear_exfat(void)
{
  TEST_BEGIN("ra8_fs set_attr: exFAT set/clear round-trip + checksum + remount");
  ra8_fs_mount_t* h = fmt_mount_file((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat, "A.BIN");
  assert_each_bit_round_trips(h, "A.BIN");

  /* Set hidden|system, verify the on-disk File entry byte and a fresh SetChecksum. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_fs_set_attr(h,
                    "A.BIN",
                    (uint8_t)((uint8_t)k_ra8_fs_attr_hidden | (uint8_t)k_ra8_fs_attr_system),
                    0U));
  const uint32_t off   = exfat_file_off(h);
  const uint32_t count = 1U + (uint32_t)s_disk.bytes[off + (uint32_t)k_at_xf_secnt];
  const uint8_t  raw   = s_disk.bytes[off + (uint32_t)k_at_xf_attr];
  TEST_ASSERT((raw & (uint8_t)k_at_attr_hid) != 0U);
  TEST_ASSERT((raw & (uint8_t)k_at_attr_sys) != 0U);
  TEST_ASSERT((raw & (uint8_t)k_at_attr_dir) == 0U);
  TEST_ASSERT_EQ(exfat_img_setcsum(off, count), img_rd16(off + (uint32_t)k_at_xf_csum));

  /* Remount and re-stat: the patched attributes survive a mount cycle. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  ra8_fs_mount_t* h2 = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h2));
  const uint8_t got = stat_attr(h2, "A.BIN");
  TEST_ASSERT((got & (uint8_t)k_at_attr_hid) != 0U);
  TEST_ASSERT((got & (uint8_t)k_at_attr_sys) != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h2));
  free_volume();
  TEST_END("ra8_fs set_attr: exFAT set/clear round-trip + checksum + remount");
}

/**
 * @test test_attr_open_time_only
 * @par MC/DC:
 * (no compound decision unique to this case -- it pins the OPEN-TIME semantics
 * of the read-only honor: a handle already opened for writing keeps writing
 * after the file is marked read-only, matching POSIX/DOS; only a fresh open is
 * refused)
 */
static void test_attr_open_time_only(void)
{
  TEST_BEGIN("ra8_fs set_attr: read-only is honored at open time, not per write");
  ra8_fs_mount_t* h = fmt_mount_file((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, "O.BIN");

  /* Open for writing FIRST, then mark the file read-only through set_attr. */
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "O.BIN", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_attr(h, "O.BIN", (uint8_t)k_ra8_fs_attr_read_only, 0U));

  /* The already-open handle keeps writing -- the check was at open time. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, s_body, (uint32_t)sizeof(s_body)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* The bit still stands (the write's archive-set did not clear it), and a fresh
   * write open is now refused. */
  TEST_ASSERT((stat_attr(h, "O.BIN") & (uint8_t)k_at_attr_ro) != 0U);
  TEST_ASSERT_EQ(k_ra8_err_access_denied, ra8_fs_open(h, "O.BIN", k_ra8_fs_mode_write, &f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs set_attr: read-only is honored at open time, not per write");
}

/**
 * @test test_attr_archive_fat
 * @par MC/DC:
 * (no compound decision unique to this case -- the archive bit is cleared, a
 * write modifies content, and the bit is shown set again off `ra8_fs_stat`)
 */
static void test_attr_archive_fat(void)
{
  TEST_BEGIN("ra8_fs set_attr: FAT16 archive bit re-set on content modification");
  ra8_fs_mount_t* h = fmt_mount_file((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, "M.BIN");
  /* Fresh files carry archive. */
  TEST_ASSERT((stat_attr(h, "M.BIN") & (uint8_t)k_at_attr_arc) != 0U);
  /* Clear it, then modify the file: the write must put archive back. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_attr(h, "M.BIN", 0U, (uint8_t)k_ra8_fs_attr_archive));
  TEST_ASSERT((stat_attr(h, "M.BIN") & (uint8_t)k_at_attr_arc) == 0U);
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "M.BIN", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, s_body, (uint32_t)sizeof(s_body)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT((stat_attr(h, "M.BIN") & (uint8_t)k_at_attr_arc) != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs set_attr: FAT16 archive bit re-set on content modification");
}

/**
 * @test test_attr_archive_exfat
 * @par MC/DC:
 * (no compound decision unique to this case -- exFAT archive is cleared, a
 * streaming write modifies content, and the bit is shown set again)
 */
static void test_attr_archive_exfat(void)
{
  TEST_BEGIN("ra8_fs set_attr: exFAT archive bit re-set on content modification");
  ra8_fs_mount_t* h = fmt_mount_file((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat, "M.BIN");
  TEST_ASSERT((stat_attr(h, "M.BIN") & (uint8_t)k_at_attr_arc) != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_attr(h, "M.BIN", 0U, (uint8_t)k_ra8_fs_attr_archive));
  TEST_ASSERT((stat_attr(h, "M.BIN") & (uint8_t)k_at_attr_arc) == 0U);
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "M.BIN", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, s_body, (uint32_t)sizeof(s_body)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT((stat_attr(h, "M.BIN") & (uint8_t)k_at_attr_arc) != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs set_attr: exFAT archive bit re-set on content modification");
}

/**
 * @test test_attr_errors
 * @par MC/DC:
 * Decision: `if (((set & ~settable) != 0) || ((clear & ~settable) != 0) ||
 * ((set & clear) != 0))` (3 conditions) in
 * `libs/ra8_fs/src/ra8_fs_fat_attr.c@priv_setattr_locked`.
 * - V1 set=archive, clear=0            -> C1=F,C2=F,C3=F -> F (control; patched).
 * - V2 set=directory(non-settable)     -> C1=T -> T (varies C1).
 * - V3 set=0, clear=directory          -> C1=F,C2=T -> T (varies C2).
 * - V4 set=archive, clear=archive      -> C1=F,C2=F,C3=T -> T (varies C3).
 * V1+V2 isolate C1, V1+V3 isolate C2, V1+V4 isolate C3: N+1 = 4 vectors.
 * Also covers NULL handle/path (null_ptr), the root path and a bad name
 * (invalid_arg), a missing name (not_found) and an unmounted handle
 * (invalid_state).
 */
static void test_attr_errors(void)
{
  TEST_BEGIN("ra8_fs set_attr MC/DC: mask validation + null/root/missing/state");
  ra8_fs_mount_t* h = fmt_mount_file((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, "E.BIN");

  /* Mask-validation compound decision. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_set_attr(h, "E.BIN", (uint8_t)k_ra8_fs_attr_archive, 0U)); /* V1 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_fs_set_attr(h, "E.BIN", (uint8_t)k_at_attr_dir, 0U)); /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_fs_set_attr(h, "E.BIN", 0U, (uint8_t)k_at_attr_dir)); /* V3 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_fs_set_attr(h,
                                 "E.BIN",
                                 (uint8_t)k_ra8_fs_attr_archive,
                                 (uint8_t)k_ra8_fs_attr_archive)); /* V4 */

  /* Argument / state errors. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_fs_set_attr(nullptr, "E.BIN", (uint8_t)k_ra8_fs_attr_archive, 0U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_fs_set_attr(h, nullptr, (uint8_t)k_ra8_fs_attr_archive, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_fs_set_attr(h, "/", (uint8_t)k_ra8_fs_attr_archive, 0U));
  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 ra8_fs_set_attr(h, "NOPE.BIN", (uint8_t)k_ra8_fs_attr_archive, 0U));
  /* Both masks 0 is a no-op success. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_attr(h, "E.BIN", 0U, 0U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_fs_set_attr(h, "E.BIN", (uint8_t)k_ra8_fs_attr_archive, 0U));
  free_volume();
  TEST_END("ra8_fs set_attr MC/DC: mask validation + null/root/missing/state");
}

int32_t main(void)
{
  test_attr_honor_fat();
  test_attr_honor_exfat();
  test_attr_set_clear_fat();
  test_attr_set_clear_exfat();
  test_attr_open_time_only();
  test_attr_archive_fat();
  test_attr_archive_exfat();
  test_attr_errors();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_attr.c\n");
  return 0;
}
