/**
 * @file mdl_net.h
 * @brief Streaming HTTP(S) GET seam (a real function-pointer vtable) for the
 *        host manga downloader (v0).
 *
 * @details
 * Dependency-inversion seam mirroring the on-device streaming-GET vtable
 * (`ra8_ota_net_iface_t`): every layer above this interface -- URL extraction,
 * politeness, the download loop, robots gating -- is identical host and
 * on-silicon. Only the injected backend differs. On the host it is libcurl
 * (`mdl_net_curl.c`, created through `mdl_net_curl.h`); on the RA8 it will be
 * NetX Duo + Mbed TLS reached over the ESP32-C6 radio.
 *
 * The interface is a genuine vtable, not a link-time name: ::mdl_net_iface_t is
 * a `{ vtable, ctx }` pair, and callers reach a backend only through the
 * dispatchers below (::mdl_net_get_buf, ::mdl_net_get_file,
 * ::mdl_net_last_status, ::mdl_net_destroy). This is the NASA Power of 10 Rule 9
 * deviation `CLAUDE.md` documents for exactly this purpose: swapping the future
 * NetX/Mbed backend, or a scripted mock in the host unit tests, is a
 * vtable substitution, not an edit at every call site or a relink.
 *
 * The library reuses the firmware error contract (`ra8_err_t`) so signatures
 * are already device-shaped: porting a caller up to the chip is a backend swap,
 * not a rewrite.
 *
 * @see mdl_net_curl.h  The concrete libcurl backend factory (composition root).
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @brief Per-request session parameters.
 * @note `user_agent` is chosen once per session and held constant -- rotating
 *       it per request (as the Kotlin original did) looks more bot-like, not
 *       less, so we deliberately do not.
 */
typedef struct {
  const char* user_agent;        /**< Session User-Agent; never per-request random.      */
  const char* referer;           /**< Referer header value, or NULL to omit.             */
  const char* if_none_match;     /**< If-None-Match conditional header (ETag), or NULL.  */
  const char* if_modified_since; /**< If-Modified-Since header (Last-Modified), or NULL. */
  uint32_t    timeout_ms;        /**< Whole-request time budget, milliseconds.           */
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
  bool        allow_private_hosts;       /**< Permit loopback/private/link-local peers. */
  bool        allow_cross_host_redirect; /**< Permit a redirect to a different host.    */
  uint64_t    max_response_bytes;        /**< Per-response byte cap (0 = unlimited).    */
  const char* proxy;                     /**< HTTP/HTTPS proxy URL, or NULL for none.   */
  const char* socks5;                    /**< SOCKS5 proxy URL, or NULL for none.       */
  const char* cookie_file;               /**< Cookie file path for libcurl, or NULL.    */
} mdl_net_policy_t;

/** @brief Captured-response-field buffer sizes. */
typedef enum : uint16_t {
  k_mdl_retry_after_max  = 64U,  /**< Raw `Retry-After` header value buffer bytes.   */
  k_mdl_etag_max         = 128U, /**< Raw `ETag` header value buffer bytes.          */
  k_mdl_last_mod_max     = 64U,  /**< Raw `Last-Modified` header value buffer bytes. */
  k_mdl_content_type_max = 128U, /**< Raw `Content-Type` header value buffer bytes.  */
} mdl_net_resp_size_t;

/**
 * @struct mdl_net_resp_t
 * @brief Per-transfer response metadata surfaced to the politeness governor.
 *
 * @details
 * The fetch dispatchers fill one of these (when the caller passes a non-NULL
 * pointer) so a caller can distinguish an absent page from a throttle from a
 * server error and, on a throttle, honour the server's own `Retry-After`. It
 * carries the finished transfer's HTTP `status` and the raw `Retry-After`
 * header value verbatim -- the governor parses the header (both delta-seconds
 * and HTTP-date forms) through ::mdl_retry_after_parse, so the network backend
 * never has to. An empty `retry_after` means the header was absent.
 *
 * @invariant `retry_after` is always NUL-terminated; `retry_after[0] == '\0'`
 *            exactly when the header was absent.
 * @invariant `status == 0` means no HTTP status was observed (transport error
 *            before a response, or an argument the dispatcher refused).
 * @see mdl_net_get_buf()
 * @see mdl_retry_after_parse()
 * @since 0.1.0
 */
typedef struct {
  long status;                               /**< Finished transfer's HTTP status, 0 if none. */
  char retry_after[k_mdl_retry_after_max];   /**< Raw `Retry-After` value, "" when absent.    */
  char etag[k_mdl_etag_max];                 /**< Raw `ETag` value, "" when absent.           */
  char last_modified[k_mdl_last_mod_max];    /**< Raw `Last-Modified` value, "" when absent.  */
  char content_type[k_mdl_content_type_max]; /**< Raw `Content-Type` value, "" when absent.   */
} mdl_net_resp_t;

