/**
 * @file ra8_tls.c
 * @brief Implementation of the ``ra8_tls`` Mbed TLS facade.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Hosts the static session pool and the thin translation layer between
 * Mbed TLS return codes and ``ra8_err_t``. Randomness comes from the PSA
 * crypto layer (Mbed TLS 4.x), which the application seeds through its
 * ``mbedtls_psa_external_get_random`` hook.
 *
 * Mbed TLS is only linked into the firmware build when
 * ``RA8_USE_MBEDTLS=ON`` is set on the top-level CMake invocation. The
 * host unit-test build (``tests/CMakeLists.txt``) defines
 * ``RA8_OFF_TARGET`` for every translation unit and intentionally
 * does not link the heavy Mbed TLS object library; in that mode this
 * file replaces every ``mbedtls_ssl_*`` call with a tiny in-memory
 * stand-in that exercises the BIO callback contract end-to-end. The
 * public ``ra8_tls_*`` surface is identical in either build.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_tls.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"

/** @brief TLS record content type: Handshake (22). */
typedef enum : uint8_t {
  k_tls_content_handshake = 0x16U, /**< TLS content handshake. */
} tls_content_type_t;

#ifndef RA8_OFF_TARGET
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#endif

#ifdef RA8_OFF_TARGET
/** @brief Deterministic cipher-suite name reported on the off-target path. */
static const char* const s_ra8_tls_fake_cipher = "off-target-loopback";
#endif

/* =============================================================================
 * Logging tag
 * =============================================================================
 */

/** @brief Logging tag prefix used by every ``ra8_tls`` log line. */
static const char* const s_ra8_tls_tag = "ra8_tls";

/* =============================================================================
 * Pool slot definition
 * =============================================================================
 */

/**
 * @struct ra8_tls_session_handle
 * @brief Concrete pool slot backing one ``ra8_tls_session_t``.
 *
 * @details
 * One slot per concurrent session. ``in_use`` doubles as the bitmap
 * bit; the array index is the slot index ``[0, k_ra8_tls_max_sessions)``.
 *
 * @invariant ``in_use`` is true if and only if ``cfg.bio_send`` is non-NULL.
 */
struct ra8_tls_session_handle {
  /* Field order places the aligned cfg block first so the host
   * (RA8_OFF_TARGET) slot -- cfg, in_use, handshake_done -- carries no
   * excess padding (clang-analyzer-optin.performance.Padding). */
  ra8_tls_session_cfg_t cfg;    /**< Cached caller configuration. */
  bool                  in_use; /**< Slot allocated.              */
#ifndef RA8_OFF_TARGET
  mbedtls_ssl_context ssl;    /**< Mbed TLS SSL context.                */
  mbedtls_ssl_config  config; /**< Mbed TLS SSL configuration block.    */
  mbedtls_x509_crt    ca;     /**< Parsed trust anchor (if cfg.ca_pem). */
#else
  bool handshake_done; /**< Fake-only flag for the loopback test path. */
#endif /**< Endif. */
};

/** @brief Per-session state pool sized at compile time. */
static struct ra8_tls_session_handle s_session_pool[k_ra8_tls_max_sessions];

/** @brief One-shot global init flag protecting the PSA layer and pool state. */
static bool s_initialized;

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
 * @pre Module has been initialized.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_handle_valid(const struct ra8_tls_session_handle* session)
{
  if (session == nullptr) {
    return false;
  }
  const struct ra8_tls_session_handle* base = &s_session_pool[0];
  const struct ra8_tls_session_handle* end  = &s_session_pool[k_ra8_tls_max_sessions];
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
RA8_INTERNAL
static struct ra8_tls_session_handle* internal_pool_acquire(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_tls_max_sessions; ++i) {
    if (!s_session_pool[i].in_use) {
      return &s_session_pool[i];
    }
  }
  return nullptr; /**< Nullptr. */
}

