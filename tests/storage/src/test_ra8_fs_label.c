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

#include "ra8_attributes.h"
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
  k_lb_attr_arch  = 0x20U, /**< ATTR_ARCHIVE: in use, not a label.    */
  k_lb_name_char  = 0x41U, /**< 'A' -- a DIR_Name[0] that is neither
                                0x00 nor 0xE5, so the slot reads as
                                occupied to both root walks.          */
} label_probe_t;

/**
 * @enum lb_walk_fault_t
 * @brief Constants for aiming a read fault at a FAT32 root walk.
 *
 * @details
 * `priv_dir_walk_next_sector` touches the FAT exactly once per cluster hop,
 * through `priv_fat_get`. Aiming at that read by ORDINAL position is not
 * reliable, because ra8_fs_fat_alloc.c caches one FAT sector: a second visit
 * to the same sector never reaches the backend at all. So the arms below aim
 * by LBA instead, and the two-cluster root exists precisely so the cache
 * cannot mask the read that has to fail:
 *
 *   root_cluster's FAT entry lives in FAT sector 0; ::k_lb_far_cluster is
 *   chosen so ITS entry lives in FAT sector 1. A walk that hops root -> far
 *   therefore leaves sector 1 cached, and the NEXT walk's first FAT read --
 *   sector 0 again -- is a genuine backend read that a fault can take.
 *
 * @invariant ::k_lb_far_cluster * ::k_lb_fat32_ent is at least one sector, so
 *            the two clusters' FAT entries never share a sector.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_lb_fat32_ent   = 4U,          /**< FAT32 entry width in bytes.          */
  k_lb_fat32_eoc   = 0x0FFFFFFFU, /**< A FAT32 end-of-chain value.          */
  k_lb_far_cluster = 128U,        /**< 128 * 4 = 512 -> FAT sector 1.       */
  k_lb_skip_none   = 0U,          /**< Fail the first matching read.        */
  k_lb_skip_first  = 1U,          /**< Let one matching read through first. */
} lb_walk_fault_t;

