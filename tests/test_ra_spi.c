/**
 * @file test_ra_spi.c
 * @brief Unit tests for the SPI_B master driver (``ra_spi_b.c``)
 *
 * @details
 * Validates the SPI_B-flavoured public ``ra_spi`` API against the
 * 32-bit SPI_B register file in ``ra8d2_spi_regs.h``. All bit
 * positions referenced here come from FSP ``R_SPI_B0_Type`` and
 * HUM Ch 43 (p 2877-2985).
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
  k_ra_spi_test_ch_oor  = 2U, /**< Only SPI0..SPI1 are modelled. */
  k_ra_spi_test_ch_huge = 200U,
} ra_spi_test_ch_t;

/**
 * @enum ra_spi_test_val_t
 * @brief Sample bytes used by the SPI tests.
 */
typedef enum : uint32_t {
  k_ra_spi_test_tx_byte = 0xA5U,
} ra_spi_test_val_t;

/**
 * @enum ra_spi_test_spsr_t
 * @brief SPSR pre-armed flag combinations used by the polling tests.
 *
 * @details
 * SPI_B SPSR places SPTEF at bit 29 and SPRF at bit 31
 * (HUM Ch 43.2.9 p 2898). Keep these mirrored in the test so the
 * register-mock pre-condition matches what the driver polls for.
 */
typedef enum : uint32_t {
  k_ra_spi_test_sptef = 0x20000000UL,                /**< SPSR.SPTEF (bit 29). */
  k_ra_spi_test_sprf  = 0x80000000UL,                /**< SPSR.SPRF  (bit 31). */
  k_ra_spi_test_both  = 0x20000000UL | 0x80000000UL, /**< SPTEF | SPRF.        */
} ra_spi_test_spsr_t;

/**
 * @enum ra_spi_test_spcr_t
 * @brief Expected SPCR layout after init (master mode, SPE asserted).
 *
 * @details
 * SPCR.SPE = bit 0 (mask 0x00000001), SPCR.SCKASE = bit 12
 * (mask 0x00001000), SPCR.MSTR = bit 30 (mask 0x40000000).
 * The driver drives SPCR = SPE | SCKASE | MSTR.
 */
typedef enum : uint32_t {
  k_ra_spi_test_spcr_en = 0x40001001UL,
} ra_spi_test_spcr_t;

static void test_master_init_happy_ch0(void)
{
  TEST_BEGIN("spi master_init ch0");
  ra_sim_mmap_reset();

  const ra_err_t err = ra_spi_master_init(k_ra_spi_test_ch_zero);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);

  volatile r_spi_regs_t* reg = ra_spi(k_ra_spi_test_ch_zero);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ((int)k_ra_spi_test_spcr_en, (int)reg->SPCR);
  /* SPCR3.SPBR field is non-zero after init at default 1.9 MHz / 125 MHz. */
  TEST_ASSERT((reg->SPCR3 & k_ra_spcr3_mask_spbr) != 0U);
  TEST_ASSERT_EQ(0, (int)reg->SPCR2);
  TEST_ASSERT_EQ(0, (int)reg->SPDECR);
  TEST_ASSERT_EQ(0, (int)reg->SPDCR);
  TEST_ASSERT_EQ(0, (int)reg->SPDCR2);
  TEST_END("spi master_init ch0");
}

static void test_master_init_happy_ch1(void)
{
  TEST_BEGIN("spi master_init ch1");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_spi_master_init(k_ra_spi_test_ch_one));
  TEST_END("spi master_init ch1");
}

