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
 * @struct mdl_accepted_view_t
 * @brief One decoded accepted response as flat values.
 * @details The layout is stated by `src/internal/mdl_session.zig@AcceptedView`.
 *          The generated message layout is protoc-c output, so it is flattened
 *          here once rather than mirrored in Zig where a regeneration could
 *          drift the copy silently.
 * @since 0.1.0
 */
typedef struct {
  uint32_t protocol_version; /**< Claimed protocol version.            */
  uint32_t job_id;           /**< Granted remote job identifier.       */
  uint32_t max_chunk_bytes;  /**< Largest chunk the remote will send.  */
  uint32_t format;           /**< Generated format enumerator echoed.  */
  uint32_t unknown_fields;   /**< Count of undecoded generated fields. */
} mdl_accepted_view_t;

/**
 * @struct mdl_chunk_key_view_t
 * @brief The correlation fields of one decoded chunk, as flat values.
 * @details The layout is stated by `src/internal/mdl_session.zig@ChunkKeyView`.
 *          Separate from ::mdl_chunk_view_t because correlation runs before the
 *          body is examined and needs no borrowed spans.
 * @since 0.1.0
 */
typedef struct {
  uint32_t protocol_version; /**< Claimed protocol version.            */
  uint32_t job_id;           /**< Claimed remote job identifier.       */
  uint32_t sequence;         /**< Claimed response sequence.           */
  uint64_t offset;           /**< Claimed offset of the body bytes.    */
  uint32_t data_len;         /**< Decoded body length.                 */
  bool     data_present;     /**< Whether a body pointer was decoded.  */
  uint32_t unknown_fields;   /**< Count of undecoded generated fields. */
} mdl_chunk_key_view_t;

/**
 * @struct mdl_cancelled_view_t
 * @brief One decoded cancellation acknowledgement as flat values.
 * @details The layout is stated by `src/internal/mdl_session.zig@CancelledView`.
 * @since 0.1.0
 */
typedef struct {
  uint32_t protocol_version; /**< Claimed protocol version.            */
  uint32_t job_id;           /**< Acknowledged remote job identifier.  */
  int32_t  status;           /**< Remote cancellation status.          */
  uint32_t unknown_fields;   /**< Count of undecoded generated fields. */
} mdl_cancelled_view_t;

/**
 * @brief Decide whether an accepted response opens a usable job.
 * @details Implemented by `src/internal/mdl_session.zig@acceptedValid`.
 * @param[in] view Flattened decoded accepted response.
 * @param[in] requested_format Format the response must echo.
 * @return Acceptance validity.
 * @retval true Protocol, job, chunk bound and format all hold.
 * @retval false Any one of them is wrong.
 * @pre @p view is non-null.
 * @post No input or global state is modified.
 * @note Pure and reentrant.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV bool priv_c6link_mdl_accepted_valid(const mdl_accepted_view_t* view,
                                                           uint32_t requested_format);

/**
 * @brief Open the session an accepted response granted.
 * @details Implemented by `src/internal/mdl_session.zig@activate`.
 * @param[in] view Flattened decoded accepted response.
 * @param[out] session Caller session to initialize.
 * @param[in] requested_format Format recorded on the session.
 * @pre @p view and @p session are non-null and @p view already validated.
 * @post @p session is active at sequence and offset zero.
 * @note Not thread-safe for a shared session.
 * @since 0.1.0
 */
RA8_PRIV void priv_c6link_mdl_session_activate(const mdl_accepted_view_t* view,
                                               ra8_mdl_session_t*         session,
                                               uint8_t                    requested_format);

