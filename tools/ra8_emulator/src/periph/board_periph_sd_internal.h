/**
 * @file board_periph_sd_internal.h
 * @brief Module-private FAT formatter shared by the board_periph_sd TUs
 *
 * @details
 * Carries what the three board_periph_sd translation units share: the modelled
 * card's state type and the single instance of it, the SD SPI sizing constants
 * that type is built from, and the blank-card (--sd-new) FAT16/FAT32 bounded
 * sector formatters. board_periph_sd.c models the SPI protocol,
 * board_periph_sd_image.c owns the sparse raw-file lifecycle, and
 * board_periph_sd_format.c serializes the initial sectors through that backend.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum board_sd_const_t
 * @brief SD SPI-mode protocol and sizing constants (no magic numbers).
 */
typedef enum : uint16_t {
  k_sd_block         = 512U,    /**< Bytes per SD block.                       */
  k_sd_cmd_len       = 6U,      /**< Command frame: opcode + 4 arg + crc.      */
  k_sd_resp_cap      = 520U,    /**< R1 + token + 512 data + 2 CRC.            */
  k_sd_cmd_start     = 0x40U,   /**< SPI command lead bits (01xxxxxx).         */
  k_sd_cmd_mask      = 0xC0U,   /**< Mask isolating the lead bits.             */
  k_sd_idx_mask      = 0x3FU,   /**< Command index in the lead byte.           */
  k_sd_idle          = 0xFFU,   /**< Bus idle / CIPO-high byte.                */
  k_sd_tok_data      = 0xFEU,   /**< Single-block read / write data token.     */
  k_sd_tok_wmulti    = 0xFCU,   /**< Multi-block write data-start token.       */
  k_sd_tok_stop      = 0xFDU,   /**< Multi-block write stop-tran token.        */
  k_sd_data_accept   = 0x05U,   /**< Data-response token: block accepted.      */
  k_sd_data_error    = 0x0DU,   /**< Data-response token: backend write error. */
  k_sd_read_error    = 0x01U,   /**< Read data-error token: backend fault.     */
  k_sd_busy          = 0x00U,   /**< CIPO held low while the card programs.    */
  k_sd_crc_len       = 2U,      /**< Trailing CRC16 bytes after a data block.  */
  k_sd_label_cap     = 12U,     /**< Volume-label field: 11 chars + NUL.       */
  k_sd_r1_idle       = 0x01U,   /**< R1 with IDLE set.                         */
  k_sd_r1_ready      = 0x00U,   /**< R1, card ready.                           */
  k_sd_r1_param_err  = 0x40U,   /**< R1 parameter/backend range error.         */
  k_sd_byte_mask     = 0xFFU,   /**< Low byte mask.                            */
  k_sd_byte_bits     = 8U,      /**< Bits per byte (shift amount).             */
  k_sd_r7_len        = 5U,      /**< R1 + 4-byte tail.                         */
  k_sd_arg_sh0       = 24U,     /**< Arg byte 0 (MSB) shift.                   */
  k_sd_arg_sh1       = 16U,     /**< Arg byte 1 shift.                         */
  k_sd_cmd8_echo     = 0xAAU,   /**< CMD8 check pattern echo.                  */
  k_sd_ocr_pwrccs    = 0xC0U,   /**< OCR byte 0: power-up done + CCS.          */
  k_sd_ocr_volt      = 0x80U,   /**< OCR byte 2: voltage window.               */
  k_sd_csd_v2        = 0x40U,   /**< CSD_STRUCTURE = 01b (v2.0 / SDHC).        */
  k_sd_csd_csize     = 0x0FU,   /**< C_SIZE byte (=> 8 MiB modelled card).     */
  k_sd_csd_len       = 16U,     /**< CSD register length.                      */
  k_sd_csd_csize_b7  = 7U,      /**< C_SIZE high byte offset in the CSD.       */
  k_sd_csd_csize_b8  = 8U,      /**< C_SIZE mid byte offset in the CSD.        */
  k_sd_csd_off       = 9U,      /**< C_SIZE LSB byte offset in the CSD.        */
  k_sd_csize_b7_mask = 0x3FU,   /**< C_SIZE field spans only 6 bits in b7.     */
  k_sd_crc_poly      = 0x1021U, /**< CRC16-CCITT polynomial.                   */
  k_sd_crc_msb       = 0x8000U, /**< Top bit of the 16-bit CRC register.       */
} board_sd_const_t;

/**
 * @enum board_sd_wr_phase_t
 * @brief Block-write sub-state once a CMD24/CMD25 has been accepted.
 */
typedef enum : uint8_t {
  k_sd_wr_idle  = 0U, /**< Not in a write transaction.                   */
  k_sd_wr_token = 1U, /**< Awaiting the data-start (or stop-tran) token. */
  k_sd_wr_data  = 2U, /**< Collecting the 512 payload bytes.             */
  k_sd_wr_crc   = 3U, /**< Collecting the trailing 2 CRC bytes.          */
} board_sd_wr_phase_t;
/**
 * @struct board_sd_state_t
 * @brief The modelled card: backing image + command/response framing.
 */
