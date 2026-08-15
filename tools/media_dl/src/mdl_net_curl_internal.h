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
 * @since 0.1.0
 */

#pragma once

#include <curl/curl.h>
#include <stddef.h>

#include "mdl_net.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/**
 * @struct buf_sink_t
 * @brief Bounded-buffer sink state for a page fetch.
 * @details Handed to ::priv_mdl_net_curl_buf_write as libcurl's write-data. The
 *          write callback appends into `buf` until `cap` is reached, then
 *          latches `overflow` and aborts the transfer.
 * @invariant `len <= cap` whenever `overflow` is false.
 * @see priv_mdl_net_curl_buf_write()
 * @since 0.1.0
 */
typedef struct {
  char*  buf;      /**< Destination buffer.              */
  size_t cap;      /**< Capacity in bytes.               */
  size_t len;      /**< Bytes written so far.            */
  bool   overflow; /**< Set once the body exceeds `cap`. */
} buf_sink_t;

/**
 * @struct mdl_net_curl_body_state_t
 * @brief Test-visible bounded adapter from libcurl chunks to an injected sink.
 * @invariant `written <= cap` whenever @ref overflow is false and cap is nonzero.
 * @since 0.1.0
 */
typedef struct {
  mdl_net_body_sink_t* sink;       /**< Borrowed injected body sink.            */
  uint64_t             written;    /**< Bytes accepted across completed chunks. */
  uint64_t             cap;        /**< Response cap, or zero for no cap.       */
  ra8_err_t            sink_error; /**< First callback failure, or ::k_ra8_ok.  */
  bool                 overflow;   /**< Arithmetic or configured cap was hit.   */
} mdl_net_curl_body_state_t;

/**
 * @brief Enable the cookie engine and import validated caller-owned rows.
 * @param[in,out] curl Fresh libcurl easy handle.
 * @param[in] cookies Newline-delimited bounded cookie bytes.
 * @return Canonical validation or option status.
 * @retval k_ra8_ok Every accepted cookie row was copied by libcurl.
 * @retval k_ra8_err_invalid_arg The view or a row is malformed or unsafe.
 * @retval k_ra8_err_invalid_size One row exceeds the fixed importer bound.
 * @retval k_ra8_fail libcurl rejected an otherwise valid option.
 * @pre @p curl and @p cookies are non-NULL.
 * @post Success enables the cookie engine without path-based file I/O.
 * @post Caller command strings cannot reach `CURLOPT_COOKIELIST`.
 * @note Libcurl owns its copy of every accepted row.
 * @since 0.1.0

 * @details Applies validated caller-owned credentials to one easy handle.
 *          Borrowed storage remains caller-owned for libcurl's required lifetime.
 * @pre Every required pointer is non-null and remains valid for the call.
 */
RA8_PRIV ra8_err_t priv_mdl_net_curl_apply_cookies(CURL* curl, const mdl_net_bytes_t* cookies);

/**
 * @brief Bind complete caller-owned CA PEM bytes with NOCOPY semantics.
 * @param[in,out] curl Fresh libcurl easy handle.
 * @param[in] ca_pem Complete nonempty PEM bytes, or an empty absent view.
 * @param[in,out] blob Stable descriptor retained by the backend when supported.
 * @return Canonical validation, option, or unsupported status.
 * @retval k_ra8_ok System CA policy remains active or the blob was bound.
 * @retval k_ra8_err_invalid_arg The view or blob descriptor is inconsistent.
 * @retval k_ra8_err_not_supported This libcurl/TLS build lacks CA blob support.
 * @retval k_ra8_fail Libcurl rejected the custom CA option.
 * @pre Nonempty @p ca_pem bytes remain readable through backend destruction.
 * @post No CA filename or hidden file read is configured.
 * @note The descriptor may be NULL only when custom CA blobs are unavailable.
 * @since 0.1.0

 * @details Applies validated caller-owned credentials to one easy handle.
 *          Borrowed storage remains caller-owned for libcurl's required lifetime.
 * @pre Every required pointer is non-null and remains valid for the call.
 * @post Documented outputs and the return value describe the same outcome.
 */
