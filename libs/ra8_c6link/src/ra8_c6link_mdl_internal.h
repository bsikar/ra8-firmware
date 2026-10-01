/**
 * @file ra8_c6link_mdl_internal.h
 * @brief Private response-validation seams for the media RPC client
 * @details Exposes the three pure predicates that judge a decoded response to
 * focused host tests. Transport, correlation, and session ownership all remain
 * private to `ra8_c6link_mdl.c`; nothing here changes what production runs.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>

#include "ra8_attributes.h"
#include "ra8_c6link_mdl.h"
#include "ra8_media_download.pb-c.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Judge one decoded HTTP header exactly as the client does
 * @details Forwards unchanged to the module-private predicate, so a focused
 * test drives the shipped bound and line-discipline logic rather than a copy.
 * An absent header is valid; the C6 service sends the empty string for one it
 * did not observe.
 * @param[in] text Candidate decoded protobuf string, or null for absent.
 * @param[in] cap Maximum extent including the terminating NUL.
 * @return Header validity.
 * @retval true Absent, or terminates before @p cap with no CR or LF.
 * @retval false The header is unterminated or carries a header-injection byte.
 * @pre @p cap is nonzero.
 * @pre Non-null @p text is readable for at least @p cap bytes.
 * @post No decoded or session state is modified.
 * @post True authorizes copying the header into the public response.
 * @note Test helper; pure and reentrant.
 * @par MC/DC:
 * The CR/LF decision needs one vector per byte class, and the C6 model would
 * need a distinct hand-packed terminal response per vector to reach them.
 * @since 0.1.0
 */
/**
 * @struct mdl_http_headers_t
 * @brief Fixed-capacity storage for the four optional MDL HTTP request headers.
 * @details The layout is stated by `src/internal/mdl_request.zig@Headers`,
 *          which fills it; this declaration is the C view of that storage. An
 *          absent header travels as an empty string rather than as a dangling
 *          pointer into caller memory.
 * @invariant Every member is NUL-terminated for its whole declared capacity.
 * @since 0.1.0
 */
typedef struct {
  char user_agent[k_ra8_mdl_user_agent_max];       /**< User-Agent staging.        */
  char referer[k_ra8_mdl_referer_max];             /**< Referer staging.           */
  char if_none_match[k_ra8_mdl_etag_max];          /**< If-None-Match staging.     */
  char if_modified_since[k_ra8_mdl_http_date_max]; /**< If-Modified-Since staging. */
} mdl_http_headers_t;

/**
 * @brief Validate one optional HTTP field against its protocol bound.
 * @details Implemented by `src/internal/mdl_request.zig@httpFieldValid`.
 *          Treats null as absent and rejects CR/LF header injection.
 * @param[in] text Optional NUL-terminated field.
 * @param[in] cap Maximum extent including NUL.
 * @return Field validity.
 * @retval true Field is absent or bounded and single-line.
 * @retval false Field is unterminated, too large, or contains CR/LF.
 * @pre @p cap is nonzero.
 * @post No input or global state is modified.
 * @note Pure and reentrant.
 * @since 0.1.0
 */
/**
 * @struct mdl_chunk_view_t
 * @brief One decoded chunk response as flat values, with no generated types.
 * @details The layout is stated by `src/internal/mdl_chunk.zig@View`, which
 *          holds every rule about these values. The generated message layout
 *          is protoc-c output, so it is flattened here once rather than
 *          mirrored in Zig where it could drift against the regenerated code.
 * @invariant Every span borrows the decoding arena and stays valid only for
 *            the synchronous call that built the view.
 * @since 0.1.0
 */
typedef struct {
  uint32_t    job_id;        /**< Correlated remote job identifier.        */
  uint32_t    sequence;      /**< Zero-based response sequence.            */
  uint64_t    offset;        /**< Offset of the body bytes.                */
  uint64_t    total_bytes;   /**< Advertised total, or zero when unknown.  */
  uint8_t     state;         /**< Generated state, as ra8_mdl_state_t.     */
  int32_t     status;        /**< Remote failure status in FAILED state.   */
  const void* data;          /**< Decoded body bytes, or null when absent. */
  size_t      data_len;      /**< Valid bytes at `data`.                   */
  const void* sha256;        /**< Decoded digest, or null when absent.     */
  size_t      sha256_len;    /**< Valid bytes at `sha256`.                 */
  int32_t     http_status;   /**< Terminal HTTP status, zero when absent.  */
  const char* retry_after;   /**< Decoded Retry-After.                     */
  const char* etag;          /**< Decoded ETag.                            */
  const char* last_modified; /**< Decoded Last-Modified.                   */
  const char* content_type;  /**< Decoded Content-Type.                    */
} mdl_chunk_view_t;

