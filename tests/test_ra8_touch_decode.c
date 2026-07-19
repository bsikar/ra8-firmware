/**
 * @file test_ra8_touch_decode.c
 * @brief Unit tests for the ra8_touch (GT911) wire-format decode parser
 *
 * @details
 * Exercises the GT911 8-byte point-record parser in isolation through
 * the ``ra8_touch_test_decode`` hook: single/multi-point decoding, the
 * two emit clamps (caller cap and 5-contact hardware limit), and the
 * hook's own argument validation. No bus traffic is involved -- the
 * parser runs against caller-built byte buffers, so this TU needs no
 * simulator priming. Driver lifecycle / bus-path tests live in
 * ``test_ra8_touch.c``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_touch.h"
#include "ra8_touch_gt911_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_decode_t
 * @brief Byte mask and the point-array capacity.
 */
typedef enum : uint8_t {
  k_t_byte_mask = 0xFFU, /**< Low-byte mask while packing a raw report field. */
  k_t_point_cap = 5U,    /**< Points the caller's array can hold.             */
} t_decode_t;

/**
 * @enum ra8_touch_decode_test_const_t
 * @brief Test-only coordinate constants (no magic numbers).
 */
typedef enum : uint16_t {
  k_test_max_points_default = 5U,      /**< Caller cap matching the hw limit. */
  k_test_x_one              = 0x0123U, /**< X coordinate, single-point case.  */
  k_test_y_one              = 0x0456U, /**< Y coordinate, single-point case.  */
  k_test_x_two              = 0x07F0U, /**< X coordinate, second contact.     */
  k_test_y_two              = 0x0218U, /**< Y coordinate, second contact.     */
  k_test_x_three            = 0x0099U, /**< X coordinate, third contact.      */
  k_test_y_three            = 0x012CU, /**< Y coordinate, third contact.      */
  k_test_x_five_a           = 0x0001U, /**< X seed for generated contacts.    */
  k_test_y_five_a           = 0x0002U, /**< Y seed for generated contacts.    */
  k_test_decode_hw_over     = 7U,      /**< n_points/max_count above 5.       */
} ra8_touch_decode_test_const_t;

/**
 * @enum ra8_touch_decode_test_val_t
 * @brief Byte-sized test constants (tracks, pressures, shifts).
 */
typedef enum : uint8_t {
  k_test_pressure_one = 0x40U, /**< Contact size (pressure), variant one. */
  k_test_pressure_two = 0x80U, /**< Contact size (pressure), variant two. */
  k_test_track_zero   = 0U,    /**< Persistent contact id 0.              */
  k_test_track_one    = 1U,    /**< Persistent contact id 1.              */
  k_test_track_two    = 2U,    /**< Persistent contact id 2.              */
  k_test_byte_shift   = 8U,    /**< Bits per byte for LE16 packing.       */
} ra8_touch_decode_test_val_t;

/**
 * @brief Pack an unsigned 16-bit value into LSB-first byte order.
 */
static void pack_le16(uint16_t v, uint8_t* out)
{
  out[0] = (uint8_t)(v & k_t_byte_mask);
  out[1] = (uint8_t)((uint32_t)v >> (uint32_t)k_test_byte_shift);
}

/**
 * @brief Build one GT911 8-byte point record.
 */
static void build_point(uint8_t* buf, uint8_t track, uint16_t x, uint16_t y, uint16_t size)
{
  buf[k_ra8_touch_gt911_point_off_track] = track;
  pack_le16(x, &buf[k_ra8_touch_gt911_point_off_x_lsb]);
  pack_le16(y, &buf[k_ra8_touch_gt911_point_off_y_lsb]);
  pack_le16(size, &buf[k_ra8_touch_gt911_point_off_size_lsb]);
  buf[k_ra8_touch_gt911_point_off_reserved] = 0U;
}

