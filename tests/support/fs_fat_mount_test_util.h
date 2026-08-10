/**
 * @file fs_fat_mount_test_util.h
 * @brief Shared fixture for test_ra8_fs_fat_mount_cov.c.
 *
 * @details
 * Header-only test fixture providing the heap-backed block device, the
 * error-injection backends (always-fail read, dummy write, failing /
 * zero-count capacity), and the FAT16 / protective-MBR / GPT-header builders
 * used by the mount coverage suite. Everything here has internal linkage, so
 * the including test executable owns a private copy of the fixture state.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "unity_minimal.h"

/* ===========================================================================
 * On-disk format offsets
 * ===========================================================================
 */

/**
 * @enum fat_bpb_off_t
 * @brief Byte offsets of the boot-sector fields written by the builders.
 *
 * @details Classic FAT BIOS Parameter Block layout plus the MBR partition
 *          fields that share sector 0. Only the fields the hand-built
 *          volumes populate are named here.
 *
 * @invariant All offsets lie inside one 512-byte boot sector.
 * @see build_fat16_volume()
 * @see write_protective_mbr()
 */
typedef enum : uint16_t {
  k_bpb_off_bytes_per_sec = 11U,    /**< BPB_BytsPerSec (u16).            */
  k_bpb_off_sec_per_clus  = 13U,    /**< BPB_SecPerClus (u8).             */
  k_bpb_off_rsvd_sec_cnt  = 14U,    /**< BPB_RsvdSecCnt (u16).            */
  k_bpb_off_num_fats      = 16U,    /**< BPB_NumFATs (u8).                */
  k_bpb_off_root_ent_cnt  = 17U,    /**< BPB_RootEntCnt (u16).            */
  k_bpb_off_tot_sec16     = 19U,    /**< BPB_TotSec16 (u16).              */
  k_bpb_off_fat_sz16      = 22U,    /**< BPB_FATSz16 (u16).               */
  k_mbr_off_part_type     = 0x1C2U, /**< Partition 0 type byte.           */
  k_mbr_off_part_lba      = 0x1C6U, /**< Partition 0 first LBA (u32 LE).  */
  k_bpb_off_sig0          = 510U,   /**< Boot signature low byte (0x55).  */
  k_bpb_off_sig1          = 511U,   /**< Boot signature high byte (0xAA). */
} fat_bpb_off_t;

/**
 * @enum gpt_hdr_off_t
 * @brief Byte offsets of the GPT header fields consulted by the mount path.
 *
 * @invariant All offsets lie inside one 512-byte GPT header sector (LBA 1).
 * @see write_gpt_header()
 */
typedef enum : uint8_t {
  k_gpt_off_entry_lba    = 0x48U, /**< Partition-entry-array first LBA (lo). */
  k_gpt_off_entry_lba_hi = 0x4CU, /**< Partition-entry-array first LBA (hi). */
  k_gpt_off_entry_count  = 0x50U, /**< Number of partition entries.          */
  k_gpt_off_entry_size   = 0x54U, /**< Bytes per partition entry.            */
} gpt_hdr_off_t;

/**
 * @enum fat_bpb_val_t
 * @brief Field values and byte-packing constants for the builders.
 *
 * @invariant k_bpb_sig0_val / k_bpb_sig1_val form the mandatory 0xAA55 boot
 *            signature.
 * @see put16()
 * @see put32()
 */
typedef enum : uint8_t {
  k_bpb_sig0_val = 0x55U, /**< First boot-signature byte.          */
  k_bpb_sig1_val = 0xAAU, /**< Second boot-signature byte.         */
  k_mbr_type_gpt = 0xEEU, /**< GPT protective partition type.      */
  k_byte_mask    = 0xFFU, /**< Low-byte mask for LE packing.       */
  k_shift_byte_2 = 16U,   /**< Third-byte shift for LE32 packing.  */
  k_shift_byte_3 = 24U,   /**< Fourth-byte shift for LE32 packing. */
} fat_bpb_val_t;

/* ===========================================================================
 * Constants
 * ===========================================================================
 */

/**
 * @enum ra8_fs_mc_k_t
 * @brief Geometry constants used throughout these tests.
 */
typedef enum : uint32_t {
  k_mc_blk   = 512U,       /**< Bytes per 512-byte sector.       */
  k_mc_fat16 = 8U * 1024U, /**< 4 MiB FAT16 disk (8192 sectors). */
} ra8_fs_mc_k_t;

/* ===========================================================================
 * Normal in-memory block device
 * ===========================================================================
 */

