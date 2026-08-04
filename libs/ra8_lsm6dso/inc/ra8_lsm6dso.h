/**
 * @file ra8_lsm6dso.h
 * @brief ST LSM6DSO 6-DoF IMU driver (accel + gyro + temperature)
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details
 * Public API for a polling, transport-agnostic driver for the
 * STMicroelectronics LSM6DSO 3-axis accelerometer + 3-axis gyroscope.
 * The part is carried by the MikroE 6DOF IMU 12 Click (Digikey
 * 1471-MIKROE-4073-ND); the same silicon ships on several other
 * MEMS-sensor break-out boards as well. All register-level citations
 * point at "LSM6DSO datasheet (ST DS12140 Rev 4)".
 *
 * Transport abstraction (Dependency Inversion, see CLAUDE.md "SOLID
 * Principles for C"):
 *
 *   - The driver does not call ``ra8_iic_b_*`` or ``ra8_spi_*`` directly.
 *   - Instead the caller hands a ``ra8_lsm6dso_bus_t`` interface whose
 *     ``read_regs`` / ``write_regs`` callbacks address the part. The
 *     production firmware wires these to ``ra8_iic_b_transfer`` /
 *     ``ra8_iic_b_write`` (I2C) or ``ra8_spi_xfer8`` (SPI); unit tests
 *     wire them to a software-mock transport in
 *     ``tests/test_ra8_lsm6dso.c``.
 *
 * Surface (mirrors the deliverables list in the task brief):
 *
 *   - ``ra8_lsm6dso_init``               Bind a bus + cache its handle.
 *   - ``ra8_lsm6dso_who_am_i``           Returns ``0x6C`` if the part is
 *                                       alive (DS12140 sec 9.11 WHO_AM_I).
 *   - ``ra8_lsm6dso_set_accel_range``    Configure +-2 / +-4 / +-8 / +-16 g.
 *   - ``ra8_lsm6dso_set_gyro_range``     Configure +-125 / +-250 / +-500 /
 *                                       +-1000 / +-2000 dps.
 *   - ``ra8_lsm6dso_set_odr``            Output Data Rate (12.5 .. 6664 Hz).
 *   - ``ra8_lsm6dso_read_accel``         Raw 16-bit two's complement counts.
 *   - ``ra8_lsm6dso_read_gyro``          Raw 16-bit two's complement counts.
 *   - ``ra8_lsm6dso_read_temp``          On-die temperature in centi-degrees
 *                                       Celsius (DS12140 sec 9.5).
 *   - ``ra8_lsm6dso_read_xl_gyro_fifo``  Batch read of XL+G samples from
 *                                       the embedded 9 kbyte FIFO.
 *
 * The driver carries no global state outside the caller's
 * ``ra8_lsm6dso_t`` -- multiple LSM6DSO parts on different buses can
 * coexist with one descriptor each.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/* =============================================================================
 * Public typed-enum constants
 * =============================================================================
 */

/**
 * @enum ra8_lsm6dso_const_t
 * @brief Part-level identification + sizing constants.
 *
 * @details
 * Per LSM6DSO DS12140 Rev 4 sec 9.11 "WHO_AM_I (0Fh)": the read-only
 * device-identification register returns ``0x6C`` (binary
 * ``0b01101100``) on a healthy part. Any other value indicates a wrong
 * part, a wired-up wrong address, or a dead bus.
 */
typedef enum : uint8_t {
  k_lsm6dso_who_am_i_value  = 0x6CU, /**< WHO_AM_I expected value (DS12140 sec 9.11).  */
  k_lsm6dso_fifo_words_max  = 0x80U, /**< Default FIFO-batch cap exposed to callers.   */
  k_lsm6dso_fifo_bytes_word = 0x07U, /**< Bytes per FIFO word (TAG + 6 data, sec 9.7). */
} ra8_lsm6dso_const_t;

/**
 * @enum ra8_lsm6dso_i2c_addr_t
 * @brief Default 7-bit I2C target addresses.
 *
 * @details
 * Per DS12140 sec 6.1.1 "I2C operation": the LSM6DSO 7-bit peripheral
 * address is ``1101 010x`` where ``x`` is the inverted SDO/SA0 pin
 * level. ``SDO/SA0 = 0`` selects ``0x6A``; ``SDO/SA0 = 1`` selects
 * ``0x6B``. The MikroE 6DOF IMU 12 Click ties SA0 high by default
 * (per MikroE schematic rev v100), so the default board address is
 * ``0x6B``.
 */