static void test_master_init_bad_channel(void)
{
  TEST_BEGIN("spi master_init bad channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_spi_master_init(k_ra_spi_test_ch_oor));
  TEST_END("spi master_init bad channel");
}

static void test_master_init_huge_channel(void)
{
  TEST_BEGIN("spi master_init huge channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_spi_master_init(k_ra_spi_test_ch_huge));
  TEST_END("spi master_init huge channel");
}

static void test_xfer8_happy_with_rx(void)
{
  TEST_BEGIN("spi xfer8 happy with rx");
  ra_sim_mmap_reset();

  volatile r_spi_regs_t* reg = ra_spi(k_ra_spi_test_ch_zero);
  TEST_ASSERT_NOT_NULL((void*)reg);
  /* Pre-arm SPTEF + SPRF so both polling loops pass immediately. */
  reg->SPSR = k_ra_spi_test_both;

  /* On the host mock, SPDR is just ordinary RAM -- the driver writes
   * TX into SPDR and later reads SPDR as RX, so the "received" byte
   * echoes the transmitted byte. */
  uint8_t rx = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_spi_xfer8(k_ra_spi_test_ch_zero, (uint8_t)k_ra_spi_test_tx_byte, &rx));
  TEST_ASSERT_EQ((int)k_ra_spi_test_tx_byte, (int)rx);
  TEST_END("spi xfer8 happy with rx");
}

static void test_xfer8_happy_null_rx(void)
{
  TEST_BEGIN("spi xfer8 happy null rx");
  ra_sim_mmap_reset();

  volatile r_spi_regs_t* reg = ra_spi(k_ra_spi_test_ch_one);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->SPSR = k_ra_spi_test_both;

  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_spi_xfer8(k_ra_spi_test_ch_one, (uint8_t)k_ra_spi_test_tx_byte, nullptr));
  TEST_END("spi xfer8 happy null rx");
}

static void test_xfer8_timeout_sptef(void)
{
  TEST_BEGIN("spi xfer8 timeout sptef");
  ra_sim_mmap_reset();

  /* SPSR = 0 -> SPTEF never sets -> first wait times out. */
  TEST_ASSERT_EQ((int)k_ra_err_hw_timeout,
                 (int)ra_spi_xfer8(k_ra_spi_test_ch_zero, (uint8_t)k_ra_spi_test_tx_byte, nullptr));
  TEST_END("spi xfer8 timeout sptef");
}

static void test_xfer8_timeout_sprf(void)
{
  TEST_BEGIN("spi xfer8 timeout sprf");
  ra_sim_mmap_reset();

  volatile r_spi_regs_t* reg = ra_spi(k_ra_spi_test_ch_zero);
  TEST_ASSERT_NOT_NULL((void*)reg);
  /* Only SPTEF is asserted -> second wait (SPRF) times out. */
  reg->SPSR = k_ra_spi_test_sptef;

  uint8_t rx = 0xFFU;
  TEST_ASSERT_EQ((int)k_ra_err_hw_timeout,
                 (int)ra_spi_xfer8(k_ra_spi_test_ch_zero, (uint8_t)k_ra_spi_test_tx_byte, &rx));
  TEST_END("spi xfer8 timeout sprf");
}

static void test_xfer8_bad_channel(void)
{
  TEST_BEGIN("spi xfer8 bad channel");
  ra_sim_mmap_reset();

  uint8_t rx = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_spi_xfer8(k_ra_spi_test_ch_oor, (uint8_t)k_ra_spi_test_tx_byte, &rx));
  TEST_END("spi xfer8 bad channel");
}

/* =============================================================================
 * Configured init / deinit / runtime / dispatch
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
  /* SPCR carries SPE | SCKASE | MSTR. */
  TEST_ASSERT((reg->SPCR & k_ra_spcr_mask_spe) != 0U);
  TEST_ASSERT((reg->SPCR & k_ra_spcr_mask_mstr) != 0U);
  /* SPCR3.SPBR is non-zero. */
  TEST_ASSERT((reg->SPCR3 & k_ra_spcr3_mask_spbr) != 0U);
  /* SPCMD0.SPB encodes 8-bit frame in [20:16]. */
  const uint32_t spb_field = (reg->SPCMD[0] & k_ra_spcmd_mask_spb) >> k_ra_spcmd_bit_spb_lo;
  TEST_ASSERT_EQ((int32_t)k_ra_spcmd_spb_8bit, (int32_t)spb_field);
  TEST_END("ra_spi_init: SPCR.SPE set");
}

