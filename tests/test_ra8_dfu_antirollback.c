/**
 * @file test_ra8_dfu_antirollback.c
 * @brief Host unit tests for the DFU anti-rollback (downgrade) gate.
 *
 * @details
 * Drives ``ra8_rot_antirollback_check`` (the pure downgrade policy) and
 * ``ra8_rot_antirollback_verify`` (read -> policy -> commit over an injected
 * storage vtable). A file-local mock store exercises the accept-and-commit,
 * equal-accept, downgrade-reject, null-store, and read-failure paths. The real
 * extra-MRAM-backed default store is then driven through its full monotonic
 * contract (erased -> 0, commit persists, same/older is a no-op, verify denies a
 * downgrade and accepts same-or-newer).
 *
 * @note The durable counter lives in extra-MRAM (data-flash) at
 *       ``k_ra8_flash_extra_start``. Under RA8_OFF_TARGET the store uses a RAM
 *       shadow (the host fake does not model the extra-MRAM data side), so these
 *       tests validate the decision logic and the monotonic round-trip; silicon
 *       and ra8_emulator exercise the real flash path.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

/*
 * Anti-rollback is opt-in behind RA8_ENABLE_ROOT_OF_TRUST (default OFF, see
 * ra8_dfu_antirollback.h). The globbed ``ra8_dfu_antirollback.c`` therefore
 * compiles to nothing in the default host build, so its symbols are not
 * otherwise linked. Enable the flag for this test and pull the flag-gated
 * implementation in directly (the test include path covers libs/ra8_dfu/src),
 * keeping the gate exercisable -- mirroring tests/test_ra8_root_of_trust.c.
 */
#define RA8_ENABLE_ROOT_OF_TRUST

#include <stdint.h>

#include "ra8_dfu_antirollback.c" // NOLINT(bugprone-suspicious-include) -- pull in flag-gated impl
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum test_arb_const_t
 * @brief Stored / image version fixtures used by the anti-rollback tests.
 */
typedef enum : uint32_t {
  k_test_arb_stored = 5U, /**< Baseline stored highest-accepted version. */
  k_test_arb_newer  = 7U, /**< A newer image version (accepted).         */
  k_test_arb_older  = 3U, /**< An older image version (rejected).        */
} test_arb_const_t;

/** @brief Mock backing: the value the mock read reports as the stored minimum. */
static uint32_t s_mock_stored = 0U;
/** @brief Mock backing: status the mock read returns (injects read failures). */
static ra8_err_t s_mock_read_result = k_ra8_ok;
/** @brief Mock backing: status the mock commit returns (injects commit fails). */
static ra8_err_t s_mock_commit_result = k_ra8_ok;
/** @brief Mock backing: set true once the mock commit has been invoked. */
static bool s_mock_commit_called = false;
/** @brief Mock backing: the version value passed to the mock commit. */
static uint32_t s_mock_committed = 0U;

/**
 * @brief Mock store read: report ::s_mock_stored with ::s_mock_read_result.
 * @param[out] out_min_version Receives the mock stored version; non-NULL.
 * @return ::s_mock_read_result (``k_ra8_ok`` unless a failure was injected).
 * @retval k_ra8_ok           Stored value reported.
 * @retval k_ra8_err_null_ptr ``out_min_version`` is NULL.
 * @pre ``mock_reset`` ran to seed the backing state.
 * @pre ``out_min_version`` is the caller's sink.
 * @post ``*out_min_version`` holds ::s_mock_stored when non-NULL.
 * @post No global mock state is mutated.
 * @note Test-only helper.
 * @since 0.1.0
 */
