/**
 * @file test_ra8_camera.c
 * @brief Host tests for the generic camera source/codec facade and backends.
 *
 * @details Exercises frame validation, fixed-memory capture, JPEG passthrough,
 * software JPEG encoding, and output through the RAM `ra8_io_stream` sink. No
 * camera peripheral, sensor, board layer, or filesystem participates.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_camera.h"
#include "ra8_camera_codec_jpeg_sw.h"
#include "ra8_camera_codec_passthrough.h"
#include "ra8_camera_source_ceu.h"
#include "ra8_camera_source_memory.h"
#include "ra8_camera_stream.h"
#include "ra8_ceu_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_ram.h"
#include "ra8_jpeg_sw.h"
#include "unity_minimal.h"

/* White-box the CEU wait helper while keeping its public symbols distinct from
 * the production copy linked through ra8_core_hal. */
// NOLINTBEGIN(readability-identifier-naming)
/** @brief Rename the CEU event accessor in the white-box coverage copy. */
#define ra8_camera_source_ceu_get_last_events ra8_camera_source_ceu_get_last_events_cov
/** @brief Rename the CEU initializer in the white-box coverage copy. */
#define ra8_camera_source_ceu_init ra8_camera_source_ceu_init_cov
// NOLINTEND(readability-identifier-naming)
#include "ra8_camera_source_ceu.c" // NOLINT(bugprone-suspicious-include) -- white-box copy

/** @brief Fixture dimensions and storage bounds. */
typedef enum : uint32_t {
  k_t_width            = 16U,            /**< RGB fixture width.                */
  k_t_height           = 16U,            /**< RGB fixture height.               */
  k_t_output_width     = 8U,             /**< Software-JPEG output width.       */
  k_t_output_height    = 8U,             /**< Software-JPEG output height.      */
  k_t_output_rgb_bytes = 8U * 8U * 3U,   /**< Software-JPEG RGB workspace.      */
  k_t_rgb_bytes        = 16U * 16U * 3U, /**< Packed RGB fixture bytes.         */
  k_t_uyvy_bytes       = 16U * 16U * 2U, /**< Packed UYVY fixture bytes.        */
  k_t_jpeg_cap         = 8192U,          /**< Ample software-JPEG output bound. */
  k_t_stream_tiny      = 8U,             /**< Sink capacity for partial writes. */
  k_t_byte_mod         = 251U,           /**< Non-power-of-two fixture pattern. */
  k_t_ceu_events       = 0x12000001U,    /**< Representative retained CEU bits. */
} t_camera_const_t;

/**
 * @brief Fill packed RGB pixels with a deterministic non-flat pattern.
 * @details Generates repeatable channel variation for software JPEG tests.
 * @param[out] pixels RGB fixture storage.
 * @pre `pixels` addresses at least `k_t_rgb_bytes` writable bytes.
 * @pre No concurrent reader observes the fixture during initialization.
 * @post Every fixture byte is initialized.
 * @post No storage beyond the fixture span is modified.
 * @note The modulus avoids a short power-of-two pattern.
 * @since 0.1.0
 */
static void fill_rgb(uint8_t* pixels)
{
  for (uint32_t i = 0U; i < (uint32_t)k_t_rgb_bytes; i += 1U) {
    pixels[i] = (uint8_t)((i * 17U) % (uint32_t)k_t_byte_mod);
  }
}

/**
 * @brief Build a valid packed RGB frame over the supplied bytes.
 * @details Wraps caller-owned fixture storage with deterministic geometry.
 * @param[in] pixels Packed RGB fixture storage.
 * @return Camera frame descriptor.
 * @retval ra8_camera_frame_t Descriptor referencing `pixels`.
 * @pre `pixels` addresses at least `k_t_rgb_bytes` readable bytes.
 * @pre The storage outlives uses of the returned descriptor.
 * @post Pixel storage remains unchanged.
 * @post Returned geometry describes the complete fixture.
 * @note The returned frame does not own its pixels.
 * @since 0.1.0
 */
static ra8_camera_frame_t rgb_frame(const uint8_t* pixels)
{
  return (ra8_camera_frame_t){
    .data         = pixels,
    .bytes        = (uint32_t)k_t_rgb_bytes,
    .stride_bytes = (uint32_t)k_t_width * 3U,
    .width        = (uint16_t)k_t_width,
    .height       = (uint16_t)k_t_height,
    .format       = k_ra8_camera_format_rgb888,
  };
}

