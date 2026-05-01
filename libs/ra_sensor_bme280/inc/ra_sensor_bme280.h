/**
 * @file ra_sensor_bme280.h
 * @brief Bosch BME280 combined humidity / pressure / temperature sensor driver.
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Hand-written driver for the Bosch BME280 environmental sensor sitting
 * on the RA8D2 IIC_B I2C bus. The chip combines a temperature, a
 * pressure and a relative-humidity channel behind a single I2C target
 * address (0x76 with SDO tied low, 0x77 with SDO tied high).
 *
 * All references in the code base are to "BME280 datasheet rev 1.6"
 * ( BST-BME280-DS002-1, May 2022 ); the Bosch register map and the
 * compensation algorithm in section 4.2 of that document are the
 * authoritative source. The driver implements the integer
 * "fixed-point" forms of the Bosch reference compensation (datasheet
 * 4.2.3 ``BME280_compensate_T_int32`` / ``BME280_compensate_P_int64`` /
 * ``bme280_compensate_H_int32``) so the firmware path remains free of
 * single-precision floating point in line with the project's NASA
 * Power-of-10 stance.
 *
 * Lifecycle:
 *
 * 1. ``ra_sensor_bme280_init(cfg)``       -- bring up IIC_B, soft-reset
 *                                            the chip, verify the chip
 *                                            id (0x60), read the 26 byte
 *                                            calibration block, programme
 *                                            the oversampling / IIR /
 *                                            standby settings, leave the
 *                                            chip in normal mode.
 * 2. ``ra_sensor_bme280_get_chip_id(&id)``-- read register 0xD0 once.
 * 3. ``ra_sensor_bme280_read(&t,&h,&p)``  -- one-shot read: pulls 8
 *                                            bytes (0xF7..0xFE), runs
 *                                            the Bosch compensation, and
 *                                            hands back fixed-point
 *                                            engineering values.
 *
 * Output units (see datasheet 4.2.3):
 *   - Temperature -- millidegrees Celsius (signed, int32_t).
 *     ``5123`` means ``+51.23 degC``.
 *   - Pressure    -- pascals (uint32_t). The Bosch Q24.8 format is
 *                    rounded down to whole pascals at the API edge.
 *   - Humidity    -- per-mille relative humidity (signed millipercent),
 *                    i.e. ``42500`` means ``42.500 %RH``. The native
 *                    Bosch Q22.10 format is rescaled by /1024 *1000.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/* =============================================================================
 * Public constants and types
 * =============================================================================
 */

/**
 * @enum ra_sensor_bme280_addr_t
 * @brief 7-bit I2C target addresses defined by the BME280 datasheet
 *        rev 1.6 sec 5.2 ("I2C interface").
 *
 * @details
 * The state of the SDO pin selects between the two addresses; both
 * are valid bus topologies on the EK-RA8D2 carrier.
 */
typedef enum : uint8_t {
  k_ra_sensor_bme280_addr_low = 0x76U,  /**< SDO tied to GND. */
  k_ra_sensor_bme280_addr_high = 0x77U, /**< SDO tied to VDDIO. */
} ra_sensor_bme280_addr_t;

/**
 * @enum ra_sensor_bme280_osrs_t
 * @brief Oversampling ratios (datasheet rev 1.6 sec 5.4.1, Table 20).
 */
typedef enum : uint8_t {
  k_ra_sensor_bme280_osrs_skip = 0x00U, /**< Channel disabled. */
  k_ra_sensor_bme280_osrs_x1 = 0x01U,   /**< Oversampling x 1. */
  k_ra_sensor_bme280_osrs_x2 = 0x02U,   /**< Oversampling x 2. */
  k_ra_sensor_bme280_osrs_x4 = 0x03U,   /**< Oversampling x 4. */
  k_ra_sensor_bme280_osrs_x8 = 0x04U,   /**< Oversampling x 8. */
  k_ra_sensor_bme280_osrs_x16 = 0x05U,  /**< Oversampling x 16. */
} ra_sensor_bme280_osrs_t;

/**
 * @enum ra_sensor_bme280_filter_t
 * @brief IIR filter coefficient (datasheet rev 1.6 sec 3.4.4 + 5.4.5).
 */
