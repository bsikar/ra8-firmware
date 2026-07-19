/**
 * @file board_periph_sd.c
 * @brief SD-card-over-SPI device model for board_sim (attached to SPI_B).
 *
 * @details
 * Implements a high-capacity (SDHC) SD card running in SPI mode, backed by a
 * host image file. The card answers the SD SPI command set the firmware's
 * @c ra8_sdmmc_spi driver issues during bring-up and block I/O:
 *
 *  - CMD0  GO_IDLE_STATE        -> R1 = 0x01
 *  - CMD8  SEND_IF_COND         -> R7 (R1 + 0x000001AA echo)
 *  - CMD55 + ACMD41             -> R1 = 0x00 (card ready)
 *  - CMD58 READ_OCR             -> R3 (R1 + OCR with CCS=1 = SDHC)
 *  - CMD9  SEND_CSD             -> R1 + 0xFE + 16-byte CSD v2.0 + CRC16
 *  - CMD17 READ_SINGLE_BLOCK    -> R1 + 0xFE + 512 data bytes + CRC16
 *  - CMD18 READ_MULTIPLE_BLOCK  -> as CMD17, then keeps streaming successive
 *                                  blocks until CMD12 STOP_TRANSMISSION
 *  - CMD12 STOP_TRANSMISSION    -> stuff byte + R1 (ends a CMD18 stream)
 *
 * Commands self-frame off the @c 01xxxxxx lead bits, so the model needs no
 * chip-select wiring. The command-framing approach is verified end to end
 * against the real driver by the in-file seed model of
 * @c tests/test_ra8_sdmmc_card_reflow.c (single-block reads); the CMD18/CMD12
 * stream and the write paths are exercised against the same wire framing by
 * the driver's own host tests (@c tests/test_ra8_sdmmc_spi.c).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "board_periph_sd.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "board_console.h"
#include "board_periph_sd_internal.h"

/** @brief Console-tap sizing for SD-over-SPI block summaries. */
typedef enum : uint32_t {
  k_sd_console_line_cap = 48U, /**< Max chars in an "SDSPI .. .." line.         */
  k_sd_console_rd_every = 16U, /**< Push 1 read line per N blocks (anti-flood). */
} sd_console_t;

/**
 * @var s_sd_spi_block_reads
 * @brief Count of SD-over-SPI data blocks staged for read this run.
 * @details Drives the console SD tab's read-tap rate limit so a book-sized
 *          multi-block read does not flood the ring (one line per N blocks).
 * @note Not thread-safe; board_sim is single-threaded.
 * @since 0.1.0
 */
static uint32_t s_sd_spi_block_reads;

/**
 * @enum board_sd_const_t
 * @brief SD SPI-mode protocol and sizing constants (no magic numbers).
 */