/**
 * @brief Reset every pool slot to a known-clean state.
 *
 * @details
 * Used by both ``global_init`` and ``global_deinit`` so the pool
 * lifecycle is symmetric.
 *
 * @pre Module has been initialized.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_pool_reset(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_tls_max_sessions; ++i) {
    struct ra8_tls_session_handle* slot = &s_session_pool[i];
#ifndef RA8_OFF_TARGET
    if (slot->in_use) {
      mbedtls_ssl_free(&slot->ssl);
      mbedtls_ssl_config_free(&slot->config);
      mbedtls_x509_crt_free(&slot->ca);
    }
#endif
    (void)memset(slot, 0, sizeof(*slot));
  }
}

/**
 * @brief Bounded, always-NUL-terminating C-string copy.
 *
 * @details
 * Copies at most ``cap - 1`` bytes from ``src`` into ``dst`` and always
 * writes a terminating NUL, so ``dst`` is a valid C string on return even
 * when ``src`` is longer than the buffer (the tail is truncated). Used by
 * ::ra8_tls_get_cipher_suite to hand back the cipher-suite name inside the
 * caller's fixed buffer.
 *
 * @param[out] dst Destination buffer with room for ``cap`` bytes.
 * @param[in]  src NUL-terminated source string.
 * @param[in]  cap Capacity of ``dst`` in bytes; must be >= 1.
 *
 * @pre ``dst`` and ``src`` are non-NULL and ``cap >= 1``.
 * @pre ``src`` is NUL-terminated.
 * @post ``dst`` is NUL-terminated.
 * @post At most ``cap - 1`` source bytes were copied.
 *
 * @note Pure helper; NASA P10 Rule 2 loop bounded by ``cap - 1``.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_copy_cstr(char* dst, const char* src, size_t cap)
{
  const size_t last = cap - 1U;
  size_t       i    = 0U;
  while ((i < last) && (src[i] != '\0')) {
    dst[i] = src[i];
    ++i;
  }
  dst[i] = '\0';
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra8_err_t ra8_tls_global_init(void)
{
  if (s_initialized) {
    ra8_log_warn(s_ra8_tls_tag, "global_init called twice");
    return k_ra8_err_exists;
  }

  internal_pool_reset();

#ifndef RA8_OFF_TARGET
  /* Mbed TLS 4.x sources randomness from the PSA crypto layer
   * (``psa_generate_random``) rather than a facade-owned CTR_DRBG. Bring
   * PSA online here; the actual entropy is pulled lazily on the first
   * random draw through the application-supplied
   * ``mbedtls_psa_external_get_random`` hook (RSIP TRNG on hardware). */
  const psa_status_t psa_rc = psa_crypto_init();
  if (psa_rc != PSA_SUCCESS) {
    ra8_log_error(s_ra8_tls_tag, "psa_crypto_init failed");
    return k_ra8_err_hw_error;
  }
#endif

  s_initialized = true;
  ra8_log_info(s_ra8_tls_tag, "global_init ok");
  return k_ra8_ok;
}

ra8_err_t ra8_tls_global_deinit(void)
{
  if (!s_initialized) {
    return k_ra8_err_not_initialized;
  }

  internal_pool_reset();
  s_initialized = false;
  return k_ra8_ok;
}

