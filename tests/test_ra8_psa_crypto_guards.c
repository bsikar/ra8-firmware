/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_psa_crypto_guards.c
 * @brief MC/DC vectors + error-leg coverage for the ra8_psa_crypto guards
 *
 * @details
 * Split sibling of the original test_ra8_psa_crypto.c suite covering
 * the compound-decision guards outside the AEAD path plus the
 * single-condition error legs of ``ra8_psa_crypto.c``:
 *
 * - handle-range validation (``internal_handle_valid``)
 * - key-import argument quad, hash-compute pairs, sign / verify
 *   argument triples (canonical N+1 MC/DC vectors)
 * - the deinit pool scrub, not-initialized guards on every entry
 *   point, and the per-operation handle / usage / size guards
 *
 * Sibling suites: test_ra8_psa_crypto_api.c (public-API contract)
 * and test_ra8_psa_crypto_aead_mcdc.c (AEAD MC/DC vectors).
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_psa_crypto.h"
#include "unity_minimal.h"

/**
 * @brief Address span guaranteed to exceed the whole static key pool.
 *
 * @details Used to derive out-of-range handle sentinels from a real in-pool
 * address: the pool's absolute placement is layout-dependent, but offsetting a
 * genuine slot pointer by more than the entire pool size lands strictly below
 * the pool base (below) or at/above the pool end (above) on any host, so both
 * range conditions of ``internal_handle_valid`` can be exercised portably.
 */
typedef enum : uint32_t {
  k_psa_pool_far_span = 0x00100000U, /**< 1 MiB >> sizeof(s_key_pool). */
} ra8_psa_pool_span_t;

static const uint8_t k_test_aes_key[16] = {
  0x00U,
  0x11U,
  0x22U,
  0x33U,
  0x44U,
  0x55U,
  0x66U,
  0x77U,
  0x88U,
  0x99U,
  0xaaU,
  0xbbU,
  0xccU,
  0xddU,
  0xeeU,
  0xffU,
};

static const uint8_t k_test_ecdsa_key[32] = {
  0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U, 0x19U, 0x1aU,
  0x1bU, 0x1cU, 0x1dU, 0x1eU, 0x1fU, 0x20U, 0x21U, 0x22U, 0x23U, 0x24U, 0x25U,
  0x26U, 0x27U, 0x28U, 0x29U, 0x2aU, 0x2bU, 0x2cU, 0x2dU, 0x2eU, 0x2fU,
};

/* ------------------------------------------------------------------ */
/* helpers */
/* ------------------------------------------------------------------ */
static void prep_init(void)
{
  /* deinit is allowed to fail with not-initialized on the very first
   * call; subsequent calls observe a properly torn-down module. */
  (void)ra8_psa_crypto_deinit();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_psa_crypto_init());
}

static void teardown(void)
{
  (void)ra8_psa_crypto_deinit();
}

/* ================================================================== */
/* MC/DC vectors */
/* ================================================================== */
/*
 * The vectors below close DO-178C Level B / IEC 61508 SIL 3 MC/DC
 * obligations for compound boolean decisions in
 * ``libs/ra8_psa_crypto/src/ra8_psa_crypto.c``. Each test follows the
 * canonical N+1 pattern (see tests/test_ra8_xspi.c::
 * test_set_xip_mode_mcdc_addr_bytes for the reference template).
 */

/**
 * @enum ra8_psa_mcdc_const_t
 * @brief Magic-number-free vector inputs for the MC/DC tests below.
 */
typedef enum : uint16_t {
  k_psa_mcdc_short_input = 1U, /**< PSA mcdc short input. */
} ra8_psa_mcdc_const_t;

/** @brief Helper: import a valid AES key with the requested usage mask. */
static ra8_psa_key_t mcdc_import_aes_key(ra8_psa_key_usage_t usage)
{
  const ra8_psa_key_attr_t attr = {
    .type  = k_ra8_psa_key_type_aes,
    .alg   = k_ra8_psa_alg_aes_gcm,
    .usage = usage,
  };
  ra8_psa_key_t k = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_psa_key_import(&k, &attr, k_test_aes_key, sizeof(k_test_aes_key)));
  return k;
}

