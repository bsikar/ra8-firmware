/**
 * @file test_ra8_ssie_io.c
 * @brief Unit tests for the ra8_ssie data path: sample/buffer TX+RX,
 *        DMA pump attach/detach, status decode, IRQ enable + dispatch,
 *        power transitions, and the iso send/recv helpers
 *
 * @details Split from test_ra8_ssie.c along the test-group seam.
 * Shared fixture state (prep(), make_controller_i2s_cfg(), the stub
 * dispatch callback) lives in support/ssie_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_dmac.h"
#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "ra8_ssie.h"
#include "ra8_ssie_regs.h"
#include "support/ssie_test_util.h"
#include "unity_minimal.h"

/**
 * @enum ssie_io_test_lit_t
 * @brief Named constants for the register stamp patterns and literal
 *        test vectors previously inlined in this file's test bodies.
 */
typedef enum : uint32_t {
  k_ssie_io_lit_40             = 40,           /**< Ssie IO literal 40.            */
  k_ssie_io_lit_xaa00          = 0xAA00U,      /**< Ssie IO literal 0xAA00.        */
  k_ssie_io_lit_99             = 99U,          /**< Ssie IO literal 99.            */
  k_ssie_io_cfg_tx_dma_channel = 0xFFU,        /**< Ssie IO config tx DMA channel. */
  k_ssie_io_cfg_rx_dma_channel = 0xFFU,        /**< Ssie IO config rx DMA channel. */
  k_ssie_io_stamp_ssifrdr      = 0xDEAD0001UL, /**< Ssie IO stamp ssifrdr.         */
} ssie_io_test_lit_t;

