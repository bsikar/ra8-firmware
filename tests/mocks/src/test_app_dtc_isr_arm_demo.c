
/**
 * @file test_app_dtc_isr_arm_demo.c
 * @brief Integration test: DTC arm/disarm demo on the ra8_isr_set_dtc primitive
 *
 * @details
 * Mirrors examples/ek_ra8d2/hw_pending/dtc_isr_arm_demo/src/main.c. The DTC
 * engine is not modelled on the host, so the test focuses on:
 *
 *  - The fill helper produces a non-trivial source and a fully-set
 *    destination for both the armed (zero) and disarmed (sentinel) passes.
 *  - The match / untouched helpers detect their pass/fail cases (MC/DC).
 *  - The Transfer Information mode word encodes block / 32-bit /
 *    increment-both as 0xA8080000 (HUM Figure 18.4).
 *  - The armed && disarmed compound verdict has full MC/DC coverage.
 *  - The ra8_isr_set_dtc() primitive the app calls really arms (DTCE = 1)
 *    and disarms (DTCE = 0) an allocated IELSR slot, preserving IELS.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_dtc_regs.h"
#include "ra8_elc_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_icu_regs.h"
#include "ra8_isr.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_t_arm_words      = 256U,    /**< T arm words.                                             */
  k_t_arm_byte_sh    = 8U,      /**< T arm byte sh.                                           */
  k_t_arm_slot_unset = 0xFFFFU, /**< Sentinel: ra8_isr_register() has not written a slot yet. */
} t_arm_const_t;

/* Mirror of the demo's TI mode-bit field values (HUM Ch 18.2.2 / 18.2.3). */
typedef enum : uint8_t {
  k_t_arm_md_block   = 0x2U, /**< T arm md block.   */
  k_t_arm_sz_word    = 0x2U, /**< T arm sz word.    */
  k_t_arm_sm_inc     = 0x2U, /**< T arm sm inc.     */
  k_t_arm_dm_inc     = 0x2U, /**< T arm dm inc.     */
  k_t_arm_mra_md_pos = 6U,   /**< T arm mra md pos. */
  k_t_arm_mra_sz_pos = 4U,   /**< T arm mra sz pos. */
  k_t_arm_mra_sm_pos = 2U,   /**< T arm mra sm pos. */
  k_t_arm_mrb_dm_pos = 2U,   /**< T arm mrb dm pos. */
  k_t_arm_mra_bpos   = 24U,  /**< T arm mra bpos.   */
  k_t_arm_mrb_bpos   = 16U,  /**< T arm mrb bpos.   */
} t_arm_mr_t;

typedef enum : uint32_t {
  k_t_arm_mr_expected = 0xA8080000UL, /**< block / 32-bit / inc-both.   */
  k_t_arm_sentinel    = 0xA5A5A5A5UL, /**< Disarmed-phase destination.  */
  k_t_arm_event       = 0x0CCUL,      /**< ELC software event 0 (IELS). */
} t_arm_word_t;

static uint32_t s_src[k_t_arm_words];
static uint32_t s_dst[k_t_arm_words];

static void reset_world(void)
{
  (void)memset(s_src, 0, sizeof(s_src));
  (void)memset(s_dst, 0, sizeof(s_dst));
}

/** @brief Copy of the demo's pattern-fill helper. */
static void fill_buffers(uint32_t dst_init)
{
  for (uint32_t i = 0U; i < (uint32_t)k_t_arm_words; ++i) {
    s_src[i] = i ^ (i >> (uint32_t)k_t_arm_byte_sh);
    s_dst[i] = dst_init;
  }
}

/** @brief Copy of the demo's armed-pass verify helper. */
static uint8_t all_match_src(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_t_arm_words; ++i) {
    if (s_dst[i] != s_src[i]) {
      return 0U;
    }
  }
  return 1U;
}

