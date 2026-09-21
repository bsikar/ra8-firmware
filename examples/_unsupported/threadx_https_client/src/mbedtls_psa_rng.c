/**
 * @file examples/_unsupported/threadx_https_client/src/mbedtls_psa_rng.c
 * @brief Mbed TLS PSA random-source adapter for the HTTPS client demo.
 * @par Tag
 * [Ring 6 / APP] {World: S}
 * @details
 * Implements the Mbed TLS external PSA random callback using the RA8D2 RSIP
 * true-random generator. Keeping the vendor callback in its own translation
 * unit isolates the TLS adaptation boundary from the ThreadX and HTTP session
 * orchestration in main.c.
 * @author Brighton Sikarskie
 * @date 2026-08-27
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_rsip.h"

#ifndef RA8_OFF_TARGET
#include "psa/crypto.h"

/**
 * @brief PSA external RNG hook -- feeds RSIP TRNG straight into PSA.
 * @details
 * Mbed TLS 4.x removed @ref MBEDTLS_ENTROPY_C / @ref MBEDTLS_CTR_DRBG_C
 * from the standard build path. With @ref MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
 * the SSL layer pulls random bytes from this callback (via
 * @ref psa_generate_random) instead of running an in-tree DRBG.
 * @param[in,out] context PSA's per-process external RNG context (unused).
 * @param[out] output Output buffer.
 * @param[in] output_size Bytes requested.
 * @param[out] output_length Bytes actually produced.
 * @return PSA status describing whether random generation succeeded.
 * @retval PSA_SUCCESS Exactly @p output_size random bytes were produced.
 * @retval PSA_ERROR_INVALID_ARGUMENT An output pointer is null.
 * @retval PSA_ERROR_HARDWARE_FAILURE The RSIP TRNG returned an error.
 * @pre RSIP has been initialized by ``ra8_rsip_init()``.
 * @pre @p output and @p output_length are non-null caller-owned storage.
 * @post On success @p output contains @p output_size bytes from the RSIP TRNG.
 * @post On success @p output_length equals @p output_size.
 * @post An invalid pointer emits an error log before returning
 * @ref PSA_ERROR_INVALID_ARGUMENT; no output storage is modified.
 * @note Not thread-safe; callers must serialize across threads.
 * @see psa_generate_random
 * @see ra8_rsip_trng_read
 * @since 0.1.0
 */
psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t* context,
                                             uint8_t*                               output,
                                             size_t                                 output_size,
                                             size_t*                                output_length)
{
  (void)context;
  RA8_CHECK_NULL_PTR_RETURN(output,
                            PSA_ERROR_INVALID_ARGUMENT,
                            "PSA_RNG",
                            "output must not be nullptr");
  RA8_CHECK_NULL_PTR_RETURN(output_length,
                            PSA_ERROR_INVALID_ARGUMENT,
                            "PSA_RNG",
                            "output_length must not be nullptr");
  ra8_err_t err = ra8_rsip_trng_read((uint8_t*)output, (uint32_t)output_size);
  if (err != k_ra8_ok) {
    return PSA_ERROR_HARDWARE_FAILURE;
  }
  *output_length = output_size;
  return PSA_SUCCESS;
}
#endif
