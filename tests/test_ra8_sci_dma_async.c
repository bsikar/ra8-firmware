/**
 * @file test_ra8_sci_dma_async.c
 * @brief DMA, async, and MC/DC tests for the SCI_B driver (ra8_sci.c).
 *
 * @details
 * Split out of test_ra8_sci.c to keep each test translation unit under the
 * repository file-size cap. This sibling owns the DMA streaming tests, the
 * interrupt-driven async read/write tests, and the MC/DC vector tests for
 * the compound boolean decisions in ra8_sci.c; the polling / IRQ-dispatch /
 * configuration contract tests stay in test_ra8_sci.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_dma.h"
#include "ra8_err.h"
#include "ra8_fake_dma.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_mstp.h"
#include "ra8_sci.h"
#include "ra8_sci_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_dma_probe_t
 * @brief Channel poison and payload patterns for the async SCI arms.
 */
typedef enum : uint8_t {
  k_t_dma_ch_unset = 0xFFU, /**< Pre-set DMA channel; a call that returns
                                 without allocating one leaves this value.      */
  k_t_rdr_byte     = 0x42U, /**< Byte staged in RDR for the single-read arm.   */
  k_t_tx_base      = 0x40U, /**< First byte of the ascending transmit payload. */
  k_t_rx_base      = 0x80U, /**< First byte of the ascending receive payload;
                                 disjoint from the transmit range so a loopback
                                 that echoes the wrong buffer is visible.       */
} t_dma_probe_t;

/**
 * @enum t_baud_t
 * @brief Baud-rate error budget for the divisor-selection arm.
 */
typedef enum : uint32_t {
  k_t_baud_nominal = 115200U, /**< Requested baud rate. */
  k_t_pct_scale    = 100U,    /**< Scales the deviation to hundredths of a
                                   percent before the integer comparison.       */
} t_baud_t;

static void prep(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  (void)ra8_mstp_init();
}

static const ra8_sci_cfg_t k_cfg = {
  .baud      = 115200U,
  .data_bits = k_ra8_sci_data_8,
  .parity    = k_ra8_sci_parity_none,
  .stop_bits = k_ra8_sci_stop_1,
  .pclk_hz   = 60000000U,
};

static int32_t s_dma_complete_count = 0;