/**
 * @test test_mcdc_handle_valid_range_check
 * @par MC/DC:
 * Decision: `if ((handle < base) || (handle >= end))` (2 conditions,
 * `internal_handle_valid`). Reached via `ra8_psa_key_destroy`.
 * V1 real slot: C1=F,C2=F -> F (in range). V2 below the pool base:
 * C1=T -> T. V3 at/above the pool end: C1=F,C2=T -> T. N+1 = 3 vectors
 * for N=2. The sentinels are derived from a genuine in-pool address by
 * `+/- k_psa_pool_far_span` so each straddles a pool boundary on any host
 * (a fixed stack/absolute address is not guaranteed to lie below the pool,
 * which previously left C1 unexercised).
 */
static void test_mcdc_handle_valid_range_check(void)
{
  TEST_BEGIN("psa MC/DC: handle range (handle<base || handle>=end)");
  prep_init();
  ra8_psa_key_t real = mcdc_import_aes_key(k_ra8_psa_usage_encrypt);
  TEST_ASSERT_NOT_NULL(real);
  /* Capture an address INSIDE the static pool before releasing the slot. */
  const uintptr_t in_pool = (uintptr_t)real;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_psa_key_destroy(real));
  /* V2: strictly below the pool base -> C1 (handle < base) true. */
  /* ra8_psa_key_t is itself a pointer typedef, so `const` here qualifies the
   * local handle -- which is exactly the intent: the handle never changes.
   * There is no pointee to qualify; the key material is owned by the pool. */
  // NOLINTNEXTLINE(misc-misplaced-const)
  const ra8_psa_key_t below = (ra8_psa_key_t)(in_pool - (uintptr_t)k_psa_pool_far_span);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_psa_key_destroy(below));
  /* V3: at/above the pool end -> C1 false, C2 (handle >= end) true. */
  /* ra8_psa_key_t is itself a pointer typedef, so `const` here qualifies the
   * local handle -- which is exactly the intent: the handle never changes.
   * There is no pointee to qualify; the key material is owned by the pool. */
  // NOLINTNEXTLINE(misc-misplaced-const)
  const ra8_psa_key_t above = (ra8_psa_key_t)(in_pool + (uintptr_t)k_psa_pool_far_span);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_psa_key_destroy(above));
  teardown();
  TEST_END("psa MC/DC: handle range (handle<base || handle>=end)");
}

/**
 * @test test_mcdc_key_import_arg_quad
 * @par MC/DC:
 * Decision: `if ((out_handle == NULL) || (attr == NULL) ||
 *               (data == NULL) || (data_len == 0U))` (4 conditions,
 * libs/ra8_psa_crypto/src/ra8_psa_crypto.c line 673, `ra8_psa_key_import`).
 * V1 all valid -> F. V2 out=NULL -> C1=T. V3 attr=NULL -> C2=T.
 * V4 data=NULL -> C3=T. V5 len=0 -> C4=T. N+1 = 5 vectors for N=4.
 */
static void test_mcdc_key_import_arg_quad(void)
{
  TEST_BEGIN("psa MC/DC: import arg quad (out||attr||data||len)");
  prep_init();
  const ra8_psa_key_attr_t attr = {.type  = k_ra8_psa_key_type_aes,
                                   .alg   = k_ra8_psa_alg_aes_gcm,
                                   .usage = k_ra8_psa_usage_encrypt};
  ra8_psa_key_t            k    = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_psa_key_import(&k, &attr, k_test_aes_key, sizeof(k_test_aes_key)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_psa_key_destroy(k));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_psa_key_import(nullptr, &attr, k_test_aes_key, sizeof(k_test_aes_key)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_psa_key_import(&k, nullptr, k_test_aes_key, sizeof(k_test_aes_key)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_psa_key_import(&k, &attr, nullptr, sizeof(k_test_aes_key)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_psa_key_import(&k, &attr, k_test_aes_key, 0U));
  teardown();
  TEST_END("psa MC/DC: import arg quad (out||attr||data||len)");
}

/**
 * @test test_mcdc_hash_compute_out_pair
 * @par MC/DC:
 * Decision: `if ((out == NULL) || (out_len == NULL))` (2 conditions,
 * libs/ra8_psa_crypto/src/ra8_psa_crypto.c line 735). N+1 = 3 vectors for N=2.
 */
static void test_mcdc_hash_compute_out_pair(void)
{
  TEST_BEGIN("psa MC/DC: hash_compute (out||out_len) NULL pair");
  prep_init();
  uint8_t       out[k_ra8_psa_sha256_len];
  size_t        ol  = 0U;
  const uint8_t inp = (uint8_t)'a';
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_hash_compute(k_ra8_psa_alg_sha_256,
                                      &inp,
                                      (size_t)k_psa_mcdc_short_input,
                                      out,
                                      sizeof(out),
                                      &ol));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_psa_hash_compute(k_ra8_psa_alg_sha_256,
                                      &inp,
                                      (size_t)k_psa_mcdc_short_input,
                                      nullptr,
                                      sizeof(out),
                                      &ol));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_psa_hash_compute(k_ra8_psa_alg_sha_256,
                                      &inp,
                                      (size_t)k_psa_mcdc_short_input,
                                      out,
                                      sizeof(out),
                                      nullptr));
  teardown();
  TEST_END("psa MC/DC: hash_compute (out||out_len) NULL pair");
}

