/**
 * @file test_ra8_sdmmc_card_reflow.c
 * @brief Full SD-over-SPI path: a modelled SD card serves a FAT volume to
 *        the real ra8_sdmmc_spi driver, ra8_fs reads a font off it, and
 *        ra8_reflow renders -- all heap-free, all on host.
 *
 * @details
 * This is the hardware-faithful version of test_ra8_fs_font_reflow: instead
 * of a mock block backend, the FAT image is served through a **modelled SD
 * card** that speaks the SD SPI-mode protocol (CMD0/CMD8/CMD58/ACMD41 init,
 * CMD17 single-block read) exactly as the @ref ra8_sdmmc_spi driver expects.
 * The driver's transport seam routes every byte exchange into the card
 * model, so the genuine ra8_sdmmc_spi command/response/data-token code runs.
 * `ra8_sdmmc_spi_bind_fs_backend` then plugs the card straight into ra8_fs.
 *
 * The SD card model here is the seed for board_sim's SD-over-SPI device
 * (so `make sim-ereader_ui` can serve a font from a simulated card).
 *
 * Pipeline: image (FAT) -> SD card model -> ra8_sdmmc_spi -> ra8_fs ->
 * ra8_reflow -> ra8_gfx framebuffer.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_fs.h"
#include "ra8_gfx.h"
#include "ra8_reflow.h"
#include "ra8_sdmmc_spi.h"
#include "unity_minimal.h"

/**
 * @enum t_bpb_off_t
 * @brief BIOS Parameter Block field offsets within the FAT boot sector.
 *
 * @details
 * Offsets are fixed by the FAT specification. Naming them lets the fixture
 * builder read as the boot sector it is writing.
 */
typedef enum : uint8_t {
  k_t_bpb_off_bytes_per_sector = 11U,   /**< BPB_BytsPerSec, 16-bit.           */
  k_t_bpb_off_sectors_per_clus = 13U,   /**< BPB_SecPerClus, 8-bit.            */
  k_t_bpb_off_reserved_sectors = 14U,   /**< BPB_RsvdSecCnt, 16-bit.           */
  k_t_bpb_off_root_entries     = 17U,   /**< BPB_RootEntCnt, 16-bit.           */
  k_t_bpb_off_total_sectors    = 19U,   /**< BPB_TotSec16, 16-bit.             */
  k_t_byte_mask                = 0xFFU, /**< Low-byte mask while writing a
                                             16-bit little-endian field.      */
} t_bpb_off_t;

/* ===========================================================================
 * SD SPI-mode card model (the seed for board_sim's board_sd device).
 * ===========================================================================
 */
typedef enum : uint16_t {
  k_sd_block      = 512U,  /**< SD block.                             */
  k_sd_cmd_len    = 6U,    /**< opcode + 4 arg + crc.                 */
  k_sd_resp_cap   = 520U,  /**< R1 + token + 512 data + 2 CRC.        */
  k_sd_cmd_start  = 0x40U, /**< SPI command lead bits (01xxxxxx).     */
  k_sd_cmd_mask   = 0xC0U, /**< Mask isolating the lead bits.         */
  k_sd_idx_mask   = 0x3FU, /**< Command index in the lead byte.       */
  k_sd_idle       = 0xFFU, /**< Bus idle / CIPO-high byte.            */
  k_sd_tok_data   = 0xFEU, /**< Single-block read data token.         */
  k_sd_r1_idle    = 0x01U, /**< R1 with IDLE set.                     */
  k_sd_r1_ready   = 0x00U, /**< R1, card ready.                       */
  k_sd_byte_mask  = 0xFFU, /**< Low byte mask.                        */
  k_sd_byte_bits  = 8U,    /**< Bits per byte (shift amount).         */
  k_sd_r7_len     = 5U,    /**< R1 + 4-byte tail.                     */
  k_sd_arg_sh0    = 24U,   /**< Arg byte 0 (MSB) shift.               */
  k_sd_arg_sh1    = 16U,   /**< Arg byte 1 shift.                     */
  k_sd_cmd8_echo  = 0xAAU, /**< CMD8 check pattern echo.              */
  k_sd_ocr_pwrccs = 0xC0U, /**< OCR byte 0: power-up done + CCS.      */
  k_sd_ocr_volt   = 0x80U, /**< OCR byte 2: voltage window.           */
  k_sd_csd_v2     = 0x40U, /**< CSD_STRUCTURE = 01b (v2.0 / SDHC).    */
  k_sd_csd_csize  = 0x0FU, /**< C_SIZE byte (=> 8 MiB modelled card). */
  k_sd_csd_len    = 16U,   /**< CSD register length.                  */
  k_sd_csd_off    = 9U,    /**< C_SIZE LSB byte offset in the CSD.    */
} sd_card_const_t;

