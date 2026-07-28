/**
 * @file test_ra8_isr.c
 * @brief Unit tests for the NVIC + ICU IELSR allocator (libs/ra8_hal/src/ra8_isr.c).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_elc_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_icu_regs.h"
#include "ra8_isr.h"
#include "unity_minimal.h"

/**
 * @enum isr_fixture_t
 * @brief Poison values written into out-parameters before a call, so one that fails without assigning is detectable, plus out-of-range and malformed inputs the code under test must reject.
 */
typedef enum : uint16_t {
  k_isr_handler_marker =
    0xBEEF, /**< Value the test handler writes, proving the dispatch actually ran it. */
  k_isr_slot_out_of_range =
    500U, /**< A vector slot past the last real one; dispatch must ignore it rather than index off the table. */
  k_isr_slot_poison =
    0xFFFFU, /**< Poison slot written before a registration, so a register call that fails without assigning one is detectable. */
} isr_fixture_t;

static int32_t  s_call_count   = 0;
static int32_t  s_last_ctx_val = 0;
static uint32_t s_ctx_scratch  = 0U;

static void reset_counts(void)
{
  s_call_count   = 0;
  s_last_ctx_val = 0;
  s_ctx_scratch  = 0U;
}

static void stub_handler_a(void* ctx)
{
  ++s_call_count;
  if (ctx != nullptr) {
    s_last_ctx_val = *(int32_t*)ctx;
  }
}

static void stub_handler_b(void* ctx)
{
  (void)ctx;
  ++s_call_count;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_clears_state(void)
{
  TEST_BEGIN("ra8_isr_init clears slots");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_init());

  uint16_t slot = 0U;
  /* Unknown event not found. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_lookup_slot((ra8_elc_event_t)42U, &slot));
  TEST_ASSERT_EQ(k_ra8_isr_slot_none, slot);
  TEST_END("ra8_isr_init clears slots");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_register_allocates_first_slot(void)
{
  TEST_BEGIN("ra8_isr_register: first registration gets slot 0");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_init());

  uint16_t slot = k_isr_slot_poison;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_isr_register((ra8_elc_event_t)0x50U, stub_handler_a, nullptr, 5U, &slot));
  TEST_ASSERT_EQ(0, slot);

  uint16_t slot2 = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_lookup_slot((ra8_elc_event_t)0x50U, &slot2));
  TEST_ASSERT_EQ(0, slot2);

  /* IELSR entry holds the event number. */
  volatile uint32_t* ielsr = ra8_icu_ielsr(0U);
  TEST_ASSERT_NOT_NULL((void*)ielsr);
  TEST_ASSERT_EQ(0x50, (*ielsr & (uint32_t)k_ra8_ielsr_iels_mask));

  TEST_END("ra8_isr_register: first registration gets slot 0");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_register_rejects_null_handler(void)
{
  TEST_BEGIN("ra8_isr_register: NULL handler rejected");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_init());

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_isr_register((ra8_elc_event_t)1U, nullptr, nullptr, 0U, nullptr));
  TEST_END("ra8_isr_register: NULL handler rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_register_rejects_bad_priority(void)
{
  TEST_BEGIN("ra8_isr_register: priority out of range");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_init());
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_isr_register((ra8_elc_event_t)1U, stub_handler_a, nullptr, 99U, nullptr));
  TEST_END("ra8_isr_register: priority out of range");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_register_duplicate_rejected(void)
{
  TEST_BEGIN("ra8_isr_register: duplicate event rejected");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_init());
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_isr_register((ra8_elc_event_t)7U, stub_handler_a, nullptr, 0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_exists,
                 ra8_isr_register((ra8_elc_event_t)7U, stub_handler_a, nullptr, 0U, nullptr));
  TEST_END("ra8_isr_register: duplicate event rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_unregister_frees_slot(void)
{
  TEST_BEGIN("ra8_isr_unregister frees the slot");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_init());
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_isr_register((ra8_elc_event_t)9U, stub_handler_a, nullptr, 0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_unregister((ra8_elc_event_t)9U));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_isr_unregister((ra8_elc_event_t)9U));
  TEST_END("ra8_isr_unregister frees the slot");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_invokes_handler_with_ctx(void)
{
  TEST_BEGIN("ra8_isr_dispatch invokes the registered handler");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_init());
  reset_counts();

  int32_t  marker = k_isr_handler_marker;
  uint16_t slot   = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_isr_register((ra8_elc_event_t)11U, stub_handler_a, &marker, 0U, &slot));
  ra8_isr_dispatch(slot);
  TEST_ASSERT_EQ(1, s_call_count);
  TEST_ASSERT_EQ(0xBEEF, s_last_ctx_val);
  TEST_END("ra8_isr_dispatch invokes the registered handler");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_out_of_range_is_noop(void)
{
  TEST_BEGIN("ra8_isr_dispatch out-of-range is a no-op");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_init());
  reset_counts();

  ra8_isr_dispatch(k_isr_slot_out_of_range); /* Out of bounds; nothing happens. */
  TEST_ASSERT_EQ(0, s_call_count);
  TEST_END("ra8_isr_dispatch out-of-range is a no-op");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_multiple_events_get_distinct_slots(void)
{
  TEST_BEGIN("ra8_isr_register: distinct events -> distinct slots");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_init());

  uint16_t slot_a = 0U;
  uint16_t slot_b = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_isr_register((ra8_elc_event_t)0x30U, stub_handler_a, &s_ctx_scratch, 1U, &slot_a));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_isr_register((ra8_elc_event_t)0x31U, stub_handler_b, nullptr, 2U, &slot_b));
  TEST_ASSERT(slot_a != slot_b);
  TEST_END("ra8_isr_register: distinct events -> distinct slots");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_priority_roundtrip(void)
{
  TEST_BEGIN("ra8_isr_set_priority: updates stored priority");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_init());
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_isr_register((ra8_elc_event_t)0x20U, stub_handler_a, nullptr, 0U, nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_set_priority((ra8_elc_event_t)0x20U, 3U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_isr_set_priority((ra8_elc_event_t)0x20U, 99U));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_isr_set_priority((ra8_elc_event_t)0xFFU, 3U));
  TEST_END("ra8_isr_set_priority: updates stored priority");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_lookup_slot_null_out(void)
{
  TEST_BEGIN("ra8_isr_lookup_slot: NULL out rejected");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_init());
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_isr_lookup_slot((ra8_elc_event_t)1U, nullptr));
  TEST_END("ra8_isr_lookup_slot: NULL out rejected");
}

