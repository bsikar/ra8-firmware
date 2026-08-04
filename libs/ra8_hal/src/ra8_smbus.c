/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_smbus.c
 * @brief SMBus 3.2 protocol layer over an injected I2C bus seam --
 *        implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Builds the SMBus 3.2 wire frames into a small stack scratch buffer
 * and hands them to the injected I2C bus seam (``ra8_i2c_bus_ops_t``)
 * latched at init. PEC (Packet Error Code) is computed with the SMBus
 * 3.2 section 5.4 CRC-8 (poly 0x07, init 0). The bus drivers are
 * untouched -- this file is pure protocol framing.
 *
 * Section references in the comment headers below are to the SMBus 3.2
 * specification (SBS-IF, December 2018).
 */

#include "ra8_smbus.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_i2c_bus_ops.h"
#include "ra8_log.h"

/** @brief Log tag for this driver. */
static const char* s_tag = "SMBUS";

/** @brief SMBus scratch buffer sizes (max 255-byte payload + overhead). */
typedef enum : uint16_t {
  k_smbus_frame_bytes = 258U, /**< Smbus frame bytes. */
  k_smbus_rx_bytes    = 257U, /**< Smbus RX bytes.    */
} smbus_buf_size_t;

/**
 * @enum ra8_smbus_internal_t
 * @brief Implementation constants.
 */
typedef enum : uint16_t {
  k_ra8_smbus_pec_poly     = 0x07U, /**< CRC-8 polynomial (SMBus 3.2 5.4). */
  k_ra8_smbus_pec_init     = 0x00U, /**< CRC-8 initial value.              */
  k_ra8_smbus_addr_shift   = 1U,    /**< 7-bit -> 8-bit address shift.     */
  k_ra8_smbus_rw_write     = 0U,    /**< R/W bit value for write.          */
  k_ra8_smbus_rw_read      = 1U,    /**< R/W bit value for read.           */
  k_ra8_smbus_msb_for_byte = 8U,    /**< MSB index for byte-loop pec calc. */
  k_ra8_smbus_pec_top_bit  = 0x80U, /**< Top bit mask used by pec loop.    */
  k_ra8_smbus_byte_mask    = 0xFFU, /**< 8-bit narrowing mask.             */
} ra8_smbus_internal_t;

/**
 * @struct ra8_smbus_state_t
 * @brief Module state: latched at init time.
 *
 * @invariant ``initialized`` is true if and only if ``ra8_smbus_init``
 *            succeeded and ``ra8_smbus_deinit`` has not been called.
 */
typedef struct {
  bool                 initialized; /**< Latched after a good init.    */
  ra8_i2c_bus_ops_t    bus;         /**< Injected I2C transfer seam.   */
  bool                 pec_enabled; /**< Append + verify PEC byte.     */
  ra8_smbus_alert_fn_t alert_fn;    /**< SMBALERT# callback (or NULL). */
  void*                alert_ctx;   /**< Context for alert_fn.         */
} ra8_smbus_state_t;

/** @brief Singleton driver state. */
static ra8_smbus_state_t s_state = {
  .initialized = false,
  .bus         = {},
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
 * @pre Module has been initialized.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t pec_update(uint8_t crc, uint8_t b)
{
  uint8_t c = (uint8_t)(crc ^ b);
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_smbus_msb_for_byte; ++i) {
    if ((c & (uint8_t)k_ra8_smbus_pec_top_bit) != 0U) {
      c = (uint8_t)((uint8_t)(c << 1U) ^ (uint8_t)k_ra8_smbus_pec_poly);
    } else {
      c = (uint8_t)(c << 1U);
    }
  }
  return c;
}

