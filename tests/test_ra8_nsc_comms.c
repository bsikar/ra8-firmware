/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_nsc_comms.c
 * @brief Unit tests for libs/ra8_nsc/src/ra8_nsc_comms.c
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_i3c.h"
#include "ra8_mstp.h"
#include "ra8_nsc_comms.h"
#include "ra8_sci.h"
#include "ra8_sci_regs.h"
#include "ra8_spi.h"
#include "ra8_spi_regs.h"
#include "ra8_usb.h"
#include "unity_minimal.h"

/**
 * @enum t_comms_cfg_t
 * @brief Bus addresses and clock rates passed across the secure gateway.
 */
typedef enum : uint32_t {
  k_t_iic_addr_7b = 0x50U,     /**< 7-bit I2C address of an EEPROM-class device. */
  k_t_baud_hz     = 1000000U,  /**< Serial baud rate: 1 Mbaud.                   */
  k_t_pclka_hz    = 60000000U, /**< PCLKA feeding the divisor calculation.       */
} t_comms_cfg_t;

static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
}

static const ra8_sci_cfg_t k_sci_cfg = {
  .baud      = 115200U,
  .data_bits = k_ra8_sci_data_8,
  .parity    = k_ra8_sci_parity_none,
  .stop_bits = k_ra8_sci_stop_1,
  .pclk_hz   = 60000000U,
};