/**
 * @brief Decide whether a chunk sits where the session is waiting.
 * @details Implemented by `src/internal/mdl_session.zig@chunkCorrelates`.
 * @param[in] view Flattened correlation fields of the decoded chunk.
 * @param[in] session Active caller session.
 * @param[in] requested_bytes Largest body the caller asked for.
 * @return Correlation validity.
 * @retval true Job, sequence, offset and body bound all match.
 * @retval false Any one of them is wrong.
 * @pre @p view and @p session are non-null.
 * @post No input or global state is modified.
 * @note Pure and reentrant.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV bool priv_c6link_mdl_chunk_correlates(const mdl_chunk_key_view_t* view,
                                                             const ra8_mdl_session_t*    session,
                                                             uint32_t requested_bytes);

/**
 * @brief Decide whether a cancellation acknowledges the active job.
 * @details Implemented by `src/internal/mdl_session.zig@cancelledValid`.
 * @param[in] view Flattened decoded cancellation.
 * @param[in] session Active caller session.
 * @return Acknowledgement validity.
 * @retval true Protocol, job and status all hold.
 * @retval false Any one of them is wrong.
 * @pre @p view and @p session are non-null.
 * @post No input or global state is modified.
 * @note Pure and reentrant.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV bool priv_c6link_mdl_cancelled_valid(const mdl_cancelled_view_t* view,
                                                            const ra8_mdl_session_t*    session);

/**
 * @brief Close a session whose cancellation was acknowledged.
 * @details Implemented by `src/internal/mdl_session.zig@deactivate`.
 * @param[in,out] session Session to deactivate.
 * @pre @p session is non-null.
 * @post @p session is inactive.
 * @note Not thread-safe for a shared session.
 * @since 0.1.0
 */
RA8_PRIV void priv_c6link_mdl_session_deactivate(ra8_mdl_session_t* session);

/**
 * @struct mdl_job_view_t
 * @brief Correlation state of the one job a service may be running
 * @details Flat read-only copy of the service fields a request is checked
 * against, so the rules never reach into ::ra8_mdl_service_t.
 * @since 0.1.0
 */
typedef struct mdl_job_view_t {
  uint64_t next_offset;   /**< Byte offset the next pull must acknowledge. */
  uint32_t active_job_id; /**< Correlation id of the running job.          */
  bool     active;        /**< Whether Next or Cancel is currently valid.  */
} mdl_job_view_t;

/**
 * @struct mdl_next_request_view_t
 * @brief Decoded NextRequest fields the correlation rules read
 * @since 0.1.0
 */
typedef struct mdl_next_request_view_t {
  uint64_t acknowledged_offset; /**< Offset the peer confirms it has.  */
  uint32_t protocol_version;    /**< Wire version the peer speaks.     */
  uint32_t job_id;              /**< Job the peer believes is running. */
  uint32_t max_bytes;           /**< Body bytes the peer will accept.  */
} mdl_next_request_view_t;

/**
 * @struct mdl_cancel_request_view_t
 * @brief Decoded CancelRequest fields the correlation rules read
 * @since 0.1.0
 */
typedef struct mdl_cancel_request_view_t {
  uint32_t protocol_version; /**< Wire version the peer speaks. */
  uint32_t job_id;           /**< Job the peer wants cancelled. */
} mdl_cancel_request_view_t;

/**
 * @struct mdl_pull_view_t
 * @brief One backend pull with the service state it must agree with
 * @since 0.1.0
 */
typedef struct mdl_pull_view_t {
  uint64_t next_offset;     /**< Offset this pull starts at.            */
  uint64_t total;           /**< Declared artifact size, 0 if unknown.  */
  uint32_t next_sequence;   /**< Sequence this pull would be packed as. */
  uint32_t max_data;        /**< Body bytes the peer permitted.         */
  uint16_t got;             /**< Body bytes the backend returned.       */
  bool     complete;        /**< Whether the pull is terminal.          */
  bool     response_valid;  /**< Whether terminal metadata is sane.     */
  int32_t  response_status; /**< HTTP status the backend reported.      */
} mdl_pull_view_t;

/**
 * @struct mdl_advance_t
 * @brief Job state after one pull has been read, packed, and sent
 * @since 0.1.0
 */
typedef struct mdl_advance_t {
  uint64_t next_offset;   /**< Offset the following pull must start at. */
  uint32_t next_sequence; /**< Sequence the following pull must carry.  */
  bool     active;        /**< Whether the job outlived this pull.      */
} mdl_advance_t;

