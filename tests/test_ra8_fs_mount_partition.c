/**
 * @file test_ra8_fs_mount_partition.c
 * @brief Partition-indexed mount coverage for libs/ra8_fs/src/ra8_fs_fat_mount.c.
 *
 * @details
 * Drives ra8_fs_mount_partition() -- the indexed sibling of ra8_fs_mount() --
 * across the MBR and GPT selection paths. The formatter only ever lays down a
 * single-partition volume, so these tests build MULTI-partition images by hand:
 * two real FAT16 volumes are formatted into offset sub-regions of one RAM card,
 * then an MBR partition table (or a protective MBR + GPT entry array) is written
 * at the front pointing each entry at its volume. Selecting a partition by index
 * must therefore land on the right base LBA and read back that partition's own
 * files, with partition-relative addressing throughout.
 *
 * Scenarios:
 *   - MBR: mount partition 1 of a two-partition image; then partition 0.
 *   - MBR: out-of-range index, empty entry, and a zero-LBA entry all error.
 *   - GPT: two Basic Data partitions, selecting the SECOND by index.
 *   - GPT: out-of-range, empty, 64-bit-LBA and zero-LBA entries all error.
 *   - GPT: a malformed header and an entry sector beyond the disk both error.
 *   - No partition table (bad boot signature) errors cleanly on any index.
 *   - A superfloppy still mounts via the default ra8_fs_mount().
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
#include "support/fs_fat_mount_test_util.h"
#include "unity_minimal.h"

/**
 * @enum mp_geom_t
 * @brief Multi-partition RAM-card geometry (in 512-byte sectors).
 *
 * @details Each partition is one FAT16 band (8192 sectors = 4 MiB) so the real
 *          formatter selects FAT16. Partition 0 starts one aligned mebibyte in;
 *          partition 1 immediately follows; the card holds both.
 * @invariant k_mp_base1 == k_mp_base0 + k_mp_part_sectors.
 * @invariant k_mp_card_sectors == k_mp_base1 + k_mp_part_sectors.
 */
typedef enum : uint32_t {
  k_mp_part_sectors = 8192U,  /**< One FAT16 partition (4 MiB).           */
  k_mp_base0        = 2048U,  /**< Partition 0 first LBA (1 MiB aligned). */
  k_mp_base1        = 10240U, /**< Partition 1 first LBA (base0 + 8192).  */
  k_mp_card_sectors = 18432U, /**< Whole card (base1 + 8192).             */
  k_mp_small_card   = 4U,     /**< Table-only card: LBA 0-3, no volumes.  */
} mp_geom_t;

/**
 * @enum mp_off_t
 * @brief On-disk offsets and field values for the hand-built partition tables.
 */
typedef enum : uint16_t {
  k_mp_mbr_stride      = 16U,   /**< Bytes between MBR primary entries.      */
  k_mp_mbr_type_fat16  = 0x0EU, /**< MBR type byte: FAT16 LBA (non-zero).    */
  k_mp_gpt_stride      = 128U,  /**< Bytes per GPT partition entry.          */
  k_mp_gpt_ent_lba     = 0x20U, /**< first_lba low word within a GPT entry.  */
  k_mp_gpt_ent_lba_hi  = 0x24U, /**< first_lba high word within a GPT entry. */
  k_mp_gpt_hdr_lba     = 2U,    /**< Entry array first LBA (header field).   */
  k_mp_gpt_hdr_count   = 4U,    /**< Entry count advertised in the header.   */
  k_mp_gpt_entry_bytes = 128U,  /**< Standard GPT entry size.                */
  k_mp_read_buf        = 32U,   /**< Read-back scratch for the marker files. */
} mp_off_t;

/**
 * @brief Microsoft Basic Data type GUID, on-disk byte order.
 * @details Mirrors ra8_fs's k_gpt_guid_basic_data
 *          (EBD0A0A2-B9E5-4433-87C0-68B6B72699C7).
 */
