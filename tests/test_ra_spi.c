/**
 * @file test_ra_spi.c
 * @brief Unit tests for spi.c (polling SPI master driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_mstp_regs.h"
#include "ra8d2_spi_regs.h"
#include "ra_dma.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_dma.h"
#include "ra_sim_mmap.h"
#include "ra_spi.h"
#include "unity_minimal.h"

/**
 * @enum ra_spi_test_ch_t
 * @brief Channel numbers used by SPI tests.
 */
typedef enum : uint8_t {
  k_ra_spi_test_ch_zero = 0U,
  k_ra_spi_test_ch_one  = 1U,
  k_ra_spi_test_ch_oor  = 2U, /**< Only SPI0..SPI1 are modelled.      */
  k_ra_spi_test_ch_huge = 200U,
} ra_spi_test_ch_t;

/**
 * @enum ra_spi_test_val_t
 * @brief Sample bytes and masks used by the SPI tests.
 */
typedef enum : uint8_t {
  k_ra_spi_test_tx_byte  = 0xA5U,
  k_ra_spi_test_rx_byte  = 0x5AU,
  k_ra_spi_test_sptef    = (uint8_t)(1U << 5U), /**< SPSR.SPTEF = bit 5.  */
  k_ra_spi_test_sprf     = (uint8_t)(1U << 7U), /**< SPSR.SPRF  = bit 7.  */
  k_ra_spi_test_both     = (uint8_t)((1U << 5U) | (1U << 7U)),
  k_ra_spi_test_spcr_en  = 0x48U, /**< k_ra_spi_spcr_enable. */
  k_ra_spi_test_spbr_def = 0x0FU, /**< k_ra_spi_spbr_default.*/
} ra_spi_test_val_t;

static void test_master_init_happy_ch0(void)
{
  TEST_BEGIN("spi master_init ch0");
  ra_sim_mmap_reset();

  const ra_err_t err = ra_spi_master_init((uint8_t)k_ra_spi_test_ch_zero);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);

  volatile r_spi_regs_t* reg = ra_spi((uint8_t)k_ra_spi_test_ch_zero);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ((int)k_ra_spi_test_spcr_en, (int)reg->SPCR);
  TEST_ASSERT_EQ((int)k_ra_spi_test_spbr_def, (int)reg->SPBR);
  TEST_ASSERT_EQ(0, (int)reg->SPPCR);
  TEST_ASSERT_EQ(0, (int)reg->SPDCR);
  TEST_ASSERT_EQ(0, (int)reg->SPCKD);
  TEST_ASSERT_EQ(0, (int)reg->SSLND);
  TEST_ASSERT_EQ(0, (int)reg->SPND);
  TEST_ASSERT_EQ(0, (int)reg->SPCR2);
  TEST_END("spi master_init ch0");
}

static void test_master_init_happy_ch1(void)
{
  TEST_BEGIN("spi master_init ch1");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_spi_master_init((uint8_t)k_ra_spi_test_ch_one));
  TEST_END("spi master_init ch1");
}

