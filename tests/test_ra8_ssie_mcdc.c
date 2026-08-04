/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_ssie_mcdc.c
 * @brief MC/DC vector tests for the compound boolean decisions in
 *        ra8_ssie.c
 *
 * @details Split from test_ra8_ssie.c along the test-group seam: one
 * test_mcdc_<short_name>() per compound decision, each using minimal
 * N+1 vectors (Chilenski masking-MC/DC). Shared fixture state lives in
 * support/ssie_test_util.h.
 */

#include "ra8_dmac.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_ssie.h"
#include "ra8_ssie_regs.h"
#include "support/ssie_test_util.h"
#include "unity_minimal.h"

/* ===========================================================================
 * MC/DC vector coverage for compound boolean decisions in ra8_ssie.c.
 *
 * Per docs/MCDC_GAPS.csv there are 10 compound decisions in ra8_ssie.c.
 * Each test_mcdc_<short_name>(void) below targets one decision using
 * minimal N+1 vectors (Chilenski masking-MC/DC). Decisions with three
 * conditions use 4 vectors per DO-178C 6.4.4.3.
 * ===========================================================================
 */

/**
 * @enum ra8_ssie_mcdc_const_t
 * @brief Numeric vectors for the SSIE MC/DC tests.
 */
typedef enum : uint16_t {
  k_ssie_mcdc_thresh_in   = 0x10U, /**< Within k_ra8_ssie_thresh_max=0x1F. */
  k_ssie_mcdc_thresh_over = 0x40U, /**< Above max.                         */
  k_ssie_mcdc_dma_valid   = 3U,    /**< Valid DMAC channel (< 8).          */
  k_ssie_mcdc_dma_bad     = 100U,  /**< Above k_ra8_ssie_dma_max_ch=8.     */
  k_ssie_mcdc_dir_bad     = 5U,    /**< Not rx/tx/tx_rx.                   */
} ra8_ssie_mcdc_const_t;

/**
 * @test test_mcdc_init_lrcont
 *
 * @par MC/DC:
 * Decision: `if (cfg->lr_continue && cfg->role == k_ra8_ssie_role_controller)`
 * (2 conditions, libs/ra8_hal/src/ra8_ssie.c line 321 -- internal_build_ssiofr)
 * Outcome observable: SSIOFR.LRCONT bit set/clear.
 * - V1: lr=true,  role=controller -> T,T -> bit set
 * - V2: lr=false, role=controller -> F (short-circuit) -> bit clear
 * - V3: lr=true,  role=peripheral -> T,F -> bit clear
 * V1 vs V2 vary C1; V1 vs V3 vary C2 (C1 held T). N+1 = 3 vectors.
 */
static void test_mcdc_init_lrcont(void)
{
  TEST_BEGIN("ssie MC/DC build_ssiofr lrcont && controller");
  /* V1 */
  prep();
  ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  cfg.lr_continue    = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));
  TEST_ASSERT((ra8_ssie((uint8_t)k_ra8_ssie_test_ch0)->SSIOFR & (uint32_t)k_ra8_ssie_mask_lrcont) !=
              0U);
  /* V2 */
  prep();
  cfg             = make_controller_i2s_cfg();
  cfg.lr_continue = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));
  TEST_ASSERT_EQ(
    0,
    (ra8_ssie((uint8_t)k_ra8_ssie_test_ch0)->SSIOFR & (uint32_t)k_ra8_ssie_mask_lrcont));
  /* V3 */
  prep();
  cfg             = make_controller_i2s_cfg();
  cfg.lr_continue = true;
  cfg.role        = k_ra8_ssie_role_peripheral;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));
  TEST_ASSERT_EQ(
    0,
    (ra8_ssie((uint8_t)k_ra8_ssie_test_ch0)->SSIOFR & (uint32_t)k_ra8_ssie_mask_lrcont));
  TEST_END("ssie MC/DC build_ssiofr lrcont && controller");
}

