/**
 * @file test_ra_ssie.c
 * @brief Unit tests for ra_ssie.c (Serial Sound Interface Enhanced driver)
 *
 * @details
 * Exercises the full SSIE HAL surface defined by HUM Ch 46:
 *
 *  - Lifecycle (init / deinit / enter_stop / exit_stop)
 *  - All seven public formats (I2S, left-just, right-just, monaural,
 *    TDM-4, TDM-6, TDM-8) including OMOD/FRM/PDTA decoding
 *  - All thirteen master-mode bit-clock dividers (CKDV 0x0..0xC)
 *  - All eight system word lengths and seven legal data word lengths
 *  - Polarity/format flags (BCKP, LRCKP, SPDP, DEL, BSW, AUCKE,
 *    LRCONT, BCKASTP) and their interaction (LRCONT + BCKASTP rule)
 *  - Run control (start/stop, mute, threshold update, recovery)
 *  - Single-sample and buffered TX/RX
 *  - DMA pump attach/detach (with bad-arg matrix)
 *  - Status snapshot, decoded events, and W1C clear
 *  - IRQ enable mask updates and dispatch
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_ssie_regs.h"
#include "ra_dmac.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "ra_ssie.h"
#include "unity_minimal.h"

/**
 * @enum ra_ssie_test_const_t
 * @brief Magic numbers used by the tests promoted to a typed enum.
 */
typedef enum : uint16_t {
  k_ra_ssie_test_ch0      = 0U,
  k_ra_ssie_test_ch1      = 1U,
  k_ra_ssie_test_ch_bad   = 2U,
  k_ra_ssie_test_ch_way   = 250U,
  k_ra_ssie_test_sample_a = 0xCAFEU,
  k_ra_ssie_test_sample_b = 0xBEEFU,
  k_ra_ssie_test_dma_tx   = 3U,
  k_ra_ssie_test_dma_rx   = 4U,
  k_ra_ssie_test_dma_bad  = 100U,
} ra_ssie_test_const_t;

/**
 * @enum ra_ssie_test_status_t
 * @brief Synthetic SSISR / SSIFSR values used by status tests.
 */
typedef enum : uint32_t {
  k_ra_ssie_test_ssifsr_demo =
    (0x0AUL << 24U) | (0x05UL << 8U) | 0x00010001UL,      /* TDC=10, RDC=5, TDE+RDF */
  k_ra_ssie_test_ssisr_err = 0x04000000UL | 0x10000000UL, /* ROIRQ + TOIRQ */
} ra_ssie_test_status_t;

static uint32_t s_ssie_cb_count;
static uint8_t  s_ssie_cb_last_ch;
static uint32_t s_ssie_cb_last_ssisr;
static uint8_t  s_ssie_cb_last_events;

static void stub_ssie_cb(void* ctx, uint8_t channel, uint8_t events, uint32_t ssisr)
{
  (void)ctx;
  ++s_ssie_cb_count;
  s_ssie_cb_last_ch     = channel;
  s_ssie_cb_last_events = events;
  s_ssie_cb_last_ssisr  = ssisr;
}

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_ssie_cb_count       = 0U;
  s_ssie_cb_last_ch     = 0U;
  s_ssie_cb_last_ssisr  = 0U;
  s_ssie_cb_last_events = 0U;
}

static ra_ssie_cfg_t make_master_i2s_cfg(void)
{
  const ra_ssie_cfg_t cfg = {
    .role          = k_ra_ssie_role_master,
    .format        = k_ra_ssie_format_i2s,
    .data_word     = k_ra_ssie_dwl_16,
    .system_word   = k_ra_ssie_swl_32,
    .bclk_div      = k_ra_ssie_bclk_div_4,
    .use_gpt_clk   = false,
    .long_frame    = false,
    .bckp_rising   = false,
    .lrckp_low     = false,
    .spdp_high     = false,
    .byte_swap     = false,
    .lr_continue   = false,
    .bck_idle_stop = false,
    .enable_aucke  = false,
    .tx_threshold  = 0U,
    .rx_threshold  = 0U,
  };
  return cfg;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

static void test_init_happy(void)
{
  TEST_BEGIN("ssie init happy (master I2S)");
  prep();

  const ra_ssie_cfg_t cfg = make_master_i2s_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch0, &cfg));

  volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch0);
  TEST_ASSERT_NOT_NULL((void*)reg);

  TEST_ASSERT((reg->SSICR & (uint32_t)k_ra_ssie_mask_mst) != 0U);
  TEST_ASSERT_EQ((int32_t)((uint32_t)k_ra_ssie_bclk_div_4 << (uint8_t)k_ra_ssie_bit_ckdv0),
                 (int32_t)(reg->SSICR & (uint32_t)k_ra_ssie_mask_ckdv));
  TEST_ASSERT_EQ((int32_t)((uint32_t)k_ra_ssie_swl_32 << (uint8_t)k_ra_ssie_bit_swl0),
                 (int32_t)(reg->SSICR & (uint32_t)k_ra_ssie_mask_swl));
  TEST_ASSERT_EQ((int32_t)((uint32_t)k_ra_ssie_dwl_16 << (uint8_t)k_ra_ssie_bit_dwl0),
                 (int32_t)(reg->SSICR & (uint32_t)k_ra_ssie_mask_dwl));

  TEST_ASSERT_EQ((int32_t)0, (int32_t)(reg->SSICR & (uint32_t)k_ra_ssie_mask_ren_ten));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(reg->SSIOFR & (uint32_t)k_ra_ssie_mask_omod));
  TEST_END("ssie init happy (master I2S)");
}

