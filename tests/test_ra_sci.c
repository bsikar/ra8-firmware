/**
 * @file test_ra_sci.c
 * @brief Unit tests for the full-featured SCI driver (libs/ra_hal/src/ra_sci.c).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_sci_regs.h"
#include "ra_dma.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sci.h"
#include "ra_sim_dma.h"
#include "ra_sim_mmap.h"
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
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_rx_count   = 0;
  s_rx_last    = 0;
  s_tx_count   = 0;
  s_tx_seq_idx = 0;
}

static const ra_sci_cfg_t k_cfg = {
  .baud      = 115200U,
  .data_bits = k_ra_sci_data_8,
  .parity    = k_ra_sci_parity_none,
  .stop_bits = k_ra_sci_stop_1,
  .pclk_hz   = 60000000U,
};

static void test_init_sets_scr_enables(void)
{
  TEST_BEGIN("ra_sci_init: SCR enables TE + RE");
  prep();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_init(0U, &k_cfg));
  volatile const r_sci_regs_t* reg = ra_sci(0U);
  const uint8_t                scr = reg->SCR;
  TEST_ASSERT((scr & (uint8_t)(1U << (uint8_t)k_ra_scr_bit_te)) != 0U);
  TEST_ASSERT((scr & (uint8_t)(1U << (uint8_t)k_ra_scr_bit_re)) != 0U);
  TEST_END("ra_sci_init: SCR enables TE + RE");
}

static void test_init_rejects_bad_channel(void)
{
  TEST_BEGIN("ra_sci_init: bad channel rejected");
  prep();

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sci_init(99U, &k_cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sci_init(0U, nullptr));
  TEST_END("ra_sci_init: bad channel rejected");
}

static void test_polling_tx_rx_round_trip(void)
{
  TEST_BEGIN("ra_sci polling tx/rx round trip");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_init(1U, &k_cfg));

  /* Pre-seed TDRE so putc's spin completes on the first iteration. */
  volatile r_sci_regs_t* reg = ra_sci(1U);
  reg->SSR                   = (uint8_t)(1U << (uint8_t)k_ra_ssr_bit_tdre);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_putc_polling(1U, 0x5AU));
  TEST_ASSERT_EQ((int32_t)0x5A, (int32_t)reg->TDR);

  /* Poll RX: pre-seed RDRF and stage a byte in RDR. */
  reg->SSR    = (uint8_t)(reg->SSR | (uint8_t)(1U << (uint8_t)k_ra_ssr_bit_rdrf));
  reg->RDR    = 0xA5U;
  uint8_t got = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_getc_polling(1U, &got));
  TEST_ASSERT_EQ((int32_t)0xA5, (int32_t)got);
  TEST_END("ra_sci polling tx/rx round trip");
}

static void test_polling_tx_timeout(void)
{
  TEST_BEGIN("ra_sci polling tx timeout");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_init(0U, &k_cfg));

  /* SSR.TDRE starts cleared; init already set it via the reset
   * path in the test helper. Force-clear it so the wait fails. */
  ra_sci(0U)->SSR = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_hw_timeout, (int32_t)ra_sci_putc_polling(0U, 0x00U));
  TEST_END("ra_sci polling tx timeout");
}