static void test_master_init_bad_channel(void)
{
  TEST_BEGIN("spi master_init bad channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_spi_master_init((uint8_t)k_ra_spi_test_ch_oor));
  TEST_END("spi master_init bad channel");
}

static void test_master_init_huge_channel(void)
{
  TEST_BEGIN("spi master_init huge channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_spi_master_init((uint8_t)k_ra_spi_test_ch_huge));
  TEST_END("spi master_init huge channel");
}

static void test_xfer8_happy_with_rx(void)
{
  TEST_BEGIN("spi xfer8 happy with rx");
  ra_sim_mmap_reset();

  volatile r_spi_regs_t* reg = ra_spi((uint8_t)k_ra_spi_test_ch_zero);
  TEST_ASSERT_NOT_NULL((void*)reg);
  /* Pre-arm SPTEF and SPRF so both polling loops pass immediately. */
  reg->SPSR = (uint8_t)k_ra_spi_test_both;

  /* On the host mock, SPDR is just ordinary RAM -- the driver writes
   * TX into SPDR and later reads SPDR as RX, so the "received" byte
   * echoes the transmitted byte. Verifying we get our TX back proves
   * the full xfer sequence executed. */
  uint8_t rx = 0U;
  TEST_ASSERT_EQ(
    (int)k_ra_ok,
    (int)ra_spi_xfer8((uint8_t)k_ra_spi_test_ch_zero, (uint8_t)k_ra_spi_test_tx_byte, &rx));
  TEST_ASSERT_EQ((int)k_ra_spi_test_tx_byte, (int)rx);
  TEST_END("spi xfer8 happy with rx");
}

static void test_xfer8_happy_null_rx(void)
{
  TEST_BEGIN("spi xfer8 happy null rx");
  ra_sim_mmap_reset();

  volatile r_spi_regs_t* reg = ra_spi((uint8_t)k_ra_spi_test_ch_one);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->SPSR = (uint8_t)k_ra_spi_test_both;

  TEST_ASSERT_EQ(
    (int)k_ra_ok,
    (int)ra_spi_xfer8((uint8_t)k_ra_spi_test_ch_one, (uint8_t)k_ra_spi_test_tx_byte, nullptr));
  TEST_END("spi xfer8 happy null rx");
}

static void test_xfer8_timeout_sptef(void)
{
  TEST_BEGIN("spi xfer8 timeout sptef");
  ra_sim_mmap_reset();

  /* SPSR = 0 -> SPTEF never sets -> first wait times out. */
  TEST_ASSERT_EQ(
    (int)k_ra_err_hw_timeout,
    (int)ra_spi_xfer8((uint8_t)k_ra_spi_test_ch_zero, (uint8_t)k_ra_spi_test_tx_byte, nullptr));
  TEST_END("spi xfer8 timeout sptef");
}

static void test_xfer8_timeout_sprf(void)
{
  TEST_BEGIN("spi xfer8 timeout sprf");
  ra_sim_mmap_reset();

  volatile r_spi_regs_t* reg = ra_spi((uint8_t)k_ra_spi_test_ch_zero);
  TEST_ASSERT_NOT_NULL((void*)reg);
  /* Only SPTEF is asserted -> second wait (SPRF) times out. */
  reg->SPSR = (uint8_t)k_ra_spi_test_sptef;

  uint8_t rx = 0xFFU;
  TEST_ASSERT_EQ(
    (int)k_ra_err_hw_timeout,
    (int)ra_spi_xfer8((uint8_t)k_ra_spi_test_ch_zero, (uint8_t)k_ra_spi_test_tx_byte, &rx));
  TEST_END("spi xfer8 timeout sprf");
}

static void test_xfer8_bad_channel(void)
{
  TEST_BEGIN("spi xfer8 bad channel");
  ra_sim_mmap_reset();

  uint8_t rx = 0U;
  TEST_ASSERT_EQ(
    (int)k_ra_err_null_ptr,
    (int)ra_spi_xfer8((uint8_t)k_ra_spi_test_ch_oor, (uint8_t)k_ra_spi_test_tx_byte, &rx));
  TEST_END("spi xfer8 bad channel");
}

/* =============================================================================
 * Wave 3.3: new API tests
 * =============================================================================
 */

static int32_t s_spi_cb_count = 0;
static int32_t s_spi_cb_err   = 0;
static void    stub_spi_cb(void* ctx, uint8_t err_mask)
{
  (void)ctx;
  ++s_spi_cb_count;
  s_spi_cb_err = (int32_t)err_mask;
}

static void prep_w33(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_spi_cb_count = 0;
  s_spi_cb_err   = 0;
}

static const ra_spi_cfg_t k_spi_cfg = {
  .baud_hz   = 10000000U, /* 10 MHz */
  .pclka_hz  = 120000000U,
  .mode      = k_ra_spi_mode_0,
  .lsb_first = false,
};

static void test_spi_init_configured(void)
{
  TEST_BEGIN("ra_spi_init: SPCR.SPE set");
  prep_w33();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_init(0U, &k_spi_cfg));
  volatile const r_spi_regs_t* reg = ra_spi(0U);
  TEST_ASSERT(reg->SPCR != 0U);
  TEST_ASSERT(reg->SPBR != 0U);
  TEST_END("ra_spi_init: SPCR.SPE set");
}

static void test_spi_init_mode_variants(void)
{
  TEST_BEGIN("ra_spi_init: mode 1/2/3 programme SPCMD bits");
  prep_w33();
  ra_spi_cfg_t cfg = k_spi_cfg;

  cfg.mode = k_ra_spi_mode_1;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_init(0U, &cfg));
  TEST_ASSERT((ra_spi(0U)->SPCMD[0] & (uint16_t)1U) != 0U);

  cfg.mode = k_ra_spi_mode_2;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_init(1U, &cfg));
  TEST_ASSERT((ra_spi(1U)->SPCMD[0] & (uint16_t)(1U << 1U)) != 0U);

  cfg.mode      = k_ra_spi_mode_3;
  cfg.lsb_first = true;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_init(0U, &cfg));
  TEST_ASSERT((ra_spi(0U)->SPCMD[0] & (uint16_t)(1U << 12U)) != 0U);
  TEST_END("ra_spi_init: mode 1/2/3 programme SPCMD bits");
}

