/**
 * @file test_app_cache_hal_enable_demo.c
 * @brief Integration test: the cache_hal_enable_demo example's boot + round-trip.
 *
 * @details
 * Exercises the app-level behaviour of
 * examples/ek_ra8d2/hw_pending/cache_hal_enable_demo, driving the REAL
 * ra8_cache HAL primitives the example depends on against the fake SCB MMIO map
 * (0xE000Exxx) -- so this is more than a compile check:
 *
 *  - The **boot cache-enable sequence** the example opts into via
 *    RA8_BOOT_CACHE_VIA_HAL: SystemInit() calls ra8_cache_icache_enable() then
 *    ra8_cache_dcache_enable(). This test runs that exact pair and asserts CCR.IC
 *    and CCR.DC end up set and that each ran its architectural invalidate first
 *    (ICIALLU for the I-cache, a set/way DCISW walk for the D-cache).
 *
 *  - The example's **cacheable-SRAM round-trip** self-test
 *    (cache_hal_test_cacheable_rw): fill a buffer with the affine pattern
 *    buf[i] = (uint8_t)(i*31 + 7), clean+invalidate the (now dirty) lines via
 *    ra8_cache_dcache_clean_invalidate_by_addr(), then read every byte back and
 *    compare. This test reproduces those exact steps and asserts the maintenance
 *    call succeeds and the readback matches -- the app's PASS condition.
 *
 * The register-level maintenance is owned + covered by ra8_cache's own unit tests
 * (test_ra8_cache.c); this file pins the example's app-level composition of them.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_cache.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "unity_minimal.h"

/**
 * @enum app_cache_hal_reg_t
 * @brief SCB cache-register addresses the test seeds and reads back.
 */
typedef enum : uintptr_t {
  k_a_ccr      = 0xE000ED14UL, /**< Configuration and Control (CCR). */
  k_a_ctr      = 0xE000ED7CUL, /**< Cache Type Register.             */
  k_a_ccsidr   = 0xE000ED80UL, /**< Cache Size ID Register.          */
  k_a_iciallu  = 0xE000EF50UL, /**< I-cache invalidate all to PoU.   */
  k_a_dcisw    = 0xE000EF60UL, /**< D-cache invalidate by set/way.   */
  k_a_dccimvac = 0xE000EF70UL, /**< D-cache clean+invalidate by MVA. */
} app_cache_hal_reg_t;

/**
 * @enum app_cache_hal_const_t
 * @brief Mirror of the demo's app-local constants + test fixtures.
 * @details The pattern coefficients and buffer size mirror
 *          cache_hal_enable_demo/main.c so a drift in the app's self-test is
 *          caught here.
 */
typedef enum : uint32_t {
  k_a_ctr_dmin_shift  = 16U,         /**< CTR.DminLine position.                */
  k_a_dmin_for_32     = 3U,          /**< DminLine=3 -> 4<<3 = 32-byte line.    */
  k_a_ccsidr_sets_sh  = 13U,         /**< CCSIDR.NumSets position.              */
  k_a_ccsidr_assoc_sh = 3U,          /**< CCSIDR.Associativity position.        */
  k_a_ccr_ic          = 0x00020000U, /**< CCR.IC (bit 17): I-cache enable.      */
  k_a_ccr_dc          = 0x00010000U, /**< CCR.DC (bit 16): D-cache enable.      */
  k_a_sentinel        = 0xDEADBEEFU, /**< Pre-set marker to detect "no write".  */
  k_a_pat_mul         = 31U,         /**< Demo affine pattern multiplier.       */
  k_a_pat_add         = 7U,          /**< Demo affine pattern addend.           */
  k_a_buf_bytes       = 256U,        /**< Round-trip buffer (>= several lines). */
} app_cache_hal_const_t;

/** @brief Typed access to a seeded/observed SCB register in the fake map. */
static volatile uint32_t* reg(app_cache_hal_reg_t addr)
{
  return (volatile uint32_t*)addr;
}

/** @brief Compose a CCSIDR with one set and one way (small, fast to walk). */
static uint32_t ccsidr_one_set_one_way(void)
{
  return (1U << (uint32_t)k_a_ccsidr_sets_sh) | (1U << (uint32_t)k_a_ccsidr_assoc_sh);
}

