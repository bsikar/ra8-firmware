/**
 * @file esp32_c6_http_model.c
 * @brief Deterministic ESP-IDF and mbedTLS stand-ins for the C6 media adapter.
 * @details Implements exactly the upstream surface the production ESP32-C6
 * HTTP backend consumes, driven entirely by one scripted model record. No
 * network, TLS or real cryptography is involved: the digest is a byte sum, so
 * the vectors verify adapter streaming rather than SHA-256 itself.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp32_c6_http_model_internal.h"
#include "esp_idf_mdl_compat_internal.h"
#include "ra8_attributes.h"
#include "ra8_c6link_mdl.h"
#include "unity_minimal.h"

static struct esp_http_client s_client;
static c6_http_model_t        s_model;

/**
 * @brief Implementation of `priv_c6_http_model()`.
 */
RA8_PRIV c6_http_model_t* priv_c6_http_model(void)
{
  return &s_model;
}

/**
 * @brief Implementation of `priv_c6_http_client()`.
 */
RA8_PRIV struct esp_http_client* priv_c6_http_client(void)
{
  return &s_client;
}

/**
 * @brief Implementation of `priv_c6_http_model_reset()`.
 */
RA8_PRIV void priv_c6_http_model_reset(const uint8_t* body, size_t body_bytes)
{
  s_model = (c6_http_model_t){.body           = body,
                              .body_bytes     = body_bytes,
                              .content_length = (int64_t)body_bytes,
                              .status         = (int)k_c6_http_status_ok,
                              .complete       = true,
                              .retry_after    = "4",
                              .etag           = "\"esp-etag\"",
                              .last_modified  = "Wed, 21 Oct 2015 07:28:00 GMT",
                              .content_type   = "application/x-rabook",
                              .etag_key       = "eTaG"};
}

/**
 * @brief Select model storage for one request header
 * @param[in] key Canonical request-header name.
 * @param[out] capacity Selected storage capacity.
 * @return Selected model buffer, or null for an unexpected name.
 * @retval non-NULL The exact observed-header buffer.
 * @retval NULL The production adapter supplied an unknown header.
 * @pre @p key and @p capacity are non-null.
 * @pre @p key is NUL-terminated.
 * @post No model buffer is modified.
 * @post Success publishes the exact matching capacity.
 * @note Header names emitted by production use canonical case.
 * @since 0.1.0
 */
RA8_INTERNAL static char* internal_model_header_slot(const char* key, size_t* capacity)
{
  if (strcmp(key, "User-Agent") == 0) {
    *capacity = sizeof(s_model.user_agent);
    return s_model.user_agent;
  }
  if (strcmp(key, "Referer") == 0) {
    *capacity = sizeof(s_model.referer);
    return s_model.referer;
  }
  if (strcmp(key, "If-None-Match") == 0) {
    *capacity = sizeof(s_model.if_none_match);
    return s_model.if_none_match;
  }
  if (strcmp(key, "If-Modified-Since") == 0) {
    *capacity = sizeof(s_model.if_modified_since);
    return s_model.if_modified_since;
  }
  *capacity = 0U;
  return nullptr;
}

/**
 * @brief Emit one modelled response-header event exactly as supplied
 * @details Borrows the supplied strings into one synchronous event and calls
 * the production callback exactly as ESP-IDF would during header parsing. A
 * null name or value is passed through unchanged, which is how the malformed
 * header-event vectors reach the production guards.
 * @param[in] key Header name supplied to the production callback, or null.
 * @param[in] value Header value supplied to the production callback, or null.
 * @pre The retained client has a configured event callback.
 * @pre Non-null @p key and @p value are NUL-terminated.
 * @post Production state has consumed the complete synchronous event.
 * @post Model response strings remain borrowed and unmodified.
 * @note Uses mixed-case names to verify HTTP field-name matching.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_model_emit_header_raw(const char* key, const char* value)
{
  esp_http_client_event_t event = {.event_id     = HTTP_EVENT_ON_HEADER,
                                   .client       = &s_client,
                                   .user_data    = s_client.user_data,
                                   .header_key   = (char*)key,
                                   .header_value = (char*)value};
  (void)s_client.event_handler(&event);
}

/**
 * @brief Emit one modelled response-header event that carries a value
 * @details Treats a null value as "this header was not present at all", so a
 * deliberately valueless header must be emitted through
 * ::internal_model_emit_header_raw instead.
 * @param[in] key Header name supplied to the production callback.
 * @param[in] value Header value, or null to omit the header entirely.
 * @pre The retained client has a configured event callback.
 * @pre A non-null @p value is NUL-terminated.
 * @post A present header reaches the production callback exactly once.
 * @post An absent header produces no event.
 * @note Uses mixed-case names to verify HTTP field-name matching.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_model_emit_header(const char* key, const char* value)
{
  if (value == nullptr) {
    return;
  }
  internal_model_emit_header_raw(key, value);
}

/* ESP-IDF stand-ins implement the contracts declared by the compatibility
 * header. */
RA8_PRIV esp_err_t esp_crt_bundle_attach(void* conf)
{
  (void)conf;
  return ESP_OK;
}

RA8_PRIV esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config)
{
  TEST_ASSERT(config != nullptr);
  ++s_model.init_calls;
  if (s_model.init_fail) {
    return nullptr;
  }
  s_client = (struct esp_http_client){.event_handler = config->event_handler,
                                      .user_data     = config->user_data};
  return &s_client;
}

