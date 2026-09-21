/**
 * @file test_ra8_fs_unicode_exfat_cov.c
 * @brief The out-of-band `fsck.exfat` image family for the exFAT unicode work (#606).
 *
 * @details
 * Split from `test_ra8_fs_unicode_exfat.c` to keep both under the 1000-line
 * cap. That suite proves the on-disk name and `NameHash` against values it
 * computes itself; this one builds the two images that were fed to a real
 * `fsck.exfat`, so the argument does not rest on this codebase alone.
 *
 * @par Out-of-band `fsck.exfat` evidence:
 * `internal_unicode_dump_image()` from the shared fixture writes the PARTITION out under
 * `RA8_EXFAT_DUMP_DIR` (not the whole RAM disk -- `ra8_fs_format()` lays exFAT
 * inside an MBR partition, and a checker handed the disk reads the MBR and says
 * so). This file used to carry a private dumper on a private variable; it now
 * uses the shared one, so one directory collects the evidence of every exFAT
 * unicode suite instead of three conventions collecting three piles:
 * @code
 *   RA8_EXFAT_DUMP_DIR=/tmp/xuni ./test_ra8_fs_unicode_exfat_cov
 *   /usr/sbin/fsck.exfat -n -v /tmp/xuni/unicode_files_three.img  # three names
 *   /usr/sbin/fsck.exfat -n -v /tmp/xuni/unicode_files_badhash.img # PRE-#606
 * @endcode
 * Confirmed 2026-08-10 on the Linux verification host, exfatprogs 1.2.0 (and
 * on 2026-08-04, byte for byte, under the private variable this file used to
 * carry -- moving the hook changed where the images land, not what is in them):
 * @verbatim
 * three   -> unicode_files_three.img: clean. directories 1, files 3
 * badhash -> ERROR: /: the name hash of a file is wrong at 0x15c60.
 *            unicode_files_badhash.img: corrupted. directories 1, files 2
 * @endverbatim
 * The second line is the whole argument in one sentence, from a checker that
 * has never seen this codebase: the hash the PRE-#606 code stored for
 * `Caf<U+00E9>.txt` is one `fsck.exfat` calls wrong. The control repairs the
 * `SetChecksum` around the edit, so what is reported is the defect and not an
 * artefact of a clumsy patch -- and without it a clean report on the first
 * image would only prove `fsck.exfat` ran.
 *
 * @par Pure-ASCII sources:
 * Every non-ASCII name is a `static const char[]` of byte escapes, and every
 * expected unit array a `uint16_t[]`. No non-ASCII literal appears here.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fs_fat_exfat_mutate_test_util.h"
#include "fs_unicode_exfat_test_util.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_internal.h"
#include "unity_minimal.h"

/**
 * @brief The NameHash `priv_exfat_name_hash()` produced BEFORE #606.
 *
 * @details The old body, reproduced: fold the caller's UTF-8 BYTES with an
 *          ASCII-only up-case, one byte plus a zero byte each. For a pure-ASCII
 *          name that happens to equal the correct answer, which is exactly why
 *          the defect survived so long; for anything else it does not.
 *
 * @param[in] path Caller's name, NUL-terminated UTF-8.
 *
 * @return The hash the pre-#606 code would have stored.
 * @retval 0..0xFFFF The folded value.
 *
 * @pre @p path is non-NULL.
 * @pre The caller is building a negative control, not a real volume.
 * @post No state is modified.
 * @post The result depends only on @p path.
 *
 * @note Not thread-safe (no shared state, but the harness's counter is).
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_legacy_name_hash(const char* path)
{
  uint32_t h = 0U;
  for (uint32_t i = 0U; path[i] != '\0'; i++) {
    uint32_t c = (uint32_t)(unsigned char)path[i];
    if ((c >= (uint32_t)'a') && (c <= (uint32_t)'z')) {
      c -= ((uint32_t)'a' - (uint32_t)'A');
    }
    h = ((((h & 1U) != 0U) ? (uint32_t)k_ux_csum_hi_bit : 0U) + (h >> 1U) + c) &
        (uint32_t)k_ux_csum_mask;
    h =
      ((((h & 1U) != 0U) ? (uint32_t)k_ux_csum_hi_bit : 0U) + (h >> 1U)) & (uint32_t)k_ux_csum_mask;
  }
  return h;
}

/**
 * @brief Recompute a 3-entry set's SetChecksum straight from the specification.
 *
 * @details Rotate-add over every byte of the set except the two the checksum
 *          itself occupies. Written here rather than called into, because the
 *          point of using it is to leave a volume in which ONLY the NameHash is
 *          wrong -- if the checksum were also wrong, a checker would stop at
 *          that and never look at the hash.
 *
 * @param[in] file_off Byte offset of the set's File entry in the RAM disk.
 *
 * @return The checksum the File entry should carry.
 * @retval 0..0xFFFF The folded value.
 *
 * @pre `s_disk.bytes` is allocated and holds a File + Stream + Name set there.
 * @pre The caller has already made whatever edit it is repairing around.
 * @post No state is modified by this function.
 * @post The result covers exactly the 96 bytes of the three entries.
 *
 * @note Not thread-safe (reads the fixture singleton).
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_set_checksum_of(uint32_t file_off)
{
  uint32_t cs = 0U;
  for (uint32_t i = 0U; i < ((uint32_t)k_mut_entry_bytes * 3U); i++) {
    if ((i == (uint32_t)k_ux_file_off_csum) || (i == ((uint32_t)k_ux_file_off_csum + 1U))) {
      continue;
    }
    const uint32_t b = (uint32_t)s_disk.bytes[file_off + i];
    cs               = ((((cs & 1U) != 0U) ? (uint32_t)k_ux_csum_hi_bit : 0U) + (cs >> 1U) + b) &
                       (uint32_t)k_ux_csum_mask;
  }
  return cs;
}

/* ===========================================================================
 * Tests
 * ===========================================================================
 */

