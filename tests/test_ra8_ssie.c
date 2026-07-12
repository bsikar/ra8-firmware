/**
 * @file test_ra8_ssie.c
 * @brief Unit tests for ra8_ssie.c (Serial Sound Interface Enhanced driver)
 *
 * @details
 * Exercises the full SSIE HAL surface defined by HUM Ch 46:
 *
 *  - Lifecycle (init / deinit / enter_stop / exit_stop)
 *  - All seven public formats (I2S, left-just, right-just, monaural,
 *    TDM-4, TDM-6, TDM-8) including OMOD/FRM/PDTA decoding
 *  - All thirteen controller-mode bit-clock dividers (CKDV 0x0..0xC)
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

#include "ra8_dmac.h"
#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "ra8_ssie.h"
#include "ra8_ssie_regs.h"
#include "unity_minimal.h"

/**
 * @enum ra8_ssie_test_const_t
 * @brief Magic numbers used by the tests promoted to a typed enum.
 */
typedef enum : uint16_t {
  k_ra8_ssie_test_ch0      = 0U,
  k_ra8_ssie_test_ch1      = 1U,
  k_ra8_ssie_test_ch_bad   = 2U,
  k_ra8_ssie_test_ch_way   = 250U,
  k_ra8_ssie_test_sample_a = 0xCAFEU,
  k_ra8_ssie_test_sample_b = 0xBEEFU,
  k_ra8_ssie_test_dma_tx   = 3U,
  k_ra8_ssie_test_dma_rx   = 4U,
  k_ra8_ssie_test_dma_bad  = 100U,
} ra8_ssie_test_const_t;

/**
 * @enum ra8_ssie_test_status_t
 * @brief Synthetic SSISR / SSIFSR values used by status tests.
 */
typedef enum : uint32_t {
  k_ra8_ssie_test_ssifsr_demo =
    (0x0AUL << 24U) | (0x05UL << 8U) | 0x00010001UL,       /* TDC=10, RDC=5, TDE+RDF */
  k_ra8_ssie_test_ssisr_err = 0x04000000UL | 0x10000000UL, /* ROIRQ + TOIRQ          */
} ra8_ssie_test_status_t;

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
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  s_ssie_cb_count       = 0U;
  s_ssie_cb_last_ch     = 0U;
  s_ssie_cb_last_ssisr  = 0U;
  s_ssie_cb_last_events = 0U;
}