/**
 * @brief Build a valid software-JPEG configuration over caller workspace.
 * @details Selects fixed output geometry and the repository default quality.
 * @param[out] workspace RGB conversion workspace retained by the caller.
 * @param[in] capacity Available workspace bytes.
 * @return Software-JPEG configuration.
 * @retval ra8_camera_codec_jpeg_sw_cfg_t Configuration referencing `workspace`.
 * @pre `workspace` is null or addresses `capacity` writable bytes.
 * @pre The workspace outlives the configured codec.
 * @post Workspace bytes remain unchanged.
 * @post Returned configuration records the supplied capacity exactly.
 * @note Initialization validates whether the capacity is sufficient.
 * @since 0.1.0
 */
static ra8_camera_codec_jpeg_sw_cfg_t jpeg_cfg(uint8_t* workspace, uint32_t capacity)
{
  return (ra8_camera_codec_jpeg_sw_cfg_t){
    .rgb_workspace          = workspace,
    .rgb_workspace_capacity = capacity,
    .output_width           = (uint16_t)k_t_output_width,
    .output_height          = (uint16_t)k_t_output_height,
    .quality                = (uint8_t)k_ra8_jpeg_sw_quality_default,
  };
}

/**
 * @brief Verify supported camera-frame layouts and geometry guards.
 * @details Exercises RGB, UYVY, JPEG, null, stride, dimension, and format paths.
 * @par MC/DC:
 * The valid RGB fixture is the all-false rejection baseline. Each malformed
 * frame vector flips one layout, geometry, or pointer guard independently.
 * @pre Unity test accounting is initialized.
 * @pre Stack storage is available for the largest fixture.
 * @post Each layout records the expected validation result.
 * @post No persistent fixture state remains.
 * @note This test performs no hardware access.
 * @since 0.1.0
 */
static void test_frame_validation(void)
{
  TEST_BEGIN("camera frame validation");
  uint8_t            rgb[(size_t)k_t_rgb_bytes] = {};
  ra8_camera_frame_t frame                      = rgb_frame(rgb);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_frame_validate(&frame));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_frame_validate(nullptr));

  frame.data = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_frame_validate(&frame));
  frame       = rgb_frame(rgb);
  frame.width = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_camera_frame_validate(&frame));
  frame = rgb_frame(rgb);
  frame.stride_bytes -= 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_camera_frame_validate(&frame));

  frame = (ra8_camera_frame_t){
    .data         = rgb,
    .bytes        = (uint32_t)k_t_uyvy_bytes,
    .stride_bytes = (uint32_t)k_t_width * 2U,
    .width        = (uint16_t)k_t_width,
    .height       = (uint16_t)k_t_height,
    .format       = k_ra8_camera_format_uyvy422,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_frame_validate(&frame));
  frame.width = 15U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_camera_frame_validate(&frame));

  frame = (ra8_camera_frame_t){.data   = rgb,
                               .bytes  = 2U,
                               .width  = 1U,
                               .height = 1U,
                               .format = k_ra8_camera_format_jpeg};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_frame_validate(&frame));
  frame.stride_bytes = 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_camera_frame_validate(&frame));
  frame.format = (ra8_camera_format_t)99U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_camera_frame_validate(&frame));
  TEST_END("camera frame validation");
}

/**
 * @brief Verify fixed-memory source metadata and capture.
 * @details Checks exact replay and destination-capacity rejection.
 * @par MC/DC:
 * The full-capacity vector keeps both rejection conditions false; the short
 * destination vector independently makes the capacity condition true.
 * @pre Unity test accounting is initialized.
 * @pre Source and destination fixtures are independently writable.
 * @post Successful capture matches the source byte for byte.
 * @post Undersized capture clears the output frame.
 * @note The backend retains no destination pointer.
 * @since 0.1.0
 */
