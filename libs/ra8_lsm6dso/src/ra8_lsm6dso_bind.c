/**
 * @file ra8_lsm6dso_bind.c
 * @brief LSM6DSO binder over the house I2C seam -- implementation
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details
 * Holds the one adapter that used to live in every consuming app: the
 * translation between the part's register-level transport interface
 * (::ra8_lsm6dso_bus_t, kept because the part also runs on SPI) and the
 * house I2C seam ::ra8_i2c_bus_ops_t, which an app binds to RIIC or to
 * the I3C block's I2C-compatibility mode through ``ra8_io_i2c_bus``.
 *
 * Kept out of ``ra8_lsm6dso.c`` deliberately: that TU is the pure
 * register-level driver and depends on nothing but ``ra8_err`` and the
 * callbacks it is handed. This TU is the one place that names a bus
 * abstraction, so the split stays readable in the link map.
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
#include "ra8_lsm6dso.h"

/* =============================================================================
 * File-local constants
 * =============================================================================
 */

/** @brief Tag for ra8_log lines from this TU. */
static const char* const s_lsm6dso_bind_tag = "lsm6dso_bind";

/** @brief Highest valid 7-bit I2C address. */
typedef enum : uint8_t {
  k_lsm6dso_bind_addr_7b_max = 0x7FU, /**< 7-bit address space ceiling. */
} ra8_lsm6dso_bind_addr_t;

/* =============================================================================
 * Seam trampolines
 * =============================================================================
 */

/**
 * @brief Read ``len`` bytes from ``reg`` through the bound house seam.
 *
 * @details
 * One write-RESTART-read transaction: the register address goes out,
 * a repeated START follows, and the part streams back auto-incremented
 * bytes (DS12140 sec 6.1.2). That is exactly the seam's ``transfer``
 * shape, so this is a forward with no framing of its own.
 *
 * @param[in]  ctx Bound ::ra8_lsm6dso_i2c_ctx_t (never NULL once bound).
 * @param[in]  reg First register address.
 * @param[out] buf Destination buffer (non-NULL when ``len`` > 0).
 * @param[in]  len Byte count.
 *
 * @return ``ra8_err_t`` forwarded from the bound transport.
 * @retval k_ra8_err_null_ptr ``ctx`` is NULL.
 *
 * @pre ``ctx`` was filled by ::ra8_lsm6dso_bind_i2c.
 * @post The wire transaction is complete or an error is returned; no
 *       state in ``ctx`` changes either way.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_lsm6dso_i2c_read(void* ctx, uint8_t reg, uint8_t* buf, uint32_t len)
{
  RA8_CHECK_NULL_PTR(ctx, s_lsm6dso_bind_tag, "i2c_read: ctx");
  const ra8_lsm6dso_i2c_ctx_t* c = (const ra8_lsm6dso_i2c_ctx_t*)ctx;
  return c->bus.transfer(c->bus.ctx, c->target_7b, &reg, 1U, buf, len);
}

/**
 * @brief Write ``len`` bytes starting at ``reg`` through the bound seam.
 *
 * @details
 * The seam writes a whole frame in one call, so ``[reg][payload]`` is
 * staged contiguously on a stack buffer and sent as a single write
 * terminated with STOP. The stage is capped at
 * ::k_lsm6dso_i2c_frame_bytes_max; the driver only ever writes one
 * payload byte, so the cap is there to fail a future multi-byte write
 * loudly rather than overrun.
 *
 * @param[in] ctx Bound ::ra8_lsm6dso_i2c_ctx_t (never NULL once bound).
 * @param[in] reg First register address.
 * @param[in] buf Source buffer (non-NULL when ``len`` > 0).
 * @param[in] len Byte count.
 *
 * @return ``ra8_err_t`` forwarded from the bound transport.
 * @retval k_ra8_err_null_ptr    ``ctx`` is NULL, or ``buf`` is NULL with
 *                              a non-zero ``len``.
 * @retval k_ra8_err_invalid_arg ``len`` does not fit the staged frame.
 *
 * @pre ``ctx`` was filled by ::ra8_lsm6dso_bind_i2c.
 * @post On a refusal nothing reaches the wire.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_lsm6dso_i2c_write(void* ctx, uint8_t reg, const uint8_t* buf, uint32_t len)
{
  RA8_CHECK_NULL_PTR(ctx, s_lsm6dso_bind_tag, "i2c_write: ctx");
  if (len > ((uint32_t)k_lsm6dso_i2c_frame_bytes_max - 1U)) {
    return k_ra8_err_invalid_arg;
  }
  if ((len > 0U) && (buf == nullptr)) {
    return k_ra8_err_null_ptr;
  }

  uint8_t frame[k_lsm6dso_i2c_frame_bytes_max] = {};
  frame[0]                                     = reg;
  for (uint32_t i = 0U; i < len; ++i) {
    frame[i + 1U] = buf[i];
  }

  const ra8_lsm6dso_i2c_ctx_t* c = (const ra8_lsm6dso_i2c_ctx_t*)ctx;
  return c->bus.write(c->bus.ctx, c->target_7b, frame, len + 1U, true);
}

/* =============================================================================
 * Binder
 * =============================================================================
 */

ra8_err_t ra8_lsm6dso_bind_i2c(ra8_lsm6dso_t*           out_dev,
                               ra8_lsm6dso_i2c_ctx_t*   out_ctx,
                               const ra8_i2c_bus_ops_t* ops,
                               uint8_t                  target_7b)
{
  RA8_CHECK_NULL_PTR(out_dev, s_lsm6dso_bind_tag, "bind_i2c: out_dev");
  RA8_CHECK_NULL_PTR(out_ctx, s_lsm6dso_bind_tag, "bind_i2c: out_ctx");
  RA8_CHECK_NULL_PTR(ops, s_lsm6dso_bind_tag, "bind_i2c: ops");
  RA8_CHECK_NULL_PTR(ops->write, s_lsm6dso_bind_tag, "bind_i2c: ops.write");
  RA8_CHECK_NULL_PTR(ops->transfer, s_lsm6dso_bind_tag, "bind_i2c: ops.transfer");
  if (target_7b > (uint8_t)k_lsm6dso_bind_addr_7b_max) {
    return k_ra8_err_invalid_arg;
  }

  const ra8_lsm6dso_i2c_ctx_t staged = {
    .bus       = *ops,
    .target_7b = target_7b,
  };
  const ra8_lsm6dso_bus_t bus = {
    .read_regs  = internal_lsm6dso_i2c_read,
    .write_regs = internal_lsm6dso_i2c_write,
    .ctx        = out_ctx,
  };

  *out_ctx              = staged;
  const ra8_err_t bound = ra8_lsm6dso_init(out_dev, &bus);
  if (bound != k_ra8_ok) {
    *out_ctx = (ra8_lsm6dso_i2c_ctx_t){};
  }
  return bound;
}
