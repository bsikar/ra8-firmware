/**
 * @file ra8_c6link_mdl.h
 * @brief Pull-based media download client over ESP-hosted CustomRpc.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details The C6 owns HTTP fetching. The RA8 owns the destination path and
 * file lifecycle: callers pull bounded chunks and write them through the RA8
 * filesystem. This keeps POSIX paths and storage handles off the co-processor
 * wire and gives the existing single-outstanding-call c6link natural
 * backpressure.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

#include "ra8_c6link.h"
#include "ra8_err.h"
#include "ra8_mdl_format.h"
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
  const char* user_agent;        /**< User-Agent value, or null/empty to omit.      */
  const char* referer;           /**< Referer value, or null/empty to omit.         */
  const char* if_none_match;     /**< If-None-Match value, or null/empty to omit.   */
  const char* if_modified_since; /**< If-Modified-Since value, or null/empty.       */
  uint32_t    timeout_ms;        /**< Whole-request timeout, or zero for default.   */
} ra8_mdl_http_policy_t;

/**
 * @struct ra8_mdl_http_response_t
 * @brief HTTP status and selected response headers proven by the C6 backend.
 * @invariant `status` is in the inclusive range 100..599 on success.
 * @invariant Every array is NUL-terminated, including when its header is absent.
 * @since 0.1.0
 */
typedef struct {
  int32_t status;                                   /**< Final HTTP status.          */
  char    retry_after[k_ra8_mdl_retry_after_max];   /**< Retry-After or empty.       */
  char    etag[k_ra8_mdl_etag_max];                 /**< ETag or empty.              */
  char    last_modified[k_ra8_mdl_http_date_max];   /**< Last-Modified or empty.     */
  char    content_type[k_ra8_mdl_content_type_max]; /**< Content-Type or empty.      */
} ra8_mdl_http_response_t;

/**
 * @struct ra8_mdl_request_t
 * @brief Complete typed HTTPS request accepted by protocol version 3.
 * @invariant `url` is a nonempty HTTPS URL shorter than ::k_ra8_mdl_url_max.
 * @invariant `format` is one concrete ::ra8_mdl_format_t value through RABOOK.
 * @since 0.1.0
 */
typedef struct {
  const char*           url;    /**< Absolute HTTPS source URL. */
  ra8_mdl_format_t      format; /**< Exact returned artifact identity. */
  ra8_mdl_http_policy_t http;   /**< Forwarded request policy. */
} ra8_mdl_request_t;

/**
 * @struct ra8_mdl_session_t
 * @brief RA8-local state for one accepted remote artifact job
 * @details Maintains the correlation values the next response must carry.
 * Callers allocate it; the client never allocates or retains caller memory.
 * @invariant An active session has a non-zero `job_id` and `max_chunk_bytes`.
 * @invariant `next_offset` and `next_sequence` advance only after a valid
 * response.
 * @code
 * ra8_mdl_session_t session = {};
 * (void)ra8_c6link_mdl_start(
 *   &link, "https://host/book.rabook", k_ra8_mdl_format_rabook, &session);
 * @endcode
 * @see ra8_c6link_mdl_start
 * @since 0.1.0
 */
typedef struct {
  uint32_t         job_id;          /**< Remote-generated non-zero identifier.              */
  uint32_t         next_sequence;   /**< Exact sequence required from the next response.    */
  uint64_t         next_offset;     /**< Exact byte offset required from the next response. */
  uint32_t         max_chunk_bytes; /**< Maximum accepted pull size negotiated at start.    */
  ra8_mdl_format_t format;          /**< Requested format echoed by the C6.                 */
  bool             active;          /**< Whether next/cancel is currently valid.            */
} ra8_mdl_session_t;

/**
 * @struct ra8_mdl_chunk_t
 * @brief One correlated bounded raw-byte response
 * @details Contains either non-empty DOWNLOADING data or an empty terminal
 * record. COMPLETE alone carries the digest of the entire HTTP body.
 * @invariant `data_len` never exceeds ::k_ra8_mdl_chunk_data_max.
 * @invariant `has_sha256` is true only for a valid COMPLETE record.
 * @code
 * ra8_mdl_chunk_t chunk = {};
 * @endcode
 * @see ra8_c6link_mdl_next
 * @since 0.1.0
 */
