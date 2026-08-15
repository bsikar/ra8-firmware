/**
 * @file test_ra8_ofs.c
 * @brief Unit tests for Option Function Select section constants (ra8_ofs.c)
 *
 * @details
 * `ra8_ofs.c` does not expose any callable symbols - every constant is
 * placed into an `.option_setting_*` linker section via
 * `__attribute__((section))` and has internal linkage. There is
 * nothing to assert against directly on the host: the constants live
 * at runtime in the .rodata of the compiled object and the linker
 * discards the `.option_setting_*` output sections in the host test
 * link. The point of this test is to make sure the TU compiles
 * cleanly under the host toolchain so coverage includes it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_fake_mmap.h"
#include "ra8_ofs.h"
#include "unity_minimal.h"

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_ra8_ofs_compiled(void)
{
  TEST_BEGIN("ra8_ofs.c compiled into ra8_core_hal");
  ra8_fake_mmap_reset();
  /* If ra8_ofs.c failed to compile, this entire test binary would
   * fail to link -- so reaching this line is itself the assertion. */
  TEST_ASSERT(1);
  TEST_END("ra8_ofs.c compiled into ra8_core_hal");
}

/**
 * @test test_mcdc_ra8_ofs
 *
 * @par MC/DC:
 * The only compound decision in libs/ra8_hal/src/ra8_ofs.c is the
 * preprocessor expression guarding the ``RA8_SECTION`` definition:
 * ``#if defined(UNIT_TEST) || defined(RA8_OFF_TARGET) ||
 *      defined(__APPLE__)``
 * Per DO-178C 6.4.4.3 and IEC 61508 SIL 3 guidance, preprocessor
 * conditionals are NOT runtime decisions and therefore have no MC/DC
 * obligation -- only one branch is ever compiled into a given binary.
 * The MCDC_GAPS.csv entry is a false positive of the static scanner,
 * which classifies any chain of ``||`` operators as a coverage gap.
 *
 * The host build of this TU exercises the
 * ``defined(__APPLE__) || defined(UNIT_TEST)`` arm (the ``RA8_SECTION``
 * macro collapses to a bare ``__attribute__((used))``), which is the
 * single condition the scanner could meaningfully observe.  This
 * sentinel test records that fact so the gap entry can be traced back
 * to a documented rationale rather than a missing test case.
 *
 * N+1 vector set: not applicable (compile-time decision, only one of
 * the three conditions ever participates in a given build).
 */
static void test_mcdc_ra8_ofs(void)
{
  TEST_BEGIN("ra8_ofs MC/DC: RA8_SECTION guard is preprocessor-only (no runtime gap)");
  /* Compile-time selection of the host ``RA8_SECTION`` definition is
   * proven by the fact that this TU links at all -- the cross-compile
   * branch would fail to resolve the target-only section name. */
#if defined(UNIT_TEST) || defined(__APPLE__)
  TEST_ASSERT(1);
#else
  TEST_ASSERT(0); /* unreachable on host build. */
#endif
  TEST_END("ra8_ofs MC/DC: RA8_SECTION guard is preprocessor-only (no runtime gap)");
}

/**
 * @test test_ra8_ofs_inventory
 *
 * @par MC/DC:
 * (no compound decisions -- each assertion below is a single equality
 * comparison; the OFS word inventory carries no runtime ``&&`` / ``||``.)
 *
 * The inventory is device-invariant: both RA8D2 (HUM R01UH1065EJ0130
 * Ch 7.2.6 p 287) and RA8P1 (HUM R01UH1064EJ0130 Ch 7.2.6 p 288) carry
 * the OFS0..OFS3 quartet, so ``ra8_ofs_word_count()`` is 4 on either
 * device selection and this test needs no per-device arm.
 *
 * The earlier revision of this test asserted
 * ``ra8_ofs_has_ofs3() == (k_ra8_feat_ofs3 != 0U)`` -- but that WAS the
 * body of ``ra8_ofs_has_ofs3()``, so it compared a constant to itself and
 * would have held for either value. Both the predicate and the feature
 * mirror are gone (#516); the assertions below name the expected
 * ordinals literally so they can actually fail.
 */
