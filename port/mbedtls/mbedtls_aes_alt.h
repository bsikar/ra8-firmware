/**
 * @file port/mbedtls/mbedtls_aes_alt.h
 * @brief AES_ALT context override for Mbed TLS on RA8D2 RSIP-E50D
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Mbed TLS exposes a documented "ALT" replacement pattern for every
 * legacy crypto primitive: define ``MBEDTLS_AES_ALT`` in the project
 * config and the upstream ``mbedtls/aes.h`` defers the structure
 * definition + every standard entry point (``mbedtls_aes_init``,
 * ``mbedtls_aes_free``, ``mbedtls_aes_setkey_enc``,
 * ``mbedtls_aes_setkey_dec``, ``mbedtls_aes_crypt_ecb``,
 * ``mbedtls_aes_crypt_cbc``, ``mbedtls_aes_crypt_ctr``) to this
 * header / matching source file. Mbed TLS only requires that the
 * struct + functions follow the public ``aes.h`` contract -- the
 * implementation is free to live anywhere.
 *
 * On the RA8D2 we satisfy that contract by storing a wrapped
 * ``ra8_rsip_key_handle_t`` inside the context and forwarding every
 * cipher invocation through ``ra8_rsip_aes_cipher`` (which drives the
 * RSIP-E50D AES core). The plaintext key is wrapped exactly once at
 * ``mbedtls_aes_setkey_enc`` / ``..._setkey_dec`` time via
 * ``ra8_rsip_aes128_install_plain`` (or 192 / 256), and the wrapped
 * blob lives only inside the context until ``mbedtls_aes_free``
 * scrubs it.
 *
 * ## Why a separate header
 *
 * The build configures ``port/mbedtls/`` BEFORE the vendored Mbed TLS
 * include directory so this file is what compilers see when the
 * upstream ``mbedtls/aes.h`` says ``#include "mbedtls_aes_alt.h"``
 * (the canonical hook). The struct lives here -- the upstream
 * ``aes.h`` then forward-declares the function prototypes against
 * this struct, and the matching ``.c`` provides the bodies.
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
 * @enum mbedtls_aes_alt_constants_t
 * @brief Compile-time constants for the Mbed TLS <-> RSIP AES bridge.
 *
 * @details
 * AES is a 128-bit block cipher (FIPS PUB 197). Mbed TLS expresses
 * key sizes in bits at the public API but its standard ``setkey_*``
 * entry points accept 128 / 192 / 256 only.
 */
typedef enum : uint16_t {
  k_mbedtls_aes_alt_block_bytes = 16U,  /**< AES block size in bytes.   */
  k_mbedtls_aes_alt_key128_bits = 128U, /**< AES-128 key length (bits). */
  k_mbedtls_aes_alt_key192_bits = 192U, /**< AES-192 key length (bits). */
  k_mbedtls_aes_alt_key256_bits = 256U, /**< AES-256 key length (bits). */
} mbedtls_aes_alt_constants_t;

/**
 * @struct mbedtls_aes_context
 * @brief Mbed TLS ``MBEDTLS_AES_ALT`` context backed by RSIP-E50D.
 *
 * @details
 * Replaces the upstream software ``mbedtls_aes_context`` (round-key
 * schedule + counter) with a wrapped-key handle that the RSIP engine
 * consumes directly. The context never holds plaintext key bytes:
 * the wrap happens inside the engine at ``setkey_*`` time and only
 * the handle is retained.
 *
 * @invariant ``key_bits`` is one of 128 / 192 / 256 once
 *            ``mbedtls_aes_setkey_enc`` returns success.
 * @invariant ``ready != 0`` only after a successful ``setkey_*``.
 *
 * @note The Mbed TLS API treats ``mbedtls_aes_context`` as opaque --
 *       callers never touch fields directly, so we are free to
 *       replace the schedule / counter layout with whatever the
 *       hardware needs.
 *
 * @code{.c}
 * mbedtls_aes_context ctx;
 * mbedtls_aes_init(&ctx);
 * mbedtls_aes_setkey_enc(&ctx, key, 128);
 * mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, len, iv, in, out);
 * mbedtls_aes_free(&ctx);
 * @endcode
 *
 * @see ra8_rsip_aes_cipher
 * @since 0.1.0
 */
typedef struct mbedtls_aes_context {
  ra8_rsip_key_handle_t key;       /**< Wrapped RSIP key handle.               */
  uint16_t             key_bits;  /**< 128 / 192 / 256 -- captured at setkey. */
  uint8_t              direction; /**< 0 = unset, 1 = enc, 2 = dec.           */
  uint8_t              ready;     /**< 1 if key was wrapped successfully.     */
} mbedtls_aes_context;

#ifdef __cplusplus
}
#endif
