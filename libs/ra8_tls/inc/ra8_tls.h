/**
 * @file ra8_tls.h
 * @brief Tiny TLS facade over the vendored Mbed TLS 4.x stack
 * @ingroup grp_security
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * ``ra8_tls`` is a thin, project-shaped wrapper around the third-party
 * Mbed TLS 4.x + TF-PSA-Crypto 1.x library that ships under
 * ``libs/third_party/mbedtls`` and ``libs/third_party/tf-psa-crypto``.
 * The goal is twofold:
 *
 * 1. Hide the Mbed TLS spelling (``mbedtls_ssl_context``,
 *    ``mbedtls_ssl_config``, ``mbedtls_ctr_drbg_context``, ...) behind
 *    a small ``ra8_tls_*`` API that returns ``ra8_err_t`` like the rest
 *    of the firmware. Higher-level apps (HTTPS client, MQTT/TLS,
 *    OTA firmware fetch) call into this facade instead of pulling a
 *    direct Mbed TLS dependency into their translation units.
 *
 * 2. Enforce NASA Power of 10 Rule 3 (no dynamic memory after init):
 *    sessions are handed out from a fixed-size static pool of
 *    ``mbedtls_ssl_context`` + ``mbedtls_ssl_config`` blocks, so a
 *    misbehaving consumer cannot fragment the heap or run the
 *    allocator past its budget.
 *
 * ## Layering
 *
 * @verbatim
 *   +---------------------------+ ra8_tls_handshake
 *   | App (HTTPS / MQTT / OTA)  | ra8_tls_send / ra8_tls_recv
 *   +-------------+-------------+
 *                 |
 *                 v
 *   +---------------------------+ ra8_tls (this header)
 *   | Mbed TLS 4.x + TF-PSA     |
 *   +-------------+-------------+
 *                 |
 *                 v
 *   +---------------------------+ user-supplied BIO callbacks
 *   | Transport (NetX Duo)      |
 *   +---------------------------+
 * @endverbatim
 *
 * The transport is bound through the BIO callbacks on
 * ``ra8_tls_session_cfg_t`` -- the facade itself does not know about
 * NetX Duo. The NetX Duo adapter lives in a follow-up library.
 *
 * ## Threading
 *
 * One global init brings the PSA crypto layer online. The
 * session-scoped APIs are not internally synchronised: callers that
 * dispatch the same session from multiple threads must serialise the
 * calls themselves (typical embedded usage opens a session from one
 * task and never shares it).
 *
 * ## Memory model
 *
 * - ``k_ra8_tls_max_sessions`` ``mbedtls_ssl_context`` blocks live in
 *   ``.bss`` inside ``ra8_tls.c``. ``ra8_tls_session_open`` hands out
 *   indices from a small bitmap; ``ra8_tls_session_close`` clears the
 *   bit and runs ``mbedtls_ssl_free`` on the slot.
 * - The opaque ``ra8_tls_session_t`` is a typed pointer to an internal
 *   struct that lives inside the same static pool, so callers cannot
 *   construct one out of thin air; the only legal source is
 *   ``ra8_tls_session_open``.
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

#include "ra8_err.h"

/* =============================================================================
 * Limits
 * =============================================================================
 */

/**
 * @enum ra8_tls_limits_t
 * @brief Static-pool sizing constants for the TLS facade.
 *
 * @details
 * These bounds are chosen to fit four concurrent TLS sessions on the
 * RA8D2 SRAM budget while leaving headroom for ThreadX stacks and
 * NetX Duo packet pools. Increasing the count requires re-sizing
 * ``s_session_pool`` in ``ra8_tls.c``.
 *
 * @invariant ``k_ra8_tls_max_sessions`` fits in a ``uint8_t``.
 */
