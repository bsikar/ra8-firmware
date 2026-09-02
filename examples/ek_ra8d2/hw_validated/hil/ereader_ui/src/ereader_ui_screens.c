/**
 * @file examples/ek_ra8d2/hw_validated/hil/ereader_ui/src/ereader_ui_screens.c
 * @brief E-reader UI chrome -- Library + Reading rendering and in-content nav.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Split out of ``ereader_ui/main.c`` to keep every translation unit under the
 * repository's 1000-line cap (a pure code move). This unit owns the box-model
 * Library screen (status bar, toolbar, book grid, bottom nav), the Reading
 * screen (status bar, reflowed / bitmap-fallback body, footer), the shared
 * bundled-font text helpers, and the in-content navigation that follows
 * `<a href>` links and page turns across the mock spine. The composited chrome
 * is byte-identical to the pre-split monolith.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "er_pageturn.h"
#include "ereader_ui_steps.h"
#include "figure_fixture.h"
#include "ra8_box.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_gfx_font.h"
#include "ra8_glyph_atlas.h"
#include "ra8_ui.h"
#include "ra8_widget.h"
#include "reflow.h"
#include "reflow_image.h"

/** @brief Baked Latin-1 font array (generated at build time). */
extern const uint8_t      g_ra8_font_literata_latin1[];
extern const unsigned int g_ra8_font_literata_latin1_len;

/* ===========================================================================
 * Library/Reading box-tree side tables (owned here -- single-screen state).
 * =========================================================================== */

/** @brief Box-tree node storage for chrome layout. */
static ra8_box_t s_nodes[k_er_max_nodes];

/** @brief Per-node text label (NULL if the node draws no text). */
static const char* s_label[k_er_max_nodes];

/** @brief Per-node text colour. */
static uint32_t s_label_col[k_er_max_nodes];

/** @brief Per-node progress percent, or -1 for "not a progress bar". */
static int16_t s_progress[k_er_max_nodes];

/** @brief Library box-node count (set by er_render_library for the band widgets). */
static uint16_t s_lib_node_count;

/** @brief First Library body box-node (root=0, status bar=1, body from here). */
static uint16_t s_lib_body_first = 2U;

/**
 * @brief In-content SVG (#112): a navy field, a gold disc, and a crimson block.
 *
 * @details Returned by ::er_image_loader for any `*.svg` `<img src>`; rendered as
 * vector `<rect>`/`<circle>` shapes by ra8_svg_render (no raster decode).
 */
static const char k_er_logo_svg[] =
  "<svg viewBox=\"0 0 120 80\">"
  "<rect x=\"0\" y=\"0\" width=\"120\" height=\"80\" fill=\"#283C82\"/>"
  "<circle cx=\"34\" cy=\"40\" r=\"24\" fill=\"#E6C84B\"/>"
  "<path d=\"M66 62 L112 62 L89 16 Z\" fill=\"#C03A37\"/>"
  "</svg>";

/** @brief SVG fixture size + the `.svg` extension length for href routing. */
typedef enum : uint32_t {
  k_er_logo_svg_len = (uint32_t)(sizeof(k_er_logo_svg) - 1U), /**< SVG byte count.   */
  k_er_ext_len      = 4U,                                     /**< Length of ".svg". */
} er_svg_len_t;

/**
 * @brief reflow image loader: resolve any `<img src>` to the baked figure.
 *
 * @details The mock library has no EPUB ZIP to read resources from, so every
 * `<img>` in the demo chapter resolves to the one bundled figure
 * (::k_er_figure_png). A real EPUB-backed build would map @p href to a manifest
 * item and return its bytes; the engine contract is identical either way.
 *
 * @param[in]  ctx      Unused loader context.
 * @param[in]  href     Image src (unused; single bundled figure).
 * @param[in]  href_len Length of @p href.
 * @param[out] out_bytes Receives the encoded PNG bytes.
 * @param[out] out_len   Receives the encoded byte count.
 * @return k_ra8_ok always (the figure is always available).
 * @retval k_ra8_ok The bundled figure bytes were returned.
 * @pre @p out_bytes and @p out_len are non-null.
 * @pre The bundled figure is a valid encoded image.
 * @post `*out_bytes` / `*out_len` describe ::k_er_figure_png.
 * @post No state mutated.
 * @note Not thread-safe (single-threaded UI loop).
 * @since 0.1.0
 */
static ra8_err_t er_image_loader(void*           ctx,
                                 const char*     href,
                                 uint32_t        href_len,
                                 const uint8_t** out_bytes,
                                 size_t*         out_len)
{
  (void)ctx;
  /* `*.svg` -> the vector logo; everything else -> the baked raster figure. */
  if ((href_len >= (uint32_t)k_er_ext_len) &&
      (memcmp(&href[href_len - (uint32_t)k_er_ext_len], ".svg", (size_t)k_er_ext_len) == 0)) {
    *out_bytes = (const uint8_t*)k_er_logo_svg;
    *out_len   = (size_t)k_er_logo_svg_len;
    return k_ra8_ok;
  }
  *out_bytes = k_er_figure_png;
  *out_len   = (size_t)k_er_figure_png_len;
  return k_ra8_ok;
}

/* ===========================================================================
 * Text helpers
 * =========================================================================== */

