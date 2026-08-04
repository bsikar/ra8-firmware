/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_fs_format_exfat.c
 * @brief exFAT formatter tests: format, partition table, up-case, mount.
 *
 * @details
 * The exFAT half of the formatter suite, split from
 * ``tests/test_ra8_fs_format.c`` for the 1000-line file-size cap. It formats a
 * RAM-backed card through the public ``ra8_fs_format()`` API and proves the
 * result by mounting it, and it covers the PC-standard partition table, the
 * cluster-size cascade, the too-small rejection and the up-case write-failure
 * abort. The shared disk fixture lives in
 * ``tests/test_ra8_fs_format_fixture.h``.
 *
 * exFAT file creation / rename / unlink is covered separately by
 * ``tests/host/exfat_fs_test.c`` and the ``fs_format_mount`` HIL. The produced
 * image is independently validated fsck.exfat-clean.
 */

#include "test_ra8_fs_format_fixture.h"

/**
 * @test test_format_exfat_mount_empty
 *
 * @brief Format an exFAT volume, mount it, and confirm an empty root.
 *
 * @details Focuses on the mkfs result itself: format, mount, assert the
 *          detected type is exFAT, list the root (the bitmap / up-case / label
 *          system entries must NOT surface as files -> empty), unmount. exFAT
 *          file creation / rename / unlink on top of this image is covered
 *          separately by `tests/host/exfat_fs_test.c` and the `fs_format_mount`
 *          HIL, so it is intentionally out of scope here. The on-disk image this
 *          produces is fsck.exfat-clean (validated out-of-band against macOS
 *          fsck_exfat).
 *
 * @par MC/DC:
 * (no compound decision is uniquely touched -- it exercises the exFAT format +
 * mount + empty-root happy path end to end; the format argument/capacity
 * compounds are passed with control inputs and are covered by the dedicated
 * MC/DC cases above, and the exFAT cluster-size cascade is covered by
 * test_exfat_spc_shift_tiers)
 */
static void test_format_exfat_mount_empty(void)
{
  TEST_BEGIN("ra8_fs format: exFAT format + mount + empty root");
  alloc_garbage_card((uint32_t)k_fmt_blocks_exfat);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_exfat;
  opts.label                = "RAEXFAT";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));

  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_fs_type_exfat, h->type);

  list_ctx_t ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", count_cb, &ctx));
  TEST_ASSERT_EQ(0, ctx.count); /* freshly formatted -> no files */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs format: exFAT format + mount + empty root");
}

/**
 * @test test_exfat_spc_shift_tiers
 *
 * @brief exFAT must pick a size-appropriate cluster size per the MS table.
 *
 * @par MC/DC:
 * `priv_exfat_spc_shift` is a 4-way size cascade. One mid-tier capacity per
 * branch (64 MB / 1 / 64 / 512 GB) is formatted through the write-sink (which
 * reports a huge capacity without a multi-GB malloc and captures the exFAT VBR
 * sector at LBA 2048), and the VBR's SectorsPerClusterShift byte is asserted:
 * 3 / 6 / 8 / 9. Subtracting the 2048-sector partition offset does not move any
 * capacity out of its cluster-size tier, so the expected shifts are unchanged.
 */
static void test_exfat_spc_shift_tiers(void)
{
  TEST_BEGIN("ra8_fs format: exFAT SectorsPerClusterShift tiers");
  static const uint32_t k_blocks[] = {
    (uint32_t)k_exfat_blk_64m,
    (uint32_t)k_exfat_blk_1g,
    (uint32_t)k_exfat_blk_64g,
    (uint32_t)k_exfat_blk_512g,
  };
  static const uint8_t k_want[] = {
    (uint8_t)k_exfat_spc_64m,
    (uint8_t)k_exfat_spc_1g,
    (uint8_t)k_exfat_spc_64g,
    (uint8_t)k_exfat_spc_512g,
  };
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_exfat;
  opts.label                = "TIER";
  const uint32_t n          = (uint32_t)(sizeof(k_blocks) / sizeof(k_blocks[0]));
  for (uint32_t i = 0U; i < n; i++) {
    s_sink_blocks = k_blocks[i];
    memset(s_sink_vbr, 0, sizeof s_sink_vbr);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_sink_backend, &opts));
    TEST_ASSERT_EQ(k_want[i], s_sink_vbr[(uint32_t)k_exfat_de_spc_off]);
  }
  TEST_END("ra8_fs format: exFAT SectorsPerClusterShift tiers");
}

/**
 * @enum ra8_fs_exfat_mbr_off_t
 * @brief MBR / VBR byte offsets the partition-table test inspects.
 */
