/**
 * @file test_ra8_agt.c
 * @brief Unit tests for ra8_agt.c (Asynchronous General-Purpose Timer)
 * @details Covers AGT channel validation, timer configuration, counter access, and callback behavior through fake peripheral registers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_agt.h"
#include "ra8_agt_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_ra8_agt_test_channel_valid  = 0U,   /**< RA8 AGT test channel valid.  */
  k_ra8_agt_test_channel_middle = 5U,   /**< RA8 AGT test channel middle. */
  k_ra8_agt_test_channel_last   = 9U,   /**< RA8 AGT test channel last.   */
  k_ra8_agt_test_channel_bad    = 10U,  /**< RA8 AGT test channel bad.    */
  k_ra8_agt_test_channel_way    = 200U, /**< RA8 AGT test channel way.    */
} ra8_agt_test_channel_t;

typedef enum : uint16_t {
  k_ra8_agt_test_reload = 0x1234U, /**< RA8 AGT test reload. */
} ra8_agt_test_reload_t;

typedef enum : uint8_t {
  k_ra8_agt_test_tstart_bit = 0x01U, /**< RA8 AGT test tstart bit. */
} ra8_agt_test_bits_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_free_run_happy(void)
{
  TEST_BEGIN("agt start_free_run happy");
  ra8_fake_mmap_reset();

  const ra8_err_t err =
    ra8_agt_start_free_run((uint8_t)k_ra8_agt_test_channel_valid, (uint16_t)k_ra8_agt_test_reload);
  TEST_ASSERT_EQ(k_ra8_ok, err);

  volatile r_agt_regs_t* reg = ra8_agt((uint8_t)k_ra8_agt_test_channel_valid);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ(k_ra8_agt_test_reload, reg->AGT);
  TEST_ASSERT_EQ(k_ra8_agt_test_tstart_bit, reg->AGTCR);
  TEST_ASSERT_EQ(0, reg->AGTMR1);
  TEST_ASSERT_EQ(0, reg->AGTMR2);
  TEST_END("agt start_free_run happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_free_run_middle_channel(void)
{
  TEST_BEGIN("agt start_free_run middle channel");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_agt_start_free_run((uint8_t)k_ra8_agt_test_channel_middle,
                                        (uint16_t)k_ra8_agt_test_reload));
  volatile r_agt_regs_t* reg = ra8_agt((uint8_t)k_ra8_agt_test_channel_middle);
  TEST_ASSERT_EQ(k_ra8_agt_test_reload, reg->AGT);
  TEST_END("agt start_free_run middle channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_free_run_last_channel(void)
{
  TEST_BEGIN("agt start_free_run last channel");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_agt_start_free_run((uint8_t)k_ra8_agt_test_channel_last, 0U));
  TEST_END("agt start_free_run last channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_free_run_bad_channel(void)
{
  TEST_BEGIN("agt start_free_run bad channel");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_agt_start_free_run((uint8_t)k_ra8_agt_test_channel_bad, 0U));
  TEST_END("agt start_free_run bad channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_start_free_run_huge_channel(void)
{
  TEST_BEGIN("agt start_free_run huge channel");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_agt_start_free_run((uint8_t)k_ra8_agt_test_channel_way, 0U));
  TEST_END("agt start_free_run huge channel");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_stop_happy(void)
{
  TEST_BEGIN("agt stop happy");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_agt_start_free_run((uint8_t)k_ra8_agt_test_channel_valid, (uint16_t)k_ra8_agt_test_reload));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_agt_stop((uint8_t)k_ra8_agt_test_channel_valid));

  volatile r_agt_regs_t* reg = ra8_agt((uint8_t)k_ra8_agt_test_channel_valid);
  TEST_ASSERT_EQ(0, reg->AGTCR);
  TEST_END("agt stop happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_stop_bad_channel(void)
{
  TEST_BEGIN("agt stop bad channel");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_agt_stop((uint8_t)k_ra8_agt_test_channel_bad));
  TEST_END("agt stop bad channel");
}

/* ---- full build-out ---- */

static uint32_t s_agt_cb_count;
static uint8_t  s_agt_cb_last_ch;

static void stub_agt_cb(void* ctx, uint8_t ch)
{
  (void)ctx;
  ++s_agt_cb_count;
  s_agt_cb_last_ch = ch;
}

static void prep_w43(void)
{
  ra8_fake_mmap_reset();
  /* Drain the per-channel AGT MSTP latch (ra8_agt's s_agt_mstp_held) so it stays
   * in sync with the ra8_mstp refcount that ra8_mstp_init() is about to zero.
   * Otherwise a stale held=true over a fresh refcount=0 makes a later
   * ra8_agt_deinit -> ra8_mstp_disable underflow and return invalid_state. */
  (void)ra8_agt_deinit((uint8_t)k_ra8_agt_test_channel_valid);
  (void)ra8_mstp_init();
  s_agt_cb_count   = 0U;
  s_agt_cb_last_ch = 0U;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("agt deinit");
  prep_w43();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_agt_start_free_run((uint8_t)k_ra8_agt_test_channel_valid, (uint16_t)k_ra8_agt_test_reload));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_agt_deinit((uint8_t)k_ra8_agt_test_channel_valid));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_agt_deinit((uint8_t)k_ra8_agt_test_channel_bad));
  TEST_END("agt deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_reload_and_status(void)
{
  TEST_BEGIN("agt set_reload + status");
  prep_w43();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_agt_set_reload((uint8_t)k_ra8_agt_test_channel_valid, 0xBEEFU));
  TEST_ASSERT_EQ(0xBEEFU, ra8_agt((uint8_t)k_ra8_agt_test_channel_valid)->AGT);

  uint8_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_agt_get_status((uint8_t)k_ra8_agt_test_channel_valid, &mask));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_agt_get_status((uint8_t)k_ra8_agt_test_channel_valid, nullptr));
  TEST_END("agt set_reload + status");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("agt attach + dispatch");
  prep_w43();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_agt_attach_handler(stub_agt_cb, (void*)(uintptr_t)0x88U));
  ra8_agt_dispatch((uint8_t)k_ra8_agt_test_channel_middle);
  TEST_ASSERT_EQ(1, s_agt_cb_count);
  TEST_ASSERT_EQ(k_ra8_agt_test_channel_middle, s_agt_cb_last_ch);

  ra8_agt_dispatch((uint8_t)k_ra8_agt_test_channel_bad);
  TEST_ASSERT_EQ(1, s_agt_cb_count);
  TEST_END("agt attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("agt power transition");
  prep_w43();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_agt_start_free_run((uint8_t)k_ra8_agt_test_channel_valid, (uint16_t)k_ra8_agt_test_reload));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_agt_enter_stop((uint8_t)k_ra8_agt_test_channel_valid));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_agt_exit_stop((uint8_t)k_ra8_agt_test_channel_valid));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_agt_enter_stop((uint8_t)k_ra8_agt_test_channel_bad));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_agt_exit_stop((uint8_t)k_ra8_agt_test_channel_bad));
  TEST_END("agt power transition");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_no_mstp_channel_power(void)
{
  TEST_BEGIN("agt no-mstp channel power");
  prep_w43();
  /* Channels >= 2 have no dedicated MSTP bit; deinit / enter_stop /
   * exit_stop should return OK without calling ra8_mstp_disable. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_agt_deinit((uint8_t)k_ra8_agt_test_channel_middle));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_agt_enter_stop((uint8_t)k_ra8_agt_test_channel_middle));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_agt_exit_stop((uint8_t)k_ra8_agt_test_channel_middle));
  TEST_END("agt no-mstp channel power");
}

/* ---- pulse-output mode ---- */

typedef enum : uint16_t {
  k_ra8_agt_test_pulse_period = 0x4000U, /**< RA8 AGT test pulse period. */
  k_ra8_agt_test_pulse_duty   = 0x2000U, /**< RA8 AGT test pulse duty.   */
} ra8_agt_test_pulse_const_t;

/**
 * @par MC/DC:
 * Decision: ``compare == k_ra8_agt_pulse_compare_a`` (atomic) inside
 * `agt_pulse_agtcmsr_value` + `agt_pulse_program_compare`. Three
 * pulse-mode test cases cover the three enum values -- none, A, B --
 * giving 100% branch coverage of the compare-selection split.
 */
static void test_start_pulse_output_compare_a(void)
{
  TEST_BEGIN("agt start_pulse_output compare A");
  prep_w43();
  const ra8_agt_pulse_cfg_t cfg = {
    .period   = (uint16_t)k_ra8_agt_test_pulse_period,
    .duty     = (uint16_t)k_ra8_agt_test_pulse_duty,
    .mode     = k_ra8_agt_pulse_mode_continuous,
    .polarity = k_ra8_agt_output_polarity_active_high,
    .compare  = k_ra8_agt_pulse_compare_a,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_agt_start_pulse_output((uint8_t)k_ra8_agt_test_channel_valid, &cfg));
  volatile r_agt_regs_t* reg = ra8_agt((uint8_t)k_ra8_agt_test_channel_valid);
  TEST_ASSERT_EQ(0x01U, reg->AGTMR1);                     /* TMOD = pulse_output */
  TEST_ASSERT_EQ(0x05U, reg->AGTIOC);                     /* TOE | TEDGSEL       */
  TEST_ASSERT_EQ(0x03U, reg->AGTCMSR);                    /* TCMEA | TOEA        */
  TEST_ASSERT_EQ(k_ra8_agt_test_pulse_duty, reg->AGTCMA); /* duty in A           */
  TEST_ASSERT_EQ(0xFFFFU, reg->AGTCMB);                   /* B parked at 0xFFFF  */
  TEST_ASSERT_EQ(k_ra8_agt_test_pulse_period, reg->AGT);  /* reload              */
  TEST_ASSERT_EQ(0x01U, reg->AGTCR);                      /* TSTART = 1          */
  TEST_END("agt start_pulse_output compare A");
}

/**
 * @par MC/DC:
 * Decision: ``compare == k_ra8_agt_pulse_compare_b`` (atomic). Pairs
 * with `compare_a` and `compare_none` for N+1=3 vectors over the
 * compare-channel branch.
 */
static void test_start_pulse_output_compare_b_inverted(void)
{
  TEST_BEGIN("agt start_pulse_output compare B + inverted polarity");
  prep_w43();
  const ra8_agt_pulse_cfg_t cfg = {
    .period   = (uint16_t)k_ra8_agt_test_pulse_period,
    .duty     = (uint16_t)k_ra8_agt_test_pulse_duty,
    .mode     = k_ra8_agt_pulse_mode_one_shot,
    .polarity = k_ra8_agt_output_polarity_active_low,
    .compare  = k_ra8_agt_pulse_compare_b,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_agt_start_pulse_output((uint8_t)k_ra8_agt_test_channel_last, &cfg));
  volatile r_agt_regs_t* reg = ra8_agt((uint8_t)k_ra8_agt_test_channel_last);
  TEST_ASSERT_EQ(0x04U, reg->AGTIOC);                     /* TOE only, no TEDGSEL */
  TEST_ASSERT_EQ(0x30U, reg->AGTCMSR);                    /* TCMEB | TOEB         */
  TEST_ASSERT_EQ(0xFFFFU, reg->AGTCMA);                   /* A parked             */
  TEST_ASSERT_EQ(k_ra8_agt_test_pulse_duty, reg->AGTCMB); /* duty in B            */
  TEST_END("agt start_pulse_output compare B + inverted polarity");
}

/**
 * @par MC/DC:
 * Decision: ``compare == k_ra8_agt_pulse_compare_none``. Third vector
 * for the compare-channel branch; ensures both compare registers are
 * parked at 0xFFFF as the spec requires.
 */
static void test_start_pulse_output_compare_none(void)
{
  TEST_BEGIN("agt start_pulse_output no compare");
  prep_w43();
  const ra8_agt_pulse_cfg_t cfg = {
    .period   = (uint16_t)k_ra8_agt_test_pulse_period,
    .duty     = 0U,
    .mode     = k_ra8_agt_pulse_mode_continuous,
    .polarity = k_ra8_agt_output_polarity_active_high,
    .compare  = k_ra8_agt_pulse_compare_none,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_agt_start_pulse_output((uint8_t)k_ra8_agt_test_channel_valid, &cfg));
  volatile r_agt_regs_t* reg = ra8_agt((uint8_t)k_ra8_agt_test_channel_valid);
  TEST_ASSERT_EQ(0x00U, reg->AGTCMSR); /* nothing armed */
  TEST_ASSERT_EQ(0xFFFFU, reg->AGTCMA);
  TEST_ASSERT_EQ(0xFFFFU, reg->AGTCMB);
  TEST_END("agt start_pulse_output no compare");
}

/**
 * @par MC/DC:
 * Decision: ``cfg == nullptr`` (atomic). Pairs with the happy-path
 * tests for N+1=2 vectors over the NULL-pointer guard.
 */
static void test_start_pulse_output_null_cfg(void)
{
  TEST_BEGIN("agt start_pulse_output null cfg");
  prep_w43();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_agt_start_pulse_output((uint8_t)k_ra8_agt_test_channel_valid, nullptr));
  TEST_END("agt start_pulse_output null cfg");
}

