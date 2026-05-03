/**
 * @file test_ra_elc.c
 * @brief Unit tests for ra_elc.c (Event Link Controller helper)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_elc_regs.h"
#include "ra_elc.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
}

typedef enum : uint8_t {
  k_ra_elc_test_slot_first = 0U,
  k_ra_elc_test_slot_mid   = 10U,
  k_ra_elc_test_slot_last  = 52U, /**< FSP R_ELC has ELSR[0..52]. */
  k_ra_elc_test_slot_bad   = 53U,
  k_ra_elc_test_slot_huge  = 200U,
  k_ra_elc_test_seg_bad    = 4U, /**< RA8D2 has ELSEGR[0..3]. */
} ra_elc_test_const_t;

static volatile uint8_t* test_elcr(void)
{
  return (volatile uint8_t*)(k_ra_elc_base_addr + k_ra_elc_off_elcr);
}

static volatile uint16_t* test_elsr(uint8_t index)
{
  /* FSP R_ELC_ELSR_Type is 4 bytes wide (16-bit data + 16-bit pad). */
  return (volatile uint16_t*)(k_ra_elc_base_addr + k_ra_elc_off_elsr0 +
                              ((uintptr_t)index * (uintptr_t)k_ra_elc_elsr_stride));
}

static volatile uint8_t* test_elsegr(uint8_t group)
{
  return (volatile uint8_t*)(k_ra_elc_base_addr + k_ra_elc_off_elsegr0 +
                             ((uintptr_t)group * (uintptr_t)k_ra_elc_elsegr_stride));
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_link_first_slot(void)
{
  TEST_BEGIN("elc link first slot");
  prep();

  const ra_err_t err = ra_elc_link((uint8_t)k_ra_elc_test_slot_first, k_ra_elc_event_icu_irq0);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)err);
  TEST_ASSERT_EQ((int32_t)k_ra_elc_event_icu_irq0,
                 (int32_t)*test_elsr((uint8_t)k_ra_elc_test_slot_first));
  TEST_END("elc link first slot");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_link_mid_slot(void)
{
  TEST_BEGIN("elc link mid slot");
  prep();

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_elc_link((uint8_t)k_ra_elc_test_slot_mid, k_ra_elc_event_icu_irq1));
  TEST_ASSERT_EQ((int32_t)k_ra_elc_event_icu_irq1,
                 (int32_t)*test_elsr((uint8_t)k_ra_elc_test_slot_mid));
  TEST_END("elc link mid slot");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_link_last_slot(void)
{
  TEST_BEGIN("elc link last valid slot");
  prep();

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_elc_link((uint8_t)k_ra_elc_test_slot_last, k_ra_elc_event_icu_irq15));
  TEST_END("elc link last valid slot");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_link_bad_slot(void)
{
  TEST_BEGIN("elc link bad slot");
  prep();

  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range,
                 (int32_t)ra_elc_link((uint8_t)k_ra_elc_test_slot_bad, k_ra_elc_event_none));
  TEST_END("elc link bad slot");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_link_huge_slot(void)
{
  TEST_BEGIN("elc link huge slot");
  prep();

  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range,
                 (int32_t)ra_elc_link((uint8_t)k_ra_elc_test_slot_huge, k_ra_elc_event_none));
  TEST_END("elc link huge slot");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_enables_controller(void)
{
  TEST_BEGIN("ra_elc_init: ELSR cleared + ELCON set");
  prep();

  /* Pollute ELSR0 so we can verify init clears it. */
  *test_elsr(0U) = 0x123U;

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_elc_init());
  TEST_ASSERT_EQ((int32_t)(1U << k_ra_elcr_bit_elcon), (int32_t)*test_elcr());
  TEST_ASSERT_EQ(0, (int32_t)*test_elsr(0U));

  bool enabled = false;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_elc_is_enabled(&enabled));
  TEST_ASSERT(enabled);
  TEST_END("ra_elc_init: ELSR cleared + ELCON set");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit_clears_elcon(void)
{
  TEST_BEGIN("ra_elc_deinit: ELCON cleared");
  prep();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_elc_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_elc_deinit());
  TEST_ASSERT_EQ(0, (int32_t)*test_elcr());

  bool enabled = true;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_elc_is_enabled(&enabled));
  TEST_ASSERT(!enabled);
  TEST_END("ra_elc_deinit: ELCON cleared");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_unlink_clears_slot(void)
{
  TEST_BEGIN("ra_elc_unlink: slot zeroed");
  prep();

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_elc_link((uint8_t)k_ra_elc_test_slot_mid, k_ra_elc_event_icu_irq1));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_elc_unlink((uint8_t)k_ra_elc_test_slot_mid));
  TEST_ASSERT_EQ(0, (int32_t)*test_elsr((uint8_t)k_ra_elc_test_slot_mid));

  TEST_ASSERT_EQ((int32_t)k_ra_err_out_of_range,
                 (int32_t)ra_elc_unlink((uint8_t)k_ra_elc_test_slot_bad));
  TEST_END("ra_elc_unlink: slot zeroed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_software_trigger(void)
{
  TEST_BEGIN("ra_elc_software_trigger: 3-step sequence latches 0x41");
  prep();

  /* After the documented HUM Ch 19.2.2 sequence
   * (0x00 -> 0x40 -> 0x41) the last byte the driver wrote (which is
   * what the simulator memory observes) is the trigger value 0x41. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_elc_software_trigger(0U));
  TEST_ASSERT_EQ((int32_t)k_ra_elc_elsegr_step_trigger, (int32_t)*test_elsegr(0U));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_elc_software_trigger(3U));
  TEST_ASSERT_EQ((int32_t)k_ra_elc_elsegr_step_trigger, (int32_t)*test_elsegr(3U));

  /* Out-of-range index: FSP has ELSEGR0..3 so index 4+ is rejected. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_elc_software_trigger((uint8_t)k_ra_elc_test_seg_bad));
  TEST_END("ra_elc_software_trigger: 3-step sequence latches 0x41");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_is_enabled_null_out(void)
{
  TEST_BEGIN("ra_elc_is_enabled: NULL out rejected");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_elc_is_enabled(nullptr));
  TEST_END("ra_elc_is_enabled: NULL out rejected");
}

int32_t main(void)
{
  test_link_first_slot();
  test_link_mid_slot();
  test_link_last_slot();
  test_link_bad_slot();
  test_link_huge_slot();
  test_init_enables_controller();
  test_deinit_clears_elcon();
  test_unlink_clears_slot();
  test_software_trigger();
  test_is_enabled_null_out();
  (void)fprintf(stderr, "[OK  ] test_ra_elc.c\n");
  return 0;
}
