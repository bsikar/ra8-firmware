/**
 * @file ra_tls.h
 * @brief Minimal TLS 1.2 client (single cipher suite) public API.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * ``ra_tls`` is a tiny TLS 1.2 client that sits on top of two existing
 * facilities in this tree:
 *
 *   - ``ra_sce`` (libs/ra_hal/inc/ra_sce.h) for the symmetric cipher
 *     (AES-128-GCM), the handshake-transcript hash (SHA-256), the
 *     HMAC-based PRF for key derivation, and the TRNG for the
 *     ClientHello random + premaster secret.
 *   - A caller-supplied transport (typically ``ra_net`` TCP sockets
 *     from libs/ra_net/inc/ra_net.h) injected via two function
 *     pointers in ``ra_tls_config_t``.
 *
 * The goal is _not_ a full TLS implementation. Real production TLS
 * needs full X.509 chain validation, multiple cipher suites, session
 * resumption, renegotiation, alert handling, and a hardened RNG.
 * Vendoring mbedTLS or wolfSSL is the correct path for that. This
 * library exists so the firmware has an HTTPS-capable starting point
 * without dragging in ~50 KLOC of dependency.
 *
 * Bounded scope (RFC 5246 "The Transport Layer Security (TLS)
 * Protocol Version 1.2", August 2008):
 *
 *   - **TLS 1.2 client only** (no server, no 1.0/1.1/1.3).
 *   - **Single cipher suite**: TLS_RSA_WITH_AES_128_GCM_SHA256 (the
 *     mandatory-to-implement suite per RFC 5246 sec 9).
 *   - **No client certificates** (CertificateRequest is rejected).
 *   - **Server certificate**: pinned by SHA-256 hash of the leaf
 *     ``Certificate`` payload. No DER parsing, no chain walk, no
 *     hostname match against subject CN / SAN. Caller is expected
 *     to bake the expected hash into the ``ra_tls_config_t`` from
 *     a prior trusted-channel acquisition.
 *   - **No resumption** (ServerHello session_id is ignored on output).
 *   - **No renegotiation** (HelloRequest from server -> fatal alert).
 *   - **Extensions**: only ``server_name`` (SNI, RFC 6066 sec 3) is
 *     emitted on ClientHello. ServerHello extensions are ignored.
 *   - **No RSA primitive**: this tree's ``ra_sce`` software stub does
 *     not currently expose RSA encrypt. ``ra_tls_client_handshake``
 *     therefore writes the ClientKeyExchange placeholder containing
 *     the unencrypted premaster secret length-prefixed in the slot
 *     where the RSA-encrypted PMS would normally go. The state
 *     machine and record framing remain RFC-compliant; the resulting
 *     session will only interoperate with a peer that mirrors this
 *     placeholder behaviour. See @warning on ``ra_tls_client_handshake``.
 *
 * The library is single-threaded. Every call goes through one
 * ``ra_tls_client_t`` context owned by the caller; concurrent calls
 * on the same context are undefined. Re-entrant use across distinct
 * contexts is fine provided the underlying ``ra_sce`` cache is not
 * straddled (the SCE driver itself is single-context, see
 * ``libs/ra_hal/inc/ra_sce.h`` thread-safety notes).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/* =============================================================================
 * Sizing constants -- typed enums per project style (no #define magic).
 * =============================================================================
 */