static void stub_dma_done(void* ctx)
{
  (void)ctx;
  ++s_dma_complete_count;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_dma_streams_buffer_to_tdr(void)
{
  TEST_BEGIN("ra8_sci_write_dma: buffer streams into TDR via DMA");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(0U, &k_cfg));

  const uint8_t src[]  = {0xAAU, 0xBBU, 0xCCU, 0xDDU};
  uint8_t       dma_ch = k_t_dma_ch_unset;
  s_dma_complete_count = 0;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_sci_write_dma(0U, src, (uint16_t)sizeof(src), stub_dma_done, nullptr, &dma_ch));
  TEST_ASSERT(dma_ch < 8U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_dma_memcpy(dma_ch));
  volatile const r_sci_regs_t* reg = ra8_sci(0U);
  /* Last byte streamed lands in TDR (dst_inc=false). DMA writes are
   * byte-wide, so only the low 8 bits of TDR are loaded. */
  TEST_ASSERT_EQ(0xDDU, (reg->TDR & 0xFFU));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_dma_complete(dma_ch));
  TEST_ASSERT_EQ(1, s_dma_complete_count);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_release(dma_ch));
  TEST_END("ra8_sci_write_dma: buffer streams into TDR via DMA");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_dma_streams_rdr_to_buffer(void)
{
  TEST_BEGIN("ra8_sci_read_dma: RDR streams into buffer via DMA");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(0U, &k_cfg));

  volatile r_sci_regs_t* reg = ra8_sci(0U);
  reg->RDR                   = k_t_rdr_byte;

  uint8_t out[3]       = {0U, 0U, 0U};
  uint8_t dma_ch       = k_t_dma_ch_unset;
  s_dma_complete_count = 0;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_read_dma(0U, out, (uint16_t)sizeof(out), stub_dma_done, nullptr, &dma_ch));
  TEST_ASSERT(dma_ch < 8U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_dma_memcpy(dma_ch));
  /* RDR (src_inc=false) was 0x42 throughout, so all destinations match. */
  TEST_ASSERT_EQ(0x42U, out[0]);
  TEST_ASSERT_EQ(0x42U, out[1]);
  TEST_ASSERT_EQ(0x42U, out[2]);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_dma_complete(dma_ch));
  TEST_ASSERT_EQ(1, s_dma_complete_count);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_release(dma_ch));
  TEST_END("ra8_sci_read_dma: RDR streams into buffer via DMA");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dma_arg_validation(void)
{
  TEST_BEGIN("ra8_sci_{write,read}_dma: arg validation");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(0U, &k_cfg));

  uint8_t       dma_ch = 0U;
  const uint8_t src[]  = {0x01U};
  uint8_t       dst[1] = {0U};

  /* NULL data / out_buf / out_dma_channel. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sci_write_dma(0U, nullptr, 1U, nullptr, nullptr, &dma_ch));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sci_write_dma(0U, src, 1U, nullptr, nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sci_read_dma(0U, nullptr, 1U, nullptr, nullptr, &dma_ch));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sci_read_dma(0U, dst, 1U, nullptr, nullptr, nullptr));

  /* Bad channel. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_write_dma(99U, src, 1U, nullptr, nullptr, &dma_ch));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_read_dma(99U, dst, 1U, nullptr, nullptr, &dma_ch));

  /* Zero-length transfer. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_write_dma(0U, src, 0U, nullptr, nullptr, &dma_ch));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_read_dma(0U, dst, 0U, nullptr, nullptr, &dma_ch));

  TEST_END("ra8_sci_{write,read}_dma: arg validation");
}

/* =====================================================================
 * Async byte-stream TX / RX (FSP r_sci_b_uart parity)
 * ===================================================================== */

enum : uint32_t {
  k_test_async_len = 16U, /**< Async TX/RX exercise length. */
};

static int32_t s_async_tx_cb_count = 0;

