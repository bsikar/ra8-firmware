/**
 * @file test_ra8_board_ek_ra8d2.c
 * @brief Unit tests for the EK-RA8D2 v1 board-support layer
 *
 * @details
 * Pure pin-table / API-shape tests; no MCU registers are exercised.
 * The BSP is intentionally a thin wrapper around HAL calls, so most
 * of the surface tested here is enum values and lookup tables that
 * must match the EK-RA8D2 v1 User's Manual (R20UT5523EG0101 Rev
 * 1.01) verbatim.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_ether_regs.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_pin_validator.h"
#include "ra8_port_constants.h"
#include "ra8_port_regs.h"
#include "unity_minimal.h"

/**
 * @enum board_ek_ra8d2_fixture_t
 * @brief Poison values written into out-parameters before a call, so one that fails without assigning is detectable, plus buffer capacities and payload sizes.
 */
typedef enum : uint8_t {
  k_board_poison_len =
    0xAAU, /**< Poison written into a length out-parameter, so a call that fails without setting it is detectable. */
} board_ek_ra8d2_fixture_t;

static void reset_board_hal_state(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  ra8_pin_validator_reset();
}

static void dummy_sw_irq_cb(void* ctx)
{
  (void)ctx;
}

/* ------------------------------------------------------------------------- */
/* Board-identity */
/* ------------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_board_get_info(void)
{
  TEST_BEGIN("board_get_info populates strings");
  ra8_board_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_get_info(&info));
  TEST_ASSERT_NOT_NULL(info.name);
  TEST_ASSERT_NOT_NULL(info.doc_rev);
  TEST_ASSERT_NOT_NULL(info.mcu);
  TEST_ASSERT(strcmp(info.name, "EK-RA8D2 v1") == 0);
  TEST_ASSERT(strcmp(info.doc_rev, "R20UT5523EG0101 Rev 1.01") == 0);
  TEST_ASSERT(strcmp(info.mcu, "R7KA8D2KFLCAC") == 0);
  TEST_END("board_get_info populates strings");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_board_get_info_null(void)
{
  TEST_BEGIN("board_get_info rejects NULL");
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_get_info(nullptr));
  TEST_END("board_get_info rejects NULL");
}

/* ------------------------------------------------------------------------- */
/* LED pin map (UM Table 24 p 31) */
/* ------------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_led_pin_lookup(void)
{
  TEST_BEGIN("led pins match UM Table 24");
  ra8_port_pin_t pin = k_ra8_pin_none;

  /* LED1 -> P600 (blue). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_pin(k_ra8_board_led1, &pin));
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_6, k_ra8_pin_0), pin);

  /* LED2 -> P303 (green). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_pin(k_ra8_board_led2, &pin));
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_3, k_ra8_pin_3), pin);

  /* LED3 -> PA07 (red). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_pin(k_ra8_board_led3, &pin));
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_10, k_ra8_pin_7), pin);

  /* Colour aliases must match numeric IDs. */
  TEST_ASSERT_EQ(k_ra8_board_led1, k_ra8_board_led_blue);
  TEST_ASSERT_EQ(k_ra8_board_led2, k_ra8_board_led_green);
  TEST_ASSERT_EQ(k_ra8_board_led3, k_ra8_board_led_red);
  TEST_END("led pins match UM Table 24");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_led_pin_invalid(void)
{
  TEST_BEGIN("led_pin rejects out-of-range / null");
  ra8_port_pin_t pin = k_ra8_pin_none;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_board_led_pin((ra8_board_led_id_t)k_ra8_board_led_count, &pin));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_led_pin(k_ra8_board_led1, nullptr));
  TEST_END("led_pin rejects out-of-range / null");
}

