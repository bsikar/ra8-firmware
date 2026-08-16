/**
 * @file test_ra8_adc_selfdiag.c
 * @brief Unit tests for the ADC_B self-diagnosis + die-temperature path
 *
 * @details
 * Covers the two safety-relevant analog features added to the HAL:
 *
 *  1. ``ra8_adc_self_diagnose`` -- the ADC_B built-in reference self-test
 *     (modes 1/2/3, HUM Ch 53.3.11 p 3411-3414). The dedicated scan
 *     group is kicked; ADSR.ADACT0 reads idle (0) from the RAM-backed
 *     register window after reset, so the busy-poll completes on its
 *     first read, and the injected ADEXDR0
 *     result drives the pass/fail decision.
 *  2. ``ra8_adc_read_internal_channel`` + ``ra8_tsn_read_die_temp_milli_c``
 *     -- the close-the-loop die-temperature read that drives the
 *     temperature-sensor channel (CNVCS = 0x64) and feeds the raw code
 *     to the existing two-point calibration math (HUM Ch 55.3.1).
 *
 * The fake MMIO window is plain RAM, so the tests inject ADEXDRn
 * results and calibration words by writing directly to the backing
 * store.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_adc.h"
#include "ra8_adc_b_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_tsn.h"
#include "ra8_tsn_regs.h"
#include "unity_minimal.h"

/**
 * @enum adc_selfdiag_code_t
 * @brief Self-diagnosis codes: one with alternating bits, one at the field's full width so a truncation is visible.
 */
typedef enum : uint16_t {
  k_adc_selfdiag_code_a = 0xAAAAU, /**< A self-diagnosis code with alternating bits. */
  k_adc_selfdiag_code_max =
    0xFFFFU, /**< The widest code the field can hold, so a truncation is visible. */
} adc_selfdiag_code_t;

/* ---------------------------------------------------------------------------
 * Fake helper: ADACT0 idle-state (deterministic, no wall-clock timer)
 * --------------------------------------------------------------------------- */

/* The ADC busy-poll reads ADSR.ADACT0 straight from the RAM-backed register
 * window. After ra8_fake_mmap_reset() ADACT0 reads 0 (idle), so the driver's
 * bounded busy-wait observes the conversion already complete on its first poll
 * and takes the success path -- deterministically, with no wall-clock timer
 * signal to race the poll. Timeout cases below leave ADACT0 set (1) so the
 * same loop runs to its budget and returns k_ra8_err_hw_timeout. */

/* ---------------------------------------------------------------------------
 * Fixtures
 * --------------------------------------------------------------------------- */

typedef enum : uint8_t {
  k_test_diag_vchan = 23U, /**< Dedicated ADCHCR slot used by the driver. */
} test_slot_t;

typedef enum : uint8_t {
  k_test_adexdr_selfdiag0 = 0U, /**< ADEXDR0: self-diagnosis A/D unit 0.  */
  k_test_adexdr_temp      = 4U, /**< ADEXDR4: temperature sensor channel. */
  k_test_adexdr_vref      = 5U, /**< ADEXDR5: internal reference voltage. */
} test_adexdr_t;

typedef enum : uint32_t {
  k_test_exd_mode1_ideal = 0x00000000UL, /**< Mode 1 ideal 0x0000.         */
  k_test_exd_mode2_ideal = 0x00008000UL, /**< Mode 2 ideal 0x8000.         */
  k_test_exd_mode3_ideal = 0x00007FFFUL, /**< Mode 3 ideal 0x7FFF.         */
  k_test_exd_out_band_hi = 0x00004000UL, /**< +16384: above the band.      */
  k_test_exd_out_band_lo = 0x0000C000UL, /**< -16384 (signed): below band. */
} test_exd_t;

typedef enum : uint16_t {
  k_test_temp_raw   = 0x0960U, /**< 2400: equals cal-hi -> 125000 mC.    */
  k_test_vref_raw   = 0x0555U, /**< Arbitrary 12-bit Vref code.          */
  k_test_cal_hi_125 = 2400U,   /**< Plausible 125 degC calibration code. */
  k_test_cal_lo_n40 = 1500U,   /**< Plausible -40 degC calibration code. */
} test_temp_t;

