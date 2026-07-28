/**
 * @file test_app_lpm_wake_matrix_demo.c
 * @brief Integration test: WUPEN0 / WUPEN1 arm + disarm walk
 *
 * @details
 * Mirrors examples/ek_ra8d2/hil_needs_revalidation/lpm_wake_matrix_demo/main.c bring-up:
 * ra8_lpm_init -> arm WUPEN0 bits in sequence -> arm WUPEN1 bits in
 * sequence -> clear both -> verify zero. The host fake mmap records
 * each register write so the test can verify the bits actually
 * landed and that ``ra8_lpm_get_exit_cause`` returns the packed
 * snapshot the demo relies on.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_lpm.h"
#include "ra8_lpm_regs.h"
#include "unity_minimal.h"

/**
 * @enum app_lpm_wake_matrix_demo_fixture_t
 * @brief Poison values written into out-parameters before a call, so one that fails without assigning is detectable.
 */
typedef enum : uint64_t {
  k_lpm_poison_cause =
    0xDEADBEEFCAFEBABEULL, /**< Poison written into the 64-bit wake-cause out-parameter; both halves are non-zero, so a call that set only one is detectable. */
} app_lpm_wake_matrix_demo_fixture_t;

/** @brief Bit-cast widths used for packing WUPEN1 into the high word. */
typedef enum : uint8_t {
  k_wake_matrix_test_wupen1_shift = 32U, /**< Wake matrix test wupen1 shift. */
} wake_matrix_test_shifts_t;

static void reset_world(void)
{
  ra8_fake_mmap_reset();
}

static ra8_lpm_config_t make_demo_cfg(void)
{
  const ra8_lpm_config_t cfg = {
    .io_port_keep     = false,
    .opa_bus_keep     = true,
    .sscr_fast_return = false,
    .dcdc_softstart   = k_ra8_lpm_dcssmode_128us,
    .sscr_low_power   = k_ra8_lpm_ss2lp_default,
  };
  return cfg;
}

/**
 * @par MC/DC:
 * Decision: ``ra8_lpm_init != ok``. One atomic condition x 2 vectors.
 */
static void test_lpm_wake_init_ok(void)
{
  reset_world();
  TEST_BEGIN("lpm_wake_matrix_demo: init ok");
  const ra8_lpm_config_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_init(&cfg));
  TEST_END("lpm_wake_matrix_demo: init ok");
}

/**
 * @brief Walking the WUPEN0 internal sources leaves all 6 bits set.
 *
 * @par MC/DC:
 * Decision: ``arm_wupen0(IWDT) != ok || arm_wupen0(PVD1) != ok || ...``
 * Six atomic conditions x N+1 = 7 vectors. The happy path runs here;
 * the per-bit error vectors are dominated by the underlying
 * ra8_lpm_arm_wupen0_bits API which has no failure path on host (no
 * NULL deref), so this test pins the happy-path readback.
 */
static void test_lpm_wake_walk_wupen0(void)
{
  reset_world();
  TEST_BEGIN("lpm_wake_matrix_demo: walk WUPEN0");
  const ra8_lpm_config_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_arm_wupen0_bits((uint32_t)k_ra8_lpm_wupen0_iwdt));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_arm_wupen0_bits((uint32_t)k_ra8_lpm_wupen0_pvd1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_arm_wupen0_bits((uint32_t)k_ra8_lpm_wupen0_pvd2));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_arm_wupen0_bits((uint32_t)k_ra8_lpm_wupen0_vbatt));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_arm_wupen0_bits((uint32_t)k_ra8_lpm_wupen0_rtcalm));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_arm_wupen0_bits((uint32_t)k_ra8_lpm_wupen0_rtcprd));
  const uint32_t expected = (uint32_t)k_ra8_lpm_wupen0_iwdt | (uint32_t)k_ra8_lpm_wupen0_pvd1 |
                            (uint32_t)k_ra8_lpm_wupen0_pvd2 | (uint32_t)k_ra8_lpm_wupen0_vbatt |
                            (uint32_t)k_ra8_lpm_wupen0_rtcalm | (uint32_t)k_ra8_lpm_wupen0_rtcprd;
  TEST_ASSERT_EQ(expected, *ra8_lpm_icu_reg32(k_ra8_lpm_wupen0_off));
  TEST_END("lpm_wake_matrix_demo: walk WUPEN0");
}

/**
 * @brief Walking the WUPEN1 internal sources leaves all 6 bits set.
 *
 * @par MC/DC:
 * Same pattern as the WUPEN0 walk: six atomic conditions x 7 vectors
 * covered by the underlying HAL test; this case pins the happy-path
 * readback against the demo's call sequence.
 */
static void test_lpm_wake_walk_wupen1(void)
{
  reset_world();
  TEST_BEGIN("lpm_wake_matrix_demo: walk WUPEN1");
  const ra8_lpm_config_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_arm_wupen1_bits((uint32_t)k_ra8_lpm_wupen1_comphs0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_arm_wupen1_bits((uint32_t)k_ra8_lpm_wupen1_sosc));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_arm_wupen1_bits((uint32_t)k_ra8_lpm_wupen1_ulpt0u));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_arm_wupen1_bits((uint32_t)k_ra8_lpm_wupen1_ulpt0a));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_arm_wupen1_bits((uint32_t)k_ra8_lpm_wupen1_ulpt0b));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_arm_wupen1_bits((uint32_t)k_ra8_lpm_wupen1_i3c0));
  const uint32_t expected = (uint32_t)k_ra8_lpm_wupen1_comphs0 | (uint32_t)k_ra8_lpm_wupen1_sosc |
                            (uint32_t)k_ra8_lpm_wupen1_ulpt0u | (uint32_t)k_ra8_lpm_wupen1_ulpt0a |
                            (uint32_t)k_ra8_lpm_wupen1_ulpt0b | (uint32_t)k_ra8_lpm_wupen1_i3c0;
  TEST_ASSERT_EQ(expected, *ra8_lpm_icu_reg32(k_ra8_lpm_wupen1_off));
  TEST_END("lpm_wake_matrix_demo: walk WUPEN1");
}

/**
 * @brief Disarming via clear helpers returns the matrix to zero.
 *
 * @par MC/DC:
 * Decision: ``clear_wupen0 != ok || clear_wupen1 != ok``.
 * Two atomic conditions x 3 vectors -- happy path (this) + each
 * branch error covered indirectly by the underlying HAL test.
 */
static void test_lpm_wake_disarm_all(void)
{
  reset_world();
  TEST_BEGIN("lpm_wake_matrix_demo: disarm all");
  const ra8_lpm_config_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_arm_wupen0_bits(0xFFFFFFFFUL));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_arm_wupen1_bits(0xFFFFFFFFUL));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_clear_wupen0_bits(0xFFFFFFFFUL));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_clear_wupen1_bits(0xFFFFFFFFUL));
  TEST_ASSERT_EQ(0U, *ra8_lpm_icu_reg32(k_ra8_lpm_wupen0_off));
  TEST_ASSERT_EQ(0U, *ra8_lpm_icu_reg32(k_ra8_lpm_wupen1_off));
  uint64_t cause = k_lpm_poison_cause;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_lpm_get_exit_cause(&cause));
  TEST_ASSERT_EQ(0U, cause);
  TEST_END("lpm_wake_matrix_demo: disarm all");
}

int main(void)
{
  test_lpm_wake_init_ok();
  test_lpm_wake_walk_wupen0();
  test_lpm_wake_walk_wupen1();
  test_lpm_wake_disarm_all();
  return 0;
}
