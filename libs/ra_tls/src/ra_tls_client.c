/**
 * @file ra_tls_client.c
 * @brief Lifecycle, application-data send/recv, and close for the
 *        TLS 1.2 client.
 *
 * @details
 * Wraps ``ra_tls_record_encrypt`` / ``ra_tls_record_decrypt`` (defined
 * in ``ra_tls_record.c``) with the public lifecycle API declared in
 * ``ra_tls.h``. The handshake state machine itself lives in
 * ``ra_tls_handshake.c``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra_err.h"
#include "ra_sce.h"
#include "ra_tls.h"
#include "ra_tls_internal.h"

/* NOLINTBEGIN(readability-magic-numbers,readability-redundant-casting,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,readability-function-size,readability-function-cognitive-complexity,readability-identifier-naming) */

/**
 * @enum ra_tls_client_priv_t
 * @brief Internal sizing knobs for the client TU.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra_tls_priv_send_scratch = 1024U, /**< Outbound encrypted-record buffer. */
  k_ra_tls_priv_recv_scratch = 4096U, /**< Inbound record buffer. */
  k_ra_tls_priv_alert_bytes  = 2U,    /**< Alert payload size. */
} ra_tls_client_priv_t;

ra_err_t ra_tls_client_init(ra_tls_client_t* ctx, const ra_tls_config_t* cfg)
{
  if ((ctx == nullptr) || (cfg == nullptr)) {
    return k_ra_err_null_ptr;
  }
  if ((cfg->hostname == nullptr) || (cfg->expected_server_cert_sha256 == nullptr) ||
      (cfg->send == nullptr) || (cfg->recv == nullptr)) {
    return k_ra_err_null_ptr;
  }
  const uint32_t hlen = (uint32_t)strlen(cfg->hostname);
  if ((hlen == 0U) || (hlen > (uint32_t)k_ra_tls_max_hostname_bytes)) {
    return k_ra_err_invalid_arg;
  }

  (void)memset(ctx, 0, sizeof(*ctx));
  ctx->cfg            = *cfg;
  ctx->state          = k_ra_tls_state_idle;
  ctx->client_seq     = 0U;
  ctx->server_seq     = 0U;
  ctx->transcript_len = 0U;
  ctx->magic          = (uint32_t)k_ra_tls_ctx_magic;
  return k_ra_ok;
}

ra_err_t ra_tls_client_send(ra_tls_client_t* ctx, const uint8_t* plaintext, uint32_t len)
{
  if ((ctx == nullptr) || (plaintext == nullptr)) {
    return k_ra_err_null_ptr;
  }
  if (ctx->magic != (uint32_t)k_ra_tls_ctx_magic) {
    return k_ra_err_invalid_state;
  }
  if (len == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (ctx->state != k_ra_tls_state_established) {
    return k_ra_err_invalid_state;
  }

  uint8_t  rec[k_ra_tls_priv_send_scratch];
  uint32_t reclen = 0U;
  ra_err_t err    = ra_tls_record_encrypt(ctx,
                                          k_ra_tls_ct_application_data,
                                          plaintext,
                                          len,
                                          rec,
                                          (uint32_t)sizeof(rec),
                                          &reclen);
  if (err != k_ra_ok) {
    return err;
  }
  return ctx->cfg.send(ctx->cfg.transport_ctx, rec, reclen);
}

ra_err_t ra_tls_client_recv(ra_tls_client_t* ctx, uint8_t* buf, uint32_t max_len, uint32_t* got_len)
{
  if ((ctx == nullptr) || (buf == nullptr) || (got_len == nullptr)) {
    return k_ra_err_null_ptr;
  }
  if (max_len == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (ctx->magic != (uint32_t)k_ra_tls_ctx_magic) {
    return k_ra_err_invalid_state;
  }
  if (ctx->state != k_ra_tls_state_established) {
    return k_ra_err_invalid_state;
  }

  uint8_t  rec[k_ra_tls_priv_recv_scratch];
  uint32_t pulled = 0U;
  ra_err_t err    = ctx->cfg.recv(ctx->cfg.transport_ctx, rec, sizeof(rec), &pulled);
  if (err != k_ra_ok) {
    return err;
  }
  if (pulled < ((uint32_t)k_ra_tls_record_header_bytes + (uint32_t)k_ra_tls_gcm_explicit_iv_bytes +
                (uint32_t)k_ra_tls_gcm_tag_bytes)) {
    return k_ra_err_protocol_error;
  }
  ra_tls_content_type_t ct = k_ra_tls_ct_handshake;
  err                      = ra_tls_record_decrypt(ctx, rec, pulled, &ct, buf, max_len, got_len);
  if (err != k_ra_ok) {
    return err;
  }
  if (ct != k_ra_tls_ct_application_data) {
    /* Alerts and handshake messages on an established session are
     * out of scope for this minimal client. */
    return k_ra_err_protocol_error;
  }
  return k_ra_ok;
}

ra_err_t ra_tls_client_close(ra_tls_client_t* ctx)
{
  if (ctx == nullptr) {
    return k_ra_err_null_ptr;
  }
  if ((ctx->magic == (uint32_t)k_ra_tls_ctx_magic) && (ctx->state == k_ra_tls_state_established) &&
      (ctx->cfg.send != nullptr)) {
    /* Best-effort close_notify (RFC 5246 sec 7.2.1). Encrypted
     * because we are post-handshake. */
    const uint8_t alert[k_ra_tls_priv_alert_bytes] = {
      (uint8_t)k_ra_tls_alert_warning,
      (uint8_t)k_ra_tls_alert_close_notify,
    };
    uint8_t  rec[k_ra_tls_priv_send_scratch];
    uint32_t reclen = 0U;
    if (ra_tls_record_encrypt(ctx,
                              k_ra_tls_ct_alert,
                              alert,
                              (uint32_t)k_ra_tls_priv_alert_bytes,
                              rec,
                              (uint32_t)sizeof(rec),
                              &reclen) == k_ra_ok) {
      (void)ctx->cfg.send(ctx->cfg.transport_ctx, rec, reclen);
    }
  }

  /* Wipe key material before clearing the magic. */
  (void)memset(ctx->client_write_key, 0, sizeof(ctx->client_write_key));
  (void)memset(ctx->server_write_key, 0, sizeof(ctx->server_write_key));
  (void)memset(ctx->client_write_iv, 0, sizeof(ctx->client_write_iv));
  (void)memset(ctx->server_write_iv, 0, sizeof(ctx->server_write_iv));
  (void)memset(ctx->master_secret, 0, sizeof(ctx->master_secret));
  (void)memset(ctx->premaster, 0, sizeof(ctx->premaster));
  ctx->state = k_ra_tls_state_closed;
  ctx->magic = 0U;
  return k_ra_ok;
}

/* NOLINTEND(readability-magic-numbers,readability-redundant-casting,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,readability-function-size,readability-function-cognitive-complexity,readability-identifier-naming) */
