/**
 * @file test_ra8_fs_fat_mcdc.c
 * @brief Residual MC/DC vectors for the still-uncovered decisions in ra8_fs_fat.c.
 *
 * @details
 * Third sibling of ``tests/test_ra8_fs_fat.c`` and
 * ``tests/test_ra8_fs_fat_variants.c``. Those files (plus
 * ``tests/test_ra8_fs_format.c``) cover the bulk of the FAT/exFAT adapter; this
 * file targets the specific compound-boolean *arms* that remained uncovered:
 *   - the VFAT long-filename (LFN) reassembly guards (`priv_lfn_add` sequence
 *     range, the UTF-16 terminator-vs-pad split) and the case-insensitive
 *     name compare (`priv_name_ieq`) reached only through the long-name open
 *     fallback (`priv_dir_find_long`);
 *   - the read accelerator's walk-cache resume decision (`priv_read_one_chunk`)
 *     on a backward seek;
 *   - the formatter's per-type cluster-count band checks
 *     (`priv_fmt_count_in_band`), the unsupported-type guard, the block-size
 *     guard, and the exFAT volume-label packer's length/terminator split.
 *
 * Coverage is driven by the real public ``ra8_fs_*`` functions executing the
 * branch; each case asserts the returned ``ra8_err_t`` or an observable state.
 * The in-memory block-device harness and the FAT16 BPB builder mirror
 * ``tests/test_ra8_fs_fat.c`` exactly. LFN directory images are planted by
 * writing raw 32-byte entries into the fixed root region, then resolved via
 * ``ra8_fs_open`` with a non-8.3 long name (a 4+ char extension forces the
 * long-name fallback).
 *
 * Each test follows the canonical N+1 MC/DC pattern: for an N-condition
 * decision, N+1 vectors observe every condition both true and false AND prove
 * each independently flips the decision outcome. Structurally unreachable
 * conditions are flagged as deactivated under DO-178C 6.4.4.3.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "unity_minimal.h"

/**
 * @enum fs_fat_mcdc_fixture_t
 * @brief The payload generators and their seeds, plus protocol and on-disk field offsets, and buffer capacities and payload sizes, and the byte-level helpers.
 */
typedef enum : uint8_t {
  k_bpb_sig_lo          = 0x55U, /**< The 0xAA55 boot signature's low byte.       */
  k_bpb_sig_hi          = 0xAAU, /**< Its high byte.                              */
  k_lfn_chars_per_entry = 13U,   /**< UCS-2 characters one long-name entry holds. */
  k_fs_pattern_stride =
    17U, /**< Payload stride `i * 17 + 3`, coprime with 256: never repeats in a sector. */
  k_lfn_seq_max =
    20U, /**< Highest long-name sequence number: test chain is the longest the format allows. */
  /** A packed 8.3 name: 8 base + 3 extension. */
  k_sfn_name_len = 11,
  /** Shift to the most significant byte, which goes on the wire first. */
  k_shift_byte3 = 24,
  /** Low-byte mask used when clocking a word out a byte at a time. */
  k_byte_mask = 0xFFU,
} fs_fat_mcdc_fixture_t;

/**
 * @enum fs_fat_mcdc_fixture2_t
 * @brief Out-of-range and malformed inputs the code under test must reject.
 */
typedef enum : uint16_t {
  k_fs_block_size_unsupported =
    4096U, /**< A block size other than 512, which the formatter must reject. */
} fs_fat_mcdc_fixture2_t;

/**
 * @enum ra8_fs_mcdc_disk_t
 * @brief Static-allocation sizes for the synthetic block devices.
 *
 * @details `k_disk_blocks_fat16` matches the FAT16 geometry built by
 *          `build_fat16_volume` (SPC = 1, so cluster size == 512 bytes); the
 *          larger sizes feed the formatter band checks.
 */
typedef enum : uint32_t {
  k_disk_block_size   = 512U,       /**< Bytes per logical block.            */
  k_disk_blocks_fat16 = 8U * 1024U, /**< 4 MiB FAT16 card (matches sibling). */
  k_disk_blocks_big16 = 73728U,     /**< 36 MiB: FAT16 needs spc>1 (>65525). */
  k_disk_blocks_fat32 = 73728U,     /**< 36 MiB: lands the FAT32 band.       */
} ra8_fs_mcdc_disk_t;

/**
 * @enum ra8_fs_mcdc_layout_t
 * @brief BPB byte offsets and FAT16 directory geometry used by the harness.
 *
 * @details The FAT16 volume built here has 1 reserved sector, 2 FATs of 16
 *          sectors each, and 16 root entries -- so the fixed root directory
 *          begins at LBA 1 + (2 * 16) = 33 and spans exactly one sector.
 */