/** @brief Read + trim the FAT BS_VolLab from the image into @p out. @details Implements the bounded disk boot label fixture step using caller-owned state. @param[in] h Value required by this filesystem vector. @param[out] out Caller-owned output populated on success. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_disk_boot_label(const ra8_fs_mount_t* h, char* out)
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

/** @brief Find the FAT16 root ATTR_VOLUME_ID entry in the image (fixed root). @details Implements the bounded fat16 find volid fixture step using caller-owned state. @param[in] h Value required by this filesystem vector. @param[out] out_entry Caller-owned output populated on success. @return Status, selected object, or bounded value produced by the named operation. @retval true The named condition holds. @retval false The condition does not hold. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static bool internal_fat16_find_volid(const ra8_fs_mount_t* h, uint8_t* out_entry)
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
RA8_INTERNAL static ra8_fs_mount_t*
internal_fmt_mount(uint32_t blocks, ra8_fs_type_t type, const char* label)
{
  internal_alloc_garbage_card(blocks);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = type;
  opts.label                = label;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  return h;
}

/** @brief Assert ::ra8_fs_get_label reports exactly @p want. @details Implements the bounded assert label fixture step using caller-owned state. @param[in,out] h Value required by this filesystem vector. @param[in] want Value required by this filesystem vector. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_assert_label(ra8_fs_mount_t* h, const char* want)
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
 * it, checking `BS_VolLab` and the root ATTR_VOLUME_ID entry stay in step) @brief Exercise the label fat16 roundtrip filesystem operation. @details Runs the label fat16 roundtrip vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_label_fat16_roundtrip(void)
{
  TEST_BEGIN("ra8_fs label: FAT16 format/set/rewrite/clear round-trip");
  ra8_fs_mount_t* h =
    internal_fmt_mount((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, "SCRATCH");

  /* Format label read back from BS_VolLab (the formatter writes no root entry). */
  internal_assert_label(h, "SCRATCH");
  uint8_t entry[k_lb_entry_size] = {};
  TEST_ASSERT(!internal_fat16_find_volid(h, entry));

  /* set_label creates the root entry AND updates BS_VolLab. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "MYDISK"));
  internal_assert_label(h, "MYDISK");
  char boot[k_ra8_fs_label_cap] = {};
  internal_disk_boot_label(h, boot);
  TEST_ASSERT_EQ(0, strcmp(boot, "MYDISK"));
  TEST_ASSERT(internal_fat16_find_volid(h, entry));
  TEST_ASSERT_EQ(0, memcmp(&entry[k_lb_dir_name], "MYDISK     ", (size_t)k_lb_width));

  /* Rewriting the existing entry (fresh == false path). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "SECOND"));
  internal_assert_label(h, "SECOND");
  TEST_ASSERT(internal_fat16_find_volid(h, entry));
  TEST_ASSERT_EQ(0, memcmp(&entry[k_lb_dir_name], "SECOND     ", (size_t)k_lb_width));

  /* Clearing restores the sentinel and removes the root entry. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, nullptr));
  internal_assert_label(h, "");
  internal_disk_boot_label(h, boot);
  TEST_ASSERT_EQ(0, strcmp(boot, "NO NAME"));
  TEST_ASSERT(!internal_fat16_find_volid(h, entry));

  /* Empty-string label clears too (the other clearing input). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "SET"));
  internal_assert_label(h, "SET");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, ""));
  internal_assert_label(h, "");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs label: FAT16 format/set/rewrite/clear round-trip");
}

/**
 * @test test_label_fat16_default_noname
 * @par MC/DC:
 * (no compound decision unique to this case -- an unlabelled format's
 * "NO NAME" sentinel reports as the empty string) @brief Exercise the label fat16 default noname filesystem operation. @details Runs the label fat16 default noname vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_label_fat16_default_noname(void)
{
  TEST_BEGIN("ra8_fs label: unlabelled FAT16 reports empty (NO NAME sentinel)");
  ra8_fs_mount_t* h =
    internal_fmt_mount((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, nullptr);
  internal_assert_label(h, "");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs label: unlabelled FAT16 reports empty (NO NAME sentinel)");
}

/**
 * @test test_label_fat32_roundtrip
 * @par MC/DC:
 * (no compound decision unique to this case -- FAT32 label set/get/clear, using
 * the FAT32 BS_VolLab offset and a cluster-chain root) @brief Exercise the label fat32 roundtrip filesystem operation. @details Runs the label fat32 roundtrip vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_label_fat32_roundtrip(void)
{
  TEST_BEGIN("ra8_fs label: FAT32 set/get/clear round-trip");
  ra8_fs_mount_t* h =
    internal_fmt_mount((uint32_t)k_fmt_blocks_fat32, k_ra8_fs_type_fat32, "BIG32");
  internal_assert_label(h, "BIG32");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "F32NAME"));
  internal_assert_label(h, "F32NAME");
  char boot[k_ra8_fs_label_cap] = {};
  internal_disk_boot_label(h, boot);
  TEST_ASSERT_EQ(0, strcmp(boot, "F32NAME"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, nullptr));
  internal_assert_label(h, "");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs label: FAT32 set/get/clear round-trip");
}

/**
 * @test test_label_exfat_roundtrip
 * @par MC/DC:
 * (no compound decision unique to this case -- exFAT Volume Label entry
 * read/rewrite/clear, plus the create-at-EOD path when the label entry is
 * absent) @brief Exercise the label exfat roundtrip filesystem operation. @details Runs the label exfat roundtrip vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_label_exfat_roundtrip(void)
{
  TEST_BEGIN("ra8_fs label: exFAT set/get/clear + create-when-absent");
  ra8_fs_mount_t* h =
    internal_fmt_mount((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat, "RTRIP");
  internal_assert_label(h, "RTRIP");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "NEWEXF"));
  internal_assert_label(h, "NEWEXF");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, nullptr));
  internal_assert_label(h, "");

  /* Wipe the Volume Label entry (root cluster, 3rd entry) so the driver must
   * CREATE one at the end-of-directory slot on the next set. */
  const uint32_t internal_root_byte =
    (h->partition_base_lba + h->first_data_lba +
     ((uint64_t)(h->root_cluster - (uint32_t)k_lb_first_clus) * h->sectors_per_cluster)) *
    (uint32_t)k_fmt_block_size;
  memset(&s_disk.bytes[internal_root_byte + (uint32_t)k_lb_xf_lbl_off], 0, (size_t)k_lb_entry_size);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "MADE"));
  internal_assert_label(h, "MADE");
  TEST_ASSERT_EQ(k_lb_xf_lbl_tag, s_disk.bytes[internal_root_byte + (uint32_t)k_lb_xf_lbl_off]);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs label: exFAT set/get/clear + create-when-absent");
}

