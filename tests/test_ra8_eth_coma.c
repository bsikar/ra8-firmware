/**
 * @file test_ra8_eth_coma.c
 * @brief Unit tests for ra8_eth_coma.c (COMA sub-driver)
 * @details Covers COMA status, control, timeout, and validation paths against deterministic hosted register values.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_eth_coma.h"
#include "ra8_ether_regs.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum eth_coma_fixture_t
 * @brief Values planted in registers to prove a read or write reaches them.
 */
typedef enum : uint16_t {
  k_coma_probe_sts_a = 0x1234U, /**< Planted in COMA_STS to prove the read reaches the register. */
  k_coma_probe_sts_b =
    0x5678U, /**< A second, different value, so the read cannot be a cached first result. */
} eth_coma_fixture_t;

/**
 * @enum eth_coma_fixture2_t
 * @brief Values planted in registers to prove a read or write reaches them.
 */
typedef enum : uint32_t {
  k_coma_probe_sts_wide = 0xC0FFEE00U, /**< A full 32-bit value proving no field is truncated. */
} eth_coma_fixture2_t;

static uint32_t s_coma_cb_count;
static uint32_t s_coma_cb_last_mask;

static void stub_coma_cb(void* ctx, uint32_t mask)
{
  (void)ctx;
  ++s_coma_cb_count;
  s_coma_cb_last_mask = mask;
}

static void prep(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  (void)ra8_mstp_init();
  s_coma_cb_count     = 0U;
  s_coma_cb_last_mask = 0U;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init(void)
{
  TEST_BEGIN("coma init");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_coma_init());
  TEST_END("coma init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("coma deinit");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_coma_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_coma_deinit());
  TEST_END("coma deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_read_and_clear(void)
{
  TEST_BEGIN("coma status read + clear");
  prep();
  ra8_coma()->COMA_STS = k_coma_probe_sts_wide;
  uint32_t mask        = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_coma_get_status(&mask));
  TEST_ASSERT_EQ(0xC0FFEE00U, mask);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_coma_clear_status(0x00FF00FFU));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_eth_coma_get_status(nullptr));
  TEST_END("coma status read + clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("coma attach + dispatch");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_coma_attach_handler(stub_coma_cb, (void*)(uintptr_t)0xC0U));
  ra8_coma()->COMA_STS = k_coma_probe_sts_a;
  ra8_eth_coma_dispatch();
  TEST_ASSERT_EQ(1, s_coma_cb_count);
  TEST_ASSERT_EQ(0x1234U, s_coma_cb_last_mask);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_coma_attach_handler(nullptr, nullptr));
  ra8_coma()->COMA_STS = k_coma_probe_sts_b;
  ra8_eth_coma_dispatch();
  TEST_ASSERT_EQ(1, s_coma_cb_count);
  TEST_END("coma attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("coma power transition");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_coma_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_coma_enter_stop());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_coma_exit_stop());
  TEST_END("coma power transition");
}

/**
 * @brief COMA bring-up happy path: the register sequence completes and the
 *        per-agent clocks are fanned out.
 *
 * @par MC/DC:
 * Decision: `if (bpr_err != k_ra8_ok)` in ra8_eth_coma_bringup (1 condition).
 * - Vector A: bpr_err = k_ra8_ok  -> false (this test -- BPR unarmed, the fake
 *   MMIO seam satisfies the wait on its first poll).
 * - Vector B: bpr_err = k_ra8_err_hw_timeout -> true (test_bringup_bpr_timeout).
 * Vectors A+B prove the condition independently affects the outcome; N+1 = 2
 * vectors for N = 1: minimal MC/DC.
 */
static void test_bringup_happy(void)
{
  TEST_BEGIN("coma bringup happy path");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_coma_bringup());
  /* Step 4 leaves RCEC = RCE | ACE[6:0]; Step 3 kicks CABPIRM.BPIOG. */
  TEST_ASSERT_EQ(k_ra8_coma_rcec_rce | (uint32_t)k_ra8_coma_rcec_ace_mask, *ra8_coma_rcec());
  TEST_ASSERT_EQ(k_ra8_coma_cabpirm_bpiog,
                 *ra8_coma_cabpirm() & (uint32_t)k_ra8_coma_cabpirm_bpiog);
  TEST_END("coma bringup happy path");
}

/**
 * @brief COMA bring-up reports the CABPIRM.BPR wait timing out.
 *
 * @par MC/DC:
 * Decision: `if (bpr_err != k_ra8_ok)` in ra8_eth_coma_bringup (1 condition).
 * See ::test_bringup_happy -- this case supplies Vector B (BPR never asserts,
 * armed via ra8_fake_mmio_fail_wait, so the bounded wait exhausts its budget
 * and bringup returns k_ra8_err_hw_timeout).
 */
static void test_bringup_bpr_timeout(void)
{
  TEST_BEGIN("coma bringup CABPIRM.BPR timeout");
  prep();
  /* Arm the buffer-pool-ready register so BPR never asserts. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)ra8_coma_cabpirm()));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_eth_coma_bringup());
  ra8_fake_mmio_reset();
  TEST_END("coma bringup CABPIRM.BPR timeout");
}

int main(void)
{
  test_init();
  test_deinit();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();
  test_bringup_happy();
  test_bringup_bpr_timeout();
  return 0;
}
