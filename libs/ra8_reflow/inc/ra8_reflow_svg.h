/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_reflow_svg.h
 * @brief Minimal SVG handling for the ra8_reflow ereader (#112).
 * @ingroup grp_ereader
 *
 * @details
 * A small, hand-written, zero-allocation SVG subset for the two forms that
 * actually appear in EPUBs:
 *
 *   1. **Cover wrappers** -- a `<svg>` whose only content is a single
 *      `<image xlink:href="cover.jpg"/>`. ::ra8_svg_image_href returns that href
 *      so the caller can decode the referenced raster through the existing
 *      ra8_img_decode_blit path (no new decoder needed).
 *   2. **Simple in-content vector graphics** -- `<rect>`, `<circle>`, `<line>`,
 *      `<polygon>` (scanline-filled), `<polyline>` (stroked), and `<path>`
 *      (filled; M/L/H/V/Z exact, the C/S/Q/T Bezier curves and the A elliptical
 *      arc flattened to line segments), with solid `fill` / `stroke`. A
 *      A full 2x3 affine `transform=` (`translate` / `scale` / `rotate` /
 *      `skewX` / `skewY` / `matrix`) -- on a shape or a `<g>` group (nested,
 *      bounded depth) -- repositions, resizes, rotates, and shears the result;
 *      axis-aligned shapes keep the exact `ra8_gfx` fast-path while rotated /
 *      sheared shapes are mapped through the affine and scanline-polygon-filled.
 *      ::ra8_svg_render maps them from the SVG `viewBox` (or width/height) into a
 *      caller box and draws them through the bound ra8_gfx framebuffer.
 *
 * Out of scope (documented limitations, tracked as follow-ups): gradient fills
 * (`linearGradient` / `radialGradient`), opacity, CSS styling, stroke width, and
 * fractional coordinates (truncated to integers).
 *
 * The parser is pure string scanning over the caller's byte buffer (no DOM, no
 * heap); ::ra8_svg_render's only side effect is drawing through ra8_gfx.
 *
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @brief Heuristically decide whether @p bytes are an SVG document.
 *
 * @details Skips a leading UTF-8 BOM + whitespace and checks for `<?xml` or a
 * `<svg` element start (case-insensitive). Used by the image path to route SVG
 * bytes here instead of to the raster decoder.
 *
 * @param[in] bytes Candidate image bytes.
 * @param[in] len   Length of @p bytes.
 *
 * @return true iff @p bytes look like SVG.
 * @retval false @p bytes is NULL/empty or does not begin like SVG.
 *
 * @pre None.
 * @pre None.
 * @post No state modified.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_svg_is_svg(const uint8_t* bytes, size_t len);

/**
 * @brief Extract the `href` of a cover-wrapper `<svg><image .../></svg>`.
 *
 * @details Scans for the first `<image>` element and returns its
 * `xlink:href` / `href` attribute value as a slice into @p svg. The caller
 * resolves + decodes that raster through its normal image path.
 *
 * @param[in]  svg     SVG document bytes.
 * @param[in]  len     Length of @p svg.
 * @param[out] out_off Receives the href slice offset into @p svg.
 * @param[out] out_len Receives the href slice length, bytes.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok            An `<image>` href was found.
 * @retval k_ra8_err_null_ptr  A required pointer is NULL.
 * @retval k_ra8_err_not_found No `<image>` with an href was present.
 *
 * @pre @p svg, @p out_off, @p out_len non-NULL.
 * @pre None.
 * @post On success the outputs slice @p svg.
 * @post On failure the outputs are unchanged.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_svg_image_href(const uint8_t* svg, size_t len, size_t* out_off, size_t* out_len);

/**
 * @brief Report an SVG's intrinsic size (width/height, else viewBox dimensions).
 *
 * @details Lets the layout reserve an `<img src="*.svg">` box without a raster
 * decode. Prefers the `<svg>` `width`/`height` attributes (units ignored), and
 * falls back to the `viewBox` width/height.
 *
 * @param[in]  svg   SVG document bytes.
 * @param[in]  len   Length of @p svg.
 * @param[out] out_w Receives the intrinsic width, pixels.
 * @param[out] out_h Receives the intrinsic height, pixels.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok            A positive size was determined.
 * @retval k_ra8_err_null_ptr  A required pointer is NULL.
 * @retval k_ra8_err_not_found No `<svg>` or no positive size present.
 *
 * @pre @p svg, @p out_w, @p out_h non-NULL.
 * @pre None.
 * @post On success @p *out_w > 0 and @p *out_h > 0.
 * @post On failure the outputs are unchanged.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_svg_size(const uint8_t* svg, size_t len, int32_t* out_w, int32_t* out_h);

/**
 * @brief Render an SVG's vector shapes into a box.
 *
 * @details Establishes a coordinate transform from the SVG `viewBox` (or its
 * `width`/`height`, defaulting to the box) onto the caller box @p (x,y,w,h),
 * then walks the supported shapes (`<rect>` / `<circle>` / `<polygon>` filled,
 * `<line>` / `<polyline>` stroked) and draws each through the bound ra8_gfx
 * framebuffer with its `fill` or `stroke` colour. `fill="none"` shapes are
 * skipped. Coordinates outside the box are clipped by ra8_gfx.
 *
 * @param[in] svg SVG document bytes.
 * @param[in] len Length of @p svg.
 * @param[in] x   Box left, framebuffer pixels.
 * @param[in] y   Box top, framebuffer pixels.
 * @param[in] w   Box width, pixels (> 0).
 * @param[in] h   Box height, pixels (> 0).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Rendered (possibly zero shapes).
 * @retval k_ra8_err_null_ptr    @p svg is NULL.
 * @retval k_ra8_err_invalid_arg @p w or @p h is non-positive.
 *
 * @pre @p svg non-NULL; ra8_gfx_init() has bound a framebuffer.
 * @pre @p w > 0 and @p h > 0.
 * @post Shapes are drawn into the bound framebuffer within @p (x,y,w,h).
 * @post No allocation performed.
 *
 * @note Not thread-safe relative to the bound ra8_gfx framebuffer.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_svg_render(const uint8_t* svg, size_t len, int32_t x, int32_t y, int32_t w, int32_t h);

#ifdef __cplusplus
}
#endif
