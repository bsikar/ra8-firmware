/**
 * @file test_ra8_camera_source_memory.c
 * @brief Host tests for the fixed-frame in-memory camera source backend.
 *
 * @details Exercises the replay contract of `ra8_camera_source_memory`, every
 * argument guard of its binding helper, and every guard on its private vtable
 * rows. The rows that the facade shields behind its own argument checks are
 * driven by dispatching directly through the bound vtable, which is the only
 * route to them. No camera peripheral, sensor, or board layer participates.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_camera.h"
#include "ra8_camera_internal.h"
#include "ra8_camera_source_memory.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/** @brief Fixture geometry, storage bounds, and pattern constants. */
typedef enum : uint32_t {
  k_t_cam_width        = 4U,    /**< Fixture width in pixels.                  */
  k_t_cam_height       = 3U,    /**< Fixture height in pixel rows.             */
  k_t_cam_stride       = 12U,   /**< Packed RGB888 row stride in bytes.        */
  k_t_cam_bytes        = 36U,   /**< Total packed fixture bytes.               */
  k_t_cam_capture_cap  = 40U,   /**< Capture capacity, deliberately oversized. */
  k_t_cam_rgb_bpp      = 3U,    /**< Packed RGB888 bytes per pixel.            */
  k_t_cam_short_stride = 8U,    /**< Stride too small to cover one RGB888 row. */
  k_t_cam_small_cap    = 16U,   /**< Capture capacity below one fixture frame. */
  k_t_cam_zero         = 0U,    /**< Rejected zero-valued metadata field.      */
  k_t_cam_pattern      = 37U,   /**< Deterministic fixture byte multiplier.    */
  k_t_cam_offset       = 11U,   /**< Deterministic fixture byte offset.        */
  k_t_cam_byte_mask    = 0xFFU, /**< Low-byte mask for the fixture pattern.    */
  k_t_cam_sentinel     = 0xA5U, /**< Pre-fill marking untouched capture bytes. */
} t_cam_const_t;

static_assert(k_t_cam_stride == (k_t_cam_width * k_t_cam_rgb_bpp),
              "fixture stride must be a tightly packed RGB888 row");
static_assert(k_t_cam_bytes == (k_t_cam_stride * k_t_cam_height),
              "fixture bytes must cover exactly every fixture row");
static_assert(k_t_cam_capture_cap > k_t_cam_bytes,
              "capture storage must exceed the frame so tail bytes stay observable");
static_assert(k_t_cam_small_cap < k_t_cam_bytes,
              "the undersized capacity must be unable to hold one fixture frame");
static_assert(k_t_cam_short_stride < (k_t_cam_width * k_t_cam_rgb_bpp),
              "the short stride must be unable to cover one fixture row");

/**
 * @brief Fill a byte range with a deterministic non-repeating pattern.
 * @details Uses an odd multiplier and offset so a misplaced copy cannot coincide with the
 *          expected bytes, which a constant fill would allow.
 * @param[out] bytes First writable byte of the range.
 * @param[in] count Number of bytes to write.
 * @pre `bytes` addresses at least `count` writable bytes. @pre `count` is the caller's own bound.
 * @post Exactly `count` bytes are written. @post No storage outside the range is touched.
 * @note Single-threaded fixture use only. @since 0.1.0
 */
RA8_INTERNAL static void internal_cam_fill(uint8_t* bytes, uint32_t count)
{
  for (uint32_t index = 0U; index < count; index++) {
    bytes[index] = (uint8_t)(((index * (uint32_t)k_t_cam_pattern) + (uint32_t)k_t_cam_offset) &
                             (uint32_t)k_t_cam_byte_mask);
  }
}

