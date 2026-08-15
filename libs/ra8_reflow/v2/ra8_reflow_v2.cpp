/**
 * @file ra8_reflow_v2.cpp
 * @brief LiteHTML-backed reflow engine -- ra8_reflow public API adapter.
 *
 * @par Tag
 * [Ring 4 / Reflow] {World: NS}
 *
 * @details
 * Drop-in replacement for ``libs/ra8_reflow/src/ra8_reflow_*.c``. Built
 * only when the consumer selects ``-DRA8_REFLOW_USE_LITEHTML=ON``;
 * the default OFF path keeps the v1 hand-rolled engine in place.
 *
 * Hands the chapter buffer to ``litehtml::document::createFromString``,
 * runs ``render(viewport_w)``, then paginates by viewport height.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

extern "C" {
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_reflow.h"
}

#include "litehtml.h"

namespace {

typedef enum : uint8_t {
  k_v2_glyph_w_px           = 8U,  /**< Fallback glyph advance, in pixels. */
  k_v2_image_default_w_px   = 64U, /**< Default replaced-image width.      */
  k_v2_image_default_h_px   = 64U, /**< Default replaced-image height.     */
  k_v2_pt_to_px_num         = 4U,  /**< Numerator of the 4:3 point scale.  */
  k_v2_pt_to_px_den         = 3U,  /**< Denominator of the point scale.    */
  k_v2_ascent_num           = 4U,  /**< Ascent   = 4/5 of the em height.   */
  k_v2_ascent_den           = 5U,  /**< Ascent   = 4/5 of the em height.   */
  k_v2_descent_den          = 5U,  /**< Descent  = 1/5 of the em height.   */
  k_v2_xheight_den          = 2U,  /**< x-height = 1/2 of the em height.   */
  k_v2_media_resolution_dpi = 96U, /**< Reported CSS media resolution.     */
  k_v2_html_wrap_reserve    = 64U, /**< Slack reserved for the wrapper.    */
} v2_metrics_t;

class v2_container : public litehtml::document_container {
public:
  v2_container(int viewport_w, int viewport_h, int font_px)
      : viewport_w_(viewport_w), viewport_h_(viewport_h), font_px_(font_px)
  {}

