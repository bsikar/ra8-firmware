/**
 * @file ra8_io_stream_posix.h
 * @brief Borrowed raw-descriptor backend for the portable byte-stream facade.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / Host Port] {World: Host}
 *
 * @details
 * Binds a caller-owned ::ra8_io_stream_t to an already-open POSIX descriptor.
 * Writes use the kernel descriptor API directly, so the adapter owns no
 * `FILE`, C-runtime buffering, allocation, path, or descriptor lifetime.
 * Composition may bind stdout, stderr, a pipe, or another blocking descriptor;
 * the caller remains responsible for opening and closing it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_io_stream.h"

/** @brief Caller-owned state retained by one POSIX stream binding. */
typedef struct {
  int fd; /**< Borrowed writable descriptor; never closed by the adapter. */
} ra8_io_stream_posix_state_t;

/**
 * @brief Bind a borrowed writable descriptor as a byte-stream sink.
 * @param[out] stream Stream handle to initialize.
 * @param[out] state Caller-owned adapter state retained by @p stream.
 * @param[in] fd Open writable descriptor borrowed for the binding lifetime.
 * @return Canonical binding status.
 * @retval k_ra8_ok The stream is bound to @p fd.
 * @retval k_ra8_err_null_ptr A required pointer is null.
 * @retval k_ra8_err_invalid_arg @p fd is negative.
 * @pre @p fd remains open and writable until the last operation through
 *      @p stream has completed.
 * @pre The caller exclusively owns @p stream and @p state while binding.
 * @post Success initializes both @p stream and @p state.
 * @post Failure leaves both objects unchanged.
 * @note Thread-safe across distinct descriptors and state objects.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_io_stream_posix_init(ra8_io_stream_t* stream, ra8_io_stream_posix_state_t* state, int fd);

#ifdef __cplusplus
}
#endif
