/**
 * @file ra8_lsm6dso.c
 * @brief ST LSM6DSO 6-DoF IMU driver -- implementation
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details
 * Polling, transport-agnostic implementation. All register-level
 * citations point at LSM6DSO DS12140 Rev 4 (Sept 2019). The transport
 * is supplied by the caller (Dependency Inversion) so this TU does
 * not link against ``ra8_i3c_i2c`` or ``ra8_spi``.
 *
 * Algorithm style throughout this file:
 *   1. Validate ``dev`` / ``bus`` / output pointer.
 *   2. Verify ``dev->initialized``.
 *   3. Validate the enum argument is in range (config setters only).
 *   4. Issue read / read-modify-write / write via the transport.
 *   5. Update ``dev->*_code`` cache only on success.
 *
 * Each public function carries at least two precondition checks and
 * two postcondition statements to satisfy NASA P10 Rule 5.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_lsm6dso.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"

/* =============================================================================
 * File-local constants and helpers
 * =============================================================================
 */

/** @brief Tag for ra8_log_error / ra8_log_info lines from this TU. */
static const char* const s_lsm6dso_tag = "lsm6dso";

/** @brief Layout constants used by burst-read helpers. */
typedef enum : uint32_t {
  k_lsm6dso_xyz_burst_bytes   = 6U, /**< 3 axes * 2 bytes (DS12140 sec 9.29 / 9.35).     */
  k_lsm6dso_temp_burst_bytes  = 2U, /**< OUT_TEMP_L + OUT_TEMP_H (DS12140 sec 9.27).     */
  k_lsm6dso_fifo_status_bytes = 2U, /**< FIFO_STATUS1 + FIFO_STATUS2 (DS12140 sec 9.44). */
} ra8_lsm6dso_burst_bytes_t;

/** @brief Two's-complement byte combination -- 0..1 indices. */
typedef enum : uint8_t {
  k_lsm6dso_idx_low  = 0U, /**< Little-endian low  byte. */
  k_lsm6dso_idx_high = 1U, /**< Little-endian high byte. */
} ra8_lsm6dso_endian_idx_t;

/** @brief XYZ sample byte offsets within the 6-byte burst. */
typedef enum : uint8_t {
  k_lsm6dso_xyz_off_x_l = 0U, /**< Lsm6dso xyz off x l. */
  k_lsm6dso_xyz_off_x_h = 1U, /**< Lsm6dso xyz off x h. */
  k_lsm6dso_xyz_off_y_l = 2U, /**< Lsm6dso xyz off y l. */
  k_lsm6dso_xyz_off_y_h = 3U, /**< Lsm6dso xyz off y h. */
  k_lsm6dso_xyz_off_z_l = 4U, /**< Lsm6dso xyz off z l. */
  k_lsm6dso_xyz_off_z_h = 5U, /**< Lsm6dso xyz off z h. */
} ra8_lsm6dso_xyz_off_t;

/** @brief CTRL1_XL / CTRL2_G field positions (DS12140 sec 9.12 / 9.13). */
typedef enum : uint8_t {
  k_lsm6dso_shift_odr    = 4U, /**< ODR_xL[3:0] / ODR_G[3:0] occupy bits [7:4].      */
  k_lsm6dso_shift_fs_xl  = 2U, /**< FS_XL[1:0]                  occupies bits [3:2]. */
  k_lsm6dso_shift_fs_g   = 2U, /**< FS_G[1:0]                   occupies bits [3:2]. */
  k_lsm6dso_shift_fs_125 = 1U, /**< FS_125 select bit           occupies bit  [1].   */
} ra8_lsm6dso_shift_t;