typedef enum : uint8_t {
  /**
   * @brief Maximum simultaneous TLS sessions handed out by the pool.
   * @details NASA Power of 10 Rule 3 cap: any further open returns
   *          ``k_ra8_err_no_mem``.
   */
  k_ra8_tls_max_sessions = 4U,
  /**
   * @brief Capacity (bytes) a caller must reserve for a cipher-suite name.
   * @details ``ra8_tls_get_cipher_suite`` never writes more than this many
   *          bytes (including the terminating NUL) into the caller buffer;
   *          the longest IANA suite string plus NUL fits comfortably.
   */
  k_ra8_tls_cipher_name_cap = 48U,
} ra8_tls_limits_t;

/**
 * @enum ra8_tls_net_const_t
 * @brief Transport-sizing constants used by ``ra8_tls_mss_clamp``.
 *
 * @details
 * The RA8D2 ESWM has a documented large-frame egress defect (issue #21):
 * frames over roughly half a KiB corrupt on the wire, so the whole
 * networking stack is pinned to a 128-byte MTU. A TLS client that dials
 * over TCP must therefore clamp its TCP Maximum Segment Size (MSS) so
 * every segment -- TLS record bytes included -- fits inside one 128-byte
 * MTU frame after the fixed IPv4 + TCP header overhead. These constants
 * express that arithmetic without a bare literal.
 *
 * @invariant ``k_ra8_tls_ipv4_hdr_bytes`` + ``k_ra8_tls_tcp_hdr_bytes`` is
 *            strictly less than ``k_ra8_tls_mtu_min`` so a positive MSS
 *            always exists at the floor.
 */
typedef enum : uint16_t {
  k_ra8_tls_ipv4_hdr_bytes = 20U,  /**< IPv4 header with no options.    */
  k_ra8_tls_tcp_hdr_bytes  = 20U,  /**< TCP header with no options.     */
  k_ra8_tls_mtu_min        = 128U, /**< #21 pinned MTU floor (bytes).   */
  k_ra8_tls_mss_min        = 64U,  /**< Smallest MSS worth clamping to. */
} ra8_tls_net_const_t;

/**
 * @enum ra8_tls_verify_mode_t
 * @brief Peer-certificate verification policy for a session.
 *
 * @details
 * Maps onto the Mbed TLS ``MBEDTLS_SSL_VERIFY_*`` authentication modes.
 * The zero value is a safe default (behaves as ``required``) so a
 * zero-initialised ``ra8_tls_session_cfg_t`` never silently disables
 * verification.
 *
 * @invariant ``k_ra8_tls_verify_default`` is treated identically to
 *            ``k_ra8_tls_verify_required`` by the facade.
 */
typedef enum : uint8_t {
  k_ra8_tls_verify_default  = 0U, /**< Facade default -- same as ``required``. */
  k_ra8_tls_verify_none     = 1U, /**< Do not verify the peer certificate.     */
  k_ra8_tls_verify_optional = 2U, /**< Verify but do not abort on failure.     */
  k_ra8_tls_verify_required = 3U, /**< Verify and abort the handshake on fail. */
} ra8_tls_verify_mode_t;

/* =============================================================================
 * BIO callback signatures
 * =============================================================================
 */

/**
 * @brief BIO send callback signature (write ciphertext to transport).
 *
 * @details
 * Mirrors the Mbed TLS ``mbedtls_ssl_send_t`` contract: returns the
 * number of bytes accepted by the transport, or a negative Mbed TLS
 * error code on failure (``MBEDTLS_ERR_SSL_WANT_WRITE`` for
 * non-blocking would-block).
 *
 * @param[in,out] ctx Opaque user pointer registered through
 *                    ``ra8_tls_session_cfg_t::bio_ctx``.
 * @param[in]     buf Buffer holding ``len`` bytes of ciphertext.
 * @param[in]     len Length of ``buf`` in bytes.
 *
 * @return Bytes written, or a negative Mbed TLS error code.
 *
 * @since 0.1.0
 */
typedef int (*ra8_tls_bio_send_fn)(void* ctx, const uint8_t* buf, size_t len);

