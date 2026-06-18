/**
 * @file ra_sdmmc_spi.c
 * @brief SD card driver in SPI-mode -- protocol implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Implementation of the SPI-mode SD card driver declared in
 * ``ra_sdmmc_spi.h``. The protocol follows SD Specification Part 1
 * Physical Layer Simplified Specification v9.10 section 7
 * ("SPI Mode"). Command framing, response classes (R1, R1b, R3, R7),
 * data tokens, CRC7 and CRC16 polynomials, and the v1.x vs v2.x card
 * discrimination flow are all documented inline at their first use.
 *
 * The driver is transport-agnostic: every byte exchange goes through
 * the caller-supplied ``ra_sdmmc_spi_transport_t``. This lets the host
 * tests inject a mock and the firmware app inject the real ``ra_spi``
 * HAL driver through a thin shim that lives in the example app.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_sdmmc_spi.h"

#include <stdint.h>
#include <string.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

/* ---------------------------------------------------------------------------
 * Log tag
 * ---------------------------------------------------------------------------
 */

/**
 * @var s_tag
 * @brief Log tag for diagnostics emitted by this module.
 * @note File-scope, read-only after init.
 * @since 0.1.0
 */
static const char* s_tag = "SDSPI";

/* ---------------------------------------------------------------------------
 * Protocol constants (SD spec PHY v9 section 7)
 * ---------------------------------------------------------------------------
 */

/**
 * @enum sd_cmd_t
 * @brief Wire-byte for every command used by this driver.
 *
 * @details
 * The wire byte is ``0x40 | cmd_index`` per SD spec PHY v9 section 7.3.1.1
 * "Command Format" (bit 7 = 0, bit 6 = 1, bits 5..0 = command index).
 */
/** @brief SD CSD field masks/shifts and command framing. */
typedef enum : uint32_t {
  k_sd_cmd_frame_len  = 5U,    /**< Command bytes preceding the CRC7. */
  k_sd_csize_msb_mask = 0x3FU, /**< CSD v2 C_SIZE MSB field (6-bit). */
  k_sd_read_bl_mask   = 0x0FU, /**< READ_BL_LEN (4-bit). */
  k_sd_csize_shift    = 10U,   /**< CSD v1 C_SIZE high-bits shift. */
  k_sd_csize_lo_mask  = 0xC0U, /**< CSD v1 C_SIZE low 2 bits (byte 8). */
  k_sd_mult_lo_mask   = 0x80U, /**< C_SIZE_MULT low bit (byte 10). */
  k_sd_mult_shift     = 7U,    /**< C_SIZE_MULT low-bit shift. */
} sd_csd_field_t;

typedef enum : uint8_t {
  k_sd_cmd_go_idle_state           = 0x40U,       /**< CMD0  GO_IDLE_STATE (0x40 | 0). */
  k_sd_cmd_send_if_cond            = 0x40U | 8U,  /**< CMD8  SEND_IF_COND         */
  k_sd_cmd_send_csd                = 0x40U | 9U,  /**< CMD9  SEND_CSD             */
  k_sd_cmd_send_cid                = 0x40U | 10U, /**< CMD10 SEND_CID             */
  k_sd_cmd_stop_transmission       = 0x40U | 12U, /**< CMD12 STOP_TRANSMISSION    */
  k_sd_cmd_set_blocklen            = 0x40U | 16U, /**< CMD16 SET_BLOCKLEN         */
  k_sd_cmd_read_single_block       = 0x40U | 17U, /**< CMD17 READ_SINGLE_BLOCK    */
  k_sd_cmd_read_multi_block        = 0x40U | 18U, /**< CMD18 READ_MULTIPLE_BLOCK  */
  k_sd_cmd_write_single_block      = 0x40U | 24U, /**< CMD24 WRITE_BLOCK          */
  k_sd_cmd_write_multi_block       = 0x40U | 25U, /**< CMD25 WRITE_MULTIPLE_BLOCK */
  k_sd_cmd_app_cmd                 = 0x40U | 55U, /**< CMD55 APP_CMD              */
  k_sd_cmd_read_ocr                = 0x40U | 58U, /**< CMD58 READ_OCR             */
  k_sd_acmd_sd_send_op_cond        = 0x40U | 41U, /**< ACMD41 SD_SEND_OP_COND     */
  k_sd_acmd_set_wr_blk_erase_count = 0x40U | 23U, /**< ACMD23 pre-erase count */
} sd_cmd_t;

/**
 * @enum sd_r1_bit_t
 * @brief R1 response bit definitions (SD spec PHY v9 section 7.3.2.1).
 */
typedef enum : uint8_t {
  k_sd_r1_idle_state           = 0x01U,
  k_sd_r1_erase_reset          = 0x02U,
  k_sd_r1_illegal_command      = 0x04U,
  k_sd_r1_com_crc_error        = 0x08U,
  k_sd_r1_erase_sequence_error = 0x10U,
  k_sd_r1_address_error        = 0x20U,
  k_sd_r1_parameter_error      = 0x40U,
  /* The top bit (0x80) is always zero -- used as the R1 sentinel. */
  k_sd_r1_sentinel = 0x80U,
} sd_r1_bit_t;

/**
 * @enum sd_data_token_t
 * @brief Data tokens used for block I/O (SD spec PHY v9 section 7.3.3).
 */
typedef enum : uint8_t {
  k_sd_token_data_start_single = 0xFEU, /**< Single-block read / single-block write start. */
  k_sd_token_data_start_multi  = 0xFCU, /**< Multi-block write start.                       */
  k_sd_token_stop_multi        = 0xFDU, /**< Multi-block write stop.                        */
  k_sd_token_idle              = 0xFFU, /**< Bus idle / CIPO high.                          */
} sd_data_token_t;

/**
 * @enum sd_data_response_t
 * @brief Data-response token bit layout (SD spec PHY v9 section 7.3.3.1).
 *
 * @details
 * Wire layout xxx0sss1 where sss = 010 "accepted", 101 "CRC error",
 * 110 "write error". Mask with ``k_sd_data_response_mask`` first.
 */
typedef enum : uint8_t {
  k_sd_data_response_mask      = 0x1FU,
  k_sd_data_response_accepted  = 0x05U, /**< 0b00101 -- data accepted.  */
  k_sd_data_response_crc_err   = 0x0BU, /**< 0b01011 -- CRC error.      */
  k_sd_data_response_write_err = 0x0DU, /**< 0b01101 -- write error.    */
} sd_data_response_t;

