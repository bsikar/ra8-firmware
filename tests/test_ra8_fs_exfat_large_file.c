/**
 * @file test_ra8_fs_exfat_large_file.c
 * @brief exFAT files past 4 GiB: write, read, seek, truncate across the line (#676).
 *
 * @details
 * The simulation evidence for the 64-bit length model, on a 6 GiB sparse fake
 * device (`tests/support/fs_sparse_backend_test_util.h`) that never allocates
 * 6 GiB of RAM -- only sectors written non-zero are stored, so the volume, the
 * grown file and the boundary I/O together cost a few hundred KiB.
 *
 * The campaign:
 *
 *   1. Format exFAT (our own formatter), create `BIG.BIN` with a 4 KiB
 *      pattern, then ::ra8_fs_truncate it UP to 4 GiB + 64 KiB. `DataLength`
 *      crosses 32 bits; `ValidDataLength` stays at the written prefix, so the
 *      grown span reads as zeros without a byte having been written for it.
 *   2. Prove the boundary offsets: seek/tell/read at 0xFFFFFFFF - 1,
 *      0xFFFFFFFF and 0x100000000 + 1.
 *   3. Make the whole length VALID the way a foreign implementation would
 *      (surgical `ValidDataLength = DataLength` + SetChecksum repair on the
 *      raw image), re-mount, and drive REAL data through the boundary: append
 *      past the end, and a 512-byte write spanning 0xFFFFFFFF, each read back
 *      byte-for-byte after a re-open.
 *   4. `ra8_fs_stat` and a fresh mount report the 64-bit size.
 *   5. Truncate DOWN through the line (to 4 GiB + 16, then to 4 KiB): the
 *      recorded length follows exactly and the original prefix bytes survive.
 *
 * The 512-byte-sector, sub-4-GiB behavior this file leans on is pinned by the
 * existing suite; everything here is the >4 GiB extension of it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_fs_meta.h"
#include "support/fs_sparse_backend_test_util.h"
#include "unity_minimal.h"

/**
 * @enum lf_geometry_t
 * @brief Device and file geometry for the >4 GiB campaign.
 */
typedef enum : uint64_t {
  k_lf_disk_blocks = 12582912U,    /**< 6 GiB at 512-byte sectors.            */
  k_lf_boundary    = 0x100000000U, /**< The 4 GiB line every offset crosses.  */
  k_lf_big_size    = 0x100010000U, /**< 4 GiB + 64 KiB: the grown DataLength. */
  k_lf_shrink_mid  = 0x100000010U, /**< Truncate target just past the line.   */
} lf_geometry_t;

/**
 * @enum lf_small_t
 * @brief Byte-sized constants for patterns, entries, and offsets.
 */
typedef enum : uint32_t {
  k_lf_bps            = 512U,    /**< Device sector size.                      */
  k_lf_prefix_len     = 4096U,   /**< Pattern written at creation.             */
  k_lf_span_len       = 512U,    /**< Boundary-crossing write length.          */
  k_lf_span_back      = 256U,    /**< Span starts this far below the line.     */
  k_lf_append_len     = 768U,    /**< Bytes appended past the grown end.       */
  k_lf_probe_len      = 8U,      /**< Small boundary read probe.               */
  k_lf_shrink_small   = 4096U,   /**< Final truncate-down target.              */
  k_lf_entry_bytes    = 32U,     /**< One exFAT directory entry.               */
  k_lf_file_index     = 3U,      /**< BIG.BIN File entry index in the root.    */
  k_lf_set_entries    = 3U,      /**< File + Stream + Name.                    */
  k_lf_csum_off       = 2U,      /**< SetChecksum offset in the File entry.    */
  k_lf_strm_valid_off = 8U,      /**< Stream ValidDataLength offset.           */
  k_lf_strm_dlen_off  = 24U,     /**< Stream DataLength offset.                */
  k_lf_seed_prefix    = 0x51U,   /**< Prefix pattern seed.                     */
  k_lf_seed_append    = 0xA7U,   /**< Append pattern seed.                     */
  k_lf_seed_span      = 0x3CU,   /**< Boundary-span pattern seed.              */
  k_lf_csum_hi_bit    = 0x8000U, /**< exFAT 16-bit rotate-add wrap bit.        */
  k_lf_first_cluster  = 2U,      /**< First data cluster number.               */
  k_lf_byte_mask      = 0xFFU,   /**< Low-byte mask for the pattern generator. */
  k_lf_probe_fill     = 0xEEU,   /**< Poison byte the probe must overwrite.    */
} lf_small_t;

