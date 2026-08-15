/**
 * @file cache_bench_io.h
 * @brief Bounded byte-source and text-sink seams for cache_bench.
 * @details Defines the ownership-neutral callbacks used by replay, reporting,
 *          scratch-backed containers, and host composition.
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Status returned by tool-local injected I/O callbacks. */
typedef enum : uint8_t {
  k_cb_io_ok       = 0U, /**< Operation completed.                    */
  k_cb_io_fault    = 1U, /**< Backing device or sink failed.          */
  k_cb_io_capacity = 2U, /**< Caller-owned storage was too small.     */
  k_cb_io_mutated  = 3U, /**< A replay source changed between passes. */
} cb_io_status_t;

/** @brief Read-at callback; zero bytes is EOF, short reads are permitted. */
typedef cb_io_status_t (
  *cb_source_read_fn)(void* ctx, uint64_t offset, uint8_t* dst, size_t capacity, size_t* out_read);

/** @brief Immutable injected byte source with a snapshotted length. */
typedef struct {
  cb_source_read_fn read; /**< Read-at implementation.   */
  void*             ctx;  /**< Caller-owned binding.     */
  uint64_t          size; /**< Size observed when bound. */
} cb_source_t;

/** @brief Write callback; short writes are permitted. */
typedef cb_io_status_t (*cb_sink_write_fn)(void*          ctx,
                                           const uint8_t* data,
                                           size_t         length,
                                           size_t*        out_written);

/** @brief Injected output sink. */
typedef struct {
  cb_sink_write_fn write; /**< Write implementation. */
  void*            ctx;   /**< Caller-owned binding. */
} cb_sink_t;

/** @brief Random-access scratch-file callback. */
typedef cb_io_status_t (*cb_scratch_io_fn)(void* ctx, uint64_t offset, void* data, size_t length);

/** @brief Injected bounded random-access scratch transaction. */
typedef struct {
  cb_scratch_io_fn read;  /**< Complete read-at callback.  */
  cb_scratch_io_fn write; /**< Complete write-at callback. */
  void*            ctx;   /**< Caller-owned binding.       */
  uint64_t         size;  /**< Logical bytes written.      */
} cb_scratch_t;

/**
 * @brief Write every byte, retrying bounded short writes.
 * @details Repeatedly invokes the injected sink until @p length bytes complete,
 *          rejecting zero progress, over-counts, or the first sink failure.
 * @param[in,out] sink Destination binding.
 * @param[in] data Bytes to publish.
 * @param[in] length Byte count.
 * @return ::k_cb_io_ok on complete publication, otherwise the first failure.
 * @retval k_cb_io_ok Every byte was accepted.
 * @retval k_cb_io_fault The binding failed or the sink made invalid progress.
 * @retval k_cb_io_capacity The injected sink reported insufficient capacity.
 * @pre @p sink has a non-NULL write callback.
 * @pre @p data is readable for @p length bytes when length is non-zero.
 * @post On success, exactly @p length bytes were published in order.
 * @post On failure, no retry occurs after the first terminal status.
 * @note Atomicity beyond an individual callback is provided by the bound sink.
 * @since 0.1.0
 */
cb_io_status_t cb_sink_write_all(cb_sink_t* sink, const void* data, size_t length);

/**
 * @brief Format one bounded record and publish it atomically to the sink seam.
 * @details Formats into fixed automatic storage, rejects truncation, then
 *          delegates complete publication to ::cb_sink_write_all.
 * @param[in,out] sink Destination binding.
 * @param[in] format printf-compatible format string.
 * @param[in] args Format arguments.
 * @return ::k_cb_io_ok, ::k_cb_io_capacity, or a sink failure.
 * @retval k_cb_io_ok The complete formatted record was published.
 * @retval k_cb_io_capacity Formatting exceeded the fixed record capacity.
 * @retval k_cb_io_fault A binding, formatting, or sink operation failed.
 * @pre @p sink and @p format are non-NULL.
 * @pre @p args matches the conversions in @p format.
 * @post On formatting failure, no bytes are offered to @p sink.
 * @post On success, exactly one complete formatted record is published.
 * @note The function acquires no storage and retains no `va_list` state.
 * @since 0.1.0
 */
cb_io_status_t cb_sink_vformat(cb_sink_t* sink, const char* format, va_list args);

/** @copydoc cb_sink_vformat */
cb_io_status_t cb_sink_format(cb_sink_t* sink, const char* format, ...);
