/**
 * @file test_ra8_pdm.c
 * @brief Unit tests for the PDM-IF driver, board mic policy and audio source
 *
 * @details
 * Exercises the real PDM-IF register model against the ``ra8_fake_mmap``
 * peripheral window: configuration writes, the start / read-enable /
 * read / stop sequence (HUM Ch 49.4), 20-bit sign extension and the
 * argument-validation contract. The receive FIFO is emulated by
 * pre-loading the channel's PDDSR (fill count) and PDDRR (sample word)
 * in mapped memory.
 *
 * The same fixture drives ``ra8_audio_source_pdm.c``, the audio-source
 * adapter stacked on this driver, because it is only observable through PDM
 * registers: bring-up order, polled capture, interrupt frame assembly and
 * teardown. Its failure legs come from the ``ra8_fake_mmio`` fault seam (the
 * module-stop read-back never settles) and from an empty FIFO (the bounded
 * poll expires).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_audio.h"
#include "ra8_audio_source_pdm.h"
#include "ra8_err.h"
#include "ra8_fake_irq.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_fake_time.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"
#include "ra8_pdm.h"
#include "ra8_pdm_regs.h"
#include "ra8_pin_validator.h"
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
 * @details Disarms the MMIO fault seam, reinitializes fake MMIO, module-stop,
 *          pin ownership and ISR state, then clears the callback counters.
 * @pre Fake MMIO support is available to the host test.
 * @pre No fake interrupt is executing concurrently.
 * @post Peripheral registers return to reset state and no fault is armed.
 * @post Stream counters, retained sample and pin claims are cleared.
 * @note Each PDM test invokes this helper before touching registers.
 * @since 0.1.0
 */