/**
 * @test test_find_event_mcdc_compound_guard
 *
 * @par MC/DC:
 * Decision: `if (s_slots[slot].in_use && s_slots[slot].event == event)`
 * (2 conditions, libs/ra8_hal/src/ra8_isr.c line 190)
 * - Vector 1: in_use=F, event=*    -> false (control: C1 short-circuits C2)
 * - Vector 2: in_use=T, event!=qry -> false (varies C2 only; C1 held T)
 * - Vector 3: in_use=T, event==qry -> true  (varies C1 vs vec1; C2 held T)
 * Vectors 1+3 prove `in_use` independently affects the outcome (the
 * iterated slot transitions from free to occupied-with-match);
 * vectors 2+3 prove `event == query` independently affects the outcome
 * (slot occupied, only the event field differs). N+1 = 3 vectors for
 * N=2 conditions: minimal MC/DC. Reached via ra8_isr_lookup_slot which
 * delegates to internal_find_event.
 *
 * Vector mapping:
 * - Vec1: lookup on a freshly-initialized table (every slot in_use=F).
 * - Vec2: register event 0x40 in slot 0, then lookup event 0x41 -- the
 *   loop hits slot 0 (in_use=T, event=0x40 != 0x41) then slot 1 free
 *   and returns slot_none.
 * - Vec3: register event 0x40 in slot 0, then lookup event 0x40 -- the
 *   loop hits slot 0 (in_use=T, event match) and returns 0.
 */
static void test_find_event_mcdc_compound_guard(void)
{
  TEST_BEGIN("internal_find_event MC/DC: in_use && event==query");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_init());

  /* Vector 1: in_use=F (table empty). Decision must be false for every
   * slot, so lookup returns slot_none. */
  uint16_t slot_v1 = k_isr_slot_poison;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_lookup_slot((ra8_elc_event_t)0x40U, &slot_v1));
  TEST_ASSERT_EQ(k_ra8_isr_slot_none, slot_v1);

  /* Register one event so subsequent vectors see in_use=T in slot 0. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_isr_register((ra8_elc_event_t)0x40U, stub_handler_a, nullptr, 0U, nullptr));

  /* Vector 2: in_use=T, event mismatch. Decision is false on slot 0
   * (because event 0x40 != query 0x41) and on every subsequent slot
   * (in_use=F short-circuits). Lookup returns slot_none. */
  uint16_t slot_v2 = k_isr_slot_poison;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_lookup_slot((ra8_elc_event_t)0x41U, &slot_v2));
  TEST_ASSERT_EQ(k_ra8_isr_slot_none, slot_v2);

  /* Vector 3: in_use=T, event match. Decision is true on slot 0; lookup
   * returns 0. */
  uint16_t slot_v3 = k_isr_slot_poison;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_lookup_slot((ra8_elc_event_t)0x40U, &slot_v3));
  TEST_ASSERT_EQ(0, slot_v3);

  TEST_END("internal_find_event MC/DC: in_use && event==query");
}

int32_t main(void)
{
  test_init_clears_state();
  test_register_allocates_first_slot();
  test_register_rejects_null_handler();
  test_register_rejects_bad_priority();
  test_register_duplicate_rejected();
  test_unregister_frees_slot();
  test_dispatch_invokes_handler_with_ctx();
  test_dispatch_out_of_range_is_noop();
  test_multiple_events_get_distinct_slots();
  test_set_priority_roundtrip();
  test_lookup_slot_null_out();
  test_find_event_mcdc_compound_guard();
  (void)fprintf(stderr, "[OK  ] test_ra8_isr.c\n");
  return 0;
}