/**
 * @enum ra_tls_sizes_t
 * @brief Fixed-size constants used throughout the TLS 1.2 record and
 *        handshake layers.
 *
 * @details
 * Values are pinned to the wire formats specified by RFC 5246. The
 * record-header size, handshake-header size, master-secret length,
 * and AES-128-GCM key/IV sizes are constants of the protocol; pinning
 * them as named enums removes magic numbers from the implementation.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra_tls_record_header_bytes   = 5U,     /**< ContentType+Version+Length, RFC 5246 sec 6.2.1. */
  k_ra_tls_hs_header_bytes       = 4U,     /**< HandshakeType+Length, RFC 5246 sec 7.4.         */
  k_ra_tls_random_bytes          = 32U,    /**< gmt_unix_time + 28 random, RFC 5246 sec 7.4.1.2.*/
  k_ra_tls_master_secret_bytes   = 48U,    /**< RFC 5246 sec 8.1.                                */
  k_ra_tls_premaster_bytes       = 48U,    /**< RFC 5246 sec 7.4.7.1, RSA premaster length.      */
  k_ra_tls_pms_version_offset    = 0U,     /**< client_version offset inside premaster.          */
  k_ra_tls_aes128_key_bytes      = 16U,    /**< AES-128 key.                                     */
  k_ra_tls_gcm_implicit_iv_bytes = 4U,     /**< RFC 5288 sec 3, salt half of GCM nonce.          */
  k_ra_tls_gcm_explicit_iv_bytes = 8U,     /**< RFC 5288 sec 3, per-record nonce_explicit.       */
  k_ra_tls_gcm_tag_bytes         = 16U,    /**< AES-GCM auth tag length.                         */
  k_ra_tls_sha256_digest_bytes   = 32U,    /**< SHA-256 output.                                  */
  k_ra_tls_finished_verify_bytes = 12U,    /**< RFC 5246 sec 7.4.9 verify_data length.           */
  k_ra_tls_session_id_max_bytes  = 32U,    /**< RFC 5246 sec 7.4.1.2 SessionID upper bound.      */
  k_ra_tls_max_record_bytes      = 16640U, /**< 2^14 + 256 (compressed) per RFC 5246 sec 6.2.3.  */
  k_ra_tls_max_handshake_bytes   = 4096U,  /**< Bounded inbound handshake-message size.          */
  k_ra_tls_max_hostname_bytes    = 255U,   /**< RFC 6066 sec 3 server_name HostName length.      */
} ra_tls_sizes_t;

/**
 * @enum ra_tls_cipher_suite_t
 * @brief TLS cipher suite identifiers (uint16_t on the wire).
 *
 * @details
 * Only the mandatory-to-implement suite is defined. The 16-bit value
 * is the IANA TLS Cipher Suite registry assignment.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  /* RFC 5288 sec 3 "AES Galois Counter Mode Cipher Suites for TLS",
   * registered value 0x009C. */
  k_ra_tls_suite_rsa_aes128_gcm_sha256 = 0x009CU,
} ra_tls_cipher_suite_t;

/**
 * @enum ra_tls_content_type_t
 * @brief TLS record-layer ContentType (RFC 5246 sec 6.2.1).
 *
 * @details
 * On-wire single byte that disambiguates record payloads.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra_tls_ct_change_cipher_spec = 20U, /**< RFC 5246 sec 7.1.   */
  k_ra_tls_ct_alert              = 21U, /**< RFC 5246 sec 7.2.   */
  k_ra_tls_ct_handshake          = 22U, /**< RFC 5246 sec 7.4.   */
  k_ra_tls_ct_application_data   = 23U, /**< RFC 5246 sec 10.    */
} ra_tls_content_type_t;

/**
 * @enum ra_tls_hs_type_t
 * @brief TLS handshake message types (RFC 5246 sec 7.4).
 *
 * @details
 * On-wire single byte. Values not used by this client (e.g.
 * ``hello_request``, ``certificate_request``) are still listed
 * so the receive path can name them in error logs.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra_tls_hs_hello_request       = 0U,
  k_ra_tls_hs_client_hello        = 1U,
  k_ra_tls_hs_server_hello        = 2U,
  k_ra_tls_hs_certificate         = 11U,
  k_ra_tls_hs_server_key_exchange = 12U,
  k_ra_tls_hs_certificate_request = 13U,
  k_ra_tls_hs_server_hello_done   = 14U,
  k_ra_tls_hs_certificate_verify  = 15U,
  k_ra_tls_hs_client_key_exchange = 16U,
  k_ra_tls_hs_finished            = 20U,
} ra_tls_hs_type_t;

/**
 * @enum ra_tls_alert_level_t
 * @brief TLS alert severity (RFC 5246 sec 7.2).
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra_tls_alert_warning = 1U, /**< Recoverable.    */
  k_ra_tls_alert_fatal   = 2U, /**< Connection torn. */
} ra_tls_alert_level_t;

