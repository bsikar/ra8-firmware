/**
 * @file test_ra8_cgc.c
 * @brief Unit tests for ra8_cgc.c (Clock Generation Circuit)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_cgc.h"
#include "ra8_cgc_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_mrms_regs.h"
#include "ra8_sram_regs.h"
#include "ra8_system_regs.h"
#include "unity_minimal.h"

/**
 * @enum cgc_fixture_t
 * @brief The recognizable values moved through the code under test.
 */
typedef enum : uint16_t {
  k_cgc_ctx_token =
    0xCAFE, /**< Token handed to the callback and checked on return, proving context survives. */
} cgc_fixture_t;

typedef enum : uint8_t {
  k_ra8_cgc_test_all_oscsf = 0xFFU, /**< All stabilisation bits set. */
} ra8_cgc_test_bits_t;

typedef enum : uint32_t {
  k_ra8_cgc_test_mrcfreq_fast = 200U, /**< Staged MRCFREQ >= PFB threshold. */
  k_ra8_cgc_test_satisfy_iter = 2U,   /**< Poll index for succeed-after-N.  */
} ra8_cgc_test_mrm_t;

/** @brief Reset the fake register mirror and the MMIO fault seam together. */
static void cgc_test_reset(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_clock_hz_null_out(void)
{
  TEST_BEGIN("cgc get_clock_hz null out");
  cgc_test_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, nullptr));
  TEST_END("cgc get_clock_hz null out");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_clock_hz_all_ids(void)
{
  TEST_BEGIN("cgc get_clock_hz all ids");
  cgc_test_reset();

  uint32_t             hz    = 0U;
  const ra8_clock_id_t ids[] = {
    k_ra8_clock_id_cpuclk0,
    k_ra8_clock_id_cpuclk1,
    k_ra8_clock_id_iclk,
    k_ra8_clock_id_pclka,
    k_ra8_clock_id_pclkb,
    k_ra8_clock_id_pclkc,
    k_ra8_clock_id_pclkd,
    k_ra8_clock_id_pclke,
    k_ra8_clock_id_fclk,
    k_ra8_clock_id_mriclk,
  };
  const uint8_t count = (uint8_t)(sizeof(ids) / sizeof(ids[0]));
  for (uint8_t i = 0U; i < count; ++i) {
    hz                  = 0U;
    const ra8_err_t err = ra8_cgc_get_clock_hz(ids[i], &hz);
    TEST_ASSERT_EQ(k_ra8_ok, err);
    TEST_ASSERT(hz != 0U);
  }
  TEST_END("cgc get_clock_hz all ids");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_clock_hz_out_of_range(void)
{
  TEST_BEGIN("cgc get_clock_hz out of range");
  cgc_test_reset();

  uint32_t hz = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_cgc_get_clock_hz((ra8_clock_id_t)200, &hz));
  TEST_END("cgc get_clock_hz out of range");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_happy_with_oscsf_preseed(void)
{
  TEST_BEGIN("cgc init happy (oscsf preseeded)");
  cgc_test_reset();

  /* Pre-seed OSCSF with every stabilisation bit set so both the
   * main-osc wait and the PLL1 wait complete on the first
   * iteration of the polling loop. */
  *ra8_sys_oscsf() = (uint8_t)k_ra8_cgc_test_all_oscsf;

  const ra8_err_t err = ra8_cgc_init();
  TEST_ASSERT_EQ(k_ra8_ok, err);

  /* After init the published cpuclk0 should be the target, not MOCO. */
  uint32_t hz = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &hz));
  TEST_ASSERT(hz != 0U);
  TEST_END("cgc init happy (oscsf preseeded)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_main_osc_timeout(void)
{
  TEST_BEGIN("cgc init main-osc timeout");
  cgc_test_reset();

  /* Arm the MMIO fault seam so the first OSCSF wait (the main-osc
   * stabilisation poll) burns its full budget and ra8_cgc_init
   * propagates k_ra8_err_hw_timeout. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)ra8_sys_oscsf()));
  const ra8_err_t err = ra8_cgc_init();
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, err);
  TEST_END("cgc init main-osc timeout");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- drives the succeed-after-N
 * continuation branch of the shared bounded wait; the loop decision
 * itself is single-condition)
 */
static void test_init_oscsf_satisfy_after(void)
{
  TEST_BEGIN("cgc init oscsf wait succeeds after N polls");
  cgc_test_reset();

  /* Model a peripheral that asserts the flag on the N-th poll: the
   * OSCSF waits iterate before converging, exercising the loop
   * continuation branch instead of the first-poll exit. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fake_mmio_satisfy_after((const volatile void*)ra8_sys_oscsf(),
                                             (uint32_t)k_ra8_cgc_test_satisfy_iter));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_init());
  TEST_END("cgc init oscsf wait succeeds after N polls");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- drives the VSCR.VSCMTSF
 * clear-wait timeout leg of ra8_cgc_init via the MMIO fault seam)
 */
static void test_init_vscr_timeout(void)
{
  TEST_BEGIN("cgc init VSCMTSF timeout");
  cgc_test_reset();

  *ra8_sys_oscsf() = (uint8_t)k_ra8_cgc_test_all_oscsf;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)ra8_sys_vscr()));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_init());
  TEST_END("cgc init VSCMTSF timeout");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- drives the MRCFREQ and
 * MREFREQ keyed-latch readback timeout legs via the MMIO fault seam)
 */
