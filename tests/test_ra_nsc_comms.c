/**
 * @file test_ra_nsc_comms.c
 * @brief Unit tests for libs/ra_nsc/src/ra_nsc_comms.c
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_sci_regs.h"
#include "ra8d2_spi_regs.h"
#include "ra_err.h"
#include "ra_iic_b.h"
#include "ra_mstp.h"
#include "ra_nsc_comms.h"
#include "ra_sci.h"
#include "ra_sim_mmap.h"
#include "ra_spi.h"
#include "ra_usb.h"
#include "unity_minimal.h"

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
}

static const ra_sci_cfg_t k_sci_cfg = {
  .baud      = 115200U,
  .data_bits = k_ra_sci_data_8,
  .parity    = k_ra_sci_parity_none,
  .stop_bits = k_ra_sci_stop_1,
  .pclk_hz   = 60000000U,
};

static const ra_iic_cfg_t k_iic_cfg = {
  .bus_hz   = 100000U,
  .pclka_hz = 60000000U,
};

/* =============================================================================
 * SCI veneers
 * =============================================================================
 */

static void test_sci_init_forwards(void)
{
  TEST_BEGIN("ra_nsc_sci_init forwards to ra_sci_init");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_sci_init(0U, &k_sci_cfg));
  TEST_END("ra_nsc_sci_init forwards to ra_sci_init");
}

static void test_sci_init_null(void)
{
  TEST_BEGIN("ra_nsc_sci_init: NULL cfg rejected");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_nsc_sci_init(0U, nullptr));
  TEST_END("ra_nsc_sci_init: NULL cfg rejected");
}

static void test_sci_putc_getc_round_trip(void)
{
  TEST_BEGIN("ra_nsc_sci_{putc,getc} round trip");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_sci_init(0U, &k_sci_cfg));
  /* Pre-set CSR.TDRE / CSR.RDRF so the polling paths see the
   * "ready" flags the real hardware would supply. */
  volatile r_sci_regs_t* reg = ra_sci(0U);
  reg->CSR = (1U << (uint8_t)k_ra_sci_csr_bit_tdre) | (1U << (uint8_t)k_ra_sci_csr_bit_rdrf);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_sci_putc(0U, 0xA5U));
  uint8_t b = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_sci_getc(0U, &b));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_nsc_sci_getc(0U, nullptr));
  TEST_END("ra_nsc_sci_{putc,getc} round trip");
}

/* =============================================================================
 * IIC veneers
 * =============================================================================
 */

static void test_iic_init_forwards(void)
{
  TEST_BEGIN("ra_nsc_iic_init forwards");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_iic_init(0U, &k_iic_cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_nsc_iic_init(0U, nullptr));
  TEST_END("ra_nsc_iic_init forwards");
}

static void test_iic_write_read_null(void)
{
  TEST_BEGIN("ra_nsc_iic_{write,read}: NULL rejected");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_iic_init(0U, &k_iic_cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_nsc_iic_write(0U, 0x50U, nullptr, 0U));
  uint8_t buf[1] = {0U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_nsc_iic_read(0U, 0x50U, nullptr, 1U));
  /* Forward path: zero-length write is a legal stub call. */
  (void)ra_nsc_iic_write(0U, 0x50U, buf, 0U);
  TEST_END("ra_nsc_iic_{write,read}: NULL rejected");
}

/* =============================================================================
 * SPI veneers
 * =============================================================================
 */

static void test_spi_init_and_xfer(void)
{
  TEST_BEGIN("ra_nsc_spi_init + xfer8");
  prep();
  ra_spi_cfg_t cfg = {};
  cfg.mode         = k_ra_spi_mode_0;
  cfg.baud_hz      = 1000000U;
  cfg.pclka_hz     = 60000000U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_spi_init(0U, &cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_nsc_spi_init(0U, nullptr));

  /* Pre-set SPSR.SPTEF + SPRF so the polling xfer sees ready flags.
   * SPI_B keeps both flags in SPSR's high half (bits 29 + 31). */
  volatile r_spi_regs_t* sreg = ra_spi(0U);
  sreg->SPSR                  = k_ra_spsr_mask_sptef | k_ra_spsr_mask_sprf;
  uint8_t rx                  = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_spi_xfer8(0U, 0xCAU, &rx));
  sreg->SPSR = k_ra_spsr_mask_sptef | k_ra_spsr_mask_sprf;
  /* NULL rx is legal for the legacy ra_spi_xfer8 contract. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_spi_xfer8(0U, 0xCAU, nullptr));
  TEST_END("ra_nsc_spi_init + xfer8");
}

/* =============================================================================
 * USB veneers
 * =============================================================================
 */

static void test_usb_init_and_attach(void)
{
  TEST_BEGIN("ra_nsc_usb_{init,attach} forwards");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_usb_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_usb_attach(k_ra_usb_speed_fs, true));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_nsc_usb_attach(k_ra_usb_speed_fs, false));
  TEST_END("ra_nsc_usb_{init,attach} forwards");
}

int32_t main(void)
{
  test_sci_init_forwards();
  test_sci_init_null();
  test_sci_putc_getc_round_trip();
  test_iic_init_forwards();
  test_iic_write_read_null();
  test_spi_init_and_xfer();
  test_usb_init_and_attach();
  (void)fprintf(stderr, "[OK ] test_ra_nsc_comms.c\n");
  return 0;
}
