/**
 * @file examples/ek_ra8d2/hw_pending/secure_boot_ns_hil/src/psa_verify_rng.c
 * @brief Fail-closed PSA external-RNG hook for the verify-only secure boot (#172).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The vendored tf-psa-crypto port sets ``MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG``, so the
 * PSA core needs an ``mbedtls_psa_external_get_random`` symbol at link time. This
 * app performs exactly ONE cryptographic operation on the boot path: ECDSA-P256
 * signature *verification* of the Non-Secure image (via ``ra8_rot_verify_image``
 * inside ``ra8_tz_secure_boot_jump_ns``). That operation is mathematically
 * deterministic and draws NO randomness, and the app never signs, generates a
 * key, or derives a secret -- there is no secret here for randomness to protect.
 *
 * Rather than link a deterministic placeholder RNG (a latent catastrophe if this
 * image were ever repurposed to sign or keygen), this hook FAILS CLOSED: any
 * request for entropy returns ``PSA_ERROR_INSUFFICIENT_ENTROPY``. On the
 * verify-only boot path it is never called, so the secure boot authenticates and
 * BLXNS-es normally. A call would mean an unexpected path drew randomness -- and
 * failing loudly (denying the boot) is the correct, safe response, never emitting
 * predictable bytes that could masquerade as entropy.
 *
 * @warning If a future feature genuinely needs entropy (a challenge-response, a
 *          signing step, a key exchange), wire a real hardware TRNG here. Do NOT
 *          relax this to a deterministic generator.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "mbedtls/platform_util.h"
#include "psa/crypto.h"

/**
 * @brief PSA external-RNG hook -- fails closed (verification draws no entropy).
 * @param[in]  context       Unused external-RNG context.
 * @param[out] output        Unused entropy output buffer.
 * @param[in]  output_size   Unused requested byte count.
 * @param[out] output_length Set to 0 when non-NULL.
 * @return Always ``PSA_ERROR_INSUFFICIENT_ENTROPY``.
 */
psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t* context,
                                             uint8_t*                               output,
                                             size_t                                 output_size,
                                             size_t*                                output_length)
{
  (void)context;
  (void)output;
  (void)output_size;
  /* Verify-only boot: no operation on the boot path draws randomness. A request
   * here is an unexpected path -- fail closed rather than emit predictable
   * bytes. The gate treats a crypto failure as default-deny (no BLXNS). */
  if (output_length != nullptr) {
    *output_length = 0U;
  }
  return PSA_ERROR_INSUFFICIENT_ENTROPY;
}

#if defined(MBEDTLS_HAVE_TIME) && defined(MBEDTLS_PLATFORM_MS_TIME_ALT)
/**
 * @brief Monotonic-time stub -- verification is time-independent (no cert expiry).
 * @return Always 0; the secure boot evaluates no time-bounded credential.
 */
mbedtls_ms_time_t mbedtls_ms_time(void)
{
  return 0;
}
#endif