static void test_spi_init_mode_variants(void)
{
  TEST_BEGIN("ra_spi_init: mode 1/2/3 programme SPCMD bits");
  prep_w33();
  ra_spi_cfg_t cfg = k_spi_cfg;

  cfg.mode = k_ra_spi_mode_1;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_init(0U, &cfg));
  TEST_ASSERT((ra_spi(0U)->SPCMD[0] & k_ra_spcmd_mask_cpha) != 0U);

  cfg.mode = k_ra_spi_mode_2;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_init(1U, &cfg));
  TEST_ASSERT((ra_spi(1U)->SPCMD[0] & k_ra_spcmd_mask_cpol) != 0U);

  cfg.mode      = k_ra_spi_mode_3;
  cfg.lsb_first = true;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_init(0U, &cfg));
  TEST_ASSERT((ra_spi(0U)->SPCMD[0] & k_ra_spcmd_mask_lsbf) != 0U);
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
  TEST_BEGIN("ra_spi_set_clock: updates SPCR3.SPBR");
  prep_w33();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_init(0U, &k_spi_cfg));
  const uint32_t before_spbr = ra_spi(0U)->SPCR3 & k_ra_spcr3_mask_spbr;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_set_clock(0U, 500000U, 120000000U));
  const uint32_t after_spbr = ra_spi(0U)->SPCR3 & k_ra_spcr3_mask_spbr;
  TEST_ASSERT(after_spbr != before_spbr);
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_spi_set_clock(0U, 0U, 120000000U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_spi_set_clock(9U, 1000000U, 120000000U));
  TEST_END("ra_spi_set_clock: updates SPCR3.SPBR");
}

static void test_spi_errors(void)
{
  TEST_BEGIN("ra_spi_get/clear_errors");
  prep_w33();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_init(0U, &k_spi_cfg));
  volatile r_spi_regs_t* reg = ra_spi(0U);
  /* Pre-arm SPSR with all four error flags asserted (OVRF/MODF/PERF/UDRF). */
  reg->SPSR = k_ra_spsr_mask_errs;

  uint8_t mask = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_get_errors(0U, &mask));
  TEST_ASSERT_EQ((int32_t)(k_ra_spi_err_overrun | k_ra_spi_err_mode | k_ra_spi_err_parity |
                           k_ra_spi_err_underrun),
                 (int32_t)mask);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_clear_errors(0U));
  /* Drivers cleared via SPSRC; the host mock backs both SPSR and SPSRC
   * with ordinary RAM so we manually clear SPSR to mirror the HW
   * write-1-clears behaviour for the get_errors readback. */
  reg->SPSR = 0U;
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

  /* Fake an overrun flag (SPSR.OVRF, bit 24) and fire ERI. */
  ra_spi(0U)->SPSR = k_ra_spsr_mask_ovrf;
  ra_spi_dispatch_spei(0U);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_spi_cb_count);
  TEST_ASSERT_EQ((int32_t)k_ra_spi_err_overrun, (int32_t)s_spi_cb_err);

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

/* =============================================================================
 * Multi-byte / multi-width polling transfers
 * =============================================================================
 */

/**
 * @enum ra_spi_test_multi_t
 * @brief Constants used by the multi-byte transfer tests.
 */
typedef enum : uint32_t {
  k_ra_spi_test_buf16  = 16U, /**< 16-byte buffer for 8-bit transfers.   */
  k_ra_spi_test_buf8   = 8U,  /**< 8-element buffer for 16-bit tests.    */
  k_ra_spi_test_buf4   = 4U,  /**< 4-element buffer for 32-bit tests.    */
  k_ra_spi_test_seed8  = 0x10UL,
  k_ra_spi_test_seed16 = 0xC100UL,
  k_ra_spi_test_seed32 = 0xCAFE0000UL,
} ra_spi_test_multi_t;

/**
 * @brief Pre-arm SPSR with both polling flags asserted.
 *
 * @details
 * On the host mock SPSR is plain RAM and SPSRC writes do not auto-clear
 * the SPSR mirror, so a single arming covers every iteration of the
 * polling loop -- matching the existing xfer8 test pattern.
 */
