/**
 * @file test_ra8_pdm.c
 * @brief Unit tests for ra8_pdm.c (PDM-IF capture driver)
 *
 * @details
 * Exercises the real PDM-IF register model against the ``ra8_fake_mmap``
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
#include "ra8_fake_irq.h"
#include "ra8_fake_mmap.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_pdm.h"
#include "ra8_pdm_regs.h"
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
  k_t_sinc_dec   = 0x7CU,   /**< SINC decimation factor.             */
  k_t_sinc_range = 0x05U,   /**< SINC output range select.           */
  k_t_hpf_s0     = 0x3F61U, /**< High-pass shift coefficient s0.     */
  k_t_hpf_k1     = 0x3EC1U, /**< High-pass feedback coefficient k1.  */
  k_t_hpf_h0     = 0x4000U, /**< High-pass FIR tap 0: +1.0 in Q1.14. */
  k_t_hpf_h1     = 0xC000U, /**< High-pass FIR tap 1: -1.0 in Q1.14. */
  k_t_comp_tap   = 0x1FE8U, /**< Compensation-filter tap; written to the
                                   first and last slot to prove the whole
                                   array is marshalled.                       */
  k_t_lpf_h0     = 0x0400U, /**< Low-pass leading tap.               */
  k_t_lpf_tap    = 0x1FF8U, /**< Low-pass tap, first and last slot.  */
  k_t_comp_last  = 10U,     /**< Last compensation-filter tap index. */
  k_t_lpf_last   = 19U,     /**< Last low-pass tap index.            */
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
  k_t_sample_pos    = 0x12345U, /**< A positive 20-bit sample.               */
  k_t_sample_neg1   = 0xFFFFFU, /**< All ones: sign-extends to -1.           */
  k_t_sample_min    = 0x80000U, /**< Sign bit only: sign-extends to -524288. */
} t_pdm_sample_t;

/** @brief Channel under test (EK-RA8D2 MEMS mic wiring). */
enum : uint8_t {
  k_test_ch = 2U, /**< Test channel. */
};

static uint32_t s_stream_calls;
static uint32_t s_stream_samples;
static int32_t  s_stream_first;

/**
 * @brief Record one interrupt-delivered PDM FIFO view.
 * @details Accumulates callback and sample counts and retains the first sample.
 * @param[in,out] ctx Expected callback-counter address.
 * @param[in] samples Borrowed FIFO sample span.
 * @param[in] count Number of samples in the span.
 * @pre `ctx` equals the fixture callback-counter address.
 * @pre `samples` contains at least one value when invoked.
 * @post Callback and sample totals are incremented.
 * @post The first delivered sample is retained.
 * @note Assertions execute in the host fake-IRQ context.
 * @since 0.1.0
 */
static void stream_callback(void* ctx, const int32_t* samples, uint32_t count)
{
  TEST_ASSERT(ctx == &s_stream_calls);
  s_stream_calls += 1U;
  s_stream_samples += count;
  s_stream_first = samples[0];
}

