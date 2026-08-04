/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_ssie.c
 * @brief Unit tests for ra8_ssie.c (Serial Sound Interface Enhanced driver)
 *
 * @details
 * Exercises the SSIE HAL config + run-control surface defined by
 * HUM Ch 46:
 *
 *  - Lifecycle (init / deinit)
 *  - All seven public formats (I2S, left-just, right-just, monaural,
 *    TDM-4, TDM-6, TDM-8) including OMOD/FRM/PDTA decoding
 *  - All thirteen controller-mode bit-clock dividers (CKDV 0x0..0xC)
 *  - All eight system word lengths and seven legal data word lengths
 *  - Polarity/format flags (BCKP, LRCKP, SPDP, DEL, BSW, AUCKE,
 *    LRCONT, BCKASTP) and their interaction (LRCONT + BCKASTP rule)
 *  - Run control (start/stop, mute, threshold update, recovery)
 *
 * The data-path/DMA/status/IRQ/power tests live in the sibling
 * test_ra8_ssie_io.c and the MC/DC vector suites in
 * test_ra8_ssie_mcdc.c; shared fixture state lives in
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

/**
 * @enum ssie_test_lit_t
 * @brief Named constants for the register stamp patterns and literal
 *        test vectors previously inlined in this file's test bodies.
 */
typedef enum : uint32_t {
  k_ssie_cfg_tx_threshold  = 7U,    /**< Ssie config tx threshold.  */
  k_ssie_cfg_tx_threshold2 = 0x40U, /**< Ssie config tx threshold2. */
  k_ssie_cfg_rx_threshold  = 0x40U, /**< Ssie config rx threshold.  */
} ssie_test_lit_t;

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
    ra8_ssie_format_t fmt;         /**< Fmt.         */
    uint8_t           omod;        /**< Omod.        */
    uint8_t           frm;         /**< Frm.         */
    bool              expect_pdta; /**< Expect pdta. */
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
  cfg.tx_threshold   = k_ssie_cfg_tx_threshold;
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
  cfg.tx_threshold   = k_ssie_cfg_tx_threshold2;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ssie_init((uint8_t)k_ra8_ssie_test_ch0, &cfg));
  cfg              = make_controller_i2s_cfg();
  cfg.rx_threshold = k_ssie_cfg_rx_threshold;
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

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom, exactly as before.
 */
static void (*const s_test_roster[])(void) = {
  test_init_happy,
  test_init_peripheral_tdm,
  test_init_all_formats,
  test_init_all_dividers,
  test_init_all_word_lengths,
  test_init_polarity_and_flags,
  test_init_lrcont_blocks_bckastp,
  test_init_bckastp_alone,
  test_init_null_cfg,
  test_init_bad_channel,
  test_init_bad_threshold,
  test_deinit,
  test_start_stop_tx,
  test_start_busy_when_not_idle,
  test_start_already_armed,
  test_start_bad_args,
  test_stop_bad_channel,
  test_recovery_clears_errors,
  test_mute_toggles_muen,
  test_set_thresholds,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK  ] test_ra8_ssie.c\n");
  return 0;
}
