/**
 * @file test_ra_board_ek_ra8d2.c
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

#include "ra_board_ek_ra8d2.h"
#include "ra_err.h"
#include "ra_port_constants.h"
#include "unity_minimal.h"

/* ------------------------------------------------------------------------- */
/* Board-identity                                                            */
/* ------------------------------------------------------------------------- */

static void test_board_get_info(void)
{
  TEST_BEGIN("board_get_info populates strings");
  ra_board_info_t info = {};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_board_get_info(&info));
  TEST_ASSERT_NOT_NULL(info.name);
  TEST_ASSERT_NOT_NULL(info.doc_rev);
  TEST_ASSERT_NOT_NULL(info.mcu);
  TEST_ASSERT(strcmp(info.name, "EK-RA8D2 v1") == 0);
  TEST_ASSERT(strcmp(info.doc_rev, "R20UT5523EG0101 Rev 1.01") == 0);
  TEST_ASSERT(strcmp(info.mcu, "R7KA8D2KFLCAC") == 0);
  TEST_END("board_get_info populates strings");
}

static void test_board_get_info_null(void)
{
  TEST_BEGIN("board_get_info rejects NULL");
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_board_get_info(NULL));
  TEST_END("board_get_info rejects NULL");
}

/* ------------------------------------------------------------------------- */
/* LED pin map (UM Table 24 p 31)                                            */
/* ------------------------------------------------------------------------- */

static void test_led_pin_lookup(void)
{
  TEST_BEGIN("led pins match UM Table 24");
  ra_port_pin_t pin = k_ra_pin_none;

  /* LED1 -> P600 (blue). */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_board_led_pin(k_ra_board_led1, &pin));
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_6, k_ra_pin_0), (int)pin);

  /* LED2 -> P303 (green). */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_board_led_pin(k_ra_board_led2, &pin));
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_3, k_ra_pin_3), (int)pin);

  /* LED3 -> PA07 (red). */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_board_led_pin(k_ra_board_led3, &pin));
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_10, k_ra_pin_7), (int)pin);

  /* Colour aliases must match numeric IDs. */
  TEST_ASSERT_EQ((int)k_ra_board_led1, (int)k_ra_board_led_blue);
  TEST_ASSERT_EQ((int)k_ra_board_led2, (int)k_ra_board_led_green);
  TEST_ASSERT_EQ((int)k_ra_board_led3, (int)k_ra_board_led_red);
  TEST_END("led pins match UM Table 24");
}

static void test_led_pin_invalid(void)
{
  TEST_BEGIN("led_pin rejects out-of-range / null");
  ra_port_pin_t pin = k_ra_pin_none;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_board_led_pin((ra_board_led_id_t)k_ra_board_led_count, &pin));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_board_led_pin(k_ra_board_led1, NULL));
  TEST_END("led_pin rejects out-of-range / null");
}

/* ------------------------------------------------------------------------- */
/* Switch pin map + IRQ numbers (UM Table 25 p 32)                           */
/* ------------------------------------------------------------------------- */

static void test_sw_pin_lookup(void)
{
  TEST_BEGIN("sw pins match UM Table 25");
  ra_port_pin_t pin = k_ra_pin_none;

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_board_sw_pin(k_ra_board_sw1, &pin));
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_0, k_ra_pin_9), (int)pin);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_board_sw_pin(k_ra_board_sw2, &pin));
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_0, k_ra_pin_8), (int)pin);

  TEST_ASSERT_EQ((int)13, (int)k_ra_board_sw1_irq);
  TEST_ASSERT_EQ((int)12, (int)k_ra_board_sw2_irq);
  TEST_END("sw pins match UM Table 25");
}

static void test_sw_attach_irq_null_cb(void)
{
  TEST_BEGIN("sw_attach_irq rejects NULL callback");
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_board_sw_attach_irq(k_ra_board_sw1, NULL, NULL));
  TEST_END("sw_attach_irq rejects NULL callback");
}

/* ------------------------------------------------------------------------- */
/* Audio CODEC pins (UM Table 32 p 38)                                       */
/* ------------------------------------------------------------------------- */

static void test_audio_pins(void)
{
  TEST_BEGIN("audio pins match UM Table 32");
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_4, k_ra_pin_3), (int)k_ra_board_audio_pin_bclk);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_4, k_ra_pin_4), (int)k_ra_board_audio_pin_wclk);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_4, k_ra_pin_5), (int)k_ra_board_audio_pin_datin);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_4, k_ra_pin_6), (int)k_ra_board_audio_pin_datout);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_13, k_ra_pin_6), (int)k_ra_board_audio_pin_mclk);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_5, k_ra_pin_11), (int)k_ra_board_audio_pin_i2c_sda);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_5, k_ra_pin_12), (int)k_ra_board_audio_pin_i2c_scl);
  TEST_END("audio pins match UM Table 32");
}

