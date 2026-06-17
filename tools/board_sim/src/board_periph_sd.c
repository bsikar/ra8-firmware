/**
 * @file board_periph_sd.c
 * @brief SD-card-over-SPI device model for board_sim (attached to SPI_B).
 *
 * @details
 * Implements a high-capacity (SDHC) SD card running in SPI mode, backed by a
 * host image file. The card answers the SD SPI command set the firmware's
 * @c ra_sdmmc_spi driver issues during bring-up and block I/O:
 *
 *  - CMD0  GO_IDLE_STATE        -> R1 = 0x01
 *  - CMD8  SEND_IF_COND         -> R7 (R1 + 0x000001AA echo)
 *  - CMD55 + ACMD41             -> R1 = 0x00 (card ready)
 *  - CMD58 READ_OCR             -> R3 (R1 + OCR with CCS=1 = SDHC)
 *  - CMD9  SEND_CSD             -> R1 + 0xFE + 16-byte CSD v2.0 + CRC16
 *  - CMD17 READ_SINGLE_BLOCK    -> R1 + 0xFE + 512 data bytes + CRC16
 *
 * Commands self-frame off the @c 01xxxxxx lead bits, so the model needs no
 * chip-select wiring. The exact same protocol logic is verified end to end
 * against the real driver in @c tests/test_ra_sdmmc_card_reflow.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "board_periph_sd.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @enum board_sd_const_t
 * @brief SD SPI-mode protocol and sizing constants (no magic numbers).
 */
typedef enum : uint16_t {
  k_sd_block       = 512U,    /**< Bytes per SD block.                     */
  k_sd_cmd_len     = 6U,      /**< Command frame: opcode + 4 arg + crc.    */
  k_sd_resp_cap    = 520U,    /**< R1 + token + 512 data + 2 CRC.          */
  k_sd_cmd_start   = 0x40U,   /**< SPI command lead bits (01xxxxxx).       */
  k_sd_cmd_mask    = 0xC0U,   /**< Mask isolating the lead bits.           */
  k_sd_idx_mask    = 0x3FU,   /**< Command index in the lead byte.         */
  k_sd_idle        = 0xFFU,   /**< Bus idle / CIPO-high byte.              */
  k_sd_tok_data    = 0xFEU,   /**< Single-block read / write data token.   */
  k_sd_tok_wmulti  = 0xFCU,   /**< Multi-block write data-start token.      */
  k_sd_tok_stop    = 0xFDU,   /**< Multi-block write stop-tran token.       */
  k_sd_data_accept = 0x05U,   /**< Data-response token: block accepted.    */
  k_sd_busy        = 0x00U,   /**< CIPO held low while the card programs.   */
  k_sd_crc_len     = 2U,      /**< Trailing CRC16 bytes after a data block. */
  k_sd_r1_idle     = 0x01U,   /**< R1 with IDLE set.                       */
  k_sd_r1_ready    = 0x00U,   /**< R1, card ready.                         */
  k_sd_byte_mask   = 0xFFU,   /**< Low byte mask.                          */
  k_sd_byte_bits   = 8U,      /**< Bits per byte (shift amount).           */
  k_sd_r7_len      = 5U,      /**< R1 + 4-byte tail.                       */
  k_sd_arg_sh0     = 24U,     /**< Arg byte 0 (MSB) shift.                 */
  k_sd_arg_sh1     = 16U,     /**< Arg byte 1 shift.                       */
  k_sd_cmd8_echo   = 0xAAU,   /**< CMD8 check pattern echo.                */
  k_sd_ocr_pwrccs  = 0xC0U,   /**< OCR byte 0: power-up done + CCS.        */
  k_sd_ocr_volt    = 0x80U,   /**< OCR byte 2: voltage window.             */
  k_sd_csd_v2      = 0x40U,   /**< CSD_STRUCTURE = 01b (v2.0 / SDHC).      */
  k_sd_csd_csize   = 0x0FU,   /**< C_SIZE byte (=> 8 MiB modelled card).   */
  k_sd_csd_len     = 16U,     /**< CSD register length.                    */
  k_sd_csd_off     = 9U,      /**< C_SIZE LSB byte offset in the CSD.      */
  k_sd_crc_poly    = 0x1021U, /**< CRC16-CCITT polynomial.                 */
  k_sd_crc_msb     = 0x8000U, /**< Top bit of the 16-bit CRC register.     */
} board_sd_const_t;

