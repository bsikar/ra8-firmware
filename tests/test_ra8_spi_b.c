/**
 * @file test_ra8_spi_b.c
 * @brief MC/DC unit tests for ra8_spi_b.c (SPI_B controller driver).
 *
 * @details
 * Targets the nine compound boolean decisions in ra8_spi_b.c that
 * lacked any test coverage prior to this file (DO-178C Level B
 * MC/DC obligations):
 *
 *  - line 177  internal_spbr   : (baud_hz == 0) || (pclka_hz == 0)
 *  - line 218  internal_spcmd  : (mode==1) || (mode==3)              [CPHA]
 *  - line 221  internal_spcmd  : (mode==2) || (mode==3)              [CPOL]
 *  - line 714  internal_xfer_common : (tx==NULL) && (rx==NULL)
 *  - line 766  ra8_spi_write    : (tx==NULL) && (len > 0)
 *  - line 790  ra8_spi_read     : (rx==NULL) && (len > 0)
 *  - line 1056 ra8_spi_write_dma: (channel >= count) || (len == 0)
 *  - line 1106 ra8_spi_read_dma : (channel >= count) || (len == 0)
 *  - line 1190 ra8_spi_dispatch_spei : (mask != 0) && (cb != NULL)
 *
 * The internal helpers are exercised via the public entry points
 * (ra8_spi_init reaches both internal_spbr and internal_spcmd; the
 * xfer wrappers reach internal_xfer_common; ra8_spi_attach_transfer
 * + ra8_spi_dispatch_spei reach the SPEI compound).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_dma.h"
#include "ra8_dmac.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_spi.h"
#include "ra8_spi_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_spi_b_t
 * @brief Transmit bytes and the DMA-channel out-parameter seed.
 */
typedef enum : uint8_t {
  k_t_tx_byte_a    = 0xA5U, /**< Byte the first transmit arm sends. */
  k_t_tx_byte_b    = 0xC3U, /**< A different byte for the second, so a stale
                                 shift register cannot pass as a fresh write.   */
  k_t_dma_ch_unset = 0xFFU, /**< Pre-set DMA channel; a call that allocates
                                 none must leave this value.                    */
} t_spi_b_t;

/**
 * @enum ra8_spi_b_test_const_t
 * @brief Local constants for the SPI_B MC/DC tests.
 */
typedef enum : uint32_t {
  k_test_baud_hz     = 1000000UL,   /**< 1 MHz nominal SPI clock.    */
  k_test_pclka_hz    = 100000000UL, /**< 100 MHz PCLKA.              */
  k_test_channel_0   = 0U,          /**< First SPI_B channel.        */
  k_test_channel_1   = 1U,          /**< Second SPI_B channel.       */
  k_test_channel_oor = 2U,          /**< Out-of-range channel index. */
} ra8_spi_b_test_const_t;

/** @var s_spei_calls    SPEI dispatcher callback hit count. */
static uint32_t s_spei_calls;
/** @var s_spei_last_mask Captured mask argument from last call. */
static uint8_t s_spei_last_mask;

/**
 * @brief Stub callback for SPEI dispatch tests.
 * @param[in] ctx Caller context (ignored).
 * @param[in] mask SPI error mask captured into s_spei_last_mask.
 */
static void stub_spei_cb(void* ctx, uint8_t mask)
{
  (void)ctx;
  ++s_spei_calls;
  s_spei_last_mask = mask;
}

/**
 * @brief Re-prime the fake + DMA state between tests.
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_dma_init();
  s_spei_calls     = 0U;
  s_spei_last_mask = 0U;
}

/**
 * @test test_mcdc_internal_spbr_zero_args
 * @par MC/DC:
 * Decision (line 177): `(baud_hz == 0U) || (pclka_hz == 0U)` (2 cond, N+1=3).
 * - V1: baud=1MHz, pclka=100MHz -> F||F -> F (control)
 * - V2: baud=0,    pclka=100MHz -> T||- -> T (varies baud)
 * - V3: baud=1MHz, pclka=0      -> F||T -> T (varies pclka)
 *
 * Reached through ra8_spi_init -> internal_spbr. SPCR3 SPBR field is
 * inspected: zero iff guard returned 0.
 */
