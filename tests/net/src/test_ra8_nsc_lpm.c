/**
 * @file test_ra8_nsc_lpm.c
 * @brief Unit tests for the Secure LPM ULPT self-wake NSC veneers (ra8_nsc_lpm.c).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_elc_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_isr.h"
#include "ra8_lpm.h"
#include "ra8_lpm_regs.h"
#include "ra8_mstp.h"
#include "ra8_nsc.h"
#include "ra8_ulpt_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_nsc_lpm_const_t
 * @brief Test fixture durations and sentinel values.
 */
typedef enum : uint32_t {
  k_t_nsc_lpm_standby_ms     = 100U,        /**< Nominal 100 ms standby duration.         */
  k_t_nsc_lpm_overflow_ms    = 131072000U,  /**< Smallest duration overflowing u32 ticks. */
  k_t_nsc_lpm_expected_100ms = 3277U,       /**< ceil(100 * 32768 / 1000).                */
  k_t_nsc_lpm_loco_stop_bit  = 0x01U,       /**< LOCOCR.LCSTP bit mask.                   */
  k_t_nsc_lpm_sentinel_count = 0xCAFEBABEU, /**< Sentinel value before querying count.    */
} t_nsc_lpm_const_t;

RA8_INTERNAL
static void internal_prep(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_lpm_idle_disarm());
}

/**
 * @par MC/DC:
 * (exercises arg-validation boundaries before init: period_ms == 0 and overflow)
 */
RA8_INTERNAL
static void internal_test_nsc_lpm_rejects_bad_durations(void)
{
  TEST_BEGIN("ra8_nsc_lpm_standby_ms: rejects zero and overflow duration");
  internal_prep();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_nsc_lpm_standby_ms(0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_nsc_lpm_standby_ms((uint32_t)k_t_nsc_lpm_overflow_ms));
  /* Rejected durations must not have armed WUPEN1.ULP0U or registered an ISR. */
  TEST_ASSERT_EQ(0U, (*ra8_lpm_icu_reg32(k_ra8_lpm_wupen1_off)));

  uint16_t slot = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_lookup_slot(k_ra8_elc_event_ulpt0_ulpti, &slot));
  TEST_ASSERT_EQ(k_ra8_isr_slot_none, slot);
  TEST_END("ra8_nsc_lpm_standby_ms: rejects zero and overflow duration");
}

/**
 * @par MC/DC:
 * Decision in ra8_nsc_lpm_idle_prepare: ``if (s_nsc_lpm_ready)``
 * - Vector 1: s_nsc_lpm_ready == false -> prepares LPM, ULPT0, LOCO, ISR, WUPEN1
 * - Vector 2: s_nsc_lpm_ready == true  -> idempotent fast return k_ra8_ok
 */
RA8_INTERNAL
static void internal_test_nsc_lpm_prepare_idempotent_and_standby(void)
{
  TEST_BEGIN("ra8_nsc_lpm: prepare idempotent, standby arms/clears, wake count increments");
  internal_prep();

  uint32_t base_wakes = (uint32_t)k_t_nsc_lpm_sentinel_count;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_lpm_get_wake_count(&base_wakes));
  TEST_ASSERT(base_wakes != (uint32_t)k_t_nsc_lpm_sentinel_count);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_lpm_idle_prepare());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_lpm_idle_prepare());

  TEST_ASSERT((*ra8_lpm_icu_reg32(k_ra8_lpm_wupen1_off) & (uint32_t)k_ra8_lpm_wupen1_ulpt0u) != 0U);
  TEST_ASSERT_EQ(0U,
                 (*ra8_lpm_sysc_reg8(k_ra8_lpm_lococr_off) & (uint8_t)k_t_nsc_lpm_loco_stop_bit));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_lpm_standby_ms((uint32_t)k_t_nsc_lpm_standby_ms));

  volatile r_ulpt_regs_t* reg = ra8_ulpt(0U);
  TEST_ASSERT_EQ((uint32_t)k_t_nsc_lpm_expected_100ms, reg->ULPTCNT);
  TEST_ASSERT_EQ(0U, reg->ULPTCR);
  TEST_ASSERT_EQ(0U, (*ra8_lpm_sysc_reg16(k_ra8_lpm_prcr_off) & (uint16_t)k_ra8_lpm_prcr_prc1_msk));

  uint32_t after_wakes = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_lpm_get_wake_count(&after_wakes));
  TEST_ASSERT_EQ(base_wakes + 1U, after_wakes);
  TEST_END("ra8_nsc_lpm: prepare idempotent, standby arms/clears, wake count increments");
}

/**
 * @par MC/DC:
 * Exercises lazy prepare inside ra8_nsc_lpm_standby_ms and fault injection via
 * ra8_fake_mmio_fail_wait leaving PRCR locked and ULPT0 stopped without incrementing
 * wake count.
 */
RA8_INTERNAL
static void internal_test_nsc_lpm_lazy_prepare_and_timeout(void)
{
  TEST_BEGIN("ra8_nsc_lpm: lazy prepare + TCSTF timeout preserves wake count and relocks PRCR");
  internal_prep();

  uint32_t before = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_lpm_get_wake_count(&before));

  volatile r_ulpt_regs_t* reg = ra8_ulpt(0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait(&reg->ULPTCR));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_nsc_lpm_standby_ms((uint32_t)k_t_nsc_lpm_standby_ms));
  TEST_ASSERT_EQ(0U, (*ra8_lpm_sysc_reg16(k_ra8_lpm_prcr_off) & (uint16_t)k_ra8_lpm_prcr_prc1_msk));

  uint32_t after = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_lpm_get_wake_count(&after));
  TEST_ASSERT_EQ(before, after);

  ra8_fake_mmio_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_nsc_lpm_idle_disarm());
  TEST_ASSERT_EQ(0U,
                 (*ra8_lpm_icu_reg32(k_ra8_lpm_wupen1_off) & (uint32_t)k_ra8_lpm_wupen1_ulpt0u));
  TEST_END("ra8_nsc_lpm: lazy prepare + TCSTF timeout preserves wake count and relocks PRCR");
}

/**
 * @par MC/DC:
 * Exercises RA8_CHECK_NULL_PTR on ra8_nsc_lpm_get_wake_count(nullptr).
 */
RA8_INTERNAL
static void internal_test_nsc_lpm_get_wake_count_null(void)
{
  TEST_BEGIN("ra8_nsc_lpm_get_wake_count: rejects nullptr");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_nsc_lpm_get_wake_count(nullptr));
  TEST_END("ra8_nsc_lpm_get_wake_count: rejects nullptr");
}

int main(void)
{
  internal_test_nsc_lpm_rejects_bad_durations();
  internal_test_nsc_lpm_prepare_idempotent_and_standby();
  internal_test_nsc_lpm_lazy_prepare_and_timeout();
  internal_test_nsc_lpm_get_wake_count_null();
  return 0;
}
