/**
 * @file mdl_cli_internal.h
 * @brief Private byte-stream helpers shared by media_dl CLI units.
 * @details Declares bounded diagnostic fragment composition used by the split
 *          CLI parser and usage presenters.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>

#include "ra8_attributes.h"
#include "ra8_io_stream.h"

/**
 * @brief Write a bounded ordered vector of NUL-terminated text fragments.
 * @param[in,out] stream Bound destination stream.
 * @param[in] parts Array of borrowed text pointers.
 * @param[in] count Number of entries in @p parts.
 * @return Canonical stream or argument status.
 * @retval k_ra8_ok Every fragment was accepted in order.
 * @retval k_ra8_err_null_ptr A required pointer or fragment was null.
 * @retval k_ra8_err_invalid_size @p count exceeds the fixed fragment ceiling.
 * @retval other The destination rejected a fragment.
 * @pre @p parts covers @p count readable pointer entries.
 * @pre Every non-null fragment is NUL-terminated.
 * @post Success writes the concatenation without separators or terminators.
 * @post Failure stops at the first rejected or invalid fragment.
 * @note Thread-safe across distinct streams.
 * @since 0.1.0

 * @details Writes fragments in order through the injected stream.
 *          The first sink error stops later writes and is returned unchanged.
 */
RA8_PRIV ra8_err_t priv_mdl_cli_put_parts(ra8_io_stream_t*   stream,
                                          const char* const* parts,
                                          size_t             count);

/**
 * @brief Write a rejection diagnostic and return invalid-argument status.
 * @param[in,out] stream Bound diagnostic stream.
 * @param[in] parts Ordered fragments forming one complete diagnostic.
 * @param[in] count Number of entries in @p parts.
 * @return Invalid-argument after a complete write, or the stream failure.
 * @retval k_ra8_err_invalid_arg Every diagnostic fragment was accepted.
 * @retval other Argument validation or destination write failed first.
 * @pre @p parts includes any required prefix and newline.
 * @pre @p stream is exclusively owned for the call.
 * @post A successful diagnostic is never reported as command success.
 * @post Stream failures are not hidden behind validation status.
 * @note Thread-safe across distinct streams.
 * @since 0.1.0

 * @details Writes fragments in order through the injected stream.
 *          The first sink error stops later writes and is returned unchanged.
 */
RA8_PRIV ra8_err_t priv_mdl_cli_reject_parts(ra8_io_stream_t*   stream,
                                             const char* const* parts,
                                             size_t             count);