/** @brief SD sizing constants that exceed 16 bits. */
typedef enum : uint32_t {
  k_sd_csd_unit = 512U * 1024U, /**< CSD v2.0 C_SIZE capacity unit: 512 KiB. */
} board_sd_const32_t;

/**
 * @enum board_sd_cmd_idx_t
 * @brief SD SPI command indices answered by the model (low 6 bits of byte 0).
 */
typedef enum : uint8_t {
  k_sd_idx_cmd0   = 0U,  /**< GO_IDLE_STATE.   */
  k_sd_idx_cmd8   = 8U,  /**< SEND_IF_COND.    */
  k_sd_idx_cmd9   = 9U,  /**< SEND_CSD.        */
  k_sd_idx_cmd16  = 16U, /**< SET_BLOCKLEN.    */
  k_sd_idx_cmd17  = 17U, /**< READ_SINGLE.     */
  k_sd_idx_cmd24  = 24U, /**< WRITE_SINGLE.    */
  k_sd_idx_cmd25  = 25U, /**< WRITE_MULTIPLE.  */
  k_sd_idx_acmd41 = 41U, /**< SD_SEND_OP_COND. */
  k_sd_idx_cmd55  = 55U, /**< APP_CMD.         */
  k_sd_idx_cmd58  = 58U, /**< READ_OCR.        */
} board_sd_cmd_idx_t;

/**
 * @enum board_sd_wr_phase_t
 * @brief Block-write sub-state once a CMD24/CMD25 has been accepted.
 */
typedef enum : uint8_t {
  k_sd_wr_idle  = 0U, /**< Not in a write transaction.                 */
  k_sd_wr_token = 1U, /**< Awaiting the data-start (or stop-tran) token.*/
  k_sd_wr_data  = 2U, /**< Collecting the 512 payload bytes.           */
  k_sd_wr_crc   = 3U, /**< Collecting the trailing 2 CRC bytes.        */
} board_sd_wr_phase_t;

/**
 * @struct board_sd_state_t
 * @brief The modelled card: backing image + command/response framing.
 */
typedef struct {
  uint8_t*            image;      /**< Backing card image (host file contents).   */
  uint32_t            image_len;  /**< Image size in bytes.                       */
  bool                attached;   /**< A `--sd` image is loaded.                  */
  uint8_t             fat_bits;   /**< 12/16/32 if formatted by --sd-new, else 0. */
  char                label[12];  /**< Volume label (11 chars + NUL), for the GUI.*/
  bool                collecting; /**< Mid command-frame collection.              */
  bool                app_cmd;    /**< Previous command was CMD55 (APP_CMD).      */
  bool                ready;      /**< ACMD41 has completed.                      */
  uint8_t             cmd[k_sd_cmd_len];
  uint32_t            cmd_idx;
  uint8_t             resp[k_sd_resp_cap];
  uint32_t            resp_len;
  uint32_t            resp_pos;
  board_sd_wr_phase_t wr_phase; /**< CMD24/CMD25 write sub-state.       */
  bool                wr_multi; /**< Write is CMD25 (multi-block).      */
  uint32_t            wr_off;   /**< Byte offset of the current block.  */
  uint32_t            wr_cnt;   /**< Bytes seen in the data/CRC phase.  */
} board_sd_state_t;

/** @brief The single modelled SD card. */
static board_sd_state_t s_sd;

/**
 * @brief CRC16-CCITT (poly 0x1021, init 0) over a buffer.
 *
 * @details Matches the firmware driver's @c ra_sdmmc_spi_crc16 so the card's
 * data-block CRCs validate.
 *
 * @param[in] data Bytes to checksum.
 * @param[in] len  Number of bytes.
 * @return The 16-bit CRC.
 * @retval 0 Empty input.
 * @pre `data` is non-null when `len > 0`.
 * @pre None.
 * @post No state is modified (pure).
 * @post No state is modified (pure).
 * @note Pure function.
 * @since 0.1.0
 */