/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTNEXTLINE(readability-non-const-parameter)
static bool stub_async_tx_visibility(void* ctx, uint8_t* byte)
{
  (void)ctx;
  (void)byte;
  ++s_async_tx_cb_count;
  return true;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_async_write_drains_buffer(void)
{
  TEST_BEGIN("ra8_sci_write: 16-byte async TX drains via TXI");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(0U, &k_cfg));

  s_async_tx_cb_count = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_attach_tx_handler(0U, stub_async_tx_visibility, nullptr));

  uint8_t payload[k_test_async_len];
  for (uint32_t i = 0U; i < k_test_async_len; ++i) {
    payload[i] = (uint8_t)(k_t_tx_base + i);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_write(0U, payload, k_test_async_len));

  volatile r_sci_regs_t* reg = ra8_sci(0U);
  TEST_ASSERT((reg->CCR0 & (1U << (uint8_t)k_ra8_sci_ccr0_bit_tie)) != 0U);

  for (uint32_t i = 0U; i < k_test_async_len; ++i) {
    ra8_sci_dispatch_txi(0U);
    TEST_ASSERT_EQ(payload[i], (reg->TDR & 0xFFU));
  }
  /* All bytes pushed -- TIE auto-cleared. */
  TEST_ASSERT_EQ(0, (reg->CCR0 & (1U << (uint8_t)k_ra8_sci_ccr0_bit_tie)));
  TEST_ASSERT_EQ(k_test_async_len, s_async_tx_cb_count);

  /* Re-arming the same channel works again now that TX state is idle. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_write(0U, payload, k_test_async_len));
  TEST_END("ra8_sci_write: 16-byte async TX drains via TXI");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_async_read_fills_buffer(void)
{
  TEST_BEGIN("ra8_sci_read: 16-byte async RX captures via RXI");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(1U, &k_cfg));

  uint8_t buf[k_test_async_len];
  for (uint32_t i = 0U; i < k_test_async_len; ++i) {
    buf[i] = 0U;
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_read(1U, buf, k_test_async_len));

  volatile r_sci_regs_t* reg = ra8_sci(1U);
  TEST_ASSERT((reg->CCR0 & (1U << (uint8_t)k_ra8_sci_ccr0_bit_rie)) != 0U);

  /* Simulate 16 RX-not-empty interrupts, each pre-loading RDR. */
  for (uint32_t i = 0U; i < k_test_async_len; ++i) {
    reg->RDR = (uint32_t)(k_t_rx_base + i);
    ra8_sci_dispatch_rxi(1U);
  }
  /* All bytes captured -- RIE auto-cleared. */
  TEST_ASSERT_EQ(0, (reg->CCR0 & (1U << (uint8_t)k_ra8_sci_ccr0_bit_rie)));
  for (uint32_t i = 0U; i < k_test_async_len; ++i) {
    TEST_ASSERT_EQ((0x80U + i), buf[i]);
  }
  TEST_END("ra8_sci_read: 16-byte async RX captures via RXI");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_async_abort_tx(void)
{
  TEST_BEGIN("ra8_sci_abort: TX direction disarms TIE mid-stream");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(2U, &k_cfg));

  uint8_t payload[k_test_async_len];
  for (uint32_t i = 0U; i < k_test_async_len; ++i) {
    payload[i] = (uint8_t)i;
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_write(2U, payload, k_test_async_len));

  /* Fire 8 of 16 dispatches, then abort. */
  for (uint32_t i = 0U; i < 8U; ++i) {
    ra8_sci_dispatch_txi(2U);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_abort(2U, k_ra8_sci_dir_tx));

  volatile const r_sci_regs_t* reg = ra8_sci(2U);
  TEST_ASSERT_EQ(0, (reg->CCR0 & (1U << (uint8_t)k_ra8_sci_ccr0_bit_tie)));

  /* After abort the channel is idle again and a new write can start. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_write(2U, payload, k_test_async_len));
  TEST_END("ra8_sci_abort: TX direction disarms TIE mid-stream");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_async_read_stop_reports_remaining(void)
{
  TEST_BEGIN("ra8_sci_read_stop: returns bytes still pending");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(3U, &k_cfg));

  uint8_t buf[k_test_async_len];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_read(3U, buf, k_test_async_len));

  volatile r_sci_regs_t* reg = ra8_sci(3U);
  for (uint32_t i = 0U; i < 4U; ++i) {
    reg->RDR = (uint32_t)i;
    ra8_sci_dispatch_rxi(3U);
  }

  uint32_t remaining = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_read_stop(3U, &remaining));
  TEST_ASSERT_EQ((k_test_async_len - 4U), remaining);
  TEST_ASSERT_EQ(0, (reg->CCR0 & (1U << (uint8_t)k_ra8_sci_ccr0_bit_rie)));

  /* read_stop with no active RX reports zero, not an error. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_read_stop(3U, &remaining));
  TEST_ASSERT_EQ(0, remaining);
  TEST_END("ra8_sci_read_stop: returns bytes still pending");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_baud_calculate_115200_at_60mhz(void)
{
  TEST_BEGIN("ra8_sci_baud_calculate: 115200 @ 60 MHz within 2 percent");
  prep();

  uint16_t brr     = 0U;
  uint8_t  clk_div = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_baud_calculate(115200U, 60000000U, &brr, &clk_div));
  /* HUM Ch 38.2.7: with n = 0 (CKS = 0), divisor = 32, so
   *   BRR = 60_000_000 / (32 * 115200) - 1 = 16 - 1 = 15.
   * Effective baud = 60_000_000 / (32 * (15+1)) = 117187.5 bps. */
  TEST_ASSERT_EQ(15U, brr);
  TEST_ASSERT_EQ(0U, clk_div);

  /* Verify the baud-rate error is < 2 percent. */
  const uint32_t divisor   = 32U * (1U << (2U * (uint32_t)clk_div));
  const uint32_t effective = 60000000U / (divisor * ((uint32_t)brr + 1U));
  uint32_t       err_x_100 = (effective > k_t_baud_nominal) ? (effective - k_t_baud_nominal)
                                                            : (k_t_baud_nominal - effective);
  err_x_100                = (err_x_100 * k_t_pct_scale) / k_t_baud_nominal;
  TEST_ASSERT(err_x_100 < 2U);

  /* Slow baud forces a higher CKS divider. 1200 baud at 60 MHz needs
   * BRR ~= 1561 with n = 0 (overflows) so the loop must climb to a
   * larger divisor. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_baud_calculate(1200U, 60000000U, &brr, &clk_div));
  TEST_ASSERT(clk_div > 0U);

  /* Unreachable baud rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_baud_calculate(0xFFFFFFFFU, 1000U, &brr, &clk_div));

  /* NULL-arg rejection. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sci_baud_calculate(115200U, 60000000U, nullptr, &clk_div));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sci_baud_calculate(115200U, 60000000U, &brr, nullptr));

  /* Zero arguments. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_baud_calculate(0U, 60000000U, &brr, &clk_div));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_baud_calculate(115200U, 0U, &brr, &clk_div));
  TEST_END("ra8_sci_baud_calculate: 115200 @ 60 MHz within 2 percent");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_receive_suspend_resume(void)
{
  TEST_BEGIN("ra8_sci_receive_suspend / resume toggles CCR0.RE");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(4U, &k_cfg));

  volatile r_sci_regs_t* reg = ra8_sci(4U);
  TEST_ASSERT((reg->CCR0 & (1U << (uint8_t)k_ra8_sci_ccr0_bit_re)) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_receive_suspend(4U));
  TEST_ASSERT_EQ(0, (reg->CCR0 & (1U << (uint8_t)k_ra8_sci_ccr0_bit_re)));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_receive_resume(4U));
  TEST_ASSERT((reg->CCR0 & (1U << (uint8_t)k_ra8_sci_ccr0_bit_re)) != 0U);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_receive_suspend(99U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_receive_resume(99U));
  TEST_END("ra8_sci_receive_suspend / resume toggles CCR0.RE");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_async_null_arg_rejection(void)
{
  TEST_BEGIN("ra8_sci async API: NULL + bad-channel rejection");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(5U, &k_cfg));

  uint8_t buf[1] = {0U};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sci_write(5U, nullptr, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sci_read(5U, nullptr, 1U));

  /* Zero-length is always ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_write(5U, nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_read(5U, nullptr, 0U));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_write(99U, buf, 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_read(99U, buf, 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_abort(99U, k_ra8_sci_dir_both));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_abort(5U, (ra8_sci_dir_t)0xFFU));

  uint32_t remaining = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sci_read_stop(5U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_read_stop(99U, &remaining));

  /* Busy rejection: re-arming a TX while one is in flight returns busy. */
  uint8_t pl[k_test_async_len];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_write(5U, pl, k_test_async_len));
  TEST_ASSERT_EQ(k_ra8_err_busy, ra8_sci_write(5U, pl, k_test_async_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_abort(5U, k_ra8_sci_dir_tx));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_read(5U, buf, 1U));
  TEST_ASSERT_EQ(k_ra8_err_busy, ra8_sci_read(5U, buf, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_abort(5U, k_ra8_sci_dir_rx));

  /* Calling write/read on an uninitialized channel is rejected. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_deinit(5U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_write(5U, pl, 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_read(5U, buf, 1U));
  TEST_END("ra8_sci async API: NULL + bad-channel rejection");
}

/* =====================================================================
 * MC/DC vector tests (DO-178C Level B / IEC 61508 SIL 3)
 *
 * Each test pins all conditions in a compound boolean decision except
 * one and shows that flipping the varied condition flips the outcome.
 * With N conditions the minimal set is N+1 vectors.
 * Source-of-truth gap rows: docs/MCDC_GAPS.csv (ra8_sci.c entries).
 * ===================================================================== */

typedef enum : uint32_t {
  k_mcdc_sci_ch_ok     = 0U,        /**< In-range SCI channel index.                    */
  k_mcdc_sci_ch_bad    = 99U,       /**< Out-of-range channel -> internal_reg() = NULL. */
  k_mcdc_sci_baud_zero = 0U,        /**< Mcdc SCI baud zero.                            */
  k_mcdc_sci_baud_ok   = 115200U,   /**< Mcdc SCI baud ok.                              */
  k_mcdc_sci_pclk_zero = 0U,        /**< Mcdc SCI pclk zero.                            */
  k_mcdc_sci_pclk_ok   = 60000000U, /**< Mcdc SCI pclk ok.                              */
  k_mcdc_sci_len_zero  = 0U,        /**< Mcdc SCI length zero.                          */
  k_mcdc_sci_len_one   = 1U,        /**< Mcdc SCI length one.                           */
} mcdc_sci_const_t;

/**
 * @test test_mcdc_internal_brr
 *
 * @par MC/DC:
 * Decision: `if ((baud == 0U) || (pclk_hz == 0U))`
 * (libs/ra8_hal/src/ra8_sci.c, internal_brr -- reachable via
 * ra8_sci_init since init does not pre-validate baud/pclk).
 * - V1: baud=115200, pclk=60e6   -> false (BRR computed normally; init returns ok).
 * - V2: baud=0,      pclk=60e6   -> true  (varies C1; internal_brr returns 0; init still ok).
 * - V3: baud=115200, pclk=0      -> true  (varies C2; internal_brr returns 0; init still ok).
 * Note: internal_brr's outcome is observable through the BRR field of
 * CCR2 -- for the false branch BRR > 0, for either true branch BRR=0.
 * N+1 = 3 vectors for N=2.
 */
static void test_mcdc_internal_brr(void)
{
  TEST_BEGIN("mcdc: internal_brr (baud|pclk == 0) decision");
  prep();

  /* V1: both non-zero -> success path. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init((uint8_t)k_mcdc_sci_ch_ok, &k_cfg));
  const uint32_t ccr2_v1 = ra8_sci((uint8_t)k_mcdc_sci_ch_ok)->CCR2;
  TEST_ASSERT(ccr2_v1 != 0U);

  /* V2: baud == 0. */
  prep();
  const ra8_sci_cfg_t cfg_v2 = {
    .baud      = (uint32_t)k_mcdc_sci_baud_zero,
    .data_bits = k_ra8_sci_data_8,
    .parity    = k_ra8_sci_parity_none,
    .stop_bits = k_ra8_sci_stop_1,
    .pclk_hz   = (uint32_t)k_mcdc_sci_pclk_ok,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init((uint8_t)k_mcdc_sci_ch_ok, &cfg_v2));

  /* V3: pclk_hz == 0. */
  prep();
  const ra8_sci_cfg_t cfg_v3 = {
    .baud      = (uint32_t)k_mcdc_sci_baud_ok,
    .data_bits = k_ra8_sci_data_8,
    .parity    = k_ra8_sci_parity_none,
    .stop_bits = k_ra8_sci_stop_1,
    .pclk_hz   = (uint32_t)k_mcdc_sci_pclk_zero,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init((uint8_t)k_mcdc_sci_ch_ok, &cfg_v3));

  TEST_END("mcdc: internal_brr (baud|pclk == 0) decision");
}

/**
 * @test test_mcdc_write_polling_data_len
 *
 * @par MC/DC:
 * Decision: `if ((data == nullptr) && (len != 0U))`
 * (libs/ra8_hal/src/ra8_sci.c, ra8_sci_write_polling).
 * - V1: data=valid, len=1   -> C1=F, short-circuit  -> false (control).
 * - V2: data=NULL,  len=0   -> C1=T, C2=F           -> false (varies C2).
 * - V3: data=NULL,  len=1   -> C1=T, C2=T           -> true  (rejected).
 * V1+V3 prove C1; V2+V3 prove C2 (with C1 held T). N+1 = 3 vectors for N=2.
 */
static void test_mcdc_write_polling_data_len(void)
{
  TEST_BEGIN("mcdc: write_polling (data&&len) decision");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init((uint8_t)k_mcdc_sci_ch_ok, &k_cfg));

  const uint8_t buf[1] = {0xA5U};
  /* V1: pre-seed TDRE so the polling tx loop completes. */
  ra8_sci((uint8_t)k_mcdc_sci_ch_ok)->CSR = (1U << (uint8_t)k_ra8_sci_csr_bit_tdre);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_sci_write_polling((uint8_t)k_mcdc_sci_ch_ok, buf, (uint32_t)k_mcdc_sci_len_one));
  /* V2: zero-length is always ok even with NULL data. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_sci_write_polling((uint8_t)k_mcdc_sci_ch_ok, nullptr, (uint32_t)k_mcdc_sci_len_zero));
  /* V3: NULL data with non-zero len -> rejected. */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_sci_write_polling((uint8_t)k_mcdc_sci_ch_ok, nullptr, (uint32_t)k_mcdc_sci_len_one));
  TEST_END("mcdc: write_polling (data&&len) decision");
}

