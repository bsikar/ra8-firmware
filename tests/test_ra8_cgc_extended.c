/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_cgc_extended.c
 * @brief Extended unit tests for ra8_cgc.c covering previously uncovered paths
 */

#include <stdint.h>

#include "ra8_cgc.h"
#include "ra8_cgc_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_system_regs.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_cgc_ext_all_oscsf = 0xFFU, /**< All oscillator-stable bits set. */
} ra8_cgc_ext_bits_t;

typedef enum : uint32_t {
  k_cgc_ext_second_wait = 1U, /**< fail-nth index of the 2nd OSCSF wait-loop. */
} ra8_cgc_ext_wait_idx_t;

/** @brief Reset the fake register mirror and the MMIO fault seam together. */
static void cgc_ext_reset(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
}

/* ---------------------------------------------------------------------------
 * ra8_cgc_get_clock_hz -- double-init / second-init paths
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify get_clock_hz returns MOCO rate before init.
 *
 * @pre ra8_fake_mmap_reset called; no ra8_cgc_init.
 * @post Returns k_ra8_ok and a non-zero Hz value.
 *
 * @par MC/DC:
 * (no compound decisions -- single bounds check)
 *
 * @since 0.1.0
 */
static void test_cgc_get_clock_hz_before_init(void)
{
  TEST_BEGIN("cgc get_clock_hz before init returns MOCO default");
  cgc_ext_reset();
  uint32_t hz = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk1, &hz));
  TEST_ASSERT(hz != 0U);
  TEST_END("cgc get_clock_hz before init returns MOCO default");
}

/* ---------------------------------------------------------------------------
 * ra8_cgc_switch_pll1_target -- zero hz rejected
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify switch_pll1_target rejects zero hz.
 *
 * @pre None.
 * @post Returns k_ra8_err_invalid_arg.
 *
 * @par MC/DC:
 * Decision: ``new_cpuclk_hz == 0U``
 * - V1: hz=0        -> true  -> error.
 * - V2: hz=500 MHz  -> false -> proceed (tested in test_ra8_cgc.c).
 *
 * @since 0.1.0
 */
static void test_cgc_switch_pll1_zero_hz(void)
{
  TEST_BEGIN("cgc switch_pll1_target rejects 0 hz");
  cgc_ext_reset();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_cgc_switch_pll1_target(0U));
  TEST_END("cgc switch_pll1_target rejects 0 hz");
}

/**
 * @brief Verify switch_pll1_target with preseeded OSCSF updates cpuclk0.
 *
 * @pre OSCSF preseeded; ra8_cgc_init called.
 * @post get_clock_hz for cpuclk0 returns the supplied value.
 *
 * @par MC/DC:
 * (no compound decisions -- happy path covered; zero path covered above)
 *
 * @since 0.1.0
 */
static void test_cgc_switch_pll1_ok(void)
{
  TEST_BEGIN("cgc switch_pll1_target ok updates cpuclk0");
  cgc_ext_reset();
  *ra8_sys_oscsf() = (uint8_t)k_cgc_ext_all_oscsf;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_init());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_switch_pll1_target(250000000U));
  uint32_t hz = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &hz));
  TEST_ASSERT_EQ(250000000U, hz);
  TEST_END("cgc switch_pll1_target ok updates cpuclk0");
}

/* ---------------------------------------------------------------------------
 * ra8_cgc_enable_stop_detection / ra8_cgc_disable_stop_detection
 * ---------------------------------------------------------------------------
 */

static int32_t s_cgc_ext_ostd_count;

static void stub_ostd(void* ctx)
{
  (void)ctx;
  ++s_cgc_ext_ostd_count;
}

/**
 * @brief Verify enable_stop_detection null handler is rejected.
 *
 * @pre None.
 * @post Returns k_ra8_err_null_ptr.
 *
 * @par MC/DC:
 * Decision: ``handler == nullptr``
 * - V1: handler=null       -> true  -> error.
 * - V2: handler=valid      -> false -> ok (covered in base tests).
 *
 * @since 0.1.0
 */
static void test_cgc_stop_detection_null_handler(void)
{
  TEST_BEGIN("cgc enable_stop_detection null handler rejected");
  cgc_ext_reset();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cgc_enable_stop_detection(nullptr, nullptr));
  TEST_END("cgc enable_stop_detection null handler rejected");
}

/**
 * @brief Verify stop detection enable/disable does not fire when disabled.
 *
 * @pre enable then disable before trigger.
 * @post Callback count stays at 0 after trigger with handler disabled.
 *
 * @par MC/DC:
 * Decision: ``s_ostd_enabled`` (inside fake_trigger)
 * - V1: enabled=true  -> fires  (tested in base test_ra8_cgc.c).
 * - V2: enabled=false -> no-op.
 *
 * @since 0.1.0
 */
