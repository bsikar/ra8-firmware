/**
 * @file test_ra8_fs_lfn.c
 * @brief VFAT long-filename (LFN) read support for ra8_fs (#101).
 *
 * @details
 * Hand-builds a FAT16 RAM volume whose root directory holds one file written as
 * `mybook.epub` -- i.e. an LFN entry chain (attr 0x0F) immediately before its
 * 8.3 alias `MYBOOK~1.EPU`. Verifies that ra8_fs_open resolves the file by its
 * long name *and* by its 8.3 alias, that a wrong name still misses, and that
 * ra8_fs_listdir reports the long name. The 8.3-only behaviour is unchanged
 * (covered by tests/test_ra8_fs_fat.c).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "unity_minimal.h"

/** @brief FAT 8.3 short-name field width, bytes (8 name + 3 extension). */
typedef enum : uint8_t {
  k_sfn_name_bytes = 11U, /**< 8.3 name field, unterminated and space-padded. */
} sfn_geom_t;

/**
 * @enum fat_bpb_field_t
 * @brief Byte offsets of the BPB fields this fixture writes, per the FAT specification.
 *
 * @details
 * The fixture hand-builds a boot sector rather than calling a formatter, so
 * every field is placed by its specified offset. Names follow the
 * specification's own (BPB_BytsPerSec, BPB_FATSz16, ...) so a reader can check
 * the layout against the document without decoding the numbers.
 */
typedef enum : uint16_t {
  k_bpb_off_bytes_per_sec = 11U,  /**< BPB_BytsPerSec: bytes per sector.           */
  k_bpb_off_sec_per_clus  = 13U,  /**< BPB_SecPerClus: sectors per cluster.        */
  k_bpb_off_rsvd_sec_cnt  = 14U,  /**< BPB_RsvdSecCnt: sectors before the 1st FAT. */
  k_bpb_off_root_ent_cnt  = 17U,  /**< BPB_RootEntCnt: root-directory entries.     */
  k_bpb_off_tot_sec16     = 19U,  /**< BPB_TotSec16: total sectors.                */
  k_bpb_off_fat_sz16      = 22U,  /**< BPB_FATSz16: sectors per FAT.               */
  k_bpb_off_sig_lo        = 510U, /**< Low byte of the 0xAA55 boot signature.      */
  k_bpb_off_sig_hi        = 511U, /**< Its high byte.                              */
} fat_bpb_field_t;

/**
 * @enum fat_dir_entry_field_t
 * @brief Byte offsets and marker values inside a 32-byte FAT directory entry.
 *
 * @details
 * A short (8.3) entry and a long-name entry share the same 32-byte frame, so
 * the attribute byte and first-cluster fields sit at one offset for both; the
 * `k_lfn_*` members are the fields only a long-name entry defines.
 */
typedef enum : uint8_t {
  k_dir_off_attr        = 11U,   /**< DIR_Attr / LDIR_Attr: the attribute byte.  */
  k_lfn_off_type        = 12U,   /**< LDIR_Type: reserved, must be zero.         */
  k_lfn_off_chksum      = 13U,   /**< LDIR_Chksum: the 8.3 alias checksum.       */
  k_dir_off_fst_clus_hi = 20U,   /**< DIR_FstClusHI: high half of first cluster. */
  k_dir_off_fst_clus_lo = 26U,   /**< DIR_FstClusLO: its low half; 0 in an LFN.  */
  k_dir_off_file_size   = 28U,   /**< DIR_FileSize: first of its four bytes.     */
  k_lfn_attr_long_name  = 0x0FU, /**< ATTR_LONG_NAME: marks an entry as a name.  */
  k_lfn_ord_first_and_last =
    0x41U,                        /**< LDIR_Ord: sequence 1 plus the 0x40 last-logical flag, so this
                single entry carries the whole name. */
  k_lfn_chars_per_entry  = 13U,   /**< UCS-2 chars one entry holds, split 5 + 6 + 2.       */
  k_sfn_name_len         = 11U,   /**< 8.3 name bytes (8 base + 3 ext) the checksum folds. */
  k_sfn_chksum_carry_bit = 0x80U, /**< Bit rotated into the top each checksum round.       */
  k_bpb_sig_lo           = 0x55U, /**< Boot-signature low byte.                            */
  k_bpb_sig_hi           = 0xAAU, /**< Boot-signature high byte.                           */
  k_byte_mask            = 0xFFU, /**< Low-byte mask used by the put16/put32 helpers.      */
  k_name_buf_bytes       = 64U,   /**< Split-name scratch capacity; over the longest
                                       fixture name.                                */
} fat_dir_entry_field_t;

/** @brief Pads every UCS-2 slot after the long name's terminator. */
static const uint16_t k_lfn_name_pad = 0xFFFFU;
/* --- RAM block device + minimal FAT16 volume (mirrors test_ra8_fs_fat.c) --- */