/**
 * @test test_mcdc_baud_calculate_args
 *
 * @par MC/DC:
 * Decision: `if ((baud == 0U) || (pclk_hz == 0U))`
 * (libs/ra8_hal/src/ra8_sci.c, ra8_sci_baud_calculate).
 * - V1: baud=115200, pclk=60e6  -> false (success).
 * - V2: baud=0,      pclk=60e6  -> true  (varies C1).
 * - V3: baud=115200, pclk=0     -> true  (varies C2).
 * N+1 = 3 vectors for N=2.
 */
static void test_mcdc_baud_calculate_args(void)
{
  TEST_BEGIN("mcdc: baud_calculate (baud|pclk == 0) decision");
  prep();

  uint16_t brr     = 0U;
  uint8_t  clk_div = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_baud_calculate((uint32_t)k_mcdc_sci_baud_ok,
                                        (uint32_t)k_mcdc_sci_pclk_ok,
                                        &brr,
                                        &clk_div));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sci_baud_calculate((uint32_t)k_mcdc_sci_baud_zero,
                                        (uint32_t)k_mcdc_sci_pclk_ok,
                                        &brr,
                                        &clk_div));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sci_baud_calculate((uint32_t)k_mcdc_sci_baud_ok,
                                        (uint32_t)k_mcdc_sci_pclk_zero,
                                        &brr,
                                        &clk_div));
  TEST_END("mcdc: baud_calculate (baud|pclk == 0) decision");
}