/**
 * @brief BIO receive callback signature (read ciphertext from transport).
 *
 * @details
 * Mirrors the Mbed TLS ``mbedtls_ssl_recv_t`` contract: returns the
 * number of bytes consumed, ``0`` on EOF, or a negative Mbed TLS error
 * code (``MBEDTLS_ERR_SSL_WANT_READ`` for non-blocking would-block).
 *
 * @param[in,out] ctx Opaque user pointer registered through
 *                    ``ra8_tls_session_cfg_t::bio_ctx``.
 * @param[out]    buf Buffer to fill with up to ``len`` bytes.
 * @param[in]     len Capacity of ``buf`` in bytes.
 *
 * @return Bytes read, ``0`` on EOF, or a negative Mbed TLS error code.
 *
 * @since 0.1.0
 */
typedef int (*ra8_tls_bio_recv_fn)(void* ctx, uint8_t* buf, size_t len);

/* =============================================================================
 * Configuration
 * =============================================================================
 */

/**
 * @struct ra8_tls_session_cfg_t
 * @brief Per-session configuration handed to ``ra8_tls_session_open``.
 *
 * @details
 * The struct carries the BIO function pointers plus an opaque user
 * context that the facade passes back unchanged on every BIO call.
 * Optional ``server_name`` enables SNI when non-NULL.
 *
 * @invariant ``bio_send`` and ``bio_recv`` are non-NULL.
 * @invariant ``bio_ctx`` is owned by the caller for the full session
 *            lifetime (``open`` -> ``close``).
 *
 * @code{.c}
 * static int loop_send(void* ctx, const uint8_t* buf, size_t len) { ... }
 * static int loop_recv(void* ctx, uint8_t* buf, size_t len)       { ... }
 *
 * ra8_tls_session_cfg_t cfg = {};
 * cfg.bio_send    = loop_send;
 * cfg.bio_recv    = loop_recv;
 * cfg.bio_ctx     = &my_socket;
 * cfg.server_name = "api.example.com";
 *
 * ra8_tls_session_t s;
 * ra8_err_t err = ra8_tls_session_open(&s, &cfg);
 * @endcode
 *
 * @since 0.1.0
 */
typedef struct ra8_tls_session_cfg {
  ra8_tls_bio_send_fn   bio_send;    /**< Transport write callback (required).       */
  ra8_tls_bio_recv_fn   bio_recv;    /**< Transport read callback  (required).       */
  void*                 bio_ctx;     /**< Opaque ctx passed back to BIO callbacks.   */
  const char*           server_name; /**< Optional SNI hostname; NULL disables SNI.  */
  ra8_tls_verify_mode_t verify_mode; /**< Peer-cert policy; 0 == required (default). */
  const char*           ca_pem;      /**< Optional trust anchor, PEM; NULL == none.  */
  size_t                ca_pem_len;  /**< ``ca_pem`` length incl. NUL, else 0.       */
} ra8_tls_session_cfg_t;

/* =============================================================================
 * Opaque session handle
 * =============================================================================
 */

/**
 * @struct ra8_tls_session_handle
 * @brief Forward declaration of the pool slot type.
 *
 * @details
 * Defined inside ``ra8_tls.c``; callers only ever see the typed pointer.
 * The pool entry holds the ``mbedtls_ssl_context``,
 * ``mbedtls_ssl_config`` and BIO bookkeeping for a single session.
 */
struct ra8_tls_session_handle;

/**
 * @brief Opaque TLS session handle (typed pointer into the static pool).
 *
 * @details
 * NULL is a sentinel for "uninitialized handle". The only legal way to
 * obtain a non-NULL value is ``ra8_tls_session_open``; passing any
 * other pointer to ``ra8_tls_session_close`` and friends is undefined
 * behaviour from the caller's perspective and yields
 * ``k_ra8_err_invalid_arg`` from this facade.
 *
 * @since 0.1.0
 */
typedef struct ra8_tls_session_handle* ra8_tls_session_t;