static uint16_t board_sd_crc16(const uint8_t* data, uint32_t len)
{
  uint16_t crc = 0U;
  for (uint32_t i = 0U; i < len; i++) {
    crc ^= (uint16_t)((uint16_t)data[i] << (uint16_t)k_sd_byte_bits);
    for (uint8_t b = 0U; b < (uint8_t)k_sd_byte_bits; b++) {
      const bool msb = (crc & (uint16_t)k_sd_crc_msb) != 0U;
      crc            = (uint16_t)(crc << 1U);
      if (msb) {
        crc = (uint16_t)(crc ^ (uint16_t)k_sd_crc_poly);
      }
    }
  }
  return crc;
}

/**
 * @brief Stage the R1 + data-token + payload + CRC16 reply for a block read.
 *
 * @param[in,out] c       Card state.
 * @param[in]      payload Source bytes.
 * @param[in]      len     Payload length (16 for CSD, 512 for a data block).
 * @return None.
 * @retval None Void.
 * @pre `c` and `payload` are non-null and `len + 4 <= k_sd_resp_cap`.
 * @pre None.
 * @post `c->resp` holds R1, token, payload, and the two CRC bytes.
 * @post `c->resp_len` is set; `c->resp_pos` is left for the caller.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void board_sd_stage_block(board_sd_state_t* c, const uint8_t* payload, uint32_t len)
{
  c->resp[0] = (uint8_t)k_sd_r1_ready;
  c->resp[1] = (uint8_t)k_sd_tok_data;
  memcpy(&c->resp[2], payload, len);
  const uint16_t crc     = board_sd_crc16(&c->resp[2], len);
  c->resp[2U + len]      = (uint8_t)(crc >> (uint16_t)k_sd_byte_bits);
  c->resp[2U + len + 1U] = (uint8_t)(crc & (uint16_t)k_sd_byte_mask);
  c->resp_len            = 2U + len + 2U;
}

/**
 * @brief Accept a CMD24/CMD25 block-write command and arm the data phase.
 *
 * @details Replies R1 (ready) and enters ::k_sd_wr_token, where ::board_sd_exchange
 * collects the host's data token + 512 payload bytes + CRC into the backing image
 * (the read side, CMD17, then serves them back). SDHC addresses by block, so the
 * argument is scaled to a byte offset.
 *
 * @param[in,out] c   Card state.
 * @param[in]     idx Command index (CMD24 single or CMD25 multi).
 * @param[in]     arg Block address argument.
 * @param[in]     r1  R1 status byte to return (ready/idle).
 * @return None.
 * @retval None Void.
 * @pre `c` is non-null and a writable image is attached.
 * @pre `idx` is ::k_sd_idx_cmd24 or ::k_sd_idx_cmd25.
 * @post `c->resp` holds R1; the write sub-state is armed.
 * @post The backing image is unchanged until the data phase runs.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void board_sd_begin_write(board_sd_state_t* c, uint8_t idx, uint32_t arg, uint8_t r1)
{
  c->resp[0]  = r1;
  c->resp_len = 1U;
  c->wr_phase = k_sd_wr_token;
  c->wr_multi = (idx == (uint8_t)k_sd_idx_cmd25);
  c->wr_off   = arg * (uint32_t)k_sd_block;
  c->wr_cnt   = 0U;
}

/**
 * @brief Feed one host byte into an in-flight block write; return the CIPO byte.
 *
 * @details Drives the SD SPI write handshake (SD PHY v9 7.3.3): skip idle bytes
 * until the data-start token (0xFE single / 0xFC multi), capture 512 payload
 * bytes into the backing image, swallow the 2 CRC bytes, then stage the
 * data-response token (0x05 accepted) + one busy byte + done. A CMD25 multi-write
 * advances to the next block and re-arms the token wait; the stop-tran token
 * (0xFD) ends it. board_sim has no real program delay, so a single busy byte
 * suffices.
 *
 * @param[in,out] c  Card state (must be mid-write).
 * @param[in]     tx Byte clocked out by the host.
 * @return The byte the card presents on CIPO (always idle here; the staged
 *         data-response is drained by ::board_sd_exchange next).
 * @retval 0xFF Bus idle while collecting the data phase.
 * @pre `c->wr_phase != k_sd_wr_idle`.
 * @pre A writable backing image is attached.
 * @post Captured payload bytes are written into `c->image`.
 * @post On block completion the data-response token is staged in `c->resp`.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static uint8_t board_sd_write_byte(board_sd_state_t* c, uint8_t tx)
{
  if (c->wr_phase == k_sd_wr_token) {
    if ((tx == (uint8_t)k_sd_tok_data) || (tx == (uint8_t)k_sd_tok_wmulti)) {
      c->wr_phase = k_sd_wr_data;
      c->wr_cnt   = 0U;
    } else if (c->wr_multi && (tx == (uint8_t)k_sd_tok_stop)) {
      c->wr_phase = k_sd_wr_idle;
      c->wr_multi = false;
      c->resp[0]  = (uint8_t)k_sd_busy; /* program time, then done. */
      c->resp[1]  = (uint8_t)k_sd_idle;
      c->resp_len = 2U;
      c->resp_pos = 0U;
    }
    return (uint8_t)k_sd_idle;
  }
  if (c->wr_phase == k_sd_wr_data) {
    const uint32_t a = c->wr_off + c->wr_cnt;
    if (a < c->image_len) {
      c->image[a] = tx;
    }
    c->wr_cnt++;
    if (c->wr_cnt >= (uint32_t)k_sd_block) {
      c->wr_phase = k_sd_wr_crc;
      c->wr_cnt   = 0U;
    }
    return (uint8_t)k_sd_idle;
  }
  /* k_sd_wr_crc: swallow the 2 CRC bytes, then ack the block. */
  c->wr_cnt++;
  if (c->wr_cnt >= (uint32_t)k_sd_crc_len) {
    c->resp[0]  = (uint8_t)k_sd_data_accept;
    c->resp[1]  = (uint8_t)k_sd_busy;
    c->resp[2]  = (uint8_t)k_sd_idle;
    c->resp_len = 3U;
    c->resp_pos = 0U;
    c->wr_cnt   = 0U;
    if (c->wr_multi) {
      c->wr_off += (uint32_t)k_sd_block; /* next block of the multi-write. */
      c->wr_phase = k_sd_wr_token;
    } else {
      c->wr_phase = k_sd_wr_idle;
    }
  }
  return (uint8_t)k_sd_idle;
}