/**
 * @par MC/DC:
 * Decision: ``channel < 10`` via ``ra8_agt(channel) == nullptr`` guard.
 * Pairs with `compare_a` happy path for 2 vectors.
 */
static void test_start_pulse_output_bad_channel(void)
{
  TEST_BEGIN("agt start_pulse_output bad channel");
  prep_w43();
  const ra8_agt_pulse_cfg_t cfg = {
    .period   = 0U,
    .duty     = 0U,
    .mode     = k_ra8_agt_pulse_mode_continuous,
    .polarity = k_ra8_agt_output_polarity_active_high,
    .compare  = k_ra8_agt_pulse_compare_none,
  };
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_agt_start_pulse_output((uint8_t)k_ra8_agt_test_channel_bad, &cfg));
  TEST_END("agt start_pulse_output bad channel");
}

/**
 * @test test_mcdc_agt_pulse_compare_enum
 *
 * @par MC/DC:
 * Decision: ``(cfg->compare != k_ra8_agt_pulse_compare_none) &&
 *             (cfg->compare != k_ra8_agt_pulse_compare_a) &&
 *             (cfg->compare != k_ra8_agt_pulse_compare_b)``
 * (3 conditions, libs/ra8_hal/src/ra8_agt.c@agt_pulse_validate_cfg)
 * Per DO-178C 6.4.4.3 the representative-subset is N+1 = 4 vectors:
 * three pulse-mode happy-path tests (compare_a, compare_b,
 * compare_none) cover each operand evaluating false in turn, and the
 * out-of-range vector below covers all three operands evaluating
 * true (decision = true).
 */