void er_text_left(int32_t x, int32_t y, const char* str, uint32_t color)
{
  (void)ra8_gfx_text_out(x, y, str, &ra8_gfx_font_8x16, color, (uint32_t)k_er_paper);
}

void er_text_right(int32_t right, int32_t y, const char* str, uint32_t color)
{
  uint32_t w = 0U;
  uint32_t h = 0U;
  if (ra8_gfx_text_size(str, &ra8_gfx_font_8x16, &w, &h) != k_ra8_ok) {
    return;
  }
  er_text_left(right - (int32_t)w, y, str, color);
}

/* ===========================================================================
 * Library screen -- built with ra8_box, rendered with ra8_gfx
 * =========================================================================== */

/**
 * @brief Reset the per-node label / progress side tables.
 *
 * @pre None.
 * @pre None.
 * @post Every node slot has no label and no progress bar.
 * @post Every label colour defaults to ink.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_reset_side_tables(void)
{
  for (uint16_t i = 0U; i < (uint16_t)k_er_max_nodes; ++i) {
    s_label[i]     = nullptr;
    s_label_col[i] = (uint32_t)k_er_ink;
    s_progress[i]  = -1;
  }
}

/**
 * @brief Make a leaf box template carrying fill / border.
 *
 * @param[in] fixed  Fixed main-axis extent (0 => flex).
 * @param[in] flex   Flex weight when not fixed.
 * @param[in] fill   Fill colour, or k_ra8_box_no_colour.
 * @param[in] border Border colour, or k_ra8_box_no_colour.
 *
 * @return A leaf ra8_box_t template.
 * @retval node Configured leaf node template.
 *
 * @pre None.
 * @pre None.
 * @post Returned node has kind leaf and the requested sizing/colours.
 * @post Tree links are left for ra8_box_add to set.
 *
 * @note Pure.
 * @since 0.1.0
 */
static ra8_box_t er_leaf(int16_t fixed, uint16_t flex, uint32_t fill, uint32_t border)
{
  ra8_box_t n = {};
  n.kind      = (uint8_t)k_ra8_box_leaf;
  n.fixed     = fixed;
  n.flex      = flex;
  n.fill      = fill;
  n.border    = border;
  n.border_w  = (border != (uint32_t)k_ra8_box_no_colour) ? (int16_t)k_er_border_w : (int16_t)0;
  n.grid_cols = 1U;
  n.tag       = (int16_t)k_ra8_box_none;
  return n;
}

/**
 * @brief Make a container box template (stack or grid).
 *
 * @param[in] kind  Container kind.
 * @param[in] fixed Fixed main-axis extent (0 => flex).
 * @param[in] pad   Inner padding.
 * @param[in] gap   Gap between children.
 * @param[in] cols  Grid columns (>= 1).
 *
 * @return A container ra8_box_t template.
 * @retval node Configured container node template.
 *
 * @pre @p kind is a container kind.
 * @pre None.
 * @post Returned node has the requested kind / sizing / spacing.
 * @post Flex defaults to 1 so it fills its parent unless `fixed` is set.
 *
 * @note Pure.
 * @since 0.1.0
 */
static ra8_box_t
er_container(ra8_box_kind_t kind, int16_t fixed, int16_t pad, int16_t gap, uint8_t cols)
{
  ra8_box_t n = {};
  n.kind      = (uint8_t)kind;
  n.fixed     = fixed;
  n.flex      = 1U;
  n.pad       = pad;
  n.gap       = gap;
  n.grid_cols = (cols >= 1U) ? cols : 1U;
  n.tag       = (int16_t)k_ra8_box_none;
  return n;
}

/**
 * @brief Add the toolbar (search field + book-count chip) under a parent.
 *
 * @param[in,out] tree   Tree builder bound to s_nodes.
 * @param[in]     parent Parent container index (the screen column).
 *
 * @pre @p tree references s_nodes; @p parent is a valid container.
 * @pre Side tables are reset.
 * @post A horizontal toolbar with two children is appended.
 * @post Their labels/colours are recorded in the side tables.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_build_toolbar(ra8_box_tree_t* tree, int16_t parent)
{
  const ra8_box_t tb_t = er_container(k_ra8_box_stack_h,
                                      (int16_t)k_er_toolbar_h,
                                      (int16_t)k_er_pad_ui,
                                      (int16_t)k_er_pad_ui,
                                      1U);
  const int16_t   tb   = ra8_box_add(tree, parent, &tb_t);

  ra8_box_t srch_t   = er_leaf(0, 1U, (uint32_t)k_ra8_box_no_colour, (uint32_t)k_er_ink);
  srch_t.tag         = (int16_t)k_er_act_search; /* tap the Search field -> keyboard */
  const int16_t srch = ra8_box_add(tree, tb, &srch_t);
  s_label[srch]      = k_er_search_hint;
  s_label_col[srch]  = (uint32_t)k_er_ink_muted;

  const ra8_box_t cnt_t =
    er_leaf((int16_t)k_er_count_w, 0U, (uint32_t)k_ra8_box_no_colour, (uint32_t)k_er_ink);
  const int16_t cnt = ra8_box_add(tree, tb, &cnt_t);
  s_label[cnt]      = k_er_count_text;
  s_label_col[cnt]  = (uint32_t)k_er_ink_muted;
}

