/**
 * @file test_app_canfd_filter_demo.c
 * @brief Integration test for examples/ek_ra8d2/canfd_filter_demo/main.c
 *
 * @details
 * Replays the two ``ra_canfd_filter_set`` calls and the loopback
 * round-trip from the demo. All MMIO is via the host
 * tests/mocks/ra_sim_mmap.c shim.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_canfd_regs.h"
#include "ra_canfd.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_test_filter_id_exact   = 0x100U,
  k_test_filter_id_mask    = 0x110U,
  k_test_filter_mask_low4  = 0x7F0U,
  k_test_filter_mask_full  = 0x7FFU,
  k_test_filter_id_nomatch = 0x200U,
  k_test_filter_bitrate    = 500000U,
} test_filter_const_t;

typedef enum : uint8_t {
  k_test_filter_channel = 0U,
  k_test_filter_slot_a  = 0U,
  k_test_filter_slot_b  = 1U,
  k_test_filter_dlc     = 8U,
} test_filter_layout_t;

static void reset_world(void)
{
  ra_sim_mmap_reset();
  (void)ra_canfd_init((uint8_t)k_test_filter_channel);
  (void)ra_canfd_set_bitrate((uint8_t)k_test_filter_channel,
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
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_canfd_filter_set((uint16_t)k_test_filter_slot_a,
                                          (uint32_t)k_test_filter_id_exact,
                                          (uint32_t)k_test_filter_mask_full,
                                          (uint8_t)k_test_filter_dlc));
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_canfd_filter_set((uint16_t)k_test_filter_slot_b,
                                          (uint32_t)k_test_filter_id_mask,
                                          (uint32_t)k_test_filter_mask_low4,
                                          (uint8_t)k_test_filter_dlc));
  TEST_END("canfd_filter_demo: program two slots ok");
}

/**
 * @par MC/DC:
 * Decision: ``ra_canfd_filter_set(bad_id, ...) != ok`` failure
 * vector. Pairs with the both-ok test for the error branch.
 */
static void test_filter_rejects_bad_filter_id(void)
{
  reset_world();
  TEST_BEGIN("canfd_filter_demo: filter_set rejects out-of-range slot");
  /* Slot index way above k_ra_canfd_afl_total. */
  TEST_ASSERT(ra_canfd_filter_set(0xFFFFU,
                                  (uint32_t)k_test_filter_id_exact,
                                  (uint32_t)k_test_filter_mask_full,
                                  (uint8_t)k_test_filter_dlc) != k_ra_ok);
  TEST_END("canfd_filter_demo: filter_set rejects out-of-range slot");
}

/**
 * @par MC/DC:
 * Decision: ``ra_canfd_filter_set(..., bad_dlc) != ok``. Pairs with
 * the both-ok test to cover the dlc-out-of-range branch.
 */
static void test_filter_rejects_bad_dlc(void)
{
  reset_world();
  TEST_BEGIN("canfd_filter_demo: filter_set rejects bad dlc");
  TEST_ASSERT(ra_canfd_filter_set((uint16_t)k_test_filter_slot_a,
                                  (uint32_t)k_test_filter_id_exact,
                                  (uint32_t)k_test_filter_mask_full,
                                  99U) != k_ra_ok);
  TEST_END("canfd_filter_demo: filter_set rejects bad dlc");
}

int main(void)
{
  test_filter_program_two_slots_ok();
  test_filter_rejects_bad_filter_id();
  test_filter_rejects_bad_dlc();
  return 0;
}