static void test_mcdc_internal_spbr_zero_args(void)
{
  TEST_BEGIN("spi_b MC/DC internal_spbr: baud||pclka == 0");
  prep();
  ra8_spi_cfg_t cfg = {
    .baud_hz   = (uint32_t)k_test_baud_hz,
    .pclka_hz  = (uint32_t)k_test_pclka_hz,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_init((uint8_t)k_test_channel_0, &cfg));
  TEST_ASSERT(((ra8_spi((uint8_t)k_test_channel_0)->SPCR3) & k_ra8_spcr3_mask_spbr) != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_deinit((uint8_t)k_test_channel_0));

  cfg.baud_hz  = 0U;
  cfg.pclka_hz = (uint32_t)k_test_pclka_hz;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_init((uint8_t)k_test_channel_0, &cfg));
  TEST_ASSERT_EQ(0, ((ra8_spi((uint8_t)k_test_channel_0)->SPCR3) & k_ra8_spcr3_mask_spbr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_deinit((uint8_t)k_test_channel_0));

  cfg.baud_hz  = (uint32_t)k_test_baud_hz;
  cfg.pclka_hz = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_init((uint8_t)k_test_channel_0, &cfg));
  TEST_ASSERT_EQ(0, ((ra8_spi((uint8_t)k_test_channel_0)->SPCR3) & k_ra8_spcr3_mask_spbr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_deinit((uint8_t)k_test_channel_0));
  TEST_END("spi_b MC/DC internal_spbr: baud||pclka == 0");
}

/**
 * @test test_mcdc_internal_spcmd_cpha
 * @par MC/DC:
 * Decision (line 218): `(mode == k_ra8_spi_mode_1) || (mode == k_ra8_spi_mode_3)`
 * (2 cond, N+1=3). Mutually exclusive on mode value.
 * - V1: mode=0 -> F||F -> F (CPHA=0)
 * - V2: mode=1 -> T||F -> T (CPHA=1, varies C1)
 * - V3: mode=3 -> F||T -> T (CPHA=1, varies C2)
 */
static void test_mcdc_internal_spcmd_cpha(void)
{
  TEST_BEGIN("spi_b MC/DC internal_spcmd: CPHA selector");
  prep();
  ra8_spi_cfg_t cfg = {
    .baud_hz   = (uint32_t)k_test_baud_hz,
    .pclka_hz  = (uint32_t)k_test_pclka_hz,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_init((uint8_t)k_test_channel_0, &cfg));
  TEST_ASSERT_EQ(0, ((ra8_spi((uint8_t)k_test_channel_0)->SPCMD[0]) & k_ra8_spcmd_mask_cpha));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_deinit((uint8_t)k_test_channel_0));

  cfg.mode = k_ra8_spi_mode_1;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_init((uint8_t)k_test_channel_0, &cfg));
  TEST_ASSERT(((ra8_spi((uint8_t)k_test_channel_0)->SPCMD[0]) & k_ra8_spcmd_mask_cpha) != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_deinit((uint8_t)k_test_channel_0));

  cfg.mode = k_ra8_spi_mode_3;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_init((uint8_t)k_test_channel_0, &cfg));
  TEST_ASSERT(((ra8_spi((uint8_t)k_test_channel_0)->SPCMD[0]) & k_ra8_spcmd_mask_cpha) != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_deinit((uint8_t)k_test_channel_0));
  TEST_END("spi_b MC/DC internal_spcmd: CPHA selector");
}

/**
 * @test test_mcdc_internal_spcmd_cpol
 * @par MC/DC:
 * Decision (line 221): `(mode == k_ra8_spi_mode_2) || (mode == k_ra8_spi_mode_3)`
 * (2 cond, N+1=3).
 * - V1: mode=0 -> F||F -> F
 * - V2: mode=2 -> T||F -> T
 * - V3: mode=3 -> F||T -> T
 */
