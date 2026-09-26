/**
 * @file ra8_imgdec.c
 * @brief The image-decoder fabric: validate, gate on capabilities, dispatch (#768).
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 3 / Imaging] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_imgdec.h"

#include <stdbool.h>
#include <stdint.h>

#include "ra8_arena.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_imgdec_backend.h"

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
 * @brief Zero a capability record so no caller reads a half-written one.
 */
RA8_INTERNAL static void internal_clear_caps(ra8_imgdec_caps_t* out) {
  const ra8_imgdec_caps_t empty = {};
  *out                          = empty;
}

/**
 * @brief Zero a result record so no caller reads a half-written one.
 */
RA8_INTERNAL static void internal_clear_image(ra8_imgdec_image_t* out) {
  const ra8_imgdec_image_t empty = {};
  *out                           = empty;
}

/**
 * @brief Fetch and sanity-check the bound backend's capability record.
 *
 * @details A backend advertising no format, no pixel layout, a format or pixel
 * bit this header does not define, or a `dim_max` past the fabric limit is a
 * programming error in that backend, reported as ::k_ra8_err_invalid_state
 * rather than quietly honoured.
 */
RA8_INTERNAL static ra8_err_t internal_fetch_caps(const ra8_imgdec_t* dec,
                                                  ra8_imgdec_caps_t*  out) {
  internal_clear_caps(out);

  if (dec->iface == nullptr) {
    return k_ra8_err_not_initialized;
  }
  if (dec->iface->get_caps == nullptr) {
    return k_ra8_err_invalid_state;
  }

  ra8_imgdec_caps_t   caps = {};
  const ra8_err_t     err  = dec->iface->get_caps(dec->ctx, &caps);
  if (err != k_ra8_ok) {
    return err;
  }

  const bool formats_ok = (caps.formats != 0U) &&
                          ((caps.formats & ~(uint32_t)k_ra8_imgdec_format_mask) == 0U);
  const bool pixels_ok = (caps.pixels != 0U) &&
                         ((caps.pixels & ~(uint32_t)k_ra8_imgdec_pixel_mask) == 0U);
  const bool dim_ok = (caps.dim_max != 0U) && (caps.dim_max <= (uint32_t)k_ra8_imgdec_dim_max);
  if (!formats_ok || !pixels_ok || !dim_ok) {
    return k_ra8_err_invalid_state;
  }

  *out = caps;
  return k_ra8_ok;
}

/**
 * @brief Check the request's own shape, before any capability is consulted.
 */
RA8_INTERNAL static ra8_err_t internal_check_req(const ra8_imgdec_req_t* req) {
  if ((req->bytes == nullptr) || (req->dst == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((req->byte_count == 0U) || (req->dst_bytes == 0U)) {
    return k_ra8_err_invalid_size;
  }
  if (!internal_one_defined_bit((uint32_t)req->want, (uint32_t)k_ra8_imgdec_pixel_mask)) {
    return k_ra8_err_invalid_arg;
  }
  if ((req->format != k_ra8_imgdec_format_none) &&
      !internal_one_defined_bit((uint32_t)req->format, (uint32_t)k_ra8_imgdec_format_mask)) {
    return k_ra8_err_invalid_arg;
  }

  const uint32_t bpp = ra8_imgdec_pixel_bytes(req->want);
  if ((req->dst_stride != 0U) && (req->dst_stride < bpp)) {
    return k_ra8_err_invalid_size;
  }
  if (req->dst_bytes < bpp) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Check the request against what the backend actually advertised.
 */
RA8_INTERNAL static ra8_err_t internal_check_against_caps(const ra8_imgdec_req_t*  req,
                                                          const ra8_imgdec_caps_t* caps) {
  if ((((uint32_t)req->want) & caps->pixels) == 0U) {
    return k_ra8_err_not_supported;
  }
  if ((req->format != k_ra8_imgdec_format_none) &&
      ((((uint32_t)req->format) & caps->formats) == 0U)) {
    return k_ra8_err_not_supported;
  }

  if (caps->scratch_bytes == 0U) {
    return k_ra8_ok;
  }
  if (req->arena == nullptr) {
    return k_ra8_err_invalid_state;
  }

  uint32_t        remaining = 0U;
  const ra8_err_t err       = ra8_arena_remaining(req->arena, &remaining);
  if (err != k_ra8_ok) {
    return err;
  }
  if (remaining < caps->scratch_bytes) {
    return k_ra8_err_no_mem;
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

uint32_t ra8_imgdec_pixel_bytes(ra8_imgdec_pixel_t pixel) {
  uint32_t bytes = 0U;
  switch (pixel) {
    case k_ra8_imgdec_pixel_grey8:
      bytes = 1U;
      break;
    case k_ra8_imgdec_pixel_rgb888:
      bytes = 3U;
      break;
    case k_ra8_imgdec_pixel_rgba8888:
      bytes = 4U;
      break;
    case k_ra8_imgdec_pixel_none:
    default:
      bytes = 0U;
      break;
  }
  return bytes;
}

ra8_err_t ra8_imgdec_get_caps(const ra8_imgdec_t* dec, ra8_imgdec_caps_t* out) {
  if ((dec == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  return internal_fetch_caps(dec, out);
}

ra8_err_t ra8_imgdec_supports(const ra8_imgdec_t* dec,
                              ra8_imgdec_format_t format,
                              ra8_imgdec_pixel_t  pixel,
                              bool*               out_ok) {
  if ((dec == nullptr) || (out_ok == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out_ok = false;

  if (!internal_one_defined_bit((uint32_t)format, (uint32_t)k_ra8_imgdec_format_mask) ||
      !internal_one_defined_bit((uint32_t)pixel, (uint32_t)k_ra8_imgdec_pixel_mask)) {
    return k_ra8_err_invalid_arg;
  }

  ra8_imgdec_caps_t caps = {};
  const ra8_err_t   err  = internal_fetch_caps(dec, &caps);
  if (err != k_ra8_ok) {
    return err;
  }

  *out_ok = ((((uint32_t)format) & caps.formats) != 0U) &&
            ((((uint32_t)pixel) & caps.pixels) != 0U);
  return k_ra8_ok;
}

ra8_err_t
ra8_imgdec_decode(const ra8_imgdec_t* dec, const ra8_imgdec_req_t* req, ra8_imgdec_image_t* out) {
  if ((dec == nullptr) || (req == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  internal_clear_image(out);

  ra8_imgdec_caps_t caps     = {};
  const ra8_err_t   caps_err = internal_fetch_caps(dec, &caps);
  if (caps_err != k_ra8_ok) {
    return caps_err;
  }
  if (dec->iface->decode == nullptr) {
    return k_ra8_err_invalid_state;
  }

  const ra8_err_t req_err = internal_check_req(req);
  if (req_err != k_ra8_ok) {
    return req_err;
  }

  const ra8_err_t gate_err = internal_check_against_caps(req, &caps);
  if (gate_err != k_ra8_ok) {
    return gate_err;
  }

  const ra8_err_t err = dec->iface->decode(dec->ctx, req, out);
  if (err != k_ra8_ok) {
    internal_clear_image(out);
  }
  return err;
}