/**
 * @enum ra_tls_alert_desc_t
 * @brief Subset of TLS alert descriptions (RFC 5246 sec 7.2).
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra_tls_alert_close_notify       = 0U,
  k_ra_tls_alert_unexpected_message = 10U,
  k_ra_tls_alert_bad_record_mac     = 20U,
  k_ra_tls_alert_handshake_failure  = 40U,
  k_ra_tls_alert_bad_certificate    = 42U,
  k_ra_tls_alert_illegal_parameter  = 47U,
  k_ra_tls_alert_decode_error       = 50U,
  k_ra_tls_alert_decrypt_error      = 51U,
  k_ra_tls_alert_protocol_version   = 70U,
  k_ra_tls_alert_internal_error     = 80U,
} ra_tls_alert_desc_t;

/**
 * @enum ra_tls_hs_state_t
 * @brief Linear handshake-driver state machine.
 *
 * @details
 * The client handshake is fully linear in TLS_RSA_WITH_*: there is no
 * branching on ServerKeyExchange or CertificateRequest in our subset.
 * Each call to ``ra_tls_client_handshake`` advances through these
 * states in order; the function returns ``k_ra_ok`` only when
 * ``k_ra_tls_state_established`` is reached.
 *
 * @startuml
 * [*] --> idle
 * idle --> sent_client_hello : send ClientHello
 * sent_client_hello --> got_server_hello_done : recv ServerHello, Certificate, ServerHelloDone
 * got_server_hello_done --> sent_finished : send CKE, CCS, Finished
 * sent_finished --> established : recv CCS, Finished
 * established --> [*]
 * @enduml
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra_tls_state_idle                  = 0U,
  k_ra_tls_state_sent_client_hello     = 1U,
  k_ra_tls_state_got_server_hello      = 2U,
  k_ra_tls_state_got_certificate       = 3U,
  k_ra_tls_state_got_server_hello_done = 4U,
  k_ra_tls_state_sent_finished         = 5U,
  k_ra_tls_state_established           = 6U,
  k_ra_tls_state_closed                = 7U,
  k_ra_tls_state_failed                = 8U,
} ra_tls_hs_state_t;

/* =============================================================================
 * Transport injection
 * =============================================================================
 */

/**
 * @typedef ra_tls_send_fn
 * @brief Caller-supplied TCP-send hook (Dependency Inversion).
 *
 * @details
 * Must transmit exactly ``len`` bytes from ``buf`` over the underlying
 * TCP connection, blocking the caller as needed. A short write must
 * be reported as ``k_ra_err_comm_error``.
 *
 * @param[in] ctx Opaque caller-side handle (TCP socket, etc.).
 * @param[in] buf Source bytes (>= ``len`` bytes).
 * @param[in] len Number of bytes to transmit.
 *
 * @return ``ra_err_t`` -- ``k_ra_ok`` on full transmit.
 *
 * @since 0.1.0
 */
typedef ra_err_t (*ra_tls_send_fn)(void* ctx, const uint8_t* buf, uint32_t len);

/**
 * @typedef ra_tls_recv_fn
 * @brief Caller-supplied TCP-recv hook (Dependency Inversion).
 *
 * @details
 * Must read up to ``max_len`` bytes into ``buf`` and write the actual
 * count into ``*got_len``. A clean peer close is reported by
 * ``*got_len == 0`` plus ``k_ra_ok``. Any I/O failure returns
 * ``k_ra_err_comm_error`` (or another communication-category code).
 *
 * @param[in]  ctx     Opaque caller-side handle.
 * @param[out] buf     Destination buffer (>= ``max_len`` bytes).
 * @param[in]  max_len Buffer capacity.
 * @param[out] got_len Bytes actually read; never NULL.
 *
 * @return ``ra_err_t``.
 *
 * @since 0.1.0
 */
