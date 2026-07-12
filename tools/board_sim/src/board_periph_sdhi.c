/**
 * @file board_periph_sdhi.c
 * @brief Native 4-bit SDHI host-controller model for board_sim (SDHI0).
 *
 * @details
 * Models the RA8D2 SDHI0 host controller (ra8_sdhi_regs.h, ra8_sdhi.c,
 * ra8_sdcard.c) at @c 0x40252000 just enough that the firmware's real native-SD
 * path runs headless: the SD Physical Layer identification
 * (CMD0 -> CMD8 -> ACMD41 -> CMD2 -> CMD3 -> CMD9 -> CMD7) plus the polled
 * 512-byte block read / write over the SD_BUF0 FIFO. Without it the
 * identification sequence times out (no @c SD_INFO1.RSPEND) so any native-SDHI
 * app parks at "card init".
 *
 * The model keeps a register shadow (read returns the shadow except for the
 * live RSPEND / BRE / BWE status bits it computes) plus a tiny command engine:
 *
 *   - A @c SD_CMD write latches the command (index = SD_CMD & 0x3F) and the
 *     pending @c SD_ARG, executes it, drops a response into SD_RSP10/32/54/76,
 *     and SETS @c SD_INFO1.RSPEND (bit 0) so ra8_sdhi_send_command's poll exits.
 *   - CMD17/18 arm a read data phase; CMD24/25 arm a write data phase. Each
 *     @c SD_BUF0 access serves / captures one 32-bit word of a 512-byte block;
 *     after 128 words a multi-block transfer advances to the next LBA and keeps
 *     the BRE / BWE flag set, else clears it.
 *
 * The SD image is shared with board_periph_sd.c (the same @c --sd / @c --sd-new
 * card): block I/O calls @ref board_sd_read_block / @ref board_sd_write_block,
 * and the CMD9 CSD encodes the card's real capacity via @ref board_sd_info.
 *
 * Window geometry is deliberately narrow -- only SDHI0's own 0x200-byte register
 * page is claimed (SDHI registers are SDHI-only, so no clock-setup register that
 * ra8_cgc_init polls is intercepted).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_console.h"
#include "board_periph_block.h"
#include "board_periph_sd.h"

/** @brief Console-tap line buffer capacity for an SDHI block summary. */
typedef enum : uint32_t {
  k_sdhi_console_line_cap = 48U, /**< Max chars in an "SD rd lba=.." line. */
} sdhi_console_t;

/** @brief SDHI0 register-window geometry + the offsets the model owns. */
typedef enum : uint64_t {
  k_sdhi_base          = 0x40252000UL, /**< SDHI0 register window base.      */
  k_sdhi_span          = 0x200UL,      /**< Narrow window: SD_CMD..SOFT_RST. */
  k_sdhi_off_sd_cmd    = 0x000UL,      /**< SD_CMD  : command type / issue.  */
  k_sdhi_off_sd_arg    = 0x008UL,      /**< SD_ARG  : command argument.      */
  k_sdhi_off_sd_stop   = 0x010UL,      /**< SD_STOP : data stop.             */
  k_sdhi_off_sd_seccnt = 0x014UL,      /**< SD_SECCNT : multi-block count.   */
  k_sdhi_off_sd_rsp10  = 0x018UL,      /**< SD_RSP10 : response word 0.      */
  k_sdhi_off_sd_rsp32  = 0x020UL,      /**< SD_RSP32 : response word 1.      */
  k_sdhi_off_sd_rsp54  = 0x028UL,      /**< SD_RSP54 : response word 2.      */
  k_sdhi_off_sd_rsp76  = 0x030UL,      /**< SD_RSP76 : response word 3.      */
  k_sdhi_off_sd_info1  = 0x038UL,      /**< SD_INFO1 : interrupt flag 1.     */
  k_sdhi_off_sd_info2  = 0x03CUL,      /**< SD_INFO2 : interrupt flag 2.     */
  k_sdhi_off_sd_option = 0x050UL,      /**< SD_OPTION : bus width + timeout. */
  k_sdhi_off_sd_buf0   = 0x060UL,      /**< SD_BUF0 : 32-bit FIFO word.      */
  k_sdhi_off_soft_rst  = 0x1C0UL,      /**< SOFT_RST : software reset.       */
} sdhi_reg_map_t;