/**
 * @brief Add one book card (cover, title, author, progress) under a grid.
 *
 * @param[in,out] tree Tree builder bound to s_nodes.
 * @param[in]     grid Grid container index.
 * @param[in]     book Book to render in the card.
 *
 * @pre @p tree references s_nodes; @p grid is a valid grid; @p book valid.
 * @pre Side tables are reset.
 * @post A vertical card subtree is appended to @p grid.
 * @post The title/author labels and progress are recorded.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_add_book_tile(ra8_box_tree_t* tree, int16_t grid, const er_book_t* book)
{
  ra8_box_t tile_t   = er_container(k_ra8_box_stack_v, 0, 0, (int16_t)k_er_tile_gap, 1U);
  tile_t.tag         = (int16_t)k_er_act_open_book; /* the whole card is a tap target */
  const int16_t tile = ra8_box_add(tree, grid, &tile_t);

  const ra8_box_t cover_t = er_leaf(0, 1U, (uint32_t)k_er_fill, (uint32_t)k_er_ink);
  (void)ra8_box_add(tree, tile, &cover_t);

  const ra8_box_t lbl_t = er_leaf((int16_t)k_er_card_label_h,
                                  0U,
                                  (uint32_t)k_ra8_box_no_colour,
                                  (uint32_t)k_ra8_box_no_colour);
  const int16_t   title = ra8_box_add(tree, tile, &lbl_t);
  s_label[title]        = book->title;
  const int16_t auth    = ra8_box_add(tree, tile, &lbl_t);
  s_label[auth]         = book->author;
  s_label_col[auth]     = (uint32_t)k_er_ink_muted;

  const ra8_box_t bar_t =
    er_leaf((int16_t)k_er_card_bar_h, 0U, (uint32_t)k_er_rule_soft, (uint32_t)k_ra8_box_no_colour);
  const int16_t bar = ra8_box_add(tree, tile, &bar_t);
  s_progress[bar]   = (int16_t)book->pct;
}

/**
 * @brief Add the bottom navigation strip under a parent.
 *
 * @param[in,out] tree   Tree builder bound to s_nodes.
 * @param[in]     parent Parent container index (the screen column).
 *
 * @pre @p tree references s_nodes; @p parent is a valid container.
 * @pre Side tables are reset.
 * @post A horizontal nav strip with one flex item per destination.
 * @post The active (first) item is inked, the rest muted.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_build_nav(ra8_box_tree_t* tree, int16_t parent)
{
  const ra8_box_t nav_t = er_container(k_ra8_box_stack_h, (int16_t)k_er_nav_h, 0, 0, 1U);
  const int16_t   nav   = ra8_box_add(tree, parent, &nav_t);
  for (uint16_t i = 0U; i < (uint16_t)k_er_nav_count; ++i) {
    ra8_box_t item_t = er_leaf(0, 1U, (uint32_t)k_ra8_box_no_colour, (uint32_t)k_ra8_box_no_colour);
#ifdef RA8_APP_SETTINGS
    /* The "Settings" nav item is tappable only when the Settings app is installed
     * (RA8_APP_SETTINGS): tagging affects target collection, not pixels, so the
     * default (uninstalled) build's chrome is byte-identical. */
    if (i == (uint16_t)k_er_nav_idx_setting) {
      item_t.tag = (int16_t)k_er_act_settings;
    }
#endif
    const int16_t item = ra8_box_add(tree, nav, &item_t);
    s_label[item]      = k_er_nav_items[i];
    s_label_col[item]  = (i == 0U) ? (uint32_t)k_er_ink : (uint32_t)k_er_ink_muted;
  }
}

/** @enum er_filter_t @brief Search-filter scan bound (NASA Rule 2). */
typedef enum : uint32_t {
  k_er_match_scan_max = 256U, /**< Max title chars scanned for a match. */
} er_filter_t;

/** @brief Lower-case an ASCII letter (identity for non-letters). */
static char er_lc(char c)
{
  return ((c >= 'A') && (c <= 'Z')) ? (char)((c - 'A') + 'a') : c;
}

/** @brief Case-insensitive: does @p needle occur within @p hay? */
static bool er_ci_contains(const char* hay, const char* needle)
{
  if (needle[0] == '\0') {
    return true;
  }
  for (uint32_t i = 0U; (i < (uint32_t)k_er_match_scan_max) && (hay[i] != '\0'); i++) {
    uint32_t j = 0U;
    while ((j < (uint32_t)k_ra8_kbd_text_max) && (needle[j] != '\0') && (hay[i + j] != '\0') &&
           (er_lc(hay[i + j]) == er_lc(needle[j]))) {
      j++;
    }
    if (needle[j] == '\0') {
      return true;
    }
  }
  return false;
}

/** @brief Does @p book pass the committed search query (true if no filter)? */
static bool er_book_matches(const er_book_t* book)
{
  if (!s_query.committed || (s_query.len == 0U)) {
    return true;
  }
  return er_ci_contains(book->title, s_query.buf);
}

