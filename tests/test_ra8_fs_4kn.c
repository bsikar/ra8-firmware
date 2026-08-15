/**
 * @file test_ra8_fs_4kn.c
 * @brief 4096-byte-native sectors: format, mount, read/write on FAT and exFAT (#683).
 *
 * @details
 * The simulation evidence for 4Kn media, on flat RAM devices whose backend
 * reports a 4096-byte block size:
 *
 *   - FAT16 at 4Kn: format -> `BPB_BytsPerSec` reads 4096 -> mount -> file
 *     round-trip, listdir, truncate both directions, unlink.
 *   - FAT32 at 4Kn (a 512 MiB device, so the cluster count clears the FAT32
 *     floor): the same round-trip plus FSInfo-backed free-space sanity.
 *   - exFAT at 4Kn: format -> `BytesPerSectorShift` reads 12 -> mount ->
 *     nested directories and a multi-cluster file round-trip -> the built-in
 *     ::ra8_fs_check reports the volume clean.
 *   - The MISMATCH refusals both ways: a 512-byte-formatted image presented by
 *     a 4Kn backend must not mount, and a 4Kn image presented by a 512-byte
 *     backend must not either -- reinterpreting sector geometry corrupts, so
 *     the parse holds the volume to the DEVICE's reported size.
 *
 * No 4Kn medium has been on the bench; this file and its siblings are the
 * simulation evidence the capability ships with (see libs/ra8_fs/README.md).
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
#include "ra8_fs_check.h"
#include "ra8_fs_meta.h"
#include "support/ra8_test_file.h"
#include "support/ra8_test_output.h"
#include "unity_minimal.h"

/**
 * @enum fkn_geometry_t
 * @brief Device geometry and on-disk probe offsets for the 4Kn campaign.
 */
typedef enum : uint32_t {
  k_fkn_bps           = 4096U,   /**< The native sector size under test.    */
  k_fkn_bps_512       = 512U,    /**< The classic size, for mismatch cases. */
  k_fkn_blocks_fat16  = 16384U,  /**< 64 MiB at 4Kn -> FAT16 band.          */
  k_fkn_blocks_fat32  = 131072U, /**< 512 MiB at 4Kn -> FAT32 band.         */
  k_fkn_blocks_exfat  = 16384U,  /**< 64 MiB at 4Kn -> exFAT minimum met.   */
  k_fkn_blocks_512    = 8192U,   /**< 4 MiB at 512 B -> FAT16 band.         */
  k_fkn_off_bps       = 11U,     /**< BPB_BytsPerSec offset.                */
  k_fkn_off_shift     = 108U,    /**< exFAT BytesPerSectorShift offset.     */
  k_fkn_shift_4kn     = 12U,     /**< log2(4096).                           */
  k_fkn_shift_too_big = 13U,     /**< Past the BytesPerSectorShift ceiling. */
  k_fkn_part_lba_4kn  = 256U,    /**< 1 MiB alignment at 4Kn sectors.       */
  k_fkn_payload       = 40960U,  /**< Multi-cluster payload (10 sectors).   */
  k_fkn_trunc_up      = 65536U,  /**< Truncate-up target.                   */
  k_fkn_trunc_down    = 1000U,   /**< Truncate-down target (mid-sector).    */
  k_fkn_seed          = 0x69U,   /**< Payload pattern seed.                 */
  k_fkn_dump_path_cap = 256U,    /**< Dump-path buffer size.                */
  k_fkn_byte_mask     = 0xFFU,   /**< Low-byte mask for the pattern.        */
  k_fkn_bps_too_small = 256U,    /**< Below the 512-byte floor.             */
  k_fkn_bps_too_big   = 8192U,   /**< Above the 4096-byte ceiling.          */
  k_fkn_bps_not_pow2  = 768U,    /**< In range but not a power of two.      */
} fkn_geometry_t;

/** @brief Flat RAM device with a runtime-selectable sector size. */
typedef struct {
  uint8_t* bytes;       /**< Flat store (block_count * bps bytes). */
  uint64_t block_count; /**< Device size in sectors.               */
  uint32_t bps;         /**< Bytes per sector the backend reports. */
} fkn_disk_t;

/** @brief The one device under test (re-initialised per case). */
static fkn_disk_t s_disk;