typedef enum : uint16_t {
  k_sd_block         = 512U,    /**< Bytes per SD block.                      */
  k_sd_cmd_len       = 6U,      /**< Command frame: opcode + 4 arg + crc.     */
  k_sd_resp_cap      = 520U,    /**< R1 + token + 512 data + 2 CRC.           */
  k_sd_cmd_start     = 0x40U,   /**< SPI command lead bits (01xxxxxx).        */
  k_sd_cmd_mask      = 0xC0U,   /**< Mask isolating the lead bits.            */
  k_sd_idx_mask      = 0x3FU,   /**< Command index in the lead byte.          */
  k_sd_idle          = 0xFFU,   /**< Bus idle / CIPO-high byte.               */
  k_sd_tok_data      = 0xFEU,   /**< Single-block read / write data token.    */
  k_sd_tok_wmulti    = 0xFCU,   /**< Multi-block write data-start token.      */
  k_sd_tok_stop      = 0xFDU,   /**< Multi-block write stop-tran token.       */
  k_sd_data_accept   = 0x05U,   /**< Data-response token: block accepted.     */
  k_sd_busy          = 0x00U,   /**< CIPO held low while the card programs.   */
  k_sd_crc_len       = 2U,      /**< Trailing CRC16 bytes after a data block. */
  k_sd_label_cap     = 12U,     /**< Volume-label field: 11 chars + NUL.      */
  k_sd_r1_idle       = 0x01U,   /**< R1 with IDLE set.                        */
  k_sd_r1_ready      = 0x00U,   /**< R1, card ready.                          */
  k_sd_byte_mask     = 0xFFU,   /**< Low byte mask.                           */
  k_sd_byte_bits     = 8U,      /**< Bits per byte (shift amount).            */
  k_sd_r7_len        = 5U,      /**< R1 + 4-byte tail.                        */
  k_sd_arg_sh0       = 24U,     /**< Arg byte 0 (MSB) shift.                  */
  k_sd_arg_sh1       = 16U,     /**< Arg byte 1 shift.                        */
  k_sd_cmd8_echo     = 0xAAU,   /**< CMD8 check pattern echo.                 */
  k_sd_ocr_pwrccs    = 0xC0U,   /**< OCR byte 0: power-up done + CCS.         */
  k_sd_ocr_volt      = 0x80U,   /**< OCR byte 2: voltage window.              */
  k_sd_csd_v2        = 0x40U,   /**< CSD_STRUCTURE = 01b (v2.0 / SDHC).       */
  k_sd_csd_csize     = 0x0FU,   /**< C_SIZE byte (=> 8 MiB modelled card).    */
  k_sd_csd_len       = 16U,     /**< CSD register length.                     */
  k_sd_csd_csize_b7  = 7U,      /**< C_SIZE high byte offset in the CSD.      */
  k_sd_csd_csize_b8  = 8U,      /**< C_SIZE mid byte offset in the CSD.       */
  k_sd_csd_off       = 9U,      /**< C_SIZE LSB byte offset in the CSD.       */
  k_sd_csize_b7_mask = 0x3FU,   /**< C_SIZE field spans only 6 bits in b7.    */
  k_sd_crc_poly      = 0x1021U, /**< CRC16-CCITT polynomial.                  */
  k_sd_crc_msb       = 0x8000U, /**< Top bit of the 16-bit CRC register.      */
} board_sd_const_t;

/** @brief SD sizing constants that exceed 16 bits. */
typedef enum : uint32_t {
  k_sd_csd_unit       = 512U * 1024U,               /**< CSD v2.0 C_SIZE unit: 512 KiB.     */
  k_sd_save_max_bytes = 2U * 1024U * 1024U * 1024U, /**< --save-sd cap: 2 GiB.              */
  k_unit_kib          = 1024U,                      /**< Binary kilo multiplier (KiB).      */
  k_sd_min_sectors    = 64U,                        /**< --sd-new floor: 32 KiB of sectors. */
} board_sd_const32_t;

/**
 * @enum board_sd_cmd_idx_t
 * @brief SD SPI command indices answered by the model (low 6 bits of byte 0).
 */
