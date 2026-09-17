/**
 * @file reflow_image.c
 * @brief Zero-heap raster image decode + nearest-neighbour scale + blit (#106).
 *
 * @details
 * Implements reflow_image.h. The decode runs through the vendored stb_image
 * (built once in apps/shared_libs/third_party/stb/stb_image_impl.c) with its allocator
 * redirected to a caller-bound bump arena, so no `malloc` is reached. The blit
 * is an integer nearest-neighbour scale-to-fit into a layout box, emitting one
 * `ra8_gfx_pixel()` per destination pixel (which clips to the framebuffer).
 *
 * @par WebP inline arm (#637):
 * `stb_image` cannot decode WebP, so an EPUB whose inline illustrations are
 * WebP rendered nothing on this path while the same bytes decoded fine as a
 * comic tile through `jof_produce` -> ra8_webp. When `RA8_REFLOW_WEBP` is
 * defined the probe and the decode dispatch a RIFF/WEBP buffer to the
 * ra8_webp facade instead, carving the whole-frame RGBA8888 buffer and the
 * decoder's scratch out of the caller's same ::ra8_img_arena_t backing store,
 * so the WebP arm is as heap-free as the stb one. The macro is defined by the
 * build for an app that already carries the vendored decoder (`LIBS` names
 * webp, jof or rabook_compile) and by the host unit-test build; without it the
 * arm compiles out entirely and a WebP buffer is rejected by stb exactly as
 * before, so no app pays libwebp's footprint for a format it never sees.
 *
 *
 * [Ring 4 / Reflow] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "reflow_image.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_img_arena.h"
#include "ra8_log.h"
#include "reflow_svg.h"
#include "stb_image.h"

#if defined(RA8_REFLOW_WEBP)
#include "ra8_webp.h"
#endif

/** @brief Log tag for the image decode/blit module. */
static const char* const s_tag_img = "ra8_img";

/**
 * @enum ra8_img_pack_t
 * @brief Pixel-packing and channel constants (no magic numbers).
 */
typedef enum : uint8_t {
  k_ra8_img_req_rgb  = 3, /**< Desired channel count requested from stb_image. */
  k_ra8_img_ch_r     = 0, /**< Red byte offset within an RGB triple.           */
  k_ra8_img_ch_g     = 1, /**< Green byte offset within an RGB triple.         */
  k_ra8_img_ch_b     = 2, /**< Blue byte offset within an RGB triple.          */
  k_ra8_img_min_edge = 1, /**< Minimum scaled / box edge length, pixels.       */
  k_ra8_img_rgba_bpp = 4, /**< Source bytes per pixel for a WebP RGBA8888 frame. */
} ra8_img_pack_t;

/**
 * @enum ra8_img_shift_t
 * @brief Channel shifts to assemble a 0x00RRGGBB colour for ra8_gfx (no magics).
 */
typedef enum : uint8_t {
  k_ra8_img_shift_r = 16, /**< Red channel shift into 0x00RRGGBB.   */
  k_ra8_img_shift_g = 8,  /**< Green channel shift into 0x00RRGGBB. */
} ra8_img_shift_t;

#if defined(RA8_REFLOW_WEBP)
/**
 * @enum ra8_img_webp_sig_t
 * @brief Byte offsets and lengths of the RIFF/WEBP container signature.
 *
 * @details A WebP file starts with the 12-byte header `"RIFF" <u32 size>
 * "WEBP"`. Both FourCCs must match: `"RIFF"` alone is also WAV, AVI and a
 * dozen other containers, so the arm sniffs the pair.
 */
typedef enum : uint8_t {
  k_ra8_img_riff_off    = 0,  /**< Offset of the "RIFF" FourCC.              */
  k_ra8_img_webp_off    = 8,  /**< Offset of the "WEBP" FourCC.              */
  k_ra8_img_fourcc_len  = 4,  /**< Length of a FourCC tag, bytes.            */
  k_ra8_img_webp_sig_n  = 12, /**< Bytes needed before the sniff is decidable. */
  k_ra8_img_webp_align  = 16, /**< Arena alignment for the decoded frame.    */
} ra8_img_webp_sig_t;