static void prep(void)
{
  ra8_fake_mmio_reset();
  ra8_fake_mmap_reset();
  ra8_pin_validator_reset();
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

/* PDM audio source adapter -- libs/ra8_audio/src/ra8_audio_source_pdm.c. */

/**
 * @enum t_pdm_source_geom_t
 * @brief Frame geometry and FIFO fill levels used by the audio-source vectors.
 *
 * @details
 * Four-sample frames keep one PDM interrupt able to deliver a complete frame
 * plus a remainder (``k_t_src_burst`` = 6), which is exactly the case the
 * stream assembler must carry across interrupts. ``k_t_src_overflow`` is the
 * smallest sample count whose PCM-S32LE byte size exceeds 32 bits.
 */
typedef enum : uint32_t {
  k_t_src_rate_hz   = 16000U,      /**< Decimated PCM sample rate.            */
  k_t_src_samples   = 4U,          /**< Samples per published frame.          */
  k_t_src_bytes     = 16U,         /**< PCM-S32LE bytes per frame.            */
  k_t_src_attempts  = 4U,          /**< Bounded FIFO-read attempts per frame. */
  k_t_src_discards  = 2U,          /**< Startup samples discarded at init.    */
  k_t_src_burst     = 6U,          /**< FIFO fill: one frame plus remainder.  */
  k_t_src_remainder = 2U,          /**< FIFO fill completing the next frame.  */
  k_t_src_overflow  = 0x40000000U, /**< Sample count overflowing 32-bit size. */
  k_t_src_ts_first  = 5U,          /**< Milliseconds before the 1st delivery. */
  k_t_src_ts_second = 7U,          /**< Further milliseconds before the 2nd.  */
} t_pdm_source_geom_t;

/**
 * @enum t_pdm_source_width_t
 * @brief Byte-wide audio-source configuration values.
 *
 * @details
 * ``k_t_src_misalign`` is the one-byte offset applied to an ``int32_t`` array
 * to build a buffer the adapter must reject as unaligned for PCM-S32LE.
 */
typedef enum : uint8_t {
  k_t_src_valid_bits = 20U, /**< PDM-IF significant PCM bits.        */
  k_t_src_priority   = 5U,  /**< Accepted stream NVIC priority.      */
  k_t_src_settle_ms  = 2U,  /**< Microphone settling delay.          */
  k_t_src_misalign   = 1U,  /**< Byte offset breaking S32 alignment. */
} t_pdm_source_width_t;

/** @brief Number of complete frames the audio-source callback has received. */
static uint32_t s_frame_calls;
/** @brief Descriptor of the most recently delivered complete frame. */
static ra8_audio_frame_t s_frame_last;
/** @brief Copy of the most recently delivered frame's samples. */
static int32_t s_frame_samples[k_t_src_samples];

/**
 * @brief Record one complete audio frame delivered by the PDM source.
 * @details Copies the frame descriptor and its samples out of the borrowed
 *          assembly buffer so the test can assert on them after the callback.
 * @param[in,out] ctx Expected frame-counter address.
 * @param[in] frame Borrowed complete PCM frame.
 * @pre `ctx` equals the fixture frame-counter address.
 * @pre `frame->data` holds exactly ::k_t_src_bytes readable bytes.
 * @post The delivered-frame counter is incremented.
 * @post The frame descriptor and samples are retained for assertions.
 * @note Runs in the host fake-IRQ context.
 * @since 0.1.0
 */
RA8_INTERNAL static void frame_callback(void* ctx, const ra8_audio_frame_t* frame)
{
  TEST_ASSERT(ctx == &s_frame_calls);
  s_frame_calls += 1U;
  s_frame_last = *frame;
  (void)memcpy(s_frame_samples, frame->data, sizeof(s_frame_samples));
}

/**
 * @brief Stage the channel-2 receive FIFO with a fill count and sample word.
 * @details Writes PDDSR (fill level) and PDDRR (the word every pop returns) in
 *          mapped memory, which is how the fake peripheral models a FIFO.
 * @param[in] count FIFO fill level reported by PDDSR.
 * @param[in] word Raw 20-bit sample word returned by every PDDRR read.
 * @pre The fake peripheral window is mapped (see ::prep).
 * @pre No fake interrupt is draining the FIFO concurrently.
 * @post PDDSR reports `count` available samples.
 * @post Every PDDRR read returns `word` until re-staged.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
RA8_INTERNAL static void stage_fifo(uint32_t count, uint32_t word)
{
  ra8_pdm_ch(k_test_ch)->PDDSR = count;
  ra8_pdm_ch(k_test_ch)->PDDRR = word;
}

/**
 * @brief Populate a valid PDM audio-source configuration.
 * @details Binds the SPH0690-style filter set from ::make_cfg to the fixed
 *          four-sample 16 kHz mono frame geometry on the test channel.
 * @param[out] cfg Configuration to populate.
 * @pre `cfg` points to writable configuration storage.
 * @pre No source retains the configuration while it is populated.
 * @post Every bounded field holds a value the validator accepts.
 * @post The embedded HAL configuration is fully initialized.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
RA8_INTERNAL static void make_source_cfg(ra8_audio_source_pdm_cfg_t* cfg)
{
  *cfg                   = (ra8_audio_source_pdm_cfg_t){};
  cfg->sample_rate_hz    = (uint32_t)k_t_src_rate_hz;
  cfg->samples_per_frame = (uint32_t)k_t_src_samples;
  cfg->settle_ms         = (uint32_t)k_t_src_settle_ms;
  cfg->discard_samples   = (uint32_t)k_t_src_discards;
  cfg->poll_attempts     = (uint32_t)k_t_src_attempts;
  cfg->channel           = (uint8_t)k_test_ch;
  cfg->valid_bits        = (uint8_t)k_t_src_valid_bits;
  cfg->irq_priority      = (uint8_t)k_t_src_priority;
  make_cfg(&cfg->pdm);
}

/**
 * @brief Bring a PDM audio source up over a staged, non-empty FIFO.
 * @details Stages enough samples for the startup discard burst and asserts the
 *          full bring-up sequence succeeded, so callers can start from a
 *          running source without repeating the sequence.
 * @param[out] source Source handle bound on success.
 * @param[out] state Caller-owned backend state bound on success.
 * @param[in] priority NVIC priority recorded for later stream starts.
 * @pre The PDM block is in its reset state (see ::prep).
 * @pre `source` and `state` outlive every later operation on them.
 * @post The channel is started with FIFO reads enabled.
 * @post `state` reports an initialized, non-streaming source.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
RA8_INTERNAL static void
open_source(ra8_audio_source_t* source, ra8_audio_source_pdm_state_t* state, uint8_t priority)
{
  ra8_audio_source_pdm_cfg_t cfg = {};
  make_source_cfg(&cfg);
  cfg.irq_priority = priority;
  stage_fifo((uint32_t)k_t_src_samples, (uint32_t)k_t_sample_pos);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_audio_source_pdm_init(source, state, &cfg));
}

/**
 * @brief Verify PDM audio-source configuration rejection.
 * @details Drives the null-argument guards and every bounded geometry field the
 *          validator checks, then proves no rejected configuration reached the
 *          peripheral: its module-stop reference is still zero.
 * @pre Fake MMIO can be reset and the PDM block starts gated.
 * @pre Unity test accounting is initialized.
 * @post Each malformed configuration records its documented error.
 * @post The PDM block was never taken out of module stop.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @par MC/DC:
 * (no compound decisions -- the validator is a chain of single-condition
 * guards; each vector makes exactly one of them true)
 */
static void test_source_cfg_validation(void)
{
  TEST_BEGIN("pdm source config validation");
  prep();
  ra8_audio_source_t           source = {};
  ra8_audio_source_pdm_state_t state  = {};
  ra8_audio_source_pdm_cfg_t   cfg    = {};
  make_source_cfg(&cfg);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_audio_source_pdm_init(&source, nullptr, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_audio_source_pdm_init(&source, &state, nullptr));
  cfg.sample_rate_hz = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_audio_source_pdm_init(&source, &state, &cfg));
  make_source_cfg(&cfg);
  cfg.samples_per_frame = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_audio_source_pdm_init(&source, &state, &cfg));
  make_source_cfg(&cfg);
  cfg.poll_attempts = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_audio_source_pdm_init(&source, &state, &cfg));
  make_source_cfg(&cfg);
  cfg.valid_bits = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_audio_source_pdm_init(&source, &state, &cfg));
  make_source_cfg(&cfg);
  cfg.samples_per_frame = (uint32_t)k_t_src_overflow;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_audio_source_pdm_init(&source, &state, &cfg));
  TEST_ASSERT_EQ(0U, ra8_pdm()->PDCSTRTR);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_pdm_deinit());
  TEST_END("pdm source config validation");
}

