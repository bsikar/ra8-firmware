/**
 * @file test_ra8_pdm.c
 * @brief Unit tests for ra8_pdm.c (PDM-IF capture driver)
 *
 * @details
 * Exercises the real PDM-IF register model against the ``ra8_sim_mmap``
 * peripheral window: configuration writes, the start / read-enable /
 * read / stop sequence (HUM Ch 49.4), 20-bit sign extension and the
 * argument-validation contract. The receive FIFO is emulated by
 * pre-loading the channel's PDDSR (fill count) and PDDRR (sample word)
 * in mapped memory.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_pdm.h"
#include "ra8_pdm_regs.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum t_pdm_filter_t
 * @brief Filter coefficients written into the channel configuration.
 *
 * @details
 * Representative Q1.14 coefficients for the SPH0690 microphone chain: a DC-
 * blocking high-pass, a droop-compensation FIR and a decimating low-pass. The
 * driver only marshals them into registers, so the values need to be plausible
 * and distinct rather than acoustically tuned -- a swapped coefficient must
 * land in the wrong register and be visible.
 */
typedef enum : uint16_t {
  k_t_sinc_dec   = 0x7CU,   /**< SINC decimation factor.                    */
  k_t_sinc_range = 0x05U,   /**< SINC output range select.                  */
  k_t_hpf_s0     = 0x3F61U, /**< High-pass shift coefficient s0.            */
  k_t_hpf_k1     = 0x3EC1U, /**< High-pass feedback coefficient k1.         */
  k_t_hpf_h0     = 0x4000U, /**< High-pass FIR tap 0: +1.0 in Q1.14.        */
  k_t_hpf_h1     = 0xC000U, /**< High-pass FIR tap 1: -1.0 in Q1.14.        */
  k_t_comp_tap   = 0x1FE8U, /**< Compensation-filter tap; written to the
                                   first and last slot to prove the whole
                                   array is marshalled.                       */
  k_t_lpf_h0     = 0x0400U, /**< Low-pass leading tap.                      */
  k_t_lpf_tap    = 0x1FF8U, /**< Low-pass tap, first and last slot.         */
  k_t_comp_last  = 10U,     /**< Last compensation-filter tap index.        */
  k_t_lpf_last   = 19U,     /**< Last low-pass tap index.                   */
} t_pdm_filter_t;

/**
 * @enum t_pdm_sample_t
 * @brief FIFO status and sample words staged into the PDM registers.
 *
 * @details
 * PDDRR carries a 20-bit two's-complement sample, so bit 19 is the sign. The
 * three values below drive the positive, all-ones (-1) and most-negative
 * cases of the driver's sign extension.
 */
typedef enum : uint32_t {
  k_t_fifo_over_cap = 10U,      /**< PDDSR count above the caller's buffer,
                                     which the read must cap.                 */
  k_t_sample_pos    = 0x12345U, /**< A positive 20-bit sample.                */
  k_t_sample_neg1   = 0xFFFFFU, /**< All ones: sign-extends to -1.            */
  k_t_sample_min    = 0x80000U, /**< Sign bit only: sign-extends to -524288.  */
} t_pdm_sample_t;

/** @brief Channel under test (EK-RA8D2 MEMS mic wiring). */
enum : uint8_t {
  k_test_ch = 2U, /**< Test channel. */
};

static void prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
}

/**
 * @brief Fill a config with a valid order-4 / 16 kHz SPH0690-style set.
 *
 * @param[out] cfg Config to populate.
 */
static void make_cfg(ra8_pdm_channel_cfg_t* cfg)
{
  *cfg                       = (ra8_pdm_channel_cfg_t){};
  cfg->sinc_order            = 4U;
  cfg->clock_div             = 0U;
  cfg->sinc_dec              = k_t_sinc_dec;
  cfg->sinc_range            = k_t_sinc_range;
  cfg->data_shift            = 0U;
  cfg->edge                  = 0U;
  cfg->rx_threshold          = 4U;
  cfg->hpf_s0                = k_t_hpf_s0;
  cfg->hpf_k1                = k_t_hpf_k1;
  cfg->hpf_h[0]              = k_t_hpf_h0;
  cfg->hpf_h[1]              = k_t_hpf_h1;
  cfg->comp_h[0]             = k_t_comp_tap;
  cfg->comp_h[k_t_comp_last] = k_t_comp_tap;
  cfg->lpf_h0                = k_t_lpf_h0;
  cfg->lpf_h1[0]             = k_t_lpf_tap;
  cfg->lpf_h1[k_t_lpf_last]  = k_t_lpf_tap;
}