static ra8_err_t mock_read(uint32_t* out_min_version)
{
  if (out_min_version == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out_min_version = s_mock_stored;
  return s_mock_read_result;
}

/**
 * @brief Mock store commit: record the version and return ::s_mock_commit_result.
 * @param[in] new_version Version the gate asks to persist.
 * @return ::s_mock_commit_result (``k_ra8_ok`` unless a failure was injected).
 * @retval k_ra8_ok Commit recorded.
 * @pre ``mock_reset`` ran to seed the backing state.
 * @pre The gate accepted the image before reaching commit.
 * @post ::s_mock_committed holds ``new_version`` and ::s_mock_commit_called is true.
 * @post No other global state is mutated.
 * @note Test-only helper.
 * @since 0.1.0
 */
static ra8_err_t mock_commit(uint32_t new_version)
{
  s_mock_committed     = new_version;
  s_mock_commit_called = true;
  return s_mock_commit_result;
}

/** @brief A fully-wired mock store (both accessors non-NULL). */
static const ra8_rot_antirollback_store_t s_mock_store = {
  .read   = mock_read,
  .commit = mock_commit,
};

/**
 * @brief Reset the mock backing to a known-good state with the given stored min.
 * @param[in] stored Value the mock read will report as the stored minimum.
 * @pre The test is about to drive a fresh verify/check sequence.
 * @pre ``stored`` is the desired baseline highest-accepted version.
 * @post Read and commit results are ``k_ra8_ok``; commit-called is false.
 * @post ::s_mock_committed is cleared to zero.
 * @note Test-only helper.
 * @since 0.1.0
 */
static void mock_reset(uint32_t stored)
{
  s_mock_stored        = stored;
  s_mock_read_result   = k_ra8_ok;
  s_mock_commit_result = k_ra8_ok;
  s_mock_commit_called = false;
  s_mock_committed     = 0U;
}

/**
 * @brief Pure policy: newer / equal accept, older reject (MC/DC on the compare).
 *
 * @par MC/DC:
 * Decision: ``if (image_version < stored_min_version)`` (1 condition).
 * - Vector 1: image=7, stored=5 -> false -> ACCEPT (k_ra8_ok).
 * - Vector 2: image=3, stored=5 -> true  -> REJECT (k_ra8_err_validation_failed).
 * Vectors 1+2 flip the single condition and the outcome: minimal MC/DC
 * (N+1 = 2 for N=1). Vector 3 (image=5, stored=5) is the equality boundary --
 * still false -> ACCEPT, proving ``>=`` (not ``>``).
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post No global state changes.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_check_policy_mcdc(void)
{
  TEST_BEGIN("anti-rollback: check newer/equal accept, older reject (MC/DC)");

  /* Vector 1: newer -> condition false -> accept. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_rot_antirollback_check((uint32_t)k_test_arb_newer, (uint32_t)k_test_arb_stored));
  /* Vector 2: older -> condition true -> reject. */
  TEST_ASSERT_EQ(
    k_ra8_err_validation_failed,
    ra8_rot_antirollback_check((uint32_t)k_test_arb_older, (uint32_t)k_test_arb_stored));
  /* Vector 3: equal boundary -> condition false -> accept. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_rot_antirollback_check((uint32_t)k_test_arb_stored, (uint32_t)k_test_arb_stored));

  TEST_END("anti-rollback: check newer/equal accept, older reject (MC/DC)");
}

/**
 * @brief Verify: a newer image is accepted and the counter is advanced.
 *
 * @par MC/DC: not applicable -- happy-path control vector. Each guard in
 *      ``ra8_rot_antirollback_verify`` is a single condition; this establishes
 *      the all-false control the deny tests below vary against.
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post No global state changes.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_verify_newer_accept_commits(void)
{
  TEST_BEGIN("anti-rollback: verify newer -> accept + commit");

  mock_reset((uint32_t)k_test_arb_stored);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rot_antirollback_verify(&s_mock_store, (uint32_t)k_test_arb_newer));
  /* Accept advances the durable counter to the new version. */
  TEST_ASSERT(s_mock_commit_called);
  TEST_ASSERT_EQ(k_test_arb_newer, s_mock_committed);

  TEST_END("anti-rollback: verify newer -> accept + commit");
}

/**
 * @brief Verify: an equal version is accepted (re-flash) and committed.
 *
 * @par MC/DC: not applicable -- exercises the ``>=`` equality boundary through
 *      the full verify flow; the policy compare's MC/DC lives in
 *      ``test_check_policy_mcdc``.
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post No global state changes.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_verify_equal_accept_commits(void)
{
  TEST_BEGIN("anti-rollback: verify equal -> accept + commit");

  mock_reset((uint32_t)k_test_arb_stored);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rot_antirollback_verify(&s_mock_store, (uint32_t)k_test_arb_stored));
  TEST_ASSERT(s_mock_commit_called);
  TEST_ASSERT_EQ(k_test_arb_stored, s_mock_committed);

  TEST_END("anti-rollback: verify equal -> accept + commit");
}

/**
 * @brief Verify: an older image is rejected and the counter is NOT advanced.
 *
 * @par MC/DC:
 * Decision: policy result ``policy_err != k_ra8_ok`` inside verify (1 condition).
 * - Vector 1: image=7 (newer) -> false -> proceed to commit (accept test above).
 * - Vector 2: image=3 (older) -> true  -> DENY (this test); commit not reached.
 * N+1 = 2 vectors for N=1 condition: minimal MC/DC.
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post No global state changes.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_verify_older_rejected(void)
{
  TEST_BEGIN("anti-rollback: verify older -> reject, no commit");

  mock_reset((uint32_t)k_test_arb_stored);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_rot_antirollback_verify(&s_mock_store, (uint32_t)k_test_arb_older));
  /* A rejected downgrade must never advance the counter. */
  TEST_ASSERT(!s_mock_commit_called);

  TEST_END("anti-rollback: verify older -> reject, no commit");
}