static void test_cgc_stop_detection_disabled_no_fire(void)
{
  TEST_BEGIN("cgc stop detection no fire when disabled");
  cgc_ext_reset();
  s_cgc_ext_ostd_count = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_enable_stop_detection(stub_ostd, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_disable_stop_detection());
  ra8_cgc_fake_trigger_stop_detection();
  TEST_ASSERT_EQ(0, s_cgc_ext_ostd_count);
  TEST_END("cgc stop detection no fire when disabled");
}

/* ---------------------------------------------------------------------------
 * ra8_cgc_pll2_enable -- validation paths
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify pll2_enable rejects zero mul_int.
 *
 * @pre None.
 * @post Returns k_ra8_err_invalid_arg.
 *
 * @par MC/DC:
 * Decision: ``mul_int == 0U``
 * - V1: mul_int=0    -> true  -> error.
 * - V2: mul_int=80   -> false -> proceed.
 *
 * @since 0.1.0
 */
static void test_cgc_pll2_enable_zero_mul(void)
{
  TEST_BEGIN("cgc pll2_enable zero mul_int rejected");
  cgc_ext_reset();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_cgc_pll2_enable(0U, 0U, k_ra8_plodiv_div4));
  TEST_END("cgc pll2_enable zero mul_int rejected");
}

/**
 * @brief Verify pll2_enable rejects out-of-range mul_quarters.
 *
 * @pre None.
 * @post Returns k_ra8_err_invalid_arg for mul_quarters > 3.
 *
 * @par MC/DC:
 * Decision: ``(uint16_t)mul_quarters > (uint16_t)k_ra8_pll2_max_quarters``
 * - V1: quarters=0  -> false -> proceed.
 * - V2: quarters=4  -> true  -> error.
 *
 * @since 0.1.0
 */
static void test_cgc_pll2_enable_bad_quarters(void)
{
  TEST_BEGIN("cgc pll2_enable bad mul_quarters rejected");
  cgc_ext_reset();
  /* k_ra8_pll2_max_quarters is 3; supply 4 to trigger the guard. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_cgc_pll2_enable(80U, 4U, k_ra8_plodiv_div4));
  TEST_END("cgc pll2_enable bad mul_quarters rejected");
}

/**
 * @brief Verify pll2_enable skips re-programming when PLL2 already locked.
 *
 * @pre OSCSF PLL2SF bit preseeded.
 * @post Returns k_ra8_ok immediately without re-programming.
 *
 * @par MC/DC:
 * Decision: ``(*ra8_sys_oscsf() & pll2sf_bit) != 0U``
 * - V1: PLL2SF=0 -> false -> program (normal path).
 * - V2: PLL2SF=1 -> true  -> early ok (idempotency path).
 *
 * @since 0.1.0
 */
static void test_cgc_pll2_enable_already_locked(void)
{
  TEST_BEGIN("cgc pll2_enable returns ok when PLL2 already locked");
  cgc_ext_reset();
  /* Set PLL2SF bit in OSCSF to pretend PLL2 is already running
   * (HUM Ch 9.2.7, k_ra8_oscsf_bit_pll2sf = 6). */
  *ra8_sys_oscsf() = (uint8_t)(1U << k_ra8_oscsf_bit_pll2sf);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_pll2_enable(80U, 0U, k_ra8_plodiv_div4));
  TEST_END("cgc pll2_enable returns ok when PLL2 already locked");
}

/**
 * @brief Verify pll2_enable happy path with all OSCSF bits preseeded.
 *
 * @pre All OSCSF bits set so the PLL2 lock poll returns immediately.
 * @post Returns k_ra8_ok.
 *
 * @par MC/DC:
 * (normal program path -- all guards false)
 *
 * @since 0.1.0
 */
static void test_cgc_pll2_enable_ok(void)
{
  TEST_BEGIN("cgc pll2_enable happy path");
  cgc_ext_reset();
  *ra8_sys_oscsf() = (uint8_t)k_cgc_ext_all_oscsf;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_pll2_enable(80U, 0U, k_ra8_plodiv_div4));
  TEST_END("cgc pll2_enable happy path");
}

/**
 * @brief Verify pll2_enable timeout when OSCSF never locks.
 *
 * @pre OSCSF left at zero.
 * @post Returns k_ra8_err_hw_timeout.
 *
 * @par MC/DC:
 * (PLL2 lock wait timeout path)
 *
 * @since 0.1.0
 */
