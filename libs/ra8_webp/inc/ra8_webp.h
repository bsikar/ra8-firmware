/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_webp.h
 * @brief Zero-heap WebP (VP8 / VP8L) decode facade over the vendored libwebp.
 * @ingroup grp_ereader
 *
 * @details
 * Longstrip corpora are increasingly WebP-heavy, and `stb_image` cannot decode
 * WebP, so the e-reader vendors libwebp (decode-only) under
 * `libs/third_party/libwebp/` and wraps it here behind a small `ra8_err_t`
 * facade. The facade decodes an in-memory WebP directly into a caller-provided
 * RGBA8888 buffer, drawing all of libwebp's transient scratch from a
 * caller-owned ::ra8_webp_arena_t (see ra8_webp_arena.h) so the decode reaches
 * no `malloc` (NASA P10 Rule 3). The bytes are treated as fully
 * attacker-controlled initial-access content -- a WebP inside a downloaded
 * book -- and are fuzzed by `tests/fuzz/fuzz_ra8_webp.c`.
 *
 * @par Integration (#290 normalize-on-import):
 * The JOF tile producer consumes this facade
 * (`ra8_jof_produce()` -> `priv_webp_transcode`): a WebP manifest image
 * is decoded whole-frame here and banded into the one normalized band-tile
 * format, so render time touches a single codec regardless of source. The
 * small-image (non-tiled) `ra8_reflow` / `ra8_img` inline raster dispatch does
 * not yet have a WebP arm -- that lands with the #289 longstrip render path; see
 * the `TODO(#289)` seam in ra8_webp.c.
 *
 *
 * [Ring 4 / WebP] {World: NS}
 *
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_webp_arena.h"

/**
 * @enum ra8_webp_limits_t
 * @brief WebP decode facade fixed limits (no magic numbers).
 *
 * @details These bound attacker-controlled inputs before any allocation:
 * @c k_ra8_webp_max_dim rejects an absurd declared width/height the way
 * `stb_image_impl.c` caps `STBI_MAX_DIMENSIONS`, keeping `w * h * bpp` far
 * below the 32-bit overflow threshold; @c k_ra8_webp_bytes_per_px fixes the
 * RGBA8888 output pixel stride.
 *
 * @invariant `k_ra8_webp_max_dim * k_ra8_webp_max_dim * k_ra8_webp_bytes_per_px`
 *            fits in a 32-bit unsigned integer without overflow.
 * @see ra8_webp_decode_rgba()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_webp_max_dim      = 8192U, /**< Max accepted width/height, pixels/axis. */
  k_ra8_webp_bytes_per_px = 4U,    /**< Output bytes per pixel (RGBA8888).      */
} ra8_webp_limits_t;

