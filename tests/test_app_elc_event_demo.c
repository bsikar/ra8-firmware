/**
 * @file test_app_elc_event_demo.c
 * @brief Integration test: ELC init + link + software-trigger flow
 *
 * @details
 * Mirrors examples/ek_ra8d2/elc_event_demo/main.c bring-up:
 * ra_elc_init -> ra_elc_link(slot, event) ->
 * ra_elc_software_trigger(idx) -> ra_elc_is_enabled. All MMIO
 * is via the host tests/mocks/ra_sim_mmap.c shim.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_elc_regs.h"
#include "ra_elc.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_elc_app_slot         = 0U,
  k_test_elc_app_sw_event_idx = 0U,
  k_test_elc_app_bad_slot     = 99U,
  k_test_elc_app_bad_sw_event = 99U,
} test_elc_app_byte_t;

static void reset_world(void)
{
  ra_sim_mmap_reset();
}

/**
 * @brief Golden-path bring-up replays main.c elc_demo_setup_or_halt.
 *
 * @par MC/DC:
 * Compound decision in app: ``ra_elc_init != ok || ra_elc_link != ok``.
 * Two atomic conditions x N+1 = 3 vectors -- this covers both-ok;
 * bad-slot covers link-fails; the init failure path runs only on
 * a malformed register block which the simulator never produces.
 */
static void test_elc_app_bringup_ok(void)
{
  reset_world();
  TEST_BEGIN("elc_event_demo: init + link ok");
  TEST_ASSERT_EQ(k_ra_ok, ra_elc_init());
  TEST_ASSERT_EQ(k_ra_ok, ra_elc_link((uint8_t)k_test_elc_app_slot, k_ra_elc_event_icu_irq0));
  bool enabled = false;
  TEST_ASSERT_EQ(k_ra_ok, ra_elc_is_enabled(&enabled));
  TEST_ASSERT(enabled);
  TEST_END("elc_event_demo: init + link ok");
}

/**
 * @brief Software trigger fires the documented 3-step sequence.
 *
 * @par MC/DC:
 * Decision in app: ``ra_elc_software_trigger != ok``. Two vectors
 * -- valid event idx (this) + invalid event idx (below) for
 * minimal MC/DC.
 */
static void test_elc_app_software_trigger_ok(void)
{
  reset_world();
  TEST_BEGIN("elc_event_demo: software trigger ok");
  TEST_ASSERT_EQ(k_ra_ok, ra_elc_init());
  TEST_ASSERT_EQ(k_ra_ok, ra_elc_software_trigger((uint8_t)k_test_elc_app_sw_event_idx));
  TEST_END("elc_event_demo: software trigger ok");
}

/**
 * @brief Bad ELSR slot rejected by ra_elc_link.
 *
 * @par MC/DC:
 * Decision: ``elsr_index >= k_ra_elc_elsr_count``. One atomic
 * condition x 2 vectors -- in-range (covered above) + out-of-range
 * (this).
 */
static void test_elc_app_link_bad_slot(void)
{
  reset_world();
  TEST_BEGIN("elc_event_demo: link bad slot rejected");
  TEST_ASSERT_EQ(k_ra_ok, ra_elc_init());
  TEST_ASSERT(ra_elc_link((uint8_t)k_test_elc_app_bad_slot, k_ra_elc_event_icu_irq0) != k_ra_ok);
  TEST_END("elc_event_demo: link bad slot rejected");
}

/**
 * @brief Bad software-event index rejected by ra_elc_software_trigger.
 *
 * @par MC/DC:
 * Decision: ``event_index >= k_ra_elc_segr_count``. One atomic
 * condition x 2 vectors -- in-range (test_elc_app_software_trigger_ok)
 * + out-of-range (this).
 */
static void test_elc_app_software_trigger_bad_idx(void)
{
  reset_world();
  TEST_BEGIN("elc_event_demo: bad sw event idx rejected");
  TEST_ASSERT_EQ(k_ra_ok, ra_elc_init());
  TEST_ASSERT(ra_elc_software_trigger((uint8_t)k_test_elc_app_bad_sw_event) != k_ra_ok);
  TEST_END("elc_event_demo: bad sw event idx rejected");
}

/**
 * @brief NULL out_enabled rejected by ra_elc_is_enabled.
 *
 * @par MC/DC:
 * Decision: ``out_enabled == nullptr``. One atomic condition x 2
 * vectors -- non-NULL (test_elc_app_bringup_ok) + NULL (this).
 */
static void test_elc_app_is_enabled_null(void)
{
  reset_world();
  TEST_BEGIN("elc_event_demo: is_enabled NULL rejected");
  TEST_ASSERT_EQ(k_ra_ok, ra_elc_init());
  TEST_ASSERT(ra_elc_is_enabled(nullptr) != k_ra_ok);
  TEST_END("elc_event_demo: is_enabled NULL rejected");
}

int main(void)
{
  test_elc_app_bringup_ok();
  test_elc_app_software_trigger_ok();
  test_elc_app_link_bad_slot();
  test_elc_app_software_trigger_bad_idx();
  test_elc_app_is_enabled_null();
  return 0;
}
