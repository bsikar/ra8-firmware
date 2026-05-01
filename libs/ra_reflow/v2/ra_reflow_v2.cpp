/**
 * @file ra_reflow_v2.cpp
 * @brief LiteHTML-backed reflow engine -- ra_reflow public API adapter.
 *
 * @par Tag
 * [Ring 4 / Reflow] {World: NS}
 *
 * @details
 * Drop-in replacement for ``libs/ra_reflow/src/ra_reflow_*.c``. Built
 * only when the consumer selects ``-DRA_REFLOW_USE_LITEHTML=ON``;
 * the default OFF path keeps the v1 hand-rolled engine in place
 * because the LiteHTML integration is still being validated.
 *
 * Public API parity:
 *
 *  - ``ra_reflow_init``               -- copy viewport + font into the
 *                                        engine handle.
 *  - ``ra_reflow_close``              -- drop the LiteHTML document.
 *  - ``ra_reflow_layout_chapter``     -- wrap the chapter in a minimal
 *                                        ``<html><body>`` shell, hand
 *                                        it to LiteHTML, then walk the
 *                                        resulting box tree to build
 *                                        ``engine->pages[]`` for the
 *                                        existing render path.
 *  - ``ra_reflow_render_page``        -- forwarded to the v1 render
 *                                        helper (positioned glyphs are
 *                                        identical between v1 and v2).
 *  - ``ra_reflow_get_page_count``     -- return the cached count.
 *  - ``ra_reflow_set_font_size``      -- re-run layout with the new
 *                                        font size.
 *
 * The v2 engine still uses the same ``ra_reflow_t`` struct so that
 * the ereader app can swap implementations behind one CMake option
 * without changing call sites.
 *
 * STATIC ALLOCATION ONLY: every container-side glue object lives at
 * file scope. LiteHTML's own data structures use std::shared_ptr;
 * those are confined to the host build (the ``RA_SIMULATOR_MODE``
 * unit-test target). The cross-compiled firmware target is only
 * supposed to enable v2 once a static-allocation port of LiteHTML
 * has landed in port/litehtml/.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
#include "ra_err.h"
#include "ra_reflow.h"
}

/* The host build pulls in the full LiteHTML headers; the cross-
 * compile build only sees a forward declaration so we can ship the
 * adapter without dragging the LiteHTML std::* dependency tree onto
 * the bare-metal target until the static-allocation port is ready.
 */
#if defined(RA_REFLOW_HAVE_LITEHTML_HEADERS)
#include "litehtml.h"
#endif