typedef enum : uint8_t {
  k_sd_idx_cmd0   = 0U,  /**< GO_IDLE_STATE.      */
  k_sd_idx_cmd8   = 8U,  /**< SEND_IF_COND.       */
  k_sd_idx_cmd9   = 9U,  /**< SEND_CSD.           */
  k_sd_idx_cmd12  = 12U, /**< STOP_TRANSMISSION.  */
  k_sd_idx_cmd16  = 16U, /**< SET_BLOCKLEN.       */
  k_sd_idx_cmd17  = 17U, /**< READ_SINGLE.        */
  k_sd_idx_cmd18  = 18U, /**< READ_MULTIPLE.      */
  k_sd_idx_cmd24  = 24U, /**< WRITE_SINGLE.       */
  k_sd_idx_cmd25  = 25U, /**< WRITE_MULTIPLE.     */
  k_sd_idx_cmd32  = 32U, /**< ERASE_WR_BLK_START. */
  k_sd_idx_cmd33  = 33U, /**< ERASE_WR_BLK_END.   */
  k_sd_idx_cmd38  = 38U, /**< ERASE.              */
  k_sd_idx_acmd41 = 41U, /**< SD_SEND_OP_COND.    */
  k_sd_idx_cmd55  = 55U, /**< APP_CMD.            */
  k_sd_idx_cmd58  = 58U, /**< READ_OCR.           */
} board_sd_cmd_idx_t;

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
  uint8_t*            image;                 /**< Backing card image (malloc or mmap-sparse).  */
  uint64_t            image_len;             /**< Image size in bytes (64-bit: cards > 4 GB).  */
  bool                attached;              /**< A `--sd` image is loaded.                    */
  bool                mmapped;               /**< image is a sparse mmap (--sd-new) vs malloc. */
  int                 map_fd;                /**< Backing temp-file fd when mmapped, else -1.  */
  uint8_t             fat_bits;              /**< 12/16/32 if formatted by --sd-new, else 0.   */
  char                label[k_sd_label_cap]; /**< Volume label (11 + NUL), for GUI.            */
  bool                collecting;            /**< Mid command-frame collection.                */
  bool                app_cmd;               /**< Previous command was CMD55 (APP_CMD).        */
  bool                ready;                 /**< ACMD41 has completed.                        */
  uint8_t             cmd[k_sd_cmd_len];     /**< Cmd.                                         */
  uint32_t            cmd_idx;               /**< Cmd index.                                   */
  uint8_t             resp[k_sd_resp_cap];   /**< Resp.                                        */
  uint32_t            resp_len;              /**< Resp length.                                 */
  uint32_t            resp_pos;              /**< Resp pos.                                    */
  board_sd_wr_phase_t wr_phase;              /**< CMD24/CMD25 write sub-state.                 */
  bool                wr_multi;              /**< Write is CMD25 (multi-block).                */
  uint64_t            wr_off;                /**< Byte offset of the current block.            */
  uint32_t            wr_cnt;                /**< Bytes seen in the data/CRC phase.            */
  bool                rd_multi;              /**< An open CMD18 read stream.                   */
  uint64_t            rd_off;                /**< Byte offset of the next streamed
                                    *   CMD18 block.                      */
  uint32_t            erase_start;           /**< CMD32 ERASE_WR_BLK_START block. */
  uint32_t            erase_end;             /**< CMD33 ERASE_WR_BLK_END block.   */
} board_sd_state_t;

/** @brief The single modelled SD card. */
static board_sd_state_t s_sd;

/** @brief Release the current backing image (munmap a sparse card, else free). */
static void board_sd_release_image(void)
{
  if (s_sd.image != nullptr) {
    if (s_sd.mmapped) {
      (void)munmap(s_sd.image, (size_t)s_sd.image_len);
      if (s_sd.map_fd >= 0) {
        (void)close(s_sd.map_fd);
      }
    } else {
      free(s_sd.image);
    }
  }
  s_sd.image   = nullptr;
  s_sd.mmapped = false;
  s_sd.map_fd  = -1;
}

/**
 * @brief CRC16-CCITT (poly 0x1021, init 0) over a buffer.
 *
 * @details Matches the firmware driver's @c ra8_sdmmc_spi_crc16 so the card's
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
 * @brief Copy one 512-byte block out of the backing image (zero past the end).
 *
 * @details The shared payload fetch for CMD17, CMD18, and the CMD18 stream
 * continuation: bytes beyond the image end read back as zero, matching the
 * public @ref board_sd_read_block accessor.
 *
 * @param[in]  c   Card state carrying the backing image.
 * @param[in]  off Byte offset of the block inside the image.
 * @param[out] blk Destination for exactly 512 bytes.
 * @return None.
 * @pre `c` and `blk` are non-null.
 * @pre A backing image is attached (or every byte reads back zero).
 * @post `blk` holds the image bytes at `off`, zero-filled past the image end.
 * @post No card state is modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void board_sd_fill_block(const board_sd_state_t* c, uint64_t off, uint8_t* blk)
{
  for (uint32_t i = 0U; i < (uint32_t)k_sd_block; ++i) {
    const uint64_t a = off + (uint64_t)i;
    blk[i]           = (a < c->image_len) ? c->image[a] : 0U;
  }
}

/**
 * @brief Stage the R1 + data-token + payload + CRC16 reply for a block read.
 *
 * @param[in,out] c       Card state.
 * @param[in]      payload Source bytes.
 * @param[in]      len     Payload length (16 for CSD, 512 for a data block).
 * @return None.
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
  /* Console SD tab: anti-flood -- the first block and then every Nth, so a
   * book-sized multi-block read shows activity without swamping the ring. */
  s_sd_spi_block_reads++;
  bool sd_report = (s_sd_spi_block_reads == 1U);
  if ((s_sd_spi_block_reads % (uint32_t)k_sd_console_rd_every) == 0U) {
    sd_report = true;
  }
  if (sd_report) {
    char ln[k_sd_console_line_cap];
    (void)
      snprintf(ln, sizeof(ln), "SDSPI rd %uB (#%u)", (unsigned)len, (unsigned)s_sd_spi_block_reads);
    board_console_push(k_board_console_ch_sd, ln);
  }
}