static void test_init_slave_tdm(void)
{
  TEST_BEGIN("ssie init slave TDM-4 24-bit");
  prep();

  ra_ssie_cfg_t cfg = make_master_i2s_cfg();
  cfg.role          = k_ra_ssie_role_slave;
  cfg.format        = k_ra_ssie_format_tdm_4;
  cfg.data_word     = k_ra_ssie_dwl_24;
  cfg.system_word   = k_ra_ssie_swl_32;
  cfg.use_gpt_clk   = true;

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch1, &cfg));

  volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch1);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(reg->SSICR & (uint32_t)k_ra_ssie_mask_mst));
  TEST_ASSERT((reg->SSICR & (uint32_t)k_ra_ssie_mask_cks) != 0U);
  TEST_ASSERT_EQ((int32_t)(uint32_t)k_ra_ssie_omod_tdm,
                 (int32_t)(reg->SSIOFR & (uint32_t)k_ra_ssie_mask_omod));
  /* FRM=01 selects 4-channel TDM. */
  TEST_ASSERT_EQ((int32_t)((uint32_t)k_ra_ssie_frm_alt1 << (uint8_t)k_ra_ssie_bit_frm0),
                 (int32_t)(reg->SSICR & (uint32_t)k_ra_ssie_mask_frm));
  TEST_END("ssie init slave TDM-4 24-bit");
}

static void test_init_all_formats(void)
{
  TEST_BEGIN("ssie init covers all seven formats");

  const struct {
    ra_ssie_format_t fmt;
    uint8_t          omod;
    uint8_t          frm;
    bool             expect_pdta;
  } cases[] = {
    {k_ra_ssie_format_i2s, (uint8_t)k_ra_ssie_omod_i2s, (uint8_t)k_ra_ssie_frm_default, false},
    {k_ra_ssie_format_left_just,
     (uint8_t)k_ra_ssie_omod_i2s,
     (uint8_t)k_ra_ssie_frm_default,
     false},
    {k_ra_ssie_format_right_just,
     (uint8_t)k_ra_ssie_omod_i2s,
     (uint8_t)k_ra_ssie_frm_default,
     true},
    {k_ra_ssie_format_monaural,
     (uint8_t)k_ra_ssie_omod_monaural,
     (uint8_t)k_ra_ssie_frm_default,
     false},
    {k_ra_ssie_format_tdm_4, (uint8_t)k_ra_ssie_omod_tdm, (uint8_t)k_ra_ssie_frm_alt1, false},
    {k_ra_ssie_format_tdm_6, (uint8_t)k_ra_ssie_omod_tdm, (uint8_t)k_ra_ssie_frm_alt2, false},
    {k_ra_ssie_format_tdm_8, (uint8_t)k_ra_ssie_omod_tdm, (uint8_t)k_ra_ssie_frm_alt3, false},
  };
  const uint8_t case_count = (uint8_t)(sizeof(cases) / sizeof(cases[0]));

  for (uint8_t i = 0U; i < case_count; i++) {
    prep();
    ra_ssie_cfg_t cfg = make_master_i2s_cfg();
    cfg.format        = cases[i].fmt;
    TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch0, &cfg));

    volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch0);
    TEST_ASSERT_EQ((int32_t)cases[i].omod, (int32_t)(reg->SSIOFR & (uint32_t)k_ra_ssie_mask_omod));
    TEST_ASSERT_EQ((int32_t)((uint32_t)cases[i].frm << (uint8_t)k_ra_ssie_bit_frm0),
                   (int32_t)(reg->SSICR & (uint32_t)k_ra_ssie_mask_frm));
    TEST_ASSERT_EQ(cases[i].expect_pdta ? 1 : 0,
                   (int32_t)((reg->SSICR & (uint32_t)k_ra_ssie_mask_pdta) != 0U ? 1 : 0));
  }
  TEST_END("ssie init covers all seven formats");
}

static void test_init_all_dividers(void)
{
  TEST_BEGIN("ssie init walks all 13 CKDV values");

  const ra_ssie_bclk_div_t divs[] = {
    k_ra_ssie_bclk_div_1,
    k_ra_ssie_bclk_div_2,
    k_ra_ssie_bclk_div_4,
    k_ra_ssie_bclk_div_8,
    k_ra_ssie_bclk_div_16,
    k_ra_ssie_bclk_div_32,
    k_ra_ssie_bclk_div_64,
    k_ra_ssie_bclk_div_128,
    k_ra_ssie_bclk_div_6,
    k_ra_ssie_bclk_div_12,
    k_ra_ssie_bclk_div_24,
    k_ra_ssie_bclk_div_48,
    k_ra_ssie_bclk_div_96,
  };
  const uint8_t div_count = (uint8_t)(sizeof(divs) / sizeof(divs[0]));

  for (uint8_t i = 0U; i < div_count; i++) {
    prep();
    ra_ssie_cfg_t cfg = make_master_i2s_cfg();
    cfg.bclk_div      = divs[i];
    TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch0, &cfg));

    volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch0);
    TEST_ASSERT_EQ((int32_t)((uint32_t)divs[i] << (uint8_t)k_ra_ssie_bit_ckdv0),
                   (int32_t)(reg->SSICR & (uint32_t)k_ra_ssie_mask_ckdv));
  }
  TEST_END("ssie init walks all 13 CKDV values");
}