/* =============================================================================
 * Public API
 * =============================================================================
 */

/**
 * @brief One-shot facade initialisation.
 *
 * @details
 * Brings the PSA crypto layer online and marks the session pool empty.
 * Mbed TLS 4.x sources randomness from PSA (``psa_generate_random``)
 * rather than a facade-owned CTR_DRBG, so the actual entropy is drawn
 * lazily on the first random call through the application-supplied
 * ``mbedtls_psa_external_get_random`` hook (the RSIP TRNG on hardware).
 * Safe to call exactly once per boot; subsequent calls without a matching
 * ``ra8_tls_global_deinit`` return ``k_ra8_err_exists``.
 *
 * Algorithm:
 * 1. If already initialized, return ``k_ra8_err_exists``.
 * 2. Reset the session pool so close-without-open paths are well-defined.
 * 3. Call ``psa_crypto_init`` (skipped in off-target mode).
 * 4. Mark the module initialized.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok            Facade ready.
 * @retval k_ra8_err_exists    Already initialized this boot.
 * @retval k_ra8_err_hw_error  ``psa_crypto_init`` failed.
 *
 * @pre Mbed TLS has been built into the firmware image (``RA8_USE_MBEDTLS=ON``)
 *      OR ``RA8_OFF_TARGET`` is defined for the host unit-test build.
 * @pre On hardware, the RSIP TRNG that backs the PSA external-RNG hook is
 *      reachable before the first handshake (skipped in off-target mode).
 * @post Module is in the initialized state on success.
 * @post Session pool is fully reset (no slot held).
 *
 * @note Not re-entrant. Call from the boot path before any TLS session
 *       is opened.
 * @warning Per-session trust anchors passed through
 *          ``ra8_tls_session_cfg_t::ca_pem`` are referenced, not copied;
 *          their storage must outlive the session.
 *
 * @par Example:
 * @code{.c}
 * ra8_err_t err = ra8_tls_global_init();
 * RA8_RETURN_ON_ERROR(err, "ra8_tls", "global_init failed");
 * @endcode
 *
 * @see ra8_tls_global_deinit()
 * @since 0.1.0
 */
ra8_err_t ra8_tls_global_init(void);

/**
 * @brief Symmetric tear-down for ``ra8_tls_global_init``.
 *
 * @details
 * Frees every still-open session, wipes the CTR_DRBG state, and
 * marks the module uninitialized so a subsequent
 * ``ra8_tls_global_init`` succeeds again.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Facade torn down.
 * @retval k_ra8_err_not_initialized ``ra8_tls_global_init`` was never called.
 *
 * @pre None (safe to call before any session open).
 * @pre Module was previously initialized.
 * @post Pool is empty and module is not initialized.
 * @post All ``mbedtls_*`` contexts freed.
 *
 * @note Not re-entrant.
 *
 * @see ra8_tls_global_init()
 * @since 0.1.0
 */
ra8_err_t ra8_tls_global_deinit(void);

/**
 * @brief Allocate a TLS session from the static pool.
 *
 * @details
 * Searches the in-use bitmap for a free slot, copies ``cfg`` into the
 * slot, runs ``mbedtls_ssl_setup`` / ``mbedtls_ssl_set_bio`` and
 * returns the typed pointer through ``out_session``.
 *
 * @param[out] out_session Receives the new opaque handle on success.
 *                         Set to NULL on any non-success return.
 * @param[in]  cfg         Session configuration; both BIO callbacks
 *                         must be non-NULL. The struct itself is
 *                         copied; the caller may free it on return.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Session allocated and ready for handshake.
 * @retval k_ra8_err_invalid_arg     ``out_session`` or ``cfg`` is NULL,
 *                                  or one of the BIO callbacks is NULL.
 * @retval k_ra8_err_not_initialized ``ra8_tls_global_init`` was never called.
 * @retval k_ra8_err_no_mem          Pool exhausted (more than
 *                                  ``k_ra8_tls_max_sessions`` open).
 *
 * @pre ``ra8_tls_global_init`` returned ``k_ra8_ok`` previously.
 * @pre ``cfg->bio_send`` and ``cfg->bio_recv`` are non-NULL.
 * @post On ``k_ra8_ok``, ``*out_session`` is non-NULL and survives until
 *       a matching ``ra8_tls_session_close``.
 * @post On any error, ``*out_session`` is set to NULL.
 *
 * @note Not thread-safe; caller must serialise allocation against
 *       concurrent close.
 *
 * @see ra8_tls_session_close()
 * @see ra8_tls_handshake()
 * @since 0.1.0
 */
