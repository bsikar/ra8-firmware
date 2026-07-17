/**
 * @file board_periph_sd_internal.h
 * @brief Module-private FAT formatter shared by the board_periph_sd TUs
 *
 * @details
 * The blank-card (--sd-new) FAT16/FAT32 in-memory formatters live in their
 * own translation unit (board_periph_sd_format.c); the card model's
 * board_sd_attach_blank consumes them through these declarations. Pure
 * image-buffer transforms -- no card state is touched here.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief FAT flavour selectors shared by the formatter and the card model. */
typedef enum : uint32_t {
  k_fat32_bits = 32U, /**< FAT32 selector / label width. */
  k_fat16_bits = 16U, /**< FAT16 selector.               */
} sd_fat_bits_t;

/** @brief FAT BPB field offsets + format constants. */
typedef enum : uint32_t {
  k_fmt_sec_bytes    = 512U,        /**< Bytes per sector.                          */
  k_fmt_fat16_max    = 65524U,      /**< Max FAT16 data clusters.                   */
  k_fmt_fat16_min    = 4085U,       /**< Min FAT16 data clusters.                   */
  k_fmt_spc_max      = 64U,         /**< Largest sectors-per-cluster we pick.       */
  k_fmt_fat32_pref   = 4194304U,    /**< FAT32: preferred max clusters (bumps spc). */
  k_fmt_root_ents    = 512U,        /**< FAT16 root-directory entries.              */
  k_fmt_volid        = 0x52A8D200U, /**< Arbitrary volume serial base.              */
  k_fmt_resv_f16     = 1U,          /**< FAT16 reserved sectors (boot).             */
  k_fmt_resv_f32     = 32U,         /**< FAT32 reserved sectors.                    */
  k_fmt_label_len    = 11U,         /**< 8.3 volume-label field width.              */
  k_fmt_totsec16_max = 0x10000U,    /**< Above this, use the 32-bit TotSec.         */
} sd_fmt_const_t;

/**
 * @brief Format @p img as a FAT16 volume of @p total_sectors.
 *
 * @details Writes a complete BPB (boot frame, geometry, label, volume id),
 * the signature bytes and the FAT[0]/FAT[1] reserved entries -- enough to
 * satisfy a host `fsck_msdos` and the firmware's ra8_fs mount alike.
 *
 * @param[out] img           Zeroed card image buffer.
 * @param[in]  total_sectors Card capacity in 512-byte sectors.
 * @param[in]  label         Volume label (space-padded to 11 chars).
 * @return Sectors per cluster the formatter selected.
 * @retval 0 Never (a positive cluster size is always chosen).
 * @pre @p img holds at least @p total_sectors * 512 zeroed bytes.
 * @pre @p total_sectors fits FAT16 cluster limits (caller picked FAT16).
 * @post The image mounts as an empty FAT16 volume.
 * @note Pure buffer transform; thread-safe on distinct buffers.
 * @since 0.1.0
 */
RA8_PRIV uint32_t sd_format_fat16(uint8_t* img, uint32_t total_sectors, const char* label);

/**
 * @brief Format @p img as a FAT32 volume of @p total_sectors.
 *
 * @details Writes the FAT32 BPB (with FSInfo + backup boot sectors), the
 * root-directory cluster chain and both FAT copies' reserved entries.
 *
 * @param[out] img           Zeroed card image buffer.
 * @param[in]  total_sectors Card capacity in 512-byte sectors.
 * @param[in]  label         Volume label (space-padded to 11 chars).
 * @return Sectors per cluster the formatter selected.
 * @retval 0 Never (a positive cluster size is always chosen).
 * @pre @p img holds at least @p total_sectors * 512 zeroed bytes.
 * @pre @p total_sectors warrants FAT32 (caller picked FAT32).
 * @post The image mounts as an empty FAT32 volume.
 * @note Pure buffer transform; thread-safe on distinct buffers.
 * @since 0.1.0
 */
RA8_PRIV uint32_t sd_format_fat32(uint8_t* img, uint32_t total_sectors, const char* label);

/**
 * @brief Copy a volume label into an 11-char space-padded 8.3 field.
 *
 * @param[out] dst   Destination 11-byte label field.
 * @param[in]  label NUL-terminated label (truncated to 11 chars).
 * @return Nothing.
 * @pre @p dst holds at least 11 bytes.
 * @pre @p label is NUL-terminated.
 * @post @p dst is space-padded to the full field width.
 * @note Pure buffer transform; thread-safe on distinct buffers.
 * @since 0.1.0
 */
RA8_PRIV void sd_label_field(uint8_t* dst, const char* label);

#ifdef __cplusplus
}
#endif
