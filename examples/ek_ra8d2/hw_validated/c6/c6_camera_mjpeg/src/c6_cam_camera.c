/**
 * @file c6_cam_camera.c
 * @brief OV5640 hardware-JPEG camera backend for the C6 MJPEG example.
 * @details Binds the reusable sensor and CEU source, captures one bounded JPEG,
 *          and exposes it through the shared C6 camera-server contract.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "c6_camera_server.h"
#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_camera.h"
#include "ra8_camera_codec_passthrough.h"
#include "ra8_camera_source_ceu.h"
#include "ra8_ceu.h"
#include "ra8_ov5640.h"
#include "ra8_time.h"

typedef enum : uint32_t {
  k_sensor_jpeg_buffer_bytes = 512U * 1024U, /**< Maximum gated frame span.    */
  k_sensor_jpeg_poll_ms      = 2U,           /**< CEU completion poll period.  */
  k_sensor_jpeg_poll_tries   = 2000U,        /**< CEU completion poll limit.   */
  k_sensor_jpeg_xclk_hz      = 24000000U,    /**< OV5640 input clock.          */
  k_sensor_jpeg_settle_ms    = 100U,         /**< Sensor routing settle delay. */
} sensor_jpeg_size_t;

typedef enum : uint16_t {
  k_sensor_jpeg_width  = 640U, /**< Encoded sensor JPEG width.  */
  k_sensor_jpeg_height = 480U, /**< Encoded sensor JPEG height. */
} sensor_jpeg_geometry_t;

/** @brief OV5640 clock and stream registers emitted on initialization failure. */
typedef enum : uint16_t {
  k_sensor_reg_system_reset00 = 0x3000U, /**< Internal MCU reset control. */
  k_sensor_reg_system_reset02 = 0x3002U, /**< JPEG reset control.         */
  k_sensor_reg_clock_enable02 = 0x3006U, /**< JPEG clock enables.         */
  k_sensor_reg_system_ctrl0   = 0x3008U, /**< Software stream state.      */
  k_sensor_reg_pll_ctrl0      = 0x3034U, /**< PLL bit-mode control.       */
  k_sensor_reg_pll_ctrl1      = 0x3035U, /**< PLL system divider.         */
  k_sensor_reg_pll_ctrl2      = 0x3036U, /**< PLL multiplier.             */
  k_sensor_reg_pll_ctrl3      = 0x3037U, /**< PLL pre-divider.            */
  k_sensor_reg_system_root    = 0x3103U, /**< System-clock source.        */
  k_sensor_reg_clock_root     = 0x3108U, /**< PCLK/SCLK root dividers.    */
  k_sensor_reg_pclk_divider   = 0x3824U, /**< DVP pixel-clock divider.    */
} sensor_diagnostic_register_t;

typedef enum : uint8_t {
  k_jpeg_marker_prefix = 0xFFU, /**< JPEG marker prefix byte.         */
  k_jpeg_marker_soi    = 0xD8U, /**< JPEG start-of-image marker byte. */
  k_jpeg_marker_eoi    = 0xD9U, /**< JPEG end-of-image marker byte.   */
} jpeg_marker_t;

/** @brief Last SCCB operation retained for initialization diagnostics. */
typedef enum : uint8_t {
  k_sccb_op_none  = 0U, /**< No SCCB transfer has run.  */
  k_sccb_op_read  = 1U, /**< Last transfer was a read.  */
  k_sccb_op_write = 2U, /**< Last transfer was a write. */
} sccb_operation_t;

[[gnu::section(".sdram_data"), gnu::aligned(32)]] static uint8_t s_jpeg[k_sensor_jpeg_buffer_bytes];
static ra8_ov5640_t                                              s_sensor;
static ra8_camera_source_t                                       s_source;
static ra8_camera_source_ceu_state_t                             s_source_state;
static ra8_camera_codec_t                                        s_codec;
static ra8_ov5640_jpeg_status_t                                  s_last_jpeg_status;
static ra8_err_t                                                 s_last_jpeg_status_err;
static ra8_err_t                                                 s_last_sccb_err;
static uint16_t                                                  s_last_sccb_reg;
static sccb_operation_t                                          s_last_sccb_op;
static bool                                                      s_camera_initialized;
static bool                                                      s_sensor_bound;
static bool                                                      s_sensor_streaming;