/** @brief The 6 GiB sparse device (sectors materialise only when written). */
static sparse_disk_t s_sp;

/** @brief Fill @p buf with a position-dependent pattern from @p seed. @details Implements the bounded lf pattern fixture step using caller-owned state. @param[in,out] buf Caller-owned bounded byte storage. @param[in] len Value required by this filesystem vector. @param[in] seed Value required by this filesystem vector. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_lf_pattern(uint8_t* buf, uint32_t len, uint8_t seed)
{
  for (uint32_t i = 0U; i < len; i++) {
    buf[i] = (uint8_t)(seed ^ (uint8_t)(i & (uint32_t)k_lf_byte_mask));
  }
}

/** @brief exFAT entry-set checksum (spec sec 6.3.3), skipping its own field. @details Implements the bounded lf set checksum fixture step using caller-owned state. @param[in] set Value required by this filesystem vector. @param[in] bytes Caller-supplied bounded extent or quantity. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static uint16_t internal_lf_set_checksum(const uint8_t* set, uint32_t bytes)
{
  uint16_t cs = 0U;
  for (uint32_t i = 0U; i < bytes; i++) {
    if ((i == (uint32_t)k_lf_csum_off) || (i == ((uint32_t)k_lf_csum_off + 1U))) {
      continue;
    }
    const uint16_t hi = (((cs & 1U) != 0U) ? (uint16_t)k_lf_csum_hi_bit : (uint16_t)0U);
    cs                = (uint16_t)(hi + (uint16_t)(cs >> 1U) + (uint16_t)set[i]);
  }
  return cs;
}

/** @brief Absolute LBA of the root-directory cluster's first sector. @details Implements the bounded lf root lba fixture step using caller-owned state. @param[in] m Value required by this filesystem vector. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static uint64_t internal_lf_root_lba(const ra8_fs_mount_t* m)
{
  return m->partition_base_lba + m->first_data_lba +
         ((uint64_t)(m->root_cluster - (uint32_t)k_lf_first_cluster) * m->sectors_per_cluster);
}

/**
 * @brief Make `ValidDataLength` equal `DataLength` on BIG.BIN's Stream entry.
 *
 * @details Image surgery standing in for a foreign implementation that wrote
 *          the whole file: the driver itself only ever raises the valid length
 *          by writing real bytes, which is exactly what a 4 GiB test must not
 *          do. The entry set sits at a known index (the first free run after
 *          the three system entries), and the SetChecksum is recomputed over
 *          the patched bytes so the volume stays self-consistent.
 *
 * @param[in] m Live mount (supplies the root-directory geometry). @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_lf_make_fully_valid(const ra8_fs_mount_t* m)
{
  const uint64_t lba                  = internal_lf_root_lba(m);
  uint8_t        sec[k_sp_sector_max] = {};
  internal_sp_peek(&s_sp, lba, sec);
  uint8_t* set  = &sec[(size_t)k_lf_file_index * (size_t)k_lf_entry_bytes];
  uint8_t* strm = &set[k_lf_entry_bytes];
  memcpy(&strm[k_lf_strm_valid_off], &strm[k_lf_strm_dlen_off], sizeof(uint64_t));
  const uint16_t cs =
    internal_lf_set_checksum(set, (uint32_t)k_lf_set_entries * (uint32_t)k_lf_entry_bytes);
  set[k_lf_csum_off]      = (uint8_t)(cs & (uint16_t)k_lf_byte_mask);
  set[k_lf_csum_off + 1U] = (uint8_t)(cs >> 8U);
  internal_sp_poke(&s_sp, lba, sec);
}

/**
 * @test test_large_grow_and_boundary_reads
 * @brief Grow past 4 GiB by truncate; probe the boundary offsets read-side.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- it drives the 64-bit
 * seek/tell/size plumbing and the `ValidDataLength < offset` zero-serving read
 * span across the 32-bit line end to end) @details Runs the large grow and boundary reads vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_large_grow_and_boundary_reads(void)
{
  TEST_BEGIN("exfat >4GiB: truncate-grow crosses the line; boundary reads serve zeros");
  internal_sp_init(&s_sp, (uint64_t)k_lf_disk_blocks, (uint32_t)k_lf_bps);
  const ra8_fs_backend_t be   = internal_sp_backend(&s_sp);
  ra8_fs_format_opts_t   opts = {};
  opts.type                   = k_ra8_fs_type_exfat;
  opts.label                  = "BIGVOL";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&be, &opts));

  ra8_fs_mount_t* m = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&be, &m));
  TEST_ASSERT_EQ(k_ra8_fs_type_exfat, m->type);

  static uint8_t prefix[k_lf_prefix_len];
  internal_lf_pattern(prefix, (uint32_t)k_lf_prefix_len, (uint8_t)k_lf_seed_prefix);
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(m, "BIG.BIN", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, prefix, (uint32_t)k_lf_prefix_len));
  /* The grow: DataLength crosses 32 bits without 4 GiB of writes. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_truncate(f, (uint64_t)k_lf_big_size));
  uint64_t size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(f, &size));
  TEST_ASSERT_EQ(k_lf_big_size, size);

  /* Boundary probes: 0xFFFFFFFF - 1 and the first bytes past the line. All of
   * it lies beyond ValidDataLength (the 4 KiB prefix), so the read path's
   * zero-serving arm must answer -- at 64-bit offsets the old 32-bit model
   * could not even express. The clusters underneath were genuinely allocated
   * by the grow; test 2 reads THROUGH them once the length is made valid. */
  uint8_t probe[k_lf_probe_len] = {};
  uint8_t zero[k_lf_probe_len]  = {};
  for (uint64_t at = (uint64_t)k_lf_boundary - 2U; at <= (uint64_t)k_lf_boundary + 1U; at++) {
    uint32_t got = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_seek(f, at));
    uint64_t pos = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_tell(f, &pos));
    TEST_ASSERT_EQ(at, pos);
    memset(probe, (int)k_lf_probe_fill, sizeof(probe));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, probe, (uint32_t)k_lf_probe_len, &got));
    TEST_ASSERT_EQ(k_lf_probe_len, got);
    TEST_ASSERT_EQ(0, memcmp(probe, zero, sizeof(probe)));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_tell(f, &pos));
    TEST_ASSERT_EQ(at + (uint64_t)k_lf_probe_len, pos);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(m));
  TEST_END("exfat >4GiB: truncate-grow crosses the line; boundary reads serve zeros");
}

