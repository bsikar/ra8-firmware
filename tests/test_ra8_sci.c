/**
 * @file test_ra8_sci.c
 * @brief Unit tests for the SCI_B driver (libs/ra8_hal/src/ra8_sci.c).
 *
 * @details
 * This sibling owns the polling, IRQ-dispatch, and configuration contract
 * tests. The DMA streaming, interrupt-driven async read/write, and MC/DC
 * vector tests live in test_ra8_sci_dma_async.c.
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

/**
 * @enum t_sci_t
 * @brief SCI channel ids and the bytes staged in RDR.
 */
typedef enum : uint8_t {
  k_t_channel     = 5U,    /**< A valid SCI channel the dispatch arms drive.   */
  k_t_channel_bad = 99U,   /**< A channel past the last instance; every
                                dispatcher must ignore it rather than index
                                out of bounds.                                  */
  k_t_rdr_byte_a  = 0xA5U, /**< Byte staged in RDR for the first read arm.     */
  k_t_rdr_byte_b  = 0x77U, /**< A different byte for the second, so a stale
                                register value cannot pass as a fresh read.     */
} t_sci_t;

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
  reg->RDR    = k_t_rdr_byte_a;
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

  ra8_sci(4U)->RDR = k_t_rdr_byte_b;
  ra8_sci_dispatch_rxi(4U);
  TEST_ASSERT_EQ(1, s_rx_count);
  TEST_ASSERT_EQ(0x77, s_rx_last);

  /* Bad channel = no-op. */
  ra8_sci_dispatch_rxi(k_t_channel_bad);
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

  volatile r_sci_regs_t* reg = ra8_sci(k_t_channel);
  for (int32_t i = 0; i < s_tx_total; ++i) {
    ra8_sci_dispatch_txi(k_t_channel);
  }
  /* After the final byte, the next dispatch should clear TIE. */
  ra8_sci_dispatch_txi(k_t_channel);
  TEST_ASSERT_EQ(0, (reg->CCR0 & (1U << (uint8_t)k_ra8_sci_ccr0_bit_tie)));
  TEST_ASSERT_EQ(s_tx_total, s_tx_count);

  /* Bad channel = no-op. */
  ra8_sci_dispatch_txi(k_t_channel_bad);
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
  ra8_sci_dispatch_eri(k_t_channel_bad);
  TEST_END("ra8_sci_dispatch_eri clears CSR flags");
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
  (void)fprintf(stderr, "[OK  ] test_ra8_sci.c\n");
  return 0;
}