/**
 * @brief Forward one SCCB read while retaining bounded failure diagnostics.
 * @details Delegates transport ownership to the EK-RA8D2 board adapter.
 * @param[in] ctx Opaque board context forwarded unchanged.
 * @param[in] address Sensor SCCB address.
 * @param[in] reg Sensor register address.
 * @param[out] out_value Destination byte.
 * @return Repository error code from the board adapter.
 * @pre Camera SCCB pins and RIIC are initialized by the board adapter.
 * @pre @p out_value points to writable storage.
 * @post Operation, register, and result are retained in static diagnostics.
 * @post On success @p out_value contains the sensor register byte.
 * @note Not thread-safe with concurrent sensor access.
 * @since 0.1.0
 */
static ra8_err_t sensor_sccb_read(void* ctx, uint8_t address, uint16_t reg, uint8_t* out_value)
{
  const ra8_err_t err = ra8_board_camera_sccb_read_reg(ctx, address, reg, out_value);
  s_last_sccb_op      = k_sccb_op_read;
  s_last_sccb_reg     = reg;
  s_last_sccb_err     = err;
  return err;
}

/**
 * @brief Forward one SCCB write while retaining bounded failure diagnostics.
 * @details Delegates transport ownership to the EK-RA8D2 board adapter.
 * @param[in] ctx Opaque board context forwarded unchanged.
 * @param[in] address Sensor SCCB address.
 * @param[in] reg Sensor register address.
 * @param[in] value Register byte to write.
 * @return Repository error code from the board adapter.
 * @pre Camera SCCB pins and RIIC are initialized by the board adapter.
 * @pre No other context accesses the sensor concurrently.
 * @post Operation, register, and result are retained in static diagnostics.
 * @post On success the sensor accepted @p value for @p reg.
 * @note Not thread-safe with concurrent sensor access.
 * @since 0.1.0
 */
static ra8_err_t sensor_sccb_write(void* ctx, uint8_t address, uint16_t reg, uint8_t value)
{
  const ra8_err_t err = ra8_board_camera_sccb_write_reg(ctx, address, reg, value);
  s_last_sccb_op      = k_sccb_op_write;
  s_last_sccb_reg     = reg;
  s_last_sccb_err     = err;
  return err;
}

/**
 * @brief Bind the hardware-JPEG sensor instance to board SCCB callbacks.
 * @details Constructs a transport over the EK-RA8D2 camera bus and delay service.
 * @return Repository error code.
 * @retval k_ra8_ok The sensor instance was initialized.
 * @retval k_ra8_err_null_ptr A required transport dependency was absent.
 * @pre Board camera support is linked and its I2C channel can be initialized.
 * @pre No capture concurrently uses `s_sensor`.
 * @post On success, `s_sensor` is initialized at the primary SCCB address.
 * @post No sensor register transfer occurs during binding.
 * @note Probing is performed later by camera initialization.
 * @since 0.1.0
 */
static ra8_err_t sensor_bind(void)
{
  const ra8_ov5640_bus_t bus = {
    .read_reg  = sensor_sccb_read,
    .write_reg = sensor_sccb_write,
    .delay_ms  = ra8_board_camera_delay_ms,
    .ctx       = nullptr,
  };
  return ra8_ov5640_init(&s_sensor, &bus);
}

/**
 * @brief Build the CEU configuration for bounded hardware JPEG capture.
 * @details Selects data-enable capture into a fixed SDRAM buffer with VGA metadata.
 * @return Generic CEU source configuration.
 * @retval ra8_camera_source_ceu_cfg_t Complete hardware-JPEG source configuration.
 * @pre The compile-time buffer capacity fits in the CEU image-area field.
 * @pre Geometry constants match the OV5640 VGA JPEG mode.
 * @post Returned configuration references no temporary storage.
 * @post Static camera state remains unchanged.
 * @note JPEG payload length is determined after capture by marker scanning.
 * @since 0.1.0
 */