/**
 * @brief Build the Library box tree (status bar, toolbar, grid, nav).
 *
 * @param[in,out] tree  Tree builder bound to s_nodes.
 * @param[in]     frame Screen rectangle to lay out within.
 *
 * @pre ra8_gfx is bound; @p tree references s_nodes.
 * @pre s_book table populated.
 * @post `tree` holds the laid-out Library; side tables hold text/progress.
 * @post Every node has its rect computed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_build_library(ra8_box_tree_t* tree, const ra8_ui_rect_t* frame)
{
  er_reset_side_tables();
  (void)ra8_box_tree_init(tree, s_nodes, (uint16_t)k_er_max_nodes);

  const ra8_box_t root_t = er_container(k_ra8_box_stack_v, 0, 0, 0, 1U);
  const int16_t   root   = ra8_box_add(tree, (int16_t)k_ra8_box_none, &root_t);

  const ra8_box_t sb_t = er_leaf((int16_t)k_er_statusbar_h,
                                 0U,
                                 (uint32_t)k_ra8_box_no_colour,
                                 (uint32_t)k_ra8_box_no_colour);
  const int16_t   sb   = ra8_box_add(tree, root, &sb_t);
  s_label[sb]          = k_er_lib_heading;
  /* The status bar is one node; everything after it is the body band (the band
   * widgets in er_render_library split the render at this index). */
  s_lib_body_first = (uint16_t)(sb + 1);

  er_build_toolbar(tree, root);

  const ra8_box_t grid_t = er_container(k_ra8_box_grid,
                                        0,
                                        (int16_t)k_er_pad_ui,
                                        (int16_t)k_er_grid_gap,
                                        (uint8_t)k_er_grid_cols);
  const int16_t   grid   = ra8_box_add(tree, root, &grid_t);
  for (uint16_t i = 0U; i < (uint16_t)k_er_book_count; ++i) {
    if (!er_book_matches(&k_er_books[i])) {
      continue; /* hidden by the committed search query */
    }
    er_add_book_tile(tree, grid, &k_er_books[i]);
  }

  er_build_nav(tree, root);

  (void)ra8_box_layout(tree, root, frame);
}

/**
 * @brief Render box nodes ``[from, to)`` of ::s_nodes (fills, borders,
 *        progress bars, labels).
 *
 * @param[in] from Index of the first node to draw (inclusive).
 * @param[in] to   Index one past the last node to draw (exclusive).
 *
 * @pre ra8_gfx is bound; the tree is laid out; side tables match its nodes.
 * @pre @p from <= @p to and @p to is within the laid-out node count.
 * @post Every node in ``[from, to)`` has its fill/border/progress/label drawn.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_render_boxnodes(uint16_t from, uint16_t to)
{
  for (uint16_t i = from; i < to; ++i) {
    const ra8_box_t*    n = &s_nodes[i];
    const ra8_ui_rect_t r = n->rect;
    if (n->fill != (uint32_t)k_ra8_box_no_colour) {
      (void)ra8_gfx_rect(r.x, r.y, r.w, r.h, n->fill, true);
    }
    if ((n->border_w > 0) && (n->border != (uint32_t)k_ra8_box_no_colour)) {
      (void)ra8_gfx_rect(r.x, r.y, r.w, r.h, n->border, false);
    }
    if (s_progress[i] >= 0) {
      const int32_t fillw = (r.w * (int32_t)s_progress[i]) / (int32_t)k_er_pct_full;
      (void)ra8_gfx_rect(r.x, r.y, fillw, r.h, (uint32_t)k_er_fill_deep, true);
    }
    if (s_label[i] != nullptr) {
      er_text_left(r.x + (int32_t)k_er_text_pad,
                   r.y + (int32_t)k_er_text_pad,
                   s_label[i],
                   s_label_col[i]);
    }
  }
}

/**
 * @brief Collect tap targets from a laid-out tree's tagged nodes.
 *
 * @param[in] tree Laid-out box tree.
 *
 * @pre @p tree is laid out.
 * @pre None.
 * @post ``s_targets`` holds (rect, action) for each tagged node, capped.
 * @post ``s_target_count`` is the number collected.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_collect_targets(const ra8_box_tree_t* tree)
{
  s_target_count = 0U;
  for (uint16_t i = 0U; i < tree->count; ++i) {
    const ra8_box_t* n = &tree->nodes[i];
    if ((n->tag != (int16_t)k_ra8_box_none) && (s_target_count < (uint16_t)k_er_max_targets)) {
      s_targets[s_target_count].rect      = n->rect;
      s_targets[s_target_count].action_id = (uint16_t)n->tag;
      s_targets[s_target_count].reserved  = 0U;
      s_target_count++;
    }
  }
}

/**
 * @brief Render the full Library screen.
 *
 * @pre ra8_gfx is bound; ``s_fb`` reflects the framebuffer geometry.
 * @pre None.
 * @post The framebuffer holds the Library screen; ``s_targets`` set.
 * @post Caller flushes the panel to make it visible.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_lib_sb_render(ra8_widget_t* w)
{
  (void)w;
  er_render_boxnodes(0U, s_lib_body_first); /* root (no-op) + the status-bar node */
  (void)ra8_gfx_rect(0,
                     (int32_t)k_er_statusbar_h - (int32_t)k_er_hair,
                     (int32_t)s_fb.width_px,
                     (int32_t)k_er_hair,
                     (uint32_t)k_er_rule,
                     true);
  er_text_right((int32_t)s_fb.width_px - (int32_t)k_er_pad_ui,
                (int32_t)k_er_text_inset_y,
                k_er_status_right,
                (uint32_t)k_er_ink_muted);
}