/**
 * @brief Validate ``ra8_tls_session_open`` inputs before slot acquisition.
 *
 * @details
 * Returns the same error codes as the inlined original so the public
 * contract is preserved bit-for-bit; the helper exists purely to keep
 * the open function within the NASA P10 Rule 4 line budget.
 *
 * @param[in,out] out_session Caller's session handle out-parameter.
 * @param[in]     cfg         Caller-supplied session configuration.
 *
 * @return ``k_ra8_ok`` when arguments are valid, otherwise the matching
 *         error code.
 *
 * @retval k_ra8_ok                 Inputs valid.
 * @retval k_ra8_err_invalid_arg    NULL out pointer / cfg / BIO callbacks.
 * @retval k_ra8_err_not_initialized Module has not been initialized.
 *
 * @pre Caller has not yet acquired a pool slot.
 * @pre ``out_session`` may be NULL (handled by this helper).
 * @post On success no state is mutated.
 * @post On error ``*out_session`` is NULL when ``out_session`` is non-NULL.
 *
 * @note Pure validation helper; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_session_validate_args(ra8_tls_session_t*           out_session,
                                                const ra8_tls_session_cfg_t* cfg)
{
  if (out_session == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  *out_session = nullptr;

  if (!s_initialized) {
    return k_ra8_err_not_initialized;
  }
  if (cfg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if ((cfg->bio_send == nullptr) || (cfg->bio_recv == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

#ifndef RA8_OFF_TARGET
/**
 * @brief Apply the caller's verify-mode and optional trust anchor to a slot.
 *
 * @details
 * Translates ``cfg->verify_mode`` into the matching Mbed TLS authentication
 * mode and, when ``cfg->ca_pem`` is supplied, parses it into the slot's
 * ``mbedtls_x509_crt`` and binds it as the CA chain. A parse failure is
 * non-fatal here: the handshake still runs and reports the outcome through
 * ``ra8_tls_get_verify_result`` (a required verify then simply fails).
 * Hoisted out of ::internal_session_mbedtls_setup to keep that function
 * within the NASA P10 Rule 4 line cap.
 *
 * @param[in,out] slot Pool slot whose ``config`` is being built.
 * @param[in]     cfg  Caller-supplied session configuration.
 *
 * @pre ``slot->config`` has been initialised by ``mbedtls_ssl_config_init``.
 * @pre ``cfg`` is non-NULL (validated by the caller).
 * @post ``slot->ca`` is initialised and, on a valid PEM, bound as the chain.
 * @post The config authmode reflects ``cfg->verify_mode``.
 *
 * @note Not thread-safe; pool serialisation is the caller's job.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_apply_verify_cfg(struct ra8_tls_session_handle* slot,
                                      const ra8_tls_session_cfg_t*   cfg)
{
  int authmode = MBEDTLS_SSL_VERIFY_REQUIRED;
  switch (cfg->verify_mode) {
    case k_ra8_tls_verify_none:
      authmode = MBEDTLS_SSL_VERIFY_NONE;
      break;
    case k_ra8_tls_verify_optional:
      authmode = MBEDTLS_SSL_VERIFY_OPTIONAL;
      break;
    case k_ra8_tls_verify_default:
    case k_ra8_tls_verify_required:
    default:
      authmode = MBEDTLS_SSL_VERIFY_REQUIRED;
      break;
  }
  mbedtls_ssl_conf_authmode(&slot->config, authmode);

  mbedtls_x509_crt_init(&slot->ca);
  if ((cfg->ca_pem != nullptr) && (cfg->ca_pem_len > 0U)) {
    const int ca_rc =
      mbedtls_x509_crt_parse(&slot->ca, (const unsigned char*)cfg->ca_pem, cfg->ca_pem_len);
    if (ca_rc == 0) {
      mbedtls_ssl_conf_ca_chain(&slot->config, &slot->ca, nullptr);
    }
  }
}

/**
 * @brief Run the Mbed TLS init/config/setup sequence for a fresh slot.
 *
 * @details
 * On any sub-step failure the helper tears down the partial Mbed TLS
 * state and zeroes the slot so the caller can return the error without
 * leaking pool capacity. Hoisted out of ::ra8_tls_session_open to keep
 * the orchestrator within the line cap.
 *
 * @param[in,out] slot Pool slot freshly marked ``in_use``.
 * @param[in]     cfg  Caller-supplied session configuration.
 *
 * @return ``k_ra8_ok`` on success, ``k_ra8_err_hw_init_failed`` on any
 *         Mbed TLS sub-step failure.
 *
 * @retval k_ra8_ok                 Slot fully configured.
 * @retval k_ra8_err_hw_init_failed Mbed TLS rejected one of the calls.
 *
 * @pre ``slot->in_use`` is true and ``slot->cfg`` is the caller config.
 * @pre Module has been initialized.
 * @post On success the slot's SSL context is wired to the BIO callbacks.
 * @post On error the slot is fully zeroed.
 *
 * @note Not thread-safe; pool serialisation is the caller's job.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_session_mbedtls_setup(struct ra8_tls_session_handle* slot,
                                                const ra8_tls_session_cfg_t*   cfg)
{
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
    return k_ra8_err_hw_init_failed;
  }
  /* No ``mbedtls_ssl_conf_rng`` on Mbed TLS 4.x: the SSL layer draws random
   * bytes from PSA crypto, seeded through the app's external-RNG hook. */
  internal_apply_verify_cfg(slot, cfg);

  const int setup_rc = mbedtls_ssl_setup(&slot->ssl, &slot->config);
  if (setup_rc != 0) {
    mbedtls_ssl_free(&slot->ssl);
    mbedtls_ssl_config_free(&slot->config);
    mbedtls_x509_crt_free(&slot->ca);
    (void)memset(slot, 0, sizeof(*slot));
    return k_ra8_err_hw_init_failed;
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
  return k_ra8_ok;
}
#endif