/** @brief SD SPI command indices the model answers (low 6 bits of byte 0). */
typedef enum : uint8_t {
  k_sd_idx_cmd0   = 0U,  /**< GO_IDLE_STATE.   */
  k_sd_idx_cmd8   = 8U,  /**< SEND_IF_COND.    */
  k_sd_idx_cmd9   = 9U,  /**< SEND_CSD.        */
  k_sd_idx_cmd16  = 16U, /**< SET_BLOCKLEN.    */
  k_sd_idx_cmd17  = 17U, /**< READ_SINGLE.     */
  k_sd_idx_acmd41 = 41U, /**< SD_SEND_OP_COND. */
  k_sd_idx_cmd55  = 55U, /**< APP_CMD.         */
  k_sd_idx_cmd58  = 58U, /**< READ_OCR.        */
} sd_cmd_idx_t;

typedef struct {
  const uint8_t* image;               /**< Backing card image (FAT volume).      */
  uint32_t       image_len;           /**< Image length.                         */
  bool           collecting;          /**< Collecting.                           */
  bool           app_cmd;             /**< Previous command was CMD55 (APP_CMD). */
  bool           ready;               /**< ACMD41 has completed.                 */
  uint8_t        cmd[k_sd_cmd_len];   /**< Cmd.                                  */
  uint32_t       cmd_idx;             /**< Cmd index.                            */
  uint8_t        resp[k_sd_resp_cap]; /**< Resp.                                 */
  uint32_t       resp_len;            /**< Resp length.                          */
  uint32_t       resp_pos;            /**< Resp pos.                             */
} sd_card_t;

static sd_card_t s_card;

/**
 * @brief CMD58 READ_OCR: fill an R3 response (R1 + OCR, CCS=1 => SDHC).
 * @param[in,out] c  Mock card whose response buffer is filled.
 * @param[in]     r1 The R1 status byte for the current ready state.
 * @pre @p c is non-null.
 * @post @p c holds a 5-byte R3 response.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void sd_cmd_read_ocr(sd_card_t* c, uint8_t r1)
{
  c->resp[0]  = r1;
  c->resp[1]  = (uint8_t)k_sd_ocr_pwrccs;
  c->resp[2]  = (uint8_t)k_sd_idle;
  c->resp[3]  = (uint8_t)k_sd_ocr_volt;
  c->resp[4]  = 0U;
  c->resp_len = (uint8_t)k_sd_r7_len;
}

/**
 * @brief CMD9 SEND_CSD: fill R1 + token + 16-byte CSD + CRC.
 * @param[in,out] c Mock card whose response buffer is filled.
 * @pre @p c is non-null.
 * @post @p c holds the CSD response with a valid CRC16.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void sd_cmd_send_csd(sd_card_t* c)
{
  const uint8_t  bm = (uint8_t)k_sd_byte_mask;
  const uint32_t bb = (uint32_t)k_sd_byte_bits;
  c->resp[0]        = (uint8_t)k_sd_r1_ready;
  c->resp[1]        = (uint8_t)k_sd_tok_data;
  uint8_t csd[k_sd_csd_len];
  memset(csd, 0, sizeof(csd));
  csd[0]            = (uint8_t)k_sd_csd_v2;    /* CSD_STRUCTURE = v2.0. */
  csd[k_sd_csd_off] = (uint8_t)k_sd_csd_csize; /* C_SIZE => 8 MiB card. */
  memcpy(&c->resp[2], csd, sizeof(csd));
  const uint16_t crc9                       = ra8_sdmmc_spi_crc16(&c->resp[2], k_sd_csd_len);
  c->resp[2U + (uint32_t)k_sd_csd_len]      = (uint8_t)(crc9 >> bb);
  c->resp[2U + (uint32_t)k_sd_csd_len + 1U] = (uint8_t)(crc9 & bm);
  c->resp_len                               = 2U + (uint32_t)k_sd_csd_len + 2U;
}

