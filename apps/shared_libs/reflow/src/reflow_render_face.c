/**
 * @file reflow_render_face.c
 * @brief Coverage-based fallback-face resolution for the render pass (#687).
 *
 * @details
 * Implements reflow_render_face.h and the pure picker declared in
 * reflow_internal.h. CSS `@font-face` selection matches a run to a face by
 * family, weight and style, which says nothing about whether that face
 * carries the run's code points, so a book whose body face lacks a character
 * loses it even when another registered face has it. This resolves the face
 * a second time, by coverage, just before the glyph is blitted.
 *
 * The coverage question is injected rather than called directly, so the
 * ordering policy is drivable from a test with no font file while production
 * answers it from each face's own cmap.
 *
 * [Ring 4 / Reflow] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "reflow_render_face.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "reflow_internal.h"
#include "stb_truetype.h"

/**
 * @struct internal_face_cov_t
 * @brief Face set handed to the coverage probe during one render pass.
 *
 * @details Wraps the per-render `stbtt_fontinfo` array so the picker can ask
 * about a face by index without knowing what a face is made of.
 */
typedef struct {
  const stbtt_fontinfo* faces; /**< Default face at 0, registered at 1..N. */
} internal_face_cov_t;

/**
 * @brief Answer ::priv_reflow_face_has_glyph_fn from a render face array.
 *
 * @details Asks the face's own cmap through `stbtt_FindGlyphIndex`; index 0
 * is stb's "this face has no glyph for that code point" answer, which is
 * exactly the coverage question the picker asks.
 *
 * @param[in] ctx      Face set (::internal_face_cov_t), or NULL.
 * @param[in] face_idx Face to probe; the picker keeps it in range.
 * @param[in] cp       Code point to look for.
 * @return Boolean coverage answer.
 * @retval true  That face carries a glyph for @p cp.
 * @retval false That face has no glyph, or @p ctx is unusable.
 * @pre @p face_idx indexes an initialised entry of `ctx->faces`.
 * @post No state mutated.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_face_has_glyph(const void* ctx, uint8_t face_idx, int32_t cp)
{
  const internal_face_cov_t* cov = (const internal_face_cov_t*)ctx;
  if ((cov == nullptr) || (cov->faces == nullptr)) {
    return false;
  }
  return stbtt_FindGlyphIndex(&cov->faces[face_idx], cp) != 0;
}

uint8_t priv_reflow_render_pick_face(priv_reflow_face_has_glyph_fn has_glyph,
                                     const void*                   ctx,
                                     uint8_t                       face_count,
                                     uint8_t                       primary,
                                     int32_t                       cp)
{
  /* No probe, a single face, a bogus index, or a code point that must stay
   * blank: the run keeps the face the layout pass chose. */
  if ((has_glyph == nullptr) || (face_count <= 1U) || (primary >= face_count) ||
      priv_reflow_render_is_blank_cp(cp)) {
    return primary;
  }
  if (has_glyph(ctx, primary, cp)) {
    return primary;
  }
  if ((primary != 0U) && has_glyph(ctx, 0U, cp)) {
    return 0U; /* The engine's bound default face is the first fallback. */
  }
  for (uint8_t k = 1U; k < face_count; ++k) {
    if ((k != primary) && has_glyph(ctx, k, cp)) {
      return k;
    }
  }
  return primary; /* Nothing covers it: the missing-glyph box draws here. */
}

uint8_t priv_reflow_render_face_for(const stbtt_fontinfo* faces,
                                    uint8_t               face_count,
                                    uint8_t               primary,
                                    int32_t               cp)
{
  const internal_face_cov_t cov = {.faces = faces};
  return priv_reflow_render_pick_face(internal_face_has_glyph, &cov, face_count, primary, cp);
}
