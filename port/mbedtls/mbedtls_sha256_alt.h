/**
 * @file port/mbedtls/mbedtls_sha256_alt.h
 * @brief SHA256_ALT context override for Mbed TLS on RA8D2 RSIP-E50D
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Mbed TLS exposes the same ALT contract for SHA-256 as it does for
 * AES: defining ``MBEDTLS_SHA256_ALT`` in the project config makes
 * the upstream ``mbedtls/sha256.h`` defer the
 * ``mbedtls_sha256_context`` definition + the standard
 * ``mbedtls_sha256_init`` / ``..._free`` / ``..._starts`` /
 * ``..._update`` / ``..._finish`` entry points to this header / its
 * matching source file.
 *
 * On the RA8D2 the underlying primitive is the RSIP-E50D HASH core,
 * driven by the streaming ``ra8_rsip_sha256_init`` / ``..._update`` /
 * ``..._final`` API. This shim wraps that streaming context plus a
 * per-instance "started" flag so the public Mbed TLS surface
 * matches the upstream software contract bit-for-bit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_rsip.h"

/**
 * @enum mbedtls_sha256_alt_constants_t
 * @brief Compile-time constants for the Mbed TLS <-> RSIP SHA-256 bridge.
 *
 * @details
 * Block / digest sizes follow FIPS PUB 180-4.
 */
typedef enum : uint16_t {
  k_mbedtls_sha256_alt_block_bytes  = 64U, /**< SHA-256 block size. */
  k_mbedtls_sha256_alt_digest_bytes = 32U, /**< Digest length.      */
} mbedtls_sha256_alt_constants_t;

/**
 * @struct mbedtls_sha256_context
 * @brief Mbed TLS ``MBEDTLS_SHA256_ALT`` context backed by RSIP-E50D.
 *
 * @details
 * The struct embeds the streaming ``ra8_rsip_sha256_ctx_t`` directly
 * so every Mbed TLS update / finish call costs at most one HAL call.
 * The ``is224`` field is part of the public Mbed TLS surface but
 * unused in this shim -- only SHA-256 is wired through hardware;
 * SHA-224 callers must stay on the software path (the project
 * config disables SHA-224).
 *
 * @invariant ``started == 0`` between ``free`` and ``starts``.
 * @invariant ``inner.initialized == started`` after every public call.
 *
 * @note The Mbed TLS API treats ``mbedtls_sha256_context`` as opaque,
 *       so the layout is owned by this port.
 *
 * @code{.c}
 * mbedtls_sha256_context ctx;
 * mbedtls_sha256_init(&ctx);
 * mbedtls_sha256_starts(&ctx, 0);
 * mbedtls_sha256_update(&ctx, msg, msg_len);
 * mbedtls_sha256_finish(&ctx, digest);
 * mbedtls_sha256_free(&ctx);
 * @endcode
 *
 * @see ra8_rsip_sha256_init
 * @see ra8_rsip_sha256_update
 * @see ra8_rsip_sha256_final
 * @since 0.1.0
 */
typedef struct mbedtls_sha256_context {
  ra8_rsip_sha256_ctx_t inner; /**< Streaming RSIP HAL context. */
  // cppcheck-suppress unusedStructMember
  uint8_t started; /**< 1 between ``starts`` and ``finish``. */
  // cppcheck-suppress unusedStructMember
  uint8_t is224; /**< 0 -- SHA-256 only on this shim. */
  // cppcheck-suppress unusedStructMember
  uint8_t pad[2]; /**< Reserved for 4-byte alignment. */
} mbedtls_sha256_context;

#ifdef __cplusplus
}
#endif
