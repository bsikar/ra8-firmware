/**
 * @file ra8_sdmmc_spi.c
 * @brief SD card driver in SPI-mode -- protocol implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Implementation of the SPI-mode SD card driver declared in
 * ``ra8_sdmmc_spi.h``. The protocol follows SD Specification Part 1
 * Physical Layer Simplified Specification v9.10 section 7
 * ("SPI Mode"). Command framing, response classes (R1, R1b, R3, R7),
 * data tokens, CRC7 and CRC16 polynomials, and the v1.x vs v2.x card
 * discrimination flow are all documented inline at their first use.
 *
 * The driver is transport-agnostic: every byte exchange goes through
 * the caller-supplied ``ra8_sdmmc_spi_transport_t``. This lets the host
 * tests inject a mock and the firmware app inject the real ``ra8_spi``
 * HAL driver through a thin shim that lives in the example app.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_sdmmc_spi.h"

#include <stdint.h>
#include <string.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_log.h"
#include "ra8_port_utils.h"
#include "ra8_sci_spi.h"
#include "ra8_sdmmc_spi_core_contracts.h"
#include "ra8_sdmmc_spi_internal.h"
#include "ra8_spi.h"

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

/** @brief SD CSD field masks/shifts and command framing. */
typedef enum : uint32_t {
  k_sd_cmd_frame_len  = 5U,    /**< Command bytes preceding the CRC7.  */
  k_sd_csize_msb_mask = 0x3FU, /**< CSD v2 C_SIZE MSB field (6-bit).   */
  k_sd_read_bl_mask   = 0x0FU, /**< READ_BL_LEN (4-bit).               */
  k_sd_csize_shift    = 10U,   /**< CSD v1 C_SIZE high-bits shift.     */
  k_sd_csize_lo_mask  = 0xC0U, /**< CSD v1 C_SIZE low 2 bits (byte 8). */
  k_sd_mult_lo_mask   = 0x80U, /**< C_SIZE_MULT low bit (byte 10).     */
  k_sd_mult_shift     = 7U,    /**< C_SIZE_MULT low-bit shift.         */
} sd_csd_field_t;

/**
 * @enum sd_r1_bit_t
 * @brief R1 response bit definitions (SD spec PHY v9 section 7.3.2.1).
 */
typedef enum : uint8_t {
  k_sd_r1_idle_state           = 0x01U, /**< SD r1 idle state.           */
  k_sd_r1_erase_reset          = 0x02U, /**< SD r1 erase reset.          */
  k_sd_r1_illegal_command      = 0x04U, /**< SD r1 illegal command.      */
  k_sd_r1_com_crc_error        = 0x08U, /**< SD r1 com CRC error.        */
  k_sd_r1_erase_sequence_error = 0x10U, /**< SD r1 erase sequence error. */
  k_sd_r1_address_error        = 0x20U, /**< SD r1 address error.        */
  k_sd_r1_parameter_error      = 0x40U, /**< SD r1 parameter error.      */
  /* The top bit (0x80) is always zero -- used as the R1 sentinel. */
  k_sd_r1_sentinel = 0x80U, /**< SD r1 sentinel. */
} sd_r1_bit_t;

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
  k_sd_frame_idx_cmd       = 0U, /**< SD frame index cmd.       */
  k_sd_frame_idx_arg_msb   = 1U, /**< SD frame index arg msb.   */
  k_sd_frame_idx_arg_byte2 = 2U, /**< SD frame index arg byte2. */
  k_sd_frame_idx_arg_byte1 = 3U, /**< SD frame index arg byte1. */
  k_sd_frame_idx_arg_lsb   = 4U, /**< SD frame index arg lsb.   */
  k_sd_frame_idx_crc       = 5U, /**< SD frame index CRC.       */
} sd_cmd_arg_idx_t;

/**
 * @enum sd_csd_layout_t
 * @brief Selected CSD-byte indices and bit shifts (SD spec PHY v9 section 5).
 */
