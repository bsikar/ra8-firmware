/**
 * @file test_ra8_ble_security_cov.c
 * @brief Coverage-completion tests for libs/ra8_ble_host/src/ra8_ble_security.c.
 *
 * @details
 * The baseline suite in test_ra8_ble_security.c exercises init / close /
 * bond-count / event dispatch, but never drives the three action APIs
 * ra8_ble_security_pair(), ra8_ble_security_passkey_reply() (happy path)
 * and ra8_ble_security_clear_bonds(). Under the host build the vendor
 * NimBLE tree is not linked (RA8_TARGET_BUILD is undefined), so those
 * functions reduce to their pure-software #else legs: an
 * initialized-guard plus an unconditional k_ra8_ok return. Every one of
 * those lines is reachable from the host with no hardware -- this file
 * calls each API in both the not-initialized and initialized states to
 * close the gap. No GCOVR_EXCL markers are required.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_ble_security.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum ble_security_cov_fixture_t
 * @brief Poison values written into out-parameters before a call, so one that fails without assigning is detectable.
 */
typedef enum : uint8_t {
  k_ble_sec_poison_out =
    0xFFU, /**< Poison written into a count out-parameter, so a call that fails without setting it is detectable. */
} ble_security_cov_fixture_t;

/** @brief Test hook -- forces the internal bond count without a store. */
extern void ra8_ble_security_test_set_bond_count(uint8_t count);

/**
 * @brief Bring the wrapper back to a known not-initialized state.
 *
 * @details Repeated close() is idempotent once already closed, so
 *          this is safe to call at the top of every case regardless of
 *          the prior test's terminal state.
 */
static void reset_state(void)
{
  (void)ra8_ble_security_close();
}

/**
 * @brief Initialize the wrapper with a benign Just-Works config.
 *
 * @param[in] io_cap Local I/O capability to seed into the config.
 */
static void init_with(ra8_ble_security_io_cap_t io_cap)
{
  const ra8_ble_security_config_t cfg = {
    .io_cap           = io_cap,
    .bonding_enable   = 1U,
    .mitm_required    = 0U,
    .sc_only          = 0U,
    .use_rsip_offload = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_security_init(&cfg));
}

/**
 * @test test_pair_not_initialized
 *
 * @par MC/DC:
 * Decision: `if (s_state.initialized == 0U)` (1 condition,
 * ra8_ble_security_pair, ra8_ble_security.c initialized-guard).
 * - V1 initialized==0 -> C1=T. Decision T (return k_ra8_err_not_initialized).
 * Paired with test_pair_initialized (C1=F) this varies the single
 * condition across both outcomes; N+1 = 2 vectors for N=1.
 */
static void test_pair_not_initialized(void)
{
  TEST_BEGIN("test_pair_not_initialized");
  reset_state();
  /* No init: the guard must reject the request. */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ble_security_pair(0x0010U));
  TEST_END("test_pair_not_initialized");
}

/**
 * @test test_pair_initialized
 *
 * @par MC/DC:
 * Decision: `if (s_state.initialized == 0U)` (1 condition,
 * ra8_ble_security_pair, ra8_ble_security.c initialized-guard).
 * - V2 initialized==1 -> C1=F. Decision F (fall through, host #else leg
 *   discards conn_handle and returns k_ra8_ok).
 * Completes the N+1 pair started in test_pair_not_initialized.
 */
static void test_pair_initialized(void)
{
  TEST_BEGIN("test_pair_initialized");
  reset_state();
  init_with(k_ra8_ble_io_cap_no_input_no_out);
  /* Host #else leg has no controller to reject the handle -> k_ra8_ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_security_pair(0x0010U));
  TEST_END("test_pair_initialized");
}

/**
 * @test test_passkey_reply_not_initialized
 *
 * @par MC/DC:
 * Decision: `if (s_state.initialized == 0U)` (1 condition,
 * ra8_ble_security_passkey_reply, ra8_ble_security.c initialized-guard).
 * - V1 initialized==0 -> C1=T. Decision T (return k_ra8_err_not_initialized),
 *   evaluated before the passkey-range check so the guard is proven
 *   independently. Paired with test_passkey_reply_happy (C1=F): N+1 = 2.
 */
static void test_passkey_reply_not_initialized(void)
{
  TEST_BEGIN("test_passkey_reply_not_initialized");
  reset_state();
  /* No init: rejected before the range check is reached. */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ble_security_passkey_reply(0x0020U, 424242U, 1U));
  TEST_END("test_passkey_reply_not_initialized");
}