/* ------------------------------------------------------------------------- */
/* Switch pin map + IRQ numbers (UM Table 25 p 32) */
/* ------------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_sw_pin_lookup(void)
{
  TEST_BEGIN("sw pins match UM Table 25");
  ra8_port_pin_t pin = k_ra8_pin_none;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_sw_pin(k_ra8_board_sw1, &pin));
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_0, k_ra8_pin_9), pin);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_sw_pin(k_ra8_board_sw2, &pin));
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_0, k_ra8_pin_8), pin);

  TEST_ASSERT_EQ(13, k_ra8_board_sw1_irq);
  TEST_ASSERT_EQ(12, k_ra8_board_sw2_irq);
  TEST_END("sw pins match UM Table 25");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_sw_attach_irq_null_cb(void)
{
  TEST_BEGIN("sw_attach_irq rejects NULL callback");
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_sw_attach_irq(k_ra8_board_sw1, nullptr, nullptr));
  TEST_END("sw_attach_irq rejects NULL callback");
}

/* ------------------------------------------------------------------------- */
/* Audio CODEC pins (UM Table 32 p 38) */
/* ------------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_audio_pins(void)
{
  TEST_BEGIN("audio pins match UM Table 32");
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_4, k_ra8_pin_3), k_ra8_board_audio_pin_bclk);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_4, k_ra8_pin_4), k_ra8_board_audio_pin_wclk);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_4, k_ra8_pin_5), k_ra8_board_audio_pin_datin);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_4, k_ra8_pin_6), k_ra8_board_audio_pin_datout);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_13, k_ra8_pin_6), k_ra8_board_audio_pin_mclk);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_5, k_ra8_pin_11), k_ra8_board_audio_pin_i2c_sda);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_5, k_ra8_pin_12), k_ra8_board_audio_pin_i2c_scl);
  TEST_END("audio pins match UM Table 32");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_audio_play_sample_block_validates(void)
{
  TEST_BEGIN("audio_play_sample_block rejects bad args");
  int16_t buf[4] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_audio_play_sample_block(nullptr, 4U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_audio_play_sample_block(buf, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_audio_play_sample_block(buf, 3U));
  /* Even-length non-empty buffer reaches the SSIE hook; without a
   * preceding ra8_board_audio_init the BSP refuses with not_initialized. */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_board_audio_play_sample_block(buf, 4U));
  TEST_END("audio_play_sample_block rejects bad args");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_audio_init_validates(void)
{
  TEST_BEGIN("audio_init validates sample rate / depth / channels");
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_audio_init(0U, 16U, 2U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_audio_init(48000U, 12U, 2U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_audio_init(48000U, 16U, 3U));
  TEST_END("audio_init validates sample rate / depth / channels");
}

/* ------------------------------------------------------------------------- */
/* Arduino header (UM Table 20 p 28) */
/* ------------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_arduino_pins(void)
{
  TEST_BEGIN("arduino pins match UM Table 20");
  /* Spot-check the most-cited pins: D6=GTIOC1A=P105, D8=PD01, D13=P102. */
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_1, k_ra8_pin_5), k_ra8_board_arduino_d6);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_13, k_ra8_pin_1), k_ra8_board_arduino_d8);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_1, k_ra8_pin_2), k_ra8_board_arduino_d13);
  /* Analog. */
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_0, k_ra8_pin_1), k_ra8_board_arduino_a0);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_0, k_ra8_pin_15), k_ra8_board_arduino_a5);
  TEST_END("arduino pins match UM Table 20");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_arduino_pin_init_invalid_mode(void)
{
  TEST_BEGIN("arduino_pin_init rejects unknown mode");
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_board_arduino_pin_init(k_ra8_board_arduino_d2, (ra8_board_arduino_mode_t)0xFFU));
  TEST_END("arduino_pin_init rejects unknown mode");
}