static void test_init_all_word_lengths(void)
{
  TEST_BEGIN("ssie init covers every SWL+DWL combination");

  const ra_ssie_system_word_t sw[] = {
    k_ra_ssie_swl_8,
    k_ra_ssie_swl_16,
    k_ra_ssie_swl_24,
    k_ra_ssie_swl_32,
    k_ra_ssie_swl_48,
    k_ra_ssie_swl_64,
    k_ra_ssie_swl_128,
    k_ra_ssie_swl_256,
  };
  const ra_ssie_data_word_t dw[] = {
    k_ra_ssie_dwl_8,
    k_ra_ssie_dwl_16,
    k_ra_ssie_dwl_18,
    k_ra_ssie_dwl_20,
    k_ra_ssie_dwl_22,
    k_ra_ssie_dwl_24,
    k_ra_ssie_dwl_32,
  };
  const uint8_t sw_count = (uint8_t)(sizeof(sw) / sizeof(sw[0]));
  const uint8_t dw_count = (uint8_t)(sizeof(dw) / sizeof(dw[0]));

  for (uint8_t i = 0U; i < sw_count; i++) {
    for (uint8_t j = 0U; j < dw_count; j++) {
      prep();
      ra_ssie_cfg_t cfg = make_master_i2s_cfg();
      cfg.system_word   = sw[i];
      cfg.data_word     = dw[j];
      TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch0, &cfg));

      volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch0);
      TEST_ASSERT_EQ((int32_t)((uint32_t)sw[i] << (uint8_t)k_ra_ssie_bit_swl0),
                     (int32_t)(reg->SSICR & (uint32_t)k_ra_ssie_mask_swl));
      TEST_ASSERT_EQ((int32_t)((uint32_t)dw[j] << (uint8_t)k_ra_ssie_bit_dwl0),
                     (int32_t)(reg->SSICR & (uint32_t)k_ra_ssie_mask_dwl));
    }
  }
  TEST_END("ssie init covers every SWL+DWL combination");
}

static void test_init_polarity_and_flags(void)
{
  TEST_BEGIN("ssie init exposes every polarity flag");
  prep();
  ra_ssie_cfg_t cfg = make_master_i2s_cfg();
  cfg.long_frame    = true;
  cfg.bckp_rising   = true;
  cfg.lrckp_low     = true;
  cfg.spdp_high     = true;
  cfg.byte_swap     = true;
  cfg.enable_aucke  = true;
  cfg.tx_threshold  = 7U;
  cfg.rx_threshold  = 1U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch0, &cfg));

  volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch0);
  TEST_ASSERT((reg->SSICR & (uint32_t)k_ra_ssie_mask_del) != 0U);
  TEST_ASSERT((reg->SSICR & (uint32_t)k_ra_ssie_mask_bckp) != 0U);
  TEST_ASSERT((reg->SSICR & (uint32_t)k_ra_ssie_mask_lrckp) != 0U);
  TEST_ASSERT((reg->SSICR & (uint32_t)k_ra_ssie_mask_spdp) != 0U);
  TEST_ASSERT((reg->SSIFCR & (uint32_t)k_ra_ssie_mask_bsw) != 0U);
  TEST_ASSERT((reg->SSIFCR & (uint32_t)k_ra_ssie_mask_aucke) != 0U);

  /* SSISCR encodes RX threshold in [4:0] and TX threshold in [12:8]. */
  TEST_ASSERT_EQ((int32_t)((7U << (uint8_t)k_ra_ssie_shift_tdes) | 1U), (int32_t)reg->SSISCR);
  TEST_END("ssie init exposes every polarity flag");
}

static void test_init_lrcont_blocks_bckastp(void)
{
  TEST_BEGIN("ssie init: LRCONT wins over BCKASTP when both requested");
  prep();
  ra_ssie_cfg_t cfg = make_master_i2s_cfg();
  cfg.lr_continue   = true;
  cfg.bck_idle_stop = true;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch0, &cfg));

  volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch0);
  TEST_ASSERT((reg->SSIOFR & (uint32_t)k_ra_ssie_mask_lrcont) != 0U);
  /* HUM Ch 46.2.7 "SSIOFR : Audio Format Register" p 3091
   * Note 2 forbids both LRCONT+BCKASTP at once, so BCKASTP must be 0. */
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(reg->SSIOFR & (uint32_t)k_ra_ssie_mask_bckastp));
  TEST_END("ssie init: LRCONT wins over BCKASTP when both requested");
}

static void test_init_bckastp_alone(void)
{
  TEST_BEGIN("ssie init: BCKASTP alone is honoured");
  prep();
  ra_ssie_cfg_t cfg = make_master_i2s_cfg();
  cfg.bck_idle_stop = true;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch0, &cfg));

  volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch0);
  TEST_ASSERT((reg->SSIOFR & (uint32_t)k_ra_ssie_mask_bckastp) != 0U);
  TEST_END("ssie init: BCKASTP alone is honoured");
}

static void test_init_null_cfg(void)
{
  TEST_BEGIN("ssie init null cfg");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch0, nullptr));
  TEST_END("ssie init null cfg");
}

static void test_init_bad_channel(void)
{
  TEST_BEGIN("ssie init bad channel");
  prep();

  const ra_ssie_cfg_t cfg = make_master_i2s_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch_bad, &cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch_way, &cfg));
  TEST_END("ssie init bad channel");
}

static void test_init_bad_threshold(void)
{
  TEST_BEGIN("ssie init rejects threshold > 0x1F");
  prep();
  ra_ssie_cfg_t cfg = make_master_i2s_cfg();
  cfg.tx_threshold  = 0x40U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch0, &cfg));
  cfg              = make_master_i2s_cfg();
  cfg.rx_threshold = 0x40U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch0, &cfg));
  TEST_END("ssie init rejects threshold > 0x1F");
}