/** @brief SD_INFO1 / SD_INFO2 status bits the firmware polls. */
typedef enum : uint32_t {
  k_sdhi_info1_rspend = 0x00000001U, /**< SD_INFO1.RSPEND (response received). */
  k_sdhi_info2_bre    = 0x00000100U, /**< SD_INFO2.BRE (read buffer ready).    */
  k_sdhi_info2_bwe    = 0x00000200U, /**< SD_INFO2.BWE (write buffer space).   */
} sdhi_status_t;

/** @brief SD_OPTION bus-width selector bits + decoded lane counts. */
typedef enum : uint32_t {
  k_sdhi_option_width_bit  = 0x00008000U, /**< SD_OPTION.WIDTH bit 15: 1 -> 1-bit.  */
  k_sdhi_option_width8_bit = 0x00002000U, /**< SD_OPTION.WIDTH8 bit 13: 1 -> 8-bit. */
  k_sdhi_lanes_1bit        = 1U,          /**< Single data lane.                    */
  k_sdhi_lanes_4bit        = 4U,          /**< Four data lanes.                     */
  k_sdhi_lanes_8bit        = 8U,          /**< Eight data lanes.                    */
} sdhi_option_bits_t;

/** @brief SD command indices the model decodes (index = SD_CMD & 0x3F). */
typedef enum : uint32_t {
  k_sdhi_cmd0_go_idle   = 0U,  /**< CMD0  GO_IDLE_STATE.        */
  k_sdhi_cmd2_send_cid  = 2U,  /**< CMD2  ALL_SEND_CID (R2).    */
  k_sdhi_cmd3_send_rca  = 3U,  /**< CMD3  SEND_RELATIVE_ADDR.   */
  k_sdhi_cmd7_select    = 7U,  /**< CMD7  SELECT_CARD.          */
  k_sdhi_cmd8_if_cond   = 8U,  /**< CMD8  SEND_IF_COND (R7).    */
  k_sdhi_cmd9_send_csd  = 9U,  /**< CMD9  SEND_CSD (R2).        */
  k_sdhi_cmd12_stop     = 12U, /**< CMD12 STOP_TRANSMISSION.    */
  k_sdhi_cmd16_blocklen = 16U, /**< CMD16 SET_BLOCKLEN.         */
  k_sdhi_cmd17_read1    = 17U, /**< CMD17 READ_SINGLE_BLOCK.    */
  k_sdhi_cmd18_readm    = 18U, /**< CMD18 READ_MULTIPLE_BLOCK.  */
  k_sdhi_cmd24_write1   = 24U, /**< CMD24 WRITE_SINGLE_BLOCK.   */
  k_sdhi_cmd25_writem   = 25U, /**< CMD25 WRITE_MULTIPLE_BLOCK. */
  k_sdhi_acmd41_op_cond = 41U, /**< ACMD41 SD_SEND_OP_COND.     */
  k_sdhi_cmd55_app_cmd  = 55U, /**< CMD55 APP_CMD prefix.       */
} sdhi_cmd_idx_t;

/** @brief Canned response field values the firmware accepts. */
typedef enum : uint32_t {
  k_sdhi_cmd_index_mask = 0x0000003FU, /**< Low 6 bits of SD_CMD = index.       */
  k_sdhi_r1_app_cmd     = 0x00000020U, /**< R1 bit 5 APP_CMD (CMD55 ack).       */
  k_sdhi_r1_ready       = 0x00000900U, /**< R1: TRAN state + ready-for-data.    */
  k_sdhi_r7_if_cond     = 0x000001AAU, /**< CMD8 R7: voltage 0x1 + echo 0xAA.   */
  k_sdhi_ocr_ready      = 0xC0FF8000U, /**< ACMD41 R3: busy=1, CCS=1, volt win. */
  k_sdhi_cid_word       = 0x52413844U, /**< Dummy CID fill ("RA8D" packed).     */
  k_sdhi_rca_value      = 0x00010000U, /**< CMD3 R6: RCA = 1 in bits [31:16].   */
  k_sdhi_csd_v2_struct  = 0x40000000U, /**< CSD v2 structure tag in rsp[3].     */
  k_sdhi_csize_lo_mask  = 0x0000003FU, /**< C_SIZE[21:16] field width.          */
  k_sdhi_csize_mid_mask = 0x0000FFFFU, /**< C_SIZE[15:0] field width.           */
} sdhi_resp_const_t;

