/**
 * @file ra8_vmem_stream.h
 * @brief Read a page-cached object as a seekable byte stream (Layer 2 helper).
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 2 / Core] {World: NS}
 *
 * @details
 * A thin adapter that turns a ::ra8_vmem-cached paged object into a random-access
 * byte-stream reader: ::ra8_vmem_stream_read_checked serves an arbitrary
 * `(offset, len)` span by paging the covering frames through the SLRU cache and
 * copying out the requested bytes, reporting a failed frame as an error rather
 * than as a short count. Hot pages (re-read headers, indices) stay
 * resident in the fixed frame pool; cold pages are re-fetched from the backing
 * through the cache's loader. The resident set never exceeds the cache's fixed
 * frame budget, independent of object size -- so a multi-GB object is readable
 * through a few tens of KiB of RAM.
 *
 * The read function's signature (opaque ctx, absolute offset, an `ra8_err_t`
 * verdict and a bytes-copied out-param) is deliberately generic so any streamed
 * consumer can drive it. In particular it is call-compatible with
 * `epub_open_streamed()`'s `epub_stream_read_fn`,
 * which is how a large `.epub` on the SD card is opened without whole-file
 * residency: register the file as a ::ra8_vsource paged object, front it
 * with a fixed ::ra8_vmem pool (the asserted RAM budget), and hand the resulting
 * ::ra8_vmem_stream_read to the EPUB reader.
 *
 * Zero allocation (NASA P10 Rule 3): all storage is the caller's ::ra8_vmem pool;
 * this adapter holds at most one pinned frame at a time and copies through it.
 *
 * @note Not thread-safe; the reader serialises access.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_vmem.h"

/**
 * @struct ra8_vmem_stream_t
 * @brief Binds one page-cached object to the byte-stream read adapter.
 *
 * @details Caller-owned; populate with ::ra8_vmem_stream_init and pass its address
 *          as the `ctx` of ::ra8_vmem_stream_read. Treat the fields as private.
 *
 * @invariant `frame_bytes == vm->cfg.frame_bytes` and `frame_bytes > 0`.
 * @since 0.1.0
 */
typedef struct {
  ra8_vmem_t* vm;          /**< Initialised page cache (fixed pool = RAM budget). */
  uint32_t    object_id;   /**< Paged object id registered in the cache's source. */
  uint32_t    frame_bytes; /**< Cache frame size, cached from `vm->cfg` at init.  */
  uint64_t    size;        /**< Object length in bytes.                           */
} ra8_vmem_stream_t;

