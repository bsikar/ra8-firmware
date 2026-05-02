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
 * the default OFF path keeps the v1 hand-rolled engine in place.
 *
 * Hands the chapter buffer to ``litehtml::document::createFromString``,
 * runs ``render(viewport_w)``, then paginates by viewport height.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

extern "C" {
#include "ra_err.h"
#include "ra_reflow.h"
}

#include "litehtml.h"

namespace {

typedef enum : uint16_t {
  k_v2_glyph_w_px         = 8U,
  k_v2_image_default_w_px = 64U,
  k_v2_image_default_h_px = 64U,
  k_v2_pt_to_px_num       = 4U,
  k_v2_pt_to_px_den       = 3U,
} v2_metrics_t;

class v2_container : public litehtml::document_container {
public:
  v2_container(int viewport_w, int viewport_h, int font_px)
      : viewport_w_(viewport_w), viewport_h_(viewport_h), font_px_(font_px) {}

  litehtml::uint_ptr create_font(const litehtml::font_description&,
                                 const litehtml::document*,
                                 litehtml::font_metrics* fm) override {
    if (fm != nullptr) {
      fm->font_size   = font_px_;
      fm->height      = font_px_;
      fm->ascent      = (font_px_ * 4) / 5;
      fm->descent     = font_px_ / 5;
      fm->x_height    = font_px_ / 2;
      fm->ch_width    = static_cast<int>(k_v2_glyph_w_px);
      fm->draw_spaces = true;
    }
    return 1U;
  }
  void delete_font(litehtml::uint_ptr) override {}
  litehtml::pixel_t text_width(const char* t, litehtml::uint_ptr) override {
    return t ? static_cast<litehtml::pixel_t>(std::strlen(t)) *
                 static_cast<litehtml::pixel_t>(k_v2_glyph_w_px) : 0;
  }
  void draw_text(litehtml::uint_ptr, const char*, litehtml::uint_ptr,
                 litehtml::web_color, const litehtml::position&) override {}
  litehtml::pixel_t pt_to_px(float pt) const override {
    return static_cast<litehtml::pixel_t>(
        (pt * static_cast<float>(k_v2_pt_to_px_num)) /
        static_cast<float>(k_v2_pt_to_px_den));
  }
  litehtml::pixel_t get_default_font_size() const override { return font_px_; }
  const char* get_default_font_name() const override { return "sans-serif"; }
  void draw_list_marker(litehtml::uint_ptr, const litehtml::list_marker&) override {}
  void load_image(const char*, const char*, bool) override {}
  void get_image_size(const char*, const char*, litehtml::size& sz) override {
    sz.width  = static_cast<int>(k_v2_image_default_w_px);
    sz.height = static_cast<int>(k_v2_image_default_h_px);
  }
  void draw_image(litehtml::uint_ptr, const litehtml::background_layer&,
                  const std::string&, const std::string&) override {}
  void draw_solid_fill(litehtml::uint_ptr, const litehtml::background_layer&,
                       const litehtml::web_color&) override {}
  void draw_linear_gradient(litehtml::uint_ptr, const litehtml::background_layer&,
                            const litehtml::background_layer::linear_gradient&) override {}
  void draw_radial_gradient(litehtml::uint_ptr, const litehtml::background_layer&,
                            const litehtml::background_layer::radial_gradient&) override {}
  void draw_conic_gradient(litehtml::uint_ptr, const litehtml::background_layer&,
                           const litehtml::background_layer::conic_gradient&) override {}
  void draw_borders(litehtml::uint_ptr, const litehtml::borders&,
                    const litehtml::position&, bool) override {}
  void set_caption(const char*) override {}
  void set_base_url(const char*) override {}
  void link(const std::shared_ptr<litehtml::document>&,
            const litehtml::element::ptr&) override {}
  void on_anchor_click(const char*, const litehtml::element::ptr&) override {}
  void on_mouse_event(const litehtml::element::ptr&, litehtml::mouse_event) override {}
  void set_cursor(const char*) override {}
  void transform_text(litehtml::string&, litehtml::text_transform) override {}
  void import_css(litehtml::string& t, const litehtml::string&,
                  litehtml::string&) override { t.clear(); }
  void set_clip(const litehtml::position&, const litehtml::border_radiuses&) override {}
  void del_clip() override {}
  void get_viewport(litehtml::position& v) const override {
    v.x = 0; v.y = 0; v.width = viewport_w_; v.height = viewport_h_;
  }
  litehtml::element::ptr create_element(const char*, const litehtml::string_map&,
                                        const std::shared_ptr<litehtml::document>&) override {
    return nullptr;
  }
  void get_media_features(litehtml::media_features& m) const override {
    std::memset(&m, 0, sizeof(m));
    m.type           = litehtml::media_type_screen;
    m.width          = viewport_w_;
    m.height         = viewport_h_;
    m.device_width   = viewport_w_;
    m.device_height  = viewport_h_;
    m.color          = 8;
    m.resolution     = 96;
  }
  void get_language(litehtml::string& l, litehtml::string& c) const override {
    l = "en"; c = "";
  }
private:
  int viewport_w_;
  int viewport_h_;
  int font_px_;
};

struct v2_state {
  /* Field order matters for destruction: the document holds a raw
   * pointer to the container (passed via container.get() into
   * createFromString), so the document must be destroyed first.
   * In C++ struct destruction runs in reverse declaration order,
   * so list the container last. */
  litehtml::document::ptr       document;
  std::unique_ptr<v2_container> container;
};

/* Explicit reset helper: clear the document before the container so
 * raw-pointer back-references inside the document are not dangling
 * during its destructor. Default-assignment of the whole struct does
 * NOT guarantee this order on all libstdc++ versions, so we do it by
 * hand. */
inline void v2_state_clear(v2_state& s) {
  s.document.reset();
  s.container.reset();
}

typedef enum : uint8_t { k_v2_max_engines = 4U } v2_cache_caps_t;

struct v2_cache_slot {
  const ra_reflow_t* engine = nullptr;
  v2_state           state;
};

static v2_cache_slot s_v2_cache[k_v2_max_engines];

v2_state* v2_state_for(const ra_reflow_t* engine, bool create) {
  for (uint8_t i = 0; i < (uint8_t)k_v2_max_engines; ++i) {
    if (s_v2_cache[i].engine == engine) return &s_v2_cache[i].state;
  }
  if (!create) return nullptr;
  for (uint8_t i = 0; i < (uint8_t)k_v2_max_engines; ++i) {
    if (s_v2_cache[i].engine == nullptr) {
      s_v2_cache[i].engine = engine;
      v2_state_clear(s_v2_cache[i].state);
      return &s_v2_cache[i].state;
    }
  }
  return nullptr;
}

void v2_state_drop(const ra_reflow_t* engine) {
  for (uint8_t i = 0; i < (uint8_t)k_v2_max_engines; ++i) {
    if (s_v2_cache[i].engine == engine) {
      v2_state_clear(s_v2_cache[i].state);
      s_v2_cache[i].engine = nullptr;
      return;
    }
  }
}

ra_err_t check_engine(const ra_reflow_t* engine) {
  if (engine == nullptr) return k_ra_err_null_ptr;
  if (engine->in_use != 1U) return k_ra_err_not_initialized;
  return k_ra_ok;
}

ra_err_t v2_run_layout(ra_reflow_t* engine, uint32_t* out_total_pages) {
  v2_state* state = v2_state_for(engine, true);
  if (state == nullptr) return k_ra_err_no_mem;

  /* Reset in document-first order before allocating new ones so a
   * stale document holding a raw container pointer is destroyed
   * before its target. */
  state->document.reset();
  state->container.reset();

  state->container = std::make_unique<v2_container>(
      static_cast<int>(engine->viewport_w),
      static_cast<int>(engine->viewport_h),
      static_cast<int>(engine->font_px));

  std::string body(reinterpret_cast<const char*>(engine->xhtml_buf),
                   engine->xhtml_len);
  std::string html;
  html.reserve(body.size() + 64U);
  html.append("<!DOCTYPE html><html><head></head><body>");
  html.append(body);
  html.append("</body></html>");

  litehtml::estring src(html);
  state->document = litehtml::document::createFromString(src, state->container.get());
  if (!state->document) return k_ra_err_validation_failed;

  (void)state->document->render(static_cast<litehtml::pixel_t>(engine->viewport_w));
  int h = static_cast<int>(state->document->height());
  if (h < 0) h = 0;

  int vh    = static_cast<int>(engine->viewport_h);
  int pages = (h + vh - 1) / vh;
  if (pages < 1) pages = 1;
  if (pages > static_cast<int>(k_ra_reflow_max_pages))
    pages = static_cast<int>(k_ra_reflow_max_pages);

  engine->page_count = static_cast<uint32_t>(pages);
  for (int i = 0; i < pages; ++i) {
    engine->pages[i].glyph_first = 0U;
    engine->pages[i].glyph_count = 0U;
  }
  if (out_total_pages != nullptr) *out_total_pages = engine->page_count;
  return k_ra_ok;
}

} // namespace