/**
 * @brief Build the valid RGB888 fixture frame descriptor.
 * @details Describes a tightly packed four-by-three image so every geometry rule is satisfied
 *          and a single perturbed field is enough to make one rule fail.
 * @param[in] bytes Fixture storage retained by the calling test.
 * @return Complete camera frame descriptor.
 * @retval ra8_camera_frame_t Descriptor referencing `bytes`.
 * @pre `bytes` addresses at least `k_t_cam_bytes` readable bytes.
 * @pre The fixture storage outlives every use of the descriptor.
 * @post The fixture storage is unchanged.
 * @post The descriptor passes ::ra8_camera_frame_validate.
 * @note The geometry is the smallest one that exercises stride and height together.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_camera_frame_t internal_cam_fixture(const uint8_t* bytes)
{
  return (ra8_camera_frame_t){
    .data         = bytes,
    .bytes        = (uint32_t)k_t_cam_bytes,
    .stride_bytes = (uint32_t)k_t_cam_stride,
    .width        = (uint16_t)k_t_cam_width,
    .height       = (uint16_t)k_t_cam_height,
    .format       = k_ra8_camera_format_rgb888,
  };
}

/**
 * @brief Verify the fixed-frame replay contract end to end.
 * @details Binds the fixture, checks the advertised geometry, captures it byte for byte, and
 *          proves the copy is bounded by the frame rather than by the buffer.
 * @pre Unity test accounting is initialized.
 * @pre Stack storage is available for the fixture and capture buffers.
 * @post The captured bytes equal the fixture exactly.
 * @post Capture storage past the frame keeps its sentinel value.
 * @note The undersized-buffer vector is rejected before any byte is written.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_memory_replay(void)
{
  TEST_BEGIN("camera memory replay");
  uint8_t fixture_bytes[k_t_cam_bytes]       = {};
  uint8_t capture_bytes[k_t_cam_capture_cap] = {};
  internal_cam_fill(fixture_bytes, (uint32_t)k_t_cam_bytes);
  (void)memset(capture_bytes, (int)k_t_cam_sentinel, sizeof(capture_bytes));
  const ra8_camera_frame_t         fixture = internal_cam_fixture(fixture_bytes);
  ra8_camera_source_t              source  = {};
  ra8_camera_source_memory_state_t state   = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_memory_init(&source, &state, &fixture));
  ra8_camera_source_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_get_info(&source, &info));
  TEST_ASSERT_EQ(k_t_cam_bytes, info.frame_bytes_max);
  TEST_ASSERT_EQ(k_t_cam_stride, info.stride_bytes);
  TEST_ASSERT_EQ(k_t_cam_width, info.width);
  TEST_ASSERT_EQ(k_t_cam_height, info.height);
  TEST_ASSERT_EQ(k_ra8_camera_format_rgb888, info.format);
  const ra8_camera_buffer_t buffer = {.data     = capture_bytes,
                                      .capacity = (uint32_t)sizeof(capture_bytes)};
  ra8_camera_frame_t        frame  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_capture(&source, &buffer, &frame));
  TEST_ASSERT(frame.data == capture_bytes);
  TEST_ASSERT_EQ(k_t_cam_bytes, frame.bytes);
  TEST_ASSERT(memcmp(capture_bytes, fixture_bytes, sizeof(fixture_bytes)) == 0);
  TEST_ASSERT_EQ(k_t_cam_sentinel, capture_bytes[k_t_cam_bytes]);
  TEST_ASSERT_EQ(k_t_cam_sentinel, capture_bytes[k_t_cam_capture_cap - 1U]);
  const ra8_camera_buffer_t small = {.data     = capture_bytes,
                                     .capacity = (uint32_t)k_t_cam_small_cap};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_camera_source_capture(&source, &small, &frame));
  TEST_ASSERT_NULL(frame.data);
  TEST_END("camera memory replay");
}

/**
 * @brief Verify the binding helper rejects malformed arguments and frames.
 * @details Drives each null argument, then three frames that fail validation for three
 *          different reasons so propagation cannot be mistaken for one canned code.
 * @pre Unity test accounting is initialized.
 * @pre The handle, state, and fixture storage are stack-owned.
 * @post Each rejected binding records the exact propagated error code.
 * @post A rejected binding leaves both the handle and the backend state untouched.
 * @note The three validation vectors return three distinct error codes by construction.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_memory_init_guards(void)
{
  TEST_BEGIN("camera memory init guards");
  uint8_t fixture_bytes[k_t_cam_bytes] = {};
  internal_cam_fill(fixture_bytes, (uint32_t)k_t_cam_bytes);
  ra8_camera_source_t              source = {};
  ra8_camera_source_memory_state_t state  = {};
  ra8_camera_frame_t               frame  = internal_cam_fixture(fixture_bytes);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_source_memory_init(nullptr, &state, &frame));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_source_memory_init(&source, nullptr, &frame));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_source_memory_init(&source, &state, nullptr));
  frame.stride_bytes = (uint32_t)k_t_cam_short_stride;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_camera_source_memory_init(&source, &state, &frame));
  frame       = internal_cam_fixture(fixture_bytes);
  frame.width = (uint16_t)k_t_cam_zero;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_camera_source_memory_init(&source, &state, &frame));
  frame      = internal_cam_fixture(fixture_bytes);
  frame.data = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_source_memory_init(&source, &state, &frame));
  TEST_ASSERT_NULL(source.iface);
  TEST_ASSERT_NULL(source.ctx);
  TEST_ASSERT_NULL(state.frame.data);
  TEST_ASSERT_EQ(k_t_cam_zero, state.frame.bytes);
  TEST_END("camera memory init guards");
}

/**
 * @brief Verify a handle whose backend state went missing is refused by both rows.
 * @details The facade forwards its context untouched, so an absent state reaches the backend
 *          rows and each must reject it rather than dereference it.
 * @pre Unity test accounting is initialized.
 * @pre The handle, state, and both byte arrays are stack-owned.
 * @post Both facade calls record `k_ra8_err_null_ptr`.
 * @post Both output descriptors are left zeroed by the facade.
 * @note This is the black-box route to the backend's own context guards.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_memory_missing_context(void)
{
  TEST_BEGIN("camera memory missing context");
  uint8_t fixture_bytes[k_t_cam_bytes]       = {};
  uint8_t capture_bytes[k_t_cam_capture_cap] = {};
  internal_cam_fill(fixture_bytes, (uint32_t)k_t_cam_bytes);
  (void)memset(capture_bytes, (int)k_t_cam_sentinel, sizeof(capture_bytes));
  const ra8_camera_frame_t         fixture = internal_cam_fixture(fixture_bytes);
  ra8_camera_source_t              source  = {};
  ra8_camera_source_memory_state_t state   = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_memory_init(&source, &state, &fixture));
  source.ctx                    = nullptr;
  ra8_camera_source_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_source_get_info(&source, &info));
  TEST_ASSERT_EQ(k_t_cam_zero, info.frame_bytes_max);
  const ra8_camera_buffer_t buffer = {.data     = capture_bytes,
                                      .capacity = (uint32_t)sizeof(capture_bytes)};
  ra8_camera_frame_t        frame  = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_camera_source_capture(&source, &buffer, &frame));
  TEST_ASSERT_NULL(frame.data);
  TEST_ASSERT_EQ(k_t_cam_sentinel, capture_bytes[0]);
  TEST_END("camera memory missing context");
}

/**
 * @brief Verify each backend row guards its own arguments, independently of the facade.
 * @details The facade rejects a null output or buffer, and re-checks the captured byte count
 *          against the buffer, before or after dispatch. Those rows are therefore reachable
 *          only by calling the bound vtable directly, which is what this test does.
 * @pre Unity test accounting is initialized.
 * @pre The bound source, state, and both byte arrays are stack-owned.
 * @post Each rejected row call records its documented error code.
 * @post No rejected call writes a single capture byte or output field.
 * @note The untouched sentinel proves rejection happened before the bounded copy, which is
 *       what separates a real capacity guard from one the facade merely masks.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_memory_row_guards(void)
{
  TEST_BEGIN("camera memory row guards");
  uint8_t fixture_bytes[k_t_cam_bytes]       = {};
  uint8_t capture_bytes[k_t_cam_capture_cap] = {};
  internal_cam_fill(fixture_bytes, (uint32_t)k_t_cam_bytes);
  (void)memset(capture_bytes, (int)k_t_cam_sentinel, sizeof(capture_bytes));
  const ra8_camera_frame_t         fixture = internal_cam_fixture(fixture_bytes);
  ra8_camera_source_t              source  = {};
  ra8_camera_source_memory_state_t state   = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_camera_source_memory_init(&source, &state, &fixture));
  const ra8_camera_source_iface_t* iface  = source.iface;
  ra8_camera_source_info_t         info   = {};
  ra8_camera_frame_t               frame  = {};
  const ra8_camera_buffer_t        buffer = {.data     = capture_bytes,
                                             .capacity = (uint32_t)sizeof(capture_bytes)};
  const ra8_camera_buffer_t        small  = {.data     = capture_bytes,
                                             .capacity = (uint32_t)k_t_cam_small_cap};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, iface->get_info(nullptr, &info));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, iface->get_info(&state, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, iface->capture(nullptr, &buffer, &frame));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, iface->capture(&state, nullptr, &frame));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, iface->capture(&state, &buffer, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, iface->capture(&state, &small, &frame));
  TEST_ASSERT_EQ(k_t_cam_zero, info.frame_bytes_max);
  TEST_ASSERT_NULL(frame.data);
  TEST_ASSERT_EQ(k_t_cam_sentinel, capture_bytes[0]);
  TEST_ASSERT_EQ(k_ra8_ok, iface->capture(&state, &buffer, &frame));
  TEST_ASSERT(memcmp(capture_bytes, fixture_bytes, sizeof(fixture_bytes)) == 0);
  TEST_END("camera memory row guards");
}

/**
 * @brief Run every fixed-frame memory-source vector exactly once.
 * @details Executes the replay contract, the binding guards, the missing-context paths, and
 *          the direct row guards so the backend is covered end to end.
 * @pre Unity test accounting is initialized.
 * @pre All fixture helpers are linked into this executable.
 * @post Every memory-source vector group has executed once.
 * @post No fixture state survives the function return.
 * @note This target links the production backend through `ra8_core_hal`.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_camera_memory_source(void)
{
  internal_test_memory_replay();
  internal_test_memory_init_guards();
  internal_test_memory_missing_context();
  internal_test_memory_row_guards();
}

int main(void)
{
  internal_test_camera_memory_source();
  return 0;
}