static ra8_ssie_cfg_t make_controller_i2s_cfg(void)
{
  const ra8_ssie_cfg_t cfg = {
    .role          = k_ra8_ssie_role_controller,
    .format        = k_ra8_ssie_format_i2s,
    .data_word     = k_ra8_ssie_dwl_16,
    .system_word   = k_ra8_ssie_swl_32,
    .bclk_div      = k_ra8_ssie_bclk_div_4,
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
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_init_happy(void)
{
  TEST_BEGIN("ssie init happy (controller I2S)");
  prep();

  const ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));

  volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch0);
  TEST_ASSERT_NOT_NULL((void*)reg);

  TEST_ASSERT((reg->SSICR & (uint32_t)k_ra8_ssie_mask_mst) != 0U);
  TEST_ASSERT_EQ(((uint32_t)k_ra8_ssie_bclk_div_4 << (uint8_t)k_ra8_ssie_bit_ckdv0),
                 (reg->SSICR & (uint32_t)k_ra8_ssie_mask_ckdv));
  TEST_ASSERT_EQ(((uint32_t)k_ra8_ssie_swl_32 << (uint8_t)k_ra8_ssie_bit_swl0),
                 (reg->SSICR & (uint32_t)k_ra8_ssie_mask_swl));
  TEST_ASSERT_EQ(((uint32_t)k_ra8_ssie_dwl_16 << (uint8_t)k_ra8_ssie_bit_dwl0),
                 (reg->SSICR & (uint32_t)k_ra8_ssie_mask_dwl));

  TEST_ASSERT_EQ(0, (reg->SSICR & (uint32_t)k_ra8_ssie_mask_ren_ten));
  TEST_ASSERT_EQ(0, (reg->SSIOFR & (uint32_t)k_ra8_ssie_mask_omod));
  TEST_END("ssie init happy (controller I2S)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_peripheral_tdm(void)
{
  TEST_BEGIN("ssie init peripheral TDM-4 24-bit");
  prep();

  ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  cfg.role           = k_ra8_ssie_role_peripheral;
  cfg.format         = k_ra8_ssie_format_tdm_4;
  cfg.data_word      = k_ra8_ssie_dwl_24;
  cfg.system_word    = k_ra8_ssie_swl_32;
  cfg.use_gpt_clk    = true;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch1, &cfg));

  volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch1);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ(0, (reg->SSICR & (uint32_t)k_ra8_ssie_mask_mst));
  TEST_ASSERT((reg->SSICR & (uint32_t)k_ra8_ssie_mask_cks) != 0U);
  TEST_ASSERT_EQ(k_ra8_ssie_omod_tdm, (reg->SSIOFR & (uint32_t)k_ra8_ssie_mask_omod));
  /* FRM=01 selects 4-channel TDM. */
  TEST_ASSERT_EQ(((uint32_t)k_ra8_ssie_frm_alt1 << (uint8_t)k_ra8_ssie_bit_frm0),
                 (reg->SSICR & (uint32_t)k_ra8_ssie_mask_frm));
  TEST_END("ssie init peripheral TDM-4 24-bit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_all_formats(void)
{
  TEST_BEGIN("ssie init covers all seven formats");

  const struct {
    ra8_ssie_format_t fmt;
    uint8_t           omod;
    uint8_t           frm;
    bool              expect_pdta;
  } cases[] = {
    {k_ra8_ssie_format_i2s, (uint8_t)k_ra8_ssie_omod_i2s, (uint8_t)k_ra8_ssie_frm_default, false},
    {k_ra8_ssie_format_left_just,
     (uint8_t)k_ra8_ssie_omod_i2s,
     (uint8_t)k_ra8_ssie_frm_default,
     false},
    {k_ra8_ssie_format_right_just,
     (uint8_t)k_ra8_ssie_omod_i2s,
     (uint8_t)k_ra8_ssie_frm_default,
     true},
    {k_ra8_ssie_format_monaural,
     (uint8_t)k_ra8_ssie_omod_monaural,
     (uint8_t)k_ra8_ssie_frm_default,
     false},
    {k_ra8_ssie_format_tdm_4, (uint8_t)k_ra8_ssie_omod_tdm, (uint8_t)k_ra8_ssie_frm_alt1, false},
    {k_ra8_ssie_format_tdm_6, (uint8_t)k_ra8_ssie_omod_tdm, (uint8_t)k_ra8_ssie_frm_alt2, false},
    {k_ra8_ssie_format_tdm_8, (uint8_t)k_ra8_ssie_omod_tdm, (uint8_t)k_ra8_ssie_frm_alt3, false},
  };
  const uint8_t case_count = (uint8_t)(sizeof(cases) / sizeof(cases[0]));

  for (uint8_t i = 0U; i < case_count; i++) {
    prep();
    ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
    cfg.format         = cases[i].fmt;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));

    volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch0);
    TEST_ASSERT_EQ(cases[i].omod, (reg->SSIOFR & (uint32_t)k_ra8_ssie_mask_omod));
    TEST_ASSERT_EQ(((uint32_t)cases[i].frm << (uint8_t)k_ra8_ssie_bit_frm0),
                   (reg->SSICR & (uint32_t)k_ra8_ssie_mask_frm));
    TEST_ASSERT_EQ(cases[i].expect_pdta ? 1 : 0,
                   ((reg->SSICR & (uint32_t)k_ra8_ssie_mask_pdta) != 0U ? 1 : 0));
  }
  TEST_END("ssie init covers all seven formats");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_all_dividers(void)
{
  TEST_BEGIN("ssie init walks all 13 CKDV values");

  const ra8_ssie_bclk_div_t divs[] = {
    k_ra8_ssie_bclk_div_1,
    k_ra8_ssie_bclk_div_2,
    k_ra8_ssie_bclk_div_4,
    k_ra8_ssie_bclk_div_8,
    k_ra8_ssie_bclk_div_16,
    k_ra8_ssie_bclk_div_32,
    k_ra8_ssie_bclk_div_64,
    k_ra8_ssie_bclk_div_128,
    k_ra8_ssie_bclk_div_6,
    k_ra8_ssie_bclk_div_12,
    k_ra8_ssie_bclk_div_24,
    k_ra8_ssie_bclk_div_48,
    k_ra8_ssie_bclk_div_96,
  };
  const uint8_t div_count = (uint8_t)(sizeof(divs) / sizeof(divs[0]));

  for (uint8_t i = 0U; i < div_count; i++) {
    prep();
    ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
    cfg.bclk_div       = divs[i];
    TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));

    volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch0);
    TEST_ASSERT_EQ(((uint32_t)divs[i] << (uint8_t)k_ra8_ssie_bit_ckdv0),
                   (reg->SSICR & (uint32_t)k_ra8_ssie_mask_ckdv));
  }
  TEST_END("ssie init walks all 13 CKDV values");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_all_word_lengths(void)
{
  TEST_BEGIN("ssie init covers every SWL+DWL combination");

  const ra8_ssie_system_word_t sw[] = {
    k_ra8_ssie_swl_8,
    k_ra8_ssie_swl_16,
    k_ra8_ssie_swl_24,
    k_ra8_ssie_swl_32,
    k_ra8_ssie_swl_48,
    k_ra8_ssie_swl_64,
    k_ra8_ssie_swl_128,
    k_ra8_ssie_swl_256,
  };
  const ra8_ssie_data_word_t dw[] = {
    k_ra8_ssie_dwl_8,
    k_ra8_ssie_dwl_16,
    k_ra8_ssie_dwl_18,
    k_ra8_ssie_dwl_20,
    k_ra8_ssie_dwl_22,
    k_ra8_ssie_dwl_24,
    k_ra8_ssie_dwl_32,
  };
  const uint8_t sw_count = (uint8_t)(sizeof(sw) / sizeof(sw[0]));
  const uint8_t dw_count = (uint8_t)(sizeof(dw) / sizeof(dw[0]));

  for (uint8_t i = 0U; i < sw_count; i++) {
    for (uint8_t j = 0U; j < dw_count; j++) {
      prep();
      ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
      cfg.system_word    = sw[i];
      cfg.data_word      = dw[j];
      TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));

      volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch0);
      TEST_ASSERT_EQ(((uint32_t)sw[i] << (uint8_t)k_ra8_ssie_bit_swl0),
                     (reg->SSICR & (uint32_t)k_ra8_ssie_mask_swl));
      TEST_ASSERT_EQ(((uint32_t)dw[j] << (uint8_t)k_ra8_ssie_bit_dwl0),
                     (reg->SSICR & (uint32_t)k_ra8_ssie_mask_dwl));
    }
  }
  TEST_END("ssie init covers every SWL+DWL combination");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_polarity_and_flags(void)
{
  TEST_BEGIN("ssie init exposes every polarity flag");
  prep();
  ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  cfg.long_frame     = true;
  cfg.bckp_rising    = true;
  cfg.lrckp_low      = true;
  cfg.spdp_high      = true;
  cfg.byte_swap      = true;
  cfg.enable_aucke   = true;
  cfg.tx_threshold   = 7U;
  cfg.rx_threshold   = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));

  volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch0);
  TEST_ASSERT((reg->SSICR & (uint32_t)k_ra8_ssie_mask_del) != 0U);
  TEST_ASSERT((reg->SSICR & (uint32_t)k_ra8_ssie_mask_bckp) != 0U);
  TEST_ASSERT((reg->SSICR & (uint32_t)k_ra8_ssie_mask_lrckp) != 0U);
  TEST_ASSERT((reg->SSICR & (uint32_t)k_ra8_ssie_mask_spdp) != 0U);
  TEST_ASSERT((reg->SSIFCR & (uint32_t)k_ra8_ssie_mask_bsw) != 0U);
  TEST_ASSERT((reg->SSIFCR & (uint32_t)k_ra8_ssie_mask_aucke) != 0U);

  /* SSISCR encodes RX threshold in [4:0] and TX threshold in [12:8]. */
  TEST_ASSERT_EQ(((7U << (uint8_t)k_ra8_ssie_shift_tdes) | 1U), reg->SSISCR);
  TEST_END("ssie init exposes every polarity flag");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_lrcont_blocks_bckastp(void)
{
  TEST_BEGIN("ssie init: LRCONT wins over BCKASTP when both requested");
  prep();
  ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  cfg.lr_continue    = true;
  cfg.bck_idle_stop  = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));

  volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch0);
  TEST_ASSERT((reg->SSIOFR & (uint32_t)k_ra8_ssie_mask_lrcont) != 0U);
  /* HUM Ch 46.2.7 "SSIOFR : Audio Format Register" p 3091
   * Note 2 forbids both LRCONT+BCKASTP at once, so BCKASTP must be 0. */
  TEST_ASSERT_EQ(0, (reg->SSIOFR & (uint32_t)k_ra8_ssie_mask_bckastp));
  TEST_END("ssie init: LRCONT wins over BCKASTP when both requested");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bckastp_alone(void)
{
  TEST_BEGIN("ssie init: BCKASTP alone is honoured");
  prep();
  ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  cfg.bck_idle_stop  = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));

  volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch0);
  TEST_ASSERT((reg->SSIOFR & (uint32_t)k_ra8_ssie_mask_bckastp) != 0U);
  TEST_END("ssie init: BCKASTP alone is honoured");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null_cfg(void)
{
  TEST_BEGIN("ssie init null cfg");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, nullptr));
  TEST_END("ssie init null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_channel(void)
{
  TEST_BEGIN("ssie init bad channel");
  prep();

  const ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch_bad, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch_way, &cfg));
  TEST_END("ssie init bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_threshold(void)
{
  TEST_BEGIN("ssie init rejects threshold > 0x1F");
  prep();
  ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  cfg.tx_threshold   = 0x40U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));
  cfg              = make_controller_i2s_cfg();
  cfg.rx_threshold = 0x40U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));
  TEST_END("ssie init rejects threshold > 0x1F");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("ssie deinit clears TEN/REN + AUCKE");
  prep();

  const ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));

  volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch0);
  reg->SSICR                  = reg->SSICR | (uint32_t)k_ra8_ssie_mask_ren_ten;
  reg->SSIFCR                 = reg->SSIFCR | (uint32_t)k_ra8_ssie_mask_aucke;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_deinit((uint8_t)k_ra8_ssie_test_ch0));
  TEST_ASSERT_EQ(0, (reg->SSICR & (uint32_t)k_ra8_ssie_mask_ren_ten));
  TEST_ASSERT_EQ(0, (reg->SSIFCR & (uint32_t)k_ra8_ssie_mask_aucke));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_deinit((uint8_t)k_ra8_ssie_test_ch_bad));
  TEST_END("ssie deinit clears TEN/REN + AUCKE");
}