/**
 * @brief CMD17 READ_SINGLE_BLOCK: fill R1 + token + one 512-byte block + CRC.
 * @param[in,out] c   Mock card whose response buffer is filled.
 * @param[in]     arg The SDHC block index requested.
 * @pre @p c is non-null.
 * @post @p c holds the block data (zero-filled past the image) with a valid CRC.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void sd_cmd_read_block(sd_card_t* c, uint32_t arg)
{
  const uint8_t  bm  = (uint8_t)k_sd_byte_mask;
  const uint32_t bb  = (uint32_t)k_sd_byte_bits;
  c->resp[0]         = (uint8_t)k_sd_r1_ready;
  c->resp[1]         = (uint8_t)k_sd_tok_data;
  const uint32_t off = arg * (uint32_t)k_sd_block;
  for (uint32_t i = 0U; i < (uint32_t)k_sd_block; ++i) {
    const uint32_t a = off + i;
    c->resp[2U + i]  = (a < c->image_len) ? c->image[a] : 0U;
  }
  const uint16_t crc17                    = ra8_sdmmc_spi_crc16(&c->resp[2], (uint32_t)k_sd_block);
  c->resp[2U + (uint32_t)k_sd_block]      = (uint8_t)(crc17 >> bb);
  c->resp[2U + (uint32_t)k_sd_block + 1U] = (uint8_t)(crc17 & bm);
  c->resp_len                             = 2U + (uint32_t)k_sd_block + 2U;
}

/**
 * @brief Stage a bare one-byte R1 reply.
 * @param[in,out] c      Card state whose response buffer is staged.
 * @param[in]     status R1 status byte to answer with.
 * @pre @p c is non-null with a staging response buffer.
 * @pre @p c->resp holds room for at least one byte.
 * @post @p c->resp[0] is @p status and @p c->resp_len is 1.
 * @post The backing image is untouched.
 * @note Not thread-safe; single-threaded host-test model.
 * @since 0.1.0
 */
static void sd_reply_r1(sd_card_t* c, uint8_t status)
{
  c->resp[0]  = status;
  c->resp_len = 1U;
}

/**
 * @brief Stage the CMD8 (SEND_IF_COND) R7 reply: R1 plus the 0x1AA echo.
 * @param[in,out] c Card state whose response buffer is staged.
 * @pre @p c is non-null with a staging response buffer.
 * @pre @p c->resp holds room for ::k_sd_r7_len bytes.
 * @post @p c->resp carries the idle R1 and the voltage/pattern echo.
 * @post @p c->resp_len is ::k_sd_r7_len.
 * @note Not thread-safe; single-threaded host-test model.
 * @since 0.1.0
 */
static void sd_cmd_send_if_cond(sd_card_t* c)
{
  c->resp[0]  = (uint8_t)k_sd_r1_idle;
  c->resp[1]  = 0U;
  c->resp[2]  = 0U;
  c->resp[3]  = 1U;
  c->resp[4]  = (uint8_t)k_sd_cmd8_echo;
  c->resp_len = (uint8_t)k_sd_r7_len;
}

