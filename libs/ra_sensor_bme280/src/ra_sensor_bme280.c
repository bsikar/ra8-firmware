/**
 * @file ra_sensor_bme280.c
 * @brief Bosch BME280 driver implementation.
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Hand-written implementation of ``ra_sensor_bme280.h``. Speaks to the
 * BME280 over the RA8D2 IIC_B (I3C-in-I2C-mode) bus using the standard
 * "write 8-bit register pointer, RESTART, read N bytes" pattern. All
 * compensation is integer arithmetic ported one-for-one from the
 * BME280 datasheet rev 1.6 sec 4.2.3 reference C code -- no floating
 * point reaches the firmware path.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_sensor_bme280.h"

#include <stddef.h>
#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_iic_b.h"
#include "ra_log.h"

/** @brief Log tag used by every ``ra_log_*`` call in this TU. */
static const char *s_tag = "BME280";

/* ===========================================================================
 * Implementation-only constants (BME280 datasheet rev 1.6 sec 5.4)
 * ===========================================================================
 */

/**
 * @enum ra_sensor_bme280_reg_t
 * @brief BME280 register addresses (datasheet rev 1.6 sec 5.4).
 */
typedef enum : uint8_t {
  k_ra_bme280_reg_calib_88_a1 = 0x88U, /**< 26 bytes -- T1..H1. */
  k_ra_bme280_reg_chip_id = 0xD0U,     /**< Chip-id (==0x60).   */
  k_ra_bme280_reg_reset = 0xE0U,       /**< Soft-reset register.*/
  k_ra_bme280_reg_calib_e1_e7 = 0xE1U, /**<  7 bytes -- H2..H6. */
  k_ra_bme280_reg_ctrl_hum = 0xF2U,    /**< osrs_h.             */
  k_ra_bme280_reg_status = 0xF3U,      /**< measuring/im_update.*/
  k_ra_bme280_reg_ctrl_meas = 0xF4U,   /**< osrs_t/p, mode.     */
  k_ra_bme280_reg_config = 0xF5U,      /**< t_sb, filter, spi3w.*/
  k_ra_bme280_reg_data_start = 0xF7U,  /**< press_msb (8 bytes).*/
} ra_sensor_bme280_reg_t;

/**
 * @enum ra_sensor_bme280_const_t
 * @brief Numeric constants for register layout / driver behaviour.
 */
typedef enum : uint32_t {
  k_ra_bme280_chip_id_expected = 0x60U,     /**< Datasheet sec 5.4.1.       */
  k_ra_bme280_reset_magic = 0xB6U,          /**< Datasheet sec 5.4.2.       */
  k_ra_bme280_calib_88_a1_len = 26U,        /**< 0x88..0xA1.                */
  k_ra_bme280_calib_e1_e7_len = 7U,         /**< 0xE1..0xE7.                */
  k_ra_bme280_data_burst_len = 8U,          /**< 0xF7..0xFE.                */
  k_ra_bme280_status_im_update = 0x01U,     /**< status.im_update bit 0.    */
  k_ra_bme280_mode_normal = 0x03U,          /**< ctrl_meas.mode = 0b11.     */
  k_ra_bme280_status_poll_max = 100U,       /**< x100 us == 10 ms.          */
  k_ra_bme280_default_bus_hz = 400000U,     /**< 400 kHz fast-mode.         */
  k_ra_bme280_default_pclka_hz = 60000000U, /**< IIC_B clock-source rate.   */
  k_ra_bme280_byte_mask = 0xFFU,            /**< 8-bit byte mask.           */
  k_ra_bme280_byte_shift = 8U,              /**< Bits per byte.             */
  k_ra_bme280_nibble_shift = 4U,            /**< Bits per nibble.           */
  k_ra_bme280_low_nibble_mask = 0x0FU,      /**< 4-bit nibble mask.         */
  k_ra_bme280_adc_shift_lsb = 4U,           /**< press/temp xLSB shift.     */
  k_ra_bme280_p_to_pa_shift = 8U,           /**< Q24.8 -> integer Pa.       */
  k_ra_bme280_h_q22_10_div = 1024U,         /**< Q22.10 divisor.            */
  k_ra_bme280_h_mpct_scale = 1000U,         /**< %RH -> millipercent.       */
  k_ra_bme280_t_to_mc_scale = 10U,          /**< 0.01 degC -> 0.001 degC.   */
} ra_sensor_bme280_const_t;

