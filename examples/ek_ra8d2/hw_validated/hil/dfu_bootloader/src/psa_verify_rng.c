/**
 * @file examples/ek_ra8d2/hw_validated/hil/dfu_bootloader/src/psa_verify_rng.c
 * @brief Fail-closed PSA external-RNG hook for the verify-only bootloader.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The vendored tf-psa-crypto port sets ``MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG``, so the
 * PSA core needs an ``mbedtls_psa_external_get_random`` symbol at link time. This
 * immutable bootloader performs exactly ONE cryptographic operation: ECDSA-P256
 * signature *verification* over the public image body, the public provisioned root
 * key, and the public trailer (via ``ra8_rot_verify_image`` from the
 * ``RA8_ENABLE_ROOT_OF_TRUST`` launch gate). That operation is mathematically
 * deterministic and draws NO randomness, and the bootloader never signs, generates
 * a key, or derives a secret -- so there is no secret here for randomness to
 * protect.
 *
 * Rather than link the deterministic placeholder RNG the KAT apps use (a latent
 * catastrophe if this image were ever repurposed to sign or keygen), this hook
 * FAILS CLOSED: any request for entropy returns
 * ``PSA_ERROR_INSUFFICIENT_ENTROPY``. On the verify-only boot path it is never
 * called, so the bootloader authenticates and launches the genuine image normally.
 * A call would mean an unexpected code path drew randomness -- and for an immutable
 * bootloader, failing loudly (rejecting the launch) is the correct, safe response,
 * never emitting predictable bytes that could masquerade as entropy.
 *
 * @warning If a future bootloader feature genuinely needs entropy (a
 *          challenge-response, a signing step, a key exchange), wire a real
 *          hardware TRNG here. Do NOT relax this to a deterministic generator.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "mbedtls/platform_util.h"
#include "psa/crypto.h"

psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t* context,
                                             uint8_t*                               output,
                                             size_t                                 output_size,
                                             size_t*                                output_length)
{
  (void)context;
  (void)output;
  (void)output_size;
  /* Verify-only bootloader: no operation on the boot path draws randomness. A
   * request here is an unexpected path -- fail closed rather than emit predictable
   * bytes. The launch gate treats a crypto failure as default-deny (no launch). */
  if (output_length != nullptr) {
    *output_length = 0U;
  }
  return PSA_ERROR_INSUFFICIENT_ENTROPY;
}

#if defined(MBEDTLS_HAVE_TIME) && defined(MBEDTLS_PLATFORM_MS_TIME_ALT)
/**
 * @brief Monotonic-time stub -- verification is time-independent (no cert expiry).
 * @return Always 0; the bootloader does not evaluate any time-bounded credential.
 */
mbedtls_ms_time_t mbedtls_ms_time(void)
{
  return 0;
}
#endif