/**
 * @test test_mcdc_hash_compute_input_and_len
 * @par MC/DC:
 * Decision: `if ((input == NULL) && (input_len != 0U))` (2 conditions,
 * libs/ra8_psa_crypto/src/ra8_psa_crypto.c line 738).
 * V1 NULL,len=0 -> C1=T,C2=F -> F. V2 !=NULL,len=1 -> C1=F -> F.
 * V3 NULL,len=1 -> C1=T,C2=T -> T. N+1 = 3 vectors for N=2.
 */
static void test_mcdc_hash_compute_input_and_len(void)
{
  TEST_BEGIN("psa MC/DC: hash_compute (input==NULL && len!=0)");
  prep_init();
  uint8_t       out[k_ra8_psa_sha256_len];
  size_t        ol  = 0U;
  const uint8_t inp = (uint8_t)'a';
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_hash_compute(k_ra8_psa_alg_sha_256, nullptr, 0U, out, sizeof(out), &ol));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_hash_compute(k_ra8_psa_alg_sha_256,
                                      &inp,
                                      (size_t)k_psa_mcdc_short_input,
                                      out,
                                      sizeof(out),
                                      &ol));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_psa_hash_compute(k_ra8_psa_alg_sha_256,
                                      nullptr,
                                      (size_t)k_psa_mcdc_short_input,
                                      out,
                                      sizeof(out),
                                      &ol));
  teardown();
  TEST_END("psa MC/DC: hash_compute (input==NULL && len!=0)");
}

/**
 * @test test_mcdc_sign_hash_arg_triple
 * @par MC/DC:
 * Decision: `if ((hash == NULL) || (sig == NULL) || (sig_len == NULL))`
 * (3 conditions, libs/ra8_psa_crypto/src/ra8_psa_crypto.c line 778).
 * N+1 = 4 vectors for N=3.
 */
static void test_mcdc_sign_hash_arg_triple(void)
{
  TEST_BEGIN("psa MC/DC: sign_hash (hash||sig||sig_len) NULL triple");
  prep_init();
  const ra8_psa_key_attr_t attr = {.type  = k_ra8_psa_key_type_ecc_p256_priv,
                                   .alg   = k_ra8_psa_alg_ecdsa_sha_256,
                                   .usage = k_ra8_psa_usage_sign};
  ra8_psa_key_t            k    = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_key_import(&k, &attr, k_test_ecdsa_key, sizeof(k_test_ecdsa_key)));
  uint8_t hash[k_ra8_psa_sha256_len] = {};
  uint8_t sig[k_ra8_psa_max_sig_bytes];
  size_t  sl = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,

    ra8_psa_sign_hash(k, k_ra8_psa_alg_ecdsa_sha_256, hash, sizeof(hash), sig, sizeof(sig), &sl));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,

                 ra8_psa_sign_hash(k,
                                   k_ra8_psa_alg_ecdsa_sha_256,
                                   nullptr,
                                   sizeof(hash),
                                   sig,
                                   sizeof(sig),
                                   &sl));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,

                 ra8_psa_sign_hash(k,
                                   k_ra8_psa_alg_ecdsa_sha_256,
                                   hash,
                                   sizeof(hash),
                                   nullptr,
                                   sizeof(sig),
                                   &sl));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,

                 ra8_psa_sign_hash(k,
                                   k_ra8_psa_alg_ecdsa_sha_256,
                                   hash,
                                   sizeof(hash),
                                   sig,
                                   sizeof(sig),
                                   nullptr));
  (void)ra8_psa_key_destroy(k);
  teardown();
  TEST_END("psa MC/DC: sign_hash (hash||sig||sig_len) NULL triple");
}

