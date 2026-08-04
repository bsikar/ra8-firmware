/**
 * @file test_ra8_fs_fat_lfn_mcdc.c
 * @brief MC/DC vectors for the compound decisions in ra8_fs_fat_lfn.c.
 *
 * @details
 * Dedicated N+1 independent-influence vector sets (issue #426) for the
 * directory / long-name helpers:
 *
 *   - `priv_lfn_name_for`        -- the "no reassembled name yet" guard, driven
 *                                   directly against a crafted `lfn_state_t`.
 *   - `priv_dir_find_free`       -- the free-slot marker test, driven directly
 *                                   against a planted root directory image.
 *   - `priv_dir_find_long_sector`-- the long-name match test, driven through
 *                                   `ra8_fs_open`'s non-8.3 long-name fallback.
 *   - `priv_free_chain`          -- the cluster-chain walk bound, covered in full
 *                                   by driving its corrupt-chain FALSE arms
 *                                   directly as out-of-contract robustness
 *                                   vectors.
 *
 * The in-memory block-device harness and the LFN-image planters mirror
 * `tests/test_ra8_fs_fat_mcdc.c`.
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
#include "ra8_fs_fat_internal.h"
#include "unity_minimal.h"

/**
 * @enum ra8_fs_lfn_mcdc_t
 * @brief Disk sizing, planting offsets and payloads for the LFN vectors.
 *
 * @details Names are prefixed `k_lt_` so they never collide with the on-disk
 *          layout enums pulled in from `ra8_fs_fat_internal.h`.
 */
typedef enum : uint32_t {
  k_lt_block_size    = 512U,       /**< Bytes per logical block.                */
  k_lt_blocks_fat16  = 8U * 1024U, /**< 4 MiB FAT16 card.                       */
  k_lt_off_first_lo  = 26U,        /**< DIR_FstClusLO byte offset.              */
  k_lt_off_size      = 28U,        /**< DIR_FileSize byte offset.               */
  k_lt_chain_payload = 1500U,      /**< > 2 clusters at SPC=1 -> a real chain.  */
  k_lt_byte_mask     = 0xFFU,      /**< Low-byte mask.                          */
  k_lt_shift_byte3   = 24U,        /**< Shift for the top payload byte.         */
  k_lt_name_len      = 11U,        /**< Packed 8.3 name width (8 base + 3 ext). */
  k_lt_lfn_chars     = 13U,        /**< UTF-16 chars one LFN sub-entry carries. */
  k_lt_poison        = 123U,       /**< Sentinel proving an out-param is set.   */
} ra8_fs_lfn_mcdc_t;

/**
 * @enum ra8_fs_lfn_mcdc_sig_t
 * @brief BPB byte offsets, geometry and boot signature for the FAT16 builder.
 */
typedef enum : uint16_t {
  k_lt_bpb_bytes_per_sec = 11U,     /**< BPB_BytsPerSec.                */
  k_lt_bpb_sec_per_clus  = 13U,     /**< BPB_SecPerClus.                */
  k_lt_bpb_rsvd          = 14U,     /**< BPB_RsvdSecCnt.                */
  k_lt_bpb_num_fats      = 16U,     /**< BPB_NumFATs.                   */
  k_lt_bpb_root_ents     = 17U,     /**< BPB_RootEntCnt.                */
  k_lt_bpb_tot_sec16     = 19U,     /**< BPB_TotSec16.                  */
  k_lt_bpb_fatsz16       = 22U,     /**< BPB_FATSz16.                   */
  k_lt_bpb_sig_lo        = 510U,    /**< 0x55 signature.                */
  k_lt_bpb_sig_hi        = 511U,    /**< 0xAA signature.                */
  k_lt_pad16             = 0xFFFFU, /**< UTF-16 pad after a group name. */
} ra8_fs_lfn_mcdc_sig_t;

/**
 * @enum ra8_fs_lfn_mcdc_b_t
 * @brief Small byte-valued geometry and sentinels.
 */
