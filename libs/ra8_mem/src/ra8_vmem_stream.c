/**
 * @file ra8_vmem_stream.c
 * @brief Read a page-cached object as a seekable byte stream -- impl (#147/#151).
 *
 * @par Tag
 * [Ring 2 / Core] {World: NS}
 *
 * @details
 * Serves an arbitrary `(offset, len)` span by walking the covering cache frames
 * one at a time: page in the frame that holds `cur` (`ra8_vmem_get`), copy the
 * in-frame slice, release the pin (`ra8_vmem_put`), and advance. At most one frame
 * is pinned at any instant, so the resident set is the caller's fixed `ra8_vmem`
 * pool plus O(1) -- never the object size.
 *
 * `ra8_vmem_stream_read_checked` is the load-bearing implementation: it returns
 * the house `ra8_err_t` and reports the byte count through an out-param, so a
 * failed page-in is distinguishable from end-of-file (#764). The byte-count-only
 * `ra8_vmem_stream_read` is a thin adapter over it for the `epub_stream_read_fn`
 * seam, and parks the error on the binding for `ra8_vmem_stream_last_err`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_vmem_stream.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_vmem.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_vmem_stream";

/**
 * @brief Park the first read failure on the binding; later ones do not overwrite it.
 * @details The legacy byte-count reader has nowhere to return an error, so the
 *          verdict lives on the binding instead (#764). First failure wins so a
 *          clean read after a fault cannot erase the fault.
 * @param[in,out] st  Bound stream (non-NULL).
 * @param[in]     err The failure to record (never ::k_ra8_ok here).
 * @post `st->last_err != k_ra8_ok` once any failure has been recorded.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_record_err(ra8_vmem_stream_t* st, ra8_err_t err)
{
  if (st->last_err == k_ra8_ok) {
    st->last_err = err;
  }
}

ra8_err_t
ra8_vmem_stream_init(ra8_vmem_stream_t* st, ra8_vmem_t* vm, uint32_t object_id, uint64_t size)
{
  RA8_CHECK_NULL_PTR(st, s_tag, "st must not be nullptr");
  RA8_CHECK_NULL_PTR(vm, s_tag, "vm must not be nullptr");
  if (size == 0U) {
    return k_ra8_err_invalid_size;
  }
  const uint32_t fb = vm->cfg.frame_bytes;
  if (fb == 0U) {
    return k_ra8_err_invalid_size;
  }
  st->vm          = vm;
  st->object_id   = object_id;
  st->frame_bytes = fb;
  st->size        = size;
  st->last_err    = k_ra8_ok;
  return k_ra8_ok;
}

ra8_err_t ra8_vmem_stream_read_checked(ra8_vmem_stream_t* st,
                                       uint64_t           offset,
                                       void*              buf,
                                       size_t             len,
                                       size_t*            out_read)
{
  RA8_CHECK_NULL_PTR(st, s_tag, "st must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  RA8_CHECK_NULL_PTR(out_read, s_tag, "out_read must not be nullptr");
  *out_read = 0U;
  if (st->frame_bytes == 0U) {
    return k_ra8_err_invalid_size; /* never bound by ra8_vmem_stream_init */
  }
  if (len == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (offset >= st->size) {
    return k_ra8_ok; /* at/after the object end: 0 bytes, and that is not a fault */
  }

  const uint64_t avail = st->size - offset;
  const size_t   want  = ((uint64_t)len > avail) ? (size_t)avail : len;

  uint8_t* const out  = (uint8_t*)buf;
  const uint32_t fb   = st->frame_bytes;
  size_t         done = 0U;
  uint64_t       cur  = offset;

  /* Bounded loop (NASA P10 Rule 2): every pass copies `chunk >= 1` bytes and
   * `done` rises monotonically toward the fixed `want`, so the loop runs at most
   * `want` times; `cur` advances by the same amount, walking frame by frame. */
  while (done < want) {
    const uint64_t frame_base = cur - (cur % (uint64_t)fb);
    const uint32_t in_frame   = (uint32_t)(cur - frame_base);
    const uint32_t frame_room = fb - in_frame;
    const size_t   remaining  = want - done;
    const size_t   chunk      = (remaining < (size_t)frame_room) ? remaining : (size_t)frame_room;

    void*     page = nullptr;
    ra8_err_t err  = ra8_vmem_get(st->vm, st->object_id, frame_base, &page);
    if (err != k_ra8_ok) {
      *out_read = done;
      internal_record_err(st, err);
      return err;
    }
    (void)memcpy(out + done, (const uint8_t*)page + in_frame, chunk);
    err = ra8_vmem_put(st->vm, page);
    if (err != k_ra8_ok) {          /* GCOVR_EXCL_START -- put fails only on a foreign page; */
      *out_read = done;             /*   the pin came from the get directly above            */
      internal_record_err(st, err);
      return err;
    } /* GCOVR_EXCL_STOP */
    done += chunk;
    cur += chunk;
  }
  *out_read = done;
  return k_ra8_ok;
}

size_t ra8_vmem_stream_read(void* ctx, uint64_t offset, void* buf, size_t len)
{
  ra8_vmem_stream_t* st = (ra8_vmem_stream_t*)ctx;
  if (st == nullptr) {
    return 0U;
  }
  size_t got = 0U;
  /* The checked read carries the verdict; this seam can only report the count,
   * so the error is parked on the binding for ra8_vmem_stream_last_err. Every
   * argument rejection below is the legacy contract's "return 0", unchanged. */
  (void)ra8_vmem_stream_read_checked(st, offset, buf, len, &got);
  return got;
}

ra8_err_t ra8_vmem_stream_last_err(const ra8_vmem_stream_t* st)
{
  RA8_CHECK_NULL_PTR(st, s_tag, "st must not be nullptr");
  return st->last_err;
}

ra8_err_t ra8_vmem_stream_clear_err(ra8_vmem_stream_t* st)
{
  RA8_CHECK_NULL_PTR(st, s_tag, "st must not be nullptr");
  st->last_err = k_ra8_ok;
  return k_ra8_ok;
}
