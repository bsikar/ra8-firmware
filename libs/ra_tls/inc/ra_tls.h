/**
 * @file ra_tls.h
 * @brief Tiny TLS facade over the vendored Mbed TLS 4.x stack
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * ``ra_tls`` is a thin, project-shaped wrapper around the third-party
 * Mbed TLS 4.x + TF-PSA-Crypto 1.x library that ships under
 * ``libs/third_party/mbedtls`` and ``libs/third_party/tf-psa-crypto``.
 * The goal is twofold:
 *
 * 1. Hide the Mbed TLS spelling (``mbedtls_ssl_context``,
 *    ``mbedtls_ssl_config``, ``mbedtls_ctr_drbg_context``, ...) behind
 *    a small ``ra_tls_*`` API that returns ``ra_err_t`` like the rest
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
 *   +---------------------------+ ra_tls_handshake
 *   | App (HTTPS / MQTT / OTA)  | ra_tls_send / ra_tls_recv
 *   +-------------+-------------+
 *                 |
 *                 v
 *   +---------------------------+ ra_tls (this header)
 *   | Mbed TLS 4.x + TF-PSA     |
 *   +-------------+-------------+
 *                 |
 *                 v
 *   +---------------------------+ user-supplied BIO callbacks
 *   | Transport (lwIP / NetX)   |
 *   +---------------------------+
 * @endverbatim
 *
 * The transport is bound through the BIO callbacks on
 * ``ra_tls_session_cfg_t`` -- the facade itself does not know about
 * lwIP or NetX Duo. lwIP and NetX adapters live in follow-up libraries.
 *
 * ## Threading
 *
 * One global init protects the shared CTR_DRBG and root-CA bundle. The
 * session-scoped APIs are not internally synchronised: callers that
 * dispatch the same session from multiple threads must serialise the
 * calls themselves (typical embedded usage opens a session from one
 * task and never shares it).
 *
 * ## Memory model
 *
 * - ``k_ra_tls_max_sessions`` ``mbedtls_ssl_context`` blocks live in
 *   ``.bss`` inside ``ra_tls.c``. ``ra_tls_session_open`` hands out
 *   indices from a small bitmap; ``ra_tls_session_close`` clears the
 *   bit and runs ``mbedtls_ssl_free`` on the slot.
 * - The opaque ``ra_tls_session_t`` is a typed pointer to an internal
 *   struct that lives inside the same static pool, so callers cannot
 *   construct one out of thin air; the only legal source is
 *   ``ra_tls_session_open``.
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

#include "ra_err.h"

/* =============================================================================
 * Limits
 * =============================================================================
 */

/**
 * @enum ra_tls_limits_t
 * @brief Static-pool sizing constants for the TLS facade.
 *
 * @details
 * These bounds are chosen to fit four concurrent TLS sessions on the
 * RA8D2 SRAM budget while leaving headroom for ThreadX stacks and
 * NetX Duo packet pools. Increasing the count requires re-sizing
 * ``s_session_pool`` in ``ra_tls.c``.
 *
 * @invariant ``k_ra_tls_max_sessions`` fits in a ``uint8_t``.
 */
typedef enum : uint8_t {
  /**
   * @brief Maximum simultaneous TLS sessions handed out by the pool.
   * @details NASA Power of 10 Rule 3 cap: any further open returns
   *          ``k_ra_err_no_mem``.
   */
  k_ra_tls_max_sessions = 4U,
} ra_tls_limits_t;

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
 *                    ``ra_tls_session_cfg_t::bio_ctx``.
 * @param[in]     buf Buffer holding ``len`` bytes of ciphertext.
 * @param[in]     len Length of ``buf`` in bytes.
 *
 * @return Bytes written, or a negative Mbed TLS error code.
 *
 * @since 0.1.0
 */
typedef int (*ra_tls_bio_send_fn)(void* ctx, const uint8_t* buf, size_t len);

/**
 * @brief BIO receive callback signature (read ciphertext from transport).
 *
 * @details
 * Mirrors the Mbed TLS ``mbedtls_ssl_recv_t`` contract: returns the
 * number of bytes consumed, ``0`` on EOF, or a negative Mbed TLS error
 * code (``MBEDTLS_ERR_SSL_WANT_READ`` for non-blocking would-block).
 *
 * @param[in,out] ctx Opaque user pointer registered through
 *                    ``ra_tls_session_cfg_t::bio_ctx``.
 * @param[out]    buf Buffer to fill with up to ``len`` bytes.
 * @param[in]     len Capacity of ``buf`` in bytes.
 *
 * @return Bytes read, ``0`` on EOF, or a negative Mbed TLS error code.
 *
 * @since 0.1.0
 */
typedef int (*ra_tls_bio_recv_fn)(void* ctx, uint8_t* buf, size_t len);

/* =============================================================================
 * Configuration
 * =============================================================================
 */