/**
 * @brief Validate terminal HTTP metadata carried by one decoded chunk.
 * @details Implemented by `src/internal/mdl_chunk.zig@httpResponseValid`.
 *          Requires a real status only on COMPLETE and bounds every selected
 *          response header before any caller copy.
 * @param[in] view Flattened decoded chunk.
 * @return Metadata validity.
 * @retval true Metadata matches the chunk state and all string bounds.
 * @retval false Status, presence, termination, or a header bound is invalid.
 * @pre @p view borrows spans that stay live for the call.
 * @post No decoded or caller-owned state is modified.
 * @note Pure and reentrant for independent views.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV bool priv_c6link_mdl_http_response_valid(const mdl_chunk_view_t* view);

/**
 * @brief Validate the state-specific fields of one correlated chunk.
 * @details Implemented by `src/internal/mdl_chunk.zig@semanticsValid`.
 *          Enforces the data/digest/status combination each state admits and
 *          checks the totals overflow-safely first.
 * @param[in] view Flattened decoded chunk.
 * @return Whether the semantic combination is valid.
 * @retval true State-specific fields and totals are coherent.
 * @retval false A state, size, status, or digest rule is violated.
 * @pre @p view borrows spans that stay live for the call.
 * @post No caller or decoded state is modified.
 * @post True guarantees later bounded copies are size-safe.
 * @note Reentrant for independent views.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV bool priv_c6link_mdl_chunk_semantics_valid(const mdl_chunk_view_t* view);

/**
 * @brief Copy one validated chunk into caller storage and advance its session.
 * @details Implemented by `src/internal/mdl_chunk.zig@accept`. Correlation and
 *          semantics are checked first, so every copy here is size-safe.
 * @param[in] view Flattened decoded chunk.
 * @param[in,out] session Correlated caller session.
 * @param[out] chunk Caller chunk destination.
 * @return Remote terminal status.
 * @retval k_ra8_ok Data, completion, or cancellation was accepted.
 * @retval other The exact nonzero FAILED status supplied by the remote.
 * @pre Every pointer is non-null and validation already succeeded.
 * @post Session correlation advances once and terminal state deactivates it.
 * @note Not thread-safe for a shared session or chunk.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_c6link_mdl_accept_chunk(const mdl_chunk_view_t* view,
                                                ra8_mdl_session_t*      session,
                                                ra8_mdl_chunk_t*        chunk);

[[nodiscard]] RA8_PRIV bool priv_c6link_mdl_http_field_valid(const char* text, size_t cap);

/**
 * @brief Validate every caller-supplied start-request field before staging.
 * @details Implemented by `src/internal/mdl_request.zig@startRequestValid`.
 *          Reports the bounded URL length so the encoder copies exactly the
 *          length that was checked rather than re-deriving it.
 * @param[in] request Caller request; may be null.
 * @param[out] out_url_len Receives the validated URL length on success.
 * @return Validation status.
 * @retval k_ra8_ok Every field satisfies the documented contract.
 * @retval k_ra8_err_null_ptr @p request, its URL, or @p out_url_len is null.
 * @retval k_ra8_err_invalid_arg A field is out of range or malformed.
 * @post @p out_url_len holds the bounded URL length on success only.
 * @note Not thread-safe for a shared request structure.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV ra8_err_t
priv_c6link_mdl_start_request_valid(const ra8_mdl_request_t* request, size_t* out_url_len);

/**
 * @brief Copy every present optional HTTP header into bounded local storage.
 * @details Implemented by `src/internal/mdl_request.zig@stageHeaders`. Each
 *          field was length-checked first, so every copy is bounded by its own
 *          protocol maximum and an absent field stays an empty string.
 * @param[in] http Caller-supplied optional headers; members may be null.
 * @param[out] out Staging storage to fill.
 * @return Nothing.
 * @pre @p http and @p out are non-null.
 * @post Every present member is copied and NUL-terminated in @p out.
 * @note Not thread-safe for a shared @p out.
 * @since 0.1.0
 */
