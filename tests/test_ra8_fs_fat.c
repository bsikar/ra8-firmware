/**
 * @file test_ra8_fs_fat.c
 * @brief MC/DC unit tests for the FAT filesystem adapter (ra8_fs_fat.c).
 *
 * @details
 * Sibling of ``tests/test_ra8_fs.c`` -- that file holds the broad
 * happy-path / unhappy-path coverage of the public ``ra8_fs`` API. This
 * file exists separately to document and exercise the **Modified
 * Condition/Decision Coverage** vectors required by DO-178C Level B
 * and IEC 61508 SIL 3 for the compound boolean decisions inside
 * ``libs/ra8_fs/src/ra8_fs_fat.c``.
 *
 * Each test follows the canonical N+1 pattern (see
 * ``tests/test_ra8_xspi.c::test_set_xip_mode_mcdc_addr_bytes`` for the
 * reference template): for an N-condition decision we exercise N+1
 * input vectors so every condition is observed both true and false
 * AND independently flips the decision outcome.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "unity_minimal.h"

/**
 * @enum fat_bpb_field_t
 * @brief Byte offsets of the BPB fields this fixture writes, per the FAT specification.
 *
 * @details
 * The fixture hand-builds a boot sector instead of calling a formatter, so
 * every field is placed at its specified offset. The names are the
 * specification's own, so the layout can be checked against the document
 * without decoding the numbers.
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
 * @enum fat_fixture_t
 * @brief Byte values and payload sizes this fixture stimulates the mount with.
 */
typedef enum : uint16_t {
  k_bpb_sig_lo     = 0x55U, /**< Boot-signature low byte.                */
  k_bpb_sig_hi     = 0xAAU, /**< Boot-signature high byte.               */
  k_byte_mask      = 0xFFU, /**< Low-byte mask used by the put16 helper. */
  k_fat_poison_out = 99U, /**< Poison written into an out-parameter before a call, so a callee that
              leaves it untouched fails the assertion instead of coasting on a
              stale zero.                                                          */
  k_fat_bad_bytes_per_sec =
    256U, /**< A BPB_BytsPerSec disagreeing with the backend's 512-byte block; the
               mount must reject it.                                               */
  k_fat_bytes_multi_cluster = 1500U, /**< Payload spanning more than one cluster. */
} fat_fixture_t;

/**
 * @enum ra8_fs_mcdc_disk_t
 * @brief Static-allocation sizes for the synthetic block device.
 */
typedef enum : uint32_t {
  k_disk_block_size   = 512U,       /**< Disk block size.   */
  k_disk_blocks_fat16 = 8U * 1024U, /**< Disk blocks fat16. */
} ra8_fs_mcdc_disk_t;

typedef struct {
  uint8_t* bytes;       /**< Bytes.       */
  uint32_t block_count; /**< Block count. */
  uint32_t byte_count;  /**< Byte count.  */
} mem_disk_t;

static mem_disk_t s_disk = {};

/** @brief Perform the mem read filesystem operation. @details Implements the bounded mem read fixture step using caller-owned state. @param[in,out] ctx Caller-owned fixture or filesystem state. @param[in] lba Value required by this filesystem vector. @param[in] count Caller-supplied bounded extent or quantity. @param[in,out] buf Caller-owned bounded byte storage. @return Status, selected object, or bounded value produced by the named operation. @retval k_ra8_ok The requested operation completed. @retval k_ra8_err_* Validation or backend work failed. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. */
RA8_INTERNAL static ra8_err_t
internal_mem_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
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

/** @brief Perform the mem write filesystem operation. @details Implements the bounded mem write fixture step using caller-owned state. @param[in,out] ctx Caller-owned fixture or filesystem state. @param[in] lba Value required by this filesystem vector. @param[in] count Caller-supplied bounded extent or quantity. @param[in] buf Caller-owned bounded byte storage. @return Status, selected object, or bounded value produced by the named operation. @retval k_ra8_ok The requested operation completed. @retval k_ra8_err_* Validation or backend work failed. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. */
RA8_INTERNAL static ra8_err_t
internal_mem_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
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

