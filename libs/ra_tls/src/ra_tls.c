/**
 * @file ra_tls.c
 * @brief Implementation of the ``ra_tls`` Mbed TLS facade.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Hosts the static session pool, the global CTR_DRBG state, and the
 * thin translation layer between Mbed TLS return codes and ``ra_err_t``.
 *
 * Mbed TLS is only linked into the firmware build when
 * ``RA_USE_MBEDTLS=ON`` is set on the top-level CMake invocation. The
 * host unit-test build (``tests/CMakeLists.txt``) defines
 * ``RA_SIMULATOR_MODE`` for every translation unit and intentionally
 * does not link the heavy Mbed TLS object library; in that mode this
 * file replaces every ``mbedtls_ssl_*`` call with a tiny in-memory
 * stand-in that exercises the BIO callback contract end-to-end. The
 * public ``ra_tls_*`` surface is identical in either build.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_tls.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

/* NOLINTBEGIN(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity,clang-analyzer-optin.performance.Padding,misc-misplaced-const) */
#ifndef RA_SIMULATOR_MODE
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#endif

/* =============================================================================
 * Logging tag
 * =============================================================================
 */

/** @brief Logging tag prefix used by every ``ra_tls`` log line. */
static const char* const k_ra_tls_tag = "ra_tls";

/* =============================================================================
 * Pool slot definition
 * =============================================================================
 */

/**
 * @struct ra_tls_session_handle
 * @brief Concrete pool slot backing one ``ra_tls_session_t``.
 *
 * @details
 * One slot per concurrent session. ``in_use`` doubles as the bitmap
 * bit; the array index is the slot index ``[0, k_ra_tls_max_sessions)``.
 *
 * @invariant ``in_use`` is true if and only if ``cfg.bio_send`` is non-NULL.
 */
struct ra_tls_session_handle {
  bool                 in_use; /**< Slot allocated.                                */
  ra_tls_session_cfg_t cfg;    /**< Cached caller configuration.                   */
#ifndef RA_SIMULATOR_MODE
  mbedtls_ssl_context ssl;    /**< Mbed TLS SSL context.                          */
  mbedtls_ssl_config  config; /**< Mbed TLS SSL configuration block.              */
#else
  bool handshake_done; /**< Simulator-only flag for the loopback test path. */
#endif
};

/** @brief Per-session state pool sized at compile time. */
static struct ra_tls_session_handle s_session_pool[k_ra_tls_max_sessions];

/** @brief One-shot global init flag protecting CTR_DRBG and pool state. */
static bool s_initialised;

#ifndef RA_SIMULATOR_MODE
/** @brief Shared deterministic random byte generator. */
static mbedtls_ctr_drbg_context s_ctr_drbg;
/** @brief Backing entropy pool feeding ``s_ctr_drbg``. */
static mbedtls_entropy_context s_entropy;
#endif

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Validate that a typed handle points into the static pool.
 *
 * @details
 * Guards every public API against forged or NULL handles. The check
 * is pointer-arithmetic-only so it stays branch-light at -O2.
 *
 * @param[in] session Handle to validate.
 *
 * @return ``true`` when ``session`` resolves to an in-use pool slot.
 *
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static bool internal_handle_valid(const ra_tls_session_t session)
{
  if (session == nullptr) {
    return false;
  }
  const struct ra_tls_session_handle* base = &s_session_pool[0];
  const struct ra_tls_session_handle* end  = &s_session_pool[k_ra_tls_max_sessions];
  if ((session < base) || (session >= end)) {
    return false;
  }
  return session->in_use;
}

/**
 * @brief Locate the first free slot in the pool.
 *
 * @return Pointer to a free slot, or NULL when the pool is exhausted.
 */
static struct ra_tls_session_handle* internal_pool_acquire(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra_tls_max_sessions; ++i) {
    if (!s_session_pool[i].in_use) {
      return &s_session_pool[i];
    }
  }
  return nullptr;
}

