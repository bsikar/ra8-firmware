/**
 * @file test_ra8_fs_label.c
 * @brief Runtime volume-label read/set round-trips (`ra8_fs_{get,set}_label`, #682).
 *
 * @details
 * Round-trips a label through ::ra8_fs_set_label / ::ra8_fs_get_label on FAT16,
 * FAT32 and exFAT, and checks the ON-DISK bytes it writes -- `BS_VolLab` and the
 * root `ATTR_VOLUME_ID` entry on FAT, the Volume Label directory entry on exFAT
 * -- because a matching return code would not prove a host reads the label back.
 * The formatter's default (`"NO NAME    "` per #634) reports as the empty
 * string; clearing restores it and drops the root entry, so the two FAT copies
 * of the label stay consistent and `fsck.fat` stays quiet.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_fs_meta.h"
#include "test_ra8_fs_format_fixture.h"

/**
 * @enum label_probe_t
 * @brief On-disk offsets / markers the label assertions read.
 */
typedef enum : uint32_t {
  k_lb_f16_off    = 43U,   /**< FAT12/16 BS_VolLab byte offset.       */
  k_lb_f32_off    = 71U,   /**< FAT32 BS_VolLab byte offset.          */
  k_lb_width      = 11U,   /**< Label field width (bytes).            */
  k_lb_dir_name   = 0U,    /**< Dir-entry DIR_Name offset.            */
  k_lb_dir_attr   = 11U,   /**< Dir-entry DIR_Attr offset.            */
  k_lb_entry_size = 32U,   /**< Directory entry size.                 */
  k_lb_attr_volid = 0x08U, /**< ATTR_VOLUME_ID.                       */
  k_lb_attr_lfn   = 0x0FU, /**< Long-name marker.                     */
  k_lb_free_perm  = 0x00U, /**< End-of-directory marker.              */
  k_lb_free_used  = 0xE5U, /**< Deleted-entry marker.                 */
  k_lb_first_clus = 2U,    /**< First data cluster.                   */
  k_lb_xf_lbl_off = 64U,   /**< exFAT root Volume Label entry offset. */
  k_lb_xf_lbl_tag = 0x83U, /**< exFAT Volume Label entry type.        */
  k_lb_inuse_bit  = 0x80U, /**< exFAT dir-entry type in-use bit.      */
} label_probe_t;

/** @brief Read + trim the FAT BS_VolLab from the image into @p out. */
static void disk_boot_label(const ra8_fs_mount_t* h, char* out)
{
  const uint32_t off =
    (h->type == k_ra8_fs_type_fat32) ? (uint32_t)k_lb_f32_off : (uint32_t)k_lb_f16_off;
  uint32_t n = (uint32_t)k_lb_width;
  while ((n > 0U) && (s_disk.bytes[off + n - 1U] == (uint8_t)' ')) {
    n--;
  }
  for (uint32_t i = 0U; i < n; i++) {
    out[i] = (char)s_disk.bytes[off + i];
  }
  out[n] = '\0';
}

/** @brief Find the FAT16 root ATTR_VOLUME_ID entry in the image (fixed root). */
static bool fat16_find_volid(const ra8_fs_mount_t* h, uint8_t* out_entry)
{
  const uint32_t base = (h->partition_base_lba + h->first_root_lba) * (uint32_t)k_fmt_block_size;
  for (uint32_t e = 0U; e < h->root_entries; e++) {
    const uint32_t off   = base + (e * (uint32_t)k_lb_entry_size);
    const uint8_t  name0 = s_disk.bytes[off + (uint32_t)k_lb_dir_name];
    const uint8_t  attr  = s_disk.bytes[off + (uint32_t)k_lb_dir_attr];
    if (name0 == (uint8_t)k_lb_free_perm) {
      return false;
    }
    if (name0 == (uint8_t)k_lb_free_used) {
      continue;
    }
    if (attr == (uint8_t)k_lb_attr_lfn) {
      continue;
    }
    if ((attr & (uint8_t)k_lb_attr_volid) != 0U) {
      memcpy(out_entry, &s_disk.bytes[off], (size_t)k_lb_entry_size);
      return true;
    }
  }
  return false;
}

/** @brief Format @p type + mount, returning the handle. */
static ra8_fs_mount_t* fmt_mount(uint32_t blocks, ra8_fs_type_t type, const char* label)
{
  alloc_garbage_card(blocks);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = type;
  opts.label                = label;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  return h;
}

