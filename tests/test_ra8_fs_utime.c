/**
 * @file test_ra8_fs_utime.c
 * @brief Per-entry timestamp set (`ra8_fs_utime()`, #682) on FAT and exFAT.
 *
 * @details
 * Creates a file, stamps chosen create / modify / access times onto it, and
 * decodes the ON-DISK timestamp fields back out of the image: the packed
 * date/time words must equal what the caller asked for, and must be a LEGAL
 * calendar value (month and day non-zero) -- the property #601 exists to keep,
 * now for a caller-chosen instant instead of the clock's. A NULL argument must
 * leave its field untouched. On exFAT the entry-set SetChecksum is recomputed
 * over the image and compared, proving the patch did not corrupt the set.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_attributes.h"
#include "ra8_fs_meta.h"
#include "test_ra8_fs_format_fixture.h"

/**
 * @enum utime_off_t
 * @brief On-disk timestamp field offsets (FAT dir entry + exFAT File entry).
 */
typedef enum : uint32_t {
  k_ut_entry_size = 32U,     /**< Directory entry size.                */
  k_ut_entries_ps = 16U,     /**< Dir entries per 512-byte sector.     */
  k_ut_dir_attr   = 11U,     /**< FAT DIR_Attr.                        */
  k_ut_attr_lfn   = 0x0FU,   /**< Long-name marker.                    */
  k_ut_attr_volid = 0x08U,   /**< ATTR_VOLUME_ID.                      */
  k_ut_attr_dir   = 0x10U,   /**< ATTR_DIRECTORY.                      */
  k_ut_dir_crtd   = 16U,     /**< FAT DIR_CrtDate.                     */
  k_ut_dir_crtt   = 14U,     /**< FAT DIR_CrtTime.                     */
  k_ut_dir_wrtd   = 24U,     /**< FAT DIR_WrtDate.                     */
  k_ut_dir_wrtt   = 22U,     /**< FAT DIR_WrtTime.                     */
  k_ut_dir_accd   = 18U,     /**< FAT DIR_LstAccDate.                  */
  k_ut_dir_free   = 0x00U,   /**< End-of-directory marker.             */
  k_ut_dir_del    = 0xE5U,   /**< Deleted-entry marker.                */
  k_ut_dir_dot    = 0x2EU,   /**< "." / ".." entry.                    */
  k_ut_first_clus = 2U,      /**< First data cluster.                  */
  k_ut_xf_file    = 0x85U,   /**< exFAT File entry type.               */
  k_ut_xf_secnt   = 1U,      /**< exFAT File entry SecondaryCount off. */
  k_ut_xf_csum    = 2U,      /**< exFAT File entry SetChecksum off.    */
  k_ut_xf_ctime   = 8U,      /**< exFAT CreateTimestamp off.           */
  k_ut_xf_mtime   = 12U,     /**< exFAT LastModifiedTimestamp off.     */
  k_ut_xf_atime   = 16U,     /**< exFAT LastAccessedTimestamp off.     */
  k_ut_epoch_year = 1980U,   /**< FAT/exFAT epoch year.                */
  k_ut_date_yshf  = 9U,      /**< Date: year field shift.              */
  k_ut_date_mshf  = 5U,      /**< Date: month field shift.             */
  k_ut_time_hshf  = 11U,     /**< Time: hour field shift.              */
  k_ut_time_mshf  = 5U,      /**< Time: minute field shift.            */
  k_ut_xf_dshf    = 16U,     /**< exFAT stamp: date half shift.        */
  k_ut_shl_b1     = 8U,      /**< LE byte-1 shift.                     */
  k_ut_shl_b2     = 16U,     /**< LE byte-2 shift.                     */
  k_ut_shl_b3     = 24U,     /**< LE byte-3 shift.                     */
  k_ut_csum_wrap  = 0x8000U, /**< exFAT SetChecksum rotate wrap bit.   */
} utime_off_t;

