/**
 * @file esp_idf_mdl_compat_internal.h
 * @brief ESP-IDF media-service declarations for target builds and AST audits
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details ESP-IDF supplies the concrete declarations when the C6 component
 * is built. Repository-hosted annotation analysis deliberately runs without
 * an installed ESP-IDF SDK, so the non-target branch describes only the exact
 * subset of that SDK surface consumed by `mdl_service.c`. It has no runtime
 * implementation and is never selected by the ESP-IDF build.
 *
 * Keeping the audit declarations beside the adapter makes the dependency
 * explicit without pretending that ESP-IDF is a portable library or allowing
 * a missing vendor header to truncate the annotation checker call graph.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#if defined(ESP_PLATFORM)

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "mbedtls/sha256.h"

#else

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ra8_attributes.h"

/** @brief Opaque host-parser stand-in for an ESP-IDF HTTP client. */
typedef struct esp_http_client* esp_http_client_handle_t;

/** @brief HTTPS certificate-bundle attach callback used by ESP-IDF. */
typedef esp_err_t (*esp_crt_bundle_attach_fn_t)(void* conf);

/**
 * @brief Consumed ESP-IDF HTTP event identifiers.
 * @details The underlying type is stated explicitly so the audit mirror has
 * the same width as the plain (implicitly `int`-backed) ESP-IDF enum this
 * declaration stands in for.
 */
typedef enum esp_http_client_event_id_t : int32_t {
  HTTP_EVENT_ERROR = 0,    /**< Transport error.                  */
  HTTP_EVENT_ON_CONNECTED, /**< Connection established.           */
  HTTP_EVENT_HEADERS_SENT, /**< Request headers sent.             */
  HTTP_EVENT_ON_HEADER,    /**< One response header is available. */
  HTTP_EVENT_ON_DATA,      /**< Response body data is available.  */
  HTTP_EVENT_ON_FINISH,    /**< Response completed.               */
  HTTP_EVENT_DISCONNECTED, /**< Connection closed.                */
  HTTP_EVENT_REDIRECT,     /**< Redirect response observed.       */
} esp_http_client_event_id_t;

/** @brief Parser mirror of the ESP-IDF HTTP event record. */
typedef struct esp_http_client_event_t {
  esp_http_client_event_id_t event_id;     /**< Event selector.             */
  esp_http_client_handle_t   client;       /**< Originating client.         */
  void*                      data;         /**< Event data, if any.         */
  int                        data_len;     /**< Event-data bytes.           */
  void*                      user_data;    /**< Configuration context.      */
  char*                      header_key;   /**< Header name for ON_HEADER.  */
  char*                      header_value; /**< Header value for ON_HEADER. */
} esp_http_client_event_t;

/** @brief HTTP event callback signature consumed by the adapter. */
typedef esp_err_t (*esp_http_client_event_cb_t)(esp_http_client_event_t* event);

/**
 * @struct esp_http_client_config_t
 * @brief Exact consumed subset of ESP-IDF's HTTP client configuration.
 */
typedef struct esp_http_client_config_t {
  const char*                url;                   /**< Initial HTTPS URL.            */
  int                        timeout_ms;            /**< Operation timeout in ms.      */
  int                        buffer_size;           /**< Receive-buffer capacity.      */
  esp_crt_bundle_attach_fn_t crt_bundle_attach;     /**< Root-certificate attachment.  */
  bool                       disable_auto_redirect; /**< Redirects remain fail-closed. */
  esp_http_client_event_cb_t event_handler;         /**< Response event callback.      */
  void*                      user_data;             /**< Event callback context.       */
} esp_http_client_config_t;

/** @brief Parser-only storage standing in for ESP-IDF's SHA-256 context. */
typedef struct mbedtls_sha256_context {
  uint64_t opaque[32]; /**< Never interpreted outside the real ESP-IDF build. */
} mbedtls_sha256_context;

/*
 * The host parser/test build supplies cross-TU stand-ins for the vendor APIs.
 * Give those first-party stand-ins private identities while leaving the real
 * ESP-IDF build's symbol spellings and ABI untouched.
 */