typedef enum : uint32_t {
  k_mbr_pe_type_off    = 450U,    /**< Partition-0 type byte (446 + 4).         */
  k_mbr_pe_lba_off     = 454U,    /**< Partition-0 first-LBA (446 + 8, 4 B LE). */
  k_mbr_sig_lo_off     = 510U,    /**< Boot-signature low byte.                 */
  k_mbr_sig_hi_off     = 511U,    /**< Boot-signature high byte.                */
  k_mbr_type_exfat     = 0x07U,   /**< exFAT/NTFS partition type.               */
  k_mbr_sig_lo_val     = 0x55U,   /**< Boot signature low.                      */
  k_mbr_sig_hi_val     = 0xAAU,   /**< Boot signature high.                     */
  k_vbr_partoff_off    = 64U,     /**< VBR PartitionOffset field.               */
  k_vbr_vollen_off     = 72U,     /**< VBR VolumeLength field.                  */
  k_vbr_fsname_off     = 3U,      /**< VBR "EXFAT   " filesystem-name field.    */
  k_exfat_small_blk    = 131072U, /**< 64 MiB RAM card for the layout probe.    */
  k_exfat_toosmall_blk = 66048U,  /**< 2048 + 64000: below part+min sectors.    */
} ra8_fs_exfat_mbr_off_t;

/** @brief Bit positions of each byte in a little-endian 32-bit field. */
typedef enum : uint8_t {
  k_exfat_le_shift_b1 = 8U,  /**< Second byte. */
  k_exfat_le_shift_b2 = 16U, /**< Third byte.  */
  k_exfat_le_shift_b3 = 24U, /**< Fourth byte. */
} exfat_le_shift_t;

/** @brief Read a little-endian u32 from the RAM disk at absolute byte offset. */
static uint32_t disk_rd32(size_t byte_off)
{
  const uint8_t* p = &s_disk.bytes[byte_off];
  return (uint32_t)p[0] | ((uint32_t)p[1] << (uint32_t)k_exfat_le_shift_b1) |
         ((uint32_t)p[2] << (uint32_t)k_exfat_le_shift_b2) |
         ((uint32_t)p[3] << (uint32_t)k_exfat_le_shift_b3);
}

/**
 * @test test_format_exfat_partition_table
 *
 * @brief A formatted exFAT card carries a real MBR at LBA 0 with a type-0x07
 *        partition at LBA 2048, and the exFAT VBR lives inside that partition.
 *
 * @details Regression for the whole-disk (superfloppy) layout that a PC would
 *          not see as a partitioned removable disk. Formats a 64 MiB RAM card,
 *          then inspects sector 0 (MBR: partition type 0x07, first-LBA 2048,
 *          0x55AA signature) and the sector at LBA 2048 (VBR: "EXFAT   " name,
 *          PartitionOffset == 2048, VolumeLength == capacity - 2048). Finally
 *          mounts it and confirms the mount followed the MBR to base LBA 2048.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- it asserts the on-disk MBR + VBR
 * field layout the formatter now writes; the format-path compounds are covered
 * by the dedicated MC/DC cases above)
 */
static void test_format_exfat_partition_table(void)
{
  TEST_BEGIN("ra8_fs format: exFAT MBR partition table + VBR at LBA 2048");
  alloc_garbage_card((uint32_t)k_exfat_small_blk);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_exfat;
  opts.label                = "PARTVOL";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));

  /* MBR at LBA 0. */
  TEST_ASSERT_EQ(k_mbr_type_exfat, s_disk.bytes[(size_t)k_mbr_pe_type_off]);
  TEST_ASSERT_EQ(k_exfat_test_part_lba, disk_rd32((size_t)k_mbr_pe_lba_off));
  TEST_ASSERT_EQ(k_mbr_sig_lo_val, s_disk.bytes[(size_t)k_mbr_sig_lo_off]);
  TEST_ASSERT_EQ(k_mbr_sig_hi_val, s_disk.bytes[(size_t)k_mbr_sig_hi_off]);

  /* VBR inside the partition (LBA 2048). */
  const size_t vbr = (size_t)k_exfat_test_part_lba * (uint32_t)k_fmt_block_size;
  TEST_ASSERT_EQ(0, memcmp(&s_disk.bytes[vbr + (uint32_t)k_vbr_fsname_off], "EXFAT   ", 8));
  TEST_ASSERT_EQ(k_exfat_test_part_lba, disk_rd32(vbr + (uint32_t)k_vbr_partoff_off));
  TEST_ASSERT_EQ(k_exfat_small_blk - (uint32_t)k_exfat_test_part_lba,
                 disk_rd32(vbr + (uint32_t)k_vbr_vollen_off));

  /* Mount must follow the MBR to the partition base. */
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_fs_type_exfat, h->type);
  TEST_ASSERT_EQ(k_exfat_test_part_lba, h->partition_base_lba);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs format: exFAT MBR partition table + VBR at LBA 2048");
}

/**
 * @test test_format_exfat_too_small
 *
 * @brief A card too small to hold the partition offset plus a minimum exFAT
 *        volume is rejected without writing anything.
 *
 * @details Drives the ``total_sectors < part_lba + min_sectors`` guard in
 *          ``priv_exfat_format``: a 66048-sector card (2048 + 64000) is one
 *          minimum-volume short of the 2048 + 65536 floor, so the format must
 *          return ``k_ra8_err_not_supported``.
 *
 * @par MC/DC:
 * (single-condition guard -- this vector drives the true (reject) leg; the
 * false (accept) leg is driven by every successful exFAT format above)
 */