/**
 * @enum sd_protocol_const_t
 * @brief Magic argument values and retry budgets.
 */
typedef enum : uint32_t {
  /* CMD0 has a static CRC7-shifted-and-tagged byte of 0x95 (SD spec PHY
   * v9 section 7.3.1.1 example). Pre-computing avoids running CRC7 over
   * the all-zero argument every probe. */
  k_sd_crc7_cmd0_byte = 0x95U,
  /* CMD8 with argument 0x000001AA uses the documented "pattern 0xAA at
   * 2.7-3.6 V" check (SD spec PHY v9 section 7.3.2.6). The CRC7-shifted
   * byte for this exact frame is 0x87, also published in the spec. */
  k_sd_cmd8_arg_check_pattern = 0x000001AAUL,
  k_sd_crc7_cmd8_byte         = 0x87U,
  /* ACMD41 with HCS=1 for v2.x cards. */
  k_sd_acmd41_arg_hcs = 0x40000000UL,
  /* OCR bit set when CCS = 1 (block-addressed SDHC/SDXC). */
  k_sd_ocr_ccs_bit  = 0x40000000UL,
  k_sd_ocr_busy_bit = 0x80000000UL,
  /* Retry budgets -- bounded loops, NASA P10 Rule 2. */
  k_sd_max_r1_wait_bytes    = 16U, /**< R1 must appear within 8 bytes per spec; allow 2x slack. */
  k_sd_max_data_token_polls = 50000U,  /**< ~500 ms at 100 us / poll. */
  k_sd_max_busy_poll_bytes  = 100000U, /**< Worst-case write timeout. */
  k_sd_max_acmd41_attempts  = 1000U,   /**< 1 s at 1 ms / attempt. */
  k_sd_init_dummy_clocks    = 80U,     /**< 80 clocks = 10 bytes of 0xFF (>=74 required). */
  k_sd_recover_flush_bytes  = 530U,    /**< >= 512 data + 2 CRC + token to flush a stuck write. */
  k_sd_recover_idle_bytes   = 64U,     /**< 512 CS-released clocks to drain busy + reset framing. */
  k_sd_max_recover_attempts = 4U,      /**< Re-flush + retry CMD0 this many times before failing. */
} sd_protocol_const_t;

/**
 * @enum sd_cmd_arg_idx_t
 * @brief Byte indices inside a 6-byte SD command frame.
 *
 * @details
 * Frame layout (SD spec PHY v9 section 7.3.1.1 "Command Format"):
 * byte 0 = 0x40 | cmd_index; bytes 1..4 = arg MSB..LSB; byte 5 =
 * (CRC7 << 1) | 1.
 */
typedef enum : uint8_t {
  k_sd_frame_idx_cmd       = 0U,
  k_sd_frame_idx_arg_msb   = 1U,
  k_sd_frame_idx_arg_byte2 = 2U,
  k_sd_frame_idx_arg_byte1 = 3U,
  k_sd_frame_idx_arg_lsb   = 4U,
  k_sd_frame_idx_crc       = 5U,
} sd_cmd_arg_idx_t;

/**
 * @enum sd_csd_layout_t
 * @brief Selected CSD-byte indices and bit shifts (SD spec PHY v9 section 5).
 */
typedef enum : uint8_t {
  /* CSD version lives in byte 0 bits 7:6: 0 = CSD v1, 1 = CSD v2. */
  k_sd_csd_byte_version  = 0U,
  k_sd_csd_version_shift = 6U,
  /* CSD v2: C_SIZE is a 22-bit field spanning bytes 7..9. */
  k_sd_csd_v2_byte_csize_msb = 7U,
  k_sd_csd_v2_byte_csize_mid = 8U,
  k_sd_csd_v2_byte_csize_lsb = 9U,
  /* Bit shift to go from C_SIZE to block count: ((C_SIZE + 1) * 1024). */
  k_sd_csd_v2_blocks_shift = 10U,
} sd_csd_layout_t;

/**
 * @enum sd_bit_shift_t
 * @brief Bit-shift constants reused across byte packing.
 */
typedef enum : uint8_t {
  k_sd_bit_byte       = 8U,
  k_sd_bit_two_byte   = 16U,
  k_sd_bit_three_byte = 24U,
} sd_bit_shift_t;

/**
 * @enum sd_crc7_const_t
 * @brief CRC7 polynomial and mask constants (SD spec PHY v9 section 4.5).
 *
 * @details
 * CRC7 generator polynomial G(x) = x^7 + x^3 + 1. Stored as a 7-bit
 * value in the low bits of a byte; the mask keeps the working CRC
 * register inside 7 bits while shifting.
 */
typedef enum : uint8_t {
  k_sd_crc7_msb_test      = 0x40U, /**< Top bit of the 7-bit working CRC. */
  k_sd_crc7_byte_msb      = 0x80U, /**< Top bit of an input byte.         */
  k_sd_crc7_register_mask = 0x7FU, /**< Mask the 7-bit working register.  */
  k_sd_crc7_poly_low7     = 0x09U, /**< G(x) low 7 bits: 0b0001001.       */
} sd_crc7_const_t;

/**
 * @enum sd_crc16_const_t
 * @brief CRC16-CCITT polynomial and mask constants (SD spec PHY v9 section 4.5).
 *
 * @details
 * CRC16-CCITT generator polynomial G(x) = x^16 + x^12 + x^5 + 1
 * encoded as 0x1021 with the implicit x^16 term. ``k_sd_crc16_msb_test``
 * isolates the top bit of the 16-bit working register so the test can
 * decide whether to XOR the polynomial in.
 */
typedef enum : uint16_t {
  k_sd_crc16_msb_test = 0x8000U, /**< Top bit of the 16-bit working CRC. */
  k_sd_crc16_poly     = 0x1021U, /**< CRC16-CCITT polynomial.            */
} sd_crc16_const_t;

/**
 * @enum sd_byte_mask_t
 * @brief Generic byte-extraction masks used across argument packing.
 *
 * @details
 * ``k_sd_mask_byte`` isolates the low 8 bits when packing a 32-bit
 * command argument into the 4 big-endian frame bytes (SD spec PHY v9
 * section 7.3.1.1 "Command Format") or splitting a 16-bit CRC16 into
 * its two on-wire bytes. ``k_sd_mask_12bit`` is the CMD8 echo-check
 * mask (SD spec PHY v9 section 7.3.2.6: bits [11:0] of the R7 echo
 * carry the voltage-range code (bits 11:8) and the host check pattern
 * (bits 7:0)).
 */
