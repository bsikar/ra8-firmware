/**
 * @file port/mbedtls/mbedtls_config.h
 * @brief Project-wide Mbed TLS 4.x configuration for the RA8D2 firmware
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
 * Mbed TLS 4.x split the cryptographic primitives out of this config
 * file into the companion TF-PSA-Crypto repository. All AES / SHA /
 * RSA / ECC / bignum / DRBG / entropy knobs are now PSA_WANT_* flags
 * in ``port/mbedtls/tf_psa_crypto_config.h`` (or the upstream
 * ``tf-psa-crypto/include/psa/crypto_config.h`` default). This file
 * only owns the TLS / X.509 / SSL / NET / TIMING / DEBUG layer.
 *
 * ## Surface this config enables
 *
 *   - **TLS 1.2 + TLS 1.3** -- both versions, client side. The
 *     example app talks to ``www.example.com`` which negotiates
 *     1.3 happily; older HTTPS endpoints on the lab network may
 *     still need 1.2, so we keep both.
 *   - **Forward-secret key exchanges** -- ECDHE-RSA + ECDHE-ECDSA.
 *     Static-RSA key exchange is omitted (no PFS, deprecated).
 *   - **X.509 certificate parsing** -- chain validation against a
 *     baked-in CA trust anchor.
 *   - **Server Name Indication (SNI)** -- needed for any host on a
 *     virtual-hosted HTTPS endpoint.
 *   - **Renegotiation + ALPN + session tickets + extended primary
 *     secret** -- the modern hardening defaults Mbed TLS ships.
 *
 * ## Surface this config disables
 *
 *   - **No DTLS** -- this firmware only does TCP TLS.
 *   - **No server side** -- client-only build (``MBEDTLS_SSL_SRV_C``
 *     is omitted).
 *   - **No filesystem / no PEM I/O** -- certs are baked in as DER
 *     blobs.
 *   - **No POSIX BSD socket adapter** -- NetX Duo is the transport;
 *     the example app supplies its own ``mbedtls_ssl_set_bio``
 *     callbacks.
 *   - **No POSIX timing helpers** -- ``MBEDTLS_TIMING_ALT`` is set
 *     and the project supplies its own monotonic-time hook from
 *     ra_time later.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* Tell Mbed TLS which config-file format we are using. The vendored
 * tree is 4.1.0 / 4.x; bump this when we move to 5.x. */
#define MBEDTLS_CONFIG_VERSION (0x04000000)

/* =============================================================================
 * Error / debug / version
 * =============================================================================
 */
#define MBEDTLS_ERROR_C
#define MBEDTLS_ERROR_STRERROR_DUMMY
#define MBEDTLS_VERSION_C
#define MBEDTLS_VERSION_FEATURES

/* =============================================================================
 * Key-exchange modes selected for the TLS handshake
 * =============================================================================
 *
 * These select which cipher-suites the SSL layer will offer / accept.
 * The underlying primitives (AES-GCM, SHA-256, ECDHE math) are
 * configured in tf_psa_crypto_config.h.
 */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED

/* =============================================================================
 * SSL / TLS layer
 * =============================================================================
 */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#define MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_SSL_SESSION_TICKETS
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_ALL_ALERT_MESSAGES
#define MBEDTLS_SSL_ALPN
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_RENEGOTIATION
#define MBEDTLS_SSL_MAX_CONTENT_LEN (16384)

/* =============================================================================
 * X.509 chain validation (client cert parse only -- we don't write certs)
 * =============================================================================
 */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CRL_PARSE_C
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT
#define MBEDTLS_X509_CHECK_KEY_USAGE
#define MBEDTLS_X509_CHECK_EXTENDED_KEY_USAGE

/* =============================================================================
 * Timing -- we provide our own (TIMING_ALT) since there is no POSIX clock.
 * =============================================================================
 *
 * The project does not yet wire ra_time into Mbed TLS, so for now
 * leave both MBEDTLS_TIMING_C and MBEDTLS_TIMING_ALT undefined --
 * the SSL layer falls back to a generation-counter heuristic where
 * timing isn't strictly required for client-side handshakes.
 */

/* =============================================================================
 * Things we explicitly do NOT enable
 * =============================================================================
 *
 *   - MBEDTLS_NET_C              -- BSD socket adapter (we use NetX directly).
 *   - MBEDTLS_TIMING_C           -- POSIX timing helpers.
 *   - MBEDTLS_HAVE_TIME / _DATE  -- system time (no fs-backed RTC yet).
 *   - MBEDTLS_DEBUG_C            -- chatty debug output (the example app
 *                                   may re-enable it locally if needed).
 *   - MBEDTLS_SSL_SRV_C          -- server side; client-only build.
 *   - MBEDTLS_SSL_PROTO_DTLS     -- we only do TCP TLS.
 *   - MBEDTLS_FS_IO              -- no filesystem in this firmware.
 */