ra8_err_t ra8_tls_session_open(ra8_tls_session_t* out_session, const ra8_tls_session_cfg_t* cfg);

/**
 * @brief Release a TLS session back to the pool.
 *
 * @details
 * Validates that ``session`` actually points into the pool, runs
 * ``mbedtls_ssl_free`` / ``mbedtls_ssl_config_free`` on the slot, and
 * clears the in-use bit. Safe to call on any open session, even one
 * that has not completed its handshake.
 *
 * @param[in,out] session Handle previously returned by
 *                        ``ra8_tls_session_open``.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Slot released.
 * @retval k_ra8_err_invalid_arg     ``session`` is NULL or does not
 *                                  point into the pool.
 * @retval k_ra8_err_not_initialized ``ra8_tls_global_init`` was never called.
 *
 * @pre ``session`` was returned by ``ra8_tls_session_open``.
 * @pre Module is initialized.
 * @post Slot is free and may be re-issued.
 * @post No further use of ``session`` is permitted (use-after-free is
 *       caller's bug).
 *
 * @see ra8_tls_session_open()
 * @since 0.1.0
 *
 * @note Not thread-safe unless documented otherwise.
 */
ra8_err_t ra8_tls_session_close(ra8_tls_session_t session);

/**
 * @brief Iterative TLS handshake driver.
 *
 * @details
 * Wraps ``mbedtls_ssl_handshake`` and translates its return value into
 * an ``ra8_err_t``. Returns ``k_ra8_err_would_block`` while the
 * underlying BIO is non-blocking and waiting for I/O so the caller can
 * loop without consuming the entire transport-level event budget.
 *
 * In ``RA8_OFF_TARGET`` (host unit-test build) the call short-
 * circuits to ``k_ra8_ok`` after a single BIO drain so the loopback
 * test path can complete without a real cryptographic handshake.
 *
 * @param[in,out] session Open session handle.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Handshake complete.
 * @retval k_ra8_err_invalid_arg     ``session`` invalid.
 * @retval k_ra8_err_not_initialized Module not initialized.
 * @retval k_ra8_err_would_block     BIO is non-blocking; retry later.
 * @retval k_ra8_err_comm_error      Mbed TLS reported a fatal handshake
 *                                  failure (cert / protocol / decode).
 *
 * @pre ``session`` is open.
 * @pre BIO callbacks have been bound (done by ``ra8_tls_session_open``).
 * @post On ``k_ra8_ok`` the session is in the application-data state.
 * @post On any non-would-block error the session must be closed.
 *
 * @see ra8_tls_send()
 * @see ra8_tls_recv()
 * @since 0.1.0
 *
 * @note Not thread-safe unless documented otherwise.
 */
ra8_err_t ra8_tls_handshake(ra8_tls_session_t session);

