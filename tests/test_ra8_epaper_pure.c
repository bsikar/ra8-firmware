/**
 * @file test_ra8_epaper_pure.c
 * @brief MC/DC and layout tests for the pure half of the IT8951 driver
 *
 * @details
 * ``ra8_epaper_geom.c`` and ``ra8_epaper_devinfo.c`` hold the parts of the
 * driver that are total functions over their arguments: pixel-format
 * sizing, the 32-pixel 1 bpp alignment grid, the waveform-map lookup,
 * descriptor validation, the ``GET_DEV_INFO`` decode, and the reported-vs-
 * configured geometry check. None of them touch a bus, a GPIO or driver
 * state, so none of them need the SPI simulator fixture that
 * ``test_ra8_epaper.c`` builds -- they are called directly here.
 *
 * That separation is the point rather than a convenience. Several of these
 * decisions cannot be varied at all from behind the bus: the host SPI
 * loopback reports 0xFFFF for every read, so a "reported geometry agrees
 * with the descriptor" case does not exist on the driven path, and the
 * cov TU's renamed symbols do not count toward the production build's
 * MC/DC. Testing the extracted predicates is what makes the vectors
 * expressible.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_epaper.h"
#include "ra8_err.h"
#include "ra8_spi_bus_ops.h"
#include "unity_minimal.h"

/**
 * @enum ra8_epaper_pure_const_t
 * @brief Fixtures for the pure-helper vectors.
 */
typedef enum : uint32_t {
  k_ra8_epaper_test_panel_w       = 800U,        /**< Descriptor panel width.   */
  k_ra8_epaper_test_panel_h       = 600U,        /**< Descriptor panel height.  */
  k_ra8_epaper_test_devinfo_words = 20U,         /**< GET_DEV_INFO word count.  */
  k_ra8_epaper_test_buf_lo        = 0x1234U,     /**< Image-buffer base, low.   */
  k_ra8_epaper_test_buf_hi        = 0x0012U,     /**< Image-buffer base, high.  */
  k_ra8_epaper_test_buf_base      = 0x00121234U, /**< Reassembled base addr.    */
  k_ra8_epaper_test_lut_w0        = 0x4D36U,     /**< "M6" packed high-first.   */
  k_ra8_epaper_test_lut_w1        = 0x3431U,     /**< "41" packed high-first.   */
  k_ra8_epaper_test_ctrl_word     = 0x0107U,     /**< Two non-printable bytes.  */
  k_ra8_epaper_test_a2_m641       = 4U,          /**< A2 mode on the M641 LUT.  */
  k_ra8_epaper_test_panel_64      = 64U,         /**< Grid-aligned panel width. */
  k_ra8_epaper_test_panel_40      = 40U,         /**< Off-grid panel width.     */
  k_ra8_epaper_test_panel_32      = 32U,         /**< One-grid-cell panel.      */
  k_ra8_epaper_test_lut_word0     = 12U,         /**< First LUT-name word slot
                                                      in the GET_DEV_INFO
                                                      descriptor.             */
  k_ra8_epaper_test_lut_word1     = 13U,         /**< Second LUT-name word.   */
  k_ra8_epaper_test_offgrid_w     = 40U,         /**< Span that clamps to an
                                                      off-grid width.         */
} ra8_epaper_pure_const_t;

/**
 * @brief Inert byte-shifter so a descriptor has a bound bus seam.
 *
 * @details
 * ``ra8_epaper_validate_cfg`` only checks that the seam is bound, and no
 * test in this TU drives a transfer, so this is never called. It exists
 * to make the descriptor legal.
 *
 * @param[in]  ctx Ignored seam cookie.
 * @param[in]  tx  Ignored transmit byte.
 * @param[out] rx  Optional receive slot; zeroed when non-null.
 *
 * @return ``k_ra8_ok`` always.
 * @retval k_ra8_ok Transfer accepted.
 *
 * @pre  Never reached by any vector in this TU.
 * @pre  @p rx is either null or writable.
 * @post No hardware is touched.
 * @post ``*rx`` is zero when @p rx is non-null.
 *
 * @note Thread-safe: holds no state.
 *
 * @since 0.1.0
 */