static void test_memory_source(void)
{
  TEST_BEGIN("camera fixed memory source");
  uint8_t source_bytes[(size_t)k_t_rgb_bytes]  = {};
  uint8_t capture_bytes[(size_t)k_t_rgb_bytes] = {};
  fill_rgb(source_bytes);
  const ra8_camera_frame_t         fixed  = rgb_frame(source_bytes);
  ra8_camera_source_t              source = {};
  ra8_camera_source_memory_state_t state  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_memory_init(&source, &state, &fixed));

  ra8_camera_source_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_get_info(&source, &info));
  TEST_ASSERT_EQ(k_t_rgb_bytes, info.frame_bytes_max);
  TEST_ASSERT_EQ(k_t_width, info.width);
  TEST_ASSERT_EQ(k_ra8_camera_format_rgb888, info.format);

  const ra8_camera_buffer_t capture = {capture_bytes, (uint32_t)sizeof capture_bytes};
  ra8_camera_frame_t        out     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_capture(&source, &capture, &out));
  TEST_ASSERT(out.data == capture_bytes);
  TEST_ASSERT_EQ(k_t_rgb_bytes, out.bytes);
  TEST_ASSERT(memcmp(source_bytes, capture_bytes, sizeof source_bytes) == 0);

  const ra8_camera_buffer_t tiny = {capture_bytes, (uint32_t)sizeof capture_bytes - 1U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_camera_source_capture(&source, &tiny, &out));
  TEST_ASSERT_NULL(out.data);
  TEST_END("camera fixed memory source");
}

/**
 * @brief Verify facade rejection of unbound and null objects.
 * @details Exercises source and codec guards before backend dispatch.
 * @par MC/DC:
 * Each vector removes one required handle, interface, buffer, or output pointer
 * while the other facade dependencies remain valid.
 * @pre Unity test accounting is initialized.
 * @pre Local facade handles begin zero-initialized.
 * @post Each invalid call records its documented error.
 * @post No backend callback is invoked.
 * @note Small local buffers are sufficient because capture never starts.
 * @since 0.1.0
 */
static void test_facade_guards(void)
{
  TEST_BEGIN("camera facade guards");
  uint8_t                   bytes[3] = {};
  const ra8_camera_frame_t  frame    = {.data         = bytes,
                                        .bytes        = 3U,
                                        .stride_bytes = 3U,
                                        .width        = 1U,
                                        .height       = 1U,
                                        .format       = k_ra8_camera_format_rgb888};
  const ra8_camera_buffer_t buffer   = {bytes, (uint32_t)sizeof bytes};
  ra8_camera_source_t       source   = {};
  ra8_camera_codec_t        codec    = {};
  ra8_camera_source_info_t  info     = {};
  ra8_camera_frame_t        out      = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_camera_source_get_info(&source, &info));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_camera_source_capture(&source, &buffer, &out));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_camera_codec_encode(&codec, &frame, &buffer, &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_source_get_info(&source, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_source_capture(&source, nullptr, &out));
  TEST_END("camera facade guards");
}

/**
 * @brief Verify caller-owned CEU diagnostic event retention.
 * @details Covers null pointers, uninitialized state, and successful event retrieval.
 * @par MC/DC:
 * The initialized state with a valid output pointer is the accepted baseline;
 * null output and uninitialized state independently flip the two guard inputs.
 * @pre Unity test accounting is initialized.
 * @pre Local CEU state is writable by the test.
 * @post Successful retrieval equals the retained event word.
 * @post Null and uninitialized calls record their expected errors.
 * @note The CEU peripheral is not accessed.
 * @since 0.1.0
 */
static void test_ceu_diagnostic_state(void)
{
  TEST_BEGIN("camera CEU diagnostic state");
  uint32_t events = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_source_ceu_get_last_events(nullptr, &events));
  ra8_camera_source_ceu_state_t state = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_source_ceu_get_last_events(&state, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_camera_source_ceu_get_last_events(&state, &events));
  TEST_ASSERT_EQ(0U, events);
  state.initialized = true;
  state.last_events = (uint32_t)k_t_ceu_events;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_ceu_get_last_events(&state, &events));
  TEST_ASSERT_EQ(state.last_events, events);
  TEST_END("camera CEU diagnostic state");
}

/**
 * @brief Reject a capture when VBP and CPE are reported together.
 * @details Reproduces the HUM-documented rare case where a capture-end flag is
 *          asserted despite an invalid vertical blanking interval.
 * @par MC/DC:
 * This VBP+CPE vector makes the fatal-input condition true while completion is
 * also true; the diagnostic-state case supplies the nonfatal event baseline.
 * @pre Host CEU MMIO backing is available.
 * @pre The white-box CEU source copy is linked into this test.
 * @post The wait helper reports a hardware error instead of a completed frame.
 * @post Both event bits remain available in caller-owned diagnostic state.
 * @note RA8D2 HUM Ch 60.2.22 requires CPE to be ignored when VBP is asserted.
 * @since 0.1.0
 */