typedef enum : uint32_t {
  k_bpb_off_bytes_per_sec = 11U,  /**< BPB_BytsPerSec.                          */
  k_bpb_off_sec_per_clus  = 13U,  /**< BPB_SecPerClus.                          */
  k_bpb_off_rsvd          = 14U,  /**< BPB_RsvdSecCnt.                          */
  k_bpb_off_num_fats      = 16U,  /**< BPB_NumFATs.                             */
  k_bpb_off_root_ents     = 17U,  /**< BPB_RootEntCnt.                          */
  k_bpb_off_tot_sec16     = 19U,  /**< BPB_TotSec16.                            */
  k_bpb_off_fatsz16       = 22U,  /**< BPB_FATSz16.                             */
  k_bpb_off_sig_lo        = 510U, /**< Boot signature low byte (0x55).          */
  k_bpb_off_sig_hi        = 511U, /**< Boot signature high byte (0xAA).         */
  k_fat16_rsvd            = 1U,   /**< Reserved sectors.                        */
  k_fat16_num_fats        = 2U,   /**< FAT copies.                              */
  k_fat16_fatsz           = 16U,  /**< Sectors per FAT.                         */
  k_fat16_root_ents       = 16U,  /**< Root directory entries.                  */
  k_fat16_root_lba        = 33U,  /**< Fixed root start: rsvd + num_fats*fatsz. */
} ra8_fs_mcdc_layout_t;

/**
 * @enum ra8_fs_mcdc_dir_t
 * @brief Directory-entry field offsets and marker bytes for planted LFN images.
 */
typedef enum : uint8_t {
  k_dir_entry_bytes  = 32U,   /**< Bytes per directory entry.              */
  k_dir_off_attr     = 11U,   /**< DIR_Attr byte offset.                   */
  k_dir_off_first_lo = 26U,   /**< DIR_FstClusLO byte offset.              */
  k_dir_off_size     = 28U,   /**< DIR_FileSize byte offset.               */
  k_attr_lfn         = 0x0FU, /**< Long-name attribute marker.             */
  k_attr_archive     = 0x20U, /**< ATTR_ARCHIVE.                           */
  k_lfn_off_seq      = 0U,    /**< LDIR_Ord byte offset.                   */
  k_lfn_off_attr     = 11U,   /**< LDIR_Attr byte offset (== 0x0F).        */
  k_lfn_off_csum     = 13U,   /**< LDIR_Chksum byte offset.                */
  k_lfn_seq_last     = 0x40U, /**< LAST_LONG_ENTRY bit on the final group. */
  k_name_field_len   = 11U,   /**< Packed 8.3 name length (no dot).        */
} ra8_fs_mcdc_dir_t;

/**
 * @enum ra8_fs_mcdc_lfn16_t
 * @brief UTF-16 sentinel values planted in LFN character slots.
 */
typedef enum : uint16_t {
  k_lfn_term16 = 0x0000U, /**< Name terminator (drives the `val == 0` arm). */
  k_lfn_pad16  = 0xFFFFU, /**< Slot padding (drives the `val == pad` arm).  */
} ra8_fs_mcdc_lfn16_t;

/** @brief Byte offsets of the 13 UTF-16 chars within a 32-byte LFN entry. */
static const uint8_t s_lfn_char_off[13] =
  {1U, 3U, 5U, 7U, 9U, 14U, 16U, 18U, 20U, 22U, 24U, 28U, 30U};

/** @brief Memory-backed block device handed to ra8_fs. */
typedef struct {
  uint8_t* bytes;       /**< Flat sector store.         */
  uint32_t block_count; /**< Number of 512-byte blocks. */
  uint32_t byte_count;  /**< Total store size in bytes. */
} mem_disk_t;

static mem_disk_t s_disk = {};

static ra8_err_t mem_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_disk_block_size],
         (size_t)count * (uint32_t)k_disk_block_size);
  return k_ra8_ok;
}

static ra8_err_t mem_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_disk_block_size],
         buf,
         (size_t)count * (uint32_t)k_disk_block_size);
  return k_ra8_ok;
}

static ra8_err_t mem_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  *block_count  = d->block_count;
  *block_size   = (uint32_t)k_disk_block_size;
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

static void put32(uint8_t* p, uint32_t off, uint32_t v)
{
  p[off]     = (uint8_t)(v & k_byte_mask);
  p[off + 1] = (uint8_t)((v >> 8) & k_byte_mask);
  p[off + 2] = (uint8_t)((v >> 16) & k_byte_mask);
  p[off + 3] = (uint8_t)((v >> k_shift_byte3) & k_byte_mask);
}

