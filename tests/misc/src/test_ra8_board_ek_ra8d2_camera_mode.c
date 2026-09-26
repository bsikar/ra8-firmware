/**
 * @file test_ra8_board_ek_ra8d2_camera_mode.c
 * @brief Unit tests for the EK-RA8D2 camera capture policy
 *
 * @details
 * Exercises ra8_board_camera_get_ceu_config(): the null guard, the
 * out-of-range mode, a zero capacity, a buffer one byte short of a
 * packed VGA frame, and both validated modes field by field against the
 * descriptors the two C6 camera applications carry by hand today.
 *
 * Nothing here touches a register; the unit is pure board policy, so no
 * fake register window is installed.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2_camera_mode.h"
#include "ra8_ceu.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum board_camera_fixture_t
 * @brief Fixture constants: the two frame-buffer capacities the applications allocate.
 */
typedef enum : uint32_t {
  k_camera_uyvy_bytes = 640U * 480U * 2U, /**< Packed VGA UYVY frame bytes. */
  k_camera_jpeg_bytes = 512U * 1024U,     /**< Gated JPEG buffer bytes.     */
} board_camera_fixture_t;

/**
 * @test test_camera_rejects_null_out
 * @brief A null output pointer is refused before any policy is built.
 * @pre None.
 * @post No caller storage is written.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_camera_rejects_null_out(void)
{
  TEST_BEGIN("camera mode: null out rejected");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_board_camera_get_ceu_config(k_ra8_board_camera_mode_vga_uyvy,
                                                 (uint32_t)k_camera_uyvy_bytes, nullptr));
  TEST_END("camera mode: null out rejected");
}

/**
 * @test test_camera_rejects_unknown_mode
 * @brief A mode past the validated set is refused rather than defaulted.
 * @pre None.
 * @post The caller's descriptor keeps its poison value.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_camera_rejects_unknown_mode(void)
{
  TEST_BEGIN("camera mode: unknown mode rejected");
  ra8_board_camera_ceu_config_t cfg = {};
  cfg.frame_bytes_max               = 0xDEADU;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_board_camera_get_ceu_config(
                   (ra8_board_camera_mode_t)k_ra8_board_camera_mode_count,
                   (uint32_t)k_camera_uyvy_bytes, &cfg));
  TEST_ASSERT_EQ(0xDEADU, cfg.frame_bytes_max);
  TEST_END("camera mode: unknown mode rejected");
}

/**
 * @test test_camera_rejects_zero_capacity
 * @brief A zero-byte frame buffer is refused for every mode.
 * @pre None.
 * @post No caller storage is written.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_camera_rejects_zero_capacity(void)
{
  TEST_BEGIN("camera mode: zero capacity rejected");
  ra8_board_camera_ceu_config_t cfg = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_board_camera_get_ceu_config(k_ra8_board_camera_mode_vga_jpeg, 0U, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_board_camera_get_ceu_config(k_ra8_board_camera_mode_vga_uyvy, 0U, &cfg));
  TEST_END("camera mode: zero capacity rejected");
}

/**
 * @test test_camera_rejects_short_packed_buffer
 * @brief One byte short of a packed frame is refused, not silently clipped.
 * @pre None.
 * @post No caller storage is written.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_camera_rejects_short_packed_buffer(void)
{
  TEST_BEGIN("camera mode: short packed buffer rejected");
  ra8_board_camera_ceu_config_t cfg = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_board_camera_get_ceu_config(k_ra8_board_camera_mode_vga_uyvy,
                                                 (uint32_t)k_camera_uyvy_bytes - 1U, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_board_camera_get_ceu_config(k_ra8_board_camera_mode_vga_uyvy,
                                                 (uint32_t)k_camera_uyvy_bytes, &cfg));
  TEST_END("camera mode: short packed buffer rejected");
}

/**
 * @test test_camera_uyvy_matches_bench_descriptor
 * @brief Packed VGA UYVY matches the bench-proven application literal.
 * @pre None.
 * @post No hardware state changes.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_camera_uyvy_matches_bench_descriptor(void)
{
  TEST_BEGIN("camera mode: packed UYVY descriptor");
  ra8_board_camera_ceu_config_t cfg = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_board_camera_get_ceu_config(k_ra8_board_camera_mode_vga_uyvy,
                                                 (uint32_t)k_camera_uyvy_bytes, &cfg));
  TEST_ASSERT_EQ(640U, cfg.ceu.width_px);
  TEST_ASSERT_EQ(480U, cfg.ceu.height_px);
  TEST_ASSERT_EQ(1280U, cfg.ceu.x_capture_px);
  TEST_ASSERT_EQ(480U, cfg.ceu.y_capture_lines);
  TEST_ASSERT_EQ(1280U, cfg.ceu.dst_stride);
  TEST_ASSERT_EQ(2U, cfg.ceu.bytes_per_pixel);
  TEST_ASSERT_EQ(k_ra8_ceu_fmt_data_synchronous, cfg.ceu.capture_format);
  TEST_ASSERT_EQ(k_ra8_ceu_capture_single, cfg.ceu.capture_mode);
  TEST_ASSERT_EQ(k_ra8_ceu_bus_8_bit, cfg.ceu.data_bus);
  TEST_ASSERT_EQ(k_ra8_ceu_pol_high_active, cfg.ceu.hsync_polarity);
  TEST_ASSERT_EQ(k_ra8_ceu_pol_high_active, cfg.ceu.vsync_polarity);
  TEST_ASSERT_EQ(k_ra8_ceu_pol_high_active, cfg.ceu.field_polarity);
  TEST_ASSERT_EQ(k_ra8_ceu_input_cb0_y0_cr0_y1, cfg.ceu.input_order);
  TEST_ASSERT_EQ(k_ra8_ceu_output_ycbcr_422, cfg.ceu.output_format);
  TEST_ASSERT_EQ(k_ra8_ceu_burst_32, cfg.ceu.burst_mode);
  TEST_ASSERT_EQ(k_ra8_ceu_field_immediate, cfg.ceu.first_field);
  TEST_ASSERT_EQ(k_ra8_ceu_edge_rising, cfg.ceu.edge.data);
  TEST_ASSERT_EQ(k_ra8_ceu_edge_rising, cfg.ceu.edge.hsync);
  TEST_ASSERT_EQ(k_ra8_ceu_edge_rising, cfg.ceu.edge.vsync);
  TEST_ASSERT_EQ(k_ra8_ceu_edge_rising, cfg.ceu.edge.field);
  TEST_ASSERT(!cfg.ceu.byte_swap.swap_8_bit);
  TEST_ASSERT(cfg.ceu.byte_swap.swap_16_bit);
  TEST_ASSERT(cfg.ceu.byte_swap.swap_32_bit);
  TEST_ASSERT_EQ(640U, cfg.ceu.scale.h_output_clip);
  TEST_ASSERT_EQ(480U, cfg.ceu.scale.v_output_clip);
  TEST_ASSERT_EQ(0U, cfg.ceu.image_area_size);
  TEST_ASSERT_EQ(1280U, cfg.stride_bytes);
  TEST_ASSERT_EQ((uint32_t)k_camera_uyvy_bytes, cfg.frame_bytes_max);
  TEST_END("camera mode: packed UYVY descriptor");
}

/**
 * @test test_camera_jpeg_gates_on_caller_capacity
 * @brief Sensor JPEG fetches data-enable and firewalls the caller's buffer.
 * @pre None.
 * @post No hardware state changes.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_camera_jpeg_gates_on_caller_capacity(void)
{
  TEST_BEGIN("camera mode: sensor JPEG descriptor");
  ra8_board_camera_ceu_config_t cfg = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_board_camera_get_ceu_config(k_ra8_board_camera_mode_vga_jpeg,
                                                 (uint32_t)k_camera_jpeg_bytes, &cfg));
  TEST_ASSERT_EQ(k_ra8_ceu_fmt_data_enable, cfg.ceu.capture_format);
  TEST_ASSERT_EQ(k_ra8_ceu_burst_256, cfg.ceu.burst_mode);
  TEST_ASSERT_EQ(1U, cfg.ceu.bytes_per_pixel);
  TEST_ASSERT_EQ(640U, cfg.ceu.dst_stride);
  TEST_ASSERT(cfg.ceu.byte_swap.swap_8_bit);
  TEST_ASSERT(cfg.ceu.byte_swap.swap_16_bit);
  TEST_ASSERT(cfg.ceu.byte_swap.swap_32_bit);
  TEST_ASSERT_EQ((uint32_t)k_camera_jpeg_bytes, cfg.ceu.image_area_size);
  TEST_ASSERT_EQ(0U, cfg.stride_bytes);
  TEST_ASSERT_EQ((uint32_t)k_camera_jpeg_bytes, cfg.frame_bytes_max);
  TEST_ASSERT_EQ(0U, cfg.ceu.scale.h_output_clip);
  TEST_END("camera mode: sensor JPEG descriptor");
}

/**
 * @test test_camera_publishes_shared_sensor_timing
 * @brief Both modes publish the same sensor clock and settle delay.
 * @pre None.
 * @post No hardware state changes.
 * @note Single-threaded host test.
 * @since 0.1.0
 */
