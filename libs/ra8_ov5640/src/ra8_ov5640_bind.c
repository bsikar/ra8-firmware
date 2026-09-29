/**
 * @file ra8_ov5640_bind.c
 * @brief OV5640 binder over the house I2C seam -- implementation
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details
 * Holds the one adapter that used to be written out in every consuming
 * app: the translation between the part's SCCB transport interface
 * (::ra8_ov5640_bus_t, kept because the sensor is addressed with 16-bit
 * register pointers) and the house I2C seam ::ra8_i2c_bus_ops_t, which
 * an app binds to RIIC or to the I3C block's I2C-compatibility mode
 * through ``ra8_io_i2c_bus``.
 *
 * Kept out of ``ra8_ov5640.c`` deliberately: that TU is the pure
 * register-level driver and depends on nothing but ``ra8_err`` and the
 * callbacks it is handed. This TU is the one place that names a bus
 * abstraction, so the split stays readable in the link map. Same shape
 * as ``ra8_lsm6dso_bind.c``, the sibling binder.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_i2c_bus_ops.h"
#include "ra8_ov5640.h"

/* =============================================================================
 * File-local constants
 * =============================================================================
 */

/** @brief Tag for ra8_log lines from this TU. */
static const char* const s_ov5640_bind_tag = "ov5640_bind";

/** @brief Bit shift selecting the high byte of a 16-bit register pointer. */
typedef enum : uint16_t {
  k_ov5640_bind_high_byte_shift = 8U, /**< Register pointer high-byte shift. */
} ra8_ov5640_bind_shift_t;

/* =============================================================================
 * Seam trampolines
 * =============================================================================
 */

/**
 * @brief Pack a 16-bit register pointer big-endian, as SCCB sends it.
 *
 * @param[in]  reg     Register address.
 * @param[out] out_buf Two-byte destination (non-NULL).
 *
 * @pre @p out_buf holds ::k_ra8_ov5640_i2c_reg_bytes bytes.
 * @post @p out_buf carries the high byte then the low byte.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static inline void internal_pack_reg(uint16_t reg, uint8_t* out_buf)
{
  out_buf[0] = (uint8_t)(reg >> (uint16_t)k_ov5640_bind_high_byte_shift);
  out_buf[1] = (uint8_t)reg;
}

/**
 * @brief Read one register through the bound house seam.
 *
 * @details
 * One write-RESTART-read transaction: the two-byte register pointer goes
 * out, a repeated START follows, and the sensor returns the byte. That is
 * exactly the seam's ``transfer`` shape, so this adds no framing of its
 * own beyond packing the pointer.
 *
 * @param[in]  ctx       Bound ::ra8_ov5640_i2c_ctx_t (never NULL once bound).
 * @param[in]  address   7-bit SCCB address the sensor is probing or using.
 * @param[in]  reg       Register address.
 * @param[out] out_value Destination byte (non-NULL).
 *
 * @return ``ra8_err_t`` forwarded from the bound transport.
 * @retval k_ra8_err_null_ptr ``ctx`` or ``out_value`` is NULL.
 *
 * @pre ``ctx`` was filled by ::ra8_ov5640_bind_i2c.
 * @post The wire transaction is complete or an error is returned; no
 *       state in ``ctx`` changes either way.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_ov5640_i2c_read(void* ctx, uint8_t address, uint16_t reg, uint8_t* out_value)
{
  RA8_CHECK_NULL_PTR(ctx, s_ov5640_bind_tag, "i2c_read: ctx");
  RA8_CHECK_NULL_PTR(out_value, s_ov5640_bind_tag, "i2c_read: out_value");
  const ra8_ov5640_i2c_ctx_t* c = (const ra8_ov5640_i2c_ctx_t*)ctx;

  uint8_t reg_bytes[k_ra8_ov5640_i2c_reg_bytes] = {};
  internal_pack_reg(reg, reg_bytes);
  return c->bus
    .transfer(c->bus.ctx, address, reg_bytes, (uint32_t)k_ra8_ov5640_i2c_reg_bytes, out_value, 1U);
}

/**
 * @brief Write one register through the bound house seam.
 *
 * @details
 * The seam writes a whole frame in one call, so ``[reg_hi][reg_lo][value]``
 * is staged contiguously on a stack buffer and sent as a single write
 * terminated with STOP, byte-for-byte what the board adapter issued.
 *
 * @param[in] ctx     Bound ::ra8_ov5640_i2c_ctx_t (never NULL once bound).
 * @param[in] address 7-bit SCCB address the sensor is probing or using.
 * @param[in] reg     Register address.
 * @param[in] value   Byte to store.
 *
 * @return ``ra8_err_t`` forwarded from the bound transport.
 * @retval k_ra8_err_null_ptr ``ctx`` is NULL.
 *
 * @pre ``ctx`` was filled by ::ra8_ov5640_bind_i2c.
 * @post On a refusal nothing reaches the wire.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_ov5640_i2c_write(void* ctx, uint8_t address, uint16_t reg, uint8_t value)
{
  RA8_CHECK_NULL_PTR(ctx, s_ov5640_bind_tag, "i2c_write: ctx");
  const ra8_ov5640_i2c_ctx_t* c = (const ra8_ov5640_i2c_ctx_t*)ctx;

  uint8_t frame[k_ra8_ov5640_i2c_frame_bytes] = {};
  internal_pack_reg(reg, frame);
  frame[k_ra8_ov5640_i2c_reg_bytes] = value;
  return c->bus.write(c->bus.ctx, address, frame, (uint32_t)k_ra8_ov5640_i2c_frame_bytes, true);
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

/* See the public header for the documented contract. */
ra8_err_t ra8_ov5640_bind_i2c(ra8_ov5640_t*            out_dev,
                              ra8_ov5640_i2c_ctx_t*    out_ctx,
                              const ra8_i2c_bus_ops_t* ops,
                              ra8_ov5640_delay_fn_t    delay_ms)
{
  RA8_CHECK_NULL_PTR(out_dev, s_ov5640_bind_tag, "bind_i2c: out_dev");
  RA8_CHECK_NULL_PTR(out_ctx, s_ov5640_bind_tag, "bind_i2c: out_ctx");
  RA8_CHECK_NULL_PTR(ops, s_ov5640_bind_tag, "bind_i2c: ops");
  RA8_CHECK_NULL_PTR((const void*)delay_ms, s_ov5640_bind_tag, "bind_i2c: delay_ms");
  RA8_CHECK_NULL_PTR((const void*)ops->write, s_ov5640_bind_tag, "bind_i2c: ops->write");
  RA8_CHECK_NULL_PTR((const void*)ops->transfer, s_ov5640_bind_tag, "bind_i2c: ops->transfer");

  *out_ctx = (ra8_ov5640_i2c_ctx_t){.bus = *ops};

  const ra8_ov5640_bus_t bus = {
    .read_reg  = internal_ov5640_i2c_read,
    .write_reg = internal_ov5640_i2c_write,
    .delay_ms  = delay_ms,
    .ctx       = out_ctx,
  };
  return ra8_ov5640_init(out_dev, &bus);
}