/**
 * @brief Build the response for a completed 6-byte command frame.
 *
 * @param[in,out] c Card state holding the collected command.
 * @return None.
 * @retval None Void.
 * @pre `c` is non-null and `c->cmd` holds a full frame.
 * @pre A card image is attached.
 * @post `c->resp` / `c->resp_len` describe the reply; `c->resp_pos` reset.
 * @post The backing image is not modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void board_sd_process_cmd(board_sd_state_t* c)
{
  const uint8_t  idx     = (uint8_t)(c->cmd[0] & (uint8_t)k_sd_idx_mask);
  const uint32_t arg     = ((uint32_t)c->cmd[1] << (uint32_t)k_sd_arg_sh0) |
                           ((uint32_t)c->cmd[2] << (uint32_t)k_sd_arg_sh1) |
                           ((uint32_t)c->cmd[3] << (uint32_t)k_sd_byte_bits) | (uint32_t)c->cmd[4];
  const bool     was_app = c->app_cmd;
  const uint8_t  r1      = c->ready ? (uint8_t)k_sd_r1_ready : (uint8_t)k_sd_r1_idle;
  c->app_cmd             = false;
  c->resp_pos            = 0U;
  c->resp_len            = 0U;

  if ((idx == (uint8_t)k_sd_idx_acmd41) && was_app) { /* ACMD41 -- init done. */
    c->ready    = true;
    c->resp[0]  = (uint8_t)k_sd_r1_ready;
    c->resp_len = 1U;
    return;
  }
  switch (idx) {
    case (uint8_t)k_sd_idx_cmd0:
      c->resp[0]  = (uint8_t)k_sd_r1_idle;
      c->resp_len = 1U;
      break;
    case (uint8_t)k_sd_idx_cmd8: /* R7: R1 + 0x000001AA echo. */
      c->resp[0]  = (uint8_t)k_sd_r1_idle;
      c->resp[1]  = 0U;
      c->resp[2]  = 0U;
      c->resp[3]  = 1U;
      c->resp[4]  = (uint8_t)k_sd_cmd8_echo;
      c->resp_len = (uint8_t)k_sd_r7_len;
      break;
    case (uint8_t)k_sd_idx_cmd55:
      c->app_cmd  = true;
      c->resp[0]  = (uint8_t)k_sd_r1_idle;
      c->resp_len = 1U;
      break;
    case (uint8_t)k_sd_idx_cmd58: /* R3: R1 + OCR (CCS=1 -> SDHC). */
      c->resp[0]  = r1;
      c->resp[1]  = (uint8_t)k_sd_ocr_pwrccs;
      c->resp[2]  = (uint8_t)k_sd_idle;
      c->resp[3]  = (uint8_t)k_sd_ocr_volt;
      c->resp[4]  = 0U;
      c->resp_len = (uint8_t)k_sd_r7_len;
      break;
    case (uint8_t)k_sd_idx_cmd16:
      c->resp[0]  = (uint8_t)k_sd_r1_ready;
      c->resp_len = 1U;
      break;
    case (uint8_t)k_sd_idx_cmd9: { /* SEND_CSD: 16-byte CSD v2.0 data block. */
      uint8_t csd[k_sd_csd_len];
      memset(csd, 0, sizeof(csd));
      csd[0] = (uint8_t)k_sd_csd_v2;
      /* CSD v2.0 capacity = (C_SIZE + 1) * 512 KiB. Derive C_SIZE (22-bit field
       * in bytes 7..9) from the actual image so --sd / --sd-new present their real
       * size to the firmware instead of a fixed 8 MiB. */
      uint32_t csize = (uint32_t)k_sd_csd_csize; /* fallback: 8 MiB. */
      if (c->image_len >= (uint32_t)k_sd_csd_unit) {
        csize = (c->image_len / (uint32_t)k_sd_csd_unit) - 1U;
      }
      csd[7]            = (uint8_t)((csize >> 16) & 0x3FU);
      csd[8]            = (uint8_t)((csize >> 8) & (uint16_t)k_sd_byte_mask);
      csd[k_sd_csd_off] = (uint8_t)(csize & (uint16_t)k_sd_byte_mask);
      board_sd_stage_block(c, csd, (uint32_t)k_sd_csd_len);
      break;
    }
    case (uint8_t)k_sd_idx_cmd17: { /* READ_SINGLE_BLOCK (SDHC: arg = block). */
      uint8_t        blk[k_sd_block] = {};
      const uint32_t off             = arg * (uint32_t)k_sd_block;
      for (uint32_t i = 0U; i < (uint32_t)k_sd_block; ++i) {
        const uint32_t a = off + i;
        blk[i]           = (a < c->image_len) ? c->image[a] : 0U;
      }
      board_sd_stage_block(c, blk, (uint32_t)k_sd_block);
      break;
    }
    case (uint8_t)k_sd_idx_cmd24: /* WRITE_SINGLE_BLOCK   */
    case (uint8_t)k_sd_idx_cmd25: /* WRITE_MULTIPLE_BLOCK */
      board_sd_begin_write(c, idx, arg, r1);
      break;
    default:
      c->resp[0]  = r1;
      c->resp_len = 1U;
      break;
  }
}