static const uint8_t k_mp_basic_guid[16] = {
  0xA2U,
  0xA0U,
  0xD0U,
  0xEBU,
  0xE5U,
  0xB9U,
  0x33U,
  0x44U,
  0x87U,
  0xC0U,
  0x68U,
  0xB6U,
  0xB7U,
  0x26U,
  0x99U,
  0xC7U,
};

/**
 * @struct sub_dev_t
 * @brief A base-offset window onto ::s_disk, so a whole FAT volume can be
 *        formatted inside one partition of the shared card.
 */
typedef struct {
  uint32_t base;    /**< First card LBA of this partition. */
  uint32_t sectors; /**< Partition length in sectors.      */
} sub_dev_t;

static ra8_err_t sub_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  const sub_dev_t* d = (const sub_dev_t*)ctx;
  if (lba + count > d->sectors) {
    return k_ra8_err_out_of_range;
  }
  return mc_read((void*)&s_disk, lba + d->base, count, buf);
}

static ra8_err_t sub_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
{
  const sub_dev_t* d = (const sub_dev_t*)ctx;
  if (lba + count > d->sectors) {
    return k_ra8_err_out_of_range;
  }
  return mc_write((void*)&s_disk, lba + d->base, count, buf);
}

static ra8_err_t sub_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  const sub_dev_t* d = (const sub_dev_t*)ctx;
  *block_count       = d->sectors;
  *block_size        = (uint32_t)k_mc_blk;
  return k_ra8_ok;
}

/**
 * @struct names_t
 * @brief Listdir tally proving which partition a mount landed on.
 */
typedef struct {
  uint32_t count;  /**< Visible entries seen.           */
  int      saw_p0; /**< "P0.TXT" / "G0.TXT" was listed. */
  int      saw_p1; /**< "P1.TXT" / "G1.TXT" was listed. */
} names_t;

static void names_cb(const char* name, uint8_t attr, uint64_t size, void* ctx)
{
  (void)attr;
  (void)size;
  names_t* n = (names_t*)ctx;
  n->count++;
  if (strcmp(name, "P0.TXT") == 0 || strcmp(name, "G0.TXT") == 0) {
    n->saw_p0 = 1;
  }
  if (strcmp(name, "P1.TXT") == 0 || strcmp(name, "G1.TXT") == 0) {
    n->saw_p1 = 1;
  }
}

/**
 * @brief Format a FAT16 volume at @p base and drop one marker file into it.
 *
 * @details Uses a ::sub_dev_t offset window so the real formatter writes a bare
 *          FAT16 superfloppy at the partition's first LBA -- exactly what an
 *          indexed whole-card mount later follows the table back to.
 */
static void seed_partition(uint32_t base, const char* fname, const char* text)
{
  sub_dev_t              d    = {.base = base, .sectors = (uint32_t)k_mp_part_sectors};
  const ra8_fs_backend_t be   = {.read_block   = sub_read,
                                 .write_block  = sub_write,
                                 .get_capacity = sub_capacity,
                                 .ctx          = &d};
  ra8_fs_format_opts_t   opts = {};
  opts.type                   = k_ra8_fs_type_fat16;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&be, &opts));

  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&be, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, fname, k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, (const uint8_t*)text, (uint32_t)strlen(text)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
}

/** @brief Read @p name back off @p h and byte-compare it against @p expected. */
static void assert_file_bytes(ra8_fs_mount_t* h, const char* name, const char* expected)
{
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, name, k_ra8_fs_mode_read, &f));
  uint8_t        buf[k_mp_read_buf] = {};
  uint32_t       got                = 0U;
  const uint32_t want               = (uint32_t)strlen(expected);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, buf, want, &got));
  TEST_ASSERT_EQ(want, got);
  TEST_ASSERT_EQ(0, memcmp(buf, expected, (size_t)want));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
}

