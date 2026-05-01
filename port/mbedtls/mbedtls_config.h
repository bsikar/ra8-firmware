/**
 * @file port/mbedtls/mbedtls_config.h
 * @brief Project-wide Mbed TLS configuration for the RA8D2 firmware
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Mbed TLS reads its compile-time feature set from a single C
 * preprocessor file -- either ``include/mbedtls/mbedtls_config.h``
 * (the upstream default) or a project-supplied override pointed to
 * by ``MBEDTLS_CONFIG_FILE``. Our ``cmake/mbedtls.cmake`` defines
 * ``MBEDTLS_CONFIG_FILE`` to this path so every TU in the vendored
 * Mbed TLS tree picks up exactly the surface this firmware needs.
 *
 * ## Surface this config enables
 *
 *   - **TLS 1.2 + TLS 1.3** -- both versions, client side. The
 *     example app talks to ``www.example.com`` which negotiates
 *     1.3 happily; older HTTPS endpoints on the lab network may
 *     still need 1.2, so we keep both.
 *   - **AES-128-GCM cipher suite** plus AES-128-CBC for fallback.
 *   - **RSA + ECDHE** key exchange with the NIST curves the RSIP
 *     hardware can drive (P-256 / P-384). Static RSA is also
 *     enabled for sites that have not migrated to PFS.
 *   - **SHA-256** + **SHA-384** for transcript hashes; SHA-1 is
 *     disabled (deprecated by RFC 8996).
 *   - **MBEDTLS_AES_ALT** + **MBEDTLS_SHA256_ALT** so the AES + SHA
 *     primitives are routed through the RSIP-E50D engine via the
 *     port shims in this directory.
 *   - **MBEDTLS_PLATFORM_MEMORY** -- ``mbedtls_calloc`` /
 *     ``mbedtls_free`` are bound to a ThreadX byte pool inside the
 *     example app so the safety-critical "no malloc after init" rule
 *     is preserved.
 *
 * ## Surface this config disables
 *
 *   - **No PSA crypto** for now -- the legacy ``mbedtls_*`` API is
 *     simpler to bind to ``ra_rsip`` and the project does not need
 *     PSA's secure-element abstraction yet.
 *   - **No test suite / self-test** -- production builds skip the
 *     known-answer tests; the integration harness covers them.
 *   - **No filesystem / no PEM I/O / no entropy source poll** -- the
 *     example app injects its own DRBG seed from ``ra_rsip_trng_read``
 *     so Mbed TLS's default entropy collectors are unnecessary.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* =============================================================================
 * System / platform abstraction
 * =============================================================================
 */

#define MBEDTLS_HAVE_ASM

/* Use the platform memory layer so the example app can bind
 * mbedtls_calloc / mbedtls_free to a ThreadX byte pool. */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_NO_STD_FUNCTIONS

/* =============================================================================
 * ALT replacements -- AES + SHA-256 routed to RSIP hardware
 * =============================================================================
 */

#define MBEDTLS_AES_ALT
#define MBEDTLS_SHA256_ALT

/* =============================================================================
 * Symmetric ciphers
 * =============================================================================
 */

#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_CIPHER_MODE_CTR
#define MBEDTLS_GCM_C
#define MBEDTLS_CCM_C

/* =============================================================================
 * Hashes
 * =============================================================================
 */

#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_MD_C

/* =============================================================================
 * Public-key primitives
 * =============================================================================
 */

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C

#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM

/* =============================================================================
 * Random number generator
 * =============================================================================
 */

#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_HMAC_DRBG_C
#define MBEDTLS_ENTROPY_C

/* =============================================================================
 * X.509
 * =============================================================================
 */

#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CHECK_KEY_USAGE
#define MBEDTLS_X509_CHECK_EXTENDED_KEY_USAGE

/* =============================================================================
 * TLS 1.2 + TLS 1.3
 * =============================================================================
 */

#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_SSL_SESSION_TICKETS

/* Suite enables -- AES-128-GCM is mandatory in the spec; AES-128-CBC
 * with HMAC-SHA-256 is the legacy fallback. */
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED

/* =============================================================================
 * Cipher-suite + transport hardening defaults
 * =============================================================================
 */

#define MBEDTLS_SSL_MAX_CONTENT_LEN 16384

/* =============================================================================
 * Things we explicitly do NOT enable
 * =============================================================================
 *
 *   - MBEDTLS_PSA_CRYPTO_C       -- PSA delegated crypto.
 *   - MBEDTLS_SELF_TEST          -- known-answer tests.
 *   - MBEDTLS_FS_IO              -- filesystem (we have no fs in this app).
 *   - MBEDTLS_NET_C              -- BSD socket adapter (we use NetX directly).
 *   - MBEDTLS_TIMING_C           -- POSIX timing helpers.
 *   - MBEDTLS_HAVE_TIME / _DATE  -- system time (we wire ra_time later).
 *   - MBEDTLS_DEBUG_C            -- chatty debug output (off by default;
 *                                   the example app re-enables it locally
 *                                   if needed).
 */