typedef enum : uint8_t {
  k_lsm6dso_mask_odr       = 0xF0U, /**< Bits [7:4]: ODR field.               */
  k_lsm6dso_mask_fs_xl     = 0x0CU, /**< Bits [3:2]: FS_XL field.             */
  k_lsm6dso_mask_fs_g      = 0x0CU, /**< Bits [3:2]: FS_G field.              */
  k_lsm6dso_mask_fs_125    = 0x02U, /**< Bit  [1]:   FS_125 select.           */
  k_lsm6dso_mask_fs_g_full = 0x0EU, /**< FS_G + FS_125 combined.              */
  k_lsm6dso_mask_nibble    = 0x0FU, /**< Low nibble (used for FIFO len high). */
  k_lsm6dso_mask_byte      = 0xFFU, /**< Full byte mask.                      */
} ra8_lsm6dso_mask_t;

/** @brief Temperature conversion constants per DS12140 sec 9.27. */
typedef enum : int32_t {
  k_lsm6dso_temp_offset_centi_c = 2500, /**< +25 C zero offset, scaled x100.  */
  k_lsm6dso_temp_scale_num      = 100,  /**< Numerator   for centi-C convert. */
  k_lsm6dso_temp_scale_den      = 256,  /**< Denominator for centi-C convert. */
} ra8_lsm6dso_temp_const_t;

/** @brief Enum upper bounds used by RA8_CHECK_RANGE_TAG. */
typedef enum : uint8_t {
  k_lsm6dso_xl_fs_max = (uint8_t)k_lsm6dso_g_fs_2000dps, /**< shared cap value 0x04. */
  k_lsm6dso_xl_fs_cap = (uint8_t)k_lsm6dso_xl_fs_8g,     /**< Highest XL FS code.    */
  k_lsm6dso_g_fs_cap  = (uint8_t)k_lsm6dso_g_fs_2000dps, /**< Highest G  FS code.    */
  k_lsm6dso_odr_cap   = (uint8_t)k_lsm6dso_odr_6660hz,   /**< Highest ODR code.      */
} ra8_lsm6dso_cap_t;

/* =============================================================================
 * Internal: build the byte that programs CTRL2_G's FS field
 * =============================================================================
 */

/**
 * @brief Compose the FS_G + FS_125 sub-field of CTRL2_G for the
 *        requested gyro full-scale code.
 *
 * @details
 * Per DS12140 sec 9.13 "CTRL2_G (11h)":
 *   - FS_125 (bit 1) = 1 selects the +-125 dps narrow scale; the
 *     FS_G[1:0] field is then a don't-care.
 *   - For FS_G code in ``{250, 500, 1000, 2000} dps`` FS_125 must be
 *     cleared and FS_G[1:0] takes the value from ``ra8_lsm6dso_g_fs_t``
 *     minus the +-125 dps slot (i.e. ``k_lsm6dso_g_fs_250dps`` maps to
 *     FS_G = 0, ``..._500dps`` to FS_G = 1, etc).
 *
 * The return value is the 3-bit sub-field [3:1] of CTRL2_G,
 * pre-shifted into place. The caller masks in the ODR nibble.
 *
 * @param[in] fs Gyro full-scale code.
 *
 * @return 3-bit sub-field [3:1] of CTRL2_G, pre-shifted.
 * @retval 0x02 FS_125 path (``fs == k_lsm6dso_g_fs_125dps``).
 * @retval other Encoded FS_G[1:0] field for the wider scales.
 *
 * @pre ``fs`` is one of the ``k_lsm6dso_g_fs_*`` enum values.
 * @pre Caller has already range-checked ``fs`` via ``RA8_CHECK_RANGE_TAG``.
 * @post Return value occupies only bits [3:1] of a CTRL2_G byte.
 * @post ODR nibble [7:4] of the result is zero (caller OR's it in).
 *
 * @note Pure function; no MMIO or transport access.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t internal_lsm6dso_g_fs_bits(ra8_lsm6dso_g_fs_t fs)
{
  /* DS12140 Table 47: FS_125 takes priority over FS_G[1:0]. */
  if (fs == k_lsm6dso_g_fs_125dps) {
    return (uint8_t)((uint8_t)1U << (uint8_t)k_lsm6dso_shift_fs_125);
  }
  /* Numerically, _250dps == 1, _500dps == 2, _1000dps == 3, _2000dps == 4.
   * Map back to the FS_G[1:0] bit field by subtracting one. */
  const uint8_t fs_g_field = (uint8_t)((uint8_t)fs - (uint8_t)k_lsm6dso_g_fs_250dps);
  return (uint8_t)((uint8_t)(fs_g_field & 0x03U) << (uint8_t)k_lsm6dso_shift_fs_g);
}