typedef enum : uint32_t {
  k_block_size    = 512U,                                      /**< Block size.       */
  k_blocks        = 8U * 1024U,                                /**< Blocks.           */
  k_reserved      = 1U,                                        /**< Reserved.         */
  k_num_fats      = 2U,                                        /**< Number fats.      */
  k_sectors_fat   = 32U,                                       /**< Sectors fat.      */
  k_root_lba      = k_reserved + (k_num_fats * k_sectors_fat), /**< Root lba (= 65).  */
  k_dir_entry_len = 32U,                                       /**< Dir entry length. */
} fs_lfn_geom_t;

typedef struct {
  uint8_t* bytes;       /**< Bytes.       */
  uint32_t block_count; /**< Block count. */
  uint32_t byte_count;  /**< Byte count.  */
} mem_disk_t;

static mem_disk_t s_disk = {};

static ra8_err_t mem_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_block_size],
         (size_t)count * (uint32_t)k_block_size);
  return k_ra8_ok;
}

static ra8_err_t mem_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_block_size],
         buf,
         (size_t)count * (uint32_t)k_block_size);
  return k_ra8_ok;
}

static ra8_err_t mem_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  *block_count  = d->block_count;
  *block_size   = (uint32_t)k_block_size;
  return k_ra8_ok;
}

static const ra8_fs_backend_t s_backend = {
  .read_block   = mem_read,
  .write_block  = mem_write,
  .get_capacity = mem_capacity,
  .ctx          = &s_disk,
};

static void put16(uint8_t* p, uint32_t off, uint16_t v)
{
  p[off]     = (uint8_t)(v & k_byte_mask);
  p[off + 1] = (uint8_t)((v >> 8) & k_byte_mask);
}

/** @brief The 8.3 short-name checksum the LFN chain must carry. */
static uint8_t sfn_checksum(const uint8_t* name83)
{
  uint8_t sum = 0U;
  for (uint32_t i = 0U; i < k_sfn_name_len; i++) {
    sum = (uint8_t)((((sum & 1U) != 0U) ? k_sfn_chksum_carry_bit : 0U) + (uint32_t)(sum >> 1U) +
                    (uint32_t)name83[i]);
  }
  return sum;
}

/* The packed 8.3 alias for "mybook.epub": base "MYBOOK~1", ext "EPU". */
static const uint8_t k_alias83[11] = {'M', 'Y', 'B', 'O', 'O', 'K', '~', '1', 'E', 'P', 'U'};

/**
 * @brief Write the FAT16 BIOS Parameter Block for the LFN fixture volume.
 *
 * @pre `s_disk.bytes` holds at least one block.
 * @pre The disk geometry constants match the fixture image size.
 * @post Block 0 describes a mountable FAT16 volume.
 * @post The 0x55 0xAA boot signature terminates the block.
 *
 * @note Not thread-safe; the fixture disk is file-scope state.
 */
static void lfn_write_bpb(void)
{
  uint8_t* bpb = &s_disk.bytes[0];
  put16(bpb, k_bpb_off_bytes_per_sec, (uint16_t)k_block_size);
  bpb[k_bpb_off_sec_per_clus] = 1U;                         /* sectors/cluster  */
  put16(bpb, k_bpb_off_rsvd_sec_cnt, (uint16_t)k_reserved); /* reserved sectors */
  bpb[16] = (uint8_t)k_num_fats;
  put16(bpb, k_bpb_off_root_ent_cnt, 16U);             /* root dir entries */
  put16(bpb, k_bpb_off_tot_sec16, (uint16_t)k_blocks); /* total sectors    */
  put16(bpb, k_bpb_off_fat_sz16, (uint16_t)k_sectors_fat);
  bpb[k_bpb_off_sig_lo] = k_bpb_sig_lo;
  bpb[k_bpb_off_sig_hi] = k_bpb_sig_hi;
}

/**
 * @brief Write the single long-file-name directory slot.
 *
 * @details
 * "mybook.epub" is 11 characters, so one LFN entry holds the whole name: the
 * order byte carries the last-logical flag, the checksum ties the slot to the
 * 8.3 alias that follows, and the unused character slots are padded 0xFFFF
 * after a single 0x0000 terminator.
 *
 * @param[out] root Root-directory region to write into.
 *
 * @pre @p root points at the root directory and has room for one entry.
 * @pre `k_alias83` is the 8.3 alias this slot checksums.
 * @post The slot is a valid single-entry LFN for the fixture name.
 * @post Character slots past the terminator read as padding.
 *
 * @note Not thread-safe; the fixture disk is file-scope state.
 */