/**
 * @brief Accept a CMD17/CMD18 block-read command and stage the first block.
 *
 * @details Stages the R1 + data-token + payload + CRC16 reply for the block at
 * @p arg (SDHC addresses by block). A CMD18 additionally opens the multi-block
 * read stream: ::board_sd_exchange keeps staging successive blocks (token +
 * payload + CRC, no R1) until CMD12 STOP_TRANSMISSION closes it.
 *
 * @param[in,out] c   Card state.
 * @param[in]     idx Command index (CMD17 single or CMD18 multi).
 * @param[in]     arg Block address argument.
 * @return None.
 * @pre `c` is non-null and a card image is attached.
 * @pre `idx` is ::k_sd_idx_cmd17 or ::k_sd_idx_cmd18.
 * @post `c->resp` holds the first block reply; `c->rd_off` points past it.
 * @post `c->rd_multi` is true iff the command was CMD18.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void board_sd_begin_read(board_sd_state_t* c, uint8_t idx, uint32_t arg)
{
  uint8_t        blk[k_sd_block] = {};
  const uint64_t off             = (uint64_t)arg * (uint64_t)k_sd_block;
  board_sd_fill_block(c, off, blk);
  board_sd_stage_block(c, blk, (uint32_t)k_sd_block);
  c->rd_multi = (idx == (uint8_t)k_sd_idx_cmd18);
  c->rd_off   = off + (uint64_t)k_sd_block;
}

/**
 * @brief Stage the next block of an open CMD18 stream (token + payload + CRC).
 *
 * @details Mid-stream blocks carry no R1 -- the card just emits the next
 * data-start token once the block is ready. Reuses ::board_sd_stage_block for
 * the framing and skips its leading R1 slot by starting the drain at the
 * token. Advances the stream offset by one block.
 *
 * @param[in,out] c Card state with an open CMD18 stream.
 * @return None.
 * @pre `c->rd_multi` is true (a CMD18 stream is open).
 * @pre A card image is attached (reads past its end yield zeros).
 * @post `c->resp` holds token + payload + CRC with `c->resp_pos` at the token.
 * @post `c->rd_off` advanced by one block.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void board_sd_read_stream_next(board_sd_state_t* c)
{
  uint8_t blk[k_sd_block] = {};
  board_sd_fill_block(c, c->rd_off, blk);
  board_sd_stage_block(c, blk, (uint32_t)k_sd_block);
  c->resp_pos = 1U; /* skip the R1 slot: mid-stream blocks carry none. */
  c->rd_off += (uint64_t)k_sd_block;
}

/**
 * @brief Answer CMD12 STOP_TRANSMISSION and close any open CMD18 stream.
 *
 * @details Stages the CMD12 reply the ra8_sdmmc_spi driver expects: one stuff
 * byte (the undefined tail of the cut-off stream, which the driver discards
 * before polling R1), the R1 status, one busy byte, then not-busy. Real cards
 * hold CIPO low briefly after CMD12; a single busy byte models that.
 *
 * @param[in,out] c  Card state.
 * @param[in]     r1 R1 status byte to return (ready/idle).
 * @return None.
 * @pre `c` is non-null.
 * @pre `c->resp_pos` / `c->resp_len` were reset by the caller.
 * @post `c->rd_multi` is false (any open stream is closed).
 * @post `c->resp` holds stuff byte + R1 + busy + not-busy.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void board_sd_stop_read(board_sd_state_t* c, uint8_t r1)
{
  c->rd_multi = false;
  c->resp[0]  = (uint8_t)k_sd_idle; /* stuff byte (tail of the cut stream). */
  c->resp[1]  = r1;
  c->resp[2]  = (uint8_t)k_sd_busy;
  c->resp[3]  = (uint8_t)k_sd_idle;
  c->resp_len = 4U;
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
  c->wr_off   = (uint64_t)arg * (uint64_t)k_sd_block;
  c->wr_cnt   = 0U;
  /* Console SD tab: one line per SD-over-SPI block-write command. */
  char ln[k_sd_console_line_cap];
  (void)snprintf(ln, sizeof(ln), "SDSPI wr blk=%u", (unsigned)arg);
  board_console_push(k_board_console_ch_sd, ln);
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
    const uint64_t a = c->wr_off + (uint64_t)c->wr_cnt;
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
      c->wr_off += (uint64_t)k_sd_block; /* next block of the multi-write. */
      c->wr_phase = k_sd_wr_token;
    } else {
      c->wr_phase = k_sd_wr_idle;
    }
  }
  return (uint8_t)k_sd_idle;
}

