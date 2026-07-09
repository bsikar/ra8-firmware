/**
 * @file test_ra_pdm.c
 * @brief Unit tests for ra_pdm.c (PDM-IF capture driver)
 *
 * @details
 * Exercises the real PDM-IF register model against the ``ra_sim_mmap``
 * peripheral window: configuration writes, the start / read-enable /
 * read / stop sequence (HUM Ch 49.4), 20-bit sign extension and the
 * argument-validation contract. The receive FIFO is emulated by
 * pre-loading the channel's PDDSR (fill count) and PDDRR (sample word)
 * in mapped memory.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_pdm_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_pdm.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/** @brief Channel under test (EK-RA8D2 MEMS mic wiring). */
enum : uint8_t {
  k_test_ch = 2U,
};

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
}

/**
 * @brief Fill a config with a valid order-4 / 16 kHz SPH0690-style set.
 *
 * @param[out] cfg Config to populate.
 */
static void make_cfg(ra_pdm_channel_cfg_t* cfg)
{
  *cfg              = (ra_pdm_channel_cfg_t){};
  cfg->sinc_order   = 4U;
  cfg->clock_div    = 0U;
  cfg->sinc_dec     = 0x7CU;
  cfg->sinc_range   = 0x05U;
  cfg->data_shift   = 0U;
  cfg->edge         = 0U;
  cfg->rx_threshold = 4U;
  cfg->hpf_s0       = 0x3F61U;
  cfg->hpf_k1       = 0x3EC1U;
  cfg->hpf_h[0]     = 0x4000U;
  cfg->hpf_h[1]     = 0xC000U;
  cfg->comp_h[0]    = 0x1FE8U;
  cfg->comp_h[10]   = 0x1FE8U;
  cfg->lpf_h0       = 0x0400U;
  cfg->lpf_h1[0]    = 0x1FF8U;
  cfg->lpf_h1[19]   = 0x1FF8U;
}

/**
 * @par MC/DC:
 * (no compound decisions in the code under test -- ra_pdm_init/deinit
 * contain no `&&`/`||`; this case exercises the lifecycle happy path)
 */
static void test_init_deinit(void)
{
  TEST_BEGIN("pdm init/deinit");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_pdm_init());
  TEST_ASSERT_EQ(k_ra_ok, ra_pdm_deinit());
  TEST_END("pdm init/deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions -- verifies configure writes and the null /
 * range rejection contract; each guard is a single condition)
 */
static void test_configure(void)
{
  TEST_BEGIN("pdm configure");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_pdm_init());
  ra_pdm_channel_cfg_t cfg = {};
  make_cfg(&cfg);
  TEST_ASSERT_EQ(k_ra_ok, ra_pdm_configure(k_test_ch, &cfg));

  volatile r_pdm_ch_regs_t* reg = ra_pdm_ch(k_test_ch);
  TEST_ASSERT_EQ(0x00000040U, reg->PDMDSR);   /* SFMD=4 << 4                */
  TEST_ASSERT_EQ(0x057C0000U, reg->PDSFCR);   /* SINCDEC=0x7C, SINCRNG=0x05 */
  TEST_ASSERT_EQ(0x00003F61U, reg->PDHFCS0R); /* HPF s0                     */
  TEST_ASSERT_EQ(0x00001FE8U, reg->PDCFCHR[0]);
  TEST_ASSERT_EQ(0x00001FE8U, reg->PDCFCHR[10]);
  TEST_ASSERT_EQ(0x00000400U, reg->PDLFCH010R);
  TEST_ASSERT_EQ(0x00001FF8U, reg->PDLFCH1R[19]);
  TEST_ASSERT_EQ(0x00000004U, reg->PDDBCR);

  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_pdm_configure(k_test_ch, nullptr));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_pdm_configure((uint8_t)k_ra_pdm_ch_count, &cfg));
  TEST_END("pdm configure");
}

/**
 * @par MC/DC:
 * (no compound decisions -- start sets the per-channel trigger bit;
 * the range guard is a single condition)
 */