static void test_write_polling_null_plus_nonzero(void)
{
  TEST_BEGIN("ra_sci write polling null buffer rejected");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_init(0U, &k_cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sci_write_polling(0U, nullptr, 1U));
  /* Zero-length is always ok, even with NULL. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_write_polling(0U, nullptr, 0U));
  TEST_END("ra_sci write polling null buffer rejected");
}

static void test_attach_rx_sets_rie(void)
{
  TEST_BEGIN("ra_sci_attach_rx_handler sets SCR.RIE");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_init(2U, &k_cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_attach_rx_handler(2U, stub_rx, nullptr));
  volatile const r_sci_regs_t* reg = ra_sci(2U);
  TEST_ASSERT((reg->SCR & (uint8_t)(1U << (uint8_t)k_ra_scr_bit_rie)) != 0U);

  /* Detach clears the bit. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_attach_rx_handler(2U, nullptr, nullptr));
  TEST_ASSERT_EQ(0, (int32_t)(reg->SCR & (uint8_t)(1U << (uint8_t)k_ra_scr_bit_rie)));
  TEST_END("ra_sci_attach_rx_handler sets SCR.RIE");
}

static void test_attach_tx_sets_tie(void)
{
  TEST_BEGIN("ra_sci_attach_tx_handler sets SCR.TIE");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_init(3U, &k_cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_attach_tx_handler(3U, stub_tx, nullptr));
  volatile const r_sci_regs_t* reg = ra_sci(3U);
  TEST_ASSERT((reg->SCR & (uint8_t)(1U << (uint8_t)k_ra_scr_bit_tie)) != 0U);

  /* Bad channel rejected. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_sci_attach_tx_handler(99U, stub_tx, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_sci_attach_rx_handler(99U, stub_rx, nullptr));
  TEST_END("ra_sci_attach_tx_handler sets SCR.TIE");
}

static void test_dispatch_rxi_invokes_handler(void)
{
  TEST_BEGIN("ra_sci_dispatch_rxi: calls handler with RDR byte");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_init(4U, &k_cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_attach_rx_handler(4U, stub_rx, nullptr));

  ra_sci(4U)->RDR = 0x77U;
  ra_sci_dispatch_rxi(4U);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_rx_count);
  TEST_ASSERT_EQ((int32_t)0x77, (int32_t)s_rx_last);

  /* Bad channel = no-op. */
  ra_sci_dispatch_rxi(99U);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_rx_count);
  TEST_END("ra_sci_dispatch_rxi: calls handler with RDR byte");
}

static void test_dispatch_txi_drains_callback(void)
{
  TEST_BEGIN("ra_sci_dispatch_txi: advances TX handler until end");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_init(5U, &k_cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_attach_tx_handler(5U, stub_tx, nullptr));

  volatile r_sci_regs_t* reg = ra_sci(5U);
  for (int32_t i = 0; i < s_tx_total; ++i) {
    ra_sci_dispatch_txi(5U);
  }
  /* After the final byte, the next dispatch should clear TIE. */
  ra_sci_dispatch_txi(5U);
  TEST_ASSERT_EQ(0, (int32_t)(reg->SCR & (uint8_t)(1U << (uint8_t)k_ra_scr_bit_tie)));
  TEST_ASSERT_EQ((int32_t)s_tx_total, (int32_t)s_tx_count);

  /* Bad channel = no-op. */
  ra_sci_dispatch_txi(99U);
  TEST_END("ra_sci_dispatch_txi: advances TX handler until end");
}

static void test_errors_mask_and_clear(void)
{
  TEST_BEGIN("ra_sci_get_errors + clear_errors");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_init(6U, &k_cfg));

  volatile r_sci_regs_t* reg = ra_sci(6U);
  reg->SSR = (uint8_t)((1U << (uint8_t)k_ra_ssr_bit_orer) | (1U << (uint8_t)k_ra_ssr_bit_fer) |
                       (1U << (uint8_t)k_ra_ssr_bit_per));

  uint8_t mask = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_get_errors(6U, &mask));
  TEST_ASSERT_EQ((int32_t)((uint8_t)k_ra_sci_err_overrun | (uint8_t)k_ra_sci_err_framing |
                           (uint8_t)k_ra_sci_err_parity),
                 (int32_t)mask);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_clear_errors(6U));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_get_errors(6U, &mask));
  TEST_ASSERT_EQ((int32_t)k_ra_sci_err_none, (int32_t)mask);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sci_get_errors(6U, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sci_get_errors(99U, &mask));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sci_clear_errors(99U));
  TEST_END("ra_sci_get_errors + clear_errors");
}

static void test_set_baud_round_trip(void)
{
  TEST_BEGIN("ra_sci_set_baud: BRR recomputed");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_init(7U, &k_cfg));

  const uint8_t brr_before = ra_sci(7U)->BRR;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_set_baud(7U, 9600U, 60000000U));
  const uint8_t brr_after = ra_sci(7U)->BRR;
  TEST_ASSERT(brr_after != brr_before);

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sci_set_baud(7U, 0U, 60000000U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sci_set_baud(99U, 115200U, 60000000U));
  TEST_END("ra_sci_set_baud: BRR recomputed");
}