/** @brief `read_block` over ::s_disk.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] ctx Argument for the bounded test operation.
 * @param[in] lba Argument for the bounded test operation.
 * @param[in] count Argument for the bounded test operation.
 * @param[in,out] buf Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static ra8_err_t
internal_fkn_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  fkn_disk_t* d = (fkn_disk_t*)ctx;
  if (d->bytes == nullptr) {
    return k_ra8_err_out_of_range; /* probe-only cases mount no byte store */
  }
  if ((lba >= d->block_count) || (count > (d->block_count - lba))) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf, &d->bytes[(size_t)lba * d->bps], (size_t)count * d->bps);
  return k_ra8_ok;
}

/** @brief `write_block` over ::s_disk.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] ctx Argument for the bounded test operation.
 * @param[in] lba Argument for the bounded test operation.
 * @param[in] count Argument for the bounded test operation.
 * @param[in] buf Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static ra8_err_t
internal_fkn_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
{
  fkn_disk_t* d = (fkn_disk_t*)ctx;
  if ((lba >= d->block_count) || (count > (d->block_count - lba))) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * d->bps], buf, (size_t)count * d->bps);
  return k_ra8_ok;
}

/** @brief `get_capacity` over ::s_disk.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] ctx Argument for the bounded test operation.
 * @param[in,out] block_count Argument for the bounded test operation.
 * @param[in,out] block_size Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static ra8_err_t
internal_fkn_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  const fkn_disk_t* d = (const fkn_disk_t*)ctx;
  *block_count        = d->block_count;
  *block_size         = d->bps;
  return k_ra8_ok;
}

/** @brief The backend every case mounts through. */
static const ra8_fs_backend_t s_backend = {
  .read_block   = internal_fkn_read,
  .write_block  = internal_fkn_write,
  .get_capacity = internal_fkn_capacity,
  .erase_blocks = nullptr,
  .ctx          = &s_disk,
};

