/**
 * @file ra8_c6link_mdl_service_internal.h
 * @brief Private bounded-allocation seam for the portable media service
 * @details Exposes the pure allocation-fit predicate and the three pure
 * validators to focused host tests; production allocation, dispatch, and
 * ownership remain in `ra8_c6link_mdl_service.c`.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_mdl_request.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Decide whether one aligned protobuf allocation fits an arena
 * @details Rejects pre-alignment overflow before proving both total and
 * remaining capacity for the fixed eight-byte protobuf alignment.
 * @param[in] used Bytes already consumed from the arena.
 * @param[in] len Requested allocation length before alignment.
 * @param[in] capacity Total arena byte capacity.
 * @return Whether the aligned request fits without arithmetic overflow.
 * @retval true Alignment and remaining-capacity checks both succeed.
 * @retval false Length arithmetic overflows or the request exceeds capacity.
 * @pre All inputs are byte counts representable by `size_t`.
 * @pre Alignment is the fixed media-service protobuf alignment.
 * @post No state or storage is modified.
 * @post True guarantees `used + aligned(len) <= capacity`.
 * @note Pure, reentrant, and exposed only for focused private tests.
 * @since 0.1.0
 */
/**
 * @struct mdl_start_view_t
 * @brief One decoded Start request as flat values, with no generated types.
 * @details The layout is stated by
 *          `src/internal/mdl_service_rules.zig@StartView`, which holds every
 *          rule about these values. The generated message layout is protoc-c
 *          output, so it is flattened here once rather than mirrored in Zig
 *          where a regeneration could drift it silently.
 * @invariant Every span borrows the per-dispatch decode arena and stays valid
 *            only for the dispatch that decoded it.
 * @since 0.1.0
 */
typedef struct {
  uint32_t    protocol_version;  /**< Claimed protocol version.           */
  const char* url;               /**< Decoded request URL.                */
  uint32_t    format;            /**< Generated format enumerator.        */
  uint32_t    timeout_ms;        /**< Caller-selected HTTP timeout.       */
  const char* user_agent;        /**< Decoded User-Agent or empty.        */
  const char* referer;           /**< Decoded Referer or empty.           */
  const char* if_none_match;     /**< Decoded If-None-Match or empty.     */
  const char* if_modified_since; /**< Decoded If-Modified-Since or empty. */
} mdl_start_view_t;

RA8_PRIV bool priv_c6link_mdl_decode_allocation_fits(size_t used, size_t len, size_t capacity);