/**
 * @test test_mcdc_async_write_data_len
 *
 * @par MC/DC:
 * Decision: `if ((data == nullptr) && (len != 0U))`
 * (libs/ra8_hal/src/ra8_sci.c, ra8_sci_write).
 * - V1: data=buf, len=1    -> false (control).
 * - V2: data=NULL,len=0    -> false (varies C2).
 * - V3: data=NULL,len=1    -> true  (rejected).
 * N+1 = 3 vectors for N=2.
 */
static void test_mcdc_async_write_data_len(void)
{
  TEST_BEGIN("mcdc: ra8_sci_write (data&&len) decision");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init((uint8_t)k_mcdc_sci_ch_ok, &k_cfg));

  const uint8_t buf[1] = {0x5AU};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_write((uint8_t)k_mcdc_sci_ch_ok, buf, (uint32_t)k_mcdc_sci_len_one));
  /* Abort to release the TX state for the next call. */
  (void)ra8_sci_abort((uint8_t)k_mcdc_sci_ch_ok, k_ra8_sci_dir_tx);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_write((uint8_t)k_mcdc_sci_ch_ok, nullptr, (uint32_t)k_mcdc_sci_len_zero));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_sci_write((uint8_t)k_mcdc_sci_ch_ok, nullptr, (uint32_t)k_mcdc_sci_len_one));
  TEST_END("mcdc: ra8_sci_write (data&&len) decision");
}