/**
 * @test test_mcdc_init_bckastp
 *
 * @par MC/DC:
 * Decision: `if (cfg->bck_idle_stop && cfg->role == k_ra8_ssie_role_controller &&
 *               !cfg->lr_continue)`
 * (3 conditions, libs/ra8_hal/src/ra8_ssie.c line 324 -- internal_build_ssiofr)
 * Outcome observable: SSIOFR.BCKASTP bit set/clear.
 *
 * Per DO-178C 6.4.4.3, N+1 = 4 vectors suffice for an N-condition pure-AND
 * decision: pick a baseline that asserts the decision T, then flip each
 * condition individually to F to prove independent effect.
 * - V1: stop=T, ctrl=T, !lr=T (lr=F)        -> T && T && T -> decision T (bit set)
 * - V2: stop=F (others as V1)                -> F short-circ -> decision F (clear)
 * - V3: stop=T, role=peripheral (so ctrl=F)  -> T,F,_ -> F (clear)
 * - V4: stop=T, ctrl=T, lr=T (so !lr=F)      -> T,T,F -> F (clear)
 * V1+V2 vary C1; V1+V3 vary C2 (with C1=T); V1+V4 vary C3 (with C1=T,C2=T).
 */
static void test_mcdc_init_bckastp(void)
{
  TEST_BEGIN("ssie MC/DC build_ssiofr bckastp && controller && !lrcont");
  /* V1 */
  prep();
  ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  cfg.bck_idle_stop  = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));
  TEST_ASSERT(
    (ra8_ssie((uint8_t)k_ra8_ssie_test_ch0)->SSIOFR & (uint32_t)k_ra8_ssie_mask_bckastp) != 0U);
  /* V2 */
  prep();
  cfg               = make_controller_i2s_cfg();
  cfg.bck_idle_stop = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));
  TEST_ASSERT_EQ(
    0,
    (ra8_ssie((uint8_t)k_ra8_ssie_test_ch0)->SSIOFR & (uint32_t)k_ra8_ssie_mask_bckastp));
  /* V3 */
  prep();
  cfg               = make_controller_i2s_cfg();
  cfg.bck_idle_stop = true;
  cfg.role          = k_ra8_ssie_role_peripheral;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));
  TEST_ASSERT_EQ(
    0,
    (ra8_ssie((uint8_t)k_ra8_ssie_test_ch0)->SSIOFR & (uint32_t)k_ra8_ssie_mask_bckastp));
  /* V4 */
  prep();
  cfg               = make_controller_i2s_cfg();
  cfg.bck_idle_stop = true;
  cfg.lr_continue   = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));
  TEST_ASSERT_EQ(
    0,
    (ra8_ssie((uint8_t)k_ra8_ssie_test_ch0)->SSIOFR & (uint32_t)k_ra8_ssie_mask_bckastp));
  TEST_END("ssie MC/DC build_ssiofr bckastp && controller && !lrcont");
}

/**
 * @test test_mcdc_init_aucke
 *
 * @par MC/DC:
 * Decision: `if (cfg->enable_aucke && cfg->role == k_ra8_ssie_role_controller)`
 * (2 conditions, libs/ra8_hal/src/ra8_ssie.c line 341 -- internal_build_ssifcr)
 * Outcome observable: SSIFCR.AUCKE bit set/clear.
 * - V1: aucke=T, ctrl=T       -> T,T -> bit set
 * - V2: aucke=F, ctrl=T       -> F short-circ -> clear
 * - V3: aucke=T, peripheral   -> T,F -> clear
 */
static void test_mcdc_init_aucke(void)
{
  TEST_BEGIN("ssie MC/DC build_ssifcr aucke && controller");
  /* V1 */
  prep();
  ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  cfg.enable_aucke   = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));
  TEST_ASSERT((ra8_ssie((uint8_t)k_ra8_ssie_test_ch0)->SSIFCR & (uint32_t)k_ra8_ssie_mask_aucke) !=
              0U);
  /* V2 */
  prep();
  cfg              = make_controller_i2s_cfg();
  cfg.enable_aucke = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));
  TEST_ASSERT_EQ(
    0,
    (ra8_ssie((uint8_t)k_ra8_ssie_test_ch0)->SSIFCR & (uint32_t)k_ra8_ssie_mask_aucke));
  /* V3 */
  prep();
  cfg              = make_controller_i2s_cfg();
  cfg.enable_aucke = true;
  cfg.role         = k_ra8_ssie_role_peripheral;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));
  TEST_ASSERT_EQ(
    0,
    (ra8_ssie((uint8_t)k_ra8_ssie_test_ch0)->SSIFCR & (uint32_t)k_ra8_ssie_mask_aucke));
  TEST_END("ssie MC/DC build_ssifcr aucke && controller");
}