/**
 * @brief Write a single byte to ``reg`` via the bound transport.
 *
 * @details
 * Thin wrapper around ``dev->bus.write_regs`` that pins the byte
 * count to ``1`` and stages the value on the caller's stack so the
 * transport can DMA from it. All real validation lives in the public
 * entry that invokes this helper.
 *
 * @param[in] dev Driver instance (already validated by caller).
 * @param[in] reg Register address.
 * @param[in] val Byte to write.
 *
 * @return Transport error code.
 * @retval k_ra8_ok Byte written.
 * @retval other   Forwarded from ``dev->bus.write_regs``.
 *
 * @pre ``dev`` is non-NULL.
 * @pre ``dev->bus.write_regs`` is non-NULL.
 * @post No driver state is mutated.
 * @post ``val`` storage on the caller's stack is still valid post-return.
 *
 * @note Not thread-safe per-instance.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_lsm6dso_write_byte(ra8_lsm6dso_t* dev, uint8_t reg, uint8_t val)
{
  return dev->bus.write_regs(dev->bus.ctx, reg, &val, 1U);
}

/**
 * @brief Read a single byte from ``reg`` via the bound transport.
 *
 * @details
 * Thin wrapper around ``dev->bus.read_regs`` that pins the byte count
 * to ``1``. All real validation lives in the public entry that
 * invokes this helper.
 *
 * @param[in]  dev Driver instance (already validated).
 * @param[in]  reg Register address.
 * @param[out] out Receives the byte on success.
 *
 * @return Transport error code.
 * @retval k_ra8_ok ``*out`` populated.
 * @retval other   Forwarded from ``dev->bus.read_regs``; ``*out`` unchanged.
 *
 * @pre ``dev`` is non-NULL.
 * @pre ``out`` points at writable storage.
 * @post On success ``*out`` carries the register byte.
 * @post On failure ``*out`` is left untouched.
 *
 * @note Not thread-safe per-instance.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_lsm6dso_read_byte(ra8_lsm6dso_t* dev, uint8_t reg, uint8_t* out)
{
  return dev->bus.read_regs(dev->bus.ctx, reg, out, 1U);
}

/**
 * @brief Read-modify-write the ODR nibble [7:4] of a CTRL register.
 *
 * @details
 * Used to share the ODR write path between CTRL1_XL (DS12140 sec 9.12)
 * and CTRL2_G (DS12140 sec 9.13) -- both registers place the ODR field
 * in bits [7:4] with the rest of the byte reserved for the per-axis
 * full-scale select. This helper reads the current register value,
 * masks off the existing ODR nibble, OR's in the new ODR bits, and
 * writes the merged byte back.
 *
 * @param[in] dev      Driver instance (already validated by caller).
 * @param[in] reg      CTRL register address (CTRL1_XL or CTRL2_G).
 * @param[in] odr_bits New ODR field, pre-shifted into bits [7:4].
 *
 * @return Transport error code; ``k_ra8_ok`` on success.
 * @retval k_ra8_ok Register byte updated.
 * @retval other   Forwarded from the read or write transport call.
 *
 * @pre ``dev`` is non-NULL and previously bound by ``ra8_lsm6dso_init``.
 * @pre ``odr_bits`` has bits [3:0] clear (caller pre-shifted into [7:4]).
 * @post On success bits [7:4] of ``reg`` match ``odr_bits``.
 * @post On failure ``reg`` is left untouched (read fault) or partially
 *       written (write fault); ``dev`` state is unchanged either way.
 *
 * @note Not thread-safe per-instance.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_lsm6dso_rmw_odr(ra8_lsm6dso_t* dev, uint8_t reg, uint8_t odr_bits)
{
  uint8_t         now = 0U;
  const ra8_err_t r   = internal_lsm6dso_read_byte(dev, reg, &now);
  if (r != k_ra8_ok) {
    return r;
  }
  const uint8_t merged = (uint8_t)((now & (uint8_t)~k_lsm6dso_mask_odr) | odr_bits);
  return internal_lsm6dso_write_byte(dev, reg, merged);
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

ra8_err_t ra8_lsm6dso_init(ra8_lsm6dso_t* out_dev, const ra8_lsm6dso_bus_t* bus)
{
  RA8_CHECK_NULL_PTR(out_dev, s_lsm6dso_tag, "init: out_dev");
  RA8_CHECK_NULL_PTR(bus, s_lsm6dso_tag, "init: bus");
  RA8_CHECK_NULL_PTR(bus->read_regs, s_lsm6dso_tag, "init: bus.read_regs");
  RA8_CHECK_NULL_PTR(bus->write_regs, s_lsm6dso_tag, "init: bus.write_regs");

  out_dev->bus           = *bus;
  out_dev->accel_fs_code = k_lsm6dso_xl_fs_2g;    /* DS12140 sec 9.12 reset default. */
  out_dev->gyro_fs_code  = k_lsm6dso_g_fs_250dps; /* DS12140 sec 9.13 reset default. */
  out_dev->odr_code      = k_lsm6dso_odr_off;     /* DS12140 sec 9.12 reset default. */
  out_dev->initialized   = true;
  return k_ra8_ok;
}