/**
 * @struct ra_tls_session_cfg_t
 * @brief Per-session configuration handed to ``ra_tls_session_open``.
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
 * ra_tls_session_cfg_t cfg = {};
 * cfg.bio_send    = loop_send;
 * cfg.bio_recv    = loop_recv;
 * cfg.bio_ctx     = &my_socket;
 * cfg.server_name = "api.example.com";
 *
 * ra_tls_session_t s;
 * ra_err_t err = ra_tls_session_open(&s, &cfg);
 * @endcode
 *
 * @since 0.1.0
 */
typedef struct ra_tls_session_cfg {
  ra_tls_bio_send_fn bio_send;    /**< Transport write callback (required).         */
  ra_tls_bio_recv_fn bio_recv;    /**< Transport read callback  (required).         */
  void*              bio_ctx;     /**< Opaque ctx passed back to BIO callbacks.     */
  const char*        server_name; /**< Optional SNI hostname; NULL disables SNI.    */
} ra_tls_session_cfg_t;

/* =============================================================================
 * Opaque session handle
 * =============================================================================
 */

/**
 * @struct ra_tls_session_handle
 * @brief Forward declaration of the pool slot type.
 *
 * @details
 * Defined inside ``ra_tls.c``; callers only ever see the typed pointer.
 * The pool entry holds the ``mbedtls_ssl_context``,
 * ``mbedtls_ssl_config`` and BIO bookkeeping for a single session.
 */
struct ra_tls_session_handle;

/**
 * @brief Opaque TLS session handle (typed pointer into the static pool).
 *
 * @details
 * NULL is a sentinel for "uninitialized handle". The only legal way to
 * obtain a non-NULL value is ``ra_tls_session_open``; passing any
 * other pointer to ``ra_tls_session_close`` and friends is undefined
 * behaviour from the caller's perspective and yields
 * ``k_ra_err_invalid_arg`` from this facade.
 *
 * @since 0.1.0
 */
typedef struct ra_tls_session_handle* ra_tls_session_t;

/* =============================================================================
 * Public API
 * =============================================================================
 */

/**
 * @brief One-shot facade initialisation.
 *
 * @details
 * Seeds the shared CTR_DRBG from the RSIP TRNG, registers the static
 * root-CA bundle, and marks the pool empty. Safe to call exactly once
 * per boot; subsequent calls without a matching
 * ``ra_tls_global_deinit`` return ``k_ra_err_exists``.
 *
 * Algorithm:
 * 1. If already initialized, return ``k_ra_err_exists``.
 * 2. Initialise the entropy + CTR_DRBG state in ``.bss``.
 * 3. Walk the session pool and call ``mbedtls_ssl_init`` /
 *    ``mbedtls_ssl_config_init`` on each slot so close-without-open
 *    paths are well-defined.
 * 4. Mark the module initialized.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok            Facade ready.
 * @retval k_ra_err_exists    Already initialized this boot.
 * @retval k_ra_err_hw_error  CTR_DRBG seed (entropy) failed.
 *
 * @pre Mbed TLS has been built into the firmware image (``RA_USE_MBEDTLS=ON``)
 *      OR ``RA_SIMULATOR_MODE`` is defined for the host unit-test build.
 * @pre RSIP TRNG is reachable (skipped in simulator mode).
 * @post Module is in the initialized state on success.
 * @post Session pool is fully reset (no slot held).
 *
 * @note Not re-entrant. Call from the boot path before any TLS session
 *       is opened.
 * @warning The root-CA bundle pointer is registered by reference; the
 *          underlying memory must remain valid until
 *          ``ra_tls_global_deinit``.
 *
 * @par Example:
 * @code{.c}
 * ra_err_t err = ra_tls_global_init();
 * RA_RETURN_ON_ERROR(err, "ra_tls", "global_init failed");
 * @endcode
 *
 * @see ra_tls_global_deinit()
 * @since 0.1.0
 */
ra_err_t ra_tls_global_init(void);

/**
 * @brief Symmetric tear-down for ``ra_tls_global_init``.
 *
 * @details
 * Frees every still-open session, wipes the CTR_DRBG state, and
 * marks the module uninitialized so a subsequent
 * ``ra_tls_global_init`` succeeds again.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Facade torn down.
 * @retval k_ra_err_not_initialized ``ra_tls_global_init`` was never called.
 *
 * @pre None (safe to call before any session open).
 * @pre Module was previously initialized.
 * @post Pool is empty and module is not initialized.
 * @post All ``mbedtls_*`` contexts freed.
 *
 * @note Not re-entrant.
 *
 * @see ra_tls_global_init()
 * @since 0.1.0
 */
ra_err_t ra_tls_global_deinit(void);

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
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Session allocated and ready for handshake.
 * @retval k_ra_err_invalid_arg     ``out_session`` or ``cfg`` is NULL,
 *                                  or one of the BIO callbacks is NULL.
 * @retval k_ra_err_not_initialized ``ra_tls_global_init`` was never called.
 * @retval k_ra_err_no_mem          Pool exhausted (more than
 *                                  ``k_ra_tls_max_sessions`` open).
 *
 * @pre ``ra_tls_global_init`` returned ``k_ra_ok`` previously.
 * @pre ``cfg->bio_send`` and ``cfg->bio_recv`` are non-NULL.
 * @post On ``k_ra_ok``, ``*out_session`` is non-NULL and survives until
 *       a matching ``ra_tls_session_close``.
 * @post On any error, ``*out_session`` is set to NULL.
 *
 * @note Not thread-safe; caller must serialise allocation against
 *       concurrent close.
 *
 * @see ra_tls_session_close()
 * @see ra_tls_handshake()
 * @since 0.1.0
 */