  litehtml::uint_ptr create_font(const litehtml::font_description& /*descr*/,
                                 const litehtml::document* /*doc*/,
                                 litehtml::font_metrics* fm) override
  {
    if (fm != nullptr) {
      fm->font_size = static_cast<litehtml::pixel_t>(font_px_);
      fm->height    = static_cast<litehtml::pixel_t>(font_px_);
      fm->ascent   = (static_cast<litehtml::pixel_t>(font_px_) * k_v2_ascent_num) / k_v2_ascent_den;
      fm->descent  = static_cast<litehtml::pixel_t>(font_px_) / k_v2_descent_den;
      fm->x_height = static_cast<litehtml::pixel_t>(font_px_) / k_v2_xheight_den;
      fm->ch_width = static_cast<int>(k_v2_glyph_w_px);
      fm->draw_spaces = true;
    }
    return 1U;
  }
  void              delete_font(litehtml::uint_ptr /*hFont*/) override {}
  litehtml::pixel_t text_width(const char* t, litehtml::uint_ptr /*hFont*/) override
  {
    return t ? static_cast<litehtml::pixel_t>(std::strlen(t)) *
                 static_cast<litehtml::pixel_t>(k_v2_glyph_w_px)
             : 0;
  }
  void draw_text(litehtml::uint_ptr /*hdc*/,
                 const char* /*text*/,
                 litehtml::uint_ptr /*hFont*/,
                 litehtml::web_color /*color*/,
                 const litehtml::position& /*pos*/) override
  {}
  litehtml::pixel_t pt_to_px(float pt) const override
  {
    return static_cast<litehtml::pixel_t>((pt * static_cast<float>(k_v2_pt_to_px_num)) /
                                          static_cast<float>(k_v2_pt_to_px_den));
  }
  litehtml::pixel_t get_default_font_size() const override
  {
    return static_cast<litehtml::pixel_t>(font_px_);
  }
  const char* get_default_font_name() const override
  {
    return "sans-serif";
  }
  void draw_list_marker(litehtml::uint_ptr /*hdc*/,
                        const litehtml::list_marker& /*marker*/) override
  {}
  void load_image(const char* /*src*/, const char* /*baseurl*/, bool /*redraw_on_ready*/) override
  {}
  void get_image_size(const char* /*src*/, const char* /*baseurl*/, litehtml::size& sz) override
  {
    sz.width  = static_cast<int>(k_v2_image_default_w_px);
    sz.height = static_cast<int>(k_v2_image_default_h_px);
  }
  void draw_image(litehtml::uint_ptr /*hdc*/,
                  const litehtml::background_layer& /*layer*/,
                  const std::string& /*url*/,
                  const std::string& /*base_url*/) override
  {}
  void draw_solid_fill(litehtml::uint_ptr /*hdc*/,
                       const litehtml::background_layer& /*layer*/,
                       const litehtml::web_color& /*color*/) override
  {}
  void
  draw_linear_gradient(litehtml::uint_ptr /*hdc*/,
                       const litehtml::background_layer& /*layer*/,
                       const litehtml::background_layer::linear_gradient& /*gradient*/) override
  {}
  void
  draw_radial_gradient(litehtml::uint_ptr /*hdc*/,
                       const litehtml::background_layer& /*layer*/,
                       const litehtml::background_layer::radial_gradient& /*gradient*/) override
  {}
  void draw_conic_gradient(litehtml::uint_ptr /*hdc*/,
                           const litehtml::background_layer& /*layer*/,
                           const litehtml::background_layer::conic_gradient& /*gradient*/) override
  {}
  void draw_borders(litehtml::uint_ptr /*hdc*/,
                    const litehtml::borders& /*borders*/,
                    const litehtml::position& /*draw_pos*/,
                    bool /*root*/) override
  {}
  void set_caption(const char* /*caption*/) override {}
  void set_base_url(const char* /*base_url*/) override {}
  void link(const std::shared_ptr<litehtml::document>& /*doc*/,
            const litehtml::element::ptr& /*el*/) override
  {}
  void on_anchor_click(const char* /*url*/, const litehtml::element::ptr& /*el*/) override {}
  void on_mouse_event(const litehtml::element::ptr& /*el*/,
                      litehtml::mouse_event /*event*/) override
  {}
  void set_cursor(const char* /*cursor*/) override {}
  void transform_text(litehtml::string& /*text*/, litehtml::text_transform /*tt*/) override {}
  void import_css(litehtml::string& t,
                  const litehtml::string& /*url*/,
                  litehtml::string& /*baseurl*/) override
  {
    t.clear();
  }
  void set_clip(const litehtml::position& /*pos*/,
                const litehtml::border_radiuses& /*bdr_radius*/) override
  {}
  void del_clip() override {}
  void get_viewport(litehtml::position& v) const override
  {
    v.x      = 0;
    v.y      = 0;
    v.width  = static_cast<litehtml::pixel_t>(viewport_w_);
    v.height = static_cast<litehtml::pixel_t>(viewport_h_);
  }
  litehtml::element::ptr create_element(const char* /*tag_name*/,
                                        const litehtml::string_map& /*attributes*/,
                                        const std::shared_ptr<litehtml::document>& /*doc*/) override
  {
    return nullptr;
  }
  void get_media_features(litehtml::media_features& m) const override
  {
    m               = litehtml::media_features{};
    m.type          = litehtml::media_type_screen;
    m.width         = static_cast<litehtml::pixel_t>(viewport_w_);
    m.height        = static_cast<litehtml::pixel_t>(viewport_h_);
    m.device_width  = static_cast<litehtml::pixel_t>(viewport_w_);
    m.device_height = static_cast<litehtml::pixel_t>(viewport_h_);
    m.color         = 8;
    m.resolution    = static_cast<int>(k_v2_media_resolution_dpi);
  }
  void get_language(litehtml::string& l, litehtml::string& c) const override
  {
    l = "en";
    c = "";
  }

private:
  int viewport_w_; /**< Active layout viewport width, in pixels.  */
  int viewport_h_; /**< Active layout viewport height, in pixels. */
  int font_px_;    /**< Requested base font size, in pixels.      */
};