static const ra8_i3c_cfg_t k_iic_cfg = {
  .mode     = k_ra8_i3c_mode_i2c,
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
  TEST_BEGIN("ra8_nsc_sci_init forwards to ra8_sci_init");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_sci_init(0U, &k_sci_cfg));
  TEST_END("ra8_nsc_sci_init forwards to ra8_sci_init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_sci_init_null(void)
{
  TEST_BEGIN("ra8_nsc_sci_init: NULL cfg rejected");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_sci_init(0U, nullptr));
  TEST_END("ra8_nsc_sci_init: NULL cfg rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_sci_putc_getc_round_trip(void)
{
  TEST_BEGIN("ra8_nsc_sci_{putc,getc} round trip");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_sci_init(0U, &k_sci_cfg));
  /* Pre-set CSR.TDRE / CSR.RDRF so the polling paths see the
   * "ready" flags the real hardware would supply. */
  volatile r_sci_regs_t* reg = ra8_sci(0U);
  reg->CSR = (1U << (uint8_t)k_ra8_sci_csr_bit_tdre) | (1U << (uint8_t)k_ra8_sci_csr_bit_rdrf);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_sci_putc(0U, 0xA5U));
  uint8_t b = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_sci_getc(0U, &b));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_sci_getc(0U, nullptr));
  TEST_END("ra8_nsc_sci_{putc,getc} round trip");
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
  TEST_BEGIN("ra8_nsc_iic_init forwards");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_iic_init(0U, &k_iic_cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_iic_init(0U, nullptr));
  TEST_END("ra8_nsc_iic_init forwards");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_iic_write_read_null(void)
{
  TEST_BEGIN("ra8_nsc_iic_{write,read}: NULL rejected");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_iic_init(0U, &k_iic_cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_iic_write(0U, 0x50U, nullptr, 0U));
  uint8_t buf[1] = {0U};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_iic_read(0U, 0x50U, nullptr, 1U));
  /* Forward path: zero-length write is a legal stub call. */
  (void)ra8_nsc_iic_write(0U, k_t_iic_addr_7b, buf, 0U);
  TEST_END("ra8_nsc_iic_{write,read}: NULL rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_iic_read_forwards(void)
{
  TEST_BEGIN("ra8_nsc_iic_read: valid buffer forwards to ra8_i3c_read");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_iic_init(0U, &k_iic_cfg));
  uint8_t buf[1] = {0U};
  /* A non-NULL out_buf passes the veneer null guard and the (host no-op)
   * NS-range check, so the veneer forwards into ra8_i3c_read. A zero-length
   * read is rejected deterministically by the I2C driver with invalid_arg,
   * driving the forward path without depending on any polled status flag. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_nsc_iic_read(0U, 0x50U, buf, 0U));
  TEST_END("ra8_nsc_iic_read: valid buffer forwards to ra8_i3c_read");
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
  TEST_BEGIN("ra8_nsc_spi_init + xfer8");
  prep();
  ra8_spi_cfg_t cfg = {};
  cfg.mode          = k_ra8_spi_mode_0;
  cfg.baud_hz       = k_t_baud_hz;
  cfg.pclka_hz      = k_t_pclka_hz;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_spi_init(0U, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_spi_init(0U, nullptr));

  /* Pre-set SPSR.SPTEF + SPRF so the polling xfer sees ready flags.
   * SPI_B keeps both flags in SPSR's high half (bits 29 + 31). */
  volatile r_spi_regs_t* sreg = ra8_spi(0U);
  sreg->SPSR                  = k_ra8_spsr_mask_sptef | k_ra8_spsr_mask_sprf;
  uint8_t rx                  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_spi_xfer8(0U, 0xCAU, &rx));
  sreg->SPSR = k_ra8_spsr_mask_sptef | k_ra8_spsr_mask_sprf;
  /* NULL rx is legal for the legacy ra8_spi_xfer8 contract. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_spi_xfer8(0U, 0xCAU, nullptr));
  TEST_END("ra8_nsc_spi_init + xfer8");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_spi_bulk_xfers(void)
{
  TEST_BEGIN("ra8_nsc_spi bulk transfers");
  prep();
  ra8_spi_cfg_t cfg = {};
  cfg.mode          = k_ra8_spi_mode_0;
  cfg.baud_hz       = k_t_baud_hz;
  cfg.pclka_hz      = k_t_pclka_hz;
  (void)ra8_nsc_spi_init(0U, &cfg);

  uint8_t tx[4] = {1, 2, 3, 4};
  uint8_t rx[4] = {};

  /* Pre-set SPSR flags for 4 bytes of transfer. In the real hardware
   * these spin loops would check SPSR, but our mock doesn't clear them
   * automatically. We just set them so the loop falls through. */
  volatile r_spi_regs_t* sreg = ra8_spi(0U);
  sreg->SPSR                  = k_ra8_spsr_mask_sptef | k_ra8_spsr_mask_sprf;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_spi_write(0U, tx, 4U, k_ra8_spi_width_8));
  sreg->SPSR = k_ra8_spsr_mask_sptef | k_ra8_spsr_mask_sprf;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_spi_write(0U, nullptr, 4U, k_ra8_spi_width_8));

  sreg->SPSR = k_ra8_spsr_mask_sptef | k_ra8_spsr_mask_sprf;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_spi_read(0U, rx, 4U, k_ra8_spi_width_8));
  sreg->SPSR = k_ra8_spsr_mask_sptef | k_ra8_spsr_mask_sprf;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_spi_read(0U, nullptr, 4U, k_ra8_spi_width_8));

  sreg->SPSR = k_ra8_spsr_mask_sptef | k_ra8_spsr_mask_sprf;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_nsc_spi_write_read(ra8_nsc_spi_ch_bw(0U, k_ra8_spi_width_8), tx, rx, 4U));
  sreg->SPSR = k_ra8_spsr_mask_sptef | k_ra8_spsr_mask_sprf;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_nsc_spi_write_read(ra8_nsc_spi_ch_bw(0U, k_ra8_spi_width_8), nullptr, rx, 4U));
  sreg->SPSR = k_ra8_spsr_mask_sptef | k_ra8_spsr_mask_sprf;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_nsc_spi_write_read(ra8_nsc_spi_ch_bw(0U, k_ra8_spi_width_8), tx, nullptr, 4U));

  TEST_END("ra8_nsc_spi bulk transfers");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the veneers gate on a
 * single-condition `if (len > 0U)` and the width helper is a plain
 * `switch`; no `&&` or `||` on the exercised paths)
 */
