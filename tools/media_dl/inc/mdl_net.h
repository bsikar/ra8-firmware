/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mdl_net.h
 * @brief Streaming HTTP(S) GET seam for the host manga downloader (v0).
 *
 * @details
 * Dependency-inversion seam mirroring the on-device streaming-GET vtable
 * (`ra8_ota_net_iface_t`): every layer above this interface -- URL extraction,
 * politeness, the download loop -- is identical host and on-silicon. Only the
 * injected backend differs. On the host it is libcurl (`mdl_net_curl.c`); on
 * the RA8 it will be NetX Duo + Mbed TLS reached over the ESP32-C6 radio.
 *
 * The library reuses the firmware error contract (`ra8_err_t`) so signatures
 * are already device-shaped: porting a caller up to the chip is a backend swap,
 * not a rewrite.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/** @brief Opaque network backend handle (one owns a reusable connection). */
typedef struct mdl_net_iface mdl_net_iface_t;

/**
 * @brief Per-request session parameters.
 * @note `user_agent` is chosen once per session and held constant -- rotating
 *       it per request (as the Kotlin original did) looks more bot-like, not
 *       less, so we deliberately do not.
 */
typedef struct {
  const char* user_agent; /**< Session User-Agent; never per-request random. */
  const char* referer;    /**< Referer header value, or NULL to omit.        */
  uint32_t    timeout_ms; /**< Whole-request time budget, milliseconds.      */
} mdl_net_req_t;

/**
 * @brief Session-wide security policy for the network backend.
 *
 * @details
 * `media_dl` fetches attacker-influenced URLs, so the backend is hardened by
 * default: only public address space is reachable, redirects may not change
 * host, and each response is size-bounded. These knobs are the deliberate
 * escape hatches, all off/safe by default.
 *
 * @invariant `max_response_bytes == 0` means "unlimited" (matching libcurl).
 * @since 0.1.0
 */
typedef struct {
  bool     allow_private_hosts;       /**< Permit loopback/private/link-local peers. */
  bool     allow_cross_host_redirect; /**< Permit a redirect to a different host.    */
  uint64_t max_response_bytes;        /**< Per-response byte cap (0 = unlimited).    */
} mdl_net_policy_t;

/**
 * @brief Create the libcurl-backed host network interface.
 * @param[in] policy Session security policy, or NULL for the safe defaults
 *                   (no private hosts, same-host redirects only, no size cap).
 * @return Owned handle, or NULL on allocation/init failure, or if any
 *         security-relevant libcurl option could not be applied.
 * @note The handle keeps one libcurl easy handle alive across requests, so the
 *       connection pool and cookie jar persist (unlike the Kotlin per-request
 *       client). Not thread-safe: one interface per worker.
 */
mdl_net_iface_t* mdl_net_curl_create(const mdl_net_policy_t* policy);

/** @brief Destroy an interface from mdl_net_curl_create(). NULL-safe. */
void mdl_net_curl_destroy(mdl_net_iface_t* net);

/**
 * @brief HTTP status code of the most recent transfer on `net`.
 *
 * @details
 * Lets a caller distinguish an absent resource (`404`) from a server error
 * (`5xx`) after a fetch, which the robots.txt convention needs: an absent
 * file means "no restrictions" while a `5xx` means "disallow all".
 *
 * @param[in] net Network interface, or NULL.
 *
 * @return The last transfer's HTTP status, or 0 when unknown.
 * @retval 0     No transfer has completed, or `net`/its handle is NULL.
 * @retval other The status libcurl recorded for the last transfer.
 *
 * @pre `net`, when non-NULL, was returned by ::mdl_net_curl_create.
 * @pre A transfer has been attempted for the value to be meaningful.
 * @post `net` is not modified.
 *
 * @note Not thread-safe: reads the shared easy handle.
 * @since 0.1.0
 */
long mdl_net_last_status(mdl_net_iface_t* net);

/**
 * @brief GET `url` fully into a caller buffer (used for HTML pages).
 *
 * @param[in]  net     Network interface.
 * @param[in]  url     Absolute http/https URL.
 * @param[in]  req     Session parameters (must be non-NULL).
 * @param[out] buf     Destination buffer.
 * @param[in]  cap     Capacity of `buf` in bytes (a trailing NUL is written when
 *                     it fits, so pass cap >= body + 1 to guarantee a C string).
 * @param[out] out_len Bytes written (excluding any NUL). May be NULL.
 *
 * @retval k_ra8_ok            Body fetched, HTTP status < 400.
 * @retval k_ra8_err_invalid_arg  NULL argument.
 * @retval k_ra8_err_no_mem    Body exceeded `cap`.
 * @retval k_ra8_err_timeout   Request exceeded `req->timeout_ms`.
 * @retval k_ra8_fail          Transport error or HTTP status >= 400.
 */
ra8_err_t mdl_net_get_buf(mdl_net_iface_t*     net,
                          const char*          url,
                          const mdl_net_req_t* req,
                          char*                buf,
                          size_t               cap,
                          size_t*              out_len);

/**
 * @brief GET `url` and stream the body to a file (used for images).
 *
 * @param[in]  net      Network interface.
 * @param[in]  url      Absolute http/https URL.
 * @param[in]  req      Session parameters (must be non-NULL).
 * @param[in]  out_path Filesystem path to create/overwrite.
 * @param[out] out_len  Bytes written. May be NULL.
 *
 * @retval k_ra8_ok            File written, HTTP status < 400.
 * @retval k_ra8_err_invalid_arg  NULL argument.
 * @retval k_ra8_err_timeout   Request exceeded `req->timeout_ms`.
 * @retval k_ra8_fail          Transport error, HTTP status >= 400, or file I/O.
 */
ra8_err_t mdl_net_get_file(mdl_net_iface_t*     net,
                           const char*          url,
                           const mdl_net_req_t* req,
                           const char*          out_path,
                           size_t*              out_len);