typedef ra_err_t (*ra_tls_recv_fn)(void* ctx, uint8_t* buf, uint32_t max_len, uint32_t* got_len);

/* =============================================================================
 * Configuration and context
 * =============================================================================
 */

/**
 * @struct ra_tls_config_t
 * @brief Caller-supplied configuration for ``ra_tls_client_init``.
 *
 * @details
 * The struct is copied into the context on init; the caller may
 * reuse or free its storage afterwards. The ``transport_ctx``
 * pointer, however, is held by reference -- it must outlive the
 * context.
 *
 * @invariant ``hostname`` is a 7-bit-ASCII NUL-terminated DNS name,
 *            length <= ``k_ra_tls_max_hostname_bytes``.
 * @invariant ``send`` and ``recv`` are non-NULL.
 *
 * @code{.c}
 * const ra_tls_config_t cfg = {
 *     .hostname                    = "example.com",
 *     .expected_server_cert_sha256 = pinned_hash,
 *     .send                        = my_tcp_send,
 *     .recv                        = my_tcp_recv,
 *     .transport_ctx               = &my_socket,
 * };
 * @endcode
 *
 * @since 0.1.0
 */
typedef struct {
  const char*    hostname;                    /**< SNI hostname, NUL-terminated. */
  const uint8_t* expected_server_cert_sha256; /**< 32-byte pinned hash. */
  ra_tls_send_fn send;                        /**< TCP send hook. */
  ra_tls_recv_fn recv;                        /**< TCP recv hook. */
  void*          transport_ctx;               /**< Opaque pass-through. */
} ra_tls_config_t;

/**
 * @struct ra_tls_client_t
 * @brief Per-connection TLS 1.2 client context.
 *
 * @details
 * The struct is opaque to callers in the sense that no field should
 * be poked directly, but it is intentionally non-PIMPL so callers
 * can stack-allocate it without dynamic allocation (NASA Power of
 * 10 Rule 3). The handshake state, derived keys, sequence numbers,
 * and the running transcript hash live here.
 *
 * @invariant After ``ra_tls_client_init`` returns ``k_ra_ok``, the
 *            ``magic`` field equals ``k_ra_tls_ctx_magic`` until
 *            ``ra_tls_client_close`` runs.
 *
 * @since 0.1.0
 */
typedef struct {
  uint32_t          magic; /**< Init sentinel; see ``k_ra_tls_ctx_magic``. */
  ra_tls_config_t   cfg;   /**< Copied from the caller's config. */
  ra_tls_hs_state_t state; /**< Handshake driver state. */

  /** @brief ClientHello.random (gmt_unix_time + 28 random bytes). */
  uint8_t client_random[k_ra_tls_random_bytes];
  /** @brief ServerHello.random (32 bytes). */
  uint8_t server_random[k_ra_tls_random_bytes];
  /** @brief 48-byte premaster secret (RFC 5246 sec 7.4.7.1). */
  uint8_t premaster[k_ra_tls_premaster_bytes];
  /** @brief 48-byte master secret (RFC 5246 sec 8.1). */
  uint8_t master_secret[k_ra_tls_master_secret_bytes];

  /** @brief Client write key (AES-128). */
  uint8_t client_write_key[k_ra_tls_aes128_key_bytes];
  /** @brief Server write key (AES-128). */
  uint8_t server_write_key[k_ra_tls_aes128_key_bytes];
  /** @brief Client implicit IV / salt (RFC 5288). */
  uint8_t client_write_iv[k_ra_tls_gcm_implicit_iv_bytes];
  /** @brief Server implicit IV / salt. */
  uint8_t server_write_iv[k_ra_tls_gcm_implicit_iv_bytes];

  /** @brief Outbound record sequence number (RFC 5246 sec 6.2.3.3). */
  uint64_t client_seq;
  /** @brief Inbound record sequence number. */
  uint64_t server_seq;

  /** @brief Running SHA-256 of the handshake transcript. */
  uint8_t transcript[k_ra_tls_sha256_digest_bytes];
  /** @brief Bytes absorbed into ``transcript`` so far (length-only counter). */
  uint32_t transcript_len;
} ra_tls_client_t;

