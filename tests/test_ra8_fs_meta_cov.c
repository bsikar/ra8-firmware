/**
 * @file test_ra8_fs_meta_cov.c
 * @brief Error-path and edge-branch coverage for the fs metadata APIs (#678/#682).
 *
 * @details
 * The happy paths live in `test_ra8_fs_{space,label,utime}.c`; this file drives
 * the branches those cannot reach with a plain backend: the backend I/O-error
 * returns (through the fault-injecting backend in the shared fixture) and the
 * rare on-disk shapes (a long-name entry in the volume-label scan, an absent or
 * over-long exFAT label entry). Each case asserts the error or the tolerated
 * result, so a regression that stopped propagating an I/O failure -- or mangled
 * a corrupt entry -- would fail here.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_attributes.h"
#include "ra8_fs_meta.h"
#include "test_ra8_fs_format_fixture.h"

/**
 * @enum meta_cov_off_t
 * @brief On-disk offsets the poke-a-corrupt-entry cases use.
 */
typedef enum : uint32_t {
  k_mc_entry_size         = 32U,   /**< Directory entry size.                 */
  k_mc_attr_off           = 11U,   /**< FAT DIR_Attr offset.                  */
  k_mc_attr_lfn           = 0x0FU, /**< Long-name marker attribute.           */
  k_mc_first_clus         = 2U,    /**< First data cluster.                   */
  k_mc_xf_lbl_off         = 64U,   /**< exFAT root Volume Label entry offset. */
  k_mc_xf_lbl_cnt         = 1U,    /**< exFAT label CharacterCount offset.    */
  k_mc_xf_lbl_tag         = 0x83U, /**< exFAT Volume Label entry type.        */
  k_mc_bad_count          = 20U,   /**< An over-long CharacterCount (> 11).   */
  k_mc_entries_per_sector = 16U,   /**< 512 / 32 dir entries per sector.      */
} meta_cov_off_t;

/** @brief Format @p type through the plain backend, mount through the fault one. */
RA8_INTERNAL static ra8_fs_mount_t* internal_fmt_fault_mount(uint32_t blocks, ra8_fs_type_t type)
{
  internal_alloc_garbage_card(blocks);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = type;
  opts.label                = "COV";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  internal_fault_reset();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_fault_backend, &h));
  internal_fault_reset(); /* clear the read count the mount ran up */
  return h;
}

/** @brief exFAT root cluster's first byte in the image. @details Implements the bounded exfat root byte fixture step using caller-owned state. @param[in] h Value required by this filesystem vector. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static uint32_t internal_exfat_root_byte(const ra8_fs_mount_t* h)
{
  return (h->partition_base_lba + h->first_data_lba +
          ((uint64_t)(h->root_cluster - (uint32_t)k_mc_first_clus) * h->sectors_per_cluster)) *
         (uint32_t)k_fmt_block_size;
}

/**
 * @test test_cov_space_io_errors
 * @par MC/DC:
 * (no compound decision unique to this case -- it drives the backend read-error
 * returns of the FAT scan and the exFAT bitmap walk so a lost propagation is
 * caught) @brief Exercise the cov space io errors filesystem operation. @details Runs the cov space io errors vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_cov_space_io_errors(void)
{
  TEST_BEGIN("ra8_fs free_space cov: FAT scan + exFAT bitmap read errors");
  ra8_fs_space_t sp = {};

  ra8_fs_mount_t* fat = internal_fmt_fault_mount((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16);
  s_fault_read_at     = 1U; /* first FAT read fails */
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_free_space(fat, &sp));
  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(fat));
  internal_free_volume();

  ra8_fs_mount_t* exf = internal_fmt_fault_mount((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat);
  s_fault_read_at     = 1U; /* bitmap-locate read fails */
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_free_space(exf, &sp));
  internal_fault_reset();
  s_fault_read_at = 2U; /* bitmap-locate OK, then the popcount read fails */
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_free_space(exf, &sp));
  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(exf));
  internal_free_volume();
  TEST_END("ra8_fs free_space cov: FAT scan + exFAT bitmap read errors");
}

