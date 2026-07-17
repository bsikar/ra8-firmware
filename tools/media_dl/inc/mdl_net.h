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
 * @brief Create the libcurl-backed host network interface.
 * @return Owned handle, or NULL on allocation/init failure.
 * @note The handle keeps one libcurl easy handle alive across requests, so the
 *       connection pool and cookie jar persist (unlike the Kotlin per-request
 *       client). Not thread-safe: one interface per worker.
 */
mdl_net_iface_t* mdl_net_curl_create(void);

/** @brief Destroy an interface from mdl_net_curl_create(). NULL-safe. */
void mdl_net_curl_destroy(mdl_net_iface_t* net);

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
