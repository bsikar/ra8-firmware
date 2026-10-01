/**
 * @file ra8_imgdec_backend.h
 * @brief Implementer-facing vtable for an `ra8_imgdec` backend (RA8FW-308).
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 3 / Imaging] {World: NS}
 *
 * @details
 * Consumers include `ra8_imgdec.h` and never this file. A backend library
 * (WebP, the first-party JPEG, the first-party PNG, the stb residue) includes
 * this one, defines a `static const ra8_imgdec_iface_t`, and publishes a
 * binder from its own header, exactly as `ra8_io_blockdev_backend.h` is used.
 *
 * @par What the fabric guarantees before `decode` is called
 * Every one of these has already been checked, so a backend must not re-check
 * them and must not treat them as possible:
 * - `req`, `req->bytes`, `req->dst` and `out` are non-NULL;
 * - `req->byte_count` and `req->dst_bytes` are non-zero;
 * - `req->want` is exactly one pixel bit the backend advertised;
 * - `req->format` names exactly one format bit the backend advertised, never
 *   ::k_ra8_imgdec_format_none: a request that left the format unstated was
 *   sniffed by the fabric with ::ra8_imgdec_sniff() and refused before this
 *   call if nothing matched, so the backend is told which container it holds;
 * - `req->dst_stride` is either 0 or at least one pixel wide;
 * - `req->arena` is non-NULL whenever `caps.scratch_bytes` is non-zero, and
 *   that arena has at least `caps.scratch_bytes` remaining.
 *
 * @par What the backend still owns
 * Verifying that the container really is what the signature claimed (the sniff
 * reads a signature, never the rest of the file), refusing an image wider or
 * taller than the `dim_max` it advertised, refusing a `dst` too small for the
 * decoded surface, and filling every field of `*out`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_err.h"
#include "ra8_imgdec.h"

/**
 * @brief Report the backend's static capabilities.
 *
 * @param[in]  ctx Backend-private context from ::ra8_imgdec_t.
 * @param[out] out Capability record to fill completely.
 *
 * @return ra8_err_t ::k_ra8_ok when `*out` was filled.
 *
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_imgdec_caps_fn)(void* ctx, ra8_imgdec_caps_t* out);

/**
 * @brief Decode one image.
 *
 * @param[in]  ctx Backend-private context from ::ra8_imgdec_t.
 * @param[in]  req Validated request (see the guarantees above).
 * @param[out] out Result record to fill completely on success.
 *
 * @return ra8_err_t ::k_ra8_ok when `req->dst` holds the decoded surface.
 *
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_imgdec_decode_fn)(void*                   ctx,
                                          const ra8_imgdec_req_t* req,
                                          ra8_imgdec_image_t*     out);

/**
 * @struct ra8_imgdec_iface
 * @brief The backend vtable. One const instance per backend, never per handle.
 *
 * @invariant Both members are non-NULL; the fabric refuses a partial vtable
 *            with ::k_ra8_err_invalid_state rather than calling through NULL.
 *
 * @since 0.1.0
 */
struct ra8_imgdec_iface {
  ra8_imgdec_caps_fn   get_caps; /**< Static capability query. Required. */
  ra8_imgdec_decode_fn decode;   /**< Decode entry point. Required.      */
};

#ifdef __cplusplus
}
#endif