typedef struct {
  int                 image_fd;              /**< Owned anonymous sparse backing descriptor.  */
  uint64_t            image_len;             /**< Image size in bytes (64-bit: cards > 4 GB). */
  bool                attached;              /**< A `--sd` image is loaded.                   */
  uint8_t             fat_bits;              /**< 12/16/32 if formatted by --sd-new, else 0.  */
  char                label[k_sd_label_cap]; /**< Volume label (11 + NUL), for GUI.           */
  bool                collecting;            /**< Mid command-frame collection.               */
  bool                app_cmd;               /**< Previous command was CMD55 (APP_CMD).       */
  bool                ready;                 /**< ACMD41 has completed.                       */
  uint8_t             cmd[k_sd_cmd_len];     /**< Cmd.                                        */
  uint32_t            cmd_idx;               /**< Cmd index.                                  */
  uint8_t             resp[k_sd_resp_cap];   /**< Resp.                                       */
  uint32_t            resp_len;              /**< Resp length.                                */
  uint32_t            resp_pos;              /**< Resp pos.                                   */
  board_sd_wr_phase_t wr_phase;              /**< CMD24/CMD25 write sub-state.                */
  bool                wr_multi;              /**< Write is CMD25 (multi-block).               */
  uint64_t            wr_off;                /**< Byte offset of the current block.           */
  uint32_t            wr_cnt;                /**< Bytes seen in the data/CRC phase.           */
  uint8_t             wr_block[k_sd_block];  /**< Bounded block-write staging buffer.         */
  bool                rd_multi;              /**< An open CMD18 read stream.                  */
  uint64_t            rd_off;                /**< Byte offset of the next streamed
                                    *   CMD18 block.                      */
  uint32_t            erase_start;           /**< CMD32 ERASE_WR_BLK_START block. */
  uint32_t            erase_end;             /**< CMD33 ERASE_WR_BLK_END block.   */
} board_sd_state_t;

/** @brief SD sizing constants that exceed 16 bits. */
typedef enum : uint32_t {
  k_sd_csd_unit       = 512U * 1024U,               /**< CSD v2.0 C_SIZE unit: 512 KiB.     */
  k_sd_save_max_bytes = 2U * 1024U * 1024U * 1024U, /**< --save-sd cap: 2 GiB.              */
  k_unit_kib          = 1024U,                      /**< Binary kilo multiplier (KiB).      */
  k_sd_min_sectors    = 64U,                        /**< --sd-new floor: 32 KiB of sectors. */
} board_sd_const32_t;

/** @brief The single modelled SD card (defined in board_periph_sd.c). */
extern board_sd_state_t local_sd;

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
 * @brief Format the attached sparse backend as a FAT16 volume.
 *
 * @details Writes a complete BPB (boot frame, geometry, label, volume id),
 * the signature bytes and the FAT[0]/FAT[1] reserved entries -- enough to
 * satisfy a host `fsck_msdos` and the firmware's ra8_fs mount alike.
 *
 * @param[in]  image_fd      Sized sparse backing descriptor to format.
 * @param[in]  total_sectors Card capacity in 512-byte sectors.
 * @param[in]  label         Volume label (space-padded to 11 chars).
 * @param[out] out_spc       Receives the selected sectors per cluster.
 * @return Whether every required sector was written exactly.
 * @retval true The empty FAT16 volume was serialized.
 * @retval false A positioned backend write failed.
 * @pre @p image_fd is writable and sized for @p total_sectors sectors.
 * @pre @p total_sectors fits FAT16 cluster limits (caller picked FAT16).
 * @post The image mounts as an empty FAT16 volume.
 * @note Uses only fixed one-sector buffers; not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] bool priv_board_sd_format_fat16(int         image_fd,
                                                       uint32_t    total_sectors,
                                                       const char* label,
                                                       uint32_t*   out_spc);

/**
 * @brief Format the attached sparse backend as a FAT32 volume.
 *
 * @details Writes the FAT32 BPB (with FSInfo + backup boot sectors), the
 * root-directory cluster chain and both FAT copies' reserved entries.
 *
 * @param[in]  image_fd      Sized sparse backing descriptor to format.
 * @param[in]  total_sectors Card capacity in 512-byte sectors.
 * @param[in]  label         Volume label (space-padded to 11 chars).
 * @param[out] out_spc       Receives the selected sectors per cluster.
 * @return Whether every required sector was written exactly.
 * @retval true The empty FAT32 volume was serialized.
 * @retval false A positioned backend write failed.
 * @pre @p image_fd is writable and sized for @p total_sectors sectors.
 * @pre @p total_sectors warrants FAT32 (caller picked FAT32).
 * @post The image mounts as an empty FAT32 volume.
 * @note Uses only fixed one-sector buffers; not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] bool priv_board_sd_format_fat32(int         image_fd,
                                                       uint32_t    total_sectors,
                                                       const char* label,
                                                       uint32_t*   out_spc);

/** @brief Read exactly from the current sparse backend at a checked byte offset. */
RA8_PRIV [[nodiscard]] bool priv_board_sd_storage_read(uint64_t offset, void* dst, size_t count);
/** @brief Write exactly to the current sparse backend at a checked byte offset. */
RA8_PRIV [[nodiscard]] bool
priv_board_sd_storage_write(uint64_t offset, const void* src, size_t count);
/** @brief Make a checked byte range read as zero while preserving sparse storage. */
RA8_PRIV [[nodiscard]] bool priv_board_sd_storage_zero(uint64_t offset, uint64_t count);

/**
 * @brief Copy a volume label into an 11-char space-padded 8.3 field.
 * @details Reads at most 11 input bytes, stops at the first NUL, and fills every
 * remaining on-disk field byte with an ASCII space.
 *
 * @param[out] dst   Destination 11-byte label field.
 * @param[in]  label NUL-terminated label (truncated to 11 chars).
 * @return Nothing.
 * @pre @p dst holds at least 11 bytes.
 * @pre @p label is NUL-terminated.
 * @post @p dst is space-padded to the full field width.
 * @post No byte beyond the fixed 11-byte destination field is changed.
 * @note Pure buffer transform; thread-safe on distinct buffers.
 * @since 0.1.0
 */
RA8_PRIV void priv_board_sd_label_field(uint8_t* dst, const char* label);

#ifdef __cplusplus
}
#endif