static void test_init_mrm_freq_timeout(void)
{
  TEST_BEGIN("cgc init MRCFREQ/MREFREQ latch timeout");

  /* MRCFREQ never latches -> step 6 propagates hw_timeout. */
  cgc_test_reset();
  *ra8_sys_oscsf() = (uint8_t)k_ra8_cgc_test_all_oscsf;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)ra8_mrms_mrcfreq()));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_init());

  /* MRCFREQ latches but MREFREQ never does -> same error, later leg. */
  cgc_test_reset();
  *ra8_sys_oscsf() = (uint8_t)k_ra8_cgc_test_all_oscsf;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)ra8_mrms_mrefreq()));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_init());
  TEST_END("cgc init MRCFREQ/MREFREQ latch timeout");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- drives both SCICKCR.CKSRDY
 * handshake timeout legs, isolated per wait-loop with fail-nth)
 */
static void test_init_sciclk_timeout_legs(void)
{
  TEST_BEGIN("cgc init SCICKCR handshake timeout legs");

  /* Leg 1: CKSRDY never asserts after CKSREQ=1 (wait-loop 0). */
  cgc_test_reset();
  *ra8_sys_oscsf() = (uint8_t)k_ra8_cgc_test_all_oscsf;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fake_mmio_fail_nth_wait((const volatile void*)ra8_sys_scickcr(), 0U));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_init());

  /* Leg 2: CKSRDY never de-asserts after CKSREQ clears (wait-loop 1). */
  cgc_test_reset();
  *ra8_sys_oscsf() = (uint8_t)k_ra8_cgc_test_all_oscsf;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fake_mmio_fail_nth_wait((const volatile void*)ra8_sys_scickcr(), 1U));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_init());
  TEST_END("cgc init SCICKCR handshake timeout legs");
}

/**
 * @par MC/DC:
 * Decision: ``mri_mhz >= k_ra8_mrcpfb_threshold_mhz`` in the PFB
 * re-enable step (1 condition).
 * - V1: staged MRCFREQ=200 -> true  -> MRCPFB written to 1.
 * - V2: MRCFREQ=0 (reset)  -> false -> MRCPFB stays 0.
 * Single-condition: two vectors prove both arms.
 */