static void lfn_write_name_slot(uint8_t* root)
{
  /* --- slot 0: single LFN entry (the name is 11 chars, fits one entry) --- */
  uint8_t* lfn          = &root[0];
  lfn[0]                = k_lfn_ord_first_and_last; /* order 1 | last-logical (0x40) */
  lfn[k_dir_off_attr]   = k_lfn_attr_long_name;     /* LFN attribute                 */
  lfn[k_lfn_off_type]   = 0x00U;
  lfn[k_lfn_off_chksum] = sfn_checksum(k_alias83);
  put16(lfn, k_dir_off_fst_clus_lo, 0U);
  const char* const lname = "mybook.epub"; /* 11 chars */
  /* UTF-16LE char slot byte-offsets within a 32-byte LFN entry. */
  static const uint8_t k_off[13] = {1U, 3U, 5U, 7U, 9U, 14U, 16U, 18U, 20U, 22U, 24U, 28U, 30U};
  for (uint32_t i = 0U; i < k_lfn_chars_per_entry; i++) {
    if (i < strlen(lname)) {
      put16(lfn, (uint32_t)k_off[i], (uint16_t)(uint8_t)lname[i]);
    } else if (i == strlen(lname)) {
      put16(lfn, (uint32_t)k_off[i], 0x0000U); /* name terminator */
    } else {
      put16(lfn, (uint32_t)k_off[i], k_lfn_name_pad); /* padding */
    }
  }
}

/** @brief Build a FAT16 volume with the LFN chain + 8.3 entry for mybook.epub. */
static void build_volume_with_lfn(void)
{
  free(s_disk.bytes);
  s_disk.byte_count  = (uint32_t)k_blocks * (uint32_t)k_block_size;
  s_disk.bytes       = (uint8_t*)calloc(1U, s_disk.byte_count);
  s_disk.block_count = (uint32_t)k_blocks;
  TEST_ASSERT(s_disk.bytes != nullptr);

  lfn_write_bpb();

  /* Root directory at LBA 65: slot 0 = LFN("mybook.epub"), slot 1 = 8.3 entry. */
  uint8_t* root = &s_disk.bytes[(size_t)(uint32_t)k_root_lba * (uint32_t)k_block_size];

  lfn_write_name_slot(root);

  /* --- slot 1: the 8.3 short entry (zero-length file is enough to open) --- */
  uint8_t* sfn = &root[(uint32_t)k_dir_entry_len];
  memcpy(sfn, k_alias83, (size_t)k_sfn_name_bytes);
  sfn[k_dir_off_attr] = 0x20U;                    /* ARCHIVE                            */
  put16(sfn, k_dir_off_fst_clus_lo, 0U);          /* first cluster low = 0 (empty file) */
  put16(sfn, k_dir_off_fst_clus_hi, 0U);          /* first cluster high                 */
  (void)memset(&sfn[k_dir_off_file_size], 0, 4U); /* DIR_FileSize = 0 (empty file)      */
  /* slot 2 stays 0x00 -> end-of-directory. */
}

static void free_volume(void)
{
  free(s_disk.bytes);
  s_disk.bytes = nullptr;
}

/* --- listdir capture --- */

typedef struct {
  char    last[k_name_buf_bytes]; /**< Last.  */
  uint8_t count;                  /**< Count. */
} listdir_capture_t;

static void capture_cb(const char* name, uint8_t attr, uint32_t size, void* ctx)
{
  (void)attr;
  (void)size;
  listdir_capture_t* c = (listdir_capture_t*)ctx;
  (void)snprintf(c->last, sizeof(c->last), "%s", name);
  c->count++;
}

/**
 * @test test_lfn_open_by_long_and_short
 * @brief A file written as mybook.epub opens by its long name AND by its 8.3
 *        alias; a wrong long name still misses.
 *
 * @par MC/DC: not applicable -- name-resolution acceptance path; the open guard
 *      decisions are covered in tests/test_ra8_fs_fat.c.
 */
static void test_lfn_open_by_long_and_short(void)
{
  TEST_BEGIN("ra8_fs LFN: open by long name + 8.3 alias");
  build_volume_with_lfn();
  ra8_fs_mount_t* mount = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &mount));

  ra8_fs_file_t* f = nullptr;
  /* Long name (4-char extension -- unreachable via 8.3) resolves now. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "mybook.epub", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  /* Case-insensitive long-name match. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "MyBook.EPUB", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  /* The 8.3 alias still opens the same entry. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "MYBOOK~1.EPU", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  /* A different long name misses. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_open(mount, "other.epub", k_ra8_fs_mode_read, &f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(mount));
  free_volume();
  TEST_END("ra8_fs LFN: open by long name + 8.3 alias");
}

/**
 * @test test_lfn_listdir_reports_long_name
 * @brief ra8_fs_listdir reports the reassembled long name, not the 8.3 alias.
 *
 * @par MC/DC: not applicable -- enumeration acceptance path.
 */
static void test_lfn_listdir_reports_long_name(void)
{
  TEST_BEGIN("ra8_fs LFN: listdir reports the long name");
  build_volume_with_lfn();
  ra8_fs_mount_t* mount = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &mount));

  listdir_capture_t cap = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(mount, "/", capture_cb, &cap));
  TEST_ASSERT_EQ(1U, cap.count);
  TEST_ASSERT_EQ(0, strcmp(cap.last, "mybook.epub"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(mount));
  free_volume();
  TEST_END("ra8_fs LFN: listdir reports the long name");
}

int32_t main(void)
{
  test_lfn_open_by_long_and_short();
  test_lfn_listdir_reports_long_name();
  return 0;
}
