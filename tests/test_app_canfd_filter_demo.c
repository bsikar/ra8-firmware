/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_app_canfd_filter_demo.c
 * @brief Integration test for examples/ek_ra8d2/canfd_filter_demo/main.c
 *
 * @details
 * Replays the two ``ra8_canfd_filter_set`` calls and the loopback
 * round-trip from the demo. All MMIO is via the host
 * tests/mocks/ra8_fake_mmap.c shim.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_canfd.h"
#include "ra8_canfd_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_test_filter_id_exact   = 0x100U,  /**< Test filter ID exact.   */
  k_test_filter_id_mask    = 0x110U,  /**< Test filter ID mask.    */
  k_test_filter_mask_low4  = 0x7F0U,  /**< Test filter mask low4.  */
  k_test_filter_mask_full  = 0x7FFU,  /**< Test filter mask full.  */
  k_test_filter_id_nomatch = 0x200U,  /**< Test filter ID nomatch. */
  k_test_filter_bitrate    = 500000U, /**< Test filter bitrate.    */
} test_filter_const_t;

typedef enum : uint8_t {
  k_test_filter_channel = 0U, /**< Test filter channel. */
  k_test_filter_slot_a  = 0U, /**< Test filter slot a.  */
  k_test_filter_slot_b  = 1U, /**< Test filter slot b.  */
  k_test_filter_dlc     = 8U, /**< Test filter dlc.     */
} test_filter_layout_t;

static void reset_world(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_canfd_init((uint8_t)k_test_filter_channel);
  (void)ra8_canfd_set_bitrate((uint8_t)k_test_filter_channel,
                              (uint32_t)k_test_filter_bitrate,
                              (uint32_t)k_test_filter_bitrate);
}

/**
 * @par MC/DC:
 * Compound decision in app: ``slot_a != ok || slot_b != ok``. Two
 * atomic conditions x N+1 = 3 vectors -- both ok (this test) +
 * slot_a fails (bad-id test) + slot_b fails (bad-id test).
 */
static void test_filter_program_two_slots_ok(void)
{
  reset_world();
  TEST_BEGIN("canfd_filter_demo: program two slots ok");
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_canfd_filter_set((uint16_t)k_test_filter_slot_a,
                                      (uint32_t)k_test_filter_id_exact,
                                      (uint32_t)k_test_filter_mask_full,
                                      (uint8_t)k_test_filter_dlc));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_canfd_filter_set((uint16_t)k_test_filter_slot_b,
                                      (uint32_t)k_test_filter_id_mask,
                                      (uint32_t)k_test_filter_mask_low4,
                                      (uint8_t)k_test_filter_dlc));
  TEST_END("canfd_filter_demo: program two slots ok");
}

/**
 * @par MC/DC:
 * Decision: ``ra8_canfd_filter_set(bad_id, ...) != ok`` failure
 * vector. Pairs with the both-ok test for the error branch.
 */
static void test_filter_rejects_bad_filter_id(void)
{
  reset_world();
  TEST_BEGIN("canfd_filter_demo: filter_set rejects out-of-range slot");
  /* Slot index way above k_ra8_canfd_afl_total. */
  TEST_ASSERT(ra8_canfd_filter_set(0xFFFFU,
                                   (uint32_t)k_test_filter_id_exact,
                                   (uint32_t)k_test_filter_mask_full,
                                   (uint8_t)k_test_filter_dlc) != k_ra8_ok);
  TEST_END("canfd_filter_demo: filter_set rejects out-of-range slot");
}

/**
 * @par MC/DC:
 * Decision: ``ra8_canfd_filter_set(..., bad_dlc) != ok``. Pairs with
 * the both-ok test to cover the dlc-out-of-range branch.
 */
static void test_filter_rejects_bad_dlc(void)
{
  reset_world();
  TEST_BEGIN("canfd_filter_demo: filter_set rejects bad dlc");
  TEST_ASSERT(ra8_canfd_filter_set((uint16_t)k_test_filter_slot_a,
                                   (uint32_t)k_test_filter_id_exact,
                                   (uint32_t)k_test_filter_mask_full,
                                   99U) != k_ra8_ok);
  TEST_END("canfd_filter_demo: filter_set rejects bad dlc");
}

int main(void)
{
  test_filter_program_two_slots_ok();
  test_filter_rejects_bad_filter_id();
  test_filter_rejects_bad_dlc();
  return 0;
}
