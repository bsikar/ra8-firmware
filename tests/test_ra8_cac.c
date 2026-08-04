/**
 * @file test_ra8_cac.c
 * @brief Unit tests for ra8_cac.c (Clock Accuracy Check)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_cac.h"
#include "ra8_cac_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_ra8_cac_test_upper    = 0xAAAAU, /**< RA8 cac test upper.    */
  k_ra8_cac_test_lower    = 0x5555U, /**< RA8 cac test lower.    */
  k_ra8_cac_test_captured = 0xBEEFU, /**< RA8 cac test captured. */
} ra8_cac_test_values_t;

typedef enum : uint8_t {
  k_ra8_cac_test_mendf_bit = 1U, /**< CASTR.MENDF bit. */
} ra8_cac_test_bits_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_writes_limits(void)
{
  TEST_BEGIN("cac init writes limits");
  ra8_fake_mmap_reset();

  const ra8_err_t err =
    ra8_cac_init((uint16_t)k_ra8_cac_test_upper, (uint16_t)k_ra8_cac_test_lower);
  TEST_ASSERT_EQ(k_ra8_ok, err);

  volatile r_cac_regs_t* reg = ra8_cac();
  TEST_ASSERT_EQ(k_ra8_cac_test_upper, reg->CAULVR);
  TEST_ASSERT_EQ(k_ra8_cac_test_lower, reg->CALLVR);
  TEST_ASSERT_EQ(0, reg->CACR0);
  TEST_ASSERT_EQ(0, reg->CACR1);
  TEST_ASSERT_EQ(0, reg->CACR2);
  TEST_END("cac init writes limits");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_measure_null_out_pointer(void)
{
  TEST_BEGIN("cac measure null out pointer");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cac_measure(nullptr));
  TEST_END("cac measure null out pointer");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_measure_happy_path(void)
{
  TEST_BEGIN("cac measure happy path");
  ra8_fake_mmap_reset();

  /* Pre-seed CASTR.MENDF so the poll loop exits immediately, and
   * pre-seed CACNTBR so the captured value is predictable. */
  volatile r_cac_regs_t* reg = ra8_cac();
  reg->CASTR                 = (uint8_t)(1U << k_ra8_cac_test_mendf_bit);
  reg->CACNTBR               = (uint16_t)k_ra8_cac_test_captured;

  uint16_t        captured = 0U;
  const ra8_err_t err      = ra8_cac_measure(&captured);
  TEST_ASSERT_EQ(k_ra8_ok, err);
  TEST_ASSERT_EQ(k_ra8_cac_test_captured, captured);
  /* CACR0 should have been cleared by the driver after success. */
  TEST_ASSERT_EQ(0, reg->CACR0);
  TEST_END("cac measure happy path");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_measure_timeout(void)
{
  TEST_BEGIN("cac measure timeout");
  ra8_fake_mmap_reset();

  /* Leave CASTR.MENDF clear so the poll loop burns its budget
   * and eventually returns k_ra8_err_hw_timeout. */
  uint16_t        captured = 0U;
  const ra8_err_t err      = ra8_cac_measure(&captured);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, err);
  TEST_END("cac measure timeout");
}

/* ---- full build-out ---- */

static uint32_t s_cac_cb_count;
static uint8_t  s_cac_cb_last_mask;

static void stub_cac_cb(void* ctx, uint8_t mask)
{
  (void)ctx;
  ++s_cac_cb_count;
  s_cac_cb_last_mask = mask;
}

static void prep_w43(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  s_cac_cb_count     = 0U;
  s_cac_cb_last_mask = 0U;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("cac deinit");
  prep_w43();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_cac_init((uint16_t)k_ra8_cac_test_upper, (uint16_t)k_ra8_cac_test_lower));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cac_deinit());
  TEST_ASSERT_EQ(0, ra8_cac()->CACR0);
  TEST_END("cac deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_read_and_clear(void)
{
  TEST_BEGIN("cac status read + clear");
  prep_w43();

  ra8_cac()->CASTR = (uint8_t)k_ra8_cac_status_mendf | (uint8_t)k_ra8_cac_status_ferrf;
  uint8_t mask     = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cac_get_status(&mask));
  TEST_ASSERT_EQ(((uint8_t)k_ra8_cac_status_mendf | (uint8_t)k_ra8_cac_status_ferrf), mask);

  /* clear_status must write the W1C bits in CAICR -- FERRFCL=bit4,
   * MENDFCL=bit5, OVFFCL=bit6 -- which sit 4 bits above the CASTR
   * FERRF/MENDF/OVFF flags. */
  ra8_cac()->CAICR = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cac_clear_status((uint8_t)k_ra8_cac_status_mendf));
  enum : uint8_t { k_castr_to_caicr_shift = 4U /**< Castr to caicr shift. */ };
  TEST_ASSERT_EQ(((uint8_t)k_ra8_cac_status_mendf << k_castr_to_caicr_shift), ra8_cac()->CAICR);

  ra8_cac()->CAICR = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cac_clear_status((uint8_t)k_ra8_cac_status_ferrf));
  TEST_ASSERT_EQ(((uint8_t)k_ra8_cac_status_ferrf << k_castr_to_caicr_shift), ra8_cac()->CAICR);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cac_get_status(nullptr));
  TEST_END("cac status read + clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("cac attach + dispatch");
  prep_w43();

  enum : uint8_t { k_castr_to_caicr_shift = 4U /**< Castr to caicr shift. */ };

  TEST_ASSERT_EQ(k_ra8_ok, ra8_cac_attach_handler(stub_cac_cb, (void*)(uintptr_t)0xC0U));
  ra8_cac()->CASTR = (uint8_t)k_ra8_cac_status_mendf;
  ra8_cac()->CAICR = 0U;
  ra8_cac_dispatch();
  TEST_ASSERT_EQ(1, s_cac_cb_count);
  TEST_ASSERT_EQ(k_ra8_cac_status_mendf, s_cac_cb_last_mask);
  /* dispatch must clear via the W1C *CL bits in CAICR, not by
   * stomping the IE-enable bits. */
  TEST_ASSERT_EQ(((uint8_t)k_ra8_cac_status_mendf << k_castr_to_caicr_shift), ra8_cac()->CAICR);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_cac_attach_handler(nullptr, nullptr));
  ra8_cac()->CASTR = (uint8_t)k_ra8_cac_status_ovff;
  ra8_cac_dispatch();
  TEST_ASSERT_EQ(1, s_cac_cb_count);
  TEST_END("cac attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("cac power transition");
  prep_w43();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_cac_init((uint16_t)k_ra8_cac_test_upper, (uint16_t)k_ra8_cac_test_lower));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cac_enter_stop());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cac_exit_stop());
  TEST_END("cac power transition");
}

int32_t main(void)
{
  test_init_writes_limits();
  test_measure_null_out_pointer();
  test_measure_happy_path();
  test_measure_timeout();
  test_deinit();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();
  (void)fprintf(stderr, "[OK ] test_ra8_cac.c\n");
  return 0;
}
