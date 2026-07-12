/**
 * @file test_ra8_sci.c
 * @brief Unit tests for the SCI_B driver (libs/ra8_hal/src/ra8_sci.c).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_dma.h"
#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_sci.h"
#include "ra8_sci_regs.h"
#include "ra8_sim_dma.h"
#include "ra8_sim_mmap.h"
#include "ra8_sim_mmio.h"
#include "unity_minimal.h"

static int32_t s_rx_count   = 0;
static int32_t s_rx_last    = 0;
static int32_t s_tx_count   = 0;
static int32_t s_tx_seq_idx = 0;

static const uint8_t s_tx_payload[] = {0x10U, 0x20U, 0x30U};
static int32_t       s_tx_total     = (int32_t)(sizeof(s_tx_payload));

static void stub_rx(void* ctx, uint8_t byte)
{
  (void)ctx;
  ++s_rx_count;
  s_rx_last = (int32_t)byte;
}

static bool stub_tx(void* ctx, uint8_t* byte)
{
  (void)ctx;
  if (s_tx_seq_idx >= s_tx_total) {
    return false;
  }
  *byte = s_tx_payload[s_tx_seq_idx];
  ++s_tx_seq_idx;
  ++s_tx_count;
  return true;
}

static void prep(void)
{
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  (void)ra8_mstp_init();
  s_rx_count   = 0;
  s_rx_last    = 0;
  s_tx_count   = 0;
  s_tx_seq_idx = 0;
}

static const ra8_sci_cfg_t k_cfg = {
  .baud      = 115200U,
  .data_bits = k_ra8_sci_data_8,
  .parity    = k_ra8_sci_parity_none,
  .stop_bits = k_ra8_sci_stop_1,
  .pclk_hz   = 60000000U,
};

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_sets_ccr0_enables(void)
{
  TEST_BEGIN("ra8_sci_init: CCR0 enables TE + RE");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(0U, &k_cfg));
  volatile const r_sci_regs_t* reg  = ra8_sci(0U);
  const uint32_t               ccr0 = reg->CCR0;
  TEST_ASSERT((ccr0 & (1U << (uint8_t)k_ra8_sci_ccr0_bit_te)) != 0U);
  TEST_ASSERT((ccr0 & (1U << (uint8_t)k_ra8_sci_ccr0_bit_re)) != 0U);
  TEST_END("ra8_sci_init: CCR0 enables TE + RE");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_rejects_bad_channel(void)
{
  TEST_BEGIN("ra8_sci_init: bad channel rejected");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_init(99U, &k_cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sci_init(0U, nullptr));
  TEST_END("ra8_sci_init: bad channel rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_polling_tx_rx_round_trip(void)
{
  TEST_BEGIN("ra8_sci polling tx/rx round trip");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(1U, &k_cfg));

  /* Pre-seed CSR.TDRE so putc's spin completes on the first iteration. */
  volatile r_sci_regs_t* reg = ra8_sci(1U);
  reg->CSR                   = (1U << (uint8_t)k_ra8_sci_csr_bit_tdre);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_putc_polling(1U, 0x5AU));
  TEST_ASSERT_EQ(0x5A, (reg->TDR & 0xFFU));

  /* Poll RX: pre-seed CSR.RDRF and stage a byte in RDR. */
  reg->CSR    = reg->CSR | (1U << (uint8_t)k_ra8_sci_csr_bit_rdrf);
  reg->RDR    = 0xA5U;
  uint8_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_getc_polling(1U, &got));
  TEST_ASSERT_EQ(0xA5, got);
  TEST_END("ra8_sci polling tx/rx round trip");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_polling_tx_timeout(void)
{
  TEST_BEGIN("ra8_sci polling tx timeout");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(0U, &k_cfg));

  /* Force-clear CSR so TDRE never sets, and arm the MMIO seam to fail the
   * bounded wait -- the driver polls ra8_hw_wait_flag_set32(&reg->CSR, ...),
   * so key the fail on &ra8_sci(0)->CSR. Without arming, an unstaged flag now
   * satisfies the wait on the first poll (T1-01 seam contract). */
  ra8_sci(0U)->CSR = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_mmio_fail_wait((const volatile void*)&ra8_sci(0U)->CSR));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_sci_putc_polling(0U, 0x00U));
  TEST_END("ra8_sci polling tx timeout");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_polling_null_plus_nonzero(void)
{
  TEST_BEGIN("ra8_sci write polling null buffer rejected");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(0U, &k_cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sci_write_polling(0U, nullptr, 1U));
  /* Zero-length is always ok, even with NULL. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_write_polling(0U, nullptr, 0U));
  TEST_END("ra8_sci write polling null buffer rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_rx_sets_rie(void)
{
  TEST_BEGIN("ra8_sci_attach_rx_handler sets CCR0.RIE");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(2U, &k_cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_attach_rx_handler(2U, stub_rx, nullptr));
  volatile const r_sci_regs_t* reg = ra8_sci(2U);
  TEST_ASSERT((reg->CCR0 & (1U << (uint8_t)k_ra8_sci_ccr0_bit_rie)) != 0U);

  /* Detach clears the bit. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_attach_rx_handler(2U, nullptr, nullptr));
  TEST_ASSERT_EQ(0, (reg->CCR0 & (1U << (uint8_t)k_ra8_sci_ccr0_bit_rie)));
  TEST_END("ra8_sci_attach_rx_handler sets CCR0.RIE");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_tx_sets_tie(void)
{
  TEST_BEGIN("ra8_sci_attach_tx_handler sets CCR0.TIE");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(3U, &k_cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_attach_tx_handler(3U, stub_tx, nullptr));
  volatile const r_sci_regs_t* reg = ra8_sci(3U);
  TEST_ASSERT((reg->CCR0 & (1U << (uint8_t)k_ra8_sci_ccr0_bit_tie)) != 0U);

  /* Bad channel rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_attach_tx_handler(99U, stub_tx, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_attach_rx_handler(99U, stub_rx, nullptr));
  TEST_END("ra8_sci_attach_tx_handler sets CCR0.TIE");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_rxi_invokes_handler(void)
{
  TEST_BEGIN("ra8_sci_dispatch_rxi: calls handler with RDR byte");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(4U, &k_cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_attach_rx_handler(4U, stub_rx, nullptr));

  ra8_sci(4U)->RDR = 0x77U;
  ra8_sci_dispatch_rxi(4U);
  TEST_ASSERT_EQ(1, s_rx_count);
  TEST_ASSERT_EQ(0x77, s_rx_last);

  /* Bad channel = no-op. */
  ra8_sci_dispatch_rxi(99U);
  TEST_ASSERT_EQ(1, s_rx_count);
  TEST_END("ra8_sci_dispatch_rxi: calls handler with RDR byte");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_txi_drains_callback(void)
{
  TEST_BEGIN("ra8_sci_dispatch_txi: advances TX handler until end");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(5U, &k_cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_attach_tx_handler(5U, stub_tx, nullptr));

  volatile r_sci_regs_t* reg = ra8_sci(5U);
  for (int32_t i = 0; i < s_tx_total; ++i) {
    ra8_sci_dispatch_txi(5U);
  }
  /* After the final byte, the next dispatch should clear TIE. */
  ra8_sci_dispatch_txi(5U);
  TEST_ASSERT_EQ(0, (reg->CCR0 & (1U << (uint8_t)k_ra8_sci_ccr0_bit_tie)));
  TEST_ASSERT_EQ(s_tx_total, s_tx_count);

  /* Bad channel = no-op. */
  ra8_sci_dispatch_txi(99U);
  TEST_END("ra8_sci_dispatch_txi: advances TX handler until end");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_errors_mask_and_clear(void)
{
  TEST_BEGIN("ra8_sci_get_errors + clear_errors");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(6U, &k_cfg));

  volatile r_sci_regs_t* reg = ra8_sci(6U);
  reg->CSR = (1U << (uint8_t)k_ra8_sci_csr_bit_orer) | (1U << (uint8_t)k_ra8_sci_csr_bit_fer) |
             (1U << (uint8_t)k_ra8_sci_csr_bit_per);

  uint8_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_get_errors(6U, &mask));
  TEST_ASSERT_EQ(((uint8_t)k_ra8_sci_err_overrun | (uint8_t)k_ra8_sci_err_framing |
                  (uint8_t)k_ra8_sci_err_parity),
                 mask);

  /* The simulator backs MMIO with ordinary RAM, so write-1-to-clear
   * does not auto-clear the source flags; emulate hardware by
   * zeroing CSR after the clear-register write. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_clear_errors(6U));
  reg->CSR = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_get_errors(6U, &mask));
  TEST_ASSERT_EQ(k_ra8_sci_err_none, mask);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sci_get_errors(6U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_get_errors(99U, &mask));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_clear_errors(99U));
  TEST_END("ra8_sci_get_errors + clear_errors");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_baud_round_trip(void)
{
  TEST_BEGIN("ra8_sci_set_baud: BRR recomputed");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(7U, &k_cfg));

  const uint32_t ccr2_before = ra8_sci(7U)->CCR2;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_set_baud(7U, 9600U, 60000000U));
  const uint32_t ccr2_after = ra8_sci(7U)->CCR2;
  TEST_ASSERT(ccr2_after != ccr2_before);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_set_baud(7U, 0U, 60000000U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_set_baud(99U, 115200U, 60000000U));
  TEST_END("ra8_sci_set_baud: BRR recomputed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enter_exit_stop(void)
{
  TEST_BEGIN("ra8_sci_enter_stop / exit_stop");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(8U, &k_cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_enter_stop(8U));

  bool stopped = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_is_stopped(k_ra8_mstp_sci8, &stopped));
  TEST_ASSERT(stopped);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_exit_stop(8U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_is_stopped(k_ra8_mstp_sci8, &stopped));
  TEST_ASSERT(!stopped);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_enter_stop(99U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_exit_stop(99U));
  TEST_END("ra8_sci_enter_stop / exit_stop");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit_releases(void)
{
  TEST_BEGIN("ra8_sci_deinit releases MSTP");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(9U, &k_cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_deinit(9U));
  bool stopped = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_is_stopped(k_ra8_mstp_sci9, &stopped));
  TEST_ASSERT(stopped);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_deinit(99U));
  TEST_END("ra8_sci_deinit releases MSTP");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_cfg_variants(void)
{
  TEST_BEGIN("ra8_sci_init: all cfg variants reach BRR/CCR3 branches");
  prep();

  /* 2 stop bits + odd parity + 7-bit data -- exercises all CCR1/CCR3 branches. */
  const ra8_sci_cfg_t cfg_a = {
    .baud      = 9600U,
    .data_bits = k_ra8_sci_data_7,
    .parity    = k_ra8_sci_parity_odd,
    .stop_bits = k_ra8_sci_stop_2,
    .pclk_hz   = 60000000U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(0U, &cfg_a));

  /* Even parity path. */
  const ra8_sci_cfg_t cfg_b = {
    .baud      = 115200U,
    .data_bits = k_ra8_sci_data_8,
    .parity    = k_ra8_sci_parity_even,
    .stop_bits = k_ra8_sci_stop_1,
    .pclk_hz   = 60000000U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(1U, &cfg_b));

  /* zero baud -> BRR returns 0, but init() still returns ok because
   * it does not validate the resulting BRR. */
  const ra8_sci_cfg_t cfg_c = {
    .baud      = 0U,
    .data_bits = k_ra8_sci_data_8,
    .parity    = k_ra8_sci_parity_none,
    .stop_bits = k_ra8_sci_stop_1,
    .pclk_hz   = 60000000U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(2U, &cfg_c));

  /* Ridiculous baud so the divisor exceeds PCLK; n == 0 branch. */
  const ra8_sci_cfg_t cfg_d = {
    .baud      = 0xFFFFFFFFU,
    .data_bits = k_ra8_sci_data_8,
    .parity    = k_ra8_sci_parity_none,
    .stop_bits = k_ra8_sci_stop_1,
    .pclk_hz   = 60000000U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(3U, &cfg_d));
  TEST_END("ra8_sci_init: all cfg variants reach BRR/CCR3 branches");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_getc_polling_null(void)
{
  TEST_BEGIN("ra8_sci_getc_polling: null + bad channel");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(0U, &k_cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sci_getc_polling(0U, nullptr));

  uint8_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_getc_polling(99U, &got));

  /* Timeout path on RX. The getc wait polls ra8_hw_wait_flag_set32(&reg->CSR,
   * ...) for RDRF, so key the fail on &ra8_sci(0)->CSR (T1-01 seam contract). */
  ra8_sci(0U)->CSR = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_mmio_fail_wait((const volatile void*)&ra8_sci(0U)->CSR));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_sci_getc_polling(0U, &got));
  TEST_END("ra8_sci_getc_polling: null + bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_putc_polling_bad_channel(void)
{
  TEST_BEGIN("ra8_sci_putc_polling: bad channel rejected");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sci_putc_polling(99U, 0U));
  TEST_END("ra8_sci_putc_polling: bad channel rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_txi_with_no_handler(void)
{
  TEST_BEGIN("ra8_sci_dispatch_txi: no-handler clears TIE");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(0U, &k_cfg));
  /* Manually set TIE. */
  volatile r_sci_regs_t* reg = ra8_sci(0U);
  reg->CCR0                  = reg->CCR0 | (1U << (uint8_t)k_ra8_sci_ccr0_bit_tie);
  /* Dispatch with no handler attached -- should clear TIE. */
  ra8_sci_dispatch_txi(0U);
  TEST_ASSERT_EQ(0, (reg->CCR0 & (1U << (uint8_t)k_ra8_sci_ccr0_bit_tie)));
  TEST_END("ra8_sci_dispatch_txi: no-handler clears TIE");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_rxi_with_no_handler(void)
{
  TEST_BEGIN("ra8_sci_dispatch_rxi: no-handler is no-op");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(0U, &k_cfg));
  /* Dispatch without attaching a handler -- just reads RDR. */
  ra8_sci_dispatch_rxi(0U);
  /* No assertion other than "did not crash"; covered the no-cb branch. */
  TEST_END("ra8_sci_dispatch_rxi: no-handler is no-op");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_eri_dispatch_clears_errors(void)
{
  TEST_BEGIN("ra8_sci_dispatch_eri clears CSR flags");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_init(0U, &k_cfg));
  volatile r_sci_regs_t* reg = ra8_sci(0U);
  reg->CSR                   = reg->CSR | (1U << (uint8_t)k_ra8_sci_csr_bit_orer);

  ra8_sci_dispatch_eri(0U);
  /* The simulator can't auto-clear write-1-to-clear bits, so emulate
   * the hardware effect manually before re-reading the mask. */
  reg->CSR     = 0U;
  uint8_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sci_get_errors(0U, &mask));
  TEST_ASSERT_EQ(k_ra8_sci_err_none, mask);

  /* Out-of-range is a no-op. */
  ra8_sci_dispatch_eri(99U);
  TEST_END("ra8_sci_dispatch_eri clears CSR flags");
}

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
  uint8_t       dma_ch = 0xFFU;
  s_dma_complete_count = 0;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_sci_write_dma(0U, src, (uint16_t)sizeof(src), stub_dma_done, nullptr, &dma_ch));
  TEST_ASSERT(dma_ch < 8U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_dma_memcpy(dma_ch));
  volatile const r_sci_regs_t* reg = ra8_sci(0U);
  /* Last byte streamed lands in TDR (dst_inc=false). DMA writes are
   * byte-wide, so only the low 8 bits of TDR are loaded. */
  TEST_ASSERT_EQ(0xDDU, (reg->TDR & 0xFFU));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_dma_complete(dma_ch));
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
  reg->RDR                   = 0x42U;

  uint8_t out[3]       = {0U, 0U, 0U};
  uint8_t dma_ch       = 0xFFU;
  s_dma_complete_count = 0;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sci_read_dma(0U, out, (uint16_t)sizeof(out), stub_dma_done, nullptr, &dma_ch));
  TEST_ASSERT(dma_ch < 8U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_dma_memcpy(dma_ch));
  /* RDR (src_inc=false) was 0x42 throughout, so all destinations match. */
  TEST_ASSERT_EQ(0x42U, out[0]);
  TEST_ASSERT_EQ(0x42U, out[1]);
  TEST_ASSERT_EQ(0x42U, out[2]);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_dma_complete(dma_ch));
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
    payload[i] = (uint8_t)(0x40U + i);
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
    reg->RDR = (uint32_t)(0x80U + i);
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
  uint32_t       err_x_100 = (effective > 115200U) ? (effective - 115200U) : (115200U - effective);
  err_x_100                = (err_x_100 * 100U) / 115200U;
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
  k_mcdc_sci_ch_ok     = 0U,  /**< In-range SCI channel index.                    */
  k_mcdc_sci_ch_bad    = 99U, /**< Out-of-range channel -> internal_reg() = NULL. */
  k_mcdc_sci_baud_zero = 0U,
  k_mcdc_sci_baud_ok   = 115200U,
  k_mcdc_sci_pclk_zero = 0U,
  k_mcdc_sci_pclk_ok   = 60000000U,
  k_mcdc_sci_len_zero  = 0U,
  k_mcdc_sci_len_one   = 1U,
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
  uint8_t       dma_ch = 0xFFU;

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
  uint8_t dma_ch = 0xFFU;

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
  test_init_sets_ccr0_enables();
  test_init_rejects_bad_channel();
  test_polling_tx_rx_round_trip();
  test_polling_tx_timeout();
  test_write_polling_null_plus_nonzero();
  test_attach_rx_sets_rie();
  test_attach_tx_sets_tie();
  test_dispatch_rxi_invokes_handler();
  test_dispatch_txi_drains_callback();
  test_errors_mask_and_clear();
  test_set_baud_round_trip();
  test_enter_exit_stop();
  test_deinit_releases();
  test_cfg_variants();
  test_getc_polling_null();
  test_putc_polling_bad_channel();
  test_dispatch_txi_with_no_handler();
  test_dispatch_rxi_with_no_handler();
  test_eri_dispatch_clears_errors();
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
  (void)fprintf(stderr, "[OK  ] test_ra8_sci.c\n");
  return 0;
}
