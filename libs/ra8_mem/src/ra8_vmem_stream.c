/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
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
 */

#include "ra8_vmem_stream.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_vmem.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_vmem_stream";

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
    return k_ra8_err_invalid_size; /* GCOVR_EXCL_LINE -- ra8_vmem_init rejects 0 frame size */
  }
  st->vm          = vm;
  st->object_id   = object_id;
  st->frame_bytes = fb;
  st->size        = size;
  return k_ra8_ok;
}

size_t ra8_vmem_stream_read(void* ctx, uint64_t offset, void* buf, size_t len)
{
  ra8_vmem_stream_t* st = (ra8_vmem_stream_t*)ctx;
  if (st == nullptr) {
    return 0U;
  }
  if (buf == nullptr) {
    return 0U;
  }
  if (st->frame_bytes == 0U) {
    return 0U;
  }
  if (len == 0U) {
    return 0U;
  }
  if (offset >= st->size) {
    return 0U;
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

    void* page = nullptr;
    if (ra8_vmem_get(st->vm, st->object_id, frame_base, &page) != k_ra8_ok) {
      break; /* GCOVR_EXCL_LINE -- ra8_vmem_get fails only on loader error or all-pinned frames */
    }
    (void)memcpy(out + done, (const uint8_t*)page + in_frame, chunk);
    if (ra8_vmem_put(st->vm, page) != k_ra8_ok) {
      break; /* GCOVR_EXCL_LINE -- put fails only on a foreign page; pin came from the get above */
    }
    done += chunk;
    cur += chunk;
  }
  return done;
}