typedef enum : uint8_t {
  /* CSD version lives in byte 0 bits 7:6: 0 = CSD v1, 1 = CSD v2. */
  k_sd_csd_byte_version  = 0U, /**< SD csd byte version.  */
  k_sd_csd_version_shift = 6U, /**< SD csd version shift. */
  /* CSD v2: C_SIZE is a 22-bit field spanning bytes 7..9. */
  k_sd_csd_v2_byte_csize_msb = 7U, /**< SD csd v2 byte csize msb. */
  k_sd_csd_v2_byte_csize_mid = 8U, /**< SD csd v2 byte csize mid. */
  k_sd_csd_v2_byte_csize_lsb = 9U, /**< SD csd v2 byte csize lsb. */
  /* Bit shift to go from C_SIZE to block count: ((C_SIZE + 1) * 1024). */
  k_sd_csd_v2_blocks_shift = 10U, /**< SD csd v2 blocks shift. */
} sd_csd_layout_t;

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
 * @brief CRC16-CCITT polynomial and mask constants (SD spec PHY v9
 * section 4.5).
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

/* ---------------------------------------------------------------------------
 * Driver state (single global instance -- one card per board today)
 * ---------------------------------------------------------------------------
 */

/**
 * @var g_sdmmc_spi_state
 * @brief Single driver instance (the module's one external definition).
 * @details Shared with the block-I/O TU through ``ra8_sdmmc_spi_internal.h``;
 *          this file owns the sole definition.
 * @note Internal mutable state; access from a single thread only.
 * @since 0.1.0
 */
RA8_PRIV sd_state_t g_sdmmc_spi_state = {};

/* ===========================================================================
 * CRC helpers (public for unit-test coverage)
 * ===========================================================================
 */

uint8_t ra8_sdmmc_spi_crc7(const uint8_t* data, uint32_t len)
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

uint16_t ra8_sdmmc_spi_crc16(const uint8_t* data, uint32_t len)
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

/* Shift out one byte and capture the response -- see implementation for
 * details. */
RA8_PRIV ra8_err_t priv_sdmmc_spi_xfer_one(uint8_t tx, uint8_t* rx)
{
  const uint8_t tx_buf[1] = {tx};
  uint8_t       rx_buf[1] = {};
  ra8_err_t     err =
    g_sdmmc_spi_state.transport.xfer(g_sdmmc_spi_state.transport.ctx, tx_buf, rx_buf, 1U);
  if (err != k_ra8_ok) {
    return err;
  }
  if (rx != nullptr) {
    *rx = rx_buf[0];
  }
  return k_ra8_ok;
}

/* Shift ``n`` idle bytes (0xFF) and discard the response -- see implementation
 * for details. */
