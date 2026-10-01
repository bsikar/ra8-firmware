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
 * @struct mdl_chunk_reply_t
 * @brief One validated backend pull, ready to encode as a Chunk
 * @details Borrowed spans only: the dispatcher keeps the body, digest, and
 * response storage alive for the call that reads this view.
 * @since 0.1.0
 */
typedef struct mdl_chunk_reply_t {
  uint64_t                       offset;   /**< Offset the body starts at.     */
  uint64_t                       total;    /**< Declared artifact size.        */
  const uint8_t*                 data;     /**< Body bytes, @c got of them.    */
  const uint8_t*                 digest;   /**< Terminal SHA-256, 32 bytes.    */
  const ra8_mdl_http_response_t* response; /**< Terminal HTTP metadata.        */
  uint32_t                       job_id;   /**< Job the Chunk belongs to.      */
  uint32_t                       sequence; /**< Sequence the Chunk is sent as. */
  uint16_t                       got;      /**< Body byte count.               */
  bool                           complete; /**< Whether the Chunk is terminal. */
} mdl_chunk_reply_t;

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
 * @brief Decode one NextRequest and admit it for a backend pull
 * @details Refuses any unknown field, checks the request names the active
 * job at its acknowledged offset with a legal bound, and proves the largest
 * data or terminal Chunk for that bound fits @p response_cap. Runs before
 * the backend is asked for a byte.
 * @param[in] request Packed NextRequest bytes.
 * @param[in] request_len Request length in bytes.
 * @param[in] job Correlation state of the service's one job.
 * @param[in] response_cap Capacity of the caller's response buffer.
 * @param[out] max_bytes Body bound the peer granted; zero on every refusal.
 * @return Admission status.
 * @retval k_ra8_ok The pull may be read.
 * @retval k_ra8_err_null_ptr A pointer argument is null.
 * @retval k_ra8_err_protocol_error Decode failed or an unknown field was present.
 * @retval k_ra8_err_invalid_state The request does not correlate.
 * @retval k_ra8_err_invalid_size The worst-case Chunk does not fit.
 * @pre @p request is readable for @p request_len bytes.
 * @post Service and backend state are never modified.
 * @note Reentrant.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV ra8_err_t priv_c6link_mdl_service_next_admit(const uint8_t*        request,
                                                                   size_t                request_len,
                                                                   const mdl_job_view_t* job,
                                                                   size_t                response_cap,
                                                                   uint32_t*             max_bytes);