/** @brief Body band widget: toolbar + book grid + nav nodes + the above-nav rule. */
static void er_lib_body_render(ra8_widget_t* w)
{
  (void)w;
  er_render_boxnodes(s_lib_body_first, s_lib_node_count); /* toolbar + grid + nav */
  (void)ra8_gfx_rect(0,
                     (int32_t)s_fb.height_px - (int32_t)k_er_nav_h,
                     (int32_t)s_fb.width_px,
                     (int32_t)k_er_hair,
                     (uint32_t)k_er_rule,
                     true);
}

/** @brief Vtables for the Library status-bar / body band widgets. */
static const ra8_widget_vtable_t k_er_lib_sb_vt   = {.render = er_lib_sb_render};
static const ra8_widget_vtable_t k_er_lib_body_vt = {.render = er_lib_body_render};

void er_render_library(void)
{
  (void)ra8_gfx_clear((uint32_t)k_er_paper);
  ra8_box_tree_t      tree;
  const ra8_ui_rect_t frame = {0, 0, (int32_t)s_fb.width_px, (int32_t)s_fb.height_px};
  er_build_library(&tree, &frame);
  er_collect_targets(&tree);
  s_lib_node_count = tree.count;

  /* Compose the screen from two band widgets (#145): a status bar over the body
   * (toolbar + grid + nav). The bands occupy disjoint y-ranges, so the widget
   * composition renders byte-identically to the monolithic box tree -- the
   * status bar can now be invalidated + partial-flushed on its own. */
  ra8_widget_t bands[2] = {};
  bands[0].vt           = &k_er_lib_sb_vt;
  bands[0].fixed        = (int16_t)k_er_statusbar_h;
  bands[0].visible      = true;
  bands[1].vt           = &k_er_lib_body_vt;
  bands[1].flex         = 1U;
  bands[1].visible      = true;
  ra8_box_t band_scratch[3];
  (void)ra8_widget_layout_stack(bands, 2U, &frame, k_ra8_widget_axis_col, 0, 0, band_scratch, 3U);
  (void)ra8_widget_invalidate(&bands[0], k_ra8_widget_refresh_quality);
  (void)ra8_widget_invalidate(&bands[1], k_ra8_widget_refresh_quality);
  (void)ra8_widget_render_dirty(bands, 2U);
}

/* ===========================================================================
 * Reading screen -- reflow body text (SD font) with a bitmap fallback
 * =========================================================================== */

/**
 * @brief Bind the Layer-3 glyph atlas (#164) to the reflow engine.
 *
 * @details A page turn re-renders the same body glyphs, so caching the
 * rasterised bitmaps (cells in SDRAM, like the framebuffer + decode arena)
 * avoids re-running stb_truetype every frame. Re-bound on each relayout
 * (reflow_init zeroes the engine); the cache clears per chapter, the natural
 * working-set boundary. Output is byte-identical to the direct path -- oversized
 * glyphs fall back to direct rasterisation. The caller-owned storage is
 * function-static (single reflow engine, single-threaded UI loop).
 *
 * @pre ::s_reflow_engine has been initialised by reflow_init().
 * @pre The SDRAM `.sdram_data` section is mapped (post ra8_sdramc_init()).
 * @post ::s_reflow_engine renders body glyphs through the glyph cache.
 * @post The cache starts empty (re-init clears it).
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_bind_glyph_atlas(void)
{
  [[gnu::section(".sdram_data")]] static uint8_t
    s_glyph_cells[(size_t)k_er_glyph_cells * (size_t)k_er_glyph_cell_bytes];
  static ra8_keycache_cell_t s_glyph_meta[k_er_glyph_cells];
  static ra8_glyph_key_t     s_glyph_keys[k_er_glyph_cells];
  static ra8_glyph_dims_t    s_glyph_dims[k_er_glyph_cells];
  /* cppcheck-suppress unassignedVariable ; hash-bucket heads are cleared by
     ra8_glyph_atlas_init() through the storage pointer; not a direct assignment. */
  static int32_t                     s_glyph_buckets[k_er_glyph_buckets];
  static ra8_glyph_atlas_t           s_glyph_atlas;
  const reflow_glyph_atlas_storage_t glyph_store = {
    .cell_mem     = s_glyph_cells,
    .cell_bytes   = (uint32_t)k_er_glyph_cell_bytes,
    .cell_count   = (uint32_t)k_er_glyph_cells,
    .meta         = s_glyph_meta,
    .keys         = s_glyph_keys,
    .dims         = s_glyph_dims,
    .buckets      = s_glyph_buckets,
    .bucket_count = (uint32_t)k_er_glyph_buckets,
  };
  (void)reflow_set_glyph_atlas(&s_reflow_engine, &s_glyph_atlas, &glyph_store);
}

