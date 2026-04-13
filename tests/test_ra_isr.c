/**
 * @file test_ra_isr.c
 * @brief Unit tests for the NVIC + ICU IELSR allocator (libs/ra_hal/src/ra_isr.c).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_elc_regs.h"
#include "ra8d2_icu_regs.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static int      s_call_count   = 0;
static int      s_last_ctx_val = 0;
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
    s_last_ctx_val = *(int*)ctx;
  }
}

static void stub_handler_b(void* ctx)
{
  (void)ctx;
  ++s_call_count;
}

static void test_init_clears_state(void)
{
  TEST_BEGIN("ra_isr_init clears slots");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_isr_init());

  uint16_t slot = 0U;
  /* Unknown event not found. */
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_isr_lookup_slot((ra_elc_event_t)42U, &slot));
  TEST_ASSERT_EQ((int)k_ra_isr_slot_none, (int)slot);
  TEST_END("ra_isr_init clears slots");
}

static void test_register_allocates_first_slot(void)
{
  TEST_BEGIN("ra_isr_register: first registration gets slot 0");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_isr_init());

  uint16_t slot = 0xFFFFU;
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_isr_register((ra_elc_event_t)0x50U, stub_handler_a, nullptr, 5U, &slot));
  TEST_ASSERT_EQ((int)0, (int)slot);

  uint16_t slot2 = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_isr_lookup_slot((ra_elc_event_t)0x50U, &slot2));
  TEST_ASSERT_EQ((int)0, (int)slot2);

  /* IELSR entry holds the event number. */
  volatile uint32_t* ielsr = ra_icu_ielsr(0U);
  TEST_ASSERT_NOT_NULL((void*)ielsr);
  TEST_ASSERT_EQ((int)0x50, (int)(*ielsr & (uint32_t)k_ra_ielsr_iels_mask));

  TEST_END("ra_isr_register: first registration gets slot 0");
}

static void test_register_rejects_null_handler(void)
{
  TEST_BEGIN("ra_isr_register: NULL handler rejected");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_isr_init());

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_isr_register((ra_elc_event_t)1U, nullptr, nullptr, 0U, nullptr));
  TEST_END("ra_isr_register: NULL handler rejected");
}

static void test_register_rejects_bad_priority(void)
{
  TEST_BEGIN("ra_isr_register: priority out of range");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_isr_init());
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_isr_register((ra_elc_event_t)1U, stub_handler_a, nullptr, 99U, nullptr));
  TEST_END("ra_isr_register: priority out of range");
}

static void test_register_duplicate_rejected(void)
{
  TEST_BEGIN("ra_isr_register: duplicate event rejected");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_isr_init());
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_isr_register((ra_elc_event_t)7U, stub_handler_a, nullptr, 0U, nullptr));
  TEST_ASSERT_EQ((int)k_ra_err_exists,
                 (int)ra_isr_register((ra_elc_event_t)7U, stub_handler_a, nullptr, 0U, nullptr));
  TEST_END("ra_isr_register: duplicate event rejected");
}

static void test_unregister_frees_slot(void)
{
  TEST_BEGIN("ra_isr_unregister frees the slot");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_isr_init());
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_isr_register((ra_elc_event_t)9U, stub_handler_a, nullptr, 0U, nullptr));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_isr_unregister((ra_elc_event_t)9U));
  TEST_ASSERT_EQ((int)k_ra_err_not_found, (int)ra_isr_unregister((ra_elc_event_t)9U));
  TEST_END("ra_isr_unregister frees the slot");
}

static void test_dispatch_invokes_handler_with_ctx(void)
{
  TEST_BEGIN("ra_isr_dispatch invokes the registered handler");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_isr_init());
  reset_counts();

  int      marker = 0xBEEF;
  uint16_t slot   = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_isr_register((ra_elc_event_t)11U, stub_handler_a, &marker, 0U, &slot));
  ra_isr_dispatch(slot);
  TEST_ASSERT_EQ((int)1, (int)s_call_count);
  TEST_ASSERT_EQ((int)0xBEEF, (int)s_last_ctx_val);
  TEST_END("ra_isr_dispatch invokes the registered handler");
}

static void test_dispatch_out_of_range_is_noop(void)
{
  TEST_BEGIN("ra_isr_dispatch out-of-range is a no-op");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_isr_init());
  reset_counts();

  ra_isr_dispatch(500U); /* Out of bounds; nothing happens. */
  TEST_ASSERT_EQ((int)0, (int)s_call_count);
  TEST_END("ra_isr_dispatch out-of-range is a no-op");
}

static void test_multiple_events_get_distinct_slots(void)
{
  TEST_BEGIN("ra_isr_register: distinct events -> distinct slots");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_isr_init());

  uint16_t slot_a = 0U;
  uint16_t slot_b = 0U;
  TEST_ASSERT_EQ(
    (int)k_ra_ok,
    (int)ra_isr_register((ra_elc_event_t)0x30U, stub_handler_a, &s_ctx_scratch, 1U, &slot_a));
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_isr_register((ra_elc_event_t)0x31U, stub_handler_b, nullptr, 2U, &slot_b));
  TEST_ASSERT(slot_a != slot_b);
  TEST_END("ra_isr_register: distinct events -> distinct slots");
}

static void test_set_priority_roundtrip(void)
{
  TEST_BEGIN("ra_isr_set_priority: updates stored priority");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_isr_init());
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_isr_register((ra_elc_event_t)0x20U, stub_handler_a, nullptr, 0U, nullptr));

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_isr_set_priority((ra_elc_event_t)0x20U, 3U));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_isr_set_priority((ra_elc_event_t)0x20U, 99U));
  TEST_ASSERT_EQ((int)k_ra_err_not_found, (int)ra_isr_set_priority((ra_elc_event_t)0xFFU, 3U));
  TEST_END("ra_isr_set_priority: updates stored priority");
}

static void test_lookup_slot_null_out(void)
{
  TEST_BEGIN("ra_isr_lookup_slot: NULL out rejected");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_isr_init());
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_isr_lookup_slot((ra_elc_event_t)1U, nullptr));
  TEST_END("ra_isr_lookup_slot: NULL out rejected");
}

int main(void)
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
  (void)fprintf(stderr, "[OK  ] test_ra_isr.c\n");
  return 0;
}