/**
 * @brief Encode the Chunk for one admitted, coherent pull
 * @details Writes the data Chunk, or the terminal Chunk with digest and HTTP
 * metadata when @c complete is set, byte-identical to the reference encoder.
 * @param[in] reply Pull to encode.
 * @param[out] response Caller-owned Chunk buffer.
 * @param[in] response_cap Response capacity in bytes.
 * @param[out] response_len Bytes written; zero on every refusal.
 * @return Encode status.
 * @retval k_ra8_ok The Chunk was written.
 * @retval k_ra8_err_null_ptr A pointer argument or a span @p reply needs is null.
 * @retval k_ra8_err_invalid_size The Chunk does not fit.
 * @pre The pull was admitted with the same @p response_cap.
 * @post Service and backend state are never modified.
 * @note Reentrant.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV ra8_err_t priv_c6link_mdl_service_pack_chunk(const mdl_chunk_reply_t* reply,
                                                                   uint8_t*                 response,
                                                                   size_t                   response_cap,
                                                                   size_t*                  response_len);

/**
 * @brief Answer one CancelRequest for the service's active job
 * @details Decodes the request, refuses any unknown field, checks it names
 * the active job at the current protocol version, and writes the Cancelled
 * acknowledgement. Does not call the backend or change service state.
 * @param[in] request Packed CancelRequest bytes.
 * @param[in] request_len Request length in bytes.
 * @param[in] job Correlation state of the service's one job.
 * @param[out] response Caller-owned acknowledgement buffer.
 * @param[in] response_cap Response capacity in bytes.
 * @param[out] response_len Bytes written; zero on every refusal.
 * @return Reply status.
 * @retval k_ra8_ok The acknowledgement was written.
 * @retval k_ra8_err_null_ptr A pointer argument is null.
 * @retval k_ra8_err_protocol_error Decode failed or an unknown field was present.
 * @retval k_ra8_err_invalid_state The request does not name the active job.
 * @retval k_ra8_err_invalid_size The acknowledgement does not fit.
 * @pre @p request is readable for @p request_len bytes.
 * @pre @p response is writable for @p response_cap bytes.
 * @post A refusal leaves @p response untouched.
 * @post Service and backend state are never modified.
 * @note Reentrant.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV ra8_err_t priv_c6link_mdl_service_cancel(const uint8_t*         request,
                                                               size_t                 request_len,
                                                               const mdl_job_view_t*  job,
                                                               uint8_t*               response,
                                                               size_t                 response_cap,
                                                               size_t*                response_len);

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

RA8_TEST_HELPER bool ra8_c6link_mdl_http_field_valid_test(const char* text, size_t cap);

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
 * @brief Decode one packed Chunk response, correlate it, and accept it.
 * @details Zig implementation; the C declaration is the membrane.
 * @param[in] data Packed Chunk bytes; may be null only when @p len is zero.
 * @param[in] len Valid bytes at @p data; the decoder reads no further.
 * @param[in,out] session Active caller session.
 * @param[out] chunk Caller chunk destination.
 * @param[in] requested_bytes Largest body the caller asked for.
 * @return Decode, correlation, or remote terminal status.
 * @retval k_ra8_ok A data or successful terminal chunk was copied.
 * @retval k_ra8_err_protocol_error The bytes were malformed or the chunk
 *         did not belong to the session.
 * @retval other The exact nonzero FAILED status supplied by the remote.
 * @post Failure leaves @p session and @p chunk unchanged.
 * @note Not thread-safe for a shared session or chunk.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_c6link_mdl_take_chunk(const uint8_t*      data,
                                              size_t              len,
                                              ra8_mdl_session_t*  session,
                                              ra8_mdl_chunk_t*    chunk,
                                              uint32_t            requested_bytes);

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
 * @brief Decode a media-download Accepted response
 * @details Zig implementation in `src/internal/mdl_decode.zig`, following
 *          protobuf-c's scan rules: unknown fields are skipped and counted,
 *          and a known field on the wrong wire type is malformed.
 * @param[in] data Packed response bytes; may be NULL only when @p len is 0.
 * @param[in] len Valid bytes at @p data.
 * @param[out] out Flat view to fill.
 * @return Whether @p out was filled.
 * @retval false The bytes are malformed or an argument was NULL.
 * @pre None.
 * @post @p out is unchanged on failure.
 * @note Pure function of its arguments; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV bool
priv_c6link_mdl_decode_accepted(const uint8_t* data, size_t len, mdl_accepted_view_t* out);

/**
 * @brief Decode a media-download Cancelled response
 * @details Zig implementation in `src/internal/mdl_decode.zig`, following
 *          protobuf-c's scan rules: unknown fields are skipped and counted,
 *          and a known field on the wrong wire type is malformed.
 * @param[in] data Packed response bytes; may be NULL only when @p len is 0.
 * @param[in] len Valid bytes at @p data.
 * @param[out] out Flat view to fill.
 * @return Whether @p out was filled.
 * @retval false The bytes are malformed or an argument was NULL.
 * @pre None.
 * @post @p out is unchanged on failure.
 * @note Pure function of its arguments; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV bool
priv_c6link_mdl_decode_cancelled(const uint8_t* data, size_t len, mdl_cancelled_view_t* out);

/**
 * @brief Encode a checked media-download StartRequest
 * @details Zig implementation in `src/internal/mdl_encode.zig`; byte-identical
 *          to the reference protobuf encoder, empty and zero fields omitted per
 *          proto3. Reads the caller's request directly, so nothing is staged.
 * @param[in] request Request already accepted by
 *            priv_c6link_mdl_start_request_valid().
 * @param[in] url_len URL length that check reported.
 * @param[out] buf Destination buffer.
 * @param[in] capacity Bytes available in @p buf.
 * @param[out] out_len Bytes written; zero on failure.
 * @return Whether the encode was written.
 * @retval false An argument was NULL or the encode does not fit.
 * @pre @p request passed priv_c6link_mdl_start_request_valid().
 * @post @p out_len is always written when non-NULL.
 * @note Pure function of its arguments; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV bool priv_c6link_mdl_encode_start(const ra8_mdl_request_t* request,
                                                         size_t                   url_len,
                                                         uint8_t*                 buf,
                                                         size_t                   capacity,
                                                         size_t*                  out_len);

/**
 * @brief Encode a media-download NextRequest
 * @details Zig implementation in `src/internal/mdl_encode.zig`; byte-identical
 *          to the reference protobuf encoder, zero fields omitted per proto3.
 * @param[in] job_id Job identifier field.
 * @param[in] acknowledged_offset Acknowledged offset field.
 * @param[in] max_bytes Requested chunk byte limit field.
 * @param[out] buf Destination buffer.
 * @param[in] capacity Bytes available in @p buf.
 * @param[out] out_len Bytes written; zero on failure.
 * @return Whether the encode was written.
 * @retval false An argument was NULL or the encode does not fit.
 * @pre None.
 * @post @p out_len is always written when non-NULL.
 * @note Pure function of its arguments; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV bool priv_c6link_mdl_encode_next(uint32_t job_id,
                                                        uint64_t acknowledged_offset,
                                                        uint32_t max_bytes,
                                                        uint8_t* buf,
                                                        size_t   capacity,
                                                        size_t*  out_len);

/**
 * @brief Encode a media-download CancelRequest
 * @details Zig implementation in `src/internal/mdl_encode.zig`; byte-identical
 *          to the reference protobuf encoder, zero fields omitted per proto3.
 * @param[in] job_id Job identifier field.
 * @param[out] buf Destination buffer.
 * @param[in] capacity Bytes available in @p buf.
 * @param[out] out_len Bytes written; zero on failure.
 * @return Whether the encode was written.
 * @retval false An argument was NULL or the encode does not fit.
 * @pre None.
 * @post @p out_len is always written when non-NULL.
 * @note Pure function of its arguments; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV bool
priv_c6link_mdl_encode_cancel(uint32_t job_id, uint8_t* buf, size_t capacity, size_t* out_len);

#ifdef __cplusplus
}
#endif
