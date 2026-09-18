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
 * stb_image cannot decode WebP, so a RIFF/WEBP buffer is dispatched instead to
 * the `ra8_webp` facade over the vendored libwebp (#637): the inline
 * small-image path now takes the same formats the band-tile producer does, and
 * an EPUB whose illustrations are WebP no longer renders them as nothing. The
 * WebP canvas is RGBA8888, so the blit carries a source bytes-per-pixel rather
 * than assuming the stb RGB triple.
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
#include "ra8_webp.h"
#include "reflow_svg.h"
#include "stb_image.h"

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
  k_ra8_img_req_rgba = 4, /**< Channel count libwebp decodes into (RGBA8888).  */
} ra8_img_pack_t;

/**
 * @enum ra8_img_shift_t
 * @brief Channel shifts to assemble a 0x00RRGGBB colour for ra8_gfx (no magics).
 */
typedef enum : uint8_t {
  k_ra8_img_shift_r = 16, /**< Red channel shift into 0x00RRGGBB.   */
  k_ra8_img_shift_g = 8,  /**< Green channel shift into 0x00RRGGBB. */
} ra8_img_shift_t;

/**
 * @enum ra8_img_webp_sig_t
 * @brief RIFF/WEBP container signature offsets and lengths (no magic numbers).
 *
 * @details A WebP file is a RIFF container: the four ASCII bytes `RIFF`, a
 * 32-bit chunk size, then the four ASCII bytes `WEBP`. Recognising those twelve
 * bytes is enough to route the buffer away from stb_image (which cannot decode
 * WebP) and into the ra8_webp facade, which then does the real validation.
 *
 * @invariant `k_ra8_img_webp_sig_len` covers both tags and the size field.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_img_webp_riff_off = 0,  /**< Offset of the `RIFF` tag.           */
  k_ra8_img_webp_tag_len  = 4,  /**< Length of either four-byte tag.     */
  k_ra8_img_webp_form_off = 8,  /**< Offset of the `WEBP` form tag.      */
  k_ra8_img_webp_sig_len  = 12, /**< Bytes needed to test the signature. */
} ra8_img_webp_sig_t;

