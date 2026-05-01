/**
 * @file test_ra_epaper.c
 * @brief Unit tests for the IT8951 e-paper SPI driver (``ra_epaper.c``)
 *
 * @details
 * Host-only tests. The simulator mmap models SPI registers as host
 * RAM, so the driver's SPI calls succeed with whatever last-write
 * value the test set. ``RA_SIMULATOR_MODE`` short-circuits the HRDY
 * busy-poll and the LUT-busy poll inside the driver, so each call
 * returns through the success path after exercising the command
 * sequencing logic.
 *
 * What we cover:
 *   - ``ra_epaper_init`` rejects NULL cfg.
 *   - ``ra_epaper_init`` rejects out-of-range cfg fields.
 *   - ``ra_epaper_init`` rejects double init.
 *   - Calls before init return ``k_ra_err_invalid_state``.
 *   - ``ra_epaper_load_image`` rejects NULL / size mismatch.
 *   - Happy-path init -> load -> display -> sleep round-trip succeeds
 *     and leaves the driver back in the uninit state.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra_epaper.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_epaper_test_const_t
 * @brief Test fixtures.
 */
typedef enum : uint32_t {
  k_ra_epaper_test_pclka_hz   = 100000000U, /**< 100 MHz PCLKA.       */
  k_ra_epaper_test_baud_hz    = 12000000U,  /**< 12 MHz SPI clock.    */
  k_ra_epaper_test_panel_w    = 800U,
  k_ra_epaper_test_panel_h    = 600U,
  k_ra_epaper_test_baud_huge  = 100000000U, /**< Above 24 MHz limit.  */
  k_ra_epaper_test_buf_pixels = 64U,        /**< 8x8 = 64 px.         */
} ra_epaper_test_const_t;

static ra_epaper_cfg_t make_cfg(void)
{
  const ra_epaper_cfg_t cfg = {
    .spi_channel  = 0U,
    .spi_baud_hz  = (uint32_t)k_ra_epaper_test_baud_hz,
    .pclka_hz     = (uint32_t)k_ra_epaper_test_pclka_hz,
    .reset_pin    = 0U,
    .busy_pin     = 0U,
    .panel_width  = (uint16_t)k_ra_epaper_test_panel_w,
    .panel_height = (uint16_t)k_ra_epaper_test_panel_h,
  };
  return cfg;
}

static void prep(void)
{
  ra_sim_mmap_reset();
  /* The driver state is file-static; a "sleep" call resets it back
   * to uninit. We achieve the same effect between tests via the
   * sleep API; tests that don't init (NULL-arg paths) don't need it.
   */
  (void)ra_epaper_sleep();
  /* Bring up MSTP so ra_spi_init can flip the SPI module-stop bit. */
  (void)ra_mstp_init();
}

static void test_init_null_cfg(void)
{
  TEST_BEGIN("test_init_null_cfg");
  prep();
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epaper_init(nullptr));
  TEST_END("test_init_null_cfg");
}

static void test_init_bad_cfg(void)
{
  TEST_BEGIN("test_init_bad_cfg");
  prep();
  ra_epaper_cfg_t cfg = make_cfg();

  cfg.spi_channel = 99U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_epaper_init(&cfg));
  cfg.spi_channel = 0U;

  cfg.panel_width = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_epaper_init(&cfg));
  cfg.panel_width = (uint16_t)k_ra_epaper_test_panel_w;

  cfg.spi_baud_hz = (uint32_t)k_ra_epaper_test_baud_huge;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_epaper_init(&cfg));
  TEST_END("test_init_bad_cfg");
}

static void test_calls_before_init(void)
{
  TEST_BEGIN("test_calls_before_init");
  prep();
  const ra_epaper_area_t area = {.x = 0U, .y = 0U, .width = 1U, .height = 1U};
  const uint8_t          buf  = 0x80U;
  TEST_ASSERT_EQ(k_ra_err_invalid_state,
                 ra_epaper_load_image(&area, &buf, 1U, k_ra_epaper_endian_little));
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_epaper_display_area(&area, k_ra_epaper_wf_gc16));
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_epaper_sleep());
  TEST_END("test_calls_before_init");
}

static void test_happy_path(void)
{
  TEST_BEGIN("test_happy_path");
  prep();
  const ra_epaper_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_epaper_init(&cfg));

  /* Double init -> invalid state. */
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_epaper_init(&cfg));

  /* Load + size mismatch. */
  uint8_t                pixels[k_ra_epaper_test_buf_pixels] = {};
  const ra_epaper_area_t area = {.x = 0U, .y = 0U, .width = 8U, .height = 8U};
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_epaper_load_image(&area, pixels, 1U, k_ra_epaper_endian_little));

  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_epaper_load_image(nullptr, pixels, sizeof(pixels), k_ra_epaper_endian_little));
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_epaper_load_image(&area, nullptr, sizeof(pixels), k_ra_epaper_endian_little));

  /* Happy load + display + sleep. */
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_epaper_load_image(&area, pixels, sizeof(pixels), k_ra_epaper_endian_little));
  TEST_ASSERT_EQ(k_ra_ok, ra_epaper_display_area(&area, k_ra_epaper_wf_gc16));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epaper_display_area(nullptr, k_ra_epaper_wf_gc16));
  TEST_ASSERT_EQ(k_ra_ok, ra_epaper_sleep());

  /* After sleep we're back to uninit. */
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_epaper_display_area(&area, k_ra_epaper_wf_gc16));
  TEST_END("test_happy_path");
}

int main(void)
{
  test_init_null_cfg();
  test_init_bad_cfg();
  test_calls_before_init();
  test_happy_path();
  return 0;
}