static void prep_spsr_both(uint8_t channel)
{
  volatile r_spi_regs_t* reg = ra_spi(channel);
  reg->SPSR                  = k_ra_spi_test_both;
}

static void test_spi_write_8bit_runs_loop(void)
{
  TEST_BEGIN("ra_spi_write: 8-bit, 16-byte payload");
  prep_w33();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_init(0U, &k_spi_cfg));
  prep_spsr_both(0U);

  uint8_t tx[k_ra_spi_test_buf16];
  for (uint32_t i = 0U; i < k_ra_spi_test_buf16; i++) {
    tx[i] = (uint8_t)(k_ra_spi_test_seed8 + i);
  }
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_spi_write(0U, tx, k_ra_spi_test_buf16, k_ra_spi_width_8));
  /* The mock retains the last byte written into SPDR. */
  volatile const r_spi_regs_t* reg = ra_spi(0U);
  TEST_ASSERT_EQ((int32_t)tx[k_ra_spi_test_buf16 - 1U], (int32_t)(reg->SPDR & 0xFFU));
  TEST_END("ra_spi_write: 8-bit, 16-byte payload");
}

static void test_spi_read_8bit_runs_loop(void)
{
  TEST_BEGIN("ra_spi_read: 8-bit, 16-byte payload");
  prep_w33();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_init(0U, &k_spi_cfg));
  prep_spsr_both(0U);

  /* Pre-load SPDR with a known pattern; on the mock, read returns the
   * most-recently-written value, so the dummy 0xFF write before each
   * pop overwrites the seeded value. We instead validate that the
   * driver consumes ``len`` units (each rx slot ends up holding the
   * driver's dummy 0xFF) and that no SPSR error is raised. */
  uint8_t rx[k_ra_spi_test_buf16];
  for (uint32_t i = 0U; i < k_ra_spi_test_buf16; i++) {
    rx[i] = 0U;
  }
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_spi_read(0U, rx, k_ra_spi_test_buf16, k_ra_spi_width_8));
  for (uint32_t i = 0U; i < k_ra_spi_test_buf16; i++) {
    TEST_ASSERT_EQ((int32_t)0xFFU, (int32_t)rx[i]);
  }
  TEST_END("ra_spi_read: 8-bit, 16-byte payload");
}

static void test_spi_write_read_16bit(void)
{
  TEST_BEGIN("ra_spi_write_read: 16-bit, 8-word full-duplex");
  prep_w33();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_init(0U, &k_spi_cfg));
  prep_spsr_both(0U);

  uint16_t tx[k_ra_spi_test_buf8];
  uint16_t rx[k_ra_spi_test_buf8];
  for (uint32_t i = 0U; i < k_ra_spi_test_buf8; i++) {
    tx[i] = (uint16_t)(k_ra_spi_test_seed16 + i);
    rx[i] = 0U;
  }
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_spi_write_read(0U, tx, rx, k_ra_spi_test_buf8, k_ra_spi_width_16));
  /* Mock echoes TX -> RX through SPDR, so each rx[i] equals tx[i]. */
  for (uint32_t i = 0U; i < k_ra_spi_test_buf8; i++) {
    TEST_ASSERT_EQ((int32_t)tx[i], (int32_t)rx[i]);
  }
  /* SPCMD0.SPB now encodes 16-bit. */
  volatile const r_spi_regs_t* reg = ra_spi(0U);
  const uint32_t spb_field         = (reg->SPCMD[0] & k_ra_spcmd_mask_spb) >> k_ra_spcmd_bit_spb_lo;
  TEST_ASSERT_EQ((int32_t)k_ra_spcmd_spb_16bit, (int32_t)spb_field);
  TEST_END("ra_spi_write_read: 16-bit, 8-word full-duplex");
}