/**
 * @brief Re-init the reflow engine and lay the current chapter into the cache.
 *
 * @details Closes any prior layout, binds the font + image arena, paginates the
 * current chapter (::s_chapter_idx) and records the cache keys
 * (::s_reflow_chapter / ::s_reflow_w / ::s_reflow_h) so subsequent same-chapter
 * page turns skip this expensive step. The costliest part of a Reading render.
 *
 * @param[in] body_w    Body width the chapter is paginated for (px).
 * @param[in] body_h    Body height the chapter is paginated for (px).
 * @param[in] font_data Font blob (SD-loaded or baked).
 * @param[in] font_len  Font blob length in bytes.
 * @return true if the chapter was laid out and the cache is now valid.
 * @retval true  Layout succeeded; ::s_reflow_open is set.
 * @retval false init / layout failed; the engine is closed.
 * @pre @p body_w and @p body_h are > 0.
 * @pre @p font_data / @p font_len describe a usable face.
 * @post On true the cache keys reflect the laid-out chapter + geometry.
 * @post On false ::s_reflow_open is false.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool
er_reflow_relayout(int32_t body_w, int32_t body_h, const uint8_t* font_data, uint32_t font_len)
{
  if (s_reflow_open) {
    (void)reflow_close(&s_reflow_engine);
    s_reflow_open = false;
  }
  if (reflow_init((uint16_t)body_w,
                  (uint16_t)body_h,
                  font_data,
                  font_len,
                  (uint16_t)k_er_reflow_px,
                  (uint32_t)k_er_reflow_ink,
                  (uint32_t)k_er_reflow_link,
                  &s_reflow_engine) != k_ra8_ok) {
    return false;
  }
  /* Bind the image loader + SDRAM decode arena so the chapter's <img> renders
   * (decode -> scale -> blit). Without this the engine reserves a placeholder. */
  static ra8_img_arena_t s_reflow_img_arena;
  s_reflow_img_arena = (ra8_img_arena_t){.base   = s_img_arena_buf,
                                         .cap    = (size_t)k_er_img_arena,
                                         .offset = 0U,
                                         .live   = 0U};
  (void)reflow_set_image_loader(&s_reflow_engine, er_image_loader, nullptr, &s_reflow_img_arena);
  er_bind_glyph_atlas();

  uint32_t            pages = 0U;
  const er_chapter_t* chap  = &k_er_spine[s_chapter_idx];
  if (reflow_layout_chapter(&s_reflow_engine, (const uint8_t*)chap->xhtml, chap->len, &pages) !=
      k_ra8_ok) {
    (void)reflow_close(&s_reflow_engine);
    return false;
  }
  s_reading_pages  = pages;
  s_reflow_open    = true;
  s_reflow_chapter = s_chapter_idx;
  s_reflow_w       = body_w;
  s_reflow_h       = body_h;
  return true;
}

/**
 * @brief Render the Reading body through reflow when an SD font is loaded.
 *
 * @details Lays the chapter XHTML out against the body rectangle (inset
 *          below the status bar, above the footer) and paints the current
 *          page (::s_reading_page) there via reflow_render_page_at.
 *          er_render_reading has already cleared the framebuffer to paper
 *          and the body colour is dark ink, so the text shows (the
 *          reflow_init colour args are the text colours, not the
 *          background). Publishes the layout's page count to ::s_reading_pages
 *          and clamps ::s_reading_page into range.
 *
 * @param[in] body_top Top y of the body band (pixels).
 * @param[in] height   Framebuffer height (pixels).
 * @return true if reflowed text was painted; false to use the bitmap fallback.
 * @retval true  reflow rendered the body.
 * @retval false No font / init / layout failure -- caller draws the bitmap body.
 * @pre ra8_gfx is bound and the body region is cleared to paper.
 * @pre @p height matches the bound framebuffer.
 * @post On true, ::s_reading_page of the chapter is blitted into the body rect
 *       and ::s_reading_pages holds the total page count.
 * @post On false, nothing is drawn.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool er_draw_reading_body_reflow(int32_t body_top, int32_t height)
{
  /* Reflow from the SD-loaded font if present, else the Latin-1 face baked into
   * flash (#66) -- so the Reading body shows real proportional text with no card
   * at all. Only a reflow-engine failure falls through to the bitmap body. */
  const uint8_t* font_data = s_have_font ? s_font_buf : g_ra8_font_literata_latin1;
  const uint32_t font_len  = s_have_font ? s_font_len : g_ra8_font_literata_latin1_len;
  const int32_t  body_w    = (int32_t)s_fb.width_px - ((int32_t)k_er_margin_x * 2);
  const int32_t  body_h =
    height - (int32_t)k_er_statusbar_h - (int32_t)k_er_footer_h - ((int32_t)k_er_body_gap * 2);
  if (body_w <= 0) {
    return false;
  }
  if (body_h <= 0) {
    return false;
  }
  /* Re-flow only when the chapter or body geometry changes. Laying a chapter out
   * (parse XHTML + paginate every page) dwarfs the cost of rasterising one page,
   * so a same-chapter page turn reuses the cached layout and only renders the
   * requested page -- otherwise every turn re-parses + re-paginates the whole
   * chapter, the multi-second page-turn stall. */
  const bool need_layout = (!s_reflow_open) || (s_reflow_chapter != s_chapter_idx) ||
                           (s_reflow_w != body_w) || (s_reflow_h != body_h);
  if (need_layout && !er_reflow_relayout(body_w, body_h, font_data, font_len)) {
    return false;
  }
  /* Clamp the current page (the chapter may paginate shorter than the last one)
   * and rasterise just that page from the cached layout. */
  if (s_reading_page >= s_reading_pages) {
    s_reading_page = s_reading_pages - 1U;
  }
  (void)reflow_render_page_at(&s_reflow_engine, s_reading_page, (int32_t)k_er_margin_x, body_top);
  return true;
}

