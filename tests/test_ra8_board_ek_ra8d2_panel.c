/**
 * @file test_ra8_board_ek_ra8d2_panel.c
 * @brief Unit tests for the board's exported panel-framebuffer contract
 *
 * @details
 * ``ra8_panel.h`` states two facts an application cannot check for itself: the
 * GLCDC's 64-byte AXI-burst alignment, and the shape of a full-panel RGB565
 * surface. Getting either wrong is not a compile error and not a runtime error
 * -- it is torn scanout -- so the facts are asserted here instead.
 *
 * ::RA8_BOARD_PANEL_FRAMEBUFFER is exercised by INSTANTIATING it, not by
 * reading its text: a macro that stopped applying the alignment attribute
 * would still expand, still compile, and still look right in review.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_panel.h"
#include "unity_minimal.h"

/**
 * @enum board_panel_fixture_t
 * @brief The panel facts as the datasheet and the GLCDC burst length give them.
 *
 * @details
 * Stated independently of the header on purpose: a test that re-derived these
 * would agree with whatever the header happened to hold, including a wrong
 * value.
 */
typedef enum : uint16_t {
  k_bp_align_bytes = 64U,   /**< GLCDC AXI burst length, bytes. */
  k_bp_panel_w     = 1024U, /**< ER-TFT070-6 native width.      */
  k_bp_panel_h     = 600U,  /**< ER-TFT070-6 native height.     */
} board_panel_fixture_t;

/** @brief Bytes per RGB565 pixel, so no bare 2 appears in a size expression. */
typedef enum : uint8_t {
  k_bp_bytes_per_px = 2U, /**< RGB565 is two bytes wide. */
} board_panel_pixel_t;

/**
 * @var s_panel_fb
 * @brief A framebuffer declared exactly the way an application declares one.
 *
 * @details
 * The subject of ::internal_test_macro_shapes_the_surface. Declared through
 * the macro so the test observes what the macro actually produces.
 *
 * @note Never written; only its address and size are inspected.
 * @warning Roughly 1.2 MiB of storage -- the real panel surface.
 * @since 0.1.0
 */
RA8_BOARD_PANEL_FRAMEBUFFER(s_panel_fb);

/**
 * @test board_panel_exports_the_alignment
 *
 * @brief Verify the exported alignment is the GLCDC's burst length.
 *
 * @return Nothing.
 * @pre The board panel header is on the include path.
 * @post Nothing is modified.
 * @note Single-threaded host test.
 * @since 0.1.0
 *
 * @par MC/DC:
 * No decision under test -- these are equality checks on compile-time
 * constants, so there is no condition to vary.
 */
RA8_INTERNAL static void internal_test_exports_the_alignment(void)
{
  TEST_BEGIN("panel: exported alignment is the GLCDC burst length");
  TEST_ASSERT_EQ(k_bp_align_bytes, k_ra8_board_fb_align_bytes);
  /* A burst alignment that is not a power of two cannot be an alignment. */
  const uint32_t align   = (uint32_t)k_ra8_board_fb_align_bytes;
  const uint32_t low_set = align & (align - 1U);
  TEST_ASSERT_EQ(0U, low_set);
  TEST_ASSERT_EQ(k_bp_panel_w, k_panel_width_px);
  TEST_ASSERT_EQ(k_bp_panel_h, k_panel_height_px);
  TEST_END("panel: exported alignment is the GLCDC burst length");
}

/**
 * @test board_panel_macro_shapes_the_surface
 *
 * @brief Verify a macro-declared framebuffer is aligned and panel-sized.
 *
 * @details
 * Both properties are checked on the declared object rather than on the macro
 * text, because both failure modes are silent: a dropped alignment attribute
 * compiles, and a mis-sized array only overruns once something draws into the
 * last scanline.
 *
 * @return Nothing.
 * @pre ::s_panel_fb was declared through the macro.
 * @post The framebuffer is not modified.
 * @note Single-threaded host test.
 * @since 0.1.0
 *
 * @par MC/DC:
 * No decision under test -- two equality checks on a declared object.
 */
RA8_INTERNAL static void internal_test_macro_shapes_the_surface(void)
{
  TEST_BEGIN("panel: macro-declared framebuffer is aligned and panel-sized");
  const uintptr_t base       = (uintptr_t)s_panel_fb;
  const uintptr_t misaligned = base % (uintptr_t)k_ra8_board_fb_align_bytes;
  TEST_ASSERT_EQ(0U, misaligned);
  const size_t expected_bytes =
    (size_t)k_bp_panel_w * (size_t)k_bp_panel_h * (size_t)k_bp_bytes_per_px;
  TEST_ASSERT_EQ(expected_bytes, sizeof(s_panel_fb));
  TEST_END("panel: macro-declared framebuffer is aligned and panel-sized");
}

/**
 * @brief Test binary entry point.
 *
 * @details No ordering dependency; nothing here holds state.
 *
 * @return 0 on success; a failing assertion exits before this returns.
 *
 * @pre The test framework has started.
 * @post The board's panel-framebuffer contract has been checked end to end.
 * @note Not thread-safe; single-threaded test runner.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_exports_the_alignment();
  internal_test_macro_shapes_the_surface();
  return 0;
}