/**
 * @brief Verify PDM source bring-up order and the reported source geometry.
 * @details Checks that init configured, started and read-enabled the channel,
 *          then that the published metadata matches the accepted configuration.
 * @pre Fake MMIO can be reset and the FIFO can be staged.
 * @pre Unity test accounting is initialized.
 * @post The channel is configured, started and read-enabled.
 * @post The reported geometry is one mono PCM-S32LE frame of the configured size.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @par MC/DC:
 * (no compound decisions -- the bring-up helper and the metadata query are
 * chains of single-condition guards, all taking their accepted arm here)
 */
static void test_source_init_and_info(void)
{
  TEST_BEGIN("pdm source init + info");
  prep();
  ra8_audio_source_t           source = {};
  ra8_audio_source_pdm_state_t state  = {};
  ra8_audio_source_pdm_cfg_t   cfg    = {};
  make_source_cfg(&cfg);
  stage_fifo((uint32_t)k_t_src_samples, (uint32_t)k_t_sample_pos);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_audio_source_pdm_init(&source, &state, &cfg));
  /* HUM Ch 49.4.1 "Start Flow": configure, start trigger, read enable. */
  TEST_ASSERT_EQ(0x00000040U, ra8_pdm_ch(k_test_ch)->PDMDSR); /* SFMD=4 << 4 */
  TEST_ASSERT_EQ((1U << k_test_ch), ra8_pdm()->PDCSTRTR);
  TEST_ASSERT_EQ((uint32_t)k_ra8_pdm_pddrcr_datre, ra8_pdm_ch(k_test_ch)->PDDRCR);

  ra8_audio_source_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_audio_source_get_info(&source, &info));
  TEST_ASSERT_EQ(k_t_src_bytes, info.frame_bytes);
  TEST_ASSERT_EQ(k_t_src_samples, info.samples_per_frame);
  TEST_ASSERT_EQ(k_t_src_rate_hz, info.sample_rate_hz);
  TEST_ASSERT_EQ(1U, info.channels);
  TEST_ASSERT_EQ(k_t_src_valid_bits, info.valid_bits);
  TEST_ASSERT_EQ(k_ra8_audio_format_pcm_s32le, info.format);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_audio_source_stop(&source));
  TEST_END("pdm source init + info");
}