/** @brief Pack a FAT date word the way the driver does. @details Implements the bounded fat date fixture step using caller-owned state. @param[in] y Value required by this filesystem vector. @param[in] mo Value required by this filesystem vector. @param[in] d Value required by this filesystem vector. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static uint16_t internal_fat_date(uint32_t y, uint32_t mo, uint32_t d)
{
  return (uint16_t)(((y - (uint32_t)k_ut_epoch_year) << (uint32_t)k_ut_date_yshf) |
                    (mo << (uint32_t)k_ut_date_mshf) | d);
}

/** @brief Pack a FAT time word the way the driver does. @details Implements the bounded fat time fixture step using caller-owned state. @param[in] h Value required by this filesystem vector. @param[in] mi Value required by this filesystem vector. @param[in] s Value required by this filesystem vector. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static uint16_t internal_fat_time(uint32_t h, uint32_t mi, uint32_t s)
{
  return (uint16_t)((h << (uint32_t)k_ut_time_hshf) | (mi << (uint32_t)k_ut_time_mshf) | (s / 2U));
}

/** @brief Perform the img rd16 filesystem operation. @details Implements the bounded img rd16 fixture step using caller-owned state. @param[in] off Value required by this filesystem vector. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. */
RA8_INTERNAL static uint16_t internal_img_rd16(uint32_t off)
{
  return (uint16_t)((uint16_t)s_disk.bytes[off] |
                    (uint16_t)((uint16_t)s_disk.bytes[off + 1U] << (uint16_t)k_ut_shl_b1));
}

/** @brief Perform the img rd32 filesystem operation. @details Implements the bounded img rd32 fixture step using caller-owned state. @param[in] off Value required by this filesystem vector. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. */
RA8_INTERNAL static uint32_t internal_img_rd32(uint32_t off)
{
  return (uint32_t)s_disk.bytes[off] | ((uint32_t)s_disk.bytes[off + 1U] << (uint32_t)k_ut_shl_b1) |
         ((uint32_t)s_disk.bytes[off + 2U] << (uint32_t)k_ut_shl_b2) |
         ((uint32_t)s_disk.bytes[off + 3U] << (uint32_t)k_ut_shl_b3);
}

/** @brief Byte offset of the first regular FAT file entry in the fixed root. @details Implements the bounded fat file off fixture step using caller-owned state. @param[in] h Value required by this filesystem vector. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static uint32_t internal_fat_file_off(const ra8_fs_mount_t* h)
{
  const uint32_t base = (h->partition_base_lba + h->first_root_lba) * (uint32_t)k_fmt_block_size;
  for (uint32_t e = 0U; e < h->root_entries; e++) {
    const uint32_t off   = base + (e * (uint32_t)k_ut_entry_size);
    const uint8_t  name0 = s_disk.bytes[off];
    const uint8_t  attr  = s_disk.bytes[off + (uint32_t)k_ut_dir_attr];
    if (name0 == (uint8_t)k_ut_dir_free) {
      break;
    }
    if ((name0 == (uint8_t)k_ut_dir_del) || (name0 == (uint8_t)k_ut_dir_dot)) {
      continue;
    }
    if (attr == (uint8_t)k_ut_attr_lfn) {
      continue;
    }
    if ((attr & ((uint8_t)k_ut_attr_volid | (uint8_t)k_ut_attr_dir)) != 0U) {
      continue;
    }
    return off;
  }
  TEST_FAIL_FMT("%s", "no FAT file entry found");
  return 0U;
}

/** @brief Byte offset of the exFAT File entry (0x85) in the root cluster. @details Implements the bounded exfat file off fixture step using caller-owned state. @param[in] h Value required by this filesystem vector. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static uint32_t internal_exfat_file_off(const ra8_fs_mount_t* h)
{
  const uint32_t root =
    (h->partition_base_lba + h->first_data_lba +
     ((uint64_t)(h->root_cluster - (uint32_t)k_ut_first_clus) * h->sectors_per_cluster)) *
    (uint32_t)k_fmt_block_size;
  const uint32_t per =
    (h->sectors_per_cluster * (uint32_t)k_fmt_block_size) / (uint32_t)k_ut_entry_size;
  for (uint32_t e = 0U; e < per; e++) {
    const uint32_t off = root + (e * (uint32_t)k_ut_entry_size);
    if (s_disk.bytes[off] == (uint8_t)k_ut_xf_file) {
      return off;
    }
  }
  TEST_FAIL_FMT("%s", "no exFAT File entry found");
  return 0U;
}

/** @brief exFAT SetChecksum over @p count entries at @p off (skips File csum). @details Implements the bounded exfat img setcsum fixture step using caller-owned state. @param[in] off Value required by this filesystem vector. @param[in] count Caller-supplied bounded extent or quantity. @return Status, selected object, or bounded value produced by the named operation. @retval 0 The computed result is empty or zero. @retval nonzero A bounded result was produced. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static uint16_t internal_exfat_img_setcsum(uint32_t off, uint32_t count)
{
  uint16_t cs = 0U;
  for (uint32_t i = 0U; i < (count * (uint32_t)k_ut_entry_size); i++) {
    if ((i == (uint32_t)k_ut_xf_csum) || (i == ((uint32_t)k_ut_xf_csum + 1U))) {
      continue;
    }
    cs = (uint16_t)(((cs & 1U) != 0U ? (uint32_t)k_ut_csum_wrap : 0U) + (cs >> 1) +
                    (uint16_t)s_disk.bytes[off + i]);
  }
  return cs;
}

/* Three distinct, in-range instants (seconds even so second/2 is exact). */
static const ra8_fs_datetime_t s_create =
  {.year = 2020U, .month = 1U, .day = 2U, .hour = 3U, .minute = 4U, .second = 6U};
