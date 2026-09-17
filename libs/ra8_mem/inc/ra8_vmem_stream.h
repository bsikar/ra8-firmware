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
 * Two read entry points serve that span. ::ra8_vmem_stream_read_checked is the
 * house shape (::ra8_err_t out, byte count through an out-param), so a storage
 * failure mid-span is never mistaken for a clean end-of-file (#764).
 * ::ra8_vmem_stream_read keeps the byte-count-only signature (opaque ctx, absolute
 * offset, bytes-read return) because it is deliberately generic so any streamed
 * consumer can drive it. In particular it
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
 * @invariant `last_err` is `k_ra8_ok` until a read fails, then holds the *first*
 *            such failure until ::ra8_vmem_stream_clear_err resets it.
 * @since 0.1.0
 */
typedef struct {
  ra8_vmem_t* vm;          /**< Initialised page cache (fixed pool = RAM budget). */
  uint32_t    object_id;   /**< Paged object id registered in the cache's source. */
  uint32_t    frame_bytes; /**< Cache frame size, cached from `vm->cfg` at init.  */
  uint64_t    size;        /**< Object length in bytes.                           */
  ra8_err_t   last_err;    /**< Sticky first failure seen by the legacy reader.   */
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
 * @post On success `st->last_err == k_ra8_ok` (a rebind starts from a clean verdict).
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
 * @details The house-shaped read (#764): the byte count leaves through
 *          @p out_read and the outcome through the return code, so a cache/loader
 *          failure mid-span is distinguishable from a clean end-of-file. Pages the
 *          covering frames through the SLRU cache one at a time, copying each
 *          in-frame slice into @p buf, and clamps the request to the object end.
 *
 *          A request at/after the object end is *not* an error: it returns
 *          ::k_ra8_ok with `*out_read == 0`. A request that straddles the end
 *          returns ::k_ra8_ok with `*out_read < len` (the clamped remainder). Only
 *          a failed frame page-in returns non-ok, and @p out_read then carries the
 *          bytes copied before the failure so the caller can still use them.
 *
 *          Shaped after ::ra8_vsource_read_fn and `ra8_cache_store_read` -- the
 *          seam *into* the page cache already carried errors; this is the seam
 *          back out of it.
 *
 * @param[in,out] st       Bound stream (its sticky ::ra8_vmem_stream_t::last_err
 *                         is updated on the first failure).
 * @param[in]     offset   Absolute byte offset within the object.
 * @param[out]    buf      Destination buffer (`len` writable bytes).
 * @param[in]     len      Bytes requested.
 * @param[out]    out_read Receives the number of bytes actually copied.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               The clamped span was served (possibly 0 at EOF).
 * @retval k_ra8_err_null_ptr     `st`, `buf` or `out_read` was NULL.
 * @retval k_ra8_err_invalid_size `len` was 0, or `st` was never bound.
 * @retval k_ra8_err_*            Propagated `ra8_vmem_get` / `_put` failure; the
 *                                bytes copied before it are in `*out_read`.
 *
 * @pre `st` was bound by ::ra8_vmem_stream_init; `buf` is writable for `len` bytes.
 * @pre The bound cache and its source out-live this call.
 * @post `*out_read <= len` on every return, including the failing ones.
 * @post At most one cache frame is pinned at any instant during the copy.
 * @post On a non-ok frame failure `st->last_err` holds that error (first wins).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vmem_stream_read_checked(ra8_vmem_stream_t* st,
                                                     uint64_t           offset,
                                                     void*              buf,
                                                     size_t             len,
                                                     size_t*            out_read);

/**
 * @brief Read `len` bytes at absolute `offset` through the page cache (legacy shape).
 *
 * @details Byte-count-only adapter over ::ra8_vmem_stream_read_checked, kept
 *          because it is call-compatible with `epub_stream_read_fn` (and miniz's
 *          `mz_file_read_func`), which is how a streamed `.epub` is opened. It
 *          cannot tell a caller *why* it came up short: a return `< len` means
 *          end-of-file **or** a failed frame. A caller that needs to know must
 *          either use ::ra8_vmem_stream_read_checked, or read the sticky
 *          ::ra8_vmem_stream_last_err afterwards -- which this function records on
 *          the binding precisely so a legacy callback site stays diagnosable.
 *
 * @param[in]  ctx    The ::ra8_vmem_stream_t binding (as a void cookie).
 * @param[in]  offset Absolute byte offset within the object.
 * @param[out] buf    Destination buffer (`len` writable bytes).
 * @param[in]  len    Bytes requested.
 *
 * @return Bytes actually copied (0 at/after EOF or on the first failing frame).
 * @retval len  The full request was satisfied (every covering frame paged in).
 * @retval 0    `offset` is at/after the object end, or the first frame failed.
 * @retval <len A frame failed mid-span, or the span was clamped to the object end.
 *
 * @pre `ctx` is a bound ::ra8_vmem_stream_t; `buf` is writable for `len` bytes.
 * @pre The bound cache and its source out-live this call.
 * @post At most one cache frame is pinned at any instant during the copy.
 * @post On a frame failure the binding's `last_err` holds that error (first wins).
 * @post No state outside `buf`, `last_err` and the cache's LRU order is modified.
 *
 * @note Not thread-safe.
 * @see ra8_vmem_stream_read_checked The shape to prefer in new code.
 * @see ra8_vmem_stream_last_err     How a legacy call site tells failure from EOF.
 * @since 0.1.0
 */
size_t ra8_vmem_stream_read(void* ctx, uint64_t offset, void* buf, size_t len);

/**
 * @brief Report the first read failure recorded on a binding since it was cleared.
 *
 * @details The escape hatch for a consumer that must hand
 *          ::ra8_vmem_stream_read to a byte-count-only seam (the streamed EPUB
 *          reader): drive the seam, then ask the binding whether any read under it
 *          actually failed, instead of inferring end-of-file from a short count.
 *          First failure wins, so a later clean read cannot mask an earlier fault.
 *
 * @param[in] st Bound stream (NULL yields ::k_ra8_err_null_ptr).
 *
 * @return ra8_err_t The sticky error, or ::k_ra8_ok if no read has failed.
 *
 * @post `st` is not modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vmem_stream_last_err(const ra8_vmem_stream_t* st);

/**
 * @brief Clear the sticky read failure recorded on a binding.
 *
 * @details Lets one binding be reused across independent spans (a per-entry
 *          read, a retry) without an earlier failure bleeding into the next
 *          verdict. ::ra8_vmem_stream_init already starts a binding clear.
 *
 * @param[in,out] st Bound stream to reset.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Cleared.
 * @retval k_ra8_err_null_ptr `st` was NULL.
 *
 * @post On k_ra8_ok `ra8_vmem_stream_last_err(st) == k_ra8_ok`.
 * @post No field other than `last_err` is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_vmem_stream_clear_err(ra8_vmem_stream_t* st);

#ifdef __cplusplus
}
#endif