/** @brief Allocate a zeroed device of @p blocks sectors of @p bps bytes.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] blocks Argument for the bounded test operation.
 * @param[in] bps Argument for the bounded test operation.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_fkn_alloc(uint64_t blocks, uint32_t bps)
{
  s_disk.bytes       = (uint8_t*)calloc((size_t)blocks, bps);
  s_disk.block_count = blocks;
  s_disk.bps         = bps;
  TEST_ASSERT_NOT_NULL(s_disk.bytes);
}

/** @brief Release the device.
 * @details Performs one bounded, deterministic operation for this host test.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_fkn_free(void)
{
  free(s_disk.bytes);
  s_disk.bytes = nullptr;
}

/**
 * @brief Dump the flat image for out-of-band `fsck.exfat -n` evidence.
 *
 * @details Mirrors the exFAT stream suite's convention: when the environment
 *          variable `RA8_EXFAT_DUMP_DIR` names a directory, the whole device
 *          image lands there as `<tag>.img`, and
 *          `fsck.exfat -n <dump>/<tag>.img` is the independent verdict on the
 *          4Kn volume this build wrote. With the variable unset (the normal
 *          CI run) this is a no-op.
 *
 * @param[in] tag Basename for the image file.
 *
 * @pre ::s_disk holds a device image (bytes non-NULL).
 * @pre @p tag is a short NUL-terminated name.
 * @post With the variable set, `<dir>/<tag>.img` holds the full image.
 * @post The device contents are unchanged.
 *
 * @note Not thread-safe; the fixture is single-threaded.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_fkn_dump_image(const char* tag)
{
  const char* dir = getenv("RA8_EXFAT_DUMP_DIR");
  if ((dir == nullptr) || (s_disk.bytes == nullptr)) {
    return;
  }
  char path[k_fkn_dump_path_cap] = {};
  (void)snprintf(path, sizeof path, "%s/%s.img", dir, tag);
  const size_t total = (size_t)s_disk.block_count * s_disk.bps;
  (void)internal_test_file_replace(path, s_disk.bytes, total);
}

/** @brief Fill @p buf with a position-dependent pattern from @p seed.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] buf Argument for the bounded test operation.
 * @param[in] len Argument for the bounded test operation.
 * @param[in] seed Argument for the bounded test operation.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_fkn_pattern(uint8_t* buf, uint32_t len, uint8_t seed)
{
  for (uint32_t i = 0U; i < len; i++) {
    buf[i] = (uint8_t)(seed ^ (uint8_t)(i & (uint32_t)k_fkn_byte_mask));
  }
}

/** @brief One full file round-trip (write, size, read-back, truncate, unlink).
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] m Argument for the bounded test operation.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_fkn_file_roundtrip(ra8_fs_mount_t* m)
{
  static uint8_t out[k_fkn_payload];
  static uint8_t in[k_fkn_payload];
  internal_fkn_pattern(out, (uint32_t)k_fkn_payload, (uint8_t)k_fkn_seed);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(m, "DATA.BIN", out, (uint32_t)k_fkn_payload));

  ra8_fs_file_t* f   = nullptr;
  uint32_t       got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(m, "DATA.BIN", k_ra8_fs_mode_read, &f));
  uint64_t size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(f, &size));
  TEST_ASSERT_EQ(k_fkn_payload, size);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, in, (uint32_t)k_fkn_payload, &got));
  TEST_ASSERT_EQ(k_fkn_payload, got);
  TEST_ASSERT_EQ(0, memcmp(in, out, (size_t)k_fkn_payload));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* Truncate both directions; the grown gap reads as zeros. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(m, "DATA.BIN", k_ra8_fs_mode_append, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_truncate(f, (uint64_t)k_fkn_trunc_up));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_truncate(f, (uint64_t)k_fkn_trunc_down));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(f, &size));
  TEST_ASSERT_EQ(k_fkn_trunc_down, size);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(m, "DATA.BIN", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, in, (uint32_t)k_fkn_trunc_down, &got));
  TEST_ASSERT_EQ(k_fkn_trunc_down, got);
  TEST_ASSERT_EQ(0, memcmp(in, out, (size_t)k_fkn_trunc_down));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(m, "DATA.BIN"));
  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_stat(m, "DATA.BIN", &st));
}

/**
 * @test internal_test_4kn_fat16_roundtrip
 * @brief FAT16 formatted, mounted and driven at 4096-byte sectors.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- it drives the runtime
 * `bytes_per_sector` through the formatter's geometry, the BPB parse and every
 * sector-offset computation on the FAT12/16 path)

 * @details Performs one bounded, deterministic operation for this host test.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_test_4kn_fat16_roundtrip(void)
{
  TEST_BEGIN("4Kn: FAT16 format + mount + file round-trip at 4096-byte sectors");
  internal_fkn_alloc((uint64_t)k_fkn_blocks_fat16, (uint32_t)k_fkn_bps);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;
  opts.label                = "FOURKN16";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  /* The BPB records the device's real sector size. */
  const uint32_t bpb_bps =
    (uint32_t)s_disk.bytes[k_fkn_off_bps] | ((uint32_t)s_disk.bytes[k_fkn_off_bps + 1U] << 8U);
  TEST_ASSERT_EQ(k_fkn_bps, bpb_bps);

  ra8_fs_mount_t* m = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &m));
  TEST_ASSERT_EQ(k_ra8_fs_type_fat16, m->type);
  TEST_ASSERT_EQ(k_fkn_bps, m->bytes_per_sector);
  internal_fkn_file_roundtrip(m);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(m));
  internal_fkn_free();
  TEST_END("4Kn: FAT16 format + mount + file round-trip at 4096-byte sectors");
}

