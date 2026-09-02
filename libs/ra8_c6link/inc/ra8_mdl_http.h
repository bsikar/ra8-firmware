/**
 * @file ra8_mdl_http.h
 * @brief Wire-neutral media HTTP request and response records.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details Shared by the RA8 client, portable C6 service, and concrete HTTP
 * backend without importing either endpoint's transport implementation.
 * Strings remain caller-owned on requests; response strings use fixed storage.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

#include "mdl_format.h"
#include "ra8_mdl_protocol.h"

/**
 * @struct ra8_mdl_http_policy_t
 * @brief Bounded request policy forwarded to the C6 HTTP client.
 * @invariant Null and empty strings both mean that the header is absent.
 * @invariant Nonempty strings contain no CR or LF characters.
 * @invariant `timeout_ms == 0` selects the C6 backend default.
 * @since 0.1.0
 */
typedef struct {
  const char* user_agent;        /**< User-Agent value, or null/empty to omit.    */
  const char* referer;           /**< Referer value, or null/empty to omit.       */
  const char* if_none_match;     /**< If-None-Match value, or null/empty to omit. */
  const char* if_modified_since; /**< If-Modified-Since value, or null/empty.     */
  uint32_t    timeout_ms;        /**< Whole-request timeout, or zero for default. */
} ra8_mdl_http_policy_t;

/**
 * @struct ra8_mdl_http_response_t
 * @brief HTTP status and selected response headers proven by the C6 backend.
 * @invariant `status` is in the inclusive range 100..599 on success.
 * @invariant Every array is NUL-terminated, including when its header is absent.
 * @since 0.1.0
 */
typedef struct {
  int32_t status;                                   /**< Final HTTP status.      */
  char    retry_after[k_ra8_mdl_retry_after_max];   /**< Retry-After or empty.   */
  char    etag[k_ra8_mdl_etag_max];                 /**< ETag or empty.          */
  char    last_modified[k_ra8_mdl_http_date_max];   /**< Last-Modified or empty. */
  char    content_type[k_ra8_mdl_content_type_max]; /**< Content-Type or empty.  */
} ra8_mdl_http_response_t;

/**
 * @struct ra8_mdl_request_t
 * @brief Complete typed HTTPS request accepted by protocol version 3.
 * @invariant `url` is a nonempty HTTPS URL shorter than ::k_ra8_mdl_url_max.
 * @invariant `format` is one concrete ::mdl_format_t value through RABOOK.
 * @since 0.1.0
 */
typedef struct {
  const char*           url;    /**< Absolute HTTPS source URL.        */
  mdl_format_t          format; /**< Exact returned artifact identity. */
  ra8_mdl_http_policy_t http;   /**< Forwarded request policy.         */
} ra8_mdl_request_t;