/**
 * @brief Test whether a buffer is a RIFF/WEBP container.
 *
 * @details Compares the two FourCC tags of the 12-byte RIFF header. The
 * intervening 4-byte chunk size is not checked: a truncated or corrupt
 * payload is rejected later by ra8_webp_get_info(), which parses the real
 * VP8/VP8L/VP8X chunk. Internal helper for the WebP arm of
 * ra8_img_probe_size() and ra8_img_decode_blit().
 *
 * @param[in] bytes Candidate buffer; must not be NULL.
 * @param[in] len   Length of @p bytes, bytes.
 * @retval true  @p len is at least ::k_ra8_img_webp_sig_n and both FourCCs match.
 * @retval false Too short, or either FourCC differs.
 *
 * @pre @p bytes points at @p len readable bytes.
 * @post @p bytes is not modified.
 *
 * @note MC/DC for `(len >= sig_n) && riff && webp`: a WebP buffer takes all
 *       three true; a 2-byte buffer flips the length term; a PNG buffer flips
 *       the RIFF term; a RIFF/WAVE buffer flips the WEBP term. All four
 *       vectors are exercised by internal_test_webp_probe_and_blit().
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_is_webp(const uint8_t* bytes, size_t len)
{
  return (len >= (size_t)k_ra8_img_webp_sig_n) &&
         (memcmp(&bytes[k_ra8_img_riff_off], "RIFF", (size_t)k_ra8_img_fourcc_len) == 0) &&
         (memcmp(&bytes[k_ra8_img_webp_off], "WEBP", (size_t)k_ra8_img_fourcc_len) == 0);
}
#endif /* RA8_REFLOW_WEBP */

ra8_err_t ra8_img_probe_size(const uint8_t* bytes, size_t len, int32_t* out_w, int32_t* out_h)
{
  RA8_CHECK_NULL_PTR(bytes, s_tag_img, "probe: null bytes");
  RA8_CHECK_NULL_PTR(out_w, s_tag_img, "probe: null out_w");
  RA8_CHECK_NULL_PTR(out_h, s_tag_img, "probe: null out_h");

  /* SVG is not a raster: take its intrinsic size from the document (#112). */
  if (ra8_svg_is_svg(bytes, len)) {
    return ra8_svg_size(bytes, len, out_w, out_h);
  }

#if defined(RA8_REFLOW_WEBP)
  /* stb_image has no WebP decoder: hand the header to ra8_webp (#637). */
  if (internal_is_webp(bytes, len)) {
    uint32_t        webp_w = 0U;
    uint32_t        webp_h = 0U;
    const ra8_err_t info_err = ra8_webp_get_info(bytes, len, &webp_w, &webp_h);
    if (info_err != k_ra8_ok) {
      ra8_log_error(s_tag_img, "probe: ra8_webp rejected the header");
      return info_err;
    }
    *out_w = (int32_t)webp_w;
    *out_h = (int32_t)webp_h;
    return k_ra8_ok;
  }
#endif /* RA8_REFLOW_WEBP */

  int x    = 0;
  int y    = 0;
  int comp = 0;
  if (stbi_info_from_memory(bytes, (int)len, &x, &y, &comp) == 0) {
    ra8_log_error(s_tag_img, "probe: stbi_info rejected the header");
    return k_ra8_err_not_supported;
  }
  *out_w = (int32_t)x;
  *out_h = (int32_t)y;
  return k_ra8_ok;
}

/**
 * @brief Compute the aspect-preserving fit rectangle for an image in a box.
 *
 * @details Picks the largest integer scale that maps the source into the box
 * while preserving aspect ratio. The tighter axis is determined by comparing
 * `box_w * src_h` with `box_h * src_w` using int64 products to prevent
 * overflow on large dimensions. The width-constrained branch scales height
 * proportionally to width; the height-constrained branch scales width
 * proportionally to height. Both output dimensions are clamped to at least
 * `k_ra8_img_min_edge` (1 pixel) so downstream callers never receive a
 * zero-size rectangle. Internal helper for ra8_img_decode_blit().
 *
 * @param[in]  src_w Source width, pixels (>= 1).
 * @param[in]  src_h Source height, pixels (>= 1).
 * @param[in]  box_w Box width, pixels (>= 1).
 * @param[in]  box_h Box height, pixels (>= 1).
 * @param[out] fit_w Receives the scaled width, pixels (>= 1).
 * @param[out] fit_h Receives the scaled height, pixels (>= 1).
 * @return Nothing.
 *
 * @pre All four dimension arguments are greater than or equal to 1.
 * @pre @p fit_w and @p fit_h are valid, writable, non-NULL pointers.
 * @post `*fit_w` and `*fit_h` are each >= 1 (clamped to k_ra8_img_min_edge).
 * @post The aspect ratio of the output is as close as integer division allows
 *       to the aspect ratio of the source.
 *
 * @note Not thread-safe; intended to be called only from ra8_img_decode_blit().
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_fit_box(int32_t  src_w,
                             int32_t  src_h,
                             int32_t  box_w,
                             int32_t  box_h,
                             int32_t* fit_w,
                             int32_t* fit_h)
{
  int32_t sw = 0;
  int32_t sh = 0;
  if (((int64_t)box_w * (int64_t)src_h) <= ((int64_t)box_h * (int64_t)src_w)) {
    /* Width is the tighter constraint: fill the box width. */
    sw = box_w;
    sh = (int32_t)(((int64_t)src_h * (int64_t)box_w) / (int64_t)src_w);
  } else {
    /* Height is the tighter constraint: fill the box height. */
    sh = box_h;
    sw = (int32_t)(((int64_t)src_w * (int64_t)box_h) / (int64_t)src_h);
  }
  *fit_w = (sw < k_ra8_img_min_edge) ? (int32_t)k_ra8_img_min_edge : sw;
  *fit_h = (sh < k_ra8_img_min_edge) ? (int32_t)k_ra8_img_min_edge : sh;
}

