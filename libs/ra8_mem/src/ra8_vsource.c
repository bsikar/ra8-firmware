/**
 * @file ra8_vsource.c
 * @brief Virtual-memory object sources -- implementation (Layer 1, #147).
 *
 * @par Tag
 * [Ring 2 / Core] {World: NS}
 *
 * @details
 * A flat object array indexed by `object_id` (the assignment order). The loader
 * adapter zeroes the whole frame first, then fills the in-range prefix from the
 * object's backing, so a short tail at the object's end reads back as zeros.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_vsource.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_check.h"
#include "ra8_err.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_vsource";

ra8_err_t ra8_vsource_init(ra8_vsource_t* vs, ra8_vsource_obj_t* objs, uint32_t cap)
{
  RA8_CHECK_NULL_PTR(vs, s_tag, "vs must not be nullptr");
  RA8_CHECK_NULL_PTR(objs, s_tag, "objs must not be nullptr");
  if (cap == 0U) {
    return k_ra8_err_invalid_size;
  }
  vs->objs  = objs;
  vs->cap   = cap;
  vs->count = 0U;
  return k_ra8_ok;
}

ra8_err_t ra8_vsource_add_paged(ra8_vsource_t*      vs,
                                ra8_vsource_read_fn read,
                                void*               ctx,
                                uint64_t            base,
                                uint64_t            size,
                                uint32_t*           out_id)
{
  RA8_CHECK_NULL_PTR(vs, s_tag, "vs must not be nullptr");
  RA8_CHECK_NULL_PTR(read, s_tag, "read must not be nullptr");
  RA8_CHECK_NULL_PTR(out_id, s_tag, "out_id must not be nullptr");
  if (size == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (vs->count >= vs->cap) {
    return k_ra8_err_no_mem;
  }
  vs->objs[vs->count] =
    (ra8_vsource_obj_t){.read = read, .ctx = ctx, .xip = nullptr, .base = base, .size = size};
  *out_id = vs->count;
  vs->count++;
  return k_ra8_ok;
}

ra8_err_t
ra8_vsource_add_xip(ra8_vsource_t* vs, const uint8_t* xip_base, uint64_t size, uint32_t* out_id)
{
  RA8_CHECK_NULL_PTR(vs, s_tag, "vs must not be nullptr");
  RA8_CHECK_NULL_PTR(xip_base, s_tag, "xip_base must not be nullptr");
  RA8_CHECK_NULL_PTR(out_id, s_tag, "out_id must not be nullptr");
  if (size == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (vs->count >= vs->cap) {
    return k_ra8_err_no_mem;
  }
  vs->objs[vs->count] =
    (ra8_vsource_obj_t){.read = nullptr, .ctx = nullptr, .xip = xip_base, .base = 0U, .size = size};
  *out_id = vs->count;
  vs->count++;
  return k_ra8_ok;
}

ra8_err_t ra8_vsource_loader(void*    ctx,
                             uint32_t object_id,
                             uint64_t offset,
                             uint8_t* frame,
                             uint32_t frame_bytes)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(frame, s_tag, "frame must not be nullptr");
  const ra8_vsource_t* vs = (const ra8_vsource_t*)ctx;
  if (object_id >= vs->count) {
    return k_ra8_err_out_of_range;
  }
  const ra8_vsource_obj_t* o = &vs->objs[object_id];
  if (offset >= o->size) {
    return k_ra8_err_out_of_range;
  }
  const uint64_t avail   = o->size - offset;
  const uint32_t to_fill = (avail < (uint64_t)frame_bytes) ? (uint32_t)avail : frame_bytes;
  (void)memset(frame, 0, (size_t)frame_bytes);
  if (o->xip != nullptr) {
    (void)memcpy(frame, &o->xip[offset], (size_t)to_fill);
    return k_ra8_ok;
  }
  return o->read(o->ctx, o->base + offset, frame, to_fill);
}

ra8_err_t ra8_vsource_xip_ptr(const ra8_vsource_t* vs,
                              uint32_t             object_id,
                              uint64_t             offset,
                              uint32_t             len,
                              const uint8_t**      out_ptr)
{
  RA8_CHECK_NULL_PTR(vs, s_tag, "vs must not be nullptr");
  RA8_CHECK_NULL_PTR(out_ptr, s_tag, "out_ptr must not be nullptr");
  if (object_id >= vs->count) {
    return k_ra8_err_out_of_range;
  }
  const ra8_vsource_obj_t* o = &vs->objs[object_id];
  if (o->xip == nullptr) {
    return k_ra8_err_not_supported;
  }
  if (offset >= o->size) {
    return k_ra8_err_out_of_range;
  }
  if ((uint64_t)len > (o->size - offset)) {
    return k_ra8_err_out_of_range;
  }
  *out_ptr = &o->xip[offset];
  return k_ra8_ok;
}
