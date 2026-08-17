/**
 * @file mdl_stream_internal.h
 * @brief Private bounded text helpers over the portable byte-stream contract.
 * @details Declares non-owning helpers that render fragments, integers, hex,
 *          repetition, and flushes through ::ra8_io_stream_t.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_io_stream.h"

/**
 * @brief Append text unless an earlier operation already failed.
 * @param[in] prior Status from the preceding append.
 * @param[in,out] stream Bound destination stream.
 * @param[in] text NUL-terminated text fragment.
 * @return @p prior when failed, otherwise the current append status.
 * @pre @p text is NUL-terminated and @p stream remains bound.
 * @pre @p prior is a canonical status.
 * @post The first failure in an append chain is preserved.
 * @post Success appends exactly the text bytes without a terminator.
 * @note Not thread-safe for concurrent use of one stream.
 * @since 0.1.0

 * @details Performs one synchronous operation against the injected stream.
 *          An earlier error is returned unchanged and prevents later stream access.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 */
RA8_PRIV ra8_err_t priv_mdl_stream_text(ra8_err_t prior, ra8_io_stream_t* stream, const char* text);

/**
 * @brief Append one unsigned integer unless an earlier append failed.
 * @param[in] prior Status from the preceding append.
 * @param[in,out] stream Bound destination stream.
 * @param[in] value Unsigned value rendered in decimal.
 * @return @p prior when failed, otherwise the current append status.
 * @pre @p stream remains bound and exclusively owned.
 * @pre @p prior is a canonical status.
 * @post The first failure in an append chain is preserved.
 * @post Success appends the exact locale-independent decimal identity.
 * @note Not thread-safe for concurrent use of one stream.
 * @since 0.1.0

 * @details Performs one synchronous operation against the injected stream.
 *          An earlier error is returned unchanged and prevents later stream access.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 */
RA8_PRIV ra8_err_t priv_mdl_stream_u64(ra8_err_t prior, ra8_io_stream_t* stream, uint64_t value);

/**
 * @brief Append one hexadecimal error/status value after prior success.
 * @param[in] prior Status from the preceding append.
 * @param[in,out] stream Bound destination stream.
 * @param[in] value Unsigned value rendered without a prefix.
 * @return @p prior when failed, otherwise the current append status.
 * @pre @p stream remains bound and exclusively owned.
 * @pre @p prior is a canonical status.
 * @post The first failure in an append chain is preserved.
 * @post Success appends at least one lowercase hexadecimal digit.
 * @note Not thread-safe for concurrent use of one stream.
 * @since 0.1.0

 * @details Performs one synchronous operation against the injected stream.
 *          An earlier error is returned unchanged and prevents later stream access.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 */
RA8_PRIV ra8_err_t priv_mdl_stream_hex(ra8_err_t prior, ra8_io_stream_t* stream, uint32_t value);

/**
 * @brief Append a bounded repeated byte after prior success.
 * @param[in] prior Status from the preceding append.
 * @param[in,out] stream Bound destination stream.
 * @param[in] byte Byte repeated in the destination.
 * @param[in] count Repetition count, at most 64.
 * @return @p prior when failed, otherwise the append status.
 * @retval k_ra8_err_invalid_size @p count exceeded the fixed bound.
 * @pre @p stream remains bound and exclusively owned.
 * @pre @p prior is a canonical status.
 * @post The first failure in an append chain is preserved.
 * @post Success appends exactly @p count copies of @p byte.
 * @note Not thread-safe for concurrent use of one stream.
 * @since 0.1.0

 * @details Performs one synchronous operation against the injected stream.
 *          An earlier error is returned unchanged and prevents later stream access.
 */
RA8_PRIV ra8_err_t priv_mdl_stream_repeat(ra8_err_t        prior,
                                          ra8_io_stream_t* stream,
                                          char             byte,
                                          size_t           count);

/**
 * @brief Flush a stream only when every earlier append succeeded.
 * @param[in] prior Status from the preceding append.
 * @param[in,out] stream Bound destination stream.
 * @return @p prior when failed, otherwise the flush status.
 * @pre @p stream remains bound and exclusively owned.
 * @pre @p prior is a canonical status.
 * @post The first failure in an append chain is preserved.
 * @post Success commits all sink-side buffered bytes.
 * @note Not thread-safe for concurrent use of one stream.
 * @since 0.1.0

 * @details Performs one synchronous operation against the injected stream.
 *          An earlier error is returned unchanged and prevents later stream access.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval other The originating validation, storage, stream, or network error.
 */
RA8_PRIV ra8_err_t priv_mdl_stream_flush(ra8_err_t prior, ra8_io_stream_t* stream);