/**
 * @brief Reset every pool slot to a known-clean state.
 *
 * @details
 * Used by both ``global_init`` and ``global_deinit`` so the pool
 * lifecycle is symmetric.
 *
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_pool_reset(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra_tls_max_sessions; ++i) {
    struct ra_tls_session_handle* slot = &s_session_pool[i];
#ifndef RA_SIMULATOR_MODE
    if (slot->in_use) {
      mbedtls_ssl_free(&slot->ssl);
      mbedtls_ssl_config_free(&slot->config);
    }
#endif
    (void)memset(slot, 0, sizeof(*slot));
  }
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

/* Ra tls global init -- see implementation for details. */
ra_err_t ra_tls_global_init(void)
{
  if (s_initialised) {
    ra_log_warn(k_ra_tls_tag, "global_init called twice");
    return k_ra_err_exists;
  }

  internal_pool_reset();

#ifndef RA_SIMULATOR_MODE
  mbedtls_entropy_init(&s_entropy);
  mbedtls_ctr_drbg_init(&s_ctr_drbg);
  const int seed_rc =
    mbedtls_ctr_drbg_seed(&s_ctr_drbg, mbedtls_entropy_func, &s_entropy, nullptr, 0U);
  if (seed_rc != 0) {
    mbedtls_ctr_drbg_free(&s_ctr_drbg);
    mbedtls_entropy_free(&s_entropy);
    ra_log_error(k_ra_tls_tag, "ctr_drbg_seed failed");
    return k_ra_err_hw_error;
  }
#endif

  s_initialised = true;
  ra_log_info(k_ra_tls_tag, "global_init ok");
  return k_ra_ok;
}

/* Ra tls global deinit -- see implementation for details. */
ra_err_t ra_tls_global_deinit(void)
{
  if (!s_initialised) {
    return k_ra_err_not_initialized;
  }

  internal_pool_reset();

#ifndef RA_SIMULATOR_MODE
  mbedtls_ctr_drbg_free(&s_ctr_drbg);
  mbedtls_entropy_free(&s_entropy);
#endif

  s_initialised = false;
  return k_ra_ok;
}