/* =============================================================================
 * Identification
 * =============================================================================
 */

ra8_err_t ra8_lsm6dso_who_am_i(ra8_lsm6dso_t* dev, uint8_t* out_id)
{
  RA8_CHECK_NULL_PTR(dev, s_lsm6dso_tag, "who_am_i: dev");
  RA8_CHECK_NULL_PTR(out_id, s_lsm6dso_tag, "who_am_i: out_id");
  RA8_VALIDATE_INIT(dev->initialized, s_lsm6dso_tag, "who_am_i: not initialized");

  /* DS12140 sec 9.11 WHO_AM_I (0Fh). */
  return internal_lsm6dso_read_byte(dev, (uint8_t)k_lsm6dso_reg_who_am_i, out_id);
}

/* =============================================================================
 * Configuration
 * =============================================================================
 */

ra8_err_t ra8_lsm6dso_set_accel_range(ra8_lsm6dso_t* dev, ra8_lsm6dso_xl_fs_t fs)
{
  RA8_CHECK_NULL_PTR(dev, s_lsm6dso_tag, "set_accel_range: dev");
  RA8_VALIDATE_INIT(dev->initialized, s_lsm6dso_tag, "set_accel_range: not initialized");
  RA8_CHECK_RANGE_TAG((uint8_t)fs,
                      0U,
                      (uint8_t)k_lsm6dso_xl_fs_cap,
                      k_ra8_err_invalid_arg,
                      s_lsm6dso_tag);

  /* DS12140 sec 9.12 CTRL1_XL: read-modify-write FS_XL[3:2]. */
  uint8_t         current = 0U;
  const ra8_err_t r = internal_lsm6dso_read_byte(dev, (uint8_t)k_lsm6dso_reg_ctrl1_xl, &current);
  if (r != k_ra8_ok) {
    return r;
  }
  const uint8_t merged =
    (uint8_t)((current & (uint8_t)~k_lsm6dso_mask_fs_xl) |
              (uint8_t)(((uint8_t)fs & 0x03U) << (uint8_t)k_lsm6dso_shift_fs_xl));
  const ra8_err_t w = internal_lsm6dso_write_byte(dev, (uint8_t)k_lsm6dso_reg_ctrl1_xl, merged);
  if (w != k_ra8_ok) {
    return w;
  }
  dev->accel_fs_code = fs;
  return k_ra8_ok;
}