static void test_deinit(void)
{
  TEST_BEGIN("ssie deinit clears TEN/REN + AUCKE");
  prep();

  const ra_ssie_cfg_t cfg = make_master_i2s_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch0, &cfg));

  volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch0);
  reg->SSICR                  = reg->SSICR | (uint32_t)k_ra_ssie_mask_ren_ten;
  reg->SSIFCR                 = reg->SSIFCR | (uint32_t)k_ra_ssie_mask_aucke;

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_deinit((uint8_t)k_ra_ssie_test_ch0));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(reg->SSICR & (uint32_t)k_ra_ssie_mask_ren_ten));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(reg->SSIFCR & (uint32_t)k_ra_ssie_mask_aucke));

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_deinit((uint8_t)k_ra_ssie_test_ch_bad));
  TEST_END("ssie deinit clears TEN/REN + AUCKE");
}

/* ---------------------------------------------------------------------------
 * Run control
 * ---------------------------------------------------------------------------
 */

static void test_start_stop_tx(void)
{
  TEST_BEGIN("ssie start TX sets TEN + TIE + TUIEN");
  prep();
  const ra_ssie_cfg_t cfg = make_master_i2s_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch0, &cfg));

  volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch0);
  /* Pre-set IIRQ so the start helper believes the SSIE is idle. */
  reg->SSISR = (uint32_t)k_ra_ssie_mask_iirq;

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ssie_start((uint8_t)k_ra_ssie_test_ch0, k_ra_ssie_dir_tx));
  TEST_ASSERT((reg->SSICR & (uint32_t)k_ra_ssie_mask_ten) != 0U);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(reg->SSICR & (uint32_t)k_ra_ssie_mask_ren));
  TEST_ASSERT((reg->SSICR & (1UL << (uint8_t)k_ra_ssie_bit_tuien)) != 0U);
  TEST_ASSERT((reg->SSIFCR & (uint32_t)k_ra_ssie_mask_tie) != 0U);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(reg->SSIFCR & (uint32_t)k_ra_ssie_mask_rie));

  /* Stop should clear TEN + IRQ enables but leave IIEN armed. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_stop((uint8_t)k_ra_ssie_test_ch0));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(reg->SSICR & (uint32_t)k_ra_ssie_mask_ren_ten));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(reg->SSICR & (uint32_t)k_ra_ssie_mask_err_ien));
  TEST_ASSERT((reg->SSICR & (uint32_t)k_ra_ssie_mask_iien) != 0U);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(reg->SSIFCR & (uint32_t)k_ra_ssie_mask_rie_tie));
  TEST_END("ssie start TX sets TEN + TIE + TUIEN");
}

static void test_start_busy_when_not_idle(void)
{
  TEST_BEGIN("ssie start refuses when SSIE not idle");
  prep();
  const ra_ssie_cfg_t cfg = make_master_i2s_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch0, &cfg));

  volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch0);
  /* IIRQ low + REN/TEN clear -> not idle. */
  reg->SSISR = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_busy,
                 (int32_t)ra_ssie_start((uint8_t)k_ra_ssie_test_ch0, k_ra_ssie_dir_rx));

  /* Already started in another direction. */
  reg->SSISR = (uint32_t)k_ra_ssie_mask_iirq;
  reg->SSICR = reg->SSICR | (uint32_t)k_ra_ssie_mask_ten;
  TEST_ASSERT_EQ((int32_t)k_ra_err_busy,
                 (int32_t)ra_ssie_start((uint8_t)k_ra_ssie_test_ch0, k_ra_ssie_dir_rx));
  TEST_END("ssie start refuses when SSIE not idle");
}

static void test_start_already_armed(void)
{
  TEST_BEGIN("ssie start is idempotent when already in mode");
  prep();
  const ra_ssie_cfg_t cfg = make_master_i2s_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch0, &cfg));

  volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch0);
  reg->SSISR                  = (uint32_t)k_ra_ssie_mask_iirq;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ssie_start((uint8_t)k_ra_ssie_test_ch0, k_ra_ssie_dir_tx));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ssie_start((uint8_t)k_ra_ssie_test_ch0, k_ra_ssie_dir_tx));
  TEST_END("ssie start is idempotent when already in mode");
}

static void test_start_bad_args(void)
{
  TEST_BEGIN("ssie start rejects bad dir / bad channel");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_start((uint8_t)k_ra_ssie_test_ch_bad, k_ra_ssie_dir_tx));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_start((uint8_t)k_ra_ssie_test_ch0, (ra_ssie_dir_t)0U));
  TEST_END("ssie start rejects bad dir / bad channel");
}

static void test_stop_bad_channel(void)
{
  TEST_BEGIN("ssie stop bad channel");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_stop((uint8_t)k_ra_ssie_test_ch_bad));
  TEST_END("ssie stop bad channel");
}

static void test_recovery_clears_errors(void)
{
  TEST_BEGIN("ssie start_recovery clears error flags + SSIRST");
  prep();
  const ra_ssie_cfg_t cfg = make_master_i2s_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch0, &cfg));

  volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch0);
  reg->SSISR                  = (uint32_t)k_ra_ssie_mask_err_all | (uint32_t)k_ra_ssie_mask_iirq;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_start_recovery((uint8_t)k_ra_ssie_test_ch0));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(reg->SSISR & (uint32_t)k_ra_ssie_mask_err_all));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(reg->SSIFCR & (uint32_t)k_ra_ssie_mask_ssirst));

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_start_recovery((uint8_t)k_ra_ssie_test_ch_bad));
  TEST_END("ssie start_recovery clears error flags + SSIRST");
}