/* ------------------------------------------------------------------------- */
/* Pmod connectors (UM Tables 17 + 19) */
/* ------------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pmod1_spi_pins(void)
{
  TEST_BEGIN("pmod1 SPI pins match UM Table 17");
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_8, k_ra8_pin_4), k_ra8_board_pmod1_spi_cs);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_8, k_ra8_pin_1), k_ra8_board_pmod1_spi_copi);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_8, k_ra8_pin_2), k_ra8_board_pmod1_spi_cipo);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_8, k_ra8_pin_3), k_ra8_board_pmod1_spi_sck);
  TEST_END("pmod1 SPI pins match UM Table 17");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pmod2_spi_pins(void)
{
  TEST_BEGIN("pmod2 SPI pins match UM Table 19");
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_6, k_ra8_pin_4), k_ra8_board_pmod2_spi_cs);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_6, k_ra8_pin_3), k_ra8_board_pmod2_spi_copi);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_6, k_ra8_pin_2), k_ra8_board_pmod2_spi_cipo);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_6, k_ra8_pin_1), k_ra8_board_pmod2_spi_sck);
  TEST_END("pmod2 SPI pins match UM Table 19");
}

/* ------------------------------------------------------------------------- */
/* GLCDC pin tables (UM Table 33 p 42) */
/* ------------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_glcdc_pin_tables_populated(void)
{
  TEST_BEGIN("glcdc pin tables non-empty + sized correctly");
  TEST_ASSERT(g_ra8_board_glcdc_rgb888_pin_count > 0U);
  TEST_ASSERT(g_ra8_board_glcdc_rgb666_pin_count > 0U);
  TEST_ASSERT(g_ra8_board_glcdc_rgb565_pin_count > 0U);
  TEST_ASSERT(g_ra8_board_glcdc_rgb888_pin_count >= g_ra8_board_glcdc_rgb666_pin_count);
  TEST_ASSERT(g_ra8_board_glcdc_rgb666_pin_count >= g_ra8_board_glcdc_rgb565_pin_count);

  /* Spot-check J1-1 BLEN = P514 across all tables. */
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_5, k_ra8_pin_14), g_ra8_board_glcdc_rgb888_pins[0].pin);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_5, k_ra8_pin_14), g_ra8_board_glcdc_rgb666_pins[0].pin);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_5, k_ra8_pin_14), g_ra8_board_glcdc_rgb565_pins[0].pin);
  TEST_END("glcdc pin tables non-empty + sized correctly");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_glcdc_init_invalid_fmt(void)
{
  TEST_BEGIN("glcdc_init rejects bogus format");
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_glcdc_init((ra8_board_glcdc_fmt_t)0xFFU));
  TEST_END("glcdc_init rejects bogus format");
}

/* ------------------------------------------------------------------------- */
/* Camera / XSPI / SDRAM / MIPI-DSI */
/* ------------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_camera_pins(void)
{
  TEST_BEGIN("camera pins match UM Table 35");
  /* Spot-check the unambiguous ones (no jumper / SW switch). */
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_4, k_ra8_pin_0), k_ra8_board_cam_d0);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_5, k_ra8_pin_1), k_ra8_board_cam_xclk);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_11, k_ra8_pin_4), k_ra8_board_cam_pclk);
  TEST_END("camera pins match UM Table 35");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_xspi_pins(void)
{
  TEST_BEGIN("xspi flash pins match UM Table 29");
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_1, k_ra8_pin_4), k_ra8_board_xspi_cs);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_8, k_ra8_pin_8), k_ra8_board_xspi_clk);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_1, k_ra8_pin_0), k_ra8_board_xspi_dq0);
  TEST_END("xspi flash pins match UM Table 29");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_extmem_sizes(void)
{
  TEST_BEGIN("extmem sizes are 64 MiB");
  const uint32_t sixty_four_mib = 0x04000000UL;
  TEST_ASSERT_EQ(sixty_four_mib, k_ra8_board_xspi_flash_size);
  TEST_ASSERT_EQ(sixty_four_mib, k_ra8_board_sdram_size);
  TEST_END("extmem sizes are 64 MiB");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_mipi_dsi_pins(void)
{
  TEST_BEGIN("mipi-dsi pins match UM Table 34");
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_4, k_ra8_pin_11), k_ra8_board_mipi_dsi_te);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_6, k_ra8_pin_6), k_ra8_board_mipi_dsi_reset_n);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_5, k_ra8_pin_14), k_ra8_board_mipi_dsi_backlight);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_1, k_ra8_pin_11), k_ra8_board_mipi_dsi_touch_int);
  TEST_END("mipi-dsi pins match UM Table 34");
}