/* Ra tls session open -- see implementation for details. */
ra_err_t ra_tls_session_open(ra_tls_session_t* out_session, const ra_tls_session_cfg_t* cfg)
{
  if (out_session == nullptr) {
    return k_ra_err_invalid_arg;
  }
  *out_session = nullptr;

  if (!s_initialised) {
    return k_ra_err_not_initialized;
  }
  if (cfg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if ((cfg->bio_send == nullptr) || (cfg->bio_recv == nullptr)) {
    return k_ra_err_invalid_arg;
  }

  struct ra_tls_session_handle* slot = internal_pool_acquire();
  if (slot == nullptr) {
    ra_log_warn(k_ra_tls_tag, "session pool exhausted");
    return k_ra_err_no_mem;
  }

  slot->in_use = true;
  slot->cfg    = *cfg;

#ifndef RA_SIMULATOR_MODE
  mbedtls_ssl_init(&slot->ssl);
  mbedtls_ssl_config_init(&slot->config);

  const int defaults_rc = mbedtls_ssl_config_defaults(&slot->config,
                                                      MBEDTLS_SSL_IS_CLIENT,
                                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                                      MBEDTLS_SSL_PRESET_DEFAULT);
  if (defaults_rc != 0) {
    mbedtls_ssl_free(&slot->ssl);
    mbedtls_ssl_config_free(&slot->config);
    (void)memset(slot, 0, sizeof(*slot));
    return k_ra_err_hw_init_failed;
  }
  mbedtls_ssl_conf_rng(&slot->config, mbedtls_ctr_drbg_random, &s_ctr_drbg);

  const int setup_rc = mbedtls_ssl_setup(&slot->ssl, &slot->config);
  if (setup_rc != 0) {
    mbedtls_ssl_free(&slot->ssl);
    mbedtls_ssl_config_free(&slot->config);
    (void)memset(slot, 0, sizeof(*slot));
    return k_ra_err_hw_init_failed;
  }

  if (cfg->server_name != nullptr) {
    (void)mbedtls_ssl_set_hostname(&slot->ssl, cfg->server_name);
  }
  /* Mbed TLS BIO send/recv signatures take ``unsigned char`` buffers; our
   * facade-public typedefs use ``uint8_t`` so the cast below is layout-safe
   * (the two types are identical on every supported target). */
  mbedtls_ssl_set_bio(&slot->ssl,
                      slot->cfg.bio_ctx,
                      (mbedtls_ssl_send_t*)cfg->bio_send,
                      (mbedtls_ssl_recv_t*)cfg->bio_recv,
                      nullptr);
#else
  slot->handshake_done = false;
#endif

  *out_session = slot;
  return k_ra_ok;
}

/* Ra tls session close -- see implementation for details. */
ra_err_t ra_tls_session_close(ra_tls_session_t session)
{
  if (!s_initialised) {
    return k_ra_err_not_initialized;
  }
  if (!internal_handle_valid(session)) {
    return k_ra_err_invalid_arg;
  }

#ifndef RA_SIMULATOR_MODE
  mbedtls_ssl_free(&session->ssl);
  mbedtls_ssl_config_free(&session->config);
#endif
  (void)memset(session, 0, sizeof(*session));
  return k_ra_ok;
}

/* Ra tls handshake -- see implementation for details. */
ra_err_t ra_tls_handshake(ra_tls_session_t session)
{
  if (!s_initialised) {
    return k_ra_err_not_initialized;
  }
  if (!internal_handle_valid(session)) {
    return k_ra_err_invalid_arg;
  }

#ifndef RA_SIMULATOR_MODE
  const int rc = mbedtls_ssl_handshake(&session->ssl);
  if (rc == 0) {
    return k_ra_ok;
  }
  if ((rc == MBEDTLS_ERR_SSL_WANT_READ) || (rc == MBEDTLS_ERR_SSL_WANT_WRITE)) {
    return k_ra_err_would_block;
  }
  ra_log_error(k_ra_tls_tag, "handshake failed");
  return k_ra_err_comm_error;
#else
  /* Simulator path: drive a single round-trip through the BIO callbacks
   * so the loopback test exercises the function-pointer plumbing without
   * a real TLS handshake. */
  uint8_t   sim_byte = 0x16U;
  const int send_rc  = session->cfg.bio_send(session->cfg.bio_ctx, &sim_byte, 1U);
  if (send_rc < 0) {
    return k_ra_err_comm_error;
  }
  uint8_t   recv_byte = 0U;
  const int recv_rc   = session->cfg.bio_recv(session->cfg.bio_ctx, &recv_byte, 1U);
  if (recv_rc < 0) {
    return k_ra_err_comm_error;
  }
  session->handshake_done = true;
  return k_ra_ok;
#endif
}

/* Ra tls send -- see implementation for details. */
ra_err_t ra_tls_send(ra_tls_session_t session, const uint8_t* buf, size_t len, size_t* out_sent)
{
  if (out_sent == nullptr) {
    return k_ra_err_invalid_arg;
  }
  *out_sent = 0U;

  if (!s_initialised) {
    return k_ra_err_not_initialized;
  }
  if (!internal_handle_valid(session)) {
    return k_ra_err_invalid_arg;
  }
  if ((buf == nullptr) && (len > 0U)) {
    return k_ra_err_invalid_arg;
  }
  if (len == 0U) {
    return k_ra_ok;
  }

#ifndef RA_SIMULATOR_MODE
  const int rc = mbedtls_ssl_write(&session->ssl, buf, len);
  if (rc >= 0) {
    *out_sent = (size_t)rc;
    return k_ra_ok;
  }
  if ((rc == MBEDTLS_ERR_SSL_WANT_READ) || (rc == MBEDTLS_ERR_SSL_WANT_WRITE)) {
    return k_ra_err_would_block;
  }
  return k_ra_err_comm_error;
#else
  const int rc = session->cfg.bio_send(session->cfg.bio_ctx, buf, len);
  if (rc < 0) {
    return k_ra_err_comm_error;
  }
  *out_sent = (size_t)rc;
  return k_ra_ok;
#endif
}

/* Ra tls recv -- see implementation for details. */
ra_err_t ra_tls_recv(ra_tls_session_t session, uint8_t* buf, size_t len, size_t* out_received)
{
  if (out_received == nullptr) {
    return k_ra_err_invalid_arg;
  }
  *out_received = 0U;

  if (!s_initialised) {
    return k_ra_err_not_initialized;
  }
  if (!internal_handle_valid(session)) {
    return k_ra_err_invalid_arg;
  }
  if ((buf == nullptr) && (len > 0U)) {
    return k_ra_err_invalid_arg;
  }
  if (len == 0U) {
    return k_ra_ok;
  }

#ifndef RA_SIMULATOR_MODE
  const int rc = mbedtls_ssl_read(&session->ssl, buf, len);
  if (rc >= 0) {
    *out_received = (size_t)rc;
    return k_ra_ok;
  }
  if ((rc == MBEDTLS_ERR_SSL_WANT_READ) || (rc == MBEDTLS_ERR_SSL_WANT_WRITE)) {
    return k_ra_err_would_block;
  }
  if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
    return k_ra_ok;
  }
  return k_ra_err_comm_error;
#else
  const int rc = session->cfg.bio_recv(session->cfg.bio_ctx, buf, len);
  if (rc < 0) {
    return k_ra_err_comm_error;
  }
  *out_received = (size_t)rc;
  return k_ra_ok;
#endif
}

/* NOLINTEND(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity,clang-analyzer-optin.performance.Padding,misc-misplaced-const) */