static void sd_process_cmd(sd_card_t* c)
{
  const uint8_t  idx     = (uint8_t)(c->cmd[0] & (uint8_t)k_sd_idx_mask);
  const uint32_t arg     = ((uint32_t)c->cmd[1] << (uint32_t)k_sd_arg_sh0) |
                           ((uint32_t)c->cmd[2] << (uint32_t)k_sd_arg_sh1) |
                           ((uint32_t)c->cmd[3] << (uint32_t)k_sd_byte_bits) | (uint32_t)c->cmd[4];
  const bool     was_app = c->app_cmd;
  c->app_cmd             = false;
  c->resp_pos            = 0U;
  c->resp_len            = 0U;
  const uint8_t r1       = c->ready ? (uint8_t)k_sd_r1_ready : (uint8_t)k_sd_r1_idle;
#ifdef SD_DBG
  (void)fprintf(stderr, "SD cmd=%u arg=%08x app=%d\n", idx, arg, (int)was_app);
#endif

  if ((idx == (uint8_t)k_sd_idx_acmd41) && was_app) { /* ACMD41 -- init done. */
    c->ready = true;
    sd_reply_r1(c, (uint8_t)k_sd_r1_ready);
    return;
  }
  switch (idx) {
    case (uint8_t)k_sd_idx_cmd0: /* GO_IDLE */
      sd_reply_r1(c, (uint8_t)k_sd_r1_idle);
      break;
    case (uint8_t)k_sd_idx_cmd8: /* SEND_IF_COND -> R7 (R1 + 0x000001AA echo) */
      sd_cmd_send_if_cond(c);
      break;
    case (uint8_t)k_sd_idx_cmd55: /* APP_CMD */
      c->app_cmd = true;
      sd_reply_r1(c, (uint8_t)k_sd_r1_idle);
      break;
    case (uint8_t)k_sd_idx_cmd58: /* READ_OCR -> R3 (R1 + OCR, CCS=1 => SDHC) */
      sd_cmd_read_ocr(c, r1);
      break;
    case (uint8_t)k_sd_idx_cmd16: /* SET_BLOCKLEN */
      sd_reply_r1(c, (uint8_t)k_sd_r1_ready);
      break;
    case (uint8_t)k_sd_idx_cmd9: /* SEND_CSD -> R1 + token + 16-byte CSD + CRC */
      sd_cmd_send_csd(c);
      break;
    case (uint8_t)k_sd_idx_cmd17: /* READ_SINGLE_BLOCK (SDHC: arg = block) */
      sd_cmd_read_block(c, arg);
      break;
    default:
      sd_reply_r1(c, r1);
      break;
  }
}

/** @brief Exchange one byte with the card (full-duplex: host-out/card-in). */
static uint8_t sd_exchange(sd_card_t* c, uint8_t tx)
{
  if (c->resp_pos < c->resp_len) {
    return c->resp[c->resp_pos++];
  }
  if (c->collecting) {
    c->cmd[c->cmd_idx] = tx;
    c->cmd_idx++;
    if (c->cmd_idx >= (uint32_t)k_sd_cmd_len) {
      c->collecting = false;
      sd_process_cmd(c);
    }
    return (uint8_t)k_sd_idle;
  }
  if ((tx & (uint8_t)k_sd_cmd_mask) == (uint8_t)k_sd_cmd_start) {
    c->collecting = true;
    c->cmd[0]     = tx;
    c->cmd_idx    = 1U;
  }
  return (uint8_t)k_sd_idle;
}

/* ---- ra8_sdmmc_spi transport bound to the card model ---------------------- */
static ra8_err_t t_set_clock(void* ctx, uint32_t hz)
{
  (void)ctx;
  (void)hz;
  return k_ra8_ok;
}
static ra8_err_t t_cs(void* ctx, bool asserted)
{
  (void)ctx;
  (void)asserted;
  return k_ra8_ok;
}
static ra8_err_t t_xfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
{
  sd_card_t* c = (sd_card_t*)ctx;
  for (uint32_t i = 0U; i < len; ++i) {
    const uint8_t out = (tx != nullptr) ? tx[i] : (uint8_t)k_sd_idle;
    const uint8_t in  = sd_exchange(c, out);
    if (rx != nullptr) {
      rx[i] = in;
    }
  }
  return k_ra8_ok;
}

/* ===========================================================================
 * FAT image populated through the direct mem backend (no SD write needed).
 * ===========================================================================
 */
typedef enum : uint32_t {
  k_disk_block_size   = 512U,        /**< Disk block size.   */
  k_disk_blocks_fat16 = 8U * 1024U,  /**< Disk blocks fat16. */
  k_bpb_off_secperfat = 22U,         /**< Bpb off secperfat. */
  k_bpb_sig_off_a     = 510U,        /**< Bpb sig off a.     */
  k_bpb_sig_off_b     = 511U,        /**< Bpb sig off b.     */
  k_bpb_sig_a         = 0x55U,       /**< Bpb sig a.         */
  k_bpb_sig_b         = 0xAAU,       /**< Bpb sig b.         */
  k_bg_argb           = 0xFF000000U, /**< Bg argb.           */
} test_disk_const_t;

typedef struct {
  uint8_t* bytes;       /**< Bytes.       */
  uint32_t block_count; /**< Block count. */
} mem_disk_t;

static mem_disk_t s_disk;