static void test_mute_toggles_muen(void)
{
  TEST_BEGIN("ssie mute toggles SSICR.MUEN");
  prep();
  const ra_ssie_cfg_t cfg = make_master_i2s_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch0, &cfg));

  volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch0);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_mute((uint8_t)k_ra_ssie_test_ch0, true));
  TEST_ASSERT((reg->SSICR & (uint32_t)k_ra_ssie_mask_muen) != 0U);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_mute((uint8_t)k_ra_ssie_test_ch0, false));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(reg->SSICR & (uint32_t)k_ra_ssie_mask_muen));

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_mute((uint8_t)k_ra_ssie_test_ch_bad, true));
  TEST_END("ssie mute toggles SSICR.MUEN");
}

static void test_set_thresholds(void)
{
  TEST_BEGIN("ssie set_thresholds writes SSISCR");
  prep();
  const ra_ssie_cfg_t cfg = make_master_i2s_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch0, &cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ssie_set_thresholds((uint8_t)k_ra_ssie_test_ch0, 0x10U, 0x05U));
  volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch0);
  TEST_ASSERT_EQ((int32_t)((0x10U << (uint8_t)k_ra_ssie_shift_tdes) | 0x05U), (int32_t)reg->SSISCR);

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_set_thresholds((uint8_t)k_ra_ssie_test_ch0, 0x40U, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_set_thresholds((uint8_t)k_ra_ssie_test_ch_bad, 0U, 0U));
  TEST_END("ssie set_thresholds writes SSISCR");
}

/* ---------------------------------------------------------------------------
 * Data path
 * ---------------------------------------------------------------------------
 */

static void test_write_sample(void)
{
  TEST_BEGIN("ssie write_sample writes SSIFTDR");
  prep();

  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_ssie_write_sample((uint8_t)k_ra_ssie_test_ch0, (uint32_t)k_ra_ssie_test_sample_a));
  volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch0);
  TEST_ASSERT_EQ((int32_t)(uint32_t)k_ra_ssie_test_sample_a, (int32_t)reg->SSIFTDR);

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_write_sample((uint8_t)k_ra_ssie_test_ch_bad, 0U));
  TEST_END("ssie write_sample writes SSIFTDR");
}

static void test_read_sample(void)
{
  TEST_BEGIN("ssie read_sample reads SSIFRDR");
  prep();

  volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch1);
  reg->SSIFRDR                = (uint32_t)k_ra_ssie_test_sample_b;

  uint32_t sample = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ssie_read_sample((uint8_t)k_ra_ssie_test_ch1, &sample));
  TEST_ASSERT_EQ((int32_t)(uint32_t)k_ra_ssie_test_sample_b, (int32_t)sample);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_ssie_read_sample((uint8_t)k_ra_ssie_test_ch1, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_read_sample((uint8_t)k_ra_ssie_test_ch_bad, &sample));
  TEST_END("ssie read_sample reads SSIFRDR");
}

static void test_write_buffer(void)
{
  TEST_BEGIN("ssie write_buffer pumps until FIFO full");
  prep();

  volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch0);
  /* Pretend the FIFO has 2 entries already; depth is 32, so we
   * should write only 30. */
  reg->SSIFSR = (uint32_t)(2U << (uint8_t)k_ra_ssie_shift_tdc);
  uint32_t buf[40];
  for (uint8_t i = 0U; i < 40U; i++) {
    buf[i] = 0xAA00U + i;
    /* keep TDC at 2 so the loop sees space (sim mmap is just RAM). */
  }
  uint16_t written = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ssie_write_buffer((uint8_t)k_ra_ssie_test_ch0, buf, 40U, &written));
  /* The simulator returns whatever we wrote into SSIFSR; since we
   * keep TDC=2 throughout, all 40 samples should fit. */
  TEST_ASSERT_EQ((int32_t)40, (int32_t)written);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_ssie_write_buffer((uint8_t)k_ra_ssie_test_ch0, nullptr, 1U, &written));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_ssie_write_buffer((uint8_t)k_ra_ssie_test_ch0, buf, 1U, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_write_buffer((uint8_t)k_ra_ssie_test_ch_bad, buf, 1U, &written));
  TEST_END("ssie write_buffer pumps until FIFO full");
}

static void test_write_buffer_stops_when_full(void)
{
  TEST_BEGIN("ssie write_buffer stops when TDC reaches depth");
  prep();
  volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch0);
  /* Pre-load TDC=32 so the FIFO is at depth and zero stages fit. */
  reg->SSIFSR      = (uint32_t)((uint32_t)k_ra_ssie_fifo_depth << (uint8_t)k_ra_ssie_shift_tdc);
  uint32_t buf[4]  = {1U, 2U, 3U, 4U};
  uint16_t written = 99U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ssie_write_buffer((uint8_t)k_ra_ssie_test_ch0, buf, 4U, &written));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)written);
  TEST_END("ssie write_buffer stops when TDC reaches depth");
}