/**
 * @test test_label_get_null_guard
 * @par MC/DC:
 * Decision: `if (handle == nullptr || out == nullptr)` (2 conditions) in
 * `libs/ra8_fs/src/ra8_fs_fat_label.c@internal_get_label_locked`.
 * - V1 handle=valid, out=valid -> F (control: both false).
 * - V2 handle=NULL,  out=valid -> C1=T -> T (varies handle only).
 * - V3 handle=valid, out=NULL  -> C1=F, C2=T -> T (varies out only).
 * Also covers the out_len==0 and not-in-use guards. @brief Exercise the label get null guard filesystem operation. @details Runs the label get null guard vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_label_get_null_guard(void)
{
  TEST_BEGIN("ra8_fs get_label MC/DC: (handle||out) NULL pair + out_len + state");
  ra8_fs_mount_t* h = internal_fmt_mount((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, "G");
  char            out[k_ra8_fs_label_cap] = {};
  const uint32_t  cap                     = (uint32_t)sizeof(out);
  /* V1 both valid; V2 handle NULL; V3 out NULL; then out_len==0; then unmounted. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_get_label(h, out, cap));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_get_label(nullptr, out, cap));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_get_label(h, nullptr, cap));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_get_label(h, out, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_get_label(h, out, cap));
  internal_free_volume();
  TEST_END("ra8_fs get_label MC/DC: (handle||out) NULL pair + out_len + state");
}

/**
 * @test test_label_set_guard
 * @par MC/DC:
 * Decision: `if (label != nullptr && priv_strlen(label) > k_fmt_label_len)`
 * (2 conditions) in `libs/ra8_fs/src/ra8_fs_fat_label.c@internal_set_label_locked`.
 * - V1 label=NULL           -> C1=F -> short-circuit F (clear; accepted).
 * - V2 label="DATA" (len 4) -> C1=T, C2=F -> F (accepted).
 * - V3 label=12-char string -> C1=T, C2=T -> T (rejected, invalid_arg).
 * V1+V3 prove C1 drives the outcome; V2+V3 prove C2 does. Also covers the null
 * handle and not-in-use guards. @brief Exercise the label set guard filesystem operation. @details Runs the label set guard vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_label_set_guard(void)
{
  TEST_BEGIN("ra8_fs set_label MC/DC: (label && overlong) guard + handle/state");
  ra8_fs_mount_t* h = internal_fmt_mount((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, "S");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_set_label(nullptr, "X"));         /* handle NULL  */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, nullptr));                     /* V1           */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "DATA"));                      /* V2           */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_set_label(h, "ABCDEFGHIJKL")); /* V3: 12 chars */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_set_label(h, "Y"));
  internal_free_volume();
  TEST_END("ra8_fs set_label MC/DC: (label && overlong) guard + handle/state");
}