static ra8_err_t mem_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_disk_block_size],
         (size_t)count * (uint32_t)k_disk_block_size);
  return k_ra8_ok;
}
static ra8_err_t mem_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_disk_block_size],
         buf,
         (size_t)count * (uint32_t)k_disk_block_size);
  return k_ra8_ok;
}
static ra8_err_t mem_cap(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  *block_count  = d->block_count;
  *block_size   = (uint32_t)k_disk_block_size;
  return k_ra8_ok;
}
static const ra8_fs_backend_t s_mem_backend = {.read_block   = mem_read,
                                               .write_block  = mem_write,
                                               .get_capacity = mem_cap,
                                               .ctx          = &s_disk};

static void put16(uint8_t* p, uint32_t off, uint16_t v)
{
  p[off]     = (uint8_t)(v & k_t_byte_mask);
  p[off + 1] = (uint8_t)((v >> 8) & k_t_byte_mask);
}
/** @brief Build the response for a completed 6-byte command frame. */
static void build_fat16(void)
{
  s_disk.bytes       = (uint8_t*)calloc(1, (size_t)k_disk_blocks_fat16 * k_disk_block_size);
  s_disk.block_count = (uint32_t)k_disk_blocks_fat16;
  uint8_t* b         = s_disk.bytes;
  put16(b, k_t_bpb_off_bytes_per_sector, (uint16_t)k_disk_block_size);
  b[k_t_bpb_off_sectors_per_clus] = 1U;
  put16(b, k_t_bpb_off_reserved_sectors, 1U);
  b[16] = 2U;
  put16(b, k_t_bpb_off_root_entries, 16U);
  put16(b, k_t_bpb_off_total_sectors, (uint16_t)k_disk_blocks_fat16);
  put16(b, (uint32_t)k_bpb_off_secperfat, 32U);
  b[k_bpb_sig_off_a] = (uint8_t)k_bpb_sig_a;
  b[k_bpb_sig_off_b] = (uint8_t)k_bpb_sig_b;
}

/* ---- font + framebuffer -------------------------------------------------- */
enum {
  k_font_cap = 2U * 1024U * 1024U, /**< Font cap. */
  k_path_cap = 1024,               /**< Path cap. */
  k_fb_w     = 384,                /**< Fb w.     */
  k_fb_h     = 256,                /**< Fb h.     */
  k_font_px  = 18,                 /**< Font px.  */
};
static uint8_t*  s_src_font;
static uint8_t*  s_card_font;
static uint32_t  s_font_len;
static uint32_t* s_fb;

static bool load_src_font(void)
{
  char path[k_path_cap];
  (void)snprintf(path, sizeof(path), "%s", __FILE__);
  for (int drop = 0; drop < 2; ++drop) {
    size_t n = strlen(path);
    while (n > 0U && path[n - 1U] != '/') {
      --n;
    }
    if (n > 0U) {
      path[n - 1U] = '\0';
    }
  }
  const size_t base = strlen(path);
  (void)snprintf(&path[base], sizeof(path) - base, "/libs/fonts/Literata-Regular.ttf");
  FILE* fp = fopen(path, "rb");
  if (fp == nullptr) {
    return false;
  }
  size_t n = fread(s_src_font, 1U, k_font_cap, fp);
  (void)fclose(fp);
  if (n < 16U) {
    return false;
  }
  s_font_len = (uint32_t)n;
  return true;
}