static void test_init_pfb_threshold(void)
{
  TEST_BEGIN("cgc init PFB threshold arms");

  /* V1: stage a latched MRCFREQ above the 100 MHz threshold. The MRM
   * latch wait is seam-satisfied on its first poll and never writes,
   * so the staged value survives for the step-9 readback. */
  cgc_test_reset();
  *ra8_sys_oscsf()    = (uint8_t)k_ra8_cgc_test_all_oscsf;
  *ra8_mrms_mrcfreq() = (uint32_t)k_ra8_cgc_test_mrcfreq_fast;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_init());
  TEST_ASSERT_EQ(k_ra8_mrcpfb_enable, *ra8_mrms_mrcpfb());

  /* V2: MRCFREQ reads 0 -> below threshold -> PFB stays disabled. */
  cgc_test_reset();
  *ra8_sys_oscsf() = (uint8_t)k_ra8_cgc_test_all_oscsf;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_init());
  TEST_ASSERT_EQ(k_ra8_mrcpfb_disable, *ra8_mrms_mrcpfb());
  TEST_END("cgc init PFB threshold arms");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_use_hoco_happy(void)
{
  TEST_BEGIN("cgc use_hoco happy (oscsf preseeded)");
  cgc_test_reset();

  *ra8_sys_oscsf()    = (uint8_t)k_ra8_cgc_test_all_oscsf;
  const ra8_err_t err = ra8_cgc_use_hoco();
  TEST_ASSERT_EQ(k_ra8_ok, err);
  TEST_END("cgc use_hoco happy (oscsf preseeded)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_use_hoco_timeout(void)
{
  TEST_BEGIN("cgc use_hoco timeout");
  cgc_test_reset();

  /* Arm the seam: the HOCOSF wait burns its budget and times out. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)ra8_sys_oscsf()));
  const ra8_err_t err = ra8_cgc_use_hoco();
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, err);
  TEST_END("cgc use_hoco timeout");
}

static int32_t s_ostd_count    = 0;
static int32_t s_ostd_last_val = 0;
static void    stub_ostd_handler(void* ctx)
{
  ++s_ostd_count;
  if (ctx != nullptr) {
    s_ostd_last_val = *(int32_t*)ctx;
  }
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_switch_pll1_target_updates_cpuclk0(void)
{
  TEST_BEGIN("cgc switch_pll1_target: updates cpuclk0 reading");
  cgc_test_reset();

  *ra8_sys_oscsf() = (uint8_t)k_ra8_cgc_test_all_oscsf;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_init());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_switch_pll1_target(500000000U));
  uint32_t hz = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &hz));
  TEST_ASSERT_EQ(500000000U, hz);

  /* Rejects zero. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_cgc_switch_pll1_target(0U));
  TEST_END("cgc switch_pll1_target: updates cpuclk0 reading");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_stop_detection_arm_and_fire(void)
{
  TEST_BEGIN("cgc stop detection: handler fires via fake trigger");
  cgc_test_reset();
  s_ostd_count    = 0;
  s_ostd_last_val = 0;

  int32_t ctx_val = k_cgc_ctx_token;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cgc_enable_stop_detection(nullptr, &ctx_val));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_enable_stop_detection(stub_ostd_handler, &ctx_val));

  /* Fire via the fake helper. */
  ra8_cgc_fake_trigger_stop_detection();
  TEST_ASSERT_EQ(1, s_ostd_count);
  TEST_ASSERT_EQ(0xCAFE, s_ostd_last_val);

  /* Disable and verify the callback no longer fires. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_disable_stop_detection());
  ra8_cgc_fake_trigger_stop_detection();
  TEST_ASSERT_EQ(1, s_ostd_count); /* unchanged */
  TEST_END("cgc stop detection: handler fires via fake trigger");
}

/**
 * @test test_mcdc_eswclk_pdctreswm
 *
 * @par MC/DC:
 * Decision at libs/ra8_hal/src/ra8_cgc.c@internal_eswclk_power_on_domain in ra8_cgc_eswclk_init
 * ``if (((state & PDCSF) == 0U) && ((state & PDPGSF) != 0U))``
 * (2 conditions, ``&&``). The body issues a write to PDCTRESWM
 * clearing PDDE; the condition gates whether the power-on kick is
 * needed at all (when the domain is already powered up there is no
 * write).
 *
 * Verifying the gate is exercised under all combinations is tricky
 * on the host fake because PDCTRESWM is plain RAM and the
 * production code can only observe the read it does itself. Drive
 * the three branches by pre-seeding PDCTRESWM and re-running
 * ``ra8_cgc_eswclk_init``; the inner write only fires when the
 * decision evaluates true. N+1 = 3 vectors:
 *  - V1: PDCTRESWM=0b1000_0001 (PDCSF=0, PDPGSF=1) -> dec T,
 *        PDDE bit gets cleared.
 *  - V2: PDCTRESWM=0b1100_0001 (PDCSF=1, PDPGSF=1) -> dec F via
 *        C1=F (PDCSF != 0 short-circuits the AND).
 *  - V3: PDCTRESWM=0b0000_0001 (PDCSF=0, PDPGSF=0) -> dec F via
 *        C2=F (PDCSF=0 but PDPGSF=0).
 * V1+V2 prove PDCSF independently affects outcome; V1+V3 prove the
 * same for PDPGSF. Minimal MC/DC vector set.
 */
static void test_mcdc_eswclk_pdctreswm(void)
{
  TEST_BEGIN("cgc MC/DC: PDCTRESWM power-on decision");
  const uint8_t pdde_mask   = (uint8_t)(1U << 0U);
  const uint8_t pdcsf_mask  = (uint8_t)(1U << 6U);
  const uint8_t pdpgsf_mask = (uint8_t)(1U << 7U);
  /* V1: domain is gated (PDCSF=0 && PDPGSF=1) -> condition TRUE,
   * write fires and clears PDDE. The init also asserts CKSREQ/SRDY
   * for ESWCKCR + ESWPCKCR; the fake handlers handle that. */
  cgc_test_reset();
  *ra8_sys_oscsf()     = (uint8_t)(1U << 0U); /* HOCOSF, satisfies HOCO wait. */
  *ra8_sys_pdctreswm() = (uint8_t)(pdde_mask | pdpgsf_mask);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_eswclk_init());
  /* Expect PDDE cleared by the protected write. */
  TEST_ASSERT_EQ(0U, (*ra8_sys_pdctreswm() & pdde_mask));
  /* V2: PDCSF asserted (C1=F short-circuit) -> condition FALSE, no write. */
  cgc_test_reset();
  *ra8_sys_oscsf()     = (uint8_t)(1U << 0U);
  *ra8_sys_pdctreswm() = (uint8_t)(pdde_mask | pdcsf_mask | pdpgsf_mask);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_eswclk_init());
  /* PDDE should be UNTOUCHED (still 1). */
  TEST_ASSERT_EQ(pdde_mask, *ra8_sys_pdctreswm() & pdde_mask);
  /* V3: PDPGSF clear (C2=F after C1=F not-short-circuit) -> condition FALSE. */
  cgc_test_reset();
  *ra8_sys_oscsf()     = (uint8_t)(1U << 0U);
  *ra8_sys_pdctreswm() = pdde_mask;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_eswclk_init());
  TEST_ASSERT_EQ(pdde_mask, *ra8_sys_pdctreswm() & pdde_mask);
  TEST_END("cgc MC/DC: PDCTRESWM power-on decision");
}