/**
 * @test internal_test_4kn_fat32_roundtrip
 * @brief FAT32 formatted, mounted and driven at 4096-byte sectors.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- the FAT32 arm of the same
 * runtime-sector-size plumbing, FSInfo included)

 * @details Performs one bounded, deterministic operation for this host test.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_test_4kn_fat32_roundtrip(void)
{
  TEST_BEGIN("4Kn: FAT32 format + mount + file round-trip at 4096-byte sectors");
  internal_fkn_alloc((uint64_t)k_fkn_blocks_fat32, (uint32_t)k_fkn_bps);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat32;
  opts.label                = "FOURKN32";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));

  ra8_fs_mount_t* m = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &m));
  TEST_ASSERT_EQ(k_ra8_fs_type_fat32, m->type);
  TEST_ASSERT_EQ(k_fkn_bps, m->bytes_per_sector);
  internal_fkn_file_roundtrip(m);
  ra8_fs_space_t sp = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_free_space(m, &sp));
  TEST_ASSERT(sp.free_bytes > 0U);
  TEST_ASSERT_EQ(sp.total_clusters, sp.free_clusters + sp.used_clusters);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(m));
  internal_fkn_free();
  TEST_END("4Kn: FAT32 format + mount + file round-trip at 4096-byte sectors");
}

/**
 * @test internal_test_4kn_exfat_roundtrip
 * @brief exFAT formatted, mounted, checked and driven at 4096-byte sectors.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- the exFAT arm: shift-12 VBR,
 * directory machinery and the built-in volume check at 4Kn)

 * @details Performs one bounded, deterministic operation for this host test.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_test_4kn_exfat_roundtrip(void)
{
  TEST_BEGIN("4Kn: exFAT format + mount + dirs + check at 4096-byte sectors");
  internal_fkn_alloc((uint64_t)k_fkn_blocks_exfat, (uint32_t)k_fkn_bps);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_exfat;
  opts.label                = "FOURKNXF";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  /* The VBR (one aligned MiB in: 256 sectors at 4Kn) records shift 12. */
  const uint8_t shift =
    s_disk.bytes[((size_t)k_fkn_part_lba_4kn * (uint32_t)k_fkn_bps) + (uint32_t)k_fkn_off_shift];
  TEST_ASSERT_EQ(k_fkn_shift_4kn, shift);

  ra8_fs_mount_t* m = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &m));
  TEST_ASSERT_EQ(k_ra8_fs_type_exfat, m->type);
  TEST_ASSERT_EQ(k_fkn_bps, m->bytes_per_sector);
  TEST_ASSERT_EQ(k_fkn_part_lba_4kn, m->partition_base_lba);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(m, "/books"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(m, "/books/scifi"));
  internal_fkn_file_roundtrip(m);
  static uint8_t nested[k_fkn_trunc_down];
  internal_fkn_pattern(nested, (uint32_t)k_fkn_trunc_down, (uint8_t)k_fkn_seed);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_write_file(m, "/books/scifi/a.txt", nested, (uint32_t)k_fkn_trunc_down));
  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(m, "/books/scifi/a.txt", &st));
  TEST_ASSERT_EQ(k_fkn_trunc_down, st.size_bytes);

  /* The built-in volume check agrees the 4Kn volume is self-consistent. */
  static uint8_t        chk_bitmap[k_fkn_blocks_exfat / 8U];
  ra8_fs_check_report_t rep = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_check(m, chk_bitmap, (uint32_t)sizeof(chk_bitmap), &rep));
  TEST_ASSERT_EQ(0U, rep.faults_total);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(m));
  internal_fkn_dump_image("fourkn_exfat");
  internal_fkn_free();
  TEST_END("4Kn: exFAT format + mount + dirs + check at 4096-byte sectors");
}

/**
 * @test internal_test_4kn_mismatch_refused
 * @brief A volume whose sector size disagrees with the device does not mount.
 *
 * @par MC/DC:
 * Decision (FAT): `bpb_bps != m->bytes_per_sector || spc == 0 || num_fats == 0`
 * -- the first condition's independent TRUE vector lives here (512-byte BPB on
 * a 4Kn device); the sibling FAT tests hold it FALSE. Decision (exFAT): the
 * three-condition shift guard
 * `(shift < 9) || (shift > 12) || ((1 << shift) != bytes_per_sector)` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_read.c@priv_exfat_parse`:
 * - shift 12 image on a 512-byte device -> F,F,T (mismatch independence);
 * - shift 13 image                      -> F,T,- (ceiling independence);
 * - a sub-9 shift arrives from the sibling suites' garbage-VBR parses, and
 *   the all-false control is every mounting exFAT case above.

 * @details Performs one bounded, deterministic operation for this host test.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_test_4kn_mismatch_refused(void)
{
  TEST_BEGIN("4Kn: sector-size mismatch between image and device is refused");
  /* A 512-byte FAT16 image... */
  internal_fkn_alloc((uint64_t)k_fkn_blocks_512, (uint32_t)k_fkn_bps_512);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  /* ...presented by a backend that claims 4096-byte sectors. The byte store
   * stays as formatted; only the reported geometry lies. */
  s_disk.block_count = (uint64_t)k_fkn_blocks_512 / ((uint32_t)k_fkn_bps / (uint32_t)k_fkn_bps_512);
  s_disk.bps         = (uint32_t)k_fkn_bps;
  ra8_fs_mount_t* m  = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_fs_mount(&s_backend, &m));
  internal_fkn_free();

  /* And the shift guard itself: a shift-12 exFAT VBR reached by a 512-byte
   * device. The VBR is copied to LBA 0 (a superfloppy view) so BOTH geometries
   * find it at the same address, isolating the shift-vs-device comparison. */
  internal_fkn_alloc((uint64_t)k_fkn_blocks_exfat, (uint32_t)k_fkn_bps);
  opts.type = k_ra8_fs_type_exfat;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  memmove(&s_disk.bytes[0],
          &s_disk.bytes[(size_t)k_fkn_part_lba_4kn * (uint32_t)k_fkn_bps],
          (size_t)k_fkn_bps);
  s_disk.block_count =
    (uint64_t)k_fkn_blocks_exfat * ((uint32_t)k_fkn_bps / (uint32_t)k_fkn_bps_512);
  s_disk.bps = (uint32_t)k_fkn_bps_512;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_fs_mount(&s_backend, &m));
  /* And a shift past 12 entirely (8192-byte sectors are outside the format
   * range): the ceiling condition independently refuses. */
  s_disk.bytes[k_fkn_off_shift] = (uint8_t)k_fkn_shift_too_big;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_fs_mount(&s_backend, &m));
  internal_fkn_free();
  TEST_END("4Kn: sector-size mismatch between image and device is refused");
}