typedef enum : uint8_t {
  k_lsm6dso_i2c_addr_sa0_low  = 0x6AU, /**< SDO/SA0 tied low.  */
  k_lsm6dso_i2c_addr_sa0_high = 0x6BU, /**< SDO/SA0 tied high. */
} ra8_lsm6dso_i2c_addr_t;

/**
 * @enum ra8_lsm6dso_reg_t
 * @brief Register-map addresses used by this driver.
 *
 * @details
 * Subset of the full LSM6DSO register map (DS12140 sec 9 "Register
 * description"). Only registers actually accessed by the driver
 * appear here -- adding embedded-functions or pedometer registers
 * is a future extension.
 */
typedef enum : uint8_t {
  k_lsm6dso_reg_fifo_ctrl1    = 0x07U, /**< FIFO_CTRL1   (DS12140 sec 9.6).       */
  k_lsm6dso_reg_fifo_ctrl2    = 0x08U, /**< FIFO_CTRL2   (DS12140 sec 9.7).       */
  k_lsm6dso_reg_fifo_ctrl3    = 0x09U, /**< FIFO_CTRL3   (DS12140 sec 9.8).       */
  k_lsm6dso_reg_fifo_ctrl4    = 0x0AU, /**< FIFO_CTRL4   (DS12140 sec 9.9).       */
  k_lsm6dso_reg_who_am_i      = 0x0FU, /**< WHO_AM_I     (DS12140 sec 9.11).      */
  k_lsm6dso_reg_ctrl1_xl      = 0x10U, /**< CTRL1_XL     (DS12140 sec 9.12).      */
  k_lsm6dso_reg_ctrl2_g       = 0x11U, /**< CTRL2_G      (DS12140 sec 9.13).      */
  k_lsm6dso_reg_ctrl3_c       = 0x12U, /**< CTRL3_C      (DS12140 sec 9.14).      */
  k_lsm6dso_reg_ctrl6_c       = 0x15U, /**< CTRL6_C      (DS12140 sec 9.17).      */
  k_lsm6dso_reg_ctrl7_g       = 0x16U, /**< CTRL7_G      (DS12140 sec 9.18).      */
  k_lsm6dso_reg_out_temp_l    = 0x20U, /**< OUT_TEMP_L   (DS12140 sec 9.27).      */
  k_lsm6dso_reg_outx_l_g      = 0x22U, /**< OUTX_L_G     (DS12140 sec 9.29).      */
  k_lsm6dso_reg_outx_l_a      = 0x28U, /**< OUTX_L_A     (DS12140 sec 9.35).      */
  k_lsm6dso_reg_fifo_status1  = 0x3AU, /**< FIFO_STATUS1 (DS12140 sec 9.44).      */
  k_lsm6dso_reg_fifo_data_out = 0x78U, /**< FIFO_DATA_OUT_TAG (DS12140 sec 9.60). */
} ra8_lsm6dso_reg_t;

/**
 * @enum ra8_lsm6dso_xl_fs_t
 * @brief Accelerometer full-scale (FS_XL) range codes.
 *
 * @details
 * Per DS12140 sec 9.12 "CTRL1_XL (10h)" Table 44 "Accelerometer
 * full-scale selection": FS_XL[1:0] occupies CTRL1_XL bits [3:2].
 * The numeric values below are the raw bit-field codes for FS_XL.
 *
 * Sensitivity (LSB/g) per FS_XL = 0.061 / 0.122 / 0.244 / 0.488 mg/LSB
 * for +-2, +-4, +-8, +-16 g respectively (DS12140 Table 3).
 */
typedef enum : uint8_t {
  k_lsm6dso_xl_fs_2g  = 0x00U, /**< +-2  g, sens 0.061 mg/LSB (default). */
  k_lsm6dso_xl_fs_16g = 0x01U, /**< +-16 g, sens 0.488 mg/LSB.           */
  k_lsm6dso_xl_fs_4g  = 0x02U, /**< +-4  g, sens 0.122 mg/LSB.           */
  k_lsm6dso_xl_fs_8g  = 0x03U, /**< +-8  g, sens 0.244 mg/LSB.           */
} ra8_lsm6dso_xl_fs_t;