static void test_mcdc_internal_spcmd_cpol(void)
{
  TEST_BEGIN("spi_b MC/DC internal_spcmd: CPOL selector");
  prep();
  ra8_spi_cfg_t cfg = {
    .baud_hz   = (uint32_t)k_test_baud_hz,
    .pclka_hz  = (uint32_t)k_test_pclka_hz,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_init((uint8_t)k_test_channel_0, &cfg));
  TEST_ASSERT_EQ(0, ((ra8_spi((uint8_t)k_test_channel_0)->SPCMD[0]) & k_ra8_spcmd_mask_cpol));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_deinit((uint8_t)k_test_channel_0));

  cfg.mode = k_ra8_spi_mode_2;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_init((uint8_t)k_test_channel_0, &cfg));
  TEST_ASSERT(((ra8_spi((uint8_t)k_test_channel_0)->SPCMD[0]) & k_ra8_spcmd_mask_cpol) != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_deinit((uint8_t)k_test_channel_0));

  cfg.mode = k_ra8_spi_mode_3;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_init((uint8_t)k_test_channel_0, &cfg));
  TEST_ASSERT(((ra8_spi((uint8_t)k_test_channel_0)->SPCMD[0]) & k_ra8_spcmd_mask_cpol) != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_deinit((uint8_t)k_test_channel_0));
  TEST_END("spi_b MC/DC internal_spcmd: CPOL selector");
}

/**
 * @test test_mcdc_xfer_common_both_null
 * @par MC/DC:
 * Decision (line 714): `(tx == nullptr) && (rx == nullptr)` (2 cond).
 * - V1: tx=valid, rx=valid     -> F&&F -> F (proceeds)
 * - V2: tx=NULL,  rx=valid     -> T&&F -> F (proceeds)
 * - V3: tx=NULL,  rx=NULL      -> T&&T -> T (null_ptr error)
 * - V4: tx=valid, rx=NULL      -> F&&T -> F (proceeds)
 * V1+V3 + V4+V3 establish independence pairs (tx and rx each flip
 * the outcome with the other held).
 */
static void test_mcdc_xfer_common_both_null(void)
{
  TEST_BEGIN("spi_b MC/DC xfer_common: tx==NULL && rx==NULL");
  prep();
  ra8_spi_cfg_t cfg = {
    .baud_hz   = (uint32_t)k_test_baud_hz,
    .pclka_hz  = (uint32_t)k_test_pclka_hz,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_init((uint8_t)k_test_channel_0, &cfg));
  ra8_spi((uint8_t)k_test_channel_0)->SPSR = k_ra8_spsr_mask_sptef | k_ra8_spsr_mask_sprf;

  uint8_t tx_buf[1] = {k_t_tx_byte_a};
  uint8_t rx_buf[1] = {0U};

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_spi_write_read((uint8_t)k_test_channel_0, tx_buf, rx_buf, 1U, k_ra8_spi_width_8));
  ra8_spi((uint8_t)k_test_channel_0)->SPSR = k_ra8_spsr_mask_sptef | k_ra8_spsr_mask_sprf;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_read((uint8_t)k_test_channel_0, rx_buf, 1U, k_ra8_spi_width_8));
  ra8_spi((uint8_t)k_test_channel_0)->SPSR = k_ra8_spsr_mask_sptef | k_ra8_spsr_mask_sprf;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_write((uint8_t)k_test_channel_0, tx_buf, 1U, k_ra8_spi_width_8));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_spi_write_read((uint8_t)k_test_channel_0, nullptr, nullptr, 1U, k_ra8_spi_width_8));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_deinit((uint8_t)k_test_channel_0));
  TEST_END("spi_b MC/DC xfer_common: tx==NULL && rx==NULL");
}