static void test_ceu_vbp_overrides_capture_end(void)
{
  TEST_BEGIN("camera CEU VBP overrides CPE");
  ra8_fake_mmap_reset();
  const uint32_t events = (uint32_t)k_ra8_ceu_evt_vd_error | (uint32_t)k_ra8_ceu_evt_cpe;
  *ra8_ceu_reg32(k_ra8_ceu_off_cetcr) = events;
  *ra8_ceu_reg32(k_ra8_ceu_off_cdssr) = 64U;

  ra8_camera_source_ceu_state_t state = {
    .info             = {.frame_bytes_max = 128U},
    .capture_format   = k_ra8_ceu_fmt_data_enable,
    .poll_interval_ms = 1U,
    .poll_attempts    = 1U,
    .initialized      = true,
  };
  uint32_t captured_bytes = 0U;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_ceu_wait_for_frame(&state, &captured_bytes));
  TEST_ASSERT_EQ(0U, captured_bytes);
  TEST_ASSERT_EQ(events, state.last_events);
  TEST_END("camera CEU VBP overrides CPE");
}

/**
 * @brief Verify JPEG passthrough aliasing and format rejection.
 * @details Checks zero-copy JPEG output and unsupported raw RGB input.
 * @par MC/DC:
 * The JPEG vector keeps format and frame guards accepted; the RGB vector flips
 * only the supported-format condition and must clear the output descriptor.
 * @pre Unity test accounting is initialized.
 * @pre Input byte arrays remain valid through each encode call.
 * @post JPEG output aliases the exact input span.
 * @post Rejected raw input clears the output descriptor.
 * @note The passthrough codec requires no output storage.
 * @since 0.1.0
 */
static void test_jpeg_passthrough(void)
{
  TEST_BEGIN("camera JPEG passthrough");
  uint8_t                   jpeg[] = {0xFFU, 0xD8U, 0xFFU, 0xD9U};
  const ra8_camera_frame_t  input  = {.data   = jpeg,
                                      .bytes  = (uint32_t)sizeof jpeg,
                                      .width  = 2U,
                                      .height = 3U,
                                      .format = k_ra8_camera_format_jpeg};
  const ra8_camera_buffer_t unused = {};
  ra8_camera_codec_t        codec  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_codec_passthrough_init(&codec));
  ra8_camera_frame_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_codec_encode(&codec, &input, &unused, &out));
  TEST_ASSERT(out.data == input.data);
  TEST_ASSERT_EQ(input.bytes, out.bytes);

  uint8_t                  rgb[3] = {};
  const ra8_camera_frame_t raw    = {.data         = rgb,
                                     .bytes        = 3U,
                                     .stride_bytes = 3U,
                                     .width        = 1U,
                                     .height       = 1U,
                                     .format       = k_ra8_camera_format_rgb888};
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_camera_codec_encode(&codec, &raw, &unused, &out));
  TEST_ASSERT_NULL(out.data);
  TEST_END("camera JPEG passthrough");
}

/**
 * @brief Verify software JPEG encoding for RGB and UYVY input.
 * @details Confirms output markers, configured dimensions, and small-buffer failure.
 * @par MC/DC:
 * The full output buffer is the accepted baseline; the undersized buffer vector
 * independently flips capacity while preserving valid image geometry.
 * @pre Unity test accounting is initialized.
 * @pre Workspace and JPEG output buffers meet configured capacities.
 * @post Valid inputs produce parseable JPEG output at target dimensions.
 * @post An undersized output clears the encoded descriptor.
 * @note Encoding is deterministic and host-only.
 * @since 0.1.0
 */
