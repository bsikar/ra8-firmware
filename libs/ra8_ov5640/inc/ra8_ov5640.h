/**
 * @file ra8_ov5640.h
 * @brief Transport-independent OmniVision OV5640 sensor driver.
 * @ingroup grp_camera
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details The driver owns the OV5640 register protocol and validated mode
 * tables, but owns no RA8 peripheral or board pin. Callers inject single-
 * register SCCB operations and a millisecond delay callback, following the
 * same dependency-inversion pattern as `ra8_lsm6dso`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief OV5640 identity and SCCB address constants. */
typedef enum : uint16_t {
  k_ra8_ov5640_chip_id = 0x5640U, /**< Expected combined chip identifier. */
} ra8_ov5640_id_t;

typedef enum : uint8_t {
  k_ra8_ov5640_addr_primary   = 0x3CU, /**< Default seven-bit SCCB address.   */
  k_ra8_ov5640_addr_secondary = 0x3DU, /**< Alternate seven-bit SCCB address. */
} ra8_ov5640_addr_t;

/** @brief Sensor output modes with validated register tables. */
typedef enum : uint8_t {
  k_ra8_ov5640_mode_vga_uyvy = 0U, /**< Packed VGA UYVY DVP stream.     */
  k_ra8_ov5640_mode_vga_jpeg = 1U, /**< Sensor-encoded VGA JPEG stream. */
} ra8_ov5640_mode_t;

/** @brief Raw OV5640 JPEG quantization-scale bounds and documented presets. */
typedef enum : uint8_t {
  k_ra8_ov5640_jpeg_quant_scale_min     = 0x00U, /**< Highest-quality raw scale. */
  k_ra8_ov5640_jpeg_quant_scale_default = 0x0CU, /**< Vendor reset-scale value.  */
  k_ra8_ov5640_jpeg_quant_scale_max     = 0x3FU, /**< Lowest-quality raw scale.  */
} ra8_ov5640_jpeg_quant_scale_t;

typedef ra8_err_t (*ra8_ov5640_read_fn_t)(void*    ctx,
                                          uint8_t  address,
                                          uint16_t reg,
                                          uint8_t* out_value);
typedef ra8_err_t (*ra8_ov5640_write_fn_t)(void* ctx, uint8_t address, uint16_t reg, uint8_t value);
typedef void (*ra8_ov5640_delay_fn_t)(void* ctx, uint32_t milliseconds);

/** @brief Injected SCCB and time services. */
typedef struct {
  ra8_ov5640_read_fn_t  read_reg;  /**< Injected single-register read.  */
  ra8_ov5640_write_fn_t write_reg; /**< Injected single-register write. */
  ra8_ov5640_delay_fn_t delay_ms;  /**< Injected millisecond delay.     */
  void*                 ctx;       /**< Caller-owned transport context. */
} ra8_ov5640_bus_t;

/** @brief Caller-owned sensor instance; supports multiple independent parts. */
typedef struct {
  ra8_ov5640_bus_t bus;         /**< Copied transport callbacks.      */
  uint8_t          address;     /**< Selected seven-bit SCCB address. */
  bool             initialized; /**< Successful binding marker.       */
} ra8_ov5640_t;

/** @brief Snapshot of the OV5640 JPEG pipeline and its most recent frame. */
typedef struct {
  uint32_t encoded_bytes;         /**< Sensor-reported JPEG payload length. */
  uint16_t compression_width;     /**< Mode-2 compressed-output width.      */
  uint16_t compression_height;    /**< Mode-2 compressed-output height.     */
  uint8_t  jpeg_ctrl01;           /**< Raw JPEG CTRL01 pacing controls.     */
  uint8_t  vfifo_ctrl00;          /**< Raw VFIFO mode-2 height control.     */
  uint8_t  href_minimum_blanking; /**< Raw JPEG HREF blanking control.      */
  bool     fifo_overflow;         /**< JPEG output FIFO overflow indicator. */
  bool     input_is_yuv422;       /**< JPEG input format selects YUV422.    */
  bool     header_output;         /**< JPEG header generation is enabled.   */
  bool     compression_enabled;   /**< Timing pipeline selects JPEG output. */
} ra8_ov5640_jpeg_status_t;