static ra8_err_t pure_bus_xfer8(void* ctx, uint8_t tx, uint8_t* rx)
{
  (void)ctx;
  (void)tx;
  if (rx != nullptr) {
    *rx = 0U;
  }
  return k_ra8_ok;
}

/** @brief Build a descriptor that is valid apart from what a vector changes. */
static ra8_epaper_cfg_t make_cfg(void)
{
  const ra8_epaper_cfg_t cfg = {
    .bus          = {.xfer8 = pure_bus_xfer8, .ctx = nullptr},
    .waveform     = {.init = 0U, .du = 1U, .gc16 = 2U, .a2 = 4U},
    .reset_pin    = 0U,
    .busy_pin     = 0U,
    .panel_width  = (uint16_t)k_ra8_epaper_test_panel_w,
    .panel_height = (uint16_t)k_ra8_epaper_test_panel_h,
  };
  return cfg;
}

/**
 * @test epaper_decode_dev_info_layout
 *
 * @details
 * Pins the 20-word ``GET_DEV_INFO`` layout as a pure decode, with no
 * controller and no bus. This is the seam that decides which waveform
 * mode number A2 refreshes use: ``lut_version`` feeds
 * ``ra8_epaper_waveform_cfg_for_lut``, so a decode that slipped a word
 * would silently select the wrong A2 mode on the real panel while every
 * bus-level test still passed.
 *
 * Also covers the two argument guards and the short-response refusal, and
 * asserts that a non-printable version byte is dropped to NUL rather than
 * reaching a log as a control character.
 */
static void test_decode_dev_info_layout(void)
{
  TEST_BEGIN("epaper: GET_DEV_INFO decode layout + guards");

  uint16_t words[k_ra8_epaper_test_devinfo_words] = {};
  words[0]                                        = (uint16_t)k_ra8_epaper_test_panel_w;
  words[1]                                        = (uint16_t)k_ra8_epaper_test_panel_h;
  words[2]                                        = (uint16_t)k_ra8_epaper_test_buf_lo;
  words[3]                                        = (uint16_t)k_ra8_epaper_test_buf_hi;
  /* "M641" in the LUT slot (word 12), two chars per word, high byte first. */
  words[k_ra8_epaper_test_lut_word0] = (uint16_t)k_ra8_epaper_test_lut_w0;
  words[k_ra8_epaper_test_lut_word1] = (uint16_t)k_ra8_epaper_test_lut_w1;
  /* A control byte in the firmware slot must not survive the decode. */
  words[4] = (uint16_t)k_ra8_epaper_test_ctrl_word;

  ra8_epaper_dev_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_epaper_decode_dev_info(nullptr, k_ra8_epaper_test_devinfo_words, &info));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_epaper_decode_dev_info(words, k_ra8_epaper_test_devinfo_words, nullptr));
  /* One word short of the fixed layout is refused rather than decoded from
   * whatever follows the buffer. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_epaper_decode_dev_info(words, k_ra8_epaper_test_devinfo_words - 1U, &info));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_epaper_decode_dev_info(words, k_ra8_epaper_test_devinfo_words, &info));
  TEST_ASSERT_EQ(k_ra8_epaper_test_panel_w, info.panel_width);
  TEST_ASSERT_EQ(k_ra8_epaper_test_panel_h, info.panel_height);
  /* The base address is assembled from two halves, high word shifted up. */
  TEST_ASSERT_EQ(k_ra8_epaper_test_buf_base, info.image_buf_base);
  TEST_ASSERT_EQ(0, strcmp(info.lut_version, "M641"));
  /* The control byte was dropped to NUL, terminating the string early. */
  TEST_ASSERT_EQ(0, strcmp(info.fw_version, ""));

  /* The decoded LUT string drives the waveform map, which is the whole
   * reason this decode is worth pinning. */
  ra8_epaper_waveform_cfg_t wf = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_waveform_cfg_for_lut(info.lut_version, &wf));
  TEST_ASSERT_EQ(k_ra8_epaper_test_a2_m641, wf.a2);

  TEST_END("epaper: GET_DEV_INFO decode layout + guards");
}