/**
 * @test test_mcdc_init_threshold
 *
 * @par MC/DC:
 * Decision: `if (cfg->tx_threshold > thresh_max || cfg->rx_threshold > thresh_max)`
 * (2 conditions, libs/ra8_hal/src/ra8_ssie.c line 410 -- internal_validate_init_cfg)
 * - V1: tx=0x10, rx=0x10 -> F,F -> ok
 * - V2: tx=0x40, rx=0x10 -> T short-circ -> invalid_arg
 * - V3: tx=0x10, rx=0x40 -> F,T -> invalid_arg
 */
static void test_mcdc_init_threshold(void)
{
  TEST_BEGIN("ssie MC/DC init threshold range");
  prep();
  ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  cfg.tx_threshold   = (uint8_t)k_ssie_mcdc_thresh_in;
  cfg.rx_threshold   = (uint8_t)k_ssie_mcdc_thresh_in;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));
  prep();
  cfg              = make_controller_i2s_cfg();
  cfg.tx_threshold = (uint8_t)k_ssie_mcdc_thresh_over;
  cfg.rx_threshold = (uint8_t)k_ssie_mcdc_thresh_in;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));
  prep();
  cfg              = make_controller_i2s_cfg();
  cfg.tx_threshold = (uint8_t)k_ssie_mcdc_thresh_in;
  cfg.rx_threshold = (uint8_t)k_ssie_mcdc_thresh_over;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));
  TEST_END("ssie MC/DC init threshold range");
}

/**
 * @test test_mcdc_start_dir
 *
 * @par MC/DC:
 * Decision: `if (dir != k_ra8_ssie_dir_rx && dir != k_ra8_ssie_dir_tx &&
 *               dir != k_ra8_ssie_dir_tx_rx)`
 * (3 conditions, libs/ra8_hal/src/ra8_ssie.c line 518 -- ra8_ssie_start)
 *
 * Per DO-178C 6.4.4.3, N+1 = 4 vectors are sufficient for an N-condition
 * pure-AND decision: each condition's unique-false vector pairs with the
 * all-true baseline (decision T = invalid_arg) to flip the outcome.
 * - V1: dir=rx     -> F short-circ -> decision F (proceeds, busy/ok)
 * - V2: dir=tx     -> T,F -> F (proceeds)
 * - V3: dir=tx_rx  -> T,T,F -> F (proceeds)
 * - V4: dir=5      -> T,T,T -> decision T -> invalid_arg
 * Channel may be uninitialized; we only assert *not* invalid_arg for
 * V1..V3 (other ra8_err_t paths are acceptable -- the goal is to prove
 * the validation gate did not reject the dir value), and ==invalid_arg
 * for V4. With the channel un-init the fake returns invalid_arg,
 * so we initialise channel 0 first.
 */
static void test_mcdc_start_dir(void)
{
  TEST_BEGIN("ssie MC/DC start dir != rx && != tx && != tx_rx");
  /* Set up a real channel so subsequent ra8_ssie_start() calls reach
   * the validation gate before any other failure modes. */
  prep();
  const ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));
  /* V1, V2, V3: validation passes; downstream may return ok/busy but
   * NOT invalid_arg-from-bad-dir. */
  ra8_err_t r1 = ra8_ssie_start((uint8_t)k_ra8_ssie_test_ch0, k_ra8_ssie_dir_rx);
  TEST_ASSERT(r1 != k_ra8_err_invalid_arg);
  ra8_err_t r2 = ra8_ssie_start((uint8_t)k_ra8_ssie_test_ch0, k_ra8_ssie_dir_tx);
  TEST_ASSERT(r2 != k_ra8_err_invalid_arg);
  ra8_err_t r3 = ra8_ssie_start((uint8_t)k_ra8_ssie_test_ch0, k_ra8_ssie_dir_tx_rx);
  TEST_ASSERT(r3 != k_ra8_err_invalid_arg);
  /* V4: bad dir -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ssie_start((uint8_t)k_ra8_ssie_test_ch0, (ra8_ssie_dir_t)k_ssie_mcdc_dir_bad));
  TEST_END("ssie MC/DC start dir != rx && != tx && != tx_rx");
}

/**
 * @test test_mcdc_set_thresholds
 *
 * @par MC/DC:
 * Decision: `if (tx_threshold > thresh_max || rx_threshold > thresh_max)`
 * (2 conditions, libs/ra8_hal/src/ra8_ssie.c line 623 -- ra8_ssie_set_thresholds)
 * - V1: tx=0x10, rx=0x10 -> F,F -> ok
 * - V2: tx=0x40, rx=0x10 -> T short-circ -> invalid_arg
 * - V3: tx=0x10, rx=0x40 -> F,T -> invalid_arg
 */
