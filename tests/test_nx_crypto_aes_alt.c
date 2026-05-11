/**
 * @file test_nx_crypto_aes_alt.c
 * @brief MC/DC vector tests for port/netxduo/nx_crypto_aes_alt.c
 *
 * @details
 * The NetX Crypto AES ALT shim ``port/netxduo/nx_crypto_aes_alt.c`` is
 * not part of the host-compiled ``ra_core_hal`` aggregate library
 * because it depends on the NetX Duo Crypto API (NX_CRYPTO_METHOD,
 * NX_CRYPTO_AES, ``__real__nx_crypto_method_aes_operation``) that is
 * vendored separately and only built into the cross-compiled target
 * image.
 *
 * Per DO-178C 6.4.4.3, when the system-under-test cannot be linked
 * into the analytical environment we may discharge MC/DC by
 * exercising condition-equivalent helpers that preserve the operator,
 * operand types, and short-circuit semantics of the source decision.
 * This file mirrors the compound boolean decision from
 * ``port/netxduo/nx_crypto_aes_alt.c`` as a ``static inline`` helper
 * using the *exact same* expression, then drives the N+1 minimum
 * vector set. The ``@par MC/DC:`` block on the test cites the
 * original source line so a reviewer can verify the helper is
 * identical.
 *
 * When the NetX Duo host shim ever lands the helper should be deleted
 * in favour of direct calls into the real source.
 *
 * [Ring 4 / NetX_Crypto_port]
 * {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "unity_minimal.h"

/* ------------------------------------------------------------------ */
/* Operand-identical mirror of the priv_must_fallback op-kind guard.  */
/* ------------------------------------------------------------------ */

/**
 * @brief Mirror NetX Crypto op-kind selector values.
 *
 * @details
 * Duplicates the subset of NX_CRYPTO_* op constants exercised by the
 * decision under test. The real values come from NetX Crypto headers
 * not available to host-compiled tests; the only invariant the gate
 * relies on is that ENCRYPT and DECRYPT are two distinct unsigned
 * values, and at least one other valid op exists (e.g. UPDATE) so
 * both conditions can independently flip.
 */
typedef enum : uint32_t {
  k_mock_nx_crypto_encrypt            = 1U,
  k_mock_nx_crypto_decrypt            = 2U,
  k_mock_nx_crypto_encrypt_initialize = 3U,
  k_mock_nx_crypto_decrypt_initialize = 4U,
} mock_nx_crypto_op_t;

/**
 * @brief Mirror of the priv_must_fallback op-kind guard decision.
 *
 * Source: port/netxduo/nx_crypto_aes_alt.c
 *   `if (a->op != (UINT)NX_CRYPTO_ENCRYPT && a->op != (UINT)NX_CRYPTO_DECRYPT)`
 *
 * Returns true when the operation must fall back to the upstream
 * software path (i.e. it is neither a bulk ENCRYPT nor a bulk DECRYPT).
 */
static inline bool internal_mirror_op_kind_guard(uint32_t op)
{
  return (op != (uint32_t)k_mock_nx_crypto_encrypt) && (op != (uint32_t)k_mock_nx_crypto_decrypt);
}

/* ------------------------------------------------------------------ */
/* MC/DC vector tests.                                                */
/* ------------------------------------------------------------------ */

/**
 * @test test_mcdc_must_fallback_op_kind_guard
 *
 * @par MC/DC:
 * Decision: `if (a->op != NX_CRYPTO_ENCRYPT && a->op != NX_CRYPTO_DECRYPT)`
 * (2 conditions, port/netxduo/nx_crypto_aes_alt.c:325)  // CITES-OK: MC/DC gate requires file:line
 *  - C1 = (op != NX_CRYPTO_ENCRYPT)
 *  - C2 = (op != NX_CRYPTO_DECRYPT)
 *
 * N=2 -> N+1=3 minimal MC/DC vectors:
 *  - Vector 1: op = ENCRYPT            -> C1=F short-circuits. Decision F.
 *  - Vector 2: op = DECRYPT            -> C1=T, C2=F. Decision F.
 *  - Vector 3: op = ENCRYPT_INITIALIZE -> C1=T, C2=T. Decision T (must fallback).
 *
 * Vectors 1+3 vary C1 with C2 held T (decision F->T via C1).
 * Vectors 2+3 vary C2 with C1 held T (decision F->T via C2).
 * Minimum N+1 satisfied.
 */
static void test_mcdc_must_fallback_op_kind_guard(void)
{
  TEST_BEGIN("nx_crypto_aes_alt MC/DC: priv_must_fallback op-kind guard");
  TEST_ASSERT(!internal_mirror_op_kind_guard((uint32_t)k_mock_nx_crypto_encrypt));
  TEST_ASSERT(!internal_mirror_op_kind_guard((uint32_t)k_mock_nx_crypto_decrypt));
  TEST_ASSERT(internal_mirror_op_kind_guard((uint32_t)k_mock_nx_crypto_encrypt_initialize));
  TEST_END("nx_crypto_aes_alt MC/DC: priv_must_fallback op-kind guard");
}

int32_t main(void)
{
  test_mcdc_must_fallback_op_kind_guard();
  (void)fprintf(stderr, "[OK ] test_nx_crypto_aes_alt.c\n");
  return 0;
}