/** @brief Perform the mem capacity filesystem operation. @details Implements the bounded mem capacity fixture step using caller-owned state. @param[in,out] ctx Caller-owned fixture or filesystem state. @param[in,out] block_count Caller-supplied bounded extent or quantity. @param[in,out] block_size Caller-supplied bounded extent or quantity. @return Status, selected object, or bounded value produced by the named operation. @retval k_ra8_ok The requested operation completed. @retval k_ra8_err_* Validation or backend work failed. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. */
RA8_INTERNAL static ra8_err_t
internal_mem_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  *block_count  = d->block_count;
  *block_size   = (uint32_t)k_disk_block_size;
  return k_ra8_ok;
}

static const ra8_fs_backend_t s_backend = {
  .read_block   = internal_mem_read,
  .write_block  = internal_mem_write,
  .get_capacity = internal_mem_capacity,
  .ctx          = &s_disk,
};

/** @brief Perform the put16 filesystem operation. @details Implements the bounded put16 fixture step using caller-owned state. @param[in,out] p Value required by this filesystem vector. @param[in] off Value required by this filesystem vector. @param[in] v Value required by this filesystem vector. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. */
RA8_INTERNAL static void internal_put16(uint8_t* p, uint32_t off, uint16_t v)
{
  p[off]     = (uint8_t)(v & k_byte_mask);
  p[off + 1] = (uint8_t)((v >> 8) & k_byte_mask);
}

/** @brief Perform the build fat16 volume filesystem operation. @details Implements the bounded build fat16 volume fixture step using caller-owned state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. */
RA8_INTERNAL static void internal_build_fat16_volume(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
  s_disk.byte_count  = (uint32_t)k_disk_blocks_fat16 * (uint32_t)k_disk_block_size;
  s_disk.bytes       = (uint8_t*)calloc(1, s_disk.byte_count);
  s_disk.block_count = (uint32_t)k_disk_blocks_fat16;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
  uint8_t* bpb = &s_disk.bytes[0];
  internal_put16(bpb, k_bpb_off_bytes_per_sec, (uint16_t)k_disk_block_size);
  bpb[k_bpb_off_sec_per_clus] = 1U;
  internal_put16(bpb, k_bpb_off_rsvd_sec_cnt, 1U);
  bpb[16] = 2U;
  internal_put16(bpb, k_bpb_off_root_ent_cnt, 16U);
  internal_put16(bpb, k_bpb_off_tot_sec16, (uint16_t)k_disk_blocks_fat16);
  internal_put16(bpb, k_bpb_off_fat_sz16, 32U);
  bpb[k_bpb_off_sig_lo] = k_bpb_sig_lo;
  bpb[k_bpb_off_sig_hi] = k_bpb_sig_hi;
}

/** @brief Perform the free volume filesystem operation. @details Implements the bounded free volume fixture step using caller-owned state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. */
RA8_INTERNAL static void internal_free_volume(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
}

