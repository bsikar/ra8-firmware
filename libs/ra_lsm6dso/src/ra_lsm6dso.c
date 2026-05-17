/**
 * @file ra_lsm6dso.c
 * @brief ST LSM6DSO 6-DoF IMU driver -- implementation
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details
 * Polling, transport-agnostic implementation. All register-level
 * citations point at LSM6DSO DS12140 Rev 4 (Sept 2019). The transport
 * is supplied by the caller (Dependency Inversion) so this TU does
 * not link against ``ra_iic_b`` or ``ra_spi``.
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

#include "ra_lsm6dso.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

/* =============================================================================
 * File-local constants and helpers
 * =============================================================================
 */

/** @brief Tag for ra_log_error / ra_log_info lines from this TU. */
static const char* const k_lsm6dso_tag = "lsm6dso";

/** @brief Layout constants used by burst-read helpers. */
typedef enum : uint32_t {
  k_lsm6dso_xyz_burst_bytes  = 6U,  /**< 3 axes * 2 bytes (DS12140 sec 9.29 / 9.35). */
  k_lsm6dso_temp_burst_bytes = 2U,  /**< OUT_TEMP_L + OUT_TEMP_H (DS12140 sec 9.27). */
  k_lsm6dso_fifo_status_bytes = 2U, /**< FIFO_STATUS1 + FIFO_STATUS2 (DS12140 sec 9.44). */
} ra_lsm6dso_burst_bytes_t;

/** @brief Two's-complement byte combination -- 0..1 indices. */
typedef enum : uint8_t {
  k_lsm6dso_idx_low  = 0U, /**< Little-endian low  byte. */
  k_lsm6dso_idx_high = 1U, /**< Little-endian high byte. */
} ra_lsm6dso_endian_idx_t;

/** @brief XYZ sample byte offsets within the 6-byte burst. */
typedef enum : uint8_t {
  k_lsm6dso_xyz_off_x_l = 0U,
  k_lsm6dso_xyz_off_x_h = 1U,
  k_lsm6dso_xyz_off_y_l = 2U,
  k_lsm6dso_xyz_off_y_h = 3U,
  k_lsm6dso_xyz_off_z_l = 4U,
  k_lsm6dso_xyz_off_z_h = 5U,
} ra_lsm6dso_xyz_off_t;

/** @brief CTRL1_XL / CTRL2_G field positions (DS12140 sec 9.12 / 9.13). */
typedef enum : uint8_t {
  k_lsm6dso_shift_odr    = 4U,    /**< ODR_xL[3:0] / ODR_G[3:0] occupy bits [7:4]. */
  k_lsm6dso_shift_fs_xl  = 2U,    /**< FS_XL[1:0]                  occupies bits [3:2]. */
  k_lsm6dso_shift_fs_g   = 2U,    /**< FS_G[1:0]                   occupies bits [3:2]. */
  k_lsm6dso_shift_fs_125 = 1U,    /**< FS_125 select bit           occupies bit  [1].   */
} ra_lsm6dso_shift_t;

typedef enum : uint8_t {
  k_lsm6dso_mask_odr    = 0xF0U, /**< Bits [7:4]: ODR field.        */
  k_lsm6dso_mask_fs_xl  = 0x0CU, /**< Bits [3:2]: FS_XL field.      */
  k_lsm6dso_mask_fs_g   = 0x0CU, /**< Bits [3:2]: FS_G field.       */
  k_lsm6dso_mask_fs_125 = 0x02U, /**< Bit  [1]:   FS_125 select.    */
  k_lsm6dso_mask_fs_g_full = 0x0EU, /**< FS_G + FS_125 combined.    */
  k_lsm6dso_mask_nibble = 0x0FU, /**< Low nibble (used for FIFO len high). */
  k_lsm6dso_mask_byte   = 0xFFU, /**< Full byte mask.               */
} ra_lsm6dso_mask_t;

/** @brief Temperature conversion constants per DS12140 sec 9.27. */
typedef enum : int32_t {
  k_lsm6dso_temp_offset_centi_c = 2500,  /**< +25 C zero offset, scaled x100. */
  k_lsm6dso_temp_scale_num      = 100,   /**< Numerator   for centi-C convert. */
  k_lsm6dso_temp_scale_den      = 256,   /**< Denominator for centi-C convert. */
} ra_lsm6dso_temp_const_t;