/** @brief Copy of the demo's disarmed-pass untouched helper. */
static uint8_t all_equal(uint32_t expect)
{
  for (uint32_t i = 0U; i < (uint32_t)k_t_arm_words; ++i) {
    if (s_dst[i] != expect) {
      return 0U;
    }
  }
  return 1U;
}

/** @brief Copy of the demo's compound verdict: armed_ok && disarmed_ok. */
static uint8_t verdict(uint8_t armed_ok, uint8_t disarmed_ok)
{
  return (armed_ok != 0U && disarmed_ok != 0U) ? 1U : 0U;
}

/** @brief Build the TI mode word exactly as the demo does. */
static uint32_t build_mr(void)
{
  const uint8_t mra = (uint8_t)(((uint8_t)k_t_arm_md_block << k_t_arm_mra_md_pos) |
                                ((uint8_t)k_t_arm_sz_word << k_t_arm_mra_sz_pos) |
                                ((uint8_t)k_t_arm_sm_inc << k_t_arm_mra_sm_pos));
  const uint8_t mrb = (uint8_t)((uint8_t)k_t_arm_dm_inc << k_t_arm_mrb_dm_pos);
  return ((uint32_t)mra << k_t_arm_mra_bpos) | ((uint32_t)mrb << k_t_arm_mrb_bpos);
}

/** @brief Stub ISR handler used only to occupy a registered slot. */
static void arm_stub_handler(void* ctx)
{
  (void)ctx;
}

/**
 * @brief Both fills produce a non-trivial source; dst takes the fill value.
 *
 * @par MC/DC:
 * No compound decision; the fill loop only has the bound check. Vectors
 * prove the armed fill zeroes dst and the disarmed fill sets the sentinel.
 */
static void test_arm_app_fill(void)
{
  reset_world();
  TEST_BEGIN("dtc_isr_arm_demo: fill zeroes / sentinels dst, src non-trivial");
  fill_buffers(0U);
  uint32_t accum_src = 0U;
  uint32_t accum_dst = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_t_arm_words; ++i) {
    accum_src |= s_src[i];
    accum_dst |= s_dst[i];
  }
  TEST_ASSERT(accum_src != 0U);
  TEST_ASSERT_EQ(0U, accum_dst);

  fill_buffers((uint32_t)k_t_arm_sentinel);
  TEST_ASSERT_EQ(1U, all_equal((uint32_t)k_t_arm_sentinel));
  TEST_END("dtc_isr_arm_demo: fill zeroes / sentinels dst, src non-trivial");
}

/**
 * @brief The armed-match and disarmed-untouched helpers cover both branches.
 *
 * @par MC/DC:
 * Two single-condition loop decisions. all_match_src: equal (copied) vs
 * first mismatch. all_equal: untouched vs first differing word.
 */
static void test_arm_app_check_helpers(void)
{
  reset_world();
  TEST_BEGIN("dtc_isr_arm_demo: match + untouched helpers cover both branches");

  /* Armed helper: match, then a single-word mismatch. */
  fill_buffers(0U);
  (void)memcpy(s_dst, s_src, sizeof(s_src));
  TEST_ASSERT_EQ(1U, all_match_src());
  s_dst[0] ^= 0x1U;
  TEST_ASSERT_EQ(0U, all_match_src());

  /* Disarmed helper: untouched, then a single sentinel word overwritten. */
  fill_buffers((uint32_t)k_t_arm_sentinel);
  TEST_ASSERT_EQ(1U, all_equal((uint32_t)k_t_arm_sentinel));
  s_dst[k_t_arm_words - 1U] = 0U;
  TEST_ASSERT_EQ(0U, all_equal((uint32_t)k_t_arm_sentinel));
  TEST_END("dtc_isr_arm_demo: match + untouched helpers cover both branches");
}

/**
 * @brief The TI mode word encodes block / 32-bit / increment-both.
 *
 * @par MC/DC:
 * Straight-line encoding; no compound decision. One vector confirms the
 * exact word (HUM Figure 18.4 p 799).
 */