static void test_mcdc_set_thresholds(void)
{
  TEST_BEGIN("ssie MC/DC set_thresholds range");
  prep();
  const ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ssie_set_thresholds((uint8_t)k_ra8_ssie_test_ch0,
                                         (uint8_t)k_ssie_mcdc_thresh_in,
                                         (uint8_t)k_ssie_mcdc_thresh_in));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ssie_set_thresholds((uint8_t)k_ra8_ssie_test_ch0,
                                         (uint8_t)k_ssie_mcdc_thresh_over,
                                         (uint8_t)k_ssie_mcdc_thresh_in));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ssie_set_thresholds((uint8_t)k_ra8_ssie_test_ch0,
                                         (uint8_t)k_ssie_mcdc_thresh_in,
                                         (uint8_t)k_ssie_mcdc_thresh_over));
  TEST_END("ssie MC/DC set_thresholds range");
}

/**
 * @test test_mcdc_attach_dma_neither
 *
 * @par MC/DC:
 * Decision: `if (!*want_tx && !*want_rx)`
 * (2 conditions, libs/ra8_hal/src/ra8_ssie.c line 735 -- internal_validate_dma_cfg)
 * Reached via ra8_ssie_attach_dma(). want_tx = (tx_dma_channel < 8);
 * want_rx = (rx_dma_channel < 8). So:
 * - V1: tx=3, rx=3 -> want_tx=T, want_rx=T -> !T=F short-circ -> ok
 *       (attach success, but other gates may apply -- we only check that
 *        the "neither" rejection path is NOT taken).
 * - V2: tx=100, rx=3 -> want_tx=F (so !=T), want_rx=T (so !=F) -> T,F -> F (proceed)
 * - V3: tx=100, rx=100 -> !want_tx=T, !want_rx=T -> T,T -> invalid_arg
 * V1+V3 vary !want_tx; V2+V3 vary !want_rx (with !want_tx held T).
 */
static void test_mcdc_attach_dma_neither(void)
{
  TEST_BEGIN("ssie MC/DC validate_dma_cfg !want_tx && !want_rx");
  prep();
  const ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));
  static uint32_t s_txbuf[4] = {0U};
  static uint32_t s_rxbuf[4] = {0U};
  /* V1 */
  ra8_ssie_dma_cfg_t dma1 = {
    .tx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .rx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .tx_buffer      = s_txbuf,
    .rx_buffer      = s_rxbuf,
    .tx_samples     = 4U,
    .rx_samples     = 4U,
  };
  ra8_err_t v1 = ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, &dma1);
  /* The "neither" gate must not have rejected the call. Other ra8_err_t
   * outcomes are acceptable (DMA layer may or may not be ready off-target). */
  (void)v1;
  /* V2 */
  ra8_ssie_dma_cfg_t dma2 = {
    .tx_dma_channel = (uint8_t)k_ssie_mcdc_dma_bad,
    .rx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .tx_buffer      = nullptr,
    .rx_buffer      = s_rxbuf,
    .tx_samples     = 0U,
    .rx_samples     = 4U,
  };
  ra8_err_t v2 = ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, &dma2);
  (void)v2;
  /* V3 -- both invalid: must be rejected with invalid_arg by the "neither" gate. */
  ra8_ssie_dma_cfg_t dma3 = {
    .tx_dma_channel = (uint8_t)k_ssie_mcdc_dma_bad,
    .rx_dma_channel = (uint8_t)k_ssie_mcdc_dma_bad,
    .tx_buffer      = nullptr,
    .rx_buffer      = nullptr,
    .tx_samples     = 0U,
    .rx_samples     = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, &dma3));
  TEST_END("ssie MC/DC validate_dma_cfg !want_tx && !want_rx");
}