/**
 * @test test_cov_label_io_errors
 * @par MC/DC:
 * (no compound decision unique to this case -- the read/write error returns of
 * get_label and set_label on FAT) @brief Exercise the cov label io errors filesystem operation. @details Runs the cov label io errors vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_cov_label_io_errors(void)
{
  TEST_BEGIN("ra8_fs label cov: FAT get/set read + write errors");
  char out[k_ra8_fs_label_cap] = {};

  /* get_label: the volume-label scan's first read fails. */
  ra8_fs_mount_t* h = internal_fmt_fault_mount((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16);
  s_fault_read_at   = 1U;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_get_label(h, out, (uint32_t)sizeof(out)));
  internal_fault_reset();
  /* get_label: scan finds no entry (1 read, hits EOD), then the boot read fails. */
  s_fault_read_at = 2U;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_get_label(h, out, (uint32_t)sizeof(out)));
  internal_fault_reset();

  /* set_label: the boot-sector write fails. */
  s_fault_write_all = true;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_set_label(h, "X"));
  internal_fault_reset();
  /* set_label: boot write OK, but the volume-label scan read fails. */
  s_fault_read_at = 2U; /* read 1 = boot; read 2 = scan */
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_set_label(h, "X"));
  internal_fault_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs label cov: FAT get/set read + write errors");
}

/**
 * @test test_cov_label_lfn_skip
 * @par MC/DC:
 * (no compound decision unique to this case -- a long-name (0x0F) entry in the
 * root is skipped by the volume-label scan rather than mistaken for one) @brief Exercise the cov label lfn skip filesystem operation. @details Runs the cov label lfn skip vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_cov_label_lfn_skip(void)
{
  TEST_BEGIN("ra8_fs label cov: volume-label scan skips a long-name entry");
  internal_alloc_garbage_card((uint32_t)k_fmt_blocks_fat16);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;
  opts.label                = "HASLFN";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* Plant a long-name entry at root slot 0 (attr 0x0F), EOD after it. */
  const uint32_t root = (h->partition_base_lba + h->first_root_lba) * (uint32_t)k_fmt_block_size;
  memset(&s_disk.bytes[root], 'A', (size_t)k_mc_entry_size);
  s_disk.bytes[root + (uint32_t)k_mc_attr_off] = (uint8_t)k_mc_attr_lfn;
  memset(&s_disk.bytes[root + (uint32_t)k_mc_entry_size], 0, (size_t)k_mc_entry_size);

  /* The scan skips the LFN entry, finds no volume-label entry, so it falls back
   * to BS_VolLab -- the format label. */
  char out[k_ra8_fs_label_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_get_label(h, out, (uint32_t)sizeof(out)));
  TEST_ASSERT_EQ(0, strcmp(out, "HASLFN"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs label cov: volume-label scan skips a long-name entry");
}

/**
 * @test test_cov_exfat_label_edges
 * @par MC/DC:
 * (no compound decision unique to this case -- an absent exFAT label reads as
 * empty, an over-long CharacterCount is truncated, and the locate/write I/O
 * errors propagate) @brief Exercise the cov exfat label edges filesystem operation. @details Runs the cov exfat label edges vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_cov_exfat_label_edges(void)
{
  TEST_BEGIN("ra8_fs label cov: exFAT absent/over-long label + I/O errors");
  char out[k_ra8_fs_label_cap] = {};

  ra8_fs_mount_t* h = internal_fmt_fault_mount((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat);
  const uint32_t  root = internal_exfat_root_byte(h);

  /* Absent label entry: zeroing it makes the scan hit EOD -> empty string. */
  memset(&s_disk.bytes[root + (uint32_t)k_mc_xf_lbl_off], 0, (size_t)k_mc_entry_size);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_get_label(h, out, (uint32_t)sizeof(out)));
  TEST_ASSERT_EQ(0, strcmp(out, ""));

  /* Recreate a label with an OVER-LONG CharacterCount: get_label truncates. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "ABCDEFGHIJK")); /* 11 chars */
  s_disk.bytes[root + (uint32_t)k_mc_xf_lbl_off + (uint32_t)k_mc_xf_lbl_cnt] =
    (uint8_t)k_mc_bad_count;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_get_label(h, out, (uint32_t)sizeof(out)));
  TEST_ASSERT_EQ(k_ra8_fs_label_cap - 1U, strlen(out)); /* clamped to 11 */

  /* locate read error, and label write error. */
  s_fault_read_at = 1U;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_get_label(h, out, (uint32_t)sizeof(out)));
  internal_fault_reset();
  s_fault_write_all = true;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_set_label(h, "Y"));
  internal_fault_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs label cov: exFAT absent/over-long label + I/O errors");
}