/** @brief Map the parser bundle callback to its module-private stand-in. */
#define esp_crt_bundle_attach                     priv_esp_crt_bundle_attach
/** @brief Map the parser HTTP constructor to its module-private stand-in. */
#define esp_http_client_init                      priv_esp_http_client_init
/** @brief Map the parser HTTP close operation to its private stand-in. */
#define esp_http_client_close                     priv_esp_http_client_close
/** @brief Map the parser URL setter to its module-private stand-in. */
#define esp_http_client_set_url                   priv_esp_http_client_set_url
/** @brief Map the parser request-header setter to its private stand-in. */
#define esp_http_client_set_header                priv_esp_http_client_set_header
/** @brief Map the parser request-header remover to its private stand-in. */
#define esp_http_client_delete_header             priv_esp_http_client_delete_header
/** @brief Map the parser timeout setter to its private stand-in. */
#define esp_http_client_set_timeout_ms            priv_esp_http_client_set_timeout_ms
/** @brief Map the parser HTTP open operation to its private stand-in. */
#define esp_http_client_open                      priv_esp_http_client_open
/** @brief Map the parser header fetch to its module-private stand-in. */
#define esp_http_client_fetch_headers             priv_esp_http_client_fetch_headers
/** @brief Map the parser status accessor to its module-private stand-in. */
#define esp_http_client_get_status_code           priv_esp_http_client_get_status_code
/** @brief Map the parser body reader to its module-private stand-in. */
#define esp_http_client_read                      priv_esp_http_client_read
/** @brief Map the parser completion query to its module-private stand-in. */
#define esp_http_client_is_complete_data_received priv_esp_http_client_is_complete_data_received
/** @brief Map the parser SHA-256 initializer to its private stand-in. */
#define mbedtls_sha256_init                       priv_mbedtls_sha256_init
/** @brief Map the parser SHA-256 start operation to its private stand-in. */
#define mbedtls_sha256_starts                     priv_mbedtls_sha256_starts
/** @brief Map the parser SHA-256 update operation to its private stand-in. */
#define mbedtls_sha256_update                     priv_mbedtls_sha256_update
/** @brief Map the parser SHA-256 finalizer to its private stand-in. */
#define mbedtls_sha256_finish                     priv_mbedtls_sha256_finish

/**
 * @brief Attach ESP-IDF's certificate bundle to one TLS configuration
 * @details Parser-only mirror of the ESP-IDF callback consumed by the media adapter.
 * @param[in,out] conf ESP-IDF TLS configuration owned by the HTTP client.
 * @return ESP-IDF bundle-attachment status.
 * @retval ESP_OK The certificate bundle was attached.
 * @retval ESP_FAIL The bundle could not be attached.
 * @pre @p conf is a valid ESP-IDF TLS configuration.
 * @pre ESP-IDF certificate-bundle support is enabled.
 * @post Success leaves the configuration ready for certificate verification.
 * @post Failure does not claim a usable certificate bundle.
 * @note Implemented by ESP-IDF; this header supplies no host implementation.
 * @since 0.1.0
 */
RA8_PRIV esp_err_t esp_crt_bundle_attach(void* conf);

/**
 * @brief Create one ESP-IDF HTTP client
 * @details Parser-only mirror of the ESP-IDF constructor used by the C6 adapter.
 * @param[in] config Complete client configuration retained by ESP-IDF as required.
 * @return Opaque client handle or `nullptr` on failure.
 * @retval nullptr Client creation failed.
 * @pre @p config is non-null and its callback fields remain callable.
 * @pre The ESP-IDF networking subsystem is available.
 * @post Success returns a handle accepted by the remaining HTTP operations.
 * @post Failure creates no usable client handle.
 * @note Implemented by ESP-IDF and may allocate internally.
 * @since 0.1.0
 */
RA8_PRIV esp_http_client_handle_t
esp_http_client_init( // alloc-allow: parser mirror of ESP-IDF SOUP
  const esp_http_client_config_t* config);

/**
 * @brief Close the active response while retaining the HTTP client object
 * @details Parser-only mirror of ESP-IDF's per-connection close operation.
 * @param[in,out] client Valid retained client handle.
 * @return ESP-IDF close status.
 * @retval ESP_OK The active connection was closed.
 * @retval ESP_FAIL Closing the connection failed.
 * @pre @p client was returned by ::esp_http_client_init.
 * @pre No other task concurrently uses @p client.
 * @post Success leaves the handle available for a later open.
 * @post Failure does not report a cleanly closed connection.
 * @note Implemented by ESP-IDF and may release internal resources.
 * @since 0.1.0
 */
RA8_PRIV esp_err_t esp_http_client_close( // alloc-allow: parser mirror of ESP-IDF SOUP
  esp_http_client_handle_t client);

/**
 * @brief Replace the retained client's request URL
 * @details Parser-only mirror of ESP-IDF's URL-setting operation.
 * @param[in,out] client Valid retained client handle.
 * @param[in] url NUL-terminated absolute HTTPS URL.
 * @return ESP-IDF URL-setting status.
 * @retval ESP_OK The client accepted the URL.
 * @retval ESP_FAIL The URL could not be retained.
 * @pre @p client was returned by ::esp_http_client_init.
 * @pre @p url remains readable for the duration of the call.
 * @post Success makes @p url the next request target.
 * @post Failure leaves no claim about the retained request URL.
 * @note Implemented by ESP-IDF and may allocate internally.
 * @since 0.1.0
 */