/**
 * @brief Reset fake hardware and PDM stream fixtures.
 * @details Reinitializes fake MMIO, module-stop, ISR, and callback state.
 * @pre Fake MMIO support is available to the host test.
 * @pre No fake interrupt is executing concurrently.
 * @post Peripheral registers return to reset state.
 * @post Stream counters and retained sample are zero.
 * @note Each PDM test invokes this helper before touching registers.
 * @since 0.1.0
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_isr_init();
  s_stream_calls   = 0U;
  s_stream_samples = 0U;
  s_stream_first   = 0;
}

/**
 * @brief Fill a config with a valid order-4 / 16 kHz SPH0690-style set.
 * @details Populates every register field observed by the configuration test
 *          with distinct, deterministic filter values.
 *
 * @param[out] cfg Config to populate.
 * @pre `cfg` points to writable configuration storage.
 * @pre No driver retains the configuration while it is populated.
 * @post Every tested coefficient is initialized deterministically.
 * @post The structure contains a valid channel configuration.
 * @note Coefficients are representative register fixtures, not acoustic tuning.
 * @since 0.1.0
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
 * @brief Verify the PDM peripheral lifecycle happy path.
 * @details Initializes and deinitializes the fake peripheral once.
 * @pre Fake MMIO and ISR infrastructure can be reset.
 * @pre Unity test accounting is initialized.
 * @post Initialization and deinitialization both report success.
 * @post No active PDM stream remains.
 * @note This test covers the lifecycle without channel configuration.
 * @since 0.1.0
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
 * @brief Verify channel configuration writes and argument guards.
 * @details Checks representative filter registers, null config, and channel range.
 * @pre Fake MMIO and ISR infrastructure can be reset.
 * @pre Unity test accounting is initialized.
 * @post Expected configuration values appear in fake registers.
 * @post Invalid arguments record their documented errors.
 * @note Coefficient placement detects array-marshalling regressions.
 * @since 0.1.0
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
 * @brief Verify PDM start and FIFO-read enable operations.
 * @details Checks trigger registers and invalid-channel guards.
 * @pre Fake MMIO and ISR infrastructure can be reset.
 * @pre Unity test accounting is initialized.
 * @post Start and read-enable bits appear in fake registers.
 * @post Out-of-range calls record invalid-argument errors.
 * @note The fake peripheral does not autonomously change state.
 * @since 0.1.0
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
 * @brief Verify sample reads, count capping, and sign extension.
 * @details Stages positive and negative 20-bit samples in the fake FIFO.
 * @pre Fake MMIO and ISR infrastructure can be reset.
 * @pre Unity test accounting is initialized.
 * @post Returned sample counts respect FIFO and buffer bounds.
 * @post Negative 20-bit samples are sign-extended to 32 bits.
 * @note Repeated fake-register reads intentionally yield the staged word.
 * @since 0.1.0
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
 * @brief Verify PDM read argument validation.
 * @details Covers null buffers, null counts, invalid channel, and zero length.
 * @pre Fake MMIO and ISR infrastructure can be reset.
 * @pre Unity test accounting is initialized.
 * @post Each malformed call records its documented error.
 * @post No caller sample buffer is consumed.
 * @note The peripheral is initialized so argument guards are isolated.
 * @since 0.1.0
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
 * @brief Verify bounded PDM stop behavior.
 * @details Covers immediate halt, stuck-state timeout, and invalid channel.
 * @pre Fake MMIO and ISR infrastructure can be reset.
 * @pre Unity test accounting is initialized.
 * @post A halted channel disables reads and records its stop trigger.
 * @post A stuck state reports the hardware timeout.
 * @note Fake MMIO supplies both channel-state conditions directly.
 * @since 0.1.0
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

/**
 * @brief Verify PDM DAT2 routing, bounded FIFO drain, and teardown.
 * @details Exercises callback registration, duplicate start, fake IRQ, and disable paths.
 * @par MC/DC:
 * A valid callback and priority provide the accepted baseline. Null callback,
 * invalid priority, duplicate enable, and disabled-state vectors independently
 * vary the stream-registration and dispatch guards.
 * @pre Fake MMIO and ISR infrastructure can be reset.
 * @pre Unity test accounting is initialized.
 * @post One fired interrupt delivers the staged FIFO samples exactly once.
 * @post Stream disable clears the data-ready interrupt bit.
 * @note The fixture fires the registered ELC event synchronously.
 * @since 0.1.0
 */
static void test_interrupt_stream(void)
{
  TEST_BEGIN("pdm interrupt stream");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdm_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdm_read_enable(k_test_ch));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_pdm_stream_enable(k_test_ch, nullptr, nullptr, 5U));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_pdm_stream_enable(k_test_ch, stream_callback, &s_stream_calls, (uint8_t)UINT8_MAX));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdm_stream_enable(k_test_ch, stream_callback, &s_stream_calls, 5U));
  /* HUM Ch 49.2.15 "PDICRCHn : Interrupt Control Register" p 3205 */
  TEST_ASSERT_EQ(k_ra8_pdm_pdicr_idre,
                 ra8_pdm_ch(k_test_ch)->PDICR & (uint32_t)k_ra8_pdm_pdicr_idre);
  TEST_ASSERT_EQ(k_ra8_err_exists,
                 ra8_pdm_stream_enable(k_test_ch, stream_callback, &s_stream_calls, 5U));
  /* HUM Ch 49.2.66 "PDDSRCHn : Data Status Register" p 3228 */
  ra8_pdm_ch(k_test_ch)->PDDSR = 3U;
  /* HUM Ch 49.2.65 "PDDRRCHn : Data Read Register" p 3227 */
  ra8_pdm_ch(k_test_ch)->PDDRR = (uint32_t)k_t_sample_neg1;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_irq_fire(k_ra8_elc_event_pdm_dat2));
  TEST_ASSERT_EQ(1U, s_stream_calls);
  TEST_ASSERT_EQ(3U, s_stream_samples);
  TEST_ASSERT_EQ(-1, s_stream_first);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdm_stream_disable(k_test_ch));
  /* HUM Ch 49.2.15 "PDICRCHn : Interrupt Control Register" p 3205 */
  TEST_ASSERT_EQ(0U, ra8_pdm_ch(k_test_ch)->PDICR & (uint32_t)k_ra8_pdm_pdicr_idre);
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_pdm_stream_disable(k_test_ch));
  TEST_END("pdm interrupt stream");
}

int main(void)
{
  test_init_deinit();
  test_configure();
  test_start_and_read_enable();
  test_read_samples();
  test_read_validation();
  test_interrupt_stream();
  test_stop();
  return 0;
}