/**
 * @test epaper_geom_mcdc
 *
 * @par MC/DC:
 * Decision A libs/ra8_hal/src/ra8_epaper_geom.c@internal_ra8_epaper_validate_waveform:
 * ``if ((wf->du == 0) || (wf->gc16 == 0) || (wf->a2 == 0))``
 * (3 conditions, ``||`` short-circuit chain) -- N+1 = 4 vectors:
 * - Vector A1: du=1, gc16=2, a2=4 -> false (control: all three non-zero)
 * - Vector A2: du=0, gc16=2, a2=4 -> true  (varies du only)
 * - Vector A3: du=1, gc16=0, a2=4 -> true  (varies gc16 only)
 * - Vector A4: du=1, gc16=2, a2=0 -> true  (varies a2 only)
 *
 * Decision B, same function:
 * ``if ((init > MAX) || (du > MAX) || (gc16 > MAX) || (a2 > MAX))``
 * (4 conditions) -- N+1 = 5 vectors. Every B vector keeps du/gc16/a2
 * non-zero, because decision A short-circuits to a return otherwise and
 * decision B is never evaluated:
 * - Vector B1: all <= MAX               -> false (control)
 * - Vector B2: init > MAX               -> true  (varies init only)
 * - Vector B3: du   > MAX               -> true  (varies du only)
 * - Vector B4: gc16 > MAX               -> true  (varies gc16 only)
 * - Vector B5: a2   > MAX               -> true  (varies a2 only)
 *
 * Decision C libs/ra8_hal/src/ra8_epaper_geom.c@ra8_epaper_image_bytes:
 * ``if ((area->width == 0) || (area->height == 0))`` -- N+1 = 3 vectors:
 * - Vector C1: w=8, h=8 -> false (control)
 * - Vector C2: w=0, h=8 -> true  (varies width only)
 * - Vector C3: w=8, h=0 -> true  (varies height only)
 *
 * @details
 * Decision A is the one that matters on a real panel. Mode 0 is INIT on
 * every documented LUT, so a descriptor that left the map zeroed would
 * not fail -- it would refresh every mode as a full INIT flash, which
 * reads as "this panel is slow" rather than "this board descriptor is
 * wrong". Each condition therefore has to be shown to reject on its own.
 */