/**
 * @brief Whether a NextRequest may act on the service's active job
 * @param[in] request Decoded request fields.
 * @param[in] job Live service correlation state.
 * @return Whether version, job id, acknowledged offset, and the requested
 * bound all agree with the running job.
 * @pre @p request and @p job are non-null.
 * @post No input is modified.
 * @note Pure and reentrant.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV bool priv_c6link_mdl_pull_next_correlates(
  const mdl_next_request_view_t* request,
  const mdl_job_view_t*          job);

/**
 * @brief Whether a CancelRequest may act on the service's active job
 * @param[in] request Decoded request fields.
 * @param[in] job Live service correlation state.
 * @return Whether version and job id agree with the running job.
 * @pre @p request and @p job are non-null.
 * @post No input is modified.
 * @note Pure and reentrant.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV bool priv_c6link_mdl_pull_cancel_correlates(
  const mdl_cancel_request_view_t* request,
  const mdl_job_view_t*            job);

/**
 * @brief Offset just past a returned body
 * @param[in] next_offset Offset the pull started at.
 * @param[in] got Body bytes returned.
 * @param[out] overflowed Set when the sum would wrap.
 * @return The end offset, or zero when @p overflowed is set.
 * @pre @p overflowed is non-null.
 * @post Only @p overflowed is written.
 * @note Pure and reentrant.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV uint64_t priv_c6link_mdl_pull_end_offset(uint64_t next_offset,
                                                                uint16_t got,
                                                                bool*    overflowed);

/**
 * @brief Whether a backend pull is coherent enough to pack
 * @param[in] view The pull and the service state it must agree with.
 * @return Whether byte count, terminal shape, sequence room, declared total,
 * and response metadata are all consistent.
 * @pre @p view is non-null.
 * @post No input is modified.
 * @note Pure and reentrant.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV bool priv_c6link_mdl_pull_coherent(const mdl_pull_view_t* view);

/**
 * @brief Job state after one pull was read, packed, and sent
 * @param[in] next_offset Offset the pull started at.
 * @param[in] next_sequence Sequence the pull was packed as.
 * @param[in] got Body bytes returned.
 * @param[in] complete Whether the pull was terminal.
 * @return The offset, sequence, and active flag the service adopts.
 * @pre The pull passed ::priv_c6link_mdl_pull_coherent.
 * @post No input is modified.
 * @note Pure and reentrant.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV mdl_advance_t priv_c6link_mdl_pull_advance(uint64_t next_offset,
                                                                  uint32_t next_sequence,
                                                                  uint16_t got,
                                                                  bool     complete);

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

/**
 * @struct mdl_envelope_view_t
 * @brief Flattened outer CustomRpc response, as the Zig envelope rules see it.
 * @since 0.1.0
 */
typedef struct mdl_envelope_view_t {
  uint32_t custom_msg_id; /**< Operation id the reply names.       */
  uint32_t operation;     /**< Operation id the call asked for.    */
  size_t   body_len;      /**< Bytes in the inner body.            */
  bool     body_present;  /**< Whether a body pointer was present. */
} mdl_envelope_view_t;