RA8_PRIV esp_err_t esp_http_client_set_url( // alloc-allow: parser mirror of ESP-IDF SOUP
  esp_http_client_handle_t client,
  const char*              url);

/**
 * @brief Set one request header on the retained HTTP client
 * @details Mirrors the exact ESP-IDF operation used to apply one bounded
 * portable request-policy field before opening a GET.
 * @param[in,out] client Valid retained client handle.
 * @param[in] key NUL-terminated header name.
 * @param[in] value NUL-terminated header value.
 * @return ESP-IDF header status.
 * @retval ESP_OK The header was retained.
 * @retval ESP_FAIL The header could not be retained.
 * @pre All pointers are non-null and strings are NUL-terminated.
 * @pre No request is open on @p client.
 * @post Success applies the header to the next request.
 * @post Failure makes no header-retention claim.
 * @note Implemented by ESP-IDF and may allocate internally.
 * @since 0.1.0
 */
RA8_PRIV esp_err_t esp_http_client_set_header( // alloc-allow: parser mirror of ESP-IDF SOUP
  esp_http_client_handle_t client,
  const char*              key,
  const char*              value);

/**
 * @brief Remove one retained request header
 * @details Mirrors the ESP-IDF operation used to clear state that must not
 * leak from a prior request on the reused client handle.
 * @param[in,out] client Valid retained client handle.
 * @param[in] key NUL-terminated header name.
 * @return ESP-IDF header-removal status.
 * @retval ESP_OK The header is absent.
 * @retval ESP_FAIL The operation could not be completed.
 * @pre All pointers are non-null and @p key is NUL-terminated.
 * @pre No request is open on @p client.
 * @post Success leaves the named header absent.
 * @post Failure makes no header-state claim.
 * @note The adapter tolerates an already-absent header.
 * @since 0.1.0
 */
RA8_PRIV esp_err_t esp_http_client_delete_header(esp_http_client_handle_t client, const char* key);

/**
 * @brief Set the retained client's operation timeout
 * @details Mirrors the ESP-IDF operation used to apply the portable request
 * budget to the reused client before opening its connection.
 * @param[in,out] client Valid retained client handle.
 * @param[in] timeout_ms Positive timeout in milliseconds.
 * @return ESP-IDF timeout-setting status.
 * @retval ESP_OK The timeout was accepted.
 * @retval ESP_FAIL The timeout could not be applied.
 * @pre @p client is valid and @p timeout_ms is positive.
 * @pre No request is open on @p client.
 * @post Success applies the timeout to the next request.
 * @post Failure makes no timeout-state claim.
 * @note Implemented by ESP-IDF.
 * @since 0.1.0
 */
RA8_PRIV esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout_ms);

/**
 * @brief Open the configured HTTP request
 * @details Parser-only mirror of ESP-IDF's connection and request-header operation.
 * @param[in,out] client Configured retained client handle.
 * @param[in] write_len Request-body length; zero for this GET-only adapter.
 * @return ESP-IDF open status.
 * @retval ESP_OK The request was opened.
 * @retval ESP_FAIL Connection, TLS, or request setup failed.
 * @pre @p client has a valid HTTPS URL.
 * @pre No response is already open on @p client.
 * @post Success permits response-header and body reads.
 * @post Failure does not publish response bytes.
 * @note Implemented by ESP-IDF and may allocate during TLS setup.
 * @since 0.1.0
 */
RA8_PRIV esp_err_t esp_http_client_open( // alloc-allow: parser mirror of ESP-IDF SOUP
  esp_http_client_handle_t client,
  int64_t                  write_len);

/**
 * @brief Fetch response headers and the advertised body length
 * @details Parser-only mirror of ESP-IDF's synchronous header fetch.
 * @param[in,out] client Open client handle.
 * @return Non-negative content length or a negative unknown/error sentinel.
 * @retval -1 Content length is unknown or header fetch failed.
 * @pre ::esp_http_client_open succeeded for @p client.
 * @pre No other task concurrently reads @p client.
 * @post A non-negative result is the advertised Content-Length.
 * @post A negative result is never treated as a verified body length.
 * @note Implemented by ESP-IDF; status must be checked independently.
 * @since 0.1.0
 */
RA8_PRIV int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);

/**
 * @brief Return the HTTP response status code
 * @details Parser-only mirror of ESP-IDF's response-status accessor.
 * @param[in] client Client whose response headers were fetched.
 * @return HTTP status code, or zero when no valid response status exists.
 * @retval 0 No valid HTTP status is available.
 * @pre Response-header processing has completed for @p client.
 * @pre @p client is a valid retained handle.
 * @post The call does not consume response body bytes.
 * @post The client remains available for body reads.
 * @note Implemented by ESP-IDF.
 * @since 0.1.0
 */