/**
 * @test test_mcdc_async_read_buf_len
 *
 * @par MC/DC:
 * Decision: `if ((buf == nullptr) && (len != 0U))`
 * (libs/ra8_hal/src/ra8_sci.c, ra8_sci_read).
 * - V1: buf=valid, len=1   -> false (control).
 * - V2: buf=NULL,  len=0   -> false (varies C2).
 * - V3: buf=NULL,  len=1   -> true  (rejected).
 * N+1 = 3 vectors for N=2.
 */
static void test_mcdc_async_read_buf_len(void)
{
  TEST_BEGIN("mcdc: ra8_sci_read (buf&&len) decision");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init((uint8_t)k_mcdc_sci_ch_ok, &k_cfg));

  uint8_t buf[1] = {0U};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_read((uint8_t)k_mcdc_sci_ch_ok, buf, (uint32_t)k_mcdc_sci_len_one));
  (void)ra8_sci_abort((uint8_t)k_mcdc_sci_ch_ok, k_ra8_sci_dir_rx);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_read((uint8_t)k_mcdc_sci_ch_ok, nullptr, (uint32_t)k_mcdc_sci_len_zero));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_sci_read((uint8_t)k_mcdc_sci_ch_ok, nullptr, (uint32_t)k_mcdc_sci_len_one));
  TEST_END("mcdc: ra8_sci_read (buf&&len) decision");
}