/**
 * @enum ra8_lsm6dso_g_fs_t
 * @brief Gyroscope full-scale (FS_G) range codes.
 *
 * @details
 * Per DS12140 sec 9.13 "CTRL2_G (11h)" Table 47 "Gyroscope full-scale
 * selection": FS_G[1:0] occupies CTRL2_G bits [3:2], with the
 * FS_125 bit at bit [1] selecting the +-125 dps narrow scale. The
 * driver folds both fields into a single enum.
 *
 * Sensitivity (mdps/LSB) per FS_G = 4.375 / 8.75 / 17.5 / 35 / 70 for
 * +-125, +-250, +-500, +-1000, +-2000 dps respectively (DS12140
 * Table 3).
 */
typedef enum : uint8_t {
  k_lsm6dso_g_fs_125dps  = 0x00U, /**< +-125  dps narrow scale (FS_125 = 1). */
  k_lsm6dso_g_fs_250dps  = 0x01U, /**< +-250  dps (default).                 */
  k_lsm6dso_g_fs_500dps  = 0x02U, /**< +-500  dps.                           */
  k_lsm6dso_g_fs_1000dps = 0x03U, /**< +-1000 dps.                           */
  k_lsm6dso_g_fs_2000dps = 0x04U, /**< +-2000 dps.                           */
} ra8_lsm6dso_g_fs_t;

/**
 * @enum ra8_lsm6dso_odr_t
 * @brief Output Data Rate codes (shared between XL and G).
 *
 * @details
 * Per DS12140 sec 9.12 "CTRL1_XL (10h)" Table 45 (ODR_XL) and
 * sec 9.13 "CTRL2_G (11h)" Table 48 (ODR_G): the four-bit ODR field
 * occupies bits [7:4] of CTRL1_XL and CTRL2_G respectively. The codes
 * are identical for both peripherals; ``ra8_lsm6dso_set_odr`` writes
 * the chosen code into the high nibble of both CTRL1_XL and CTRL2_G.
 */
typedef enum : uint8_t {
  k_lsm6dso_odr_off    = 0x00U, /**< Power-down (default).   */
  k_lsm6dso_odr_12_5hz = 0x01U, /**< 12.5  Hz (low-power).   */
  k_lsm6dso_odr_26hz   = 0x02U, /**< 26    Hz.               */
  k_lsm6dso_odr_52hz   = 0x03U, /**< 52    Hz.               */
  k_lsm6dso_odr_104hz  = 0x04U, /**< 104   Hz (normal mode). */
  k_lsm6dso_odr_208hz  = 0x05U, /**< 208   Hz.               */
  k_lsm6dso_odr_416hz  = 0x06U, /**< 416   Hz.               */
  k_lsm6dso_odr_833hz  = 0x07U, /**< 833   Hz.               */
  k_lsm6dso_odr_1660hz = 0x08U, /**< 1.66  kHz.              */
  k_lsm6dso_odr_3330hz = 0x09U, /**< 3.33  kHz.              */
  k_lsm6dso_odr_6660hz = 0x0AU, /**< 6.66  kHz.              */
} ra8_lsm6dso_odr_t;

/**
 * @struct ra8_lsm6dso_xyz_t
 * @brief 3-axis raw sample (X, Y, Z as 16-bit two's-complement counts).
 *
 * @details
 * Returned by ``ra8_lsm6dso_read_accel`` and ``ra8_lsm6dso_read_gyro``.
 * Conversion to physical units (g / dps) is up to the caller because
 * it requires the active full-scale code at the time of sampling --
 * see DS12140 Table 3 for the per-FS sensitivity values.
 */
typedef struct {
  int16_t x; /**< X-axis raw count. */
  int16_t y; /**< Y-axis raw count. */
  int16_t z; /**< Z-axis raw count. */
} ra8_lsm6dso_xyz_t;

/* =============================================================================
 * Transport abstraction (Dependency Inversion)
 * =============================================================================
 */