/**
 * @brief Write real bytes past the line: an append and a boundary-spanning write.
 *
 * @param[in] m Live mount holding BIG.BIN, its whole length valid.
 *
 * @pre BIG.BIN's `ValidDataLength` equals its `DataLength` (no gap fill runs).
 * @pre The mount is in use.
 * @post The file is `k_lf_big_size + k_lf_append_len` bytes.
 * @post Pattern bytes sit at the boundary span and the appended tail. @details Implements the bounded lf write past line fixture step using caller-owned state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_lf_write_past_line(ra8_fs_mount_t* m)
{
  static uint8_t append[k_lf_append_len];
  static uint8_t span[k_lf_span_len];
  internal_lf_pattern(append, (uint32_t)k_lf_append_len, (uint8_t)k_lf_seed_append);
  internal_lf_pattern(span, (uint32_t)k_lf_span_len, (uint8_t)k_lf_seed_span);
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(m, "BIG.BIN", k_ra8_fs_mode_append, &f));
  uint64_t pos = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_tell(f, &pos));
  TEST_ASSERT_EQ(k_lf_big_size, pos);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, append, (uint32_t)k_lf_append_len));
  /* A write SPANNING the 4 GiB line itself. */
  const uint64_t span_at = (uint64_t)k_lf_boundary - (uint64_t)k_lf_span_back;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_seek(f, span_at));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, span, (uint32_t)k_lf_span_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
}