typedef enum : uint8_t {
  k_lt_sig_lo        = 0x55U, /**< Boot signature low byte.       */
  k_lt_sig_hi        = 0xAAU, /**< Boot signature high byte.      */
  k_lt_rsvd          = 1U,    /**< Reserved sectors.              */
  k_lt_num_fats      = 2U,    /**< FAT copies.                    */
  k_lt_fatsz         = 16U,   /**< Sectors per FAT.               */
  k_lt_root_ents     = 16U,   /**< Root entries.                  */
  k_lt_occupied_mark = 'A',   /**< A live 8.3 entry's first byte. */
  k_lt_term16_lo     = 0x00U, /**< UTF-16 terminator low byte.    */
} ra8_fs_lfn_mcdc_b_t;

/** @brief Byte offsets of the 13 UTF-16 chars within a 32-byte LFN entry. */
static const uint8_t s_lfn_char_off[k_lt_lfn_chars] =
  {1U, 3U, 5U, 7U, 9U, 14U, 16U, 18U, 20U, 22U, 24U, 28U, 30U};

/** @brief Memory-backed block device handed to ra8_fs. */
typedef struct {
  uint8_t* bytes;       /**< Flat sector store.         */
  uint32_t block_count; /**< Number of 512-byte blocks. */
} mem_disk_t;

static mem_disk_t s_disk = {};

static ra8_err_t mem_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_lt_block_size],
         (size_t)count * (uint32_t)k_lt_block_size);
  return k_ra8_ok;
}

static ra8_err_t mem_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_lt_block_size],
         buf,
         (size_t)count * (uint32_t)k_lt_block_size);
  return k_ra8_ok;
}

static ra8_err_t mem_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  *block_count  = d->block_count;
  *block_size   = (uint32_t)k_lt_block_size;
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
  p[off]     = (uint8_t)(v & (uint16_t)k_lt_byte_mask);
  p[off + 1] = (uint8_t)((v >> 8) & (uint16_t)k_lt_byte_mask);
}

static void put32(uint8_t* p, uint32_t off, uint32_t v)
{
  p[off]     = (uint8_t)(v & (uint32_t)k_lt_byte_mask);
  p[off + 1] = (uint8_t)((v >> 8) & (uint32_t)k_lt_byte_mask);
  p[off + 2] = (uint8_t)((v >> 16) & (uint32_t)k_lt_byte_mask);
  p[off + 3] = (uint8_t)((v >> (uint32_t)k_lt_shift_byte3) & (uint32_t)k_lt_byte_mask);
}

static void free_volume(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
}

static void build_fat16_volume(void)
{
  free_volume();
  s_disk.block_count = (uint32_t)k_lt_blocks_fat16;
  s_disk.bytes       = (uint8_t*)calloc(1, (size_t)s_disk.block_count * (uint32_t)k_lt_block_size);
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
  uint8_t* bpb = &s_disk.bytes[0];
  put16(bpb, (uint32_t)k_lt_bpb_bytes_per_sec, (uint16_t)k_lt_block_size);
  bpb[(uint32_t)k_lt_bpb_sec_per_clus] = 1U;
  put16(bpb, (uint32_t)k_lt_bpb_rsvd, (uint16_t)k_lt_rsvd);
  bpb[(uint32_t)k_lt_bpb_num_fats] = (uint8_t)k_lt_num_fats;
  put16(bpb, (uint32_t)k_lt_bpb_root_ents, (uint16_t)k_lt_root_ents);
  put16(bpb, (uint32_t)k_lt_bpb_tot_sec16, (uint16_t)k_lt_blocks_fat16);
  put16(bpb, (uint32_t)k_lt_bpb_fatsz16, (uint16_t)k_lt_fatsz);
  bpb[(uint32_t)k_lt_bpb_sig_lo] = (uint8_t)k_lt_sig_lo;
  bpb[(uint32_t)k_lt_bpb_sig_hi] = (uint8_t)k_lt_sig_hi;
}

