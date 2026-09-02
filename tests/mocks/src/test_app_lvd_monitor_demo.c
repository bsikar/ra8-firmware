/**
 * @file test_app_lvd_monitor_demo.c
 * @brief Integration test: LVD / PVD VCC-monitor bring-up (pure logic)
 *
 * @details
 * Mirrors examples/ek_ra8d2/hil_needs_revalidation/lvd_monitor_demo/src/main.c. The demo's
 * hardware-touching paths (PRCR unlock, ra8_lvd_channel_init,
 * ra8_lvd_get_status) are owned + covered by ra8_lvd's own unit tests; this
 * test pins down the app-level pure logic:
 *
 *  - The "healthy rail" verdict ``ok = above && !crossed`` (MC/DC).
 *  - Decoding PVD1SR.MON / PVD1SR.DET out of the status byte.
 *  - The PRCR.PRC3 unlock word the demo writes (0xA508 = key | PRC3).
 *
 * No ra8_fake_mmap MMIO is required, so this test runs in-process.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_lvd_regs.h"
#include "ra8_system_regs.h"
#include "unity_minimal.h"

/** @brief Expected PRCR unlock word + a sample PVD1SR snapshot. */
typedef enum : uint16_t {
  k_t_lvd_prcr_unlock = 0xA508U, /**< key 0xA500 | PRC3 (0x0008). */
} t_lvd_word_t;

/**
 * @brief App copy of the demo's "healthy rail" verdict.
 *
 * @details Mirror of ``lvd_demo_sample``: VCC above threshold and no
 * latched crossing.
 */
static uint8_t compute_ok(bool above, bool crossed)
{
  return (above && !crossed) ? 1U : 0U;
}

/** @brief App copy of the demo's PVD1SR.MON decode. */
static bool decode_above(uint8_t sr)
{
  return (sr & (uint8_t)k_ra8_lvd_sr_mask_mon) != 0U;
}

/** @brief App copy of the demo's PVD1SR.DET decode. */
static bool decode_crossed(uint8_t sr)
{
  return (sr & (uint8_t)k_ra8_lvd_sr_mask_det) != 0U;
}

/**
 * @brief ``ok = above && !crossed`` exercised for full MC/DC.
 *
 * @par MC/DC:
 * Decision ``above && !crossed`` (2 conditions). N+1 = 3 vectors:
 * - above=1, crossed=0 -> 1   (control: both favorable)
 * - above=0, crossed=0 -> 0   (varies ``above`` only)
 * - above=1, crossed=1 -> 0   (varies ``crossed`` only)
 * 1+2 prove ``above`` independently drives the outcome; 1+3 prove the
 * same for ``crossed``.
 */
static void test_lvd_app_ok_mcdc(void)
{
  TEST_BEGIN("lvd_monitor_demo: ok = above && !crossed (MC/DC)");
  TEST_ASSERT_EQ(1U, compute_ok(true, false));  /* control          */
  TEST_ASSERT_EQ(0U, compute_ok(false, false)); /* vary above       */
  TEST_ASSERT_EQ(0U, compute_ok(true, true));   /* vary crossed     */
  TEST_ASSERT_EQ(0U, compute_ok(false, true));  /* both unfavorable */
  TEST_END("lvd_monitor_demo: ok = above && !crossed (MC/DC)");
}

/**
 * @brief PVD1SR.MON / PVD1SR.DET decode both bits independently.
 *
 * @par MC/DC:
 * Two single-condition masks; one vector each for set + clear (4 total).
 */
static void test_lvd_app_status_decode(void)
{
  TEST_BEGIN("lvd_monitor_demo: decode MON/DET from PVD1SR");
  const uint8_t sr_above = (uint8_t)k_ra8_lvd_sr_mask_mon;
  const uint8_t sr_cross = (uint8_t)k_ra8_lvd_sr_mask_det;

  TEST_ASSERT(decode_above(sr_above));
  TEST_ASSERT(!decode_above(0U));
  TEST_ASSERT(decode_crossed(sr_cross));
  TEST_ASSERT(!decode_crossed(0U));

  /* A real "healthy" snapshot: MON=1, DET=0 -> ok. */
  TEST_ASSERT_EQ(1U, compute_ok(decode_above(sr_above), decode_crossed(sr_above)));
  /* A brown-out snapshot: MON=0, DET=1 -> not ok. */
  const uint8_t sr_brownout = (uint8_t)k_ra8_lvd_sr_mask_det;
  TEST_ASSERT_EQ(0U, compute_ok(decode_above(sr_brownout), decode_crossed(sr_brownout)));
  TEST_END("lvd_monitor_demo: decode MON/DET from PVD1SR");
}

/**
 * @brief The demo's PRCR unlock word carries the 0xA5 key + PRC3 bit.
 *
 * @par MC/DC:
 * Straight-line bit algebra; no compound decision. One vector confirms the
 * composed value matches the PVD register-protection requirement
 * (HUM Ch 8.2.4 p 305 "Set the PRCR.PRC3 bit to 1").
 */
static void test_lvd_app_prcr_unlock_word(void)
{
  TEST_BEGIN("lvd_monitor_demo: PRCR unlock = key | PRC3 (0xA508)");
  const uint16_t composed = (uint16_t)(k_ra8_prcr_key | k_ra8_prcr_grp3_pvd);
  TEST_ASSERT_EQ(k_t_lvd_prcr_unlock, composed);
  /* Key byte present and PRC3 (bit 3) set. */
  TEST_ASSERT_EQ(k_ra8_prcr_key, (composed & 0xFF00U));
  TEST_ASSERT((composed & (uint16_t)k_ra8_prcr_grp3_pvd) != 0U);
  /* Relock word drops every group bit but keeps the key. */
  TEST_ASSERT_EQ(k_ra8_prcr_key, k_ra8_prcr_lock_all);
  TEST_END("lvd_monitor_demo: PRCR unlock = key | PRC3 (0xA508)");
}

int main(void)
{
  test_lvd_app_ok_mcdc();
  test_lvd_app_status_decode();
  test_lvd_app_prcr_unlock_word();
  return 0;
}