static const ra8_fs_datetime_t s_modify =
  {.year = 2021U, .month = 6U, .day = 7U, .hour = 8U, .minute = 9U, .second = 10U};
static const ra8_fs_datetime_t s_access =
  {.year = 2022U, .month = 11U, .day = 12U, .hour = 13U, .minute = 14U, .second = 16U};

RA8_INTERNAL static ra8_fs_mount_t*
internal_fmt_mount_file(uint32_t blocks, ra8_fs_type_t type, const char* name)
{
  internal_alloc_garbage_card(blocks);
  ra8_fs_format_opts_t opts = {};
  opts.type                 = type;
  opts.label                = "UTIME";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  static const uint8_t body[4] = {'d', 'a', 't', 'a'};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, name, body, (uint32_t)sizeof(body)));
  return h;
}

/**
 * @test test_utime_fat16
 * @par MC/DC:
 * (no compound decision unique to this case -- FAT create/modify/access stamps
 * are written from caller readings and decoded back off the disk, then a
 * modify-only utime proves the create field is left untouched) @brief Exercise the utime fat16 filesystem operation. @details Runs the utime fat16 vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_utime_fat16(void)
{
  TEST_BEGIN("ra8_fs utime: FAT16 create/modify/access on-disk decode");
  ra8_fs_mount_t* h =
    internal_fmt_mount_file((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, "LOG.BIN");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_utime(h, "LOG.BIN", &s_create, &s_modify, &s_access));

  uint32_t off = internal_fat_file_off(h);
  TEST_ASSERT_EQ(internal_fat_date(2020U, 1U, 2U),
                 internal_img_rd16(off + (uint32_t)k_ut_dir_crtd));
  TEST_ASSERT_EQ(internal_fat_time(3U, 4U, 6U), internal_img_rd16(off + (uint32_t)k_ut_dir_crtt));
  TEST_ASSERT_EQ(internal_fat_date(2021U, 6U, 7U),
                 internal_img_rd16(off + (uint32_t)k_ut_dir_wrtd));
  TEST_ASSERT_EQ(internal_fat_time(8U, 9U, 10U), internal_img_rd16(off + (uint32_t)k_ut_dir_wrtt));
  TEST_ASSERT_EQ(internal_fat_date(2022U, 11U, 12U),
                 internal_img_rd16(off + (uint32_t)k_ut_dir_accd));

  /* modify-only utime: create and access must not move. */
  const ra8_fs_datetime_t m2 = {.year = 2019U, .month = 3U, .day = 4U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_utime(h, "LOG.BIN", nullptr, &m2, nullptr));
  off = internal_fat_file_off(h);
  TEST_ASSERT_EQ(internal_fat_date(2020U, 1U, 2U),
                 internal_img_rd16(off + (uint32_t)k_ut_dir_crtd)); /* unchanged */
  TEST_ASSERT_EQ(internal_fat_date(2019U, 3U, 4U),
                 internal_img_rd16(off + (uint32_t)k_ut_dir_wrtd)); /* moved     */
  TEST_ASSERT_EQ(internal_fat_date(2022U, 11U, 12U),
                 internal_img_rd16(off + (uint32_t)k_ut_dir_accd)); /* unchanged */

  /* all-NULL utime is a no-op success. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_utime(h, "LOG.BIN", nullptr, nullptr, nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs utime: FAT16 create/modify/access on-disk decode");
}

/**
 * @test test_utime_exfat
 * @par MC/DC:
 * (no compound decision unique to this case -- exFAT create/modify/access
 * stamps decoded off the disk, with the entry-set SetChecksum recomputed and
 * verified so the patch is proven non-corrupting) @brief Exercise the utime exfat filesystem operation. @details Runs the utime exfat vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_utime_exfat(void)
{
  TEST_BEGIN("ra8_fs utime: exFAT create/modify/access + checksum integrity");
  ra8_fs_mount_t* h =
    internal_fmt_mount_file((uint32_t)k_fmt_blocks_exfat, k_ra8_fs_type_exfat, "REC.BIN");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_utime(h, "REC.BIN", &s_create, &s_modify, &s_access));

  const uint32_t off   = internal_exfat_file_off(h);
  const uint32_t count = 1U + (uint32_t)s_disk.bytes[off + (uint32_t)k_ut_xf_secnt];
  const uint32_t cdate = (uint32_t)internal_fat_date(2020U, 1U, 2U);
  const uint32_t ctime = (uint32_t)internal_fat_time(3U, 4U, 6U);
  TEST_ASSERT_EQ((cdate << (uint32_t)k_ut_xf_dshf) | ctime,
                 internal_img_rd32(off + (uint32_t)k_ut_xf_ctime));
  const uint32_t mdate = (uint32_t)internal_fat_date(2021U, 6U, 7U);
  const uint32_t mtime = (uint32_t)internal_fat_time(8U, 9U, 10U);
  TEST_ASSERT_EQ((mdate << (uint32_t)k_ut_xf_dshf) | mtime,
                 internal_img_rd32(off + (uint32_t)k_ut_xf_mtime));
  const uint32_t adate = (uint32_t)internal_fat_date(2022U, 11U, 12U);
  const uint32_t atime = (uint32_t)internal_fat_time(13U, 14U, 16U);
  TEST_ASSERT_EQ((adate << (uint32_t)k_ut_xf_dshf) | atime,
                 internal_img_rd32(off + (uint32_t)k_ut_xf_atime));

  /* The stored SetChecksum matches a fresh recompute -> the set is intact. */
  TEST_ASSERT_EQ(internal_exfat_img_setcsum(off, count),
                 internal_img_rd16(off + (uint32_t)k_ut_xf_csum));

  /* And the volume still mounts and resolves the file (nothing corrupted). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  ra8_fs_mount_t* h2 = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h2));
  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h2, "REC.BIN", &st));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h2));
  internal_free_volume();
  TEST_END("ra8_fs utime: exFAT create/modify/access + checksum integrity");
}

/**
 * @test test_utime_clamp
 * @par MC/DC:
 * (no compound decision unique to this case -- an out-of-range reading is
 * clamped to a legal date rather than rejected) @brief Exercise the utime clamp filesystem operation. @details Runs the utime clamp vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_utime_clamp(void)
{
  TEST_BEGIN("ra8_fs utime: out-of-range reading is clamped, never rejected");
  ra8_fs_mount_t* h =
    internal_fmt_mount_file((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, "C.BIN");
  const ra8_fs_datetime_t bad = {.year = 1900U, .month = 13U, .day = 40U, .hour = 30U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_utime(h, "C.BIN", &bad, nullptr, nullptr));
  const uint32_t off  = internal_fat_file_off(h);
  const uint16_t date = internal_img_rd16(off + (uint32_t)k_ut_dir_crtd);
  const uint32_t mo   = (uint32_t)((date >> (uint32_t)k_ut_date_mshf) & 0x0FU);
  const uint32_t day  = (uint32_t)(date & 0x1FU);
  TEST_ASSERT(mo >= 1U);
  TEST_ASSERT(mo <= 12U);
  TEST_ASSERT(day >= 1U);
  TEST_ASSERT(day <= 31U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs utime: out-of-range reading is clamped, never rejected");
}

/**
 * @test test_utime_errors
 * @par MC/DC:
 * Decision: `if (handle == nullptr || path == nullptr)` (2 conditions) in
 * `libs/ra8_fs/src/ra8_fs_fat_utime.c@priv_utime_locked`.
 * - V1 handle=valid, path=valid -> F (control; the stamp runs).
 * - V2 handle=NULL,  path=valid -> C1=T -> T (varies handle only).
 * - V3 handle=valid, path=NULL  -> C1=F, C2=T -> T (varies path only).
 * Also covers the root path (invalid_arg), a missing name (not_found), and an
 * unmounted handle (invalid_state). @brief Exercise the utime errors filesystem operation. @details Runs the utime errors vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_utime_errors(void)
{
  TEST_BEGIN("ra8_fs utime MC/DC: (handle||path) NULL pair + root/missing/state");
  ra8_fs_mount_t* h =
    internal_fmt_mount_file((uint32_t)k_fmt_blocks_fat16, k_ra8_fs_type_fat16, "E.BIN");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_utime(h, "E.BIN", &s_create, nullptr, nullptr)); /* V1 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_utime(nullptr, "E.BIN", &s_create, nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_fs_utime(h, nullptr, &s_create, nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_utime(h, "/", &s_create, nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_utime(h, "NOPE.BIN", &s_create, nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_utime(h, "E.BIN", &s_create, nullptr, nullptr));
  internal_free_volume();
  TEST_END("ra8_fs utime MC/DC: (handle||path) NULL pair + root/missing/state");
}

int32_t main(void)
{
  internal_test_utime_fat16();
  internal_test_utime_exfat();
  internal_test_utime_clamp();
  internal_test_utime_errors();
  return 0;
}