/** @brief The rotate-right-add 8.3 checksum (MS FAT spec sec 7). */
static uint8_t sfn_checksum(const uint8_t* name11)
{
  uint8_t sum = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_lt_name_len; i++) {
    const uint32_t high = ((sum & 1U) != 0U) ? (uint32_t)k_sfn_csum_high_bit : 0U;
    sum                 = (uint8_t)(high + (uint32_t)(sum >> 1U) + (uint32_t)name11[i]);
  }
  return sum;
}

/** @brief Write a packed 8.3 directory entry (attr archive, size bytes). */
static void plant_short_entry(uint8_t* ent, const char name11[k_lt_name_len], uint32_t size)
{
  memset(ent, 0, (size_t)k_ra8_fs_dir_entry_bytes);
  memcpy(ent, name11, (size_t)k_lt_name_len);
  ent[(uint32_t)k_dir_off_attr] = (uint8_t)k_ra8_fs_attr_archive;
  put16(ent, (uint32_t)k_lt_off_first_lo, 0U);
  put32(ent, (uint32_t)k_lt_off_size, size);
}

/** @brief Write one last-group LFN sub-entry carrying @p text (<= 13 chars). */
static void plant_lfn_entry(uint8_t* ent, uint8_t order, uint8_t csum, const char* text)
{
  memset(ent, 0, (size_t)k_ra8_fs_dir_entry_bytes);
  ent[(uint32_t)k_lfn_off_seq]      = (uint8_t)(order | (uint8_t)k_lfn_seq_last);
  ent[(uint32_t)k_dir_off_attr]     = (uint8_t)k_ra8_fs_attr_lfn;
  ent[(uint32_t)k_lfn_off_checksum] = csum;
  const uint32_t len                = (uint32_t)strlen(text);
  for (uint32_t i = 0U; i < (uint32_t)k_lt_lfn_chars; i++) {
    const uint32_t off = (uint32_t)s_lfn_char_off[i];
    if (i < len) {
      put16(ent, off, (uint16_t)(uint8_t)text[i]);
    } else if (i == len) {
      put16(ent, off, (uint16_t)k_lt_term16_lo); /* UTF-16 NUL terminator */
    } else {
      put16(ent, off, (uint16_t)k_lt_pad16); /* pad */
    }
  }
}

/**
 * @test test_mcdc_lfn_name_for_guard
 * @par MC/DC:
 * Decision: `if ((s->have == 0U) || (s->name[0] == '\0'))` in
 * `libs/ra8_fs/src/ra8_fs_fat_lfn.c@priv_lfn_name_for` (2 conditions). Driven
 * directly against a crafted reassembly state.
 * - V1: have=1, name[0]='A' -> C1=F, C2=F -> dec F (returns the name).
 * - V2: have=0             -> C1=T short -> dec T -> nullptr.
 * - V3: have=1, name[0]='\0' -> C1=F, C2=T -> dec T -> nullptr.
 * V1+V2 isolate the have flag; V1+V3 isolate the empty-name test. The V1 state
 * carries the matching checksum so the following (single-condition) checksum
 * gate lets the name through. N+1 = 3 for N=2.
 */
static void test_mcdc_lfn_name_for_guard(void)
{
  TEST_BEGIN("ra8_fs MC/DC: priv_lfn_name_for have/empty guard");
  const char    name83[11] = {'A', 'B', '1', '2', '3', '4', '5', '6', 'E', 'P', 'U'};
  const uint8_t csum       = sfn_checksum((const uint8_t*)name83);

  lfn_state_t s = {};
  s.have        = 1U;
  s.checksum    = csum;
  s.name[0]     = 'A';
  s.name[1]     = '\0';
  TEST_ASSERT_NOT_NULL(priv_lfn_name_for(&s, (const uint8_t*)name83)); /* V1 */

  lfn_state_t s_nohave = {};
  s_nohave.have        = 0U;
  s_nohave.name[0]     = 'A';
  TEST_ASSERT_NULL(priv_lfn_name_for(&s_nohave, (const uint8_t*)name83)); /* V2 */

  lfn_state_t s_empty = {};
  s_empty.have        = 1U;
  s_empty.checksum    = csum;
  s_empty.name[0]     = '\0';
  TEST_ASSERT_NULL(priv_lfn_name_for(&s_empty, (const uint8_t*)name83)); /* V3 */
  TEST_END("ra8_fs MC/DC: priv_lfn_name_for have/empty guard");
}

