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

#include "ra8_attributes.h"
#include "ra8_camera.h"
#include "ra8_camera_codec_jpeg_sw.h"
#include "ra8_camera_codec_passthrough.h"
#include "ra8_camera_internal.h"
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
  k_t_width            = 16U,            /**< RGB fixture width.                  */
  k_t_height           = 16U,            /**< RGB fixture height.                 */
  k_t_output_width     = 8U,             /**< Software-JPEG output width.         */
  k_t_output_height    = 8U,             /**< Software-JPEG output height.        */
  k_t_output_rgb_bytes = 8U * 8U * 3U,   /**< Software-JPEG RGB workspace.        */
  k_t_rgb_bytes        = 16U * 16U * 3U, /**< Packed RGB fixture bytes.           */
  k_t_uyvy_bytes       = 16U * 16U * 2U, /**< Packed UYVY fixture bytes.          */
  k_t_jpeg_cap         = 8192U,          /**< Ample software-JPEG output bound.   */
  k_t_stream_tiny      = 8U,             /**< Sink capacity for partial writes.   */
  k_t_byte_mod         = 251U,           /**< Non-power-of-two fixture pattern.   */
  k_t_ceu_events       = 0x12000001U,    /**< Representative retained CEU bits.   */
  k_t_small_bytes      = 8U,             /**< Tiny guard-vector buffer bytes.     */
  k_t_tiny_frame       = 3U,             /**< One RGB888 pixel, stride and all.   */
  k_t_overflow_stride  = 0x80000000U,    /**< Stride whose row product wraps.     */
  k_t_overflow_height  = 4U,             /**< Rows that overflow that stride.     */
  k_t_wrap_width       = 23307U,         /**< Output width whose RGB size wraps.  */
  k_t_wrap_height      = 61426U,         /**< Output height completing that wrap. */
  k_t_bad_quality      = 101U,           /**< One above the legal JPEG quality.   */
  k_t_odd_sample_width = 12U,            /**< Width that samples odd columns.     */
  k_t_chroma_extreme   = 255U,           /**< Cb and Cr sample forcing clamps.    */
  k_t_luma_sub_black   = 0U,             /**< Luma below the limited-range floor. */
  k_t_clamp_red        = 203U,           /**< BT.601 red for the clamp fixture.   */
  k_t_clamp_green      = 0U,             /**< Low-clamped green for that pixel.   */
  k_t_clamp_blue       = 255U,           /**< High-clamped blue for that pixel.   */
  k_t_rgb_triple       = 3U,             /**< Packed RGB bytes per pixel.         */
  k_t_written_sentinel = 77U,            /**< Detects an untouched byte count.    */
} t_camera_const_t;

/**
 * @struct t_fault_ctx_t
 * @brief Response a fault-injecting source or codec row publishes.
 * @details Lets one backend replay any frame view and any error code so the
 *          facade's own post-dispatch contract checks can be reached. The call
 *          counter proves whether the facade rejected before dispatch or after.
 * @invariant `frame` is copied verbatim into the caller's output descriptor.
 * @since 0.1.0
 */
typedef struct {
  ra8_camera_frame_t frame;  /**< Frame view published on every call.   */
  ra8_err_t          result; /**< Error code handed back to the facade. */
  uint32_t           calls;  /**< Times the facade reached the backend. */
} t_fault_ctx_t;

