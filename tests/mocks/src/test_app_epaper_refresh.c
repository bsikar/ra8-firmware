/**
 * @file test_app_epaper_refresh.c
 * @brief Integration test: epaper_refresh demo logic (pure)
 *
 * @details
 * Mirrors examples/ek_ra8d2/hw_pending/epaper_refresh/src/main.c. The IT8951 SPI
 * sequencing is owned + covered by ra8_epaper's own unit tests and the display
 * PAL e-ink backend by test_ra8_display_pal.c; this test pins down the
 * app-level pure logic:
 *
 *  - the checkerboard test-pattern colour,
 *  - the partial-region bounds guard ``ep_region_fits`` with full MC/DC,
 *  - the PASS verdict ``ep_flush_ok`` with full MC/DC,
 *  - the decimal formatter used for the UART timing report.
 *
 * No MMIO is required, so this test runs in-process.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum t_ep_geom_t
 * @brief Mirror of the app's geometry + region constants.
 */
typedef enum : uint16_t {
  k_t_ep_panel_w = 128U, /**< Panel width.              */
  k_t_ep_panel_h = 128U, /**< Panel height.             */
  k_t_ep_cell    = 16U,  /**< Checkerboard cell size.   */
  k_t_ep_box_x   = 48U,  /**< Partial-region left edge. */
  k_t_ep_box_y   = 48U,  /**< Partial-region top edge.  */
  k_t_ep_box_w   = 32U,  /**< Partial-region width.     */
  k_t_ep_box_h   = 32U,  /**< Partial-region height.    */
} t_ep_geom_t;

/**
 * @enum t_ep_color_t
 * @brief Mirror of the app's RGB565 shades.
 */
typedef enum : uint16_t {
  k_t_ep_white = 0xFFFFU, /**< Even checker cell. */
  k_t_ep_black = 0x0000U, /**< Odd checker cell.  */
} t_ep_color_t;

/** @brief Decimal-format sizing (mirror of the app). */
typedef enum : uint8_t {
  k_t_ep_dec_digits = 10U, /**< T ep dec digits. */
  k_t_ep_radix      = 10U, /**< T ep radix.      */
} t_ep_fmt_t;

/** @brief Mirror of ep_checker_px(). */
static uint16_t ep_checker_px(uint16_t x, uint16_t y)
{
  const uint32_t cell = (uint32_t)k_t_ep_cell;
  const uint32_t sum  = ((uint32_t)x / cell) + ((uint32_t)y / cell);
  return ((sum & 1U) != 0U) ? (uint16_t)k_t_ep_black : (uint16_t)k_t_ep_white;
}

/** @brief Mirror of ep_region_fits() (two independent edge tests). */
static bool ep_region_fits(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t pw, uint16_t ph)
{
  return ((uint32_t)x + (uint32_t)w <= (uint32_t)pw) && ((uint32_t)y + (uint32_t)h <= (uint32_t)ph);
}

/** @brief Mirror of ep_flush_ok(). */
static bool ep_flush_ok(ra8_err_t init_err, ra8_err_t full_err, ra8_err_t part_err)
{
  return (init_err == k_ra8_ok) && (full_err == k_ra8_ok) && (part_err == k_ra8_ok);
}

/** @brief Mirror of ep_u32_to_dec(). */
static uint32_t ep_u32_to_dec(uint8_t* buf, uint32_t val)
{
  if (val == 0U) {
    buf[0] = (uint8_t)'0';
    return 1U;
  }
  uint8_t  tmp[k_t_ep_dec_digits];
  uint32_t n = 0U;
  uint32_t v = val;
  while ((v != 0U) && (n < (uint32_t)k_t_ep_dec_digits)) {
    tmp[n] = (uint8_t)('0' + (uint8_t)(v % (uint32_t)k_t_ep_radix));
    v      = v / (uint32_t)k_t_ep_radix;
    n++;
  }
  for (uint32_t i = 0U; i < n; i++) {
    buf[i] = tmp[n - 1U - i];
  }
  return n;
}

/**
 * @brief The checkerboard colour alternates per cell and is stable within one.
 *
 * @par MC/DC:
 * No compound decision; the single ``(sum & 1)`` parity test is exercised both
 * ways (even cell -> white, odd cell -> black) plus intra-cell stability.
 */
static void test_ep_checker_pattern(void)
{
  TEST_BEGIN("epaper_refresh: checkerboard pattern");
  /* Cell (0,0): sum 0, even -> white; whole cell is white. */
  TEST_ASSERT_EQ(k_t_ep_white, ep_checker_px(0U, 0U));
  TEST_ASSERT_EQ(k_t_ep_white, ep_checker_px(15U, 15U));
  /* Cell (1,0): sum 1, odd -> black. */
  TEST_ASSERT_EQ(k_t_ep_black, ep_checker_px(16U, 0U));
  /* Cell (0,1): sum 1, odd -> black. */
  TEST_ASSERT_EQ(k_t_ep_black, ep_checker_px(0U, 16U));
  /* Cell (1,1): sum 2, even -> white. */
  TEST_ASSERT_EQ(k_t_ep_white, ep_checker_px(16U, 16U));
  /* Partial-region origin cell (3,3): sum 6, even -> white. */
  TEST_ASSERT_EQ(k_t_ep_white, ep_checker_px((uint16_t)k_t_ep_box_x, (uint16_t)k_t_ep_box_y));
  TEST_END("epaper_refresh: checkerboard pattern");
}