/**
 * @test test_mcdc_verify_hash_arg_pair
 * @par MC/DC:
 * Decision: `if ((hash == NULL) || (sig == NULL))` (2 conditions,
 * libs/ra8_psa_crypto/src/ra8_psa_crypto.c line 834). N+1 = 3 vectors for N=2.
 */
static void test_mcdc_verify_hash_arg_pair(void)
{
  TEST_BEGIN("psa MC/DC: verify_hash (hash||sig) NULL pair");
  prep_init();
  const ra8_psa_key_attr_t attr = {
    .type  = k_ra8_psa_key_type_ecc_p256_priv,
    .alg   = k_ra8_psa_alg_ecdsa_sha_256,
    .usage = (ra8_psa_key_usage_t)(k_ra8_psa_usage_sign | k_ra8_psa_usage_verify)};
  ra8_psa_key_t k = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_key_import(&k, &attr, k_test_ecdsa_key, sizeof(k_test_ecdsa_key)));
  uint8_t hash[k_ra8_psa_sha256_len] = {};
  uint8_t sig[k_ra8_psa_sha256_len]  = {};
  size_t  sl                         = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,

    ra8_psa_sign_hash(k, k_ra8_psa_alg_ecdsa_sha_256, hash, sizeof(hash), sig, sizeof(sig), &sl));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_verify_hash(k, k_ra8_psa_alg_ecdsa_sha_256, hash, sizeof(hash), sig, sl));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_psa_verify_hash(k, k_ra8_psa_alg_ecdsa_sha_256, nullptr, sizeof(hash), sig, sl));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_psa_verify_hash(k, k_ra8_psa_alg_ecdsa_sha_256, hash, sizeof(hash), nullptr, sl));
  (void)ra8_psa_key_destroy(k);
  teardown();
  TEST_END("psa MC/DC: verify_hash (hash||sig) NULL pair");
}

/* ================================================================== */
/* Error-leg coverage (single-condition guards) */
/* ================================================================== */
/*
 * The cases below drive the single-condition error returns that the
 * happy-path and MC/DC vectors above never reach: the deinit pool
 * scrub, the not-initialized guards on every entry point, the
 * per-operation handle / usage / size guards, and the two emu-body
 * ``k_ra8_err_invalid_size`` legs (oversized plaintext / undersized
 * output). None introduces a new compound-decision obligation.
 */

/**
 * @enum ra8_psa_errleg_const_t
 * @brief Magic-number-free inputs for the error-leg coverage tests.
 */
typedef enum : uint16_t {
  k_psa_leg_big_plain  = 257U, /**< Plaintext > fake keystream scratch (256).     */
  k_psa_leg_wrong_hash = 16U,  /**< hash_len != k_ra8_psa_sha256_len.             */
  k_psa_leg_short_sig  = 16U,  /**< sig_len != k_ra8_psa_sha256_len (fake guard). */
  k_psa_leg_small_cap  = 2U,   /**< out_cap below the recovered plaintext.        */
  k_psa_leg_extra      = 4U,   /**< Spare bytes past the AEAD tag.                */
  k_psa_leg_cipher_len = 32U,  /**< cipher_len -> plain_len 16.                   */
} ra8_psa_errleg_const_t;

/** @brief Helper: import a P-256 key with the requested usage mask. */
static ra8_psa_key_t errleg_import_ecc_key(ra8_psa_key_type_t type, ra8_psa_key_usage_t usage)
{
  const ra8_psa_key_attr_t attr = {
    .type  = type,
    .alg   = k_ra8_psa_alg_ecdsa_sha_256,
    .usage = usage,
  };
  ra8_psa_key_t k = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_psa_key_import(&k, &attr, k_test_ecdsa_key, sizeof(k_test_ecdsa_key)));
  return k;
}

/**
 * @par MC/DC:
 * Single-condition guard `if (s_key_pool[i].in_use)` in
 * ``ra8_psa_crypto_deinit``. Drives the true leg (the pool-scrub block)
 * that the other tests miss because they destroy every key before
 * deinit. No compound decision: one condition, no independent-influence
 * obligation.
 */