/**
 * @struct mc_disk_t
 * @brief Heap-backed flat sector store for the normal backend.
 */
typedef struct {
  uint8_t* bytes;       /**< Heap-backed byte array. */
  uint32_t block_count; /**< Total 512-byte sectors. */
} mc_disk_t;

/** @var s_disk
 * @brief Shared disk image used by most tests in this file.
 * @warning Direct access only from setup/teardown helpers.
 * @since 0.1.0
 */
static mc_disk_t s_disk = {};

static inline ra8_err_t mc_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  const mc_disk_t* d = (const mc_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf, &d->bytes[(size_t)lba * (uint32_t)k_mc_blk], (size_t)count * (uint32_t)k_mc_blk);
  return k_ra8_ok;
}

static inline ra8_err_t mc_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
{
  mc_disk_t* d = (mc_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_mc_blk], buf, (size_t)count * (uint32_t)k_mc_blk);
  return k_ra8_ok;
}

static inline ra8_err_t mc_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  const mc_disk_t* d = (const mc_disk_t*)ctx;
  *block_count       = d->block_count;
  *block_size        = (uint32_t)k_mc_blk;
  return k_ra8_ok;
}

/** @brief Backend wired to s_disk. */
static const ra8_fs_backend_t s_backend = {
  .read_block   = mc_read,
  .write_block  = mc_write,
  .get_capacity = mc_capacity,
  .ctx          = &s_disk,
};

/* ===========================================================================
 * Fail / dummy backends for error injection
 * ===========================================================================
 */

/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTNEXTLINE(readability-non-const-parameter)
static inline ra8_err_t always_fail_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  (void)lba;
  (void)count;
  (void)buf;
  return k_ra8_err_hw_error;
}

static inline ra8_err_t dummy_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
{
  (void)ctx;
  (void)lba;
  (void)count;
  (void)buf;
  return k_ra8_ok;
}

static inline ra8_err_t dummy_capacity_ok(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  *block_count = (uint32_t)k_mc_fat16;
  *block_size  = (uint32_t)k_mc_blk;
  return k_ra8_ok;
}

/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTNEXTLINE(readability-non-const-parameter)
static inline ra8_err_t fail_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  (void)block_count;
  (void)block_size;
  return k_ra8_err_hw_error;
}

static inline ra8_err_t zero_count_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  *block_count = 0U; /* well-formed sector size, no sectors */
  *block_size  = (uint32_t)k_mc_blk;
  return k_ra8_ok;
}

/* ===========================================================================
 * Helpers
 * ===========================================================================
 */

/**
 * @brief Store a 16-bit value little-endian at byte offset off.
 *
 * @param[out] p   Destination byte buffer.
 * @param[in]  off Byte offset of the low byte.
 * @param[in]  v   Value to store.
 *
 * @pre p has at least off + 2 bytes of space.
 * @post p[off] and p[off + 1] hold v little-endian.
 *
 * @note Trivially thread-safe (writes only through p).
 * @since 0.1.0
 */
static inline void put16(uint8_t* p, uint32_t off, uint16_t v)
{
  p[off]     = (uint8_t)(v & k_byte_mask);
  p[off + 1] = (uint8_t)((v >> 8U) & k_byte_mask);
}

/**
 * @brief Store a 32-bit value little-endian at byte offset off.
 *
 * @param[out] p   Destination byte buffer.
 * @param[in]  off Byte offset of the low byte.
 * @param[in]  v   Value to store.
 *
 * @pre p has at least off + 4 bytes of space.
 * @post p[off..off+3] hold v little-endian.
 *
 * @note Trivially thread-safe (writes only through p).
 * @since 0.1.0
 */
static inline void put32(uint8_t* p, uint32_t off, uint32_t v)
{
  p[off]     = (uint8_t)(v & k_byte_mask);
  p[off + 1] = (uint8_t)((v >> 8U) & k_byte_mask);
  p[off + 2] = (uint8_t)((v >> (uint32_t)k_shift_byte_2) & k_byte_mask);
  p[off + 3] = (uint8_t)((v >> (uint32_t)k_shift_byte_3) & k_byte_mask);
}