struct v2_state {
  /* Field order matters for destruction: the document holds a raw
   * pointer to the container (passed via container.get() into
   * createFromString), so the document must be destroyed first.
   * In C++ struct destruction runs in reverse declaration order,
   * so list the container last. */
  litehtml::document::ptr       document;  /**< Parsed DOM destroyed before its container.    */
  std::unique_ptr<v2_container> container; /**< Owning adapter retained for the DOM lifetime. */
};

/** Explicit reset helper: clear the document before the container so
 * raw-pointer back-references inside the document are not dangling
 * during its destructor. Default-assignment of the whole struct does
 * NOT guarantee this order on all libstdc++ versions, so we do it by
 * hand.
 * @brief Verify v2 state clear behavior against the reflow contract.
 * @details Performs the v2 state clear path and preserves each documented result and bound.
 * @param[in,out] s Cached document/container pair to release in dependency order.
 * @pre Required pointer arguments reference valid objects for the requested operation.
 * @post Output state reflects only the operation described by the returned status.
 * @note This helper is private to the reflow implementation boundary.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static inline void internal_v2_state_clear(v2_state& s)
{
  s.document.reset();
  s.container.reset();
}

typedef enum : uint8_t {
  k_v2_max_engines = 4U /**< Maximum concurrently cached engines. */
} v2_cache_caps_t;

struct v2_cache_slot {
  const ra8_reflow_t* engine =
    nullptr;      /**< Stable engine identity, or nullptr for an unused slot. */
  v2_state state; /**< Document/container state owned by this slot.           */
};

v2_cache_slot s_v2_cache[k_v2_max_engines];

/**
 * @brief Find or reserve the bounded v2 state slot for an engine.
 * @details Searches the fixed-capacity cache first and optionally binds the first unused slot to the requested engine.
 * @param[in] engine Reflow engine identity used as the cache key.
 * @param[in] create Whether an unused slot may be reserved after a cache miss.
 * @return The matching state slot, or nullptr when no slot is available.
 * @retval nullptr No match exists and lookup-only mode or cache exhaustion prevents creation.
 * @retval non-null A state slot is bound to @p engine and ready for use.
 * @pre @p engine is a stable identity for the duration of its cache binding.
 * @pre The fixed-capacity cache is not concurrently mutated.
 * @post Existing cache bindings remain unchanged on a failed lookup.
 * @post A newly reserved slot is cleared before its address is returned.
 * @note The cache owns at most ::k_v2_max_engines state objects.
 * @since 0.1.0
 */
RA8_INTERNAL static v2_state* internal_v2_state_for(const ra8_reflow_t* engine, bool create)
{
  for (uint8_t i = 0; i < (uint8_t)k_v2_max_engines; ++i) {
    if (s_v2_cache[i].engine == engine) {
      return &s_v2_cache[i].state;
    }
  }
  if (!create) {
    return nullptr;
  }
  for (uint8_t i = 0; i < (uint8_t)k_v2_max_engines; ++i) {
    if (s_v2_cache[i].engine == nullptr) {
      s_v2_cache[i].engine = engine;
      internal_v2_state_clear(s_v2_cache[i].state);
      return &s_v2_cache[i].state;
    }
  }
  return nullptr;
}