static void test_deinit_frees_live_key(void)
{
  TEST_BEGIN("psa deinit frees a still-imported key");
  prep_init();
  ra8_psa_key_t k = mcdc_import_aes_key(k_ra8_psa_usage_encrypt);
  TEST_ASSERT_NOT_NULL(k);
  /* Deinit while the slot is still in_use: exercises the scrub branch. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_psa_crypto_deinit());
  /* Slot was freed: the stale handle is no longer in_use. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_psa_crypto_init());
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_psa_key_destroy(k));
  teardown();
  TEST_END("psa deinit frees a still-imported key");
}

/**
 * @par MC/DC:
 * Single-condition guard `if (!s_initialized)` in ``ra8_psa_key_destroy``.
 * The init guard fires before the handle validation, so a non-pool
 * pointer still returns not-initialized. No compound decision.
 */
static void test_destroy_uninitialized(void)
{
  TEST_BEGIN("psa destroy without init");
  (void)ra8_psa_crypto_deinit();
  uint8_t fake = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_psa_key_destroy((ra8_psa_key_t)&fake));
  TEST_END("psa destroy without init");
}

/**
 * @par MC/DC:
 * Single-condition guard `if (!s_initialized)` in ``ra8_psa_hash_compute``,
 * reached with valid out / out_len / alg so the init guard is the leg
 * under test. No compound decision.
 */
static void test_hash_uninitialized(void)
{
  TEST_BEGIN("psa hash_compute without init");
  (void)ra8_psa_crypto_deinit();
  uint8_t       out[k_ra8_psa_sha256_len];
  size_t        ol  = 0U;
  const uint8_t inp = (uint8_t)'a';
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_psa_hash_compute(k_ra8_psa_alg_sha_256,
                                      &inp,
                                      (size_t)k_psa_mcdc_short_input,
                                      out,
                                      sizeof(out),
                                      &ol));
  TEST_END("psa hash_compute without init");
}

/**
 * @par MC/DC:
 * Single-condition guards in ``ra8_psa_sign_hash``, one vector each:
 * `!s_initialized`, `!internal_handle_valid(handle)`, and
 * `(usage & k_ra8_psa_usage_sign) == 0`. Each is a lone condition with
 * no independent-influence obligation; the `hash||sig||sig_len` and alg
 * guards they transit are covered by the dedicated vectors above. The
 * two size guards live in test_sign_error_size_legs.
 */
static void test_sign_error_legs(void)
{
  TEST_BEGIN("psa sign_hash error legs (init/handle/usage)");
  uint8_t hash[k_ra8_psa_sha256_len] = {};
  uint8_t sig[k_ra8_psa_max_sig_bytes];
  size_t  sl = 0U;

  /* not initialized */
  (void)ra8_psa_crypto_deinit();
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_psa_sign_hash((ra8_psa_key_t)0x1U,
                                   k_ra8_psa_alg_ecdsa_sha_256,
                                   hash,
                                   sizeof(hash),
                                   sig,
                                   sizeof(sig),
                                   &sl));

  prep_init();
  /* handle not from the pool */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_psa_sign_hash(nullptr,
                                   k_ra8_psa_alg_ecdsa_sha_256,
                                   hash,
                                   sizeof(hash),
                                   sig,
                                   sizeof(sig),
                                   &sl));

  /* valid handle but missing sign usage */
  ra8_psa_key_t kv =
    errleg_import_ecc_key(k_ra8_psa_key_type_ecc_p256_priv, k_ra8_psa_usage_verify);
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_psa_sign_hash(kv, k_ra8_psa_alg_ecdsa_sha_256, hash, sizeof(hash), sig, sizeof(sig), &sl));

  (void)ra8_psa_key_destroy(kv);
  teardown();
  TEST_END("psa sign_hash error legs (init/handle/usage)");
}

/**
 * @par MC/DC:
 * Single-condition size guards in ``ra8_psa_sign_hash``, one vector
 * each: `hash_len != k_ra8_psa_sha256_len` and
 * `sig_cap < k_ra8_psa_sha256_len`. Each is a lone condition with no
 * independent-influence obligation; the init / handle / usage guards
 * they transit are covered by test_sign_error_legs.
 */