/**
 * @test test_mcdc_find_free_root
 * @par MC/DC:
 * Decision: `if (name0 == free_perm || name0 == free_used)` (2 conditions) in
 * `libs/ra8_fs/src/ra8_fs_fat_label.c@internal_fat_find_free_root`, the free-slot
 * test, reached when set_label creates a new volume-label entry.
 * - V1 first slot is end-of-directory (0x00) -> C1=T -> found (fresh volume).
 * - V2 first slot is deleted (0xE5)           -> C1=F, C2=T -> found (an
 *      unlinked file left a tombstone at slot 0).
 * - V3 first slot is a live file entry         -> C1=F, C2=F -> skip; the scan
 *      finds the next free slot.
 * V1+V3 flip C1 (empty vs occupied); V2+V3 flip C2 (deleted vs live). @brief Exercise the mcdc find free root filesystem operation. @details Runs the mcdc find free root vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_find_free_root(void)
{
  TEST_BEGIN("ra8_fs label MC/DC: find_free_root (perm||used) slot test");
  static const uint8_t body[2] = {'q', 'q'};

  /* V1: fresh root -> slot 0 is EOD (0x00). */
  ra8_fs_mount_t* h =
    internal_fmt_mount((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, nullptr);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "AA"));
  internal_assert_label(h, "AA");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();

  /* V2: unlink a file so slot 0 is a 0xE5 tombstone the scan reuses. */
  h = internal_fmt_mount((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, nullptr);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "D.BIN", body, (uint32_t)sizeof(body)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "D.BIN"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "BB"));
  internal_assert_label(h, "BB");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();

  /* V3: a live file at slot 0 is skipped; the next slot is free. */
  h = internal_fmt_mount((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, nullptr);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "E.BIN", body, (uint32_t)sizeof(body)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "CC"));
  internal_assert_label(h, "CC");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs label MC/DC: find_free_root (perm||used) slot test");
}

/**
 * @test test_mcdc_set_label_fat
 * @par MC/DC:
 * Two decisions in `libs/ra8_fs/src/ra8_fs_fat_label.c@internal_set_label_fat`.
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
 * - ferr=hw_error  -> C1=T, C2=T (propagate) -- covered in test_ra8_fs_meta_cov.c. @brief Exercise the mcdc set label fat filesystem operation. @details Runs the mcdc set label fat vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_set_label_fat(void)
{
  TEST_BEGIN("ra8_fs label MC/DC: set_label_fat clearing + lookup-result guards");
  ra8_fs_mount_t* h =
    internal_fmt_mount((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, nullptr);

  /* B: ferr=not_found (create), A: "X" non-empty. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "X"));
  internal_assert_label(h, "X");
  /* B: ferr=ok (rewrite the entry just created), A: "Y" non-empty. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "Y"));
  internal_assert_label(h, "Y");
  /* A: empty string clears (C1=F, C2=T). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, ""));
  internal_assert_label(h, "");
  /* A: NULL clears (C1=T). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "Z"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, nullptr));
  internal_assert_label(h, "");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
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
 * V1+V2 flip C1 (present vs absent); V1+V3 flip C2 (in-use 0x83 vs cleared 0x03). @brief Exercise the mcdc exfat get label filesystem operation. @details Runs the mcdc exfat get label vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_exfat_get_label(void)
{
  TEST_BEGIN("ra8_fs label MC/DC: exfat_get_label (!present || not-0x83)");
  ra8_fs_mount_t* h =
    internal_fmt_mount((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat, "PRESENT");
  const uint32_t lbl =
    (h->partition_base_lba + h->first_data_lba +
     ((uint64_t)(h->root_cluster - (uint32_t)k_lb_first_clus) * h->sectors_per_cluster)) *
      (uint32_t)k_fmt_block_size +
    (uint32_t)k_lb_xf_lbl_off;

  /* V1: the formatted 0x83 label entry decodes. */
  internal_assert_label(h, "PRESENT");
  /* V3: clear the in-use bit (0x83 -> 0x03) -> reported empty. */
  s_disk.bytes[lbl] = (uint8_t)(s_disk.bytes[lbl] & (uint8_t)~(uint8_t)k_lb_inuse_bit);
  internal_assert_label(h, "");
  /* V2: zero the entry entirely (absent) -> reported empty. */
  memset(&s_disk.bytes[lbl], 0, (size_t)k_lb_entry_size);
  internal_assert_label(h, "");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs label MC/DC: exfat_get_label (!present || not-0x83)");
}