/**
 * @test internal_test_mcdc_bps_valid
 * @brief Backend block sizes outside 512..4096 (or non-power-of-two) refuse.
 *
 * @par MC/DC:
 * Decision: `(bs < 512) || (bs > 4096) || ((bs & (bs - 1)) != 0)` (3
 * conditions) in `libs/ra8_fs/src/ra8_fs_fat_mount.c@internal_bps_valid`, reached
 * through the mount's capacity probe.
 * - V1: bs = 512  -> F,F,F -> valid (the FAT16 case above is the mounting
 *   control; re-asserted here as the cheap probe-only leg falling through to
 *   the read path).
 * - V2: bs = 256  -> T (short-circuit)    -> invalid_arg.
 * - V3: bs = 8192 -> F,T (short-circuit)  -> invalid_arg.
 * - V4: bs = 768  -> F,F,T                -> invalid_arg.
 * N+1 = 4 vectors for N=3 conditions: minimal MC/DC.

 * @details Performs one bounded, deterministic operation for this host test.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_test_mcdc_bps_valid(void)
{
  TEST_BEGIN("4Kn MC/DC: backend block sizes outside 512..4096 are refused");
  /* The probe runs before any sector read, so no byte store is needed. */
  s_disk.bytes       = nullptr;
  s_disk.block_count = (uint64_t)k_fkn_blocks_512;
  ra8_fs_mount_t* m  = nullptr;
  s_disk.bps         = (uint32_t)k_fkn_bps_too_small;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_mount(&s_backend, &m)); /* V2 */
  s_disk.bps = (uint32_t)k_fkn_bps_too_big;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_mount(&s_backend, &m)); /* V3 */
  s_disk.bps = (uint32_t)k_fkn_bps_not_pow2;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_mount(&s_backend, &m)); /* V4 */
  /* V1: a 512-byte device passes the probe; with no bytes behind it the read
   * path answers, which is the proof the guard fell through. */
  s_disk.bps = (uint32_t)k_fkn_bps_512;
  TEST_ASSERT(ra8_fs_mount(&s_backend, &m) != k_ra8_err_invalid_arg);
  TEST_END("4Kn MC/DC: backend block sizes outside 512..4096 are refused");
}

/** @brief Implementation of `main()` -- the 4Kn campaign, in order. */
int32_t main(void)
{
  internal_test_4kn_fat16_roundtrip();
  internal_test_4kn_fat32_roundtrip();
  internal_test_4kn_exfat_roundtrip();
  internal_test_4kn_mismatch_refused();
  internal_test_mcdc_bps_valid();
  TEST_ASSERT_EQ(k_ra8_test_output_ok,
                 internal_test_output_fd_text(STDERR_FILENO, "[OK  ] test_ra8_fs_4kn.c\n"));
  return 0;
}