RA8_PRIV int esp_http_client_get_status_code(esp_http_client_handle_t client);

/**
 * @brief Read at most one bounded span of response body bytes
 * @details Parser-only mirror of ESP-IDF's synchronous body reader.
 * @param[in,out] client Open response handle.
 * @param[out] buffer Caller-owned destination with capacity @p len.
 * @param[in] len Maximum bytes writable to @p buffer.
 * @return Positive byte count, zero at EOF, or a negative error.
 * @retval 0 No further body bytes were returned.
 * @retval -1 A body read failed.
 * @pre @p buffer is writable for @p len bytes.
 * @pre ::esp_http_client_fetch_headers completed for @p client.
 * @post A positive result initializes exactly that many bytes in @p buffer.
 * @post A non-positive result initializes no bytes claimed by the caller.
 * @note Implemented by ESP-IDF.
 * @since 0.1.0
 */
RA8_PRIV int esp_http_client_read(esp_http_client_handle_t client, char* buffer, int len);

/**
 * @brief Report whether ESP-IDF received the complete response body
 * @details Parser-only mirror used to distinguish verified EOF from truncation.
 * @param[in] client Response handle after a zero-length body read.
 * @return Completeness verdict.
 * @retval true ESP-IDF observed a complete response body.
 * @retval false The body is incomplete or response state is invalid.
 * @pre @p client is a valid retained handle.
 * @pre Body reading reached an apparent EOF.
 * @post The response body position is unchanged.
 * @post False never authorizes a completed download.
 * @note Implemented by ESP-IDF.
 * @since 0.1.0
 */
RA8_PRIV bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client);

/**
 * @brief Initialize one SHA-256 context
 * @details Parser-only mirror of mbedTLS context initialization.
 * @param[out] ctx Context storage to initialize.
 * @pre @p ctx is writable and suitably aligned.
 * @pre No active hash operation owns @p ctx.
 * @post @p ctx may be passed to ::mbedtls_sha256_starts.
 * @post No digest input has been consumed.
 * @note Implemented by the ESP-IDF-provided mbedTLS library.
 * @since 0.1.0
 */
RA8_PRIV void mbedtls_sha256_init(mbedtls_sha256_context* ctx);

/**
 * @brief Start a SHA-224 or SHA-256 operation
 * @details Parser-only mirror; the media adapter always requests SHA-256.
 * @param[in,out] ctx Initialized hash context.
 * @param[in] is224 Zero for SHA-256, non-zero for SHA-224.
 * @return mbedTLS status.
 * @retval 0 Hash initialization succeeded.
 * @pre ::mbedtls_sha256_init initialized @p ctx.
 * @pre No concurrent operation uses @p ctx.
 * @post Success resets the streaming hash state.
 * @post Failure does not authorize digest publication.
 * @note Implemented by the ESP-IDF-provided mbedTLS library.
 * @since 0.1.0
 */
RA8_PRIV int mbedtls_sha256_starts(mbedtls_sha256_context* ctx, int is224);

/**
 * @brief Add bytes to the active SHA operation
 * @details Parser-only mirror of mbedTLS's streaming hash update.
 * @param[in,out] ctx Active hash context.
 * @param[in] input Readable input bytes.
 * @param[in] ilen Number of bytes at @p input.
 * @return mbedTLS status.
 * @retval 0 All input bytes were incorporated.
 * @pre ::mbedtls_sha256_starts succeeded for @p ctx.
 * @pre @p input is readable for @p ilen bytes.
 * @post Success advances the hash by exactly @p ilen bytes.
 * @post Failure does not authorize digest publication.
 * @note Implemented by the ESP-IDF-provided mbedTLS library.
 * @since 0.1.0
 */
RA8_PRIV int
mbedtls_sha256_update(mbedtls_sha256_context* ctx, const unsigned char* input, size_t ilen);

/**
 * @brief Finalize the active SHA-256 operation
 * @details Parser-only mirror of mbedTLS's fixed-size digest output.
 * @param[in,out] ctx Active hash context.
 * @param[out] output Caller-owned 32-byte digest buffer.
 * @return mbedTLS status.
 * @retval 0 The digest was written successfully.
 * @pre ::mbedtls_sha256_starts succeeded for @p ctx.
 * @pre @p output is writable for 32 bytes.
 * @post Success initializes all 32 bytes at @p output.
 * @post Failure does not authorize use of @p output.
 * @note Implemented by the ESP-IDF-provided mbedTLS library.
 * @since 0.1.0
 */
RA8_PRIV int mbedtls_sha256_finish(mbedtls_sha256_context* ctx, unsigned char output[32]);

#endif