typedef enum : uint32_t {
  k_sd_mask_byte  = 0xFFU,  /**< Low 8 bits of a wider value.              */
  k_sd_mask_12bit = 0xFFFU, /**< CMD8 R7 echo voltage-range + check pattern. */
} sd_byte_mask_t;

/* ---------------------------------------------------------------------------
 * Driver state (single global instance -- one card per board today)
 * ---------------------------------------------------------------------------
 */

/**
 * @struct sd_state_t
 * @brief Cached protocol state held across the driver public API.
 *
 * @invariant ``initialized == true`` iff CMD0 ... CMD16 init succeeded.
 */
typedef struct {
  ra_sdmmc_spi_transport_t transport;       /**< Bound transport callbacks.            */
  ra_sdmmc_spi_card_type_t card_type;       /**< Detected card class.                  */
  uint32_t                 capacity_blocks; /**< 512-byte block count.               */
  bool                     initialized;     /**< True once the SD init sequence ran.   */
} sd_state_t;

/**
 * @var s_state
 * @brief Single driver instance.
 * @note Internal mutable state; access from a single thread only.
 * @since 0.1.0
 */
static sd_state_t s_state = {};

/* ===========================================================================
 * CRC helpers (public for unit-test coverage)
 * ===========================================================================
 */

uint8_t ra_sdmmc_spi_crc7(const uint8_t* data, uint32_t len)
{
  uint8_t crc = 0U;
  if (data == nullptr) {
    return 0U;
  }
  for (uint32_t i = 0U; i < len; i++) {
    uint8_t byte = data[i];
    for (uint8_t b = 0U; b < (uint8_t)k_sd_bit_byte; b++) {
      const uint8_t top = (uint8_t)((crc & (uint8_t)k_sd_crc7_msb_test) ^
                                    ((byte & (uint8_t)k_sd_crc7_byte_msb) >> 1U));
      crc               = (uint8_t)((crc << 1U) & (uint8_t)k_sd_crc7_register_mask);
      if (top != 0U) {
        crc ^= (uint8_t)k_sd_crc7_poly_low7;
      }
      byte = (uint8_t)(byte << 1U);
    }
  }
  return crc;
}

uint16_t ra_sdmmc_spi_crc16(const uint8_t* data, uint32_t len)
{
  uint16_t crc = 0U;
  if (data == nullptr) {
    return 0U;
  }
  for (uint32_t i = 0U; i < len; i++) {
    crc ^= (uint16_t)((uint16_t)data[i] << k_sd_bit_byte);
    for (uint8_t b = 0U; b < (uint8_t)k_sd_bit_byte; b++) {
      if ((crc & (uint16_t)k_sd_crc16_msb_test) != 0U) {
        crc = (uint16_t)((crc << 1U) ^ (uint16_t)k_sd_crc16_poly);
      } else {
        crc = (uint16_t)(crc << 1U);
      }
    }
  }
  return crc;
}

/* ===========================================================================
 * Low-level byte exchange wrappers
 * ===========================================================================
 */

/* Shift out one byte and capture the response -- see implementation for details. */
static ra_err_t internal_xfer_one(uint8_t tx, uint8_t* rx)
{
  const uint8_t tx_buf[1] = {tx};
  uint8_t       rx_buf[1] = {};
  ra_err_t      err       = s_state.transport.xfer(s_state.transport.ctx, tx_buf, rx_buf, 1U);
  if (err != k_ra_ok) {
    return err;
  }
  if (rx != nullptr) {
    *rx = rx_buf[0];
  }
  return k_ra_ok;
}