static ra8_camera_source_ceu_cfg_t sensor_ceu_config(void)
{
  const ra8_ceu_config_t ceu = {
    .width_px        = (uint16_t)k_sensor_jpeg_width,
    .height_px       = (uint16_t)k_sensor_jpeg_height,
    .x_capture_px    = (uint16_t)k_sensor_jpeg_width,
    .y_capture_lines = (uint16_t)k_sensor_jpeg_height,
    .bytes_per_pixel = 1U,
    .dst_stride      = (uint16_t)k_sensor_jpeg_width,
    .capture_format  = k_ra8_ceu_fmt_data_enable,
    .capture_mode    = k_ra8_ceu_capture_single,
    .data_bus        = k_ra8_ceu_bus_8_bit,
    .hsync_polarity  = k_ra8_ceu_pol_high_active,
    .vsync_polarity  = k_ra8_ceu_pol_high_active,
    .field_polarity  = k_ra8_ceu_pol_high_active,
    .input_order     = k_ra8_ceu_input_cb0_y0_cr0_y1,
    .output_format   = k_ra8_ceu_output_ycbcr_422,
    .byte_swap       = {.swap_8_bit = true, .swap_16_bit = true, .swap_32_bit = true},
    .burst_mode      = k_ra8_ceu_burst_256,
    .first_field     = k_ra8_ceu_field_immediate,
    .edge            = {k_ra8_ceu_edge_rising,
                        k_ra8_ceu_edge_rising,
                        k_ra8_ceu_edge_rising,
                        k_ra8_ceu_edge_rising},
    .image_area_size = (uint32_t)k_sensor_jpeg_buffer_bytes,
  };
  return (ra8_camera_source_ceu_cfg_t){
    .ceu = ceu,
    .output =
      {
        .frame_bytes_max = (uint32_t)k_sensor_jpeg_buffer_bytes,
        .stride_bytes    = 0U,
        .width           = (uint16_t)k_sensor_jpeg_width,
        .height          = (uint16_t)k_sensor_jpeg_height,
        .format          = k_ra8_camera_format_jpeg,
      },
    .poll_interval_ms = (uint32_t)k_sensor_jpeg_poll_ms,
    .poll_attempts    = (uint32_t)k_sensor_jpeg_poll_tries,
  };
}