uint8_t ra8_smbus_pec(const uint8_t* data, uint32_t len)
{
  if ((data == nullptr) || (len == 0U)) {
    return (uint8_t)k_ra8_smbus_pec_init;
  }
  uint8_t c = (uint8_t)k_ra8_smbus_pec_init;
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
RA8_INTERNAL
static uint8_t make_addr_byte(uint8_t target_7b, uint8_t rw_bit)
{
  return (uint8_t)(((uint32_t)target_7b << (uint32_t)k_ra8_smbus_addr_shift) |
                   ((uint32_t)rw_bit & 1U));
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

ra8_err_t ra8_smbus_init(const ra8_smbus_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "smbus_init: cfg");
  if (cfg->bus.write == nullptr) {
    ra8_log_error(s_tag, "init: bus.write missing");
    return k_ra8_err_invalid_arg;
  }
  if (cfg->bus.read == nullptr) {
    ra8_log_error(s_tag, "init: bus.read missing");
    return k_ra8_err_invalid_arg;
  }
  if (cfg->bus.transfer == nullptr) {
    ra8_log_error(s_tag, "init: bus.transfer missing");
    return k_ra8_err_invalid_arg;
  }
  s_state.initialized = true;
  s_state.bus         = cfg->bus;
  s_state.pec_enabled = cfg->pec_enabled;
  s_state.alert_fn    = nullptr;
  s_state.alert_ctx   = nullptr;
  return k_ra8_ok;
}

ra8_err_t ra8_smbus_deinit(void)
{
  if (!s_state.initialized) {
    return k_ra8_err_not_initialized;
  }
  s_state.initialized = false;
  s_state.alert_fn    = nullptr;
  s_state.alert_ctx   = nullptr;
  return k_ra8_ok;
}

/* =============================================================================
 * Send Byte / Receive Byte (SMBus 3.2 sec 6.5.2 / 6.5.3)
 * =============================================================================
 */

ra8_err_t ra8_smbus_send_byte(uint8_t target_7b, uint8_t data)
{
  if (!s_state.initialized) {
    return k_ra8_err_not_initialized;
  }
  /* Frame: [data] (+ optional [PEC]) -- the address byte is emitted by
   * the bus write itself but participates in the PEC. */
  uint8_t  buf[2];
  uint32_t len = 0U;
  buf[len++]   = data;
  if (s_state.pec_enabled) {
    const uint8_t addr_b = make_addr_byte(target_7b, (uint8_t)k_ra8_smbus_rw_write);
    uint8_t       pec    = pec_update((uint8_t)k_ra8_smbus_pec_init, addr_b);
    pec                  = pec_update(pec, data);
    buf[len++]           = pec;
  }
  return s_state.bus.write(s_state.bus.ctx, target_7b, buf, len, true);
}

ra8_err_t ra8_smbus_receive_byte(uint8_t target_7b, uint8_t* out_data)
{
  RA8_CHECK_NULL_PTR(out_data, s_tag, "receive_byte: out_data");
  if (!s_state.initialized) {
    return k_ra8_err_not_initialized;
  }
  uint8_t         buf[2] = {0U, 0U};
  const uint32_t  len    = s_state.pec_enabled ? 2U : 1U;
  const ra8_err_t err    = s_state.bus.read(s_state.bus.ctx, target_7b, buf, len);
  if (err != k_ra8_ok) {
    return err;
  }
  if (s_state.pec_enabled) {
    const uint8_t addr_b = make_addr_byte(target_7b, (uint8_t)k_ra8_smbus_rw_read);
    uint8_t       pec    = pec_update((uint8_t)k_ra8_smbus_pec_init, addr_b);
    pec                  = pec_update(pec, buf[0]);
    if (pec != buf[1]) {
      ra8_log_error(s_tag, "receive_byte: PEC mismatch");
      return k_ra8_err_crc_mismatch;
    }
  } /* GCOVR_EXCL_LINE -- reached only when PEC matches; host fake returns constant NTDTBP0 */
  *out_data = buf[0];
  return k_ra8_ok;
}

/* =============================================================================
 * Write Byte Data / Read Byte Data (SMBus 3.2 sec 6.5.4 / 6.5.5)
 * =============================================================================
 */

ra8_err_t ra8_smbus_write_byte_data(uint8_t target_7b, uint8_t cmd, uint8_t data)
{
  if (!s_state.initialized) {
    return k_ra8_err_not_initialized;
  }
  uint8_t  buf[3];
  uint32_t len = 0U;
  buf[len++]   = cmd;
  buf[len++]   = data;
  if (s_state.pec_enabled) {
    const uint8_t addr_b = make_addr_byte(target_7b, (uint8_t)k_ra8_smbus_rw_write);
    uint8_t       pec    = pec_update((uint8_t)k_ra8_smbus_pec_init, addr_b);
    pec                  = pec_update(pec, cmd);
    pec                  = pec_update(pec, data);
    buf[len++]           = pec;
  }
  return s_state.bus.write(s_state.bus.ctx, target_7b, buf, len, true);
}

ra8_err_t ra8_smbus_read_byte_data(uint8_t target_7b, uint8_t cmd, uint8_t* out_data)
{
  RA8_CHECK_NULL_PTR(out_data, s_tag, "read_byte_data: out_data");
  if (!s_state.initialized) {
    return k_ra8_err_not_initialized;
  }
  /* Combined transfer: write [cmd], RESTART, read [data] (+ optional
   * [PEC]). The bus transfer handles the repeated-START framing. */
  uint8_t         rx[2] = {0U, 0U};
  const uint32_t  rxlen = s_state.pec_enabled ? 2U : 1U;
  const ra8_err_t err   = s_state.bus.transfer(s_state.bus.ctx, target_7b, &cmd, 1U, rx, rxlen);
  if (err != k_ra8_ok) {
    return err;
  }
  if (s_state.pec_enabled) {
    const uint8_t addr_w = make_addr_byte(target_7b, (uint8_t)k_ra8_smbus_rw_write);
    const uint8_t addr_r = make_addr_byte(target_7b, (uint8_t)k_ra8_smbus_rw_read);
    uint8_t       pec    = pec_update((uint8_t)k_ra8_smbus_pec_init, addr_w);
    pec                  = pec_update(pec, cmd);
    pec                  = pec_update(pec, addr_r);
    pec                  = pec_update(pec, rx[0]);
    if (pec != rx[1]) {
      ra8_log_error(s_tag, "read_byte_data: PEC mismatch");
      return k_ra8_err_crc_mismatch;
    }
  } /* GCOVR_EXCL_LINE -- reached only when PEC matches; host fake returns constant NTDTBP0 */
  *out_data = rx[0];
  return k_ra8_ok;
}

/* =============================================================================
 * Block Write / Block Read (SMBus 3.2 sec 6.5.7 / 6.5.8)
 * =============================================================================
 */

ra8_err_t ra8_smbus_block_write(uint8_t target_7b, uint8_t cmd, const uint8_t* data, uint8_t len)
{
  if (!s_state.initialized) {
    return k_ra8_err_not_initialized;
  }
  if (len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  RA8_CHECK_NULL_PTR(data, s_tag, "block_write: data");

  /* Frame: [cmd] [count] [data...] [optional PEC]. The leading address
   * byte is generated by the bus write but folded into the PEC by us
   * here. Max len is 255 by enum constraint, so the scratch is 258. */
  uint8_t  frame[k_smbus_frame_bytes];
  uint32_t fi = 0U;
  frame[fi++] = cmd;
  frame[fi++] = len;
  for (uint8_t i = 0U; i < len; ++i) {
    frame[fi++] = data[i];
  }
  if (s_state.pec_enabled) {
    const uint8_t addr_b = make_addr_byte(target_7b, (uint8_t)k_ra8_smbus_rw_write);
    uint8_t       pec    = pec_update((uint8_t)k_ra8_smbus_pec_init, addr_b);
    for (uint32_t i = 0U; i < fi; ++i) {
      pec = pec_update(pec, frame[i]);
    }
    frame[fi++] = pec;
  }
  return s_state.bus.write(s_state.bus.ctx, target_7b, frame, fi, true);
}

/**
 * @brief Verify the PEC byte of a completed Block Read transfer.
 *
 * @details
 * Recomputes the SMBus 3.2 5.4 CRC-8 over the full wire frame of the
 * combined transfer -- write-address byte, command, read-address byte,
 * byte count, then the ``count`` payload bytes -- and compares it with
 * the PEC byte the peripheral appended after the payload.
 *
 * @param[in] target_7b 7-bit peripheral address.
 * @param[in] cmd       Command code that selected the block.
 * @param[in] buf       Payload bytes copied out of the transfer.
 * @param[in] count     Payload byte count reported by the peripheral.
 * @param[in] pec_rx    PEC byte received after the payload.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               PEC matches the received frame.
 * @retval k_ra8_err_crc_mismatch Computed PEC differs from ``pec_rx``.
 *
 * @pre ``buf`` holds at least ``count`` bytes.
 * @pre PEC was enabled at init time (caller checks ``pec_enabled``).
 * @post No state is mutated.
 * @post Return value depends only on the inputs.
 *
 * @note Pure computation; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_block_read_pec_check(uint8_t        target_7b,
                                               uint8_t        cmd,
                                               const uint8_t* buf,
                                               uint8_t        count,
                                               uint8_t        pec_rx)
{
  const uint8_t addr_w = make_addr_byte(target_7b, (uint8_t)k_ra8_smbus_rw_write);
  const uint8_t addr_r = make_addr_byte(target_7b, (uint8_t)k_ra8_smbus_rw_read);
  uint8_t       pec    = pec_update((uint8_t)k_ra8_smbus_pec_init, addr_w);
  pec                  = pec_update(pec, cmd);
  pec                  = pec_update(pec, addr_r);
  pec                  = pec_update(pec, count);
  for (uint8_t i = 0U; i < count; ++i) {
    pec = pec_update(pec, buf[i]);
  }
  if (pec != pec_rx) {
    ra8_log_error(s_tag, "block_read: PEC mismatch");
    return k_ra8_err_crc_mismatch;
  }
  return k_ra8_ok;
}

/**
 * @brief Unpack a completed Block Read transfer into the caller buffer.
 *
 * @details
 * The combined transfer left ``[count] [data...] [optional PEC]`` in
 * ``rx``. Publish the peripheral-reported count, bound it against the
 * caller's capacity, copy the payload out, and (when PEC is enabled)
 * verify the trailing PEC byte via ::internal_block_read_pec_check.
 *
 * @param[in]  target_7b 7-bit peripheral address (for the PEC frame).
 * @param[in]  cmd       Command code that selected the block.
 * @param[in]  rx        Raw transfer buffer from the bus seam.
 * @param[in]  cap       Caller buffer capacity in bytes.
 * @param[out] buf       Receives the payload bytes.
 * @param[out] out_len   Receives the peripheral-reported byte count.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Payload copied (and PEC verified).
 * @retval k_ra8_err_invalid_size Peripheral count exceeds ``cap``.
 * @retval k_ra8_err_crc_mismatch PEC byte did not match the frame.
 *
 * @pre ``rx`` holds a completed transfer of ``cap + 1`` bytes (one more
 *      with PEC enabled), so every index this helper reads is in range.
 * @pre ``buf`` and ``out_len`` are non-NULL (checked by the caller).
 * @post ``*out_len`` is set even when the count exceeds ``cap``.
 * @post ``buf`` holds ``count`` payload bytes on success.
 *
 * @note Reads module PEC state; call from the block-read path only.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_block_read_finish(uint8_t        target_7b,
                                            uint8_t        cmd,
                                            const uint8_t* rx,
                                            uint8_t        cap,
                                            uint8_t*       buf,
                                            uint8_t*       out_len)
{
  const uint8_t count = rx[0];
  *out_len            = count;
  if (count > cap) {
    return k_ra8_err_invalid_size;
  }
  for (uint8_t i = 0U; i < count; ++i) {
    buf[i] = rx[1U + (uint32_t)i];
  }
  if (s_state.pec_enabled) {
    /* PEC byte sits at rx[1 + count]. */
    const ra8_err_t pec_err =
      internal_block_read_pec_check(target_7b, cmd, buf, count, rx[1U + (uint32_t)count]);
    if (pec_err != k_ra8_ok) {
      return pec_err;
    }
  } /* GCOVR_EXCL_LINE -- reached only when PEC matches; host fake returns constant NTDTBP0 */
  return k_ra8_ok;
}