static void test_start_pulse_output_bad_compare(void)
{
  TEST_BEGIN("agt start_pulse_output bad compare enum");
  prep_w43();
  const ra8_agt_pulse_cfg_t cfg = {
    .period   = 0U,
    .duty     = 0U,
    .mode     = k_ra8_agt_pulse_mode_continuous,
    .polarity = k_ra8_agt_output_polarity_active_high,
    .compare  = (ra8_agt_pulse_compare_t)0xFEU,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_agt_start_pulse_output((uint8_t)k_ra8_agt_test_channel_valid, &cfg));
  TEST_END("agt start_pulse_output bad compare enum");
}

/**
 * @test test_mcdc_agt_pulse_polarity_enum
 *
 * @par MC/DC:
 * Decision: ``(cfg->polarity != k_ra8_agt_output_polarity_active_high) &&
 *             (cfg->polarity != k_ra8_agt_output_polarity_active_low)``
 * (2 conditions, libs/ra8_hal/src/ra8_agt.c@agt_pulse_validate_cfg)
 * Per DO-178C 6.4.4.3 the representative-subset is N+1 = 3 vectors:
 * active_high happy-path covers operand 1 = false, active_low happy-
 * path covers operand 2 = false (both upstream pulse-mode tests),
 * and the out-of-range vector below drives both operands true so
 * the decision evaluates true.
 */
