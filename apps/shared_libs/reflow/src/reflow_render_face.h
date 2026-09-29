/**
 * @file reflow_render_face.h
 * @brief Coverage-based fallback-face resolution for the render pass (#687).
 * @ingroup grp_ereader
 *
 * @details
 * Not part of the public API: a src-local seam between the render pass and
 * the face picker, so `reflow_render.c` asks one question ("which face draws
 * this code point?") and the answer lives beside the coverage probe that
 * produces it. The pure picker itself is exposed for tests through
 * `reflow_internal.h`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Reflow] {World: NS}
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "stb_truetype.h"

/**
 * @brief Pick the face that draws @p cp out of one render pass's face set.
 *
 * @details
 * Wraps ::priv_reflow_render_pick_face with the cmap-backed coverage probe,
 * so the render pass never has to know how coverage is answered. Order is
 * the run's own face, then the engine's bound default face at index 0, then
 * the registered `@font-face` blobs by index; @p primary comes back when
 * nothing covers @p cp, so the missing-glyph box is drawn at the metrics of
 * the surrounding text.
 *
 * @param[in] faces      Per-render font set: default at 0, registered at 1..N.
 * @param[in] face_count Usable entries in @p faces.
 * @param[in] primary    Face the layout pass selected for this run.
 * @param[in] cp         Code point from the layout pass.
 *
 * @return Face index to render @p cp with.
 * @retval primary The run's own face covers @p cp, no face does, @p cp is
 *                 blank, or @p faces is NULL.
 *
 * @pre @p faces holds @p face_count initialised entries, or is NULL.
 * @pre @p primary indexes @p faces, or fallback is skipped.
 * @post No state mutated; the font set is only read.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV uint8_t priv_reflow_render_face_for(const stbtt_fontinfo* faces,
                                             uint8_t               face_count,
                                             uint8_t               primary,
                                             int32_t               cp);