/**
 * @typedef ra8_lsm6dso_read_fn_t
 * @brief Read ``len`` bytes from ``reg`` on the target.
 *
 * @details
 * Implementations must honor the LSM6DSO auto-increment behaviour
 * (DS12140 sec 6.1.2 "I2C Operation") so that a multi-byte read
 * starting at register N returns bytes from N, N+1, ... -- the
 * production I2C and SPI HAL paths do this naturally.
 *
 * @param[in]  ctx Transport context (e.g. ``ra8_lsm6dso_i2c_ctx_t`` or
 *                 ``ra8_lsm6dso_spi_ctx_t`` cast to ``void*``).
 * @param[in]  reg First register address.
 * @param[out] buf Destination buffer (non-NULL when ``len`` > 0).
 * @param[in]  len Byte count.
 *
 * @return ``ra8_err_t`` Error code propagated from the transport.
 * @retval k_ra8_ok           Transfer succeeded.
 * @retval k_ra8_err_nack     Target NACKed (I2C) or returned a hard
 *                           error (SPI).
 * @retval k_ra8_err_null_ptr ``buf`` is NULL with non-zero ``len``.
 */
typedef ra8_err_t (*ra8_lsm6dso_read_fn_t)(void* ctx, uint8_t reg, uint8_t* buf, uint32_t len);

/**
 * @typedef ra8_lsm6dso_write_fn_t
 * @brief Write ``len`` bytes starting at ``reg`` on the target.
 *
 * @param[in] ctx Transport context.
 * @param[in] reg First register address.
 * @param[in] buf Source buffer (non-NULL when ``len`` > 0).
 * @param[in] len Byte count.
 *
 * @return ``ra8_err_t`` Error code propagated from the transport.
 */
typedef ra8_err_t (*ra8_lsm6dso_write_fn_t)(void*          ctx,
                                            uint8_t        reg,
                                            const uint8_t* buf,
                                            uint32_t       len);

/**
 * @struct ra8_lsm6dso_bus_t
 * @brief Transport interface bound at ``ra8_lsm6dso_init`` time.
 *
 * @details
 * Production code wires ``read_regs`` and ``write_regs`` to thin
 * adapters over ``ra8_iic_b_transfer`` / ``ra8_iic_b_write`` or
 * ``ra8_spi_xfer8``. Unit tests wire them to a canned-response mock.
 * The ``ctx`` field is passed through to both callbacks unchanged.
 */
typedef struct {
  ra8_lsm6dso_read_fn_t  read_regs;  /**< Read callback.  Non-NULL. */
  ra8_lsm6dso_write_fn_t write_regs; /**< Write callback. Non-NULL. */
  void*                  ctx;        /**< Caller-supplied context.  */
} ra8_lsm6dso_bus_t;

/**
 * @struct ra8_lsm6dso_t
 * @brief Per-instance driver state.
 *
 * @details
 * Holds a copy of the transport interface plus the most recent
 * configuration codes so ``ra8_lsm6dso_read_*`` can apply the right
 * scaling if a future revision of the API extends the read helpers
 * to return engineering units. Allocated by the caller (NASA Rule 3:
 * no dynamic allocation inside the driver).
 */
typedef struct {
  ra8_lsm6dso_bus_t   bus;           /**< Transport interface.                  */
  ra8_lsm6dso_xl_fs_t accel_fs_code; /**< Last accel full-scale code written.   */
  ra8_lsm6dso_g_fs_t  gyro_fs_code;  /**< Last gyro  full-scale code written.   */
  ra8_lsm6dso_odr_t   odr_code;      /**< Last ODR code written to both blocks. */
  bool                initialized;   /**< True after a successful ``_init``.    */
} ra8_lsm6dso_t;

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Bind a transport to a driver instance.
 *
 * @details
 * Copies the supplied ``bus`` interface into ``out_dev`` and marks the
 * instance initialised. Does not touch the wire -- the caller is
 * expected to follow this with ``ra8_lsm6dso_who_am_i`` (sanity check)
 * and then ``ra8_lsm6dso_set_*`` calls to configure the part.
 *
 * @param[out] out_dev Driver state, populated on success.
 * @param[in]  bus     Transport interface (must have non-NULL
 *                     ``read_regs`` and ``write_regs``).
 *
 * @return ``ra8_err_t`` Error code.
 * @retval k_ra8_ok              Driver state initialized.
 * @retval k_ra8_err_null_ptr    ``out_dev`` or ``bus`` is NULL, or one
 *                              of the callbacks in ``bus`` is NULL.
 *
 * @pre ``out_dev`` points at writable storage.
 * @pre ``bus->read_regs`` and ``bus->write_regs`` are non-NULL.
 * @post On success ``out_dev->initialized == true``.
 * @post On success the transport is captured by value; the caller
 *       may release ``*bus`` after this returns.
 *
 * @note Not thread-safe. Call once per driver instance from init context.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lsm6dso_init(ra8_lsm6dso_t* out_dev, const ra8_lsm6dso_bus_t* bus);