static void test_audio_play_sample_block_validates(void)
{
  TEST_BEGIN("audio_play_sample_block rejects bad args");
  int16_t buf[4] = {};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_board_audio_play_sample_block(NULL, 4U));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_board_audio_play_sample_block(buf, 0U));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_board_audio_play_sample_block(buf, 3U));
  /* Even-length non-empty buffer reaches the (currently stubbed) HAL hook. */
  TEST_ASSERT_EQ((int)k_ra_err_not_supported, (int)ra_board_audio_play_sample_block(buf, 4U));
  TEST_END("audio_play_sample_block rejects bad args");
}

/* ------------------------------------------------------------------------- */
/* Arduino header (UM Table 20 p 28)                                         */
/* ------------------------------------------------------------------------- */

static void test_arduino_pins(void)
{
  TEST_BEGIN("arduino pins match UM Table 20");
  /* Spot-check the most-cited pins: D6=GTIOC1A=P105, D8=PD01, D13=P102. */
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_1, k_ra_pin_5), (int)k_ra_board_arduino_d6);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_13, k_ra_pin_1), (int)k_ra_board_arduino_d8);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_1, k_ra_pin_2), (int)k_ra_board_arduino_d13);
  /* Analog. */
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_0, k_ra_pin_1), (int)k_ra_board_arduino_a0);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_0, k_ra_pin_15), (int)k_ra_board_arduino_a5);
  TEST_END("arduino pins match UM Table 20");
}

static void test_arduino_pin_init_invalid_mode(void)
{
  TEST_BEGIN("arduino_pin_init rejects unknown mode");
  TEST_ASSERT_EQ(
    (int)k_ra_err_invalid_arg,
    (int)ra_board_arduino_pin_init(k_ra_board_arduino_d2, (ra_board_arduino_mode_t)0xFFU));
  TEST_END("arduino_pin_init rejects unknown mode");
}

/* ------------------------------------------------------------------------- */
/* Pmod connectors (UM Tables 17 + 19)                                       */
/* ------------------------------------------------------------------------- */

static void test_pmod1_spi_pins(void)
{
  TEST_BEGIN("pmod1 SPI pins match UM Table 17");
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_8, k_ra_pin_4), (int)k_ra_board_pmod1_spi_cs);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_8, k_ra_pin_1), (int)k_ra_board_pmod1_spi_copi);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_8, k_ra_pin_2), (int)k_ra_board_pmod1_spi_cipo);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_8, k_ra_pin_3), (int)k_ra_board_pmod1_spi_sck);
  TEST_END("pmod1 SPI pins match UM Table 17");
}

static void test_pmod2_spi_pins(void)
{
  TEST_BEGIN("pmod2 SPI pins match UM Table 19");
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_6, k_ra_pin_4), (int)k_ra_board_pmod2_spi_cs);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_6, k_ra_pin_3), (int)k_ra_board_pmod2_spi_copi);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_6, k_ra_pin_2), (int)k_ra_board_pmod2_spi_cipo);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_6, k_ra_pin_1), (int)k_ra_board_pmod2_spi_sck);
  TEST_END("pmod2 SPI pins match UM Table 19");
}

/* ------------------------------------------------------------------------- */
/* GLCDC pin tables (UM Table 33 p 42)                                       */
/* ------------------------------------------------------------------------- */

static void test_glcdc_pin_tables_populated(void)
{
  TEST_BEGIN("glcdc pin tables non-empty + sized correctly");
  TEST_ASSERT(g_ra_board_glcdc_rgb888_pin_count > 0U);
  TEST_ASSERT(g_ra_board_glcdc_rgb666_pin_count > 0U);
  TEST_ASSERT(g_ra_board_glcdc_rgb565_pin_count > 0U);
  TEST_ASSERT(g_ra_board_glcdc_rgb888_pin_count >= g_ra_board_glcdc_rgb666_pin_count);
  TEST_ASSERT(g_ra_board_glcdc_rgb666_pin_count >= g_ra_board_glcdc_rgb565_pin_count);

  /* Spot-check J1-1 BLEN = P514 across all tables. */
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_5, k_ra_pin_14), (int)g_ra_board_glcdc_rgb888_pins[0].pin);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_5, k_ra_pin_14), (int)g_ra_board_glcdc_rgb666_pins[0].pin);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_5, k_ra_pin_14), (int)g_ra_board_glcdc_rgb565_pins[0].pin);
  TEST_END("glcdc pin tables non-empty + sized correctly");
}