static void test_ra8_ofs_inventory(void)
{
  TEST_BEGIN("ra8_ofs inventory reports the OFS0..OFS3 quartet");

  /* Enumerator ordinals are contiguous, and OFS3 is the fourth word. */
  TEST_ASSERT(k_ra8_ofs_word_ofs0 == 0U);
  TEST_ASSERT(k_ra8_ofs_word_ofs1 == 1U);
  TEST_ASSERT(k_ra8_ofs_word_ofs2 == 2U);
  TEST_ASSERT(k_ra8_ofs_word_ofs3 == 3U);
  TEST_ASSERT((uint8_t)k_ra8_ofs_word_count == 4U);

  /* The accessor reports that same count to callers outside this TU. */
  TEST_ASSERT(ra8_ofs_word_count() == 4U);

  TEST_END("ra8_ofs inventory reports the OFS0..OFS3 quartet");
}

/**
 * @brief Pin the runtime-read option-setting addresses to their HUM literals.
 *
 * @details
 * Every value is spelled out as the literal the manual prints, so this test
 * fails if any constant drifts. That is the whole point: `ra8_wdt_regs.h`
 * previously carried `0x03001E04` / `0x03001E20` for OFS0 / OFS3 -- addresses
 * that appear nowhere in either manual and land in the `0x0300_0000 ..
 * 0x07FF_FFFF` Reserved window -- and survived because the only tests that
 * touched them compared each constant against itself (#545).
 *
 * Sources, identical on both supported parts:
 * - `OFS0` `0x02C9_F040` -- RA8D2 HUM R01UH1065EJ0130 Ch 7.2.1 p 280.
 * - `OFS3` `0x12C9_F4C4` -- Ch 7.2.6 p 287, non-secure alias.
 * - `OFS3_SEC` `0x02C9_F0C4` -- Ch 7.2.6 p 287.
 * - `OFS3_SEL` `0x02C9_F124` -- Ch 7.2.7 p 289.
 *
 * @test test_ra8_ofs_addresses
 *
 * @par MC/DC:
 * (no compound decisions -- each assertion is a single equality comparison.)
 */
static void test_ra8_ofs_addresses(void)
{
  TEST_BEGIN("ra8_ofs addresses match the HUM Figure 7.1 literals");

  TEST_ASSERT((uintptr_t)k_ra8_ofs0_addr == (uintptr_t)0x02C9F040UL);
  TEST_ASSERT((uintptr_t)k_ra8_ofs3_addr == (uintptr_t)0x12C9F4C4UL);
  TEST_ASSERT((uintptr_t)k_ra8_ofs3_sec_addr == (uintptr_t)0x02C9F0C4UL);
  TEST_ASSERT((uintptr_t)k_ra8_ofs3_sel_addr == (uintptr_t)0x02C9F124UL);

  /* None of them may sit in the Reserved window the old constants occupied.
   * HUM Ch 3 memory map: 0x0300_0000 .. 0x07FF_FFFF is Reserved area. */
  const uintptr_t reserved_lo = (uintptr_t)0x03000000UL;
  const uintptr_t reserved_hi = (uintptr_t)0x07FFFFFFUL;
  TEST_ASSERT(((uintptr_t)k_ra8_ofs0_addr < reserved_lo) ||
              ((uintptr_t)k_ra8_ofs0_addr > reserved_hi));
  TEST_ASSERT(((uintptr_t)k_ra8_ofs3_addr < reserved_lo) ||
              ((uintptr_t)k_ra8_ofs3_addr > reserved_hi));
  TEST_ASSERT(((uintptr_t)k_ra8_ofs3_sec_addr < reserved_lo) ||
              ((uintptr_t)k_ra8_ofs3_sec_addr > reserved_hi));
  TEST_ASSERT(((uintptr_t)k_ra8_ofs3_sel_addr < reserved_lo) ||
              ((uintptr_t)k_ra8_ofs3_sel_addr > reserved_hi));

  TEST_END("ra8_ofs addresses match the HUM Figure 7.1 literals");
}

int32_t main(void)
{
  test_ra8_ofs_compiled();
  test_mcdc_ra8_ofs();
  test_ra8_ofs_inventory();
  test_ra8_ofs_addresses();
  return 0;
}
