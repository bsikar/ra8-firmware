/**
 * @file ra8_io_stream_backend.h
 * @brief Public implementer contract for ra8_io byte-stream backends.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Defines the narrow backend vtable and binding helper needed by an out-of-tree
 * or platform adapter. Consumers continue to include only `ra8_io_stream.h`;
 * implementers include this header, define one immutable vtable, and bind it to
 * caller-owned state. Publishing this seam prevents a POSIX, RTOS, or board
 * adapter from reaching into a sibling library's private `src/` directory.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_io_stream.h"

/**
 * @struct ra8_io_stream_iface
 * @brief One immutable vtable per byte-sink backend.
 *
 * @details
 * `write` is mandatory. It must publish an accepted count no larger than
 * `len`; success requires the count to equal `len`. `flush` may be NULL for an
 * unbuffered sink, which the facade maps to success.
 *
 * @invariant `write` is non-NULL.
 *
 * @since 0.1.0
 */
struct ra8_io_stream_iface {
  /**
   * @brief Write `len` bytes from `buf`, reporting the accepted count.
   * @details `out_written` receives the bytes the sink consumed (<= `len`).
   */
  ra8_err_t (*write)(void* ctx, const uint8_t* buf, uint32_t len, uint32_t* out_written);

  /** @brief Commit any buffering. May be NULL (nothing buffered). */
  ra8_err_t (*flush)(void* ctx);
};

/**
 * @brief Bind a backend vtable and caller-owned state into a stream handle.
 * @param[out] stream Stream handle to initialize.
 * @param[in] iface Immutable backend operations.
 * @param[in,out] context Backend-owned state retained by the handle.
 * @return Canonical binding status.
 * @retval k_ra8_ok The handle is bound and ready.
 * @retval k_ra8_err_null_ptr A required pointer is null.
 * @retval k_ra8_err_invalid_arg The mandatory write operation is absent.
 * @pre @p iface and @p context outlive every operation through @p stream.
 * @pre The caller exclusively owns @p stream while binding it.
 * @post Success replaces @p stream with the requested binding.
 * @post Failure leaves @p stream unchanged.
 * @note Thread-safe across distinct handles and backend states.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_io_stream_bind(ra8_io_stream_t* stream, const ra8_io_stream_iface_t* iface, void* context);

#ifdef __cplusplus
}
#endif