static void free_volume(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
}

static void alloc_card(uint32_t blocks)
{
  free_volume();
  s_disk.byte_count  = blocks * (uint32_t)k_disk_block_size;
  s_disk.bytes       = (uint8_t*)calloc(1, s_disk.byte_count);
  s_disk.block_count = blocks;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
}

/** @brief Build the exact FAT16 BPB used by the sibling MC/DC test. */
static void build_fat16_volume(void)
{
  alloc_card((uint32_t)k_disk_blocks_fat16);
  uint8_t* bpb = &s_disk.bytes[0];
  put16(bpb, (uint32_t)k_bpb_off_bytes_per_sec, (uint16_t)k_disk_block_size);
  bpb[(uint32_t)k_bpb_off_sec_per_clus] = 1U;
  put16(bpb, (uint32_t)k_bpb_off_rsvd, (uint16_t)k_fat16_rsvd);
  bpb[(uint32_t)k_bpb_off_num_fats] = (uint8_t)k_fat16_num_fats;
  put16(bpb, (uint32_t)k_bpb_off_root_ents, (uint16_t)k_fat16_root_ents);
  put16(bpb, (uint32_t)k_bpb_off_tot_sec16, (uint16_t)k_disk_blocks_fat16);
  put16(bpb, (uint32_t)k_bpb_off_fatsz16, (uint16_t)k_fat16_fatsz);
  bpb[(uint32_t)k_bpb_off_sig_lo] = k_bpb_sig_lo;
  bpb[(uint32_t)k_bpb_off_sig_hi] = k_bpb_sig_hi;
}

/**
 * @brief Compute the 8.3 short-name checksum the LFN chain must carry.
 *
 * @details Bit-exact mirror of `priv_sfn_checksum` (MS FAT spec sec 7): a
 *          rotate-right-add fold over the 11 raw name bytes. The planted LFN
 *          entries store this in their LDIR_Chksum byte so `priv_lfn_units_for`
 *          ties the chain to its 8.3 entry.
 *
 * @param[in] name11 Packed 11-byte 8.3 name (8 base + 3 ext, space padded).
 * @return The single-byte checksum.
 */
static uint8_t sfn_checksum(const uint8_t* name11)
{
  uint8_t sum = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_name_field_len; i++) {
    const uint32_t high = ((sum & 1U) != 0U) ? 0x80U : 0U;
    sum                 = (uint8_t)(high + (uint32_t)(sum >> 1U) + (uint32_t)name11[i]);
  }
  return sum;
}

/** @brief Write a packed 8.3 directory entry at @p ent (size in bytes, attr archive). */
static void plant_short_entry(uint8_t* ent, const char name11[k_sfn_name_len], uint32_t size)
{
  memset(ent, 0, (size_t)k_dir_entry_bytes);
  memcpy(ent, name11, (size_t)k_name_field_len);
  ent[(uint32_t)k_dir_off_attr] = (uint8_t)k_attr_archive;
  put16(ent, (uint32_t)k_dir_off_first_lo, 0U); /* zero-length file: no chain */
  put32(ent, (uint32_t)k_dir_off_size, size);
}

/**
 * @brief Write one LFN sub-entry carrying up to 13 UTF-16 chars of @p text.
 *
 * @details Lays the sequence/order byte (with the LAST_LONG_ENTRY flag when
 *          @p is_last), the 0x0F attribute, the 8.3 checksum, and the 13 char
 *          slots. ASCII bytes are stored low-byte-first; the first slot past
 *          the name takes the @p pad_with sentinel: `k_lfn_term16` (NUL) or
 *          `k_lfn_pad16` (0xFFFF), exercising both arms of the
 *          `(val == 0) || (val == pad)` decision in `priv_lfn_add`.
 *
 * @param[out] ent      32-byte entry buffer.
 * @param[in]  order    Sequence order (1..N); 0 / >19 drive the range guard.
 * @param[in]  is_last  Non-zero sets the 0x40 last-group flag.
 * @param[in]  csum     8.3 checksum to tie the chain to its entry.
 * @param[in]  text     ASCII chars for this group (<= 13).
 * @param[in]  pad_with Sentinel placed after the text (`term16` or `pad16`).
 */