static void test_camera_publishes_shared_sensor_timing(void)
{
  TEST_BEGIN("camera mode: shared sensor timing");
  ra8_board_camera_ceu_config_t packed = {};
  ra8_board_camera_ceu_config_t gated  = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_board_camera_get_ceu_config(k_ra8_board_camera_mode_vga_uyvy,
                                                 (uint32_t)k_camera_uyvy_bytes, &packed));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_board_camera_get_ceu_config(k_ra8_board_camera_mode_vga_jpeg,
                                                 (uint32_t)k_camera_jpeg_bytes, &gated));
  TEST_ASSERT_EQ(24000000U, packed.xclk_hz);
  TEST_ASSERT_EQ(packed.xclk_hz, gated.xclk_hz);
  TEST_ASSERT_EQ(100U, packed.settle_ms);
  TEST_ASSERT_EQ(packed.settle_ms, gated.settle_ms);
  TEST_ASSERT_EQ(5U, packed.poll_interval_ms);
  TEST_ASSERT_EQ(800U, packed.poll_attempts);
  TEST_ASSERT_EQ(2U, gated.poll_interval_ms);
  TEST_ASSERT_EQ(2000U, gated.poll_attempts);
  TEST_ASSERT_EQ(640U, packed.width_px);
  TEST_ASSERT_EQ(480U, gated.height_px);
  TEST_END("camera mode: shared sensor timing");
}

/* -------------------------------------------------------------------------
 * Entry point
 * -------------------------------------------------------------------------
 */

/**
 * @brief Test binary entry point.
 * @return 0 on success; a failing assertion exits non-zero first.
 * @pre None.
 * @post Every case above has run in order.
 * @note Not thread-safe; single-threaded test runner.
 * @since 0.1.0
 */
int main(void)
{
  test_camera_rejects_null_out();
  test_camera_rejects_unknown_mode();
  test_camera_rejects_zero_capacity();
  test_camera_rejects_short_packed_buffer();
  test_camera_uyvy_matches_bench_descriptor();
  test_camera_jpeg_gates_on_caller_capacity();
  test_camera_publishes_shared_sensor_timing();
  return 0;
}
