/**
 * @file ra8_io_stream.h
 * @brief ra8_io targetable byte-stream facade -- one writer, many destinations.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * A stream is a byte sink the application names rather than couples to: bind a
 * backend (UART/SCI, USB-CDC, an in-RAM buffer, or a raw block device) into a
 * caller-owned ::ra8_io_stream_t, then write to it. This is the "do an output
 * operation and pick the target" half of the fabric -- the same `puts` / `putc`
 * / `write` / `put_u32` / `put_hex` call lands wherever the bound backend sends
 * it, exactly as a block-device backend is chosen for storage.
 *
 * The formatted helpers are deliberately varargs-free. The firmware traps
 * `_sbrk` and bans the newlib `printf` family (its `vfprintf` blows the
 * bounded-stack budget), so this layer offers small, bounded, no-allocation
 * primitives instead of a `fprintf`: integers go out through
 * ::ra8_io_stream_put_u32 /
 * ::ra8_io_stream_put_hex, strings through ::ra8_io_stream_puts.
 *
 * @code
 * static uint8_t s_buf[256];
 * static ra8_io_stream_ram_state_t s_st;
 * ra8_io_stream_t out = {};
 * (void)ra8_io_stream_ram_init(&out, &s_st, s_buf, sizeof(s_buf));
 * (void)ra8_io_stream_puts(&out, "temp=");
 * (void)ra8_io_stream_put_u32(&out, 42U);
 * (void)ra8_io_stream_putc(&out, '\n');
 * @endcode
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

/**
 * @struct ra8_io_stream_iface
 * @brief Opaque to stream users; backend implementers use the public backend
 *        contract in `ra8_io_stream_backend.h`.
 *
 * @details
 * Ordinary stream users never construct this table; they call a sink's
 * `_init()` helper. Portable backend implementers include
 * `ra8_io_stream_backend.h`, define an immutable table, and bind it into a
 * caller-owned ::ra8_io_stream_t with ::ra8_io_stream_bind.
 *
 * @since 0.1.0
 */
typedef struct ra8_io_stream_iface ra8_io_stream_iface_t;

/**
 * @struct ra8_io_stream_t
 * @brief Caller-allocated byte-stream handle binding a sink to its context.
 *
 * @details
 * Zero-initialise (`= {}`) and pass to a sink `_init()` helper, which fills the
 * vtable and context. Treat the fields as private; reach the sink only through
 * the `ra8_io_stream_*` functions. The handle and the sink's context must
 * out-live every call made through them.
 *
 * @invariant `iface` is non-NULL once a sink has been bound.
 *
 * @since 0.1.0
 */
typedef struct {
  const ra8_io_stream_iface_t* iface; /**< Bound sink vtable (private).    */
  void*                        ctx;   /**< Sink-private context (private). */
} ra8_io_stream_t;

