/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
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
 * @invariant All four function pointers are non-NULL in any table handed to
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
   * @return An ::ra8_err_t per the ::mdl_net_get_buf contract.
   */
  ra8_err_t (*get_buf)(void*                ctx,
                       const char*          url,
                       const mdl_net_req_t* req,
                       char*                buf,
                       size_t               cap,
                       size_t*              out_len);

  /**
   * @brief GET `url` and stream the body to a file (used for images).
   * @param[in]  ctx      Backend-private state (never NULL).
   * @param[in]  url      Absolute, scheme-validated http/https URL.
   * @param[in]  req      Session parameters (never NULL).
   * @param[in]  out_path Filesystem path to create/overwrite (never NULL).
   * @param[out] out_len  Bytes written. May be NULL.
   * @return An ::ra8_err_t per the ::mdl_net_get_file contract.
   */
  ra8_err_t (*get_file)(void*                ctx,
                        const char*          url,
                        const mdl_net_req_t* req,
                        const char*          out_path,
                        size_t*              out_len);

  /**
   * @brief HTTP status code of the most recent transfer.
   * @param[in] ctx Backend-private state (never NULL).
   * @return The last transfer's HTTP status, or 0 when unknown.
   */
  long (*last_status)(void* ctx);

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
 * @invariant `vtable` is non-NULL and points to a table with all four methods
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
 * @brief HTTP status code of the most recent transfer on `net`.
 *
 * @details
 * Lets a caller distinguish an absent resource (`404`) from a server error
 * (`5xx`) after a fetch, which the robots.txt convention needs: an absent
 * file means "no restrictions" while a `5xx` means "disallow all". Dispatches
 * to the backend's `last_status` method.
 *
 * @param[in] net Network interface, or NULL.
 *
 * @return The last transfer's HTTP status, or 0 when unknown.
 * @retval 0     No transfer has completed, or `net`/its vtable is NULL.
 * @retval other The status the backend recorded for the last transfer.
 *
 * @pre `net`, when non-NULL, was returned by a backend factory.
 * @pre A transfer has been attempted for the value to be meaningful.
 * @post `net` is not modified by the dispatcher.
 *
 * @note Not thread-safe: reads shared backend state.
 * @since 0.1.0
 */
long mdl_net_last_status(mdl_net_iface_t* net);

/**
 * @brief GET `url` fully into a caller buffer (used for HTML pages).
 *
 * @details
 * Validates the handle and arguments, then dispatches to the backend's
 * `get_buf` method. The backend enforces the scheme allowlist and any security
 * policy before contacting the network.
 *
 * @param[in]  net     Network interface.
 * @param[in]  url     Absolute http/https URL.
 * @param[in]  req     Session parameters (must be non-NULL).
 * @param[out] buf     Destination buffer.
 * @param[in]  cap     Capacity of `buf` in bytes (a trailing NUL is written when
 *                     it fits, so pass cap >= body + 1 to guarantee a C string).
 * @param[out] out_len Bytes written (excluding any NUL). May be NULL.
 *
 * @return An ::ra8_err_t transfer result.
 * @retval k_ra8_ok               Body fetched, HTTP status < 400.
 * @retval k_ra8_err_invalid_arg  NULL argument, `cap == 0`, or refused scheme.
 * @retval k_ra8_err_no_mem       Body exceeded `cap`.
 * @retval k_ra8_err_timeout      Request exceeded `req->timeout_ms`.
 * @retval k_ra8_fail             Transport error or HTTP status >= 400.
 *
 * @pre `net`, when non-NULL, holds a fully populated vtable.
 * @pre `req`, `buf` are non-NULL and `cap > 0` for a fetch to be attempted.
 * @post On any error the buffer contents are unspecified.
 * @post `*out_len`, when `out_len` is non-NULL, is set only on ::k_ra8_ok.
 *
 * @note Not thread-safe: one interface per worker.
 * @since 0.1.0
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
 * @details
 * Validates the handle and arguments, then dispatches to the backend's
 * `get_file` method, which streams the body to `out_path` under the session
 * size cap and removes a torn/oversized file on failure.
 *
 * @param[in]  net      Network interface.
 * @param[in]  url      Absolute http/https URL.
 * @param[in]  req      Session parameters (must be non-NULL).
 * @param[in]  out_path Filesystem path to create/overwrite.
 * @param[out] out_len  Bytes written. May be NULL.
 *
 * @return An ::ra8_err_t transfer result.
 * @retval k_ra8_ok               File written, HTTP status < 400.
 * @retval k_ra8_err_invalid_arg  NULL argument or refused scheme.
 * @retval k_ra8_err_no_mem       Body exceeded the session size cap.
 * @retval k_ra8_err_timeout      Request exceeded `req->timeout_ms`.
 * @retval k_ra8_fail             Transport error, HTTP status >= 400, or I/O.
 *
 * @pre `net`, when non-NULL, holds a fully populated vtable.
 * @pre `req`, `out_path` are non-NULL for a fetch to be attempted.
 * @post On any error no partial output file is left behind.
 * @post `*out_len`, when `out_len` is non-NULL, is set only on ::k_ra8_ok.
 *
 * @note Not thread-safe: one interface per worker.
 * @since 0.1.0
 */
ra8_err_t mdl_net_get_file(mdl_net_iface_t*     net,
                           const char*          url,
                           const mdl_net_req_t* req,
                           const char*          out_path,
                           size_t*              out_len);