/**
 * @brief Report whether an id names a media operation
 * @details Zig implementation; the C declaration is the membrane, not a
 *          reimplementation of it.
 * @param[in] operation Candidate CustomRpc operation id.
 * @return Whether the id is one of start, next, or cancel.
 * @retval true The id names a media operation.
 * @retval false The id belongs to another protocol family.
 * @pre None.
 * @post No state is observed or changed.
 * @note Pure predicate; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV bool priv_c6link_mdl_envelope_operation_valid(uint32_t operation);

/**
 * @brief Report which generated response an operation is answered with
 * @details Zig implementation; the C declaration is the membrane, not a
 *          reimplementation of it.
 * @param[in] operation CustomRpc operation id the call used.
 * @return Expected ::mdl_take_kind_t value, or zero when the id is unknown.
 * @retval 0 The id names no media operation.
 * @pre None.
 * @post No state is observed or changed.
 * @note Pure mapping; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV uint8_t priv_c6link_mdl_envelope_kind_for(uint32_t operation);

/**
 * @brief Decide whether an outer CustomRpc response may be decoded
 * @details Zig implementation; the C declaration is the membrane, not a
 *          reimplementation of it. Refuses a reply that names a different
 *          operation than the call asked for, and one carrying no body.
 * @param[in] view Flattened outer response.
 * @return Expected ::mdl_take_kind_t value, or zero when the response is
 *         refused.
 * @retval 0 Identity or body made the response undecodable.
 * @pre @p view is non-null.
 * @post No state is observed or changed.
 * @note Pure predicate; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV uint8_t priv_c6link_mdl_envelope_accept(const mdl_envelope_view_t* view);

/**
 * @brief Decide which response extractor an outer CustomRpc reply may run
 * @details Zig implementation; the C declaration is the membrane, not a
 *          reimplementation of it. Folds the envelope decision and the
 *          caller's expectation into one answer, so a refused envelope and a
 *          reply carrying a different media response are the same outcome.
 * @param[in] view Flattened outer response.
 * @param[in] expected ::mdl_take_kind_t the initiating call selected.
 * @return Extractor to run as an ::mdl_take_kind_t value, or zero for none.
 * @retval 0 The reply is undecodable or is not the expected inner response.
 * @pre @p view is non-null.
 * @post No state is observed or changed.
 * @note Pure predicate; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV uint8_t priv_c6link_mdl_take_selected(const mdl_envelope_view_t* view,
                                                             uint8_t                    expected);

/**
 * @brief Decide whether a decoded chunk may reach the caller's session
 * @details Zig implementation; the C declaration is the membrane, not a
 *          reimplementation of it. Correlation and the state-specific chunk
 *          rules both have to hold, so the call site copies a chunk or
 *          refuses it on one answer.
 * @param[in] key Flattened correlation fields of the decoded chunk.
 * @param[in] view Flattened decoded chunk.
 * @param[in] session Active caller session.
 * @param[in] requested_bytes Largest body the caller asked for.
 * @return Admissibility of the decoded chunk.
 * @retval true The chunk correlates and its semantics hold.
 * @retval false Either half failed.
 * @pre @p key, @p view and @p session are non-null.
 * @post No input or global state is modified.
 * @note Pure and reentrant.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV bool priv_c6link_mdl_chunk_admissible(const mdl_chunk_key_view_t* key,
                                                             const mdl_chunk_view_t*     view,
                                                             const ra8_mdl_session_t*    session,
                                                             uint32_t requested_bytes);

/**
 * @brief Decide whether a cancel may be issued for a session
 * @details Zig implementation; the C declaration is the membrane, not a
 *          reimplementation of it. An active session always carries a
 *          non-zero job id, so a zero one means the caller kept a session
 *          across a failed start.
 * @param[in] session Caller-owned session the cancel would name.
 * @return Issue status.
 * @retval k_ra8_ok The session may be cancelled.
 * @retval k_ra8_err_invalid_state The session is inactive or uncorrelated.
 * @pre @p session is non-null.
 * @post No state is observed or changed.
 * @note Pure predicate; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV ra8_err_t priv_c6link_mdl_cancel_allowed(const ra8_mdl_session_t* session);

/**
 * @brief Decide whether a next request may ask for a span
 * @details Zig implementation; the C declaration is the membrane, not a
 *          reimplementation of it. The ask is bounded both by what the peer
 *          negotiated at accept time and by the protocol chunk ceiling, since
 *          a peer may offer more than this build can receive.
 * @param[in] session Caller-owned active session.
 * @param[in] max_bytes Span the caller wants from the next chunk.
 * @return Issue status.
 * @retval k_ra8_ok The ask is within both ceilings.
 * @retval k_ra8_err_invalid_state The session is inactive or uncorrelated.
 * @retval k_ra8_err_invalid_size The ask is zero or past a ceiling.
 * @pre @p session is non-null.
 * @post No state is observed or changed.
 * @note Pure predicate; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV ra8_err_t priv_c6link_mdl_next_allowed(const ra8_mdl_session_t* session,
                                                              uint16_t max_bytes);

/**
 * @brief Decide whether one encoded request is self-consistent
 * @details Zig implementation; the C declaration is the membrane, not a
 *          reimplementation of it. Fail-closed backstop against a codec
 *          defect rather than an input class: a sized message is never empty,
 *          a message whose fields were bounded first fits the request buffer,
 *          and pack() writes exactly what get_packed_size() counted.
 * @param[in] sized Byte count the encoder reported before packing.
 * @param[in] written Byte count the encoder reported after packing.
 * @param[in] capacity Bytes available in the link request buffer.
 * @return Whether the encode may be transmitted.
 * @retval false The encode is empty, oversized, or disagreed with itself.
 * @pre None.
 * @post No state is observed or changed.
 * @note Pure predicate; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV bool
priv_c6link_mdl_packed_coherent(size_t sized, size_t written, size_t capacity);

#ifdef __cplusplus
}
#endif