/**
 * @test test_mcdc_mount_args_pair
 * @par MC/DC:
 * Decision: `if (backend == NULL || out_handle == NULL)` (2 conditions,
 * libs/ra8_fs/src/ra8_fs_fat.c line 867, function `ra8_fs_mount`).
 * V1 both non-NULL -> F. V2 backend=NULL -> C1=T -> T. V3 out=NULL -> C1=F,C2=T -> T.
 * N+1 = 3 vectors for N=2. @brief Exercise the mcdc mount args pair filesystem operation. @details Runs the mcdc mount args pair vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_mount_args_pair(void)
{
  TEST_BEGIN("ra8_fs MC/DC: mount (backend||out) NULL pair");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_mount(nullptr, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_mount(&s_backend, nullptr));
  internal_free_volume();
  TEST_END("ra8_fs MC/DC: mount (backend||out) NULL pair");
}

/**
 * @test test_mcdc_mount_backend_fn_triple
 * @par MC/DC:
 * Decision: `if (backend->read_block == NULL || backend->write_block == NULL ||
 *               backend->get_capacity == NULL)` (3 conditions,
 * libs/ra8_fs/src/ra8_fs_fat.c line 870). N+1 = 4 vectors for N=3. @brief Exercise the mcdc mount backend fn triple filesystem operation. @details Runs the mcdc mount backend fn triple vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_mount_backend_fn_triple(void)
{
  TEST_BEGIN("ra8_fs MC/DC: mount backend fn-ptr triple");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  ra8_fs_backend_t bad2 = s_backend;
  bad2.read_block       = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_mount(&bad2, &h));
  ra8_fs_backend_t bad3 = s_backend;
  bad3.write_block      = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_mount(&bad3, &h));
  ra8_fs_backend_t bad4 = s_backend;
  bad4.get_capacity     = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_mount(&bad4, &h));
  internal_free_volume();
  TEST_END("ra8_fs MC/DC: mount backend fn-ptr triple");
}

/**
 * @test test_mcdc_bpb_signature_pair
 * @par MC/DC:
 * Decision: `if (priv_scratch[lo] != 0x55 || priv_scratch[hi] != 0xAA)` (2 conditions,
 * libs/ra8_fs/src/ra8_fs_fat.c line 816, function `priv_parse_bpb_into_mount`).
 * N+1 = 3 vectors for N=2. @brief Exercise the mcdc bpb signature pair filesystem operation. @details Runs the mcdc bpb signature pair vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_bpb_signature_pair(void)
{
  TEST_BEGIN("ra8_fs MC/DC: bpb signature (lo||hi) bad");
  ra8_fs_mount_t* h = nullptr;
  internal_build_fat16_volume();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  internal_build_fat16_volume();
  s_disk.bytes[k_bpb_off_sig_lo] = 0x00U;
  s_disk.bytes[k_bpb_off_sig_hi] = k_bpb_sig_hi;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_fs_mount(&s_backend, &h));
  internal_free_volume();
  internal_build_fat16_volume();
  s_disk.bytes[k_bpb_off_sig_lo] = k_bpb_sig_lo;
  s_disk.bytes[k_bpb_off_sig_hi] = 0x00U;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_fs_mount(&s_backend, &h));
  internal_free_volume();
  TEST_END("ra8_fs MC/DC: bpb signature (lo||hi) bad");
}

/**
 * @test test_mcdc_bpb_geometry_triple
 * @par MC/DC:
 * Decision: `if (m->bytes_per_sector != 512 || m->sectors_per_cluster == 0U ||
 *               m->num_fats == 0U)` (3 conditions, libs/ra8_fs/src/ra8_fs_fat.c line 825).
 * N+1 = 4 vectors for N=3. @brief Exercise the mcdc bpb geometry triple filesystem operation. @details Runs the mcdc bpb geometry triple vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_bpb_geometry_triple(void)
{
  TEST_BEGIN("ra8_fs MC/DC: bpb geometry triple");
  ra8_fs_mount_t* h = nullptr;
  internal_build_fat16_volume();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  internal_build_fat16_volume();
  internal_put16(s_disk.bytes, k_bpb_off_bytes_per_sec, k_fat_bad_bytes_per_sec);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_fs_mount(&s_backend, &h));
  internal_free_volume();
  internal_build_fat16_volume();
  s_disk.bytes[k_bpb_off_sec_per_clus] = 0U;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_fs_mount(&s_backend, &h));
  internal_free_volume();
  internal_build_fat16_volume();
  s_disk.bytes[16] = 0U;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_fs_mount(&s_backend, &h));
  internal_free_volume();
  TEST_END("ra8_fs MC/DC: bpb geometry triple");
}

/**
 * @test test_mcdc_open_args_triple
 * @par MC/DC:
 * Decision: `if (handle == NULL || path == NULL || out_file == NULL)` (3 conditions,
 * libs/ra8_fs/src/ra8_fs_fat.c line 1042, function `ra8_fs_open`). N+1 = 4 vectors. @brief Exercise the mcdc open args triple filesystem operation. @details Runs the mcdc open args triple vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_open_args_triple(void)
{
  TEST_BEGIN("ra8_fs MC/DC: open arg triple NULL");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_open(nullptr, "B.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_open(h, nullptr, k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_open(h, "C.TXT", k_ra8_fs_mode_write, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs MC/DC: open arg triple NULL");
}

/**
 * @test test_mcdc_read_args_triple
 * @par MC/DC:
 * Decision: `if (file == NULL || buf == NULL || got_len == NULL)` (3 conditions,
 * libs/ra8_fs/src/ra8_fs_fat.c line 1140). N+1 = 4 vectors for N=3. @brief Exercise the mcdc read args triple filesystem operation. @details Runs the mcdc read args triple vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_read_args_triple(void)
{
  TEST_BEGIN("ra8_fs MC/DC: read arg triple NULL");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "R.TXT", k_ra8_fs_mode_write, &f));
  uint8_t payload = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, &payload, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "R.TXT", k_ra8_fs_mode_read, &f));
  uint8_t  buf[16] = {};
  uint32_t got     = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_read(nullptr, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_read(f, nullptr, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_read(f, buf, sizeof(buf), nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs MC/DC: read arg triple NULL");
}

/**
 * @test test_mcdc_read_eof_or_zero_pair
 * @par MC/DC:
 * Decision: `if (file->offset >= file->size_bytes || max_len == 0U)` (2 conditions,
 * libs/ra8_fs/src/ra8_fs_fat.c line 1147). N+1 = 3 vectors for N=2. @brief Exercise the mcdc read eof or zero pair filesystem operation. @details Runs the mcdc read eof or zero pair vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_read_eof_or_zero_pair(void)
{
  TEST_BEGIN("ra8_fs MC/DC: read (offset>=size || max_len==0)");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "E.TXT", k_ra8_fs_mode_write, &f));
  uint8_t payload[4] = {1U, 2U, 3U, 4U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, payload, sizeof(payload)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "E.TXT", k_ra8_fs_mode_read, &f));
  uint8_t  buf[8] = {};
  uint32_t got    = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(4, got);
  got = k_fat_poison_out;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(0, got);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_seek(f, 0U));
  got = k_fat_poison_out;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, buf, 0U, &got));
  TEST_ASSERT_EQ(0, got);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs MC/DC: read (offset>=size || max_len==0)");
}

/**
 * @test test_mcdc_write_args_pair
 * @par MC/DC:
 * Decision: `if (file == NULL || buf == NULL)` (2 conditions,
 * libs/ra8_fs/src/ra8_fs_fat.c line 1278). N+1 = 3 vectors for N=2. @brief Exercise the mcdc write args pair filesystem operation. @details Runs the mcdc write args pair vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_write_args_pair(void)
{
  TEST_BEGIN("ra8_fs MC/DC: write (file||buf) NULL pair");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "W.TXT", k_ra8_fs_mode_write, &f));
  uint8_t payload = (uint8_t)'Y';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, &payload, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_write(nullptr, &payload, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_write(f, nullptr, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs MC/DC: write (file||buf) NULL pair");
}

/**
 * @test test_mcdc_write_state_pair
 * @par MC/DC:
 * Decision: `if (file->in_use == 0U || file->mode == k_ra8_fs_mode_read)` (2 conditions,
 * libs/ra8_fs/src/ra8_fs_fat.c line 1281). N+1 = 3 vectors for N=2. @brief Exercise the mcdc write state pair filesystem operation. @details Runs the mcdc write state pair vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_write_state_pair(void)
{
  TEST_BEGIN("ra8_fs MC/DC: write (!in_use || mode==read)");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "S.TXT", k_ra8_fs_mode_write, &f));
  uint8_t payload = (uint8_t)'Z';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, &payload, 1U));
  ra8_fs_file_t closed = *f;
  closed.in_use        = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_write(&closed, &payload, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "S.TXT", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_write(f, &payload, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs MC/DC: write (!in_use || mode==read)");
}

/**
 * @test test_mcdc_tell_args_pair
 * @par MC/DC:
 * Decision: `if (file == NULL || out_offset == NULL)` (2 conditions,
 * libs/ra8_fs/src/ra8_fs_fat.c line 1319). N+1 = 3 vectors for N=2. @brief Exercise the mcdc tell args pair filesystem operation. @details Runs the mcdc tell args pair vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_tell_args_pair(void)
{
  TEST_BEGIN("ra8_fs MC/DC: tell (file||out) NULL pair");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "T.TXT", k_ra8_fs_mode_write, &f));
  uint64_t pos = k_fat_poison_out;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_tell(f, &pos));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_tell(nullptr, &pos));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_tell(f, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs MC/DC: tell (file||out) NULL pair");
}

/**
 * @test test_mcdc_size_args_pair
 * @par MC/DC:
 * Decision: `if (file == NULL || out_bytes == NULL)` (2 conditions,
 * libs/ra8_fs/src/ra8_fs_fat.c line 1331). N+1 = 3 vectors for N=2. @brief Exercise the mcdc size args pair filesystem operation. @details Runs the mcdc size args pair vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_size_args_pair(void)
{
  TEST_BEGIN("ra8_fs MC/DC: size (file||out) NULL pair");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "Z.TXT", k_ra8_fs_mode_write, &f));
  uint64_t sz = k_fat_poison_out;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(f, &sz));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_size(nullptr, &sz));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_size(f, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs MC/DC: size (file||out) NULL pair");
}

/** @brief Perform the mcdc listdir cb filesystem operation. @details Implements the bounded mcdc listdir cb fixture step using caller-owned state. @param[in] name Validated fixture path or name value. @param[in] attr Value required by this filesystem vector. @param[in] size Caller-supplied bounded extent or quantity. @param[in,out] ctx Caller-owned fixture or filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. */
RA8_INTERNAL static void
internal_mcdc_listdir_cb(const char* name, uint8_t attr, uint64_t size, void* ctx)
{
  (void)name;
  (void)attr;
  (void)size;
  if (ctx != nullptr) {
    (*(uint32_t*)ctx)++;
  }
}