static void test_spi_init_bad(void)
{
  TEST_BEGIN("ra_spi_init: bad inputs rejected");
  prep_w33();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_spi_init(0U, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_spi_init(9U, &k_spi_cfg));
  TEST_END("ra_spi_init: bad inputs rejected");
}

static void test_spi_deinit(void)
{
  TEST_BEGIN("ra_spi_deinit: SPCR cleared, MSTP released");
  prep_w33();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_init(0U, &k_spi_cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_deinit(0U));
  bool stopped = false;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mstp_is_stopped(k_ra_mstp_spi0, &stopped));
  TEST_ASSERT(stopped);
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_spi_deinit(9U));
  TEST_END("ra_spi_deinit: SPCR cleared, MSTP released");
}

static void test_spi_set_clock(void)
{
  TEST_BEGIN("ra_spi_set_clock: updates SPBR");
  prep_w33();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_init(0U, &k_spi_cfg));
  const uint8_t before = ra_spi(0U)->SPBR;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_set_clock(0U, 500000U, 120000000U));
  TEST_ASSERT(ra_spi(0U)->SPBR != before);
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_spi_set_clock(0U, 0U, 120000000U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_spi_set_clock(9U, 1000000U, 120000000U));
  TEST_END("ra_spi_set_clock: updates SPBR");
}

static void test_spi_errors(void)
{
  TEST_BEGIN("ra_spi_get/clear_errors");
  prep_w33();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_init(0U, &k_spi_cfg));
  volatile r_spi_regs_t* reg = ra_spi(0U);
  reg->SPSR                  = (uint8_t)((1U << 0U) | (1U << 2U) | (1U << 3U) | (1U << 4U));

  uint8_t mask = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_get_errors(0U, &mask));
  TEST_ASSERT_EQ((int32_t)((uint8_t)k_ra_spi_err_overrun | (uint8_t)k_ra_spi_err_mode |
                           (uint8_t)k_ra_spi_err_parity | (uint8_t)k_ra_spi_err_underrun),
                 (int32_t)mask);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_clear_errors(0U));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_get_errors(0U, &mask));
  TEST_ASSERT_EQ((int32_t)k_ra_spi_err_none, (int32_t)mask);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_spi_get_errors(0U, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_spi_get_errors(9U, &mask));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_spi_clear_errors(9U));
  TEST_END("ra_spi_get/clear_errors");
}

static void test_spi_attach(void)
{
  TEST_BEGIN("ra_spi_attach_transfer_handler: dispatch fires callback");
  prep_w33();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_init(0U, &k_spi_cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_spi_attach_transfer_handler(0U, stub_spi_cb, nullptr));

  /* Fake an overrun flag and fire ERI. */
  ra_spi(0U)->SPSR = (uint8_t)(1U << 0U);
  ra_spi_dispatch_spei(0U);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_spi_cb_count);
  TEST_ASSERT_EQ((int32_t)(uint8_t)k_ra_spi_err_overrun, (int32_t)s_spi_cb_err);

  /* Zero-mask dispatch is a no-op. */
  ra_spi(0U)->SPSR = 0U;
  s_spi_cb_count   = 0;
  ra_spi_dispatch_spei(0U);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)s_spi_cb_count);

  /* Out-of-range is a no-op. */
  ra_spi_dispatch_spei(9U);
  ra_spi_dispatch_spti(0U);
  ra_spi_dispatch_spti(9U);
  ra_spi_dispatch_spri(0U);
  ra_spi_dispatch_spri(9U);
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_spi_attach_transfer_handler(9U, stub_spi_cb, nullptr));
  TEST_END("ra_spi_attach_transfer_handler: dispatch fires callback");
}

