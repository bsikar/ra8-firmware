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
#include "ra8_mdl_protocol.h"

/**
 * @struct ra8_mdl_session_t
 * @brief RA8-local state for one accepted remote raw-byte job
 * @details Maintains the correlation values the next response must carry.
 * Callers allocate it; the client never allocates or retains caller memory.
 * @invariant An active session has a non-zero `job_id` and `max_chunk_bytes`.
 * @invariant `next_offset` and `next_sequence` advance only after a valid response.
 * @code
 * ra8_mdl_session_t session = {};
 * (void)ra8_c6link_mdl_start(&link, "https://host/book.rabook", &session);
 * @endcode
 * @see ra8_c6link_mdl_start
 * @since 0.1.0
 */
typedef struct {
  uint32_t job_id;          /**< Remote-generated non-zero identifier.              */
  uint32_t next_sequence;   /**< Exact sequence required from the next response.    */
  uint64_t next_offset;     /**< Exact byte offset required from the next response. */
  uint32_t max_chunk_bytes; /**< Maximum accepted pull size negotiated at start.    */
  bool     active;          /**< Whether next/cancel is currently valid.            */
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
} ra8_mdl_chunk_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start raw HTTPS body retrieval and receive its job identifier
 * @details Encodes a generated protobuf StartRequest. It requests no media
 * conversion, export, or site scraping; those are RA8/application policy.
 * @param[in,out] link Already-open exclusively owned c6link.
 * @param[in] url NUL-terminated HTTPS source URL below ::k_ra8_mdl_url_max.
 * @param[out] session Accepted job correlation state.
 * @return Start status.
 * @retval k_ra8_ok Remote job accepted.
 * @retval k_ra8_err_null_ptr A required pointer is null.
 * @retval k_ra8_err_invalid_arg URL is empty.
 * @retval k_ra8_err_invalid_size URL or encoded request exceeds its bound.
 * @retval k_ra8_err_protocol_error Remote response is malformed.
 * @retval k_ra8_err_timeout C6 did not answer inside the RPC budget.
 * @pre ::ra8_c6link_open completed successfully for @p link.
 * @pre No other thread uses @p link concurrently.
 * @post Success produces an active non-zero job in @p session.
 * @post Failure leaves @p session inactive and zeroed after argument validation.
 * @note Not thread-safe; the c6link handle is single-owner.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_c6link_mdl_start(ra8_c6link_t* link, const char* url, ra8_mdl_session_t* session);

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
 * @retval k_ra8_err_protocol_error Job, sequence, offset, state, or fields are incoherent.
 * @retval k_ra8_err_timeout C6 did not answer inside the RPC budget.
 * @pre @p session came from a successful ::ra8_c6link_mdl_start.
 * @pre `max_bytes` is non-zero and no larger than the negotiated maximum.
 * @post Success advances session correlation by exactly the returned data length.
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