/* =============================================================================
 * Decode parser (test_decode hook)
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_decode_one_point(void)
{
  TEST_BEGIN("ra8_touch_test_decode: single point");
  uint8_t raw[(uint32_t)k_ra8_touch_gt911_point_bytes] = {};
  build_point(raw,
              (uint8_t)k_test_track_zero,
              (uint16_t)k_test_x_one,
              (uint16_t)k_test_y_one,
              (uint16_t)k_test_pressure_one);

  ra8_touch_point_t pts[k_t_point_cap];
  uint8_t           got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_touch_test_decode(raw, 1U, pts, (uint8_t)k_test_max_points_default, &got));
  TEST_ASSERT_EQ(1, got);
  TEST_ASSERT_EQ(k_test_track_zero, pts[0].track_id);
  TEST_ASSERT_EQ(k_test_x_one, pts[0].x);
  TEST_ASSERT_EQ(k_test_y_one, pts[0].y);
  TEST_ASSERT_EQ(k_test_pressure_one, pts[0].pressure);
  TEST_END("ra8_touch_test_decode: single point");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_decode_three_points(void)
{
  TEST_BEGIN("ra8_touch_test_decode: three points");
  uint8_t raw[(uint32_t)k_ra8_touch_gt911_point_bytes * 3U] = {};
  build_point(&raw[(size_t)0U * (uint32_t)k_ra8_touch_gt911_point_bytes],
              (uint8_t)k_test_track_zero,
              (uint16_t)k_test_x_one,
              (uint16_t)k_test_y_one,
              (uint16_t)k_test_pressure_one);
  build_point(&raw[(size_t)1U * (uint32_t)k_ra8_touch_gt911_point_bytes],
              (uint8_t)k_test_track_one,
              (uint16_t)k_test_x_two,
              (uint16_t)k_test_y_two,
              (uint16_t)k_test_pressure_two);
  build_point(&raw[(size_t)2U * (uint32_t)k_ra8_touch_gt911_point_bytes],
              (uint8_t)k_test_track_two,
              (uint16_t)k_test_x_three,
              (uint16_t)k_test_y_three,
              (uint16_t)k_test_pressure_one);

  ra8_touch_point_t pts[k_t_point_cap];
  uint8_t           got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_touch_test_decode(raw, 3U, pts, (uint8_t)k_test_max_points_default, &got));
  TEST_ASSERT_EQ(3, got);
  TEST_ASSERT_EQ(k_test_x_one, pts[0].x);
  TEST_ASSERT_EQ(k_test_x_two, pts[1].x);
  TEST_ASSERT_EQ(k_test_y_three, pts[2].y);
  TEST_ASSERT_EQ(k_test_track_two, pts[2].track_id);
  TEST_END("ra8_touch_test_decode: three points");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_decode_five_points_max(void)
{
  TEST_BEGIN("ra8_touch_test_decode: five points (max)");
  uint8_t raw[(uint32_t)k_ra8_touch_gt911_point_bytes * k_t_point_cap] = {};
  for (uint8_t i = 0U; i < k_t_point_cap; i++) {
    build_point(&raw[(size_t)(uint32_t)i * (uint32_t)k_ra8_touch_gt911_point_bytes],
                i,
                (uint16_t)((uint32_t)k_test_x_five_a + i),
                (uint16_t)((uint32_t)k_test_y_five_a + i),
                (uint16_t)k_test_pressure_one);
  }

  ra8_touch_point_t pts[k_t_point_cap];
  uint8_t           got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_touch_test_decode(raw, 5U, pts, (uint8_t)k_test_max_points_default, &got));
  TEST_ASSERT_EQ(5, got);
  for (uint8_t i = 0U; i < k_t_point_cap; i++) {
    TEST_ASSERT_EQ(i, pts[i].track_id);
  }
  TEST_END("ra8_touch_test_decode: five points (max)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_decode_clamp_to_max_count(void)
{
  TEST_BEGIN("ra8_touch_test_decode: input > max_count clamps");
  uint8_t raw[(uint32_t)k_ra8_touch_gt911_point_bytes * 3U] = {};
  for (uint8_t i = 0U; i < 3U; i++) {
    build_point(&raw[(size_t)(uint32_t)i * (uint32_t)k_ra8_touch_gt911_point_bytes],
                i,
                (uint16_t)((uint32_t)k_test_x_five_a + i),
                (uint16_t)((uint32_t)k_test_y_five_a + i),
                (uint16_t)k_test_pressure_one);
  }
  ra8_touch_point_t pts[2U];
  uint8_t           got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_test_decode(raw, 3U, pts, 2U, &got));
  TEST_ASSERT_EQ(2, got);
  TEST_END("ra8_touch_test_decode: input > max_count clamps");
}

/**
 * @test test_decode_clamp_to_hw_max
 *
 * @par MC/DC:
 * (no compound decisions -- n_points and max_count both 7 leave emit at 7
 * past the ``emit > max_count`` clamp, so the single-condition
 * ``emit > k_ra8_touch_gt911_max_points`` clamp drops it to 5.)
 */