/**
 * @test test_mcdc_listdir_args_triple
 * @par MC/DC:
 * Decision: `if (handle == NULL || cb == NULL || path == NULL)` (3 conditions,
 * libs/ra8_fs/src/ra8_fs_fat.c line 1374). N+1 = 4 vectors for N=3. @brief Exercise the mcdc listdir args triple filesystem operation. @details Runs the mcdc listdir args triple vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_listdir_args_triple(void)
{
  TEST_BEGIN("ra8_fs MC/DC: listdir arg triple NULL");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  uint32_t count = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", internal_mcdc_listdir_cb, &count));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_fs_listdir(nullptr, "/", internal_mcdc_listdir_cb, &count));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_listdir(h, "/", nullptr, &count));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_listdir(h, nullptr, internal_mcdc_listdir_cb, &count));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs MC/DC: listdir arg triple NULL");
}

/**
 * @test test_mcdc_listdir_path_check
 * @par MC/DC:
 * The former root-only compound `if (path[0] != '/' || (path[0] == '/' &&
 * path[1] != '\0'))` was replaced by full nested path resolution
 * (`priv_resolve_dir`) for FAT volumes, so that compound decision no longer
 * exists. This vector now exercises the replacement behaviour: the root lists,
 * a missing subdirectory resolves to not_found, and a created subdirectory
 * lists successfully. No compound decision under test. @brief Exercise the mcdc listdir path check filesystem operation. @details Runs the mcdc listdir path check vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_listdir_path_check(void)
{
  TEST_BEGIN("ra8_fs MC/DC: listdir nested path resolution");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  uint32_t count = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", internal_mcdc_listdir_cb, &count));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_listdir(h, "/NOPE", internal_mcdc_listdir_cb, &count));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SUB"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/SUB", internal_mcdc_listdir_cb, &count));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs MC/DC: listdir nested path resolution");
}

/**
 * @test test_mcdc_unlink_args_pair
 * @par MC/DC:
 * Decision: `if (handle == NULL || path == NULL)` (2 conditions,
 * libs/ra8_fs/src/ra8_fs_fat.c line 1405). N+1 = 3 vectors for N=2. @brief Exercise the mcdc unlink args pair filesystem operation. @details Runs the mcdc unlink args pair vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_unlink_args_pair(void)
{
  TEST_BEGIN("ra8_fs MC/DC: unlink (handle||path) NULL pair");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "U.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "U.TXT"));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_unlink(nullptr, "U.TXT"));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_unlink(h, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs MC/DC: unlink (handle||path) NULL pair");
}

/**
 * @test test_mcdc_priv_to_upper_range
 * @par MC/DC:
 * Decision: `if (c >= 'a' && c <= 'z')` (2 conditions,
 * libs/ra8_fs/src/ra8_fs_fat.c line 478, function `priv_to_upper`). Reached
 * via `priv_path_to_83` from `ra8_fs_open`.
 * V1 "UPPER.TXT" c='U': C1=F -> F. V2 "lower.txt" c='l': C1=T,C2=T -> T (uppercased).
 * V3 "AB{.TXT" c='{': C1=T,C2=F -> F. N+1 = 3 vectors for N=2. @brief Exercise the mcdc priv to upper range filesystem operation. @details Runs the mcdc priv to upper range vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_priv_to_upper_range(void)
{
  TEST_BEGIN("ra8_fs MC/DC: priv_to_upper (c>='a' && c<='z')");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "UPPER.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "lower.txt", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "LOWER.TXT", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "AB{.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "AB{.TXT", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs MC/DC: priv_to_upper (c>='a' && c<='z')");
}

/**
 * @test test_mcdc_pack_base_terminator
 * @par MC/DC:
 * Decision: `while (*path != '\0' && *path != '.')` (2 conditions,
 * libs/ra8_fs/src/ra8_fs_fat.c line 491, function `priv_pack_base`). Reached
 * via `ra8_fs_open`.
 * V1 "ABC.TXT": exits on '.' -> C1=T,C2=F. V2 ".TXT": first char '.' rejected.
 * V3 "": first char NUL -> C1=F. N+1 = 3 vectors for N=2.
 *
 * V2 no longer FAILS the open. `.TXT` has an empty 8.3 base, so `priv_pack_base`
 * still rejects it -- which is the decision under test -- but since #600 that
 * verdict routes the name to a long-name chain instead of ending the call, and
 * the file is created under the alias `TXT~1`. The vector is unchanged; only
 * what the caller sees afterwards is. @brief Exercise the mcdc pack base terminator filesystem operation. @details Runs the mcdc pack base terminator vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_pack_base_terminator(void)
{
  TEST_BEGIN("ra8_fs MC/DC: pack_base (*p!=0 && *p!='.')");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "ABC.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, ".TXT", k_ra8_fs_mode_write, &f)); /* V2 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_open(h, "", k_ra8_fs_mode_write, &f)); /* V3 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs MC/DC: pack_base (*p!=0 && *p!='.')");
}

/**
 * @test test_mcdc_priv_path_to_83_args_pair
 * @par MC/DC:
 * Decision: `if (path == NULL || out11 == NULL)` (2 conditions,
 * libs/ra8_fs/src/ra8_fs_fat.c line 526, function `priv_path_to_83`). Reached
 * via `ra8_fs_open`. The `out11` argument is always non-NULL inside the public-
 * API call site (stack buffer), so independence of C2 is structurally
 * unreachable -- deactivated under DO-178C 6.4.4.3. C1 is observed indirectly
 * via the ra8_fs_open NULL-arg test above. @brief Exercise the mcdc priv path to 83 args pair filesystem operation. @details Runs the mcdc priv path to 83 args pair vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_priv_path_to_83_args_pair(void)
{
  TEST_BEGIN("ra8_fs MC/DC: priv_path_to_83 NULL pair (deactivated C2)");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "P.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs MC/DC: priv_path_to_83 NULL pair (deactivated C2)");
}

/**
 * @test test_mcdc_dir_find_free_marker_pair
 * @par MC/DC:
 * Decision: `if (ent[name]==marker_free_perm || ent[name]==marker_free_used)`
 * (2 conditions, libs/ra8_fs/src/ra8_fs_fat.c, `priv_dir_find_free`).
 * Reached via `ra8_fs_open` write-mode.
 * V1 fresh slot 0x00 -> C1=T. V2 unlinked slot 0xE5 -> C1=F,C2=T. V3 populated
 * slot -> C1=F,C2=F. N+1 = 3 vectors for N=2. @brief Exercise the mcdc dir find free marker pair filesystem operation. @details Runs the mcdc dir find free marker pair vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_dir_find_free_marker_pair(void)
{
  TEST_BEGIN("ra8_fs MC/DC: dir_find_free (perm||used) markers");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "FRESH.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "SECOND.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "FRESH.TXT"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "REUSED.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs MC/DC: dir_find_free (perm||used) markers");
}

/**
 * @test test_mcdc_free_chain_range_pair
 * @par MC/DC:
 * Decision: `while (cur >= k_cluster_first_data &&
 *                  (cur - k_cluster_first_data) < count_of_clusters)`
 * (2 conditions, libs/ra8_fs/src/ra8_fs_fat.c line 756, `priv_free_chain`).
 * The caller's outer guard at line 1423 ensures cur>=first_data on entry,
 * so C1=F is structurally unreachable -- deactivated under DO-178C 6.4.4.3.
 * Single- and multi-cluster file unlinks exercise C2. @brief Exercise the mcdc free chain range pair filesystem operation. @details Runs the mcdc free chain range pair vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_free_chain_range_pair(void)
{
  TEST_BEGIN("ra8_fs MC/DC: free_chain (cur>=first && cur-first<count)");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "ONE.BIN", k_ra8_fs_mode_write, &f));
  uint8_t one[16] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, one, sizeof(one)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "ONE.BIN"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "MULTI.BIN", k_ra8_fs_mode_write, &f));
  uint8_t big[k_fat_bytes_multi_cluster] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, big, sizeof(big)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "MULTI.BIN"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "EMPTY.BIN", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "EMPTY.BIN"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs MC/DC: free_chain (cur>=first && cur-first<count)");
}

/**
 * @test test_mcdc_83_to_str_kanji_triple
 * @par MC/DC:
 * Decision: `if (j > 0 && (uint8_t)out12[0] == 0xE5 && in11[0] == 0xE5)`
 * (3 conditions, libs/ra8_fs/src/ra8_fs_fat.c line 564, `priv_83_to_str`).
 * Reached via `ra8_fs_listdir`. C=(in11[0]==0xE5) is structurally unreachable
 * via the public API (8.3 names accepted are uppercase ASCII), so it is a
 * deactivated condition under DO-178C 6.4.4.3. A and B are observable.
 * V1 list dir w/ one entry -> A=T,B=F -> F. V2 list empty -> A=F -> F. @brief Exercise the mcdc 83 to str kanji triple filesystem operation. @details Runs the mcdc 83 to str kanji triple vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_83_to_str_kanji_triple(void)
{
  TEST_BEGIN("ra8_fs MC/DC: 83_to_str kanji-escape triple");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  uint32_t count = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", internal_mcdc_listdir_cb, &count));
  TEST_ASSERT_EQ(0, count);
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  count = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", internal_mcdc_listdir_cb, &count));
  TEST_ASSERT_EQ(1, count);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs MC/DC: 83_to_str kanji-escape triple");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom, exactly as before.
 */
static void (*const s_test_roster[])(void) = {
  internal_test_mcdc_mount_args_pair,
  internal_test_mcdc_mount_backend_fn_triple,
  internal_test_mcdc_bpb_signature_pair,
  internal_test_mcdc_bpb_geometry_triple,
  internal_test_mcdc_open_args_triple,
  internal_test_mcdc_read_args_triple,
  internal_test_mcdc_read_eof_or_zero_pair,
  internal_test_mcdc_write_args_pair,
  internal_test_mcdc_write_state_pair,
  internal_test_mcdc_tell_args_pair,
  internal_test_mcdc_size_args_pair,
  internal_test_mcdc_listdir_args_triple,
  internal_test_mcdc_listdir_path_check,
  internal_test_mcdc_unlink_args_pair,
  internal_test_mcdc_priv_to_upper_range,
  internal_test_mcdc_pack_base_terminator,
  internal_test_mcdc_priv_path_to_83_args_pair,
  internal_test_mcdc_dir_find_free_marker_pair,
  internal_test_mcdc_free_chain_range_pair,
  internal_test_mcdc_83_to_str_kanji_triple,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  return 0;
}