/**
 * @brief Map a decode failure to the closest ra8_err_t via stbi_failure_reason.
 *
 * @details Queries `stbi_failure_reason()` immediately after a failed
 * `stbi_load_from_memory()` call and inspects the returned string for the
 * substring "outofmem". When that tag is present the arena exhausted its
 * capacity before the decode completed; the function returns
 * ::k_ra8_err_no_mem so the caller can report a memory shortage rather than
 * a format error. Any other reason string (corrupt header, unsupported
 * colour depth, unsupported format, etc.) maps to ::k_ra8_err_not_supported.
 * A NULL reason string is treated the same way as an unrecognised string.
 * Internal helper for ra8_img_decode_blit().
 *
 * @return ra8_err_t Classification of the most-recent stb_image failure.
 * @retval k_ra8_err_no_mem         The "outofmem" tag was found in the reason.
 * @retval k_ra8_err_not_supported  Any other failure (corrupt or unsupported).
 *
 * @pre `stbi_load_from_memory()` has just returned NULL (sets the reason).
 * @pre The stb_image thread-local reason pointer is valid for this thread.
 * @post No stb_image state is modified; the reason string is only read.
 * @post The returned code is one of the two documented retval constants.
 *
 * @note Not thread-safe; stb_image stores the reason in a module-static
 *       variable. Caller must ensure single-threaded access.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_decode_fail(void)
{
  const char* const reason = stbi_failure_reason();
  /*
   * internal_decode_fail() is called only from ra8_img_decode_blit() after
   * stbi_load_from_memory() has already reported a decode failure. stb_image
   * sets its global failure string on every failure path, so stbi_failure_reason()
   * is non-null on every call that reaches here; the (reason == nullptr) arm is
   * unreachable defensive code and cannot give the first condition independent
   * influence.
   */
  /* mcdc-deactivated: stbi sets a reason on every failure, so (reason != nullptr) is always true here. */
  if ((reason != nullptr) && (strstr(reason, "outofmem") != nullptr)) {
    return k_ra8_err_no_mem;
  }
  return k_ra8_err_not_supported;
}

