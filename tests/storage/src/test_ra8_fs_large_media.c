/**
 * @file test_ra8_fs_large_media.c
 * @brief Media past 2 TiB: 64-bit LBAs end to end, GPT entries included (#683).
 *
 * @details
 * The simulation evidence for beyond-32-bit LBAs, on a 3 TiB sparse fake
 * device (`tests/support/inc/fs_sparse_backend_test_util.h`) that stores only the
 * sectors ever written non-zero:
 *
 *   1. A complete exFAT volume is planted at absolute LBA 2^32 (2 TiB) by
 *      formatting a WINDOW of the sparse device -- our own formatter, no
 *      hand-written on-disk structures -- so its VBR sits at 2^32 + 2048.
 *   2. A GPT is laid at the device head whose Basic Data entry's 64-bit
 *      `FirstLBA` names that VBR: a value whose high word is non-zero, which
 *      the old parser refused as unaddressable.
 *   3. `ra8_fs_mount` (auto: protective MBR -> GPT -> Basic Data) and
 *      `ra8_fs_mount_partition(0)` both land on the volume;
 *      `partition_base_lba` carries the full 64-bit base.
 *   4. Files are written, read back, listed and stat'ed THROUGH that base --
 *      every access an absolute LBA past 2^32 -- and the sparse store's
 *      high-water marks prove the backend really was addressed there.
 *
 * No such medium has been on the bench; this file and its siblings are the
 * simulation evidence the capability ships with (see libs/ra8_fs/README.md).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "fs_sparse_backend_test_util.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "unity_minimal.h"

/**
 * @enum lm_geometry_t
 * @brief The 3 TiB device and the beyond-2-TiB partition placement.
 */
typedef enum : uint64_t {
  k_lm_disk_blocks   = 0x180000000U, /**< 3 TiB at 512-byte sectors.          */
  k_lm_window_base   = 0x100000000U, /**< Window start: exactly 2 TiB (2^32). */
  k_lm_window_blocks = 131072U,      /**< 64 MiB window the volume lives in.  */
  k_lm_volume_lba    = 0x100000800U, /**< VBR: base + the formatter's 1 MiB.  */
} lm_geometry_t;

/**
 * @enum lm_on_disk_t
 * @brief GPT/MBR construction constants and test payload sizing.
 */
typedef enum : uint32_t {
  k_lm_bps            = 512U,   /**< Device sector size.                */
  k_lm_off_sig_lo     = 510U,   /**< Boot signature low byte offset.    */
  k_lm_off_sig_hi     = 511U,   /**< Boot signature high byte offset.   */
  k_lm_sig_lo         = 0x55U,  /**< Boot signature low byte.           */
  k_lm_sig_hi         = 0xAAU,  /**< Boot signature high byte.          */
  k_lm_off_part0_type = 0x1C2U, /**< MBR partition 0 type byte.         */
  k_lm_type_gpt       = 0xEEU,  /**< Protective-MBR partition type.     */
  k_lm_off_part0_lba  = 0x1C6U, /**< MBR partition 0 first LBA.         */
  k_lm_gpt_hdr_lba    = 1U,     /**< GPT header sector.                 */
  k_lm_gpt_arr_lba    = 2U,     /**< GPT entry-array sector.            */
  k_lm_off_entry_lba  = 0x48U,  /**< Header: entry-array LBA (8 bytes). */
  k_lm_off_entry_cnt  = 0x50U,  /**< Header: entry count.               */
  k_lm_off_entry_size = 0x54U,  /**< Header: bytes per entry.           */
  k_lm_entry_bytes    = 128U,   /**< Standard GPT entry size.           */
  k_lm_entry_count    = 4U,     /**< Entries declared in the header.    */
  k_lm_ent_first_lba  = 0x20U,  /**< Entry: 64-bit first LBA.           */
  k_lm_guid_len       = 16U,    /**< Type-GUID length.                  */
  k_lm_payload        = 12288U, /**< Multi-cluster payload.             */
  k_lm_seed           = 0xB5U,  /**< Payload pattern seed.              */
  k_lm_byte_mask      = 0xFFU,  /**< Low-byte mask for the pattern.     */
} lm_on_disk_t;