/**
 * @brief Allocate a zeroed multi-sector disk and install it as s_disk.
 *
 * @param[in] sectors Number of 512-byte sectors to allocate.
 *
 * @pre sectors > 0.
 * @post s_disk.bytes holds a calloc-zeroed array; s_disk.block_count == sectors.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static inline void alloc_disk(uint32_t sectors)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
  s_disk.block_count = sectors;
  s_disk.bytes       = (uint8_t*)calloc(1, (size_t)sectors * (size_t)k_mc_blk);
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
}

/**
 * @brief Free s_disk.bytes and reset the disk descriptor.
 *
 * @pre None.
 * @post s_disk.bytes is nullptr.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static inline void free_disk(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
}

/**
 * @brief Build a minimal valid FAT16 BPB at sector 0 of s_disk.
 *
 * @details SPC=1, 2 FATs each 32 sectors, 16 root entries, 8192 total sectors.
 *          Geometry: first_fat=1, first_root=65, first_data=66.
 *
 * @pre s_disk.bytes is nullptr.
 * @post s_disk holds a calloc-zeroed 4 MiB image with a valid FAT16 BPB.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static inline void build_fat16_volume(void)
{
  alloc_disk((uint32_t)k_mc_fat16);
  uint8_t* bpb = s_disk.bytes;
  put16(bpb, (uint32_t)k_bpb_off_bytes_per_sec, (uint16_t)k_mc_blk);
  bpb[k_bpb_off_sec_per_clus] = 1U;
  put16(bpb, (uint32_t)k_bpb_off_rsvd_sec_cnt, 1U);
  bpb[k_bpb_off_num_fats] = 2U;
  put16(bpb, (uint32_t)k_bpb_off_root_ent_cnt, 16U);
  put16(bpb, (uint32_t)k_bpb_off_tot_sec16, (uint16_t)k_mc_fat16);
  put16(bpb, (uint32_t)k_bpb_off_fat_sz16, 32U);
  bpb[k_bpb_off_sig0] = (uint8_t)k_bpb_sig0_val;
  bpb[k_bpb_off_sig1] = (uint8_t)k_bpb_sig1_val;
}

/**
 * @brief Write a protective MBR into a sector buffer.
 *
 * @details Sets the 0x55/0xAA boot signature, partition type 0xEE (GPT
 *          protective), and the partition's first LBA at MBR offset 0x1C6.
 *          Leaves bytes_per_sector (offset 11-12) at zero so the subsequent
 *          BPB parse fails and the MBR-detection branch is taken.
 *
 * @param[in] buf      Pointer to a 512-byte sector buffer.
 * @param[in] part_lba Partition first LBA to embed (little-endian).
 *
 * @pre buf is non-NULL and points to at least 512 writable bytes.
 * @post buf[510]=0x55, buf[511]=0xAA, buf[0x1C2]=0xEE.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static inline void write_protective_mbr(uint8_t* buf, uint32_t part_lba)
{
  buf[k_bpb_off_sig0]      = (uint8_t)k_bpb_sig0_val;
  buf[k_bpb_off_sig1]      = (uint8_t)k_bpb_sig1_val;
  buf[k_mbr_off_part_type] = (uint8_t)k_mbr_type_gpt;
  put32(buf, (uint32_t)k_mbr_off_part_lba, part_lba);
}

/**
 * @brief Write a GPT header with the "EFI PART" signature into a sector buffer.
 *
 * @details Only the four fields consulted by priv_gpt_locate_volume are set:
 *          signature (bytes 0-7), entry_lba (0x48), entry_lba_hi (0x4C),
 *          entry_count (0x50), and entry_size (0x54).
 *
 * @param[in] buf          Pointer to a 512-byte sector buffer (LBA 1).
 * @param[in] entry_lba    Low 32 bits of the partition-entry-array first LBA.
 * @param[in] entry_lba_hi High 32 bits of the same field (0 for standard disks).
 * @param[in] count        Number of partition entries.
 * @param[in] size         Bytes per partition entry (128 for standard GPT).
 *
 * @pre buf is non-NULL and points to at least 512 writable bytes.
 * @post buf[0..7] == "EFI PART"; geometry fields are set at their GPT offsets.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static inline void write_gpt_header(uint8_t* buf,
                                    uint32_t entry_lba,
                                    uint32_t entry_lba_hi,
                                    uint32_t count,
                                    uint32_t size)
{
  /* "EFI PART" -- UEFI spec 2.10 table 5.5. */
  static const uint8_t k_efi[8] = {0x45U, 0x46U, 0x49U, 0x20U, 0x50U, 0x41U, 0x52U, 0x54U};
  memcpy(buf, k_efi, 8U);
  put32(buf, (uint32_t)k_gpt_off_entry_lba, entry_lba);
  put32(buf, (uint32_t)k_gpt_off_entry_lba_hi, entry_lba_hi);
  put32(buf, (uint32_t)k_gpt_off_entry_count, count);
  put32(buf, (uint32_t)k_gpt_off_entry_size, size);
}
