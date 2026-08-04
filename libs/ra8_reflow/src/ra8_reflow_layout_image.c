/**
 * @file ra8_reflow_layout_image.c
 * @brief Block-level `<img>` layout for the ra8_reflow engine (#106).
 *
 * @details
 * Splits the image-layout sub-responsibility out of `ra8_reflow_layout.c` so
 * each translation unit stays under the project file-size cap. An `<img>` is
 * laid out as a block: when a loader + arena are bound and the token carries a
 * source slice, the encoded bytes are fetched, the intrinsic size is probed
 * (zero-alloc), the box is scaled to fit the text column without upscaling, the
 * cursor page-breaks as needed, and the box is recorded for the render pass.
 * If no loader is bound -- or any sizing step fails -- the historic fixed-size
 * placeholder advance is used so image-free content remains byte-identical.
 *
 * The driver reaches this module via @ref ra8_reflow_layout_apply_image; the
 * core inline-flow helpers it reuses (line wrap, page flush) are shared through
 * `ra8_reflow_layout_internal.h`.
 *
 *
 * [Ring 4 / Reflow] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_reflow.h"
#include "ra8_reflow_image.h"
#include "ra8_reflow_internal.h"
#include "ra8_reflow_layout_internal.h"
#include "stb_truetype.h"

/**
 * @brief Apply an `<img>` token without a bound loader: placeholder advance.
 *
 * @details See implementation. Historical v1 behaviour, kept so image-free
 * content (and content laid out before a loader is bound) is byte-identical.
 * @param[in] engine See implementation.
 * @param[in] cur See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_apply_image_placeholder(ra8_reflow_t* engine, priv_cursor_t* cur)
{
  const int32_t advance     = (int32_t)k_priv_image_placeholder_px;
  const int32_t right_limit = (int32_t)engine->viewport_w - (int32_t)k_ra8_reflow_margin_px;
  if (ra8_reflow_internal_right_overflow_break(cur->x,
                                               advance,
                                               right_limit,
                                               cur->line_has_content)) {
    if (!ra8_reflow_layout_newline(engine, cur, false)) {
      return false;
    }
  }
  cur->x += advance;
  cur->line_has_content = 1U;
  return true;
}

/**
 * @brief True iff the page under construction already holds glyphs or images.
 *
 * @details See implementation.
 * @param[in] engine See implementation.
 * @param[in] cur See implementation.
 * @return Boolean.
 * @retval true Page has content.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post No state mutated.
 * @post No state mutated.
 * @note Pure read.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_page_has_content(const ra8_reflow_t* engine, const priv_cursor_t* cur)
{
  return (engine->glyph_count > cur->page_first_glyph) ||
         (engine->image_box_count > cur->page_first_image);
}

/**
 * @brief Scale an intrinsic image size to fit the text column (no upscaling).
 *
 * @details See implementation. Caps width at the column, height at one page;
 * preserves aspect ratio with int64 products to avoid overflow.
 * @param[in] iw See implementation.
 * @param[in] ih See implementation.
 * @param[in] col_w See implementation.
 * @param[in] avail_h See implementation.
 * @param[out] out_w See implementation.
 * @param[out] out_h See implementation.
 * @return None.
 * @pre `iw > 0` and `ih > 0`.
 * @pre `col_w > 0` and `avail_h > 0`.
 * @post `*out_w` in [1, col_w] and `*out_h` in [1, avail_h].
 * @post Aspect ratio preserved within integer rounding.
 * @note Pure function.
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_image_fit(int32_t  iw,
                           int32_t  ih,
                           int32_t  col_w,
                           int32_t  avail_h,
                           int32_t* out_w,
                           int32_t* out_h)
{
  int32_t bw = (iw < col_w) ? iw : col_w;
  int32_t bh = (int32_t)(((int64_t)ih * (int64_t)bw) / (int64_t)iw);
  if (bh > avail_h) {
    bh = avail_h;
    bw = (int32_t)(((int64_t)iw * (int64_t)bh) / (int64_t)ih);
  }
  *out_w = (bw < 1) ? 1 : bw;
  *out_h = (bh < 1) ? 1 : bh;
}

/**
 * @brief Resolve an image token to a column-fitted box size via the loader.
 *
 * @details See implementation. Calls the bound loader for the encoded bytes,
 * probes the intrinsic size (zero-alloc), and fits it to the column.
 * @param[in] engine See implementation.
 * @param[in] tok See implementation.
 * @param[out] out_w See implementation.
 * @param[out] out_h See implementation.
 * @return Boolean.
 * @retval true Box size resolved.
 * @retval false Loader failed, probe failed, or viewport too small.
 * @pre `engine->img_loader != nullptr`.
 * @pre `tok->text_len > 0`.
 * @post On true, `*out_w`/`*out_h` are a valid column-fit box.
 * @post On false, no box is produced (caller falls back).
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_image_resolve_size(ra8_reflow_t*             engine,
                                    const ra8_reflow_token_t* tok,
                                    int32_t*                  out_w,
                                    int32_t*                  out_h)
{
  const char*    href  = (const char*)&engine->text_pool[tok->text_off];
  const uint8_t* bytes = nullptr;
  size_t         blen  = 0U;
  if (engine->img_loader(engine->img_loader_ctx, href, tok->text_len, &bytes, &blen) != k_ra8_ok) {
    return false;
  }
  int32_t iw = 0;
  int32_t ih = 0;
  /* mcdc-deactivated: ra8_img_probe_size returns k_ra8_ok only via stbi_info (which rejects 0-pixel headers) or ra8_svg_size (which errors on w/h <= 0), so iw and ih are provably > 0 here; the (iw <= 0) / (ih <= 0) defensive guards are unreachable on any public path. */
  if ((ra8_img_probe_size(bytes, blen, &iw, &ih) != k_ra8_ok) || (iw <= 0) || (ih <= 0)) {
    return false;
  }
  const int32_t col_w   = (int32_t)engine->viewport_w - (2 * (int32_t)k_ra8_reflow_margin_px);
  const int32_t avail_h = (int32_t)engine->viewport_h - (2 * (int32_t)k_ra8_reflow_margin_px);
  if ((col_w < 1) || (avail_h < 1)) {
    return false;
  }
  priv_image_fit(iw, ih, col_w, avail_h, out_w, out_h);
  return true;
}