static void test_spi_power(void)
{
  TEST_BEGIN("ra_spi_enter_stop / exit_stop");
  prep_w33();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_init(1U, &k_spi_cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_enter_stop(1U));
  bool stopped = false;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mstp_is_stopped(k_ra_mstp_spi1, &stopped));
  TEST_ASSERT(stopped);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_exit_stop(1U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_spi_enter_stop(9U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_spi_exit_stop(9U));
  TEST_END("ra_spi_enter_stop / exit_stop");
}

static int32_t s_spi_dma_done = 0;

static void stub_spi_dma_done(void* ctx)
{
  (void)ctx;
  ++s_spi_dma_done;
}

static void test_spi_write_dma_streams_to_spdr(void)
{
  TEST_BEGIN("ra_spi_write_dma: buffer streams into SPDR");
  prep_w33();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_init(0U, &k_spi_cfg));

  const uint8_t src[] = {0x77U, 0x88U, 0x99U};
  uint8_t       dch   = 0xFFU;
  s_spi_dma_done      = 0;
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_spi_write_dma(0U, src, (uint16_t)sizeof(src), stub_spi_dma_done, nullptr, &dch));
  TEST_ASSERT(dch < 8U);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sim_dma_memcpy(dch));
  volatile const r_spi_regs_t* reg = ra_spi(0U);
  TEST_ASSERT_EQ((int32_t)0x99U, (int32_t)(reg->SPDR & 0xFFU));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sim_dma_complete(dch));
  TEST_ASSERT_EQ(1, s_spi_dma_done);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_release(dch));
  TEST_END("ra_spi_write_dma: buffer streams into SPDR");
}

static void test_spi_read_dma_streams_from_spdr(void)
{
  TEST_BEGIN("ra_spi_read_dma: SPDR streams into buffer");
  prep_w33();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_init(0U, &k_spi_cfg));

  volatile r_spi_regs_t* reg = ra_spi(0U);
  reg->SPDR                  = 0x66U;

  uint8_t out[2] = {0U, 0U};
  uint8_t dch    = 0xFFU;
  s_spi_dma_done = 0;
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_spi_read_dma(0U, out, (uint16_t)sizeof(out), stub_spi_dma_done, nullptr, &dch));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sim_dma_memcpy(dch));
  TEST_ASSERT_EQ((int32_t)0x66U, (int32_t)out[0]);
  TEST_ASSERT_EQ((int32_t)0x66U, (int32_t)out[1]);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sim_dma_complete(dch));
  TEST_ASSERT_EQ(1, s_spi_dma_done);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_release(dch));
  TEST_END("ra_spi_read_dma: SPDR streams into buffer");
}

static void test_spi_dma_arg_validation(void)
{
  TEST_BEGIN("ra_spi_{write,read}_dma: arg validation");
  prep_w33();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_init(0U, &k_spi_cfg));

  uint8_t       dch    = 0U;
  const uint8_t src[]  = {0x12U};
  uint8_t       dst[1] = {0U};

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_spi_write_dma(0U, nullptr, 1U, nullptr, nullptr, &dch));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_spi_write_dma(0U, src, 1U, nullptr, nullptr, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_spi_read_dma(0U, nullptr, 1U, nullptr, nullptr, &dch));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_spi_read_dma(0U, dst, 1U, nullptr, nullptr, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_spi_write_dma(99U, src, 1U, nullptr, nullptr, &dch));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_spi_read_dma(99U, dst, 1U, nullptr, nullptr, &dch));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_spi_write_dma(0U, src, 0U, nullptr, nullptr, &dch));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_spi_read_dma(0U, dst, 0U, nullptr, nullptr, &dch));
  TEST_END("ra_spi_{write,read}_dma: arg validation");
}

int32_t main(void)
{
  test_master_init_happy_ch0();
  test_master_init_happy_ch1();
  test_master_init_bad_channel();
  test_master_init_huge_channel();
  test_xfer8_happy_with_rx();
  test_xfer8_happy_null_rx();
  test_xfer8_timeout_sptef();
  test_xfer8_timeout_sprf();
  test_xfer8_bad_channel();
  test_spi_init_configured();
  test_spi_init_mode_variants();
  test_spi_init_bad();
  test_spi_deinit();
  test_spi_set_clock();
  test_spi_errors();
  test_spi_attach();
  test_spi_power();
  test_spi_write_dma_streams_to_spdr();
  test_spi_read_dma_streams_from_spdr();
  test_spi_dma_arg_validation();
  (void)fprintf(stderr, "[OK  ] test_ra_spi.c\n");
  return 0;
}
