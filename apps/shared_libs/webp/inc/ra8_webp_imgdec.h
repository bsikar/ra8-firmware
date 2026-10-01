/**
 * @file ra8_webp_imgdec.h
 * @brief `ra8_imgdec` backend over the vendored libwebp facade (RA8FW-308).
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / WebP] {World: NS}
 *
 * @details
 * RA8FW-308 names four binders, one per decoder the tree already carries, and this
 * is `ra8_imgdec_bind_webp()`: the one format no other backend in this tree
 * opens, and the one whose absence the issue leads with. An app rendering an
 * EPUB's inline `<img>` cannot show a WebP today because the reflow seam is
 * typed to the stb arena; with this binder a consumer holds an
 * ::ra8_imgdec_t that does, and the format matrix stops depending on which
 * decode path the consumer happened to pick.
 *
 * @par Where the scratch comes from
 * ra8_webp_decode_rgba() takes its arena as a parameter, so unlike the stb
 * residue this backend is not forced through a file-static slot. It is still
 * bound once, at ::ra8_webp_imgdec_bind, rather than carved out of the
 * request's ::ra8_arena_t, because libwebp's peak scratch is a function of the
 * image (a few KiB for a thumbnail, a few MiB for a page) and a `scratch_bytes`
 * the backend publishes has to be one fixed number. Publishing a guess and
 * letting the fabric carve to it would refuse large images that a caller-sized
 * arena decodes fine. The caps record therefore reports `scratch_bytes = 0`,
 * which is the literal truth about `req->arena`: this backend draws nothing
 * from it.
 *
 * @par What it advertises
 * WebP into RGBA8888 and nothing else. ra8_webp_decode_rgba() writes one
 * layout, and inventing a grey8 or rgb888 conversion here would be new pixel
 * logic in a binder whose whole job is to not add any.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include "ra8_err.h"
#include "ra8_imgdec.h"
#include "ra8_webp_arena.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bind the libwebp facade as an ::ra8_imgdec_t backend.
 *
 * @details Fills @p out with this module's vtable and records @p scratch as
 * the arena every decode through @p out will bump. @p scratch is not touched
 * here and not required to be empty: ra8_webp_decode_rgba() resets it on entry
 * and drains it before returning, on both the success and the failure path.
 *
 * The arena must be large enough for libwebp's transient scratch alone. It
 * does @e not have to hold the decoded surface, because the facade decodes
 * straight into `req->dst` rather than into an intermediate buffer, which is
 * the one sizing difference from the stb backend.
 *
 * @param[out] out     Handle to fill. Must not be NULL.
 * @param[in]  scratch Caller-owned arena backing every decode. Must not be
 *                     NULL, and `base` must point at `cap` writable bytes.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           @p out is bound and usable.
 * @retval k_ra8_err_null_ptr @p out or @p scratch is NULL.
 *
 * @pre @p scratch outlives @p out; the handle stores the pointer, not a copy.
 * @post On success `out->iface` and `out->ctx` are non-NULL.
 * @post On any non-ok return `*out` is untouched.
 *
 * @note Not thread-safe, and no two bound handles may decode concurrently:
 *       libwebp is single-threaded on this target and its allocator hooks
 *       reach one bound arena slot.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_webp_imgdec_bind(ra8_imgdec_t* out, ra8_webp_arena_t* scratch);

#ifdef __cplusplus
}
#endif