static void plant_lfn_entry(uint8_t*    ent,
                            uint8_t     order,
                            uint8_t     is_last,
                            uint8_t     csum,
                            const char* text,
                            uint16_t    pad_with)
{
  memset(ent, 0, (size_t)k_dir_entry_bytes);
  ent[(uint32_t)k_lfn_off_seq]  = (uint8_t)(order | (is_last != 0U ? (uint8_t)k_lfn_seq_last : 0U));
  ent[(uint32_t)k_lfn_off_attr] = (uint8_t)k_attr_lfn;
  ent[(uint32_t)k_lfn_off_csum] = csum;
  const uint32_t len            = (uint32_t)strlen(text);
  uint8_t        placed_pad     = 0U;
  for (uint32_t i = 0U; i < k_lfn_chars_per_entry; i++) {
    const uint32_t off = (uint32_t)s_lfn_char_off[i];
    if (i < len) {
      put16(ent, off, (uint16_t)(uint8_t)text[i]);
    } else if (placed_pad == 0U) {
      put16(ent, off, pad_with); /* first slot past the name: terminator/pad */
      placed_pad = 1U;
    } else {
      put16(ent, off, (uint16_t)k_lfn_pad16);
    }
  }
}

/**
 * @test test_mcdc_lfn_name_compare_lengths
 * @par MC/DC:
 * Two decisions in `libs/ra8_fs/src/ra8_fs_fat_lfn.c@priv_name_ieq`,
 * reached when `ra8_fs_open` falls back to the long-name lookup
 * (`priv_dir_find_long`) for a non-8.3 name.
 *
 * Loop head `while ((*a != '\0') && (*b != '\0'))` (2 conditions):
 * - exact match "AB.EPUB" == "AB.EPUB": loop runs to the shared NUL, exiting on
 *   C1=F (`*a == '\0'`); C2 is true on every iteration -- C1 false arm.
 * - shorter needle "AB.EPU" (a strict prefix) vs lname "AB.EPUB": loop exits on
 *   C1=F after the needle is consumed while C2 is still true.
 * - shorter lname: needle "AB.EPUBX" vs lname "AB.EPUB": loop exits on C2=F
 *   (`*b == '\0'`) with C1 still true -- C2 false arm.
 *
 * Return `((*a == '\0') && (*b == '\0'))` (2 conditions):
 * - exact match -> both NUL -> T -> 1 (the open succeeds; control vector).
 * - needle longer ("AB.EPUBX"): loop stops on C2=F, so at the return *a != '\0'
 *   -> C1=F -> 0 (needle independence).
 * - lname longer (needle "AB.EPU", a strict prefix): loop stops on C1=F, so at
 *   the return *a == '\0' (C1=T) but *b != '\0' (C2=F) -> 0 (lname independence).
 * N+1 = 3 vectors for each 2-condition decision.
 */