static void inject_cal_words(uint32_t hi_code, uint32_t lo_code)
{
  volatile r_tsn_cal_regs_t* cal   = (volatile r_tsn_cal_regs_t*)(uintptr_t)k_ra8_tsn_cal_base_addr;
  *(volatile uint32_t*)&cal->TSCDR = hi_code;
  *(volatile uint32_t*)&cal->TSCDR2 = lo_code;
}

static ra8_tsn_config_t make_tsn_cfg(void)
{
  const ra8_tsn_config_t cfg = {
    .high_ref_degc = k_ra8_tsn_cal_temp_high_125,
    .low_ref_degc  = k_ra8_tsn_cal_temp_low_n40,
    .stab_us       = 30U,
  };
  return cfg;
}

/* ---------------------------------------------------------------------------
 * ra8_adc_self_diagnose: validation + happy / fail paths
 * --------------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the null-pointer and
 * invalid-mode rejection contract; no `&&` or `||` in the touched code)
 */
static void test_self_diagnose_rejects(void)
{
  TEST_BEGIN("self_diagnose rejects null + bad mode");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init());

  uint16_t code = 0U;
  bool     pass = true;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_adc_self_diagnose(k_ra8_adc_selfdiag_mode_1, nullptr, &pass));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_adc_self_diagnose(k_ra8_adc_selfdiag_mode_1, &code, nullptr));
  /* Mode 0 (and any value outside 1..3) must be rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_adc_self_diagnose((ra8_adc_selfdiag_mode_t)0U, &code, &pass));
  TEST_END("self_diagnose rejects null + bad mode");
}

/**
 * @par MC/DC:
 * (no compound decisions touched -- verifies the register-programming
 * postconditions and that the ideal result yields out_pass == true)
 */
static void test_self_diagnose_mode1_pass(void)
{
  TEST_BEGIN("self_diagnose mode 1 ideal -> pass");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init());

  /* Inject the ideal mode-1 result (0x0000, ERR clear) into ADEXDR0. */
  *ra8_adc_b_adexdr(k_test_adexdr_selfdiag0) = k_test_exd_mode1_ideal;

  uint16_t        code = k_adc_selfdiag_code_max;
  bool            pass = false;
  const ra8_err_t err  = ra8_adc_self_diagnose(k_ra8_adc_selfdiag_mode_1, &code, &pass);

  TEST_ASSERT_EQ(k_ra8_ok, err);
  TEST_ASSERT_EQ(0x0000U, code);
  TEST_ASSERT(pass);

  /* Postcondition: DIAGVAL is cleared back to off after the run. */
  const uint32_t diagval =
    *ra8_adc_b_adsgdcr(k_ra8_adc_b_scan_groups - 1U) & k_ra8_adsgdcr_mask_diagval;
  TEST_ASSERT_EQ(0U, diagval);

  /* Postcondition: the dedicated slot maps the self-diagnosis channel in
   * differential, 16-bit signed format. */
  const uint32_t chcr  = *ra8_adc_b_adchcr(k_test_diag_vchan);
  const uint32_t cnvcs = (chcr & k_ra8_adchcr_mask_cnvcs) >> (uint32_t)k_ra8_adchcr_bit_cnvcs;
  TEST_ASSERT_EQ(k_ra8_adc_b_chan_selfdiag_adc0, cnvcs);
  TEST_ASSERT((chcr & k_ra8_adchcr_mask_ainmd) != 0U);

  const uint32_t opcrc = *ra8_adc_b_addopcrc(k_test_diag_vchan);
  const uint32_t adprc = (opcrc & k_ra8_addopcrc_mask_adprc) >> (uint32_t)k_ra8_addopcrc_bit_adprc;
  const uint32_t signsl =
    (opcrc & k_ra8_addopcrc_mask_signsel) >> (uint32_t)k_ra8_addopcrc_bit_signsel;
  TEST_ASSERT_EQ(k_ra8_addopcrc_adprc_16bit, adprc);
  TEST_ASSERT_EQ(k_ra8_addopcrc_signsel_signed, signsl);
  TEST_END("self_diagnose mode 1 ideal -> pass");
}

