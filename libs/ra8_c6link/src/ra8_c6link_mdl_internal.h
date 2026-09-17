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