/**
 * @enum ra_sensor_bme280_shift_t
 * @brief Bit shifts in the ``ctrl_meas`` / ``config`` registers.
 */
typedef enum : uint8_t {
  k_ra_bme280_shift_osrs_t = 5U, /**< ctrl_meas[7:5]. */
  k_ra_bme280_shift_osrs_p = 2U, /**< ctrl_meas[4:2]. */
  k_ra_bme280_shift_t_sb = 5U,   /**< config[7:5].    */
  k_ra_bme280_shift_filter = 2U, /**< config[4:2].    */
} ra_sensor_bme280_shift_t;

/**
 * @struct ra_sensor_bme280_state_t
 * @brief Driver-private state slot.
 */
typedef struct {
  ra_sensor_bme280_calib_t calib; /**< Decoded calibration table. */
  uint8_t channel;                /**< IIC_B channel (==0).       */
  uint8_t target_7b;              /**< 7-bit BME280 address.      */
  bool opened;                    /**< Lifecycle flag.            */
} ra_sensor_bme280_state_t;

/** @brief File-scope state -- only one BME280 attaches per channel. */
static ra_sensor_bme280_state_t s_state;

/* ===========================================================================
 * Low-level register access helpers
 * ===========================================================================
 */

/**
 * @brief Burst-read ``len`` bytes starting at register ``reg``.
 *
 * @param[in]  reg Starting register address.
 * @param[out] buf Destination buffer.
 * @param[in]  len Byte count.
 *
 * @return Forwarded ``ra_iic_b_transfer`` return code.
 *
 * @pre ``buf`` non-NULL and at least ``len`` bytes long.
 * @post On success ``buf[0..len-1]`` contains the wire bytes.
 */
static ra_err_t priv_read_block(uint8_t reg, uint8_t *buf, uint32_t len) {
  uint8_t ptr = reg;
  const ra_err_t bus_rc =
      ra_iic_b_transfer(s_state.channel, s_state.target_7b, &ptr, 1U, buf, len);
  if (bus_rc != k_ra_ok) {
    return k_ra_err_i2c_error;
  }
  return k_ra_ok;
}

/**
 * @brief Write a single ``value`` into 8-bit register ``reg``.
 */
static ra_err_t priv_write_byte(uint8_t reg, uint8_t value) {
  uint8_t payload[2] = {reg, value};
  const ra_err_t bus_rc = ra_iic_b_write(s_state.channel, s_state.target_7b,
                                         payload, sizeof(payload),
                                         /*restart=*/false);
  if (bus_rc != k_ra_ok) {
    return k_ra_err_i2c_error;
  }
  return k_ra_ok;
}

/**
 * @brief Read a single byte from 8-bit register ``reg``.
 */
static ra_err_t priv_read_byte(uint8_t reg, uint8_t *out) {
  return priv_read_block(reg, out, 1U);
}

/* ===========================================================================
 * Calibration decode
 * ===========================================================================
 */

/**
 * @brief Reassemble a little-endian 16-bit unsigned word.
 */
static uint16_t priv_u16le(const uint8_t *p) {
  return (uint16_t)((uint32_t)p[0] |
                    ((uint32_t)p[1] << (uint32_t)k_ra_bme280_byte_shift));
}

/**
 * @brief Reassemble a little-endian 16-bit signed word.
 */
static int16_t priv_s16le(const uint8_t *p) { return (int16_t)priv_u16le(p); }