/** @brief Geometry / sizing constants (no magic numbers). */
typedef enum : uint32_t {
  k_sdhi_words         = 128U,  /**< 0x200-byte window as 32-bit words.  */
  k_sdhi_block_bytes   = 512U,  /**< SD block size in bytes.             */
  k_sdhi_words_per_blk = 128U,  /**< 512 / 4 = 128 FIFO words per block. */
  k_sdhi_byte_bits     = 8U,    /**< Bits per byte.                      */
  k_sdhi_byte_mask     = 0xFFU, /**< One byte.                           */
  k_sdhi_csize_unit    = 1024U, /**< CSD v2: blocks per (C_SIZE+1) unit. */
  k_sdhi_csize_shift   = 16U,   /**< C_SIZE high-half shift.             */
} sdhi_geom_t;

/** @brief End-of-run report-order slot (after the XSPI block at 95). */
typedef enum : uint32_t {
  k_sdhi_block_order = 96U, /**< Report ordering only. */
} sdhi_order_t;

/** @brief Block-transfer phase the model is mid-way through. */
typedef enum : uint8_t {
  k_sdhi_xfer_none  = 0U, /**< No data phase in flight.   */
  k_sdhi_xfer_read  = 1U, /**< CMD17/18 read data phase.  */
  k_sdhi_xfer_write = 2U, /**< CMD24/25 write data phase. */
} sdhi_xfer_t;

/** @brief SDHI model state: register shadow + command + data-phase engine. */
typedef struct {
  uint32_t regs[k_sdhi_words];        /**< 0x200-byte window shadow.         */
  uint32_t rsp[4];                    /**< Latched response words.           */
  uint8_t  stage[k_sdhi_block_bytes]; /**< Active 512-byte block buffer.     */
  uint32_t word_idx;                  /**< Next SD_BUF0 word in the block.   */
  uint32_t lba;                       /**< Current block address.            */
  uint32_t blocks_left;               /**< Remaining blocks in the transfer. */
  uint8_t  app_cmd;                   /**< 1 => previous command was CMD55.  */
  uint8_t  xfer;                      /**< ::sdhi_xfer_t data-phase state.   */
  uint32_t reads;                     /**< Block reads served.               */
  uint32_t writes;                    /**< Block writes committed.           */
} sdhi_state_t;

static sdhi_state_t s_sdhi;

/** @brief Word index of register at byte offset @p off inside the shadow. */
static uint32_t sdhi_word(uint64_t off)
{
  return (uint32_t)(off / 4U);
}

/** @brief Copy the latched response words into the SD_RSP* shadow registers. */
static void sdhi_publish_response(void)
{
  s_sdhi.regs[sdhi_word(k_sdhi_off_sd_rsp10)] = s_sdhi.rsp[0];
  s_sdhi.regs[sdhi_word(k_sdhi_off_sd_rsp32)] = s_sdhi.rsp[1];
  s_sdhi.regs[sdhi_word(k_sdhi_off_sd_rsp54)] = s_sdhi.rsp[2];
  s_sdhi.regs[sdhi_word(k_sdhi_off_sd_rsp76)] = s_sdhi.rsp[3];
}

/** @brief Encode the attached card's capacity into a CSD v2 response (CMD9). */
static void sdhi_make_csd(void)
{
  uint64_t bytes = 0U;
  board_sd_info(nullptr, &bytes, nullptr, nullptr);
  uint32_t cap_blocks = (uint32_t)(bytes / (uint64_t)k_sdhi_block_bytes);
  if (cap_blocks < (uint32_t)k_sdhi_csize_unit) {
    cap_blocks = (uint32_t)k_sdhi_csize_unit; /* floor: keep c_size non-negative. */
  }
  const uint32_t c_size = (cap_blocks / (uint32_t)k_sdhi_csize_unit) - 1U;
  s_sdhi.rsp[3]         = (uint32_t)k_sdhi_csd_v2_struct;
  s_sdhi.rsp[2]         = (c_size >> (uint32_t)k_sdhi_csize_shift) & (uint32_t)k_sdhi_csize_lo_mask;
  s_sdhi.rsp[1] = (c_size & (uint32_t)k_sdhi_csize_mid_mask) << (uint32_t)k_sdhi_csize_shift;
  s_sdhi.rsp[0] = 0U;
}

