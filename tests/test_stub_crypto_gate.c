/**
 * @file test_stub_crypto_gate.c
 * @brief Stub-vs-production crypto build-gate reachability test (issue #180)
 *
 * @par Tag
 * [Ring 1 / Core] {World: S}
 *
 * @details
 * Companion to the compile-time / gate-time fail-closed enforcement:
 *
 *   - cmake/ra_add_app.cmake defines RA_INSECURE_STUB_CRYPTO (OFF by default),
 *   - each stub TU wraps its insecure body in
 *     `#if defined(RA_INSECURE_STUB_CRYPTO) || defined(RA_SIMULATOR_MODE)` with a
 *     fail-closed #else (every entry point returns k_ra_err_not_supported),
 *   - scripts/utils/check_stub_crypto_guarded.py fails the build at gate time if
 *     any insecure body escapes that guard.
 *
 * This ctest runs in the host build, which compiles the stub TUs under
 * RA_SIMULATOR_MODE (the #if branch). It proves the OTHER half of the guard:
 * that in stub mode every insecure entry point is REACHABLE and exhibits its
 * placeholder behaviour -- a deterministic (predictable) TRNG, a self-minted
 * key-import blob the stub MAC accepts, a deterministic vault challenge, and a
 * round-tripping non-cryptographic RSIP key-wrap. If the fail-closed #else were
 * compiled instead, every one of these calls would return k_ra_err_not_supported
 * and this test would fail -- so the test also demonstrates that the two guard
 * branches are genuinely different. The deterministic / round-trip properties
 * asserted here are exactly the INSECURE behaviour a real backend must replace.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "key_import.h"
#include "key_vault.h"
#include "ra_err.h"
#include "ra_rsip_key_injection.h"
#include "secure_trng.h"
#include "unity_minimal.h"

/** @brief Fixed sizes used by the reachability cases (no magic numbers). */
typedef enum : uint16_t {
  k_stub_trng_len   = 32U,  /**< TRNG bytes drawn per read.               */
  k_stub_key_bytes  = 32U,  /**< 256-bit key (vault + import).            */
  k_stub_blob_bytes = 36U,  /**< Sealed import blob (k_ra_key_import...). */
  k_stub_chal_bytes = 32U,  /**< Vault challenge / digest length.         */
  k_stub_aes_bytes  = 16U,  /**< AES-128 raw key length.                  */
  k_stub_rsip_total = 636U, /**< Wrapped-key buffer (k_ra_rsip_...total). */
  k_stub_slot_zero  = 0U,   /**< Vault / import slot under test.          */
} test_stub_sizes_t;

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises public-API reachability
 * and the stub's deterministic behaviour; no `&&` / `||` in the code paths it
 * touches.)
 */
static void test_trng_is_deterministic_stub(void)
{
  TEST_BEGIN("stub-gate: TRNG is a reachable deterministic PRNG (insecure)");
  uint8_t a[k_stub_trng_len] = {};
  uint8_t b[k_stub_trng_len] = {};

  TEST_ASSERT_EQ(k_ra_ok, ra_secure_trng_reset());
  TEST_ASSERT_EQ(k_ra_ok, ra_secure_trng_read(a, (uint32_t)k_stub_trng_len));
  TEST_ASSERT_EQ(k_ra_ok, ra_secure_trng_reset());
  TEST_ASSERT_EQ(k_ra_ok, ra_secure_trng_read(b, (uint32_t)k_stub_trng_len));

  /* Two reads after reset are byte-identical: a deterministic PRNG, NOT a real
   * TRNG. This is the #1 insecure property; the fail-closed #else would have
   * returned k_ra_err_not_supported above instead. */
  TEST_ASSERT_EQ(0, memcmp(a, b, (size_t)k_stub_trng_len));

  /* Reachable: output is not all-zero. */
  bool any_nonzero = false;
  for (uint32_t i = 0U; i < (uint32_t)k_stub_trng_len; ++i) {
    if (a[i] != 0U) {
      any_nonzero = true;
    }
  }
  TEST_ASSERT(any_nonzero);
  TEST_END("stub-gate: TRNG is a reachable deterministic PRNG (insecure)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test.)
 */
static void test_key_import_accepts_self_minted_blob(void)
{
  TEST_BEGIN("stub-gate: key-import stub accepts a self-minted blob (forgeable MAC)");
  uint8_t key[k_stub_key_bytes]   = {};
  uint8_t blob[k_stub_blob_bytes] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_stub_key_bytes; ++i) {
    key[i] = (uint8_t)i;
  }

  TEST_ASSERT_EQ(k_ra_ok, ra_key_import_reset());
  /* Anyone who knows the weak fold can build a blob the stub accepts. */
  TEST_ASSERT_EQ(k_ra_ok, ra_key_import_build_blob(key, blob));

  uint32_t handle = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_key_import_seal(blob, (uint32_t)k_stub_blob_bytes, &handle));
  TEST_ASSERT(handle != 0U);

  uint16_t slot = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_key_import_resolve(handle, &slot));
  TEST_END("stub-gate: key-import stub accepts a self-minted blob (forgeable MAC)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test.)
 */
static void test_key_vault_challenge_is_reachable(void)
{
  TEST_BEGIN("stub-gate: vault store + challenge reachable and deterministic");
  uint8_t key[k_stub_key_bytes]   = {};
  uint8_t chal[k_stub_chal_bytes] = {};
  uint8_t out1[k_stub_chal_bytes] = {};
  uint8_t out2[k_stub_chal_bytes] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_stub_key_bytes; ++i) {
    key[i]  = (uint8_t)i;
    chal[i] = (uint8_t)(k_stub_key_bytes - i);
  }

  TEST_ASSERT_EQ(k_ra_ok, ra_key_vault_init());
  TEST_ASSERT_EQ(k_ra_ok, ra_key_vault_store((uint16_t)k_stub_slot_zero, key));
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_key_vault_sha256_xor_challenge((uint16_t)k_stub_slot_zero, chal, out1));
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_key_vault_sha256_xor_challenge((uint16_t)k_stub_slot_zero, chal, out2));

  /* Same challenge -> same response (software vault reachable in stub mode). */
  TEST_ASSERT_EQ(0, memcmp(out1, out2, (size_t)k_stub_chal_bytes));
  TEST_END("stub-gate: vault store + challenge reachable and deterministic");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test.)
 */
static void test_rsip_key_wrap_roundtrips_stub(void)
{
  TEST_BEGIN("stub-gate: RSIP key-wrap round-trips (non-cryptographic)");
  uint8_t raw[k_stub_aes_bytes]      = {};
  uint8_t wrapped[k_stub_rsip_total] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_stub_aes_bytes; ++i) {
    raw[i] = (uint8_t)i;
  }

  TEST_ASSERT_EQ(k_ra_ok, ra_rsip_key_inject_aes(wrapped, raw, k_ra_rsip_aes_key_bits_128));
  /* The stub MAC validates its own wrap: reachable round-trip in stub mode. */
  TEST_ASSERT_EQ(k_ra_ok, ra_rsip_key_validate(wrapped, k_ra_rsip_wrapped_type_aes));
  TEST_END("stub-gate: RSIP key-wrap round-trips (non-cryptographic)");
}

int32_t main(void)
{
  test_trng_is_deterministic_stub();
  test_key_import_accepts_self_minted_blob();
  test_key_vault_challenge_is_reachable();
  test_rsip_key_wrap_roundtrips_stub();
  (void)fprintf(stderr,
                "[OK ] test_stub_crypto_gate.c -- stubs reachable + insecure under the guard\n");
  return 0;
}