static void test_start_pulse_output_bad_polarity(void)
{
  TEST_BEGIN("agt start_pulse_output bad polarity enum");
  prep_w43();
  const ra8_agt_pulse_cfg_t cfg = {
    .period   = 0U,
    .duty     = 0U,
    .mode     = k_ra8_agt_pulse_mode_continuous,
    .polarity = (ra8_agt_output_polarity_t)0xFEU,
    .compare  = k_ra8_agt_pulse_compare_none,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_agt_start_pulse_output((uint8_t)k_ra8_agt_test_channel_valid, &cfg));
  TEST_END("agt start_pulse_output bad polarity enum");
}

/* ---- cascade mode ---- */

typedef enum : uint32_t {
  k_ra8_agt_test_cas_reload32 = 0x12345678UL, /**< RA8 AGT test cas reload32. */
} ra8_agt_test_cas_const_t;

typedef enum : uint16_t {
  k_ra8_agt_test_cas_lo16 = 0x5678U, /**< RA8 AGT test cas lo16. */
  k_ra8_agt_test_cas_hi16 = 0x1234U, /**< RA8 AGT test cas hi16. */
} ra8_agt_test_cas_halves_t;

static uint32_t s_cas_cb_count;

static void stub_cas_cb(void* ctx, uint8_t ch)
{
  (void)ctx;
  (void)ch;
  ++s_cas_cb_count;
}

