/**
 * @file ra8_imgdec_mux.c
 * @brief Route a decode across a set of backends, as one format matrix (#768).
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 3 / Imaging] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_imgdec_mux.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_imgdec.h"
#include "ra8_imgdec_scratch.h"

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief True when @p mask has exactly one bit set and that bit is defined.
 */
RA8_INTERNAL static bool internal_one_defined_bit(uint32_t mask, uint32_t defined) {
  const bool single = (mask != 0U) && ((mask & (mask - 1U)) == 0U);
  return single && ((mask & defined) != 0U);
}

/**
 * @brief Reject a mux that is absent or still empty.
 *
 * @details An empty mux is ::k_ra8_err_not_initialized rather than a quiet
 * "nothing supported": a consumer that forgot to add its backends is asking a
 * question the set cannot answer, which is a different thing from a set that
 * genuinely does not open a format.
 */
RA8_INTERNAL static ra8_err_t internal_usable(const ra8_imgdec_mux_t* mux) {
  if (mux == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (mux->count == 0U) {
    return k_ra8_err_not_initialized;
  }
  return k_ra8_ok;
}

/**
 * @brief True when @p caps advertises both @p format and @p pixel.
 */
RA8_INTERNAL static bool internal_caps_cover(const ra8_imgdec_caps_t* caps,
                                             ra8_imgdec_format_t      format,
                                             ra8_imgdec_pixel_t       pixel) {
  const bool has_format = ((((uint32_t)format) & caps->formats) != 0U);
  const bool has_pixel  = ((((uint32_t)pixel) & caps->pixels) != 0U);
  return has_format && has_pixel;
}

/**
 * @brief Walk the members in priority order, stopping at the first that covers
 *        the pair.
 *
 * @details The one place the routing rule lives, so ::ra8_imgdec_mux_supports,
 * ::ra8_imgdec_mux_route and ::ra8_imgdec_mux_decode cannot drift apart about
 * which member serves a request. `*out` stays NULL when no member covers it,
 * which the callers read as "not supported" rather than as an error.
 */
RA8_INTERNAL static ra8_err_t internal_find(const ra8_imgdec_mux_t* mux,
                                            ra8_imgdec_format_t     format,
                                            ra8_imgdec_pixel_t      pixel,
                                            const ra8_imgdec_t**    out) {
  *out = nullptr;

  for (uint32_t i = 0U; i < mux->count; ++i) {
    const ra8_imgdec_t* const member = &mux->members[i];

    ra8_imgdec_caps_t caps = {};
    const ra8_err_t   err  = ra8_imgdec_get_caps(member, &caps);
    if (err != k_ra8_ok) {
      return err;
    }
    if (internal_caps_cover(&caps, format, pixel)) {
      *out = member;
      return k_ra8_ok;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Validate the pair every query takes, then prove the mux is usable.
 */
RA8_INTERNAL static ra8_err_t internal_pair_ok(const ra8_imgdec_mux_t* mux,
                                               ra8_imgdec_format_t     format,
                                               ra8_imgdec_pixel_t      pixel) {
  if (!internal_one_defined_bit((uint32_t)format, (uint32_t)k_ra8_imgdec_format_mask) ||
      !internal_one_defined_bit((uint32_t)pixel, (uint32_t)k_ra8_imgdec_pixel_mask)) {
    return k_ra8_err_invalid_arg;
  }
  return internal_usable(mux);
}

/**
 * @brief Check only what routing itself needs from a request.
 *
 * @details Deliberately not the full request check: ::ra8_imgdec_decode owns
 * that and will run it on the member this picks. Repeating it here would be
 * two copies of one policy, which is the duplication #768 exists to remove.
 * Routing needs the bytes (to sniff), a count (so the sniff has something to
 * read) and a destination layout (half the pair it routes on).
 */
RA8_INTERNAL static ra8_err_t internal_route_req_ok(const ra8_imgdec_req_t* req) {
  if ((req->bytes == nullptr) || (req->dst == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (req->byte_count == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (!internal_one_defined_bit((uint32_t)req->want, (uint32_t)k_ra8_imgdec_pixel_mask)) {
    return k_ra8_err_invalid_arg;
  }
  if ((req->format != k_ra8_imgdec_format_none) &&
      !internal_one_defined_bit((uint32_t)req->format, (uint32_t)k_ra8_imgdec_format_mask)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Settle the container a request is about, sniffing only when unstated.
 */
RA8_INTERNAL static ra8_err_t internal_resolve(const ra8_imgdec_req_t* req,
                                               ra8_imgdec_format_t*    out) {
  *out = k_ra8_imgdec_format_none;

  if (req->format != k_ra8_imgdec_format_none) {
    *out = req->format;
    return k_ra8_ok;
  }

  ra8_imgdec_format_t sniffed = k_ra8_imgdec_format_none;
  const ra8_err_t     err     = ra8_imgdec_sniff(req->bytes, req->byte_count, &sniffed);
  if (err != k_ra8_ok) {
    return k_ra8_err_not_supported;
  }

  *out = sniffed;
  return k_ra8_ok;
}

/**
 * @brief Zero a result record so no caller reads a half-written one.
 */
RA8_INTERNAL static void internal_clear_image(ra8_imgdec_image_t* out) {
  const ra8_imgdec_image_t empty = {};
  *out                           = empty;
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra8_err_t ra8_imgdec_mux_init(ra8_imgdec_mux_t* mux) {
  if (mux == nullptr) {
    return k_ra8_err_null_ptr;
  }
  const ra8_imgdec_mux_t empty = {};
  *mux                         = empty;
  return k_ra8_ok;
}

ra8_err_t ra8_imgdec_mux_add(ra8_imgdec_mux_t* mux, const ra8_imgdec_t* dec) {
  if ((mux == nullptr) || (dec == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (mux->count >= (uint32_t)k_ra8_imgdec_mux_max) {
    return k_ra8_err_no_mem;
  }

  ra8_imgdec_caps_t caps = {};
  const ra8_err_t   err  = ra8_imgdec_get_caps(dec, &caps);
  if (err != k_ra8_ok) {
    return err;
  }

  mux->members[mux->count] = *dec;
  mux->count += 1U;
  return k_ra8_ok;
}

ra8_err_t ra8_imgdec_mux_formats(const ra8_imgdec_mux_t* mux,
                                 ra8_imgdec_pixel_t      pixel,
                                 uint32_t*               out_formats) {
  if (out_formats == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out_formats = (uint32_t)k_ra8_imgdec_format_none;

  if (!internal_one_defined_bit((uint32_t)pixel, (uint32_t)k_ra8_imgdec_pixel_mask)) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t usable = internal_usable(mux);
  if (usable != k_ra8_ok) {
    return usable;
  }

  uint32_t openable = 0U;
  for (uint32_t i = 0U; i < mux->count; ++i) {
    ra8_imgdec_caps_t caps = {};
    const ra8_err_t   err  = ra8_imgdec_get_caps(&mux->members[i], &caps);
    if (err != k_ra8_ok) {
      return err;
    }
    if ((((uint32_t)pixel) & caps.pixels) != 0U) {
      openable |= caps.formats;
    }
  }

  *out_formats = openable;
  return k_ra8_ok;
}

ra8_err_t ra8_imgdec_mux_supports(const ra8_imgdec_mux_t* mux,
                                  ra8_imgdec_format_t     format,
                                  ra8_imgdec_pixel_t      pixel,
                                  bool*                   out_ok) {
  if (out_ok == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out_ok = false;

  const ra8_err_t ready = internal_pair_ok(mux, format, pixel);
  if (ready != k_ra8_ok) {
    return ready;
  }

  const ra8_imgdec_t* member = nullptr;
  const ra8_err_t     err    = internal_find(mux, format, pixel, &member);
  if (err != k_ra8_ok) {
    return err;
  }

  *out_ok = (member != nullptr);
  return k_ra8_ok;
}

ra8_err_t ra8_imgdec_mux_route(const ra8_imgdec_mux_t* mux,
                               ra8_imgdec_format_t     format,
                               ra8_imgdec_pixel_t      pixel,
                               const ra8_imgdec_t**    out) {
  if (out == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out = nullptr;

  const ra8_err_t ready = internal_pair_ok(mux, format, pixel);
  if (ready != k_ra8_ok) {
    return ready;
  }

  const ra8_imgdec_t* member = nullptr;
  const ra8_err_t     err    = internal_find(mux, format, pixel, &member);
  if (err != k_ra8_ok) {
    return err;
  }
  if (member == nullptr) {
    return k_ra8_err_not_supported;
  }

  *out = member;
  return k_ra8_ok;
}

ra8_err_t ra8_imgdec_mux_decode(const ra8_imgdec_mux_t* mux,
                                const ra8_imgdec_req_t* req,
                                ra8_imgdec_image_t*     out) {
  if ((req == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  internal_clear_image(out);

  const ra8_err_t usable = internal_usable(mux);
  if (usable != k_ra8_ok) {
    return usable;
  }
  const ra8_err_t req_err = internal_route_req_ok(req);
  if (req_err != k_ra8_ok) {
    return req_err;
  }

  ra8_imgdec_format_t format     = k_ra8_imgdec_format_none;
  const ra8_err_t     format_err = internal_resolve(req, &format);
  if (format_err != k_ra8_ok) {
    return format_err;
  }

  const ra8_imgdec_t* member = nullptr;
  const ra8_err_t     route  = ra8_imgdec_mux_route(mux, format, req->want, &member);
  if (route != k_ra8_ok) {
    return route;
  }

  ra8_imgdec_req_t resolved = *req;
  resolved.format           = format;
  return ra8_imgdec_decode(member, &resolved, out);
}

/**
 * @brief Widen a running peak budget by one member's published record.
 *
 * @details Bytes take the maximum because only one member decodes at a time,
 * so the peak funds whichever the router picks. Alignment takes the maximum
 * too, and a member reporting 0 contributes nothing: 0 means "no preference",
 * not "byte-aligned", so treating it as a candidate maximum would let a
 * silent member weaken a loud one.
 */
RA8_INTERNAL static void internal_widen(const ra8_imgdec_caps_t* caps,
                                        uint32_t*                bytes,
                                        uint32_t*                align) {
  if (caps->scratch_bytes > *bytes) {
    *bytes = caps->scratch_bytes;
  }
  if (caps->scratch_align > *align) {
    *align = caps->scratch_align;
  }
}

ra8_err_t ra8_imgdec_mux_scratch_budget(const ra8_imgdec_mux_t* mux,
                                        uint32_t*               out_bytes,
                                        uint32_t*               out_align) {
  if ((out_bytes == nullptr) || (out_align == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out_bytes = 0U;
  *out_align = 0U;

  const ra8_err_t usable = internal_usable(mux);
  if (usable != k_ra8_ok) {
    return usable;
  }

  uint32_t bytes = 0U;
  uint32_t align = 0U;
  for (uint32_t i = 0U; i < mux->count; ++i) {
    ra8_imgdec_caps_t caps = {};
    const ra8_err_t   err  = ra8_imgdec_get_caps(&mux->members[i], &caps);
    if (err != k_ra8_ok) {
      *out_bytes = 0U;
      *out_align = 0U;
      return err;
    }
    internal_widen(&caps, &bytes, &align);
  }

  *out_bytes = bytes;
  *out_align = (align != 0U) ? align : (uint32_t)k_ra8_imgdec_scratch_align;
  return k_ra8_ok;
}

ra8_err_t ra8_imgdec_mux_carve(const ra8_imgdec_mux_t* mux,
                               ra8_arena_t*            arena,
                               ra8_imgdec_scratch_t*   out) {
  if ((arena == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out = (ra8_imgdec_scratch_t){};

  uint32_t        bytes  = 0U;
  uint32_t        align  = 0U;
  const ra8_err_t budget = ra8_imgdec_mux_scratch_budget(mux, &bytes, &align);
  if (budget != k_ra8_ok) {
    return budget;
  }
  if (bytes == 0U) {
    return k_ra8_ok; /* nothing in the set decodes with scratch */
  }
  return ra8_imgdec_scratch_carve(out, arena, bytes, align);
}