/** @brief Write MBR primary entry @p idx (type byte @p type, first LBA @p lba). */
static void write_mbr_entry(uint8_t* mbr, uint8_t idx, uint8_t type, uint32_t lba)
{
  const uint32_t slot                       = (uint32_t)idx * (uint32_t)k_mp_mbr_stride;
  mbr[(uint32_t)k_mbr_off_part_type + slot] = type;
  put32(mbr, (uint32_t)k_mbr_off_part_lba + slot, lba);
}

/** @brief Write a GPT entry @p idx with a Microsoft Basic Data GUID at @p lba. */
static void write_gpt_basic_entry(uint8_t* sector, uint8_t idx, uint32_t lba)
{
  const uint32_t slot = (uint32_t)idx * (uint32_t)k_mp_gpt_stride;
  memcpy(&sector[slot], k_mp_basic_guid, sizeof(k_mp_basic_guid));
  put32(sector, slot + (uint32_t)k_mp_gpt_ent_lba, lba);
}

/** @brief Lay the fixed protective MBR + GPT header used by the GPT tests. */
static uint8_t* gpt_prologue(uint32_t count)
{
  write_protective_mbr(s_disk.bytes, (uint32_t)k_mp_gpt_hdr_lba);
  uint8_t* lba1 = &s_disk.bytes[(uint32_t)k_mc_blk];
  write_gpt_header(lba1, (uint32_t)k_mp_gpt_hdr_lba, 0U, count, (uint32_t)k_mp_gpt_entry_bytes);
  return &s_disk.bytes[(size_t)2U * (uint32_t)k_mc_blk];
}

/* ===========================================================================
 * MBR: select a chosen primary partition
 * ===========================================================================
 */

/**
 * @test test_mbr_mount_partition_one
 * @brief ra8_fs_mount_partition() reaches the SECOND MBR primary partition, and
 *        the first, each with its own files.
 *
 * @details Two FAT16 volumes are formatted at k_mp_base0 / k_mp_base1 with
 *          distinct marker files, then an MBR names entry 0 and entry 1. Mount
 *          index 1 must land on base1 and see only "P1.TXT"; index 0 on base0
 *          and only "P0.TXT" -- proving the index, not first-match, chose.
 *
 * @par MC/DC:
 * Decision: `if (index != k_ra8_fs_partition_auto)` in priv_read_boot_sector.
 * - V1: index = auto -> false -> superfloppy path (the default-mount tests).
 * - V2: index = 1 / 0 -> true -> indexed path (covered here).
 *
 * @pre s_disk is nullptr.
 * @post s_disk freed; no mount left active.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_mbr_mount_partition_one(void)
{
  TEST_BEGIN("ra8_fs_mount_partition: MBR selects partition 1 then 0");
  alloc_disk((uint32_t)k_mp_card_sectors);
  seed_partition((uint32_t)k_mp_base0, "P0.TXT", "partition-zero");
  seed_partition((uint32_t)k_mp_base1, "P1.TXT", "partition-one");
  uint8_t* mbr        = s_disk.bytes;
  mbr[k_bpb_off_sig0] = (uint8_t)k_bpb_sig0_val;
  mbr[k_bpb_off_sig1] = (uint8_t)k_bpb_sig1_val;
  write_mbr_entry(mbr, 0U, (uint8_t)k_mp_mbr_type_fat16, (uint32_t)k_mp_base0);
  write_mbr_entry(mbr, 1U, (uint8_t)k_mp_mbr_type_fat16, (uint32_t)k_mp_base1);

  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount_partition(&s_backend, 1U, &h));
  TEST_ASSERT_EQ(k_mp_base1, h->partition_base_lba);
  names_t n1 = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", names_cb, &n1));
  TEST_ASSERT_EQ(1, n1.count);
  TEST_ASSERT_EQ(1, n1.saw_p1);
  TEST_ASSERT_EQ(0, n1.saw_p0);
  assert_file_bytes(h, "P1.TXT", "partition-one");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount_partition(&s_backend, 0U, &h));
  TEST_ASSERT_EQ(k_mp_base0, h->partition_base_lba);
  assert_file_bytes(h, "P0.TXT", "partition-zero");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_disk();
  TEST_END("ra8_fs_mount_partition: MBR selects partition 1 then 0");
}

/**
 * @test test_mbr_index_errors
 * @brief An out-of-range index, an unused entry, and a zero-LBA entry each
 *        fail with a distinct error.
 *
 * @details priv_mbr_select_entry rejects index >= 4 (out_of_range), an entry
 *          whose type byte is 0 (not_found), and a populated entry whose first
 *          LBA is 0 (validation_failed).
 *
 * @par MC/DC:
 * Three single-condition decisions in priv_mbr_select_entry:
 * - `index >= k_mbr_part_entry_count` : V-true = 4 (here), V-false = 0/1/2.
 * - `buf[type_off] == 0`              : V-true = entry 1 (here), V-false = 0.
 * - `base == 0`                       : V-true = entry 2 (here), V-false = 0.
 *
 * @pre s_disk is nullptr.
 * @post s_disk freed.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_mbr_index_errors(void)
{
  TEST_BEGIN("ra8_fs_mount_partition: MBR index errors");
  alloc_disk((uint32_t)k_mp_small_card);
  uint8_t* mbr        = s_disk.bytes;
  mbr[k_bpb_off_sig0] = (uint8_t)k_bpb_sig0_val;
  mbr[k_bpb_off_sig1] = (uint8_t)k_bpb_sig1_val;
  /* Entry 0 present (but no volume behind it); entry 1 unused; entry 2 zero-LBA. */
  write_mbr_entry(mbr, 0U, (uint8_t)k_mp_mbr_type_fat16, 3U);
  write_mbr_entry(mbr, 2U, (uint8_t)k_mp_mbr_type_fat16, 0U);

  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_fs_mount_partition(&s_backend, 4U, &h));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_mount_partition(&s_backend, 1U, &h));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_fs_mount_partition(&s_backend, 2U, &h));
  free_disk();
  TEST_END("ra8_fs_mount_partition: MBR index errors");
}