/**
 * @test test_exfat_dump_images_for_fsck
 * @brief Build the images the out-of-band `fsck.exfat` evidence is taken from.
 *
 * @details Two states, because a clean report on its own only proves the
 *          checker ran: a volume holding three non-ASCII names written through
 *          the public API, and a control in which one Stream entry's `NameHash`
 *          is corrupted -- exactly the disagreement the ASCII-only up-casing
 *          used to produce for every non-ASCII name.
 *
 * @par MC/DC:
 * No decision this file does not already cover; this case exists for the
 * artefacts.
 *
 * @pre exfatprogs is available on the host for the out-of-band step (optional).
 * @pre `RA8_EXFAT_DUMP_DIR` is set for the dumps to appear.
 * @post The clean image is `fsck.exfat`-clean; the control is not.
 *
 * @since 0.1.0 @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_exfat_dump_images_for_fsck(void)
{
  TEST_BEGIN("exfat unicode: images for the out-of-band fsck.exfat run");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  const uint8_t payload = (uint8_t)'x';
  const char*   names[] = {s_acc_u8, s_cjk_u8, s_emoji_u8};
  for (uint32_t i = 0U; i < (uint32_t)(sizeof(names) / sizeof(names[0])); i++) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, names[i], &payload, 1U));
  }
  const uint32_t base = h->partition_base_lba;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_unicode_dump_image("unicode_files_three", base);

  /* The control, and it is the specific one worth having: put back the hash the
   * PRE-#606 code stored for this name -- an ASCII-only fold over UTF-8 bytes --
   * and repair the SetChecksum around it, so the set is well-formed in every
   * other respect. What a real checker then reports is the defect itself, not
   * an artefact of a clumsy edit. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t file = internal_root_byte(h, (uint32_t)k_mut_root_file0_idx);
  const uint32_t strm = internal_root_byte(h, (uint32_t)k_mut_root_strm0_idx);
  const uint32_t old  = internal_legacy_name_hash(s_acc_u8);
  /* It has to actually differ, or the control proves nothing. */
  TEST_ASSERT(old != internal_disk_rd16(strm + (uint32_t)k_ux_strm_off_hsh));
  s_disk.bytes[strm + (uint32_t)k_ux_strm_off_hsh] = (uint8_t)(old & (uint32_t)k_mut_mask_byte);
  s_disk.bytes[strm + (uint32_t)k_ux_strm_off_hsh + 1U] =
    (uint8_t)((old >> (uint32_t)k_mut_shift_byte8) & (uint32_t)k_mut_mask_byte);
  const uint32_t cs                                 = internal_set_checksum_of(file);
  s_disk.bytes[file + (uint32_t)k_ux_file_off_csum] = (uint8_t)(cs & (uint32_t)k_mut_mask_byte);
  s_disk.bytes[file + (uint32_t)k_ux_file_off_csum + 1U] =
    (uint8_t)((cs >> (uint32_t)k_mut_shift_byte8) & (uint32_t)k_mut_mask_byte);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_unicode_dump_image("unicode_files_badhash", base);

  internal_free_volume();
  TEST_END("exfat unicode: images for the out-of-band fsck.exfat run");
}

/**
 * @brief Run the image-dump case.
 *
 * @return Process exit status.
 * @retval 0 The case passed; a failure aborts inside the harness instead.
 *
 * @pre No other suite shares this process.
 * @pre The fixture's RAM disk is not held by anything else.
 * @post The case has run and released its volume.
 * @post Nothing remains allocated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_exfat_dump_images_for_fsck();
  return 0;
}