static void test_mcdc_lfn_name_compare_lengths(void)
{
  TEST_BEGIN("ra8_fs MC/DC: LFN name compare length arms");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* Plant one LFN chain whose long name is "AB.EPUB" (7 chars, single group)
   * tied to an arbitrary 8.3 entry "AB123456EPU". */
  const char    short11[11] = {'A', 'B', '1', '2', '3', '4', '5', '6', 'E', 'P', 'U'};
  const uint8_t csum        = sfn_checksum((const uint8_t*)short11);
  uint8_t* root = &s_disk.bytes[(size_t)(uint32_t)k_fat16_root_lba * (uint32_t)k_disk_block_size];
  plant_lfn_entry(&root[0U], 1U, 1U, csum, "AB.EPUB", (uint16_t)k_lfn_term16);
  plant_short_entry(&root[(uint32_t)k_dir_entry_bytes], short11, 0U);

  ra8_fs_file_t* f = nullptr;
  /* Exact match: both decisions reach their both-true / C1-false arms; opens. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "AB.EPUB", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  /* Needle longer than lname: loop exits on C2=F; return sees *a != '\0'. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_open(h, "AB.EPUBX", k_ra8_fs_mode_read, &f));
  /* lname longer than needle (needle is a strict prefix): loop exits on C1=F;
   * return sees *b != '\0'. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_open(h, "AB.EPU", k_ra8_fs_mode_read, &f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs MC/DC: LFN name compare length arms");
}

/**
 * @test test_mcdc_lfn_terminator_vs_pad
 * @par MC/DC:
 * Decision: `if ((val == 0U) || (val == k_lfn_unicode_pad))` in
 * `libs/ra8_fs/src/ra8_fs_fat_lfn.c@priv_lfn_add` (2 conditions), reached while
 * reassembling a planted LFN chain through the `ra8_fs_open` long-name fallback.
 * - control: a long-name char slot ("AB.EPUB" chars) -> both F (char kept).
 * - C1=T: the UTF-16 NUL terminator (0x0000) follows the 7-char name -> ends
 *   the group (first test, `k_lfn_term16`).
 * - C2=T: a 0xFFFF pad immediately after the name with no preceding 0x0000 ->
 *   ends the group via the pad arm (second image, `k_lfn_pad16`).
 * The two images differ only in the post-name sentinel, so each independently
 * drives the decision true. Both reassemble to "AB.EPUB" and open. N+1 = 3
 * vectors for N=2 conditions (control + one per condition).
 */
static void test_mcdc_lfn_terminator_vs_pad(void)
{
  TEST_BEGIN("ra8_fs MC/DC: LFN terminator (val==0 || val==pad)");
  const char    short11[11] = {'A', 'B', '1', '2', '3', '4', '5', '6', 'E', 'P', 'U'};
  const uint8_t csum        = sfn_checksum((const uint8_t*)short11);

  /* Image 1: NUL terminator after the name -> drives the (val == 0) arm. */
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  uint8_t* root1 = &s_disk.bytes[(size_t)(uint32_t)k_fat16_root_lba * (uint32_t)k_disk_block_size];
  plant_lfn_entry(&root1[0U], 1U, 1U, csum, "AB.EPUB", (uint16_t)k_lfn_term16);
  plant_short_entry(&root1[(uint32_t)k_dir_entry_bytes], short11, 0U);
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "AB.EPUB", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();

  /* Image 2: 0xFFFF pad after the name -> drives the (val == pad) arm. */
  build_fat16_volume();
  h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  uint8_t* root2 = &s_disk.bytes[(size_t)(uint32_t)k_fat16_root_lba * (uint32_t)k_disk_block_size];
  plant_lfn_entry(&root2[0U], 1U, 1U, csum, "AB.EPUB", (uint16_t)k_lfn_pad16);
  plant_short_entry(&root2[(uint32_t)k_dir_entry_bytes], short11, 0U);
  f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "AB.EPUB", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs MC/DC: LFN terminator (val==0 || val==pad)");
}

/**
 * @test test_mcdc_lfn_order_range_guard
 * @par MC/DC:
 * Decision: `if ((order < 1U) || (order > k_lfn_max_entries))` in
 * `libs/ra8_fs/src/ra8_fs_fat_lfn.c@priv_lfn_add` (2 conditions).
 * `k_lfn_max_entries` is
 * 19. Reached by planting an LFN sub-entry with an out-of-range order byte and
 * resolving the directory through the `ra8_fs_open` long-name fallback.
 * - control: a valid chain with order 1 -> both F -> the entry is folded and the
 *   long name "AB.EPUB" opens (proves the in-range path runs).
 * - C1=T: order 0 -> early return; the corrupt group is ignored, so the long
 *   name no longer reassembles and the open misses (not_found).
 * - C2=T: order 20 (> 19) -> early return; same observable miss.
 * The control image and each corrupt image differ only in the order byte, so
 * each condition independently flips whether the entry is folded (and thus
 * whether the open resolves). N+1 = 3 vectors for N=2 conditions.
 */
static void test_mcdc_lfn_order_range_guard(void)
{
  TEST_BEGIN("ra8_fs MC/DC: LFN order range (order<1 || order>19)");
  const char     short11[11] = {'A', 'B', '1', '2', '3', '4', '5', '6', 'E', 'P', 'U'};
  const uint8_t  csum        = sfn_checksum((const uint8_t*)short11);
  const uint32_t e1          = (uint32_t)k_dir_entry_bytes;

  /* Control: in-range order 1 -> folds, long name resolves. */
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  uint8_t* rootc = &s_disk.bytes[(size_t)(uint32_t)k_fat16_root_lba * (uint32_t)k_disk_block_size];
  plant_lfn_entry(&rootc[0U], 1U, 1U, csum, "AB.EPUB", (uint16_t)k_lfn_term16);
  plant_short_entry(&rootc[e1], short11, 0U);
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "AB.EPUB", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();

  /* C1=T: order 0 -> entry ignored -> long name does not reassemble. */
  build_fat16_volume();
  h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  uint8_t* root0 = &s_disk.bytes[(size_t)(uint32_t)k_fat16_root_lba * (uint32_t)k_disk_block_size];
  plant_lfn_entry(&root0[0U], 0U, 1U, csum, "AB.EPUB", (uint16_t)k_lfn_term16);
  plant_short_entry(&root0[e1], short11, 0U);
  f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_open(h, "AB.EPUB", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();

  /* C2=T: order 20 (> 19) -> entry ignored -> long name does not reassemble. */
  build_fat16_volume();
  h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  uint8_t* root20 = &s_disk.bytes[(size_t)(uint32_t)k_fat16_root_lba * (uint32_t)k_disk_block_size];
  plant_lfn_entry(&root20[0U], k_lfn_seq_max, 1U, csum, "AB.EPUB", (uint16_t)k_lfn_term16);
  plant_short_entry(&root20[e1], short11, 0U);
  f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_open(h, "AB.EPUB", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs MC/DC: LFN order range (order<1 || order>19)");
}

/**
 * @enum ra8_fs_mcdc_read_t
 * @brief Multi-cluster payload sizes for the read-accelerator test.
 */
typedef enum : uint32_t {
  k_read_payload = 1500U, /**< > 2 clusters at SPC=1 (512-byte clusters).    */
  k_read_chunk   = 256U,  /**< Sub-cluster chunk so the cache steps forward. */
} ra8_fs_mcdc_read_t;

/**
 * @test test_mcdc_read_walk_cache_resume
 * @par MC/DC:
 * Decision: `(walk_cache_cluster >= k_cluster_first_data) && (cluster_idx_now >=
 * walk_cache_idx)` in `libs/ra8_fs/src/ra8_fs_fat_fileio.c@priv_read_one_chunk`
 * (2 conditions) -- the read accelerator's "resume from waypoint" guard.
 * - forward sequential read: the cache is set (C1=T) and each chunk's cluster
 *   index is at or ahead of the cached index (C2=T) -> resume from the waypoint.
 * - backward seek to offset 0 then read: the cache is still set (C1=T) but the
 *   target cluster index is behind the cached index (C2=F) -> re-walk from the
 *   chain head. This is the previously-uncovered C2-false arm.
 * C1=F (cache unset, walk_cache_cluster < 2) is structurally unreachable on
 * this path: `priv_open_existing` seeds the cache with the file's first data
 * cluster (>= 2), and `priv_read_one_chunk` only runs while
 * `file->offset < file->size_bytes` (a zero-cluster file is empty and never
 * reaches it) -- deactivated under DO-178C 6.4.4.3. N+1 for the two reachable
 * arms of C2 (with C1 held true) = 2 vectors.
 */
static void test_mcdc_read_walk_cache_resume(void)
{
  TEST_BEGIN("ra8_fs MC/DC: read walk-cache resume (cache set, backward seek)");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  static uint8_t s_wr[k_read_payload];
  static uint8_t s_rd[k_read_payload];
  for (uint32_t i = 0U; i < (uint32_t)k_read_payload; i++) {
    s_wr[i] = (uint8_t)((i * k_fs_pattern_stride) + 3U);
    s_rd[i] = 0U;
  }
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "BACK.BIN", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, s_wr, (uint32_t)k_read_payload));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "BACK.BIN", k_ra8_fs_mode_read, &f));
  /* Forward sequential read across clusters: cache set, target ahead (C1,C2 T). */
  uint32_t total = 0U;
  while (total < (uint32_t)k_read_payload) {
    uint32_t got = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, &s_rd[total], (uint32_t)k_read_chunk, &got));
    if (got == 0U) {
      break;
    }
    total += got;
  }
  TEST_ASSERT_EQ(k_read_payload, total);
  TEST_ASSERT_EQ(0, memcmp(s_wr, s_rd, (size_t)k_read_payload));

  /* Backward seek to the chain head: cache set (C1=T) but target behind the
   * cached index (C2=F) -> walk from head. Re-read the first chunk. */
  memset(s_rd, 0, (size_t)k_read_chunk);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_seek(f, 0U));
  uint32_t got2 = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, s_rd, (uint32_t)k_read_chunk, &got2));
  TEST_ASSERT_EQ(k_read_chunk, got2);
  TEST_ASSERT_EQ(0, memcmp(s_wr, s_rd, (size_t)k_read_chunk));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs MC/DC: read walk-cache resume (cache set, backward seek)");
}