/**
 * @struct mdl_net_vtable_t
 * @brief Method table one network backend registers (Dependency Inversion).
 *
 * @details
 * The concrete backend (libcurl on the host, NetX/Mbed on the device, or a
 * scripted fake in tests) fills one immutable, statically-allocated table of
 * these four function pointers. Callers never see it: they hold a
 * ::mdl_net_iface_t and go through the dispatchers, which forward to the table
 * passing the backend's opaque `ctx`. Every pointer receives that `ctx` as its
 * first argument so one table serves many independent handles.
 *
 * @invariant All three function pointers are non-NULL in any table handed to
 *            ::mdl_net_iface_t. The dispatchers refuse a NULL `get_*` method.
 * @see mdl_net_iface_t
 * @see mdl_net_get_buf()
 * @since 0.1.0
 */
typedef struct {
  /**
   * @brief GET `url` fully into a caller buffer (used for HTML pages).
   * @param[in]  ctx     Backend-private state (never NULL).
   * @param[in]  url     Absolute, scheme-validated http/https URL.
   * @param[in]  req     Session parameters (never NULL).
   * @param[out] buf     Destination buffer (never NULL).
   * @param[in]  cap     Capacity of `buf` in bytes (never 0).
   * @param[out] out_len Bytes written (excluding any NUL). May be NULL.
   * @param[out] resp    Response metadata (status + Retry-After). May be NULL.
   * @return An ::ra8_err_t per the ::mdl_net_get_buf contract.
   */
  ra8_err_t (*get_buf)(void*                ctx,
                       const char*          url,
                       const mdl_net_req_t* req,
                       char*                buf,
                       size_t               cap,
                       size_t*              out_len,
                       mdl_net_resp_t*      resp);

  /**
   * @brief GET `url` and stream the body to a file (used for images).
   * @param[in]  ctx      Backend-private state (never NULL).
   * @param[in]  url      Absolute, scheme-validated http/https URL.
   * @param[in]  req      Session parameters (never NULL).
   * @param[in]  out_path Filesystem path to create/overwrite (never NULL).
   * @param[out] out_len  Bytes written. May be NULL.
   * @param[out] resp     Response metadata (status + Retry-After). May be NULL.
   * @return An ::ra8_err_t per the ::mdl_net_get_file contract.
   */
  ra8_err_t (*get_file)(void*                ctx,
                        const char*          url,
                        const mdl_net_req_t* req,
                        const char*          out_path,
                        size_t*              out_len,
                        mdl_net_resp_t*      resp);

  /**
   * @brief Release the backend-private state (called by ::mdl_net_destroy).
   * @param[in] ctx Backend-private state (never NULL).
   */
  void (*destroy)(void* ctx);
} mdl_net_vtable_t;

/**
 * @struct mdl_net_iface_t
 * @brief A network backend handle: an immutable method table plus its state.
 *
 * @details
 * This is the object every layer above the seam passes around. It is created
 * by a backend factory (::mdl_net_curl_create for production, or a test helper
 * for a fake) and consumed only through the dispatchers. Holding the `vtable`
 * and `ctx` together is what makes the substitution real: the same download
 * loop runs against libcurl or against a mock with no code change.
 *
 * @invariant `vtable` is non-NULL and points to a table with all three methods
 *            populated for the whole lifetime of the handle.
 * @invariant `ctx` is whatever the backend factory stored; the dispatchers
 *            never interpret it, only forward it.
 * @see mdl_net_curl_create()
 * @since 0.1.0
 */
typedef struct mdl_net_iface {
  const mdl_net_vtable_t* vtable; /**< Backend method table (never NULL).       */
  void*                   ctx;    /**< Backend-private state passed to methods. */
} mdl_net_iface_t;

/**
 * @brief Release a network interface returned by a backend factory. NULL-safe.
 *
 * @details
 * Forwards to the backend's `destroy` method to free its private state, then
 * frees the handle itself. The handle must have been produced by a factory
 * that heap-allocated it (e.g. ::mdl_net_curl_create); a stack-constructed
 * handle (as a test fake uses) is owned by its constructor and must not be
 * passed here.
 *
 * @param[in] net Interface to release, or NULL.
 *
 * @return Nothing.
 *
 * @pre `net`, when non-NULL, came from a heap-allocating backend factory.
 * @pre No dispatcher call on `net` is in progress.
 * @post `net` and its backend state are freed; the pointer is dangling.
 * @post A NULL argument is a no-op.
 *
 * @note Not thread-safe: one interface per worker.
 * @since 0.1.0
 */
