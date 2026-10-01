/**
 * @file ra8_img_imgdec.h
 * @brief `ra8_imgdec` backend over the vendored stb_image residue (RA8FW-308).
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Reflow] {World: NS}
 *
 * @details
 * RA8FW-308 names four binders, one per decoder the tree already carries, and this
 * is the one over the vendored stb_image residue: the decoder that reaches a
 * reader today only because `reflow_image.c` calls `stbi_load_from_memory()`
 * directly.
 *
 * @par What this backend advertises, and why PNG is on the list
 * `stb_image_impl.c` compiles with `STBI_ONLY_JPEG`, `STBI_ONLY_PNG`,
 * `STBI_ONLY_GIF` and `STBI_ONLY_BMP`. There is no `STBI_ONLY_TGA`, so **TGA
 * is not in the firmware image** and this binder must not claim it, whatever
 * RA8FW-308's prose says. The advertised matrix is therefore PNG, GIF and BMP.
 *
 * PNG is on that list for a reason worth stating, because RA8FW-308 proposes a
 * separate `ra8_imgdec_bind_png()` promoting `jof_png.c` to a public
 * `libs/ra8_png`. That promotion has not happened and is not a small job:
 * `priv_jof_png_rows()` is a pull-based streaming decoder declared `RA8_PRIV`
 * in `jof_internal.h`, with no public header and no whole-frame entry. Until
 * it exists, **stb is the only bindable PNG decoder in the tree**, and the
 * consumers this seam is built to replace -- the reflow inline-image path and
 * the RABOOK compiler -- decode PNG through exactly this `stbi` call today. A
 * binder that refused PNG could not stand in for any of them, which would
 * leave the seam unable to do the one job it exists for.
 *
 * JPEG stays off the list. The first-party codec is already bound
 * (`ra8_jpeg_imgdec_bind()`), it is the better decoder, and nothing is blocked
 * by leaving JPEG to it.
 *
 * Overlap, when `libs/ra8_png` does arrive, is not a conflict the fabric has
 * to be protected from: ::ra8_imgdec_mux resolves a format in member order,
 * so a mux listing a first-party PNG backend ahead of this one routes PNG
 * there and never reaches this arm. At that point the `k_ra8_imgdec_format_png`
 * bit below can be dropped, and this paragraph with it.
 *
 * @par Where the scratch comes from
 * `STBI_MALLOC` and friends are macros with no context parameter, so the
 * fabric's per-request `ra8_arena_t` cannot reach them. The arena is supplied
 * once, to ::ra8_img_imgdec_bind, and the decode hook binds it around the stb
 * call through the existing file-static slot in `ra8_img_arena.c`. The caps
 * record therefore reports `scratch_bytes = 0`, which is the literal truth
 * about `req->arena`: this backend draws nothing from it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include "ra8_err.h"
#include "ra8_imgdec.h"
#include "reflow_image.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bind the stb_image residue as an ::ra8_imgdec_t backend.
 *
 * @details Fills @p out with this module's vtable and records @p scratch as
 * the arena every decode through @p out will bump. @p scratch is not touched
 * here and not required to be empty: each decode resets it on entry and drains
 * it on return, exactly as ra8_img_decode_blit() does.
 *
 * The arena must be large enough for the whole decoded surface *plus*
 * stb_image's own transient scratch, because stb allocates the output buffer
 * from it and this backend then copies that buffer into `req->dst`. Sizing is
 * the caller's, unchanged from the direct stb path.
 *
 * @param[out] out     Handle to fill. Must not be NULL.
 * @param[in]  scratch Caller-owned arena backing every decode. Must not be
 *                     NULL, and `base` must point at `cap` writable bytes.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok            @p out is bound and usable.
 * @retval k_ra8_err_null_ptr  @p out or @p scratch is NULL.
 *
 * @pre @p scratch outlives @p out; the handle stores the pointer, not a copy.
 * @post On success `out->iface` and `out->ctx` are non-NULL.
 * @post On any non-ok return `*out` is untouched.
 *
 * @note Not thread-safe, and no two bound handles may decode concurrently:
 *       the stb hooks reach their arena through one file-static slot.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_img_imgdec_bind(ra8_imgdec_t* out, ra8_img_arena_t* scratch);

#ifdef __cplusplus
}
#endif