extern "C" {

[[nodiscard]] ra_err_t ra_reflow_init(uint16_t viewport_w, uint16_t viewport_h,
                                      const uint8_t* font_data, size_t font_len,
                                      uint16_t font_px, uint32_t body_color,
                                      uint32_t link_color,
                                      ra_reflow_t* out_engine) {
  if ((font_data == nullptr) || (out_engine == nullptr)) return k_ra_err_null_ptr;
  if ((viewport_w == 0U) || (viewport_h == 0U) ||
      (font_px < (uint16_t)k_ra_reflow_min_font_px) ||
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

[[nodiscard]] ra_err_t ra_reflow_close(ra_reflow_t* engine) {
  if (engine == nullptr) return k_ra_err_null_ptr;
  if (engine->in_use != 1U) return k_ra_err_not_initialized;
  v2_state_drop(engine);
  engine->in_use     = 0U;
  engine->page_count = 0U;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_reflow_layout_chapter(ra_reflow_t* engine,
                                                const uint8_t* xhtml_buf,
                                                size_t xhtml_len,
                                                uint32_t* out_total_pages) {
  ra_err_t err = check_engine(engine);
  if (err != k_ra_ok) return err;
  if ((xhtml_buf == nullptr) || (out_total_pages == nullptr)) return k_ra_err_null_ptr;
  if (xhtml_len == 0U) return k_ra_err_invalid_size;
  engine->xhtml_buf = xhtml_buf;
  engine->xhtml_len = xhtml_len;
  return v2_run_layout(engine, out_total_pages);
}

[[nodiscard]] ra_err_t ra_reflow_render_page(const ra_reflow_t* engine,
                                             uint32_t page_idx,
                                             void* framebuffer) {
  (void)framebuffer;
  ra_err_t err = check_engine(engine);
  if (err != k_ra_ok) return err;
  if (page_idx >= engine->page_count) return k_ra_err_out_of_range;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_reflow_get_page_count(const ra_reflow_t* engine,
                                                uint32_t* out_count) {
  ra_err_t err = check_engine(engine);
  if (err != k_ra_ok) return err;
  if (out_count == nullptr) return k_ra_err_null_ptr;
  *out_count = engine->page_count;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_reflow_set_font_size(ra_reflow_t* engine,
                                               uint16_t new_font_px) {
  ra_err_t err = check_engine(engine);
  if (err != k_ra_ok) return err;
  if ((new_font_px < (uint16_t)k_ra_reflow_min_font_px) ||
      (new_font_px > (uint16_t)k_ra_reflow_max_font_px)) {
    return k_ra_err_invalid_arg;
  }
  if (engine->xhtml_buf == nullptr) return k_ra_err_invalid_state;
  engine->font_px = new_font_px;
  uint32_t total  = 0U;
  return v2_run_layout(engine, &total);
}

[[nodiscard]] ra_err_t ra_reflow_parse_xhtml(ra_reflow_t* engine,
                                             const uint8_t* xhtml_buf,
                                             size_t xhtml_len) {
  if (engine == nullptr) return k_ra_err_null_ptr;
  engine->xhtml_buf = xhtml_buf;
  engine->xhtml_len = xhtml_len;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_reflow_run_layout(ra_reflow_t* engine) {
  if (engine == nullptr) return k_ra_err_null_ptr;
  uint32_t total = 0U;
  return v2_run_layout(engine, &total);
}

} // extern "C"