/**
 * @brief Report fixed geometry from an injected source row.
 * @details Supplies a non-null information row so a vtable can be incomplete in
 *          exactly one place at a time.
 * @param[in] ctx Unused injected-source context.
 * @param[out] out_info Receives the fixture geometry.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Information published.
 * @pre `out_info` is writable. @pre Installed only by this single-threaded test.
 * @post `out_info` is fully initialized. @post No fixture state changes.
 * @note Thread-safe because the row keeps no state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t t_fault_get_info(void* ctx, ra8_camera_source_info_t* out_info)
{
  (void)ctx;
  *out_info = (ra8_camera_source_info_t){
    .frame_bytes_max = (uint32_t)k_t_tiny_frame,
    .stride_bytes    = (uint32_t)k_t_tiny_frame,
    .width           = 1U,
    .height          = 1U,
    .format          = k_ra8_camera_format_rgb888,
  };
  return k_ra8_ok;
}

/**
 * @brief Publish the injected frame and error from a source capture row.
 * @details Reaches the facade's alias, capacity, and validation checks that a
 *          well-behaved backend can never violate.
 * @param[in] ctx Injected ::t_fault_ctx_t response.
 * @param[in] buffer Unused destination description.
 * @param[out] out_frame Receives the injected frame view.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The injected response was success.
 * @retval other The injected error code.
 * @pre `ctx` addresses a live ::t_fault_ctx_t. @pre `out_frame` is writable.
 * @post `out_frame` equals the injected frame. @post The call counter advances.
 * @note Not thread-safe with respect to the shared injected response.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
t_fault_capture(void* ctx, const ra8_camera_buffer_t* buffer, ra8_camera_frame_t* out_frame)
{
  (void)buffer;
  t_fault_ctx_t* fault = (t_fault_ctx_t*)ctx;
  fault->calls += 1U;
  *out_frame = fault->frame;
  return fault->result;
}

/**
 * @brief Publish the injected frame and error from a codec encode row.
 * @details Reaches the facade's post-encode output validation and its error
 *          propagation path without a real codec backend.
 * @param[in] ctx Injected ::t_fault_ctx_t response.
 * @param[in] input Unused input frame.
 * @param[in] output_buffer Unused output description.
 * @param[out] out_frame Receives the injected frame view.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The injected response was success.
 * @retval other The injected error code.
 * @pre `ctx` addresses a live ::t_fault_ctx_t. @pre `out_frame` is writable.
 * @post `out_frame` equals the injected frame. @post The call counter advances.
 * @note Not thread-safe with respect to the shared injected response.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t t_fault_encode(void*                      ctx,
                                             const ra8_camera_frame_t*  input,
                                             const ra8_camera_buffer_t* output_buffer,
                                             ra8_camera_frame_t*        out_frame)
{
  (void)input;
  (void)output_buffer;
  t_fault_ctx_t* fault = (t_fault_ctx_t*)ctx;
  fault->calls += 1U;
  *out_frame = fault->frame;
  return fault->result;
}

/** @brief Complete source vtable dispatching to the injected response. */
static const ra8_camera_source_iface_t s_t_fault_source_iface = {
  .get_info = t_fault_get_info,
  .capture  = t_fault_capture,
};

/** @brief Source vtable whose mandatory information row is unbound. */
static const ra8_camera_source_iface_t s_t_no_get_info_iface = {
  .get_info = nullptr,
  .capture  = t_fault_capture,
};

/** @brief Source vtable whose mandatory capture row is unbound. */
static const ra8_camera_source_iface_t s_t_no_capture_iface = {
  .get_info = t_fault_get_info,
  .capture  = nullptr,
};

/** @brief Codec vtable dispatching to the injected response. */
static const ra8_camera_codec_iface_t s_t_fault_codec_iface = {.encode = t_fault_encode};

/** @brief Codec vtable whose mandatory encode row is unbound. */
static const ra8_camera_codec_iface_t s_t_no_encode_iface = {.encode = nullptr};

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
 * @brief Verify facade rejection of unbound, half-bound, and null objects.
 * @details Exercises source and codec guards before backend dispatch, including
 *          each half-populated source vtable and both destination-buffer guards.
 * @par MC/DC:
 * Each vector removes one required handle, interface row, buffer, or output
 * pointer while the other facade dependencies remain valid.
 * @pre Unity test accounting is initialized.
 * @pre Local facade handles begin zero-initialized.
 * @post Each invalid call records its documented error.
 * @post No backend callback observes a destination buffer.
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
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_source_get_info(nullptr, &info));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_source_capture(&source, &buffer, nullptr));

  ra8_camera_source_t half_info = {.iface = &s_t_no_get_info_iface, .ctx = nullptr};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_camera_source_get_info(&half_info, &info));
  ra8_camera_source_t half_capture = {.iface = &s_t_no_capture_iface, .ctx = nullptr};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_camera_source_capture(&half_capture, &buffer, &out));

  t_fault_ctx_t             fault   = {};
  ra8_camera_source_t       bound   = {.iface = &s_t_fault_source_iface, .ctx = &fault};
  const ra8_camera_buffer_t no_data = {nullptr, (uint32_t)sizeof bytes};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_source_capture(&bound, &no_data, &out));
  const ra8_camera_buffer_t empty = {bytes, 0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_camera_source_capture(&bound, &empty, &out));
  TEST_ASSERT_EQ(0U, fault.calls);
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
 * @brief Verify JPEG passthrough binding, aliasing, and format rejection.
 * @details Checks zero-copy JPEG output, unsupported raw RGB input, the null
 *          binding guard, and both backend-row null guards the facade filters.
 * @par MC/DC:
 * The JPEG vector keeps format and frame guards accepted; the RGB vector flips
 * only the supported-format condition and must clear the output descriptor.
 * @pre Unity test accounting is initialized.
 * @pre Input byte arrays remain valid through each encode call.
 * @post JPEG output aliases the exact input span.
 * @post Rejected raw input clears the output descriptor.
 * @note The backend row is reached directly where the facade filters nulls.
 * @since 0.1.0
 */