/**
 * @brief Paint the Reading body: reflowed text (SD or baked font), else bitmap.
 *
 * @param[in] height Framebuffer height in pixels.
 *
 * @return true if the body was reflowed (paginated); false on the bitmap fallback.
 * @retval true  reflow painted the current page (SD font or the baked font).
 * @retval false The bundled bitmap lines were drawn (reflow-engine failure).
 * @pre ra8_gfx is bound.
 * @pre @p height matches the bound framebuffer.
 * @post The body band holds reflowed text or the bitmap fallback.
 * @post Drawing stops before the footer band.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool er_draw_reading_body(int32_t height)
{
  const int32_t body_top = (int32_t)k_er_statusbar_h + (int32_t)k_er_body_gap;
  if (er_draw_reading_body_reflow(body_top, height)) {
    return true; /* Live proportional text (SD font, else the baked flash font). */
  }
  /* Fallback: the bundled 8x16 bitmap font (no SD card / no FONT.OTF). */
  const int32_t body_bot = height - (int32_t)k_er_footer_h - (int32_t)k_er_body_gap;
  er_text_left((int32_t)k_er_margin_x, body_top, k_er_chapter, (uint32_t)k_er_ink);
  int32_t y = body_top + ((int32_t)k_er_blank_lines * (int32_t)k_er_line_h);
  for (uint16_t i = 0U; i < (uint16_t)k_er_body_line_count; ++i) {
    if ((y + (int32_t)k_er_glyph_h) > body_bot) {
      break;
    }
    er_text_left((int32_t)k_er_margin_x, y, k_er_body_lines[i], (uint32_t)k_er_ink);
    y += (int32_t)k_er_line_h;
  }
  return false; /* bitmap fallback -- the body is not paginated */
}

/** @brief Stash: did the body band reflow live text? The footer bar tracks it. */
static bool s_read_reflowed = false;

/** @brief Status-bar band widget: wordmark + book title + status-right + hairline. */
static void er_read_sb_render(ra8_widget_t* w)
{
  (void)w;
  const int32_t width = (int32_t)s_fb.width_px;
  er_text_left((int32_t)k_er_pad_ui, (int32_t)k_er_text_inset_y, k_er_wordmark, (uint32_t)k_er_ink);
  const int32_t title_x =
    (int32_t)k_er_pad_ui + ((int32_t)sizeof(k_er_wordmark) * (int32_t)k_er_glyph_w);
  er_text_left(title_x, (int32_t)k_er_text_inset_y, k_er_book_title, (uint32_t)k_er_ink_muted);
  er_text_right(width - (int32_t)k_er_pad_ui,
                (int32_t)k_er_text_inset_y,
                k_er_status_right,
                (uint32_t)k_er_ink_muted);
  (void)ra8_gfx_rect(0,
                     (int32_t)k_er_statusbar_h - (int32_t)k_er_hair,
                     width,
                     (int32_t)k_er_hair,
                     (uint32_t)k_er_rule,
                     true);
}

/** @brief Body band widget: the reflowed (or bitmap-fallback) chapter text. */
static void er_read_body_render(ra8_widget_t* w)
{
  (void)w;
  s_read_reflowed = er_draw_reading_body((int32_t)s_fb.height_px);
}

/** @brief Footer band widget: rule + chapter title + page label + progress bar. */
static void er_read_footer_render(ra8_widget_t* w)
{
  (void)w;
  const int32_t width    = (int32_t)s_fb.width_px;
  const int32_t height   = (int32_t)s_fb.height_px;
  const int32_t band_top = height - (int32_t)k_er_footer_h;
  (void)ra8_gfx_rect(0, band_top, width, (int32_t)k_er_hair, (uint32_t)k_er_rule, true);
  const int32_t text_y = band_top + (int32_t)k_er_progress_gap;
  er_text_left((int32_t)k_er_pad_ui, text_y, k_er_book_title, (uint32_t)k_er_ink_muted);
  er_text_right(width - (int32_t)k_er_pad_ui, text_y, k_er_page_label, (uint32_t)k_er_ink_muted);
  const int32_t track_x = (int32_t)k_er_pad_ui;
  const int32_t track_w = width - (2 * (int32_t)k_er_pad_ui);
  const int32_t track_y = height - (int32_t)k_er_progress_gap - (int32_t)k_er_progress_h;
  (void)ra8_gfx_rect(track_x,
                     track_y,
                     track_w,
                     (int32_t)k_er_progress_h,
                     (uint32_t)k_er_rule_soft,
                     true);
  /* In the reflow path the bar tracks the live page; the bitmap fallback keeps
   * the static placeholder ratio. */
  const int32_t pg_cur =
    s_read_reflowed ? (int32_t)(s_reading_page + 1U) : (int32_t)k_er_page_current;
  const int32_t pg_tot = s_read_reflowed ? (int32_t)s_reading_pages : (int32_t)k_er_page_total;
  const int32_t fill_w = (track_w * pg_cur) / pg_tot;
  (void)ra8_gfx_rect(track_x,
                     track_y,
                     fill_w,
                     (int32_t)k_er_progress_h,
                     (uint32_t)k_er_fill_deep,
                     true);
}

/** @brief Vtables for the Reading status-bar / body / footer band widgets. */
static const ra8_widget_vtable_t k_er_read_sb_vt     = {.render = er_read_sb_render};
static const ra8_widget_vtable_t k_er_read_body_vt   = {.render = er_read_body_render};
static const ra8_widget_vtable_t k_er_read_footer_vt = {.render = er_read_footer_render};