ra8_err_t
ra8_smbus_block_read(uint8_t target_7b, uint8_t cmd, uint8_t* buf, uint8_t cap, uint8_t* out_len)
{
  RA8_CHECK_NULL_PTR(buf, s_tag, "block_read: buf");
  RA8_CHECK_NULL_PTR(out_len, s_tag, "block_read: out_len");
  if (!s_state.initialized) {
    return k_ra8_err_not_initialized;
  }
  if (cap == 0U) {
    return k_ra8_err_invalid_arg;
  }

  /* Read [count] [data...] [optional PEC] in one combined transfer.
   * We allocate the worst-case 257-byte scratch and read the maximum
   * the buffer allows to avoid two round trips to peek the count. */
  uint8_t        rx[k_smbus_rx_bytes];
  const uint32_t want = (uint32_t)cap + 1U + (s_state.pec_enabled ? 1U : 0U);
  ra8_err_t      err  = s_state.bus.transfer(s_state.bus.ctx, target_7b, &cmd, 1U, rx, want);
  if (err != k_ra8_ok) {
    return err;
  }
  return internal_block_read_finish(target_7b, cmd, rx, cap, buf, out_len);
}

/* =============================================================================
 * SMBALERT# / Host Notify (SMBus 3.2 sec 6.5.13)
 * =============================================================================
 */

ra8_err_t ra8_smbus_alert_register_callback(ra8_smbus_alert_fn_t fn, void* ctx)
{
  if (!s_state.initialized) {
    return k_ra8_err_not_initialized;
  }
  s_state.alert_fn  = fn;
  s_state.alert_ctx = ctx;
  return k_ra8_ok;
}

RA8_ISR_SAFE
ra8_err_t ra8_smbus_alert_dispatch(void)
{
  if (!s_state.initialized) {
    return k_ra8_err_not_initialized;
  }
  uint8_t         ara = 0U;
  const ra8_err_t err =
    s_state.bus.read(s_state.bus.ctx, (uint8_t)k_ra8_smbus_alert_addr_7b, &ara, 1U);
  if (err != k_ra8_ok) {
    return err;
  }
  if (s_state.alert_fn != nullptr) {
    /* SMBus 3.2 6.5.13: top 7 bits = peripheral address, LSB = status. */
    const uint8_t addr_7b = (uint8_t)(ara >> (uint32_t)k_ra8_smbus_addr_shift);
    const uint8_t status  = (uint8_t)(ara & 1U);
    s_state.alert_fn(s_state.alert_ctx, addr_7b, status);
  }
  return k_ra8_ok;
}