static void test_jpeg_sw_backend(void)
{
  TEST_BEGIN("camera software JPEG backend");
  uint8_t rgb[(size_t)k_t_rgb_bytes]              = {};
  uint8_t workspace[(size_t)k_t_output_rgb_bytes] = {};
  uint8_t jpeg[(size_t)k_t_jpeg_cap]              = {};
  fill_rgb(rgb);
  const ra8_camera_frame_t             input  = rgb_frame(rgb);
  const ra8_camera_buffer_t            output = {jpeg, (uint32_t)sizeof jpeg};
  ra8_camera_codec_t                   codec  = {};
  ra8_camera_codec_jpeg_sw_state_t     state  = {};
  const ra8_camera_codec_jpeg_sw_cfg_t cfg    = jpeg_cfg(workspace, sizeof workspace);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));
  ra8_camera_frame_t encoded = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_codec_encode(&codec, &input, &output, &encoded));
  TEST_ASSERT(encoded.data == jpeg);
  TEST_ASSERT_EQ(k_ra8_camera_format_jpeg, encoded.format);
  TEST_ASSERT(encoded.bytes > 4U);
  TEST_ASSERT_EQ(0xFF, jpeg[0]);
  TEST_ASSERT_EQ(0xD8, jpeg[1]);
  uint16_t width  = 0U;
  uint16_t height = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jpeg_sw_get_dimensions(jpeg, encoded.bytes, &width, &height));
  TEST_ASSERT_EQ(k_t_output_width, width);
  TEST_ASSERT_EQ(k_t_output_height, height);

  uint8_t uyvy[(size_t)k_t_uyvy_bytes] = {};
  for (uint32_t i = 0U; i < (uint32_t)sizeof uyvy; i += 4U) {
    uyvy[i]      = 128U;
    uyvy[i + 1U] = (uint8_t)(32U + (i % 192U));
    uyvy[i + 2U] = 128U;
    uyvy[i + 3U] = (uint8_t)(48U + (i % 176U));
  }
  const ra8_camera_frame_t uyvy_input = {.data         = uyvy,
                                         .bytes        = (uint32_t)sizeof uyvy,
                                         .stride_bytes = (uint32_t)k_t_width * 2U,
                                         .width        = (uint16_t)k_t_width,
                                         .height       = (uint16_t)k_t_height,
                                         .format       = k_ra8_camera_format_uyvy422};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_codec_encode(&codec, &uyvy_input, &output, &encoded));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_jpeg_sw_get_dimensions(jpeg, encoded.bytes, &width, &height));
  TEST_ASSERT_EQ(k_t_output_width, width);
  TEST_ASSERT_EQ(k_t_output_height, height);

  const ra8_camera_buffer_t tiny = {jpeg, 4U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_camera_codec_encode(&codec, &input, &tiny, &encoded));
  TEST_ASSERT_NULL(encoded.data);
  TEST_END("camera software JPEG backend");
}

/**
 * @brief Verify software JPEG initialization and input guards.
 * @details Exercises null, invalid quality, short workspace, and unsupported input paths.
 * @par MC/DC:
 * A valid configuration provides the all-false rejection baseline. Each invalid
 * vector flips one pointer, quality, workspace, or input-format guard independently.
 * @pre Unity test accounting is initialized.
 * @pre Local codec state and workspace are writable.
 * @post Every invalid configuration records its expected error.
 * @post Unsupported JPEG input does not publish encoded output.
 * @note A valid initialization is included before input-format rejection.
 * @since 0.1.0
 */
static void test_jpeg_sw_guards(void)
{
  TEST_BEGIN("camera software JPEG guards");
  ra8_camera_codec_t               codec                                   = {};
  ra8_camera_codec_jpeg_sw_state_t state                                   = {};
  uint8_t                          workspace[(size_t)k_t_output_rgb_bytes] = {};
  ra8_camera_codec_jpeg_sw_cfg_t   cfg = jpeg_cfg(workspace, sizeof workspace);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_codec_jpeg_sw_init(nullptr, &state, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_codec_jpeg_sw_init(&codec, &state, nullptr));
  cfg.quality = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));
  cfg = jpeg_cfg(workspace, sizeof workspace - 1U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));
  cfg = jpeg_cfg(workspace, sizeof workspace);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));

  uint8_t                   raw[(size_t)k_t_rgb_bytes] = {};
  uint8_t                   jpeg[(size_t)k_t_jpeg_cap] = {};
  const ra8_camera_buffer_t output                     = {jpeg, (uint32_t)sizeof jpeg};
  ra8_camera_frame_t        input                      = {.data   = raw,
                                                          .bytes  = 4U,
                                                          .width  = 1U,
                                                          .height = 1U,
                                                          .format = k_ra8_camera_format_jpeg};
  ra8_camera_frame_t        encoded                    = {};
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_camera_codec_encode(&codec, &input, &output, &encoded));
  TEST_END("camera software JPEG guards");
}