/** @brief Enum upper bounds used by RA_CHECK_RANGE_TAG. */
typedef enum : uint8_t {
  k_lsm6dso_xl_fs_max = (uint8_t)k_lsm6dso_g_fs_2000dps, /* shared cap value 0x04 */
  k_lsm6dso_xl_fs_cap = (uint8_t)k_lsm6dso_xl_fs_8g,     /**< Highest XL FS code. */
  k_lsm6dso_g_fs_cap  = (uint8_t)k_lsm6dso_g_fs_2000dps, /**< Highest G  FS code. */
  k_lsm6dso_odr_cap   = (uint8_t)k_lsm6dso_odr_6660hz,   /**< Highest ODR code.   */
} ra_lsm6dso_cap_t;

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
 *     cleared and FS_G[1:0] takes the value from ``ra_lsm6dso_g_fs_t``
 *     minus the +-125 dps slot (i.e. ``k_lsm6dso_g_fs_250dps`` maps to
 *     FS_G = 0, ``..._500dps`` to FS_G = 1, etc).
 *
 * The return value is the 3-bit sub-field [3:1] of CTRL2_G,
 * pre-shifted into place. The caller masks in the ODR nibble.
 *
 * @param[in] fs Gyro full-scale code.
 *
 * @return 3-bit sub-field [3:1] of CTRL2_G, pre-shifted.
 */
static uint8_t internal_lsm6dso_g_fs_bits(ra_lsm6dso_g_fs_t fs)
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
 * @param[in] dev Driver instance (already validated by caller).
 * @param[in] reg Register address.
 * @param[in] val Byte to write.
 *
 * @return Transport error code.
 */
static ra_err_t internal_lsm6dso_write_byte(ra_lsm6dso_t* dev, uint8_t reg, uint8_t val)
{
  return dev->bus.write_regs(dev->bus.ctx, reg, &val, 1U);
}

/**
 * @brief Read a single byte from ``reg`` via the bound transport.
 *
 * @param[in]  dev Driver instance (already validated).
 * @param[in]  reg Register address.
 * @param[out] out Receives the byte on success.
 *
 * @return Transport error code.
 */