/** @brief Assert ::ra8_fs_get_label reports exactly @p want. */
static void assert_label(ra8_fs_mount_t* h, const char* want)
{
  char got[k_ra8_fs_label_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_get_label(h, got, (uint32_t)sizeof(got)));
  TEST_ASSERT_EQ(0, strcmp(got, want));
}

/**
 * @test test_label_fat16_roundtrip
 * @par MC/DC:
 * (no compound decision unique to this case -- it drives the FAT16 label
 * lifecycle: read the format label, create a root entry, rewrite it, then clear
 * it, checking `BS_VolLab` and the root ATTR_VOLUME_ID entry stay in step)
 */
static void test_label_fat16_roundtrip(void)
{
  TEST_BEGIN("ra8_fs label: FAT16 format/set/rewrite/clear round-trip");
  ra8_fs_mount_t* h = fmt_mount((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, "SCRATCH");

  /* Format label read back from BS_VolLab (the formatter writes no root entry). */
  assert_label(h, "SCRATCH");
  uint8_t entry[k_lb_entry_size] = {};
  TEST_ASSERT(!fat16_find_volid(h, entry));

  /* set_label creates the root entry AND updates BS_VolLab. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "MYDISK"));
  assert_label(h, "MYDISK");
  char boot[k_ra8_fs_label_cap] = {};
  disk_boot_label(h, boot);
  TEST_ASSERT_EQ(0, strcmp(boot, "MYDISK"));
  TEST_ASSERT(fat16_find_volid(h, entry));
  TEST_ASSERT_EQ(0, memcmp(&entry[k_lb_dir_name], "MYDISK     ", (size_t)k_lb_width));

  /* Rewriting the existing entry (fresh == false path). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "SECOND"));
  assert_label(h, "SECOND");
  TEST_ASSERT(fat16_find_volid(h, entry));
  TEST_ASSERT_EQ(0, memcmp(&entry[k_lb_dir_name], "SECOND     ", (size_t)k_lb_width));

  /* Clearing restores the sentinel and removes the root entry. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, nullptr));
  assert_label(h, "");
  disk_boot_label(h, boot);
  TEST_ASSERT_EQ(0, strcmp(boot, "NO NAME"));
  TEST_ASSERT(!fat16_find_volid(h, entry));

  /* Empty-string label clears too (the other clearing input). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "SET"));
  assert_label(h, "SET");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, ""));
  assert_label(h, "");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs label: FAT16 format/set/rewrite/clear round-trip");
}

/**
 * @test test_label_fat16_default_noname
 * @par MC/DC:
 * (no compound decision unique to this case -- an unlabelled format's
 * "NO NAME" sentinel reports as the empty string)
 */
static void test_label_fat16_default_noname(void)
{
  TEST_BEGIN("ra8_fs label: unlabelled FAT16 reports empty (NO NAME sentinel)");
  ra8_fs_mount_t* h = fmt_mount((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, nullptr);
  assert_label(h, "");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs label: unlabelled FAT16 reports empty (NO NAME sentinel)");
}

/**
 * @test test_label_fat32_roundtrip
 * @par MC/DC:
 * (no compound decision unique to this case -- FAT32 label set/get/clear, using
 * the FAT32 BS_VolLab offset and a cluster-chain root)
 */
static void test_label_fat32_roundtrip(void)
{
  TEST_BEGIN("ra8_fs label: FAT32 set/get/clear round-trip");
  ra8_fs_mount_t* h = fmt_mount((uint32_t)k_fmt_blocks_fat32, k_ra8_fs_type_fat32, "BIG32");
  assert_label(h, "BIG32");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "F32NAME"));
  assert_label(h, "F32NAME");
  char boot[k_ra8_fs_label_cap] = {};
  disk_boot_label(h, boot);
  TEST_ASSERT_EQ(0, strcmp(boot, "F32NAME"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, nullptr));
  assert_label(h, "");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs label: FAT32 set/get/clear round-trip");
}

/**
 * @test test_label_exfat_roundtrip
 * @par MC/DC:
 * (no compound decision unique to this case -- exFAT Volume Label entry
 * read/rewrite/clear, plus the create-at-EOD path when the label entry is
 * absent)
 */
static void test_label_exfat_roundtrip(void)
{
  TEST_BEGIN("ra8_fs label: exFAT set/get/clear + create-when-absent");
  ra8_fs_mount_t* h = fmt_mount((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat, "RTRIP");
  assert_label(h, "RTRIP");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "NEWEXF"));
  assert_label(h, "NEWEXF");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, nullptr));
  assert_label(h, "");

  /* Wipe the Volume Label entry (root cluster, 3rd entry) so the driver must
   * CREATE one at the end-of-directory slot on the next set. */
  const uint32_t root_byte =
    (h->partition_base_lba + h->first_data_lba +
     ((h->root_cluster - (uint32_t)k_lb_first_clus) * h->sectors_per_cluster)) *
    (uint32_t)k_fmt_block_size;
  memset(&s_disk.bytes[root_byte + (uint32_t)k_lb_xf_lbl_off], 0, (size_t)k_lb_entry_size);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "MADE"));
  assert_label(h, "MADE");
  TEST_ASSERT_EQ(k_lb_xf_lbl_tag, s_disk.bytes[root_byte + (uint32_t)k_lb_xf_lbl_off]);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs label: exFAT set/get/clear + create-when-absent");
}