/**
 * @brief Write `len` bytes to the bound sink.
 *
 * @details
 * Forwards to the sink's write primitive. `out_written` (if non-NULL) receives
 * the number of bytes the sink accepted, which may be less than `len` for a
 * bounded sink (e.g. a full RAM buffer).
 *
 * @param[in]  s           Bound stream handle.
 * @param[in]  buf         Source bytes.
 * @param[in]  len         Number of bytes to write.
 * @param[out] out_written Bytes accepted by the sink, or NULL if not needed.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  All `len` bytes accepted.
 * @retval k_ra8_err_null_ptr        `s` or `buf` was NULL.
 * @retval k_ra8_err_not_initialized No sink is bound to `s`.
 * @retval k_ra8_err_no_mem          A bounded sink could not accept all bytes.
 * @retval k_ra8_err_protocol_error  Backend reported an impossible count or
 *                                   success after a short write.
 *
 * @pre A sink has been bound into `s`.
 * @pre `buf` is readable for `len` bytes.
 * @post On success the sink has consumed all `len` bytes.
 * @post `*out_written` (when provided) holds the accepted byte count unless a
 *       backend published an impossible over-request count, in which case it
 *       is preserved and ::k_ra8_err_protocol_error is returned.
 *
 * @note Not thread-safe with respect to the same stream.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_io_stream_write(ra8_io_stream_t* s, const uint8_t* buf, uint32_t len, uint32_t* out_written);

/**
 * @brief Flush any sink-side buffering to its destination.
 *
 * @param[in] s Bound stream handle.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Pending bytes committed (or none pending).
 * @retval k_ra8_err_null_ptr        `s` was NULL.
 * @retval k_ra8_err_not_initialized No sink is bound to `s`.
 *
 * @pre A sink has been bound into `s`.
 * @pre The stream is idle (no concurrent writer).
 * @post On success the destination reflects every prior successful write.
 * @post No handle state is mutated by a flush of an unbuffered sink.
 *
 * @note Not thread-safe with respect to the same stream.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_stream_flush(ra8_io_stream_t* s);

/**
 * @brief Write a single byte to the bound sink.
 *
 * @param[in] s Bound stream handle.
 * @param[in] c Byte to write.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Byte written.
 * @retval k_ra8_err_null_ptr        `s` was NULL.
 * @retval k_ra8_err_not_initialized No sink is bound to `s`.
 * @retval k_ra8_err_no_mem          A bounded sink was full.
 *
 * @pre A sink has been bound into `s`.
 * @pre The stream is idle.
 * @post On success one byte was appended to the sink.
 * @post No more than one byte is consumed.
 *
 * @note Not thread-safe with respect to the same stream.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_stream_putc(ra8_io_stream_t* s, char c);

/**
 * @brief Write a NUL-terminated string (without the NUL) to the bound sink.
 *
 * @details
 * Scans up to a bounded maximum for the terminator (NASA Power-of-10 Rule 2),
 * then writes that many bytes. An unterminated or overlong string is rejected;
 * it is never silently truncated.
 *
 * @param[in] s   Bound stream handle.
 * @param[in] str NUL-terminated string.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  String written.
 * @retval k_ra8_err_null_ptr        `s` or `str` was NULL.
 * @retval k_ra8_err_not_initialized No sink is bound to `s`.
 * @retval k_ra8_err_no_mem          A bounded sink could not accept all bytes.
 * @retval k_ra8_err_invalid_size    No terminator appeared inside the scan
 * bound.
 *
 * @pre A sink has been bound into `s`.
 * @pre `str` is NUL-terminated.
 * @post On success the sink consumed the string's bytes.
 * @post The terminating NUL is never written.
 *
 * @note Not thread-safe with respect to the same stream.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_stream_puts(ra8_io_stream_t* s, const char* str);

/**
 * @brief Write `value` as unsigned decimal ASCII (no leading zeros).
 *
 * @param[in] s     Bound stream handle.
 * @param[in] value Value to render.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Digits written.
 * @retval k_ra8_err_null_ptr        `s` was NULL.
 * @retval k_ra8_err_not_initialized No sink is bound to `s`.
 * @retval k_ra8_err_no_mem          A bounded sink could not accept the digits.
 *
 * @pre A sink has been bound into `s`.
 * @pre The stream is idle.
 * @post On success the decimal rendering of `value` was written.
 * @post At most ten ASCII digits are produced.
 *
 * @note Not thread-safe; uses a bounded stack buffer (no allocation).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_stream_put_u32(ra8_io_stream_t* s, uint32_t value);

/**
 * @brief Write `value` as unsigned 64-bit decimal ASCII without leading zeros.
 * @param[in] s Bound stream handle.
 * @param[in] value Value to render.
 * @return Canonical stream status.
 * @retval k_ra8_ok Digits written.
 * @retval k_ra8_err_null_ptr @p s was null.
 * @retval k_ra8_err_not_initialized No sink is bound to @p s.
 * @retval k_ra8_err_no_mem A bounded sink could not accept the digits.
 * @pre A sink has been bound into @p s and the stream is idle.
 * @post Success writes the exact base-10 identity of @p value.
 * @post At most twenty ASCII digits are produced.
 * @note Not thread-safe for concurrent use of one stream.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_stream_put_u64(ra8_io_stream_t* s, uint64_t value);

/**
 * @brief Write `value` as lowercase hex ASCII, zero-padded to `min_digits`.
 *
 * @param[in] s          Bound stream handle.
 * @param[in] value      Value to render.
 * @param[in] min_digits Minimum digit count (1..8); pad with leading zeros.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Digits written.
 * @retval k_ra8_err_null_ptr        `s` was NULL.
 * @retval k_ra8_err_not_initialized No sink is bound to `s`.
 * @retval k_ra8_err_invalid_arg     `min_digits` is zero or greater than eight.
 * @retval k_ra8_err_no_mem          A bounded sink could not accept the digits.
 *
 * @pre A sink has been bound into `s`.
 * @pre `min_digits` is in 1..8.
 * @post On success the hex rendering of `value` was written (no `0x` prefix).
 * @post At most eight ASCII digits are produced.
 *
 * @note Not thread-safe; uses a bounded stack buffer (no allocation).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_io_stream_put_hex(ra8_io_stream_t* s, uint32_t value, uint8_t min_digits);

#ifdef __cplusplus
}
#endif