static void test_start_and_read_enable(void)
{
  TEST_BEGIN("pdm start + read_enable");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_pdm_init());
  TEST_ASSERT_EQ(k_ra_ok, ra_pdm_start(k_test_ch));
  TEST_ASSERT_EQ((uint32_t)(1U << k_test_ch), ra_pdm()->PDCSTRTR);

  TEST_ASSERT_EQ(k_ra_ok, ra_pdm_read_enable(k_test_ch));
  TEST_ASSERT_EQ(0x1U, ra_pdm_ch(k_test_ch)->PDDRCR);

  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_pdm_start((uint8_t)k_ra_pdm_ch_count));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_pdm_read_enable((uint8_t)k_ra_pdm_ch_count));
  TEST_END("pdm start + read_enable");
}

/**
 * @par MC/DC:
 * (no compound decisions -- covers both sign-extension branches and
 * both sides of the FIFO-count cap ternary, each a single condition)
 */
static void test_read_samples(void)
{
  TEST_BEGIN("pdm read samples");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_pdm_init());
  volatile r_pdm_ch_regs_t* reg    = ra_pdm_ch(k_test_ch);
  int32_t                   buf[8] = {};
  uint32_t                  got    = 0U;

  /* Positive sample, FIFO fill (3) below buffer capacity (8). */
  reg->PDDSR = 3U;
  reg->PDDRR = 0x12345U;
  TEST_ASSERT_EQ(k_ra_ok, ra_pdm_read(k_test_ch, buf, 8U, &got));
  TEST_ASSERT_EQ(3U, got);
  TEST_ASSERT_EQ(0x12345, buf[0]);

  /* Negative sample (bit19 set) -> sign extended to -1. */
  reg->PDDSR = 1U;
  reg->PDDRR = 0xFFFFFU;
  TEST_ASSERT_EQ(k_ra_ok, ra_pdm_read(k_test_ch, buf, 8U, &got));
  TEST_ASSERT_EQ(1U, got);
  TEST_ASSERT_EQ(-1, buf[0]);

  /* FIFO count (10) above capacity (4) -> capped. */
  reg->PDDSR = 10U;
  reg->PDDRR = 0x80000U; /* -524288 */
  TEST_ASSERT_EQ(k_ra_ok, ra_pdm_read(k_test_ch, buf, 4U, &got));
  TEST_ASSERT_EQ(4U, got);
  TEST_ASSERT_EQ(-524288, buf[3]);
  TEST_END("pdm read samples");
}

/**
 * @par MC/DC:
 * (no compound decisions -- null / range / zero-length guards are each
 * a single condition evaluated in isolation)
 */
static void test_read_validation(void)
{
  TEST_BEGIN("pdm read validation");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_pdm_init());
  int32_t  buf[4] = {};
  uint32_t got    = 0U;
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_pdm_read(k_test_ch, nullptr, 4U, &got));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_pdm_read(k_test_ch, buf, 4U, nullptr));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_pdm_read((uint8_t)k_ra_pdm_ch_count, buf, 4U, &got));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_pdm_read(k_test_ch, buf, 0U, &got));
  TEST_END("pdm read validation");
}

/**
 * @par MC/DC:
 * (no compound decisions -- stop's completion test is a single
 * condition; covers the halted-fast path and the timeout path)
 */
static void test_stop(void)
{
  TEST_BEGIN("pdm stop");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_pdm_init());

  /* STATE bit clear -> channel reports halted immediately. */
  ra_pdm()->PDCSR = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_pdm_stop(k_test_ch));
  TEST_ASSERT_EQ((uint32_t)(1U << k_test_ch), ra_pdm()->PDCSTPTR);
  TEST_ASSERT_EQ(0U, ra_pdm_ch(k_test_ch)->PDDRCR);

  /* STATE bit stuck set -> bounded poll expires with timeout. */
  ra_pdm()->PDCSR = (uint32_t)(1U << k_test_ch);
  TEST_ASSERT_EQ(k_ra_err_hw_timeout, ra_pdm_stop(k_test_ch));

  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_pdm_stop((uint8_t)k_ra_pdm_ch_count));
  TEST_END("pdm stop");
}

int32_t main(void)
{
  test_init_deinit();
  test_configure();
  test_start_and_read_enable();
  test_read_samples();
  test_read_validation();
  test_stop();
  (void)fprintf(stderr, "[OK  ] test_ra_pdm.c\n");
  return 0;
}