/**
 * @brief Encrypt and send application data.
 *
 * @details
 * Wraps ``mbedtls_ssl_write``. Returns the number of bytes accepted
 * by the TLS layer through ``out_sent``; partial writes are reported
 * back to the caller so they can advance their buffer pointer.
 *
 * @param[in,out] session  Open session handle in the application-data state.
 * @param[in]     buf      Plaintext input buffer.
 * @param[in]     len      Number of bytes to send (``0`` is a no-op).
 * @param[out]    out_sent Bytes consumed by the TLS layer (always
 *                         ``<= len``).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Wrote ``*out_sent`` bytes.
 * @retval k_ra8_err_invalid_arg     Any pointer NULL or session invalid.
 * @retval k_ra8_err_not_initialized Module not initialized.
 * @retval k_ra8_err_would_block     Non-blocking BIO returned WANT_WRITE.
 * @retval k_ra8_err_comm_error      Fatal TLS-layer error.
 *
 * @pre ``session`` has completed its handshake.
 * @pre ``buf`` is non-NULL when ``len > 0``.
 * @post On ``k_ra8_ok`` ``*out_sent <= len``.
 * @post On any error ``*out_sent == 0``.
 *
 * @see ra8_tls_recv()
 * @since 0.1.0
 *
 * @note Not thread-safe unless documented otherwise.
 */
ra8_err_t ra8_tls_send(ra8_tls_session_t session, const uint8_t* buf, size_t len, size_t* out_sent);

/**
 * @brief Decrypt and receive application data.
 *
 * @details
 * Wraps ``mbedtls_ssl_read``. ``*out_received == 0`` together with
 * ``k_ra8_ok`` denotes a clean peer close-notify; ``k_ra8_err_would_block``
 * means the underlying transport had no data ready.
 *
 * @param[in,out] session      Open session handle in the application-data state.
 * @param[out]    buf          Plaintext output buffer.
 * @param[in]     len          Capacity of ``buf`` in bytes.
 * @param[out]    out_received Bytes decrypted into ``buf``.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Decrypted ``*out_received`` bytes
 *                                  (0 on clean close).
 * @retval k_ra8_err_invalid_arg     Any pointer NULL or session invalid.
 * @retval k_ra8_err_not_initialized Module not initialized.
 * @retval k_ra8_err_would_block     No ciphertext available yet.
 * @retval k_ra8_err_comm_error      Fatal TLS-layer error.
 *
 * @pre ``session`` has completed its handshake.
 * @pre ``buf`` is non-NULL when ``len > 0``.
 * @post ``*out_received <= len``.
 * @post On any error ``*out_received == 0``.
 *
 * @see ra8_tls_send()
 * @since 0.1.0
 *
 * @note Not thread-safe unless documented otherwise.
 */
ra8_err_t ra8_tls_recv(ra8_tls_session_t session, uint8_t* buf, size_t len, size_t* out_received);

/* =============================================================================
 * Session introspection
 * =============================================================================
 */

/**
 * @brief Report the negotiated cipher suite for a session.
 *
 * @details
 * After a successful ``ra8_tls_handshake`` this returns the IANA
 * cipher-suite name (e.g. ``TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256``)
 * and its 16-bit IANA identifier so an application can log exactly what
 * was negotiated. In ``RA8_OFF_TARGET`` (host unit-test build) the
 * handshake is a loopback drain rather than a real negotiation, so a
 * deterministic sentinel is reported: ``out_name`` becomes
 * ``"off-target-loopback"`` and ``*out_id`` becomes ``0``. The output name
 * is always NUL-terminated and never exceeds ``name_cap`` bytes.
 *
 * @param[in]  session  Open session handle in the application-data state.
 * @param[out] out_id   Receives the 16-bit IANA cipher-suite id
 *                      (``0`` when unknown / fake).
 * @param[out] out_name Receives the NUL-terminated cipher-suite name.
 *                      Truncated to fit when the real name is longer.
 * @param[in]  name_cap Capacity of ``out_name`` in bytes; must be >= 1.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Cipher reported into the outputs.
 * @retval k_ra8_err_invalid_arg     Any pointer NULL, ``name_cap`` zero,
 *                                  or ``session`` invalid.
 * @retval k_ra8_err_not_initialized Module not initialized.
 *
 * @pre ``session`` has completed its handshake.
 * @pre ``out_id`` and ``out_name`` are non-NULL and ``name_cap >= 1``.
 * @post On ``k_ra8_ok`` ``out_name`` is NUL-terminated.
 * @post On any error ``*out_id == 0`` and ``out_name[0] == '\0'`` when
 *       the buffers are writable.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @see ra8_tls_get_verify_result()
 * @since 0.1.0
 */
