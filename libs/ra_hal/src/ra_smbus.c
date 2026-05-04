/**
 * @file ra_smbus.c
 * @brief SMBus 3.2 protocol layer on top of ``ra_iic_b`` -- implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Builds the SMBus 3.2 wire frames into a small stack scratch buffer
 * and hands them to ``ra_iic_b_write`` / ``ra_iic_b_read`` /
 * ``ra_iic_b_transfer``. PEC (Packet Error Code) is computed with the
 * SMBus 3.2 section 5.4 CRC-8 (poly 0x07, init 0). The IIC_B driver is
 * untouched -- this file is pure protocol framing.
 *
 * Section references in the comment headers below are to the SMBus 3.2
 * specification (SBS-IF, December 2018).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_smbus.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_iic_b.h"
#include "ra_log.h"

/* NOLINTBEGIN(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity) */

/** @brief Log tag for this driver. */
static const char* s_tag = "SMBUS";

/**
 * @enum ra_smbus_internal_t
 * @brief Implementation constants.
 */
typedef enum : uint16_t {
  k_ra_smbus_pec_poly     = 0x07U, /**< CRC-8 polynomial (SMBus 3.2 5.4). */
  k_ra_smbus_pec_init     = 0x00U, /**< CRC-8 initial value.              */
  k_ra_smbus_addr_shift   = 1U,    /**< 7-bit -> 8-bit address shift.     */
  k_ra_smbus_rw_write     = 0U,    /**< R/W bit value for write.          */
  k_ra_smbus_rw_read      = 1U,    /**< R/W bit value for read.           */
  k_ra_smbus_msb_for_byte = 8U,    /**< MSB index for byte-loop pec calc. */
  k_ra_smbus_pec_top_bit  = 0x80U, /**< Top bit mask used by pec loop.    */
  k_ra_smbus_byte_mask    = 0xFFU, /**< 8-bit narrowing mask.             */
} ra_smbus_internal_t;

/**
 * @struct ra_smbus_state_t
 * @brief Module state: latched at init time.
 *
 * @invariant ``initialised`` is true if and only if ``ra_iic_b_init``
 *            succeeded and ``ra_smbus_deinit`` has not been called.
 */
typedef struct {
  bool                initialised; /**< Latched after a good init.      */
  uint8_t             channel;     /**< IIC_B channel.                  */
  bool                pec_enabled; /**< Append + verify PEC byte.       */
  ra_smbus_alert_fn_t alert_fn;    /**< SMBALERT# callback (or NULL).   */
  void*               alert_ctx;   /**< Context for alert_fn.           */
} ra_smbus_state_t;

/** @brief Singleton driver state. */
static ra_smbus_state_t s_state = {
  .initialised = false,
  .channel     = 0U,
  .pec_enabled = false,
  .alert_fn    = nullptr,
  .alert_ctx   = nullptr,
};

/* =============================================================================
 * PEC: CRC-8 / SMBus
 * =============================================================================
 */

/**
 * @brief Update one byte into a running CRC-8/SMBus accumulator.
 *
 * @details
 * Bitwise implementation -- 8 shifts per byte. With the firmware's
 * call frequency (a handful of SMBus transactions per second) a table
 * lookup would be premature optimisation, and it would burn 256 bytes
 * of MRAM. The poly and init values are mandated by SMBus 3.2 5.4.
 *
 * @param[in] crc Current CRC.
 * @param[in] b   Byte to fold in.
 *
 * @return Updated CRC.
 *
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint8_t pec_update(uint8_t crc, uint8_t b)
{
  uint8_t c = (uint8_t)(crc ^ b);
  for (uint8_t i = 0U; i < (uint8_t)k_ra_smbus_msb_for_byte; ++i) {
    if ((c & (uint8_t)k_ra_smbus_pec_top_bit) != 0U) {
      c = (uint8_t)((uint8_t)(c << 1U) ^ (uint8_t)k_ra_smbus_pec_poly);
    } else {
      c = (uint8_t)(c << 1U);
    }
  }
  return c;
}

/* Ra smbus pec -- see implementation for details. */
uint8_t ra_smbus_pec(const uint8_t* data, uint32_t len)
{
  if ((data == nullptr) || (len == 0U)) {
    return (uint8_t)k_ra_smbus_pec_init;
  }
  uint8_t c = (uint8_t)k_ra_smbus_pec_init;
  for (uint32_t i = 0U; i < len; ++i) {
    c = pec_update(c, data[i]);
  }
  return c;
}

