/**
 * @file mdl_service.c
 * @brief ESP-IDF HTTP backend and synchronous CustomRpc hook for media chunks.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "mbedtls/sha256.h"
#include "ra8_c6link_mdl_msg.h"

/** @brief Single remote transfer owned by the ESP-hosted RPC task. */
typedef struct {
  esp_http_client_handle_t http;
  mbedtls_sha256_context   sha;
  char                     url[RA8_MDL_URL_MAX];
  uint64_t                 total;
  uint64_t                 received;
  bool                     opened;
  bool                     hashing;
} mdl_http_state_t;

static mdl_http_state_t  s_http;
static ra8_mdl_service_t s_service;
static bool              s_initialised;

/** @brief Link-time identity for the first-party component ABI. */
uint32_t ra8_mdl_service_component_abi(void);

typedef uint32_t (*mdl_component_abi_fn_t)(void);

__attribute__((noinline)) uint32_t ra8_mdl_service_component_abi(void)
{
  return 1U;
}

/* The volatile indirection is intentional: it keeps a relocation to the ABI
 * marker through optimization and --gc-sections, making the post-link symbol
 * assertion meaningful in release images. */
static mdl_component_abi_fn_t volatile s_component_abi = ra8_mdl_service_component_abi;

/** @brief Release every ESP-IDF resource associated with the current request. */
static void mdl_http_reset(mdl_http_state_t* state)
{
  if (state->http != nullptr) {
    (void)esp_http_client_close(state->http);
    (void)esp_http_client_cleanup(state->http);
  }
  if (state->hashing) {
    mbedtls_sha256_free(&state->sha);
  }
  *state = (mdl_http_state_t){};
}

static ra8_err_t mdl_http_begin(void* ctx, const char* url, ra8_mdl_format_t format)
{
  mdl_http_state_t* state            = (mdl_http_state_t*)ctx;
  const size_t      https_prefix_len = sizeof("https://") - 1U;
  if ((url == nullptr) || (strncmp(url, "https://", https_prefix_len) != 0) ||
      (url[https_prefix_len] == '\0') || (format < k_ra8_mdl_format_rabook) ||
      (format > k_ra8_mdl_format_epub)) {
    return k_ra8_err_invalid_arg;
  }
  mdl_http_reset(state);
  const size_t len = strnlen(url, sizeof(state->url));
  if ((len == 0U) || (len >= sizeof(state->url))) {
    return k_ra8_err_invalid_size;
  }
  memcpy(state->url, url, len + 1U);
  const esp_http_client_config_t cfg = {
    .url                   = state->url,
    .timeout_ms            = 15000,
    .buffer_size           = RA8_MDL_CHUNK_DATA_MAX,
    .crt_bundle_attach     = esp_crt_bundle_attach,
    .disable_auto_redirect = true,
  };
  state->http = esp_http_client_init(&cfg);
  if (state->http == nullptr) {
    mdl_http_reset(state);
    return k_ra8_err_no_mem;
  }
  mbedtls_sha256_init(&state->sha);
  if (mbedtls_sha256_starts(&state->sha, 0) != 0) {
    mdl_http_reset(state);
    return k_ra8_fail;
  }
  state->hashing = true;
  return k_ra8_ok;
}

/** @brief Open lazily so Start returns without waiting for a network exchange. */
static ra8_err_t mdl_http_open(mdl_http_state_t* state)
{
  if (state->opened) {
    return k_ra8_ok;
  }
  if (esp_http_client_open(state->http, 0) != ESP_OK) {
    return k_ra8_fail;
  }
  const int64_t content_length = esp_http_client_fetch_headers(state->http);
  const int     status         = esp_http_client_get_status_code(state->http);
  if ((status < 200) || (status >= 300)) {
    return k_ra8_err_protocol_error;
  }
  state->total  = (content_length > 0) ? (uint64_t)content_length : 0U;
  state->opened = true;
  return k_ra8_ok;
}

static ra8_err_t mdl_http_read(void*     ctx,
                               uint8_t*  out,
                               uint16_t  cap,
                               uint16_t* got,
                               uint64_t* total_bytes,
                               bool*     complete,
                               uint8_t   sha256[RA8_MDL_SHA256_BYTES])
{
  *got                     = 0U;
  *total_bytes             = 0U;
  *complete                = false;
  mdl_http_state_t* state  = (mdl_http_state_t*)ctx;
  const ra8_err_t   opened = mdl_http_open(state);
  if (opened != k_ra8_ok) {
    mdl_http_reset(state);
    return opened;
  }
  const int read = esp_http_client_read(state->http, (char*)out, cap);
  if ((read < 0) || ((uint32_t)read > (uint32_t)cap)) {
    mdl_http_reset(state);
    return k_ra8_fail;
  }
  if (read > 0) {
    if (state->received > (UINT64_MAX - (uint64_t)read)) {
      mdl_http_reset(state);
      return k_ra8_err_invalid_size;
    }
    if (mbedtls_sha256_update(&state->sha, out, (size_t)read) != 0) {
      mdl_http_reset(state);
      return k_ra8_fail;
    }
    state->received += (uint64_t)read;
    *got         = (uint16_t)read;
    *total_bytes = state->total;
  } else {
    const bool body_complete  = esp_http_client_is_complete_data_received(state->http);
    const bool length_matches = (state->total == 0U) || (state->received == state->total);
    if ((!body_complete) || (!length_matches)) {
      mdl_http_reset(state);
      return k_ra8_err_protocol_error;
    }
    if (mbedtls_sha256_finish(&state->sha, sha256) != 0) {
      mdl_http_reset(state);
      return k_ra8_fail;
    }
    /* A close-delimited or chunked response has no advertised length. Publish
     * the independently counted length in its final COMPLETE response. */
    if (state->total == 0U) {
      state->total = state->received;
    }
    *total_bytes = state->total;
    *complete    = true;
    mdl_http_reset(state);
  }
  return k_ra8_ok;
}

static ra8_err_t mdl_http_cancel(void* ctx)
{
  mdl_http_reset((mdl_http_state_t*)ctx);
  return k_ra8_ok;
}

/**
 * @brief Strong implementation consumed by the pinned ESP-hosted patch.
 *
 * @details The hook runs on ESP-hosted's serialized control path. It returns
 * a fully packed inner protobuf in caller-owned storage; the patched upstream
 * handler immediately wraps those bytes in its generated CustomRpc response.
 */
esp_err_t esp_hosted_custom_rpc_sync_handler(uint32_t       message_id,
                                             const uint8_t* request,
                                             size_t         request_len,
                                             uint8_t*       response,
                                             size_t         response_cap,
                                             size_t*        response_len)
{
  if (s_component_abi() != 1U) {
    return ESP_FAIL;
  }
  if (!s_initialised) {
    const ra8_mdl_service_backend_t backend = {.begin  = mdl_http_begin,
                                               .read   = mdl_http_read,
                                               .cancel = mdl_http_cancel,
                                               .ctx    = &s_http};
    if (ra8_mdl_service_init(&s_service, &backend) != k_ra8_ok) {
      return ESP_FAIL;
    }
    s_initialised = true;
  }
  const ra8_err_t result = ra8_mdl_service_dispatch(&s_service,
                                                    message_id,
                                                    request,
                                                    request_len,
                                                    response,
                                                    response_cap,
                                                    response_len);
  return (result == k_ra8_ok) ? ESP_OK : (esp_err_t)result;
}