/** @brief Load the block at the current LBA into the staging buffer (read phase). */
static void sdhi_load_block(void)
{
  (void)memset(s_sdhi.stage, 0, sizeof(s_sdhi.stage));
  (void)board_sd_read_block(s_sdhi.lba, s_sdhi.stage);
  s_sdhi.word_idx = 0U;
  s_sdhi.regs[sdhi_word(k_sdhi_off_sd_info2)] |= (uint32_t)k_sdhi_info2_bre;
}

/** @brief Arm a read data phase for CMD17 (single) or CMD18 (multi). */
static void sdhi_begin_read(bool multi)
{
  s_sdhi.xfer        = (uint8_t)k_sdhi_xfer_read;
  s_sdhi.blocks_left = multi ? s_sdhi.regs[sdhi_word(k_sdhi_off_sd_seccnt)] : 1U;
  if (s_sdhi.blocks_left == 0U) {
    s_sdhi.blocks_left = 1U;
  }
  /* Console SD tab: one line per block-read command (CMD17/CMD18). */
  char ln[k_sdhi_console_line_cap];
  (void)snprintf(ln,
                 sizeof(ln),
                 "SDHI rd lba=%u x%u",
                 (unsigned)s_sdhi.lba,
                 (unsigned)s_sdhi.blocks_left);
  board_console_push(k_board_console_ch_sd, ln);
  sdhi_load_block();
}

/** @brief Arm a write data phase for CMD24 (single) or CMD25 (multi). */
static void sdhi_begin_write(bool multi)
{
  s_sdhi.xfer        = (uint8_t)k_sdhi_xfer_write;
  s_sdhi.blocks_left = multi ? s_sdhi.regs[sdhi_word(k_sdhi_off_sd_seccnt)] : 1U;
  if (s_sdhi.blocks_left == 0U) {
    s_sdhi.blocks_left = 1U;
  }
  (void)memset(s_sdhi.stage, 0, sizeof(s_sdhi.stage));
  s_sdhi.word_idx = 0U;
  s_sdhi.regs[sdhi_word(k_sdhi_off_sd_info2)] |= (uint32_t)k_sdhi_info2_bwe;
  /* Console SD tab: one line per block-write command (CMD24/CMD25). */
  char ln[k_sdhi_console_line_cap];
  (void)snprintf(ln,
                 sizeof(ln),
                 "SDHI wr lba=%u x%u",
                 (unsigned)s_sdhi.lba,
                 (unsigned)s_sdhi.blocks_left);
  board_console_push(k_board_console_ch_sd, ln);
}

/** @brief Decode the identification + addressing commands into a response. */
static void sdhi_exec_ident(uint32_t idx, uint32_t arg)
{
  (void)arg;
  if ((idx == (uint32_t)k_sdhi_acmd41_op_cond) && (s_sdhi.app_cmd != 0U)) {
    s_sdhi.rsp[0] = (uint32_t)k_sdhi_ocr_ready;
    return;
  }
  switch (idx) {
    case (uint32_t)k_sdhi_cmd8_if_cond:
      s_sdhi.rsp[0] = (uint32_t)k_sdhi_r7_if_cond;
      break;
    case (uint32_t)k_sdhi_cmd55_app_cmd:
      s_sdhi.app_cmd = 1U;
      /* Echo APP_CMD so the ACMD6 SET_BUS_WIDTH that may follow is
       * honoured by the driver's 4-bit negotiation check. */
      s_sdhi.rsp[0] = (uint32_t)k_sdhi_r1_ready | (uint32_t)k_sdhi_r1_app_cmd;
      break;
    case (uint32_t)k_sdhi_cmd2_send_cid:
      s_sdhi.rsp[0] = (uint32_t)k_sdhi_cid_word;
      s_sdhi.rsp[1] = (uint32_t)k_sdhi_cid_word;
      s_sdhi.rsp[2] = (uint32_t)k_sdhi_cid_word;
      s_sdhi.rsp[3] = (uint32_t)k_sdhi_cid_word;
      break;
    case (uint32_t)k_sdhi_cmd3_send_rca:
      s_sdhi.rsp[0] = (uint32_t)k_sdhi_rca_value;
      break;
    case (uint32_t)k_sdhi_cmd9_send_csd:
      sdhi_make_csd();
      break;
    default:
      s_sdhi.rsp[0] = (uint32_t)k_sdhi_r1_ready;
      break;
  }
}