/**
 * @brief Read the appended tail and the boundary span back, byte for byte.
 *
 * @param[in] m Live mount holding BIG.BIN after ::lf_write_past_line.
 *
 * @pre The append and span writes committed.
 * @pre The mount is in use.
 * @post The 64-bit size and both patterns verified through a fresh handle.
 * @post The handle is closed again. @details Implements the bounded lf read back past line fixture step using caller-owned state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_lf_read_back_past_line(ra8_fs_mount_t* m)
{
  static uint8_t expect[k_lf_append_len];
  static uint8_t back[k_lf_append_len];
  uint32_t       got = 0U;
  ra8_fs_file_t* f   = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(m, "BIG.BIN", k_ra8_fs_mode_read, &f));
  uint64_t size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(f, &size));
  TEST_ASSERT_EQ(k_lf_big_size + (uint64_t)k_lf_append_len, size);
  internal_lf_pattern(expect, (uint32_t)k_lf_append_len, (uint8_t)k_lf_seed_append);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_seek(f, (uint64_t)k_lf_big_size));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, back, (uint32_t)k_lf_append_len, &got));
  TEST_ASSERT_EQ(k_lf_append_len, got);
  TEST_ASSERT_EQ(0, memcmp(back, expect, (size_t)k_lf_append_len));
  internal_lf_pattern(expect, (uint32_t)k_lf_span_len, (uint8_t)k_lf_seed_span);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_seek(f, (uint64_t)k_lf_boundary - (uint64_t)k_lf_span_back));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, back, (uint32_t)k_lf_span_len, &got));
  TEST_ASSERT_EQ(k_lf_span_len, got);
  TEST_ASSERT_EQ(0, memcmp(back, expect, (size_t)k_lf_span_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
}

/**
 * @test test_large_written_data_across_boundary
 * @brief Real bytes at and past 4 GiB: append, boundary-spanning write, stat.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- it pins the 64-bit
 * offset-to-cluster-to-LBA mapping with content that must round-trip, and the
 * 64-bit `DataLength` / `ValidDataLength` flush + re-parse) @details Runs the large written data across boundary vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_large_written_data_across_boundary(void)
{
  TEST_BEGIN("exfat >4GiB: appended and boundary-spanning bytes round-trip");
  const ra8_fs_backend_t be = internal_sp_backend(&s_sp);
  ra8_fs_mount_t*        m  = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&be, &m));
  /* Make the whole grown length valid, as a foreign writer would leave it. */
  internal_lf_make_fully_valid(m);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(m));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&be, &m));

  /* The 64-bit size survives a fresh parse, through stat and open alike. */
  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(m, "BIG.BIN", &st));
  TEST_ASSERT_EQ(k_lf_big_size, st.size_bytes);

  internal_lf_write_past_line(m);
  internal_lf_read_back_past_line(m);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(m));
  TEST_END("exfat >4GiB: appended and boundary-spanning bytes round-trip");
}

/**
 * @test test_large_truncate_down_through_line
 * @brief Truncate from >4 GiB to just past the line, then far below it.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- it drives the 64-bit shrink
 * paths, the cluster release of a >4 GiB allocation, and the survival of the
 * original prefix bytes) @details Runs the large truncate down through line vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_large_truncate_down_through_line(void)
{
  TEST_BEGIN("exfat >4GiB: truncate down through the line keeps the prefix");
  const ra8_fs_backend_t be = internal_sp_backend(&s_sp);
  ra8_fs_mount_t*        m  = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&be, &m));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(m, "BIG.BIN", k_ra8_fs_mode_append, &f));
  uint64_t size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_truncate(f, (uint64_t)k_lf_shrink_mid));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(f, &size));
  TEST_ASSERT_EQ(k_lf_shrink_mid, size);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_truncate(f, (uint64_t)k_lf_shrink_small));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(f, &size));
  TEST_ASSERT_EQ(k_lf_shrink_small, size);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* The original creation-time prefix is intact below the shrink. */
  static uint8_t prefix[k_lf_prefix_len];
  static uint8_t back[k_lf_prefix_len];
  internal_lf_pattern(prefix, (uint32_t)k_lf_prefix_len, (uint8_t)k_lf_seed_prefix);
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(m, "BIG.BIN", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, back, (uint32_t)k_lf_prefix_len, &got));
  TEST_ASSERT_EQ(k_lf_prefix_len, got);
  TEST_ASSERT_EQ(0, memcmp(back, prefix, (size_t)k_lf_prefix_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* The sparse store stayed inside its budget the whole campaign. */
  TEST_ASSERT(s_sp.stored < (uint32_t)k_sp_slots);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(m));
  TEST_END("exfat >4GiB: truncate down through the line keeps the prefix");
}

/** @brief Implementation of `main()` -- the >4 GiB exFAT campaign, in order. */
int32_t main(void)
{
  internal_test_large_grow_and_boundary_reads();
  internal_test_large_written_data_across_boundary();
  internal_test_large_truncate_down_through_line();
  return 0;
}