typedef enum : uint8_t {
  k_ra_sensor_bme280_filter_off = 0x00U, /**< Filter disabled.   */
  k_ra_sensor_bme280_filter_2 = 0x01U,   /**< Coefficient 2.     */
  k_ra_sensor_bme280_filter_4 = 0x02U,   /**< Coefficient 4.     */
  k_ra_sensor_bme280_filter_8 = 0x03U,   /**< Coefficient 8.     */
  k_ra_sensor_bme280_filter_16 = 0x04U,  /**< Coefficient 16.    */
} ra_sensor_bme280_filter_t;

/**
 * @enum ra_sensor_bme280_standby_t
 * @brief ``t_sb`` standby duration in normal mode (datasheet 5.4.5).
 */
typedef enum : uint8_t {
  k_ra_sensor_bme280_sb_0p5_ms = 0x00U,  /**< 0.5 ms. */
  k_ra_sensor_bme280_sb_62p5_ms = 0x01U, /**< 62.5 ms. */
  k_ra_sensor_bme280_sb_125_ms = 0x02U,  /**< 125 ms. */
  k_ra_sensor_bme280_sb_250_ms = 0x03U,  /**< 250 ms. */
  k_ra_sensor_bme280_sb_500_ms = 0x04U,  /**< 500 ms. */
  k_ra_sensor_bme280_sb_1000_ms = 0x05U, /**< 1000 ms. */
  k_ra_sensor_bme280_sb_10_ms = 0x06U,   /**< 10 ms (BME280 only). */
  k_ra_sensor_bme280_sb_20_ms = 0x07U,   /**< 20 ms (BME280 only). */
} ra_sensor_bme280_standby_t;