/** @brief Execute the latched command, build its response, raise RSPEND. */
static void sdhi_exec_command(uint32_t cmd)
{
  const uint32_t idx = cmd & (uint32_t)k_sdhi_cmd_index_mask;
  const uint32_t arg = s_sdhi.regs[sdhi_word(k_sdhi_off_sd_arg)];

  s_sdhi.rsp[0] = (uint32_t)k_sdhi_r1_ready;
  s_sdhi.rsp[1] = 0U;
  s_sdhi.rsp[2] = 0U;
  s_sdhi.rsp[3] = 0U;

  switch (idx) {
    case (uint32_t)k_sdhi_cmd17_read1:
      s_sdhi.lba = arg;
      sdhi_begin_read(false);
      break;
    case (uint32_t)k_sdhi_cmd18_readm:
      s_sdhi.lba = arg;
      sdhi_begin_read(true);
      break;
    case (uint32_t)k_sdhi_cmd24_write1:
      s_sdhi.lba = arg;
      sdhi_begin_write(false);
      break;
    case (uint32_t)k_sdhi_cmd25_writem:
      s_sdhi.lba = arg;
      sdhi_begin_write(true);
      break;
    case (uint32_t)k_sdhi_cmd12_stop:
      s_sdhi.xfer = (uint8_t)k_sdhi_xfer_none;
      break;
    default:
      sdhi_exec_ident(idx, arg);
      break;
  }
  /* CMD55 sets app_cmd; every other command clears the one-shot prefix. */
  if (idx != (uint32_t)k_sdhi_cmd55_app_cmd) {
    s_sdhi.app_cmd = 0U;
  }
  sdhi_publish_response();
  s_sdhi.regs[sdhi_word(k_sdhi_off_sd_info1)] |= (uint32_t)k_sdhi_info1_rspend;
}

/** @brief Serve one 32-bit FIFO word from the staging buffer (read phase). */
static uint32_t sdhi_buf_read(void)
{
  if (s_sdhi.xfer != (uint8_t)k_sdhi_xfer_read) {
    return 0U;
  }
  const uint32_t b    = s_sdhi.word_idx * 4U;
  const uint32_t word = (uint32_t)s_sdhi.stage[b] |
                        ((uint32_t)s_sdhi.stage[b + 1U] << (uint32_t)k_sdhi_byte_bits) |
                        ((uint32_t)s_sdhi.stage[b + 2U] << (2U * (uint32_t)k_sdhi_byte_bits)) |
                        ((uint32_t)s_sdhi.stage[b + 3U] << (3U * (uint32_t)k_sdhi_byte_bits));
  s_sdhi.word_idx++;
  if (s_sdhi.word_idx < (uint32_t)k_sdhi_words_per_blk) {
    return word;
  }
  /* Block drained: advance a multi-block read, else end the phase. */
  s_sdhi.reads++;
  if (s_sdhi.blocks_left > 0U) {
    s_sdhi.blocks_left--;
  }
  if (s_sdhi.blocks_left > 0U) {
    s_sdhi.lba++;
    sdhi_load_block();
  } else {
    s_sdhi.xfer = (uint8_t)k_sdhi_xfer_none;
    s_sdhi.regs[sdhi_word(k_sdhi_off_sd_info2)] &= ~(uint32_t)k_sdhi_info2_bre;
  }
  return word;
}

/** @brief Capture one 32-bit FIFO word into the staging buffer (write phase). */
static void sdhi_buf_write(uint32_t word)
{
  if (s_sdhi.xfer != (uint8_t)k_sdhi_xfer_write) {
    return;
  }
  const uint32_t b = s_sdhi.word_idx * 4U;
  s_sdhi.stage[b]  = (uint8_t)(word & (uint32_t)k_sdhi_byte_mask);
  s_sdhi.stage[b + 1U] =
    (uint8_t)((word >> (uint32_t)k_sdhi_byte_bits) & (uint32_t)k_sdhi_byte_mask);
  s_sdhi.stage[b + 2U] =
    (uint8_t)((word >> (2U * (uint32_t)k_sdhi_byte_bits)) & (uint32_t)k_sdhi_byte_mask);
  s_sdhi.stage[b + 3U] =
    (uint8_t)((word >> (3U * (uint32_t)k_sdhi_byte_bits)) & (uint32_t)k_sdhi_byte_mask);
  s_sdhi.word_idx++;
  if (s_sdhi.word_idx < (uint32_t)k_sdhi_words_per_blk) {
    return;
  }
  /* Block filled: commit it, advance a multi-block write, else end the phase. */
  (void)board_sd_write_block(s_sdhi.lba, s_sdhi.stage);
  s_sdhi.writes++;
  s_sdhi.word_idx = 0U;
  if (s_sdhi.blocks_left > 0U) {
    s_sdhi.blocks_left--;
  }
  if (s_sdhi.blocks_left > 0U) {
    s_sdhi.lba++;
  } else {
    s_sdhi.xfer = (uint8_t)k_sdhi_xfer_none;
    s_sdhi.regs[sdhi_word(k_sdhi_off_sd_info2)] &= ~(uint32_t)k_sdhi_info2_bwe;
  }
}