RA8_PRIV void priv_c6link_mdl_stage_headers(const ra8_mdl_http_policy_t* http,
                                            mdl_http_headers_t*          out);

RA8_TEST_HELPER bool ra8_c6link_mdl_http_field_valid_test(const char* text, size_t cap);

/**
 * @brief Judge one decoded response's HTTP metadata exactly as the client does
 * @details Forwards unchanged to the module-private predicate that separates a
 * non-terminal response, which must carry no metadata at all, from a COMPLETE
 * response, whose status must be HTTP-shaped and whose four selected headers
 * must each be bounded single-line text.
 * @param[in] msg Decoded generated chunk.
 * @return Metadata validity.
 * @retval true The metadata matches what this response's state permits.
 * @retval false A status or header rule for that state is violated.
 * @pre @p msg is non-null and decoded into a live bounded arena.
 * @pre Every string member is null or NUL-terminated within its bound.
 * @post No decoded or session state is modified.
 * @post True authorizes the state-specific semantic checks that follow.
 * @note Test helper; pure and reentrant.
 * @par MC/DC:
 * Two decisions, six conditions in the terminal one. Driving them through the
 * modelled transport would need one malformed-header fault per condition, and
 * the non-terminal decision would need a data response carrying metadata that
 * the service is structurally unable to emit.
 * @since 0.1.0
 */
RA8_TEST_HELPER bool ra8_c6link_mdl_http_response_valid_test(const Ra8__Mdl__Chunk* msg);

/**
 * @brief Judge one decoded response's state semantics exactly as the client
 * does
 * @details Forwards unchanged to the module-private predicate that enforces
 * the data/digest/status combination each state permits, plus the
 * overflow-safe relationship between offset, data length, and declared total.
 * @param[in] msg Decoded generated chunk.
 * @return Semantic validity.
 * @retval true State, size, status, and digest fields are mutually coherent.
 * @retval false A state-specific rule or the total-covers-data rule is broken.
 * @pre @p msg is non-null and decoded into a live bounded arena.
 * @pre Binary-data lengths describe their decoded buffers.
 * @post No decoded or session state is modified.
 * @post True guarantees the later bounded copies are size-safe.
 * @note Test helper; pure and reentrant.
 * @par MC/DC:
 * Five decisions across four mutually exclusive states, up to six conditions
 * each. Every vector needs one field of one state changed in isolation, which
 * a transport fault cannot express without a new injection per condition.
 * @since 0.1.0
 */
RA8_TEST_HELPER bool ra8_c6link_mdl_chunk_semantics_valid_test(const Ra8__Mdl__Chunk* msg);

/**
 * @brief Judge one cancellation acknowledgement exactly as the client does.
 * @details Builds the fields of the take context the cancelled path reads
 *          and runs the identical decode-and-correlate path, so a
 *          test observes the client's real acceptance rule rather than a
 *          reimplementation of it.
 * @param[in,out] link Open link whose bounded arena decodes the message.
 * @param[in,out] session Caller session the acknowledgement must correlate to.
 * @param[in] packed Packed generated Cancelled bytes.
 * @param[in] len Valid bytes at @p packed.
 * @return Decode status.
 * @retval k_ra8_ok A matching acknowledgement deactivated @p session.
 * @retval k_ra8_err_protocol_error Decode or correlation validation failed.
 * @pre @p link is open and @p session carries the expected job identity.
 * @pre @p packed is readable for @p len bytes.
 * @post Success makes @p session inactive; failure preserves its state.
 * @post The decoded message is released before return; no decoded pointer
 *       escapes into @p session or to the caller.
 * @note Test helper; not thread-safe for a shared link or session.
 * @since 0.1.0
 */
RA8_TEST_HELPER ra8_err_t ra8_c6link_mdl_take_cancelled_test(ra8_c6link_t*      link,
                                                             ra8_mdl_session_t* session,
                                                             const uint8_t*     packed,
                                                             size_t             len);

#ifdef __cplusplus
}
#endif