/**
 * @par MC/DC:
 * (no compound decisions touched -- mode-2 / mode-3 ideal codes pass)
 */
static void test_self_diagnose_mode2_mode3_pass(void)
{
  TEST_BEGIN("self_diagnose mode 2/3 ideal -> pass");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init());

  *ra8_adc_b_adexdr(k_test_adexdr_selfdiag0) = k_test_exd_mode2_ideal;
  uint16_t code                              = 0U;
  bool     pass                              = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_self_diagnose(k_ra8_adc_selfdiag_mode_2, &code, &pass));
  TEST_ASSERT_EQ(0x8000U, code);
  TEST_ASSERT(pass);

  *ra8_adc_b_adexdr(k_test_adexdr_selfdiag0) = k_test_exd_mode3_ideal;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_self_diagnose(k_ra8_adc_selfdiag_mode_3, &code, &pass));
  TEST_ASSERT_EQ(0x7FFFU, code);
  TEST_ASSERT(pass);
  TEST_END("self_diagnose mode 2/3 ideal -> pass");
}

/**
 * @par MC/DC:
 * (no compound decisions touched -- ERR flag forces out_pass == false)
 */
static void test_self_diagnose_err_flag_fails(void)
{
  TEST_BEGIN("self_diagnose ERR flag -> fail");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init());

  /* Ideal data but ERR bit set -> conversion invalid -> fail. */
  *ra8_adc_b_adexdr(k_test_adexdr_selfdiag0) = k_test_exd_mode1_ideal | k_ra8_adexdr_mask_err;
  uint16_t code                              = 0U;
  bool     pass                              = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_self_diagnose(k_ra8_adc_selfdiag_mode_1, &code, &pass));
  TEST_ASSERT(!pass);
  TEST_END("self_diagnose ERR flag -> fail");
}

/**
 * @par MC/DC:
 * (no compound decisions touched -- the busy poll never sees ADACT0
 * clear, so the driver reports a hardware timeout)
 */
static void test_self_diagnose_timeout(void)
{
  TEST_BEGIN("self_diagnose timeout");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init());

  /* Force ADACT0 stuck high (never cleared) so the busy poll never exits. */
  *ra8_adc_b_adsr() = k_ra8_adsr_mask_adact0;
  uint16_t code     = k_adc_selfdiag_code_a;
  bool     pass     = true;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_adc_self_diagnose(k_ra8_adc_selfdiag_mode_1, &code, &pass));
  /* On the error path the outputs are left at their safe defaults. */
  TEST_ASSERT_EQ(0U, code);
  TEST_ASSERT(!pass);
  TEST_END("self_diagnose timeout");
}

/* ---------------------------------------------------------------------------
 * ra8_adc_read_internal_channel
 * --------------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions touched -- null-out rejection contract)
 */
static void test_read_internal_null(void)
{
  TEST_BEGIN("read_internal null out");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init());
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_adc_read_internal_channel(k_ra8_adc_chan_temperature, nullptr));
  TEST_END("read_internal null out");
}

/**
 * @par MC/DC:
 * (no compound decisions touched -- happy-path channel-select + ADEXDR
 * read for both temperature and Vref internal channels)
 */
static void test_read_internal_happy(void)
{
  TEST_BEGIN("read_internal temp + vref happy");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init());

  /* Temperature channel -> ADEXDR4. */
  *ra8_adc_b_adexdr(k_test_adexdr_temp) = (uint32_t)k_test_temp_raw;
  uint16_t raw                          = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_read_internal_channel(k_ra8_adc_chan_temperature, &raw));
  TEST_ASSERT_EQ(k_test_temp_raw, raw);
  /* The dedicated slot now selects CNVCS 0x64 in single-ended mode. */
  const uint32_t chcr  = *ra8_adc_b_adchcr(k_test_diag_vchan);
  const uint32_t cnvcs = (chcr & k_ra8_adchcr_mask_cnvcs) >> (uint32_t)k_ra8_adchcr_bit_cnvcs;
  TEST_ASSERT_EQ(k_ra8_adc_chan_temperature, cnvcs);
  TEST_ASSERT((chcr & k_ra8_adchcr_mask_ainmd) == 0U);

  /* Internal reference voltage channel -> ADEXDR5. */
  *ra8_adc_b_adexdr(k_test_adexdr_vref) = (uint32_t)k_test_vref_raw;
  raw                                   = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_read_internal_channel(k_ra8_adc_chan_int_ref_volt, &raw));
  TEST_ASSERT_EQ(k_test_vref_raw, raw);
  TEST_END("read_internal temp + vref happy");
}

