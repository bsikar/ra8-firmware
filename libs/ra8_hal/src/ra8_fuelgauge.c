/**
 * @file ra8_fuelgauge.c
 * @brief Battery fuel-gauge driver -- MAX17048 backend (implementation)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * MAX17048-specific implementation of `ra8_fuelgauge.h`. Every access goes
 * through the injected I2C seam (::ra8_i2c_bus_ops_t), so the driver names
 * no peripheral and performs no MMIO of its own -- the bound transport
 * carries the HUM citations. All state lives in the caller's handle.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_fuelgauge.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_fuelgauge_max17048_regs.h"
#include "ra8_i2c_bus_ops.h"

/** @brief Log tag. */
static const char* s_tag = "FUELGAUGE";

/**
 * @enum ra8_fuelgauge_internal_t
 * @brief Implementation-only constants (no magic numbers).
 */
typedef enum : uint32_t {
  k_ra8_fuelgauge_byte_shift  = 8U,      /**< Bits per byte.              */
  k_ra8_fuelgauge_byte_mask   = 0xFFU,   /**< 8-bit byte mask.            */
  k_ra8_fuelgauge_probe_dead  = 0x0000U, /**< Segment stuck low reading.  */
  k_ra8_fuelgauge_probe_float = 0xFFFFU, /**< Segment stuck high reading. */
} ra8_fuelgauge_internal_t;

/* ===========================================================================
 * Helpers -- transport + decoding
 * ===========================================================================
 */

/**
 * @brief Read one 16-bit register, MSB first.
 *
 * @details
 * Issues the part's standard access: write the one-byte register pointer,
 * inject a repeated START, read two bytes back.
 *
 * @param[in]  fg  Open handle.
 * @param[in]  reg Register pointer.
 * @param[out] out Decoded 16-bit value, written only on success.
 *
 * @return ``bus.transfer`` return code.
 * @retval k_ra8_ok Register read and decoded.
 *
 * @pre ``fg`` is open and ``out`` is non-NULL.
 * @post On failure ``*out`` is untouched.
 * @note Not thread-safe with respect to the same handle.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_read_reg16(const ra8_fuelgauge_t* fg, uint8_t reg, uint16_t* out)
{
  uint8_t   raw[k_ra8_fuelgauge_max17048_reg_bytes] = {0U, 0U};
  ra8_err_t err = fg->bus.transfer(fg->bus.ctx,
                                   fg->target_7b,
                                   &reg,
                                   (uint32_t)k_ra8_fuelgauge_max17048_reg_ptr_bytes,
                                   raw,
                                   (uint32_t)k_ra8_fuelgauge_max17048_reg_bytes);
  if (err != k_ra8_ok) {
    return err;
  }

  *out = (uint16_t)(((uint32_t)raw[0] << k_ra8_fuelgauge_byte_shift) |
                    ((uint32_t)raw[1] & k_ra8_fuelgauge_byte_mask));
  return k_ra8_ok;
}

/**
 * @brief Convert a raw VCELL reading to millivolts.
 *
 * @details
 * The MAX17048 VCELL LSB is 78.125 uV, which is exactly 5/64 mV, so the
 * conversion is integer arithmetic with no floating point and no rounding
 * table. The widest input (0xFFFF) yields 5119 mV, so the result fits a
 * ``uint16_t`` for every possible reading.
 *
 * @param[in] raw Raw VCELL register value.
 *
 * @return Cell voltage in millivolts, truncated toward zero.
 *
 * @pre None.
 * @post No state is changed.
 * @note Pure function; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint16_t internal_vcell_mv(uint16_t raw)
{
  return (uint16_t)(((uint32_t)raw * (uint32_t)k_ra8_fuelgauge_max17048_vcell_mv_num) /
                    (uint32_t)k_ra8_fuelgauge_max17048_vcell_mv_den);
}

/* ===========================================================================
 * Public API
 * ===========================================================================
 */

ra8_err_t ra8_fuelgauge_open(ra8_fuelgauge_t* fg, const ra8_fuelgauge_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(fg, s_tag, "fg is NULL");
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg is NULL");

  if (cfg->bus.transfer == NULL) {
    return k_ra8_err_invalid_arg;
  }

  const ra8_fuelgauge_t probe = {
    .bus       = cfg->bus,
    .target_7b = cfg->target_7b,
    .opened    = true,
  };

  uint16_t  version = 0U;
  ra8_err_t err =
    internal_read_reg16(&probe, (uint8_t)k_ra8_fuelgauge_max17048_reg_version, &version);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((version == (uint16_t)k_ra8_fuelgauge_probe_dead) ||
      (version == (uint16_t)k_ra8_fuelgauge_probe_float)) {
    return k_ra8_err_hw_not_ready;
  }

  *fg = probe;
  return k_ra8_ok;
}

ra8_err_t ra8_fuelgauge_read(ra8_fuelgauge_t* fg, ra8_fuelgauge_state_t* out)
{
  RA8_CHECK_NULL_PTR(fg, s_tag, "fg is NULL");
  RA8_CHECK_NULL_PTR(out, s_tag, "out is NULL");

  if (!fg->opened) {
    return k_ra8_err_not_initialized;
  }

  uint16_t  vcell = 0U;
  ra8_err_t err = internal_read_reg16(fg, (uint8_t)k_ra8_fuelgauge_max17048_reg_vcell, &vcell);
  if (err != k_ra8_ok) {
    return err;
  }

  uint16_t soc = 0U;
  err          = internal_read_reg16(fg, (uint8_t)k_ra8_fuelgauge_max17048_reg_soc, &soc);
  if (err != k_ra8_ok) {
    return err;
  }

  uint16_t crate = 0U;
  err            = internal_read_reg16(fg, (uint8_t)k_ra8_fuelgauge_max17048_reg_crate, &crate);
  if (err != k_ra8_ok) {
    return err;
  }

  const int16_t crate_signed = (int16_t)crate;

  out->vcell_mv = internal_vcell_mv(vcell);
  out->soc_pct  = (uint8_t)(((uint32_t)soc >> k_ra8_fuelgauge_byte_shift) &
                           (uint32_t)k_ra8_fuelgauge_byte_mask);
  out->crate_raw = crate_signed;
  out->charging  = (crate_signed >= 0);
  return k_ra8_ok;
}

ra8_err_t ra8_fuelgauge_close(ra8_fuelgauge_t* fg)
{
  RA8_CHECK_NULL_PTR(fg, s_tag, "fg is NULL");

  if (!fg->opened) {
    return k_ra8_err_not_initialized;
  }

  const ra8_fuelgauge_t cleared = {};
  *fg                           = cleared;
  return k_ra8_ok;
}