/* =============================================================================
 * Address-byte helpers
 * =============================================================================
 */

/* Build the on-the-wire address byte for a 7-bit target -- see implementation for details. */
static uint8_t make_addr_byte(uint8_t target_7b, uint8_t rw_bit)
{
  return (uint8_t)(((uint32_t)target_7b << (uint32_t)k_ra_smbus_addr_shift) |
                   ((uint32_t)rw_bit & 1U));
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/* Ra smbus init -- see implementation for details. */
ra_err_t ra_smbus_init(const ra_smbus_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "smbus_init: cfg");
  if (cfg->channel != 0U) {
    ra_log_error(s_tag, "init: channel out of range");
    return k_ra_err_invalid_arg;
  }
  const ra_err_t err = ra_iic_b_init(cfg->channel, &cfg->iic_cfg);
  if (err != k_ra_ok) {
    return err;
  }
  s_state.initialised = true;
  s_state.channel     = cfg->channel;
  s_state.pec_enabled = cfg->pec_enabled;
  s_state.alert_fn    = nullptr;
  s_state.alert_ctx   = nullptr;
  return k_ra_ok;
}

/* Ra smbus deinit -- see implementation for details. */
ra_err_t ra_smbus_deinit(void)
{
  if (!s_state.initialised) {
    return k_ra_err_not_initialized;
  }
  const ra_err_t err  = ra_iic_b_deinit(s_state.channel);
  s_state.initialised = false;
  s_state.alert_fn    = nullptr;
  s_state.alert_ctx   = nullptr;
  return err;
}

/* =============================================================================
 * Send Byte / Receive Byte (SMBus 3.2 sec 6.5.2 / 6.5.3)
 * =============================================================================
 */

/* Ra smbus send byte -- see implementation for details. */
ra_err_t ra_smbus_send_byte(uint8_t target_7b, uint8_t data)
{
  if (!s_state.initialised) {
    return k_ra_err_not_initialized;
  }
  /* Frame: [data] (+ optional [PEC]) -- the address byte is emitted by
   * ra_iic_b_write itself but participates in the PEC. */
  uint8_t  buf[2];
  uint32_t len = 0U;
  buf[len++]   = data;
  if (s_state.pec_enabled) {
    const uint8_t addr_b = make_addr_byte(target_7b, (uint8_t)k_ra_smbus_rw_write);
    uint8_t       pec    = pec_update((uint8_t)k_ra_smbus_pec_init, addr_b);
    pec                  = pec_update(pec, data);
    buf[len++]           = pec;
  }
  return ra_iic_b_write(s_state.channel, target_7b, buf, len, false);
}

/* Ra smbus receive byte -- see implementation for details. */
ra_err_t ra_smbus_receive_byte(uint8_t target_7b, uint8_t* out_data)
{
  RA_CHECK_NULL_PTR(out_data, s_tag, "receive_byte: out_data");
  if (!s_state.initialised) {
    return k_ra_err_not_initialized;
  }
  uint8_t        buf[2] = {0U, 0U};
  const uint32_t len    = s_state.pec_enabled ? 2U : 1U;
  const ra_err_t err    = ra_iic_b_read(s_state.channel, target_7b, buf, len, false);
  if (err != k_ra_ok) {
    return err;
  }
  if (s_state.pec_enabled) {
    const uint8_t addr_b = make_addr_byte(target_7b, (uint8_t)k_ra_smbus_rw_read);
    uint8_t       pec    = pec_update((uint8_t)k_ra_smbus_pec_init, addr_b);
    pec                  = pec_update(pec, buf[0]);
    if (pec != buf[1]) {
      ra_log_error(s_tag, "receive_byte: PEC mismatch");
      return k_ra_err_crc_mismatch;
    }
  }
  *out_data = buf[0];
  return k_ra_ok;
}