/**
 * @brief Report whether @p bytes opens with a RIFF/WEBP container signature.
 *
 * @details Compares the first four bytes against `RIFF` and bytes 8..11 against
 * `WEBP`, the two fixed tags of a WebP file (the four bytes between them are the
 * RIFF chunk size and carry no signature). A buffer shorter than
 * ::k_ra8_img_webp_sig_len cannot hold both tags and is reported as not-WebP, so
 * it falls through to stb_image and is rejected there as a truncated image
 * rather than mis-routed into the WebP facade. The predicate is deliberately
 * cheap: it only decides which decoder sees the bytes, and the facade does the
 * real container and dimension validation. Internal helper for
 * ra8_img_probe_size() and ra8_img_decode_blit().
 *
 * @param[in] bytes Encoded image bytes; must not be NULL.
 * @param[in] len   Length of @p bytes in bytes.
 *
 * @return bool Whether the buffer is a WebP container.
 * @retval true  Both tags matched and @p len is at least the signature length.
 * @retval false @p len is too short, or either tag did not match.
 *
 * @pre @p bytes points to at least @p len readable bytes.
 * @post @p bytes is not modified (read-only comparison).
 *
 * @note Pure read of @p bytes; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_is_webp(const uint8_t* bytes, size_t len)
{
  if (len < (size_t)k_ra8_img_webp_sig_len) {
    return false;
  }
  return (memcmp(&bytes[k_ra8_img_webp_riff_off], "RIFF", (size_t)k_ra8_img_webp_tag_len) == 0) &&
         (memcmp(&bytes[k_ra8_img_webp_form_off], "WEBP", (size_t)k_ra8_img_webp_tag_len) == 0);
}

/**
 * @brief Map an ra8_webp facade status onto ra8_img's documented error set.
 *
 * @details The facade reports a richer set than this module publishes: a
 * malformed container, a non-positive canvas, and a failed body decode all come
 * back as ::k_ra8_err_validation_failed, and an over-cap dimension as
 * ::k_ra8_err_not_supported. Both mean the same thing to a caller of this
 * module -- these bytes cannot be drawn -- so they collapse onto
 * ::k_ra8_err_not_supported, keeping ra8_img_decode_blit()'s published retval
 * list unchanged. ::k_ra8_err_null_ptr and ::k_ra8_err_invalid_arg pass through
 * because they mean the same thing in both contracts.
 *
 * @param[in] err Status returned by ra8_webp_get_info() / ra8_webp_decode_rgba().
 *
 * @return ra8_err_t The equivalent ra8_img status.
 * @retval k_ra8_ok                Passed through unchanged.
 * @retval k_ra8_err_null_ptr      Passed through unchanged.
 * @retval k_ra8_err_invalid_arg   Passed through unchanged.
 * @retval k_ra8_err_not_supported Any other facade status.
 *
 * @pre @p err is a status value returned by the ra8_webp facade.
 * @post No state is read or modified; the mapping is pure.
 *
 * @note One caveat this mapping cannot avoid: libwebp reports an exhausted
 *       scratch arena as a plain decode failure, so a shortfall in the WebP
 *       scratch surfaces as ::k_ra8_err_not_supported, not ::k_ra8_err_no_mem.
 *       The canvas allocation, which dominates the arena footprint, is checked
 *       separately in internal_webp_decode_blit() and does report no_mem.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_webp_classify(ra8_err_t err)
{
  if ((err == k_ra8_ok) || (err == k_ra8_err_null_ptr) || (err == k_ra8_err_invalid_arg)) {
    return err;
  }
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_img_probe_size(const uint8_t* bytes, size_t len, int32_t* out_w, int32_t* out_h)
{
  RA8_CHECK_NULL_PTR(bytes, s_tag_img, "probe: null bytes");
  RA8_CHECK_NULL_PTR(out_w, s_tag_img, "probe: null out_w");
  RA8_CHECK_NULL_PTR(out_h, s_tag_img, "probe: null out_h");

  /* SVG is not a raster: take its intrinsic size from the document (#112). */
  if (ra8_svg_is_svg(bytes, len)) {
    return ra8_svg_size(bytes, len, out_w, out_h);
  }

  /* stb_image cannot parse a WebP header; the ra8_webp facade can (#637). */
  if (internal_is_webp(bytes, len)) {
    uint32_t        webp_w = 0U;
    uint32_t        webp_h = 0U;
    const ra8_err_t err    = ra8_webp_get_info(bytes, len, &webp_w, &webp_h);
    if (err != k_ra8_ok) {
      ra8_log_error(s_tag_img, "probe: webp header rejected");
      return internal_webp_classify(err);
    }
    *out_w = (int32_t)webp_w;
    *out_h = (int32_t)webp_h;
    return k_ra8_ok;
  }

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
 * @param[in] pixels  Decoded source buffer, row-major pixels of @p src_bpp
 *                    bytes each, size `src_w * src_h * src_bpp`; not NULL.
 * @param[in] src_w   Source width, pixels (>= 1).
 * @param[in] src_h   Source height, pixels (>= 1).
 * @param[in] src_bpp Source bytes per pixel: ::k_ra8_img_req_rgb for an
 *                    stb_image RGB decode, ::k_ra8_img_req_rgba for a libwebp
 *                    RGBA decode. The red, green, and blue bytes sit at the
 *                    same offsets in both packings, so only the stride differs.
 * @param[in] fit_w   Destination width, pixels (>= 1).
 * @param[in] fit_h   Destination height, pixels (>= 1).
 * @param[in] dst_x   Destination left edge in framebuffer coordinates.
 * @param[in] dst_y   Destination top edge in framebuffer coordinates.
 * @return Nothing.
 *
 * @pre @p pixels is a valid pointer to `src_w * src_h * src_bpp` readable bytes.
 * @pre All dimension arguments (@p src_w, @p src_h, @p fit_w, @p fit_h)
 *      are >= 1 so neither loop bound is zero and no division by zero occurs.
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
                                 int32_t        src_bpp,
                                 int32_t        fit_w,
                                 int32_t        fit_h,
                                 int32_t        dst_x,
                                 int32_t        dst_y)
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