ra_err_t ra_tls_session_open(ra_tls_session_t* out_session, const ra_tls_session_cfg_t* cfg);

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
 *                        ``ra_tls_session_open``.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Slot released.
 * @retval k_ra_err_invalid_arg     ``session`` is NULL or does not
 *                                  point into the pool.
 * @retval k_ra_err_not_initialized ``ra_tls_global_init`` was never called.
 *
 * @pre ``session`` was returned by ``ra_tls_session_open``.
 * @pre Module is initialized.
 * @post Slot is free and may be re-issued.
 * @post No further use of ``session`` is permitted (use-after-free is
 *       caller's bug).
 *
 * @see ra_tls_session_open()
 * @since 0.1.0
 *
 * @note Not thread-safe unless documented otherwise.
 */
ra_err_t ra_tls_session_close(ra_tls_session_t session);

/**
 * @brief Iterative TLS handshake driver.
 *
 * @details
 * Wraps ``mbedtls_ssl_handshake`` and translates its return value into
 * an ``ra_err_t``. Returns ``k_ra_err_would_block`` while the
 * underlying BIO is non-blocking and waiting for I/O so the caller can
 * loop without consuming the entire transport-level event budget.
 *
 * In ``RA_SIMULATOR_MODE`` (host unit-test build) the call short-
 * circuits to ``k_ra_ok`` after a single BIO drain so the loopback
 * test path can complete without a real cryptographic handshake.
 *
 * @param[in,out] session Open session handle.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Handshake complete.
 * @retval k_ra_err_invalid_arg     ``session`` invalid.
 * @retval k_ra_err_not_initialized Module not initialized.
 * @retval k_ra_err_would_block     BIO is non-blocking; retry later.
 * @retval k_ra_err_comm_error      Mbed TLS reported a fatal handshake
 *                                  failure (cert / protocol / decode).
 *
 * @pre ``session`` is open.
 * @pre BIO callbacks have been bound (done by ``ra_tls_session_open``).
 * @post On ``k_ra_ok`` the session is in the application-data state.
 * @post On any non-would-block error the session must be closed.
 *
 * @see ra_tls_send()
 * @see ra_tls_recv()
 * @since 0.1.0
 *
 * @note Not thread-safe unless documented otherwise.
 */
ra_err_t ra_tls_handshake(ra_tls_session_t session);

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
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Wrote ``*out_sent`` bytes.
 * @retval k_ra_err_invalid_arg     Any pointer NULL or session invalid.
 * @retval k_ra_err_not_initialized Module not initialized.
 * @retval k_ra_err_would_block     Non-blocking BIO returned WANT_WRITE.
 * @retval k_ra_err_comm_error      Fatal TLS-layer error.
 *
 * @pre ``session`` has completed its handshake.
 * @pre ``buf`` is non-NULL when ``len > 0``.
 * @post On ``k_ra_ok`` ``*out_sent <= len``.
 * @post On any error ``*out_sent == 0``.
 *
 * @see ra_tls_recv()
 * @since 0.1.0
 *
 * @note Not thread-safe unless documented otherwise.
 */
ra_err_t ra_tls_send(ra_tls_session_t session, const uint8_t* buf, size_t len, size_t* out_sent);

/**
 * @brief Decrypt and receive application data.
 *
 * @details
 * Wraps ``mbedtls_ssl_read``. ``*out_received == 0`` together with
 * ``k_ra_ok`` denotes a clean peer close-notify; ``k_ra_err_would_block``
 * means the underlying transport had no data ready.
 *
 * @param[in,out] session      Open session handle in the application-data state.
 * @param[out]    buf          Plaintext output buffer.
 * @param[in]     len          Capacity of ``buf`` in bytes.
 * @param[out]    out_received Bytes decrypted into ``buf``.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Decrypted ``*out_received`` bytes
 *                                  (0 on clean close).
 * @retval k_ra_err_invalid_arg     Any pointer NULL or session invalid.
 * @retval k_ra_err_not_initialized Module not initialized.
 * @retval k_ra_err_would_block     No ciphertext available yet.
 * @retval k_ra_err_comm_error      Fatal TLS-layer error.
 *
 * @pre ``session`` has completed its handshake.
 * @pre ``buf`` is non-NULL when ``len > 0``.
 * @post ``*out_received <= len``.
 * @post On any error ``*out_received == 0``.
 *
 * @see ra_tls_send()
 * @since 0.1.0
 *
 * @note Not thread-safe unless documented otherwise.
 */
ra_err_t ra_tls_recv(ra_tls_session_t session, uint8_t* buf, size_t len, size_t* out_received);

#ifdef __cplusplus
}
#endif