/* =============================================================================
 * Identification
 * =============================================================================
 */

/**
 * @brief Read the WHO_AM_I register.
 *
 * @details
 * Reads register ``0x0F`` and returns the raw byte. On a healthy
 * LSM6DSO this is ``0x6C`` (``k_lsm6dso_who_am_i_value``); any other
 * value means the wrong part is wired or the bus is mis-addressed.
 *
 * @param[in]  dev    Driver instance.
 * @param[out] out_id Receives the WHO_AM_I byte.
 *
 * @return ``ra8_err_t`` Error code.
 * @retval k_ra8_ok              ``*out_id`` populated.
 * @retval k_ra8_err_null_ptr    ``dev`` or ``out_id`` is NULL.
 * @retval k_ra8_err_invalid_state ``dev`` has not been initialized.
 * @retval k_ra8_err_nack        Transport reported a NACK.
 *
 * @pre ``ra8_lsm6dso_init`` returned ``k_ra8_ok`` for ``dev``.
 * @pre ``out_id`` points at writable storage.
 * @post On success ``*out_id`` carries the device-ID byte.
 * @post On failure ``*out_id`` is unmodified.
 *
 * @note Not thread-safe per-instance.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lsm6dso_who_am_i(ra8_lsm6dso_t* dev, uint8_t* out_id);

/* =============================================================================
 * Configuration
 * =============================================================================
 */

