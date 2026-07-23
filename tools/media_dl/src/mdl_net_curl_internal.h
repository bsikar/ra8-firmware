/**
 * @file mdl_net_curl_internal.h
 * @brief Module-private libcurl-backend helpers promoted for host unit tests.
 *
 * @details
 * The libcurl backend's transfer-result classifier and its bounded-buffer write
 * callback are pure decisions with no network dependency, but they live in a
 * translation unit that pulls in `<curl/curl.h>`. Promoting them to TU-external
 * linkage here (per the "Test access to internal symbols" rule in `CLAUDE.md`)
 * lets the host tests drive each status class and the overflow-abort path
 * directly, rather than only through a live transfer that no test can perform.
 *
 * Nothing here is part of the backend-facing API in `mdl_net.h` or the factory
 * in `mdl_net_curl.h`; production callers use the vtable dispatchers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#include <curl/curl.h>
#include <stddef.h>

#include "ra8_attributes.h"
#include "ra8_err.h"

/**
 * @struct buf_sink_t
 * @brief Bounded-buffer sink state for a page fetch.
 * @details Handed to ::mdl_net_curl_buf_write as libcurl's write-data. The
 *          write callback appends into `buf` until `cap` is reached, then
 *          latches `overflow` and aborts the transfer.
 * @invariant `len <= cap` whenever `overflow` is false.
 * @see mdl_net_curl_buf_write()
 * @since 0.1.0
 */
typedef struct {
  char*  buf;      /**< Destination buffer.              */
  size_t cap;      /**< Capacity in bytes.               */
  size_t len;      /**< Bytes written so far.            */
  bool   overflow; /**< Set once the body exceeds `cap`. */
} buf_sink_t;

/**
 * @brief Map a completed libcurl transfer to an ::ra8_err_t (pure classifier).
 *
 * @details
 * The backend's decision table, factored out of the getinfo wrapper so it is a
 * pure function of the finished transfer's three observable outcomes and can be
 * unit-tested for every status class without a network. Precedence is
 * overflow, then timeout, then any other transport error, then the HTTP status.
 *
 * @param[in] code     libcurl completion code from `curl_easy_perform`.
 * @param[in] overflow Whether a sink latched a size-cap overflow.
 * @param[in] status   HTTP response status (from `CURLINFO_RESPONSE_CODE`).
 *
 * @return The classified transfer result.
 * @retval k_ra8_err_no_mem  `overflow` is true.
 * @retval k_ra8_err_timeout `code` is `CURLE_OPERATION_TIMEDOUT`.
 * @retval k_ra8_fail        Any other non-OK `code`, or `status >= 400`.
 * @retval k_ra8_ok          OK code and `status < 400`.
 *
 * @pre `status` is a meaningful HTTP status only when `code == CURLE_OK`.
 * @pre The caller has finished the transfer before classifying it.
 * @post No state is modified.
 *
 * @note Thread-safe: depends only on its arguments.
 *
 * @par MC/DC:
 * The status test `status >= k_http_status_err_min` is a single relational
 * condition, not a compound `&&`/`||` decision; the surrounding branches are
 * independent `if`s. Vectors exercise each class: overflow, timeout, other
 * error, `status >= 400`, and `status < 400`.
 *
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t mdl_net_curl_classify(CURLcode code, bool overflow, long status);

/**
 * @brief libcurl write callback: append into a bounded buffer, abort on cap.
 *
 * @details
 * The registered `CURLOPT_WRITEFUNCTION` for a page fetch. Appends `size *
 * nmemb` bytes into the ::buf_sink_t `user` unless doing so would exceed its
 * capacity, in which case it latches `overflow` and returns 0 to abort the
 * transfer (libcurl's contract for a short write).
 *
 * @param[in]  data Bytes libcurl delivered (not NUL-terminated).
 * @param[in]  size Size of each member (libcurl passes 1).
 * @param[in]  nmemb Number of members.
 * @param[in,out] user The ::buf_sink_t receiving the bytes, or NULL.
 *
 * @return The number of members consumed, or 0 to abort.
 * @retval nmemb All bytes were appended.
 * @retval 0     `user` is NULL, or the append would exceed `cap` (overflow).
 *
 * @pre `user`, when non-NULL, is a ::buf_sink_t with a valid `buf`/`cap`.
 * @pre The caller treats a 0 return as a deliberate transfer abort.
 * @post On success `sink->len` advanced by `size * nmemb`.
 * @post On overflow `sink->overflow` is true and `sink->buf` is unchanged.
 *
 * @note Thread-safe: writes only the caller-provided sink.
 *
 * @par MC/DC:
 * The two guards -- `user == NULL` and `(len + bytes) > cap` -- are single
 * conditions, not compound decisions. Vectors: NULL sink, an appending write,
 * and an over-cap write that latches overflow.
 *
 * @since 0.1.0
 */
RA8_PRIV size_t mdl_net_curl_buf_write(char* data, size_t size, size_t nmemb, void* user);