/* ===========================================================================
 * GPT: select a chosen entry-array partition
 * ===========================================================================
 */

/**
 * @test test_gpt_select_second_basic_data
 * @brief On a GPT disk with two Basic Data partitions, index 1 selects the
 *        SECOND, not the first-match the auto path would pick.
 *
 * @details Two FAT16 volumes at k_mp_base0 / k_mp_base1 are named by GPT entries
 *          0 and 1 (both Basic Data). ra8_fs_mount() would always take entry 0;
 *          ra8_fs_mount_partition(1) must instead land on base1 and see only
 *          "G1.TXT".
 *
 * @par MC/DC:
 * Decision: `if (s_scratch[k_mbr_off_part0_type] == k_gpt_part_type_protective)`
 * in priv_locate_indexed.
 * - V-true : type 0xEE -> GPT path (covered here).
 * - V-false: FAT16 type -> MBR path (test_mbr_mount_partition_one).
 *
 * @pre s_disk is nullptr.
 * @post s_disk freed; no mount left active.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_gpt_select_second_basic_data(void)
{
  TEST_BEGIN("ra8_fs_mount_partition: GPT selects the second Basic Data volume");
  alloc_disk((uint32_t)k_mp_card_sectors);
  seed_partition((uint32_t)k_mp_base0, "G0.TXT", "gpt-zero");
  seed_partition((uint32_t)k_mp_base1, "G1.TXT", "gpt-one");
  uint8_t* entries = gpt_prologue((uint32_t)k_mp_gpt_hdr_count);
  write_gpt_basic_entry(entries, 0U, (uint32_t)k_mp_base0);
  write_gpt_basic_entry(entries, 1U, (uint32_t)k_mp_base1);

  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount_partition(&s_backend, 1U, &h));
  TEST_ASSERT_EQ(k_mp_base1, h->partition_base_lba);
  names_t n1 = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", names_cb, &n1));
  TEST_ASSERT_EQ(1, n1.count);
  TEST_ASSERT_EQ(1, n1.saw_p1);
  TEST_ASSERT_EQ(0, n1.saw_p0);
  assert_file_bytes(h, "G1.TXT", "gpt-one");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount_partition(&s_backend, 0U, &h));
  TEST_ASSERT_EQ(k_mp_base0, h->partition_base_lba);
  assert_file_bytes(h, "G0.TXT", "gpt-zero");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_disk();
  TEST_END("ra8_fs_mount_partition: GPT selects the second Basic Data volume");
}

/**
 * @test test_gpt_index_errors
 * @brief GPT out-of-range, empty, 64-bit-LBA and zero-LBA entries each error.
 *
 * @details Entry 0 is a valid Basic Data slot; entry 1 is empty (null GUID);
 *          entry 2 is allocated but its first LBA has a non-zero high word
 *          (> 32 bits); entry 3 is allocated with a zero first LBA. Index 4 is
 *          past the count.
 *
 * @par MC/DC:
 * Covers priv_gpt_locate_partition `index >= count` (V-true = 4) and the two
 * priv_gpt_entry_select decisions -- null GUID (not_found), zero LBA
 * (validation_failed) -- each against the valid entry 0 control. An entry
 * whose 64-bit first LBA exceeds 32 bits is no longer a select error (#683):
 * the address is followed, and on this small fake the backend's own
 * out_of_range comes back, proving the LBA reached it un-truncated.
 *
 * @pre s_disk is nullptr.
 * @post s_disk freed.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_gpt_index_errors(void)
{
  TEST_BEGIN("ra8_fs_mount_partition: GPT index errors");
  alloc_disk((uint32_t)k_mp_small_card);
  uint8_t* entries = gpt_prologue((uint32_t)k_mp_gpt_hdr_count);
  /* Entry 0 is valid; entry 1 stays null (an empty slot). */
  write_gpt_basic_entry(entries, 0U, 3U);
  /* Entry 2: a non-null GUID but a 64-bit first LBA this backend cannot reach. */
  const size_t e2 = (size_t)2U * (size_t)k_mp_gpt_stride;
  memcpy(&entries[e2], k_mp_basic_guid, sizeof(k_mp_basic_guid));
  entries[e2 + (size_t)k_mp_gpt_ent_lba_hi] = 1U;
  /* Entry 3: allocated but a zero first LBA. */
  write_gpt_basic_entry(entries, 3U, 0U);

  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_fs_mount_partition(&s_backend, 4U, &h));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_mount_partition(&s_backend, 1U, &h));
  /* Entry 2's first LBA is past 2 TiB: followed to the backend (#683), whose
   * fake medium answers out_of_range -- not refused at the parser. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_fs_mount_partition(&s_backend, 2U, &h));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_fs_mount_partition(&s_backend, 3U, &h));
  free_disk();
  TEST_END("ra8_fs_mount_partition: GPT index errors");
}

/**
 * @test test_gpt_header_and_read_faults
 * @brief A malformed GPT header, and an entry sector beyond the disk, both
 *        propagate through the indexed GPT path.
 *
 * @details First: a protective MBR with an all-zero LBA 1 (no "EFI PART") makes
 *          priv_gpt_read_geom fail, which priv_gpt_locate_partition returns.
 *          Second: a valid header advertising more entries than the 3-sector
 *          disk holds makes the entry-sector read fault for an in-count index.
 *
 * @par MC/DC:
 * Covers priv_gpt_locate_partition's two error-propagation guards: the
 * `priv_gpt_read_geom() != k_ra8_ok` return, and the entry-sector
 * `priv_read_sector() != k_ra8_ok` return.
 *
 * @pre s_disk is nullptr.
 * @post s_disk freed.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_gpt_header_and_read_faults(void)
{
  TEST_BEGIN("ra8_fs_mount_partition: GPT header + entry-read faults");
  ra8_fs_mount_t* h = nullptr;

  /* No "EFI PART" at LBA 1 -> priv_gpt_read_geom returns validation_failed. */
  alloc_disk((uint32_t)k_mp_small_card);
  write_protective_mbr(s_disk.bytes, (uint32_t)k_mp_gpt_hdr_lba);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_fs_mount_partition(&s_backend, 0U, &h));
  free_disk();

  /* Header claims 8 entries; index 4 lands in LBA 3, past a 3-sector disk. */
  alloc_disk(3U);
  write_protective_mbr(s_disk.bytes, (uint32_t)k_mp_gpt_hdr_lba);
  write_gpt_header(&s_disk.bytes[(uint32_t)k_mc_blk],
                   (uint32_t)k_mp_gpt_hdr_lba,
                   0U,
                   8U,
                   (uint32_t)k_mp_gpt_entry_bytes);
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_fs_mount_partition(&s_backend, 4U, &h));
  free_disk();
  TEST_END("ra8_fs_mount_partition: GPT header + entry-read faults");
}

