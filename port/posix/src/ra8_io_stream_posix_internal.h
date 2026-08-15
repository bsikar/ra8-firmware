/**
 * @file ra8_io_stream_posix_internal.h
 * @brief Private exact-write engine for the POSIX stream adapter.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / Host Port] {World: Host}
 *
 * @details
 * Declares the cross-translation-unit private write loop and its injected raw
 * syscall shape. Production supplies `write(2)`; focused tests supply bounded
 * deterministic results without mutable global hooks.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"

/** @brief Injected descriptor-write primitive used by the exact-write loop. */
typedef int64_t (*ra8_io_stream_posix_write_fn_t)(void*          context,
                                                  int            fd,
                                                  const uint8_t* bytes,
                                                  uint32_t       length,
                                                  int*           out_errno);

/**
 * @brief Complete one descriptor write through an injected raw primitive.
 *
 * @details
 * Repeats interrupted attempts up to the fixed adapter ceiling, accumulates
 * only proven progress, and rejects zero or over-reported successful writes.
 *
 * @param[in] fd Writable descriptor forwarded to @p writer.
 * @param[in] bytes Source bytes.
 * @param[in] length Requested byte count.
 * @param[out] out_written Bytes accepted before success or failure.
 * @param[in] writer Raw write primitive.
 * @param[in,out] writer_context Opaque context forwarded to @p writer.
 * @return Canonical transfer status.
 * @retval k_ra8_ok Exactly @p length bytes were accepted.
 * @retval k_ra8_err_retry_limit The bounded interruption ceiling was exceeded.
 * @retval k_ra8_err_protocol_error The writer reported impossible progress.
 * @pre @p bytes spans @p length readable bytes.
 * @pre @p out_written and @p writer are non-null.
 * @post Success publishes exactly @p length accepted bytes.
 * @post Failure publishes only the proven accepted prefix.
 * @note Thread-safe across distinct writer contexts.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_ra8_io_stream_posix_write_loop(int                            fd,
                                                       const uint8_t*                 bytes,
                                                       uint32_t                       length,
                                                       uint32_t*                      out_written,
                                                       ra8_io_stream_posix_write_fn_t writer,
                                                       void* writer_context);