/**
 * @brief Program the accelerometer full-scale range.
 *
 * @details
 * Read-modify-writes the FS_XL field in CTRL1_XL (DS12140 sec 9.12).
 * The ODR_XL nibble is preserved by reading the current value first.
 *
 * @param[in,out] dev Driver instance.
 * @param[in]     fs  Range code (one of ``k_lsm6dso_xl_fs_*``).
 *
 * @return ``ra8_err_t`` Error code.
 * @retval k_ra8_ok                Range programmed.
 * @retval k_ra8_err_null_ptr      ``dev`` is NULL.
 * @retval k_ra8_err_invalid_state ``dev`` not initialized.
 * @retval k_ra8_err_invalid_arg   ``fs`` out of range.
 * @retval k_ra8_err_nack          Transport reported a NACK.
 *
 * @pre ``ra8_lsm6dso_init`` returned ``k_ra8_ok`` for ``dev``.
 * @pre ``fs`` is one of the accel full-scale enum values.
 * @post On success ``dev->accel_fs_code == fs``.
 * @post On failure ``dev->accel_fs_code`` is unmodified.
 *
 * @note Not thread-safe per-instance.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lsm6dso_set_accel_range(ra8_lsm6dso_t* dev, ra8_lsm6dso_xl_fs_t fs);

/**
 * @brief Program the gyroscope full-scale range.
 *
 * @details
 * Read-modify-writes the FS_G[1:0] and FS_125 fields in CTRL2_G
 * (DS12140 sec 9.13). The ODR_G nibble is preserved by reading the
 * current value first.
 *
 * @param[in,out] dev Driver instance.
 * @param[in]     fs  Range code (one of ``k_lsm6dso_g_fs_*``).
 *
 * @return ``ra8_err_t`` Error code.
 * @retval k_ra8_ok                Range programmed.
 * @retval k_ra8_err_null_ptr      ``dev`` is NULL.
 * @retval k_ra8_err_invalid_state ``dev`` not initialized.
 * @retval k_ra8_err_invalid_arg   ``fs`` out of range.
 * @retval k_ra8_err_nack          Transport reported a NACK.
 *
 * @pre ``ra8_lsm6dso_init`` returned ``k_ra8_ok`` for ``dev``.
 * @pre ``fs`` is one of the gyro full-scale enum values.
 * @post On success ``dev->gyro_fs_code == fs``.
 * @post On failure ``dev->gyro_fs_code`` is unmodified.
 *
 * @note Not thread-safe per-instance.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lsm6dso_set_gyro_range(ra8_lsm6dso_t* dev, ra8_lsm6dso_g_fs_t fs);

/**
 * @brief Program the Output Data Rate for both the accelerometer and
 *        the gyroscope.
 *
 * @details
 * Writes ``odr`` into the ODR_XL field of CTRL1_XL and the ODR_G field
 * of CTRL2_G. The full-scale nibble of each register is preserved by
 * reading the current value first. The two blocks share an ODR scale
 * (DS12140 sec 5.3 "Output data rate and bandwidth selection") so
 * one parameter covers both.
 *
 * @param[in,out] dev Driver instance.
 * @param[in]     odr ODR code (one of ``k_lsm6dso_odr_*``).
 *
 * @return ``ra8_err_t`` Error code.
 * @retval k_ra8_ok                ODR programmed for XL and G.
 * @retval k_ra8_err_null_ptr      ``dev`` is NULL.
 * @retval k_ra8_err_invalid_state ``dev`` not initialized.
 * @retval k_ra8_err_invalid_arg   ``odr`` out of range.
 * @retval k_ra8_err_nack          Transport reported a NACK.
 *
 * @pre ``ra8_lsm6dso_init`` returned ``k_ra8_ok`` for ``dev``.
 * @pre ``odr`` is one of the ODR enum values.
 * @post On success ``dev->odr_code == odr``.
 * @post On failure ``dev->odr_code`` is unmodified.
 *
 * @note Not thread-safe per-instance.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lsm6dso_set_odr(ra8_lsm6dso_t* dev, ra8_lsm6dso_odr_t odr);

/* =============================================================================
 * Sample read paths
 * =============================================================================
 */