ra8_err_t ra8_tls_session_open(ra8_tls_session_t* out_session, const ra8_tls_session_cfg_t* cfg)
{
  const ra8_err_t arg_rc = internal_session_validate_args(out_session, cfg);
  if (arg_rc != k_ra8_ok) {
    return arg_rc;
  }

  struct ra8_tls_session_handle* slot = internal_pool_acquire();
  if (slot == nullptr) {
    ra8_log_warn(s_ra8_tls_tag, "session pool exhausted");
    return k_ra8_err_no_mem;
  }

  slot->in_use = true;
  slot->cfg    = *cfg;

#ifndef RA8_OFF_TARGET
  const ra8_err_t setup_rc = internal_session_mbedtls_setup(slot, cfg);
  if (setup_rc != k_ra8_ok) {
    return setup_rc;
  }
#else
  slot->handshake_done = false;
#endif

  *out_session = slot;
  return k_ra8_ok;
}

ra8_err_t ra8_tls_session_close(ra8_tls_session_t session)
{
  if (!s_initialized) {
    return k_ra8_err_not_initialized;
  }
  if (!internal_handle_valid(session)) {
    return k_ra8_err_invalid_arg;
  }

#ifndef RA8_OFF_TARGET
  mbedtls_ssl_free(&session->ssl);
  mbedtls_ssl_config_free(&session->config);
  mbedtls_x509_crt_free(&session->ca);
#endif
  (void)memset(session, 0, sizeof(*session));
  return k_ra8_ok;
}

ra8_err_t ra8_tls_handshake(ra8_tls_session_t session)
{
  if (!s_initialized) {
    return k_ra8_err_not_initialized;
  }
  if (!internal_handle_valid(session)) {
    return k_ra8_err_invalid_arg;
  }

#ifndef RA8_OFF_TARGET
  const int rc = mbedtls_ssl_handshake(&session->ssl);
  if (rc == 0) {
    return k_ra8_ok;
  }
  if ((rc == MBEDTLS_ERR_SSL_WANT_READ) || (rc == MBEDTLS_ERR_SSL_WANT_WRITE)) {
    return k_ra8_err_would_block;
  }
  ra8_log_error(s_ra8_tls_tag, "handshake failed");
  return k_ra8_err_comm_error;
#else
  /* Off-target path: drive a single round-trip through the BIO callbacks
   * so the loopback test exercises the function-pointer plumbing without
   * a real TLS handshake. */
  uint8_t   fake_byte = k_tls_content_handshake;
  const int send_rc   = session->cfg.bio_send(session->cfg.bio_ctx, &fake_byte, 1U);
  if (send_rc < 0) {
    return k_ra8_err_comm_error;
  }
  uint8_t   recv_byte = 0U;
  const int recv_rc   = session->cfg.bio_recv(session->cfg.bio_ctx, &recv_byte, 1U);
  if (recv_rc < 0) {
    return k_ra8_err_comm_error;
  }
  session->handshake_done = true;
  return k_ra8_ok;
#endif
}