/**
 * @brief Stage the 16-byte CSD v2.0 data block for a CMD9 (SEND_CSD) reply.
 *
 * @details
 * Builds a CSD version 2.0 register whose capacity field (C_SIZE, the 22-bit
 * value in bytes 7..9) is derived from the attached image so `--sd` / `--sd-new`
 * present their real size to the firmware instead of a fixed 8 MiB. CSD v2.0
 * capacity is `(C_SIZE + 1) * 512 KiB`; a card smaller than one 512 KiB unit
 * falls back to the fixed `k_sd_csd_csize`. Hands the block to
 * `board_sd_stage_block()` for streaming. Extracted verbatim from the CMD9
 * arm of `board_sd_process_cmd()`.
 *
 * @param[in,out] c Card state whose response buffer receives the CSD block.
 * @return None.
 * @pre `c` is non-null with a staging response buffer.
 * @pre `c->image_len` reflects the attached card size.
 * @post `c->resp` / `c->resp_len` hold the staged 16-byte CSD block.
 * @post The backing image is not modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void board_sd_cmd_send_csd(board_sd_state_t* c)
{
  uint8_t csd[k_sd_csd_len];
  memset(csd, 0, sizeof(csd));
  csd[0]         = (uint8_t)k_sd_csd_v2;
  uint32_t csize = (uint32_t)k_sd_csd_csize; /* fallback: 8 MiB. */
  if (c->image_len >= (uint32_t)k_sd_csd_unit) {
    csize = (c->image_len / (uint32_t)k_sd_csd_unit) - 1U;
  }
  csd[k_sd_csd_csize_b7] =
    (uint8_t)((csize >> (uint32_t)k_sd_arg_sh1) & (uint32_t)k_sd_csize_b7_mask);
  csd[k_sd_csd_csize_b8] =
    (uint8_t)((csize >> (uint32_t)k_sd_byte_bits) & (uint16_t)k_sd_byte_mask);
  csd[k_sd_csd_off] = (uint8_t)(csize & (uint16_t)k_sd_byte_mask);
  board_sd_stage_block(c, csd, (uint32_t)k_sd_csd_len);
}

