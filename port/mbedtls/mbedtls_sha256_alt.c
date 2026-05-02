/**
 * @file port/mbedtls/mbedtls_sha256_alt.c
 * @brief SHA-256 ALT implementation for Mbed TLS, routed through ra_rsip
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Provides the standard Mbed TLS SHA-256 public surface (init / free
 * / starts / update / finish) on top of ``ra_rsip_sha256_init`` /
 * ``..._update`` / ``..._final``. When the project config defines
 * both ``MBEDTLS_SHA256_ALT`` and ``MBEDTLS_SHA256_C``, the upstream
 * ``mbedtls/sha256.h`` becomes a thin forward-declaration shell --
 * this TU supplies the definitions.
 *
 * The shim is SHA-256 only; the project config disables SHA-224 so
 * the ``is224`` parameter of ``mbedtls_sha256_starts`` is rejected
 * if non-zero. The internal block primitive
 * ``mbedtls_internal_sha256_process`` is also routed here for
 * completeness so callers that hand-feed blocks (e.g. HKDF / HMAC
 * test vectors) keep working.
 *
 * Error mapping:
 *
 *   - ``k_ra_err_null_ptr`` / ``k_ra_err_invalid_arg`` ->
 *     ``MBEDTLS_ERR_SHA256_BAD_INPUT_DATA``.
 *   - ``k_ra_err_hw_*`` -> ``MBEDTLS_ERR_PLATFORM_HW_ACCEL_FAILED``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

/* NOLINTBEGIN(readability-magic-numbers, cppcoreguidelines-avoid-magic-numbers) */
/* The Mbed TLS SHA-256 public surface is defined against fixed,
 * spec-mandated block (64 bytes) and digest (32 bytes) lengths
 * (FIPS PUB 180-4). They appear in this file as bare literals so
 * call sites match the upstream public headers exactly. */

#include "mbedtls_sha256_alt.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mbedtls/error.h"
#include "mbedtls/sha256.h"
#include "ra_err.h"
#include "ra_rsip.h"

/**
 * @brief Translate ``ra_err_t`` failures into Mbed TLS SHA-256 error codes.
 *
 * @param[in] err HAL error code returned by ``ra_rsip_*``.
 *
 * @return ``0`` on ``k_ra_ok``, otherwise a negative error code
 *         suitable for direct ``return``.
 *
 * @pre None.
 *
 * @since 0.1.0
 *
 * @details See implementation for details.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 */
static int priv_map_err(ra_err_t err)
{
  if (err == k_ra_ok) {
    return 0;
  }
  if (err == k_ra_err_null_ptr || err == k_ra_err_invalid_arg || err == k_ra_err_invalid_state) {
    return MBEDTLS_ERR_SHA256_BAD_INPUT_DATA;
  }
  return MBEDTLS_ERR_PLATFORM_HW_ACCEL_FAILED;
}

/**
 * @brief Mbed TLS ``mbedtls_sha256_init`` ALT replacement.
 *
 * @param[in,out] ctx Context allocated by the caller.
 *
 * @post ``ctx`` is fully zeroed; ``starts`` is required before update.
 *
 * @since 0.1.0
 *
 * @details See implementation for details.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
void mbedtls_sha256_init(mbedtls_sha256_context* ctx)
{
  if (ctx == NULL) {
    return;
  }
  (void)memset((void*)ctx, 0, sizeof(*ctx));
}

/**
 * @brief Mbed TLS ``mbedtls_sha256_free`` ALT replacement.
 *
 * @param[in,out] ctx Context to scrub. NULL is a no-op.
 *
 * @post ``ctx`` is fully zeroed if non-NULL.
 *
 * @since 0.1.0
 *
 * @details See implementation for details.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
void mbedtls_sha256_free(mbedtls_sha256_context* ctx)
{
  if (ctx == NULL) {
    return;
  }
  (void)memset((void*)ctx, 0, sizeof(*ctx));
}

/**
 * @brief Mbed TLS ``mbedtls_sha256_clone`` ALT replacement.
 *
 * @details
 * Implements the standard public clone: deep-copy the source context
 * into the destination so the two streams diverge from this point
 * onward. The RSIP HAL context is plain old data (a buffer + a
 * ``used`` cursor + a flag) so a memcpy is sufficient.
 *
 * @param[out] dst Destination context.
 * @param[in]  src Source context to copy.
 *
 * @pre Both pointers are non-NULL.
 * @post ``*dst`` is a value-copy of ``*src``.
 *
 * @since 0.1.0
 *
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @note Not thread-safe unless documented otherwise.
 */
void mbedtls_sha256_clone(mbedtls_sha256_context* dst, const mbedtls_sha256_context* src)
{
  if (dst == NULL || src == NULL) {
    return;
  }
  *dst = *src;
}