RA8_PRIV ra8_err_t priv_mdl_net_curl_apply_ca_blob(CURL*                  curl,
                                                   const mdl_net_bytes_t* ca_pem,
                                                   struct curl_blob*      blob);

/**
 * @brief Map a completed libcurl transfer to an ::ra8_err_t (pure classifier).
 *
 * @details
 * The backend's decision table, factored out of the getinfo wrapper so it is a
 * pure function of the finished transfer's three observable outcomes and can be
 * unit-tested for every status class without a network. Precedence is overflow,
 * then timeout, then any other transport error, then the HTTP status class.
 * Mapping distinct status classes to distinct codes is what lets the politeness
 * governor tell a throttle (back off) from an absent page (skip) from a server
 * error (retry later).
 *
 * @param[in] code     libcurl completion code from `curl_easy_perform`.
 * @param[in] overflow Whether a sink latched a size-cap overflow.
 * @param[in] status   HTTP response status (from `CURLINFO_RESPONSE_CODE`).
 *
 * @return The classified transfer result.
 * @retval k_ra8_err_no_mem    `overflow` is true.
 * @retval k_ra8_err_timeout   `code` is `CURLE_OPERATION_TIMEDOUT`.
 * @retval k_ra8_err_busy      OK code and `status` is 429 or 503 (throttle).
 * @retval k_ra8_err_not_found OK code and `status` is 404 or another 4xx.
 * @retval k_ra8_fail          Any other non-OK `code`, or `status >= 500`.
 * @retval k_ra8_ok            OK code and `status < 400`.
 *
 * @pre `status` is a meaningful HTTP status only when `code == CURLE_OK`.
 * @pre The caller has finished the transfer before classifying it.
 * @post No state is modified.
 *
 * @note Thread-safe: depends only on its arguments.
 *
 * @par MC/DC:
 * The status tests (`status == 429 || status == 503`, then `status >= 500`,
 * then `status >= 400`) gate independent `if`s reached only when `code ==
 * CURLE_OK`. The throttle test is a two-condition OR: vectors 429 and 503 each
 * vary one condition true while a non-throttle status (e.g. 200/404/500) holds
 * both false, and the 500/404/200 vectors exercise the remaining relational
 * branches. Combined with the overflow and timeout vectors, every class the
 * function distinguishes has a vector.
 *
 * @since 0.1.0

 * @post Documented outputs and the return value describe the same outcome.
 */
RA8_PRIV ra8_err_t priv_mdl_net_curl_classify(CURLcode code, bool overflow, long status);

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
RA8_PRIV size_t priv_mdl_net_curl_buf_write(char* data, size_t size, size_t nmemb, void* user);

/**
 * @brief Adapt one libcurl body chunk to an injected bounded sink.
 * @details Validates multiplication and configured capacity before dispatching
 *          one complete chunk to the injected writer.
 * @param[in] data Bytes supplied by libcurl.
 * @param[in] size Element size.
 * @param[in] nmemb Element count.
 * @param[in,out] user ::mdl_net_curl_body_state_t state.
 * @return Exact byte count accepted, or zero to abort libcurl.
 * @retval 0 Overflow, invalid state, or sink failure aborted the transfer.
 * @retval other Exact `size * nmemb` bytes accepted by the sink.
 * @pre Representable nonempty input is readable for `size * nmemb` bytes.
 * @pre @p user is NULL or points to mutable callback state.
 * @post Overflow or sink failure is latched before returning zero.
 * @post Success advances the cumulative written extent exactly once.
 * @note Signature is fixed by libcurl's callback ABI.
 * @since 0.1.0
 */
RA8_PRIV size_t priv_mdl_net_curl_body_write(char* data, size_t size, size_t nmemb, void* user);