/**
 * @brief Round one arena request up to the published span alignment.
 * @details Implemented by `src/internal/mdl_service_rules.zig@alignedSize`.
 *          Only meaningful once ::priv_c6link_mdl_decode_allocation_fits has
 *          accepted the same length, which is what rules out wrapping.
 * @param[in] len Requested bytes.
 * @return The rounded span size.
 * @pre The same @p len was already accepted as fitting.
 * @post No arena or service state is modified.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV size_t priv_c6link_mdl_decode_aligned_size(size_t len);

/**
 * @brief Validate one decoded optional request or response header.
 * @details Implemented by `src/internal/mdl_service_rules.zig@fieldValid`.
 *          Requires bounded single-line text, which is what stops a remote
 *          request from injecting extra headers into the backend's request.
 * @param[in] text Decoded protobuf string, or fixed response storage.
 * @param[in] cap Maximum extent including NUL.
 * @return Header validity.
 * @retval true Text terminates before @p cap and contains no CR or LF.
 * @retval false Pointer, bound, termination, or line discipline is invalid.
 * @pre @p cap is nonzero.
 * @post No decoded or service state is modified.
 * @note Empty strings are valid and mean the header is absent.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV bool priv_c6link_mdl_service_field_valid(const char* text, size_t cap);

/**
 * @brief Validate fixed terminal response metadata returned by a backend.
 * @details Implemented by
 *          `src/internal/mdl_service_rules.zig@responseValid`, whose
 *          `ResponseView` mirrors ::ra8_mdl_http_response_t.
 * @param[in] response Candidate status and selected headers.
 * @return Response validity.
 * @retval true Status is HTTP-shaped and every array is bounded single-line.
 * @retval false Status or a selected header violates the protocol contract.
 * @pre @p response is non-null and fully initialised by the backend.
 * @post No response or service state is modified.
 * @post True authorises protobuf packing of every selected header.
 * @note Pure and reentrant.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV bool priv_c6link_mdl_service_response_valid(
  const ra8_mdl_http_response_t* response);

/**
 * @brief Decide whether one decoded Start request may begin a job.
 * @details Implemented by `src/internal/mdl_service_rules.zig@startValid`.
 *          Requires an https URL with something after the scheme, since a bare
 *          scheme decodes fine and would reach the backend as a request for
 *          nothing.
 * @param[in] request Flattened decoded Start request.
 * @return Request validity.
 * @retval true Version, URL, format, timeout, and every header are valid.
 * @retval false Any one of those rules is violated.
 * @pre @p request borrows spans that stay live for the call.
 * @post No service state is modified.
 * @note Pure and reentrant.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV bool priv_c6link_mdl_service_start_valid(const mdl_start_view_t* request);

/**
 * @brief Decide whether a whole packed response fits caller storage.
 * @details Implemented by
 *          `src/internal/mdl_service_rules.zig@responseSizeOk`. Zero is
 *          refused: the service packs a whole response or none, so a zero
 *          length means the codec disagreed with itself.
 * @param[in] len Required packed bytes.
 * @param[in] response_cap Caller-owned response capacity.
 * @return Whether the complete response fits.
 * @retval true The response is non-empty and within capacity.
 * @retval false The length is zero or exceeds capacity.
 * @pre Both values are expressed in bytes.
 * @post No state or output buffer is modified.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] RA8_PRIV bool priv_c6link_mdl_service_response_size_ok(size_t len,
                                                                     size_t response_cap);

/**
 * @brief Judge one decoded request header exactly as dispatch does
 * @details Forwards unchanged to the module-private predicate, so a focused
 * test drives the shipped bound and line-discipline logic rather than a copy.
 * @param[in] text Candidate decoded protobuf string, or null.
 * @param[in] cap Maximum extent including the terminating NUL.
 * @return Header validity.
 * @retval true Text terminates before @p cap and contains no CR or LF.
 * @retval false Pointer, bound, termination, or line discipline is invalid.
 * @pre @p cap is nonzero.
 * @pre Non-null @p text is readable for at least @p cap bytes.
 * @post No service or caller state is modified.
 * @post True authorizes passing the string to the backend.
 * @note Test helper; pure and reentrant.
 * @par MC/DC:
 * The CR/LF decision cannot be driven to its two single-byte vectors through
 * ::ra8_mdl_service_dispatch, whose decoded strings would each need a distinct
 * hand-packed protobuf request per condition.
 * @since 0.1.0
 */
RA8_TEST_HELPER bool ra8_mdl_service_field_valid_test(const char* text, size_t cap);

/**
 * @brief Judge one backend terminal response exactly as dispatch does
 * @details Forwards unchanged to the module-private predicate that gates every
 * COMPLETE response before its headers reach the generated packer.
 * @param[in] response Candidate status and selected headers.
 * @return Response validity.
 * @retval true Status is HTTP-shaped and every header is bounded single-line.
 * @retval false Status or a selected header violates the protocol contract.
 * @pre @p response is non-null and fully initialized.
 * @pre Every array member is readable for its declared extent.
 * @post No service or caller state is modified.
 * @post True authorizes protobuf packing of every selected header.
 * @note Test helper; pure and reentrant.
 * @par MC/DC:
 * Six conditions needing seven vectors. Reaching them through the public
 * dispatch would require a backend that returns a different single malformed
 * header per pull, which the read seam cannot express one condition at a time.
 * @since 0.1.0
 */
RA8_TEST_HELPER bool ra8_mdl_service_response_valid_test(const ra8_mdl_http_response_t* response);

/**
 * @brief Judge one packed-response length exactly as dispatch does
 * @details Forwards unchanged to the module-private capacity predicate every
 * pack path consults before it writes a byte into caller storage.
 * @param[in] len Packed length the generated codec reported.
 * @param[in] response_cap Capacity of the caller's response buffer.
 * @return Canonical capacity status.
 * @retval k_ra8_ok The packed response fits and is non-empty.
 * @retval k_ra8_err_invalid_size The length is zero or exceeds capacity.
 * @pre Both arguments are byte counts representable by `size_t`.
 * @pre @p response_cap is the actual writable response capacity.
 * @post No service or caller state is modified.
 * @post Success guarantees a following pack of @p len bytes is in bounds.
 * @note Test helper; pure and reentrant.
 * @par MC/DC:
 * The zero-length condition is unreachable through dispatch: every generated
 * response carries a non-default protocol version, so the codec never reports
 * zero. Only this seam can vary it independently of the capacity condition.
 * @since 0.1.0
 */
RA8_TEST_HELPER ra8_err_t ra8_mdl_service_check_size_test(size_t len, size_t response_cap);

#ifdef __cplusplus
}
#endif