/**
 * @brief Bind a page-cached paged object to the byte-stream reader.
 *
 * @param[out] st        Stream binding to populate (zero-initialised by caller).
 * @param[in]  vm        Initialised cache whose loader serves @p object_id.
 * @param[in]  object_id A paged object id registered with @p vm's source.
 * @param[in]  size      Object length in bytes (> 0).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Bound; ::ra8_vmem_stream_read may be used.
 * @retval k_ra8_err_null_ptr     `st` or `vm` was NULL.
 * @retval k_ra8_err_invalid_size `size` was 0, or `vm`'s frame size was 0.
 *
 * @pre `vm` was populated by ::ra8_vmem_init and its loader serves @p object_id.
 * @pre `st` out-lives every ::ra8_vmem_stream_read call that uses it.
 * @post On success `st->frame_bytes == vm->cfg.frame_bytes` and `st->size == size`.
 * @post On any non-ok return `st` is left unbound.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_vmem_stream_init(ra8_vmem_stream_t* st, ra8_vmem_t* vm, uint32_t object_id, uint64_t size);

/**
 * @brief Read `len` bytes at absolute `offset`, reporting failure separately from EOF.
 *
 * @details Pages the covering frames through the SLRU cache one at a time,
 *          copying each in-frame slice into @p buf, and clamps the request to the
 *          object end. This is the implementation; ::ra8_vmem_stream_read is a
 *          count-returning binding over it.
 *
 *          The distinction the count-returning form cannot express is the whole
 *          point of this one. A short read is two different events:
 *          - **End of file.** `k_ra8_ok` with `*out_read < len`, because the
 *            request ran past the object end. `*out_read == 0` when @p offset is
 *            already at or after the end.
 *          - **A failed frame.** The cache's error, with `*out_read` holding the
 *            bytes copied before the failure. Those bytes are genuine; the rest of
 *            the span is not readable right now.
 *
 *          A consumer that treats the second case as the first compiles a
 *          truncated object out of a failing card and reports success.
 *
 * @param[in]  st       Bound stream (::ra8_vmem_stream_init).
 * @param[in]  offset   Absolute byte offset within the object.
 * @param[out] buf      Destination buffer (`len` writable bytes).
 * @param[in]  len      Bytes requested (> 0).
 * @param[out] out_read Receives the bytes copied, on every return path.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Nothing failed; `*out_read` is `len`, or less at EOF.
 * @retval k_ra8_err_null_ptr      `st`, `buf` or `out_read` was NULL.
 * @retval k_ra8_err_invalid_size  `len` was 0.
 * @retval k_ra8_err_invalid_state `st` was not bound (frame size 0).
 * @retval other                   The cache's own error for the frame that failed.
 *
 * @pre `buf` is writable for `len` bytes.
 * @pre The bound cache and its source out-live this call.
 * @post `*out_read` is set on every path that has an `out_read` to set, including
 *       every failure, so a partial copy is always accounted for.
 * @post `*out_read <= len`, and the bytes below it are the object's real bytes.
 * @post At most one cache frame is pinned at any instant during the copy.
 * @post No state outside `buf`, `*out_read` and the cache's LRU order is modified.
 *
 * @note Not thread-safe.
 * @see ra8_vmem_stream_read  The count-returning binding for the callback seam.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vmem_stream_read_checked(ra8_vmem_stream_t* st,
                                                     uint64_t           offset,
                                                     void*              buf,
                                                     uint32_t           len,
                                                     uint32_t*          out_read);

/**
 * @brief Read `len` bytes at absolute `offset` through an opaque-cookie seam.
 *
 * @details The callback binding for seams typed as ::epub_stream_read_fn --
 *          `ra8_err_t (*)(void*, uint64_t, void*, uint32_t, uint32_t*)` -- which
 *          is what `epub_open_streamed()` takes. It differs from
 *          ::ra8_vmem_stream_read_checked only in taking the binding as a void
 *          cookie; the verdict and the byte count are passed through untouched.
 *
 *          It used to return a bare `size_t`, which is what made a dead card
 *          indistinguishable from a clean end of file all the way up into the
 *          book importer. Nothing is discarded here any more.
 *
 * @param[in]  ctx      The ::ra8_vmem_stream_t binding (as a void cookie).
 * @param[in]  offset   Absolute byte offset within the object.
 * @param[out] buf      Destination buffer (`len` writable bytes).
 * @param[in]  len      Bytes requested (> 0).
 * @param[out] out_read Receives the bytes copied, on every return path.
 *
 * @return ra8_err_t Error code, exactly ::ra8_vmem_stream_read_checked's.
 * @retval k_ra8_ok                Nothing failed; `*out_read` is `len`, or less at EOF.
 * @retval k_ra8_err_null_ptr      `ctx`, `buf` or `out_read` was NULL.
 * @retval k_ra8_err_invalid_size  `len` was 0.
 * @retval k_ra8_err_invalid_state `ctx` was not bound (frame size 0).
 * @retval other                   The cache's own error for the frame that failed.
 *
 * @pre `ctx` is a bound ::ra8_vmem_stream_t; `buf` is writable for `len` bytes.
 * @pre The bound cache and its source out-live this call.
 * @post At most one cache frame is pinned at any instant during the copy.
 * @post No state outside `buf`, `*out_read` and the cache's LRU order is modified.
 *
 * @note Not thread-safe.
 * @see ra8_vmem_stream_read_checked  The typed door this binds.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_vmem_stream_read(void* ctx, uint64_t offset, void* buf, uint32_t len, uint32_t* out_read);

#ifdef __cplusplus
}
#endif