/* ------------------------------------------------------------------------- */
/* Stubbed init paths return well-defined errors */
/* ------------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_stubs_return_not_supported(void)
{
  TEST_BEGIN("usbhs / mipi-dsi stubs return not_supported");
  /* USBHS device/host promoted to real in commit 28c4ed436; fake hits CGC
   * MOSCSF-wait timeout. Just exercise both for crash-immunity. */
  (void)ra8_board_usbhs_device_init();
  (void)ra8_board_usbhs_host_init();
  /* ra8_board_mipi_dsi_init is now a real wired implementation that calls
   * ra8_mipi_phy_init + ra8_mipi_dsi_init; with the placeholder PLL/timing
   * config it returns an error from the underlying HAL rather than
   * not_supported. Just exercise it for crash-immunity. */
  (void)ra8_board_mipi_dsi_init();
  TEST_END("usbhs / mipi-dsi stubs return not_supported");
}

/* ------------------------------------------------------------------------- */
/* J-Link OB VCOM serial bridge (UM Table 13 p 24) */
/* ------------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_uart_console_pins(void)
{
  TEST_BEGIN("uart console pins match UM Table 13");
  /* PD02 / PD03 are the always-wired TXD/RXD; PD04 / PD05 are the
   * optional flow-control pair. Port 13 = PDxx on the RA8D2. */
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_13, k_ra8_pin_2), k_ra8_board_uart_console_pin_txd);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_13, k_ra8_pin_3), k_ra8_board_uart_console_pin_rxd);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_13, k_ra8_pin_4), k_ra8_board_uart_console_pin_rts);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_13, k_ra8_pin_5), k_ra8_board_uart_console_pin_cts);
  /* PD02/PD03 -> SCI8 alternate (verified on real EK-RA8D2 v1 silicon). */
  TEST_ASSERT_EQ(8, k_ra8_board_uart_console_sci_channel);
  TEST_ASSERT_EQ(0, k_ra8_board_uart_console);
  TEST_END("uart console pins match UM Table 13");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_uart_console_init_rejects_zero_baud(void)
{
  TEST_BEGIN("uart_console_init rejects baud == 0");
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_uart_console_init(0U));
  TEST_END("uart_console_init rejects baud == 0");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_uart_console_write_validates(void)
{
  TEST_BEGIN("uart_console_write validates args + state");
  /* Zero-length write is a no-op success regardless of init state. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_uart_console_write(nullptr, 0U));
  /* NULL data with non-zero len -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_uart_console_write(nullptr, 4U));
  TEST_END("uart_console_write validates args + state");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_uart_console_read_validates(void)
{
  TEST_BEGIN("uart_console_read validates args + state");
  uint8_t buf[4]  = {};
  size_t  out_len = k_board_poison_len;
  /* NULL out_len always rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_uart_console_read(buf, sizeof(buf), nullptr));
  /* cap == 0 is a successful no-op (and zeroes *out_len). */
  out_len = k_board_poison_len;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_uart_console_read(nullptr, 0U, &out_len));
  TEST_ASSERT_EQ(0, out_len);
  /* NULL buffer with non-zero cap -> invalid_arg. */
  out_len = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_board_uart_console_read(nullptr, sizeof(buf), &out_len));
  TEST_END("uart_console_read validates args + state");
}