static void test_format_exfat_too_small(void)
{
  TEST_BEGIN("ra8_fs format: exFAT rejects a card below part+min sectors");
  alloc_garbage_card((uint32_t)k_exfat_toosmall_blk);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_exfat;
  opts.label                = "TINY";
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_fs_format(&s_backend, &opts));
  free_volume();
  TEST_END("ra8_fs format: exFAT rejects a card below part+min sectors");
}

/**
 * @test test_format_exfat_partition_roundtrip
 *
 * @brief Whole-file create / read-back / unlink works on the partitioned exFAT
 *        volume the formatter now writes.
 *
 * @details Formats a 64 MiB RAM card, mounts it (exercising the MBR -> partition
 *          retarget), writes a multi-cluster file with ``ra8_fs_write_file``,
 *          reads it back byte-for-byte, then unlinks it and confirms the root is
 *          empty again -- proving the up-case / bitmap / FAT the formatter laid
 *          inside the partition drive the exFAT write path correctly.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- it is an end-to-end exFAT
 * create/read/unlink cycle on the partitioned volume)
 */
static void test_format_exfat_partition_roundtrip(void)
{
  TEST_BEGIN("ra8_fs format: exFAT partitioned create/read/unlink round-trip");
  alloc_garbage_card((uint32_t)k_exfat_small_blk);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_exfat;
  opts.label                = "RTRIP";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));

  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_fs_type_exfat, h->type);

  static uint8_t s_wr[k_fmt_payload_bytes];
  static uint8_t s_rd[k_fmt_payload_bytes];
  for (uint32_t i = 0U; i < (uint32_t)k_fmt_payload_bytes; i++) {
    s_wr[i] = (uint8_t)((i * k_fs_pattern_stride) + k_fs_pattern_bias);
    s_rd[i] = 0U;
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "BOOK.BIN", s_wr, (uint32_t)k_fmt_payload_bytes));

  ra8_fs_file_t* f   = nullptr;
  uint32_t       got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "BOOK.BIN", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, s_rd, (uint32_t)k_fmt_payload_bytes, &got));
  TEST_ASSERT_EQ(k_fmt_payload_bytes, got);
  TEST_ASSERT_EQ(0, memcmp(s_wr, s_rd, (size_t)k_fmt_payload_bytes));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "BOOK.BIN"));
  list_ctx_t ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", count_cb, &ctx));
  TEST_ASSERT_EQ(0, ctx.count);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs format: exFAT partitioned create/read/unlink round-trip");
}

/** @brief Backend write that fails on the up-case table's first sector.
 * @details The up-case table sector uniquely begins 0x00 0x00 0x01 0x00 (the
 *          identity mapping of code points 0x0000 and 0x0001), unlike the MBR,
 *          boot, FAT, bitmap, root, or all-zero clear-region writes -- so this
 *          fails ``ra8_fs_write_file``'s format at exactly the up-case write. */
static ra8_err_t upcase_fail_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  if (count >= 1U && buf[0] == 0x00U && buf[1] == 0x00U && buf[2] == 0x01U && buf[3] == 0x00U) {
    return k_ra8_err_hw_error;
  }
  return mem_write(ctx, lba, count, buf);
}

static const ra8_fs_backend_t s_backend_upcase_fail = {
  .read_block   = mem_read,
  .write_block  = upcase_fail_write,
  .get_capacity = mem_capacity,
  .ctx          = &s_disk,
};

/**
 * @test test_format_exfat_upcase_write_fail
 *
 * @brief A backend write failure during the up-case table write aborts the
 *        format and surfaces the backend error.
 *
 * @details Drives the ``if (err != k_ra8_ok) return err;`` error path inside
 *          ``priv_exfat_write_upcase``: the injected backend fails the first
 *          up-case sector write (identified by its 0x00 0x00 0x01 0x00 prefix),
 *          so ``ra8_fs_format`` must return the backend's error rather than
 *          k_ra8_ok. The success legs of that loop are covered by every passing
 *          exFAT format above.
 *
 * @par MC/DC:
 * (single-condition backend-error branch -- this vector drives its true leg;
 * the false leg is driven by the successful up-case writes elsewhere)
 */
static void test_format_exfat_upcase_write_fail(void)
{
  TEST_BEGIN("ra8_fs format: exFAT up-case write failure aborts the format");
  alloc_garbage_card((uint32_t)k_exfat_small_blk);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_exfat;
  opts.label                = "UPFAIL";
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_format(&s_backend_upcase_fail, &opts));
  free_volume();
  TEST_END("ra8_fs format: exFAT up-case write failure aborts the format");
}

int32_t main(void)
{
  test_format_exfat_mount_empty();
  test_exfat_spc_shift_tiers();
  test_format_exfat_partition_table();
  test_format_exfat_too_small();
  test_format_exfat_partition_roundtrip();
  test_format_exfat_upcase_write_fail();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_format_exfat.c\n");
  return 0;
}