ra8_err_t ra8_lsm6dso_set_gyro_range(ra8_lsm6dso_t* dev, ra8_lsm6dso_g_fs_t fs)
{
  RA8_CHECK_NULL_PTR(dev, s_lsm6dso_tag, "set_gyro_range: dev");
  RA8_VALIDATE_INIT(dev->initialized, s_lsm6dso_tag, "set_gyro_range: not initialized");
  RA8_CHECK_RANGE_TAG((uint8_t)fs,
                      0U,
                      (uint8_t)k_lsm6dso_g_fs_cap,
                      k_ra8_err_invalid_arg,
                      s_lsm6dso_tag);

  /* DS12140 sec 9.13 CTRL2_G: read-modify-write FS_G[3:2] + FS_125[1]. */
  uint8_t         current = 0U;
  const ra8_err_t r = internal_lsm6dso_read_byte(dev, (uint8_t)k_lsm6dso_reg_ctrl2_g, &current);
  if (r != k_ra8_ok) {
    return r;
  }
  const uint8_t   fs_bits = internal_lsm6dso_g_fs_bits(fs);
  const uint8_t   merged  = (uint8_t)((current & (uint8_t)~k_lsm6dso_mask_fs_g_full) | fs_bits);
  const ra8_err_t w = internal_lsm6dso_write_byte(dev, (uint8_t)k_lsm6dso_reg_ctrl2_g, merged);
  if (w != k_ra8_ok) {
    return w;
  }
  dev->gyro_fs_code = fs;
  return k_ra8_ok;
}

ra8_err_t ra8_lsm6dso_set_odr(ra8_lsm6dso_t* dev, ra8_lsm6dso_odr_t odr)
{
  RA8_CHECK_NULL_PTR(dev, s_lsm6dso_tag, "set_odr: dev");
  RA8_VALIDATE_INIT(dev->initialized, s_lsm6dso_tag, "set_odr: not initialized");
  RA8_CHECK_RANGE_TAG((uint8_t)odr,
                      0U,
                      (uint8_t)k_lsm6dso_odr_cap,
                      k_ra8_err_invalid_arg,
                      s_lsm6dso_tag);

  /* DS12140 sec 9.12 / 9.13: ODR field is bits [7:4] of CTRL1_XL and CTRL2_G. */
  const uint8_t odr_bits =
    (uint8_t)(((uint8_t)odr & (uint8_t)k_lsm6dso_mask_nibble) << (uint8_t)k_lsm6dso_shift_odr);

  const ra8_err_t rxl = internal_lsm6dso_rmw_odr(dev, (uint8_t)k_lsm6dso_reg_ctrl1_xl, odr_bits);
  if (rxl != k_ra8_ok) {
    return rxl;
  }
  const ra8_err_t rg = internal_lsm6dso_rmw_odr(dev, (uint8_t)k_lsm6dso_reg_ctrl2_g, odr_bits);
  if (rg != k_ra8_ok) {
    return rg;
  }

  dev->odr_code = odr;
  return k_ra8_ok;
}

/* =============================================================================
 * Sample-read helpers
 * =============================================================================
 */