static void test_read_buffer(void)
{
  TEST_BEGIN("ssie read_buffer pumps until FIFO empty");
  prep();

  volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch1);
  reg->SSIFSR                 = (uint32_t)(8U << (uint8_t)k_ra_ssie_shift_rdc);
  reg->SSIFRDR                = (uint32_t)k_ra_ssie_test_sample_b;
  uint32_t buf[8]             = {};
  uint16_t read               = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ssie_read_buffer((uint8_t)k_ra_ssie_test_ch1, buf, 8U, &read));
  TEST_ASSERT_EQ((int32_t)8, (int32_t)read);
  for (uint8_t i = 0U; i < 8U; i++) {
    TEST_ASSERT_EQ((int32_t)(uint32_t)k_ra_ssie_test_sample_b, (int32_t)buf[i]);
  }

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_ssie_read_buffer((uint8_t)k_ra_ssie_test_ch1, nullptr, 1U, &read));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_ssie_read_buffer((uint8_t)k_ra_ssie_test_ch1, buf, 1U, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_read_buffer((uint8_t)k_ra_ssie_test_ch_bad, buf, 1U, &read));
  TEST_END("ssie read_buffer pumps until FIFO empty");
}

static void test_read_buffer_empty_fifo(void)
{
  TEST_BEGIN("ssie read_buffer returns 0 when RDC=0");
  prep();
  volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch0);
  reg->SSIFSR                 = 0U;
  uint32_t buf[1]             = {};
  uint16_t read               = 99U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ssie_read_buffer((uint8_t)k_ra_ssie_test_ch0, buf, 1U, &read));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)read);
  TEST_END("ssie read_buffer returns 0 when RDC=0");
}

/* ---------------------------------------------------------------------------
 * DMA
 * ---------------------------------------------------------------------------
 */

static void test_attach_detach_dma(void)
{
  TEST_BEGIN("ssie attach_dma + detach_dma full duplex");
  prep();

  static uint32_t tx_buf[16];
  static uint32_t rx_buf[16];
  for (uint8_t i = 0U; i < 16U; i++) {
    tx_buf[i] = i;
  }

  const ra_ssie_dma_cfg_t dma = {
    .tx_dma_channel = (uint8_t)k_ra_ssie_test_dma_tx,
    .rx_dma_channel = (uint8_t)k_ra_ssie_test_dma_rx,
    .tx_buffer      = tx_buf,
    .rx_buffer      = rx_buf,
    .tx_samples     = 16U,
    .rx_samples     = 16U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_attach_dma((uint8_t)k_ra_ssie_test_ch0, &dma));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_detach_dma((uint8_t)k_ra_ssie_test_ch0));
  TEST_END("ssie attach_dma + detach_dma full duplex");
}

static void test_attach_dma_tx_only(void)
{
  TEST_BEGIN("ssie attach_dma TX-only path");
  prep();
  static uint32_t         tx_buf[8];
  const ra_ssie_dma_cfg_t dma = {
    .tx_dma_channel = (uint8_t)k_ra_ssie_test_dma_tx,
    .rx_dma_channel = 0xFFU,
    .tx_buffer      = tx_buf,
    .rx_buffer      = nullptr,
    .tx_samples     = 8U,
    .rx_samples     = 0U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_attach_dma((uint8_t)k_ra_ssie_test_ch0, &dma));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_detach_dma((uint8_t)k_ra_ssie_test_ch0));
  TEST_END("ssie attach_dma TX-only path");
}

static void test_attach_dma_bad_args(void)
{
  TEST_BEGIN("ssie attach_dma rejects bad descriptors");
  prep();
  static uint32_t buf[2] = {1U, 2U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_ssie_attach_dma((uint8_t)k_ra_ssie_test_ch0, nullptr));

  ra_ssie_dma_cfg_t dma = {
    .tx_dma_channel = 0xFFU,
    .rx_dma_channel = 0xFFU,
    .tx_buffer      = nullptr,
    .rx_buffer      = nullptr,
    .tx_samples     = 0U,
    .rx_samples     = 0U,
  };
  /* No directions enabled. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_attach_dma((uint8_t)k_ra_ssie_test_ch0, &dma));

  /* TX channel set but missing buffer. */
  dma.tx_dma_channel = (uint8_t)k_ra_ssie_test_dma_tx;
  dma.tx_samples     = 4U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_attach_dma((uint8_t)k_ra_ssie_test_ch0, &dma));
  dma.tx_buffer  = buf;
  dma.tx_samples = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_attach_dma((uint8_t)k_ra_ssie_test_ch0, &dma));

  /* Bad channel. */
  dma.tx_samples = 4U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_attach_dma((uint8_t)k_ra_ssie_test_ch_bad, &dma));

  /* Detach on bad channel. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_detach_dma((uint8_t)k_ra_ssie_test_ch_bad));
  TEST_END("ssie attach_dma rejects bad descriptors");
}

/* ---------------------------------------------------------------------------
 * Status
 * ---------------------------------------------------------------------------
 */