bool board_sd_attach(const char* path)
{
  if (path == nullptr) {
    return false;
  }
  FILE* fp = fopen(path, "rb");
  if (fp == nullptr) {
    (void)fprintf(stderr, "board_sim: --sd: cannot open '%s'\n", path);
    return false;
  }
  (void)fseek(fp, 0L, SEEK_END);
  const long size = ftell(fp);
  (void)fseek(fp, 0L, SEEK_SET);
  if (size <= 0L) {
    (void)fclose(fp);
    (void)fprintf(stderr, "board_sim: --sd: empty image '%s'\n", path);
    return false;
  }
  free(s_sd.image);
  s_sd       = (board_sd_state_t){};
  s_sd.image = (uint8_t*)malloc((size_t)size);
  if (s_sd.image == nullptr) {
    (void)fclose(fp);
    return false;
  }
  const size_t got = fread(s_sd.image, 1U, (size_t)size, fp);
  (void)fclose(fp);
  if (got != (size_t)size) {
    free(s_sd.image);
    s_sd.image = nullptr;
    return false;
  }
  s_sd.image_len = (uint32_t)size;
  s_sd.attached  = true;
  (void)fprintf(stderr, "board_sim: SD card attached (%ld bytes) from %s\n", size, path);
  return true;
}

bool board_sd_attached(void)
{
  return s_sd.attached;
}