/**
 * @test test_passkey_reply_happy
 *
 * @par MC/DC:
 * Two single-condition decisions in ra8_ble_security_passkey_reply:
 * - `if (s_state.initialized == 0U)`  -> C1=F (fall through).
 * - `if (passkey > k_ble_passkey_max)` -> C1=F (in range, fall through).
 * With initialized==1 and passkey==0 (the minimum valid value) both
 * guards take their false leg, reaching the host #else body that
 * discards conn_handle / accept and returns k_ra8_ok. Pairs with
 * test_passkey_reply_not_initialized (guard T) and the baseline
 * out-of-range case (range T) for N+1 coverage of each guard.
 */
static void test_passkey_reply_happy(void)
{
  TEST_BEGIN("test_passkey_reply_happy");
  reset_state();
  init_with(k_ra8_ble_io_cap_keyboard_only);
  /* In-range passkey (boundary min) + accept flag -> host leg returns k_ra8_ok. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_security_passkey_reply(0x0020U, 0U, 0U));
  /* Boundary max 999999 also accepted. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_security_passkey_reply(0x0020U, 999999U, 1U));
  TEST_END("test_passkey_reply_happy");
}

/**
 * @test test_clear_bonds_not_initialized
 *
 * @par MC/DC:
 * Decision: `if (s_state.initialized == 0U)` (1 condition,
 * ra8_ble_security_clear_bonds, ra8_ble_security.c initialized-guard).
 * - V1 initialized==0 -> C1=T. Decision T (return k_ra8_err_not_initialized).
 * Paired with test_clear_bonds_initialized (C1=F): N+1 = 2.
 */
static void test_clear_bonds_not_initialized(void)
{
  TEST_BEGIN("test_clear_bonds_not_initialized");
  reset_state();
  /* No init: nothing to clear, guard rejects. */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_ble_security_clear_bonds());
  TEST_END("test_clear_bonds_not_initialized");
}

/**
 * @test test_clear_bonds_initialized
 *
 * @par MC/DC:
 * Decision: `if (s_state.initialized == 0U)` (1 condition,
 * ra8_ble_security_clear_bonds, ra8_ble_security.c initialized-guard).
 * - V2 initialized==1 -> C1=F. Decision F (fall through): the host #else
 *   leg skips the vendor store call, resets s_state.bond_count to 0 and
 *   returns k_ra8_ok. Completes the N+1 pair; also asserts the postcondition
 *   s_state.bond_count == 0 by reading it back through bond_count().
 */
static void test_clear_bonds_initialized(void)
{
  TEST_BEGIN("test_clear_bonds_initialized");
  reset_state();
  init_with(k_ra8_ble_io_cap_no_input_no_out);
  /* Seed a non-zero count so the reset-to-zero postcondition is observable. */
  ra8_ble_security_test_set_bond_count(3U);
  uint8_t count = k_ble_sec_poison_out;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_security_bond_count(&count));
  TEST_ASSERT_EQ(3U, count);
  /* Clear -> host leg zeroes the mirror. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_security_clear_bonds());
  count = k_ble_sec_poison_out;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_security_bond_count(&count));
  TEST_ASSERT_EQ(0U, count);
  TEST_END("test_clear_bonds_initialized");
}

int main(void)
{
  test_pair_not_initialized();
  test_pair_initialized();
  test_passkey_reply_not_initialized();
  test_passkey_reply_happy();
  test_clear_bonds_not_initialized();
  test_clear_bonds_initialized();
  return 0;
}
