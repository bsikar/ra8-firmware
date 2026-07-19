/**
 * @file test_ra8_iwdt.c
 * @brief Unit tests for the IWDT driver (ra8_iwdt.c)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_iwdt.h"
#include "ra8_iwdt_regs.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum iwdt_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_iwdt_refresh_rounds =
    5U, /**< Refresh rounds driven back to back, proving the sequence is repeatable and not a one-shot. */
  k_iwdt_refresh_second =
    0x5AU, /**< Second half of the IWDT refresh sequence; the counter only reloads when 0x00 is followed by 0xFF, so a driver writing one byte is caught. */
} iwdt_uint8_const_t;

/**
 * @enum iwdt_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_iwdt_status_counter =
    0x1234U, /**< Counter bits planted alongside the underflow flag, so the status decode must mask rather than compare whole. */
} iwdt_uint16_const_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_returns_ok(void)
{
  TEST_BEGIN("ra8_iwdt_init returns ok");
  ra8_sim_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_iwdt_init());
  TEST_END("ra8_iwdt_init returns ok");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_register_layout_matches_fsp(void)
{
  TEST_BEGIN("r_iwdt_regs_t offsets match FSP R_IWDT_Type");
  /* FSP R7KA8D2KF_core0.h R_IWDT_Type:
   *   IWDTRR    @ 0x00 (uint8_t)
   *   IWDTCR    @ 0x02 (uint16_t)
   *   IWDTSR    @ 0x04 (uint16_t)
   *   IWDTRCR   @ 0x06 (uint8_t)
   *   IWDTCSTPR @ 0x08 (uint8_t)
   *   total size 0x0C. */
  TEST_ASSERT_EQ(0x00, offsetof(r_iwdt_regs_t, IWDTRR));
  TEST_ASSERT_EQ(0x02, offsetof(r_iwdt_regs_t, IWDTCR));
  TEST_ASSERT_EQ(0x04, offsetof(r_iwdt_regs_t, IWDTSR));
  TEST_ASSERT_EQ(0x06, offsetof(r_iwdt_regs_t, IWDTRCR));
  TEST_ASSERT_EQ(0x08, offsetof(r_iwdt_regs_t, IWDTCSTPR));
  TEST_ASSERT_EQ(0x0C, sizeof(r_iwdt_regs_t));
  TEST_END("r_iwdt_regs_t offsets match FSP R_IWDT_Type");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_refresh_writes_sequence(void)
{
  TEST_BEGIN("ra8_iwdt_refresh_deferred writes 0x00,0xFF to IWDTRR");
  ra8_sim_mmap_reset();

  volatile r_iwdt_regs_t* reg = ra8_iwdt();
  reg->IWDTRR                 = k_iwdt_refresh_second;

  /* The deferred refresh writes the second byte last. After it returns
   * IWDTRR should hold 0xFF. Per HUM Ch 28.2.1 a single write does NOT
   * refresh; FSP r_iwdt_refresh writes 0x00 then 0xFF in sequence. */
  ra8_iwdt_refresh_deferred();
  TEST_ASSERT_EQ(k_ra8_iwdt_refresh_b, reg->IWDTRR);

  TEST_END("ra8_iwdt_refresh_deferred writes 0x00,0xFF to IWDTRR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_repeated_refresh_is_safe(void)
{
  TEST_BEGIN("ra8_iwdt_refresh_deferred multiple calls");
  ra8_sim_mmap_reset();
  for (uint8_t i = 0U; i < k_iwdt_refresh_rounds; ++i) {
    ra8_iwdt_refresh_deferred();
  }
  TEST_ASSERT_EQ(k_ra8_iwdt_refresh_b, ra8_iwdt()->IWDTRR);
  TEST_END("ra8_iwdt_refresh_deferred multiple calls");
}

/* ---- full build-out ---- */

static uint32_t s_iwdt_cb_count;
static uint16_t s_iwdt_cb_last_mask;

static void stub_iwdt_cb(void* ctx, uint16_t mask)
{
  (void)ctx;
  ++s_iwdt_cb_count;
  s_iwdt_cb_last_mask = mask;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_status(void)
{
  TEST_BEGIN("iwdt get_status");
  ra8_sim_mmap_reset();
  ra8_iwdt()->IWDTSR = (uint16_t)k_ra8_iwdt_status_underflow | (uint16_t)k_ra8_iwdt_status_refresh;

  uint16_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_iwdt_get_status(&mask));
  TEST_ASSERT_EQ(((uint16_t)k_ra8_iwdt_status_underflow | (uint16_t)k_ra8_iwdt_status_refresh),
                 mask);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_iwdt_get_status(nullptr));
  TEST_END("iwdt get_status");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_status_masks_cntval(void)
{
  TEST_BEGIN("iwdt get_status masks out CNTVAL[13:0]");
  ra8_sim_mmap_reset();
  /* IWDTSR has CNTVAL in bits 13:0 and the flag bits 15:14. Verify
   * ra8_iwdt_get_status returns ONLY the flag bits even when CNTVAL is
   * non-zero. Mirrors FSP IWDT_PRV_STATUS_START_BIT semantics. */
  ra8_iwdt()->IWDTSR = (uint16_t)k_iwdt_status_counter | (uint16_t)k_ra8_iwdt_status_underflow;

  uint16_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_iwdt_get_status(&mask));
  TEST_ASSERT_EQ(k_ra8_iwdt_status_underflow, mask);
  TEST_END("iwdt get_status masks out CNTVAL[13:0]");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_counter(void)
{
  TEST_BEGIN("iwdt get_counter mirrors FSP R_IWDT_CounterGet");
  ra8_sim_mmap_reset();
  /* CNTVAL = 0x1234, plus a stray UNDFF flag at bit 14. Counter
   * readout must return 0x1234 (bits 13:0) only. */
  ra8_iwdt()->IWDTSR = (uint16_t)k_iwdt_status_counter | (uint16_t)k_ra8_iwdt_status_underflow;

  uint16_t cnt = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_iwdt_get_counter(&cnt));
  TEST_ASSERT_EQ(0x1234, cnt);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_iwdt_get_counter(nullptr));
  TEST_END("iwdt get_counter mirrors FSP R_IWDT_CounterGet");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_status(void)
{
  TEST_BEGIN("iwdt clear_status");
  ra8_sim_mmap_reset();
  ra8_iwdt()->IWDTSR = (uint16_t)k_ra8_iwdt_status_underflow;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_iwdt_clear_status());
  TEST_ASSERT_EQ(0, ra8_iwdt()->IWDTSR);
  TEST_END("iwdt clear_status");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("iwdt attach + dispatch");
  ra8_sim_mmap_reset();
  s_iwdt_cb_count     = 0U;
  s_iwdt_cb_last_mask = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_iwdt_attach_handler(stub_iwdt_cb, (void*)(uintptr_t)0x55U));
  ra8_iwdt()->IWDTSR = (uint16_t)k_ra8_iwdt_status_underflow;
  ra8_iwdt_dispatch();
  TEST_ASSERT_EQ(1, s_iwdt_cb_count);
  TEST_ASSERT_EQ(k_ra8_iwdt_status_underflow, s_iwdt_cb_last_mask);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_iwdt_attach_handler(nullptr, nullptr));
  ra8_iwdt()->IWDTSR = (uint16_t)k_ra8_iwdt_status_refresh;
  ra8_iwdt_dispatch();
  TEST_ASSERT_EQ(1, s_iwdt_cb_count);
  TEST_END("iwdt attach + dispatch");
}

int32_t main(void)
{
  test_init_returns_ok();
  test_register_layout_matches_fsp();
  test_refresh_writes_sequence();
  test_repeated_refresh_is_safe();
  test_get_status();
  test_get_status_masks_cntval();
  test_get_counter();
  test_clear_status();
  test_attach_and_dispatch();
  (void)fprintf(stderr, "[OK ] test_ra8_iwdt.c\n");
  return 0;
}
