/**
 * @file test_ra_cgc.c
 * @brief Unit tests for ra_cgc.c (Clock Generation Circuit)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_cgc_regs.h"
#include "ra8d2_system_regs.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_ra_cgc_test_all_oscsf = 0xFFU, /**< All stabilisation bits set.    */
} ra_cgc_test_bits_t;

static void test_get_clock_hz_null_out(void)
{
  TEST_BEGIN("cgc get_clock_hz null out");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, nullptr));
  TEST_END("cgc get_clock_hz null out");
}

static void test_get_clock_hz_all_ids(void)
{
  TEST_BEGIN("cgc get_clock_hz all ids");
  ra_sim_mmap_reset();

  uint32_t            hz    = 0U;
  const ra_clock_id_t ids[] = {
    k_ra_clock_id_cpuclk0,
    k_ra_clock_id_cpuclk1,
    k_ra_clock_id_iclk,
    k_ra_clock_id_pclka,
    k_ra_clock_id_pclkb,
    k_ra_clock_id_pclkc,
    k_ra_clock_id_pclkd,
    k_ra_clock_id_pclke,
    k_ra_clock_id_fclk,
    k_ra_clock_id_mriclk,
  };
  const uint8_t count = (uint8_t)(sizeof(ids) / sizeof(ids[0]));
  for (uint8_t i = 0U; i < count; ++i) {
    hz                 = 0U;
    const ra_err_t err = ra_cgc_get_clock_hz(ids[i], &hz);
    TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
    TEST_ASSERT(hz != 0U);
  }
  TEST_END("cgc get_clock_hz all ids");
}

static void test_get_clock_hz_out_of_range(void)
{
  TEST_BEGIN("cgc get_clock_hz out of range");
  ra_sim_mmap_reset();

  uint32_t hz = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_cgc_get_clock_hz((ra_clock_id_t)200, &hz));
  TEST_END("cgc get_clock_hz out of range");
}

static void test_init_happy_with_oscsf_preseed(void)
{
  TEST_BEGIN("cgc init happy (oscsf preseeded)");
  ra_sim_mmap_reset();

  /* Pre-seed OSCSF with every stabilisation bit set so both the
   * main-osc wait and the PLL1 wait complete on the first
   * iteration of the polling loop. */
  *ra_sys_oscsf() = (uint8_t)k_ra_cgc_test_all_oscsf;

  const ra_err_t err = ra_cgc_init();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);

  /* After init the published cpuclk0 should be the target, not MOCO. */
  uint32_t hz = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &hz));
  TEST_ASSERT(hz != 0U);
  TEST_END("cgc init happy (oscsf preseeded)");
}

static void test_init_main_osc_timeout(void)
{
  TEST_BEGIN("cgc init main-osc timeout");
  ra_sim_mmap_reset();

  /* Leave OSCSF zero: the polling loop will burn its budget on
   * the main-oscillator wait and return k_ra_err_hw_timeout. */
  const ra_err_t err = ra_cgc_init();
  TEST_ASSERT_EQ((int)k_ra_err_hw_timeout, (int)err);
  TEST_END("cgc init main-osc timeout");
}

static void test_use_hoco_happy(void)
{
  TEST_BEGIN("cgc use_hoco happy (oscsf preseeded)");
  ra_sim_mmap_reset();

  *ra_sys_oscsf()    = (uint8_t)k_ra_cgc_test_all_oscsf;
  const ra_err_t err = ra_cgc_use_hoco();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  TEST_END("cgc use_hoco happy (oscsf preseeded)");
}

static void test_use_hoco_timeout(void)
{
  TEST_BEGIN("cgc use_hoco timeout");
  ra_sim_mmap_reset();

  /* OSCSF left as zero -> HOCO wait eventually times out. */
  const ra_err_t err = ra_cgc_use_hoco();
  TEST_ASSERT_EQ((int)k_ra_err_hw_timeout, (int)err);
  TEST_END("cgc use_hoco timeout");
}

static int  s_ostd_count    = 0;
static int  s_ostd_last_val = 0;
static void stub_ostd_handler(void* ctx)
{
  ++s_ostd_count;
  if (ctx != nullptr) {
    s_ostd_last_val = *(int*)ctx;
  }
}

static void test_switch_pll1_target_updates_cpuclk0(void)
{
  TEST_BEGIN("cgc switch_pll1_target: updates cpuclk0 reading");
  ra_sim_mmap_reset();

  *ra_sys_oscsf() = (uint8_t)k_ra_cgc_test_all_oscsf;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_cgc_init());

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_cgc_switch_pll1_target(500000000U));
  uint32_t hz = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &hz));
  TEST_ASSERT_EQ((long long)500000000U, (long long)hz);

  /* Rejects zero. */
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_cgc_switch_pll1_target(0U));
  TEST_END("cgc switch_pll1_target: updates cpuclk0 reading");
}

static void test_stop_detection_arm_and_fire(void)
{
  TEST_BEGIN("cgc stop detection: handler fires via sim trigger");
  ra_sim_mmap_reset();
  s_ostd_count    = 0;
  s_ostd_last_val = 0;

  int ctx_val = 0xCAFE;
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_cgc_enable_stop_detection(nullptr, &ctx_val));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_cgc_enable_stop_detection(stub_ostd_handler, &ctx_val));

  /* Fire via the sim helper. */
  ra_cgc_sim_trigger_stop_detection();
  TEST_ASSERT_EQ((int)1, (int)s_ostd_count);
  TEST_ASSERT_EQ((int)0xCAFE, (int)s_ostd_last_val);

  /* Disable and verify the callback no longer fires. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_cgc_disable_stop_detection());
  ra_cgc_sim_trigger_stop_detection();
  TEST_ASSERT_EQ((int)1, (int)s_ostd_count); /* unchanged */
  TEST_END("cgc stop detection: handler fires via sim trigger");
}

int main(void)
{
  test_get_clock_hz_null_out();
  test_get_clock_hz_all_ids();
  test_get_clock_hz_out_of_range();
  test_init_happy_with_oscsf_preseed();
  test_init_main_osc_timeout();
  test_use_hoco_happy();
  test_use_hoco_timeout();
  test_switch_pll1_target_updates_cpuclk0();
  test_stop_detection_arm_and_fire();
  (void)fprintf(stderr, "[OK  ] test_ra_cgc.c\n");
  return 0;
}