static void test_sign_error_size_legs(void)
{
  TEST_BEGIN("psa sign_hash error legs (hash_len/sig_cap sizes)");
  prep_init();
  uint8_t hash[k_ra8_psa_sha256_len] = {};
  uint8_t sig[k_ra8_psa_max_sig_bytes];
  size_t  sl = 0U;

  /* sign-capable key for the size legs */
  ra8_psa_key_t ks = errleg_import_ecc_key(k_ra8_psa_key_type_ecc_p256_priv, k_ra8_psa_usage_sign);
  /* hash_len != 32 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_psa_sign_hash(ks,
                                   k_ra8_psa_alg_ecdsa_sha_256,
                                   hash,
                                   (size_t)k_psa_leg_wrong_hash,
                                   sig,
                                   sizeof(sig),
                                   &sl));
  /* sig_cap < 32 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_psa_sign_hash(ks,
                                   k_ra8_psa_alg_ecdsa_sha_256,
                                   hash,
                                   sizeof(hash),
                                   sig,
                                   (size_t)k_psa_leg_small_cap,
                                   &sl));

  (void)ra8_psa_key_destroy(ks);
  teardown();
  TEST_END("psa sign_hash error legs (hash_len/sig_cap sizes)");
}

/**
 * @par MC/DC:
 * Single-condition guards in ``ra8_psa_verify_hash``, one vector each:
 * `!s_initialized`, `!internal_handle_valid(handle)`,
 * `alg != k_ra8_psa_alg_ecdsa_sha_256`,
 * `(usage & k_ra8_psa_usage_verify) == 0`, `hash_len != k_ra8_psa_sha256_len`,
 * and the fake `sig_len != k_ra8_psa_sha256_len` tag-length guard. Each is a
 * lone condition; the `hash||sig` guard they transit is covered above.
 */
static void test_verify_error_legs(void)
{
  TEST_BEGIN("psa verify_hash error legs (init/handle/alg/usage/sizes)");
  uint8_t hash[k_ra8_psa_sha256_len] = {};
  uint8_t sig[k_ra8_psa_sha256_len]  = {};

  /* not initialized */
  (void)ra8_psa_crypto_deinit();
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_psa_verify_hash((ra8_psa_key_t)0x1U,
                                     k_ra8_psa_alg_ecdsa_sha_256,
                                     hash,
                                     sizeof(hash),
                                     sig,
                                     sizeof(sig)));

  prep_init();
  /* handle not from the pool */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_psa_verify_hash(nullptr,
                                     k_ra8_psa_alg_ecdsa_sha_256,
                                     hash,
                                     sizeof(hash),
                                     sig,
                                     sizeof(sig)));

  /* verify-capable key: alg / hash_len / sig_len legs */
  ra8_psa_key_t kv = errleg_import_ecc_key(k_ra8_psa_key_type_ecc_p256_pub, k_ra8_psa_usage_verify);
  /* wrong algorithm */
  TEST_ASSERT_EQ(
    k_ra8_err_not_supported,
    ra8_psa_verify_hash(kv, k_ra8_psa_alg_aes_gcm, hash, sizeof(hash), sig, sizeof(sig)));
  /* hash_len != 32 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_psa_verify_hash(kv,
                                     k_ra8_psa_alg_ecdsa_sha_256,
                                     hash,
                                     (size_t)k_psa_leg_wrong_hash,
                                     sig,
                                     sizeof(sig)));
  /* sig_len != 32 (fake tag-length guard) */
  TEST_ASSERT_EQ(k_ra8_err_crc_mismatch,
                 ra8_psa_verify_hash(kv,
                                     k_ra8_psa_alg_ecdsa_sha_256,
                                     hash,
                                     sizeof(hash),
                                     sig,
                                     (size_t)k_psa_leg_short_sig));

  /* valid handle but missing verify usage */
  ra8_psa_key_t ks = errleg_import_ecc_key(k_ra8_psa_key_type_ecc_p256_priv, k_ra8_psa_usage_sign);
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_psa_verify_hash(ks, k_ra8_psa_alg_ecdsa_sha_256, hash, sizeof(hash), sig, sizeof(sig)));

  (void)ra8_psa_key_destroy(kv);
  (void)ra8_psa_key_destroy(ks);
  teardown();
  TEST_END("psa verify_hash error legs (init/handle/alg/usage/sizes)");
}

/**
 * @par MC/DC:
 * Two single-condition legs: `!s_initialized` in
 * ``internal_aead_encrypt_check`` and `if (ser != k_ra8_ok)` in
 * ``ra8_psa_aead_encrypt`` (the fake body returns invalid_size when the
 * plaintext exceeds the 256-byte keystream scratch). Neither is a
 * compound decision.
 */