/**
 * @test test_cov_utime_io_errors
 * @par MC/DC:
 * (no compound decision unique to this case -- utime's resolve, read and write
 * error returns on FAT, and the find-set / write error returns on exFAT) @brief Exercise the cov utime io errors filesystem operation. @details Runs the cov utime io errors vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_cov_utime_io_errors(void)
{
  TEST_BEGIN("ra8_fs utime cov: FAT + exFAT resolve/read/write errors");
  static const uint8_t    body[4] = {'x', 'y', 'z', 'w'};
  const ra8_fs_datetime_t when    = {.year = 2020U, .month = 1U, .day = 1U};

  /* FAT: a nested path whose parent is missing -> not_found (resolve error). */
  ra8_fs_mount_t* fat = internal_fmt_fault_mount((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(fat, "U.BIN", body, (uint32_t)sizeof(body)));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_utime(fat, "/NODIR/x", &when, nullptr, nullptr));
  /* read error while reading the entry's sector. */
  s_fault_read_at = 1U;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_utime(fat, "U.BIN", &when, nullptr, nullptr));
  internal_fault_reset();
  /* write-back error. */
  s_fault_write_all = true;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_utime(fat, "U.BIN", &when, nullptr, nullptr));
  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(fat));
  internal_free_volume();

  /* exFAT: find-set read error, and write error. */
  ra8_fs_mount_t* exf = internal_fmt_fault_mount((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(exf, "V.BIN", body, (uint32_t)sizeof(body)));
  s_fault_read_at = 1U;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_utime(exf, "V.BIN", &when, nullptr, nullptr));
  internal_fault_reset();
  s_fault_write_all = true;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_utime(exf, "V.BIN", &when, nullptr, nullptr));
  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(exf));
  internal_free_volume();
  TEST_END("ra8_fs utime cov: FAT + exFAT resolve/read/write errors");
}

/**
 * @test test_cov_label_full_sector
 * @par MC/DC:
 * (no compound decision unique to this case -- 16 files fill the root's first
 * sector so the volume-label scan and the free-slot scan must walk into the
 * second sector, reaching the multi-sector branches) @brief Exercise the cov label full sector filesystem operation. @details Runs the cov label full sector vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_cov_label_full_sector(void)
{
  TEST_BEGIN("ra8_fs label cov: scans walk into the second root sector");
  internal_alloc_garbage_card((uint32_t)k_fmt_blocks_fat16);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;
  opts.label                = "FULLSEC";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  static const uint8_t body[2] = {'z', 'z'};
  for (uint32_t i = 0U; i < (uint32_t)k_mc_entries_per_sector; i++) {
    char name[16] = {};
    (void)snprintf(name, sizeof(name), "F%u.BIN", (unsigned)i);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, name, body, (uint32_t)sizeof(body)));
  }
  /* Sector 0 is full of files: the volume-label scan walks to sector 1, finds
   * no label entry, and falls back to BS_VolLab. */
  char out[k_ra8_fs_label_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_get_label(h, out, (uint32_t)sizeof(out)));
  TEST_ASSERT_EQ(0, strcmp(out, "FULLSEC"));
  /* set_label creates a new entry: the free-slot scan also walks to sector 1. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(h, "WALKED"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_get_label(h, out, (uint32_t)sizeof(out)));
  TEST_ASSERT_EQ(0, strcmp(out, "WALKED"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs label cov: scans walk into the second root sector");
}

/**
 * @test test_cov_label_set_deep_io
 * @par MC/DC:
 * (no compound decision unique to this case -- the read/write error returns
 * buried in set_label's boot write, free-slot scan, entry create and entry
 * delete steps, reached by failing the Nth backend read) @brief Exercise the cov label set deep io filesystem operation. @details Runs the cov label set deep io vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_cov_label_set_deep_io(void)
{
  TEST_BEGIN("ra8_fs label cov: set_label deep read errors (create + clear)");

  /* No existing entry: boot read fails first. */
  ra8_fs_mount_t* h = internal_fmt_fault_mount((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16);
  s_fault_read_at   = 1U;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_set_label(h, "X")); /* boot_set_label read */
  internal_fault_reset();
  /* boot read+write OK, vol-id scan OK (EOD), free-slot scan read fails. */
  s_fault_read_at = 3U;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_set_label(h, "X")); /* find_free_root read */
  internal_fault_reset();
  /* ...and one read further, the fresh-entry write's read fails. */
  s_fault_read_at = 4U;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_set_label(h, "X")); /* put_vol_id read */
  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();

  /* Now with an EXISTING vol-id entry, exercise the rewrite and clear reads. */
  internal_alloc_garbage_card((uint32_t)k_fmt_blocks_fat16);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;
  opts.label                = "HAVE";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* p = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &p));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_set_label(p, "HAVE")); /* create the root entry */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(p));
  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_fault_backend, &p));
  internal_fault_reset();
  /* rewrite: boot(1) OK, vol-id scan(2) finds it, put_vol_id read(3) fails. */
  s_fault_read_at = 3U;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_set_label(p, "NEW"));
  internal_fault_reset();
  /* clear: boot(1) OK, vol-id scan(2) finds it, del_entry read(3) fails. */
  s_fault_read_at = 3U;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_set_label(p, nullptr));
  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(p));
  internal_free_volume();
  TEST_END("ra8_fs label cov: set_label deep read errors (create + clear)");
}