/**
 * @brief Nearest-neighbour blit a decoded RGB image into the bound framebuffer.
 *
 * @details Iterates over every pixel in the `fit_w x fit_h` destination
 * rectangle. For each destination pixel `(dx, dy)` the corresponding source
 * row and column are computed with integer division scaled by int64 products
 * to avoid overflow: `map_y = (dy * src_h) / fit_h` and
 * `map_x = (dx * src_w) / fit_w`. The RGB triple at that position in the
 * row-major `pixels` buffer is then packed into a 0x00RRGGBB word and
 * emitted via `ra8_gfx_pixel(dst_x + dx, dst_y + dy, color)`, which clips
 * coordinates that fall outside the bound framebuffer. The function reads
 * every pixel in the destination rectangle once; no sub-pixel filtering is
 * applied. Internal helper for ra8_img_decode_blit().
 *
 * @param[in] pixels   Decoded source buffer, row-major pixels of
 *                     @p src_bpp bytes each, size `src_w * src_h * src_bpp`
 *                     bytes; must not be NULL.
 * @param[in] src_w    Source width, pixels (>= 1).
 * @param[in] src_h    Source height, pixels (>= 1).
 * @param[in] fit_w    Destination width, pixels (>= 1).
 * @param[in] fit_h    Destination height, pixels (>= 1).
 * @param[in] dst_x    Destination left edge in framebuffer coordinates.
 * @param[in] dst_y    Destination top edge in framebuffer coordinates.
 * @param[in] src_bpp  Source bytes per pixel: ::k_ra8_img_req_rgb for an
 *                     stb_image RGB decode, ::k_ra8_img_rgba_bpp for a
 *                     ra8_webp RGBA8888 frame. Only the leading R, G and B
 *                     bytes are read, so a trailing alpha byte is skipped
 *                     rather than composited (the reflow framebuffer is
 *                     opaque RGB).
 * @return Nothing.
 *
 * @pre @p pixels is a valid pointer to `src_w * src_h * src_bpp` readable bytes.
 * @pre All dimension arguments (@p src_w, @p src_h, @p fit_w, @p fit_h)
 *      are >= 1 so neither loop bound is zero and no division by zero occurs.
 * @pre @p src_bpp is >= ::k_ra8_img_req_rgb.
 * @post Exactly `fit_w * fit_h` calls to `ra8_gfx_pixel()` have been made.
 * @post The @p pixels buffer is not modified (read-only traversal).
 *
 * @note Not thread-safe; both `ra8_gfx_pixel()` and the stb arena backing
 *       @p pixels use module-static state. Caller must ensure exclusive access.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_blit_scaled(const uint8_t* pixels,
                                 int32_t        src_w,
                                 int32_t        src_h,
                                 int32_t        fit_w,
                                 int32_t        fit_h,
                                 int32_t        dst_x,
                                 int32_t        dst_y,
                                 uint8_t        src_bpp)
{
  for (int32_t dy = 0; dy < fit_h; dy++) {
    const int32_t map_y = (int32_t)(((int64_t)dy * (int64_t)src_h) / (int64_t)fit_h);
    for (int32_t dx = 0; dx < fit_w; dx++) {
      const int32_t map_x = (int32_t)(((int64_t)dx * (int64_t)src_w) / (int64_t)fit_w);
      const size_t  idx =
        (((size_t)map_y * (size_t)src_w) + (size_t)map_x) * (size_t)src_bpp;
      const uint32_t color = ((uint32_t)pixels[idx + (size_t)k_ra8_img_ch_r] << k_ra8_img_shift_r) |
                             ((uint32_t)pixels[idx + (size_t)k_ra8_img_ch_g] << k_ra8_img_shift_g) |
                             (uint32_t)pixels[idx + (size_t)k_ra8_img_ch_b];
      (void)ra8_gfx_pixel(dst_x + dx, dst_y + dy, color);
    }
  }
}

/**
 * @brief Unbind the decode arena and force it back to the fully-drained state.
 *
 * @details Calls `ra8_img_arena_unbind()` to clear the module-static pointer
 * that redirects stb_image allocations, then resets both bookkeeping fields
 * of the arena struct to zero: `offset` (the bump pointer) and `live` (the
 * outstanding allocation count). This guarantees the arena is ready for
 * reuse and that no stale stb_image allocation callbacks can reach it after
 * the call. The `base` and `cap` fields, which are owned by the caller, are
 * left untouched. Called on every return path of ra8_img_decode_blit() --
 * both on success after `stbi_image_free()` and on failure before returning
 * an error code. Internal helper for ra8_img_decode_blit().
 *
 * @param[in,out] arena Bump arena to unbind and reset; must not be NULL.
 * @return Nothing.
 *
 * @pre @p arena is a valid non-NULL pointer to a bound or partially-used
 *      `ra8_img_arena_t` that was previously passed to `ra8_img_arena_bind()`.
 * @pre The stb_image allocator is currently redirected to @p arena (i.e.,
 *      `ra8_img_arena_bind()` has been called and not yet paired with unbind).
 * @post @p arena->offset == 0 and @p arena->live == 0.
 * @post The module-static current-arena pointer is NULL; no further stb
 *       allocation callbacks can reach @p arena.
 *
 * @note Not thread-safe; uses the same module-static arena pointer as the
 *       stb_image hook. Caller must ensure single-threaded access.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_arena_release(ra8_img_arena_t* arena)
{
  ra8_img_arena_unbind();
  arena->offset = 0U;
  arena->live   = 0U;
}

#if defined(RA8_REFLOW_WEBP)
/**
 * @brief Decode a RIFF/WEBP buffer out of the caller's arena and blit it.
 *
 * @details Splits the caller's ::ra8_img_arena_t backing store into two
 * slices, in the same shape `priv_jof_webp_transcode()` uses: the leading
 * `w * h * ::k_ra8_img_rgba_bpp` bytes hold the decoded RGBA8888 frame, and
 * whatever remains (::k_ra8_img_webp_align aligned) becomes the
 * ::ra8_webp_arena_t scratch the decoder allocates from. Nothing is taken
 * from the heap and the stb allocator hook is never bound on this path, so
 * the arena's own `offset` / `live` bookkeeping stays at zero throughout.
 * The decoded frame is then fitted with internal_fit_box() and drawn by
 * internal_blit_scaled() at ::k_ra8_img_rgba_bpp, which reads the R, G and B
 * bytes and skips the alpha. Internal helper for ra8_img_decode_blit().
 *
 * @param[in,out] arena Caller-owned bump arena, used purely as a byte slab.
 * @param[in]     bytes WebP container; must not be NULL.
 * @param[in]     len   Length of @p bytes, bytes.
 * @param[in]     dst_x Destination left edge in framebuffer coordinates.
 * @param[in]     dst_y Destination top edge in framebuffer coordinates.
 * @param[in]     box_w Layout box width, pixels (>= 1).
 * @param[in]     box_h Layout box height, pixels (>= 1).
 * @param[out]    out_w Drawn width, pixels; ignored when NULL.
 * @param[out]    out_h Drawn height, pixels; ignored when NULL.
 * @retval k_ra8_ok                     Frame decoded and blitted.
 * @retval k_ra8_err_no_mem             `arena->cap` cannot hold the frame plus
 *                                      a non-empty scratch slice.
 * @retval k_ra8_err_not_supported      Dimensions exceed ra8_webp's limits.
 * @retval k_ra8_err_validation_failed  Corrupt or truncated bitstream, or the
 *                                      scratch slice was exhausted mid-decode.
 *
 * @pre internal_is_webp(@p bytes, @p len) is true.
 * @pre `arena->base` points at `arena->cap` writable bytes.
 * @post `arena->offset == 0` and `arena->live == 0` on every return path.
 *
 * @note MC/DC for the capacity decision `(frame_n > cap) || (scratch_n == 0)`:
 *       a 1 KiB arena against an 8x8 frame takes both false; a frame larger
 *       than the arena flips the first; an arena sized to exactly the frame
 *       flips the second. Vectors live in internal_test_webp_probe_and_blit().
 * @note Not thread-safe; internal_blit_scaled() writes through the
 *       module-static ra8_gfx target.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_webp_decode_blit(ra8_img_arena_t* arena,
                                           const uint8_t*   bytes,
                                           size_t           len,
                                           int32_t          dst_x,
                                           int32_t          dst_y,
                                           int32_t          box_w,
                                           int32_t          box_h,
                                           int32_t*         out_w,
                                           int32_t*         out_h)
{
  uint32_t        webp_w   = 0U;
  uint32_t        webp_h   = 0U;
  const ra8_err_t info_err = ra8_webp_get_info(bytes, len, &webp_w, &webp_h);
  if (info_err != k_ra8_ok) {
    ra8_log_error(s_tag_img, "blit: ra8_webp rejected the header");
    return info_err;
  }

  const size_t frame_n =
    (size_t)webp_w * (size_t)webp_h * (size_t)k_ra8_img_rgba_bpp;
  const size_t frame_pad =
    (frame_n + (size_t)k_ra8_img_webp_align - 1U) & ~((size_t)k_ra8_img_webp_align - 1U);
  const size_t scratch_n = (frame_pad < arena->cap) ? (arena->cap - frame_pad) : 0U;
  if ((frame_pad > arena->cap) || (scratch_n == 0U)) {
    ra8_log_error(s_tag_img, "blit: arena too small for the webp frame");
    return k_ra8_err_no_mem;
  }

  uint8_t* const   frame = arena->base;
  ra8_webp_arena_t scratch = {
    .base   = &arena->base[frame_pad],
    .cap    = scratch_n,
    .offset = 0U,
    .live   = 0U,
  };

  const size_t    row_stride = (size_t)webp_w * (size_t)k_ra8_img_rgba_bpp;
  const ra8_err_t dec_err =
    ra8_webp_decode_rgba(bytes, len, &scratch, frame, row_stride, frame_n, nullptr, nullptr);
  if (dec_err != k_ra8_ok) {
    ra8_log_error(s_tag_img, "blit: webp decode failed");
    return dec_err;
  }

  int32_t fit_w = 0;
  int32_t fit_h = 0;
  internal_fit_box((int32_t)webp_w, (int32_t)webp_h, box_w, box_h, &fit_w, &fit_h);
  internal_blit_scaled(frame,
                       (int32_t)webp_w,
                       (int32_t)webp_h,
                       fit_w,
                       fit_h,
                       dst_x,
                       dst_y,
                       (uint8_t)k_ra8_img_rgba_bpp);

  if (out_w != nullptr) {
    *out_w = fit_w;
  }
  if (out_h != nullptr) {
    *out_h = fit_h;
  }
  return k_ra8_ok;
}
#endif /* RA8_REFLOW_WEBP */