static void test_cgc_pll2_enable_timeout(void)
{
  TEST_BEGIN("cgc pll2_enable times out when PLL2 never locks");
  cgc_ext_reset();
  /* Fail only the SECOND OSCSF wait-loop (the PLL2SF lock poll); the
   * first loop (the PLL2SF stop-clear poll) succeeds, so the timeout
   * surfaces from the lock leg specifically. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fake_mmio_fail_nth_wait((const volatile void*)ra8_sys_oscsf(),
                                             (uint32_t)k_cgc_ext_second_wait));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_pll2_enable(80U, 0U, k_ra8_plodiv_div4));
  TEST_END("cgc pll2_enable times out when PLL2 never locks");
}

/* ---------------------------------------------------------------------------
 * ra8_cgc_usbfs_clock_enable
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify usbfs_clock_enable returns ok with all stabs preseeded.
 *
 * @pre All OSCSF bits preseeded.
 * @post Returns k_ra8_ok.
 *
 * @par MC/DC:
 * (no compound decisions exercised here -- happy path)
 *
 * @since 0.1.0
 */
static void test_cgc_usbfs_clock_enable_ok(void)
{
  TEST_BEGIN("cgc usbfs_clock_enable ok with OSCSF preseeded");
  cgc_ext_reset();
  *ra8_sys_oscsf() = (uint8_t)k_cgc_ext_all_oscsf;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_usbfs_clock_enable());
  TEST_END("cgc usbfs_clock_enable ok with OSCSF preseeded");
}

/**
 * @brief Verify usbfs_clock_enable times out when OSCSF=0.
 *
 * @pre OSCSF left at zero.
 * @post Returns k_ra8_err_hw_timeout.
 *
 * @par MC/DC:
 * (PLL2 lock timeout branch inside usbfs_clock_enable)
 *
 * @since 0.1.0
 */
static void test_cgc_usbfs_clock_enable_timeout(void)
{
  TEST_BEGIN("cgc usbfs_clock_enable times out when PLL2 never locks");
  cgc_ext_reset();
  /* Fail the second OSCSF wait-loop: PLL2 stop-clear succeeds, the
   * PLL2SF lock poll inside the nested pll2_enable times out and the
   * error propagates out of usbfs_clock_enable. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fake_mmio_fail_nth_wait((const volatile void*)ra8_sys_oscsf(),
                                             (uint32_t)k_cgc_ext_second_wait));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_usbfs_clock_enable());
  TEST_END("cgc usbfs_clock_enable times out when PLL2 never locks");
}

/* ---------------------------------------------------------------------------
 * ra8_cgc_usbhs_pll_enable
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify usbhs_pll_enable returns ok with all OSCSF bits preseeded.
 *
 * @pre All OSCSF bits preseeded.
 * @post Returns k_ra8_ok.
 *
 * @par MC/DC:
 * (no compound decisions -- happy path)
 *
 * @since 0.1.0
 */
static void test_cgc_usbhs_pll_enable_ok(void)
{
  TEST_BEGIN("cgc usbhs_pll_enable ok with OSCSF preseeded");
  cgc_ext_reset();
  *ra8_sys_oscsf() = (uint8_t)k_cgc_ext_all_oscsf;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_usbhs_pll_enable());
  TEST_END("cgc usbhs_pll_enable ok with OSCSF preseeded");
}

/**
 * @brief Verify usbhs_pll_enable times out when OSCSF=0.
 *
 * @pre OSCSF left at zero.
 * @post Returns k_ra8_err_hw_timeout.
 *
 * @par MC/DC:
 * (PLL timeout branch)
 *
 * @since 0.1.0
 */
static void test_cgc_usbhs_pll_enable_timeout(void)
{
  TEST_BEGIN("cgc usbhs_pll_enable times out when MOSCSF never sets");
  cgc_ext_reset();
  /* Fail the first OSCSF wait-loop: the main-XTAL stabilisation poll
   * at the top of usbhs_pll_enable burns its budget and returns. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)ra8_sys_oscsf()));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_usbhs_pll_enable());
  TEST_END("cgc usbhs_pll_enable times out when MOSCSF never sets");
}

int32_t main(void)
{
  test_cgc_get_clock_hz_before_init();
  test_cgc_switch_pll1_zero_hz();
  test_cgc_switch_pll1_ok();
  test_cgc_stop_detection_null_handler();
  test_cgc_stop_detection_disabled_no_fire();
  test_cgc_pll2_enable_zero_mul();
  test_cgc_pll2_enable_bad_quarters();
  test_cgc_pll2_enable_already_locked();
  test_cgc_pll2_enable_ok();
  test_cgc_pll2_enable_timeout();
  test_cgc_usbfs_clock_enable_ok();
  test_cgc_usbfs_clock_enable_timeout();
  test_cgc_usbhs_pll_enable_ok();
  test_cgc_usbhs_pll_enable_timeout();
  (void)fprintf(stderr, "[OK  ] test_ra8_cgc_extended.c\n");
  return 0;
}