/**
 * @struct ra_sensor_bme280_cfg_t
 * @brief Configuration descriptor consumed by ``ra_sensor_bme280_init``.
 *
 * @invariant ``i2c_channel`` == 0 on the RA8D2 (only one IIC_B
 *            channel exists on this part).
 * @invariant ``target_7b`` is one of ``ra_sensor_bme280_addr_t``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t i2c_channel;                /**< IIC_B channel (always 0).   */
  uint8_t target_7b;                  /**< 7-bit BME280 address.       */
  ra_sensor_bme280_osrs_t osrs_t;     /**< Temperature oversampling.   */
  ra_sensor_bme280_osrs_t osrs_p;     /**< Pressure oversampling.      */
  ra_sensor_bme280_osrs_t osrs_h;     /**< Humidity oversampling.      */
  ra_sensor_bme280_filter_t filter;   /**< IIR filter coefficient.     */
  ra_sensor_bme280_standby_t standby; /**< Normal-mode standby time.   */
} ra_sensor_bme280_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Bring up the BME280 driver against an IIC_B channel.
 *
 * @details
 * Sequence (datasheet rev 1.6 sec 5.4 "Memory map and register
 * description"):
 *
 *   1. ``ra_iic_b_init`` -- 400 kHz fast-mode clock, 60 MHz PCLKA.
 *   2. Read register ``0xD0`` and verify the chip id is ``0x60``
 *      (datasheet sec 5.4.1).
 *   3. Write ``0xB6`` to register ``0xE0`` to issue a soft reset
 *      (datasheet sec 5.4.2). Wait for ``status.im_update`` (0xF3) to
 *      clear, polling up to 10 ms.
 *   4. Read 26 bytes of calibration data starting at ``0x88`` and 7
 *      bytes starting at ``0xE1`` -- decode dig_T1..dig_H6 per
 *      datasheet Table 16.
 *   5. Write ``ctrl_hum`` (0xF2) with ``osrs_h`` -- this register only
 *      latches when ``ctrl_meas`` (0xF4) is written, so that comes
 *      next.
 *   6. Write ``config`` (0xF5): ``standby<<5 | filter<<2``.
 *   7. Write ``ctrl_meas`` (0xF4): ``osrs_t<<5 | osrs_p<<2 | mode``,
 *      with ``mode = 0b11`` (normal mode).
 *
 * @param[in] cfg Configuration descriptor.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok                 Driver is up; chip is in normal mode.
 * @retval k_ra_err_null_ptr       ``cfg`` is NULL.
 * @retval k_ra_err_invalid_arg    ``cfg`` field out of range.
 * @retval k_ra_err_invalid_state  Driver already initialised.
 * @retval k_ra_err_hw_init_failed Chip id mismatch or status never
 *                                 cleared after soft reset.
 * @retval k_ra_err_i2c_error      Underlying ``ra_iic_b_*`` failure.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Driver not currently initialised.
 * @post On ``k_ra_ok`` the chip is sampling continuously in normal
 *       mode and ``ra_sensor_bme280_read`` is callable.
 *
 * @note Not thread-safe.
 *
 * @par Example:
 * @code
 * const ra_sensor_bme280_cfg_t cfg = {
 *   .i2c_channel = 0U,
 *   .target_7b   = (uint8_t)k_ra_sensor_bme280_addr_low,
 *   .osrs_t      = k_ra_sensor_bme280_osrs_x2,
 *   .osrs_p      = k_ra_sensor_bme280_osrs_x16,
 *   .osrs_h      = k_ra_sensor_bme280_osrs_x1,
 *   .filter      = k_ra_sensor_bme280_filter_16,
 *   .standby     = k_ra_sensor_bme280_sb_500_ms,
 * };
 * (void)ra_sensor_bme280_init(&cfg);
 * @endcode
 *
 * @see ra_sensor_bme280_read
 * @see ra_sensor_bme280_get_chip_id
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sensor_bme280_init(const ra_sensor_bme280_cfg_t *cfg);

/**
 * @brief Tear the driver down and release the IIC_B channel.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok                 Driver torn down.
 * @retval k_ra_err_not_initialized Driver was not initialised.
 *
 * @pre Caller is not in the middle of ``ra_sensor_bme280_read``.
 * @post Driver state cleared; subsequent reads will return
 *       ``k_ra_err_not_initialized`` until ``init`` is called again.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sensor_bme280_deinit(void);

/* =============================================================================
 * One-shot accessors
 * =============================================================================
 */

/**
 * @brief Read the BME280 chip-id byte at register 0xD0.
 *
 * @details
 * Per datasheet rev 1.6 sec 5.4.1 the BME280 always reports ``0x60``;
 * BMP280 reports ``0x58`` and BME680 reports ``0x61``. The driver
 * uses this read internally during init to gate further bring-up,
 * but the function is also exposed publicly so callers can probe the
 * bus.
 *
 * @param[out] out_id Destination for the chip-id byte. Non-NULL.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok                  ``*out_id`` populated.
 * @retval k_ra_err_null_ptr        ``out_id`` is NULL.
 * @retval k_ra_err_not_initialized Driver init never ran.
 * @retval k_ra_err_i2c_error       Bus-level failure.
 *
 * @pre Driver previously initialised.
 * @post On ``k_ra_ok``, ``*out_id`` holds the byte read from 0xD0.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sensor_bme280_get_chip_id(uint8_t *out_id);

/**
 * @brief One-shot read of the three compensated channels.
 *
 * @details
 * Steps (datasheet rev 1.6 sec 4 "Measurement flow"):
 *   1. Burst-read 8 bytes from 0xF7 (press_msb..hum_lsb).
 *   2. Decode 20-bit press / 20-bit temp / 16-bit hum raw counts.
 *   3. Run ``BME280_compensate_T_int32`` to obtain ``t_fine``.
 *   4. Run ``BME280_compensate_P_int64`` -> Q24.8 Pa.
 *   5. Run ``bme280_compensate_H_int32`` -> Q22.10 %RH.
 *   6. Rescale to integer engineering units.
 *
 * Output units:
 *   - ``out_temperature_mc``  : signed millidegrees Celsius (e.g.
 *                                25340 == 25.340 degC).
 *   - ``out_pressure_pa``     : whole pascals (Q24.8 truncated).
 *   - ``out_humidity_mpct``   : signed millipercent RH (e.g. 42500
 *                                == 42.500 %RH).
 *
 * @param[out] out_temperature_mc Signed millidegrees Celsius.
 * @param[out] out_humidity_mpct  Signed millipercent relative humidity.
 * @param[out] out_pressure_pa    Whole pascals.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok                  All three channels valid.
 * @retval k_ra_err_null_ptr        Any output pointer is NULL.
 * @retval k_ra_err_not_initialized Driver init never ran.
 * @retval k_ra_err_i2c_error       Burst read failed.
 *
 * @pre Driver previously initialised and chip is in normal mode.
 * @post On ``k_ra_ok`` all three outputs are populated.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_sensor_bme280_read(int32_t *out_temperature_mc,
                                             int32_t *out_humidity_mpct,
                                             uint32_t *out_pressure_pa);

/* =============================================================================
 * Test hooks (visible for ra_sensor_bme280 unit tests only)
 * =============================================================================
 */

/**
 * @struct ra_sensor_bme280_calib_t
 * @brief Decoded factory calibration block (datasheet rev 1.6 Table 16).
 *
 * @details
 * Loaded once during ``ra_sensor_bme280_init`` and consumed by every
 * subsequent compensation call. Field names match the Bosch reference
 * code one-for-one so the formulas remain visually verifiable against
 * the datasheet.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint16_t dig_T1; /**< 0x88 / 0x89, unsigned. */
  int16_t dig_T2;  /**< 0x8A / 0x8B.           */
  int16_t dig_T3;  /**< 0x8C / 0x8D.           */
  uint16_t dig_P1; /**< 0x8E / 0x8F, unsigned. */
  int16_t dig_P2;  /**< 0x90 / 0x91.           */
  int16_t dig_P3;  /**< 0x92 / 0x93.           */
  int16_t dig_P4;  /**< 0x94 / 0x95.           */
  int16_t dig_P5;  /**< 0x96 / 0x97.           */
  int16_t dig_P6;  /**< 0x98 / 0x99.           */
  int16_t dig_P7;  /**< 0x9A / 0x9B.           */
  int16_t dig_P8;  /**< 0x9C / 0x9D.           */
  int16_t dig_P9;  /**< 0x9E / 0x9F.           */
  uint8_t dig_H1;  /**< 0xA1, unsigned.        */
  int16_t dig_H2;  /**< 0xE1 / 0xE2.           */
  uint8_t dig_H3;  /**< 0xE3, unsigned.        */
  int16_t dig_H4;  /**< 0xE4 / 0xE5[3:0].      */
  int16_t dig_H5;  /**< 0xE5[7:4] / 0xE6.      */
  int8_t dig_H6;   /**< 0xE7.                  */
} ra_sensor_bme280_calib_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Decode the 26-byte ``0x88..0xA1`` + 7-byte ``0xE1..0xE7``
 *        calibration block into a ``ra_sensor_bme280_calib_t``.
 *
 * @details
 * Exposed for test use so the host suite can replay the Bosch
 * reference data block from datasheet 4.2.3 ("Compensation formulas")
 * and verify the decode + compensation pipeline against the
 * known-answer outputs.
 *
 * @param[in]  raw_88_a1 Pointer to a 26-byte buffer covering 0x88..0xA1.
 * @param[in]  raw_e1_e7 Pointer to a  7-byte buffer covering 0xE1..0xE7.
 * @param[out] out       Decoded calibration descriptor.
 *
 * @pre All three pointers are non-NULL.
 * @post ``*out`` reflects the wire-format calibration table.
 *
 * @since 0.1.0
 */