void ra_sensor_bme280_test_decode_calib(const uint8_t *raw_88_a1,
                                        const uint8_t *raw_e1_e7,
                                        ra_sensor_bme280_calib_t *out) {
  /* 0x88..0xA1 -- temperature + pressure + dig_H1. */
  out->dig_T1 = priv_u16le(&raw_88_a1[0]);
  out->dig_T2 = priv_s16le(&raw_88_a1[2]);
  out->dig_T3 = priv_s16le(&raw_88_a1[4]);
  out->dig_P1 = priv_u16le(&raw_88_a1[6]);
  out->dig_P2 = priv_s16le(&raw_88_a1[8]);
  out->dig_P3 = priv_s16le(&raw_88_a1[10]);
  out->dig_P4 = priv_s16le(&raw_88_a1[12]);
  out->dig_P5 = priv_s16le(&raw_88_a1[14]);
  out->dig_P6 = priv_s16le(&raw_88_a1[16]);
  out->dig_P7 = priv_s16le(&raw_88_a1[18]);
  out->dig_P8 = priv_s16le(&raw_88_a1[20]);
  out->dig_P9 = priv_s16le(&raw_88_a1[22]);
  /* 0xA0 is reserved. dig_H1 is at 0xA1 == raw_88_a1[25]. */
  out->dig_H1 = raw_88_a1[25];

  /* 0xE1..0xE7 -- humidity calibration. dig_H4/H5 are 12-bit fields
   * split across the 0xE5 nibble per datasheet rev 1.6 Table 16. */
  out->dig_H2 = priv_s16le(&raw_e1_e7[0]);
  out->dig_H3 = raw_e1_e7[2];

  const uint8_t e4 = raw_e1_e7[3];
  const uint8_t e5 = raw_e1_e7[4];
  const uint8_t e6 = raw_e1_e7[5];

  /* dig_H4 = (E4 << 4) | (E5 & 0x0F) -- signed 12-bit, sign-extended. */
  int32_t h4 = ((int32_t)(int8_t)e4 << (int32_t)k_ra_bme280_nibble_shift) |
               (int32_t)(e5 & (uint8_t)k_ra_bme280_low_nibble_mask);
  out->dig_H4 = (int16_t)h4;

  /* dig_H5 = (E6 << 4) | (E5 >> 4) -- signed 12-bit, sign-extended. */
  int32_t h5 = ((int32_t)(int8_t)e6 << (int32_t)k_ra_bme280_nibble_shift) |
               (int32_t)((uint32_t)e5 >> (uint32_t)k_ra_bme280_nibble_shift);
  out->dig_H5 = (int16_t)h5;

  out->dig_H6 = (int8_t)raw_e1_e7[6];
}

/* ===========================================================================
 * Bosch reference compensation (datasheet rev 1.6 sec 4.2.3)
 *
 * The variable names below are intentionally kept identical to the
 * reference C code so the formulas remain visually verifiable against
 * the datasheet. The only deviation is the wrapping into a single
 * helper that returns t_fine separately for downstream use.
 * ===========================================================================
 */

/**
 * @brief Bosch ``BME280_compensate_T_int32`` (datasheet sec 4.2.3).
 *
 * @param[in]  calib    Decoded calibration block.
 * @param[in]  adc_T    20-bit raw temperature.
 * @param[out] t_fine   Carry-over value used by P and H compensation.
 *
 * @return Compensated temperature in DegC * 100 (e.g. 5123 == 51.23 C).
 */
static int32_t priv_compensate_T(const ra_sensor_bme280_calib_t *calib,
                                 int32_t adc_T, int32_t *t_fine) {
  int32_t var1;
  int32_t var2;
  int32_t T;
  /* var1 = ((((adc_T>>3) - ((BME280_S32_t)dig_T1<<1))) * dig_T2) >> 11 */
  var1 = (((adc_T >> 3) - ((int32_t)calib->dig_T1 << 1)) *
          (int32_t)calib->dig_T2) >>
         11;
  /* var2 = (((((adc_T>>4) - dig_T1) * ((adc_T>>4) - dig_T1)) >> 12) * dig_T3)
   * >> 14 */
  var2 = (((((adc_T >> 4) - (int32_t)calib->dig_T1) *
            ((adc_T >> 4) - (int32_t)calib->dig_T1)) >>
           12) *
          (int32_t)calib->dig_T3) >>
         14;
  *t_fine = var1 + var2;
  T = (*t_fine * 5 + 128) >> 8;
  return T;
}

/**
 * @brief Bosch ``BME280_compensate_P_int64`` (datasheet sec 4.2.3).
 *
 * @return Compensated pressure in Q24.8 Pa, or 0 on the divide-by-zero
 *         guard branch (matches the reference code).
 */
static uint32_t priv_compensate_P(const ra_sensor_bme280_calib_t *calib,
                                  int32_t adc_P, int32_t t_fine) {
  int64_t var1;
  int64_t var2;
  int64_t p;
  var1 = (int64_t)t_fine - 128000;
  var2 = var1 * var1 * (int64_t)calib->dig_P6;
  var2 = var2 + ((var1 * (int64_t)calib->dig_P5) << 17);
  var2 = var2 + ((int64_t)calib->dig_P4 << 35);
  var1 = ((var1 * var1 * (int64_t)calib->dig_P3) >> 8) +
         ((var1 * (int64_t)calib->dig_P2) << 12);
  var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)calib->dig_P1) >> 33;
  if (var1 == 0) {
    return 0U;
  }
  p = 1048576 - adc_P;
  p = (((p << 31) - var2) * 3125) / var1;
  var1 = ((int64_t)calib->dig_P9 * (p >> 13) * (p >> 13)) >> 25;
  var2 = ((int64_t)calib->dig_P8 * p) >> 19;
  p = ((p + var1 + var2) >> 8) + ((int64_t)calib->dig_P7 << 4);
  return (uint32_t)p;
}