/**
 * @brief Mbed TLS ``mbedtls_sha256_starts`` ALT replacement.
 *
 * @param[in,out] ctx   Context primed by ``mbedtls_sha256_init``.
 * @param[in]     is224 0 for SHA-256, non-zero for SHA-224 (rejected).
 *
 * @return ``0`` on success, ``MBEDTLS_ERR_SHA256_BAD_INPUT_DATA`` on
 *         a SHA-224 request.
 *
 * @pre ``ctx != NULL``.
 *
 * @since 0.1.0
 *
 * @details See implementation for details.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 */
int mbedtls_sha256_starts(mbedtls_sha256_context* ctx, int is224)
{
  if (ctx == NULL) {
    return MBEDTLS_ERR_SHA256_BAD_INPUT_DATA;
  }
  if (is224 != 0) {
    /* SHA-224 is not enabled in the project config, so reject. */
    return MBEDTLS_ERR_SHA256_BAD_INPUT_DATA;
  }
  ctx->is224   = 0U;
  ctx->started = 0U;
  ra_err_t err = ra_rsip_sha256_init(&ctx->inner);
  if (err != k_ra_ok) {
    return priv_map_err(err);
  }
  ctx->started = 1U;
  return 0;
}

/**
 * @brief Mbed TLS ``mbedtls_sha256_update`` ALT replacement.
 *
 * @param[in,out] ctx   Context with ``starts`` previously called.
 * @param[in]     input Bytes to absorb (may be NULL when ``ilen == 0``).
 * @param[in]     ilen  Number of bytes.
 *
 * @return ``0`` on success.
 *
 * @pre ``ctx != NULL``.
 *
 * @since 0.1.0
 *
 * @details See implementation for details.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 */
int mbedtls_sha256_update(mbedtls_sha256_context* ctx, const unsigned char* input, size_t ilen)
{
  if (ctx == NULL) {
    return MBEDTLS_ERR_SHA256_BAD_INPUT_DATA;
  }
  if (ctx->started == 0U) {
    return MBEDTLS_ERR_SHA256_BAD_INPUT_DATA;
  }
  if (ilen == 0U) {
    return 0;
  }
  if (input == NULL) {
    return MBEDTLS_ERR_SHA256_BAD_INPUT_DATA;
  }
  ra_err_t err = ra_rsip_sha256_update(&ctx->inner, (const uint8_t*)input, (uint32_t)ilen);
  return priv_map_err(err);
}

/**
 * @brief Mbed TLS ``mbedtls_sha256_finish`` ALT replacement.
 *
 * @param[in,out] ctx    Context populated by prior updates.
 * @param[out]    output 32-byte digest output buffer.
 *
 * @return ``0`` on success.
 *
 * @pre ``ctx != NULL`` and ``output != NULL``.
 *
 * @since 0.1.0
 *
 * @details See implementation for details.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 */
int mbedtls_sha256_finish(mbedtls_sha256_context* ctx, unsigned char* output)
{
  if (ctx == NULL || output == NULL) {
    return MBEDTLS_ERR_SHA256_BAD_INPUT_DATA;
  }
  if (ctx->started == 0U) {
    return MBEDTLS_ERR_SHA256_BAD_INPUT_DATA;
  }
  ra_err_t err = ra_rsip_sha256_final(&ctx->inner, (uint8_t*)output);
  ctx->started = 0U;
  return priv_map_err(err);
}

/**
 * @brief Mbed TLS ``mbedtls_internal_sha256_process`` ALT replacement.
 *
 * @details
 * Mbed TLS exposes the per-block hash compression to internal
 * users (HKDF / DRBG); we satisfy that contract by absorbing the
 * 64-byte block through the same streaming context the public API
 * uses. This keeps the running state consistent regardless of
 * which entry point the caller chose.
 *
 * @param[in,out] ctx  Context with ``starts`` previously called.
 * @param[in]     data 64-byte block.
 *
 * @return ``0`` on success.
 *
 * @pre ``ctx != NULL`` and ``data != NULL``.
 *
 * @since 0.1.0
 *
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 */
int mbedtls_internal_sha256_process(mbedtls_sha256_context* ctx, const unsigned char data[64])
{
  if (ctx == NULL || data == NULL) {
    return MBEDTLS_ERR_SHA256_BAD_INPUT_DATA;
  }
  if (ctx->started == 0U) {
    return MBEDTLS_ERR_SHA256_BAD_INPUT_DATA;
  }
  ra_err_t err = ra_rsip_sha256_update(&ctx->inner,
                                       (const uint8_t*)data,
                                       (uint32_t)k_mbedtls_sha256_alt_block_bytes);
  return priv_map_err(err);
}

/* NOLINTEND(readability-magic-numbers, cppcoreguidelines-avoid-magic-numbers) */
