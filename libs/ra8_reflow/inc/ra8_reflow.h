/**
 * @file ra8_reflow.h
 * @brief HTML / CSS reflow + paginate engine for the ra8d2 ereader.
 * @ingroup grp_ereader
 *
 * @details
 * `ra8_reflow` is a small, hand-written HTML reflow + pagination engine
 * sitting between `libs/ra8_epub` (which produces per-chapter XHTML
 * byte streams) and `libs/ra8_gfx` (which owns the framebuffer). It is
 * the page-layout core of the ereader application.
 *
 * The engine is intentionally bare-metal friendly:
 *
 *   - **Zero dynamic allocation.** Every buffer is statically sized via
 *     a `ra8_reflow_limits_t` enum and lives inside the engine handle.
 *   - **Greedy line-break.** Walks each text run word-by-word measuring
 *     glyph widths via `stb_truetype`; when adding the next word would
 *     exceed `viewport_w - 2 * k_ra8_reflow_margin_px`, the engine
 *     breaks and starts a new line.
 *   - **Page-break-on-overflow.** When the accumulated line height
 *     would exceed `viewport_h - 2 * k_ra8_reflow_margin_px`, the
 *     engine starts a new page.
 *   - **Caller-owned framebuffer.** Rendering blits each glyph via
 *     `ra8_gfx_pixel()` so the same engine works on the GLCDC plane,
 *     an off-screen scratch buffer, or a host-test buffer.
 *
 * ## Supported HTML subset (v1)
 *
 *   - Block-flow tags:    `<p>`, `<h1>` .. `<h6>`, `<blockquote>`,
 *                          `<ul>`, `<ol>`, `<li>`, `<hr>`.
 *   - Tables:             `<table>` / `<tr>` / `<td>` / `<th>` -- an
 *                          equal-column grid with per-cell text flow and
 *                          row-level page breaks (#107).
 *   - Inline tags:        `<em>`, `<strong>`, `<b>`, `<i>`, `<a>`,
 *                          `<br>`. `<a href>` links are hit-testable and
 *                          followable (#110).
 *   - Replaced elements:  `<img>` -- decoded + scaled + blitted when an
 *                          image loader is bound, else a placeholder (#106).
 *   - Alignment:          `text-align` (left / right / centre / justify)
 *                          from an inline `style` on a block (#108).
 *   - Everything else (the rest of CSS, scripts, `<div>`, `<span>`) is
 *     treated as a transparent flow-pass-through; child content is still
 *     laid out, the wrapping element itself contributes no styling.
 *
 * ## Lifecycle
 *
 *   1. `ra8_reflow_init()` -- bind viewport, font, colours.
 *   2. `ra8_reflow_layout_chapter()` -- parse + lay out one XHTML
 *      chapter, return total page count.
 *   3. `ra8_reflow_render_page()` -- rasterise page N into the active
 *      ra8_gfx framebuffer.
 *   4. (optional) `ra8_reflow_set_font_size()` -- triggers a re-flow on
 *      the cached chapter.
 *   5. `ra8_reflow_close()` -- mark engine unused.
 *
 * @note This is a thin umbrella header. The data model (enums, structs,
 *       the engine handle) lives in `ra8_reflow_types.h`, and the callable
 *       function prototypes live in `ra8_reflow_api.h`. Consumers should
 *       continue to include `ra8_reflow.h` directly -- it pulls in both
 *       sub-headers so the public surface is unchanged.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
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

#include "ra8_reflow_api.h"   /* lifecycle / layout / render / internal prototypes */
#include "ra8_reflow_types.h" /* enums, structs, typedefs, engine handle           */

#ifdef __cplusplus
}
#endif
