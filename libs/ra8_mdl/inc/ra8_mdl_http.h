/**
 * @file ra8_mdl_http.h
 * @brief Transport-neutral media HTTP request policy and response record.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details One home for the HTTP contract every media downloader backend
 * speaks: the bounded request policy a caller forwards, the status and header
 * metadata a transfer reports back, and the capacities that bound both. The
 * header names no transport, so the C6 RPC endpoint, a host libcurl backend
 * and a test double all include it without importing each other.
 *
 * Request policy strings stay caller-owned and are read only for the duration
 * of the call they are passed to; response strings use fixed storage sized by
 * ::ra8_mdl_http_capacity_t.
 *
 * @see ra8_mdl_request.h  The C6 protocol request record built on this policy.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

/**
 * @brief Bounded capacities of the HTTP header fields this contract carries.
 * @details Every size includes the terminating NUL. The four response
 * capacities size the arrays in ::ra8_mdl_http_response_t directly; the two
 * request capacities bound what a backend may pack out of
 * ::ra8_mdl_http_policy_t.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_mdl_user_agent_max   = 256U,   /**< Maximum User-Agent size, including NUL.     */
  k_ra8_mdl_referer_max      = 512U,   /**< Maximum Referer size, including NUL.        */
  k_ra8_mdl_etag_max         = 128U,   /**< Maximum ETag size, including NUL.           */
  k_ra8_mdl_http_date_max    = 64U,    /**< Maximum HTTP-date size, including NUL.      */
  k_ra8_mdl_retry_after_max  = 64U,    /**< Maximum Retry-After size, including NUL.    */
  k_ra8_mdl_content_type_max = 128U,   /**< Maximum Content-Type size, including NUL.   */
  k_ra8_mdl_timeout_ms_max   = 60000U, /**< Maximum caller-selected HTTP timeout in ms. */
} ra8_mdl_http_capacity_t;

/**
 * @brief Inclusive bounds of the HTTP status codes this contract admits.
 * @details RFC 9110 assigns status codes the three-digit range 100..599, so a
 * response outside it is malformed rather than merely unsuccessful. A backend
 * rejects such a response instead of forwarding it.
 * @invariant ::k_ra8_mdl_http_status_min <= ::k_ra8_mdl_http_status_max.
 * @see ra8_mdl_http_response_t
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_mdl_http_status_min = 100U, /**< Lowest well-formed HTTP status code.  */
  k_ra8_mdl_http_status_max = 599U, /**< Highest well-formed HTTP status code. */
} ra8_mdl_http_status_bound_t;

/**
 * @struct ra8_mdl_http_policy_t
 * @brief Bounded request policy forwarded to a media HTTP backend.
 * @invariant Null and empty strings both mean that the header is absent.
 * @invariant Nonempty strings contain no CR or LF characters.
 * @invariant `timeout_ms == 0` selects the backend default; a nonzero value is
 *            at most ::k_ra8_mdl_timeout_ms_max.
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
 * @brief HTTP status and selected response headers reported by a backend.
 * @invariant `status == 0` means no HTTP status was observed, whether because
 *            the transport failed before a response or the call was refused.
 * @invariant A nonzero `status` is within ::k_ra8_mdl_http_status_min ..
 *            ::k_ra8_mdl_http_status_max inclusive.
 * @invariant Every array is NUL-terminated, including when its header is
 *            absent, in which case the first byte is NUL.
 *
 * @note This is the one HTTP response record in the tree. The downloader-side
 *       twin it used to sit beside is deleted, and `mdl_net_c6link.c` assigns
 *       this record straight across instead of copying it member by member
 *       (#746).
 * @since 0.1.0
 */
typedef struct {
  int32_t status;                                   /**< Final HTTP status, 0 if none. */
  char    retry_after[k_ra8_mdl_retry_after_max];   /**< Retry-After or empty.         */
  char    etag[k_ra8_mdl_etag_max];                 /**< ETag or empty.                */
  char    last_modified[k_ra8_mdl_http_date_max];   /**< Last-Modified or empty.       */
  char    content_type[k_ra8_mdl_content_type_max]; /**< Content-Type or empty.        */
} ra8_mdl_http_response_t;