/**
 * @brief The bounds guard admits an on-panel rect and rejects an oversized one.
 *
 * @par MC/DC:
 * Decision: ``(x+w <= pw) && (y+h <= ph)`` (2 conditions, A and B).
 * - Vector 1: x+w=80<=128, y+h=80<=128 -> true  (control: A=T, B=T)
 * - Vector 2: x+w=200>128, y+h=80<=128 -> false (varies A only)
 * - Vector 3: x+w=80<=128, y+h=200>128 -> false (varies B only)
 * Vectors 1+2 prove A independently affects the outcome; 1+3 prove the same
 * for B. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 */
static void test_ep_region_fits_mcdc(void)
{
  TEST_BEGIN("epaper_refresh: region-fits MC/DC");
  /* Control: the demo's 32x32 box at (48,48) fits the 128x128 panel. */
  TEST_ASSERT(ep_region_fits((uint16_t)k_t_ep_box_x,
                             (uint16_t)k_t_ep_box_y,
                             (uint16_t)k_t_ep_box_w,
                             (uint16_t)k_t_ep_box_h,
                             (uint16_t)k_t_ep_panel_w,
                             (uint16_t)k_t_ep_panel_h));
  /* Vary A (x axis) false: width overruns the panel. */
  TEST_ASSERT(
    !ep_region_fits(120U, 48U, 80U, 32U, (uint16_t)k_t_ep_panel_w, (uint16_t)k_t_ep_panel_h));
  /* Vary B (y axis) false: height overruns the panel. */
  TEST_ASSERT(
    !ep_region_fits(48U, 120U, 32U, 80U, (uint16_t)k_t_ep_panel_w, (uint16_t)k_t_ep_panel_h));
  TEST_END("epaper_refresh: region-fits MC/DC");
}

/**
 * @brief The PASS verdict is true only when all three stages returned ok.
 *
 * @par MC/DC:
 * Decision: ``(init==ok) && (full==ok) && (part==ok)`` (3 conditions A/B/C).
 * - Vector 1: ok, ok, ok             -> true  (control)
 * - Vector 2: invalid_arg, ok, ok    -> false (varies A only)
 * - Vector 3: ok, invalid_arg, ok    -> false (varies B only)
 * - Vector 4: ok, ok, invalid_arg    -> false (varies C only)
 * Vectors 1+2/1+3/1+4 prove A/B/C each independently affect the outcome.
 * N+1 = 4 vectors for N=3 conditions: minimal MC/DC.
 */
static void test_ep_flush_ok_mcdc(void)
{
  TEST_BEGIN("epaper_refresh: flush-ok verdict MC/DC");
  TEST_ASSERT(ep_flush_ok(k_ra8_ok, k_ra8_ok, k_ra8_ok));               /* control */
  TEST_ASSERT(!ep_flush_ok(k_ra8_err_invalid_arg, k_ra8_ok, k_ra8_ok)); /* vary A  */
  TEST_ASSERT(!ep_flush_ok(k_ra8_ok, k_ra8_err_invalid_arg, k_ra8_ok)); /* vary B  */
  TEST_ASSERT(!ep_flush_ok(k_ra8_ok, k_ra8_ok, k_ra8_err_invalid_arg)); /* vary C  */
  TEST_END("epaper_refresh: flush-ok verdict MC/DC");
}

/**
 * @brief The decimal formatter renders 0, the full-refresh and partial pixel
 *        counts correctly.
 *
 * @par MC/DC:
 * No compound decision; the ``val == 0`` early-out and the digit loop are both
 * exercised (0 and multi-digit values).
 */
static void test_ep_u32_to_dec(void)
{
  TEST_BEGIN("epaper_refresh: decimal formatter");
  uint8_t  buf[k_t_ep_dec_digits + 1U];
  uint32_t n = ep_u32_to_dec(buf, 0U);
  buf[n]     = (uint8_t)'\0';
  TEST_ASSERT_EQ(1, n);
  TEST_ASSERT_EQ(0, strcmp((const char*)buf, "0"));

  n      = ep_u32_to_dec(buf, (uint32_t)k_t_ep_panel_w * (uint32_t)k_t_ep_panel_h); /* 16384 */
  buf[n] = (uint8_t)'\0';
  TEST_ASSERT_EQ(0, strcmp((const char*)buf, "16384"));

  n      = ep_u32_to_dec(buf, (uint32_t)k_t_ep_box_w * (uint32_t)k_t_ep_box_h); /* 1024 */
  buf[n] = (uint8_t)'\0';
  TEST_ASSERT_EQ(0, strcmp((const char*)buf, "1024"));
  TEST_END("epaper_refresh: decimal formatter");
}

int main(void)
{
  test_ep_checker_pattern();
  test_ep_region_fits_mcdc();
  test_ep_flush_ok_mcdc();
  test_ep_u32_to_dec();
  return 0;
}
