/**
 * @file test_app_gpt_edge_capture_count.c
 * @brief Integration test for examples/.../hw_pending/gpt_edge_capture_count.
 *
 * @details
 * Covers the two things the demo relies on:
 *   1. the app's capture-delta window predicate (``gpt_ecc_period_valid``),
 *      mirrored here and driven with a minimal MC/DC vector set, and
 *   2. the real driver call sequence the demo issues -- input capture
 *      (``ra8_gpt_capture_configure`` / ``ra8_gpt_capture_read``) and
 *      external pulse counting (``ra8_gpt_event_count_configure`` +
 *      ``ra8_gpt_read``) -- observed through the fake MMIO window.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_gpt.h"
#include "ra8_gpt_capture.h"
#include "ra8_gpt_regs.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum test_gpt_ecc_chan_t
 * @brief GPT channel ids used by the demo (capture + count).
 */
typedef enum : uint8_t {
  k_test_gpt_ecc_capture_channel = 0U, /**< Input-capture channel (GPT0). */
  k_test_gpt_ecc_count_channel   = 1U, /**< Event-count channel (GPT1).   */
} test_gpt_ecc_chan_t;

/**
 * @enum test_gpt_ecc_const_t
 * @brief Capture-window thresholds and stimulus values.
 *
 * @details ``min`` / ``max`` mirror ``k_gpt_ecc_min_valid_ticks`` /
 * ``k_gpt_ecc_max_valid_ticks`` in the app's main.c.
 */
typedef enum : uint32_t {
  k_test_gpt_ecc_min       = 0x00000064UL, /**< Glitch-reject floor (100).  */
  k_test_gpt_ecc_max       = 0x7FFFFFFFUL, /**< Wrap / stale ceiling.       */
  k_test_gpt_ecc_in_window = 0x00001000UL, /**< Valid delta (in window).    */
  k_test_gpt_ecc_too_small = 0x00000010UL, /**< Below floor (16 < 100).     */
  k_test_gpt_ecc_too_big   = 0x80000000UL, /**< Above ceiling.              */
  k_test_gpt_ecc_latch     = 0x0002ABCDUL, /**< GTCCRA capture stimulus.    */
  k_test_gpt_ecc_pulses    = 0x00000123UL, /**< GTCNT pulse-count stimulus. */
} test_gpt_ecc_const_t;

/**
 * @brief Reset the fake MMIO window and MSTP model before a test.
 * @pre The host MMIO substrate is linked into the test binary.
 * @pre ``ra8_mstp_init`` is safe to call repeatedly.
 * @post All GPT registers in the window read as zero.
 * @post The MSTP model is initialized.
 */
static void prep_ecc(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
}

/**
 * @brief Mirror of the app's ``gpt_ecc_period_valid`` window predicate.
 *
 * @details Kept byte-for-byte in step with main.c: a captured delta is a
 * genuine period only inside [min, max]. Example main.c is not linked
 * into the host test image, so the decision is mirrored here to carry
 * its MC/DC vectors.
 *
 * @param[in] delta_ticks Candidate capture delta.
 * @return true when ``min <= delta_ticks <= max``.
 * @pre ``delta_ticks`` is an unsigned tick difference.
 * @post No state is modified (pure predicate).
 */
static bool mirror_period_valid(uint32_t delta_ticks)
{
  return (delta_ticks >= (uint32_t)k_test_gpt_ecc_min) &&
         (delta_ticks <= (uint32_t)k_test_gpt_ecc_max);
}

/**
 * @test test_mcdc_period_valid_window
 *
 * @par MC/DC:
 * Decision: `(delta >= min) && (delta <= max)` (2 conditions, in
 * ``gpt_ecc_period_valid``).
 * - Vector 1: delta=in_window -> T,T decision T (control: both true).
 * - Vector 2: delta=too_small -> F,_ decision F (varies C1; C2 masked by
 *   short-circuit).
 * - Vector 3: delta=too_big   -> T,F decision F (C1 held T, varies C2).
 * Vectors 1+2 prove C1 independently affects the outcome; 1+3 prove the
 * same for C2. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 */
static void test_mcdc_period_valid_window(void)
{
  TEST_BEGIN("gpt_ecc period_valid MC/DC: delta>=min && delta<=max");
  prep_ecc();

  TEST_ASSERT(mirror_period_valid((uint32_t)k_test_gpt_ecc_in_window));  /* V1: T,T -> T */
  TEST_ASSERT(!mirror_period_valid((uint32_t)k_test_gpt_ecc_too_small)); /* V2: F,_ -> F */
  TEST_ASSERT(!mirror_period_valid((uint32_t)k_test_gpt_ecc_too_big));   /* V3: T,F -> F */
  TEST_END("gpt_ecc period_valid MC/DC: delta>=min && delta<=max");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- replays the demo's input-capture
 * driver sequence: arm GTICASR, latch a value, read it back)
 */
static void test_capture_sequence(void)
{
  TEST_BEGIN("gpt_ecc input-capture driver sequence");
  prep_ecc();

  /* Arm rising-edge capture into GTCCRA (mirrors gpt_ecc_timers_or_halt). */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gpt_capture_configure((uint8_t)k_test_gpt_ecc_capture_channel,
                                           k_ra8_gpt_ccr_a,
                                           (uint32_t)k_ra8_gpt_cap_src_ioca_rising));
  volatile r_gpt_channel_regs_t* reg = ra8_gpt((uint8_t)k_test_gpt_ecc_capture_channel);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ(k_ra8_gpt_cap_src_ioca_rising, reg->GTICASR);

  /* Simulate a hardware latch and read it back via the driver. */
  reg->GTCCR[0]  = (uint32_t)k_test_gpt_ecc_latch;
  uint32_t latch = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gpt_capture_read((uint8_t)k_test_gpt_ecc_capture_channel, k_ra8_gpt_ccr_a, &latch));
  TEST_ASSERT_EQ(k_test_gpt_ecc_latch, latch);
  TEST_END("gpt_ecc input-capture driver sequence");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- replays the demo's pulse-count
 * driver sequence: arm GTUPSR up-source, read accumulated GTCNT)
 */
static void test_count_sequence(void)
{
  TEST_BEGIN("gpt_ecc event-count driver sequence");
  prep_ecc();

  /* Pulse counter: up on GTIOC1A rising, no down source. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gpt_event_count_configure((uint8_t)k_test_gpt_ecc_count_channel,
                                               (uint32_t)k_ra8_gpt_cnt_src_ioca_rising,
                                               (uint32_t)k_ra8_gpt_cnt_src_none));
  volatile r_gpt_channel_regs_t* reg = ra8_gpt((uint8_t)k_test_gpt_ecc_count_channel);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ(k_ra8_gpt_cnt_src_ioca_rising, reg->GTUPSR);
  TEST_ASSERT_EQ(0U, reg->GTDNSR);

  /* Simulate accumulated edges in GTCNT and read them back. */
  reg->GTCNT      = (uint32_t)k_test_gpt_ecc_pulses;
  uint32_t pulses = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gpt_read((uint8_t)k_test_gpt_ecc_count_channel, &pulses));
  TEST_ASSERT_EQ(k_test_gpt_ecc_pulses, pulses);
  TEST_END("gpt_ecc event-count driver sequence");
}

int main(void)
{
  test_mcdc_period_valid_window();
  test_capture_sequence();
  test_count_sequence();
  return 0;
}