/* Shift ``n`` idle bytes (0xFF) and discard the response -- see implementation for details. */
static ra_err_t internal_send_idle(uint32_t n)
{
  uint8_t byte;
  for (uint32_t i = 0U; i < n; i++) {
    ra_err_t err = internal_xfer_one((uint8_t)k_sd_token_idle, &byte);
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

/* Drive CS low and clock a single idle byte so CIPO is sampled -- see implementation for details. */
static ra_err_t internal_cs_assert(void)
{
  ra_err_t err = s_state.transport.cs(s_state.transport.ctx, true);
  if (err != k_ra_ok) {
    return err;
  }
  return internal_send_idle(1U);
}

/* Drive CS high after clocking one idle byte (SD spec section 7.2.4) -- see implementation for details. */
static ra_err_t internal_cs_release(void)
{
  ra_err_t err = s_state.transport.cs(s_state.transport.ctx, false);
  if (err != k_ra_ok) {
    return err;
  }
  /* Clock 8 idle cycles after CS release so the card can finish any
   * pending internal state transition (SD spec PHY v9 section 7.2.4). */
  return internal_send_idle(1U);
}

/* ===========================================================================
 * Command framing
 * ===========================================================================
 */

/* Build a 6-byte command frame in ``out_frame`` -- see implementation for details. */
static void internal_build_frame(sd_cmd_t cmd, uint32_t arg, uint8_t* out_frame)
{
  out_frame[k_sd_frame_idx_cmd] = (uint8_t)cmd;
  out_frame[k_sd_frame_idx_arg_msb] =
    (uint8_t)((arg >> k_sd_bit_three_byte) & (uint32_t)k_sd_mask_byte);
  out_frame[k_sd_frame_idx_arg_byte2] =
    (uint8_t)((arg >> k_sd_bit_two_byte) & (uint32_t)k_sd_mask_byte);
  out_frame[k_sd_frame_idx_arg_byte1] =
    (uint8_t)((arg >> k_sd_bit_byte) & (uint32_t)k_sd_mask_byte);
  out_frame[k_sd_frame_idx_arg_lsb] = (uint8_t)(arg & (uint32_t)k_sd_mask_byte);
  if (cmd == k_sd_cmd_go_idle_state) {
    out_frame[k_sd_frame_idx_crc] = (uint8_t)k_sd_crc7_cmd0_byte;
  } else if ((cmd == k_sd_cmd_send_if_cond) && (arg == (uint32_t)k_sd_cmd8_arg_check_pattern)) {
    out_frame[k_sd_frame_idx_crc] = (uint8_t)k_sd_crc7_cmd8_byte;
  } else {
    const uint8_t crc7            = ra_sdmmc_spi_crc7(out_frame, k_sd_cmd_frame_len);
    out_frame[k_sd_frame_idx_crc] = (uint8_t)((crc7 << 1U) | 1U);
  }
}

/* Wait for the R1 token (first byte without the sentinel bit set) -- see implementation for details. */
static ra_err_t internal_read_r1(uint8_t* out_r1)
{
  for (uint32_t i = 0U; i < (uint32_t)k_sd_max_r1_wait_bytes; i++) {
    uint8_t  byte = 0U;
    ra_err_t err  = internal_xfer_one((uint8_t)k_sd_token_idle, &byte);
    if (err != k_ra_ok) {
      return err;
    }
    if ((byte & (uint8_t)k_sd_r1_sentinel) == 0U) {
      *out_r1 = byte;
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/* Send a command frame and capture the R1 response byte -- see implementation for details. */
static ra_err_t internal_send_command(sd_cmd_t cmd, uint32_t arg, uint8_t* out_r1)
{
  uint8_t frame[k_ra_sdmmc_spi_cmd_frame_bytes];
  internal_build_frame(cmd, arg, frame);
  uint8_t  rx_dummy[k_ra_sdmmc_spi_cmd_frame_bytes];
  ra_err_t err = s_state.transport.xfer(s_state.transport.ctx,
                                        frame,
                                        rx_dummy,
                                        (uint32_t)k_ra_sdmmc_spi_cmd_frame_bytes);
  if (err != k_ra_ok) {
    return err;
  }
  return internal_read_r1(out_r1);
}

/* Send an ACMD by prefixing it with CMD55 (APP_CMD) -- see implementation for details. */
static ra_err_t internal_send_acmd(sd_cmd_t acmd, uint32_t arg, uint8_t* out_r1)
{
  uint8_t  r1  = 0U;
  ra_err_t err = internal_send_command(k_sd_cmd_app_cmd, 0U, &r1);
  if (err != k_ra_ok) {
    return err;
  }
  /* CMD55 is allowed to leave R1 == idle (0x01) during the init loop. */
  return internal_send_command(acmd, arg, out_r1);
}

/* ===========================================================================
 * R7 / R3 extension responses (CMD8, CMD58)
 * ===========================================================================
 */

/* Drain the four trailing bytes of an R3 / R7 response -- see implementation for details. */
static ra_err_t internal_read_r3_or_r7_tail(uint32_t* out_word)
{
  uint8_t  bytes[4] = {};
  ra_err_t err      = s_state.transport.xfer(s_state.transport.ctx, nullptr, bytes, 4U);
  if (err != k_ra_ok) {
    /* Some transports do not allow NULL tx; fall back to per-byte. */
    for (uint32_t i = 0U; i < 4U; i++) {
      err = internal_xfer_one((uint8_t)k_sd_token_idle, &bytes[i]);
      if (err != k_ra_ok) {
        return err;
      }
    }
  }
  *out_word = ((uint32_t)bytes[0] << k_sd_bit_three_byte) |
              ((uint32_t)bytes[1] << k_sd_bit_two_byte) | ((uint32_t)bytes[2] << k_sd_bit_byte) |
              (uint32_t)bytes[3];
  return k_ra_ok;
}

/* ===========================================================================
 * Data-token I/O
 * ===========================================================================
 */

/* Poll for the data-start token (0xFE), bounded by a retry budget -- see implementation for details. */
static ra_err_t internal_wait_data_token(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_sd_max_data_token_polls; i++) {
    uint8_t  byte = 0U;
    ra_err_t err  = internal_xfer_one((uint8_t)k_sd_token_idle, &byte);
    if (err != k_ra_ok) {
      return err;
    }
    if (byte == (uint8_t)k_sd_token_data_start_single) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/* Wait for the card to release the busy token (CIPO returns to 0xFF) -- see implementation for details. */
static ra_err_t internal_wait_not_busy(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_sd_max_busy_poll_bytes; i++) {
    uint8_t  byte = 0U;
    ra_err_t err  = internal_xfer_one((uint8_t)k_sd_token_idle, &byte);
    if (err != k_ra_ok) {
      return err;
    }
    if (byte == (uint8_t)k_sd_token_idle) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/* ===========================================================================
 * CSD parsing (capacity decode)
 * ===========================================================================
 */

/* Compute capacity (in 512-byte blocks) from a 16-byte CSD register -- see implementation for details. */
static uint32_t internal_csd_to_blocks(const uint8_t* csd)
{
  const uint8_t version = (uint8_t)((csd[k_sd_csd_byte_version] >> k_sd_csd_version_shift) & 0x03U);
  if (version == 1U) {
    /* CSD v2: ``capacity = (C_SIZE + 1) * 512 * 1024 / 512``. */
    const uint32_t c_size =
      (((uint32_t)csd[k_sd_csd_v2_byte_csize_msb] & k_sd_csize_msb_mask) << k_sd_bit_two_byte) |
      ((uint32_t)csd[k_sd_csd_v2_byte_csize_mid] << k_sd_bit_byte) |
      (uint32_t)csd[k_sd_csd_v2_byte_csize_lsb];
    return (c_size + 1U) << (uint32_t)k_sd_csd_v2_blocks_shift;
  }
  if (version == 0U) {
    /* CSD v1 (SD spec PHY v9 section 5.3.2):
     *   capacity = (C_SIZE+1) * 2^(C_SIZE_MULT+2) * 2^READ_BL_LEN / 512.
     * C_SIZE is a 12-bit field spanning csd[6][1:0] (high 2 bits),
     * csd[7] (middle 8 bits), csd[8][7:6] (low 2 bits). */
    const uint8_t  read_bl_len = (uint8_t)(csd[5] & k_sd_read_bl_mask);
    const uint32_t c_size      = (((uint32_t)csd[6] & 0x03U) << k_sd_csize_shift) |
                                 ((uint32_t)csd[7] << 2U) |
                                 ((uint32_t)(csd[8] & k_sd_csize_lo_mask) >> 6U);
    const uint8_t  c_size_mult =
      (uint8_t)((((uint8_t)csd[9] & 0x03U) << 1U) |
                (((uint8_t)csd[10] & k_sd_mult_lo_mask) >> k_sd_mult_shift));
    const uint32_t mult      = (uint32_t)1U << ((uint32_t)c_size_mult + 2U);
    const uint32_t block_len = (uint32_t)1U << (uint32_t)read_bl_len;
    /* Multiply in 64 bits to avoid overflow before dividing by 512. */
    const uint64_t bytes = (uint64_t)(c_size + 1U) * (uint64_t)mult * (uint64_t)block_len;
    return (uint32_t)(bytes / (uint64_t)k_ra_sdmmc_spi_block_size);
  }
  return 0U;
}

/* ===========================================================================
 * Init sequence
 * ===========================================================================
 */

/* Validate the transport descriptor: every callback non-NULL -- see implementation for details. */
static ra_err_t internal_validate_transport(const ra_sdmmc_spi_transport_t* transport)
{
  if (transport == nullptr) {
    return k_ra_err_null_ptr;
  }
  if ((transport->set_clock == nullptr) || (transport->cs == nullptr) ||
      (transport->xfer == nullptr)) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/**
 * @brief Best-effort recovery for a card stranded mid-write by an interrupted
 *        transaction (e.g. an MCU reset during a long format).
 *
 * @details Four phases, each safe to run against an already-idle card (0xFD and
 * 0xFF are both non-command bytes that clock past as idle, and CMD12 on an idle
 * card is a no-op that returns to idle):
 * 1. CS released, a long idle burst -- lets a card still BUSY (programming) from
 *    the interrupted write drain its internal timer, and resets the card's byte
 *    framing for the next CS assertion (a reset mid-byte can leave it misaligned).
 * 2. CS asserted, a stop-tran token then >= 512 + CRC idle bytes -- aborts a
 *    wedged CMD25 multi-block write and completes a wedged CMD24 single-block
 *    data phase, so the card programs the slack and returns to the transfer
 *    state; then wait out the programming.
 * 3. CMD12 STOP_TRANSMISSION -- aborts any open multi-block transfer the card
 *    still believes is active; wait out the trailing busy.
 * 4. CS released, a final idle burst to settle framing before the wake/CMD0.
 *
 * Run before each wake/CMD0 retry so a stuck card can re-init without a physical
 * power cycle. (Limits: cannot recover a card whose controller needs a true
 * power-on reset, since the MCU cannot remove card VBUS.)
 *
 * @return None.
 * @pre The transport is bound (called from the init probe).
 * @post Any in-flight write/read transaction is terminated and the bus is idle.
 * @post CS is released.
 * @note Not thread-safe; part of single-threaded init.
 * @since 0.1.0
 */
static void internal_recover_stuck_card(void)
{
  /* Phase 1: drain busy + reset byte framing with CS released. */
  (void)s_state.transport.cs(s_state.transport.ctx, false);
  (void)internal_send_idle((uint32_t)k_sd_recover_idle_bytes);

  /* Phase 2: complete/abort a wedged data phase with CS asserted. */
  if (s_state.transport.cs(s_state.transport.ctx, true) != k_ra_ok) {
    return;
  }
  (void)internal_xfer_one((uint8_t)k_sd_token_stop_multi, nullptr);
  (void)internal_send_idle((uint32_t)k_sd_recover_flush_bytes);
  (void)internal_wait_not_busy();

  /* Phase 3: explicit STOP_TRANSMISSION for any open multi-block transfer. */
  uint8_t r1 = 0U;
  (void)internal_send_command(k_sd_cmd_stop_transmission, 0U, &r1);
  (void)internal_wait_not_busy();
  (void)s_state.transport.cs(s_state.transport.ctx, false);

  /* Phase 4: settle framing before the caller's wake/CMD0. */
  (void)internal_send_idle((uint32_t)k_sd_recover_idle_bytes);
}

/* Drive >= 74 dummy clocks with CS high to wake the card (SD spec section 7.2.1) -- see implementation for details. */
static ra_err_t internal_wake_card(void)
{
  ra_err_t err = s_state.transport.cs(s_state.transport.ctx, false);
  if (err != k_ra_ok) {
    return err;
  }
  return internal_send_idle((uint32_t)k_sd_init_dummy_clocks / (uint32_t)k_sd_bit_byte);
}

/* Send CMD0 (GO_IDLE_STATE) and require R1 == 0x01 -- see implementation for details. */
static ra_err_t internal_send_cmd0(void)
{
  ra_err_t err = internal_cs_assert();
  if (err != k_ra_ok) {
    return err;
  }
  uint8_t r1 = 0U;
  err        = internal_send_command(k_sd_cmd_go_idle_state, 0U, &r1);
  (void)internal_cs_release();
  if (err != k_ra_ok) {
    return err;
  }
  if (r1 != (uint8_t)k_sd_r1_idle_state) {
    return k_ra_err_protocol_error;
  }
  return k_ra_ok;
}

/* Send CMD8 (SEND_IF_COND) and classify the card as v1.x / v2.x -- see implementation for details. */
static ra_err_t internal_send_cmd8(bool* out_is_v2)
{
  ra_err_t err = internal_cs_assert();
  if (err != k_ra_ok) {
    return err;
  }
  uint8_t r1 = 0U;
  err = internal_send_command(k_sd_cmd_send_if_cond, (uint32_t)k_sd_cmd8_arg_check_pattern, &r1);
  if (err != k_ra_ok) {
    (void)internal_cs_release();
    return err;
  }
  if ((r1 & (uint8_t)k_sd_r1_illegal_command) != 0U) {
    *out_is_v2 = false;
    (void)internal_cs_release();
    return k_ra_ok;
  }
  uint32_t echo = 0U;
  err           = internal_read_r3_or_r7_tail(&echo);
  (void)internal_cs_release();
  if (err != k_ra_ok) {
    return err;
  }
  if ((echo & (uint32_t)k_sd_mask_12bit) !=
      ((uint32_t)k_sd_cmd8_arg_check_pattern & (uint32_t)k_sd_mask_12bit)) {
    return k_ra_err_protocol_error;
  }
  *out_is_v2 = true;
  return k_ra_ok;
}

/* Loop ACMD41 until the card reports ready (R1 idle bit clear) -- see implementation for details. */
static ra_err_t internal_acmd41_loop(bool is_v2)
{
  const uint32_t arg = is_v2 ? (uint32_t)k_sd_acmd41_arg_hcs : 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_sd_max_acmd41_attempts; i++) {
    ra_err_t err = internal_cs_assert();
    if (err != k_ra_ok) {
      return err;
    }
    uint8_t r1 = 0U;
    err        = internal_send_acmd(k_sd_acmd_sd_send_op_cond, arg, &r1);
    (void)internal_cs_release();
    if (err != k_ra_ok) {
      return err;
    }
    if ((r1 & (uint8_t)k_sd_r1_idle_state) == 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_init_failed;
}

/* Send CMD58 (READ_OCR) and capture the CCS bit -- see implementation for details. */
static ra_err_t internal_read_ocr(bool* out_is_hc)
{
  ra_err_t err = internal_cs_assert();
  if (err != k_ra_ok) {
    return err;
  }
  uint8_t r1 = 0U;
  err        = internal_send_command(k_sd_cmd_read_ocr, 0U, &r1);
  if (err != k_ra_ok) {
    (void)internal_cs_release();
    return err;
  }
  uint32_t ocr = 0U;
  err          = internal_read_r3_or_r7_tail(&ocr);
  (void)internal_cs_release();
  if (err != k_ra_ok) {
    return err;
  }
  *out_is_hc = ((ocr & (uint32_t)k_sd_ocr_ccs_bit) != 0U);
  return k_ra_ok;
}

/* Send CMD9 (SEND_CSD) and parse capacity -- see implementation for details. */
static ra_err_t internal_read_csd(uint32_t* out_blocks)
{
  ra_err_t err = internal_cs_assert();
  if (err != k_ra_ok) {
    return err;
  }
  uint8_t r1 = 0U;
  err        = internal_send_command(k_sd_cmd_send_csd, 0U, &r1);
  if (err != k_ra_ok) {
    (void)internal_cs_release();
    return err;
  }
  if (r1 != 0U) {
    (void)internal_cs_release();
    return k_ra_err_protocol_error;
  }
  err = internal_wait_data_token();
  if (err != k_ra_ok) {
    (void)internal_cs_release();
    return err;
  }
  uint8_t csd[k_ra_sdmmc_spi_csd_response_len] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_ra_sdmmc_spi_csd_response_len; i++) {
    err = internal_xfer_one((uint8_t)k_sd_token_idle, &csd[i]);
    if (err != k_ra_ok) {
      (void)internal_cs_release();
      return err;
    }
  }
  /* Drain trailing CRC16 (2 bytes). */
  uint8_t crc_bytes[2] = {};
  (void)internal_xfer_one((uint8_t)k_sd_token_idle, &crc_bytes[0]);
  (void)internal_xfer_one((uint8_t)k_sd_token_idle, &crc_bytes[1]);
  (void)internal_cs_release();
  *out_blocks = internal_csd_to_blocks(csd);
  if (*out_blocks == 0U) {
    return k_ra_err_protocol_error;
  }
  return k_ra_ok;
}

/* Send CMD16 (SET_BLOCKLEN, 512) to lock the block size -- see implementation for details. */
static ra_err_t internal_set_block_len(void)
{
  ra_err_t err = internal_cs_assert();
  if (err != k_ra_ok) {
    return err;
  }
  uint8_t r1 = 0U;
  err = internal_send_command(k_sd_cmd_set_blocklen, (uint32_t)k_ra_sdmmc_spi_block_size, &r1);
  (void)internal_cs_release();
  if (err != k_ra_ok) {
    return err;
  }
  if (r1 != 0U) {
    return k_ra_err_protocol_error;
  }
  return k_ra_ok;
}

/* Classify the detected card from the v2 / HC flags -- see implementation for details. */
static ra_sdmmc_spi_card_type_t internal_classify_card(bool is_v2, bool is_hc)
{
  if (is_hc) {
    return k_ra_sdmmc_spi_type_sdhc;
  }
  if (is_v2) {
    return k_ra_sdmmc_spi_type_sdv2;
  }
  return k_ra_sdmmc_spi_type_sdv1;
}

/* Drive the CMD0..CMD8..ACMD41..CMD58 probe sequence (no CSD/CMD16) -- see implementation for details. */
static ra_err_t internal_probe_card(bool* out_is_v2, bool* out_is_hc)
{
  ra_err_t err = internal_wake_card();
  if (err == k_ra_ok) {
    err = internal_send_cmd0();
  }
  /* CMD0 failed: the card may be stranded mid-write from an interrupted
   * transaction. Flush it and retry, escalating across a few attempts. A
   * healthy card answers CMD0 on the first pass and never enters this loop, so
   * the recovery cannot disturb a normal bring-up. */
  for (uint32_t attempt = 0U; (err != k_ra_ok) && (attempt < (uint32_t)k_sd_max_recover_attempts);
       attempt++) {
    internal_recover_stuck_card();
    err = internal_wake_card();
    if (err == k_ra_ok) {
      err = internal_send_cmd0();
    }
  }
  if (err != k_ra_ok) {
    return err;
  }
  err = internal_send_cmd8(out_is_v2);
  if (err != k_ra_ok) {
    return err;
  }
  err = internal_acmd41_loop(*out_is_v2);
  if (err != k_ra_ok) {
    return err;
  }
  *out_is_hc = false;
  if (*out_is_v2) {
    err = internal_read_ocr(out_is_hc);
  }
  return err;
}

/* Walk the full SD identification sequence and learn capacity / type -- see implementation for details. */
static ra_err_t internal_run_init_sequence(void)
{
  bool     is_v2 = false;
  bool     is_hc = false;
  ra_err_t err   = internal_probe_card(&is_v2, &is_hc);
  if (err != k_ra_ok) {
    return err;
  }
  uint32_t blocks = 0U;
  err             = internal_read_csd(&blocks);
  RA_RETURN_ON_ERROR(err, s_tag, "CMD9 SEND_CSD");
  /* SDSC (v1.x or v2.x SDSC) needs an explicit block-size set; HC cards
   * accept it as a no-op but the spec says we should still issue it. */
  err = internal_set_block_len();
  RA_RETURN_ON_ERROR(err, s_tag, "CMD16 SET_BLOCKLEN");
  s_state.card_type       = internal_classify_card(is_v2, is_hc);
  s_state.capacity_blocks = blocks;
  return k_ra_ok;
}

/* ===========================================================================
 * Public API
 * ===========================================================================
 */

/* Bind the transport, validate non-reinit, then bring up the init clock -- see implementation for details. */
static ra_err_t internal_prepare_init(const ra_sdmmc_spi_transport_t* transport)
{
  if (s_state.initialized) {
    ra_log_error(s_tag, "already initialized");
    return k_ra_err_invalid_state;
  }
  s_state.transport       = *transport;
  s_state.card_type       = k_ra_sdmmc_spi_type_unknown;
  s_state.capacity_blocks = 0U;
  return s_state.transport.set_clock(s_state.transport.ctx, (uint32_t)k_ra_sdmmc_spi_clock_init_hz);
}

/* Drop the bus to data-rate clock and mark the driver initialized -- see implementation for details. */
static ra_err_t internal_finalize_init(void)
{
  ra_err_t err =
    s_state.transport.set_clock(s_state.transport.ctx, (uint32_t)k_ra_sdmmc_spi_clock_data_hz);
  if (err != k_ra_ok) {
    return err;
  }
  s_state.initialized = true;
  return k_ra_ok;
}

ra_err_t ra_sdmmc_spi_init(const ra_sdmmc_spi_transport_t* transport)
{
  ra_err_t err = internal_validate_transport(transport);
  RA_RETURN_ON_ERROR(err, s_tag, "transport invalid");
  err = internal_prepare_init(transport);
  RA_RETURN_ON_ERROR(err, s_tag, "prepare init");
  err = internal_run_init_sequence();
  RA_RETURN_ON_ERROR(err, s_tag, "SD init sequence");
  err = internal_finalize_init();
  RA_RETURN_ON_ERROR(err, s_tag, "finalize init");
  return k_ra_ok;
}

ra_err_t ra_sdmmc_spi_deinit(void)
{
  s_state.initialized     = false;
  s_state.card_type       = k_ra_sdmmc_spi_type_unknown;
  s_state.capacity_blocks = 0U;
  return k_ra_ok;
}

/* Convert the caller's LBA into the on-wire command argument -- see implementation for details. */
static uint32_t internal_lba_to_arg(uint32_t lba)
{
  if (s_state.card_type == k_ra_sdmmc_spi_type_sdhc) {
    return lba;
  }
  return lba * (uint32_t)k_ra_sdmmc_spi_block_size;
}

/* Drain 512 payload bytes from the card into ``buf`` -- see implementation for details. */
static ra_err_t internal_read_block_payload(uint8_t* buf)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ra_sdmmc_spi_block_size; i++) {
    ra_err_t err = internal_xfer_one((uint8_t)k_sd_token_idle, &buf[i]);
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

/* Drain the 2-byte CRC16 trailer from the card and verify it -- see implementation for details. */
static ra_err_t internal_read_block_crc_check(const uint8_t* buf)
{
  uint8_t crc_hi = 0U;
  uint8_t crc_lo = 0U;
  (void)internal_xfer_one((uint8_t)k_sd_token_idle, &crc_hi);
  (void)internal_xfer_one((uint8_t)k_sd_token_idle, &crc_lo);
  const uint16_t expected = ra_sdmmc_spi_crc16(buf, (uint32_t)k_ra_sdmmc_spi_block_size);
  const uint16_t actual   = (uint16_t)(((uint16_t)crc_hi << k_sd_bit_byte) | (uint16_t)crc_lo);
  if (expected != actual) {
    return k_ra_err_crc_mismatch;
  }
  return k_ra_ok;
}

/* Run the CMD17 + data-token + payload-drain phase under CS asserted -- see implementation for details. */
static ra_err_t internal_read_data_phase(uint32_t lba, uint8_t* buf)
{
  uint8_t  r1  = 0U;
  ra_err_t err = internal_send_command(k_sd_cmd_read_single_block, internal_lba_to_arg(lba), &r1);
  if (err != k_ra_ok) {
    return err;
  }
  if (r1 != 0U) {
    return k_ra_err_protocol_error;
  }
  err = internal_wait_data_token();
  if (err != k_ra_ok) {
    return err;
  }
  err = internal_read_block_payload(buf);
  if (err != k_ra_ok) {
    return err;
  }
  return internal_read_block_crc_check(buf);
}

ra_err_t ra_sdmmc_spi_read_block(uint32_t lba, uint8_t* buf)
{
  RA_CHECK_NULL_PTR(buf, s_tag, "buf is null");
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  if (lba >= s_state.capacity_blocks) {
    return k_ra_err_out_of_range;
  }
  ra_err_t err = internal_cs_assert();
  RA_RETURN_ON_ERROR(err, s_tag, "cs assert");
  err = internal_read_data_phase(lba, buf);
  (void)internal_cs_release();
  return err;
}

/* Stream one data block out (token + payload + CRC16) and check -- see implementation for details. */
static ra_err_t internal_write_data_block(const uint8_t* buf, uint8_t start_token)
{
  ra_err_t err = internal_send_idle(1U); /* N_WR pad (spec >= 1 byte). */
  if (err != k_ra_ok) {
    return err;
  }
  err = internal_xfer_one(start_token, nullptr);
  if (err != k_ra_ok) {
    return err;
  }
  err = s_state.transport.xfer(s_state.transport.ctx,
                               buf,
                               nullptr,
                               (uint32_t)k_ra_sdmmc_spi_block_size);
  if (err != k_ra_ok) {
    /* Some transports require non-NULL rx -- fall back to per-byte. */
    for (uint32_t i = 0U; i < (uint32_t)k_ra_sdmmc_spi_block_size; i++) {
      uint8_t dummy = 0U;
      err           = internal_xfer_one(buf[i], &dummy);
      if (err != k_ra_ok) {
        return err;
      }
    }
  }
  const uint16_t crc = ra_sdmmc_spi_crc16(buf, (uint32_t)k_ra_sdmmc_spi_block_size);
  (void)internal_xfer_one((uint8_t)((uint32_t)(crc >> k_sd_bit_byte) & (uint32_t)k_sd_mask_byte),
                          nullptr);
  (void)internal_xfer_one((uint8_t)((uint32_t)crc & (uint32_t)k_sd_mask_byte), nullptr);
  uint8_t response = 0U;
  err              = internal_xfer_one((uint8_t)k_sd_token_idle, &response);
  if (err != k_ra_ok) {
    return err;
  }
  if ((response & (uint8_t)k_sd_data_response_mask) != (uint8_t)k_sd_data_response_accepted) {
    (void)internal_wait_not_busy();
    return k_ra_err_protocol_error;
  }
  return internal_wait_not_busy();
}

ra_err_t ra_sdmmc_spi_write_block(uint32_t lba, const uint8_t* buf)
{
  RA_CHECK_NULL_PTR(buf, s_tag, "buf is null");
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  if (lba >= s_state.capacity_blocks) {
    return k_ra_err_out_of_range;
  }
  ra_err_t err = internal_cs_assert();
  RA_RETURN_ON_ERROR(err, s_tag, "cs assert");
  uint8_t r1 = 0U;
  err        = internal_send_command(k_sd_cmd_write_single_block, internal_lba_to_arg(lba), &r1);
  if (err != k_ra_ok) {
    (void)internal_cs_release();
    return err;
  }
  if (r1 != 0U) {
    (void)internal_cs_release();
    return k_ra_err_protocol_error;
  }
  err = internal_write_data_block(buf, (uint8_t)k_sd_token_data_start_single);
  (void)internal_cs_release();
  return err;
}

/* Stream @p count blocks then the stop token inside an open CMD25 -- see implementation for details. */
static ra_err_t internal_write_multi_stream(const uint8_t* buf, uint32_t count)
{
  for (uint32_t i = 0U; i < count; i++) {
    const ra_err_t err =
      internal_write_data_block(&buf[(size_t)i * (size_t)k_ra_sdmmc_spi_block_size],
                                (uint8_t)k_sd_token_data_start_multi);
    if (err != k_ra_ok) {
      return err;
    }
  }
  (void)internal_send_idle(1U);
  (void)internal_xfer_one((uint8_t)k_sd_token_stop_multi, nullptr);
  (void)internal_send_idle(1U);
  return internal_wait_not_busy();
}

/**
 * @brief Write @p count contiguous 512-byte blocks with one CMD25 transaction.
 *
 * @details The fast bulk-write path: an optional ACMD23 pre-erase hint, then a
 * single WRITE_MULTIPLE_BLOCK (CMD25) streaming every block with the multi-block
 * data-start token (0xFC), terminated by the stop-tran token (0xFD). One command
 * for the whole run instead of @p count single-block CMD24 writes -- the SD spec
 * fast path for clearing a large region (e.g. a multi-MB FAT during format).
 *
 * @param[in] lba   First logical block address.
 * @param[in] buf   Source buffer of @p count * 512 bytes.
 * @param[in] count Number of contiguous blocks (>= 1).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok All @p count blocks were accepted and programmed.
 * @retval k_ra_err_null_ptr      @p buf is NULL.
 * @retval k_ra_err_invalid_state The driver is not initialised.
 * @retval k_ra_err_out_of_range  @p lba + @p count exceeds the card capacity.
 * @retval k_ra_err_protocol_error A command R1 or data-response token rejected.
 *
 * @pre The driver is initialised and a card is present.
 * @pre @p buf holds at least @p count * 512 bytes.
 * @post The stop token has been sent and the card is no longer busy.
 * @post CS is released on every return path.
 *
 * @note Not thread-safe; serialise card access. ACMD23 is best-effort -- a card
 *       that rejects it still gets a correct (if unpre-erased) CMD25 stream.
 * @since 0.1.0
 */
ra_err_t ra_sdmmc_spi_write_blocks(uint32_t lba, const uint8_t* buf, uint32_t count)
{
  RA_CHECK_NULL_PTR(buf, s_tag, "buf is null");
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  if (count == 0U) {
    return k_ra_ok;
  }
  if ((lba >= s_state.capacity_blocks) || (count > (s_state.capacity_blocks - lba))) {
    return k_ra_err_out_of_range;
  }
  if (count == 1U) {
    return ra_sdmmc_spi_write_block(lba, buf);
  }
  ra_err_t err = internal_cs_assert();
  RA_RETURN_ON_ERROR(err, s_tag, "cs assert");
  /* Pre-erase hint (ACMD23): best-effort -- ignore the response so a card that
   * does not implement it still streams correctly below. */
  uint8_t acmd_r1 = 0U;
  (void)internal_send_acmd(k_sd_acmd_set_wr_blk_erase_count, count, &acmd_r1);
  uint8_t r1 = 0U;
  err        = internal_send_command(k_sd_cmd_write_multi_block, internal_lba_to_arg(lba), &r1);
  if ((err != k_ra_ok) || (r1 != 0U)) {
    (void)internal_cs_release();
    return (err != k_ra_ok) ? err : k_ra_err_protocol_error;
  }
  err = internal_write_multi_stream(buf, count);
  (void)internal_cs_release();
  return err;
}

ra_err_t ra_sdmmc_spi_get_capacity(uint32_t* out_blocks)
{
  RA_CHECK_NULL_PTR(out_blocks, s_tag, "out_blocks is null");
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  *out_blocks = s_state.capacity_blocks;
  return k_ra_ok;
}

ra_err_t ra_sdmmc_spi_get_card_type(ra_sdmmc_spi_card_type_t* out_type)
{
  RA_CHECK_NULL_PTR(out_type, s_tag, "out_type is null");
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  *out_type = s_state.card_type;
  return k_ra_ok;
}

/* ===========================================================================
 * ra_fs backend adapter
 * ===========================================================================
 */

/* ``read_block`` shim glue used by the ra_fs backend descriptor -- see implementation for details. */
static ra_err_t internal_fs_read_block(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  if (buf == nullptr) {
    return k_ra_err_null_ptr;
  }
  for (uint32_t i = 0U; i < count; i++) {
    ra_err_t err =
      ra_sdmmc_spi_read_block(lba + i, &buf[(size_t)i * (size_t)k_ra_sdmmc_spi_block_size]);
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

/* ``write_block`` shim glue used by the ra_fs backend descriptor -- see implementation for details. */
static ra_err_t internal_fs_write_block(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  (void)ctx;
  if (buf == nullptr) {
    return k_ra_err_null_ptr;
  }
  /* One CMD25 multi-block transaction for the whole run (fast); the single-block
   * path is used only for a lone block. */
  return ra_sdmmc_spi_write_blocks(lba, buf, count);
}

/* ``get_capacity`` shim glue used by the ra_fs backend descriptor -- see implementation for details. */
static ra_err_t internal_fs_get_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  (void)ctx;
  if ((block_count == nullptr) || (block_size == nullptr)) {
    return k_ra_err_null_ptr;
  }
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  *block_count = s_state.capacity_blocks;
  *block_size  = (uint32_t)k_ra_sdmmc_spi_block_size;
  return k_ra_ok;
}

ra_err_t ra_sdmmc_spi_bind_fs_backend(ra_fs_backend_t* out_backend)
{
  RA_CHECK_NULL_PTR(out_backend, s_tag, "out_backend is null");
  if (!s_state.initialized) {
    return k_ra_err_invalid_state;
  }
  out_backend->read_block   = internal_fs_read_block;
  out_backend->write_block  = internal_fs_write_block;
  out_backend->get_capacity = internal_fs_get_capacity;
  out_backend->ctx          = nullptr;
  return k_ra_ok;
}