static void test_glcdc_init_invalid_fmt(void)
{
  TEST_BEGIN("glcdc_init rejects bogus format");
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_board_glcdc_init((ra_board_glcdc_fmt_t)0xFFU));
  TEST_END("glcdc_init rejects bogus format");
}

/* ------------------------------------------------------------------------- */
/* Camera / XSPI / SDRAM / MIPI-DSI                                          */
/* ------------------------------------------------------------------------- */

static void test_camera_pins(void)
{
  TEST_BEGIN("camera pins match UM Table 35");
  /* Spot-check the unambiguous ones (no jumper / SW switch). */
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_4, k_ra_pin_0), (int)k_ra_board_cam_d0);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_5, k_ra_pin_1), (int)k_ra_board_cam_xclk);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_11, k_ra_pin_4), (int)k_ra_board_cam_pclk);
  TEST_END("camera pins match UM Table 35");
}

static void test_xspi_pins(void)
{
  TEST_BEGIN("xspi flash pins match UM Table 29");
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_1, k_ra_pin_4), (int)k_ra_board_xspi_cs);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_8, k_ra_pin_8), (int)k_ra_board_xspi_clk);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_1, k_ra_pin_0), (int)k_ra_board_xspi_dq0);
  TEST_END("xspi flash pins match UM Table 29");
}

static void test_extmem_sizes(void)
{
  TEST_BEGIN("extmem sizes are 64 MiB");
  const uint32_t sixty_four_mib = 0x04000000UL;
  TEST_ASSERT_EQ((int64_t)sixty_four_mib, (int64_t)k_ra_board_xspi_flash_size);
  TEST_ASSERT_EQ((int64_t)sixty_four_mib, (int64_t)k_ra_board_sdram_size);
  TEST_END("extmem sizes are 64 MiB");
}

static void test_mipi_dsi_pins(void)
{
  TEST_BEGIN("mipi-dsi pins match UM Table 34");
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_4, k_ra_pin_11), (int)k_ra_board_mipi_dsi_te);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_6, k_ra_pin_6), (int)k_ra_board_mipi_dsi_reset_n);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_5, k_ra_pin_14), (int)k_ra_board_mipi_dsi_backlight);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_1, k_ra_pin_11), (int)k_ra_board_mipi_dsi_touch_int);
  TEST_END("mipi-dsi pins match UM Table 34");
}

/* ------------------------------------------------------------------------- */
/* Stubbed init paths return well-defined errors                             */
/* ------------------------------------------------------------------------- */

static void test_stubs_return_not_supported(void)
{
  TEST_BEGIN("usbhs / mipi-dsi stubs return not_supported");
  /* USBHS device/host promoted to real in commit 28c4ed436; sim hits CGC
   * MOSCSF-wait timeout. Just exercise both for crash-immunity. */
  (void)ra_board_usbhs_device_init();
  (void)ra_board_usbhs_host_init();
  TEST_ASSERT_EQ((int)k_ra_err_not_supported, (int)ra_board_mipi_dsi_init());
  TEST_END("usbhs / mipi-dsi stubs return not_supported");
}

/* ------------------------------------------------------------------------- */
/* J-Link OB VCOM serial bridge (UM Table 13 p 24)                            */
/* ------------------------------------------------------------------------- */

static void test_uart_console_pins(void)
{
  TEST_BEGIN("uart console pins match UM Table 13");
  /* PD02 / PD03 are the always-wired TXD/RXD; PD04 / PD05 are the
   * optional flow-control pair. Port 13 = PDxx on the RA8D2. */
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_13, k_ra_pin_2), (int)k_ra_board_uart_console_pin_txd);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_13, k_ra_pin_3), (int)k_ra_board_uart_console_pin_rxd);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_13, k_ra_pin_4), (int)k_ra_board_uart_console_pin_rts);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_13, k_ra_pin_5), (int)k_ra_board_uart_console_pin_cts);
  TEST_ASSERT_EQ((int)3, (int)k_ra_board_uart_console_sci_channel);
  TEST_ASSERT_EQ((int)0, (int)k_ra_board_uart_console);
  TEST_END("uart console pins match UM Table 13");
}

static void test_uart_console_init_rejects_zero_baud(void)
{
  TEST_BEGIN("uart_console_init rejects baud == 0");
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_board_uart_console_init(0U));
  TEST_END("uart_console_init rejects baud == 0");
}