/**
 * @brief Verify one polled PDM capture and its published frame descriptor.
 * @details Stages a negative 20-bit sample, captures one frame into caller
 *          storage and checks the descriptor, the capture-start timestamp and
 *          the sign-extended samples that landed in the buffer.
 * @pre Fake MMIO, fake time and the FIFO can be staged.
 * @pre Unity test accounting is initialized.
 * @post The frame aliases the caller buffer and matches the source geometry.
 * @post Every captured sample is the staged sign-extended value.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @par MC/DC:
 * (no compound decisions -- capture's size, alignment and fill-error guards
 * are single conditions, all taking their accepted arm here)
 */
static void test_source_capture(void)
{
  TEST_BEGIN("pdm source capture");
  prep();
  ra8_fake_time_reset();
  ra8_audio_source_t           source = {};
  ra8_audio_source_pdm_state_t state  = {};
  open_source(&source, &state, (uint8_t)k_t_src_priority);

  int32_t                  pcm[k_t_src_samples] = {};
  const ra8_audio_buffer_t buffer               = {
    .data     = pcm,
    .capacity = (uint32_t)sizeof(pcm),
  };
  ra8_audio_frame_t frame = {};
  ra8_fake_time_advance_ms((uint32_t)k_t_src_ts_first);
  stage_fifo((uint32_t)k_t_src_samples, (uint32_t)k_t_sample_neg1);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_audio_source_capture(&source, &buffer, &frame));
  TEST_ASSERT(frame.data == pcm);
  TEST_ASSERT_EQ(k_t_src_bytes, frame.bytes);
  TEST_ASSERT_EQ(k_t_src_samples, frame.sample_count);
  TEST_ASSERT_EQ(k_t_src_rate_hz, frame.sample_rate_hz);
  TEST_ASSERT_EQ(k_t_src_ts_first, frame.timestamp_ms);
  TEST_ASSERT_EQ(1U, frame.channels);
  TEST_ASSERT_EQ(k_t_src_valid_bits, frame.valid_bits);
  TEST_ASSERT_EQ(k_ra8_audio_format_pcm_s32le, frame.format);
  TEST_ASSERT_EQ(-1, pcm[0]);
  TEST_ASSERT_EQ(-1, pcm[k_t_src_samples - 1U]);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_audio_source_stop(&source));
  TEST_END("pdm source capture");
}

/**
 * @brief Verify the polled-capture rejection and error-propagation legs.
 * @details Drives the unaligned-buffer guard, the silent-microphone timeout
 *          from the bounded FIFO poll, and a corrupted channel binding whose
 *          HAL rejection must surface unchanged.
 * @pre Fake MMIO and the FIFO can be staged.
 * @pre Unity test accounting is initialized.
 * @post Each rejected capture records its documented error.
 * @post No rejected capture publishes a frame descriptor.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @par MC/DC:
 * (no compound decisions -- the alignment guard and the fill-error guard are
 * single conditions; this test supplies the taken vector for each, with the
 * not-taken vectors supplied by test_source_capture)
 */