/**
 * @brief Reset every camera diagnostic and lifecycle flag to its cold state.
 * @details Runs first in `c6_cam_camera_init` so a re-init after a partial
 *          failure never reports stale SCCB or streaming state.
 * @pre No capture or diagnostic report is concurrently in progress.
 * @pre Static camera state was previously left in a defined state (init or
 *      a prior failed init).
 * @post Every static diagnostic and lifecycle flag holds its cold-start value.
 * @post No hardware register or bus transfer is touched.
 * @note Not thread-safe with concurrent capture or diagnostic reporting.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_reset_camera_diagnostics(void)
{
  s_camera_initialized = false;
  s_sensor_bound       = false;
  s_last_sccb_op       = k_sccb_op_none;
  s_last_sccb_reg      = 0U;
  s_last_sccb_err      = k_ra8_ok;
  s_sensor_streaming   = false;
}

/**
 * @brief Bring the OV5640 sensor up to streaming hardware-JPEG mode.
 * @details Sequences clock start, board reset, SCCB bind, chip-id probe, and
 *          VGA-JPEG mode configuration, stopping at the first failure.
 * @return Repository error code from the first failed step, or success.
 * @retval k_ra8_ok The sensor is bound, probed, and streaming VGA JPEG.
 * @retval k_ra8_err_hw_init_failed The probed chip id did not match OV5640.
 * @retval other The board or sensor step that failed reported this code.
 * @pre The board camera pins and clock are not already owned by another user.
 * @pre `s_sensor` is not concurrently accessed by another init or capture.
 * @post On success `s_sensor_bound` and `s_sensor_streaming` are both true.
 * @post On any failure the function returns before later steps run.
 * @note Not thread-safe with concurrent sensor access.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_bring_up_sensor(void)
{
  ra8_err_t err = ra8_board_camera_xclk_start((uint32_t)k_sensor_jpeg_xclk_hz);
  if (err == k_ra8_ok) {
    err = ra8_board_camera_select_parallel();
  }
  if (err == k_ra8_ok) {
    ra8_delay_ms((uint32_t)k_sensor_jpeg_settle_ms);
    err = ra8_board_camera_reset();
  }
  if (err == k_ra8_ok) {
    err = sensor_bind();
    if (err == k_ra8_ok) {
      s_sensor_bound = true;
    }
  }
  uint16_t chip_id = 0U;
  if (err == k_ra8_ok) {
    err = ra8_ov5640_probe(&s_sensor, &chip_id);
  }
  if ((err == k_ra8_ok) && (chip_id != (uint16_t)k_ra8_ov5640_chip_id)) {
    err = k_ra8_err_hw_init_failed;
  }
  if (err == k_ra8_ok) {
    err                = ra8_ov5640_configure(&s_sensor, k_ra8_ov5640_mode_vga_jpeg);
    s_sensor_streaming = (err == k_ra8_ok);
  }
  return err;
}

/**
 * @brief Route the parallel capture pins and bind the CEU source and codec.
 * @details Runs after the sensor is streaming: routes the DVP pins away from
 *          their bring-up owner, then binds the CEU capture source and the
 *          passthrough JPEG codec.
 * @return Repository error code from the first failed step, or success.
 * @retval k_ra8_ok The capture pipeline is bound and ready to capture.
 * @retval other The pin-routing, CEU, or codec step that failed reported this.
 * @pre `internal_bring_up_sensor` has already completed successfully.
 * @pre `s_source`, `s_source_state`, and `s_codec` are not concurrently used.
 * @post On success `s_source` and `s_codec` are initialized for capture.
 * @post On any failure the function returns before later steps run.
 * @note Not thread-safe with concurrent capture or re-initialization.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_bring_up_capture_pipeline(void)
{
  ra8_err_t err = ra8_board_camera_route_parallel_pins();
  if (err == k_ra8_ok) {
    const ra8_camera_source_ceu_cfg_t cfg = sensor_ceu_config();
    err = ra8_camera_source_ceu_init(&s_source, &s_source_state, &cfg);
  }
  if (err == k_ra8_ok) {
    err = ra8_camera_codec_passthrough_init(&s_codec);
  }
  return err;
}

ra8_err_t c6_cam_camera_init(void)
{
  internal_reset_camera_diagnostics();
  ra8_err_t err = internal_bring_up_sensor();
  if (err == k_ra8_ok) {
    err = internal_bring_up_capture_pipeline();
  }
  s_camera_initialized = (err == k_ra8_ok);
  return err;
}

/**
 * @brief Locate the first complete JPEG payload in a captured span.
 * @details Validates the SOI prefix and returns the length through the first EOI marker.
 * @param[in] jpeg Captured sensor byte span.
 * @param[in] bytes Available capture bytes.
 * @return Trimmed JPEG payload length.
 * @retval 0 The span lacks a valid SOI or complete EOI.
 * @pre `jpeg` addresses at least `bytes` readable bytes.
 * @pre `bytes` is bounded by the static JPEG capture capacity.
 * @post The input span remains unchanged.
 * @post A nonzero result includes both EOI marker bytes.
 * @note Bytes after the first EOI are intentionally excluded.
 * @since 0.1.0
 */
static uint32_t jpeg_trim_to_eoi(const uint8_t* jpeg, uint32_t bytes)
{
  if ((bytes < 4U) || (jpeg[0] != (uint8_t)k_jpeg_marker_prefix) ||
      (jpeg[1] != (uint8_t)k_jpeg_marker_soi)) {
    return 0U;
  }
  for (uint32_t i = 1U; i < bytes; i += 1U) {
    if ((jpeg[i - 1U] == (uint8_t)k_jpeg_marker_prefix) &&
        (jpeg[i] == (uint8_t)k_jpeg_marker_eoi)) {
      return i + 1U;
    }
  }
  return 0U;
}