/**
 * @par MC/DC:
 * Compound decision on ``cfg->on_underflow != nullptr``. Two vectors:
 * this happy case (non-NULL) + the `cascade_no_callback` test (NULL).
 */
static void test_start_cascade_happy(void)
{
  TEST_BEGIN("agt start_cascade happy");
  prep_w43();
  s_cas_cb_count                  = 0U;
  const ra8_agt_cascade_cfg_t cfg = {
    .reload32     = (uint32_t)k_ra8_agt_test_cas_reload32,
    .clock        = k_ra8_agt_cascade_clk_pclkb,
    .on_underflow = stub_cas_cb,
    .ctx          = nullptr,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_agt_start_cascade(&cfg));
  volatile r_agt_regs_t* lo = ra8_agt(0U);
  volatile r_agt_regs_t* hi = ra8_agt(1U);
  TEST_ASSERT_EQ(k_ra8_agt_test_cas_lo16, lo->AGT);
  TEST_ASSERT_EQ(k_ra8_agt_test_cas_hi16, hi->AGT);
  TEST_ASSERT_EQ(0x00U, lo->AGTMR1); /* timer mode + PCLKB                 */
  TEST_ASSERT_EQ(0x50U, hi->AGTMR1); /* timer mode + AGT0-underflow source */
  TEST_ASSERT_EQ(0x01U, lo->AGTCR);  /* TSTART                             */
  TEST_ASSERT_EQ(0x01U, hi->AGTCR);
  /* callback installed -> dispatch increments */
  ra8_agt_dispatch(1U);
  TEST_ASSERT_EQ(1, s_cas_cb_count);
  TEST_END("agt start_cascade happy");
}

/**
 * @par MC/DC:
 * Decision: ``cfg->clock == k_ra8_agt_cascade_clk_pclkb_div8``. Pairs
 * with the pclkb-direct case for branch coverage of the switch in
 * `agt_cascade_clock_to_tck`.
 */
static void test_start_cascade_div8(void)
{
  TEST_BEGIN("agt start_cascade div8");
  prep_w43();
  const ra8_agt_cascade_cfg_t cfg = {
    .reload32     = 0x00010001UL,
    .clock        = k_ra8_agt_cascade_clk_pclkb_div8,
    .on_underflow = nullptr,
    .ctx          = nullptr,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_agt_start_cascade(&cfg));
  volatile r_agt_regs_t* lo = ra8_agt(0U);
  TEST_ASSERT_EQ(0x10U, lo->AGTMR1); /* TCK = 001b (PCLKB/8) shifted to bit 4 */
  TEST_END("agt start_cascade div8");
}