/**
 * @brief Absolute byte offset of a FAT32 data cluster in the RAM image.
 *
 * @details Mirrors `priv_cluster_to_lba` independently of the driver, so a
 *          corrupted image cannot be blamed on a shared helper.
 *
 * @param[in] h    Mounted FAT32 volume.
 * @param[in] clus Cluster number (>= 2).
 *
 * @return Byte offset of the cluster's first sector in `s_disk.bytes`.
 *
 * @pre `s_disk.bytes` is allocated and @p h is mounted.
 * @pre @p clus is at least ::k_lb_first_clus.
 * @post No image byte is modified.
 * @post The offset is inside the allocated image.
 *
 * @note Test-only helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_lb_clus_off(const ra8_fs_mount_t* h, uint32_t clus)
{
  const uint64_t vlba =
    h->first_data_lba + ((uint64_t)(clus - (uint32_t)k_lb_first_clus) * h->sectors_per_cluster);
  return (uint32_t)((h->partition_base_lba + vlba) * (uint64_t)k_fmt_block_size);
}

/**
 * @brief Absolute LBA of the FAT sector holding @p clus's FAT32 entry.
 *
 * @param[in] h    Mounted FAT32 volume.
 * @param[in] clus Cluster whose entry is wanted.
 *
 * @return The sector number the backend will be asked for.
 *
 * @pre @p h is a mounted FAT32 volume.
 * @pre @p clus is within the volume's cluster count.
 * @post No image byte is modified.
 * @post The result includes `partition_base_lba`, as the backend sees it.
 *
 * @note Test-only helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_lb_fat_lba(const ra8_fs_mount_t* h, uint32_t clus)
{
  const uint64_t byte_off = (uint64_t)clus * (uint32_t)k_lb_fat32_ent;
  return h->partition_base_lba + h->first_fat_lba + (byte_off / (uint32_t)k_fmt_block_size);
}

/**
 * @brief Write a FAT32 chain entry straight into the RAM image.
 *
 * @details Patches the first FAT only; the driver reads its chain from
 *          `first_fat_lba`, and the mirror copy is never consulted on the read
 *          path.
 *
 * @param[in] h    Mounted FAT32 volume.
 * @param[in] clus Cluster whose entry is written.
 * @param[in] val  Successor cluster, or ::k_lb_fat32_eoc to end the chain.
 *
 * @pre `s_disk.bytes` is allocated and @p h is mounted FAT32.
 * @pre @p clus is within the volume's cluster count.
 * @post The on-disk FAT entry for @p clus reads back as @p val.
 * @post No other FAT entry changes.
 *
 * @note Test-only helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_lb_fat32_set(const ra8_fs_mount_t* h, uint32_t clus, uint32_t val)
{
  TEST_ASSERT_NOT_NULL(s_disk.bytes);
  const uint32_t off =
    (uint32_t)((h->partition_base_lba + h->first_fat_lba) * (uint64_t)k_fmt_block_size) +
    (clus * (uint32_t)k_lb_fat32_ent);
  s_disk.bytes[off]      = (uint8_t)(val & 0xFFU);
  s_disk.bytes[off + 1U] = (uint8_t)((val >> 8U) & 0xFFU);
  s_disk.bytes[off + 2U] = (uint8_t)((val >> 16U) & 0xFFU);
  s_disk.bytes[off + 3U] = (uint8_t)((val >> 24U) & 0xFFU);
}

/**
 * @brief Fill a directory region with occupied, non-label, non-LFN entries.
 *
 * @details Every slot gets DIR_Name[0] = 'A' and ATTR_ARCHIVE, so
 *          `internal_fat_find_vol_id` skips it (it is not a volume-ID entry
 *          and not a long-name fragment) and `internal_fat_find_free_root`
 *          rejects it (it is neither 0x00 nor 0xE5). A region filled this way
 *          forces BOTH walks to run to the end of the directory instead of
 *          returning early -- which is the only way to reach the code after
 *          their inner loops.
 *
 * @param[in] base  Byte offset of the first entry.
 * @param[in] bytes Region length; a whole number of 32-byte entries.
 *
 * @pre `s_disk.bytes` covers [base, base + bytes).
 * @pre @p bytes is a multiple of ::k_lb_entry_size.
 * @post Every slot in the region reads as occupied.
 * @post No byte outside the region changes.
 *
 * @note Test-only helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_lb_fill_occupied(uint32_t base, uint32_t bytes)
{
  TEST_ASSERT_NOT_NULL(s_disk.bytes);
  for (uint32_t o = 0U; o < bytes; o += (uint32_t)k_lb_entry_size) {
    memset(&s_disk.bytes[base + o], (int)' ', (size_t)k_lb_entry_size);
    s_disk.bytes[base + o + (uint32_t)k_lb_dir_name] = (uint8_t)k_lb_name_char;
    s_disk.bytes[base + o + (uint32_t)k_lb_dir_attr] = (uint8_t)k_lb_attr_arch;
  }
}

/**
 * @brief Format a FAT32 card through the plain backend and mount it through
 *        the fault-injecting one, with its root cluster fully occupied.
 *
 * @details The format has to go through the clean backend -- a fault during
 *          format would produce an unmountable image and prove nothing. The
 *          MOUNT is what has to be faultable, because the walks under test run
 *          after it.
 *
 * @return The mounted handle.
 *
 * @pre No volume is currently allocated.
 * @pre The fault counters are reset by this call before mounting.
 * @post The root is one cluster with every slot occupied and no EOD marker.
 * @post No fault is armed on return.
 *
 * @note Test-only helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_fs_mount_t* internal_lb_fat32_full_root(void)
{
  internal_alloc_garbage_card((uint32_t)k_fmt_blocks_fat32);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat32;
  opts.label                = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_fault_backend, &h));
  internal_lb_fill_occupied(internal_lb_clus_off(h, h->root_cluster),
                            h->sectors_per_cluster * (uint32_t)k_fmt_block_size);
  internal_fault_reset();
  return h;
}

/**
 * @test internal_test_label_fat32_root_walk_error
 * @brief A backend failure inside the FAT32 root walk surfaces out of
 *        `internal_fat_find_vol_id` instead of being read as "no label".
 *
 * @details
 * On FAT12/16 the root is a fixed sector range and `priv_dir_walk_next_sector`
 * cannot fail -- which is what the old suppression asserted. On FAT32 the root
 * is a cluster chain, so the walk calls `priv_fat_get`, and a backend read
 * failure there IS the error this line propagates. Reaching it needs a root
 * whose every slot is occupied (otherwise the scan returns a hit or an EOD
 * before the cluster ends) and a fault placed exactly on the FAT read.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` after `priv_dir_walk_next_sector` in
 * libs/ra8_fs/src/ra8_fs_fat_label.c@internal_fat_find_vol_id (1 condition).
 * - V1: no fault armed -> the FAT read succeeds and reports EOC -> the
 *   decision is false, the walk ends normally and get_label falls back to
 *   BS_VolLab (control, at the end of this case).
 * - V2: the walk's FAT read faulted -> the decision is true and the error is
 *   returned to the caller (this case).
 * N = 1 condition, N+1 = 2 vectors. Both run the identical call over the
 * identical image and differ only in whether the FAT read succeeded.
 *
 * @pre The root cluster is fully occupied so the walk reaches its end.
 * @pre The fault is armed on the read index the walk's FAT read occupies.
 * @post `ra8_fs_get_label` reported the backend error, not k_ra8_ok.
 * @post With the fault cleared the same call succeeds.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_label_fat32_root_walk_error(void)
{
  TEST_BEGIN("ra8_fs label: FAT32 root-walk read failure surfaces from find_vol_id");
  ra8_fs_mount_t* h = internal_lb_fat32_full_root();

  char got[k_ra8_fs_label_cap] = {};
  /* The only FAT read `ra8_fs_get_label` makes is the one inside the walk, so
     aiming at that sector cannot hit anything else. */
  internal_fault_read_lba(internal_lb_fat_lba(h, h->root_cluster), (uint32_t)k_lb_skip_none);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_get_label(h, got, (uint32_t)sizeof(got)));

  /* Control: with the fault cleared the same walk completes and the label
     falls back to BS_VolLab, which an unlabelled format leaves empty. */
  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_get_label(h, got, (uint32_t)sizeof(got)));
  TEST_ASSERT_EQ(0, strcmp(got, ""));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs label: FAT32 root-walk read failure surfaces from find_vol_id");
}