static ra_err_t internal_lsm6dso_read_byte(ra_lsm6dso_t* dev, uint8_t reg, uint8_t* out)
{
  return dev->bus.read_regs(dev->bus.ctx, reg, out, 1U);
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

ra_err_t ra_lsm6dso_init(ra_lsm6dso_t* out_dev, const ra_lsm6dso_bus_t* bus)
{
  RA_CHECK_NULL_PTR(out_dev, k_lsm6dso_tag, "init: out_dev");
  RA_CHECK_NULL_PTR(bus, k_lsm6dso_tag, "init: bus");
  RA_CHECK_NULL_PTR(bus->read_regs, k_lsm6dso_tag, "init: bus.read_regs");
  RA_CHECK_NULL_PTR(bus->write_regs, k_lsm6dso_tag, "init: bus.write_regs");

  out_dev->bus           = *bus;
  out_dev->accel_fs_code = k_lsm6dso_xl_fs_2g;     /* DS12140 sec 9.12 reset default. */
  out_dev->gyro_fs_code  = k_lsm6dso_g_fs_250dps;  /* DS12140 sec 9.13 reset default. */
  out_dev->odr_code      = k_lsm6dso_odr_off;      /* DS12140 sec 9.12 reset default. */
  out_dev->initialized   = true;
  return k_ra_ok;
}

/* =============================================================================
 * Identification
 * =============================================================================
 */

ra_err_t ra_lsm6dso_who_am_i(ra_lsm6dso_t* dev, uint8_t* out_id)
{
  RA_CHECK_NULL_PTR(dev, k_lsm6dso_tag, "who_am_i: dev");
  RA_CHECK_NULL_PTR(out_id, k_lsm6dso_tag, "who_am_i: out_id");
  RA_VALIDATE_INIT(dev->initialized, k_lsm6dso_tag, "who_am_i: not initialized");

  /* DS12140 sec 9.11 WHO_AM_I (0Fh). */
  return internal_lsm6dso_read_byte(dev, (uint8_t)k_lsm6dso_reg_who_am_i, out_id);
}

/* =============================================================================
 * Configuration
 * =============================================================================
 */

ra_err_t ra_lsm6dso_set_accel_range(ra_lsm6dso_t* dev, ra_lsm6dso_xl_fs_t fs)
{
  RA_CHECK_NULL_PTR(dev, k_lsm6dso_tag, "set_accel_range: dev");
  RA_VALIDATE_INIT(dev->initialized, k_lsm6dso_tag, "set_accel_range: not initialized");
  RA_CHECK_RANGE_TAG((uint8_t)fs,
                     0U,
                     (uint8_t)k_lsm6dso_xl_fs_cap,
                     k_ra_err_invalid_arg,
                     k_lsm6dso_tag);

  /* DS12140 sec 9.12 CTRL1_XL: read-modify-write FS_XL[3:2]. */
  uint8_t        current = 0U;
  const ra_err_t r       = internal_lsm6dso_read_byte(dev,
                                                (uint8_t)k_lsm6dso_reg_ctrl1_xl,
                                                &current);
  if (r != k_ra_ok) {
    return r;
  }
  const uint8_t merged =
    (uint8_t)((current & (uint8_t)~k_lsm6dso_mask_fs_xl) |
              (uint8_t)(((uint8_t)fs & 0x03U) << (uint8_t)k_lsm6dso_shift_fs_xl));
  const ra_err_t w =
    internal_lsm6dso_write_byte(dev, (uint8_t)k_lsm6dso_reg_ctrl1_xl, merged);
  if (w != k_ra_ok) {
    return w;
  }
  dev->accel_fs_code = fs;
  return k_ra_ok;
}

ra_err_t ra_lsm6dso_set_gyro_range(ra_lsm6dso_t* dev, ra_lsm6dso_g_fs_t fs)
{
  RA_CHECK_NULL_PTR(dev, k_lsm6dso_tag, "set_gyro_range: dev");
  RA_VALIDATE_INIT(dev->initialized, k_lsm6dso_tag, "set_gyro_range: not initialized");
  RA_CHECK_RANGE_TAG((uint8_t)fs,
                     0U,
                     (uint8_t)k_lsm6dso_g_fs_cap,
                     k_ra_err_invalid_arg,
                     k_lsm6dso_tag);

  /* DS12140 sec 9.13 CTRL2_G: read-modify-write FS_G[3:2] + FS_125[1]. */
  uint8_t        current = 0U;
  const ra_err_t r       = internal_lsm6dso_read_byte(dev,
                                                (uint8_t)k_lsm6dso_reg_ctrl2_g,
                                                &current);
  if (r != k_ra_ok) {
    return r;
  }
  const uint8_t fs_bits = internal_lsm6dso_g_fs_bits(fs);
  const uint8_t merged =
    (uint8_t)((current & (uint8_t)~k_lsm6dso_mask_fs_g_full) | fs_bits);
  const ra_err_t w =
    internal_lsm6dso_write_byte(dev, (uint8_t)k_lsm6dso_reg_ctrl2_g, merged);
  if (w != k_ra_ok) {
    return w;
  }
  dev->gyro_fs_code = fs;
  return k_ra_ok;
}

ra_err_t ra_lsm6dso_set_odr(ra_lsm6dso_t* dev, ra_lsm6dso_odr_t odr)
{
  RA_CHECK_NULL_PTR(dev, k_lsm6dso_tag, "set_odr: dev");
  RA_VALIDATE_INIT(dev->initialized, k_lsm6dso_tag, "set_odr: not initialized");
  RA_CHECK_RANGE_TAG((uint8_t)odr,
                     0U,
                     (uint8_t)k_lsm6dso_odr_cap,
                     k_ra_err_invalid_arg,
                     k_lsm6dso_tag);

  /* DS12140 sec 9.12 / 9.13: ODR field is bits [7:4] of CTRL1_XL and CTRL2_G. */
  const uint8_t odr_bits =
    (uint8_t)(((uint8_t)odr & (uint8_t)k_lsm6dso_mask_nibble) <<
              (uint8_t)k_lsm6dso_shift_odr);

  /* --- CTRL1_XL --- */
  uint8_t        xl_now = 0U;
  const ra_err_t rxl    = internal_lsm6dso_read_byte(dev,
                                                  (uint8_t)k_lsm6dso_reg_ctrl1_xl,
                                                  &xl_now);
  if (rxl != k_ra_ok) {
    return rxl;
  }
  const uint8_t xl_new =
    (uint8_t)((xl_now & (uint8_t)~k_lsm6dso_mask_odr) | odr_bits);
  const ra_err_t wxl =
    internal_lsm6dso_write_byte(dev, (uint8_t)k_lsm6dso_reg_ctrl1_xl, xl_new);
  if (wxl != k_ra_ok) {
    return wxl;
  }

  /* --- CTRL2_G --- */
  uint8_t        g_now = 0U;
  const ra_err_t rg    = internal_lsm6dso_read_byte(dev,
                                                 (uint8_t)k_lsm6dso_reg_ctrl2_g,
                                                 &g_now);
  if (rg != k_ra_ok) {
    return rg;
  }
  const uint8_t g_new =
    (uint8_t)((g_now & (uint8_t)~k_lsm6dso_mask_odr) | odr_bits);
  const ra_err_t wg =
    internal_lsm6dso_write_byte(dev, (uint8_t)k_lsm6dso_reg_ctrl2_g, g_new);
  if (wg != k_ra_ok) {
    return wg;
  }

  dev->odr_code = odr;
  return k_ra_ok;
}

/* =============================================================================
 * Sample-read helpers
 * =============================================================================
 */

/**
 * @brief Common XYZ burst-read used by both ``read_accel`` and
 *        ``read_gyro``.
 *
 * @param[in]  dev Driver instance (validated).
 * @param[in]  reg First register address (``OUTX_L_A`` / ``OUTX_L_G``).
 * @param[out] out 3-axis raw sample.
 *
 * @return Transport error code; ``out`` is left untouched on failure.
 */
static ra_err_t internal_lsm6dso_read_xyz(ra_lsm6dso_t*    dev,
                                          uint8_t          reg,
                                          ra_lsm6dso_xyz_t* out)
{
  uint8_t        bytes[6] = {};
  const ra_err_t r        = dev->bus.read_regs(dev->bus.ctx,
                                        reg,
                                        bytes,
                                        (uint32_t)k_lsm6dso_xyz_burst_bytes);
  if (r != k_ra_ok) {
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
  return k_ra_ok;
}

ra_err_t ra_lsm6dso_read_accel(ra_lsm6dso_t* dev, ra_lsm6dso_xyz_t* out)
{
  RA_CHECK_NULL_PTR(dev, k_lsm6dso_tag, "read_accel: dev");
  RA_CHECK_NULL_PTR(out, k_lsm6dso_tag, "read_accel: out");
  RA_VALIDATE_INIT(dev->initialized, k_lsm6dso_tag, "read_accel: not initialized");
  /* DS12140 sec 9.35 OUTX_L_A (28h) -- auto-increment burst over 6 bytes. */
  return internal_lsm6dso_read_xyz(dev, (uint8_t)k_lsm6dso_reg_outx_l_a, out);
}

ra_err_t ra_lsm6dso_read_gyro(ra_lsm6dso_t* dev, ra_lsm6dso_xyz_t* out)
{
  RA_CHECK_NULL_PTR(dev, k_lsm6dso_tag, "read_gyro: dev");
  RA_CHECK_NULL_PTR(out, k_lsm6dso_tag, "read_gyro: out");
  RA_VALIDATE_INIT(dev->initialized, k_lsm6dso_tag, "read_gyro: not initialized");
  /* DS12140 sec 9.29 OUTX_L_G (22h) -- auto-increment burst over 6 bytes. */
  return internal_lsm6dso_read_xyz(dev, (uint8_t)k_lsm6dso_reg_outx_l_g, out);
}

ra_err_t ra_lsm6dso_read_temp(ra_lsm6dso_t* dev, int32_t* out_centi_c)
{
  RA_CHECK_NULL_PTR(dev, k_lsm6dso_tag, "read_temp: dev");
  RA_CHECK_NULL_PTR(out_centi_c, k_lsm6dso_tag, "read_temp: out_centi_c");
  RA_VALIDATE_INIT(dev->initialized, k_lsm6dso_tag, "read_temp: not initialized");

  /* DS12140 sec 9.27 OUT_TEMP_L (20h) + sec 9.28 OUT_TEMP_H (21h). */
  uint8_t        bytes[2] = {};
  const ra_err_t r        = dev->bus.read_regs(dev->bus.ctx,
                                        (uint8_t)k_lsm6dso_reg_out_temp_l,
                                        bytes,
                                        (uint32_t)k_lsm6dso_temp_burst_bytes);
  if (r != k_ra_ok) {
    return r;
  }
  const int16_t raw =
    (int16_t)(((uint16_t)bytes[k_lsm6dso_idx_high] << 8) |
              (uint16_t)bytes[k_lsm6dso_idx_low]);
  /* DS12140 sec 4.3 "Temperature sensor characteristics":
   *   T[degC] = raw / 256 + 25
   * Scale into centi-deg C to keep integer arithmetic: */
  *out_centi_c = (int32_t)((int32_t)raw * (int32_t)k_lsm6dso_temp_scale_num /
                           (int32_t)k_lsm6dso_temp_scale_den) +
                 (int32_t)k_lsm6dso_temp_offset_centi_c;
  return k_ra_ok;
}

ra_err_t ra_lsm6dso_read_xl_gyro_fifo(ra_lsm6dso_t* dev,
                                      uint8_t*      out_buf,
                                      uint32_t      max_words,
                                      uint32_t*     out_words)
{
  /* Set the output count early so callers can rely on it on failure. */
  if (out_words != nullptr) {
    *out_words = 0U;
  }
  RA_CHECK_NULL_PTR(dev, k_lsm6dso_tag, "read_fifo: dev");
  RA_CHECK_NULL_PTR(out_buf, k_lsm6dso_tag, "read_fifo: out_buf");
  RA_CHECK_NULL_PTR(out_words, k_lsm6dso_tag, "read_fifo: out_words");
  RA_VALIDATE_INIT(dev->initialized, k_lsm6dso_tag, "read_fifo: not initialized");
  if (max_words == 0U) {
    ra_log_error(k_lsm6dso_tag, "read_fifo: max_words is zero");
    return k_ra_err_invalid_arg;
  }

  /* DS12140 sec 9.44 FIFO_STATUS1 + sec 9.45 FIFO_STATUS2:
   * DIFF_FIFO[9:0] is the live FIFO word count (10-bit). */
  uint8_t        status[2] = {};
  const ra_err_t rs        = dev->bus.read_regs(dev->bus.ctx,
                                         (uint8_t)k_lsm6dso_reg_fifo_status1,
                                         status,
                                         (uint32_t)k_lsm6dso_fifo_status_bytes);
  if (rs != k_ra_ok) {
    return rs;
  }
  const uint32_t live =
    (uint32_t)status[k_lsm6dso_idx_low] |
    ((uint32_t)(status[k_lsm6dso_idx_high] & (uint8_t)k_lsm6dso_mask_nibble) << 8U);
  uint32_t to_read = live;
  if (to_read > max_words) {
    to_read = max_words;
  }
  if (to_read == 0U) {
    return k_ra_ok;
  }

  /* DS12140 sec 9.60 FIFO_DATA_OUT_TAG (78h): 7-byte words come out
   * with auto-increment. We burst-read the lot in one call. */
  const uint32_t total_bytes = to_read * (uint32_t)k_lsm6dso_fifo_bytes_word;
  const ra_err_t rd          = dev->bus.read_regs(dev->bus.ctx,
                                         (uint8_t)k_lsm6dso_reg_fifo_data_out,
                                         out_buf,
                                         total_bytes);
  if (rd != k_ra_ok) {
    return rd;
  }
  *out_words = to_read;
  return k_ra_ok;
}