static void test_aead_encrypt_error_legs(void)
{
  TEST_BEGIN("psa aead_encrypt error legs (not-init, oversized plaintext)");
  uint8_t       nonce[k_ra8_psa_gcm_nonce_len]                     = {};
  uint8_t       small_out[k_ra8_psa_gcm_tag_len + k_psa_leg_extra] = {};
  size_t        ol                                                 = 0U;
  const uint8_t plain2[2]                                          = {'h', 'i'};

  /* not initialized */
  (void)ra8_psa_crypto_deinit();
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_psa_aead_encrypt((ra8_psa_key_t)0x1U,
                                      k_ra8_psa_alg_aes_gcm,
                                      nonce,
                                      sizeof(nonce),
                                      nullptr,
                                      0U,
                                      plain2,
                                      sizeof(plain2),
                                      small_out,
                                      sizeof(small_out),
                                      &ol));

  prep_init();
  ra8_psa_key_t k = mcdc_import_aes_key(k_ra8_psa_usage_encrypt);
  TEST_ASSERT_NOT_NULL(k);
  /* plaintext exceeds the fake keystream scratch (256 bytes) */
  uint8_t big_plain[k_psa_leg_big_plain]                       = {};
  uint8_t big_out[k_psa_leg_big_plain + k_ra8_psa_gcm_tag_len] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_psa_aead_encrypt(k,
                                      k_ra8_psa_alg_aes_gcm,
                                      nonce,
                                      sizeof(nonce),
                                      nullptr,
                                      0U,
                                      big_plain,
                                      sizeof(big_plain),
                                      big_out,
                                      sizeof(big_out),
                                      &ol));
  (void)ra8_psa_key_destroy(k);
  teardown();
  TEST_END("psa aead_encrypt error legs (not-init, oversized plaintext)");
}

/**
 * @par MC/DC:
 * Two single-condition legs in ``internal_aead_decrypt_check``:
 * `!s_initialized` and `out_cap < plain_len`. The size leg is reached
 * with a valid handle / alg / usage and a non-NULL out buffer whose
 * capacity is below the recovered plaintext length. Neither is a
 * compound decision.
 */
static void test_aead_decrypt_error_legs(void)
{
  TEST_BEGIN("psa aead_decrypt error legs (not-init, out_cap < plain_len)");
  uint8_t nonce[k_ra8_psa_gcm_nonce_len] = {};
  uint8_t cipher[k_psa_leg_cipher_len]   = {};
  uint8_t small_out[k_psa_leg_small_cap] = {};
  size_t  ol                             = 0U;

  /* not initialized */
  (void)ra8_psa_crypto_deinit();
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_psa_aead_decrypt((ra8_psa_key_t)0x1U,
                                      k_ra8_psa_alg_aes_gcm,
                                      nonce,
                                      sizeof(nonce),
                                      nullptr,
                                      0U,
                                      cipher,
                                      sizeof(cipher),
                                      small_out,
                                      sizeof(small_out),
                                      &ol));

  prep_init();
  ra8_psa_key_t k =
    mcdc_import_aes_key((ra8_psa_key_usage_t)(k_ra8_psa_usage_encrypt | k_ra8_psa_usage_decrypt));
  TEST_ASSERT_NOT_NULL(k);
  /* out buffer too small for the recovered plaintext (16 bytes) */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_psa_aead_decrypt(k,
                                      k_ra8_psa_alg_aes_gcm,
                                      nonce,
                                      sizeof(nonce),
                                      nullptr,
                                      0U,
                                      cipher,
                                      sizeof(cipher),
                                      small_out,
                                      sizeof(small_out),
                                      &ol));
  (void)ra8_psa_key_destroy(k);
  teardown();
  TEST_END("psa aead_decrypt error legs (not-init, out_cap < plain_len)");
}

int32_t main(void)
{
  test_mcdc_handle_valid_range_check();
  test_mcdc_key_import_arg_quad();
  test_mcdc_hash_compute_out_pair();
  test_mcdc_hash_compute_input_and_len();
  test_mcdc_sign_hash_arg_triple();
  test_mcdc_verify_hash_arg_pair();
  test_deinit_frees_live_key();
  test_destroy_uninitialized();
  test_hash_uninitialized();
  test_sign_error_legs();
  test_sign_error_size_legs();
  test_verify_error_legs();
  test_aead_encrypt_error_legs();
  test_aead_decrypt_error_legs();
  (void)fprintf(stderr, "[OK  ] test_ra8_psa_crypto_guards.c\n");
  return 0;
}