static void test_jpeg_passthrough(void)
{
  TEST_BEGIN("camera JPEG passthrough");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_codec_passthrough_init(nullptr));
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

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, codec.iface->encode(codec.ctx, nullptr, &unused, &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, codec.iface->encode(codec.ctx, &input, &unused, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, codec.iface->encode(codec.ctx, &input, &unused, &out));
  TEST_ASSERT(out.data == jpeg);
  TEST_ASSERT_EQ(input.bytes, out.bytes);
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
 * @details Exercises every null argument, both quality bounds, zero output
 *          dimensions, overflowing output geometry, a short workspace, and
 *          unsupported input format.
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
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_codec_jpeg_sw_init(&codec, nullptr, &cfg));
  cfg = jpeg_cfg(nullptr, sizeof workspace);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));
  cfg         = jpeg_cfg(workspace, sizeof workspace);
  cfg.quality = (uint8_t)k_t_bad_quality;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));
  cfg              = jpeg_cfg(workspace, sizeof workspace);
  cfg.output_width = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));
  cfg               = jpeg_cfg(workspace, sizeof workspace);
  cfg.output_height = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));
  /* 23307 * 3 * 61426 is 50 past 2^32, so the workspace-capacity check alone
   * would compare against 50 and accept: only the overflow guard rejects. */
  cfg               = jpeg_cfg(workspace, sizeof workspace);
  cfg.output_width  = (uint16_t)k_t_wrap_width;
  cfg.output_height = (uint16_t)k_t_wrap_height;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));
  TEST_ASSERT_NULL(codec.iface);

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
 * @details Checks exact writes, partial byte counts when the sink fills, the
 *          missing-sink guard, and an unchanged codec error with a zeroed count.
 * @par MC/DC:
 * The full sink keeps source and capacity guards accepted; the tiny sink flips
 * only the remaining-capacity condition and reports the bounded prefix.
 * @pre Unity test accounting is initialized.
 * @pre RAM stream backing arrays remain writable during each call.
 * @post The full sink matches the JPEG fixture byte for byte.
 * @post A refused sink leaves the caller's byte count untouched.
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

  written = (uint32_t)k_t_written_sentinel;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_camera_codec_encode_to_stream(&codec, &input, &unused, nullptr, &written));
  TEST_ASSERT_EQ(k_t_written_sentinel, written);
  ra8_camera_codec_t unbound = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_camera_codec_encode_to_stream(&unbound, &input, &unused, &stream, &written));
  TEST_ASSERT_EQ(0U, written);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_camera_codec_encode_to_stream(&codec, &input, &unused, &stream, nullptr));
  TEST_ASSERT(memcmp(jpeg, &sink_bytes[sizeof jpeg], sizeof jpeg) == 0);
  TEST_END("camera stream bridge");
}

/**
 * @brief Reject the frame geometries the existing layout vectors leave untested.
 * @details Covers a zero height, a JPEG view carrying no bytes, a stride whose
 *          product with the row count cannot be represented, and a byte count
 *          one short of the rows a legal stride describes.
 * @par MC/DC: (no compound decision -- each vector drives one single-condition guard.)
 * @pre Unity test accounting is initialized. @pre Stack fixture storage is available.
 * @post Each malformed frame records its documented error. @post No fixture byte changes.
 * @note No hardware or peripheral MMIO is accessed.
 * @since 0.1.0
 */
