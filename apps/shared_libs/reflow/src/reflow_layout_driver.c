/**
 * @file reflow_layout_driver.c
 * @brief Token-stream driver + public lifecycle API for the reflow engine.
 *
 * @details
 * Splits the outer layout driver and the public entry points out of
 * `reflow_layout.c` so each translation unit stays under the project
 * file-size cap. The driver (`internal_layout_tokens`) walks the parsed token
 * stream once, dispatching `<table>` ranges to @ref priv_reflow_layout_table
 * and every other token to @ref priv_reflow_layout_apply_token, then flushes a
 * final page and builds the tappable link rectangles.
 *
 * The public API covers engine lifecycle (`reflow_init`,
 * `reflow_close`), loader binding (image / CSS), chapter dispatch
 * (`reflow_layout_chapter`, `reflow_set_font_size`), font (re)binding,
 * and face registration.
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
#include "ra8_stbtt_guard.h"
#include "reflow.h"
#include "reflow_internal.h"
#include "reflow_layout_internal.h"
#include "stb_truetype.h"

/**
 * @brief Run one pass over the token stream populating `engine->glyphs[]`
 *        and `engine->pages[]`.
 *
 * @details See implementation.
 * @param[in] engine See implementation.
 * @param[in] font See implementation.
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
static ra8_err_t internal_layout_tokens(reflow_t* engine, const stbtt_fontinfo* font)
{
  priv_cursor_t cur = {
    .x                = (int32_t)k_reflow_margin_px,
    .y                = (int32_t)k_reflow_margin_px,
    .line_top         = (int32_t)k_reflow_margin_px,
    .line_height_px   = priv_reflow_layout_line_height(engine->font_px),
    .active_font_px   = engine->font_px,
    .active_style     = k_reflow_style_normal,
    .indent_px        = 0U,
    .page_first_glyph = 0U,
    .page_first_image = 0U,
    .line_first_glyph = 0U,
    .line_has_content = 0U,
    .align            = (uint8_t)k_reflow_align_left,
    .reserved8        = {0U, 0U},
  };

  uint32_t i = 0U;
  while (i < engine->token_count) {
    const reflow_token_t* tok = &engine->tokens[i];
    if ((tok->kind == (uint8_t)k_reflow_tok_block_start) &&
        (tok->tag == (uint8_t)k_reflow_tag_table)) {
      ra8_err_t err = priv_reflow_layout_table(engine, &cur, font, i, &i);
      if (err != k_ra8_ok) {
        return err;
      }
    } else {
      ra8_err_t err = priv_reflow_layout_apply_token(engine, &cur, font, tok);
      if (err != k_ra8_ok) {
        return err;
      }
      i++;
    }
  }

  /* Flush the final page if it has glyph or image content. */
  if ((engine->glyph_count > cur.page_first_glyph) ||
      (engine->image_box_count > cur.page_first_image)) {
    if (!priv_reflow_layout_finish_page(engine, &cur)) {
      return k_ra8_err_no_mem;
    }
  }
  return k_ra8_ok;
}

/* ===========================================================================
 * Public helpers (declared in reflow.h)
 * ===========================================================================
 */

ra8_err_t reflow_run_layout(reflow_t* engine)
{
  if (engine == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra8_err_not_initialized;
  }

  /* Wipe glyph, page, image, link and anchor state but keep the token stream. */
  engine->glyph_count     = 0U;
  engine->page_count      = 0U;
  engine->image_box_count = 0U;
  engine->link_rect_count = 0U;
  engine->anchor_count    = 0U;

  stbtt_fontinfo font;
  ra8_err_t      err = priv_reflow_layout_init_font(engine, &font);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_layout_tokens(engine, &font);
  if (err != k_ra8_ok) {
    return err;
  }
  /* Guarantee at least one page on non-empty input. */
  if (priv_reflow_internal_final_page_needed(engine->page_count, engine->token_count)) {
    if (engine->page_count >= k_reflow_max_pages) {
      return k_ra8_err_no_mem;
    }
    engine->pages[0].glyph_first = 0U;
    engine->pages[0].glyph_count = engine->glyph_count;
    engine->page_count           = (uint32_t)k_priv_min_chapter_pages;
  }
  /* Build tappable link rectangles from the link-tagged glyphs (#110). */
  priv_reflow_layout_build_link_rects(engine, &font);
  return k_ra8_ok;
}