static void test_arm_app_ti_encoding(void)
{
  reset_world();
  TEST_BEGIN("dtc_isr_arm_demo: TI mode word == 0xA8080000");
  r_dtc_xfer_info_t ti = {};
  ti.MR                = build_mr();
  ti.CRB               = 0x0001U;
  ti.CRA               = 0x0000U;
  TEST_ASSERT_EQ(k_t_arm_mr_expected, ti.MR);
  TEST_ASSERT_EQ(0x0001U, ti.CRB);
  TEST_ASSERT_EQ(0x0000U, ti.CRA);
  TEST_END("dtc_isr_arm_demo: TI mode word == 0xA8080000");
}

/**
 * @test test_arm_app_verdict_mcdc
 *
 * @par MC/DC:
 * Decision: ``armed_ok != 0 && disarmed_ok != 0`` (2 conditions).
 * - Vector 1: armed=1, disarmed=1 -> true  (control: both conditions true)
 * - Vector 2: armed=0, disarmed=1 -> false (varies armed_ok only)
 * - Vector 3: armed=1, disarmed=0 -> false (varies disarmed_ok only)
 * Vectors 1+2 prove ``armed_ok`` independently affects the outcome; 1+3
 * prove the same for ``disarmed_ok``. N+1 = 3 vectors for N=2 conditions:
 * minimal MC/DC. This is the demo's pass/fail verdict.
 */
static void test_arm_app_verdict_mcdc(void)
{
  TEST_BEGIN("dtc_isr_arm_demo MC/DC: armed_ok && disarmed_ok");
  TEST_ASSERT_EQ(1U, verdict(1U, 1U)); /* Vector 1 */
  TEST_ASSERT_EQ(0U, verdict(0U, 1U)); /* Vector 2 */
  TEST_ASSERT_EQ(0U, verdict(1U, 0U)); /* Vector 3 */
  TEST_END("dtc_isr_arm_demo MC/DC: armed_ok && disarmed_ok");
}

/**
 * @brief The app's ra8_isr_set_dtc calls really arm and disarm the slot.
 *
 * @par MC/DC:
 * No compound decision; exercises the integration the app depends on --
 * arm sets DTCE and disarm clears it on the slot ra8_isr_register handed
 * back, with IELS preserved throughout.
 */
static void test_arm_app_set_dtc_integration(void)
{
  TEST_BEGIN("dtc_isr_arm_demo: ra8_isr_set_dtc arms + disarms the slot");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_init());

  uint16_t slot = (uint16_t)k_t_arm_slot_unset;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_isr_register((ra8_elc_event_t)k_t_arm_event, arm_stub_handler, nullptr, 12U, &slot));

  volatile uint32_t* ielsr = ra8_icu_ielsr(slot);
  TEST_ASSERT_NOT_NULL((void*)ielsr);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_set_dtc(slot, true));
  TEST_ASSERT((*ielsr & (uint32_t)k_ra8_ielsr_dtce_mask) != 0U);
  TEST_ASSERT_EQ(k_t_arm_event, (*ielsr & (uint32_t)k_ra8_ielsr_iels_mask));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_isr_set_dtc(slot, false));
  TEST_ASSERT_EQ(0U, (*ielsr & (uint32_t)k_ra8_ielsr_dtce_mask));
  TEST_ASSERT_EQ(k_t_arm_event, (*ielsr & (uint32_t)k_ra8_ielsr_iels_mask));
  TEST_END("dtc_isr_arm_demo: ra8_isr_set_dtc arms + disarms the slot");
}

int main(void)
{
  test_arm_app_fill();
  test_arm_app_check_helpers();
  test_arm_app_ti_encoding();
  test_arm_app_verdict_mcdc();
  test_arm_app_set_dtc_integration();
  return 0;
}