void mdl_net_destroy(mdl_net_iface_t* net);

/**
 * @brief GET `url` fully into a caller buffer (used for HTML pages).
 *
 * @details
 * Validates the handle and arguments, then dispatches to the backend's
 * `get_buf` method. The backend enforces the scheme allowlist and any security
 * policy before contacting the network. When @p resp is non-NULL it receives
 * the finished transfer's HTTP status and raw `Retry-After` header, letting the
 * caller tell an absent page from a throttle from a server error -- the
 * distinction the robots.txt convention and the politeness governor both need.
 *
 * @param[in]  net     Network interface.
 * @param[in]  url     Absolute http/https URL.
 * @param[in]  req     Session parameters (must be non-NULL).
 * @param[out] buf     Destination buffer.
 * @param[in]  cap     Capacity of `buf` in bytes (a trailing NUL is written when
 *                     it fits, so pass cap >= body + 1 to guarantee a C string).
 * @param[out] out_len Bytes written (excluding any NUL). May be NULL.
 * @param[out] resp    Response metadata (status + Retry-After), or NULL to skip.
 *
 * @return An ::ra8_err_t transfer result.
 * @retval k_ra8_ok               Body fetched, HTTP status < 400.
 * @retval k_ra8_err_invalid_arg  NULL argument, `cap == 0`, or refused scheme.
 * @retval k_ra8_err_no_mem       Body exceeded `cap`.
 * @retval k_ra8_err_timeout      Request exceeded `req->timeout_ms`.
 * @retval k_ra8_err_busy         HTTP 429 or 503 (throttled -- back off).
 * @retval k_ra8_err_not_found    HTTP 404 or another 4xx (skip this resource).
 * @retval k_ra8_fail             Transport error or HTTP 5xx (server error).
 *
 * @pre `net`, when non-NULL, holds a fully populated vtable.
 * @pre `req`, `buf` are non-NULL and `cap > 0` for a fetch to be attempted.
 * @post On any error the buffer contents are unspecified.
 * @post `*out_len`, when `out_len` is non-NULL, is set only on ::k_ra8_ok.
 * @post `*resp`, when non-NULL, is filled with the observed status/header.
 *
 * @note Not thread-safe: one interface per worker.
 * @since 0.1.0
 */
ra8_err_t mdl_net_get_buf(mdl_net_iface_t*     net,
                          const char*          url,
                          const mdl_net_req_t* req,
                          char*                buf,
                          size_t               cap,
                          size_t*              out_len,
                          mdl_net_resp_t*      resp);

/**
 * @brief GET `url` and stream the body to a file (used for images).
 *
 * @details
 * Validates the handle and arguments, then dispatches to the backend's
 * `get_file` method, which streams the body to `out_path` under the session
 * size cap and removes a torn/oversized file on failure. When @p resp is
 * non-NULL it receives the finished transfer's HTTP status and raw
 * `Retry-After` header so the governor can back off on a throttle.
 *
 * @param[in]  net      Network interface.
 * @param[in]  url      Absolute http/https URL.
 * @param[in]  req      Session parameters (must be non-NULL).
 * @param[in]  out_path Filesystem path to create/overwrite.
 * @param[out] out_len  Bytes written. May be NULL.
 * @param[out] resp     Response metadata (status + Retry-After), or NULL to skip.
 *
 * @return An ::ra8_err_t transfer result.
 * @retval k_ra8_ok               File written, HTTP status < 400.
 * @retval k_ra8_err_invalid_arg  NULL argument or refused scheme.
 * @retval k_ra8_err_no_mem       Body exceeded the session size cap.
 * @retval k_ra8_err_timeout      Request exceeded `req->timeout_ms`.
 * @retval k_ra8_err_busy         HTTP 429 or 503 (throttled -- back off).
 * @retval k_ra8_err_not_found    HTTP 404 or another 4xx (skip this resource).
 * @retval k_ra8_fail             Transport error, HTTP 5xx, or local I/O error.
 *
 * @pre `net`, when non-NULL, holds a fully populated vtable.
 * @pre `req`, `out_path` are non-NULL for a fetch to be attempted.
 * @post On any error no partial output file is left behind.
 * @post `*out_len`, when `out_len` is non-NULL, is set only on ::k_ra8_ok.
 * @post `*resp`, when non-NULL, is filled with the observed status/header.
 *
 * @note Not thread-safe: one interface per worker.
 * @since 0.1.0
 */
ra8_err_t mdl_net_get_file(mdl_net_iface_t*     net,
                           const char*          url,
                           const mdl_net_req_t* req,
                           const char*          out_path,
                           size_t*              out_len,
                           mdl_net_resp_t*      resp);
