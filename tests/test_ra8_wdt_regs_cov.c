/**
 * @file test_ra8_wdt_regs_cov.c
 * @brief Coverage tests for the inline helpers in ra8_wdt_regs.h
 *
 * @details
 * The existing test_ra8_wdt.c and test_ra8_wdt_extended.c suites exercise
 * ra8_wdt.c (the high-level driver) but leave the non-default arms of the
 * header's switch-dispatch inline ra8_wdt_for() uncovered -- both the
 * k_ra8_wdt1 arm and the k_ra8_wdt_instance_count/default arm.
 *
 * This file calls that inline directly with the missing selector values,
 * asserts the documented return values, and also exercises
 * ra8_wdt_refresh_instance() end-to-end against WDT1.
 *
 * All addresses lie in the peri bus window (0x40000000, 8 MiB) that
 * ra8_fake_mmap installs at start-up, so every pointer dereference is safe
 * on the host.
 *
 * The option-setting addresses these instances latch from are asserted
 * against their HUM literals in test_ra8_ofs.c, not here: they are no longer
 * reachable through a WDT-header accessor (#545).
 *
 * @par MC/DC:
 * ra8_wdt_for() contains no compound boolean decision (it is a plain
 * switch/case dispatcher). No MC/DC vectors are required beyond exhausting
 * each case arm, which the tests below do.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_fake_mmap.h"
#include "ra8_wdt_regs.h"
#include "unity_minimal.h"

/* ---------------------------------------------------------------------------
 * ra8_wdt_for -- WDT1 arm (line 321)
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify ra8_wdt_for() returns the WDT1 register block for k_ra8_wdt1.
 *
 * @details
 * Calls ra8_wdt_for(k_ra8_wdt1) and confirms the returned pointer equals the
 * WDT1 base address (0x40202700). That address sits inside the 8 MiB
 * peripheral window mapped by ra8_fake_mmap, so the cast-and-compare is safe.
 *
 * @pre ra8_fake_mmap constructor has run (guaranteed before main()).
 * @post ra8_wdt_for returns exactly k_ra8_wdt1_base_addr for k_ra8_wdt1.
 *
 * @par MC/DC:
 * (switch arm -- no compound boolean decision)
 *
 * @since 0.1.0
 */
static void test_wdt_for_wdt1_returns_wdt1_base(void)
{
  TEST_BEGIN("ra8_wdt_for(k_ra8_wdt1) returns WDT1 base address");
  ra8_fake_mmap_reset();
  volatile r_wdt_regs_t* p = ra8_wdt_for(k_ra8_wdt1);
  TEST_ASSERT_EQ((uintptr_t)k_ra8_wdt1_base_addr, (uintptr_t)p);
  TEST_END("ra8_wdt_for(k_ra8_wdt1) returns WDT1 base address");
}

/* ---------------------------------------------------------------------------
 * ra8_wdt_for -- k_ra8_wdt_instance_count/default arm (lines 322, 324)
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify ra8_wdt_for() falls back to WDT0 for an out-of-range selector.
 *
 * @details
 * k_ra8_wdt_instance_count is the sentinel value (2) that falls through to
 * the default arm of ra8_wdt_for()'s switch. The documented defensive
 * fallback is WDT0 (0x40202600).
 *
 * @pre ra8_fake_mmap constructor has run.
 * @post ra8_wdt_for returns k_ra8_wdt0_base_addr for k_ra8_wdt_instance_count.
 *
 * @par MC/DC:
 * (switch arm -- no compound boolean decision)
 *
 * @since 0.1.0
 */
static void test_wdt_for_oob_falls_back_to_wdt0(void)
{
  TEST_BEGIN("ra8_wdt_for(k_ra8_wdt_instance_count) falls back to WDT0 base");
  ra8_fake_mmap_reset();
  volatile r_wdt_regs_t* p = ra8_wdt_for(k_ra8_wdt_instance_count);
  TEST_ASSERT_EQ((uintptr_t)k_ra8_wdt0_base_addr, (uintptr_t)p);
  TEST_END("ra8_wdt_for(k_ra8_wdt_instance_count) falls back to WDT0 base");
}

/* ---------------------------------------------------------------------------
 * ra8_wdt_refresh_instance -- end-to-end for WDT1
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify ra8_wdt_refresh_instance() writes the unlock sequence to WDT1.
 *
 * @details
 * ra8_wdt_refresh_instance(k_ra8_wdt1) internally calls ra8_wdt_for(k_ra8_wdt1)
 * (covering line 321 of the header) and then writes 0x00 then 0xFF to the
 * WDT1 WDTRR register. After the call the register must hold 0xFF
 * (k_ra8_wdt_refresh_b), the second byte of the unlock sequence.
 *
 * WDT1 is at 0x40202700 which is 0x202700 into the 8 MiB peri window
 * (0x40000000..0x40800000) mapped by ra8_fake_mmap -- the write is safe.
 *
 * @pre ra8_fake_mmap_reset() zeros the peri window.
 * @post WDT1 WDTRR holds k_ra8_wdt_refresh_b (0xFF) after the call.
 *
 * @par MC/DC:
 * (no compound boolean decisions in the path under test)
 *
 * @since 0.1.0
 */
static void test_wdt_refresh_instance_wdt1_writes_sequence(void)
{
  TEST_BEGIN("ra8_wdt_refresh_instance(k_ra8_wdt1) writes 0xFF to WDT1 WDTRR");
  ra8_fake_mmap_reset();
  ra8_wdt_refresh_instance(k_ra8_wdt1);
  volatile r_wdt_regs_t* w1 = ra8_wdt_for(k_ra8_wdt1);
  TEST_ASSERT_EQ(k_ra8_wdt_refresh_b, w1->WDTRR);
  TEST_END("ra8_wdt_refresh_instance(k_ra8_wdt1) writes 0xFF to WDT1 WDTRR");
}

/**
 * @brief Verify ra8_wdt_refresh_instance() with the oob selector falls back to WDT0.
 *
 * @details
 * ra8_wdt_refresh_instance(k_ra8_wdt_instance_count) internally calls
 * ra8_wdt_for(k_ra8_wdt_instance_count) which falls through to the default arm
 * (lines 322, 324 of the header) and returns the WDT0 base. The refresh
 * sequence is then written to WDT0 WDTRR.
 *
 * @pre ra8_fake_mmap_reset() zeros the peri window.
 * @post WDT0 WDTRR holds k_ra8_wdt_refresh_b (0xFF) after the call.
 *
 * @par MC/DC:
 * (no compound boolean decisions in the path under test)
 *
 * @since 0.1.0
 */
static void test_wdt_refresh_instance_oob_writes_to_wdt0(void)
{
  TEST_BEGIN("ra8_wdt_refresh_instance(k_ra8_wdt_instance_count) falls back to WDT0");
  ra8_fake_mmap_reset();
  ra8_wdt_refresh_instance(k_ra8_wdt_instance_count);
  volatile r_wdt_regs_t* w0 = ra8_wdt_for(k_ra8_wdt0);
  TEST_ASSERT_EQ(k_ra8_wdt_refresh_b, w0->WDTRR);
  TEST_END("ra8_wdt_refresh_instance(k_ra8_wdt_instance_count) falls back to WDT0");
}

/* ---------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------------
 */

int main(void)
{
  test_wdt_for_wdt1_returns_wdt1_base();
  test_wdt_for_oob_falls_back_to_wdt0();
  test_wdt_refresh_instance_wdt1_writes_sequence();
  test_wdt_refresh_instance_oob_writes_to_wdt0();
  return 0;
}