/* =============================================================================
 * Write Byte Data / Read Byte Data (SMBus 3.2 sec 6.5.4 / 6.5.5)
 * =============================================================================
 */

/* Ra smbus write byte data -- see implementation for details. */
ra_err_t ra_smbus_write_byte_data(uint8_t target_7b, uint8_t cmd, uint8_t data)
{
  if (!s_state.initialised) {
    return k_ra_err_not_initialized;
  }
  uint8_t  buf[3];
  uint32_t len = 0U;
  buf[len++]   = cmd;
  buf[len++]   = data;
  if (s_state.pec_enabled) {
    const uint8_t addr_b = make_addr_byte(target_7b, (uint8_t)k_ra_smbus_rw_write);
    uint8_t       pec    = pec_update((uint8_t)k_ra_smbus_pec_init, addr_b);
    pec                  = pec_update(pec, cmd);
    pec                  = pec_update(pec, data);
    buf[len++]           = pec;
  }
  return ra_iic_b_write(s_state.channel, target_7b, buf, len, false);
}

/* Ra smbus read byte data -- see implementation for details. */
ra_err_t ra_smbus_read_byte_data(uint8_t target_7b, uint8_t cmd, uint8_t* out_data)
{
  RA_CHECK_NULL_PTR(out_data, s_tag, "read_byte_data: out_data");
  if (!s_state.initialised) {
    return k_ra_err_not_initialized;
  }
  /* Combined transfer: write [cmd], RESTART, read [data] (+ optional
   * [PEC]). ra_iic_b_transfer handles the repeated-START framing. */
  uint8_t        rx[2] = {0U, 0U};
  const uint32_t rxlen = s_state.pec_enabled ? 2U : 1U;
  const ra_err_t err   = ra_iic_b_transfer(s_state.channel, target_7b, &cmd, 1U, rx, rxlen);
  if (err != k_ra_ok) {
    return err;
  }
  if (s_state.pec_enabled) {
    const uint8_t addr_w = make_addr_byte(target_7b, (uint8_t)k_ra_smbus_rw_write);
    const uint8_t addr_r = make_addr_byte(target_7b, (uint8_t)k_ra_smbus_rw_read);
    uint8_t       pec    = pec_update((uint8_t)k_ra_smbus_pec_init, addr_w);
    pec                  = pec_update(pec, cmd);
    pec                  = pec_update(pec, addr_r);
    pec                  = pec_update(pec, rx[0]);
    if (pec != rx[1]) {
      ra_log_error(s_tag, "read_byte_data: PEC mismatch");
      return k_ra_err_crc_mismatch;
    }
  }
  *out_data = rx[0];
  return k_ra_ok;
}

/* =============================================================================
 * Block Write / Block Read (SMBus 3.2 sec 6.5.7 / 6.5.8)
 * =============================================================================
 */

/* Ra smbus block write -- see implementation for details. */
ra_err_t ra_smbus_block_write(uint8_t target_7b, uint8_t cmd, const uint8_t* data, uint8_t len)
{
  if (!s_state.initialised) {
    return k_ra_err_not_initialized;
  }
  if (len == 0U) {
    return k_ra_err_invalid_arg;
  }
  RA_CHECK_NULL_PTR(data, s_tag, "block_write: data");

  /* Frame: [cmd] [count] [data...] [optional PEC]. The leading address
   * byte is generated by ra_iic_b_write but folded into the PEC by us
   * here. Max len is 255 by enum constraint, so the scratch is 258. */
  uint8_t  frame[258];
  uint32_t fi = 0U;
  frame[fi++] = cmd;
  frame[fi++] = len;
  for (uint8_t i = 0U; i < len; ++i) {
    frame[fi++] = data[i];
  }
  if (s_state.pec_enabled) {
    const uint8_t addr_b = make_addr_byte(target_7b, (uint8_t)k_ra_smbus_rw_write);
    uint8_t       pec    = pec_update((uint8_t)k_ra_smbus_pec_init, addr_b);
    for (uint32_t i = 0U; i < fi; ++i) {
      pec = pec_update(pec, frame[i]);
    }
    frame[fi++] = pec;
  }
  return ra_iic_b_write(s_state.channel, target_7b, frame, fi, false);
}