/**
 * @par MC/DC:
 * (no compound decisions in the code under test -- ra8_pdm_init/deinit
 * contain no `&&`/`||`; this case exercises the lifecycle happy path)
 */
static void test_init_deinit(void)
{
  TEST_BEGIN("pdm init/deinit");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdm_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdm_deinit());
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdm_init());
  ra8_pdm_channel_cfg_t cfg = {};
  make_cfg(&cfg);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdm_configure(k_test_ch, &cfg));

  volatile r_pdm_ch_regs_t* reg = ra8_pdm_ch(k_test_ch);
  TEST_ASSERT_EQ(0x00000040U, reg->PDMDSR);   /* SFMD=4 << 4                */
  TEST_ASSERT_EQ(0x057C0000U, reg->PDSFCR);   /* SINCDEC=0x7C, SINCRNG=0x05 */
  TEST_ASSERT_EQ(0x00003F61U, reg->PDHFCS0R); /* HPF s0                     */
  TEST_ASSERT_EQ(0x00001FE8U, reg->PDCFCHR[0]);
  TEST_ASSERT_EQ(0x00001FE8U, reg->PDCFCHR[10]);
  TEST_ASSERT_EQ(0x00000400U, reg->PDLFCH010R);
  TEST_ASSERT_EQ(0x00001FF8U, reg->PDLFCH1R[19]);
  TEST_ASSERT_EQ(0x00000004U, reg->PDDBCR);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_pdm_configure(k_test_ch, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_pdm_configure((uint8_t)k_ra8_pdm_ch_count, &cfg));
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdm_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdm_start(k_test_ch));
  TEST_ASSERT_EQ((1U << k_test_ch), ra8_pdm()->PDCSTRTR);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdm_read_enable(k_test_ch));
  TEST_ASSERT_EQ(0x1U, ra8_pdm_ch(k_test_ch)->PDDRCR);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_pdm_start((uint8_t)k_ra8_pdm_ch_count));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_pdm_read_enable((uint8_t)k_ra8_pdm_ch_count));
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdm_init());
  volatile r_pdm_ch_regs_t* reg    = ra8_pdm_ch(k_test_ch);
  int32_t                   buf[8] = {};
  uint32_t                  got    = 0U;

  /* Positive sample, FIFO fill (3) below buffer capacity (8). */
  reg->PDDSR = 3U;
  reg->PDDRR = k_t_sample_pos;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdm_read(k_test_ch, buf, 8U, &got));
  TEST_ASSERT_EQ(3U, got);
  TEST_ASSERT_EQ(0x12345, buf[0]);

  /* Negative sample (bit19 set) -> sign extended to -1. */
  reg->PDDSR = 1U;
  reg->PDDRR = k_t_sample_neg1;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdm_read(k_test_ch, buf, 8U, &got));
  TEST_ASSERT_EQ(1U, got);
  TEST_ASSERT_EQ(-1, buf[0]);

  /* FIFO count (10) above capacity (4) -> capped. */
  reg->PDDSR = k_t_fifo_over_cap;
  reg->PDDRR = k_t_sample_min; /* -524288 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdm_read(k_test_ch, buf, 4U, &got));
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdm_init());
  int32_t  buf[4] = {};
  uint32_t got    = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_pdm_read(k_test_ch, nullptr, 4U, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_pdm_read(k_test_ch, buf, 4U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_pdm_read((uint8_t)k_ra8_pdm_ch_count, buf, 4U, &got));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_pdm_read(k_test_ch, buf, 0U, &got));
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdm_init());

  /* STATE bit clear -> channel reports halted immediately. */
  ra8_pdm()->PDCSR = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdm_stop(k_test_ch));
  TEST_ASSERT_EQ((1U << k_test_ch), ra8_pdm()->PDCSTPTR);
  TEST_ASSERT_EQ(0U, ra8_pdm_ch(k_test_ch)->PDDRCR);

  /* STATE bit stuck set -> bounded poll expires with timeout. */
  ra8_pdm()->PDCSR = (uint32_t)(1U << k_test_ch);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_pdm_stop(k_test_ch));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_pdm_stop((uint8_t)k_ra8_pdm_ch_count));
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
  (void)fprintf(stderr, "[OK  ] test_ra8_pdm.c\n");
  return 0;
}