/**
 * @brief The demo's RA8_BOOT_CACHE_VIA_HAL boot sequence enables both L1 caches.
 *
 * @details Runs the exact ra8_cache_icache_enable() + ra8_cache_dcache_enable()
 *          pair SystemInit() performs for this example, and asserts the caches
 *          come up (CCR.IC | CCR.DC) with each invalidate having run first.
 *
 * @par MC/DC: not applicable -- straight-line composition of two enable calls;
 *      the only compound decision (the CCSIDR geometry guard) is owned + covered
 *      by test_ra8_cache.c's test_invalidate_all_geometry_guard.
 */
static void test_boot_enable_sequence_brings_up_both_caches(void)
{
  TEST_BEGIN("cache_hal_enable_demo: boot HAL sequence enables I+D cache");
  ra8_fake_mmap_reset();
  *reg(k_a_ccsidr)  = ccsidr_one_set_one_way(); /* valid geometry -> set/way runs */
  *reg(k_a_ccr)     = 0U;
  *reg(k_a_iciallu) = (uint32_t)k_a_sentinel;
  *reg(k_a_dcisw)   = (uint32_t)k_a_sentinel;

  /* Exactly what SystemInit() runs under RA8_BOOT_CACHE_VIA_HAL for this app. */
  ra8_cache_icache_enable();
  ra8_cache_dcache_enable();

  const uint32_t both = (uint32_t)k_a_ccr_ic | (uint32_t)k_a_ccr_dc;
  TEST_ASSERT_EQ(both, *reg(k_a_ccr) & both);             /* both caches enabled    */
  TEST_ASSERT_EQ(0U, *reg(k_a_iciallu));                  /* I-cache invalidate ran */
  TEST_ASSERT(*reg(k_a_dcisw) != (uint32_t)k_a_sentinel); /* set/way invalidate ran */
  TEST_END("cache_hal_enable_demo: boot HAL sequence enables I+D cache");
}

/**
 * @brief The demo's cacheable-SRAM round-trip returns its PASS condition.
 *
 * @details Reproduces cache_hal_test_cacheable_rw(): fill the buffer with the
 *          affine pattern, clean+invalidate through the HAL by-address primitive,
 *          then verify byte-for-byte. On the fake map the bytes persist (no cache
 *          modelled), so a correct app composition reads back cleanly -- the same
 *          trivial pass the emulator sees; the assertion pins the fill/maintain/
 *          verify pipeline and that the maintenance call succeeds.
 *
 * @par MC/DC: not applicable -- the round-trip is straight-line fill + verify;
 *      the maintenance call's internal guards are covered by test_ra8_cache.c.
 */
static void test_cacheable_round_trip_matches(void)
{
  TEST_BEGIN("cache_hal_enable_demo: cacheable-SRAM round-trip matches");
  ra8_fake_mmap_reset();
  *reg(k_a_ctr)      = (uint32_t)k_a_dmin_for_32 << (uint32_t)k_a_ctr_dmin_shift; /* 32-byte line */
  *reg(k_a_dccimvac) = (uint32_t)k_a_sentinel;

  uint8_t buf[k_a_buf_bytes];
  for (uint32_t i = 0U; i < (uint32_t)k_a_buf_bytes; i++) {
    buf[i] = (uint8_t)((i * (uint32_t)k_a_pat_mul) + (uint32_t)k_a_pat_add);
  }

  /* The exact maintenance call the demo's self-test makes. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_dcache_clean_invalidate_by_addr(buf, (uint32_t)sizeof(buf)));
  TEST_ASSERT(*reg(k_a_dccimvac) != (uint32_t)k_a_sentinel); /* maintenance actually ran */

  bool match = true;
  for (uint32_t i = 0U; i < (uint32_t)k_a_buf_bytes; i++) {
    const uint8_t expected = (uint8_t)((i * (uint32_t)k_a_pat_mul) + (uint32_t)k_a_pat_add);
    if (buf[i] != expected) {
      match = false;
    }
  }
  TEST_ASSERT(match); /* the demo's PASS condition */
  TEST_END("cache_hal_enable_demo: cacheable-SRAM round-trip matches");
}

int32_t main(void)
{
  test_boot_enable_sequence_brings_up_both_caches();
  test_cacheable_round_trip_matches();
  return 0;
}