/**
 * @brief Verify: a NULL store / NULL accessor default-denies.
 *
 * @par MC/DC:
 * Three independent NULL guards (``RA8_CHECK_NULL_PTR``), each a single
 * condition:
 * - store=NULL                       -> deny (varies the store guard).
 * - store={read=NULL, commit=ok}     -> deny (varies the read guard).
 * - store={read=ok,   commit=NULL}   -> deny (varies the commit guard).
 * Combined with the valid-store accept control, each guard independently forces
 * the deny outcome.
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post No global state changes.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_verify_null_store_denied(void)
{
  TEST_BEGIN("anti-rollback: verify null store / accessor -> denied");

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rot_antirollback_verify(nullptr, (uint32_t)k_test_arb_newer));

  const ra8_rot_antirollback_store_t no_read = {.read = nullptr, .commit = mock_commit};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rot_antirollback_verify(&no_read, (uint32_t)k_test_arb_newer));

  const ra8_rot_antirollback_store_t no_commit = {.read = mock_read, .commit = nullptr};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rot_antirollback_verify(&no_commit, (uint32_t)k_test_arb_newer));

  TEST_END("anti-rollback: verify null store / accessor -> denied");
}

/**
 * @brief Verify: a store read failure default-denies (no policy, no commit).
 *
 * @par MC/DC:
 * Decision: ``read_err != k_ra8_ok`` inside verify (1 condition).
 * - Vector 1: read returns k_ra8_ok -> false -> proceed (accept test).
 * - Vector 2: read returns k_ra8_err_hw_error -> true -> DENY (this test).
 * N+1 = 2 vectors for N=1 condition: minimal MC/DC. The injected read error is
 * propagated verbatim and commit is never reached.
 *
 * @pre None.
 * @pre None.
 * @post No persistent side effects.
 * @post No global state changes.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_verify_read_failure_denied(void)
{
  TEST_BEGIN("anti-rollback: verify read failure -> denied");

  mock_reset((uint32_t)k_test_arb_stored);
  s_mock_read_result = k_ra8_err_hw_error; /* inject a backing-store read fault */
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 ra8_rot_antirollback_verify(&s_mock_store, (uint32_t)k_test_arb_newer));
  /* A failed read must short-circuit before the policy and the commit. */
  TEST_ASSERT(!s_mock_commit_called);

  TEST_END("anti-rollback: verify read failure -> denied");
}

/**
 * @brief Default store: extra-MRAM-backed, persistent, and monotonic.
 *
 * @par MC/DC: not applicable -- exercises the NV-backed default store's
 *      read/commit round-trip, its monotonic (no-downgrade) commit, and the
 *      verify accept/deny it now enforces.
 *
 * @pre The extra-MRAM counter starts erased (fresh fake / device).
 * @pre The default store exposes non-null read and commit.
 * @post The durable counter holds the highest committed version.
 * @post No unrelated global state changes.
 * @note Test-only.
 * @since 0.1.0
 */
static void test_default_store_nv_backed(void)
{
  TEST_BEGIN("anti-rollback: default store NV-backed + monotonic");

  const ra8_rot_antirollback_store_t* store = ra8_rot_antirollback_default_store();
  TEST_ASSERT(store != nullptr);
  TEST_ASSERT(store->read != nullptr);
  TEST_ASSERT(store->commit != nullptr);

  /* Simulate a fresh device: force the durable counter (host RAM shadow) to its
   * erased value. The store must map erased -> version 0, and the null guard
   * still fires. */
  s_fake_ar_counter = (uint32_t)k_ra8_rot_ar_erased;
  uint32_t stored   = k_test_arb_stored;
  TEST_ASSERT_EQ(k_ra8_ok, store->read(&stored));
  TEST_ASSERT_EQ(0U, stored);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, store->read(nullptr));

  /* Commit persists to the durable counter and reads back. */
  TEST_ASSERT_EQ(k_ra8_ok, store->commit((uint32_t)k_test_arb_newer));
  TEST_ASSERT_EQ(k_ra8_ok, store->read(&stored));
  TEST_ASSERT_EQ(k_test_arb_newer, stored);

  /* Re-committing an older version is a monotonic no-op -- the floor holds. */
  TEST_ASSERT_EQ(k_ra8_ok, store->commit((uint32_t)k_test_arb_older));
  TEST_ASSERT_EQ(k_ra8_ok, store->read(&stored));
  TEST_ASSERT_EQ(k_test_arb_newer, stored);

  /* verify() now denies a real downgrade and accepts a same-or-newer image. */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_rot_antirollback_verify(store, (uint32_t)k_test_arb_older));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rot_antirollback_verify(store, (uint32_t)k_test_arb_newer));

  TEST_END("anti-rollback: default store NV-backed + monotonic");
}

int main(void)
{
  test_check_policy_mcdc();
  test_verify_newer_accept_commits();
  test_verify_equal_accept_commits();
  test_verify_older_rejected();
  test_verify_null_store_denied();
  test_verify_read_failure_denied();
  test_default_store_nv_backed();
  return 0;
}