/* ----------------------------------------------------------------------------
 * Blank-card creation (--sd-new): format an empty FAT volume in memory so a
 * size/format can be set up with no pre-built image. The BPB is complete enough
 * to satisfy a host `fsck_msdos` and the firmware's ra_fs mount alike.
 * --------------------------------------------------------------------------*/

/** @brief FAT BPB field offsets + format constants. */
typedef enum : uint32_t {
  k_fmt_sec_bytes = 512U,        /**< Bytes per sector.                    */
  k_fmt_fat16_max = 65524U,      /**< Max FAT16 data clusters.             */
  k_fmt_fat16_min = 4085U,       /**< Min FAT16 data clusters.             */
  k_fmt_spc_max   = 64U,         /**< Largest sectors-per-cluster we pick. */
  k_fmt_root_ents = 512U,        /**< FAT16 root-directory entries.        */
  k_fmt_volid     = 0x52A8D200U, /**< Arbitrary volume serial base.        */
  k_fmt_resv_f16  = 1U,          /**< FAT16 reserved sectors (boot).       */
  k_fmt_resv_f32  = 32U,         /**< FAT32 reserved sectors.              */
} sd_fmt_const_t;

/** @brief Little-endian 16-bit store into the image. */
static void sd_put16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

/** @brief Little-endian 32-bit store into the image. */
static void sd_put32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8) & 0xFFU);
  p[2] = (uint8_t)((v >> 16) & 0xFFU);
  p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

/** @brief Write the shared boot prologue (jump + OEM) and the 0x55AA signature. */
static void sd_boot_frame(uint8_t* img, const char* oem)
{
  img[0] = 0xEBU;
  img[1] = 0x58U;
  img[2] = 0x90U;
  (void)memcpy(&img[3], oem, 8U);
  img[510] = 0x55U;
  img[511] = 0xAAU;
}

/** @brief Pad an ASCII volume label to the 11-byte 8.3 field (space-filled). */
static void sd_label_field(uint8_t* dst, const char* label)
{
  bool past_end = (label == nullptr);
  for (uint32_t i = 0U; i < 11U; i++) {
    if (!past_end && (label[i] == '\0')) {
      past_end = true; /* stop reading at the NUL; pad the rest with spaces. */
    }
    dst[i] = past_end ? (uint8_t)' ' : (uint8_t)label[i];
  }
}

/** @brief Format an empty FAT16 volume; returns the chosen sectors-per-cluster. */
static uint32_t sd_format_fat16(uint8_t* img, uint32_t total_sectors, const char* label)
{
  const uint32_t root_sectors = (((uint32_t)k_fmt_root_ents * 32U) + 511U) / 512U;
  uint32_t       spc          = 1U;
  uint32_t       fatsz        = 1U;
  for (;;) {
    const uint32_t tmp1 = total_sectors - ((uint32_t)k_fmt_resv_f16 + root_sectors);
    const uint32_t tmp2 = (256U * spc) + 2U; /* 256 = 512 bytes / 2-byte FAT16 entry. */
    fatsz               = (tmp1 + tmp2 - 1U) / tmp2;
    const uint32_t data = total_sectors - (uint32_t)k_fmt_resv_f16 - (2U * fatsz) - root_sectors;
    const uint32_t clus = data / spc;
    if (clus <= (uint32_t)k_fmt_fat16_max) {
      break;
    }
    spc *= 2U;
    if (spc >= (uint32_t)k_fmt_spc_max) {
      spc = (uint32_t)k_fmt_spc_max;
      break;
    }
  }
  sd_boot_frame(img, "MSDOS5.0");
  sd_put16(&img[11], (uint16_t)k_fmt_sec_bytes);
  img[13] = (uint8_t)spc;
  sd_put16(&img[14], (uint16_t)k_fmt_resv_f16);
  img[16] = 2U; /* number of FATs */
  sd_put16(&img[17], (uint16_t)k_fmt_root_ents);
  if (total_sectors < 0x10000U) {
    sd_put16(&img[19], (uint16_t)total_sectors);
  } else {
    sd_put32(&img[32], total_sectors);
  }
  img[21] = 0xF8U; /* media: fixed disk */
  sd_put16(&img[22], (uint16_t)fatsz);
  sd_put16(&img[24], 63U);  /* sectors per track */
  sd_put16(&img[26], 255U); /* heads */
  img[36] = 0x80U;          /* drive number */
  img[38] = 0x29U;          /* extended boot signature */
  sd_put32(&img[39], (uint32_t)k_fmt_volid | total_sectors);
  sd_label_field(&img[43], label);
  (void)memcpy(&img[54], "FAT16   ", 8U);
  /* FAT[0] media + FAT[1] EOC, in both FAT copies. */
  for (uint32_t f = 0U; f < 2U; f++) {
    uint8_t* fat = &img[((uint32_t)k_fmt_resv_f16 + (f * fatsz)) * (uint32_t)k_fmt_sec_bytes];
    sd_put16(&fat[0], 0xFFF8U);
    sd_put16(&fat[2], 0xFFFFU);
  }
  return spc;
}

