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
#include "ra8_mdl_http.h"

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
RA8_PRIV bool priv_c6link_mdl_decode_allocation_fits(size_t used, size_t len, size_t capacity);

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