void er_render_reading(void)
{
  (void)ra8_gfx_clear((uint32_t)k_er_paper);
  const ra8_ui_rect_t frame    = {0, 0, (int32_t)s_fb.width_px, (int32_t)s_fb.height_px};
  ra8_widget_t        bands[3] = {};
  bands[0].vt                  = &k_er_read_sb_vt;
  bands[0].fixed               = (int16_t)k_er_statusbar_h;
  bands[0].visible             = true;
  bands[1].vt                  = &k_er_read_body_vt;
  bands[1].flex                = 1U;
  bands[1].visible             = true;
  bands[2].vt                  = &k_er_read_footer_vt;
  bands[2].fixed               = (int16_t)k_er_footer_h;
  bands[2].visible             = true;
  ra8_box_t band_scratch[4];
  (void)ra8_widget_layout_stack(bands, 3U, &frame, k_ra8_widget_axis_col, 0, 0, band_scratch, 4U);
  for (uint16_t i = 0U; i < 3U; ++i) {
    (void)ra8_widget_invalidate(&bands[i], k_ra8_widget_refresh_quality);
  }
  (void)ra8_widget_render_dirty(bands, 3U);
}

/* ===========================================================================
 * In-content navigation (#110) -- page turns + `<a href>` link following
 * =========================================================================== */

void er_push_loc(void)
{
  if (s_loc_back_count < (uint32_t)k_er_page_back_cap) {
    s_loc_back[s_loc_back_count].chapter = s_chapter_idx;
    s_loc_back[s_loc_back_count].page    = s_reading_page;
    s_loc_back_count++;
  }
}

uint32_t er_spine_count(void)
{
  return (uint32_t)k_er_spine_count;
}

bool er_apply_pageturn(er_dir_t dir)
{
  uint32_t n_chap  = s_chapter_idx;
  uint32_t n_page  = s_reading_page;
  bool     crossed = false;
  if (!er_pageturn_step(s_chapter_idx,
                        s_reading_page,
                        s_reading_pages,
                        er_spine_count(),
                        dir,
                        &n_chap,
                        &n_page,
                        &crossed)) {
    return false;
  }
  s_chapter_idx   = n_chap;
  s_reading_page  = n_page;
  s_pending_event = crossed ? k_display_event_chapter : k_display_event_turn;
  g_er_turns++;
  return true;
}

/**
 * @brief Follow a same-chapter `#fragment`: jump to the anchored page.
 * @param[in] off      Href text-pool offset.
 * @param[in] frag_off Fragment offset within the href.
 * @param[in] frag_len Fragment length.
 * @return true iff the page changed (old location pushed for Back).
 */
static bool er_nav_fragment(uint32_t off, uint32_t frag_off, uint32_t frag_len)
{
  uint32_t page = 0U;
  if (reflow_find_anchor(&s_reflow_engine,
                         (const char*)&s_reflow_engine.text_pool[off + frag_off],
                         frag_len,
                         &page) != k_ra8_ok) {
    return false;
  }
  if (page == s_reading_page) {
    return false;
  }
  er_push_loc();
  s_reading_page = page;
  return true;
}

/**
 * @brief Follow a cross-chapter link: resolve the path against the mock spine.
 * @param[in] off      Href text-pool offset.
 * @param[in] path_len Length of the path part (excluding any fragment).
 * @return true iff a different chapter was loaded (old location pushed for Back).
 */
static bool er_nav_chapter(uint32_t off, uint32_t path_len)
{
  const char*    path  = (const char*)&s_reflow_engine.text_pool[off];
  const uint32_t count = er_spine_count();
  for (uint32_t i = 0U; i < count; ++i) {
    if ((k_er_spine[i].href_len != path_len) ||
        (memcmp(path, k_er_spine[i].href, (size_t)path_len) != 0)) {
      continue;
    }
    if (i == s_chapter_idx) {
      return false;
    }
    er_push_loc();
    s_chapter_idx  = i;
    s_reading_page = 0U;
    return true;
  }
  return false;
}

bool er_reading_link_tap(int32_t x, int32_t y)
{
  const int32_t body_top = (int32_t)k_er_statusbar_h + (int32_t)k_er_body_gap;
  uint32_t      off      = 0U;
  uint32_t      len      = 0U;
  if (reflow_hit_test_link(&s_reflow_engine,
                           s_reading_page,
                           x - (int32_t)k_er_margin_x,
                           y - body_top,
                           &off,
                           &len) != k_ra8_ok) {
    return false;
  }
  reflow_href_kind_t kind     = k_reflow_href_empty;
  uint32_t           path_len = 0U;
  uint32_t           frag_off = 0U;
  uint32_t           frag_len = 0U;
  if (reflow_href_split((const char*)&s_reflow_engine.text_pool[off],
                        len,
                        &kind,
                        &path_len,
                        &frag_off,
                        &frag_len) != k_ra8_ok) {
    return false;
  }
  if (kind == k_reflow_href_fragment) {
    return er_nav_fragment(off, frag_off, frag_len);
  }
  if ((kind == k_reflow_href_chapter) || (kind == k_reflow_href_chapter_fragment)) {
    return er_nav_chapter(off, path_len);
  }
  return false;
}