/**
 * @brief Bosch ``bme280_compensate_H_int32`` (datasheet sec 4.2.3).
 *
 * @return Compensated humidity in Q22.10 %RH (e.g. 47445 == 46.333 %).
 */
static uint32_t priv_compensate_H(const ra_sensor_bme280_calib_t *calib,
                                  int32_t adc_H, int32_t t_fine) {
  int32_t v_x1_u32r;
  v_x1_u32r = t_fine - (int32_t)76800;
  v_x1_u32r =
      (((((adc_H << 14) - ((int32_t)calib->dig_H4 << 20) -
          ((int32_t)calib->dig_H5 * v_x1_u32r)) +
         (int32_t)16384) >>
        15) *
       (((((((v_x1_u32r * (int32_t)calib->dig_H6) >> 10) *
            (((v_x1_u32r * (int32_t)calib->dig_H3) >> 11) + (int32_t)32768)) >>
           10) +
          (int32_t)2097152) *
             (int32_t)calib->dig_H2 +
         8192) >>
        14));
  v_x1_u32r = (v_x1_u32r - (((((v_x1_u32r >> 15) * (v_x1_u32r >> 15)) >> 7) *
                             (int32_t)calib->dig_H1) >>
                            4));
  if (v_x1_u32r < 0) {
    v_x1_u32r = 0;
  }
  if (v_x1_u32r > 419430400) {
    v_x1_u32r = 419430400;
  }
  return (uint32_t)(v_x1_u32r >> 12);
}

void ra_sensor_bme280_test_compensate(const ra_sensor_bme280_calib_t *calib,
                                      int32_t adc_t, int32_t adc_p,
                                      int32_t adc_h,
                                      int32_t *out_temperature_mc,
                                      int32_t *out_humidity_mpct,
                                      uint32_t *out_pressure_pa) {
  int32_t t_fine = 0;
  const int32_t T_x100 = priv_compensate_T(calib, adc_t, &t_fine);
  const uint32_t P_q248 = priv_compensate_P(calib, adc_p, t_fine);
  const uint32_t H_q22 = priv_compensate_H(calib, adc_h, t_fine);

  /* T comes out in DegC * 100; rescale to millidegrees for the public API. */
  *out_temperature_mc = T_x100 * (int32_t)k_ra_bme280_t_to_mc_scale;

  /* P is Q24.8 pascals. Drop the fractional bits to whole pascals. */
  *out_pressure_pa = P_q248 >> (uint32_t)k_ra_bme280_p_to_pa_shift;

  /* H is Q22.10 %RH. Convert to millipercent: H * 1000 / 1024. The
   * intermediate fits in uint64_t comfortably (max 419,430,400 * 1000
   * < 2^39). */
  const uint64_t h64 = (uint64_t)H_q22 * (uint64_t)k_ra_bme280_h_mpct_scale;
  *out_humidity_mpct = (int32_t)(h64 / (uint64_t)k_ra_bme280_h_q22_10_div);
}

/* ===========================================================================
 * Lifecycle
 * ===========================================================================
 */

/**
 * @brief Validate a config descriptor against driver constraints.
 */