/** @brief MMIO read inside the SDHI window (SD_BUF0 is a live FIFO). */
static uint64_t sdhi_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_sdhi_base;
  if (off == (uint64_t)k_sdhi_off_sd_buf0) {
    return (uint64_t)sdhi_buf_read();
  }
  if (sdhi_word(off) >= (uint32_t)k_sdhi_words) {
    return 0U;
  }
  return (uint64_t)s_sdhi.regs[sdhi_word(off)];
}

/** @brief MMIO write inside the SDHI window (SD_CMD issues; SD_BUF0 captures). */
static void sdhi_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_sdhi_base;
  if (off == (uint64_t)k_sdhi_off_sd_buf0) {
    sdhi_buf_write((uint32_t)value);
    return;
  }
  if (off == (uint64_t)k_sdhi_off_soft_rst) {
    /* The driver writes 0 then 1; clear the FIFO/command engine either way. */
    s_sdhi.xfer                                 = (uint8_t)k_sdhi_xfer_none;
    s_sdhi.word_idx                             = 0U;
    s_sdhi.blocks_left                          = 0U;
    s_sdhi.app_cmd                              = 0U;
    s_sdhi.regs[sdhi_word(off)]                 = (uint32_t)value;
    s_sdhi.regs[sdhi_word(k_sdhi_off_sd_info1)] = 0U;
    s_sdhi.regs[sdhi_word(k_sdhi_off_sd_info2)] = 0U;
    return;
  }
  if (sdhi_word(off) < (uint32_t)k_sdhi_words) {
    s_sdhi.regs[sdhi_word(off)] = (uint32_t)value;
  }
  if (off == (uint64_t)k_sdhi_off_sd_cmd) {
    sdhi_exec_command((uint32_t)value);
  }
}

/** @brief Reset the SDHI model: zero the shadow + command/data-phase engine. */
static void sdhi_reset(void)
{
  (void)memset(&s_sdhi, 0, sizeof(s_sdhi));
}

/** @brief Decode the SD_OPTION shadow into the host bus-width lane count. */
static uint32_t sdhi_bus_lanes(void)
{
  const uint32_t opt = s_sdhi.regs[sdhi_word(k_sdhi_off_sd_option)];
  if ((opt & (uint32_t)k_sdhi_option_width_bit) != 0U) {
    return (uint32_t)k_sdhi_lanes_1bit;
  }
  if ((opt & (uint32_t)k_sdhi_option_width8_bit) != 0U) {
    return (uint32_t)k_sdhi_lanes_8bit;
  }
  return (uint32_t)k_sdhi_lanes_4bit;
}

/** @brief End-of-run SDHI section: block I/O counts (only if the bus was used). */
static void sdhi_report(void)
{
  if ((s_sdhi.reads == 0U) && (s_sdhi.writes == 0U)) {
    return;
  }
  (void)fprintf(stderr,
                "  SDHI card     : %u block reads  %u block writes  %u-bit bus\n",
                s_sdhi.reads,
                s_sdhi.writes,
                (unsigned)sdhi_bus_lanes());
}

/** @brief SDHI0 block descriptor (self-registered with the core). */
static const board_periph_block_t k_sdhi_block = {
  .base   = (uint32_t)k_sdhi_base,
  .span   = (uint32_t)k_sdhi_span,
  .order  = (uint32_t)k_sdhi_block_order,
  .read   = sdhi_read,
  .write  = sdhi_write,
  .tick   = nullptr,
  .reset  = sdhi_reset,
  .report = sdhi_report,
  .name   = "SDHI",
};

/** @brief Register the SDHI block before main (host constructor). */
[[gnu::constructor]] static void sdhi_block_register(void)
{
  board_periph_register_block(&k_sdhi_block);
}
