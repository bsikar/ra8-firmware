/**
 * @file mkfontimg.c
 * @brief Build an atomic FAT16 font image through bounded filesystem streaming
 *
 * @details
 * Creates the same 4 MiB FAT16 superfloppy used by the firmware and emulator,
 * but binds `ra8_fs` to a sparse sibling temporary rather than a resident disk
 * buffer. Font mode streams and readback-verifies one stable host input; blank
 * mode publishes the identical formatted empty image. Publication occurs only
 * after filesystem close/unmount, file sync, atomic rename, and directory sync.
 *
 * Usage: `mkfontimg <font-in> <image-out> [dest-name]`
 *        `mkfontimg --blank <image-out>`
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "mkfontimg_internal.h"
#include "ra8_attributes.h"
#include "ra8_fs.h"

/** @brief FAT16 geometry, BPB fields, and accepted font limits. */
typedef enum : uint32_t {
  k_block_size         = 512U,               /**< Bytes per sector.            */
  k_blocks_fat16       = 8U * 1024U,         /**< Sectors in the image.        */
  k_byte_lo_mask       = 0xFFU,              /**< Low-byte mask.               */
  k_byte_shift         = 8U,                 /**< One-byte bit shift.          */
  k_font_min_bytes     = 16U,                /**< Minimum accepted font bytes. */
  k_font_cap           = 4U * 1024U * 1024U, /**< Maximum accepted font bytes. */
  k_bpb_off_bytspersec = 11U,                /**< BPB bytes/sector offset.     */
  k_bpb_off_secperclus = 13U,                /**< BPB sectors/cluster offset.  */
  k_bpb_off_rsvdseccnt = 14U,                /**< BPB reserved-sector offset.  */
  k_bpb_off_numfats    = 16U,                /**< BPB FAT-count offset.        */
  k_bpb_off_rootentcnt = 17U,                /**< BPB root-entry offset.       */
  k_bpb_off_totsec16   = 19U,                /**< BPB total-sector offset.     */
  k_bpb_off_secfat     = 22U,                /**< BPB FAT-size offset.         */
  k_bpb_secperclus     = 1U,                 /**< Sectors per cluster.         */
  k_bpb_rsvdseccnt     = 1U,                 /**< Reserved sectors.            */
  k_bpb_numfats        = 2U,                 /**< FAT copy count.              */
  k_bpb_rootentcnt     = 16U,                /**< Fixed root entries.          */
  k_bpb_fatsz16        = 32U,                /**< Sectors per FAT.             */
  k_bpb_sig_off_a      = 510U,               /**< Signature low offset.        */
  k_bpb_sig_off_b      = 511U,               /**< Signature high offset.       */
  k_bpb_sig_a          = 0x55U,              /**< Signature low byte.          */
  k_bpb_sig_b          = 0xAAU,              /**< Signature high byte.         */
} mkfontimg_constant_t;