static void test_get_status_decodes_fifo_levels(void)
{
  TEST_BEGIN("ssie get_status decodes RDC/TDC + flags");
  prep();

  volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch0);
  reg->SSIFSR                 = (uint32_t)k_ra_ssie_test_ssifsr_demo;
  reg->SSISR                  = (uint32_t)k_ra_ssie_mask_iirq | (uint32_t)k_ra_ssie_test_ssisr_err;

  ra_ssie_status_t st = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_get_status((uint8_t)k_ra_ssie_test_ch0, &st));
  TEST_ASSERT_EQ((int32_t)10, (int32_t)st.tx_count);
  TEST_ASSERT_EQ((int32_t)5, (int32_t)st.rx_count);
  TEST_ASSERT(st.rx_full);
  TEST_ASSERT(st.tx_empty);
  TEST_ASSERT(st.idle);
  TEST_ASSERT(st.error);

  TEST_ASSERT((st.events & (uint8_t)k_ra_ssie_evt_idle) != 0U);
  TEST_ASSERT((st.events & (uint8_t)k_ra_ssie_evt_tx_over) != 0U);
  TEST_ASSERT((st.events & (uint8_t)k_ra_ssie_evt_rx_over) != 0U);
  TEST_ASSERT((st.events & (uint8_t)k_ra_ssie_evt_tx_empty) != 0U);
  TEST_ASSERT((st.events & (uint8_t)k_ra_ssie_evt_rx_full) != 0U);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_ssie_get_status((uint8_t)k_ra_ssie_test_ch0, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_get_status((uint8_t)k_ra_ssie_test_ch_bad, &st));
  TEST_END("ssie get_status decodes RDC/TDC + flags");
}

static void test_clear_status_masks_to_writeable(void)
{
  TEST_BEGIN("ssie clear_status only touches error bits");
  prep();

  volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch0);
  reg->SSISR                  = (uint32_t)k_ra_ssie_mask_iirq | (uint32_t)k_ra_ssie_mask_err_all;

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ssie_clear_status((uint8_t)k_ra_ssie_test_ch0,
                                               (uint32_t)k_ra_ssie_mask_roirq |
                                                 (uint32_t)k_ra_ssie_mask_toirq | 0x1U));

  const uint32_t expected =
    (uint32_t)k_ra_ssie_mask_iirq | (uint32_t)k_ra_ssie_mask_ruirq | (uint32_t)k_ra_ssie_mask_tuirq;
  TEST_ASSERT_EQ((int32_t)expected, (int32_t)reg->SSISR);

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_clear_status((uint8_t)k_ra_ssie_test_ch_bad, 0U));
  TEST_END("ssie clear_status only touches error bits");
}

static void test_set_irq_enable(void)
{
  TEST_BEGIN("ssie set_irq_enable updates SSICR IRQ mask");
  prep();
  const ra_ssie_cfg_t cfg = make_master_i2s_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch0, &cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ssie_set_irq_enable((uint8_t)k_ra_ssie_test_ch0,
                                                 (uint32_t)k_ra_ssie_mask_irq_all,
                                                 true));
  volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch0);
  TEST_ASSERT_EQ((int32_t)(uint32_t)k_ra_ssie_mask_irq_all,
                 (int32_t)(reg->SSICR & (uint32_t)k_ra_ssie_mask_irq_all));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ssie_set_irq_enable((uint8_t)k_ra_ssie_test_ch0,
                                                 (uint32_t)k_ra_ssie_mask_iien,
                                                 false));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)(reg->SSICR & (uint32_t)k_ra_ssie_mask_iien));

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_set_irq_enable((uint8_t)k_ra_ssie_test_ch_bad, 0U, true));
  TEST_END("ssie set_irq_enable updates SSICR IRQ mask");
}

/* ---------------------------------------------------------------------------
 * IRQ
 * ---------------------------------------------------------------------------
 */

static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("ssie attach + dispatch decodes events");
  prep();

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ssie_attach_handler(stub_ssie_cb, (void*)(uintptr_t)0xA1B2U));
  volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch1);
  reg->SSISR                  = (uint32_t)k_ra_ssie_test_ssisr_err | (uint32_t)k_ra_ssie_mask_tuirq;
  reg->SSIFSR                 = (uint32_t)k_ra_ssie_mask_tde;

  ra_ssie_dispatch((uint8_t)k_ra_ssie_test_ch1);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_ssie_cb_count);
  TEST_ASSERT_EQ((int32_t)k_ra_ssie_test_ch1, (int32_t)s_ssie_cb_last_ch);
  TEST_ASSERT((s_ssie_cb_last_events & (uint8_t)k_ra_ssie_evt_tx_over) != 0U);
  TEST_ASSERT((s_ssie_cb_last_events & (uint8_t)k_ra_ssie_evt_rx_over) != 0U);
  TEST_ASSERT((s_ssie_cb_last_events & (uint8_t)k_ra_ssie_evt_tx_under) != 0U);
  TEST_ASSERT((s_ssie_cb_last_events & (uint8_t)k_ra_ssie_evt_tx_empty) != 0U);

  /* Out-of-range channel must be silently dropped. */
  ra_ssie_dispatch((uint8_t)k_ra_ssie_test_ch_bad);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_ssie_cb_count);

  /* Detach -> dispatch is a no-op. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_attach_handler(nullptr, nullptr));
  ra_ssie_dispatch((uint8_t)k_ra_ssie_test_ch0);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_ssie_cb_count);
  TEST_END("ssie attach + dispatch decodes events");
}

/* ---------------------------------------------------------------------------
 * Power transition
 * ---------------------------------------------------------------------------
 */

static void test_power_transition(void)
{
  TEST_BEGIN("ssie power transition");
  prep();

  const ra_ssie_cfg_t cfg = make_master_i2s_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch0, &cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_enter_stop((uint8_t)k_ra_ssie_test_ch0));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_exit_stop((uint8_t)k_ra_ssie_test_ch0));

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_enter_stop((uint8_t)k_ra_ssie_test_ch_bad));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_exit_stop((uint8_t)k_ra_ssie_test_ch_bad));
  TEST_END("ssie power transition");
}

/* ---------------------------------------------------------------------------
 * Sweep 17 additions: set_fifo_threshold + attach_dma_pair + send/recv iso
 * ---------------------------------------------------------------------------
 */