/**
 * @test test_mcdc_write_null_guard
 * @par MC/DC:
 * Decision (line 766): `(tx == nullptr) && (len > 0U)` (2 cond, N+1=3).
 * - V1: tx=valid, len=1 -> F&&T -> F
 * - V2: tx=NULL,  len=1 -> T&&T -> T
 * - V3: tx=NULL,  len=0 -> T&&F -> F
 */
static void test_mcdc_write_null_guard(void)
{
  TEST_BEGIN("spi_b MC/DC ra8_spi_write: tx==NULL && len>0");
  prep();
  ra8_spi_cfg_t cfg = {
    .baud_hz   = (uint32_t)k_test_baud_hz,
    .pclka_hz  = (uint32_t)k_test_pclka_hz,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_init((uint8_t)k_test_channel_0, &cfg));
  ra8_spi((uint8_t)k_test_channel_0)->SPSR = k_ra8_spsr_mask_sptef | k_ra8_spsr_mask_sprf;

  uint8_t tx_buf[1] = {k_t_tx_byte_b};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_write((uint8_t)k_test_channel_0, tx_buf, 1U, k_ra8_spi_width_8));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_spi_write((uint8_t)k_test_channel_0, nullptr, 1U, k_ra8_spi_width_8));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_spi_write((uint8_t)k_test_channel_0, nullptr, 0U, k_ra8_spi_width_8));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_deinit((uint8_t)k_test_channel_0));
  TEST_END("spi_b MC/DC ra8_spi_write: tx==NULL && len>0");
}

/**
 * @test test_mcdc_read_null_guard
 * @par MC/DC:
 * Decision (line 790): `(rx == nullptr) && (len > 0U)` (2 cond, N+1=3).
 */
static void test_mcdc_read_null_guard(void)
{
  TEST_BEGIN("spi_b MC/DC ra8_spi_read: rx==NULL && len>0");
  prep();
  ra8_spi_cfg_t cfg = {
    .baud_hz   = (uint32_t)k_test_baud_hz,
    .pclka_hz  = (uint32_t)k_test_pclka_hz,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_init((uint8_t)k_test_channel_0, &cfg));
  ra8_spi((uint8_t)k_test_channel_0)->SPSR = k_ra8_spsr_mask_sptef | k_ra8_spsr_mask_sprf;

  uint8_t rx_buf[1] = {0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_read((uint8_t)k_test_channel_0, rx_buf, 1U, k_ra8_spi_width_8));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_spi_read((uint8_t)k_test_channel_0, nullptr, 1U, k_ra8_spi_width_8));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_read((uint8_t)k_test_channel_0, nullptr, 0U, k_ra8_spi_width_8));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_deinit((uint8_t)k_test_channel_0));
  TEST_END("spi_b MC/DC ra8_spi_read: rx==NULL && len>0");
}

/**
 * @test test_mcdc_write_dma_arg_guard
 * @par MC/DC:
 * Decision (line 1056): `(channel >= count) || (len == 0U)` (2 cond, N+1=3).
 */
static void test_mcdc_write_dma_arg_guard(void)
{
  TEST_BEGIN("spi_b MC/DC ra8_spi_write_dma: channel || len arg guard");
  prep();
  uint8_t   out_dma_ch = k_t_dma_ch_unset;
  uint8_t   buf[1]     = {0x01U};
  ra8_err_t e =
    ra8_spi_write_dma((uint8_t)k_test_channel_0, buf, 1U, nullptr, nullptr, &out_dma_ch);
  TEST_ASSERT(e != k_ra8_err_invalid_arg);
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_spi_write_dma((uint8_t)k_test_channel_oor, buf, 1U, nullptr, nullptr, &out_dma_ch));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_spi_write_dma((uint8_t)k_test_channel_0, buf, 0U, nullptr, nullptr, &out_dma_ch));
  TEST_END("spi_b MC/DC ra8_spi_write_dma: channel || len arg guard");
}