static void test_source_capture_faults(void)
{
  TEST_BEGIN("pdm source capture faults");
  prep();
  ra8_audio_source_t           source = {};
  ra8_audio_source_pdm_state_t state  = {};
  open_source(&source, &state, (uint8_t)k_t_src_priority);

  int32_t                  pcm[k_t_src_samples + 1U] = {};
  const ra8_audio_buffer_t misaligned                = {
    .data     = (void*)((uint8_t*)pcm + (uintptr_t)k_t_src_misalign),
    .capacity = (uint32_t)k_t_src_bytes,
  };
  const ra8_audio_buffer_t buffer = {
    .data     = pcm,
    .capacity = (uint32_t)k_t_src_bytes,
  };
  ra8_audio_frame_t frame = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_audio_source_capture(&source, &misaligned, &frame));

  /* Silent microphone: the FIFO never fills and the bounded poll expires. */
  stage_fifo(0U, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_audio_source_capture(&source, &buffer, &frame));
  TEST_ASSERT_NULL(frame.data);

  /* A corrupted channel binding surfaces the HAL rejection unchanged. */
  stage_fifo((uint32_t)k_t_src_samples, (uint32_t)k_t_sample_pos);
  state.channel = (uint8_t)k_ra8_pdm_ch_count;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_audio_source_capture(&source, &buffer, &frame));
  state.channel = (uint8_t)k_test_ch;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_audio_source_stop(&source));
  TEST_END("pdm source capture faults");
}

/**
 * @brief Verify PDM source teardown, its failure legs and post-stop rejection.
 * @details Covers a channel stuck running (HAL stop timeout), a stolen
 *          module-stop reference (deinit failure), the clean stop, and the
 *          rejection of a stopped or unbound backend context.
 * @pre Fake MMIO can be reset and the FIFO can be staged.
 * @pre Unity test accounting is initialized.
 * @post A failed stop leaves the source initialized for a retry.
 * @post A successful stop clears the handle and the initialized flag.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @par MC/DC:
 * (no compound decisions -- the stop path's context, initialized, stop-error
 * and deinit-error tests are each a single condition; this test drives both
 * arms of all four)
 */
static void test_source_stop_paths(void)
{
  TEST_BEGIN("pdm source stop paths");
  prep();
  ra8_audio_source_t           source = {};
  ra8_audio_source_pdm_state_t state  = {};
  open_source(&source, &state, (uint8_t)k_t_src_priority);
  ra8_audio_source_t bound = source;

  /* HUM Ch 49.2.6 "PDCSR : Channel Status Register" p 3199 -- STATE stuck
   * set, so the HAL stop poll expires and the source stays initialized. */
  ra8_pdm()->PDCSR = (uint32_t)(1U << k_test_ch);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_audio_source_stop(&source));
  TEST_ASSERT(state.initialized);

  /* Module-stop reference stolen: the channel stops but the release fails,
   * which must leave the source initialized rather than half-torn-down. */
  ra8_pdm()->PDCSR = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdm_deinit());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_audio_source_stop(&source));
  TEST_ASSERT(state.initialized);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_pdm_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_audio_source_stop(&source));
  TEST_ASSERT(!state.initialized);
  TEST_ASSERT_NULL(source.iface);

  ra8_audio_source_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_audio_source_stop(&bound));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_audio_source_get_info(&bound, &info));

  ra8_audio_source_t detached = bound;
  detached.ctx                = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_audio_source_stop(&detached));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_audio_source_get_info(&detached, &info));
  TEST_END("pdm source stop paths");
}

/**
 * @brief Verify interrupt frame assembly across two FIFO deliveries.
 * @details Starts a stream, delivers six samples so one frame completes and two
 *          are carried over, then delivers two more to complete the next frame.
 *          Pins the carried samples and proves the frame timestamp is taken
 *          when a frame's first sample arrives, not when it is published.
 * @pre Fake MMIO, fake time and the fake ELC event source are available.
 * @pre Unity test accounting is initialized.
 * @post Exactly one frame is published per completed sample count.
 * @post A span arriving while streaming is cleared publishes nothing.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @par MC/DC:
 * (no compound decisions -- the assembler's streaming guard, its
 * first-sample timestamp test, the copy-size ternary and the frame-complete
 * test are each a single condition; this test drives both arms of all four)
 */
