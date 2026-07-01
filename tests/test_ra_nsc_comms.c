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
#include "ra_i3c.h"
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

static const ra_i3c_cfg_t k_iic_cfg = {
  .mode     = k_ra_i3c_mode_i2c,
  .bus_hz   = 100000U,
  .pclka_hz = 60000000U,
};

/* =============================================================================
 * SCI veneers
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_sci_init_forwards(void)
{
  TEST_BEGIN("ra_nsc_sci_init forwards to ra_sci_init");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_nsc_sci_init(0U, &k_sci_cfg));
  TEST_END("ra_nsc_sci_init forwards to ra_sci_init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_sci_init_null(void)
{
  TEST_BEGIN("ra_nsc_sci_init: NULL cfg rejected");
  prep();
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_nsc_sci_init(0U, nullptr));
  TEST_END("ra_nsc_sci_init: NULL cfg rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_sci_putc_getc_round_trip(void)
{
  TEST_BEGIN("ra_nsc_sci_{putc,getc} round trip");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_nsc_sci_init(0U, &k_sci_cfg));
  /* Pre-set CSR.TDRE / CSR.RDRF so the polling paths see the
   * "ready" flags the real hardware would supply. */
  volatile r_sci_regs_t* reg = ra_sci(0U);
  reg->CSR = (1U << (uint8_t)k_ra_sci_csr_bit_tdre) | (1U << (uint8_t)k_ra_sci_csr_bit_rdrf);
  TEST_ASSERT_EQ(k_ra_ok, ra_nsc_sci_putc(0U, 0xA5U));
  uint8_t b = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_nsc_sci_getc(0U, &b));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_nsc_sci_getc(0U, nullptr));
  TEST_END("ra_nsc_sci_{putc,getc} round trip");
}

/* =============================================================================
 * IIC veneers
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_iic_init_forwards(void)
{
  TEST_BEGIN("ra_nsc_iic_init forwards");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_nsc_iic_init(0U, &k_iic_cfg));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_nsc_iic_init(0U, nullptr));
  TEST_END("ra_nsc_iic_init forwards");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_iic_write_read_null(void)
{
  TEST_BEGIN("ra_nsc_iic_{write,read}: NULL rejected");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_nsc_iic_init(0U, &k_iic_cfg));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_nsc_iic_write(0U, 0x50U, nullptr, 0U));
  uint8_t buf[1] = {0U};
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_nsc_iic_read(0U, 0x50U, nullptr, 1U));
  /* Forward path: zero-length write is a legal stub call. */
  (void)ra_nsc_iic_write(0U, 0x50U, buf, 0U);
  TEST_END("ra_nsc_iic_{write,read}: NULL rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_iic_read_forwards(void)
{
  TEST_BEGIN("ra_nsc_iic_read: valid buffer forwards to ra_i3c_read");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_nsc_iic_init(0U, &k_iic_cfg));
  uint8_t buf[1] = {0U};
  /* A non-NULL out_buf passes the veneer null guard and the (host no-op)
   * NS-range check, so the veneer forwards into ra_i3c_read. A zero-length
   * read is rejected deterministically by the I2C driver with invalid_arg,
   * driving the forward path without depending on any polled status flag. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_nsc_iic_read(0U, 0x50U, buf, 0U));
  TEST_END("ra_nsc_iic_read: valid buffer forwards to ra_i3c_read");
}

/* =============================================================================
 * SPI veneers
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_spi_init_and_xfer(void)
{
  TEST_BEGIN("ra_nsc_spi_init + xfer8");
  prep();
  ra_spi_cfg_t cfg = {};
  cfg.mode         = k_ra_spi_mode_0;
  cfg.baud_hz      = 1000000U;
  cfg.pclka_hz     = 60000000U;
  TEST_ASSERT_EQ(k_ra_ok, ra_nsc_spi_init(0U, &cfg));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_nsc_spi_init(0U, nullptr));

  /* Pre-set SPSR.SPTEF + SPRF so the polling xfer sees ready flags.
   * SPI_B keeps both flags in SPSR's high half (bits 29 + 31). */
  volatile r_spi_regs_t* sreg = ra_spi(0U);
  sreg->SPSR                  = k_ra_spsr_mask_sptef | k_ra_spsr_mask_sprf;
  uint8_t rx                  = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_nsc_spi_xfer8(0U, 0xCAU, &rx));
  sreg->SPSR = k_ra_spsr_mask_sptef | k_ra_spsr_mask_sprf;
  /* NULL rx is legal for the legacy ra_spi_xfer8 contract. */
  TEST_ASSERT_EQ(k_ra_ok, ra_nsc_spi_xfer8(0U, 0xCAU, nullptr));
  TEST_END("ra_nsc_spi_init + xfer8");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_spi_bulk_xfers(void)
{
  TEST_BEGIN("ra_nsc_spi bulk transfers");
  prep();
  ra_spi_cfg_t cfg = {};
  cfg.mode         = k_ra_spi_mode_0;
  cfg.baud_hz      = 1000000U;
  cfg.pclka_hz     = 60000000U;
  (void)ra_nsc_spi_init(0U, &cfg);

  uint8_t tx[4] = {1, 2, 3, 4};
  uint8_t rx[4] = {};

  /* Pre-set SPSR flags for 4 bytes of transfer. In the real hardware
   * these spin loops would check SPSR, but our mock doesn't clear them
   * automatically. We just set them so the loop falls through. */
  volatile r_spi_regs_t* sreg = ra_spi(0U);
  sreg->SPSR                  = k_ra_spsr_mask_sptef | k_ra_spsr_mask_sprf;

  TEST_ASSERT_EQ(k_ra_ok, ra_nsc_spi_write(0U, tx, 4U, k_ra_spi_width_8));
  sreg->SPSR = k_ra_spsr_mask_sptef | k_ra_spsr_mask_sprf;
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_nsc_spi_write(0U, nullptr, 4U, k_ra_spi_width_8));

  sreg->SPSR = k_ra_spsr_mask_sptef | k_ra_spsr_mask_sprf;
  TEST_ASSERT_EQ(k_ra_ok, ra_nsc_spi_read(0U, rx, 4U, k_ra_spi_width_8));
  sreg->SPSR = k_ra_spsr_mask_sptef | k_ra_spsr_mask_sprf;
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_nsc_spi_read(0U, nullptr, 4U, k_ra_spi_width_8));

  sreg->SPSR = k_ra_spsr_mask_sptef | k_ra_spsr_mask_sprf;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_nsc_spi_write_read(ra_nsc_spi_ch_bw(0U, k_ra_spi_width_8), tx, rx, 4U));
  sreg->SPSR = k_ra_spsr_mask_sptef | k_ra_spsr_mask_sprf;
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_nsc_spi_write_read(ra_nsc_spi_ch_bw(0U, k_ra_spi_width_8), nullptr, rx, 4U));
  sreg->SPSR = k_ra_spsr_mask_sptef | k_ra_spsr_mask_sprf;
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_nsc_spi_write_read(ra_nsc_spi_ch_bw(0U, k_ra_spi_width_8), tx, nullptr, 4U));

  TEST_END("ra_nsc_spi bulk transfers");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the veneers gate on a
 * single-condition `if (len > 0U)` and the width helper is a plain
 * `switch`; no `&&` or `||` on the exercised paths)
 */