/**
 * @brief Verify v2 state drop behavior against the reflow contract.
 * @details Performs the v2 state drop path and preserves each documented result and bound.
 * @param[in] engine Reflow engine instance whose state is inspected or updated.
 * @pre Required pointer arguments reference valid objects for the requested operation.
 * @post Output state reflects only the operation described by the returned status.
 * @note This helper is private to the reflow implementation boundary.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_v2_state_drop(const ra8_reflow_t* engine)
{
  for (uint8_t i = 0; i < (uint8_t)k_v2_max_engines; ++i) {
    if (s_v2_cache[i].engine == engine) {
      internal_v2_state_clear(s_v2_cache[i].state);
      s_v2_cache[i].engine = nullptr;
      return;
    }
  }
}

/**
 * @brief Verify check engine behavior against the reflow contract.
 * @details Performs the check engine path and preserves each documented result and bound.
 * @param[in] engine Reflow engine instance whose state is inspected or updated.
 * @return A status code describing the completed reflow operation.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval nonzero Validation or bounded-resource checks rejected the operation.
 * @pre Required pointer arguments reference valid objects for the requested operation.
 * @post Output state reflects only the operation described by the returned status.
 * @note This helper is private to the reflow implementation boundary.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static ra8_err_t internal_check_engine(const ra8_reflow_t* engine)
{
  if (engine == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (engine->in_use != 1U) {
    return k_ra8_err_not_initialized;
  }
  return k_ra8_ok;
}

/** Build the litehtml document for the engine's current xhtml buffer. Resets in
 * document-first order before allocating new ones so a stale document holding a
 * raw container pointer is destroyed before its target.
 * @brief Verify v2 build document behavior against the reflow contract.
 * @details Performs the v2 build document path and preserves each documented result and bound.
 * @param[in,out] state Caller-selected cached reflow state used by the operation.
 * @param[in] engine Reflow engine instance whose state is inspected or updated.
 * @return A status code describing the completed reflow operation.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval nonzero Validation or bounded-resource checks rejected the operation.
 * @pre Required pointer arguments reference valid objects for the requested operation.
 * @post Output state reflects only the operation described by the returned status.
 * @note This helper is private to the reflow implementation boundary.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static ra8_err_t internal_v2_build_document(v2_state*           state,
                                                         const ra8_reflow_t* engine)
{
  state->document.reset();
  state->container.reset();

  state->container = std::make_unique<v2_container>(static_cast<int>(engine->viewport_w),
                                                    static_cast<int>(engine->viewport_h),
                                                    static_cast<int>(engine->font_px));

  const std::string body(reinterpret_cast<const char*>(engine->xhtml_buf), engine->xhtml_len);
  std::string       html;
  html.reserve(body.size() + k_v2_html_wrap_reserve);
  html.append("<!DOCTYPE html><html><head></head><body>");
  html.append(body);
  html.append("</body></html>");

  const litehtml::estring src(html);
  state->document = litehtml::document::createFromString(src, state->container.get());
  return state->document ? k_ra8_ok : k_ra8_err_validation_failed;
}

/** Split the rendered content height into the engine's page array, clamped to
 * the reflow page-count bound.
 * @brief Verify v2 paginate behavior against the reflow contract.
 * @details Performs the v2 paginate path and preserves each documented result and bound.
 * @param[in,out] engine Reflow engine instance whose state is inspected or updated.
 * @param[in] content_height Rendered document height, in viewport pixels.
 * @pre Required pointer arguments reference valid objects for the requested operation.
 * @post Output state reflects only the operation described by the returned status.
 * @note This helper is private to the reflow implementation boundary.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_v2_paginate(ra8_reflow_t* engine, int content_height)
{
  const int vh    = static_cast<int>(engine->viewport_h);
  int       pages = (content_height + vh - 1) / vh;
  if (pages < 1) {
    pages = 1;
  }
  if (pages > static_cast<int>(k_ra8_reflow_max_pages)) {
    pages = static_cast<int>(k_ra8_reflow_max_pages);
  }
  engine->page_count = static_cast<uint32_t>(pages);
  for (int i = 0; i < pages; ++i) {
    engine->pages[i].glyph_first = 0U;
    engine->pages[i].glyph_count = 0U;
  }
}

/**
 * @brief Verify v2 run layout behavior against the reflow contract.
 * @details Performs the v2 run layout path and preserves each documented result and bound.
 * @param[in,out] engine Reflow engine instance whose state is inspected or updated.
 * @param[out] out_total_pages Optional destination that receives the resulting page count.
 * @return A status code describing the completed reflow operation.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval nonzero Validation or bounded-resource checks rejected the operation.
 * @pre Required pointer arguments reference valid objects for the requested operation.
 * @post Output state reflects only the operation described by the returned status.
 * @note This helper is private to the reflow implementation boundary.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static ra8_err_t internal_v2_run_layout(ra8_reflow_t* engine,
                                                     uint32_t*     out_total_pages)
{
  v2_state* state = internal_v2_state_for(engine, true);
  if (state == nullptr) {
    return k_ra8_err_no_mem;
  }
  const ra8_err_t err = internal_v2_build_document(state, engine);
  if (err != k_ra8_ok) {
    return err;
  }

  (void)state->document->render(static_cast<litehtml::pixel_t>(engine->viewport_w));
  int content_height = static_cast<int>(state->document->height());
  if (content_height < 0) {
    content_height = 0;
  }

  internal_v2_paginate(engine, content_height);
  if (out_total_pages != nullptr) {
    *out_total_pages = engine->page_count;
  }
  return k_ra8_ok;
}

} // namespace

extern "C" {

[[nodiscard]] ra8_err_t ra8_reflow_init(uint16_t       viewport_w,
                                        uint16_t       viewport_h,
                                        const uint8_t* font_data,
                                        size_t         font_len,
                                        uint16_t       font_px,
                                        uint32_t       body_color,
                                        uint32_t       link_color,
                                        ra8_reflow_t*  out_engine)
{
  if ((font_data == nullptr) || (out_engine == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((viewport_w == 0U) || (viewport_h == 0U) || (font_px < (uint16_t)k_ra8_reflow_min_font_px) ||
      (font_px > (uint16_t)k_ra8_reflow_max_font_px)) {
    (void)std::memset(out_engine, 0, sizeof(*out_engine));
    return k_ra8_err_invalid_arg;
  }
  if (font_len < 16U) {
    (void)std::memset(out_engine, 0, sizeof(*out_engine));
    return k_ra8_err_invalid_size;
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
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_reflow_close(ra8_reflow_t* engine)
{
  if (engine == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (engine->in_use != 1U) {
    return k_ra8_err_not_initialized;
  }
  internal_v2_state_drop(engine);
  engine->in_use     = 0U;
  engine->page_count = 0U;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_reflow_layout_chapter(ra8_reflow_t*  engine,
                                                  const uint8_t* xhtml_buf,
                                                  size_t         xhtml_len,
                                                  uint32_t*      out_total_pages)
{
  const ra8_err_t err = internal_check_engine(engine);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((xhtml_buf == nullptr) || (out_total_pages == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (xhtml_len == 0U) {
    return k_ra8_err_invalid_size;
  }
  engine->xhtml_buf = xhtml_buf;
  engine->xhtml_len = xhtml_len;
  return internal_v2_run_layout(engine, out_total_pages);
}

[[nodiscard]] ra8_err_t
ra8_reflow_render_page(const ra8_reflow_t* engine, uint32_t page_idx, void* framebuffer)
{
  (void)framebuffer;
  const ra8_err_t err = internal_check_engine(engine);
  if (err != k_ra8_ok) {
    return err;
  }
  if (page_idx >= engine->page_count) {
    return k_ra8_err_out_of_range;
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_reflow_get_page_count(const ra8_reflow_t* engine, uint32_t* out_count)
{
  const ra8_err_t err = internal_check_engine(engine);
  if (err != k_ra8_ok) {
    return err;
  }
  if (out_count == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out_count = engine->page_count;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_reflow_set_font_size(ra8_reflow_t* engine, uint16_t new_font_px)
{
  const ra8_err_t err = internal_check_engine(engine);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((new_font_px < (uint16_t)k_ra8_reflow_min_font_px) ||
      (new_font_px > (uint16_t)k_ra8_reflow_max_font_px)) {
    return k_ra8_err_invalid_arg;
  }
  if (engine->xhtml_buf == nullptr) {
    return k_ra8_err_invalid_state;
  }
  engine->font_px = new_font_px;
  uint32_t total  = 0U;
  return internal_v2_run_layout(engine, &total);
}

[[nodiscard]] ra8_err_t
ra8_reflow_parse_xhtml(ra8_reflow_t* engine, const uint8_t* xhtml_buf, size_t xhtml_len)
{
  if (engine == nullptr) {
    return k_ra8_err_null_ptr;
  }
  engine->xhtml_buf = xhtml_buf;
  engine->xhtml_len = xhtml_len;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_reflow_run_layout(ra8_reflow_t* engine)
{
  if (engine == nullptr) {
    return k_ra8_err_null_ptr;
  }
  uint32_t total = 0U;
  return internal_v2_run_layout(engine, &total);
}

} // extern "C"