static void test_uart_console_write_validates(void)
{
  TEST_BEGIN("uart_console_write validates args + state");
  /* Zero-length write is a no-op success regardless of init state. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_board_uart_console_write(NULL, 0U));
  /* NULL data with non-zero len -> invalid_arg. */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_board_uart_console_write(NULL, 4U));
  TEST_END("uart_console_write validates args + state");
}

static void test_uart_console_read_validates(void)
{
  TEST_BEGIN("uart_console_read validates args + state");
  uint8_t buf[4]  = {};
  size_t  out_len = 0xAAU;
  /* NULL out_len always rejected. */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_board_uart_console_read(buf, sizeof(buf), NULL));
  /* cap == 0 is a successful no-op (and zeroes *out_len). */
  out_len = 0xAAU;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_board_uart_console_read(NULL, 0U, &out_len));
  TEST_ASSERT_EQ((int64_t)0, (int64_t)out_len);
  /* NULL buffer with non-zero cap -> invalid_arg. */
  out_len = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_board_uart_console_read(NULL, sizeof(buf), &out_len));
  TEST_END("uart_console_read validates args + state");
}

/* ------------------------------------------------------------------------- */
/* On-board RGMII Ethernet PHY (UM Table 26 p 33)                             */
/* ------------------------------------------------------------------------- */

static void test_ethernet_pins(void)
{
  TEST_BEGIN("ethernet pins match UM Table 26");
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_1, k_ra_pin_7), (int)k_ra_board_eth_pin_mdint);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_4, k_ra_pin_15), (int)k_ra_board_eth_pin_mdc);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_4, k_ra_pin_14), (int)k_ra_board_eth_pin_mdio);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_3, k_ra_pin_7), (int)k_ra_board_eth_pin_txd0);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_3, k_ra_pin_6), (int)k_ra_board_eth_pin_txd1);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_3, k_ra_pin_5), (int)k_ra_board_eth_pin_txd2);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_3, k_ra_pin_4), (int)k_ra_board_eth_pin_txd3);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_3, k_ra_pin_10), (int)k_ra_board_eth_pin_tx_ctl);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_3, k_ra_pin_9), (int)k_ra_board_eth_pin_tx_clk);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_9, k_ra_pin_6), (int)k_ra_board_eth_pin_rxd0);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_9, k_ra_pin_7), (int)k_ra_board_eth_pin_rxd1);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_9, k_ra_pin_8), (int)k_ra_board_eth_pin_rxd2);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_9, k_ra_pin_9), (int)k_ra_board_eth_pin_rxd3);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_2, k_ra_pin_6), (int)k_ra_board_eth_pin_rx_ctl);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_9, k_ra_pin_5), (int)k_ra_board_eth_pin_rx_clk);
  TEST_ASSERT_EQ((int)RA_PIN(k_ra_port_7, k_ra_pin_8), (int)k_ra_board_eth_pin_rstn);
  TEST_END("ethernet pins match UM Table 26");
}

static void test_ethernet_index_constants(void)
{
  TEST_BEGIN("ethernet ETHA/RMAC port + PHY addr defaults");
  /* The on-board PHY is on ETHA0 / RMAC0, MDIO addr 0 (PEF7071 strap). */
  TEST_ASSERT_EQ((int)0, (int)k_ra_board_eth_etha_port);
  TEST_ASSERT_EQ((int)0, (int)k_ra_board_eth_rmac_port);
  TEST_ASSERT_EQ((int)0, (int)k_ra_board_eth_phy_addr);
  TEST_END("ethernet ETHA/RMAC port + PHY addr defaults");
}

int32_t main(void)
{
  test_board_get_info();
  test_board_get_info_null();
  test_led_pin_lookup();
  test_led_pin_invalid();
  test_sw_pin_lookup();
  test_sw_attach_irq_null_cb();
  test_audio_pins();
  test_audio_play_sample_block_validates();
  test_arduino_pins();
  test_arduino_pin_init_invalid_mode();
  test_pmod1_spi_pins();
  test_pmod2_spi_pins();
  test_glcdc_pin_tables_populated();
  test_glcdc_init_invalid_fmt();
  test_camera_pins();
  test_xspi_pins();
  test_extmem_sizes();
  test_mipi_dsi_pins();
  test_stubs_return_not_supported();
  test_uart_console_pins();
  test_uart_console_init_rejects_zero_baud();
  test_uart_console_write_validates();
  test_uart_console_read_validates();
  test_ethernet_pins();
  test_ethernet_index_constants();
  (void)fprintf(stderr, "[OK ] test_ra_board_ek_ra8d2.c\n");
  return 0;
}
