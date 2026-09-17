/**
 * @file test_ra8_rsip_devsec_failclosed.c
 * @brief Production fail-closed test for the RSIP device-security path (issue
 * #216)
 *
 * @par Tag
 * [Ring 1 / Core] {World: S}
 *
 * @details
 * ``ra8_rsip_devsec.c`` (device lifecycle / debug authorisation / tamper /
 * SPA-DPA arm) used to drive an invented "RSIP security-state" register block
 * cited to HUM Ch 51 "Security Features" -- a prose feature index with no
 * register map. Issue #216 fail-closes that fiction the same way #214 / #215
 * did: the insecure off-target command path stays behind
 * ``#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)`` and the
 * production ``#else`` returns ``k_ra8_err_not_supported`` for every entry
 * point, writing no fabricated state.
 *
 * The rest of the host build compiles ``ra8_rsip_devsec.c`` under
 * ``RA8_OFF_TARGET`` (the guarded ``#if`` branch -- exercised by ``test_life``
 * / ``test_debug_level`` / ``test_tamper`` in ``test_ra8_rsip.c``). This target
 * is registered by hand in ``tests/CMakeLists.txt`` and rebuilds JUST that TU
 * with the two guard flags UNDEFINED (``-URA8_OFF_TARGET
 * -URA8_INSECURE_STUB_CRYPTO``), so the fail-closed production ``#else`` is the
 * compiled body. It then proves the contract that matters most for a
 * security-state query: it must refuse rather than lie. Every entry point must
 * return ``k_ra8_err_not_supported`` and every output sentinel must be left
 * untouched -- if the fake ``#if`` branch were compiled instead, the getters
 * would return ``k_ra8_ok`` and overwrite the sentinels, and this test would
 * fail. This is the host-side companion to
 * ``scripts/checks/check_stub_crypto_guarded.py`` (which enforces the guard at
 * gate time) and the ARM cross-build (which compiles the ``#else``).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_rsip.h"
#include "unity_minimal.h"

/** @brief Distinct sentinels a fail-closed getter must never overwrite. */
typedef enum : uint32_t {
  k_devsec_life_sentinel   = 0xA5A5A5A5UL, /**< Lifecycle out-word sentinel.     */
  k_devsec_dbg_sentinel    = 0x5A5A5A5AUL, /**< Debug-level out-word sentinel.   */
  k_devsec_tamper_sentinel = 0xDEADBEEFUL, /**< Tamper-status out-word sentinel. */
} devsec_sentinel_t;

/**
 * @brief Lifecycle read / advance fail closed and touch no state.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the production `#else` under test is a
 * straight-line hard return; no `&&` or `||` in the code it touches)
 */
static void test_life_fail_closed(void)
{
  TEST_BEGIN("devsec: lifecycle fails closed (no fabricated state)");

  ra8_rsip_life_state_t st = (ra8_rsip_life_state_t)k_devsec_life_sentinel;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_rsip_life_get(&st));
  /* The out-word must be untouched -- no fabricated lifecycle is written. */
  TEST_ASSERT_EQ(k_devsec_life_sentinel, st);

  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_rsip_life_advance(k_ra8_rsip_life_dpl));

  TEST_END("devsec: lifecycle fails closed (no fabricated state)");
}

/**
 * @brief Debug-level read / set fail closed and touch no state.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the production `#else` under test is a
 * straight-line hard return; no `&&` or `||` in the code it touches)
 */
static void test_debug_level_fail_closed(void)
{
  TEST_BEGIN("devsec: debug level fails closed (no fabricated state)");

  ra8_rsip_debug_level_t lvl = (ra8_rsip_debug_level_t)k_ra8_rsip_debug_al2;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_rsip_debug_level_get(&lvl));
  /* Getter must not fabricate a "no debug" AL0 (or any) authorisation level. */
  TEST_ASSERT_EQ(k_ra8_rsip_debug_al2, lvl);

  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_rsip_debug_level_set(k_ra8_rsip_debug_al1));

  TEST_END("devsec: debug level fails closed (no fabricated state)");
}

/**
 * @brief Tamper enable / status / ack + DPA arm fail closed.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the production `#else` under test is a
 * straight-line hard return; no `&&` or `||` in the code it touches)
 */
static void test_tamper_dpa_fail_closed(void)
{
  TEST_BEGIN("devsec: tamper + DPA fail closed (no fabricated state)");

  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_rsip_tamper_enable((uint32_t)k_ra8_rsip_tamper_src_ext0));

  uint32_t flags = (uint32_t)k_devsec_tamper_sentinel;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_rsip_tamper_status(&flags));
  /* Status must not fabricate an all-clear (or any) tamper reading. */
  TEST_ASSERT_EQ(k_devsec_tamper_sentinel, flags);

  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_rsip_tamper_ack((uint32_t)k_ra8_rsip_tamper_src_ext0));

  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_rsip_dpa_arm(true));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_rsip_dpa_arm(false));

  TEST_END("devsec: tamper + DPA fail closed (no fabricated state)");
}

int main(void)
{
  test_life_fail_closed();
  test_debug_level_fail_closed();
  test_tamper_dpa_fail_closed();
  (void)internal_test_output_fd_text(STDERR_FILENO,
                                     "[OK ] test_ra8_rsip_devsec_failclosed.c -- "
                                     "RSIP device-security fails closed\n");
  return 0;
}