ra8_err_t ra8_tls_get_cipher_suite(ra8_tls_session_t session,
                                   uint16_t*         out_id,
                                   char*             out_name,
                                   size_t            name_cap);

/**
 * @brief Report the peer-certificate verification result for a session.
 *
 * @details
 * Wraps ``mbedtls_ssl_get_verify_result``: ``0`` means the peer chain
 * satisfied the configured ``verify_mode``; any non-zero value is the OR
 * of ``MBEDTLS_X509_BADCERT_*`` / ``MBEDTLS_X509_BADCRL_*`` flags. In
 * ``RA8_OFF_TARGET`` the loopback path reports ``0`` (verified).
 *
 * @param[in]  session   Open session handle in the application-data state.
 * @param[out] out_flags Receives the verification bit set (``0`` == OK).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Result written to ``*out_flags``.
 * @retval k_ra8_err_invalid_arg     ``out_flags`` NULL or ``session`` invalid.
 * @retval k_ra8_err_not_initialized Module not initialized.
 *
 * @pre ``session`` has completed its handshake.
 * @pre ``out_flags`` is non-NULL.
 * @post On ``k_ra8_ok`` ``*out_flags`` holds the verification bit set.
 * @post On any error ``*out_flags == 0`` when the pointer is writable.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @see ra8_tls_get_cipher_suite()
 * @since 0.1.0
 */
ra8_err_t ra8_tls_get_verify_result(ra8_tls_session_t session, uint32_t* out_flags);

/**
 * @brief Compute the TCP MSS that keeps a segment inside one MTU frame.
 *
 * @details
 * Pure arithmetic helper for TLS-over-TCP clients on the #21-pinned
 * 128-byte MTU link: subtracts the fixed IPv4 + TCP header overhead from
 * @p mtu to yield the largest TCP payload (Maximum Segment Size) that
 * still fits one Ethernet frame the ESWM egress transmits cleanly. The
 * result is what a caller feeds to the transport (e.g. an Mbed TLS
 * maximum-fragment-length hint or a NetX Duo socket MSS) so the TLS
 * record layer never asks the MAC to send an over-length frame.
 *
 * @param[in]  mtu     Link MTU in bytes (payload, excluding the 14-byte
 *                     Ethernet header); must be >= ``k_ra8_tls_mtu_min``.
 * @param[out] out_mss Receives the clamped MSS
 *                     (``mtu - IPv4 - TCP``) in bytes.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              MSS written to ``*out_mss``.
 * @retval k_ra8_err_invalid_arg ``out_mss`` NULL, or @p mtu too small to
 *                              leave at least ``k_ra8_tls_mss_min`` bytes.
 *
 * @pre ``out_mss`` is non-NULL.
 * @pre @p mtu leaves room for a >= ``k_ra8_tls_mss_min`` byte segment.
 * @post On ``k_ra8_ok`` ``*out_mss`` is in ``[k_ra8_tls_mss_min, mtu)``.
 * @post On any error ``*out_mss == 0`` when the pointer is writable.
 *
 * @note Pure function; safe from any context.
 *
 * @par Example:
 * @code{.c}
 * uint16_t mss = 0U;
 * if (ra8_tls_mss_clamp(k_ra8_tls_mtu_min, &mss) == k_ra8_ok) {
 *   // mss == 88 for the 128-byte MTU: 128 - 20 (IP) - 20 (TCP)
 * }
 * @endcode
 *
 * @since 0.1.0
 */
ra8_err_t ra8_tls_mss_clamp(uint16_t mtu, uint16_t* out_mss);

#ifdef __cplusplus
}
#endif