ra8_err_t ra8_tls_send(ra8_tls_session_t session, const uint8_t* buf, size_t len, size_t* out_sent)
{
  if (out_sent == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  *out_sent = 0U;

  if (!s_initialized) {
    return k_ra8_err_not_initialized;
  }
  if (!internal_handle_valid(session)) {
    return k_ra8_err_invalid_arg;
  }
  if ((buf == nullptr) && (len > 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if (len == 0U) {
    return k_ra8_ok;
  }

#ifndef RA8_OFF_TARGET
  const int rc = mbedtls_ssl_write(&session->ssl, buf, len);
  if (rc >= 0) {
    *out_sent = (size_t)rc;
    return k_ra8_ok;
  }
  if ((rc == MBEDTLS_ERR_SSL_WANT_READ) || (rc == MBEDTLS_ERR_SSL_WANT_WRITE)) {
    return k_ra8_err_would_block;
  }
  return k_ra8_err_comm_error;
#else
  const int rc = session->cfg.bio_send(session->cfg.bio_ctx, buf, len);
  if (rc < 0) {
    return k_ra8_err_comm_error;
  }
  *out_sent = (size_t)rc;
  return k_ra8_ok;
#endif
}

ra8_err_t ra8_tls_recv(ra8_tls_session_t session, uint8_t* buf, size_t len, size_t* out_received)
{
  if (out_received == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  *out_received = 0U;

  if (!s_initialized) {
    return k_ra8_err_not_initialized;
  }
  if (!internal_handle_valid(session)) {
    return k_ra8_err_invalid_arg;
  }
  if ((buf == nullptr) && (len > 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if (len == 0U) {
    return k_ra8_ok;
  }

#ifndef RA8_OFF_TARGET
  const int rc = mbedtls_ssl_read(&session->ssl, buf, len);
  if (rc >= 0) {
    *out_received = (size_t)rc;
    return k_ra8_ok;
  }
  if ((rc == MBEDTLS_ERR_SSL_WANT_READ) || (rc == MBEDTLS_ERR_SSL_WANT_WRITE)) {
    return k_ra8_err_would_block;
  }
  if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
    return k_ra8_ok;
  }
  return k_ra8_err_comm_error;
#else
  const int rc = session->cfg.bio_recv(session->cfg.bio_ctx, buf, len);
  if (rc < 0) {
    return k_ra8_err_comm_error;
  }
  *out_received = (size_t)rc;
  return k_ra8_ok;
#endif
}

ra8_err_t ra8_tls_get_cipher_suite(ra8_tls_session_t session,
                                   uint16_t*         out_id,
                                   char*             out_name,
                                   size_t            name_cap)
{
  if ((out_id == nullptr) || (out_name == nullptr) || (name_cap == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  *out_id     = 0U;
  out_name[0] = '\0';

  if (!s_initialized) {
    return k_ra8_err_not_initialized;
  }
  if (!internal_handle_valid(session)) {
    return k_ra8_err_invalid_arg;
  }

#ifndef RA8_OFF_TARGET
  const char* name = mbedtls_ssl_get_ciphersuite(&session->ssl);
  if (name != nullptr) {
    *out_id = (uint16_t)mbedtls_ssl_get_ciphersuite_id_from_ssl(&session->ssl);
    internal_copy_cstr(out_name, name, name_cap);
  }
  return k_ra8_ok;
#else
  internal_copy_cstr(out_name, s_ra8_tls_fake_cipher, name_cap);
  return k_ra8_ok;
#endif
}

ra8_err_t ra8_tls_get_verify_result(ra8_tls_session_t session, uint32_t* out_flags)
{
  if (out_flags == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  *out_flags = 0U;

  if (!s_initialized) {
    return k_ra8_err_not_initialized;
  }
  if (!internal_handle_valid(session)) {
    return k_ra8_err_invalid_arg;
  }

#ifndef RA8_OFF_TARGET
  *out_flags = mbedtls_ssl_get_verify_result(&session->ssl);
#endif
  return k_ra8_ok;
}

ra8_err_t ra8_tls_mss_clamp(uint16_t mtu, uint16_t* out_mss)
{
  if (out_mss == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  *out_mss = 0U;

  const uint16_t overhead =
    (uint16_t)((uint16_t)k_ra8_tls_ipv4_hdr_bytes + (uint16_t)k_ra8_tls_tcp_hdr_bytes);
  if ((mtu <= overhead) || ((mtu - overhead) < (int)k_ra8_tls_mss_min)) {
    return k_ra8_err_invalid_arg;
  }
  *out_mss = (uint16_t)(mtu - overhead);
  return k_ra8_ok;
}