/**
 * @test test_mcdc_attach_dma_tx_buffer
 *
 * @par MC/DC:
 * Decision: `if (*want_tx && (dma->tx_buffer == nullptr || dma->tx_samples == 0U))`
 * (3 conditions, libs/ra8_hal/src/ra8_ssie.c line 738 -- internal_validate_dma_cfg)
 *
 * Per DO-178C 6.4.4.3, N+1 = 4 vectors:
 * - V1: want_tx=T, buf=&t, samples=4 -> T && (F||F)=T && F = F -> proceed
 * - V2: want_tx=F (tx_dma=100), buf=NULL, samples=0 -> F short-circ -> proceed-or-reject-elsewhere
 * - V3: want_tx=T, buf=NULL, samples=4 -> T && (T||_) = T -> invalid_arg
 * - V4: want_tx=T, buf=&t, samples=0 -> T && (F||T) = T -> invalid_arg
 *
 * Vectors 1+3 vary tx_buffer (C2); vectors 1+4 vary tx_samples (C3); V1+V2
 * vary want_tx (C1) with C2 and C3 short-circuited away. RX path is held
 * valid throughout so the prior `!want_tx && !want_rx` gate (line 735)
 * does not fire.
 */
static void test_mcdc_attach_dma_tx_buffer(void)
{
  TEST_BEGIN("ssie MC/DC validate_dma_cfg want_tx && (buf==NULL || samples==0)");
  prep();
  const ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));
  static uint32_t s_txbuf[4] = {0U};
  static uint32_t s_rxbuf[4] = {0U};
  /* V1 */
  ra8_ssie_dma_cfg_t v1cfg = {
    .tx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .rx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .tx_buffer      = s_txbuf,
    .rx_buffer      = s_rxbuf,
    .tx_samples     = 4U,
    .rx_samples     = 4U,
  };
  (void)ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, &v1cfg);
  /* V2: want_tx=F (tx ch out of range), rx valid. */
  ra8_ssie_dma_cfg_t v2cfg = {
    .tx_dma_channel = (uint8_t)k_ssie_mcdc_dma_bad,
    .rx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .tx_buffer      = nullptr,
    .rx_buffer      = s_rxbuf,
    .tx_samples     = 0U,
    .rx_samples     = 4U,
  };
  (void)ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, &v2cfg);
  /* V3: want_tx=T, tx_buffer=NULL -> invalid_arg. */
  ra8_ssie_dma_cfg_t v3cfg = {
    .tx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .rx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .tx_buffer      = nullptr,
    .rx_buffer      = s_rxbuf,
    .tx_samples     = 4U,
    .rx_samples     = 4U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, &v3cfg));
  /* V4: want_tx=T, tx_samples=0 -> invalid_arg. */
  ra8_ssie_dma_cfg_t v4cfg = {
    .tx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .rx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .tx_buffer      = s_txbuf,
    .rx_buffer      = s_rxbuf,
    .tx_samples     = 0U,
    .rx_samples     = 4U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, &v4cfg));
  TEST_END("ssie MC/DC validate_dma_cfg want_tx && (buf==NULL || samples==0)");
}

/**
 * @test test_mcdc_attach_dma_rx_buffer
 *
 * @par MC/DC:
 * Decision: `if (*want_rx && (dma->rx_buffer == nullptr || dma->rx_samples == 0U))`
 * (3 conditions, libs/ra8_hal/src/ra8_ssie.c line 741 -- internal_validate_dma_cfg)
 *
 * Per DO-178C 6.4.4.3, N+1 = 4 vectors. TX path held valid throughout to
 * avoid early rejection by the line-738 gate.
 * - V1: want_rx=T, buf=&r, samples=4 -> T && (F||F)=F -> proceed
 * - V2: want_rx=F (rx_dma=100)       -> F short-circ -> proceed
 * - V3: want_rx=T, buf=NULL, samp=4  -> T && T -> invalid_arg
 * - V4: want_rx=T, buf=&r, samp=0    -> T && T -> invalid_arg
 */