RA8_PRIV esp_err_t esp_http_client_close(esp_http_client_handle_t client)
{
  TEST_ASSERT(client == &s_client);
  ++s_model.close_calls;
  return ESP_OK;
}

RA8_PRIV esp_err_t esp_http_client_set_url(esp_http_client_handle_t client, const char* url)
{
  TEST_ASSERT(client == &s_client);
  TEST_ASSERT(url != nullptr);
  if (s_model.set_url_fail) {
    return ESP_FAIL;
  }
  client->url    = url;
  s_model.cursor = 0U;
  return ESP_OK;
}

RA8_PRIV esp_err_t esp_http_client_set_header(esp_http_client_handle_t client,
                                              const char*              key,
                                              const char*              value)
{
  TEST_ASSERT(client == &s_client);
  TEST_ASSERT(key != nullptr);
  TEST_ASSERT(value != nullptr);
  if (s_model.set_header_fail) {
    return ESP_FAIL;
  }
  size_t capacity = 0U;
  char*  slot     = internal_model_header_slot(key, &capacity);
  TEST_ASSERT(slot != nullptr);
  const size_t length = strnlen(value, capacity);
  TEST_ASSERT(length < capacity);
  (void)memcpy(slot, value, length + 1U);
  return ESP_OK;
}

RA8_PRIV esp_err_t esp_http_client_delete_header(esp_http_client_handle_t client, const char* key)
{
  TEST_ASSERT(client == &s_client);
  TEST_ASSERT(key != nullptr);
  size_t capacity = 0U;
  char*  slot     = internal_model_header_slot(key, &capacity);
  TEST_ASSERT(slot != nullptr);
  slot[0] = '\0';
  return ESP_OK;
}

RA8_PRIV esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout_ms)
{
  TEST_ASSERT(client == &s_client);
  TEST_ASSERT(timeout_ms > 0);
  if (s_model.set_timeout_fail) {
    return ESP_FAIL;
  }
  s_model.timeout_ms = timeout_ms;
  return ESP_OK;
}

RA8_PRIV esp_err_t esp_http_client_open(esp_http_client_handle_t client, int64_t write_len)
{
  TEST_ASSERT(client == &s_client);
  TEST_ASSERT_EQ(0, write_len);
  return s_model.open_fail ? ESP_FAIL : ESP_OK;
}

RA8_PRIV int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client)
{
  TEST_ASSERT(client == &s_client);
  internal_model_emit_header("rEtRy-AfTeR", s_model.retry_after);
  if (s_model.null_etag_value) {
    internal_model_emit_header_raw(s_model.etag_key, nullptr);
  } else {
    internal_model_emit_header(s_model.etag_key, s_model.etag);
  }
  internal_model_emit_header("lAsT-mOdIfIeD", s_model.last_modified);
  internal_model_emit_header("cOnTeNt-TyPe", s_model.content_type);
  return s_model.content_length;
}

RA8_PRIV int esp_http_client_get_status_code(esp_http_client_handle_t client)
{
  TEST_ASSERT(client == &s_client);
  return s_model.status;
}

RA8_PRIV int esp_http_client_read(esp_http_client_handle_t client, char* buffer, int len)
{
  TEST_ASSERT(client == &s_client);
  TEST_ASSERT(buffer != nullptr);
  if (s_model.read_fail) {
    return -1;
  }
  if (s_model.read_oversize) {
    return len + 1;
  }
  const size_t remaining = s_model.body_bytes - s_model.cursor;
  if (remaining == 0U) {
    return 0;
  }
  size_t count = remaining;
  if (count > (size_t)len) {
    count = (size_t)len;
  }
  (void)memcpy(buffer, &s_model.body[s_model.cursor], count);
  s_model.cursor += count;
  return (int)count;
}

RA8_PRIV bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client)
{
  TEST_ASSERT(client == &s_client);
  return s_model.complete;
}

RA8_PRIV void mbedtls_sha256_init(mbedtls_sha256_context* ctx)
{
  TEST_ASSERT(ctx != nullptr);
  *ctx = (mbedtls_sha256_context){};
}

RA8_PRIV int mbedtls_sha256_starts(mbedtls_sha256_context* ctx, int is224)
{
  TEST_ASSERT(ctx != nullptr);
  TEST_ASSERT_EQ(0, is224);
  ctx->opaque[0] = 0U;
  return s_model.sha_start_fail ? -1 : 0;
}

RA8_PRIV int
mbedtls_sha256_update(mbedtls_sha256_context* ctx, const unsigned char* input, size_t ilen)
{
  TEST_ASSERT(ctx != nullptr);
  TEST_ASSERT(input != nullptr);
  if (s_model.sha_update_fail) {
    return -1;
  }
  for (size_t i = 0U; i < ilen; ++i) {
    ctx->opaque[0] += input[i];
  }
  return 0;
}

RA8_PRIV int mbedtls_sha256_finish(mbedtls_sha256_context* ctx, unsigned char output[32])
{
  TEST_ASSERT(ctx != nullptr);
  TEST_ASSERT(output != nullptr);
  if (s_model.sha_finish_fail) {
    return -1;
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_mdl_sha256_bytes; ++i) {
    output[i] = (uint8_t)(ctx->opaque[0] + i);
  }
  return 0;
}