static void test_spi_width16_32(void)
{
  TEST_BEGIN("ra_nsc_spi 16/32-bit widths forward");
  prep();
  ra_spi_cfg_t cfg = {};
  cfg.mode         = k_ra_spi_mode_0;
  cfg.baud_hz      = 1000000U;
  cfg.pclka_hz     = 60000000U;
  (void)ra_nsc_spi_init(0U, &cfg);

  uint8_t tx[4] = {1, 2, 3, 4};
  uint8_t rx[4] = {};

  volatile r_spi_regs_t* sreg = ra_spi(0U);
  /* 16-bit frames: internal_spi_unit_bytes returns 2 bytes/frame, so the
   * range check spans 4 bytes and the write forwards to ra_spi_write. */
  sreg->SPSR = k_ra_spsr_mask_sptef | k_ra_spsr_mask_sprf;
  TEST_ASSERT_EQ(k_ra_ok, ra_nsc_spi_write(0U, tx, 2U, k_ra_spi_width_16));
  /* 32-bit frames: internal_spi_unit_bytes returns 4 bytes/frame. */
  sreg->SPSR = k_ra_spsr_mask_sptef | k_ra_spsr_mask_sprf;
  TEST_ASSERT_EQ(k_ra_ok, ra_nsc_spi_read(0U, rx, 1U, k_ra_spi_width_32));
  TEST_END("ra_nsc_spi 16/32-bit widths forward");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the veneers gate on
 * single-condition `if (len > 0U)` / `if (bytes_per_unit == 0U)` checks
 * and the width helper's `switch` default; no `&&` or `||` on the
 * exercised paths)
 */
static void test_spi_invalid_width(void)
{
  TEST_BEGIN("ra_nsc_spi unsupported width rejected");
  prep();
  ra_spi_cfg_t cfg = {};
  cfg.mode         = k_ra_spi_mode_0;
  cfg.baud_hz      = 1000000U;
  cfg.pclka_hz     = 60000000U;
  (void)ra_nsc_spi_init(0U, &cfg);

  uint8_t tx[4] = {1, 2, 3, 4};
  uint8_t rx[4] = {};
  /* An unsupported frame width (not 8/16/32) drives internal_spi_unit_bytes
   * to its `switch` default (returns 0), so each multi-frame veneer rejects
   * with invalid_arg before touching the bus -- no status flag involved. */
  const ra_spi_bit_width_t bad_width = (ra_spi_bit_width_t)0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_nsc_spi_write(0U, tx, 4U, bad_width));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_nsc_spi_read(0U, rx, 4U, bad_width));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_nsc_spi_write_read(ra_nsc_spi_ch_bw(0U, bad_width), tx, rx, 4U));
  TEST_END("ra_nsc_spi unsupported width rejected");
}

/* =============================================================================
 * USB veneers
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_usb_init_and_attach(void)
{
  TEST_BEGIN("ra_nsc_usb_{init,attach} forwards");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_nsc_usb_init(k_ra_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra_ok, ra_nsc_usb_attach(k_ra_usb_speed_fs, true));
  TEST_ASSERT_EQ(k_ra_ok, ra_nsc_usb_attach(k_ra_usb_speed_fs, false));
  TEST_END("ra_nsc_usb_{init,attach} forwards");
}

int32_t main(void)
{
  test_sci_init_forwards();
  test_sci_init_null();
  test_sci_putc_getc_round_trip();
  test_iic_init_forwards();
  test_iic_write_read_null();
  test_iic_read_forwards();
  test_spi_init_and_xfer();
  test_spi_bulk_xfers();
  test_spi_width16_32();
  test_spi_invalid_width();
  test_usb_init_and_attach();
  (void)fprintf(stderr, "[OK ] test_ra_nsc_comms.c\n");
  return 0;
}