/**
 * @brief Format a FAT16 image and write the source font through the mem backend.
 * @pre The source font is loaded into s_src_font (length s_font_len).
 * @post FONT.OTF is present in the card image, ready for SD read-back.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void sd_reflow_populate_card(void)
{
  build_fat16();
  ra8_fs_mount_t* setup = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_mem_backend, &setup));
  ra8_fs_file_t* wf = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(setup, "FONT.OTF", k_ra8_fs_mode_write, &wf));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(wf, s_src_font, s_font_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(wf));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(setup));
}

/**
 * @brief Bring up the real SD-SPI driver, mount the card FAT and read the font.
 * @pre The card image holds FONT.OTF (sd_reflow_populate_card ran).
 * @post s_card_font holds the byte-identical font read back off the SD model.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void sd_reflow_read_font_back(void)
{
  s_card.image                          = s_disk.bytes;
  s_card.image_len                      = s_disk.block_count * (uint32_t)k_disk_block_size;
  const ra8_sdmmc_spi_transport_t trans = {.set_clock = t_set_clock,
                                           .cs        = t_cs,
                                           .xfer      = t_xfer,
                                           .ctx       = &s_card};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_init(&trans));

  ra8_fs_backend_t sd_backend = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdmmc_spi_bind_fs_backend(&sd_backend));
  ra8_fs_mount_t* mnt = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&sd_backend, &mnt));
  ra8_fs_file_t* rf = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mnt, "FONT.OTF", k_ra8_fs_mode_read, &rf));
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(rf, s_card_font, k_font_cap, &got));
  TEST_ASSERT_EQ(s_font_len, got);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(rf));
  TEST_ASSERT_EQ(0, memcmp(s_src_font, s_card_font, s_font_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(mnt));
}

/**
 * @brief Render a page with the SD-sourced font and assert non-blank output.
 * @pre s_card_font holds a valid font of length s_font_len.
 * @post At least one non-background pixel was inked into s_fb.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void sd_reflow_render_and_count(void)
{
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_init(s_fb, (uint16_t)k_fb_w, (uint16_t)k_fb_h, k_ra8_gfx_format_argb8888));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear((uint32_t)k_bg_argb));
  ra8_reflow_t engine = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init((uint16_t)k_fb_w,
                                 (uint16_t)k_fb_h,
                                 s_card_font,
                                 s_font_len,
                                 (uint16_t)k_font_px,
                                 0xFFFFFFFFU,
                                 0xFF4080FFU,
                                 &engine));
  const char* xhtml = "<html><body><p>This font was read off an SD card model "
                      "over the real SPI driver.</p></body></html>";
  uint32_t    pages = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_layout_chapter(&engine, (const uint8_t*)xhtml, strlen(xhtml), &pages));
  TEST_ASSERT(pages >= 1U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_render_page(&engine, 0U, nullptr));

  uint32_t ink = 0U;
  for (size_t i = 0U; i < (size_t)k_fb_w * (size_t)k_fb_h; ++i) {
    if (s_fb[i] != (uint32_t)k_bg_argb) {
      ++ink;
    }
  }
  (void)fprintf(stderr, "[info] %u-byte font via SD-SPI+FAT; %u inked pixels\n", s_font_len, ink);
  TEST_ASSERT(ink > 0U);
}

/**
 * @test test_sd_card_serves_font_to_reflow
 *
 * @par MC/DC:
 * Integration test -- no compound decision of its own. It exercises the
 * already-covered ra8_sdmmc_spi / ra8_fs / ra8_reflow APIs end to end through
 * a modelled SD card. The single boolean checks (init ok, bytes match, ink
 * present) each pass on the success path and fail the assert otherwise.
 */
static void test_sd_card_serves_font_to_reflow(void)
{
  TEST_BEGIN("SD card (SPI) serves FAT font -> ra8_fs -> ra8_reflow");

  s_src_font  = (uint8_t*)malloc(k_font_cap);
  s_card_font = (uint8_t*)malloc(k_font_cap);
  s_fb        = (uint32_t*)malloc((size_t)k_fb_w * (size_t)k_fb_h * sizeof(uint32_t));
  TEST_ASSERT_NOT_NULL(s_src_font);
  TEST_ASSERT_NOT_NULL(s_card_font);
  TEST_ASSERT_NOT_NULL(s_fb);

  if (!load_src_font()) {
    (void)fprintf(stderr, "[SKIP] Literata font not found; skipping\n");
    TEST_END("SD card (SPI) serves FAT font -> ra8_fs -> ra8_reflow");
    return;
  }

  /* Populate the card image: format + write the font via the direct mem
   * backend (the SD model only needs to serve reads). */
  sd_reflow_populate_card();
  /* Bring up the SD card and read the font back off the FAT it serves. */
  sd_reflow_read_font_back();
  /* Render with the font that came off the (modelled) SD card. */
  sd_reflow_render_and_count();

  (void)ra8_sdmmc_spi_deinit();
  free(s_disk.bytes);
  free(s_src_font);
  free(s_card_font);
  free(s_fb);
  TEST_END("SD card (SPI) serves FAT font -> ra8_fs -> ra8_reflow");
}

int32_t main(void)
{
  test_sd_card_serves_font_to_reflow();
  (void)fprintf(stderr, "[OK ] test_ra8_sdmmc_card_reflow.c\n");
  return 0;
}