/**
 * @brief Handle the identification / configuration commands (CMD0/8/55/58/16).
 *
 * @details
 * Builds the fixed R1/R3/R7 responses for the card-identification and block-len
 * commands. Returns whether `idx` was one of these so the caller can fall
 * through to the data-transfer dispatch otherwise. Extracted verbatim from the
 * corresponding arms of `board_sd_process_cmd()`.
 *
 * @param[in,out] c   Card state whose response buffer is populated.
 * @param[in]     idx 6-bit command index already masked from the frame.
 * @param[in]     r1  Pre-computed R1 status byte (idle vs ready).
 * @return Whether the command was recognised and handled here.
 * @retval true  `idx` was an identification/config command; `c->resp` is set.
 * @retval false `idx` is not handled here; caller must dispatch elsewhere.
 * @pre `c` is non-null with a staging response buffer.
 * @pre `idx` is the masked command index of a complete frame.
 * @post On true, `c->resp` / `c->resp_len` describe the reply.
 * @post On false, `c->resp` / `c->resp_len` are left unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool board_sd_dispatch_ident(board_sd_state_t* c, uint8_t idx, uint8_t r1)
{
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
    default:
      return false;
  }
  return true;
}

/**
 * @brief Handle the data-transfer and erase commands (CMD9/17/18/12/24/25/32/33/38).
 *
 * @details
 * Dispatches the block read/write, stop, and erase commands, plus the CMD9 CSD
 * reply (via `board_sd_cmd_send_csd()`). Any unrecognised index falls to the
 * default R1 reply. Extracted verbatim from the corresponding arms of
 * `board_sd_process_cmd()`; called only after `board_sd_dispatch_ident()`
 * declines the frame.
 *
 * @param[in,out] c   Card state whose response / stream is updated.
 * @param[in]     idx 6-bit command index already masked from the frame.
 * @param[in]     arg 32-bit big-endian argument decoded from the frame.
 * @param[in]     r1  Pre-computed R1 status byte (idle vs ready).
 * @return None.
 * @pre `c` is non-null with a staging response buffer.
 * @pre `board_sd_dispatch_ident()` already declined `idx`.
 * @post `c->resp` / `c->resp_len` (and any read/write stream state) describe the reply.
 * @post The backing image is modified only for the erase (CMD38) command.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void board_sd_dispatch_data(board_sd_state_t* c, uint8_t idx, uint32_t arg, uint8_t r1)
{
  switch (idx) {
    case (uint8_t)k_sd_idx_cmd9: /* SEND_CSD: 16-byte CSD v2.0 data block. */
      board_sd_cmd_send_csd(c);
      break;
    case (uint8_t)k_sd_idx_cmd17: /* READ_SINGLE_BLOCK   (SDHC: arg = block). */
    case (uint8_t)k_sd_idx_cmd18: /* READ_MULTIPLE_BLOCK -- streams to CMD12. */
      board_sd_begin_read(c, idx, arg);
      break;
    case (uint8_t)k_sd_idx_cmd12: /* STOP_TRANSMISSION: end a CMD18 stream. */
      board_sd_stop_read(c, r1);
      break;
    case (uint8_t)k_sd_idx_cmd24: /* WRITE_SINGLE_BLOCK   */
    case (uint8_t)k_sd_idx_cmd25: /* WRITE_MULTIPLE_BLOCK */
      board_sd_begin_write(c, idx, arg, r1);
      break;
    case (uint8_t)k_sd_idx_cmd32: /* ERASE_WR_BLK_START (SDHC: arg = block). */
      c->erase_start = arg;
      c->resp[0]     = r1;
      c->resp_len    = 1U;
      break;
    case (uint8_t)k_sd_idx_cmd33: /* ERASE_WR_BLK_END (SDHC: arg = block). */
      c->erase_end = arg;
      c->resp[0]   = r1;
      c->resp_len  = 1U;
      break;
    case (uint8_t)k_sd_idx_cmd38: { /* ERASE: zero [start, end] -- model a zero-erase card. */
      if (c->erase_end >= c->erase_start) {
        const uint64_t lo  = (uint64_t)c->erase_start * (uint64_t)k_sd_block;
        const uint64_t hi  = ((uint64_t)c->erase_end + 1U) * (uint64_t)k_sd_block;
        const uint64_t end = (hi < c->image_len) ? hi : c->image_len;
        if (lo < end) {
          memset(&c->image[lo], 0, (size_t)(end - lo));
        }
      }
      c->resp[0]  = r1;
      c->resp_len = 1U;
      break;
    }
    default:
      c->resp[0]  = r1;
      c->resp_len = 1U;
      break;
  }
}

/**
 * @brief Build the response for a completed 6-byte command frame.
 *
 * @param[in,out] c Card state holding the collected command.
 * @return None.
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
  if (board_sd_dispatch_ident(c, idx, r1)) {
    return;
  }
  board_sd_dispatch_data(c, idx, arg, r1);
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
  board_sd_release_image();
  s_sd        = (board_sd_state_t){};
  s_sd.map_fd = -1;
  s_sd.image  = (uint8_t*)malloc((size_t)size);
  if (s_sd.image == nullptr) {
    (void)fclose(fp);
    return false;
  }
  const size_t got = fread(s_sd.image, 1U, (size_t)size, fp);
  (void)fclose(fp);
  if (got != (size_t)size) {
    board_sd_release_image();
    return false;
  }
  s_sd.image_len = (uint64_t)size;
  s_sd.attached  = true;
  (void)fprintf(stderr, "board_sim: SD card attached (%ld bytes) from %s\n", size, path);
  return true;
}

bool board_sd_attached(void)
{
  return s_sd.attached;
}

/**
 * @brief Allocate a sparse, anonymous mmap-backed image buffer for a blank card.
 *
 * @details
 * Backs the card with a sparse mmap'd temp file so a multi-GB card only ever
 * materialises the few sectors the formatter + firmware actually touch (e.g. a
 * 30 GB card costs kilobytes of host RAM, not 30 GB). Creates an anonymous temp
 * file with `mkstemp` + `unlink`, sizes it with `ftruncate`, then maps it
 * read/write and shared. On any failure it emits the same diagnostic the caller
 * used to emit inline, closes the descriptor if one was opened, and returns
 * `nullptr`. Extracted verbatim from `board_sd_attach_blank()`.
 *
 * @param[in]  bytes  Card size in bytes to reserve for the mapping.
 * @param[out] out_fd Receives the backing file descriptor on success only.
 * @return Pointer to the mapped image buffer, or `nullptr` on failure.
 * @retval nullptr mkstemp, ftruncate, or mmap failed (descriptor already closed).
 * @retval non-null Mapped buffer of `bytes`; `*out_fd` holds its live descriptor.
 * @pre `out_fd` is non-null.
 * @pre `bytes` is non-zero.
 * @post On success `*out_fd` is an open descriptor owning the mapping's storage.
 * @post On failure no descriptor leaks and `*out_fd` is left unmodified.
 * @note Not thread-safe; intended for single-threaded card setup.
 * @since 0.1.0
 */