/**
 * @brief Store one little-endian 16-bit BPB field.
 * @details Encodes directly into the caller-owned deterministic boot sector.
 * @param[out] sector Boot-sector buffer.
 * @param[in]  offset Field byte offset.
 * @param[in]  value  Value to encode.
 * @pre @p sector has at least ::k_block_size writable bytes.
 * @pre @p offset leaves room for two bytes.
 * @post Exactly two bytes hold @p value low byte first.
 * @post All other bytes remain unchanged.
 * @note Pure apart from @p sector; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_put16(uint8_t* sector, uint32_t offset, uint16_t value)
{
  sector[offset]      = (uint8_t)(value & (uint16_t)k_byte_lo_mask);
  sector[offset + 1U] = (uint8_t)((value >> (uint16_t)k_byte_shift) & (uint16_t)k_byte_lo_mask);
}

/**
 * @brief Build the legacy-compatible minimal FAT16 boot sector.
 * @details Writes only fixed geometry fields and the required trailing signature.
 * @param[out] sector Zeroed 512-byte boot-sector buffer.
 * @pre @p sector is non-NULL and zero-initialized.
 * @pre The enum geometry remains consistent with the legacy generator.
 * @post The BPB mounts as the exact 4 MiB FAT16 layout expected by `ra8_fs`.
 * @post Bytes outside named fields and the signature remain zero.
 * @note Pure apart from @p sector; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_build_fat16(uint8_t sector[k_block_size])
{
  internal_put16(sector, (uint32_t)k_bpb_off_bytspersec, (uint16_t)k_block_size);
  sector[k_bpb_off_secperclus] = (uint8_t)k_bpb_secperclus;
  internal_put16(sector, (uint32_t)k_bpb_off_rsvdseccnt, (uint16_t)k_bpb_rsvdseccnt);
  sector[k_bpb_off_numfats] = (uint8_t)k_bpb_numfats;
  internal_put16(sector, (uint32_t)k_bpb_off_rootentcnt, (uint16_t)k_bpb_rootentcnt);
  internal_put16(sector, (uint32_t)k_bpb_off_totsec16, (uint16_t)k_blocks_fat16);
  internal_put16(sector, (uint32_t)k_bpb_off_secfat, (uint16_t)k_bpb_fatsz16);
  sector[k_bpb_sig_off_a] = (uint8_t)k_bpb_sig_a;
  sector[k_bpb_sig_off_b] = (uint8_t)k_bpb_sig_b;
}

/**
 * @brief Create, seed, and mount one unpublished FAT16 image.
 * @details Keeps all mutations in a private sibling until final publication.
 * @param[in]  image_out Requested final path.
 * @param[out] host      Receives hosted backend/publication state.
 * @param[out] mount     Receives mounted filesystem.
 * @return true only when the fresh image mounts successfully.
 * @retval true  Caller owns @p host and must unmount @p mount.
 * @retval false No descriptor or temporary remains owned.
 * @pre All arguments are non-NULL.
 * @pre @p image_out is NUL-terminated.
 * @post On true the legacy-compatible BPB is present at sector zero.
 * @post On false any sibling temporary is removed.
 * @note Not thread-safe through global `ra8_fs` mount slots.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_mount_image(const char* image_out, mkfontimg_host_t* host, ra8_fs_mount_t** mount)
{
  if (!priv_mkfontimg_host_begin(image_out, k_blocks_fat16, k_block_size, host)) {
    return false;
  }
  uint8_t sector[k_block_size] = {};
  internal_build_fat16(sector);
  if (!priv_mkfontimg_host_seed(host, sector, sizeof(sector)) ||
      ra8_fs_mount(&host->backend, mount) != k_ra8_ok) {
    priv_mkfontimg_host_abort(host);
    return false;
  }
  return true;
}

/**
 * @brief Finish filesystem state and publish the image atomically.
 * @details Refuses publication after unmount or sticky backend I/O failure.
 * @param[in,out] host  Hosted temporary state.
 * @param[in,out] mount Mounted FAT16 volume.
 * @return true when unmount and durable publication both succeed.
 * @retval true  Final path atomically names the complete image.
 * @retval false Temporary was removed or durability is unconfirmed.
 * @pre No card file remains open.
 * @pre @p mount was returned by ::internal_mount_image.
 * @post No temporary descriptor remains owned.
 * @post Before rename, any failure preserves an existing destination.
 * @note Not thread-safe through global `ra8_fs` mount slots.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_finish_image(mkfontimg_host_t* host, ra8_fs_mount_t* mount)
{
  if (ra8_fs_unmount(mount) != k_ra8_ok || host->disk.io_failed) {
    priv_mkfontimg_host_abort(host);
    return false;
  }
  return priv_mkfontimg_host_commit(host);
}

/**
 * @brief Generate and publish a formatted empty FAT16 image.
 * @details Runs the same transaction path as font images without creating a file.
 * @param[in] image_out Final output path.
 * @return Process status.
 * @retval 0 Blank image was published.
 * @retval 1 Build, unmount, or publication failed.
 * @pre @p image_out is non-NULL and NUL-terminated.
 * @pre The destination parent directory exists.
 * @post On success output bytes match the legacy blank generator.
 * @post On failure before rename an existing destination is preserved.
 * @note Not thread-safe through global `ra8_fs` slots.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_run_blank(const char* image_out)
{
  mkfontimg_host_t host;
  ra8_fs_mount_t*  mount = nullptr;
  if (!internal_mount_image(image_out, &host, &mount) || !internal_finish_image(&host, mount)) {
    priv_mkfontimg_diag("mkfontimg: blank image generation failed; destination preserved\n");
    return 1;
  }
  priv_mkfontimg_diag("mkfontimg: wrote ");
  priv_mkfontimg_diag(image_out);
  priv_mkfontimg_diag(" (blank FAT16, no font)\n");
  return 0;
}

/**
 * @brief Generate a FAT16 image containing one verified streamed font.
 * @details Requires stable metadata and a byte-for-byte second pass before rename.
 * @param[in] font_in   Host font path.
 * @param[in] image_out Final output path.
 * @param[in] card_name Destination name on the FAT volume.
 * @return Process status.
 * @retval 0 Font image was published.
 * @retval 1 Input, filesystem, verification, or publication failed.
 * @pre All strings are non-NULL and NUL-terminated.
 * @pre @p card_name satisfies the `ra8_fs` filename contract.
 * @post On success card bytes exactly match one stable host-input snapshot.
 * @post On failure before rename an existing destination is preserved.
 * @note Not thread-safe through global `ra8_fs` slots.
 * @since 0.1.0
 */