static void test_spi_write_read_32bit(void)
{
  TEST_BEGIN("ra_spi_write_read: 32-bit, 4-word full-duplex");
  prep_w33();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_init(0U, &k_spi_cfg));
  prep_spsr_both(0U);

  uint32_t tx[k_ra_spi_test_buf4];
  uint32_t rx[k_ra_spi_test_buf4];
  for (uint32_t i = 0U; i < k_ra_spi_test_buf4; i++) {
    tx[i] = k_ra_spi_test_seed32 + i;
    rx[i] = 0U;
  }
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_spi_write_read(0U, tx, rx, k_ra_spi_test_buf4, k_ra_spi_width_32));
  for (uint32_t i = 0U; i < k_ra_spi_test_buf4; i++) {
    TEST_ASSERT_EQ((int32_t)tx[i], (int32_t)rx[i]);
  }
  volatile const r_spi_regs_t* reg = ra_spi(0U);
  const uint32_t spb_field         = (reg->SPCMD[0] & k_ra_spcmd_mask_spb) >> k_ra_spcmd_bit_spb_lo;
  TEST_ASSERT_EQ((int32_t)k_ra_spcmd_spb_32bit, (int32_t)spb_field);
  TEST_END("ra_spi_write_read: 32-bit, 4-word full-duplex");
}

static void test_spi_multi_null_args(void)
{
  TEST_BEGIN("ra_spi_{write,read,write_read}: NULL-arg rejection");
  prep_w33();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_init(0U, &k_spi_cfg));
  uint8_t buf[1] = {0U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_spi_write(0U, nullptr, 1U, k_ra_spi_width_8));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_spi_read(0U, nullptr, 1U, k_ra_spi_width_8));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_spi_write_read(0U, nullptr, buf, 1U, k_ra_spi_width_8));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_spi_write_read(0U, buf, nullptr, 1U, k_ra_spi_width_8));
  TEST_END("ra_spi_{write,read,write_read}: NULL-arg rejection");
}

static void test_spi_multi_zero_len(void)
{
  TEST_BEGIN("ra_spi_{write,read,write_read}: len==0 is no-op success");
  prep_w33();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_init(0U, &k_spi_cfg));
  /* SPSR is zero, so any actual polling would hit k_ra_err_hw_timeout.
   * len==0 must never touch SPSR or SPDR -> we expect k_ra_ok. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_write(0U, nullptr, 0U, k_ra_spi_width_8));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_read(0U, nullptr, 0U, k_ra_spi_width_8));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_spi_write_read(0U, nullptr, nullptr, 0U, k_ra_spi_width_8));
  TEST_END("ra_spi_{write,read,write_read}: len==0 is no-op success");
}

static void test_spi_multi_bad_width(void)
{
  TEST_BEGIN("ra_spi_{write,read,write_read}: invalid bit_width rejected");
  prep_w33();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_spi_init(0U, &k_spi_cfg));
  uint8_t buf[1] = {0U};
  /* 0xAA is not in {7, 15, 31}. */
  const ra_spi_bit_width_t bogus = (ra_spi_bit_width_t)0xAAU;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_spi_write(0U, buf, 1U, bogus));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_spi_read(0U, buf, 1U, bogus));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_spi_write_read(0U, buf, buf, 1U, bogus));
  TEST_END("ra_spi_{write,read,write_read}: invalid bit_width rejected");
}

static void test_spi_multi_bad_channel(void)
{
  TEST_BEGIN("ra_spi_{write,read,write_read}: invalid channel rejected");
  prep_w33();
  uint8_t buf[1] = {0U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_spi_write(9U, buf, 1U, k_ra_spi_width_8));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_spi_read(9U, buf, 1U, k_ra_spi_width_8));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_spi_write_read(9U, buf, buf, 1U, k_ra_spi_width_8));
  TEST_END("ra_spi_{write,read,write_read}: invalid channel rejected");
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
  test_spi_write_8bit_runs_loop();
  test_spi_read_8bit_runs_loop();
  test_spi_write_read_16bit();
  test_spi_write_read_32bit();
  test_spi_multi_null_args();
  test_spi_multi_zero_len();
  test_spi_multi_bad_width();
  test_spi_multi_bad_channel();
  (void)fprintf(stderr, "[OK ] test_ra_spi.c\n");
  return 0;
}