/**
 * @brief Bind a caller-supplied SCCB transport without touching the sensor.
 * @details Copies the transport callbacks and selects the primary address.
 * @param[out] dev Caller-owned sensor instance to initialize.
 * @param[in] bus Read, write, delay, and opaque-context callbacks.
 * @return Error code.
 * @retval k_ra8_ok The instance was initialized.
 * @retval k_ra8_err_null_ptr An argument or mandatory callback was `nullptr`.
 * @pre @p dev points to writable storage.
 * @pre @p bus and its callbacks remain valid for the instance lifetime.
 * @post On success @p dev is ready for probe and register access.
 * @post No SCCB transaction or delay callback has occurred.
 * @note Independent instances are thread-safe when their transports are.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ov5640_init(ra8_ov5640_t* dev, const ra8_ov5640_bus_t* bus);

/**
 * @brief Probe both legal SCCB addresses and verify chip ID `0x5640`.
 * @details Reads the two identification registers at 0x3C, then 0x3D.
 * @param[in,out] dev Initialized caller-owned sensor instance.
 * @param[out] out_id Last chip ID read, or the verified OV5640 ID.
 * @return Error code.
 * @retval k_ra8_ok An OV5640 was found and selected.
 * @retval k_ra8_err_not_found Neither legal address returned the expected ID.
 * @retval k_ra8_err_null_ptr An argument was `nullptr`.
 * @retval k_ra8_err_not_initialized @p dev was not initialized.
 * @pre ::ra8_ov5640_init completed successfully.
 * @pre Sensor XCLK is running and hardware reset is released.
 * @post On success @p dev retains the responding SCCB address.
 * @post On failure @p dev returns to the primary SCCB address.
 * @note Not thread-safe with respect to the same instance or transport.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ov5640_probe(ra8_ov5640_t* dev, uint16_t* out_id);

/**
 * @brief Software-reset and program one validated output mode.
 * @details Applies the proven VGA UYVY base table, optionally switches the
 *          sensor JPEG engine, then verifies critical register readbacks.
 * @param[in,out] dev Probed sensor instance.
 * @param[in] mode Validated output mode to program.
 * @return Error code.
 * @retval k_ra8_ok The selected mode was programmed and verified.
 * @retval k_ra8_err_not_supported @p mode has no validated table.
 * @retval k_ra8_err_not_initialized @p dev was not initialized.
 * @retval other Propagated SCCB transaction or readback error.
 * @pre ::ra8_ov5640_probe selected a responding sensor address.
 * @pre Sensor XCLK remains running throughout configuration.
 * @post On success the sensor is awake and streaming the selected mode.
 * @post On success critical output registers match their expected masks.
 * @note Not thread-safe with respect to the same instance or transport.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ov5640_configure(ra8_ov5640_t* dev, ra8_ov5640_mode_t mode);

/**
 * @brief Set the sensor JPEG encoder's raw quantization scale.
 *
 * @details Programs JPEG CTRL07 bits [5:0]. The legal range is 0..63 and a
 *          smaller value produces higher quality. This is the sensor's raw
 *          scale, not a synthetic 1..100 quality percentage.
 *
 * @param[in,out] dev Sensor instance initialized by ::ra8_ov5640_init.
 * @param[in] quant_scale Raw OV5640 quantization scale in the inclusive range
 *                        [::k_ra8_ov5640_jpeg_quant_scale_min,
 *                         ::k_ra8_ov5640_jpeg_quant_scale_max].
 * @return ::k_ra8_ok on success, or the transport/validation error.
 * @retval k_ra8_err_invalid_arg @p quant_scale exceeds 63.
 * @retval k_ra8_err_not_initialized @p dev has not been initialized.
 * @retval other Propagated SCCB read or write error.
 * @pre ::ra8_ov5640_init completed successfully.
 * @pre JPEG mode was selected before changing its encoder scale.
 * @post On success JPEG CTRL07 bits [5:0] equal @p quant_scale.
 * @post Unrelated JPEG CTRL07 bits retain their prior values.
 * @note Not thread-safe with respect to the same instance or transport.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ov5640_set_jpeg_quantization_scale(ra8_ov5640_t* dev,
                                                               uint8_t       quant_scale);

/**
 * @brief Snapshot sensor JPEG length, overflow, and routing controls.
 * @details Reads the documented JPEG length and status registers plus the
 *          input-format, header-output, and compression-enable controls.
 * @param[in,out] dev Sensor instance initialized by ::ra8_ov5640_init.
 * @param[out] out_status Caller-owned destination for the decoded snapshot.
 * @return Error code.
 * @retval k_ra8_ok Every status register was read and decoded.
 * @retval k_ra8_err_null_ptr @p dev or @p out_status was `nullptr`.
 * @retval k_ra8_err_not_initialized @p dev has not been initialized.
 * @retval other Propagated SCCB read error.
 * @pre Sensor XCLK is running and the bound SCCB transport is idle.
 * @pre @p out_status points to writable storage.
 * @post On success @p out_status describes the sampled sensor state.
 * @post Sensor configuration and streaming state remain unchanged.
 * @note Stop streaming before this call when a frame-coherent length is needed.
 * @note Not thread-safe with respect to the same instance or transport.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ov5640_jpeg_status_get(ra8_ov5640_t*             dev,
                                                   ra8_ov5640_jpeg_status_t* out_status);

/**
 * @brief Enter software standby or resume sensor streaming.
 * @details Writes the OV5640 system-control streaming state and waits for the
 *          transition to settle through the injected delay callback.
 * @param[in,out] dev Initialized sensor instance.
 * @param[in] enabled `true` to stream, `false` for software standby.
 * @return Error code.
 * @retval k_ra8_ok The requested state was written.
 * @retval k_ra8_err_not_initialized @p dev was not initialized.
 * @retval other Propagated SCCB write error.
 * @pre ::ra8_ov5640_init completed successfully.
 * @pre Sensor XCLK remains running.
 * @post On success the sensor is in the requested streaming state.
 * @note Not thread-safe with respect to the same instance or transport.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ov5640_stream_set(ra8_ov5640_t* dev, bool enabled);

/**
 * @brief Read one sensor register through the bound transport.
 * @details Dispatches one 16-bit-register SCCB read at the selected address.
 * @param[in,out] dev Initialized sensor instance.
 * @param[in] reg Sensor register address.
 * @param[out] out_value Register byte on success.
 * @return Error code.
 * @retval k_ra8_ok The byte was read.
 * @retval k_ra8_err_null_ptr @p dev or @p out_value was `nullptr`.
 * @retval k_ra8_err_not_initialized @p dev was not initialized.
 * @retval other Propagated transport error.
 * @pre ::ra8_ov5640_init completed successfully.
 * @pre The bound transport is idle and the sensor is clocked.
 * @post On success @p out_value contains the register byte.
 * @post The selected device address is unchanged.
 * @note Not thread-safe with respect to the same instance or transport.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ov5640_read_reg(ra8_ov5640_t* dev, uint16_t reg, uint8_t* out_value);

/**
 * @brief Write one sensor register through the bound transport.
 * @details Dispatches one 16-bit-register SCCB write at the selected address.
 * @param[in,out] dev Initialized sensor instance.
 * @param[in] reg Sensor register address.
 * @param[in] value Register byte to write.
 * @return Error code.
 * @retval k_ra8_ok The byte was written.
 * @retval k_ra8_err_null_ptr @p dev was `nullptr`.
 * @retval k_ra8_err_not_initialized @p dev was not initialized.
 * @retval other Propagated transport error.
 * @pre ::ra8_ov5640_init completed successfully.
 * @pre The bound transport is idle and the sensor is clocked.
 * @post On success the transport accepted the complete register write.
 * @post The selected device address is unchanged.
 * @note Not thread-safe with respect to the same instance or transport.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_ov5640_write_reg(ra8_ov5640_t* dev, uint16_t reg, uint8_t value);

#ifdef __cplusplus
}
#endif