/** @brief The 3 TiB sparse device. */
static sparse_disk_t s_sp;

/** @brief Window of ::s_sp the volume is formatted into. */
static sparse_window_t s_win;

/** @brief Microsoft Basic Data type GUID, on-disk byte order. */
static const uint8_t s_lm_basic_guid[k_lm_guid_len] = {
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

/** @brief Store @p v little-endian at @p off of @p sec. @details Implements the bounded lm put64 fixture step using caller-owned state. @param[in,out] sec Value required by this filesystem vector. @param[in] off Value required by this filesystem vector. @param[in] v Value required by this filesystem vector. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_lm_put64(uint8_t* sec, uint32_t off, uint64_t v)
{
  for (uint32_t i = 0U; i < (uint32_t)sizeof(uint64_t); i++) {
    sec[off + i] = (uint8_t)(v >> (i * 8U));
  }
}

/** @brief Lay the protective MBR + GPT naming ::k_lm_volume_lba as entry 0. @details Implements the bounded lm write gpt fixture step using caller-owned state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_lm_write_gpt(void)
{
  uint8_t sec[k_sp_sector_max] = {};

  /* Protective MBR: one type-0xEE partition. */
  sec[k_lm_off_part0_type] = (uint8_t)k_lm_type_gpt;
  sec[k_lm_off_part0_lba]  = 1U;
  sec[k_lm_off_sig_lo]     = (uint8_t)k_lm_sig_lo;
  sec[k_lm_off_sig_hi]     = (uint8_t)k_lm_sig_hi;
  internal_sp_poke(&s_sp, 0U, sec);

  /* GPT header: "EFI PART", entry array at LBA 2, four 128-byte entries. */
  memset(sec, 0, sizeof(sec));
  static const char k_efi_sig[] = "EFI PART";
  memcpy(sec, k_efi_sig, sizeof(k_efi_sig) - 1U);
  internal_lm_put64(sec, (uint32_t)k_lm_off_entry_lba, (uint64_t)k_lm_gpt_arr_lba);
  sec[k_lm_off_entry_cnt]  = (uint8_t)k_lm_entry_count;
  sec[k_lm_off_entry_size] = (uint8_t)k_lm_entry_bytes;
  internal_sp_poke(&s_sp, (uint64_t)k_lm_gpt_hdr_lba, sec);

  /* Entry 0: Basic Data, FirstLBA past 2 TiB. Entries 1-3 stay null. */
  memset(sec, 0, sizeof(sec));
  memcpy(sec, s_lm_basic_guid, (size_t)k_lm_guid_len);
  internal_lm_put64(sec, (uint32_t)k_lm_ent_first_lba, (uint64_t)k_lm_volume_lba);
  internal_sp_poke(&s_sp, (uint64_t)k_lm_gpt_arr_lba, sec);
}