static void test_frame_validate_geometry_guards(void)
{
  TEST_BEGIN("camera frame validation geometry guards");
  uint8_t            rgb[(size_t)k_t_rgb_bytes] = {};
  ra8_camera_frame_t frame                      = rgb_frame(rgb);
  frame.height                                  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_camera_frame_validate(&frame));

  frame = (ra8_camera_frame_t){.data   = rgb,
                               .bytes  = 0U,
                               .width  = 1U,
                               .height = 1U,
                               .format = k_ra8_camera_format_jpeg};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_camera_frame_validate(&frame));

  frame = (ra8_camera_frame_t){.data         = rgb,
                               .bytes        = UINT32_MAX,
                               .stride_bytes = (uint32_t)k_t_overflow_stride,
                               .width        = 1U,
                               .height       = (uint16_t)k_t_overflow_height,
                               .format       = k_ra8_camera_format_rgb888};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_camera_frame_validate(&frame));

  frame = rgb_frame(rgb);
  frame.bytes -= 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_camera_frame_validate(&frame));
  TEST_END("camera frame validation geometry guards");
}

/**
 * @brief Enforce the capture contract against a misbehaving source backend.
 * @details A fault row replays a propagated error, a frame aliasing foreign
 *          storage, an over-long frame, and an invalid geometry in turn.
 * @par MC/DC: (no compound decision -- each vector drives one single-condition guard.)
 * @pre Unity test accounting is initialized. @pre Both fixture arrays are distinct objects.
 * @post Every rejected capture clears the output frame. @post Destination bytes stay zero.
 * @note The injected row never writes the destination buffer.
 * @since 0.1.0
 */
static void test_source_capture_contract(void)
{
  TEST_BEGIN("camera source capture contract");
  uint8_t                   elsewhere[(size_t)k_t_small_bytes]     = {};
  uint8_t                   capture_bytes[(size_t)k_t_small_bytes] = {};
  const ra8_camera_buffer_t buffer = {capture_bytes, (uint32_t)sizeof capture_bytes};
  t_fault_ctx_t             fault  = {
    .frame  = {.data         = capture_bytes,
               .bytes        = (uint32_t)k_t_tiny_frame,
               .stride_bytes = (uint32_t)k_t_tiny_frame,
               .width        = 1U,
               .height       = 1U,
               .format       = k_ra8_camera_format_rgb888},
    .result = k_ra8_err_timeout,
  };
  ra8_camera_source_t source = {.iface = &s_t_fault_source_iface, .ctx = &fault};
  ra8_camera_frame_t  out    = {};
  TEST_ASSERT_EQ(k_ra8_err_timeout, ra8_camera_source_capture(&source, &buffer, &out));
  TEST_ASSERT_NULL(out.data);

  fault.result     = k_ra8_ok;
  fault.frame.data = elsewhere;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_camera_source_capture(&source, &buffer, &out));
  TEST_ASSERT_NULL(out.data);

  fault.frame.data  = capture_bytes;
  fault.frame.bytes = (uint32_t)sizeof capture_bytes + 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_camera_source_capture(&source, &buffer, &out));
  TEST_ASSERT_NULL(out.data);

  fault.frame.bytes = (uint32_t)k_t_tiny_frame;
  fault.frame.width = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_camera_source_capture(&source, &buffer, &out));
  TEST_ASSERT_NULL(out.data);
  TEST_END("camera source capture contract");
}

/**
 * @brief Reject every unusable codec argument and every invalid codec result.
 * @details Covers the four null arguments, an unbound encode row, an input that
 *          fails validation, a propagated backend error, and an invalid output.
 * @par MC/DC: (no compound decision -- each vector drives one single-condition guard.)
 * @pre Unity test accounting is initialized. @pre Local handles begin zeroed.
 * @post Each vector records its documented error. @post Every rejection clears the output.
 * @note No hardware or peripheral MMIO is accessed.
 * @since 0.1.0
 */