/**
 * @enum ra_tls_misc_t
 * @brief Miscellaneous magic values used internally.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  /* "TLS1" little-endian -- arbitrary sentinel proving init() ran. */
  k_ra_tls_ctx_magic = 0x31534C54U,
} ra_tls_misc_t;

/* =============================================================================
 * Public API
 * =============================================================================
 */

/**
 * @brief Initialise a TLS 1.2 client context.
 *
 * @details
 * Copies ``cfg`` into ``ctx``, zero-initialises every transient
 * field (RFC 5246 sec 7.4.1.2 random not yet sampled), and parks the
 * state machine in ``k_ra_tls_state_idle``. Does NOT touch the
 * transport: the caller is expected to have already established the
 * underlying TCP connection.
 *
 * @param[out] ctx Caller-owned context to populate.
 * @param[in]  cfg Configuration (see ``ra_tls_config_t``).
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok Initialised; ``ctx`` is ready for handshake.
 * @retval k_ra_err_null_ptr ``ctx``, ``cfg``, ``cfg->hostname``,
 *                           ``cfg->expected_server_cert_sha256``,
 *                           ``cfg->send`` or ``cfg->recv`` was NULL.
 * @retval k_ra_err_invalid_arg ``hostname`` was empty or longer than
 *                              ``k_ra_tls_max_hostname_bytes``.
 *
 * @pre ``ctx`` and ``cfg`` are non-NULL.
 * @pre ``cfg->hostname`` length is in ``[1, 255]``.
 * @post On success, ``ctx->magic == k_ra_tls_ctx_magic``.
 * @post On success, ``ctx->state == k_ra_tls_state_idle``.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra_tls_client_handshake
 * @see ra_tls_client_close
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_tls_client_init(ra_tls_client_t* ctx, const ra_tls_config_t* cfg);

/**
 * @brief Drive the TLS 1.2 client handshake to completion.
 *
 * @details
 * Sends ClientHello, then receives ServerHello, Certificate, and
 * ServerHelloDone. Pins the leaf certificate by SHA-256 against
 * ``cfg.expected_server_cert_sha256``. Sends ClientKeyExchange,
 * ChangeCipherSpec, and Finished, then receives ChangeCipherSpec
 * and Finished. Returns ``k_ra_ok`` only when state reaches
 * ``k_ra_tls_state_established``.
 *
 * @warning Because ``ra_sce`` does not yet expose RSA encrypt, the
 *          ClientKeyExchange in this implementation carries the
 *          unencrypted premaster secret in the slot where
 *          ``EncryptedPreMasterSecret`` would normally sit. This is
 *          NOT interoperable with a stock TLS server. The framing,
 *          PRF derivation, AES-GCM record encryption, and Finished
 *          verify_data computation are all RFC-compliant; only the
 *          RSA wrap step is stubbed. Callers that require real
 *          interop must drop in an RSA primitive (e.g. via a future
 *          ``ra_sce_rsa_public_encrypt`` hook).
 *
 * @param[in,out] ctx Initialised TLS client context.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok Handshake complete; record I/O may begin.
 * @retval k_ra_err_null_ptr ``ctx`` was NULL.
 * @retval k_ra_err_invalid_state ``ctx`` was not initialised, or the
 *                                handshake had already failed/closed.
 * @retval k_ra_err_protocol_error Peer sent an unexpected message
 *                                 type, version, or cipher suite.
 * @retval k_ra_err_bad_certificate-equivalent ``k_ra_err_validation_failed``
 *                                  if the leaf-cert hash did not
 *                                  match the pinned value.
 * @retval k_ra_err_comm_error Underlying transport I/O failed.
 *
 * @pre ``ctx->magic == k_ra_tls_ctx_magic``.
 * @pre Underlying TCP connection is established.
 * @post On success, ``ctx->state == k_ra_tls_state_established``.
 * @post On success, derived keys are populated and seq numbers reset.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra_tls_client_send
 * @see ra_tls_client_recv
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_tls_client_handshake(ra_tls_client_t* ctx);

/**
 * @brief Encrypt and emit a TLS application_data record.
 *
 * @details
 * Wraps ``plaintext`` in a TLS 1.2 record with ContentType =
 * ``application_data``, encrypts with AES-128-GCM using
 * ``ctx->client_write_key`` and ``ctx->client_write_iv ||
 * nonce_explicit`` (RFC 5288 sec 3), and ships the resulting record
 * through ``cfg.send``. Increments ``ctx->client_seq`` on success.
 *
 * @param[in,out] ctx       Established TLS client context.
 * @param[in]     plaintext Bytes to send (>= ``len`` bytes).
 * @param[in]     len       Plaintext length, must be non-zero and
 *                          fit within the record ceiling.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok Record transmitted.
 * @retval k_ra_err_null_ptr ``ctx`` or ``plaintext`` was NULL.
 * @retval k_ra_err_invalid_arg ``len`` was zero or too large.
 * @retval k_ra_err_invalid_state Handshake not yet complete.
 * @retval k_ra_err_comm_error Transport send failed.
 *
 * @pre ``ctx->state == k_ra_tls_state_established``.
 * @pre ``plaintext`` is non-NULL and ``len > 0``.
 * @post On success, ``ctx->client_seq`` has been incremented by 1.
 * @post On success, ``cfg.send`` has been called exactly once.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra_tls_client_recv
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_tls_client_send(ra_tls_client_t* ctx, const uint8_t* plaintext, uint32_t len);

/**
 * @brief Receive and decrypt the next TLS application_data record.
 *
 * @details
 * Drains a single record from the transport via ``cfg.recv``, verifies
 * the GCM tag using ``ctx->server_write_key``, and copies the
 * recovered plaintext into ``buf`` (up to ``max_len`` bytes; over-
 * length records are rejected). Increments ``ctx->server_seq`` on
 * success.
 *
 * @param[in,out] ctx     Established TLS client context.
 * @param[out]    buf     Destination buffer (>= ``max_len`` bytes).
 * @param[in]     max_len Buffer capacity.
 * @param[out]    got_len Decrypted plaintext length; never NULL.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok Record decrypted; ``*got_len`` >= 0.
 * @retval k_ra_err_null_ptr ``ctx``, ``buf``, or ``got_len`` NULL.
 * @retval k_ra_err_invalid_arg ``max_len`` was zero.
 * @retval k_ra_err_invalid_state Handshake not yet complete.
 * @retval k_ra_err_no_mem Record larger than ``max_len``.
 * @retval k_ra_err_crc_mismatch GCM tag did not verify.
 * @retval k_ra_err_comm_error Transport recv failed.
 *
 * @pre ``ctx->state == k_ra_tls_state_established``.
 * @pre ``buf`` and ``got_len`` are non-NULL.
 * @post On success, ``ctx->server_seq`` is incremented.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra_tls_client_send
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_tls_client_recv(ra_tls_client_t* ctx, uint8_t* buf, uint32_t max_len, uint32_t* got_len);

/**
 * @brief Send a close_notify alert and tear down the context.
 *
 * @details
 * Emits a TLS Alert(level=warning, description=close_notify), wipes
 * key material, and parks the state in ``k_ra_tls_state_closed``.
 * Subsequent calls to ``send``/``recv``/``handshake`` will return
 * ``k_ra_err_invalid_state``. Tearing down the underlying TCP
 * connection is the caller's responsibility.
 *
 * @param[in,out] ctx Initialised context.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok Alert emitted (or skipped if state was already
 *                 idle/failed) and context wiped.
 * @retval k_ra_err_null_ptr ``ctx`` was NULL.
 *
 * @pre ``ctx`` is non-NULL.
 * @post ``ctx->magic`` is cleared.
 * @post ``ctx->state == k_ra_tls_state_closed``.
 *
 * @note Thread safety: not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_tls_client_close(ra_tls_client_t* ctx);

#ifdef __cplusplus
}
#endif