/**
 * @brief Common XYZ burst-read used by both ``read_accel`` and
 *        ``read_gyro``.
 *
 * @details
 * Issues a single 6-byte burst at ``reg`` (auto-incrementing across
 * X/Y/Z low+high bytes per DS12140 sec 9.29 .. 9.40) and packs the
 * little-endian two's-complement bytes into the caller's
 * ``ra8_lsm6dso_xyz_t`` struct.
 *
 * @param[in]  dev Driver instance (validated).
 * @param[in]  reg First register address (``OUTX_L_A`` / ``OUTX_L_G``).
 * @param[out] out 3-axis raw sample.
 *
 * @return Transport error code; ``out`` is left untouched on failure.
 * @retval k_ra8_ok ``*out`` populated with the latest sample.
 * @retval other   Forwarded from ``dev->bus.read_regs``.
 *
 * @pre ``dev`` was bound by ``ra8_lsm6dso_init`` and is initialized.
 * @pre ``out`` points at writable ``ra8_lsm6dso_xyz_t`` storage.
 * @post On success ``*out`` is fully overwritten.
 * @post On failure ``*out`` is unmodified.
 *
 * @note Not thread-safe per-instance.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_lsm6dso_read_xyz(ra8_lsm6dso_t* dev, uint8_t reg, ra8_lsm6dso_xyz_t* out)
{
  uint8_t         bytes[6] = {};
  const ra8_err_t r =
    dev->bus.read_regs(dev->bus.ctx, reg, bytes, (uint32_t)k_lsm6dso_xyz_burst_bytes);
  if (r != k_ra8_ok) {
    return r;
  }
  /* LSM6DSO XYZ samples are little-endian two's complement
   * (DS12140 sec 9.29 .. 9.40). */
  out->x = (int16_t)(((uint16_t)bytes[k_lsm6dso_xyz_off_x_h] << 8) |
                     (uint16_t)bytes[k_lsm6dso_xyz_off_x_l]);
  out->y = (int16_t)(((uint16_t)bytes[k_lsm6dso_xyz_off_y_h] << 8) |
                     (uint16_t)bytes[k_lsm6dso_xyz_off_y_l]);
  out->z = (int16_t)(((uint16_t)bytes[k_lsm6dso_xyz_off_z_h] << 8) |
                     (uint16_t)bytes[k_lsm6dso_xyz_off_z_l]);
  return k_ra8_ok;
}

ra8_err_t ra8_lsm6dso_read_accel(ra8_lsm6dso_t* dev, ra8_lsm6dso_xyz_t* out)
{
  RA8_CHECK_NULL_PTR(dev, s_lsm6dso_tag, "read_accel: dev");
  RA8_CHECK_NULL_PTR(out, s_lsm6dso_tag, "read_accel: out");
  RA8_VALIDATE_INIT(dev->initialized, s_lsm6dso_tag, "read_accel: not initialized");
  /* DS12140 sec 9.35 OUTX_L_A (28h) -- auto-increment burst over 6 bytes. */
  return internal_lsm6dso_read_xyz(dev, (uint8_t)k_lsm6dso_reg_outx_l_a, out);
}

ra8_err_t ra8_lsm6dso_read_gyro(ra8_lsm6dso_t* dev, ra8_lsm6dso_xyz_t* out)
{
  RA8_CHECK_NULL_PTR(dev, s_lsm6dso_tag, "read_gyro: dev");
  RA8_CHECK_NULL_PTR(out, s_lsm6dso_tag, "read_gyro: out");
  RA8_VALIDATE_INIT(dev->initialized, s_lsm6dso_tag, "read_gyro: not initialized");
  /* DS12140 sec 9.29 OUTX_L_G (22h) -- auto-increment burst over 6 bytes. */
  return internal_lsm6dso_read_xyz(dev, (uint8_t)k_lsm6dso_reg_outx_l_g, out);
}

ra8_err_t ra8_lsm6dso_read_temp(ra8_lsm6dso_t* dev, int32_t* out_centi_c)
{
  RA8_CHECK_NULL_PTR(dev, s_lsm6dso_tag, "read_temp: dev");
  RA8_CHECK_NULL_PTR(out_centi_c, s_lsm6dso_tag, "read_temp: out_centi_c");
  RA8_VALIDATE_INIT(dev->initialized, s_lsm6dso_tag, "read_temp: not initialized");

  /* DS12140 sec 9.27 OUT_TEMP_L (20h) + sec 9.28 OUT_TEMP_H (21h). */
  uint8_t         bytes[2] = {};
  const ra8_err_t r        = dev->bus.read_regs(dev->bus.ctx,
                                                (uint8_t)k_lsm6dso_reg_out_temp_l,
                                                bytes,
                                                (uint32_t)k_lsm6dso_temp_burst_bytes);
  if (r != k_ra8_ok) {
    return r;
  }
  const int16_t raw =
    (int16_t)(((uint16_t)bytes[k_lsm6dso_idx_high] << 8) | (uint16_t)bytes[k_lsm6dso_idx_low]);
  /* DS12140 sec 4.3 "Temperature sensor characteristics":
   *   T[degC] = raw / 256 + 25
   * Scale into centi-deg C to keep integer arithmetic: */
  *out_centi_c = (int32_t)((int32_t)raw * (int32_t)k_lsm6dso_temp_scale_num /
                           (int32_t)k_lsm6dso_temp_scale_den) +
                 (int32_t)k_lsm6dso_temp_offset_centi_c;
  return k_ra8_ok;
}