/**
 * @test test_mcdc_abort_direction
 *
 * @par MC/DC:
 * Decision: `if ((direction != k_ra8_sci_dir_tx) &&
 *                (direction != k_ra8_sci_dir_rx) &&
 *                (direction != k_ra8_sci_dir_both))`
 * (libs/ra8_hal/src/ra8_sci.c, ra8_sci_abort, 3 conditions).
 * The decision is true (rejected) only when direction matches none of
 * the three enum values. AND chain short-circuits left-to-right.
 * Naming: C1=(dir!=TX), C2=(dir!=RX), C3=(dir!=BOTH).
 * - V1: dir=TX   -> C1=F, short-circuits          -> false (control: ok).
 * - V2: dir=RX   -> C1=T, C2=F                    -> false (varies C2).
 * - V3: dir=BOTH -> C1=T, C2=T, C3=F              -> false (varies C3).
 * - V4: dir=0xFF -> C1=T, C2=T, C3=T              -> true  (rejected).
 * V1+V4 prove C1; V2+V4 prove C2; V3+V4 prove C3. N+1 = 4 vectors for N=3.
 */
static void test_mcdc_abort_direction(void)
{
  TEST_BEGIN("mcdc: ra8_sci_abort 3-condition direction decision");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init((uint8_t)k_mcdc_sci_ch_ok, &k_cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_abort((uint8_t)k_mcdc_sci_ch_ok, k_ra8_sci_dir_tx));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_abort((uint8_t)k_mcdc_sci_ch_ok, k_ra8_sci_dir_rx));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_abort((uint8_t)k_mcdc_sci_ch_ok, k_ra8_sci_dir_both));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sci_abort((uint8_t)k_mcdc_sci_ch_ok, (ra8_sci_dir_t)0xFFU));
  TEST_END("mcdc: ra8_sci_abort 3-condition direction decision");
}