/**
 * @test test_cov_setattr_io_errors
 * @par MC/DC:
 * (no compound decision unique to this case -- set_attr's resolve, read and
 * write error returns on FAT, and the find-set / write error returns on exFAT;
 * the mask-validation compound decision is driven in `test_ra8_fs_attr.c`) @brief Exercise the cov setattr io errors filesystem operation. @details Runs the cov setattr io errors vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_cov_setattr_io_errors(void)
{
  TEST_BEGIN("ra8_fs set_attr cov: FAT + exFAT resolve/read/write errors");
  static const uint8_t body[4] = {'x', 'y', 'z', 'w'};
  const uint8_t        arc     = (uint8_t)k_ra8_fs_attr_archive;

  /* FAT: a nested path whose parent is missing -> not_found (resolve error). */
  ra8_fs_mount_t* fat = internal_fmt_fault_mount((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(fat, "U.BIN", body, (uint32_t)sizeof(body)));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_set_attr(fat, "/NODIR/x", arc, 0U));
  /* read error while reading the entry's sector. */
  s_fault_read_at = 1U;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_set_attr(fat, "U.BIN", arc, 0U));
  internal_fault_reset();
  /* write-back error. */
  s_fault_write_all = true;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_set_attr(fat, "U.BIN", arc, 0U));
  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(fat));
  internal_free_volume();

  /* exFAT: a missing name (not_found), then find-set read error and write error. */
  ra8_fs_mount_t* exf = internal_fmt_fault_mount((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(exf, "V.BIN", body, (uint32_t)sizeof(body)));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_set_attr(exf, "NOPE.BIN", arc, 0U));
  s_fault_read_at = 1U;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_set_attr(exf, "V.BIN", arc, 0U));
  internal_fault_reset();
  s_fault_write_all = true;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_set_attr(exf, "V.BIN", arc, 0U));
  internal_fault_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(exf));
  internal_free_volume();
  TEST_END("ra8_fs set_attr cov: FAT + exFAT resolve/read/write errors");
}

int main(void)
{
  internal_test_cov_space_io_errors();
  internal_test_cov_label_io_errors();
  internal_test_cov_label_lfn_skip();
  internal_test_cov_label_full_sector();
  internal_test_cov_label_set_deep_io();
  internal_test_cov_exfat_label_edges();
  internal_test_cov_utime_io_errors();
  internal_test_cov_setattr_io_errors();
  return 0;
}