/**
 * @par MC/DC:
 * Decision: ``cfg->clock == k_ra8_agt_cascade_clk_pclkb_div2``. Third
 * vector for the switch-statement branch coverage.
 */
static void test_start_cascade_div2(void)
{
  TEST_BEGIN("agt start_cascade div2");
  prep_w43();
  const ra8_agt_cascade_cfg_t cfg = {
    .reload32     = 0x00010001UL,
    .clock        = k_ra8_agt_cascade_clk_pclkb_div2,
    .on_underflow = nullptr,
    .ctx          = nullptr,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_agt_start_cascade(&cfg));
  volatile r_agt_regs_t* lo = ra8_agt(0U);
  TEST_ASSERT_EQ(0x30U, lo->AGTMR1); /* TCK = 011b -> 0x30 */
  TEST_END("agt start_cascade div2");
}

/**
 * @par MC/DC:
 * Decision: ``cfg == nullptr`` (atomic). Pairs with happy-path tests
 * for 2 vectors over the NULL guard.
 */
static void test_start_cascade_null(void)
{
  TEST_BEGIN("agt start_cascade null cfg");
  prep_w43();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_agt_start_cascade(nullptr));
  TEST_END("agt start_cascade null cfg");
}

/**
 * @par MC/DC:
 * Decision: ``clock`` switch default arm. Pairs with the three good
 * enumerator tests for 4 vectors = N+1 over the switch.
 */
static void test_start_cascade_bad_clock(void)
{
  TEST_BEGIN("agt start_cascade bad clock enum");
  prep_w43();
  const ra8_agt_cascade_cfg_t cfg = {
    .reload32     = 0U,
    .clock        = (ra8_agt_cascade_clk_t)0xFEU,
    .on_underflow = nullptr,
    .ctx          = nullptr,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_agt_start_cascade(&cfg));
  TEST_END("agt start_cascade bad clock enum");
}

/**
 * @par MC/DC:
 * Decision: ``cfg->on_underflow != nullptr`` (atomic). NULL branch:
 * the previously-installed callback must not be overwritten.
 */
static void test_start_cascade_no_callback(void)
{
  TEST_BEGIN("agt start_cascade no callback leaves slot alone");
  prep_w43();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_agt_attach_handler(stub_agt_cb, (void*)(uintptr_t)0x99U));
  const ra8_agt_cascade_cfg_t cfg = {
    .reload32     = 0U,
    .clock        = k_ra8_agt_cascade_clk_pclkb,
    .on_underflow = nullptr,
    .ctx          = nullptr,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_agt_start_cascade(&cfg));
  /* old shared callback should still fire */
  ra8_agt_dispatch(1U);
  TEST_ASSERT_EQ(1, s_agt_cb_count);
  TEST_END("agt start_cascade no callback leaves slot alone");
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
  test_start_free_run_happy,
  test_start_free_run_middle_channel,
  test_start_free_run_last_channel,
  test_start_free_run_bad_channel,
  test_start_free_run_huge_channel,
  test_stop_happy,
  test_stop_bad_channel,
  test_deinit,
  test_set_reload_and_status,
  test_attach_and_dispatch,
  test_power_transition,
  test_no_mstp_channel_power,
  test_start_pulse_output_compare_a,
  test_start_pulse_output_compare_b_inverted,
  test_start_pulse_output_compare_none,
  test_start_pulse_output_null_cfg,
  test_start_pulse_output_bad_channel,
  test_start_pulse_output_bad_compare,
  test_start_pulse_output_bad_polarity,
  test_start_cascade_happy,
  test_start_cascade_div8,
  test_start_cascade_div2,
  test_start_cascade_null,
  test_start_cascade_bad_clock,
  test_start_cascade_no_callback,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  return 0;
}