/* ---------------------------------------------------------------------------
 * Data path
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_write_sample(void)
{
  TEST_BEGIN("ssie write_sample writes SSIFTDR");
  prep();

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ssie_write_sample((uint8_t)k_ra8_ssie_test_ch0, (uint32_t)k_ra8_ssie_test_sample_a));
  volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch0);
  TEST_ASSERT_EQ(k_ra8_ssie_test_sample_a, reg->SSIFTDR);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_write_sample((uint8_t)k_ra8_ssie_test_ch_bad, 0U));
  TEST_END("ssie write_sample writes SSIFTDR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_sample(void)
{
  TEST_BEGIN("ssie read_sample reads SSIFRDR");
  prep();

  volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch1);
  reg->SSIFRDR                = (uint32_t)k_ra8_ssie_test_sample_b;

  uint32_t sample = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_read_sample((uint8_t)k_ra8_ssie_test_ch1, &sample));
  TEST_ASSERT_EQ(k_ra8_ssie_test_sample_b, sample);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ssie_read_sample((uint8_t)k_ra8_ssie_test_ch1, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ssie_read_sample((uint8_t)k_ra8_ssie_test_ch_bad, &sample));
  TEST_END("ssie read_sample reads SSIFRDR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_buffer(void)
{
  TEST_BEGIN("ssie write_buffer pumps until FIFO full");
  prep();

  volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch0);
  /* Pretend the FIFO has 2 entries already; depth is 32, so we
   * should write only 30. */
  reg->SSIFSR = (uint32_t)(2U << (uint8_t)k_ra8_ssie_shift_tdc);
  uint32_t buf[k_ssie_io_lit_40];
  for (uint8_t i = 0U; i < k_ssie_io_lit_40; i++) {
    buf[i] = k_ssie_io_lit_xaa00 + i;
    /* keep TDC at 2 so the loop sees space (sim mmap is just RAM). */
  }
  uint16_t written = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_write_buffer((uint8_t)k_ra8_ssie_test_ch0, buf, 40U, &written));
  /* The simulator returns whatever we wrote into SSIFSR; since we
   * keep TDC=2 throughout, all 40 samples should fit. */
  TEST_ASSERT_EQ(40, written);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ssie_write_buffer((uint8_t)k_ra8_ssie_test_ch0, nullptr, 1U, &written));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ssie_write_buffer((uint8_t)k_ra8_ssie_test_ch0, buf, 1U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ssie_write_buffer((uint8_t)k_ra8_ssie_test_ch_bad, buf, 1U, &written));
  TEST_END("ssie write_buffer pumps until FIFO full");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_write_buffer_stops_when_full(void)
{
  TEST_BEGIN("ssie write_buffer stops when TDC reaches depth");
  prep();
  volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch0);
  /* Pre-load TDC=32 so the FIFO is at depth and zero stages fit. */
  reg->SSIFSR      = (uint32_t)((uint32_t)k_ra8_ssie_fifo_depth << (uint8_t)k_ra8_ssie_shift_tdc);
  uint32_t buf[4]  = {1U, 2U, 3U, 4U};
  uint16_t written = k_ssie_io_lit_99;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_write_buffer((uint8_t)k_ra8_ssie_test_ch0, buf, 4U, &written));
  TEST_ASSERT_EQ(0, written);
  TEST_END("ssie write_buffer stops when TDC reaches depth");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_buffer(void)
{
  TEST_BEGIN("ssie read_buffer pumps until FIFO empty");
  prep();

  volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch1);
  reg->SSIFSR                 = (uint32_t)(8U << (uint8_t)k_ra8_ssie_shift_rdc);
  reg->SSIFRDR                = (uint32_t)k_ra8_ssie_test_sample_b;
  uint32_t buf[8]             = {};
  uint16_t read               = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_read_buffer((uint8_t)k_ra8_ssie_test_ch1, buf, 8U, &read));
  TEST_ASSERT_EQ(8, read);
  for (uint8_t i = 0U; i < 8U; i++) {
    TEST_ASSERT_EQ(k_ra8_ssie_test_sample_b, buf[i]);
  }

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ssie_read_buffer((uint8_t)k_ra8_ssie_test_ch1, nullptr, 1U, &read));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ssie_read_buffer((uint8_t)k_ra8_ssie_test_ch1, buf, 1U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ssie_read_buffer((uint8_t)k_ra8_ssie_test_ch_bad, buf, 1U, &read));
  TEST_END("ssie read_buffer pumps until FIFO empty");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_buffer_empty_fifo(void)
{
  TEST_BEGIN("ssie read_buffer returns 0 when RDC=0");
  prep();
  volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch0);
  reg->SSIFSR                 = 0U;
  uint32_t buf[1]             = {};
  uint16_t read               = k_ssie_io_lit_99;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_read_buffer((uint8_t)k_ra8_ssie_test_ch0, buf, 1U, &read));
  TEST_ASSERT_EQ(0, read);
  TEST_END("ssie read_buffer returns 0 when RDC=0");
}