void ra_sensor_bme280_test_decode_calib(const uint8_t *raw_88_a1,
                                        const uint8_t *raw_e1_e7,
                                        ra_sensor_bme280_calib_t *out);

/**
 * @brief Run the Bosch integer compensation against caller-supplied
 *        raw counts and calibration.
 *
 * @details
 * Implements the three reference functions from datasheet rev 1.6
 * sec 4.2.3 ("BME280_compensate_T_int32", "BME280_compensate_P_int64",
 * "bme280_compensate_H_int32") and rescales to the public engineering
 * units documented on ``ra_sensor_bme280_read``.
 *
 * @param[in]  calib              Decoded calibration block.
 * @param[in]  adc_t              20-bit raw temperature.
 * @param[in]  adc_p              20-bit raw pressure.
 * @param[in]  adc_h              16-bit raw humidity.
 * @param[out] out_temperature_mc Signed millidegrees Celsius.
 * @param[out] out_humidity_mpct  Signed millipercent RH.
 * @param[out] out_pressure_pa    Whole pascals.
 *
 * @pre All pointers non-NULL.
 * @post Outputs populated.
 *
 * @since 0.1.0
 */
void ra_sensor_bme280_test_compensate(const ra_sensor_bme280_calib_t *calib,
                                      int32_t adc_t, int32_t adc_p,
                                      int32_t adc_h,
                                      int32_t *out_temperature_mc,
                                      int32_t *out_humidity_mpct,
                                      uint32_t *out_pressure_pa);

#ifdef __cplusplus
}
#endif