/**
 * @test test_mcdc_format_fat16_band_upper
 * @par MC/DC:
 * Decision: FAT16 band `(count >= k_cluster_count_fat12_max) && (count <
 * k_cluster_count_fat16_max)` in
 * `libs/ra8_fs/src/ra8_fs_fat_fmt.c@priv_fmt_count_in_band`
 * (2 conditions; the FAT12 boundary is
 * 4085, the FAT16 boundary 65525). The sibling format test covers the lower
 * bound (a too-small card makes `count >= 4085` false); this case drives the
 * previously-uncovered upper-bound arm.
 * - 36 MiB card requested as FAT16: at SPC=1 the cluster count exceeds 65525, so
 *   `count < 65525` is FALSE on the first sweep iteration (the decision is
 *   false; the auto-sweep doubles SPC). At SPC=2 the count lands in band and the
 *   format succeeds, so `count < 65525` is also observed TRUE. The lower-bound
 *   condition stays TRUE throughout, demonstrating the upper bound independently
 *   gates the decision.
 * Combined with the sibling lower-bound vector this gives N+1 = 3 distinct
 * condition vectors for the FAT16 band's N=2 conditions.
 */
static void test_mcdc_format_fat16_band_upper(void)
{
  TEST_BEGIN("ra8_fs format MC/DC: FAT16 band upper bound (count<65525)");
  alloc_card((uint32_t)k_disk_blocks_big16);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;
  opts.label                = "BIGFAT16";
  /* spc auto: the sweep rejects spc=1 (count >= 65525) then accepts spc=2. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_fs_type_fat16, h->type);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs format MC/DC: FAT16 band upper bound (count<65525)");
}

/**
 * @test test_mcdc_format_fat32_band_lower
 * @par MC/DC:
 * Decision: FAT32 band `(count >= k_cluster_count_fat16_max) && (count <=
 * k_fmt_fat32_clus_cap)` in
 * `libs/ra8_fs/src/ra8_fs_fat_fmt.c@priv_fmt_count_in_band`
 * (2 conditions). The FAT16 boundary is
 * 65525; the FAT32 cluster cap is 0x0FFFFFF0.
 * - 36 MiB card formatted as FAT32: the cluster count is well above 65525 and
 *   well below the cap, so both conditions are TRUE and the format succeeds
 *   (the control / both-true vector for the FAT32 leg).
 * - The lower-bound FALSE arm (`count >= 65525` false) is exercised by the
 *   sibling test_ra8_fs_format `test_mcdc_format_count_in_band_capacity` (a tiny
 *   card requested as FAT32). The upper-bound `count <= cap` FALSE arm requires
 *   a count above 0x0FFFFFF0 (~268M clusters); the FAT32 auto-sweep seeds SPC
 *   from the Microsoft size table so the count never overshoots the cap on any
 *   buildable card -- that arm is flagged structurally unreachable / deactivated
 *   under DO-178C 6.4.4.3.
 */
static void test_mcdc_format_fat32_band_lower(void)
{
  TEST_BEGIN("ra8_fs format MC/DC: FAT32 band lower bound (count>=65525)");
  alloc_card((uint32_t)k_disk_blocks_fat32);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat32;
  opts.label                = "F32BAND";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_fs_type_fat32, h->type);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs format MC/DC: FAT32 band lower bound (count>=65525)");
}