RA8_PRIV ra8_err_t priv_sdmmc_spi_send_idle(uint32_t n)
{
  uint8_t byte;
  for (uint32_t i = 0U; i < n; i++) {
    ra8_err_t err = priv_sdmmc_spi_xfer_one((uint8_t)k_sd_token_idle, &byte);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/* Drive CS low and clock a single idle byte so CIPO is sampled -- see
 * implementation for details. */
RA8_PRIV ra8_err_t priv_sdmmc_spi_cs_assert(void)
{
  ra8_err_t err = g_sdmmc_spi_state.transport.cs(g_sdmmc_spi_state.transport.ctx, true);
  if (err != k_ra8_ok) {
    return err;
  }
  return priv_sdmmc_spi_send_idle(1U);
}

/* Drive CS high after clocking one idle byte (SD spec section 7.2.4) -- see
 * implementation for details. */
RA8_PRIV ra8_err_t priv_sdmmc_spi_cs_release(void)
{
  ra8_err_t err = g_sdmmc_spi_state.transport.cs(g_sdmmc_spi_state.transport.ctx, false);
  if (err != k_ra8_ok) {
    return err;
  }
  /* Clock 8 idle cycles after CS release so the card can finish any
   * pending internal state transition (SD spec PHY v9 section 7.2.4). */
  return priv_sdmmc_spi_send_idle(1U);
}

/* ===========================================================================
 * Command framing
 * ===========================================================================
 */

/* see header for full description */
RA8_INTERNAL static void internal_build_frame(sd_cmd_t cmd, uint32_t arg, uint8_t* out_frame)
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
    const uint8_t crc7            = ra8_sdmmc_spi_crc7(out_frame, k_sd_cmd_frame_len);
    out_frame[k_sd_frame_idx_crc] = (uint8_t)(((uint32_t)crc7 << 1U) | 1U);
  }
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_read_r1(uint8_t* out_r1)
{
  for (uint32_t i = 0U; i < (uint32_t)k_sd_max_r1_wait_bytes; i++) {
    uint8_t   byte = 0U;
    ra8_err_t err  = priv_sdmmc_spi_xfer_one((uint8_t)k_sd_token_idle, &byte);
    if (err != k_ra8_ok) {
      return err;
    }
    if ((byte & (uint8_t)k_sd_r1_sentinel) == 0U) {
      *out_r1 = byte;
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

/* Send a command frame and capture the R1 response byte -- see implementation
 * for details. */
RA8_PRIV ra8_err_t priv_sdmmc_spi_send_command(sd_cmd_t cmd, uint32_t arg, uint8_t* out_r1)
{
  uint8_t frame[k_ra8_sdmmc_spi_cmd_frame_bytes];
  internal_build_frame(cmd, arg, frame);
  uint8_t   rx_dummy[k_ra8_sdmmc_spi_cmd_frame_bytes];
  ra8_err_t err = g_sdmmc_spi_state.transport.xfer(g_sdmmc_spi_state.transport.ctx,
                                                   frame,
                                                   rx_dummy,
                                                   (uint32_t)k_ra8_sdmmc_spi_cmd_frame_bytes);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_read_r1(out_r1);
}

/* Send an ACMD by prefixing it with CMD55 (APP_CMD) -- see implementation for
 * details. */
RA8_PRIV ra8_err_t priv_sdmmc_spi_send_acmd(sd_cmd_t acmd, uint32_t arg, uint8_t* out_r1)
{
  uint8_t   r1  = 0U;
  ra8_err_t err = priv_sdmmc_spi_send_command(k_sd_cmd_app_cmd, 0U, &r1);
  if (err != k_ra8_ok) {
    return err;
  }
  /* CMD55 is allowed to leave R1 == idle (0x01) during the init loop. */
  return priv_sdmmc_spi_send_command(acmd, arg, out_r1);
}

/* Send CMD12 (STOP_TRANSMISSION) with the stuff-byte skip before R1 -- see
 * implementation for details. */
RA8_PRIV ra8_err_t priv_sdmmc_spi_send_stop_transmission(uint8_t* out_r1)
{
  uint8_t frame[k_ra8_sdmmc_spi_cmd_frame_bytes];
  internal_build_frame(k_sd_cmd_stop_transmission, 0U, frame);
  uint8_t   rx_dummy[k_ra8_sdmmc_spi_cmd_frame_bytes];
  ra8_err_t err = g_sdmmc_spi_state.transport.xfer(g_sdmmc_spi_state.transport.ctx,
                                                   frame,
                                                   rx_dummy,
                                                   (uint32_t)k_ra8_sdmmc_spi_cmd_frame_bytes);
  if (err != k_ra8_ok) {
    return err;
  }
  /* The byte immediately following the CMD12 frame is a stuff byte: it
   * carries the tail of the interrupted read stream and its value is
   * undefined, so it must be discarded before the R1 poll -- a bit7-clear
   * garbage byte would otherwise be misread as the response. This is why
   * CMD12 cannot go through priv_sdmmc_spi_send_command, which polls for R1
   * straight after the frame. */
  err = priv_sdmmc_spi_send_idle(1U);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_read_r1(out_r1);
}

/* ===========================================================================
 * R7 / R3 extension responses (CMD8, CMD58)
 * ===========================================================================
 */

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_read_r3_or_r7_tail(uint32_t* out_word)
{
  uint8_t   bytes[4] = {};
  ra8_err_t err =
    g_sdmmc_spi_state.transport.xfer(g_sdmmc_spi_state.transport.ctx, nullptr, bytes, 4U);
  if (err != k_ra8_ok) {
    /* Some transports do not allow NULL tx; fall back to per-byte. */
    for (uint32_t i = 0U; i < 4U; i++) {
      err = priv_sdmmc_spi_xfer_one((uint8_t)k_sd_token_idle, &bytes[i]);
      if (err != k_ra8_ok) {
        return err;
      }
    }
  }
  *out_word = ((uint32_t)bytes[0] << k_sd_bit_three_byte) |
              ((uint32_t)bytes[1] << k_sd_bit_two_byte) | ((uint32_t)bytes[2] << k_sd_bit_byte) |
              (uint32_t)bytes[3];
  return k_ra8_ok;
}

/* ===========================================================================
 * Data-token I/O
 * ===========================================================================
 */

/* Poll for the data-start token (0xFE), bounded by a retry budget -- see
 * implementation for details. */
RA8_PRIV ra8_err_t priv_sdmmc_spi_wait_data_token(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_sd_max_data_token_polls; i++) {
    uint8_t   byte = 0U;
    ra8_err_t err  = priv_sdmmc_spi_xfer_one((uint8_t)k_sd_token_idle, &byte);
    if (err != k_ra8_ok) {
      return err;
    }
    if (byte == (uint8_t)k_sd_token_data_start_single) {
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

/* Wait for the card to release busy (CIPO -> 0xFF), bounded by @p max_polls --
 * see implementation for details. */
RA8_PRIV ra8_err_t priv_sdmmc_spi_wait_not_busy_bounded(uint32_t max_polls)
{
  for (uint32_t i = 0U; i < max_polls; i++) {
    uint8_t   byte = 0U;
    ra8_err_t err  = priv_sdmmc_spi_xfer_one((uint8_t)k_sd_token_idle, &byte);
    if (err != k_ra8_ok) {
      return err;
    }
    if (byte == (uint8_t)k_sd_token_idle) {
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

/* Wait for the card to release the busy token (CIPO returns to 0xFF) -- see
 * implementation for details. */
RA8_PRIV ra8_err_t priv_sdmmc_spi_wait_not_busy(void)
{
  return priv_sdmmc_spi_wait_not_busy_bounded((uint32_t)k_sd_max_busy_poll_bytes);
}

/* ===========================================================================
 * CSD parsing (capacity decode)
 * ===========================================================================
 */

/* see header for full description */
RA8_INTERNAL static uint32_t internal_csd_to_blocks(const uint8_t* csd)
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
    return (uint32_t)(bytes / (uint64_t)k_ra8_sdmmc_spi_block_size);
  }
  return 0U;
}

/* ===========================================================================
 * Init sequence
 * ===========================================================================
 */

/* Validate the transport descriptor: every callback non-NULL -- see
 * implementation for details. */
RA8_PRIV ra8_err_t priv_sdmmc_spi_validate_transport(const ra8_sdmmc_spi_transport_t* transport)
{
  if (transport == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((transport->set_clock == nullptr) || (transport->cs == nullptr) ||
      (transport->xfer == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Best-effort recovery for a card stranded mid-write by an interrupted
 *        transaction (e.g. an MCU reset during a long format).
 *
 * @details Four phases, each safe to run against an already-idle card (0xFD and
 * 0xFF are both non-command bytes that clock past as idle, and CMD12 on an idle
 * card is a no-op that returns to idle):
 * 1. CS released, a long idle burst -- lets a card still BUSY (programming)
 * from the interrupted write drain its internal timer, and resets the card's
 * byte framing for the next CS assertion (a reset mid-byte can leave it
 * misaligned).
 * 2. CS asserted, a stop-tran token then >= 512 + CRC idle bytes -- aborts a
 *    wedged CMD25 multi-block write and completes a wedged CMD24 single-block
 *    data phase, so the card programs the slack and returns to the transfer
 *    state; then wait out the programming.
 * 3. CMD12 STOP_TRANSMISSION -- aborts any open multi-block transfer the card
 *    still believes is active; wait out the trailing busy.
 * 4. CS released, a final idle burst to settle framing before the wake/CMD0.
 *
 * Run before each wake/CMD0 retry so a stuck card can re-init without a
 * physical power cycle. (Limits: cannot recover a card whose controller needs a
 * true power-on reset, since the MCU cannot remove card VBUS.)
 *
 * @return None.
 * @pre The transport is bound (called from the init probe).
 * @pre All transport callbacks (@c cs, @c xfer) are non-NULL in @c
 * g_sdmmc_spi_state.transport.
 * @post Any in-flight write/read transaction is terminated and the bus is idle.
 * @post CS is released.
 * @note Not thread-safe; part of single-threaded init.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_recover_stuck_card(void)
{
  /* Phase 1: drain busy + reset byte framing with CS released. */
  (void)g_sdmmc_spi_state.transport.cs(g_sdmmc_spi_state.transport.ctx, false);
  (void)priv_sdmmc_spi_send_idle((uint32_t)k_sd_recover_idle_bytes);

  /* Phase 2: complete/abort a wedged data phase with CS asserted. */
  if (g_sdmmc_spi_state.transport.cs(g_sdmmc_spi_state.transport.ctx, true) != k_ra8_ok) {
    return;
  }
  (void)priv_sdmmc_spi_xfer_one((uint8_t)k_sd_token_stop_multi, nullptr);
  (void)priv_sdmmc_spi_send_idle((uint32_t)k_sd_recover_flush_bytes);
  (void)priv_sdmmc_spi_wait_not_busy();

  /* Phase 3: explicit STOP_TRANSMISSION for any open multi-block transfer. */
  uint8_t r1 = 0U;
  (void)priv_sdmmc_spi_send_command(k_sd_cmd_stop_transmission, 0U, &r1);
  (void)priv_sdmmc_spi_wait_not_busy();
  (void)g_sdmmc_spi_state.transport.cs(g_sdmmc_spi_state.transport.ctx, false);

  /* Phase 4: settle framing before the caller's wake/CMD0. */
  (void)priv_sdmmc_spi_send_idle((uint32_t)k_sd_recover_idle_bytes);
}

/* Drive >= 74 dummy clocks with CS high to wake the card (SD spec
 * section 7.2.1) -- see implementation for details. */
RA8_INTERNAL static ra8_err_t internal_wake_card(void)
{
  ra8_err_t err = g_sdmmc_spi_state.transport.cs(g_sdmmc_spi_state.transport.ctx, false);
  if (err != k_ra8_ok) {
    return err;
  }
  return priv_sdmmc_spi_send_idle((uint32_t)k_sd_init_dummy_clocks / (uint32_t)k_sd_bit_byte);
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_send_cmd0(void)
{
  ra8_err_t err = priv_sdmmc_spi_cs_assert();
  if (err != k_ra8_ok) {
    return err;
  }
  uint8_t r1 = 0U;
  err        = priv_sdmmc_spi_send_command(k_sd_cmd_go_idle_state, 0U, &r1);
  (void)priv_sdmmc_spi_cs_release();
  if (err != k_ra8_ok) {
    return err;
  }
  if (r1 != (uint8_t)k_sd_r1_idle_state) {
    return k_ra8_err_protocol_error;
  }
  return k_ra8_ok;
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_send_cmd8(bool* out_is_v2)
{
  ra8_err_t err = priv_sdmmc_spi_cs_assert();
  if (err != k_ra8_ok) {
    return err;
  }
  uint8_t r1 = 0U;
  err =
    priv_sdmmc_spi_send_command(k_sd_cmd_send_if_cond, (uint32_t)k_sd_cmd8_arg_check_pattern, &r1);
  if (err != k_ra8_ok) {
    (void)priv_sdmmc_spi_cs_release();
    return err;
  }
  if ((r1 & (uint8_t)k_sd_r1_illegal_command) != 0U) {
    *out_is_v2 = false;
    (void)priv_sdmmc_spi_cs_release();
    return k_ra8_ok;
  }
  uint32_t echo = 0U;
  err           = internal_read_r3_or_r7_tail(&echo);
  (void)priv_sdmmc_spi_cs_release();
  if (err != k_ra8_ok) {
    return err;
  }
  if ((echo & (uint32_t)k_sd_mask_12bit) !=
      ((uint32_t)k_sd_cmd8_arg_check_pattern & (uint32_t)k_sd_mask_12bit)) {
    return k_ra8_err_protocol_error;
  }
  *out_is_v2 = true;
  return k_ra8_ok;
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_acmd41_loop(bool is_v2)
{
  const uint32_t arg = is_v2 ? (uint32_t)k_sd_acmd41_arg_hcs : 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_sd_max_acmd41_attempts; i++) {
    ra8_err_t err = priv_sdmmc_spi_cs_assert();
    if (err != k_ra8_ok) {
      return err;
    }
    uint8_t r1 = 0U;
    err        = priv_sdmmc_spi_send_acmd(k_sd_acmd_sd_send_op_cond, arg, &r1);
    (void)priv_sdmmc_spi_cs_release();
    if (err != k_ra8_ok) {
      return err;
    }
    if ((r1 & (uint8_t)k_sd_r1_idle_state) == 0U) {
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_init_failed;
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_read_ocr(bool* out_is_hc)
{
  ra8_err_t err = priv_sdmmc_spi_cs_assert();
  if (err != k_ra8_ok) {
    return err;
  }
  uint8_t r1 = 0U;
  err        = priv_sdmmc_spi_send_command(k_sd_cmd_read_ocr, 0U, &r1);
  if (err != k_ra8_ok) {
    (void)priv_sdmmc_spi_cs_release();
    return err;
  }
  uint32_t ocr = 0U;
  err          = internal_read_r3_or_r7_tail(&ocr);
  (void)priv_sdmmc_spi_cs_release();
  if (err != k_ra8_ok) {
    return err;
  }
  *out_is_hc = ((ocr & (uint32_t)k_sd_ocr_ccs_bit) != 0U);
  return k_ra8_ok;
}

/* Send CMD9 (SEND_CSD) and parse capacity -- see implementation for details. */
RA8_INTERNAL static ra8_err_t internal_read_csd(uint32_t* out_blocks)
{
  ra8_err_t err = priv_sdmmc_spi_cs_assert();
  if (err != k_ra8_ok) {
    return err;
  }
  uint8_t r1 = 0U;
  err        = priv_sdmmc_spi_send_command(k_sd_cmd_send_csd, 0U, &r1);
  if (err != k_ra8_ok) {
    (void)priv_sdmmc_spi_cs_release();
    return err;
  }
  if (r1 != 0U) {
    (void)priv_sdmmc_spi_cs_release();
    return k_ra8_err_protocol_error;
  }
  err = priv_sdmmc_spi_wait_data_token();
  if (err != k_ra8_ok) {
    (void)priv_sdmmc_spi_cs_release();
    return err;
  }
  uint8_t csd[k_ra8_sdmmc_spi_csd_response_len] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_sdmmc_spi_csd_response_len; i++) {
    err = priv_sdmmc_spi_xfer_one((uint8_t)k_sd_token_idle, &csd[i]);
    if (err != k_ra8_ok) {
      (void)priv_sdmmc_spi_cs_release();
      return err;
    }
  }
  /* Drain trailing CRC16 (2 bytes). */
  uint8_t crc_bytes[2] = {};
  (void)priv_sdmmc_spi_xfer_one((uint8_t)k_sd_token_idle, &crc_bytes[0]);
  (void)priv_sdmmc_spi_xfer_one((uint8_t)k_sd_token_idle, &crc_bytes[1]);
  (void)priv_sdmmc_spi_cs_release();
  *out_blocks = internal_csd_to_blocks(csd);
  if (*out_blocks == 0U) {
    return k_ra8_err_protocol_error;
  }
  return k_ra8_ok;
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_set_block_len(void)
{
  ra8_err_t err = priv_sdmmc_spi_cs_assert();
  if (err != k_ra8_ok) {
    return err;
  }
  uint8_t r1 = 0U;
  err =
    priv_sdmmc_spi_send_command(k_sd_cmd_set_blocklen, (uint32_t)k_ra8_sdmmc_spi_block_size, &r1);
  (void)priv_sdmmc_spi_cs_release();
  if (err != k_ra8_ok) {
    return err;
  }
  if (r1 != 0U) {
    return k_ra8_err_protocol_error;
  }
  return k_ra8_ok;
}

/* see header for full description */
RA8_INTERNAL static ra8_sdmmc_spi_card_type_t internal_classify_card(bool is_v2, bool is_hc)
{
  if (is_hc) {
    return k_ra8_sdmmc_spi_type_sdhc;
  }
  if (is_v2) {
    return k_ra8_sdmmc_spi_type_sdv2;
  }
  return k_ra8_sdmmc_spi_type_sdv1;
}

/* see header for full description */
RA8_INTERNAL static ra8_err_t internal_probe_card(bool* out_is_v2, bool* out_is_hc)
{
  ra8_err_t err = internal_wake_card();
  if (err == k_ra8_ok) {
    err = internal_send_cmd0();
  }
  /* CMD0 failed: the card may be stranded mid-write from an interrupted
   * transaction. Flush it and retry, escalating across a few attempts. A
   * healthy card answers CMD0 on the first pass and never enters this loop, so
   * the recovery cannot disturb a normal bring-up. */
  for (uint32_t attempt = 0U; (err != k_ra8_ok) && (attempt < (uint32_t)k_sd_max_recover_attempts);
       attempt++) {
    internal_recover_stuck_card();
    err = internal_wake_card();
    if (err == k_ra8_ok) {
      err = internal_send_cmd0();
    }
  }
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_send_cmd8(out_is_v2);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_acmd41_loop(*out_is_v2);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_is_hc = false;
  if (*out_is_v2) {
    err = internal_read_ocr(out_is_hc);
  }
  return err;
}

/* Walk the full SD identification sequence and learn capacity / type -- see
 * implementation for details. */
RA8_PRIV ra8_err_t priv_sdmmc_spi_run_init_sequence(void)
{
  bool      is_v2 = false;
  bool      is_hc = false;
  ra8_err_t err   = internal_probe_card(&is_v2, &is_hc);
  if (err != k_ra8_ok) {
    return err;
  }
  uint32_t blocks = 0U;
  err             = internal_read_csd(&blocks);
  RA8_RETURN_ON_ERROR(err, s_tag, "CMD9 SEND_CSD");
  /* SDSC (v1.x or v2.x SDSC) needs an explicit block-size set; HC cards
   * accept it as a no-op but the spec says we should still issue it. */
  err = internal_set_block_len();
  RA8_RETURN_ON_ERROR(err, s_tag, "CMD16 SET_BLOCKLEN");
  g_sdmmc_spi_state.card_type       = internal_classify_card(is_v2, is_hc);
  g_sdmmc_spi_state.capacity_blocks = blocks;
  return k_ra8_ok;
}