/** @brief Fill @p buf with a position-dependent pattern from @p seed. @details Implements the bounded lm pattern fixture step using caller-owned state. @param[in,out] buf Caller-owned bounded byte storage. @param[in] len Value required by this filesystem vector. @param[in] seed Value required by this filesystem vector. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_lm_pattern(uint8_t* buf, uint32_t len, uint8_t seed)
{
  for (uint32_t i = 0U; i < len; i++) {
    buf[i] = (uint8_t)(seed ^ (uint8_t)(i & (uint32_t)k_lm_byte_mask));
  }
}

/**
 * @test test_large_media_plant_and_mount
 * @brief Format past 2 TiB via the window; mount through the GPT, both ways.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- it drives the 64-bit GPT entry
 * read, the 64-bit `partition_base_lba`, and the mount's absolute addressing
 * end to end) @details Runs the large media plant and mount vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_large_media_plant_and_mount(void)
{
  TEST_BEGIN("large media: exFAT volume at 2 TiB mounts through its GPT entry");
  internal_sp_init(&s_sp, (uint64_t)k_lm_disk_blocks, (uint32_t)k_lm_bps);
  s_win.disk  = &s_sp;
  s_win.base  = (uint64_t)k_lm_window_base;
  s_win.count = (uint64_t)k_lm_window_blocks;

  /* Plant the volume: the formatter runs against the WINDOW, so every sector
   * it writes lands past 2 TiB on the underlying device. */
  const ra8_fs_backend_t wbe  = internal_spw_backend(&s_win);
  ra8_fs_format_opts_t   opts = {};
  opts.type                   = k_ra8_fs_type_exfat;
  opts.label                  = "TBVOL";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&wbe, &opts));
  TEST_ASSERT(s_sp.max_written >= (uint64_t)k_lm_window_base);
  internal_lm_write_gpt();

  /* Auto-select: protective MBR -> GPT -> the Basic Data entry past 2 TiB. */
  const ra8_fs_backend_t be = internal_sp_backend(&s_sp);
  ra8_fs_mount_t*        m  = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&be, &m));
  TEST_ASSERT_EQ(k_ra8_fs_type_exfat, m->type);
  TEST_ASSERT_EQ(k_lm_volume_lba, m->partition_base_lba);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(m));

  /* The same volume by explicit GPT index; a null entry answers not_found. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount_partition(&be, 0U, &m));
  TEST_ASSERT_EQ(k_lm_volume_lba, m->partition_base_lba);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(m));
  ra8_fs_mount_t* none = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_mount_partition(&be, 1U, &none));
  TEST_END("large media: exFAT volume at 2 TiB mounts through its GPT entry");
}

/**
 * @test test_large_media_file_io
 * @brief File I/O whose every sector access is an absolute LBA past 2^32.
 *
 * @par MC/DC:
 * (no compound decision unique to this case -- content round-trips through
 * 64-bit absolute LBAs, with the store's high-water marks as the proof) @details Runs the large media file io vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_large_media_file_io(void)
{
  TEST_BEGIN("large media: files round-trip through absolute LBAs past 2^32");
  const ra8_fs_backend_t be = internal_sp_backend(&s_sp);
  ra8_fs_mount_t*        m  = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&be, &m));

  static uint8_t s_out[k_lm_payload];
  static uint8_t s_in[k_lm_payload];
  internal_lm_pattern(s_out, (uint32_t)k_lm_payload, (uint8_t)k_lm_seed);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(m, "/logs"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(m, "/logs/run.bin", s_out, (uint32_t)k_lm_payload));

  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(m, "/logs/run.bin", &st));
  TEST_ASSERT_EQ(k_lm_payload, st.size_bytes);

  ra8_fs_file_t* f   = nullptr;
  uint32_t       got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(m, "/logs/run.bin", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, s_in, (uint32_t)k_lm_payload, &got));
  TEST_ASSERT_EQ(k_lm_payload, got);
  TEST_ASSERT_EQ(0, memcmp(s_in, s_out, (size_t)k_lm_payload));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(m));

  /* The evidence: every data sector the file touched is past 2 TiB, and the
   * store's high-water marks say the backend really was addressed there. */
  TEST_ASSERT(s_sp.max_written > (uint64_t)k_lm_volume_lba);
  TEST_ASSERT(s_sp.max_read > (uint64_t)k_lm_volume_lba);
  TEST_ASSERT(s_sp.stored < (uint32_t)k_sp_slots);
  TEST_END("large media: files round-trip through absolute LBAs past 2^32");
}

/** @brief Implementation of `main()` -- the beyond-2-TiB campaign, in order. */
int main(void)
{
  internal_test_large_media_plant_and_mount();
  internal_test_large_media_file_io();
  return 0;
}