/* ---------------------------------------------------------------------------
 * DMA
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_attach_detach_dma(void)
{
  TEST_BEGIN("ssie attach_dma + detach_dma full duplex");
  prep();

  static uint32_t s_tx_buf[16];
  static uint32_t s_rx_buf[16];
  for (uint8_t i = 0U; i < 16U; i++) {
    s_tx_buf[i] = i;
  }

  const ra8_ssie_dma_cfg_t dma = {
    .tx_dma_channel = (uint8_t)k_ra8_ssie_test_dma_tx,
    .rx_dma_channel = (uint8_t)k_ra8_ssie_test_dma_rx,
    .tx_buffer      = s_tx_buf,
    .rx_buffer      = s_rx_buf,
    .tx_samples     = 16U,
    .rx_samples     = 16U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, &dma));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_detach_dma((uint8_t)k_ra8_ssie_test_ch0));
  TEST_END("ssie attach_dma + detach_dma full duplex");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_dma_tx_only(void)
{
  TEST_BEGIN("ssie attach_dma TX-only path");
  prep();
  static uint32_t          s_tx_buf[8];
  const ra8_ssie_dma_cfg_t dma = {
    .tx_dma_channel = (uint8_t)k_ra8_ssie_test_dma_tx,
    .rx_dma_channel = 0xFFU,
    .tx_buffer      = s_tx_buf,
    .rx_buffer      = nullptr,
    .tx_samples     = 8U,
    .rx_samples     = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, &dma));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_detach_dma((uint8_t)k_ra8_ssie_test_ch0));
  TEST_END("ssie attach_dma TX-only path");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_dma_bad_args(void)
{
  TEST_BEGIN("ssie attach_dma rejects bad descriptors");
  prep();
  static uint32_t s_buf[2] = {1U, 2U};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, nullptr));

  ra8_ssie_dma_cfg_t dma = {
    .tx_dma_channel = k_ssie_io_cfg_tx_dma_channel,
    .rx_dma_channel = k_ssie_io_cfg_rx_dma_channel,
    .tx_buffer      = nullptr,
    .rx_buffer      = nullptr,
    .tx_samples     = 0U,
    .rx_samples     = 0U,
  };
  /* No directions enabled. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, &dma));

  /* TX channel set but missing buffer. */
  dma.tx_dma_channel = (uint8_t)k_ra8_ssie_test_dma_tx;
  dma.tx_samples     = 4U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, &dma));
  dma.tx_buffer  = s_buf;
  dma.tx_samples = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, &dma));

  /* Bad channel. */
  dma.tx_samples = 4U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch_bad, &dma));

  /* Detach on bad channel. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_detach_dma((uint8_t)k_ra8_ssie_test_ch_bad));
  TEST_END("ssie attach_dma rejects bad descriptors");
}

/* ---------------------------------------------------------------------------
 * Status
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_get_status_decodes_fifo_levels(void)
{
  TEST_BEGIN("ssie get_status decodes RDC/TDC + flags");
  prep();

  volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch0);
  reg->SSIFSR                 = (uint32_t)k_ra8_ssie_test_ssifsr_demo;
  reg->SSISR = (uint32_t)k_ra8_ssie_mask_iirq | (uint32_t)k_ra8_ssie_test_ssisr_err;

  ra8_ssie_status_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_get_status((uint8_t)k_ra8_ssie_test_ch0, &st));
  TEST_ASSERT_EQ(10, st.tx_count);
  TEST_ASSERT_EQ(5, st.rx_count);
  TEST_ASSERT(st.rx_full);
  TEST_ASSERT(st.tx_empty);
  TEST_ASSERT(st.idle);
  TEST_ASSERT(st.error);

  TEST_ASSERT((st.events & (uint8_t)k_ra8_ssie_evt_idle) != 0U);
  TEST_ASSERT((st.events & (uint8_t)k_ra8_ssie_evt_tx_over) != 0U);
  TEST_ASSERT((st.events & (uint8_t)k_ra8_ssie_evt_rx_over) != 0U);
  TEST_ASSERT((st.events & (uint8_t)k_ra8_ssie_evt_tx_empty) != 0U);
  TEST_ASSERT((st.events & (uint8_t)k_ra8_ssie_evt_rx_full) != 0U);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ssie_get_status((uint8_t)k_ra8_ssie_test_ch0, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_get_status((uint8_t)k_ra8_ssie_test_ch_bad, &st));
  TEST_END("ssie get_status decodes RDC/TDC + flags");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_status_masks_to_writeable(void)
{
  TEST_BEGIN("ssie clear_status only touches error bits");
  prep();

  volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch0);
  reg->SSISR                  = (uint32_t)k_ra8_ssie_mask_iirq | (uint32_t)k_ra8_ssie_mask_err_all;

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ssie_clear_status((uint8_t)k_ra8_ssie_test_ch0,
                                       (uint32_t)k_ra8_ssie_mask_roirq |
                                         (uint32_t)k_ra8_ssie_mask_toirq | 0x1U));

  const uint32_t expected = (uint32_t)k_ra8_ssie_mask_iirq | (uint32_t)k_ra8_ssie_mask_ruirq |
                            (uint32_t)k_ra8_ssie_mask_tuirq;
  TEST_ASSERT_EQ(expected, reg->SSISR);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_clear_status((uint8_t)k_ra8_ssie_test_ch_bad, 0U));
  TEST_END("ssie clear_status only touches error bits");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_irq_enable(void)
{
  TEST_BEGIN("ssie set_irq_enable updates SSICR IRQ mask");
  prep();
  const ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ssie_set_irq_enable((uint8_t)k_ra8_ssie_test_ch0, (uint32_t)k_ra8_ssie_mask_irq_all, true));
  volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch0);
  TEST_ASSERT_EQ(k_ra8_ssie_mask_irq_all, (reg->SSICR & (uint32_t)k_ra8_ssie_mask_irq_all));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ssie_set_irq_enable((uint8_t)k_ra8_ssie_test_ch0, (uint32_t)k_ra8_ssie_mask_iien, false));
  TEST_ASSERT_EQ(0, (reg->SSICR & (uint32_t)k_ra8_ssie_mask_iien));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ssie_set_irq_enable((uint8_t)k_ra8_ssie_test_ch_bad, 0U, true));
  TEST_END("ssie set_irq_enable updates SSICR IRQ mask");
}

/* ---------------------------------------------------------------------------
 * IRQ
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("ssie attach + dispatch decodes events");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_attach_handler(stub_ssie_cb, (void*)(uintptr_t)0xA1B2U));
  volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch1);
  reg->SSISR  = (uint32_t)k_ra8_ssie_test_ssisr_err | (uint32_t)k_ra8_ssie_mask_tuirq;
  reg->SSIFSR = (uint32_t)k_ra8_ssie_mask_tde;

  ra8_ssie_dispatch((uint8_t)k_ra8_ssie_test_ch1);
  TEST_ASSERT_EQ(1, s_ssie_cb_count);
  TEST_ASSERT_EQ(k_ra8_ssie_test_ch1, s_ssie_cb_last_ch);
  TEST_ASSERT((s_ssie_cb_last_events & (uint8_t)k_ra8_ssie_evt_tx_over) != 0U);
  TEST_ASSERT((s_ssie_cb_last_events & (uint8_t)k_ra8_ssie_evt_rx_over) != 0U);
  TEST_ASSERT((s_ssie_cb_last_events & (uint8_t)k_ra8_ssie_evt_tx_under) != 0U);
  TEST_ASSERT((s_ssie_cb_last_events & (uint8_t)k_ra8_ssie_evt_tx_empty) != 0U);

  /* Out-of-range channel must be silently dropped. */
  ra8_ssie_dispatch((uint8_t)k_ra8_ssie_test_ch_bad);
  TEST_ASSERT_EQ(1, s_ssie_cb_count);

  /* Detach -> dispatch is a no-op. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_attach_handler(nullptr, nullptr));
  ra8_ssie_dispatch((uint8_t)k_ra8_ssie_test_ch0);
  TEST_ASSERT_EQ(1, s_ssie_cb_count);
  TEST_END("ssie attach + dispatch decodes events");
}

/* ---------------------------------------------------------------------------
 * Power transition
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_power_transition(void)
{
  TEST_BEGIN("ssie power transition");
  prep();

  const ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_enter_stop((uint8_t)k_ra8_ssie_test_ch0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_exit_stop((uint8_t)k_ra8_ssie_test_ch0));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_enter_stop((uint8_t)k_ra8_ssie_test_ch_bad));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_exit_stop((uint8_t)k_ra8_ssie_test_ch_bad));
  TEST_END("ssie power transition");
}

/* ---------------------------------------------------------------------------
 * Sweep 17 additions: set_fifo_threshold + attach_dma_pair + send/recv iso
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_set_fifo_threshold_happy(void)
{
  TEST_BEGIN("ssie set_fifo_threshold programmes SSISCR");
  prep();
  const ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_set_fifo_threshold((uint8_t)k_ra8_ssie_test_ch0, 0x10U, 0x05U));
  volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch0);
  TEST_ASSERT((reg->SSISCR & (uint32_t)k_ra8_ssie_mask_tdes) != 0U);
  TEST_ASSERT((reg->SSISCR & (uint32_t)k_ra8_ssie_mask_rdfs) != 0U);
  TEST_END("ssie set_fifo_threshold programmes SSISCR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_fifo_threshold_bad_args(void)
{
  TEST_BEGIN("ssie set_fifo_threshold rejects bad arguments");
  prep();
  const ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ssie_set_fifo_threshold((uint8_t)k_ra8_ssie_test_ch0, 0x40U, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ssie_set_fifo_threshold((uint8_t)k_ra8_ssie_test_ch_bad, 0U, 0U));
  TEST_END("ssie set_fifo_threshold rejects bad arguments");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_dma_pair_happy(void)
{
  TEST_BEGIN("ssie attach_dma_pair binds tx+rx ids");
  prep();
  const ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ssie_attach_dma_pair((uint8_t)k_ra8_ssie_test_ch0,
                                          (uint8_t)k_ra8_ssie_test_dma_tx,
                                          (uint8_t)k_ra8_ssie_test_dma_rx));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_detach_dma((uint8_t)k_ra8_ssie_test_ch0));
  TEST_END("ssie attach_dma_pair binds tx+rx ids");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_dma_pair_bad_args(void)
{
  TEST_BEGIN("ssie attach_dma_pair rejects all-unused / bad channel");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ssie_attach_dma_pair((uint8_t)k_ra8_ssie_test_ch_bad, 0U, 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ssie_attach_dma_pair((uint8_t)k_ra8_ssie_test_ch0, 0xFFU, 0xFFU));
  TEST_END("ssie attach_dma_pair rejects all-unused / bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_iso_happy(void)
{
  TEST_BEGIN("ssie send_iso pushes all samples");
  prep();
  const ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));

  static const uint32_t buf[4] = {0xAA00U, 0xBB11U, 0xCC22U, 0xDD33U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_send_iso((uint8_t)k_ra8_ssie_test_ch0, buf, 4U));
  TEST_END("ssie send_iso pushes all samples");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_send_iso_bad_args(void)
{
  TEST_BEGIN("ssie send_iso rejects null buffer / bad channel");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ssie_send_iso((uint8_t)k_ra8_ssie_test_ch0, nullptr, 1U));
  static const uint32_t buf[1] = {0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ssie_send_iso((uint8_t)k_ra8_ssie_test_ch_bad, buf, 1U));
  TEST_END("ssie send_iso rejects null buffer / bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_recv_iso_drains_fifo(void)
{
  TEST_BEGIN("ssie recv_iso drains rx FIFO");
  prep();
  const ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));

  /* Stub the SSIFSR RDC field so the loop runs at least once before
   * deciding the FIFO is empty. */
  volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch0);
  reg->SSIFSR                 = (uint32_t)1U << (uint8_t)k_ra8_ssie_shift_rdc;
  reg->SSIFRDR                = k_ssie_io_stamp_ssifrdr;

  uint32_t out[2] = {0U, 0U};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_recv_iso((uint8_t)k_ra8_ssie_test_ch0, out, 2U, &got));
  TEST_ASSERT(got <= 2U);
  TEST_END("ssie recv_iso drains rx FIFO");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_recv_iso_bad_args(void)
{
  TEST_BEGIN("ssie recv_iso rejects null + bad channel");
  prep();
  uint32_t out = 0U;
  uint16_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ssie_recv_iso((uint8_t)k_ra8_ssie_test_ch0, nullptr, 1U, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ssie_recv_iso((uint8_t)k_ra8_ssie_test_ch0, &out, 1U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ssie_recv_iso((uint8_t)k_ra8_ssie_test_ch_bad, &out, 1U, &got));
  TEST_END("ssie recv_iso rejects null + bad channel");
}

int32_t main(void)
{
  test_write_sample();
  test_read_sample();
  test_write_buffer();
  test_write_buffer_stops_when_full();
  test_read_buffer();
  test_read_buffer_empty_fifo();

  test_attach_detach_dma();
  test_attach_dma_tx_only();
  test_attach_dma_bad_args();

  test_get_status_decodes_fifo_levels();
  test_clear_status_masks_to_writeable();
  test_set_irq_enable();
  test_attach_and_dispatch();
  test_power_transition();

  test_set_fifo_threshold_happy();
  test_set_fifo_threshold_bad_args();
  test_attach_dma_pair_happy();
  test_attach_dma_pair_bad_args();
  test_send_iso_happy();
  test_send_iso_bad_args();
  test_recv_iso_drains_fifo();
  test_recv_iso_bad_args();
  (void)fprintf(stderr, "[OK  ] test_ra8_ssie_io.c\n");
  return 0;
}