/**
 * @test test_mcdc_dir_find_free_marker
 * @par MC/DC:
 * Decision: `if (ent[name] == k_dir_marker_free_perm || ent[name] ==
 * k_dir_marker_free_used)` in
 * `libs/ra8_fs/src/ra8_fs_fat_lfn.c@priv_dir_find_free` (2 conditions), driven
 * directly against a planted root directory.
 * - Root A: entry0 = live ('A'), entry1 = free-used (0xE5). Scanning entry0 sees
 *   0x41 != 0x00 (C1=F) and 0x41 != 0xE5 (C2=F) -> dec F, skip; entry1 sees C1=F,
 *   C2=T -> dec T -> returns entry1. Covers the both-false control and C2=T.
 * - Root B: entry0 = free-perm (0x00) -> C1=T short -> dec T -> returns entry0.
 *   Covers C1=T.
 * The two roots differ only in entry0's marker, isolating each condition.
 * N+1 = 3 for N=2.
 */
static void test_mcdc_dir_find_free_marker(void)
{
  TEST_BEGIN("ra8_fs MC/DC: priv_dir_find_free marker (perm || used)");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t ent1 = (uint32_t)k_ra8_fs_dir_entry_bytes;
  uint8_t*       root = &s_disk.bytes[(size_t)h->first_root_lba * (uint32_t)k_lt_block_size];
  dir_loc_t      loc  = {};
  loc.is_root         = 1U;

  /* Root A: live entry0 then free-used entry1 -> F,F then C2=T. */
  root[(uint32_t)k_dir_off_name]        = (uint8_t)k_lt_occupied_mark;
  root[ent1 + (uint32_t)k_dir_off_name] = (uint8_t)k_dir_marker_free_used;
  uint32_t lba                          = 0U;
  uint32_t off                          = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, priv_dir_find_free(h, &loc, &lba, &off));
  TEST_ASSERT_EQ(ent1, off); /* entry1 chosen */

  /* Root B: free-perm entry0 -> C1=T. */
  root[(uint32_t)k_dir_off_name] = (uint8_t)k_dir_marker_free_perm;
  lba                            = 0U;
  off                            = (uint32_t)k_lt_poison;
  TEST_ASSERT_EQ(k_ra8_ok, priv_dir_find_free(h, &loc, &lba, &off));
  TEST_ASSERT_EQ(0U, off); /* entry0 chosen */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs MC/DC: priv_dir_find_free marker (perm || used)");
}

/**
 * @test test_mcdc_dir_find_long_sector_match
 * @par MC/DC:
 * Decision: `if ((lname != nullptr) && (priv_name_ieq(needle, lname) != 0U))` in
 * `libs/ra8_fs/src/ra8_fs_fat_lfn.c@priv_dir_find_long_sector` (2 conditions),
 * reached through `ra8_fs_open`'s non-8.3 long-name fallback. The planted root
 * holds one LFN chain for "AB.EPUB", its 8.3 entry, and one unrelated plain 8.3
 * entry ("CD" with no LFN chain).
 * - Open "AB.EPUB": at the LFN's 8.3 entry, `lname`="AB.EPUB" (C1=T) and the
 *   name matches (C2=T) -> dec T -> found (ok).
 * - Open "AB.EPUBX" (a strict-superstring, still non-8.3): at the LFN's 8.3
 *   entry C1=T but the names differ (C2=F) -> dec F; the chain resets and the
 *   plain "CD" entry has no reassembled name so `lname`==nullptr (C1=F) -> dec F;
 *   the directory ends -> not_found.
 * The exact-match open isolates C2=T; the miss open exercises C2=F (at the AB
 * entry) and C1=F (at the CD entry). N+1 = 3 for N=2.
 */