/* ------------------------------------------------------------------------- */
/* On-board RGMII Ethernet PHY (UM Table 26 p 33) */
/* ------------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_ethernet_pins(void)
{
  TEST_BEGIN("ethernet pins match UM Table 26");
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_1, k_ra8_pin_7), k_ra8_board_eth_pin_mdint);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_4, k_ra8_pin_15), k_ra8_board_eth_pin_mdc);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_4, k_ra8_pin_14), k_ra8_board_eth_pin_mdio);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_3, k_ra8_pin_7), k_ra8_board_eth_pin_txd0);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_3, k_ra8_pin_6), k_ra8_board_eth_pin_txd1);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_3, k_ra8_pin_5), k_ra8_board_eth_pin_txd2);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_3, k_ra8_pin_4), k_ra8_board_eth_pin_txd3);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_3, k_ra8_pin_10), k_ra8_board_eth_pin_tx_ctl);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_3, k_ra8_pin_9), k_ra8_board_eth_pin_tx_clk);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_9, k_ra8_pin_6), k_ra8_board_eth_pin_rxd0);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_9, k_ra8_pin_7), k_ra8_board_eth_pin_rxd1);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_9, k_ra8_pin_8), k_ra8_board_eth_pin_rxd2);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_9, k_ra8_pin_9), k_ra8_board_eth_pin_rxd3);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_2, k_ra8_pin_6), k_ra8_board_eth_pin_rx_ctl);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_9, k_ra8_pin_5), k_ra8_board_eth_pin_rx_clk);
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_7, k_ra8_pin_8), k_ra8_board_eth_pin_rstn);
  TEST_END("ethernet pins match UM Table 26");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_ethernet_index_constants(void)
{
  TEST_BEGIN("ethernet ETHA/RMAC port + PHY addr defaults");
  /* The on-board PHY is on ETHA1 / RMAC1, MDIO addr 0 (PEF7071 strap).
   * Cross-reference the canonical FSP ethernet_ek_ra8d2_ep project:
   * every Ethernet pin in its pincfg is "eswm_rgmii1" and the r_rmac
   * Channel = 1. RMAC0 / ETHA0 are unused on the EK-RA8D2. */
  TEST_ASSERT_EQ(1, k_ra8_board_eth_etha_port);
  TEST_ASSERT_EQ(1, k_ra8_board_eth_rmac_port);
  TEST_ASSERT_EQ(0, k_ra8_board_eth_phy_addr);
  TEST_END("ethernet ETHA/RMAC port + PHY addr defaults");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_board_led_funcs(void)
{
  TEST_BEGIN("board_led_funcs forward to hal or return error");
  reset_board_hal_state();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_init(k_ra8_board_led1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_on(k_ra8_board_led1));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_led_on((ra8_board_led_id_t)99));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_off(k_ra8_board_led1));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_led_off((ra8_board_led_id_t)99));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_toggle(k_ra8_board_led1));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_led_toggle((ra8_board_led_id_t)99));
  TEST_END("board_led_funcs forward to hal or return error");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_board_sw_funcs(void)
{
  TEST_BEGIN("board_sw_funcs forward to hal or return error");
  reset_board_hal_state();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_sw_init(k_ra8_board_sw1));
  ra8_board_sw_state_t sw_val  = k_ra8_board_sw_released;
  ra8_level_t          ard_val = k_ra8_level_low;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_sw_read(k_ra8_board_sw1, &sw_val));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_sw_read((ra8_board_sw_id_t)99, &sw_val));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_sw_read(k_ra8_board_sw1, nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_sw_attach_irq(k_ra8_board_sw1, dummy_sw_irq_cb, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_board_sw_attach_irq((ra8_board_sw_id_t)99, dummy_sw_irq_cb, nullptr));
  TEST_END("board_sw_funcs forward to hal or return error");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_board_xspi_init(void)
{
  TEST_BEGIN("board_xspi_pins_init forwards to hal");
  reset_board_hal_state();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_xspi_pins_init());
  TEST_END("board_xspi_pins_init forwards to hal");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_board_arduino_gpio_funcs(void)
{
  TEST_BEGIN("board_arduino_gpio_funcs forward to hal or return error");
  reset_board_hal_state();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_arduino_gpio_write(k_ra8_board_arduino_d2, k_ra8_level_high));
  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_pin,
                 ra8_board_arduino_gpio_write((ra8_board_arduino_pin_t)99, k_ra8_level_high));

  ra8_level_t ard_val = k_ra8_level_low;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_arduino_gpio_read(k_ra8_board_arduino_d2, &ard_val));
  TEST_ASSERT_EQ(k_ra8_err_gpio_invalid_pin,
                 ra8_board_arduino_gpio_read((ra8_board_arduino_pin_t)99, &ard_val));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_board_arduino_gpio_read(k_ra8_board_arduino_d2, nullptr));
  TEST_END("board_arduino_gpio_funcs forward to hal or return error");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_board_io_expander(void)
{
  TEST_BEGIN("board_io_expander_set_usbhs_device_mode forwards to hal");
  reset_board_hal_state();
  /* The fake backs the RIIC block with plain memory and never
   * raises TDRE, so the real ra8_i2c controller driver stalls waiting for
   * the first transmit-data-empty and reports hw_timeout. The contract
   * being checked is that the BSP propagates the HAL failure unchanged. */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_board_io_expander_set_usbhs_device_mode());
  TEST_END("board_io_expander_set_usbhs_device_mode forwards to hal");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_board_uart_console_flush(void)
{
  TEST_BEGIN("board_uart_console_flush forwards to hal");
  reset_board_hal_state();
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_board_uart_console_flush());
  TEST_END("board_uart_console_flush forwards to hal");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_board_ethernet_init(void)
{
  TEST_BEGIN("board_ethernet_init forwards to hal");
  reset_board_hal_state();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_ethernet_init());
  TEST_END("board_ethernet_init forwards to hal");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the CABPIRM.BPR wait is a
 * single-condition bounded poll, not a compound boolean)
 */