/**
 * @brief Read a WebP header's pixel dimensions without decoding the body.
 *
 * @details Wraps libwebp's `WebPGetInfo`. Validates the RIFF/VP8(L) container
 * and returns the declared canvas size, rejecting a non-WebP buffer or an
 * absurd dimension (> ::k_ra8_webp_max_dim per axis) so a caller can size an
 * output buffer / arena before committing to a full decode.
 *
 * @param[in]  data  Pointer to the in-memory WebP bytes. Must be non-NULL.
 * @param[in]  size  Length of @p data in bytes. Must be non-zero.
 * @param[out] out_w Receives the decoded width in pixels. Must be non-NULL.
 * @param[out] out_h Receives the decoded height in pixels. Must be non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Header parsed; @p out_w / @p out_h set.
 * @retval k_ra8_err_null_ptr          @p data, @p out_w or @p out_h is NULL.
 * @retval k_ra8_err_invalid_arg       @p size is zero.
 * @retval k_ra8_err_validation_failed Not a valid WebP / non-positive dims.
 * @retval k_ra8_err_not_supported     A dimension exceeds ::k_ra8_webp_max_dim.
 *
 * @pre @p data points to at least @p size readable bytes.
 * @pre @p out_w and @p out_h point to writable `uint32_t` storage.
 * @post On k_ra8_ok, `*out_w` and `*out_h` are in `[1, k_ra8_webp_max_dim]`.
 * @post On any error, `*out_w` and `*out_h` are left unmodified.
 *
 * @note Not thread-safe: WebP decoding is single-threaded on this target.
 * @note Pure query: does not allocate and does not bind an arena.
 *
 * @par Example:
 * @code
 * uint32_t w = 0, h = 0;
 * if (ra8_webp_get_info(buf, len, &w, &h) == k_ra8_ok) { ... }
 * @endcode
 *
 * @see ra8_webp_decode_rgba()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_webp_get_info(const uint8_t* data, size_t size, uint32_t* out_w, uint32_t* out_h);

/**
 * @brief Decode a WebP into a caller-owned RGBA8888 buffer, heap-free.
 *
 * @details Validates the header via ra8_webp_get_info(), checks the output
 * buffer is large enough for `height * stride`, binds @p arena as libwebp's
 * scratch allocator, and calls `WebPDecodeRGBAInto` to decode straight into
 * @p out_rgba. The arena supplies every internal allocation, so the decode
 * reaches no `malloc`; it fully drains before this call returns. Both VP8
 * (lossy) and VP8L (lossless) WebP bitstreams are handled by the one codec.
 *
 * @param[in]  data         In-memory WebP bytes. Must be non-NULL.
 * @param[in]  size         Length of @p data in bytes. Must be non-zero.
 * @param[in,out] arena     Scratch arena; reset, used, and drained here. Must
 *                          be non-NULL and back at least the decode's peak
 *                          scratch footprint (a few KiB for a thumbnail up to
 *                          a few MiB for a full page).
 * @param[out] out_rgba     Destination RGBA8888 pixels. Must be non-NULL and
 *                          span at least @p out_capacity bytes.
 * @param[in]  out_stride   Destination row stride in bytes; must be at least
 *                          `width * k_ra8_webp_bytes_per_px` and `<= INT_MAX`.
 * @param[in]  out_capacity Size of @p out_rgba in bytes; must be at least
 *                          `height * out_stride`.
 * @param[out] out_w        Receives the decoded width. May be NULL.
 * @param[out] out_h        Receives the decoded height. May be NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                     Decoded; @p out_rgba filled top-to-bottom.
 * @retval k_ra8_err_null_ptr           @p data, @p arena or @p out_rgba is NULL.
 * @retval k_ra8_err_invalid_arg        @p size is zero.
 * @retval k_ra8_err_validation_failed  Not a valid WebP, or the decode failed
 *                                      (corrupt body or arena exhausted).
 * @retval k_ra8_err_not_supported      A dimension exceeds ::k_ra8_webp_max_dim.
 * @retval k_ra8_err_range_check_failed @p out_stride / @p out_capacity too small,
 *                                      or @p out_stride exceeds `INT_MAX`.
 *
 * @pre @p arena has `base` pointing at `cap` writable bytes and `cap` >= the
 *      decode's peak scratch footprint.
 * @pre @p out_rgba spans at least @p out_capacity writable bytes.
 * @post On k_ra8_ok, @p out_rgba holds `height` rows of RGBA8888 at @p out_stride.
 * @post On return the arena is drained (`live == 0`, `offset == 0`) and unbound.
 *
 * @note Not thread-safe: WebP decoding is single-threaded on this target.
 * @warning @p arena is reset on entry; any prior contents are discarded.
 *
 * @par Example:
 * @code
 * alignas(16) static uint8_t scratch[2U * 1024U * 1024U];
 * ra8_webp_arena_t arena = {.base = scratch, .cap = sizeof scratch};
 * uint32_t w = 0, h = 0;
 * ra8_err_t e = ra8_webp_decode_rgba(buf, len, &arena, fb, w * 4U,
 *                                    sizeof fb, &w, &h);
 * @endcode
 *
 * @see ra8_webp_get_info()
 * @see ra8_webp_arena_bind()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_webp_decode_rgba(const uint8_t*    data,
                                             size_t            size,
                                             ra8_webp_arena_t* arena,
                                             uint8_t*          out_rgba,
                                             size_t            out_stride,
                                             size_t            out_capacity,
                                             uint32_t*         out_w,
                                             uint32_t*         out_h);
