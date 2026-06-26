/**
 * @file ra_io_stream_ram.h
 * @brief ra_io byte-stream sink over a caller-owned RAM buffer.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Appends written bytes into a fixed caller-owned buffer until it is full, then
 * reports ::k_ra_err_no_mem. Useful as a capture buffer for tests, an in-memory
 * log ring, or staging bytes before a bulk transfer. Host-friendly: pure memory.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"
#include "ra_io_stream.h"

/**
 * @struct ra_io_stream_ram_state_t
 * @brief Caller-owned private state for a RAM stream sink.
 *
 * @details
 * Zero-initialise and pass to ::ra_io_stream_ram_init. Treat the fields as
 * private; read the captured length back with ::ra_io_stream_ram_used. The
 * buffer and this state must out-live every call made through the stream.
 *
 * @invariant `len <= cap`.
 *
 * @since 0.1.0
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t* buf; /**< Caller-owned capture buffer (private). */
  uint32_t cap; /**< Buffer capacity in bytes (private).    */
  uint32_t len; /**< Bytes captured so far (private).       */
} ra_io_stream_ram_state_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Bind a RAM stream sink into a caller-owned stream handle.
 *
 * @param[out] s     Stream handle to bind (zero-initialised by the caller).
 * @param[out] state Caller-owned sink state to populate.
 * @param[in]  buf   Capture buffer of `cap` bytes.
 * @param[in]  cap   Buffer capacity in bytes (>= 1).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Sink bound; `s` is usable.
 * @retval k_ra_err_null_ptr     `s`, `state`, or `buf` was NULL.
 * @retval k_ra_err_invalid_size `cap` was zero.
 *
 * @pre `buf` covers at least `cap` bytes.
 * @pre `s` and `state` out-live every call made through the stream.
 * @post On success `s` appends written bytes into `buf`.
 * @post On any non-ok return `s` and `state` are left unbound/untouched.
 *
 * @note Not thread-safe with respect to the same stream.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_io_stream_ram_init(ra_io_stream_t*           s,
                                             ra_io_stream_ram_state_t* state,
                                             uint8_t*                  buf,
                                             uint32_t                  cap);

/**
 * @brief Report how many bytes have been captured into the RAM sink.
 *
 * @param[in]  state    Bound RAM sink state.
 * @param[out] out_used Number of captured bytes.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok           `*out_used` populated.
 * @retval k_ra_err_null_ptr `state` or `out_used` was NULL.
 *
 * @pre `state` was populated by ::ra_io_stream_ram_init.
 * @pre `out_used` is writable.
 * @post On success `*out_used` holds the captured length.
 * @post No state is mutated.
 *
 * @note Thread-safe (pure read).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_io_stream_ram_used(const ra_io_stream_ram_state_t* state,
                                             uint32_t*                       out_used);

#ifdef __cplusplus
}
#endif