static uint8_t* board_sd_map_blank_image(uint64_t bytes, int* out_fd)
{
  char tmpl[] = "/tmp/board_sim_sd.XXXXXX";
  int  fd     = mkstemp(tmpl);
  if (fd < 0) {
    (void)fprintf(stderr, "board_sim: --sd-new: mkstemp failed\n");
    return nullptr;
  }
  (void)unlink(tmpl); /* anonymous: the storage lives until the fd is closed. */
  if (ftruncate(fd, (off_t)bytes) != 0) {
    (void)close(fd);
    (void)fprintf(stderr,
                  "board_sim: --sd-new: ftruncate to %llu bytes failed\n",
                  (unsigned long long)bytes);
    return nullptr;
  }
  uint8_t* img = (uint8_t*)mmap(nullptr, (size_t)bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (img == MAP_FAILED) {
    (void)close(fd);
    (void)fprintf(stderr,
                  "board_sim: --sd-new: mmap of %llu bytes failed\n",
                  (unsigned long long)bytes);
    return nullptr;
  }
  *out_fd = fd;
  return img;
}

/**
 * @brief Emit the "SD card created" diagnostic for a freshly formatted card.
 *
 * @details
 * Prints the card geometry to stderr in GiB when the card is at least 1 GiB and
 * in MiB otherwise, matching the two-branch message the caller used to emit
 * inline. Extracted verbatim from `board_sd_attach_blank()`.
 *
 * @param[in] bytes    Card size in bytes.
 * @param[in] fat_bits FAT width just applied (16 or 32).
 * @param[in] spc      Sectors-per-cluster chosen by the formatter.
 * @return None.
 * @pre `bytes` is the size of an already-attached card.
 * @pre `spc` is the formatter's returned sectors-per-cluster.
 * @post Exactly one diagnostic line is written to stderr.
 * @post No program state other than the stderr stream is modified.
 * @note Not thread-safe; writes to the shared stderr stream.
 * @since 0.1.0
 */
static void board_sd_report_created(uint64_t bytes, uint8_t fat_bits, uint32_t spc)
{
  if (bytes >= ((uint64_t)k_unit_kib * (uint64_t)k_unit_kib * (uint64_t)k_unit_kib)) {
    (void)fprintf(stderr,
                  "board_sim: SD card created (%llu GiB FAT%u, %u sec/clus) sparse + attached\n",
                  (unsigned long long)(bytes / ((uint64_t)k_unit_kib * (uint64_t)k_unit_kib *
                                                (uint64_t)k_unit_kib)),
                  (unsigned)fat_bits,
                  (unsigned)spc);
  } else {
    (void)fprintf(stderr,
                  "board_sim: SD card created (%llu MiB FAT%u, %u sec/clus) sparse + attached\n",
                  (unsigned long long)(bytes / ((uint64_t)k_unit_kib * (uint64_t)k_unit_kib)),
                  (unsigned)fat_bits,
                  (unsigned)spc);
  }
}

bool board_sd_attach_blank(uint32_t total_sectors, uint8_t fat_bits, const char* label)
{
  if (total_sectors < (uint32_t)k_sd_min_sectors) {
    (void)fprintf(stderr, "board_sim: --sd-new: size too small (need >= 32 KiB)\n");
    return false;
  }
  const uint64_t bytes = (uint64_t)total_sectors * (uint64_t)k_fmt_sec_bytes;
  int            fd    = -1;
  uint8_t*       img   = board_sd_map_blank_image(bytes, &fd);
  if (img == nullptr) {
    return false;
  }
  const uint32_t spc = (fat_bits == (uint8_t)k_fat32_bits)
                         ? sd_format_fat32(img, total_sectors, label)
                         : sd_format_fat16(img, total_sectors, label);
  board_sd_release_image();
  s_sd           = (board_sd_state_t){};
  s_sd.image     = img;
  s_sd.image_len = bytes;
  s_sd.mmapped   = true;
  s_sd.map_fd    = fd;
  s_sd.attached  = true;
  s_sd.fat_bits =
    (fat_bits == (uint8_t)k_fat32_bits) ? (uint8_t)k_fat32_bits : (uint8_t)k_fat16_bits;
  sd_label_field((uint8_t*)s_sd.label, label);
  s_sd.label[k_fmt_label_len] = '\0';
  board_sd_report_created(bytes, s_sd.fat_bits, spc);
  return true;
}

bool board_sd_save(const char* path)
{
  if ((path == nullptr) || !s_sd.attached || (s_sd.image == nullptr)) {
    return false;
  }
  /* A sparse multi-GB card would dump GBs of mostly-zeros; cap the dump so
   * --save-sd stays sane. Inspect a large card by its live mount instead. */
  if (s_sd.image_len > (uint64_t)k_sd_save_max_bytes) {
    (void)fprintf(
      stderr,
      "board_sim: --save-sd: card is %llu MiB (> %u MiB cap) -- skipped\n",
      (unsigned long long)(s_sd.image_len / ((uint64_t)k_unit_kib * (uint64_t)k_unit_kib)),
      (unsigned)((uint64_t)k_sd_save_max_bytes / ((uint64_t)k_unit_kib * (uint64_t)k_unit_kib)));
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
  (void)fprintf(stderr,
                "board_sim: SD card image saved (%llu bytes) to %s\n",
                (unsigned long long)s_sd.image_len,
                path);
  return true;
}

void board_sd_info(bool* attached, uint64_t* bytes, uint8_t* fat_bits, const char** label)
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
    return (uint8_t)k_sd_idle;
  }
  if (s_sd.rd_multi) {
    /* Open CMD18 stream: the host clocking idle is fetching the next block.
     * The command-start check above stays first so a CMD12 frame interrupts
     * the stream instead of being swallowed as read clocking. */
    board_sd_read_stream_next(&s_sd);
    return s_sd.resp[s_sd.resp_pos++];
  }
  return (uint8_t)k_sd_idle;
}