/* ===========================================================================
 * Public API entry points -- lifecycle + chapter dispatch
 * ===========================================================================
 */

ra8_err_t reflow_init(uint16_t       viewport_w,
                      uint16_t       viewport_h,
                      const uint8_t* font_data,
                      size_t         font_len,
                      uint16_t       font_px,
                      uint32_t       body_color,
                      uint32_t       link_color,
                      reflow_t*      out_engine)
{
  if ((font_data == nullptr) || (out_engine == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((viewport_w == 0U) || (viewport_h == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if ((font_px < k_reflow_min_font_px) || (font_px > k_reflow_max_font_px)) {
    return k_ra8_err_invalid_arg;
  }
  if (font_len < 16U) {
    return k_ra8_err_invalid_size;
  }

  priv_reflow_layout_byte_zero((uint8_t*)out_engine, sizeof(*out_engine));

  out_engine->viewport_w = viewport_w;
  out_engine->viewport_h = viewport_h;
  out_engine->font_data  = font_data;
  out_engine->font_len   = font_len;
  out_engine->font_px    = font_px;
  out_engine->body_color = body_color;
  out_engine->link_color = link_color;
  out_engine->in_use     = 1U;
  return k_ra8_ok;
}

ra8_err_t reflow_close(reflow_t* engine)
{
  if (engine == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra8_err_not_initialized;
  }
  engine->in_use          = 0U;
  engine->page_count      = 0U;
  engine->glyph_count     = 0U;
  engine->token_count     = 0U;
  engine->text_pool_used  = 0U;
  engine->image_box_count = 0U;
  engine->xhtml_buf       = nullptr;
  engine->xhtml_len       = 0U;
  return k_ra8_ok;
}

ra8_err_t reflow_set_image_loader(reflow_t*              engine,
                                  reflow_image_loader_fn loader,
                                  void*                  ctx,
                                  ra8_img_arena_t*       arena)
{
  if (engine == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra8_err_not_initialized;
  }
  engine->img_loader     = loader;
  engine->img_loader_ctx = ctx;
  engine->img_arena      = arena;
  return k_ra8_ok;
}

ra8_err_t reflow_set_css_loader(reflow_t* engine, reflow_css_loader_fn loader, void* ctx)
{
  if (engine == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra8_err_not_initialized;
  }
  engine->css_loader     = loader;
  engine->css_loader_ctx = ctx;
  return k_ra8_ok;
}

ra8_err_t reflow_layout_chapter(reflow_t*      engine,
                                const uint8_t* xhtml_buf,
                                size_t         xhtml_len,
                                uint32_t*      out_total_pages)
{
  if ((engine == nullptr) || (xhtml_buf == nullptr) || (out_total_pages == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra8_err_not_initialized;
  }
  if (xhtml_len == 0U) {
    return k_ra8_err_invalid_size;
  }

  *out_total_pages = 0U;

  ra8_err_t err = reflow_parse_xhtml(engine, xhtml_buf, xhtml_len);
  if (err != k_ra8_ok) {
    return err;
  }
  err = reflow_run_layout(engine);
  if (err != k_ra8_ok) {
    return err;
  }

  /* Cache the pointer so set_font_size() can re-flow without the
   * caller resupplying the buffer. */
  engine->xhtml_buf = xhtml_buf;
  engine->xhtml_len = xhtml_len;
  *out_total_pages  = engine->page_count;
  return k_ra8_ok;
}

ra8_err_t reflow_get_page_count(const reflow_t* engine, uint32_t* out_count)
{
  if ((engine == nullptr) || (out_count == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra8_err_not_initialized;
  }
  *out_count = engine->page_count;
  return k_ra8_ok;
}

ra8_err_t reflow_set_font_size(reflow_t* engine, uint16_t new_font_px)
{
  if (engine == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra8_err_not_initialized;
  }
  if ((new_font_px < k_reflow_min_font_px) || (new_font_px > k_reflow_max_font_px)) {
    return k_ra8_err_invalid_arg;
  }
  if (priv_reflow_internal_xhtml_invalid(engine->xhtml_buf, engine->xhtml_len)) {
    return k_ra8_err_invalid_state;
  }
  engine->font_px = new_font_px;

  /* Re-flow against the cached buffer. */
  uint32_t pages = 0U;
  return reflow_layout_chapter(engine, engine->xhtml_buf, engine->xhtml_len, &pages);
}

/** @brief Implementation of `reflow_bind_font()` -- validate then swap. */
ra8_err_t reflow_bind_font(reflow_t* engine, const uint8_t* font_data, size_t font_len)
{
  if ((engine == nullptr) || (font_data == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra8_err_not_initialized;
  }
  if (font_len < (size_t)k_reflow_min_font_bytes) {
    return k_ra8_err_invalid_size;
  }

  /* Validate the blob BEFORE swapping; on failure the current face is kept
   * (graceful degradation). The probe fontinfo is a stack temporary -- the
   * layout / render paths re-init their own per-call fontinfo from font_data. */
  const int32_t offset = stbtt_GetFontOffsetForIndex(font_data, 0);
  /* Bound-check the sfnt table directory before stbtt_InitFont walks it: the
   * @font-face bytes are attacker-controlled and stb_truetype reads the
   * directory with no length check (#217). A non-sfnt blob (offset < 0) is
   * left to the existing decision below. */
  if (offset >= 0) {
    if (!ra8_stbtt_sfnt_dir_in_bounds(font_data, font_len, (uint32_t)offset)) {
      return k_ra8_err_not_supported;
    }
  }
  stbtt_fontinfo probe;
  if ((offset < 0) || (stbtt_InitFont(&probe, font_data, (int)font_len, offset) == 0)) {
    return k_ra8_err_not_supported;
  }

  engine->font_data = font_data;
  engine->font_len  = font_len;

  /* If a chapter is already laid out, re-flow it against the new face so the
   * change is immediate (mirrors reflow_set_font_size); otherwise the next
   * layout picks it up. */
  if (!priv_reflow_internal_xhtml_invalid(engine->xhtml_buf, engine->xhtml_len)) {
    uint32_t pages = 0U;
    return reflow_layout_chapter(engine, engine->xhtml_buf, engine->xhtml_len, &pages);
  }
  return k_ra8_ok;
}

/** @brief Implementation of `reflow_register_face()` -- validate then append. */
ra8_err_t
reflow_register_face(reflow_t* engine, uint8_t css_face_idx, const uint8_t* blob, size_t len)
{
  if ((engine == nullptr) || (blob == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (engine->in_use == 0U) {
    return k_ra8_err_not_initialized;
  }
  if (len < (size_t)k_reflow_min_font_bytes) {
    return k_ra8_err_invalid_size;
  }
  if (engine->face_count >= (uint8_t)k_reflow_max_faces) {
    return k_ra8_err_no_mem;
  }
  /* Validate the blob before recording it; a bad face is rejected so layout falls
   * back to the default face (graceful degradation). The probe is a stack
   * temporary -- the render pass re-inits its own per-face fontinfo. */
  const int32_t offset = stbtt_GetFontOffsetForIndex(blob, 0);
  /* Bound-check the sfnt table directory before stbtt_InitFont walks it: the
   * @font-face bytes are attacker-controlled and stb_truetype reads the
   * directory with no length check (#217). A non-sfnt blob (offset < 0) is
   * left to the existing decision below. */
  if (offset >= 0) {
    if (!ra8_stbtt_sfnt_dir_in_bounds(blob, len, (uint32_t)offset)) {
      return k_ra8_err_not_supported;
    }
  }
  stbtt_fontinfo probe;
  if ((offset < 0) || (stbtt_InitFont(&probe, blob, (int)len, offset) == 0)) {
    return k_ra8_err_not_supported;
  }
  reflow_face_t* face = &engine->faces[engine->face_count];
  face->blob          = blob;
  face->len           = len;
  face->css_face_idx  = css_face_idx;
  engine->face_count++;
  return k_ra8_ok;
}