RA8_INTERNAL static int
internal_run_font(const char* font_in, const char* image_out, const char* card_name)
{
  mkfontimg_host_t host;
  ra8_fs_mount_t*  mount = nullptr;
  if (!internal_mount_image(image_out, &host, &mount)) {
    priv_mkfontimg_diag("mkfontimg: cannot create or mount safe output\n");
    return 1;
  }
  uint64_t font_bytes = 0U;
  bool     copied     = priv_mkfontimg_host_copy(&host,
                                                 mount,
                                                 font_in,
                                                 card_name,
                                                 k_font_min_bytes,
                                                 k_font_cap,
                                                 &font_bytes);
  if (!copied) {
    (void)ra8_fs_unmount(mount);
    priv_mkfontimg_host_abort(&host);
    priv_mkfontimg_diag("mkfontimg: font changed, is invalid, or could not be copied exactly: ");
    priv_mkfontimg_diag(font_in);
    priv_mkfontimg_diag("\n");
    return 1;
  }
  if (!internal_finish_image(&host, mount)) {
    priv_mkfontimg_diag("mkfontimg: publication failed; destination preserved\n");
    return 1;
  }
  priv_mkfontimg_diag("mkfontimg: wrote ");
  priv_mkfontimg_diag(image_out);
  priv_mkfontimg_diag(" (");
  priv_mkfontimg_diag(card_name);
  priv_mkfontimg_diag(" = ");
  priv_mkfontimg_diag_u64(font_bytes);
  priv_mkfontimg_diag(" bytes)\n");
  return 0;
}

/**
 * @brief Parse CLI mode and delegate to blank or font image generation.
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return Process status: zero success, one runtime failure, two usage error.
 * @retval 0 A complete verified image was published.
 * @retval 1 Generation failed.
 * @retval 2 Command-line shape was invalid.
 * @pre Hosted runtime supplies @p argc entries in @p argv.
 * @pre Argument strings are NUL-terminated.
 * @post No temporary descriptor remains open.
 * @post Usage errors never touch the destination.
 * @note Not thread-safe through global `ra8_fs` slots.
 * @since 0.1.0
 */
int main(int argc, char** argv)
{
  if ((argc >= 2) && (strcmp(argv[1], "--blank") == 0)) {
    if (argc != 3) {
      priv_mkfontimg_diag("usage: mkfontimg --blank <image-out>\n");
      return 2;
    }
    return internal_run_blank(argv[2]);
  }
  if (argc < 3 || argc > 4) {
    priv_mkfontimg_diag("usage: mkfontimg <font-in> <image-out> [dest-name]\n");
    return 2;
  }
  const char* card_name = (argc == 4) ? argv[3] : "FONT.OTF";
  return internal_run_font(argv[1], argv[2], card_name);
}