/** @brief Implementation of `ra8_img_decode_blit()` -- nearest-neighbour scale. */
ra8_err_t ra8_img_decode_blit(ra8_img_arena_t* arena,
                              const uint8_t*   bytes,
                              size_t           len,
                              int32_t          dst_x,
                              int32_t          dst_y,
                              int32_t          box_w,
                              int32_t          box_h,
                              int32_t*         out_w,
                              int32_t*         out_h)
{
  RA8_CHECK_NULL_PTR(arena, s_tag_img, "blit: null arena");
  RA8_CHECK_NULL_PTR(bytes, s_tag_img, "blit: null bytes");
  if ((len == 0U) || (box_w < (int32_t)k_ra8_img_min_edge) ||
      (box_h < (int32_t)k_ra8_img_min_edge)) {
    ra8_log_error(s_tag_img, "blit: empty input or box");
    return k_ra8_err_invalid_arg;
  }

#if defined(RA8_REFLOW_WEBP)
  /* stb_image has no WebP decoder: route the whole frame via ra8_webp (#637). */
  if (internal_is_webp(bytes, len)) {
    const ra8_err_t webp_err =
      internal_webp_decode_blit(arena, bytes, len, dst_x, dst_y, box_w, box_h, out_w, out_h);
    arena->offset = 0U;
    arena->live   = 0U;
    return webp_err;
  }
#endif /* RA8_REFLOW_WEBP */

  ra8_img_arena_bind(arena); /* resets the arena to empty */
  int sx   = 0;
  int sy   = 0;
  int comp = 0;
  /* The stb call stays on one line so the no-alloc audit
     (scripts/checks/check_no_dynamic_alloc.py) finds its opt-out on the flagged
     call line; clang-format would otherwise wrap it across many lines. */
  // clang-format off: the allocation opt-out comment must stay on the flagged call line.
  uint8_t* const pixels = stbi_load_from_memory(bytes, (int)len, &sx, &sy, &comp, (int)k_ra8_img_req_rgb); /* alloc-allow: stb is backed by the fixed ra8_img_arena (zero-heap), not malloc */
  // clang-format on
  /*
   * stbi_load_from_memory() returns a non-null pixel pointer only when it
   * decoded a bitmap of at least 1x1 (it rejects zero-dimension images with a
   * null return). So whenever pixels != nullptr, sx >= 1 and sy >= 1: the
   * (sx <= 0) and (sy <= 0) guards are defensive belt-and-suspenders that can
   * only be true when pixels == nullptr already made the decision true. Neither
   * can be flipped independently, so full MC/DC of this decision is unreachable.
   */
  /* mcdc-deactivated: stbi guarantees sx,sy >= 1 when pixels != nullptr; sx/sy guards are unreachable. */
  if ((pixels == nullptr) || (sx <= 0) || (sy <= 0)) {
    const ra8_err_t err = internal_decode_fail();
    internal_arena_release(arena);
    ra8_log_error(s_tag_img, "blit: decode failed");
    return err;
  }

  int32_t fit_w = 0;
  int32_t fit_h = 0;
  internal_fit_box((int32_t)sx, (int32_t)sy, box_w, box_h, &fit_w, &fit_h);
  internal_blit_scaled(pixels,
                       (int32_t)sx,
                       (int32_t)sy,
                       fit_w,
                       fit_h,
                       dst_x,
                       dst_y,
                       (uint8_t)k_ra8_img_req_rgb);
  // clang-format off: the allocation opt-out comment must stay on the flagged call line.
  stbi_image_free(pixels); /* alloc-allow: ra8_img_arena-backed (zero-heap), not malloc */
  // clang-format on
  internal_arena_release(arena);

  if (out_w != nullptr) {
    *out_w = fit_w;
  }
  if (out_h != nullptr) {
    *out_h = fit_h;
  }
  return k_ra8_ok;
}