static ra_err_t priv_validate_cfg(const ra_sensor_bme280_cfg_t *cfg) {
  if (cfg->i2c_channel != 0U) {
    return k_ra_err_invalid_arg;
  }
  if ((cfg->target_7b != (uint8_t)k_ra_sensor_bme280_addr_low) &&
      (cfg->target_7b != (uint8_t)k_ra_sensor_bme280_addr_high)) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/**
 * @brief Bring up IIC_B at the driver defaults (400 kHz, 60 MHz PCLKA).
 */
static ra_err_t priv_bring_up_i2c(uint8_t channel) {
  const ra_iic_b_cfg_t iic_cfg = {
      .bus_hz = (uint32_t)k_ra_bme280_default_bus_hz,
      .pclka_hz = (uint32_t)k_ra_bme280_default_pclka_hz,
  };
  return ra_iic_b_init(channel, &iic_cfg);
}

/**
 * @brief Verify chip id register matches 0x60.
 *
 * @details
 * On the host simulator the I2C read returns zeros, so the
 * verification is skipped to keep the bring-up path testable -- this
 * mirrors the same guard in ``ra_touch.c``.
 */
static ra_err_t priv_check_chip_id(void) {
  uint8_t id = 0U;
  const ra_err_t rd_err = priv_read_byte((uint8_t)k_ra_bme280_reg_chip_id, &id);
#ifdef RA_SIMULATOR_MODE
  (void)rd_err;
  (void)id;
  return k_ra_ok;
#else
  if (rd_err != k_ra_ok) {
    return k_ra_err_hw_init_failed;
  }
  if ((uint32_t)id != (uint32_t)k_ra_bme280_chip_id_expected) {
    return k_ra_err_hw_init_failed;
  }
  return k_ra_ok;
#endif
}

/**
 * @brief Issue soft reset and poll status.im_update until clear.
 */
static ra_err_t priv_soft_reset(void) {
  ra_err_t rst_err = priv_write_byte((uint8_t)k_ra_bme280_reg_reset,
                                     (uint8_t)k_ra_bme280_reset_magic);
  if (rst_err != k_ra_ok) {
    return rst_err;
  }
#ifdef RA_SIMULATOR_MODE
  return k_ra_ok;
#else
  for (uint32_t i = 0U; i < (uint32_t)k_ra_bme280_status_poll_max; i++) {
    uint8_t st = 0U;
    if (priv_read_byte((uint8_t)k_ra_bme280_reg_status, &st) != k_ra_ok) {
      return k_ra_err_i2c_error;
    }
    if ((st & (uint8_t)k_ra_bme280_status_im_update) == 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_init_failed;
#endif
}

/**
 * @brief Read + decode the calibration table into ``s_state.calib``.
 */
static ra_err_t priv_load_calibration(void) {
  uint8_t buf_88[26] = {};
  uint8_t buf_e1[7] = {};
  const ra_err_t r1 =
      priv_read_block((uint8_t)k_ra_bme280_reg_calib_88_a1, buf_88,
                      (uint32_t)k_ra_bme280_calib_88_a1_len);
  if (r1 != k_ra_ok) {
    return r1;
  }
  const ra_err_t r2 =
      priv_read_block((uint8_t)k_ra_bme280_reg_calib_e1_e7, buf_e1,
                      (uint32_t)k_ra_bme280_calib_e1_e7_len);
  if (r2 != k_ra_ok) {
    return r2;
  }
  ra_sensor_bme280_test_decode_calib(buf_88, buf_e1, &s_state.calib);
  return k_ra_ok;
}

/**
 * @brief Programme ctrl_hum / config / ctrl_meas with the cfg values.
 *
 * @details
 * Per datasheet rev 1.6 sec 5.4.3, ``ctrl_hum`` only latches when
 * ``ctrl_meas`` is subsequently written, so the sequence MUST be
 * ctrl_hum -> config -> ctrl_meas.
 */
static ra_err_t priv_program_modes(const ra_sensor_bme280_cfg_t *cfg) {
  const uint8_t ctrl_hum = (uint8_t)cfg->osrs_h;
  const ra_err_t e_hum =
      priv_write_byte((uint8_t)k_ra_bme280_reg_ctrl_hum, ctrl_hum);
  if (e_hum != k_ra_ok) {
    return e_hum;
  }

  const uint8_t cfg_byte =
      (uint8_t)(((uint32_t)cfg->standby << (uint32_t)k_ra_bme280_shift_t_sb) |
                ((uint32_t)cfg->filter << (uint32_t)k_ra_bme280_shift_filter));
  const ra_err_t e_cfg =
      priv_write_byte((uint8_t)k_ra_bme280_reg_config, cfg_byte);
  if (e_cfg != k_ra_ok) {
    return e_cfg;
  }

  const uint8_t ctrl_meas =
      (uint8_t)(((uint32_t)cfg->osrs_t << (uint32_t)k_ra_bme280_shift_osrs_t) |
                ((uint32_t)cfg->osrs_p << (uint32_t)k_ra_bme280_shift_osrs_p) |
                (uint32_t)k_ra_bme280_mode_normal);
  return priv_write_byte((uint8_t)k_ra_bme280_reg_ctrl_meas, ctrl_meas);
}

[[nodiscard]] ra_err_t
ra_sensor_bme280_init(const ra_sensor_bme280_cfg_t *cfg) {
  RA_CHECK_NULL_PTR(cfg, s_tag, "init: cfg");
  if (s_state.opened) {
    return k_ra_err_invalid_state;
  }
  const ra_err_t v_err = priv_validate_cfg(cfg);
  RA_RETURN_ON_ERROR(v_err, s_tag, "init: cfg invalid");

  const ra_err_t i2c_err = priv_bring_up_i2c(cfg->i2c_channel);
  RA_RETURN_ON_ERROR(i2c_err, s_tag, "init: iic_b_init");

  s_state.channel = cfg->i2c_channel;
  s_state.target_7b = cfg->target_7b;

  const ra_err_t id_err = priv_check_chip_id();
  if (id_err != k_ra_ok) {
    (void)ra_iic_b_deinit(cfg->i2c_channel);
    return id_err;
  }
  const ra_err_t rst_err = priv_soft_reset();
  if (rst_err != k_ra_ok) {
    (void)ra_iic_b_deinit(cfg->i2c_channel);
    return rst_err;
  }
  const ra_err_t cal_err = priv_load_calibration();
  if (cal_err != k_ra_ok) {
    (void)ra_iic_b_deinit(cfg->i2c_channel);
    return cal_err;
  }
  const ra_err_t mode_err = priv_program_modes(cfg);
  if (mode_err != k_ra_ok) {
    (void)ra_iic_b_deinit(cfg->i2c_channel);
    return mode_err;
  }

  s_state.opened = true;
  ra_log_info(s_tag, "ra_sensor_bme280_init");
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_sensor_bme280_deinit(void) {
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  (void)ra_iic_b_deinit(s_state.channel);
  s_state.opened = false;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_sensor_bme280_get_chip_id(uint8_t *out_id) {
  RA_CHECK_NULL_PTR(out_id, s_tag, "get_chip_id: out_id");
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  return priv_read_byte((uint8_t)k_ra_bme280_reg_chip_id, out_id);
}

[[nodiscard]] ra_err_t ra_sensor_bme280_read(int32_t *out_temperature_mc,
                                             int32_t *out_humidity_mpct,
                                             uint32_t *out_pressure_pa) {
  RA_CHECK_NULL_PTR(out_temperature_mc, s_tag, "read: temp");
  RA_CHECK_NULL_PTR(out_humidity_mpct, s_tag, "read: hum");
  RA_CHECK_NULL_PTR(out_pressure_pa, s_tag, "read: pres");
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }

  uint8_t burst[8] = {};
  const ra_err_t bus_rc =
      priv_read_block((uint8_t)k_ra_bme280_reg_data_start, burst,
                      (uint32_t)k_ra_bme280_data_burst_len);
  if (bus_rc != k_ra_ok) {
    return bus_rc;
  }

  /* Press: 0xF7 (msb), 0xF8 (lsb), 0xF9[7:4] (xlsb) -- 20-bit unsigned.
   * Temp:  0xFA (msb), 0xFB (lsb), 0xFC[7:4] (xlsb) -- 20-bit unsigned.
   * Hum:   0xFD (msb), 0xFE (lsb)                   -- 16-bit unsigned. */
  const int32_t adc_p =
      (int32_t)(((uint32_t)burst[0] << 12) |
                ((uint32_t)burst[1] << (uint32_t)k_ra_bme280_nibble_shift) |
                ((uint32_t)burst[2] >> (uint32_t)k_ra_bme280_nibble_shift));
  const int32_t adc_t =
      (int32_t)(((uint32_t)burst[3] << 12) |
                ((uint32_t)burst[4] << (uint32_t)k_ra_bme280_nibble_shift) |
                ((uint32_t)burst[5] >> (uint32_t)k_ra_bme280_nibble_shift));
  const int32_t adc_h =
      (int32_t)(((uint32_t)burst[6] << (uint32_t)k_ra_bme280_byte_shift) |
                (uint32_t)burst[7]);

  ra_sensor_bme280_test_compensate(&s_state.calib, adc_t, adc_p, adc_h,
                                   out_temperature_mc, out_humidity_mpct,
                                   out_pressure_pa);
  return k_ra_ok;
}