/**
 * @test test_mcdc_read_dma_arg_guard
 * @par MC/DC:
 * Decision (line 1106): `(channel >= count) || (len == 0U)` (2 cond, N+1=3).
 */
static void test_mcdc_read_dma_arg_guard(void)
{
  TEST_BEGIN("spi_b MC/DC ra8_spi_read_dma: channel || len arg guard");
  prep();
  uint8_t   out_dma_ch = k_t_dma_ch_unset;
  uint8_t   buf[1]     = {0U};
  ra8_err_t e = ra8_spi_read_dma((uint8_t)k_test_channel_0, buf, 1U, nullptr, nullptr, &out_dma_ch);
  TEST_ASSERT(e != k_ra8_err_invalid_arg);
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_spi_read_dma((uint8_t)k_test_channel_oor, buf, 1U, nullptr, nullptr, &out_dma_ch));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_spi_read_dma((uint8_t)k_test_channel_0, buf, 0U, nullptr, nullptr, &out_dma_ch));
  TEST_END("spi_b MC/DC ra8_spi_read_dma: channel || len arg guard");
}

/**
 * @test test_mcdc_dispatch_spei_callback_guard
 * @par MC/DC:
 * Decision (line 1190): `(mask != 0U) && (cb != nullptr)` (2 cond, N+1=3).
 * - V1: mask=err, cb=stub -> T&&T -> T (callback fires)
 * - V2: mask=0,   cb=stub -> F&&T -> F (cb not invoked)
 * - V3: mask=err, cb=NULL -> T&&F -> F (cb not invoked)
 */
static void test_mcdc_dispatch_spei_callback_guard(void)
{
  TEST_BEGIN("spi_b MC/DC dispatch_spei: mask && cb");
  prep();
  ra8_spi_cfg_t cfg = {
    .baud_hz   = (uint32_t)k_test_baud_hz,
    .pclka_hz  = (uint32_t)k_test_pclka_hz,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_init((uint8_t)k_test_channel_0, &cfg));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_spi_attach_transfer_handler((uint8_t)k_test_channel_0,
                                                 stub_spei_cb,
                                                 (void*)(uintptr_t)0xCAFEU));
  ra8_spi((uint8_t)k_test_channel_0)->SPSR = k_ra8_spsr_mask_ovrf;
  s_spei_calls                             = 0U;
  ra8_spi_dispatch_spei((uint8_t)k_test_channel_0);
  TEST_ASSERT_EQ(1, s_spei_calls);
  TEST_ASSERT(s_spei_last_mask != 0U);

  ra8_spi((uint8_t)k_test_channel_0)->SPSR = 0U;
  s_spei_calls                             = 0U;
  ra8_spi_dispatch_spei((uint8_t)k_test_channel_0);
  TEST_ASSERT_EQ(0, s_spei_calls);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_spi_attach_transfer_handler((uint8_t)k_test_channel_0, nullptr, nullptr));
  ra8_spi((uint8_t)k_test_channel_0)->SPSR = k_ra8_spsr_mask_modf;
  s_spei_calls                             = 0U;
  ra8_spi_dispatch_spei((uint8_t)k_test_channel_0);
  TEST_ASSERT_EQ(0, s_spei_calls);

  ra8_spi_dispatch_spei((uint8_t)k_test_channel_oor);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_deinit((uint8_t)k_test_channel_0));
  TEST_END("spi_b MC/DC dispatch_spei: mask && cb");
}

int32_t main(void)
{
  ra8_fake_mmap_reset();
  test_mcdc_internal_spbr_zero_args();
  test_mcdc_internal_spcmd_cpha();
  test_mcdc_internal_spcmd_cpol();
  test_mcdc_xfer_common_both_null();
  test_mcdc_write_null_guard();
  test_mcdc_read_null_guard();
  test_mcdc_write_dma_arg_guard();
  test_mcdc_read_dma_arg_guard();
  test_mcdc_dispatch_spei_callback_guard();
  (void)fprintf(stderr, "[OK ] test_ra8_spi_b.c\n");
  return 0;
}