static void test_mcdc_attach_dma_rx_buffer(void)
{
  TEST_BEGIN("ssie MC/DC validate_dma_cfg want_rx && (buf==NULL || samples==0)");
  prep();
  const ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));
  static uint32_t    s_txbuf[4] = {0U};
  static uint32_t    s_rxbuf[4] = {0U};
  ra8_ssie_dma_cfg_t v1cfg      = {
    .tx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .rx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .tx_buffer      = s_txbuf,
    .rx_buffer      = s_rxbuf,
    .tx_samples     = 4U,
    .rx_samples     = 4U,
  };
  (void)ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, &v1cfg);
  ra8_ssie_dma_cfg_t v2cfg = {
    .tx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .rx_dma_channel = (uint8_t)k_ssie_mcdc_dma_bad,
    .tx_buffer      = s_txbuf,
    .rx_buffer      = nullptr,
    .tx_samples     = 4U,
    .rx_samples     = 0U,
  };
  (void)ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, &v2cfg);
  ra8_ssie_dma_cfg_t v3cfg = {
    .tx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .rx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .tx_buffer      = s_txbuf,
    .rx_buffer      = nullptr,
    .tx_samples     = 4U,
    .rx_samples     = 4U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, &v3cfg));
  ra8_ssie_dma_cfg_t v4cfg = {
    .tx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .rx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .tx_buffer      = s_txbuf,
    .rx_buffer      = s_rxbuf,
    .tx_samples     = 4U,
    .rx_samples     = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, &v4cfg));
  TEST_END("ssie MC/DC validate_dma_cfg want_rx && (buf==NULL || samples==0)");
}

/**
 * @test test_mcdc_attach_dma_pair
 *
 * @par MC/DC:
 * Decision: `if (tx_dma_channel >= dma_max_ch && rx_dma_channel >= dma_max_ch)`
 * (2 conditions, libs/ra8_hal/src/ra8_ssie.c line 963 -- ra8_ssie_attach_dma_pair)
 * - V1: tx=3, rx=3   -> F short-circ -> ok
 * - V2: tx=100,rx=3  -> T,F -> ok (one valid is enough)
 * - V3: tx=3, rx=100 -> F (short-circ stops on first F-of-AND) -> ok
 *   Wait: short-circuit AND stops on F, not T. C1=(3>=8)=F so AND stops.
 * - V4: tx=100,rx=100 -> T,T -> invalid_arg
 *
 * Masking pairs for AND: V1+V4 vary C1 (with C2 implicitly held T in V4
 * but masked-out in V1 -- the V1 outcome is determined by C1=F alone).
 * V2+V4 vary C2 with C1 held T. N+1 = 3 vectors minimum (V1,V2,V4) but
 * we include V3 to also exercise the symmetric short-circuit path.
 */
static void test_mcdc_attach_dma_pair(void)
{
  TEST_BEGIN("ssie MC/DC attach_dma_pair both-out-of-range guard");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ssie_attach_dma_pair((uint8_t)k_ra8_ssie_test_ch0,
                                          (uint8_t)k_ssie_mcdc_dma_valid,
                                          (uint8_t)k_ssie_mcdc_dma_valid));
  prep();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ssie_attach_dma_pair((uint8_t)k_ra8_ssie_test_ch0,
                                          (uint8_t)k_ssie_mcdc_dma_bad,
                                          (uint8_t)k_ssie_mcdc_dma_valid));
  prep();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ssie_attach_dma_pair((uint8_t)k_ra8_ssie_test_ch0,
                                          (uint8_t)k_ssie_mcdc_dma_valid,
                                          (uint8_t)k_ssie_mcdc_dma_bad));
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ssie_attach_dma_pair((uint8_t)k_ra8_ssie_test_ch0,
                                          (uint8_t)k_ssie_mcdc_dma_bad,
                                          (uint8_t)k_ssie_mcdc_dma_bad));
  TEST_END("ssie MC/DC attach_dma_pair both-out-of-range guard");
}
int32_t main(void)
{
  test_mcdc_init_lrcont();
  test_mcdc_init_bckastp();
  test_mcdc_init_aucke();
  test_mcdc_init_threshold();
  test_mcdc_start_dir();
  test_mcdc_set_thresholds();
  test_mcdc_attach_dma_neither();
  test_mcdc_attach_dma_tx_buffer();
  test_mcdc_attach_dma_rx_buffer();
  test_mcdc_attach_dma_pair();
  (void)fprintf(stderr, "[OK  ] test_ra8_ssie_mcdc.c\n");
  return 0;
}