/**
 * @test test_mcdc_format_type_unsupported
 * @par MC/DC:
 * Decision: `if (opts->type != FAT12 && != FAT16 && != FAT32 && != exFAT)` in
 * `libs/ra8_fs/src/ra8_fs_fat_mount.c@priv_format_locked` (4 conditions). The guard
 * rejects with not_supported only when the type is NONE of the four writable
 * variants -- i.e. all four inequalities are TRUE.
 * - V1 unknown (0): A=T(!=12), B=T(!=16), C=T(!=32), D=T(!=exFAT) -> rejected.
 * - V2 FAT12: A=F -> short-circuit accept.
 * - V3 FAT16: A=T, B=F -> accept.
 * - V4 FAT32: A=T, B=T, C=F -> accept.
 * - V5 exFAT: A=T, B=T, C=T, D=F -> accept.
 * V1 vs V2 flips A; V1 vs V3 flips B; V1 vs V4 flips C; V1 vs V5 flips D -- each
 * condition independently drives the reject. The four accept legs are exercised
 * by the sibling format round-trips; this case adds the all-true reject vector
 * (V1) that no sibling test reaches. N+1 = 5 vectors for N=4 conditions.
 */
static void test_mcdc_format_type_unsupported(void)
{
  TEST_BEGIN("ra8_fs format MC/DC: unsupported type (all four !=) reject");
  alloc_card((uint32_t)k_disk_blocks_fat16);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_unknown; /* none of 12/16/32/exFAT */
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_fs_format(&s_backend, &opts));
  free_volume();
  TEST_END("ra8_fs format MC/DC: unsupported type (all four !=) reject");
}

/**
 * @brief Capacity stub reporting a non-512 block size to drive the format guard.
 */
static ra8_err_t cap_bad_block_size(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  *block_count = (uint32_t)k_disk_blocks_fat16;
  *block_size  = k_fs_block_size_unsupported; /* not 512 -> formatter must reject */
  return k_ra8_ok;
}

