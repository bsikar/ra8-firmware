/**
 * @file ra8_c6link_mdl_service_internal.h
 * @brief Private bounded-allocation seam for the portable media service
 * @details Exposes the pure allocation-fit predicate to focused host tests;
 * production allocation and ownership remain in `ra8_c6link_mdl_service.c`.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>

#include "ra8_attributes.h"

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

#ifdef __cplusplus
}
#endif