static void test_decode_clamp_to_hw_max(void)
{
  TEST_BEGIN("ra8_touch_test_decode: input > hw max clamps to 5");
  uint8_t raw[(uint32_t)k_ra8_touch_gt911_point_bytes * (uint32_t)k_test_decode_hw_over] = {};
  for (uint8_t i = 0U; i < (uint8_t)k_test_decode_hw_over; i++) {
    build_point(&raw[(size_t)(uint32_t)i * (uint32_t)k_ra8_touch_gt911_point_bytes],
                i,
                (uint16_t)((uint32_t)k_test_x_five_a + i),
                (uint16_t)((uint32_t)k_test_y_five_a + i),
                (uint16_t)k_test_pressure_one);
  }
  ra8_touch_point_t pts[(uint32_t)k_test_decode_hw_over];
  uint8_t           got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_touch_test_decode(raw,
                                       (uint8_t)k_test_decode_hw_over,
                                       pts,
                                       (uint8_t)k_test_decode_hw_over,
                                       &got));
  TEST_ASSERT_EQ(k_ra8_touch_gt911_max_points, got);
  TEST_END("ra8_touch_test_decode: input > hw max clamps to 5");
}

/**
 * @test test_decode_zero_max_count
 *
 * @par MC/DC:
 * (no compound decisions -- exercises the single-condition
 * ``max_count == 0`` rejection leg of the ra8_touch_test_decode hook.)
 */
static void test_decode_zero_max_count(void)
{
  TEST_BEGIN("ra8_touch_test_decode: max_count == 0 rejected");
  uint8_t           raw[(uint32_t)k_ra8_touch_gt911_point_bytes] = {};
  ra8_touch_point_t pts[1U];
  uint8_t           got = 3U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_test_decode(raw, 1U, pts, 0U, &got));
  TEST_ASSERT_EQ(0, got);
  TEST_END("ra8_touch_test_decode: max_count == 0 rejected");
}

/**
 * @test test_decode_null_args
 *
 * @par MC/DC:
 * (no compound decisions -- each ``RA8_CHECK_NULL_PTR`` guard of the
 * ra8_touch_test_decode hook is a single-condition decision; the three
 * null vectors here take each rejection leg, and every other decode
 * test in this file takes the pass-through legs.)
 */
static void test_decode_null_args(void)
{
  TEST_BEGIN("ra8_touch_test_decode: NULL args rejected");
  uint8_t           raw[(uint32_t)k_ra8_touch_gt911_point_bytes] = {};
  ra8_touch_point_t pts[1U];
  uint8_t           got = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_test_decode(nullptr, 1U, pts, 1U, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_test_decode(raw, 1U, nullptr, 1U, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_test_decode(raw, 1U, pts, 1U, nullptr));
  TEST_END("ra8_touch_test_decode: NULL args rejected");
}

int32_t main(void)
{
  test_decode_one_point();
  test_decode_three_points();
  test_decode_five_points_max();
  test_decode_clamp_to_max_count();
  test_decode_clamp_to_hw_max();
  test_decode_zero_max_count();
  test_decode_null_args();
  (void)fprintf(stderr, "[OK ] test_ra8_touch_decode.c\n");
  return 0;
}