/* ===========================================================================
 * No table, and the superfloppy default
 * ===========================================================================
 */

/**
 * @test test_no_partition_table
 * @brief An explicit index on a sector 0 that is not a partition table (bad
 *        boot signature) returns not_found.
 *
 * @details priv_locate_indexed checks the 0x55 and 0xAA signature bytes
 *          separately; a missing 0x55 (first case) and a wrong 0xAA (second)
 *          both mean "no table to index".
 *
 * @par MC/DC:
 * The two single-condition guards in priv_locate_indexed:
 * - `s_scratch[510] != 0x55` : V-true = zeroed sector (case 1).
 * - `s_scratch[511] != 0xAA` : V-true = 0x55 present, 0xAA wrong (case 2).
 *
 * @pre s_disk is nullptr.
 * @post s_disk freed.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_no_partition_table(void)
{
  TEST_BEGIN("ra8_fs_mount_partition: no partition table returns not_found");
  ra8_fs_mount_t* h = nullptr;

  alloc_disk((uint32_t)k_mp_small_card); /* all-zero: no 0x55 at byte 510 */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_mount_partition(&s_backend, 0U, &h));
  s_disk.bytes[k_bpb_off_sig0] = (uint8_t)k_bpb_sig0_val; /* 0x55 present, 0xAA still 0 */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_mount_partition(&s_backend, 0U, &h));
  free_disk();
  TEST_END("ra8_fs_mount_partition: no partition table returns not_found");
}