static void test_geom_mcdc(void)
{
  TEST_BEGIN("epaper MC/DC: waveform validation 3-cond + 4-cond, image_bytes 2-cond");

  ra8_epaper_cfg_t cfg = make_cfg();

  /* Decision A. */
  cfg.waveform = (ra8_epaper_waveform_cfg_t){.init = 0U, .du = 1U, .gc16 = 2U, .a2 = 4U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_validate_cfg(&cfg));
  cfg.waveform = (ra8_epaper_waveform_cfg_t){.init = 0U, .du = 0U, .gc16 = 2U, .a2 = 4U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_validate_cfg(&cfg));
  cfg.waveform = (ra8_epaper_waveform_cfg_t){.init = 0U, .du = 1U, .gc16 = 0U, .a2 = 4U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_validate_cfg(&cfg));
  cfg.waveform = (ra8_epaper_waveform_cfg_t){.init = 0U, .du = 1U, .gc16 = 2U, .a2 = 0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_validate_cfg(&cfg));

  /* Decision B -- all four kept non-zero so decision A falls through. */
  const uint8_t over = (uint8_t)k_ra8_epaper_wf_mode_max + 1U;
  cfg.waveform       = (ra8_epaper_waveform_cfg_t){.init = 0U, .du = 1U, .gc16 = 2U, .a2 = 4U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_validate_cfg(&cfg));
  cfg.waveform = (ra8_epaper_waveform_cfg_t){.init = over, .du = 1U, .gc16 = 2U, .a2 = 4U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_validate_cfg(&cfg));
  cfg.waveform = (ra8_epaper_waveform_cfg_t){.init = 0U, .du = over, .gc16 = 2U, .a2 = 4U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_validate_cfg(&cfg));
  cfg.waveform = (ra8_epaper_waveform_cfg_t){.init = 0U, .du = 1U, .gc16 = over, .a2 = 4U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_validate_cfg(&cfg));
  cfg.waveform = (ra8_epaper_waveform_cfg_t){.init = 0U, .du = 1U, .gc16 = 2U, .a2 = over};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_validate_cfg(&cfg));

  /* The null guard on the newly-public validator. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epaper_validate_cfg(nullptr));

  /* Decision C. */
  size_t                  bytes = 0U;
  const ra8_epaper_area_t ok    = {.x = 0U, .y = 0U, .width = 8U, .height = 8U};
  const ra8_epaper_area_t no_w  = {.x = 0U, .y = 0U, .width = 0U, .height = 8U};
  const ra8_epaper_area_t no_h  = {.x = 0U, .y = 0U, .width = 8U, .height = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_image_bytes(&ok, k_ra8_epaper_pf_8bpp, &bytes));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_epaper_image_bytes(&no_w, k_ra8_epaper_pf_8bpp, &bytes));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_epaper_image_bytes(&no_h, k_ra8_epaper_pf_8bpp, &bytes));

  TEST_END("epaper MC/DC: waveform validation 3-cond + 4-cond, image_bytes 2-cond");
}

/**
 * @test epaper_align_area_clamp_mcdc
 *
 * @par MC/DC:
 * Decision libs/ra8_hal/src/ra8_epaper_geom.c@ra8_epaper_align_area:
 * ``if ((x_hi <= x_lo) || (((x_hi - x_lo) % grid) != 0))``
 * (2 conditions, ``||``) -- N+1 = 3 vectors:
 * - Vector 1: panel 64, area x=0 w=32  -> x_lo=0,  x_hi=32 -> false, false
 * - Vector 2: panel 32, area x=32 w=0  -> x_lo=32, x_hi=32 -> true  (varies
 *   the first condition only; the second is not evaluated)
 * - Vector 3: panel 40, area x=0 w=40  -> x_lo=0,  x_hi=40 -> first false,
 *   second true (40 % 32 = 8), varying the second condition only
 *
 * @details
 * This is the branch that reports "no aligned superset exists". It is
 * reachable only through two degenerate shapes, which is exactly why it
 * needs pinning rather than trusting: vector 2 is a zero-width window
 * starting on the grid, and vector 3 is a panel whose own width is not a
 * multiple of the 32-pixel 1 bpp grid, so growing outward would run off
 * the panel. Both must be refused rather than clamped to a misaligned
 * window -- a misaligned 1 bpp update renders nothing at all while the
 * controller reports success.
 */
static void test_align_area_clamp_mcdc(void)
{
  TEST_BEGIN("epaper MC/DC: align_area clamp 2-condition");

  /* Vector 1 -- both conditions false: the window aligns cleanly. */
  ra8_epaper_area_t v1 = {.x = 0U, .y = 0U, .width = 32U, .height = 1U};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_epaper_align_area(&v1, k_ra8_epaper_pf_1bpp, k_ra8_epaper_test_panel_64));
  TEST_ASSERT_EQ(0U, v1.x);
  TEST_ASSERT_EQ(32U, v1.width);

  /* Vector 2 -- x_hi == x_lo: a zero-width window that starts on the grid
   * and is clamped to the panel edge. */
  ra8_epaper_area_t v2 = {.x = 32U, .y = 0U, .width = 0U, .height = 1U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_epaper_align_area(&v2, k_ra8_epaper_pf_1bpp, k_ra8_epaper_test_panel_32));

  /* Vector 3 -- x_hi > x_lo but the clamped span is off-grid. */
  ra8_epaper_area_t v3 = {
    .x = 0U, .y = 0U, .width = k_ra8_epaper_test_offgrid_w, .height = 1U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_epaper_align_area(&v3, k_ra8_epaper_pf_1bpp, k_ra8_epaper_test_panel_40));

  TEST_END("epaper MC/DC: align_area clamp 2-condition");
}