static void test_enter_exit_stop(void)
{
  TEST_BEGIN("ra_sci_enter_stop / exit_stop");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_init(8U, &k_cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_enter_stop(8U));

  bool stopped = false;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mstp_is_stopped(k_ra_mstp_sci8, &stopped));
  TEST_ASSERT(stopped);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_exit_stop(8U));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mstp_is_stopped(k_ra_mstp_sci8, &stopped));
  TEST_ASSERT(!stopped);

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sci_enter_stop(99U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sci_exit_stop(99U));
  TEST_END("ra_sci_enter_stop / exit_stop");
}

static void test_deinit_releases(void)
{
  TEST_BEGIN("ra_sci_deinit releases MSTP");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_init(9U, &k_cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_deinit(9U));
  bool stopped = false;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mstp_is_stopped(k_ra_mstp_sci9, &stopped));
  TEST_ASSERT(stopped);

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sci_deinit(99U));
  TEST_END("ra_sci_deinit releases MSTP");
}

static void test_cfg_variants(void)
{
  TEST_BEGIN("ra_sci_init: all cfg variants reach BRR/SMR branches");
  prep();

  /* 2 stop bits + odd parity + 7-bit data -- exercises all SMR branches. */
  const ra_sci_cfg_t cfg_a = {
    .baud      = 9600U,
    .data_bits = k_ra_sci_data_7,
    .parity    = k_ra_sci_parity_odd,
    .stop_bits = k_ra_sci_stop_2,
    .pclk_hz   = 60000000U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_init(0U, &cfg_a));

  /* Even parity path. */
  const ra_sci_cfg_t cfg_b = {
    .baud      = 115200U,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_even,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = 60000000U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_init(1U, &cfg_b));

  /* zero baud -> BRR returns 0, but init() still returns ok because
   * it does not validate the resulting BRR. */
  const ra_sci_cfg_t cfg_c = {
    .baud      = 0U,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = 60000000U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_init(2U, &cfg_c));

  /* Ridiculous baud so the divisor exceeds PCLK; n == 0 branch. */
  const ra_sci_cfg_t cfg_d = {
    .baud      = 0xFFFFFFFFU,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = 60000000U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_init(3U, &cfg_d));
  TEST_END("ra_sci_init: all cfg variants reach BRR/SMR branches");
}

static void test_getc_polling_null(void)
{
  TEST_BEGIN("ra_sci_getc_polling: null + bad channel");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_init(0U, &k_cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sci_getc_polling(0U, nullptr));

  uint8_t got = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sci_getc_polling(99U, &got));

  /* Timeout path on RX. */
  ra_sci(0U)->SSR = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_hw_timeout, (int32_t)ra_sci_getc_polling(0U, &got));
  TEST_END("ra_sci_getc_polling: null + bad channel");
}

static void test_putc_polling_bad_channel(void)
{
  TEST_BEGIN("ra_sci_putc_polling: bad channel rejected");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sci_putc_polling(99U, 0U));
  TEST_END("ra_sci_putc_polling: bad channel rejected");
}

static void test_dispatch_txi_with_no_handler(void)
{
  TEST_BEGIN("ra_sci_dispatch_txi: no-handler clears TIE");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_init(0U, &k_cfg));
  /* Manually set TIE. */
  volatile r_sci_regs_t* reg = ra_sci(0U);
  reg->SCR                   = (uint8_t)(reg->SCR | (uint8_t)(1U << (uint8_t)k_ra_scr_bit_tie));
  /* Dispatch with no handler attached -- should clear TIE. */
  ra_sci_dispatch_txi(0U);
  TEST_ASSERT_EQ(0, (int32_t)(reg->SCR & (uint8_t)(1U << (uint8_t)k_ra_scr_bit_tie)));
  TEST_END("ra_sci_dispatch_txi: no-handler clears TIE");
}

static void test_dispatch_rxi_with_no_handler(void)
{
  TEST_BEGIN("ra_sci_dispatch_rxi: no-handler is no-op");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_init(0U, &k_cfg));
  /* Dispatch without attaching a handler -- just reads RDR. */
  ra_sci_dispatch_rxi(0U);
  /* No assertion other than "did not crash"; covered the no-cb branch. */
  TEST_END("ra_sci_dispatch_rxi: no-handler is no-op");
}