static void test_codec_encode_guards(void)
{
  TEST_BEGIN("camera codec encode guards");
  uint8_t                   bytes[(size_t)k_t_small_bytes] = {};
  const ra8_camera_frame_t  frame  = {.data         = bytes,
                                      .bytes        = (uint32_t)k_t_tiny_frame,
                                      .stride_bytes = (uint32_t)k_t_tiny_frame,
                                      .width        = 1U,
                                      .height       = 1U,
                                      .format       = k_ra8_camera_format_rgb888};
  const ra8_camera_buffer_t buffer = {bytes, (uint32_t)sizeof bytes};
  ra8_camera_codec_t        codec  = {};
  ra8_camera_frame_t        out    = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_codec_encode(nullptr, &frame, &buffer, &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_codec_encode(&codec, nullptr, &buffer, &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_codec_encode(&codec, &frame, nullptr, &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_codec_encode(&codec, &frame, &buffer, nullptr));

  ra8_camera_codec_t half_codec = {.iface = &s_t_no_encode_iface, .ctx = nullptr};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_camera_codec_encode(&half_codec, &frame, &buffer, &out));

  t_fault_ctx_t            fault       = {};
  ra8_camera_codec_t       fault_codec = {.iface = &s_t_fault_codec_iface, .ctx = &fault};
  const ra8_camera_frame_t bad_input   = {.data   = nullptr,
                                          .bytes  = (uint32_t)k_t_tiny_frame,
                                          .width  = 1U,
                                          .height = 1U,
                                          .format = k_ra8_camera_format_jpeg};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_camera_codec_encode(&fault_codec, &bad_input, &buffer, &out));
  TEST_ASSERT_EQ(0U, fault.calls);

  fault.result = k_ra8_err_busy;
  TEST_ASSERT_EQ(k_ra8_err_busy, ra8_camera_codec_encode(&fault_codec, &frame, &buffer, &out));
  TEST_ASSERT_NULL(out.data);

  fault.result = k_ra8_ok;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_codec_encode(&fault_codec, &frame, &buffer, &out));
  TEST_ASSERT_NULL(out.data);
  TEST_ASSERT_EQ(2U, fault.calls);
  TEST_END("camera codec encode guards");
}

/**
 * @brief Reject every unusable software-JPEG encode argument.
 * @details Drives the four backend null guards directly, then the two output
 *          buffer guards the facade forwards without inspecting.
 * @par MC/DC: (no compound decision -- each vector drives one single-condition guard.)
 * @pre Unity test accounting is initialized. @pre The codec was bound successfully.
 * @post Each vector records its documented error. @post No workspace byte is published.
 * @note The backend row is reached directly because the facade filters null arguments.
 * @since 0.1.0
 */
static void test_jpeg_sw_encode_guards(void)
{
  TEST_BEGIN("camera software JPEG encode guards");
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

  ra8_camera_frame_t out = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, codec.iface->encode(nullptr, &input, &output, &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, codec.iface->encode(codec.ctx, nullptr, &output, &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, codec.iface->encode(codec.ctx, &input, nullptr, &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, codec.iface->encode(codec.ctx, &input, &output, nullptr));

  const ra8_camera_buffer_t no_data = {nullptr, (uint32_t)sizeof jpeg};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_codec_encode(&codec, &input, &no_data, &out));
  const ra8_camera_buffer_t empty = {jpeg, 0U};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_codec_encode(&codec, &input, &empty, &out));
  TEST_ASSERT_NULL(out.data);
  /* Every rejection above precedes conversion, so the workspace is still the
   * zeroed fixture rather than the sampled second channel of `rgb`. */
  TEST_ASSERT_EQ(0U, workspace[1]);
  TEST_END("camera software JPEG encode guards");
}

/**
 * @brief Saturate the BT.601 conversion in all three directions.
 * @details Every source pixel carries luma below the limited-range floor and
 *          both chroma samples at maximum, which drives the luma floor, the
 *          low green clamp, and the high blue clamp on the same pixel. The
 *          caller-owned workspace is the conversion result, so the expected
 *          bytes are asserted exactly rather than through the encoded stream.
 * @par MC/DC: (no compound decision -- each clamp is a single-condition guard.)
 * @pre Unity test accounting is initialized. @pre The workspace covers the output geometry.
 * @post Every workspace pixel equals the saturated triple. @post Encoding still succeeds.
 * @note The workspace remains caller-owned, so it can be inspected after encoding.
 * @since 0.1.0
 */