/**
 * @test internal_test_label_fat32_free_slot_walk_error
 * @brief A backend failure inside the FAT32 root walk surfaces out of
 *        `internal_fat_find_free_root` too.
 *
 * @details
 * Same shape as the vol-id case, one walk later: with no label entry present
 * and no free slot anywhere in the root, `internal_set_label_fat` runs
 * find_vol_id (not_found) and then find_free_root, whose own
 * `priv_dir_walk_next_sector` is the one faulted here.
 *
 * Reaching it takes one more step than the vol-id case, because
 * ra8_fs_fat_alloc.c caches a FAT sector: with a single-cluster root,
 * find_free_root's FAT read is served from what find_vol_id's walk left in
 * that cache and never reaches the backend. So the root is extended to a
 * second cluster whose FAT entry lives in a DIFFERENT FAT sector. The first
 * walk then ends with sector 1 cached, and the second walk's first FAT read
 * -- sector 0 -- is a real backend read the fault can take (skipping the one
 * the first walk made).
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` after `priv_dir_walk_next_sector` in
 * libs/ra8_fs/src/ra8_fs_fat_label.c@internal_fat_find_free_root (1
 * condition).
 * - V1: no fault -> the FAT read reports EOC -> the decision is false and the
 *   walk falls out of its loop (control; that is what
 *   internal_test_label_fat32_full_root_reports_no_mem below observes).
 * - V2: the walk's FAT read faulted -> the decision is true and the error
 *   propagates out of `ra8_fs_set_label` (this case).
 * N = 1 condition, N+1 = 2 vectors differing only in the read outcome.
 *
 * @pre The root cluster is fully occupied, so neither walk returns early.
 * @pre No volume-label entry exists, so find_free_root is reached at all.
 * @post `ra8_fs_set_label` reported the backend error.
 * @post The image still holds no volume-label entry.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_label_fat32_free_slot_walk_error(void)
{
  TEST_BEGIN("ra8_fs label: FAT32 root-walk read failure surfaces from find_free_root");
  ra8_fs_mount_t* h = internal_lb_fat32_full_root();

  /* Extend the root to a second, also-occupied cluster whose FAT entry lives
     in a different FAT sector than the first's. */
  const uint32_t far = (uint32_t)k_lb_far_cluster;
  TEST_ASSERT(far < h->count_of_clusters);
  internal_lb_fat32_set(h, h->root_cluster, far);
  internal_lb_fat32_set(h, far, (uint32_t)k_lb_fat32_eoc);
  internal_lb_fill_occupied(internal_lb_clus_off(h, far),
                            h->sectors_per_cluster * (uint32_t)k_fmt_block_size);
  TEST_ASSERT(internal_lb_fat_lba(h, far) != internal_lb_fat_lba(h, h->root_cluster));

  /* V1 (control): clean run. Both walks complete, no free slot exists, and
     find_free_root falls out of its loop reporting no_mem. */
  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_set_label(h, "NEW"));

  /* V2: fail the SECOND backend read of the root's FAT sector -- the first
     belongs to find_vol_id's walk, the second to find_free_root's. */
  internal_fault_reset();
  internal_fault_read_lba(internal_lb_fat_lba(h, h->root_cluster), (uint32_t)k_lb_skip_first);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_set_label(h, "NEW"));

  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs label: FAT32 root-walk read failure surfaces from find_free_root");
}

