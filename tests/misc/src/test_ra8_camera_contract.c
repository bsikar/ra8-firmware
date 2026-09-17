/**
 * @file test_ra8_camera_contract.c
 * @brief Host tests for the camera frame-validation and capture-contract
 *        guards, split out of test_ra8_camera.c to keep both under the
 *        repository's per-file line cap.
 *
 * @details Exercises the geometry guards ::ra8_camera_frame_validate applies
 * beyond the layout vectors already covered elsewhere, and the capture
 * contract ::ra8_camera_source_capture enforces against a misbehaving source
 * backend. No camera peripheral, sensor, board layer, or filesystem
 * participates.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_camera.h"
#include "ra8_camera_internal.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/** @brief Fixture dimensions and storage bounds. */
typedef enum : uint32_t {
  k_t_width           = 16U,            /**< RGB fixture width.                */
  k_t_height          = 16U,            /**< RGB fixture height.               */
  k_t_rgb_bytes       = 16U * 16U * 3U, /**< Packed RGB fixture bytes.         */
  k_t_small_bytes     = 8U,             /**< Tiny guard-vector buffer bytes.   */
  k_t_tiny_frame      = 3U,             /**< One RGB888 pixel, stride and all. */
  k_t_overflow_stride = 0x80000000U,    /**< Stride whose row product wraps.   */
  k_t_overflow_height = 4U,             /**< Rows that overflow that stride.   */
} t_camera_contract_const_t;

/**
 * @struct t_fault_ctx_t
 * @brief Response a fault-injecting source publishes.
 * @details Lets the injected backend replay any frame view and any error code
 *          so the facade's own post-dispatch contract checks can be reached.
 *          The call counter proves whether the facade rejected before dispatch
 *          or after.
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

/** @brief Complete source vtable dispatching to the injected response. */
static const ra8_camera_source_iface_t s_t_fault_source_iface = {
  .get_info = t_fault_get_info,
  .capture  = t_fault_capture,
};

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

int main(void)
{
  test_frame_validate_geometry_guards();
  test_source_capture_contract();
  return 0;
}