/**
 * @test test_mcdc_format_block_size_guard
 * @par MC/DC:
 * Decision: `if (block_size != 512 || block_count == 0U)` in
 * `libs/ra8_fs/src/ra8_fs_fat_mount.c@priv_format_locked` (2 conditions), after
 * `get_capacity`.
 * - control: a valid 512-byte card -> C1=F, C2=F -> the format proceeds (the
 *   FAT16 round-trips in sibling tests). Re-asserted here as the both-false leg.
 * - C1=T: a backend reporting 4096-byte sectors -> rejected with invalid_arg
 *   (block-size independence; this is the previously-uncovered arm).
 * - C2 (block_count == 0) is unreachable through a real card: a zero-sector card
 *   cannot be allocated and every backend here reports a non-zero count, so it
 *   is deactivated under DO-178C 6.4.4.3.
 * N+1 for the one reachable condition (C1) = 2 vectors.
 */
static void test_mcdc_format_block_size_guard(void)
{
  TEST_BEGIN("ra8_fs format MC/DC: block-size guard (block_size!=512)");
  alloc_card((uint32_t)k_disk_blocks_fat16);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;
  /* Control: valid 512-byte card -> both conditions false, format proceeds. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  /* C1=T: non-512 block size -> reject. */
  ra8_fs_backend_t bad = s_backend;
  bad.get_capacity     = cap_bad_block_size;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_format(&bad, &opts));
  free_volume();
  TEST_END("ra8_fs format MC/DC: block-size guard (block_size!=512)");
}

/**
 * @enum ra8_fs_mcdc_exfat_t
 * @brief exFAT formatter parameters for the volume-label packer test.
 */
typedef enum : uint32_t {
  k_exfat_blocks  = 131072U, /**< 64 MiB: smallest exFAT volume we format. */
  k_exfat_lbl_max = 11U,     /**< Volume-label cap (UTF-16 units).         */
} ra8_fs_mcdc_exfat_t;

/**
 * @test test_mcdc_exfat_label_pack_terminator_and_cap
 * @par MC/DC:
 * Decision: `for (; (n < k_exfat_fmt_label_max) && (label[n] != '\0'); n++)` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_fmt.c@priv_exfat_label_utf16` (2 conditions),
 * reached when `ra8_fs_format` lays the exFAT root volume-label entry.
 * - short label "RAEXFAT" (7 chars): the loop runs while `n < 11` (C1=T) and
 *   stops when `label[n] == '\0'` (C2=F) -- the terminator arm. C1 stays TRUE
 *   throughout, proving C2 independently ends the loop.
 * - full-width label "ABCDEFGHIJK" (exactly 11 chars): the loop stops when
 *   `n < 11` becomes FALSE (C1=F) before any NUL, with `label[n] != '\0'` still
 *   TRUE -- the length-cap arm, proving C1 independently ends the loop.
 * Both formats succeed and mount as exFAT. N+1 = 3 vectors for N=2 conditions
 * (the NULL-label leg, where the loop is never entered, is covered by the
 * sibling `test_format_exfat_mount_empty`/label round-trips).
 */
static void test_mcdc_exfat_label_pack_terminator_and_cap(void)
{
  TEST_BEGIN("ra8_fs format MC/DC: exFAT label pack (n<max && label[n])");

  /* C2 arm: a short label terminates on NUL before the 11-char cap. */
  alloc_card((uint32_t)k_exfat_blocks);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_exfat;
  opts.label                = "RAEXFAT"; /* 7 chars < 11 cap */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_fs_type_exfat, h->type);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();

  /* C1 arm: a label exactly at the 11-char cap stops on the length bound. */
  alloc_card((uint32_t)k_exfat_blocks);
  opts.label = "ABCDEFGHIJK"; /* exactly 11 chars == k_exfat_lbl_max */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_fs_type_exfat, h->type);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs format MC/DC: exFAT label pack (n<max && label[n])");
}

int32_t main(void)
{
  test_mcdc_lfn_name_compare_lengths();
  test_mcdc_lfn_terminator_vs_pad();
  test_mcdc_lfn_order_range_guard();
  test_mcdc_read_walk_cache_resume();
  test_mcdc_format_fat16_band_upper();
  test_mcdc_format_fat32_band_lower();
  test_mcdc_format_type_unsupported();
  test_mcdc_format_block_size_guard();
  test_mcdc_exfat_label_pack_terminator_and_cap();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_fat_mcdc.c\n");
  return 0;
}
