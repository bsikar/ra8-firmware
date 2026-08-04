/**
 * @file ra8_vmem_stream.h
 * @brief Read a page-cached object as a seekable byte stream (Layer 2 helper, #147/#151).
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 2 / Core] {World: NS}
 *
 * @details
 * A thin adapter that turns a ::ra8_vmem-cached paged object into a random-access
 * byte-stream reader: ::ra8_vmem_stream_read serves an arbitrary
 * `(offset, len)` span by paging the covering frames through the SLRU cache and
 * copying out the requested bytes. Hot pages (re-read headers, indices) stay
 * resident in the fixed frame pool; cold pages are re-fetched from the backing
 * through the cache's loader. The resident set never exceeds the cache's fixed
 * frame budget, independent of object size -- so a multi-GB object is readable
 * through a few tens of KiB of RAM.
 *
 * The read function's signature (opaque ctx, absolute offset, bytes-read return)
 * is deliberately generic so any streamed consumer can drive it. In particular it
 * is call-compatible with `ra8_epub_open_streamed()`'s `ra8_epub_stream_read_fn`,
 * which is how a large `.epub` on the SD card is opened without whole-file
 * residency (#151): register the file as a ::ra8_vsource paged object, front it
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
 *
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
 * @brief Read `len` bytes at absolute `offset` through the page cache.
 *
 * @details Pages the covering frames through the SLRU cache one at a time,
 *          copying each in-frame slice into @p buf, and clamps the request to the
 *          object end. A read fully past the end returns 0; a partial read (a
 *          cache/loader failure mid-span) returns the bytes copied so far, which a
 *          consumer like `ra8_epub_open_streamed()` treats as end-of-file.
 *
 * @param[in]  ctx    The ::ra8_vmem_stream_t binding (as a void cookie).
 * @param[in]  offset Absolute byte offset within the object.
 * @param[out] buf    Destination buffer (`len` writable bytes).
 * @param[in]  len    Bytes requested.
 *
 * @return Bytes actually copied (0 at/after EOF or on the first failing frame).
 * @retval len  The full request was satisfied (every covering frame paged in).
 * @retval 0    `offset` is at/after the object end, or the first frame failed.
 * @retval <len A frame failed mid-span; the bytes copied before the failure.
 *
 * @pre `ctx` is a bound ::ra8_vmem_stream_t; `buf` is writable for `len` bytes.
 * @pre The bound cache and its source out-live this call.
 * @post At most one cache frame is pinned at any instant during the copy.
 * @post No state outside `buf` and the cache's LRU order is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
size_t ra8_vmem_stream_read(void* ctx, uint64_t offset, void* buf, size_t len);

#ifdef __cplusplus
}
#endif