/**
 * @test epaper_geometry_agrees_mcdc
 *
 * @par MC/DC:
 * Decision libs/ra8_hal/src/ra8_epaper_geom.c@ra8_epaper_geometry_agrees:
 * ``return (info->panel_width == cfg->panel_width) &&
 *         (info->panel_height == cfg->panel_height)``
 * (2 conditions, ``&&``) -- N+1 = 3 vectors:
 * - Vector 1: w match, h match    -> true  (control: both conditions true)
 * - Vector 2: w differs, h match  -> false (varies width only)
 * - Vector 3: w match, h differs  -> false (varies height only; reachable
 *   only because the width still matches, so the chain does not
 *   short-circuit)
 *
 * @details
 * This predicate is why ``ra8_epaper_init`` warns instead of failing when
 * the controller's reported geometry disagrees with the descriptor: a
 * controller that has not finished loading its waveform answers the first
 * GET_DEV_INFO with zeroes, and an app may legitimately drive a
 * sub-window of a larger panel.
 *
 * It is tested here rather than through ``init`` because inside ``init``
 * the reported dimensions come off the bus, and the host fixture's
 * loopback reports 0xFFFF -- far above the descriptor size ceiling, so
 * the width comparison always short-circuits and the "agrees" case is
 * unreachable. Extracting the comparison is what makes all three vectors
 * expressible.
 */
static void test_geometry_agrees_mcdc(void)
{
  TEST_BEGIN("epaper MC/DC: init geometry cross-check 2-condition");

  ra8_epaper_cfg_t cfg = make_cfg();
  cfg.panel_width      = (uint16_t)k_ra8_epaper_test_panel_w;
  cfg.panel_height     = (uint16_t)k_ra8_epaper_test_panel_h;

  ra8_epaper_dev_info_t info = {};

  /* Vector 1 -- both dimensions agree. */
  info.panel_width  = (uint16_t)k_ra8_epaper_test_panel_w;
  info.panel_height = (uint16_t)k_ra8_epaper_test_panel_h;
  TEST_ASSERT_EQ(true, ra8_epaper_geometry_agrees(&info, &cfg));

  /* Vector 2 -- width disagrees; the chain short-circuits. */
  info.panel_width  = (uint16_t)k_ra8_epaper_test_panel_w + 1U;
  info.panel_height = (uint16_t)k_ra8_epaper_test_panel_h;
  TEST_ASSERT_EQ(false, ra8_epaper_geometry_agrees(&info, &cfg));

  /* Vector 3 -- width agrees so the height comparison is evaluated. */
  info.panel_width  = (uint16_t)k_ra8_epaper_test_panel_w;
  info.panel_height = (uint16_t)k_ra8_epaper_test_panel_h + 1U;
  TEST_ASSERT_EQ(false, ra8_epaper_geometry_agrees(&info, &cfg));

  /* A NULL on either side reports disagreement rather than faulting, so a
   * caller that never ran a successful GET_DEV_INFO cannot read the check
   * as agreement. */
  TEST_ASSERT_EQ(false, ra8_epaper_geometry_agrees(nullptr, &cfg));
  TEST_ASSERT_EQ(false, ra8_epaper_geometry_agrees(&info, nullptr));

  TEST_END("epaper MC/DC: init geometry cross-check 2-condition");
}

int main(void)
{
  test_decode_dev_info_layout();
  test_geom_mcdc();
  test_align_area_clamp_mcdc();
  test_geometry_agrees_mcdc();
  return 0;
}