typedef struct {
  uint32_t        job_id;                         /**< Correlated remote job identifier.          */
  uint32_t        sequence;                       /**< Zero-based response sequence.              */
  uint64_t        offset;                         /**< Offset of `data` in the complete body.     */
  uint64_t        total_bytes;                    /**< Advertised or independently counted total. */
  ra8_mdl_state_t state;                          /**< DOWNLOADING or terminal state.             */
  ra8_err_t       status;                         /**< Failure status in FAILED state.            */
  uint16_t        data_len;                       /**< Valid bytes in `data`.                     */
  uint8_t         data[k_ra8_mdl_chunk_data_max]; /**< Bounded raw body bytes.                    */
  bool            has_sha256;                     /**< Whether `sha256` is valid.                 */
  uint8_t         sha256[k_ra8_mdl_sha256_bytes]; /**< Complete-body SHA-256.                     */
  ra8_mdl_http_response_t response;               /**< Terminal HTTP metadata.                   */
} ra8_mdl_chunk_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start retrieval of one selected artifact and receive its job
 * identifier
 * @details Encodes a generated protobuf StartRequest with the exact artifact
 * identity. `loose` selects an untyped source body; other values require the
 * returned bytes to be that format and must be validated before publication.
 * @param[in,out] link Already-open exclusively owned c6link.
 * @param[in] url NUL-terminated HTTPS source URL below ::k_ra8_mdl_url_max.
 * @param[in] format Artifact identity requested from the C6 backend.
 * @param[out] session Accepted job correlation state.
 * @return Start status.
 * @retval k_ra8_ok Remote job accepted.
 * @retval k_ra8_err_null_ptr A required pointer is null.
 * @retval k_ra8_err_invalid_arg URL or format is invalid.
 * @retval k_ra8_err_invalid_size URL or encoded request exceeds its bound.
 * @retval k_ra8_err_protocol_error Remote response is malformed.
 * @retval k_ra8_err_timeout C6 did not answer inside the RPC budget.
 * @pre ::ra8_c6link_open completed successfully for @p link.
 * @pre No other thread uses @p link concurrently.
 * @post Success produces an active non-zero job in @p session.
 * @post Failure leaves @p session inactive and zeroed after argument
 * validation.
 * @note Not thread-safe; the c6link handle is single-owner.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_c6link_mdl_start(ra8_c6link_t*      link,
                                             const char*        url,
                                             ra8_mdl_format_t   format,
                                             ra8_mdl_session_t* session);

/**
 * @brief Start one typed request while preserving downloader HTTP policy.
 * @details This is the protocol-v3 entry point used by the portable network
 * adapter. It validates every bounded header locally before encoding it.
 * ::ra8_c6link_mdl_start is the empty-policy convenience wrapper.
 * @param[in,out] link Already-open exclusively owned c6link.
 * @param[in] request Complete typed HTTPS request and optional headers.
 * @param[out] session Accepted job correlation state.
 * @return Start status.
 * @retval k_ra8_ok Remote job accepted.
 * @retval k_ra8_err_null_ptr A required pointer is null.
 * @retval k_ra8_err_invalid_arg URL, format, timeout, or header is invalid.
 * @retval k_ra8_err_invalid_size A bounded string or request encoding is too large.
 * @retval k_ra8_err_protocol_error Remote response is malformed.
 * @pre ::ra8_c6link_open completed successfully for @p link.
 * @pre No other thread uses @p link concurrently.
 * @post Success produces an active non-zero job in @p session.
 * @post Failure leaves @p session inactive after pointer validation.
 * @note Not thread-safe; the c6link handle is single-owner.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_c6link_mdl_start_request(ra8_c6link_t*            link,
                                                     const ra8_mdl_request_t* request,
                                                     ra8_mdl_session_t*       session);

/**
 * @brief Pull the next bounded chunk while acknowledging the prior offset
 * @details Encodes the session's exact next sequence/offset contract and
 * advances it only after a correlated protobuf response passes validation.
 * @param[in,out] link Already-open exclusively owned c6link.
 * @param[in,out] session Active job correlation state.
 * @param[in] max_bytes Requested body-byte bound for this response.
 * @param[out] chunk Correlated response, including terminal metadata.
 * @return Pull status.
 * @retval k_ra8_ok A valid data or terminal response was decoded.
 * @retval k_ra8_err_null_ptr A required pointer is null.
 * @retval k_ra8_err_invalid_state Session is inactive or invalid.
 * @retval k_ra8_err_invalid_size Requested or encoded size exceeds a bound.
 * @retval k_ra8_err_protocol_error Job, sequence, offset, state, or fields are
 * incoherent.
 * @retval k_ra8_err_timeout C6 did not answer inside the RPC budget.
 * @pre @p session came from a successful ::ra8_c6link_mdl_start.
 * @pre `max_bytes` is non-zero and no larger than the negotiated maximum.
 * @post Success advances session correlation by exactly the returned data
 * length.
 * @post A terminal response makes @p session inactive.
 * @note Not thread-safe; the c6link handle and session are single-owner.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_c6link_mdl_next(ra8_c6link_t*      link,
                                            ra8_mdl_session_t* session,
                                            uint16_t           max_bytes,
                                            ra8_mdl_chunk_t*   chunk);

/**
 * @brief Cancel one active remote job and invalidate its local session
 * @details Sends the generated CancelRequest and validates the returned job id
 * before changing local state.
 * @param[in,out] link Already-open exclusively owned c6link.
 * @param[in,out] session Active job to cancel.
 * @return Cancellation status.
 * @retval k_ra8_ok Remote acknowledged cancellation and session is inactive.
 * @retval k_ra8_err_null_ptr A required pointer is null.
 * @retval k_ra8_err_invalid_state Session is not active.
 * @retval k_ra8_err_invalid_size Encoded request exceeds its static buffer.
 * @retval k_ra8_err_protocol_error Cancellation acknowledgement is incoherent.
 * @retval k_ra8_err_timeout C6 did not answer inside the RPC budget.
 * @pre @p session came from a successful ::ra8_c6link_mdl_start.
 * @pre No other thread uses @p link concurrently.
 * @post Success leaves @p session inactive.
 * @post Failure preserves the session for retry or caller recovery.
 * @note Not thread-safe; the c6link handle and session are single-owner.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_c6link_mdl_cancel(ra8_c6link_t* link, ra8_mdl_session_t* session);

#ifdef __cplusplus
}
#endif