static void test_source_stream_delivery(void)
{
  TEST_BEGIN("pdm source stream delivery");
  prep();
  ra8_fake_time_reset();
  ra8_audio_source_t           source = {};
  ra8_audio_source_pdm_state_t state  = {};
  open_source(&source, &state, (uint8_t)k_t_src_priority);

  int32_t                  assembly[k_t_src_samples] = {};
  const ra8_audio_buffer_t buffer                    = {
    .data     = assembly,
    .capacity = (uint32_t)sizeof(assembly),
  };
  s_frame_calls = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_audio_source_stream_start(&source, &buffer, frame_callback, &s_frame_calls));
  /* HUM Ch 49.2.15 "PDICRCHn : Interrupt Control Register" p 3205 */
  TEST_ASSERT_EQ(k_ra8_pdm_pdicr_idre,
                 ra8_pdm_ch(k_test_ch)->PDICR & (uint32_t)k_ra8_pdm_pdicr_idre);
  TEST_ASSERT_EQ(k_ra8_err_exists,
                 ra8_audio_source_stream_start(&source, &buffer, frame_callback, &s_frame_calls));

  ra8_fake_time_advance_ms((uint32_t)k_t_src_ts_first);
  stage_fifo((uint32_t)k_t_src_burst, (uint32_t)k_t_sample_pos);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_irq_fire(k_ra8_elc_event_pdm_dat2));
  TEST_ASSERT_EQ(1U, s_frame_calls);
  TEST_ASSERT_EQ(k_t_src_bytes, s_frame_last.bytes);
  TEST_ASSERT_EQ(k_t_src_samples, s_frame_last.sample_count);
  TEST_ASSERT_EQ(k_t_src_ts_first, s_frame_last.timestamp_ms);
  TEST_ASSERT_EQ(k_t_sample_pos, s_frame_samples[k_t_src_samples - 1U]);

  /* The two carried samples complete the next frame, whose timestamp was
   * taken during the previous interrupt. */
  ra8_fake_time_advance_ms((uint32_t)k_t_src_ts_second);
  stage_fifo((uint32_t)k_t_src_remainder, (uint32_t)k_t_sample_neg1);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_irq_fire(k_ra8_elc_event_pdm_dat2));
  TEST_ASSERT_EQ(2U, s_frame_calls);
  TEST_ASSERT_EQ(k_t_src_ts_first, s_frame_last.timestamp_ms);
  TEST_ASSERT_EQ(k_t_sample_pos, s_frame_samples[1]);
  TEST_ASSERT_EQ(-1, s_frame_samples[2]);

  /* A span delivered after streaming was cleared is dropped. */
  state.streaming = false;
  stage_fifo((uint32_t)k_t_src_burst, (uint32_t)k_t_sample_pos);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_irq_fire(k_ra8_elc_event_pdm_dat2));
  TEST_ASSERT_EQ(2U, s_frame_calls);
  state.streaming = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_audio_source_stop(&source));
  TEST_ASSERT_EQ(0U, ra8_pdm_ch(k_test_ch)->PDICR & (uint32_t)k_ra8_pdm_pdicr_idre);
  TEST_END("pdm source stream delivery");
}

/**
 * @brief Verify stream-start rejection and its retained-pointer rollback.
 * @details Rejects an unaligned assembly buffer, then makes the HAL reject the
 *          recorded NVIC priority and proves nothing was latched: after fixing
 *          the priority the retry starts a stream instead of reporting one
 *          already exists.
 * @pre Fake MMIO can be reset and the FIFO can be staged.
 * @pre Unity test accounting is initialized.
 * @post A rejected stream start leaves no retained buffer, callback or context.
 * @post The data-ready interrupt stays disabled until a start succeeds.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @par MC/DC:
 * (no compound decisions -- the alignment guard and the HAL-error rollback
 * test are single conditions; this test supplies the taken vector for each,
 * with the not-taken vectors supplied by test_source_stream_delivery)
 */
