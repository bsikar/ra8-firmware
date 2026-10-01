/**
 * @file reflow_image.h
 * @brief Zero-heap raster image decode + scale + blit for reflow.
 * @ingroup grp_ereader
 *
 * @details
 * Decodes an in-memory JPEG/PNG/GIF/BMP (the formats EPUB covers + figures
 * use) through `stb_image` and blits it -- nearest-neighbour scaled to fit a
 * layout box -- into the `ra8_gfx` framebuffer. The cover path
 * and the in-chapter `<img>` path (Phase 2) both call ::ra8_img_decode_blit.
 *
 * **Zero heap (NASA P10 Rule 3).** `stb_image` normally `malloc`s; here it is
 * redirected (at build time in `stb_image_impl.c`) to a caller-owned **bump
 * arena** (::ra8_img_arena_t). The caller sizes the scratch for the largest
 * image it decodes (a few KiB in SRAM for thumbnails, a few MiB in SDRAM for a
 * full cover) -- the module allocates nothing static and reaches no `malloc`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_imgdec_scratch.h"

/**
 * @typedef ra8_img_arena_t
 * @brief Caller-owned bump arena backing a single image decode.
 *
 * @details One spelling of ::ra8_imgdec_scratch_t (RA8FW-308): a linear bump
 * allocator over `base[0..cap)` with a live-block count, so `stb_image`'s
 * allocations bump `offset`, each free decrements `live`, and the arena
 * auto-resets to empty when `live` reaches 0. It fully drains after each
 * decode with no caller bookkeeping and no fragmentation.
 *
 * The name is kept because thirty-odd call sites across `examples/`,
 * `apps/shared_libs/` and `tests/` name it, and because "the arena stb_image
 * decodes out of" is still the useful thing to call it here. The arithmetic
 * behind it is no longer written in this module; ::ra8_imgdec_scratch_t owns
 * it, once, for every decoder shim in the tree.
 *
 * @invariant `offset <= cap`.
 */
typedef ra8_imgdec_scratch_t ra8_img_arena_t;

/**
 * @brief Decode @p bytes and blit it, scaled to fit, into the bound framebuffer.
 *
 * @details Decodes the image to RGB through `stb_image` (allocating only from
 * @p arena), computes a fit rectangle inside `box_w x box_h` that preserves the
 * source aspect ratio, then nearest-neighbour blits the scaled image at
 * `(dst_x, dst_y)` via `ra8_gfx`. The arena is fully reset on return (success or
 * failure). The caller must have bound a framebuffer with `ra8_gfx_init()`.
 *
 * @param[in,out] arena  Bump arena (reset on entry); scratch for the decode.
 * @param[in]     bytes  Encoded image bytes (JPEG/PNG/GIF/BMP).
 * @param[in]     len    Length of @p bytes.
 * @param[in]     dst_x  Destination left edge, framebuffer pixels.
 * @param[in]     dst_y  Destination top edge, framebuffer pixels.
 * @param[in]     box_w  Available box width to scale into (>= 1).
 * @param[in]     box_h  Available box height to scale into (>= 1).
 * @param[out]    out_w  Receives the blitted (scaled) width (NULL ok).
 * @param[out]    out_h  Receives the blitted (scaled) height (NULL ok).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                 Image decoded, scaled, and blitted.
 * @retval k_ra8_err_null_ptr       @p arena or @p bytes is NULL.
 * @retval k_ra8_err_invalid_arg    @p len is 0, or @p box_w / @p box_h is < 1.
 * @retval k_ra8_err_not_supported  stb_image could not decode the bytes.
 * @retval k_ra8_err_no_mem         The arena is too small for this image.
 *
 * @pre `ra8_gfx_init()` bound a framebuffer.
 * @pre @p arena->base holds @p arena->cap writable bytes.
 * @post On success the scaled image is drawn; @p arena is reset to empty.
 * @post On any return @p arena->offset == 0 and @p arena->live == 0.
 *
 * @note Not thread-safe (uses a file-static "current arena" for the stb hooks).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_img_decode_blit(ra8_img_arena_t* arena,
                                            const uint8_t*   bytes,
                                            size_t           len,
                                            int32_t          dst_x,
                                            int32_t          dst_y,
                                            int32_t          box_w,
                                            int32_t          box_h,
                                            int32_t*         out_w,
                                            int32_t*         out_h);

/**
 * @brief Probe an image's intrinsic dimensions without a full decode.
 *
 * @details Wraps `stbi_info_from_memory` -- lets the layout pass size an
 * `<img>` box from the source dimensions before deciding to decode + blit.
 * Allocates nothing.
 *
 * @param[in]  bytes Encoded image bytes.
 * @param[in]  len   Length of @p bytes.
 * @param[out] out_w Receives intrinsic width.
 * @param[out] out_h Receives intrinsic height.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                Dimensions read.
 * @retval k_ra8_err_null_ptr      Any pointer argument is NULL.
 * @retval k_ra8_err_not_supported stb_image could not parse the header.
 *
 * @pre @p bytes holds @p len bytes; @p out_w / @p out_h are writable.
 * @post On success `*out_w` / `*out_h` are the source pixel dimensions.
 *
 * @note Pure read of @p bytes; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_img_probe_size(const uint8_t* bytes, size_t len, int32_t* out_w, int32_t* out_h);