/**
 * @brief Verify camera codec output through the RAM stream bridge.
 * @details Checks exact writes and partial byte counts when the sink fills.
 * @par MC/DC:
 * The full sink keeps source and capacity guards accepted; the tiny sink flips
 * only the remaining-capacity condition and reports the bounded prefix.
 * @pre Unity test accounting is initialized.
 * @pre RAM stream backing arrays remain writable during each call.
 * @post The full sink matches the JPEG fixture byte for byte.
 * @post The tiny sink reports its exact written capacity.
 * @note Passthrough isolates stream behavior from JPEG encoding.
 * @since 0.1.0
 */
static void test_stream_bridge(void)
{
  TEST_BEGIN("camera stream bridge");
  uint8_t                   jpeg[] = {0xFFU, 0xD8U, 1U, 2U, 3U, 4U, 0xFFU, 0xD9U, 9U};
  const ra8_camera_frame_t  input  = {.data   = jpeg,
                                      .bytes  = (uint32_t)sizeof jpeg,
                                      .width  = 4U,
                                      .height = 4U,
                                      .format = k_ra8_camera_format_jpeg};
  const ra8_camera_buffer_t unused = {};
  ra8_camera_codec_t        codec  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_codec_passthrough_init(&codec));

  uint8_t                   sink_bytes[32] = {};
  ra8_io_stream_ram_state_t sink_state     = {};
  ra8_io_stream_t           stream         = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_stream_ram_init(&stream, &sink_state, sink_bytes, sizeof sink_bytes));
  uint32_t written = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_camera_codec_encode_to_stream(&codec, &input, &unused, &stream, &written));
  TEST_ASSERT_EQ(sizeof jpeg, written);
  TEST_ASSERT(memcmp(jpeg, sink_bytes, sizeof jpeg) == 0);

  uint8_t                   tiny_bytes[(size_t)k_t_stream_tiny] = {};
  ra8_io_stream_ram_state_t tiny_state                          = {};
  ra8_io_stream_t           tiny_stream                         = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_stream_ram_init(&tiny_stream, &tiny_state, tiny_bytes, (uint32_t)sizeof tiny_bytes));
  TEST_ASSERT_EQ(
    k_ra8_err_no_mem,
    ra8_camera_codec_encode_to_stream(&codec, &input, &unused, &tiny_stream, &written));
  TEST_ASSERT_EQ(k_t_stream_tiny, written);
  TEST_END("camera stream bridge");
}

/**
 * @brief Run the camera facade and backend MC/DC vectors.
 * @par MC/DC:
 * Decisions: libs/ra8_camera/src/ra8_camera.c@ra8_camera_codec_encode,
 * libs/ra8_camera/src/ra8_camera.c@ra8_camera_frame_validate,
 * libs/ra8_camera/src/ra8_camera.c@ra8_camera_source_capture,
 * libs/ra8_camera/src/ra8_camera.c@internal_source_validate,
 * libs/ra8_camera/src/ra8_camera_codec_jpeg_sw.c@internal_jpeg_sw_encode,
 * libs/ra8_camera/src/ra8_camera_codec_jpeg_sw.c@ra8_camera_codec_jpeg_sw_init,
 * libs/ra8_camera/src/ra8_camera_codec_passthrough.c@internal_passthrough_encode,
 * libs/ra8_camera/src/ra8_camera_source_ceu.c@internal_ceu_capture,
 * libs/ra8_camera/src/ra8_camera_source_ceu.c@internal_ceu_get_info,
 * libs/ra8_camera/src/ra8_camera_source_ceu.c@internal_ceu_wait_for_frame,
 * libs/ra8_camera/src/ra8_camera_source_ceu.c@ra8_camera_source_ceu_init,
 * libs/ra8_camera/src/ra8_camera_source_memory.c@internal_memory_capture,
 * libs/ra8_camera/src/ra8_camera_source_memory.c@internal_memory_get_info,
 * libs/ra8_camera/src/ra8_camera_source_memory.c@ra8_camera_source_memory_init.
 * @pre Unity test accounting is initialized.
 * @post Every camera vector group has executed once.
 * @since 0.1.0
 */
static void test_mcdc_camera_facade_backends(void)
{
  test_frame_validation();
  test_memory_source();
  test_facade_guards();
  test_ceu_diagnostic_state();
  test_ceu_vbp_overrides_capture_end();
  test_jpeg_passthrough();
  test_jpeg_sw_backend();
  test_jpeg_sw_guards();
  test_stream_bridge();
}

int main(void)
{
  test_mcdc_camera_facade_backends();
  return 0;
}