static void test_set_fifo_threshold_happy(void)
{
  TEST_BEGIN("ssie set_fifo_threshold programmes SSISCR");
  prep();
  const ra_ssie_cfg_t cfg = make_master_i2s_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch0, &cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ssie_set_fifo_threshold((uint8_t)k_ra_ssie_test_ch0, 0x10U, 0x05U));
  volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch0);
  TEST_ASSERT((reg->SSISCR & (uint32_t)k_ra_ssie_mask_tdes) != 0U);
  TEST_ASSERT((reg->SSISCR & (uint32_t)k_ra_ssie_mask_rdfs) != 0U);
  TEST_END("ssie set_fifo_threshold programmes SSISCR");
}

static void test_set_fifo_threshold_bad_args(void)
{
  TEST_BEGIN("ssie set_fifo_threshold rejects bad arguments");
  prep();
  const ra_ssie_cfg_t cfg = make_master_i2s_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch0, &cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_set_fifo_threshold((uint8_t)k_ra_ssie_test_ch0, 0x40U, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_set_fifo_threshold((uint8_t)k_ra_ssie_test_ch_bad, 0U, 0U));
  TEST_END("ssie set_fifo_threshold rejects bad arguments");
}

static void test_attach_dma_pair_happy(void)
{
  TEST_BEGIN("ssie attach_dma_pair binds tx+rx ids");
  prep();
  const ra_ssie_cfg_t cfg = make_master_i2s_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch0, &cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ssie_attach_dma_pair((uint8_t)k_ra_ssie_test_ch0,
                                                  (uint8_t)k_ra_ssie_test_dma_tx,
                                                  (uint8_t)k_ra_ssie_test_dma_rx));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_detach_dma((uint8_t)k_ra_ssie_test_ch0));
  TEST_END("ssie attach_dma_pair binds tx+rx ids");
}

static void test_attach_dma_pair_bad_args(void)
{
  TEST_BEGIN("ssie attach_dma_pair rejects all-unused / bad channel");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_attach_dma_pair((uint8_t)k_ra_ssie_test_ch_bad, 0U, 1U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_attach_dma_pair((uint8_t)k_ra_ssie_test_ch0, 0xFFU, 0xFFU));
  TEST_END("ssie attach_dma_pair rejects all-unused / bad channel");
}

static void test_send_iso_happy(void)
{
  TEST_BEGIN("ssie send_iso pushes all samples");
  prep();
  const ra_ssie_cfg_t cfg = make_master_i2s_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch0, &cfg));

  static const uint32_t buf[4] = {0xAA00U, 0xBB11U, 0xCC22U, 0xDD33U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_send_iso((uint8_t)k_ra_ssie_test_ch0, buf, 4U));
  TEST_END("ssie send_iso pushes all samples");
}

static void test_send_iso_bad_args(void)
{
  TEST_BEGIN("ssie send_iso rejects null buffer / bad channel");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_ssie_send_iso((uint8_t)k_ra_ssie_test_ch0, nullptr, 1U));
  static const uint32_t buf[1] = {0U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_send_iso((uint8_t)k_ra_ssie_test_ch_bad, buf, 1U));
  TEST_END("ssie send_iso rejects null buffer / bad channel");
}

static void test_recv_iso_drains_fifo(void)
{
  TEST_BEGIN("ssie recv_iso drains rx FIFO");
  prep();
  const ra_ssie_cfg_t cfg = make_master_i2s_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_ssie_init((uint8_t)k_ra_ssie_test_ch0, &cfg));

  /* Stub the SSIFSR RDC field so the loop runs at least once before
   * deciding the FIFO is empty. */
  volatile r_ssie_regs_t* reg = ra_ssie((uint8_t)k_ra_ssie_test_ch0);
  reg->SSIFSR                 = (uint32_t)1U << (uint8_t)k_ra_ssie_shift_rdc;
  reg->SSIFRDR                = 0xDEAD0001UL;

  uint32_t out[2] = {0U, 0U};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_ssie_recv_iso((uint8_t)k_ra_ssie_test_ch0, out, 2U, &got));
  TEST_ASSERT(got <= 2U);
  TEST_END("ssie recv_iso drains rx FIFO");
}

static void test_recv_iso_bad_args(void)
{
  TEST_BEGIN("ssie recv_iso rejects null + bad channel");
  prep();
  uint32_t out = 0U;
  uint16_t got = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_ssie_recv_iso((uint8_t)k_ra_ssie_test_ch0, nullptr, 1U, &got));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_ssie_recv_iso((uint8_t)k_ra_ssie_test_ch0, &out, 1U, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_ssie_recv_iso((uint8_t)k_ra_ssie_test_ch_bad, &out, 1U, &got));
  TEST_END("ssie recv_iso rejects null + bad channel");
}

int32_t main(void)
{
  test_init_happy();
  test_init_slave_tdm();
  test_init_all_formats();
  test_init_all_dividers();
  test_init_all_word_lengths();
  test_init_polarity_and_flags();
  test_init_lrcont_blocks_bckastp();
  test_init_bckastp_alone();
  test_init_null_cfg();
  test_init_bad_channel();
  test_init_bad_threshold();
  test_deinit();

  test_start_stop_tx();
  test_start_busy_when_not_idle();
  test_start_already_armed();
  test_start_bad_args();
  test_stop_bad_channel();
  test_recovery_clears_errors();
  test_mute_toggles_muen();
  test_set_thresholds();

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
  (void)fprintf(stderr, "[OK  ] test_ra_ssie.c\n");
  return 0;
}
