/**
 * @file ra8_jpeg_imgdec.h
 * @brief Bind the first-party software JPEG codec as an `ra8_imgdec` backend
 *        (RA8FW-308).
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * The `ra8_imgdec` seam has carried a fabric, a container sniff, a geometry
 * probe, a scratch contract and a mux since RA8FW-308 opened, and no decoder at
 * all: every consumer still calls its own decoder directly, which is the
 * duplication the issue is about. This is the first real backend, and it binds
 * the one decoder the tree already owns outright.
 *
 * @par Why the binder lives here and not in libs/ra8_imgdec
 * `ra8_imgdec_backend.h` is [Ring 3 / Imaging] and this codec is
 * [Ring 4 / Domain]. A binder inside `libs/ra8_imgdec` would make the fabric
 * depend on a decoder, which is the ring inversion the seam exists to remove;
 * a binder here is a ring-4 module reaching down to a ring-3 interface, which
 * is the direction the tree allows. It is also the shape
 * `ra8_io_blockdev_backend.h` is already used in: the backend library defines
 * the vtable and publishes its own binder.
 *
 * @par What this backend opens
 * Baseline JPEG only (::k_ra8_imgdec_format_jpeg), into packed RGB888 only
 * (::k_ra8_imgdec_pixel_rgb888), because that is the whole of what
 * ::ra8_jpeg_sw_decode produces. It carves no scratch: the codec keeps its
 * working state in module statics, so there is no arena budget to publish.
 * That also means the handle is stateless and the codec's concurrency
 * contract is unchanged by binding it -- see the Concurrency section of
 * `ra8_jpeg_sw.h`. Two handles are not two decoders.
 *
 * @code
 * ra8_imgdec_t dec = {};
 * (void)ra8_jpeg_imgdec_bind(&dec);
 *
 * const ra8_imgdec_req_t req = {
 *   .bytes      = jpeg,
 *   .byte_count = jpeg_len,
 *   .dst        = surface,
 *   .dst_bytes  = sizeof surface,
 *   .want       = k_ra8_imgdec_pixel_rgb888,
 * };
 * ra8_imgdec_image_t img = {};
 * (void)ra8_imgdec_decode(&dec, &req, &img);
 * @endcode
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
 * @brief Bind the software JPEG codec into an ::ra8_imgdec_t handle.
 *
 * @details Writes the module's vtable and a NULL private context into @p out.
 * The context is NULL deliberately: the codec's state is module-static, so a
 * per-handle context would claim an independence the decoder does not have.
 *
 * @param[out] out Handle to bind. Overwritten whole on success.
 *
 * @return ra8_err_t ::k_ra8_ok when @p out is bound.
 * @retval k_ra8_err_null_ptr @p out was NULL; nothing was written.
 *
 * @pre @p out is writable.
 * @post On success @p out is usable with ::ra8_imgdec_decode.
 * @post On failure no state is mutated.
 *
 * @note Thread-safe: writes only @p out. The bound decoder is not -- see the
 *       Concurrency section of `ra8_jpeg_sw.h`.
 *
 * @see ra8_imgdec_decode(), ra8_jpeg_sw_decode()
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_jpeg_imgdec_bind(ra8_imgdec_t* out);

#ifdef __cplusplus
}
#endif