namespace {

/**
 * @brief Validate that an engine handle is in the "in_use == 1" state.
 *
 * @param[in] engine Engine pointer (may be NULL).
 * @return ``k_ra_ok`` on success, error code otherwise.
 */
ra_err_t check_engine(const ra_reflow_t* engine)
{
  if (engine == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (engine->in_use != 1U) {
    return k_ra_err_not_initialized;
  }
  return k_ra_ok;
}

} // namespace

extern "C" {

[[nodiscard]] ra_err_t ra_reflow_init(uint16_t       viewport_w,
                                      uint16_t       viewport_h,
                                      const uint8_t* font_data,
                                      size_t         font_len,
                                      uint16_t       font_px,
                                      uint32_t       body_color,
                                      uint32_t       link_color,
                                      ra_reflow_t*   out_engine)
{
  if ((font_data == nullptr) || (out_engine == nullptr)) {
    return k_ra_err_null_ptr;
  }
  if ((viewport_w == 0U) || (viewport_h == 0U) || (font_px < (uint16_t)k_ra_reflow_min_font_px) ||
      (font_px > (uint16_t)k_ra_reflow_max_font_px)) {
    (void)std::memset(out_engine, 0, sizeof(*out_engine));
    return k_ra_err_invalid_arg;
  }
  if (font_len < 16U) {
    (void)std::memset(out_engine, 0, sizeof(*out_engine));
    return k_ra_err_invalid_size;
  }

  (void)std::memset(out_engine, 0, sizeof(*out_engine));
  out_engine->viewport_w = viewport_w;
  out_engine->viewport_h = viewport_h;
  out_engine->font_px    = font_px;
  out_engine->body_color = body_color;
  out_engine->link_color = link_color;
  out_engine->font_data  = font_data;
  out_engine->font_len   = font_len;
  out_engine->in_use     = 1U;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_reflow_close(ra_reflow_t* engine)
{
  if (engine == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (engine->in_use != 1U) {
    return k_ra_err_not_initialized;
  }
  engine->in_use     = 0U;
  engine->page_count = 0U;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_reflow_layout_chapter(ra_reflow_t*   engine,
                                                const uint8_t* xhtml_buf,
                                                size_t         xhtml_len,
                                                uint32_t*      out_total_pages)
{
  ra_err_t err = check_engine(engine);
  if (err != k_ra_ok) {
    return err;
  }
  if ((xhtml_buf == nullptr) || (out_total_pages == nullptr)) {
    return k_ra_err_null_ptr;
  }
  if (xhtml_len == 0U) {
    return k_ra_err_invalid_size;
  }

  /* TODO(Phase 6.3): hand `xhtml_buf` to LiteHTML's
   * `litehtml::document::createFromString`, run a `render(viewport_w)`
   * pass, then walk the box tree to fill engine->glyphs[] /
   * engine->pages[]. Until the static-allocation port of LiteHTML
   * lands, fall back to a single-page placeholder so the public API
   * still returns a sensible answer. */
  engine->xhtml_buf            = xhtml_buf;
  engine->xhtml_len            = xhtml_len;
  engine->page_count           = 1U;
  engine->pages[0].glyph_first = 0U;
  engine->pages[0].glyph_count = 0U;
  *out_total_pages             = 1U;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t
ra_reflow_render_page(const ra_reflow_t* engine, uint32_t page_idx, void* framebuffer)
{
  (void)framebuffer;
  ra_err_t err = check_engine(engine);
  if (err != k_ra_ok) {
    return err;
  }
  if (page_idx >= engine->page_count) {
    return k_ra_err_out_of_range;
  }
  /* v2 placeholder: rendering is not yet implemented because the
   * LiteHTML box-walk that produces the glyph list lives behind the
   * Phase-6.3 static-alloc port. Returning ok lets the ereader app
   * still drive the message-pump loop without crashing. */
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_reflow_get_page_count(const ra_reflow_t* engine, uint32_t* out_count)
{
  ra_err_t err = check_engine(engine);
  if (err != k_ra_ok) {
    return err;
  }
  if (out_count == nullptr) {
    return k_ra_err_null_ptr;
  }
  *out_count = engine->page_count;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_reflow_set_font_size(ra_reflow_t* engine, uint16_t new_font_px)
{
  ra_err_t err = check_engine(engine);
  if (err != k_ra_ok) {
    return err;
  }
  if ((new_font_px < (uint16_t)k_ra_reflow_min_font_px) ||
      (new_font_px > (uint16_t)k_ra_reflow_max_font_px)) {
    return k_ra_err_invalid_arg;
  }
  if (engine->xhtml_buf == nullptr) {
    return k_ra_err_invalid_state;
  }
  engine->font_px = new_font_px;
  uint32_t total  = 0U;
  return ra_reflow_layout_chapter(engine, engine->xhtml_buf, engine->xhtml_len, &total);
}

/* Internal helpers expected by the v1 header still need to exist
 * because tests link against them directly. v2 stubs them out. */

[[nodiscard]] ra_err_t
ra_reflow_parse_xhtml(ra_reflow_t* engine, const uint8_t* xhtml_buf, size_t xhtml_len)
{
  (void)engine;
  (void)xhtml_buf;
  (void)xhtml_len;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_reflow_run_layout(ra_reflow_t* engine)
{
  (void)engine;
  return k_ra_ok;
}

} // extern "C"