/* ---------------------------------------------------------------------------
 * ra8_tsn_read_die_temp_milli_c: close-the-loop
 * --------------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions touched -- exercises the closed-loop temp read
 * reusing the existing two-point cal-math; raw == cal-hi -> 125000 mC)
 */
static void test_die_temp_closed_loop(void)
{
  TEST_BEGIN("die-temp closed loop -> 125000 mC");
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();

  inject_cal_words((uint32_t)k_test_cal_hi_125, (uint32_t)k_test_cal_lo_n40);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init());
  const ra8_tsn_config_t cfg = make_tsn_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_init(&cfg));

  /* The ADC reports the high-reference code -> two-point math returns T1. */
  *ra8_adc_b_adexdr(k_test_adexdr_temp) = (uint32_t)k_test_temp_raw;
  int32_t milli                         = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_read_die_temp_milli_c(&milli));
  TEST_ASSERT_EQ(125000, milli);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_tsn_deinit());
  TEST_END("die-temp closed loop -> 125000 mC");
}

/**
 * @par MC/DC:
 * (no compound decisions touched -- the closed-loop read refuses to run
 * before ra8_tsn_init and rejects a null output)
 */
static void test_die_temp_guards(void)
{
  TEST_BEGIN("die-temp guards (null + before init)");
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_tsn_deinit(); /* Ensure the init flag is clear. */

  int32_t milli = 0;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tsn_read_die_temp_milli_c(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_tsn_read_die_temp_milli_c(&milli));
  TEST_END("die-temp guards (null + before init)");
}

/* ---------------------------------------------------------------------------
 * MC/DC: the three new compound decisions
 * --------------------------------------------------------------------------- */

/**
 * @test test_mcdc_adc_selfdiag
 *
 * @par MC/DC:
 * Three 2-condition decisions, each covered with an N+1 = 3 vector
 * subset (DO-178C 6.4.4.3 representative MC/DC). The decisions are cited
 * by enclosing function for the new-compound gate:
 * libs/ra8_hal/src/adc.c@internal_selfdiag_in_band,
 * libs/ra8_hal/src/adc.c@ra8_adc_self_diagnose,
 * libs/ra8_hal/src/adc.c@internal_is_supported_ext_chan.
 *
 * Decision A: ``internal_selfdiag_in_band`` (adc.c):
 * ``return (diff <= +tol) && (diff >= -tol);`` driven through
 * ``ra8_adc_self_diagnose`` mode 1 (expected 0) by the injected ADEXDR0
 * code, with the conversion forced healthy (ERR = 0):
 * - V1: code=0x0000  -> diff=0      -> C1=T, C2=T -> in_band=T -> pass=T
 * - V2: code=0x4000  -> diff=+16384 -> C1=F, C2=T -> in_band=F -> pass=F
 * - V3: code=0xC000  -> diff=-16384 -> C1=T, C2=F -> in_band=F -> pass=F
 * V1+V2 vary C1 (C2 held T); V1+V3 vary C2 (C1 held T).
 *
 * Decision B: ``*out_pass = (!err_flag) && in_band;`` (adc.c):
 * - V1: err=0, in_band=T -> !err=T, C2=T -> pass=T (code=0x0000)
 * - V2: err=1, in_band=T -> !err=F        -> pass=F (code=0x0000|ERR)
 * - V3: err=0, in_band=F -> !err=T, C2=F -> pass=F (code=0x4000)
 * V1+V2 vary !err_flag (in_band held T); V1+V3 vary in_band (!err held T).
 *
 * Decision C: ``internal_is_supported_ext_chan`` (adc.c):
 * ``return (chan == temp) || (chan == vref);`` via
 * ``ra8_adc_read_internal_channel``:
 * - V1: chan=temperature  -> C1=T          -> supported -> k_ra8_ok
 * - V2: chan=int_ref_volt -> C1=F, C2=T    -> supported -> k_ra8_ok
 * - V3: chan=0x66 (vbatt) -> C1=F, C2=F    -> rejected  -> invalid_arg
 * V1+V3 vary C1 (C2 held F); V2+V3 vary C2 (C1 held F).
 */