ra8_err_t c6_cam_camera_capture_jpeg(const uint8_t**        out_jpeg,
                                     uint32_t*              out_bytes,
                                     c6_cam_frame_timing_t* out_timing)
{
  if ((out_jpeg == nullptr) || (out_bytes == nullptr) || (out_timing == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out_jpeg                        = nullptr;
  *out_bytes                       = 0U;
  *out_timing                      = (c6_cam_frame_timing_t){};
  const uint32_t start             = ra8_time_ms();
  out_timing->timestamp_ms         = start;
  const ra8_camera_buffer_t buffer = {
    .data     = s_jpeg,
    .capacity = (uint32_t)sizeof(s_jpeg),
  };
  ra8_err_t stream_err = k_ra8_ok;
  if (!s_sensor_streaming) {
    stream_err         = ra8_ov5640_stream_set(&s_sensor, true);
    s_sensor_streaming = (stream_err == k_ra8_ok);
  }
  if (stream_err != k_ra8_ok) {
    return stream_err;
  }
  ra8_camera_frame_t frame = {};
  const ra8_err_t    err   = ra8_camera_source_capture(&s_source, &buffer, &frame);
  out_timing->capture_ms   = ra8_time_ms() - start;
  s_last_jpeg_status_err   = ra8_ov5640_jpeg_status_get(&s_sensor, &s_last_jpeg_status);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint32_t jpeg_bytes = jpeg_trim_to_eoi(frame.data, frame.bytes);
  if (jpeg_bytes == 0U) {
    return k_ra8_err_invalid_size;
  }
  frame.bytes                         = jpeg_bytes;
  const ra8_camera_buffer_t no_output = {};
  ra8_camera_frame_t        jpeg      = {};
  const ra8_err_t codec_err = ra8_camera_codec_encode(&s_codec, &frame, &no_output, &jpeg);
  if (codec_err != k_ra8_ok) {
    return codec_err;
  }
  *out_jpeg  = jpeg.data;
  *out_bytes = jpeg.bytes;
  return k_ra8_ok;
}

const char* c6_cam_camera_description(void)
{
  return "frame=640x480 sensor-JPEG CEU data-enable backend";
}

/**
 * @brief Emit one named OV5640 register value when it remains readable.
 * @details Keeps failed JPEG bring-up diagnostics bounded and heap-free.
 * @param[in] label Console label including its leading separator.
 * @param[in] reg Sensor register address.
 * @pre The diagnostic console is initialized.
 * @pre The OV5640 transport was bound successfully.
 * @post A successful read emits the register value in decimal.
 * @post A failed read emits the repository error string.
 * @note This helper intentionally updates the retained SCCB operation fields.
 * @since 0.1.0
 */
static void sensor_report_register(const char* label, uint16_t reg)
{
  uint8_t         value = 0U;
  const ra8_err_t err   = ra8_ov5640_read_reg(&s_sensor, reg, &value);
  c6_cam_puts(label);
  if (err == k_ra8_ok) {
    c6_cam_put_u32(value);
  } else {
    c6_cam_puts(ra8_err_to_str(err));
  }
}

/**
 * @brief Report the most recent SCCB transfer direction, register, and result.
 * @details Always emitted regardless of camera or sensor bring-up state,
 *          since a bring-up failure is exactly when this is most needed.
 * @pre The diagnostic console is initialized.
 * @pre `s_last_sccb_op`, `s_last_sccb_reg`, and `s_last_sccb_err` describe
 *      one complete SCCB transfer attempt (or the cold-start "none" state).
 * @post One `sccb_op=`/`sccb_reg=`/`sccb_err=` triple is emitted.
 * @post No static diagnostic state is modified.
 * @note Not thread-safe with a concurrent SCCB transfer.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_report_sccb_state(void)
{
  c6_cam_puts(" sccb_op=");
  if (s_last_sccb_op == k_sccb_op_read) {
    c6_cam_puts("read");
  } else if (s_last_sccb_op == k_sccb_op_write) {
    c6_cam_puts("write");
  } else {
    c6_cam_puts("none");
  }
  c6_cam_puts(" sccb_reg=");
  c6_cam_put_u32(s_last_sccb_reg);
  c6_cam_puts(" sccb_err=");
  c6_cam_puts(ra8_err_to_str(s_last_sccb_err));
}

/**
 * @brief Report the OV5640 clock and stream registers when the sensor bound.
 * @details No-op when `sensor_bind` never succeeded, since the registers
 *          would not be readable.
 * @pre The diagnostic console is initialized.
 * @pre `s_sensor` was bound by a prior `internal_bring_up_sensor` call.
 * @post When `s_sensor_bound`, every diagnostic register in
 *       ::sensor_diagnostic_register_t is emitted once.
 * @post No static diagnostic state is modified.
 * @note Not thread-safe with concurrent sensor access.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_report_sensor_registers(void)
{
  if (!s_sensor_bound) {
    return;
  }
  sensor_report_register(" r3000=", (uint16_t)k_sensor_reg_system_reset00);
  sensor_report_register(" r3002=", (uint16_t)k_sensor_reg_system_reset02);
  sensor_report_register(" r3006=", (uint16_t)k_sensor_reg_clock_enable02);
  sensor_report_register(" r3008=", (uint16_t)k_sensor_reg_system_ctrl0);
  sensor_report_register(" r3034=", (uint16_t)k_sensor_reg_pll_ctrl0);
  sensor_report_register(" r3035=", (uint16_t)k_sensor_reg_pll_ctrl1);
  sensor_report_register(" r3036=", (uint16_t)k_sensor_reg_pll_ctrl2);
  sensor_report_register(" r3037=", (uint16_t)k_sensor_reg_pll_ctrl3);
  sensor_report_register(" r3103=", (uint16_t)k_sensor_reg_system_root);
  sensor_report_register(" r3108=", (uint16_t)k_sensor_reg_clock_root);
  sensor_report_register(" r3824=", (uint16_t)k_sensor_reg_pclk_divider);
}

/**
 * @brief Report the CEU event mask observed by the most recent capture.
 * @details Silent when the CEU source cannot report an event mask, e.g.
 *          before any capture has run.
 * @pre The diagnostic console is initialized.
 * @pre `s_source_state` was initialized by `c6_cam_camera_init`.
 * @post On success one `ceu_events=` field is emitted.
 * @post No static diagnostic state is modified.
 * @note Not thread-safe with a concurrent capture.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_report_capture_events(void)
{
  uint32_t events = 0U;
  if (ra8_camera_source_ceu_get_last_events(&s_source_state, &events) == k_ra8_ok) {
    c6_cam_puts(" ceu_events=");
    c6_cam_put_u32(events);
  }
}

/**
 * @brief Report the OV5640 hardware-JPEG status block from the last capture.
 * @details No-op unless the last `ra8_ov5640_jpeg_status_get` succeeded,
 *          since the status fields are undefined otherwise.
 * @pre The diagnostic console is initialized.
 * @pre `s_last_jpeg_status` was populated by the most recent capture attempt.
 * @post When `s_last_jpeg_status_err == k_ra8_ok`, every field of
 *       `s_last_jpeg_status` is emitted once.
 * @post No static diagnostic state is modified.
 * @note Not thread-safe with a concurrent capture.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_report_jpeg_status_detail(void)
{
  if (s_last_jpeg_status_err != k_ra8_ok) {
    return;
  }
  c6_cam_puts(" sensor_jpeg_bytes=");
  c6_cam_put_u32(s_last_jpeg_status.encoded_bytes);
  c6_cam_puts(" overflow=");
  c6_cam_put_u32(s_last_jpeg_status.fifo_overflow ? 1U : 0U);
  c6_cam_puts(" yuv422=");
  c6_cam_put_u32(s_last_jpeg_status.input_is_yuv422 ? 1U : 0U);
  c6_cam_puts(" header=");
  c6_cam_put_u32(s_last_jpeg_status.header_output ? 1U : 0U);
  c6_cam_puts(" jpeg_enabled=");
  c6_cam_put_u32(s_last_jpeg_status.compression_enabled ? 1U : 0U);
  c6_cam_puts(" width=");
  c6_cam_put_u32(s_last_jpeg_status.compression_width);
  c6_cam_puts(" height=");
  c6_cam_put_u32(s_last_jpeg_status.compression_height);
  c6_cam_puts(" ctrl01=");
  c6_cam_put_u32(s_last_jpeg_status.jpeg_ctrl01);
  c6_cam_puts(" vfifo00=");
  c6_cam_put_u32(s_last_jpeg_status.vfifo_ctrl00);
  c6_cam_puts(" href_min=");
  c6_cam_put_u32(s_last_jpeg_status.href_minimum_blanking);
}

void c6_cam_camera_report_last_error(void)
{
  internal_report_sccb_state();
  internal_report_sensor_registers();
  if (!s_camera_initialized) {
    return;
  }
  internal_report_capture_events();
  c6_cam_puts(" sensor_status=");
  c6_cam_puts(ra8_err_to_str(s_last_jpeg_status_err));
  internal_report_jpeg_status_detail();
}