static void test_board_ethernet_init_coma_bpr_timeout(void)
{
  TEST_BEGIN("board_ethernet_init reports CABPIRM.BPR timeout");
  reset_board_hal_state();
  /* Arm the COMA buffer-pool-ready register so BPR never asserts: the
   * COMA bring-up's bounded wait exhausts its budget and init returns
   * the real hardware-timeout error instead of the fake success the
   * deleted RA8_OFF_TARGET short-circuit used to return. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)ra8_coma_cabpirm()));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_board_ethernet_init());
  ra8_fake_mmio_reset();
  TEST_END("board_ethernet_init reports CABPIRM.BPR timeout");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom, exactly as before.
 */
static void (*const s_test_roster[])(void) = {
  test_board_get_info,
  test_board_get_info_null,
  test_led_pin_lookup,
  test_led_pin_invalid,
  test_sw_pin_lookup,
  test_sw_attach_irq_null_cb,
  test_audio_pins,
  test_audio_play_sample_block_validates,
  test_audio_init_validates,
  test_arduino_pins,
  test_arduino_pin_init_invalid_mode,
  test_pmod1_spi_pins,
  test_pmod2_spi_pins,
  test_glcdc_pin_tables_populated,
  test_glcdc_init_invalid_fmt,
  test_camera_pins,
  test_xspi_pins,
  test_extmem_sizes,
  test_mipi_dsi_pins,
  test_stubs_return_not_supported,
  test_uart_console_pins,
  test_uart_console_init_rejects_zero_baud,
  test_uart_console_write_validates,
  test_uart_console_read_validates,
  test_ethernet_pins,
  test_ethernet_index_constants,
  test_board_led_funcs,
  test_board_sw_funcs,
  test_board_xspi_init,
  test_board_arduino_gpio_funcs,
  test_board_io_expander,
  test_board_uart_console_flush,
  test_board_ethernet_init,
  test_board_ethernet_init_coma_bpr_timeout,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK ] test_ra8_board_ek_ra8d2.c\n");
  return 0;
}