static void test_mcdc_adc_selfdiag(void)
{
  TEST_BEGIN("MC/DC: in_band AND, pass AND, supported_chan OR");
  uint16_t code = 0U;
  bool     pass = false;

  /* --- Decision A: internal_selfdiag_in_band ---------------------- */
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init());
  *ra8_adc_b_adexdr(k_test_adexdr_selfdiag0) = k_test_exd_mode1_ideal; /* diff 0 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_self_diagnose(k_ra8_adc_selfdiag_mode_1, &code, &pass));
  TEST_ASSERT(pass); /* V1: in_band T */

  *ra8_adc_b_adexdr(k_test_adexdr_selfdiag0) = k_test_exd_out_band_hi; /* +16384 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_self_diagnose(k_ra8_adc_selfdiag_mode_1, &code, &pass));
  TEST_ASSERT(!pass); /* V2: C1 false */

  *ra8_adc_b_adexdr(k_test_adexdr_selfdiag0) = k_test_exd_out_band_lo; /* -16384 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_self_diagnose(k_ra8_adc_selfdiag_mode_1, &code, &pass));
  TEST_ASSERT(!pass); /* V3: C2 false */

  /* --- Decision B: (!err_flag) && in_band ------------------------- */
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init());
  *ra8_adc_b_adexdr(k_test_adexdr_selfdiag0) = k_test_exd_mode1_ideal; /* err 0, band T */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_self_diagnose(k_ra8_adc_selfdiag_mode_1, &code, &pass));
  TEST_ASSERT(pass); /* V1 */

  *ra8_adc_b_adexdr(k_test_adexdr_selfdiag0) = k_test_exd_mode1_ideal | k_ra8_adexdr_mask_err;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_self_diagnose(k_ra8_adc_selfdiag_mode_1, &code, &pass));
  TEST_ASSERT(!pass); /* V2: !err false */

  *ra8_adc_b_adexdr(k_test_adexdr_selfdiag0) = k_test_exd_out_band_hi; /* err 0, band F */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_self_diagnose(k_ra8_adc_selfdiag_mode_1, &code, &pass));
  TEST_ASSERT(!pass); /* V3: in_band false */

  /* --- Decision C: internal_is_supported_ext_chan ----------------- */
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_adc_init());
  uint16_t raw                          = 0U;
  *ra8_adc_b_adexdr(k_test_adexdr_temp) = (uint32_t)k_test_temp_raw;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_adc_read_internal_channel(k_ra8_adc_chan_temperature, &raw)); /* V1 */

  *ra8_adc_b_adexdr(k_test_adexdr_vref) = (uint32_t)k_test_vref_raw;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_adc_read_internal_channel(k_ra8_adc_chan_int_ref_volt, &raw)); /* V2 */

  /* V3: 0x66 (VBATT 1/6) is a real CNVCS code but NOT a supported HAL
   * internal channel -> both conditions false -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_adc_read_internal_channel((ra8_adc_internal_chan_t)0x66U, &raw));

  TEST_END("MC/DC: in_band AND, pass AND, supported_chan OR");
}

int main(void)
{
  test_self_diagnose_rejects();
  test_self_diagnose_mode1_pass();
  test_self_diagnose_mode2_mode3_pass();
  test_self_diagnose_err_flag_fails();
  test_self_diagnose_timeout();
  test_read_internal_null();
  test_read_internal_happy();
  test_die_temp_closed_loop();
  test_die_temp_guards();
  test_mcdc_adc_selfdiag();
  return 0;
}