/**
 * @test internal_test_label_fat16_full_root_reports_no_mem
 * @brief A FAT16 fixed root with every slot occupied leaves `ra8_fs_set_label`
 *        nowhere to write, and it must say so rather than overwrite one.
 *
 * @details
 * `internal_fat_find_free_root` returns k_ra8_err_no_mem only after its walk
 * has visited every root sector without finding a 0x00 or 0xE5 slot. A
 * freshly formatted volume always has free slots, so the only way to reach
 * that return is to occupy all `root_entries` of them first -- which is what
 * this case does directly in the RAM image.
 *
 * @par MC/DC:
 * Decision: the `while (eod == 0U)` loop exit in
 * libs/ra8_fs/src/ra8_fs_fat_label.c@internal_fat_find_free_root (1
 * condition), and the exhausted-walk return that follows it.
 * - V1: a root with at least one free slot -> the loop returns from inside,
 *   the exhausted-walk return is never reached and set_label succeeds
 *   (control; the freed-slot retry at the end of this case, and
 *   internal_test_mcdc_find_free_root above).
 * - V2: every slot occupied -> the loop runs to `eod` and falls through to
 *   `return k_ra8_err_no_mem;` (this case).
 * N = 1 condition, N+1 = 2 vectors. The two differ only in whether one slot
 * is free, and they produce opposite outcomes.
 *
 * @pre The volume is formatted without a label, so no root entry exists.
 * @pre Every root slot is occupied before the call.
 * @post `ra8_fs_set_label` reported k_ra8_err_no_mem.
 * @post Freeing a single slot makes the identical call succeed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_label_fat16_full_root_reports_no_mem(void)
{
  TEST_BEGIN("ra8_fs label: a full FAT16 root reports no_mem");
  ra8_fs_mount_t* h =
    internal_fmt_mount((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, nullptr);
  const uint32_t base =
    (uint32_t)((h->partition_base_lba + h->first_root_lba) * (uint64_t)k_fmt_block_size);
  internal_lb_fill_occupied(base, h->root_entries * (uint32_t)k_lb_entry_size);

  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_set_label(h, "FULL"));

  /* Control: free the very last slot and the identical call succeeds, proving
     the refusal was the exhausted walk and not the label or the volume. */
  const uint32_t last = base + ((h->root_entries - 1U) * (uint32_t)k_lb_entry_size);
  s_disk.bytes[last + (uint32_t)k_lb_dir_name] = (uint8_t)k_lb_free_used;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "FULL"));
  internal_assert_label(h, "FULL");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs label: a full FAT16 root reports no_mem");
}

int main(void)
{
  internal_test_label_fat16_roundtrip();
  internal_test_label_fat16_default_noname();
  internal_test_label_fat32_roundtrip();
  internal_test_label_exfat_roundtrip();
  internal_test_label_get_null_guard();
  internal_test_label_set_guard();
  internal_test_mcdc_find_free_root();
  internal_test_mcdc_set_label_fat();
  internal_test_mcdc_exfat_get_label();
  internal_test_label_fat32_root_walk_error();
  internal_test_label_fat32_free_slot_walk_error();
  internal_test_label_fat16_full_root_reports_no_mem();
  return 0;
}