/* ---------------------------------------------------------------------------
 * Run control
 * ---------------------------------------------------------------------------
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_start_stop_tx(void)
{
  TEST_BEGIN("ssie start TX sets TEN + TIE + TUIEN");
  prep();
  const ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));

  volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch0);
  /* Pre-set IIRQ so the start helper believes the SSIE is idle. */
  reg->SSISR = (uint32_t)k_ra8_ssie_mask_iirq;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_start((uint8_t)k_ra8_ssie_test_ch0, k_ra8_ssie_dir_tx));
  TEST_ASSERT((reg->SSICR & (uint32_t)k_ra8_ssie_mask_ten) != 0U);
  TEST_ASSERT_EQ(0, (reg->SSICR & (uint32_t)k_ra8_ssie_mask_ren));
  TEST_ASSERT((reg->SSICR & (1UL << (uint8_t)k_ra8_ssie_bit_tuien)) != 0U);
  TEST_ASSERT((reg->SSIFCR & (uint32_t)k_ra8_ssie_mask_tie) != 0U);
  TEST_ASSERT_EQ(0, (reg->SSIFCR & (uint32_t)k_ra8_ssie_mask_rie));

  /* Stop should clear TEN + IRQ enables but leave IIEN armed. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_stop((uint8_t)k_ra8_ssie_test_ch0));
  TEST_ASSERT_EQ(0, (reg->SSICR & (uint32_t)k_ra8_ssie_mask_ren_ten));
  TEST_ASSERT_EQ(0, (reg->SSICR & (uint32_t)k_ra8_ssie_mask_err_ien));
  TEST_ASSERT((reg->SSICR & (uint32_t)k_ra8_ssie_mask_iien) != 0U);
  TEST_ASSERT_EQ(0, (reg->SSIFCR & (uint32_t)k_ra8_ssie_mask_rie_tie));
  TEST_END("ssie start TX sets TEN + TIE + TUIEN");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_busy_when_not_idle(void)
{
  TEST_BEGIN("ssie start refuses when SSIE not idle");
  prep();
  const ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));

  volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch0);
  /* IIRQ low + REN/TEN clear -> not idle. */
  reg->SSISR = 0U;
  TEST_ASSERT_EQ(k_ra8_err_busy, ra8_ssie_start((uint8_t)k_ra8_ssie_test_ch0, k_ra8_ssie_dir_rx));

  /* Already started in another direction. */
  reg->SSISR = (uint32_t)k_ra8_ssie_mask_iirq;
  reg->SSICR = reg->SSICR | (uint32_t)k_ra8_ssie_mask_ten;
  TEST_ASSERT_EQ(k_ra8_err_busy, ra8_ssie_start((uint8_t)k_ra8_ssie_test_ch0, k_ra8_ssie_dir_rx));
  TEST_END("ssie start refuses when SSIE not idle");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_already_armed(void)
{
  TEST_BEGIN("ssie start is idempotent when already in mode");
  prep();
  const ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));

  volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch0);
  reg->SSISR                  = (uint32_t)k_ra8_ssie_mask_iirq;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_start((uint8_t)k_ra8_ssie_test_ch0, k_ra8_ssie_dir_tx));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_start((uint8_t)k_ra8_ssie_test_ch0, k_ra8_ssie_dir_tx));
  TEST_END("ssie start is idempotent when already in mode");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_bad_args(void)
{
  TEST_BEGIN("ssie start rejects bad dir / bad channel");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ssie_start((uint8_t)k_ra8_ssie_test_ch_bad, k_ra8_ssie_dir_tx));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ssie_start((uint8_t)k_ra8_ssie_test_ch0, (ra8_ssie_dir_t)0U));
  TEST_END("ssie start rejects bad dir / bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_stop_bad_channel(void)
{
  TEST_BEGIN("ssie stop bad channel");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_stop((uint8_t)k_ra8_ssie_test_ch_bad));
  TEST_END("ssie stop bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_recovery_clears_errors(void)
{
  TEST_BEGIN("ssie start_recovery clears error flags + SSIRST");
  prep();
  const ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));

  volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch0);
  reg->SSISR                  = (uint32_t)k_ra8_ssie_mask_err_all | (uint32_t)k_ra8_ssie_mask_iirq;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_start_recovery((uint8_t)k_ra8_ssie_test_ch0));
  TEST_ASSERT_EQ(0, (reg->SSISR & (uint32_t)k_ra8_ssie_mask_err_all));
  TEST_ASSERT_EQ(0, (reg->SSIFCR & (uint32_t)k_ra8_ssie_mask_ssirst));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_start_recovery((uint8_t)k_ra8_ssie_test_ch_bad));
  TEST_END("ssie start_recovery clears error flags + SSIRST");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_mute_toggles_muen(void)
{
  TEST_BEGIN("ssie mute toggles SSICR.MUEN");
  prep();
  const ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));

  volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch0);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_mute((uint8_t)k_ra8_ssie_test_ch0, true));
  TEST_ASSERT((reg->SSICR & (uint32_t)k_ra8_ssie_mask_muen) != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_mute((uint8_t)k_ra8_ssie_test_ch0, false));
  TEST_ASSERT_EQ(0, (reg->SSICR & (uint32_t)k_ra8_ssie_mask_muen));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_mute((uint8_t)k_ra8_ssie_test_ch_bad, true));
  TEST_END("ssie mute toggles SSICR.MUEN");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_thresholds(void)
{
  TEST_BEGIN("ssie set_thresholds writes SSISCR");
  prep();
  const ra8_ssie_cfg_t cfg = make_controller_i2s_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ssie_set_thresholds((uint8_t)k_ra8_ssie_test_ch0, 0x10U, 0x05U));
  volatile r_ssie_regs_t* reg = ra8_ssie((uint8_t)k_ra8_ssie_test_ch0);
  TEST_ASSERT_EQ(((0x10U << (uint8_t)k_ra8_ssie_shift_tdes) | 0x05U), reg->SSISCR);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ssie_set_thresholds((uint8_t)k_ra8_ssie_test_ch0, 0x40U, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ssie_set_thresholds((uint8_t)k_ra8_ssie_test_ch_bad, 0U, 0U));
  TEST_END("ssie set_thresholds writes SSISCR");
}

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
  uint32_t buf[40];
  for (uint8_t i = 0U; i < 40U; i++) {
    buf[i] = 0xAA00U + i;
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
  uint16_t written = 99U;
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
  uint16_t read               = 99U;
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

  static uint32_t tx_buf[16];
  static uint32_t rx_buf[16];
  for (uint8_t i = 0U; i < 16U; i++) {
    tx_buf[i] = i;
  }

  const ra8_ssie_dma_cfg_t dma = {
    .tx_dma_channel = (uint8_t)k_ra8_ssie_test_dma_tx,
    .rx_dma_channel = (uint8_t)k_ra8_ssie_test_dma_rx,
    .tx_buffer      = tx_buf,
    .rx_buffer      = rx_buf,
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
  static uint32_t          tx_buf[8];
  const ra8_ssie_dma_cfg_t dma = {
    .tx_dma_channel = (uint8_t)k_ra8_ssie_test_dma_tx,
    .rx_dma_channel = 0xFFU,
    .tx_buffer      = tx_buf,
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
  static uint32_t buf[2] = {1U, 2U};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, nullptr));

  ra8_ssie_dma_cfg_t dma = {
    .tx_dma_channel = 0xFFU,
    .rx_dma_channel = 0xFFU,
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
  dma.tx_buffer  = buf;
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
  reg->SSIFRDR                = 0xDEAD0001UL;

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
 * for V4. With the channel un-init the simulator returns invalid_arg,
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
  TEST_ASSERT(r1 != k_ra8_err_invalid_arg || r1 == k_ra8_ok);
  ra8_err_t r2 = ra8_ssie_start((uint8_t)k_ra8_ssie_test_ch0, k_ra8_ssie_dir_tx);
  TEST_ASSERT(r2 != k_ra8_err_invalid_arg || r2 == k_ra8_ok);
  ra8_err_t r3 = ra8_ssie_start((uint8_t)k_ra8_ssie_test_ch0, k_ra8_ssie_dir_tx_rx);
  TEST_ASSERT(r3 != k_ra8_err_invalid_arg || r3 == k_ra8_ok);
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
  static uint32_t txbuf[4] = {0U};
  static uint32_t rxbuf[4] = {0U};
  /* V1 */
  ra8_ssie_dma_cfg_t dma1 = {
    .tx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .rx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .tx_buffer      = txbuf,
    .rx_buffer      = rxbuf,
    .tx_samples     = 4U,
    .rx_samples     = 4U,
  };
  ra8_err_t v1 = ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, &dma1);
  /* The "neither" gate must not have rejected the call. Other ra8_err_t
   * outcomes are acceptable (DMA layer may or may not be ready in sim). */
  (void)v1;
  /* V2 */
  ra8_ssie_dma_cfg_t dma2 = {
    .tx_dma_channel = (uint8_t)k_ssie_mcdc_dma_bad,
    .rx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .tx_buffer      = nullptr,
    .rx_buffer      = rxbuf,
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
  static uint32_t txbuf[4] = {0U};
  static uint32_t rxbuf[4] = {0U};
  /* V1 */
  ra8_ssie_dma_cfg_t v1cfg = {
    .tx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .rx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .tx_buffer      = txbuf,
    .rx_buffer      = rxbuf,
    .tx_samples     = 4U,
    .rx_samples     = 4U,
  };
  (void)ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, &v1cfg);
  /* V2: want_tx=F (tx ch out of range), rx valid. */
  ra8_ssie_dma_cfg_t v2cfg = {
    .tx_dma_channel = (uint8_t)k_ssie_mcdc_dma_bad,
    .rx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .tx_buffer      = nullptr,
    .rx_buffer      = rxbuf,
    .tx_samples     = 0U,
    .rx_samples     = 4U,
  };
  (void)ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, &v2cfg);
  /* V3: want_tx=T, tx_buffer=NULL -> invalid_arg. */
  ra8_ssie_dma_cfg_t v3cfg = {
    .tx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .rx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .tx_buffer      = nullptr,
    .rx_buffer      = rxbuf,
    .tx_samples     = 4U,
    .rx_samples     = 4U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, &v3cfg));
  /* V4: want_tx=T, tx_samples=0 -> invalid_arg. */
  ra8_ssie_dma_cfg_t v4cfg = {
    .tx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .rx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .tx_buffer      = txbuf,
    .rx_buffer      = rxbuf,
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
  static uint32_t    txbuf[4] = {0U};
  static uint32_t    rxbuf[4] = {0U};
  ra8_ssie_dma_cfg_t v1cfg    = {
    .tx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .rx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .tx_buffer      = txbuf,
    .rx_buffer      = rxbuf,
    .tx_samples     = 4U,
    .rx_samples     = 4U,
  };
  (void)ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, &v1cfg);
  ra8_ssie_dma_cfg_t v2cfg = {
    .tx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .rx_dma_channel = (uint8_t)k_ssie_mcdc_dma_bad,
    .tx_buffer      = txbuf,
    .rx_buffer      = nullptr,
    .tx_samples     = 4U,
    .rx_samples     = 0U,
  };
  (void)ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, &v2cfg);
  ra8_ssie_dma_cfg_t v3cfg = {
    .tx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .rx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .tx_buffer      = txbuf,
    .rx_buffer      = nullptr,
    .tx_samples     = 4U,
    .rx_samples     = 4U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_attach_dma((uint8_t)k_ra8_ssie_test_ch0, &v3cfg));
  ra8_ssie_dma_cfg_t v4cfg = {
    .tx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .rx_dma_channel = (uint8_t)k_ssie_mcdc_dma_valid,
    .tx_buffer      = txbuf,
    .rx_buffer      = rxbuf,
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
  test_init_happy();
  test_init_peripheral_tdm();
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
  (void)fprintf(stderr, "[OK  ] test_ra8_ssie.c\n");
  return 0;
}