/** @brief Format an empty FAT32 volume; returns the chosen sectors-per-cluster. */
static uint32_t sd_format_fat32(uint8_t* img, uint32_t total_sectors, const char* label)
{
  uint32_t spc      = 1U;
  uint32_t fatsz    = 1U;
  uint32_t clusters = 0U;
  for (;;) {
    const uint32_t tmp1 = total_sectors - (uint32_t)k_fmt_resv_f32;
    const uint32_t tmp2 = (128U * spc) + 2U; /* 128 = 512 bytes / 4-byte FAT32 entry. */
    fatsz               = (tmp1 + tmp2 - 1U) / tmp2;
    const uint32_t data = total_sectors - (uint32_t)k_fmt_resv_f32 - (2U * fatsz);
    clusters            = data / spc;
    if (clusters <= 0x0FFFFFF0U) {
      break;
    }
    spc *= 2U;
    if (spc >= (uint32_t)k_fmt_spc_max) {
      spc = (uint32_t)k_fmt_spc_max;
      break;
    }
  }
  sd_boot_frame(img, "MSDOS5.0");
  sd_put16(&img[11], (uint16_t)k_fmt_sec_bytes);
  img[13] = (uint8_t)spc;
  sd_put16(&img[14], (uint16_t)k_fmt_resv_f32);
  img[16] = 2U;             /* number of FATs */
  sd_put16(&img[17], 0U);   /* root entries: 0 for FAT32 */
  sd_put16(&img[19], 0U);   /* totsec16: 0, use totsec32 */
  img[21] = 0xF8U;          /* media */
  sd_put16(&img[22], 0U);   /* fatsz16: 0 for FAT32 */
  sd_put16(&img[24], 63U);  /* sectors per track */
  sd_put16(&img[26], 255U); /* heads */
  sd_put32(&img[32], total_sectors);
  sd_put32(&img[36], fatsz); /* BPB_FATSz32 */
  sd_put32(&img[44], 2U);    /* root cluster = 2 */
  sd_put16(&img[48], 1U);    /* FSInfo sector */
  sd_put16(&img[50], 6U);    /* backup boot sector */
  img[64] = 0x80U;           /* drive number */
  img[66] = 0x29U;           /* extended boot signature */
  sd_put32(&img[67], (uint32_t)k_fmt_volid | total_sectors);
  sd_label_field(&img[71], label);
  (void)memcpy(&img[82], "FAT32   ", 8U);
  /* FSInfo sector (lead + struct + trail signatures). */
  uint8_t* fsi = &img[1U * (uint32_t)k_fmt_sec_bytes];
  sd_put32(&fsi[0], 0x41615252U);
  sd_put32(&fsi[484], 0x61417272U);
  sd_put32(&fsi[488], (clusters > 0U) ? (clusters - 1U) : 0xFFFFFFFFU); /* free clusters  */
  sd_put32(&fsi[492], 3U);                                              /* next free cluster */
  sd_put32(&fsi[508], 0xAA550000U);
  /* Backup boot copy at sector 6. */
  (void)memcpy(&img[6U * (uint32_t)k_fmt_sec_bytes], img, (size_t)k_fmt_sec_bytes);
  /* FAT[0..2]: media + EOC + root-cluster EOC, in both FAT copies. */
  for (uint32_t f = 0U; f < 2U; f++) {
    uint8_t* fat = &img[((uint32_t)k_fmt_resv_f32 + (f * fatsz)) * (uint32_t)k_fmt_sec_bytes];
    sd_put32(&fat[0], 0x0FFFFFF8U);
    sd_put32(&fat[4], 0x0FFFFFFFU);
    sd_put32(&fat[8], 0x0FFFFFFFU);
  }
  return spc;
}