static void test_jpeg_sw_color_clamp(void)
{
  TEST_BEGIN("camera software JPEG colour clamp");
  uint8_t uyvy[(size_t)k_t_uyvy_bytes]            = {};
  uint8_t workspace[(size_t)k_t_output_rgb_bytes] = {};
  uint8_t jpeg[(size_t)k_t_jpeg_cap]              = {};
  for (uint32_t i = 0U; i < (uint32_t)sizeof uyvy; i += 2U) {
    uyvy[i]      = (uint8_t)k_t_chroma_extreme;
    uyvy[i + 1U] = (uint8_t)k_t_luma_sub_black;
  }
  /* A non-multiple input width makes the sampled source column odd for part of
   * the row, the only way to select the second luma of a UYVY chroma pair. */
  const uint32_t                       stride = (uint32_t)k_t_odd_sample_width * 2U;
  const ra8_camera_frame_t             input  = {.data         = uyvy,
                                                 .bytes        = stride * (uint32_t)k_t_height,
                                                 .stride_bytes = stride,
                                                 .width        = (uint16_t)k_t_odd_sample_width,
                                                 .height       = (uint16_t)k_t_height,
                                                 .format       = k_ra8_camera_format_uyvy422};
  const ra8_camera_buffer_t            output = {jpeg, (uint32_t)sizeof jpeg};
  ra8_camera_codec_t                   codec  = {};
  ra8_camera_codec_jpeg_sw_state_t     state  = {};
  const ra8_camera_codec_jpeg_sw_cfg_t cfg    = jpeg_cfg(workspace, sizeof workspace);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));

  ra8_camera_frame_t encoded = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_codec_encode(&codec, &input, &output, &encoded));
  for (uint32_t i = 0U; i < (uint32_t)k_t_output_rgb_bytes; i += (uint32_t)k_t_rgb_triple) {
    TEST_ASSERT_EQ(k_t_clamp_red, workspace[i]);
    TEST_ASSERT_EQ(k_t_clamp_green, workspace[i + 1U]);
    TEST_ASSERT_EQ(k_t_clamp_blue, workspace[i + 2U]);
  }
  TEST_END("camera software JPEG colour clamp");
}

/**
 * @brief Run the camera facade and backend MC/DC vectors.
 * @par MC/DC:
 * Decisions: libs/ra8_camera/src/ra8_camera.c@ra8_camera_codec_encode,
 * libs/ra8_camera/src/ra8_camera.c@ra8_camera_frame_validate,
 * libs/ra8_camera/src/ra8_camera.c@ra8_camera_source_capture,
 * libs/ra8_camera/src/ra8_camera.c@internal_source_validate,
 * libs/ra8_camera/src/ra8_camera_codec_jpeg_sw.c@internal_jpeg_sw_clamp,
 * libs/ra8_camera/src/ra8_camera_codec_jpeg_sw.c@internal_jpeg_sw_encode,
 * libs/ra8_camera/src/ra8_camera_codec_jpeg_sw.c@internal_jpeg_sw_ycbcr_to_rgb,
 * libs/ra8_camera/src/ra8_camera_codec_jpeg_sw.c@ra8_camera_codec_jpeg_sw_init,
 * libs/ra8_camera/src/ra8_camera_codec_passthrough.c@internal_passthrough_encode,
 * libs/ra8_camera/src/ra8_camera_source_ceu.c@internal_ceu_capture,
 * libs/ra8_camera/src/ra8_camera_source_ceu.c@internal_ceu_get_info,
 * libs/ra8_camera/src/ra8_camera_source_ceu.c@internal_ceu_wait_for_frame,
 * libs/ra8_camera/src/ra8_camera_source_ceu.c@ra8_camera_source_ceu_init,
 * libs/ra8_camera/src/ra8_camera_source_memory.c@internal_memory_capture,
 * libs/ra8_camera/src/ra8_camera_source_memory.c@internal_memory_get_info,
 * libs/ra8_camera/src/ra8_camera_source_memory.c@ra8_camera_source_memory_init,
 * libs/ra8_camera_io/src/ra8_camera_stream.c@ra8_camera_codec_encode_to_stream.
 * @pre Unity test accounting is initialized.
 * @post Every camera vector group has executed once.
 * @since 0.1.0
 */
static void test_mcdc_camera_facade_backends(void)
{
  test_frame_validation();
  test_frame_validate_geometry_guards();
  test_memory_source();
  test_facade_guards();
  test_source_capture_contract();
  test_codec_encode_guards();
  test_ceu_diagnostic_state();
  test_ceu_vbp_overrides_capture_end();
  test_jpeg_passthrough();
  test_jpeg_sw_backend();
  test_jpeg_sw_guards();
  test_jpeg_sw_encode_guards();
  test_jpeg_sw_color_clamp();
  test_stream_bridge();
}

int main(void)
{
  test_mcdc_camera_facade_backends();
  return 0;
}
