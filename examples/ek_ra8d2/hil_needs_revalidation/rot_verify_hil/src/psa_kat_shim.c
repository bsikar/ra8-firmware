/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/rot_verify_hil/src/psa_kat_shim.c
 * @brief TEST-ONLY platform shims for the tf-psa-crypto software backend.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The vendored tf-psa-crypto port config enables MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG,
 * so psa_crypto_init needs an external RNG entry point, and (if MBEDTLS_HAVE_TIME
 * is set) an mbedtls_ms_time. This app is a known-answer test whose operations --
 * SHA-256, AES-128-GCM with a fixed nonce, and ECDSA-P256 *verify* -- draw NO
 * randomness, so a deterministic RNG here does not affect the KAT result. It
 * exists only so psa_crypto_init succeeds.
 *
 * @warning NOT cryptographically secure. This deterministic RNG must never ship
 *          in a real image; a production build needs a real entropy source
 *          (see libs/ra8_hal/src/ra8_rsip.c -- the RSIP TRNG needs an FSP port).
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "mbedtls/platform_util.h"
#include "psa/crypto.h"

/** @brief LCG multiplier (Numerical Recipes) for the deterministic test RNG. */
typedef enum : uint32_t {
  k_shim_lcg_mul   = 1664525U,    /**< Shim lcg mul.   */
  k_shim_lcg_add   = 1013904223U, /**< Shim lcg add.   */
  k_shim_seed      = 0x1234ABCDU, /**< Shim seed.      */
  k_shim_top_shift = 24U,         /**< Shim top shift. */
} shim_const_t;

psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t* context,
                                             uint8_t*                               output,
                                             size_t                                 output_size,
                                             size_t*                                output_length)
{
  (void)context;
  static uint32_t s = (uint32_t)k_shim_seed;
  for (size_t i = 0U; i < output_size; ++i) {
    s         = (s * (uint32_t)k_shim_lcg_mul) + (uint32_t)k_shim_lcg_add;
    output[i] = (uint8_t)(s >> (uint32_t)k_shim_top_shift);
  }
  *output_length = output_size;
  return PSA_SUCCESS;
}

#if defined(MBEDTLS_HAVE_TIME) && defined(MBEDTLS_PLATFORM_MS_TIME_ALT)
mbedtls_ms_time_t mbedtls_ms_time(void)
{
  return 0;
}
#endif