/**
 * @test test_label_get_null_guard
 * @par MC/DC:
 * Decision: `if (handle == nullptr || out == nullptr)` (2 conditions) in
 * `libs/ra8_fs/src/ra8_fs_fat_label.c@priv_get_label_locked`.
 * - V1 handle=valid, out=valid -> F (control: both false).
 * - V2 handle=NULL,  out=valid -> C1=T -> T (varies handle only).
 * - V3 handle=valid, out=NULL  -> C1=F, C2=T -> T (varies out only).
 * Also covers the out_len==0 and not-in-use guards.
 */
static void test_label_get_null_guard(void)
{
  TEST_BEGIN("ra8_fs get_label MC/DC: (handle||out) NULL pair + out_len + state");
  ra8_fs_mount_t* h = fmt_mount((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, "G");
  char            out[k_ra8_fs_label_cap] = {};
  const uint32_t  cap                     = (uint32_t)sizeof(out);
  /* V1 both valid; V2 handle NULL; V3 out NULL; then out_len==0; then unmounted. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_get_label(h, out, cap));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_get_label(nullptr, out, cap));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_get_label(h, nullptr, cap));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_get_label(h, out, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_get_label(h, out, cap));
  free_volume();
  TEST_END("ra8_fs get_label MC/DC: (handle||out) NULL pair + out_len + state");
}

/**
 * @test test_label_set_guard
 * @par MC/DC:
 * Decision: `if (label != nullptr && priv_strlen(label) > k_fmt_label_len)`
 * (2 conditions) in `libs/ra8_fs/src/ra8_fs_fat_label.c@priv_set_label_locked`.
 * - V1 label=NULL           -> C1=F -> short-circuit F (clear; accepted).
 * - V2 label="DATA" (len 4) -> C1=T, C2=F -> F (accepted).
 * - V3 label=12-char string -> C1=T, C2=T -> T (rejected, invalid_arg).
 * V1+V3 prove C1 drives the outcome; V2+V3 prove C2 does. Also covers the null
 * handle and not-in-use guards.
 */
static void test_label_set_guard(void)
{
  TEST_BEGIN("ra8_fs set_label MC/DC: (label && overlong) guard + handle/state");
  ra8_fs_mount_t* h = fmt_mount((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, "S");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_set_label(nullptr, "X"));         /* handle NULL  */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, nullptr));                     /* V1           */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "DATA"));                      /* V2           */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_set_label(h, "ABCDEFGHIJKL")); /* V3: 12 chars */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_set_label(h, "Y"));
  free_volume();
  TEST_END("ra8_fs set_label MC/DC: (label && overlong) guard + handle/state");
}

/**
 * @test test_mcdc_find_free_root
 * @par MC/DC:
 * Decision: `if (name0 == free_perm || name0 == free_used)` (2 conditions) in
 * `libs/ra8_fs/src/ra8_fs_fat_label.c@priv_fat_find_free_root`, the free-slot
 * test, reached when set_label creates a new volume-label entry.
 * - V1 first slot is end-of-directory (0x00) -> C1=T -> found (fresh volume).
 * - V2 first slot is deleted (0xE5)           -> C1=F, C2=T -> found (an
 *      unlinked file left a tombstone at slot 0).
 * - V3 first slot is a live file entry         -> C1=F, C2=F -> skip; the scan
 *      finds the next free slot.
 * V1+V3 flip C1 (empty vs occupied); V2+V3 flip C2 (deleted vs live).
 */