/**
 * @brief Decode a WebP through the ra8_webp facade and blit it scaled to fit.
 *
 * @details The WebP arm of ra8_img_decode_blit() (#637). Reads the canvas size
 * from the container, then splits the caller's single arena in two: the decoded
 * RGBA8888 canvas is bump-allocated from it through ra8_img_arena_malloc(), and
 * whatever capacity is left above that allocation backs a sibling
 * ::ra8_webp_arena_t for libwebp's own transient scratch. Two arenas are needed
 * because the facade and stb_image have separate allocator hooks; one backing
 * store still serves both, so a caller sizes exactly one buffer. libwebp
 * decodes straight into the canvas, the canvas is nearest-neighbour blitted at
 * ::k_ra8_img_req_rgba, and the arena is released on every path.
 *
 * @param[in,out] arena Bump arena (reset on entry); backs the canvas and the
 *                      WebP scratch. Must not be NULL.
 * @param[in]     bytes WebP container bytes. Must not be NULL.
 * @param[in]     len   Length of @p bytes.
 * @param[in]     dst_x Destination left edge, framebuffer pixels.
 * @param[in]     dst_y Destination top edge, framebuffer pixels.
 * @param[in]     box_w Available box width to scale into (>= 1).
 * @param[in]     box_h Available box height to scale into (>= 1).
 * @param[out]    out_w Receives the blitted (scaled) width (NULL ok).
 * @param[out]    out_h Receives the blitted (scaled) height (NULL ok).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                 Canvas decoded, scaled, and blitted.
 * @retval k_ra8_err_no_mem         The arena cannot hold the decoded canvas.
 * @retval k_ra8_err_not_supported  Bad container, over-cap dimension, or the
 *                                  body failed to decode.
 *
 * @pre `ra8_gfx_init()` bound a framebuffer.
 * @pre internal_is_webp(@p bytes, @p len) is true.
 * @pre @p box_w and @p box_h are each >= ::k_ra8_img_min_edge.
 * @post On any return @p arena->offset == 0 and @p arena->live == 0, and no
 *       arena remains bound to either allocator.
 *
 * @note Not thread-safe: both arenas are reached through module-static hooks.
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
  uint32_t        src_w = 0U;
  uint32_t        src_h = 0U;
  const ra8_err_t info  = ra8_webp_get_info(bytes, len, &src_w, &src_h);
  if (info != k_ra8_ok) {
    ra8_log_error(s_tag_img, "blit: webp header rejected");
    return internal_webp_classify(info);
  }

  const size_t stride = (size_t)src_w * (size_t)k_ra8_img_req_rgba;
  const size_t canvas = (size_t)src_h * stride;

  ra8_img_arena_bind(arena); /* resets the arena to empty */
  uint8_t* const rgba = ra8_img_arena_malloc(canvas);
  if (rgba == nullptr) {
    internal_arena_release(arena);
    ra8_log_error(s_tag_img, "blit: webp canvas does not fit the arena");
    return k_ra8_err_no_mem;
  }

  /* Whatever the canvas did not take backs libwebp's own scratch. */
  ra8_webp_arena_t scratch = {.base   = &arena->base[arena->offset],
                              .cap    = arena->cap - arena->offset,
                              .offset = 0U,
                              .live   = 0U};
  const ra8_err_t  dec     = ra8_webp_decode_rgba(bytes,
                                             len,
                                             &scratch,
                                             rgba,
                                             stride,
                                             canvas,
                                             nullptr,
                                             nullptr);
  if (dec != k_ra8_ok) {
    internal_arena_release(arena);
    ra8_log_error(s_tag_img, "blit: webp decode failed");
    return internal_webp_classify(dec);
  }

  int32_t fit_w = 0;
  int32_t fit_h = 0;
  internal_fit_box((int32_t)src_w, (int32_t)src_h, box_w, box_h, &fit_w, &fit_h);
  internal_blit_scaled(rgba,
                       (int32_t)src_w,
                       (int32_t)src_h,
                       (int32_t)k_ra8_img_req_rgba,
                       fit_w,
                       fit_h,
                       dst_x,
                       dst_y);
  internal_arena_release(arena);

  if (out_w != nullptr) {
    *out_w = fit_w;
  }
  if (out_h != nullptr) {
    *out_h = fit_h;
  }
  return k_ra8_ok;
}

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

  /* stb_image has no WebP decoder: route the container to libwebp (#637). */
  if (internal_is_webp(bytes, len)) {
    return internal_webp_decode_blit(arena, bytes, len, dst_x, dst_y, box_w, box_h, out_w, out_h);
  }

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
                       (int32_t)k_ra8_img_req_rgb,
                       fit_w,
                       fit_h,
                       dst_x,
                       dst_y);
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