bool board_sd_attach_blank(uint32_t total_sectors, uint8_t fat_bits, const char* label)
{
  if (total_sectors < 64U) {
    (void)fprintf(stderr, "board_sim: --sd-new: size too small (need >= 32 KiB)\n");
    return false;
  }
  const size_t bytes = (size_t)total_sectors * (size_t)k_fmt_sec_bytes;
  uint8_t*     img   = (uint8_t*)calloc(1U, bytes);
  if (img == nullptr) {
    (void)fprintf(stderr, "board_sim: --sd-new: out of memory for %zu bytes\n", bytes);
    return false;
  }
  const uint32_t spc = (fat_bits == 32U) ? sd_format_fat32(img, total_sectors, label)
                                         : sd_format_fat16(img, total_sectors, label);
  free(s_sd.image);
  s_sd           = (board_sd_state_t){};
  s_sd.image     = img;
  s_sd.image_len = (uint32_t)bytes;
  s_sd.attached  = true;
  s_sd.fat_bits  = (fat_bits == 32U) ? 32U : 16U;
  sd_label_field((uint8_t*)s_sd.label, label);
  s_sd.label[11] = '\0';
  (void)fprintf(stderr,
                "board_sim: SD card created (%u MiB FAT%u, %u sec/clus) blank + attached\n",
                (unsigned)(bytes / (1024U * 1024U)),
                (unsigned)s_sd.fat_bits,
                (unsigned)spc);
  return true;
}

bool board_sd_save(const char* path)
{
  if ((path == nullptr) || !s_sd.attached || (s_sd.image == nullptr)) {
    return false;
  }
  FILE* fp = fopen(path, "wb");
  if (fp == nullptr) {
    (void)fprintf(stderr, "board_sim: --save-sd: cannot write '%s'\n", path);
    return false;
  }
  const size_t put = fwrite(s_sd.image, 1U, (size_t)s_sd.image_len, fp);
  (void)fclose(fp);
  if (put != (size_t)s_sd.image_len) {
    return false;
  }
  (void)fprintf(stderr, "board_sim: SD card image saved (%u bytes) to %s\n", s_sd.image_len, path);
  return true;
}

void board_sd_info(bool* attached, uint32_t* bytes, uint8_t* fat_bits, const char** label)
{
  if (attached != nullptr) {
    *attached = s_sd.attached;
  }
  if (bytes != nullptr) {
    *bytes = s_sd.image_len;
  }
  if (fat_bits != nullptr) {
    *fat_bits = s_sd.fat_bits;
  }
  if (label != nullptr) {
    *label = s_sd.label;
  }
}

uint8_t board_sd_exchange(uint8_t tx)
{
  if (s_sd.resp_pos < s_sd.resp_len) {
    return s_sd.resp[s_sd.resp_pos++];
  }
  if (s_sd.wr_phase != k_sd_wr_idle) {
    return board_sd_write_byte(&s_sd, tx);
  }
  if (s_sd.collecting) {
    s_sd.cmd[s_sd.cmd_idx] = tx;
    s_sd.cmd_idx++;
    if (s_sd.cmd_idx >= (uint32_t)k_sd_cmd_len) {
      s_sd.collecting = false;
      board_sd_process_cmd(&s_sd);
    }
    return (uint8_t)k_sd_idle;
  }
  if ((tx & (uint8_t)k_sd_cmd_mask) == (uint8_t)k_sd_cmd_start) {
    s_sd.collecting = true;
    s_sd.cmd[0]     = tx;
    s_sd.cmd_idx    = 1U;
  }
  return (uint8_t)k_sd_idle;
}

void board_sd_reset(void)
{
  s_sd.collecting = false;
  s_sd.app_cmd    = false;
  s_sd.ready      = false;
  s_sd.cmd_idx    = 0U;
  s_sd.resp_len   = 0U;
  s_sd.resp_pos   = 0U;
  s_sd.wr_phase   = k_sd_wr_idle;
  s_sd.wr_multi   = false;
  s_sd.wr_off     = 0U;
  s_sd.wr_cnt     = 0U;
}