/**
 * @brief Read one accelerometer sample (raw 16-bit two's-complement).
 *
 * @details
 * Burst-reads 6 bytes starting at ``OUTX_L_A`` (``0x28``) into a 3x16
 * sample (DS12140 sec 9.35 .. 9.40). The LSM6DSO is little-endian so
 * the bytes are combined as ``(high << 8) | low``.
 *
 * @param[in]  dev Driver instance.
 * @param[out] out 3-axis raw sample.
 *
 * @return ``ra8_err_t`` Error code.
 * @retval k_ra8_ok                Sample read.
 * @retval k_ra8_err_null_ptr      ``dev`` or ``out`` is NULL.
 * @retval k_ra8_err_invalid_state ``dev`` not initialized.
 * @retval k_ra8_err_nack          Transport reported a NACK.
 *
 * @pre ``ra8_lsm6dso_init`` returned ``k_ra8_ok`` for ``dev``.
 * @pre Caller has previously enabled the XL with ``ra8_lsm6dso_set_odr``.
 * @post On success ``*out`` carries the latest XL sample.
 * @post On failure ``*out`` is unmodified.
 *
 * @note Not thread-safe per-instance.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lsm6dso_read_accel(ra8_lsm6dso_t* dev, ra8_lsm6dso_xyz_t* out);

/**
 * @brief Read one gyroscope sample (raw 16-bit two's-complement).
 *
 * @details
 * Burst-reads 6 bytes starting at ``OUTX_L_G`` (``0x22``) into a 3x16
 * sample (DS12140 sec 9.29 .. 9.34). The LSM6DSO is little-endian so
 * the bytes are combined as ``(high << 8) | low``.
 *
 * @param[in]  dev Driver instance.
 * @param[out] out 3-axis raw sample.
 *
 * @return ``ra8_err_t`` Error code.
 * @retval k_ra8_ok                Sample read.
 * @retval k_ra8_err_null_ptr      ``dev`` or ``out`` is NULL.
 * @retval k_ra8_err_invalid_state ``dev`` not initialized.
 * @retval k_ra8_err_nack          Transport reported a NACK.
 *
 * @pre ``ra8_lsm6dso_init`` returned ``k_ra8_ok`` for ``dev``.
 * @pre Caller has previously enabled the G with ``ra8_lsm6dso_set_odr``.
 * @post On success ``*out`` carries the latest G sample.
 * @post On failure ``*out`` is unmodified.
 *
 * @note Not thread-safe per-instance.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lsm6dso_read_gyro(ra8_lsm6dso_t* dev, ra8_lsm6dso_xyz_t* out);

/**
 * @brief Read the on-die temperature in centi-degrees Celsius.
 *
 * @details
 * Reads the 16-bit two's-complement OUT_TEMP register pair
 * (``0x20``..``0x21``, DS12140 sec 9.27 / 9.28) and converts to
 * centi-degrees Celsius using the datasheet formula
 * ``T[C] = (raw / 256) + 25``. Multiplied by 100 to stay in integer
 * arithmetic the formula becomes
 * ``out_centi_c = (raw * 100) / 256 + 2500``, which is what the
 * driver implements.
 *
 * @param[in]  dev          Driver instance.
 * @param[out] out_centi_c  Receives the temperature in centi-deg C.
 *
 * @return ``ra8_err_t`` Error code.
 * @retval k_ra8_ok                Temperature read.
 * @retval k_ra8_err_null_ptr      ``dev`` or ``out_centi_c`` is NULL.
 * @retval k_ra8_err_invalid_state ``dev`` not initialized.
 * @retval k_ra8_err_nack          Transport reported a NACK.
 *
 * @pre ``ra8_lsm6dso_init`` returned ``k_ra8_ok`` for ``dev``.
 * @pre ``out_centi_c`` points at writable storage.
 * @post On success ``*out_centi_c`` carries centi-degC.
 * @post On failure ``*out_centi_c`` is unmodified.
 *
 * @note Not thread-safe per-instance.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lsm6dso_read_temp(ra8_lsm6dso_t* dev, int32_t* out_centi_c);

/**
 * @brief Drain up to ``max_samples`` paired XL+G samples from the
 *        embedded FIFO.
 *
 * @details
 * Polls FIFO_STATUS1/2 (DS12140 sec 9.44 / 9.45) for the live sample
 * count, then issues a burst read of ``FIFO_DATA_OUT_TAG`` (``0x78``,
 * DS12140 sec 9.60 .. 9.66) for each available 7-byte FIFO word. The
 * tag byte identifies the source (XL vs G) per Table 167 "Tag
 * codes"; this helper returns whatever the FIFO happens to hold and
 * lets the caller demux.
 *
 * @param[in]  dev         Driver instance.
 * @param[out] out_buf     Destination buffer; receives raw FIFO words
 *                         laid out as
 *                         ``[tag][b0 b1 b2 b3 b4 b5]`` repeated.
 * @param[in]  max_words   Maximum FIFO words the caller can store
 *                         (capacity of ``out_buf`` in bytes is
 *                         ``max_words * k_lsm6dso_fifo_bytes_word``).
 * @param[out] out_words   Number of FIFO words actually read into
 *                         ``out_buf``.
 *
 * @return ``ra8_err_t`` Error code.
 * @retval k_ra8_ok                FIFO drained (possibly to zero words).
 * @retval k_ra8_err_null_ptr      Any of the pointers is NULL.
 * @retval k_ra8_err_invalid_state ``dev`` not initialized.
 * @retval k_ra8_err_invalid_arg   ``max_words == 0``.
 * @retval k_ra8_err_nack          Transport reported a NACK.
 *
 * @pre ``ra8_lsm6dso_init`` returned ``k_ra8_ok`` for ``dev``.
 * @pre The FIFO has been enabled by the caller (FIFO_CTRL1..4).
 * @post On success ``*out_words`` <= ``max_words``.
 * @post On failure ``*out_words`` is set to 0.
 *
 * @note Not thread-safe per-instance.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_lsm6dso_read_xl_gyro_fifo(ra8_lsm6dso_t* dev,
                                                      uint8_t*       out_buf,
                                                      uint32_t       max_words,
                                                      uint32_t*      out_words);

#ifdef __cplusplus
}
#endif