static void test_spi_width16_32(void)
{
  TEST_BEGIN("ra8_nsc_spi 16/32-bit widths forward");
  prep();
  ra8_spi_cfg_t cfg = {};
  cfg.mode          = k_ra8_spi_mode_0;
  cfg.baud_hz       = k_t_baud_hz;
  cfg.pclka_hz      = k_t_pclka_hz;
  (void)ra8_nsc_spi_init(0U, &cfg);

  uint8_t tx[4] = {1, 2, 3, 4};
  uint8_t rx[4] = {};

  volatile r_spi_regs_t* sreg = ra8_spi(0U);
  /* 16-bit frames: internal_spi_unit_bytes returns 2 bytes/frame, so the
   * range check spans 4 bytes and the write forwards to ra8_spi_write. */
  sreg->SPSR = k_ra8_spsr_mask_sptef | k_ra8_spsr_mask_sprf;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_spi_write(0U, tx, 2U, k_ra8_spi_width_16));
  /* 32-bit frames: internal_spi_unit_bytes returns 4 bytes/frame. */
  sreg->SPSR = k_ra8_spsr_mask_sptef | k_ra8_spsr_mask_sprf;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_spi_read(0U, rx, 1U, k_ra8_spi_width_32));
  TEST_END("ra8_nsc_spi 16/32-bit widths forward");
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
  TEST_BEGIN("ra8_nsc_spi unsupported width rejected");
  prep();
  ra8_spi_cfg_t cfg = {};
  cfg.mode          = k_ra8_spi_mode_0;
  cfg.baud_hz       = k_t_baud_hz;
  cfg.pclka_hz      = k_t_pclka_hz;
  (void)ra8_nsc_spi_init(0U, &cfg);

  uint8_t tx[4] = {1, 2, 3, 4};
  uint8_t rx[4] = {};
  /* An unsupported frame width (not 8/16/32) drives internal_spi_unit_bytes
   * to its `switch` default (returns 0), so each multi-frame veneer rejects
   * with invalid_arg before touching the bus -- no status flag involved. */
  const ra8_spi_bit_width_t bad_width = (ra8_spi_bit_width_t)0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_nsc_spi_write(0U, tx, 4U, bad_width));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_nsc_spi_read(0U, rx, 4U, bad_width));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_nsc_spi_write_read(ra8_nsc_spi_ch_bw(0U, bad_width), tx, rx, 4U));
  TEST_END("ra8_nsc_spi unsupported width rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the single-condition
 * span-overflow guard `span > UINT32_MAX` added for T5-07; no `&&` or `||`.)
 */
static void test_spi_span_overflow_rejected(void)
{
  TEST_BEGIN("ra8_nsc_spi rejects a frame count whose byte span overflows uint32");
  prep();
  ra8_spi_cfg_t cfg = {};
  cfg.mode          = k_ra8_spi_mode_0;
  cfg.baud_hz       = k_t_baud_hz;
  cfg.pclka_hz      = k_t_pclka_hz;
  (void)ra8_nsc_spi_init(0U, &cfg);

  uint8_t tx[4] = {1, 2, 3, 4};
  uint8_t rx[4] = {};
  /* internal_spi_byte_span must compute bytes_per_unit * len in 64-bit:
   * 0x40000000 frames at width 32 (4 bytes) spans 0x100000000 (4 GiB), which
   * a 32-bit range-check length would wrap to 0 -- under-validating while the
   * driver still transfers the full len (a secret-exfiltration primitive). The
   * veneers must reject with invalid_arg before touching the bus (T5-07). */
  const uint32_t ovf_len = 0x40000000U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_nsc_spi_write(0U, tx, ovf_len, k_ra8_spi_width_32));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_nsc_spi_read(0U, rx, ovf_len, k_ra8_spi_width_32));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_nsc_spi_write_read(ra8_nsc_spi_ch_bw(0U, k_ra8_spi_width_32), tx, rx, ovf_len));
  TEST_END("ra8_nsc_spi rejects a frame count whose byte span overflows uint32");
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
  TEST_BEGIN("ra8_nsc_usb_{init,attach} forwards");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_usb_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_usb_attach(k_ra8_usb_speed_fs, true));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_usb_attach(k_ra8_usb_speed_fs, false));
  TEST_END("ra8_nsc_usb_{init,attach} forwards");
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
  test_spi_span_overflow_rejected();
  test_usb_init_and_attach();
  (void)fprintf(stderr, "[OK ] test_ra8_nsc_comms.c\n");
  return 0;
}