static void test_source_stream_start_faults(void)
{
  TEST_BEGIN("pdm source stream start faults");
  prep();
  ra8_audio_source_t           source = {};
  ra8_audio_source_pdm_state_t state  = {};
  open_source(&source, &state, (uint8_t)UINT8_MAX);

  int32_t                  assembly[k_t_src_samples + 1U] = {};
  const ra8_audio_buffer_t misaligned                     = {
    .data     = (void*)((uint8_t*)assembly + (uintptr_t)k_t_src_misalign),
    .capacity = (uint32_t)k_t_src_bytes,
  };
  const ra8_audio_buffer_t buffer = {
    .data     = assembly,
    .capacity = (uint32_t)k_t_src_bytes,
  };
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_audio_source_stream_start(&source, &misaligned, frame_callback, &s_frame_calls));
  TEST_ASSERT(!state.streaming);

  /* The HAL rejects the recorded priority: every retained pointer must be
   * rolled back, including the streaming flag. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_audio_source_stream_start(&source, &buffer, frame_callback, &s_frame_calls));
  TEST_ASSERT(!state.streaming);
  TEST_ASSERT_NULL(state.stream_data);
  TEST_ASSERT(state.stream_callback == nullptr);
  TEST_ASSERT_NULL(state.stream_ctx);
  TEST_ASSERT_EQ(0U, ra8_pdm_ch(k_test_ch)->PDICR & (uint32_t)k_ra8_pdm_pdicr_idre);

  state.irq_priority = (uint8_t)k_t_src_priority;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_audio_source_stream_start(&source, &buffer, frame_callback, &s_frame_calls));
  TEST_ASSERT(state.streaming);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_audio_source_stop(&source));
  TEST_END("pdm source stream start faults");
}

/**
 * @brief Verify PDM source bring-up failure legs and their cleanup.
 * @details Arms the module-stop read-back so the PDM block never leaves module
 *          stop, then leaves the FIFO empty so the startup discard burst times
 *          out, and checks that the second leg stopped the channel and
 *          released the module before returning.
 * @pre The fake-MMIO fault seam and fake MMIO can be reset.
 * @pre Unity test accounting is initialized.
 * @post A failed module-stop release configures no channel register.
 * @post A failed discard burst leaves the channel stopped and the module released.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @par MC/DC:
 * (no compound decisions -- the bring-up helper's init-error, start-error,
 * discard-error and cleanup tests are each a single condition; this test
 * supplies the taken vector for the init-error and discard-error legs)
 */
static void test_source_prepare_faults(void)
{
  TEST_BEGIN("pdm source prepare faults");
  prep();
  ra8_audio_source_t           source = {};
  ra8_audio_source_pdm_state_t state  = {};
  ra8_audio_source_pdm_cfg_t   cfg    = {};
  make_source_cfg(&cfg);

  /* MSTPCRC read-back never settles: the block never leaves module stop. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait(&ra8_mstp()->MSTPCRC));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_audio_source_pdm_init(&source, &state, &cfg));
  TEST_ASSERT_EQ(0U, ra8_pdm_ch(k_test_ch)->PDMDSR);
  TEST_ASSERT_EQ(0U, ra8_pdm()->PDCSTRTR);
  TEST_ASSERT(!state.initialized);
  TEST_ASSERT_NULL(source.iface);
  ra8_fake_mmio_reset();

  /* Silent microphone during the startup discard burst: the bounded poll
   * expires, and the channel is stopped and the module released. */
  stage_fifo(0U, 0U);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_audio_source_pdm_init(&source, &state, &cfg));
  TEST_ASSERT_EQ((1U << k_test_ch), ra8_pdm()->PDCSTPTR);
  TEST_ASSERT(!state.initialized);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_pdm_deinit());
  TEST_END("pdm source prepare faults");
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
  test_source_cfg_validation();
  test_source_init_and_info();
  test_source_capture();
  test_source_capture_faults();
  test_source_stop_paths();
  test_source_stream_delivery();
  test_source_stream_start_faults();
  test_source_prepare_faults();
  return 0;
}