/* Ra smbus block read -- see implementation for details. */
ra_err_t
ra_smbus_block_read(uint8_t target_7b, uint8_t cmd, uint8_t* buf, uint8_t cap, uint8_t* out_len)
{
  RA_CHECK_NULL_PTR(buf, s_tag, "block_read: buf");
  RA_CHECK_NULL_PTR(out_len, s_tag, "block_read: out_len");
  if (!s_state.initialised) {
    return k_ra_err_not_initialized;
  }
  if (cap == 0U) {
    return k_ra_err_invalid_arg;
  }

  /* Read [count] [data...] [optional PEC] in one combined transfer.
   * We allocate the worst-case 257-byte scratch and read the maximum
   * the buffer allows to avoid two round trips to peek the count. */
  uint8_t        rx[257];
  const uint32_t want = (uint32_t)cap + 1U + (s_state.pec_enabled ? 1U : 0U);
  ra_err_t       err  = ra_iic_b_transfer(s_state.channel, target_7b, &cmd, 1U, rx, want);
  if (err != k_ra_ok) {
    return err;
  }
  const uint8_t count = rx[0];
  *out_len            = count;
  if (count > cap) {
    return k_ra_err_invalid_size;
  }
  for (uint8_t i = 0U; i < count; ++i) {
    buf[i] = rx[1U + (uint32_t)i];
  }
  if (s_state.pec_enabled) {
    const uint8_t addr_w = make_addr_byte(target_7b, (uint8_t)k_ra_smbus_rw_write);
    const uint8_t addr_r = make_addr_byte(target_7b, (uint8_t)k_ra_smbus_rw_read);
    uint8_t       pec    = pec_update((uint8_t)k_ra_smbus_pec_init, addr_w);
    pec                  = pec_update(pec, cmd);
    pec                  = pec_update(pec, addr_r);
    pec                  = pec_update(pec, count);
    for (uint8_t i = 0U; i < count; ++i) {
      pec = pec_update(pec, buf[i]);
    }
    /* PEC byte sits at rx[1 + count]. */
    if (pec != rx[1U + (uint32_t)count]) {
      ra_log_error(s_tag, "block_read: PEC mismatch");
      return k_ra_err_crc_mismatch;
    }
  }
  return k_ra_ok;
}

/* =============================================================================
 * SMBALERT# / Host Notify (SMBus 3.2 sec 6.5.13)
 * =============================================================================
 */

/* Ra smbus alert register callback -- see implementation for details. */
ra_err_t ra_smbus_alert_register_callback(ra_smbus_alert_fn_t fn, void* ctx)
{
  if (!s_state.initialised) {
    return k_ra_err_not_initialized;
  }
  s_state.alert_fn  = fn;
  s_state.alert_ctx = ctx;
  return k_ra_ok;
}

/* Ra smbus alert dispatch -- see implementation for details. */
ra_err_t ra_smbus_alert_dispatch(void)
{
  if (!s_state.initialised) {
    return k_ra_err_not_initialized;
  }
  uint8_t        ara = 0U;
  const ra_err_t err =
    ra_iic_b_read(s_state.channel, (uint8_t)k_ra_smbus_alert_addr_7b, &ara, 1U, false);
  if (err != k_ra_ok) {
    return err;
  }
  if (s_state.alert_fn != nullptr) {
    /* SMBus 3.2 6.5.13: top 7 bits = peripheral address, LSB = status. */
    const uint8_t addr_7b = (uint8_t)(ara >> (uint32_t)k_ra_smbus_addr_shift);
    const uint8_t status  = (uint8_t)(ara & 1U);
    s_state.alert_fn(s_state.alert_ctx, addr_7b, status);
  }
  return k_ra_ok;
}

/* NOLINTEND(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity) */