/**
 * @test test_superfloppy_default_still_mounts
 * @brief The default ra8_fs_mount() still mounts a superfloppy at LBA 0, while
 *        an explicit index on the same card finds no table.
 *
 * @details A hand-built FAT16 BPB at LBA 0 (no partition table) mounts via the
 *          auto path with partition_base_lba == 0. Asking for partition 0
 *          explicitly instead resolves the MBR entry region, which is zeroed,
 *          so the entry is unused and the mount returns not_found.
 *
 * @par MC/DC:
 * Decision: `if (index != k_ra8_fs_partition_auto)` in priv_read_boot_sector,
 * V-false leg -- the superfloppy auto path -- paired with the V-true legs the
 * MBR/GPT tests drive.
 *
 * @pre s_disk is nullptr.
 * @post s_disk freed.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_superfloppy_default_still_mounts(void)
{
  TEST_BEGIN("ra8_fs_mount_partition: superfloppy still mounts via the default");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(0U, h->partition_base_lba);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_mount_partition(&s_backend, 0U, &h));
  free_disk();
  TEST_END("ra8_fs_mount_partition: superfloppy still mounts via the default");
}

/* ===========================================================================
 * main
 * ===========================================================================
 */

int main(void)
{
  test_mbr_mount_partition_one();
  test_mbr_index_errors();
  test_gpt_select_second_basic_data();
  test_gpt_index_errors();
  test_gpt_header_and_read_faults();
  test_no_partition_table();
  test_superfloppy_default_still_mounts();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_mount_partition.c\n");
  return 0;
}