/**
 * @test cgc_init_programs_sram_wait_state
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it asserts a single
 * post-condition of ra8_cgc_init, that SRAMWTSC.WTEN is set; the
 * decision inside ra8_sram_set_wait_state_for_clock is
 * single-condition and is covered in test_ra8_sram_ecc.c)
 *
 * @details
 * Regression guard for tracker #524. ra8_cgc_init takes ICLK to
 * k_ra8_iclk_hz, above half the part's rated maximum, and HUM Ch 58.3.7
 * "Wait State" p 3540 requires a wait cycle from that point on: "when
 * the wait is not inserted, the operation is not guaranteed". It was
 * never programmed, and the resulting failure was silent -- a single
 * bit dropped from a value read back out of SRAM, with the SRAM itself
 * holding the correct word. Nothing in the boot log, the emulator or
 * any existing gate could see it; only the wire test and a bare-metal
 * BusFault could, and both were misdiagnosed as an Ethernet DMA address
 * constraint for a month (#499). A missing register write with that
 * blast radius gets a unit test, so it cannot be quietly dropped again.
 */
static void test_init_programs_sram_wait_state(void)
{
  TEST_BEGIN("cgc init programs SRAMWTSC.WTEN");
  cgc_test_reset();
  *ra8_sys_oscsf() = (uint8_t)k_ra8_cgc_test_all_oscsf;

  volatile r_sram_regs_t* sram = ra8_sram_regs();
  /* HUM Ch 58.2.6 "SRAMWTSC : SRAM Wait State Control Register" p 3531 */
  sram->SRAMWTSC = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_init());
  /* HUM Ch 58.2.6 "SRAMWTSC : SRAM Wait State Control Register" p 3531 */
  TEST_ASSERT_EQ(k_ra8_sram_wtsc_wten, sram->SRAMWTSC);
  TEST_END("cgc init programs SRAMWTSC.WTEN");
}

int32_t main(void)
{
  test_get_clock_hz_null_out();
  test_get_clock_hz_all_ids();
  test_get_clock_hz_out_of_range();
  test_init_happy_with_oscsf_preseed();
  test_init_programs_sram_wait_state();
  test_init_main_osc_timeout();
  test_init_oscsf_satisfy_after();
  test_init_vscr_timeout();
  test_init_mrm_freq_timeout();
  test_init_sciclk_timeout_legs();
  test_init_pfb_threshold();
  test_use_hoco_happy();
  test_use_hoco_timeout();
  test_switch_pll1_target_updates_cpuclk0();
  test_stop_detection_arm_and_fire();
  test_mcdc_eswclk_pdctreswm();
  (void)fprintf(stderr, "[OK  ] test_ra8_cgc.c\n");
  return 0;
}