static void test_mcdc_find_free_root(void)
{
  TEST_BEGIN("ra8_fs label MC/DC: find_free_root (perm||used) slot test");
  static const uint8_t body[2] = {'q', 'q'};

  /* V1: fresh root -> slot 0 is EOD (0x00). */
  ra8_fs_mount_t* h = fmt_mount((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, nullptr);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "AA"));
  assert_label(h, "AA");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();

  /* V2: unlink a file so slot 0 is a 0xE5 tombstone the scan reuses. */
  h = fmt_mount((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, nullptr);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "D.BIN", body, (uint32_t)sizeof(body)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "D.BIN"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "BB"));
  assert_label(h, "BB");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();

  /* V3: a live file at slot 0 is skipped; the next slot is free. */
  h = fmt_mount((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, nullptr);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "E.BIN", body, (uint32_t)sizeof(body)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "CC"));
  assert_label(h, "CC");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs label MC/DC: find_free_root (perm||used) slot test");
}

/**
 * @test test_mcdc_set_label_fat
 * @par MC/DC:
 * Two decisions in `libs/ra8_fs/src/ra8_fs_fat_label.c@priv_set_label_fat`.
 *
 * Decision A -- `clearing = (label == nullptr) || (label[0] == '\0')`:
 * - V1 label=NULL -> C1=T (clear).
 * - V2 label=""   -> C1=F, C2=T (clear).
 * - V3 label="X"  -> C1=F, C2=F (set).
 *
 * Decision B -- `if (ferr != k_ra8_ok && ferr != k_ra8_err_not_found)`, where
 * `ferr` is the volume-label-entry lookup result:
 * - ferr=ok        -> C1=F (rewrite the existing entry).
 * - ferr=not_found -> C1=T, C2=F (create a new entry).
 * - ferr=hw_error  -> C1=T, C2=T (propagate) -- covered in test_ra8_fs_meta_cov.c.
 */
static void test_mcdc_set_label_fat(void)
{
  TEST_BEGIN("ra8_fs label MC/DC: set_label_fat clearing + lookup-result guards");
  ra8_fs_mount_t* h = fmt_mount((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, nullptr);

  /* B: ferr=not_found (create), A: "X" non-empty. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "X"));
  assert_label(h, "X");
  /* B: ferr=ok (rewrite the entry just created), A: "Y" non-empty. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "Y"));
  assert_label(h, "Y");
  /* A: empty string clears (C1=F, C2=T). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, ""));
  assert_label(h, "");
  /* A: NULL clears (C1=T). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "Z"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, nullptr));
  assert_label(h, "");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs label MC/DC: set_label_fat clearing + lookup-result guards");
}

/**
 * @test test_mcdc_exfat_get_label
 * @par MC/DC:
 * Decision: `if (!present || entry[0] != k_exfat_entry_label)` (2 conditions) in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_label.c@priv_exfat_get_label`.
 * - V1 present, entry is 0x83 -> C1=F, C2=F -> decode the label.
 * - V2 no label entry present -> C1=T -> empty (a zeroed/absent entry).
 * - V3 present, entry is 0x03 -> C1=F, C2=T -> empty (a cleared entry).
 * V1+V2 flip C1 (present vs absent); V1+V3 flip C2 (in-use 0x83 vs cleared 0x03).
 */
static void test_mcdc_exfat_get_label(void)
{
  TEST_BEGIN("ra8_fs label MC/DC: exfat_get_label (!present || not-0x83)");
  ra8_fs_mount_t* h   = fmt_mount((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat, "PRESENT");
  const uint32_t  lbl = (h->partition_base_lba + h->first_data_lba +
                         ((h->root_cluster - (uint32_t)k_lb_first_clus) * h->sectors_per_cluster)) *
                          (uint32_t)k_fmt_block_size +
                        (uint32_t)k_lb_xf_lbl_off;

  /* V1: the formatted 0x83 label entry decodes. */
  assert_label(h, "PRESENT");
  /* V3: clear the in-use bit (0x83 -> 0x03) -> reported empty. */
  s_disk.bytes[lbl] = (uint8_t)(s_disk.bytes[lbl] & (uint8_t)~(uint8_t)k_lb_inuse_bit);
  assert_label(h, "");
  /* V2: zero the entry entirely (absent) -> reported empty. */
  memset(&s_disk.bytes[lbl], 0, (size_t)k_lb_entry_size);
  assert_label(h, "");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs label MC/DC: exfat_get_label (!present || not-0x83)");
}

int32_t main(void)
{
  test_label_fat16_roundtrip();
  test_label_fat16_default_noname();
  test_label_fat32_roundtrip();
  test_label_exfat_roundtrip();
  test_label_get_null_guard();
  test_label_set_guard();
  test_mcdc_find_free_root();
  test_mcdc_set_label_fat();
  test_mcdc_exfat_get_label();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_label.c\n");
  return 0;
}