/**
 * @brief Read the live FIFO word count (DIFF_FIFO[9:0]).
 *
 * @details
 * Reads FIFO_STATUS1 + FIFO_STATUS2 (DS12140 sec 9.44 / 9.45) and
 * extracts the 10-bit DIFF_FIFO field into a 32-bit count. The low
 * 8 bits live in FIFO_STATUS1, the upper 2 bits live in the low
 * nibble of FIFO_STATUS2.
 *
 * @param[in]  dev   Driver instance (already validated by caller).
 * @param[out] out_n Receives the live FIFO word count on success.
 *
 * @return Transport error code; ``k_ra8_ok`` on success.
 * @retval k_ra8_ok ``*out_n`` populated with the DIFF_FIFO count.
 * @retval other   Forwarded from ``dev->bus.read_regs``; ``*out_n`` unchanged.
 *
 * @pre ``dev`` was bound by ``ra8_lsm6dso_init`` and is initialized.
 * @pre ``out_n`` points at writable storage.
 * @post On success ``*out_n`` is in the range ``[0, 1023]`` (10-bit field).
 * @post On failure ``*out_n`` is unmodified.
 *
 * @note Not thread-safe per-instance.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_lsm6dso_read_fifo_depth(ra8_lsm6dso_t* dev, uint32_t* out_n)
{
  uint8_t         status[2] = {};
  const ra8_err_t rs        = dev->bus.read_regs(dev->bus.ctx,
                                                 (uint8_t)k_lsm6dso_reg_fifo_status1,
                                                 status,
                                                 (uint32_t)k_lsm6dso_fifo_status_bytes);
  if (rs != k_ra8_ok) {
    return rs;
  }
  *out_n = (uint32_t)status[k_lsm6dso_idx_low] |
           ((uint32_t)(status[k_lsm6dso_idx_high] & (uint8_t)k_lsm6dso_mask_nibble) << 8U);
  return k_ra8_ok;
}

/**
 * @brief Burst-read ``n_words`` 7-byte FIFO records from FIFO_DATA_OUT_TAG.
 *
 * @details
 * Per DS12140 sec 9.60 ``FIFO_DATA_OUT_TAG (78h)`` the FIFO data
 * registers auto-increment across the 7-byte word (1 TAG + 6 sample
 * bytes). We issue a single bus burst so the IIC/SPI controller can
 * stream the entire window without per-byte START/STOP overhead.
 *
 * @param[in]  dev     Driver instance (already validated by caller).
 * @param[out] out_buf Destination buffer (must be >= ``n_words * 7``).
 * @param[in]  n_words Number of 7-byte words to fetch (must be > 0).
 *
 * @return Transport error code; ``k_ra8_ok`` on success.
 * @retval k_ra8_ok ``out_buf`` populated with ``n_words * 7`` bytes.
 * @retval other   Forwarded from ``dev->bus.read_regs``.
 *
 * @pre ``dev`` was bound by ``ra8_lsm6dso_init`` and is initialized.
 * @pre ``out_buf`` has capacity for ``n_words * 7`` bytes and ``n_words > 0``.
 * @post On success the buffer holds ``n_words`` consecutive FIFO records.
 * @post On failure the buffer contents are unspecified.
 *
 * @note Not thread-safe per-instance.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_lsm6dso_burst_fifo(ra8_lsm6dso_t* dev, uint8_t* out_buf, uint32_t n_words)
{
  const uint32_t total_bytes = n_words * (uint32_t)k_lsm6dso_fifo_bytes_word;
  return dev->bus.read_regs(dev->bus.ctx,
                            (uint8_t)k_lsm6dso_reg_fifo_data_out,
                            out_buf,
                            total_bytes);
}

/**
 * @brief Validate the inputs to ``ra8_lsm6dso_read_xl_gyro_fifo``.
 *
 * @details
 * Returns the same error codes as the public entry point would on
 * each individual check, but lives in its own function so the public
 * entry stays within the NASA P10 Rule 4 statement budget. Behaviour
 * matches the public API one-to-one: NULL pointer -> ``k_ra8_err_null_ptr``,
 * not initialized -> ``k_ra8_err_not_initialized``, zero ``max_words``
 * -> ``k_ra8_err_invalid_arg``.
 *
 * @param[in] dev       Driver instance.
 * @param[in] out_buf   Caller buffer.
 * @param[in] max_words Caller's word cap.
 * @param[in] out_words Caller's output count pointer.
 *
 * @return ``k_ra8_ok`` if all preconditions hold, otherwise the
 *         matching error code.
 * @retval k_ra8_ok                All preconditions satisfied.
 * @retval k_ra8_err_null_ptr      Any of ``dev`` / ``out_buf`` / ``out_words`` is NULL.
 * @retval k_ra8_err_not_initialized ``dev->initialized`` is false.
 * @retval k_ra8_err_invalid_arg   ``max_words`` is zero.
 *
 * @pre Caller must not have validated ``dev`` / ``out_buf`` / ``out_words``
 *      previously -- this helper is the single validation gate.
 * @pre ``dev`` is either NULL or points at storage owned by the caller.
 * @post No state is mutated by this helper.
 * @post On failure the returned code matches the first failing check.
 *
 * @note Not thread-safe per-instance.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_lsm6dso_fifo_check_args(const ra8_lsm6dso_t* dev,
                                                  const uint8_t*       out_buf,
                                                  uint32_t             max_words,
                                                  const uint32_t*      out_words)
{
  RA8_CHECK_NULL_PTR(dev, s_lsm6dso_tag, "read_fifo: dev");
  RA8_CHECK_NULL_PTR(out_buf, s_lsm6dso_tag, "read_fifo: out_buf");
  RA8_CHECK_NULL_PTR(out_words, s_lsm6dso_tag, "read_fifo: out_words");
  RA8_VALIDATE_INIT(dev->initialized, s_lsm6dso_tag, "read_fifo: not initialized");
  if (max_words == 0U) {
    ra8_log_error(s_lsm6dso_tag, "read_fifo: max_words is zero");
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_lsm6dso_read_xl_gyro_fifo(ra8_lsm6dso_t* dev,
                                        uint8_t*       out_buf,
                                        uint32_t       max_words,
                                        uint32_t*      out_words)
{
  /* Set the output count early so callers can rely on it on failure. */
  if (out_words != nullptr) {
    *out_words = 0U;
  }
  const ra8_err_t chk = internal_lsm6dso_fifo_check_args(dev, out_buf, max_words, out_words);
  if (chk != k_ra8_ok) {
    return chk;
  }

  uint32_t        live = 0U;
  const ra8_err_t rs   = internal_lsm6dso_read_fifo_depth(dev, &live);
  if (rs != k_ra8_ok) {
    return rs;
  }
  const uint32_t to_read = (live > max_words) ? max_words : live;
  if (to_read == 0U) {
    return k_ra8_ok;
  }
  const ra8_err_t rd = internal_lsm6dso_burst_fifo(dev, out_buf, to_read);
  if (rd != k_ra8_ok) {
    return rd;
  }
  *out_words = to_read;
  return k_ra8_ok;
}