static void test_mcdc_dir_find_long_sector_match(void)
{
  TEST_BEGIN("ra8_fs MC/DC: priv_dir_find_long_sector (lname && name_ieq)");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  const char     ab11[11] = {'A', 'B', '1', '2', '3', '4', '5', '6', 'E', 'P', 'U'};
  const char     cd11[11] = {'C', 'D', '1', '2', '3', '4', '5', '6', 'T', 'X', 'T'};
  const uint8_t  csum     = sfn_checksum((const uint8_t*)ab11);
  const uint32_t eb       = (uint32_t)k_ra8_fs_dir_entry_bytes;
  uint8_t*       root     = &s_disk.bytes[(size_t)h->first_root_lba * (uint32_t)k_lt_block_size];
  plant_lfn_entry(&root[0U], 1U, csum, "AB.EPUB");
  plant_short_entry(&root[eb], ab11, 0U);
  plant_short_entry(&root[(size_t)eb * 2U], cd11, 0U);

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "AB.EPUB", k_ra8_fs_mode_read, &f)); /* C2=T */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  /* Miss drives C2=F at the AB entry then C1=F at the CD entry. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_open(h, "AB.EPUBX", k_ra8_fs_mode_read, &f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs MC/DC: priv_dir_find_long_sector (lname && name_ieq)");
}

/**
 * @test test_mcdc_free_chain_walk_bound
 * @par MC/DC:
 * Decision: `while (cur >= k_cluster_first_data && (cur - k_cluster_first_data)
 * < m->count_of_clusters)` in
 * `libs/ra8_fs/src/ra8_fs_fat_lfn.c@priv_free_chain` (2 conditions).
 *
 * This is a defensive bound: on any chain reachable through the allocator every
 * cluster satisfies both conditions, so the loop always exits through the
 * interior end-of-chain `break` and a FALSE arm is reached only on a corrupt
 * chain (the source carries the matching `mcdc-deactivated` production-
 * reachability note). Full MC/DC is nonetheless obtained by supplying those
 * out-of-contract cluster indices directly as robustness vectors:
 * - V1 (T,T): freeing a real >2-cluster file chain runs the body (both
 *   conditions true) for each cluster.
 * - V2 (C1=F): start = 0 (< k_cluster_first_data) -> the loop is skipped.
 * - V3 (C2=F): start = k_cluster_first_data + count_of_clusters (past the
 *   volume) -> the loop is skipped.
 * V1 vs V2 isolates the lower bound; V1 vs V3 isolates the upper bound. All
 * three calls succeed (k_ra8_ok). N+1 = 3 for N=2.
 */
static void test_mcdc_free_chain_walk_bound(void)
{
  TEST_BEGIN("ra8_fs MC/DC: priv_free_chain walk bound (robustness vectors)");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* A real multi-cluster file gives a valid chain to walk (dec T). */
  static uint8_t s_payload[k_lt_chain_payload] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_lt_chain_payload; i++) {
    s_payload[i] = (uint8_t)(i & (uint32_t)k_lt_byte_mask);
  }
  ra8_fs_file_t* wf = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "CHAIN.BIN", k_ra8_fs_mode_write, &wf));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(wf, s_payload, (uint32_t)k_lt_chain_payload));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(wf));
  ra8_fs_file_t* rf = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "CHAIN.BIN", k_ra8_fs_mode_read, &rf));
  const uint32_t head = rf->first_cluster;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(rf));

  /* dec T: walk the real chain. */
  TEST_ASSERT_EQ(k_ra8_ok, priv_free_chain(h, head));
  /* dec F via C1: start below the first data cluster. */
  TEST_ASSERT_EQ(k_ra8_ok, priv_free_chain(h, 0U));
  /* dec F via C2: start past the last valid cluster. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 priv_free_chain(h, (uint32_t)k_cluster_first_data + h->count_of_clusters));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs MC/DC: priv_free_chain walk bound (robustness vectors)");
}

int32_t main(void)
{
  test_mcdc_lfn_name_for_guard();
  test_mcdc_dir_find_free_marker();
  test_mcdc_dir_find_long_sector_match();
  test_mcdc_free_chain_walk_bound();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_fat_lfn_mcdc.c\n");
  return 0;
}