/**
 * @test test_mcdc_write_dma_reg_len
 *
 * @par MC/DC:
 * Decision: `if ((reg == nullptr) || (len == 0U))`
 * (libs/ra8_hal/src/ra8_sci.c, ra8_sci_write_dma).
 * `reg` is the result of internal_reg(channel) which returns NULL for
 * out-of-range channels.
 * - V1: ch=valid (reg!=NULL), len=1   -> false (control: ok path).
 * - V2: ch=99    (reg==NULL), len=1   -> true  (varies C1).
 * - V3: ch=valid (reg!=NULL), len=0   -> true  (varies C2).
 * N+1 = 3 vectors for N=2.
 */
static void test_mcdc_write_dma_reg_len(void)
{
  TEST_BEGIN("mcdc: write_dma (reg||len==0) decision");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init((uint8_t)k_mcdc_sci_ch_ok, &k_cfg));

  const uint8_t src[1] = {0x77U};
  uint8_t       dma_ch = k_t_dma_ch_unset;

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_write_dma((uint8_t)k_mcdc_sci_ch_ok,
                                   src,
                                   (uint16_t)k_mcdc_sci_len_one,
                                   nullptr,
                                   nullptr,
                                   &dma_ch));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sci_write_dma((uint8_t)k_mcdc_sci_ch_bad,
                                   src,
                                   (uint16_t)k_mcdc_sci_len_one,
                                   nullptr,
                                   nullptr,
                                   &dma_ch));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sci_write_dma((uint8_t)k_mcdc_sci_ch_ok,
                                   src,
                                   (uint16_t)k_mcdc_sci_len_zero,
                                   nullptr,
                                   nullptr,
                                   &dma_ch));
  TEST_END("mcdc: write_dma (reg||len==0) decision");
}

/**
 * @test test_mcdc_read_dma_reg_len
 *
 * @par MC/DC:
 * Decision: `if ((reg == nullptr) || (len == 0U))`
 * (libs/ra8_hal/src/ra8_sci.c, ra8_sci_read_dma).
 * - V1: ch=valid, len=1   -> false (control).
 * - V2: ch=99,    len=1   -> true  (varies C1).
 * - V3: ch=valid, len=0   -> true  (varies C2).
 * N+1 = 3 vectors for N=2.
 */
static void test_mcdc_read_dma_reg_len(void)
{
  TEST_BEGIN("mcdc: read_dma (reg||len==0) decision");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dma_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init((uint8_t)k_mcdc_sci_ch_ok, &k_cfg));

  uint8_t dst[1] = {0U};
  uint8_t dma_ch = k_t_dma_ch_unset;

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_read_dma((uint8_t)k_mcdc_sci_ch_ok,
                                  dst,
                                  (uint16_t)k_mcdc_sci_len_one,
                                  nullptr,
                                  nullptr,
                                  &dma_ch));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sci_read_dma((uint8_t)k_mcdc_sci_ch_bad,
                                  dst,
                                  (uint16_t)k_mcdc_sci_len_one,
                                  nullptr,
                                  nullptr,
                                  &dma_ch));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sci_read_dma((uint8_t)k_mcdc_sci_ch_ok,
                                  dst,
                                  (uint16_t)k_mcdc_sci_len_zero,
                                  nullptr,
                                  nullptr,
                                  &dma_ch));
  TEST_END("mcdc: read_dma (reg||len==0) decision");
}

int32_t main(void)
{
  test_write_dma_streams_buffer_to_tdr();
  test_read_dma_streams_rdr_to_buffer();
  test_dma_arg_validation();
  test_async_write_drains_buffer();
  test_async_read_fills_buffer();
  test_async_abort_tx();
  test_async_read_stop_reports_remaining();
  test_baud_calculate_115200_at_60mhz();
  test_receive_suspend_resume();
  test_async_null_arg_rejection();
  test_mcdc_internal_brr();
  test_mcdc_write_polling_data_len();
  test_mcdc_baud_calculate_args();
  test_mcdc_async_write_data_len();
  test_mcdc_async_read_buf_len();
  test_mcdc_abort_direction();
  test_mcdc_write_dma_reg_len();
  test_mcdc_read_dma_reg_len();
  return 0;
}