/**
 * @brief Record a laid-out image box and advance the cursor below it.
 *
 * @details See implementation. Stores the box at the left margin / current
 * baseline tagged with the active page, then drops the cursor past it.
 * @param[in] engine See implementation.
 * @param[in] cur See implementation.
 * @param[in] tok See implementation.
 * @param[in] bw See implementation.
 * @param[in] bh See implementation.
 * @return None.
 * @pre `engine->image_box_count < k_ra8_reflow_max_images`.
 * @pre `bw >= 1` and `bh >= 1`.
 * @post One image box appended; `image_box_count` incremented.
 * @post Cursor advanced below the image, line reset to the left margin.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_image_record(ra8_reflow_t*             engine,
                              priv_cursor_t*            cur,
                              const ra8_reflow_token_t* tok,
                              int32_t                   bw,
                              int32_t                   bh)
{
  ra8_reflow_image_box_t* box = &engine->image_boxes[engine->image_box_count];
  box->x                      = (int32_t)k_ra8_reflow_margin_px;
  box->y                      = cur->y;
  box->w                      = bw;
  box->h                      = bh;
  box->src_off                = tok->text_off;
  box->src_len                = tok->text_len;
  box->page_index             = engine->page_count;
  box->reserved               = 0U;
  engine->image_box_count++;

  cur->y += bh + (int32_t)k_ra8_reflow_paragraph_gap_px;
  cur->line_top         = cur->y;
  cur->x                = (int32_t)k_ra8_reflow_margin_px + (int32_t)cur->indent_px;
  cur->line_has_content = 0U;
}

/**
 * @brief Lay out a real `<img>` as a block: size it, page-break, record it.
 *
 * @details See implementation. Returns false (caller falls back to the
 * placeholder) on a full image pool, unresolved src, or a flush overflow.
 * @param[in] engine See implementation.
 * @param[in] cur See implementation.
 * @param[in] tok See implementation.
 * @return Boolean.
 * @retval true Image placed and recorded.
 * @retval false Could not place; caller uses the placeholder.
 * @pre `engine->img_loader != nullptr` and `engine->img_arena != nullptr`.
 * @pre `tok->text_len > 0`.
 * @post On true, one image box exists and the cursor sits below it.
 * @post On false, engine image state is unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool
priv_place_image(ra8_reflow_t* engine, priv_cursor_t* cur, const ra8_reflow_token_t* tok)
{
  if (engine->image_box_count >= (uint32_t)k_ra8_reflow_max_images) {
    return false;
  }
  int32_t bw = 0;
  int32_t bh = 0;
  if (!priv_image_resolve_size(engine, tok, &bw, &bh)) {
    return false;
  }
  /* Block-level: drop below the current line, then page-break if needed. */
  if (cur->line_has_content != 0U) {
    if (!ra8_reflow_layout_newline(engine, cur, false)) {
      return false;
    }
  }
  const int32_t bottom_limit = (int32_t)engine->viewport_h - (int32_t)k_ra8_reflow_margin_px;
  if (((cur->y + bh) > bottom_limit) && priv_page_has_content(engine, cur)) {
    if (!ra8_reflow_layout_finish_page(engine, cur)) {
      return false;
    }
  }
  priv_image_record(engine, cur, tok, bw, bh);
  /* mcdc-deactivated: priv_image_record incremented image_box_count immediately above, so priv_page_has_content is invariantly true here; its false arm is unreachable once an image box has been recorded on the current page. */
  if (((cur->y + (int32_t)cur->line_height_px) > bottom_limit) &&
      priv_page_has_content(engine, cur)) {
    if (!ra8_reflow_layout_finish_page(engine, cur)) {
      return false;
    }
  }
  return true;
}

bool ra8_reflow_layout_apply_image(ra8_reflow_t*             engine,
                                   priv_cursor_t*            cur,
                                   const ra8_reflow_token_t* tok)
{
  if ((engine->img_loader != nullptr) && (engine->img_arena != nullptr) && (tok->text_len > 0U)) {
    if (priv_place_image(engine, cur, tok)) {
      return true;
    }
    /* Resolution / pool overflow: fall back to the placeholder advance. */
  }
  return priv_apply_image_placeholder(engine, cur);
}