bool board_sd_read_block(uint32_t lba, uint8_t* dst)
{
  if (!s_sd.attached || (s_sd.image == nullptr) || (dst == nullptr)) {
    return false;
  }
  /* Same SDHC block-addressing as the CMD17 path: arg is a block index. */
  const uint64_t off = (uint64_t)lba * (uint64_t)k_sd_block;
  if (off >= s_sd.image_len) {
    return false; /* past the end of the card -- let the real driver error out. */
  }
  board_sd_fill_block(&s_sd, off, dst);
  return true;
}

bool board_sd_write_block(uint32_t lba, const uint8_t* src)
{
  if (!s_sd.attached || (s_sd.image == nullptr) || (src == nullptr)) {
    return false;
  }
  /* Same SDHC block-addressing as the CMD24 path: arg is a block index. */
  const uint64_t off = (uint64_t)lba * (uint64_t)k_sd_block;
  if (off >= s_sd.image_len) {
    return false; /* past the end of the card -- let the real driver error out. */
  }
  if ((off + (uint64_t)k_sd_block) > s_sd.image_len) {
    return false; /* a full block would run off the end of the image. */
  }
  for (uint32_t i = 0U; i < (uint32_t)k_sd_block; ++i) {
    s_sd.image[off + (uint64_t)i] = src[i];
  }
  return true;
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
  s_sd.rd_multi   = false;
  s_sd.rd_off     = 0U;
}