static void test_eri_dispatch_clears_errors(void)
{
  TEST_BEGIN("ra_sci_dispatch_eri clears SSR flags");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_init(0U, &k_cfg));
  volatile r_sci_regs_t* reg = ra_sci(0U);
  reg->SSR                   = (uint8_t)(reg->SSR | (uint8_t)(1U << (uint8_t)k_ra_ssr_bit_orer));

  ra_sci_dispatch_eri(0U);
  uint8_t mask = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_get_errors(0U, &mask));
  TEST_ASSERT_EQ((int32_t)k_ra_sci_err_none, (int32_t)mask);

  /* Out-of-range is a no-op. */
  ra_sci_dispatch_eri(99U);
  TEST_END("ra_sci_dispatch_eri clears SSR flags");
}

static int32_t s_dma_complete_count = 0;

static void stub_dma_done(void* ctx)
{
  (void)ctx;
  ++s_dma_complete_count;
}

static void test_write_dma_streams_buffer_to_tdr(void)
{
  TEST_BEGIN("ra_sci_write_dma: buffer streams into TDR via DMA");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_init(0U, &k_cfg));

  const uint8_t src[]  = {0xAAU, 0xBBU, 0xCCU, 0xDDU};
  uint8_t       dma_ch = 0xFFU;
  s_dma_complete_count = 0;
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_sci_write_dma(0U, src, (uint16_t)sizeof(src), stub_dma_done, nullptr, &dma_ch));
  TEST_ASSERT(dma_ch < 8U);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sim_dma_memcpy(dma_ch));
  volatile const r_sci_regs_t* reg = ra_sci(0U);
  /* Last byte streamed lands in TDR (dst_inc=false). */
  TEST_ASSERT_EQ((int32_t)0xDDU, (int32_t)reg->TDR);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sim_dma_complete(dma_ch));
  TEST_ASSERT_EQ(1, s_dma_complete_count);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_release(dma_ch));
  TEST_END("ra_sci_write_dma: buffer streams into TDR via DMA");
}

static void test_read_dma_streams_rdr_to_buffer(void)
{
  TEST_BEGIN("ra_sci_read_dma: RDR streams into buffer via DMA");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_init(0U, &k_cfg));

  volatile r_sci_regs_t* reg = ra_sci(0U);
  reg->RDR                   = 0x42U;

  uint8_t out[3]       = {0U, 0U, 0U};
  uint8_t dma_ch       = 0xFFU;
  s_dma_complete_count = 0;
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_sci_read_dma(0U, out, (uint16_t)sizeof(out), stub_dma_done, nullptr, &dma_ch));
  TEST_ASSERT(dma_ch < 8U);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sim_dma_memcpy(dma_ch));
  /* RDR (src_inc=false) was 0x42 throughout, so all destinations match. */
  TEST_ASSERT_EQ((int32_t)0x42U, (int32_t)out[0]);
  TEST_ASSERT_EQ((int32_t)0x42U, (int32_t)out[1]);
  TEST_ASSERT_EQ((int32_t)0x42U, (int32_t)out[2]);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sim_dma_complete(dma_ch));
  TEST_ASSERT_EQ(1, s_dma_complete_count);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_release(dma_ch));
  TEST_END("ra_sci_read_dma: RDR streams into buffer via DMA");
}

static void test_dma_arg_validation(void)
{
  TEST_BEGIN("ra_sci_{write,read}_dma: arg validation");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sci_init(0U, &k_cfg));

  uint8_t       dma_ch = 0U;
  const uint8_t src[]  = {0x01U};
  uint8_t       dst[1] = {0U};

  /* NULL data / out_buf / out_dma_channel. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_sci_write_dma(0U, nullptr, 1U, nullptr, nullptr, &dma_ch));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_sci_write_dma(0U, src, 1U, nullptr, nullptr, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_sci_read_dma(0U, nullptr, 1U, nullptr, nullptr, &dma_ch));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_sci_read_dma(0U, dst, 1U, nullptr, nullptr, nullptr));

  /* Bad channel. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_sci_write_dma(99U, src, 1U, nullptr, nullptr, &dma_ch));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_sci_read_dma(99U, dst, 1U, nullptr, nullptr, &dma_ch));

  /* Zero-length transfer. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_sci_write_dma(0U, src, 0U, nullptr, nullptr, &dma_ch));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_sci_read_dma(0U, dst, 0U, nullptr, nullptr, &dma_ch));

  TEST_END("ra_sci_{write,read}_dma: arg validation");
}

int32_t main(void)
{
  test_init_sets_scr_enables();
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
  (void)fprintf(stderr, "[OK  ] test_ra_sci.c\n");
  return 0;
}
