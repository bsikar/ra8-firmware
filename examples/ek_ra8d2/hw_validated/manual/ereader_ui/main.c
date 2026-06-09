/**
 * @file examples/ek_ra8d2/hw_validated/manual/ereader_ui/main.c
 * @brief E-reader device chrome -- Library + Reading screens
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * E-reader UI chrome (issue #80). The application
 * shell is laid out by the bounded box-model engine ``libs/ra_box``
 * (the #80 box model: stacks, a fixed-column grid, padding/gap, fixed
 * vs flex sizing), rendered into the GLCDC framebuffer through
 * ``libs/ra_gfx`` in the flat 16-level-grayscale language of the verified
 * "PAPYR" proof-of-concept, and navigated through the ``libs/ra_ui``
 * screen stack. Book content reflow (``libs/ra_reflow``) is a later
 * milestone; the Reading screen here still uses the bundled bitmap font.
 *
 * Two screens:
 *   - Library: status bar, toolbar (search + count), a 2-column grid of
 *     book cards (cover + title + author + reading-progress bar), and a
 *     bottom navigation strip -- all laid out by ra_box.
 *   - Reading: status bar, body text at the reading margin, footer with
 *     page label + progress bar.
 *
 * Boot: clocks/MSTP/SysTick/LEDs, then SDRAM + GLCDC (the 1024x600 RGB565
 * framebuffer lives in external SDRAM), then ra_gfx bound to it. Layout
 * is resolution-adaptive from the framebuffer the backend reports.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_box.h"
#include "ra_cgc.h"
#include "ra_display_pal.h"
#include "ra_display_pal_lcd.h"
#include "ra_err.h"
#include "ra_gfx.h"
#include "ra_gfx_font.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_panel.h"
#include "ra_panel_timing.h"
#include "ra_sdramc.h"
#include "ra_time.h"
#include "ra_touch.h"
#include "ra_ui.h"

/* ===========================================================================
 * Compile-time configuration -- typed enums per the no-magic-number rule.
 * =========================================================================== */

/**
 * @enum er_fb_dim_t
 * @brief Framebuffer dimensions, sourced from the BSP panel descriptor.
 */
typedef enum : uint16_t {
  k_er_fb_w = k_panel_width_px,  /**< Framebuffer width  (pixels). */
  k_er_fb_h = k_panel_height_px, /**< Framebuffer height (pixels). */
} er_fb_dim_t;

/**
 * @enum er_fb_misc_t
 * @brief Framebuffer byte-math and pacing constants.
 */
typedef enum : uint16_t {
  k_er_fb_align  = 64U,  /**< 64-byte AXI-burst alignment.       */
  k_er_settle_ms = 500U, /**< PLL / SDRAM / panel-POR settle.    */
  k_er_frame_ms  = 100U, /**< Heartbeat / flush pacing (ms).     */
} er_fb_misc_t;

/**
 * @enum er_color_t
 * @brief PAPYR 16-level grayscale ramp, semantic roles (0xRRGGBB).
 */
typedef enum : uint32_t {
  k_er_paper     = 0xFFFFFFU, /**< Page background (g15).             */
  k_er_ink       = 0x000000U, /**< Primary text / glyphs (g0).        */
  k_er_ink_muted = 0x555555U, /**< Secondary text / metadata (g5).    */
  k_er_rule      = 0xAAAAAAU, /**< Hairline dividers (g10).           */
  k_er_rule_soft = 0xCCCCCCU, /**< Faintest hairlines / track (g12).  */
  k_er_fill      = 0xDDDDDDU, /**< Flat fills / covers (g13).         */
  k_er_fill_deep = 0x888888U, /**< Heavier fill / progress (g8).      */
} er_color_t;

/**
 * @enum er_layout_t
 * @brief Shared chrome layout metrics (8 px grid), pixels.
 */
typedef enum : uint16_t {
  k_er_statusbar_h  = 64U,  /**< Top status-bar band height.         */
  k_er_footer_h     = 56U,  /**< Reading footer band height.         */
  k_er_nav_h        = 88U,  /**< Library bottom-nav band height.     */
  k_er_toolbar_h    = 72U,  /**< Library toolbar band height.        */
  k_er_margin_x     = 64U,  /**< Reading left/right margin.           */
  k_er_pad_ui       = 32U,  /**< Side padding for bands / grid.      */
  k_er_body_gap     = 24U,  /**< Gap between band and body text.      */
  k_er_line_h       = 26U,  /**< Body line advance.                 */
  k_er_glyph_w      = 8U,   /**< Bundled font cell width.            */
  k_er_glyph_h      = 16U,  /**< Bundled font cell height.           */
  k_er_text_inset_y = 24U,  /**< 16 px text centred in a 64 px band. */
  k_er_text_pad     = 6U,   /**< Inset for text inside a box.        */
  k_er_hair         = 1U,   /**< Hairline weight.                   */
  k_er_border_w     = 2U,   /**< Card / control border weight.       */
  k_er_progress_h   = 6U,   /**< Reading-progress bar height.        */
  k_er_progress_gap = 12U,  /**< Gap above the reading progress bar. */
  k_er_blank_lines  = 2U,   /**< Blank advance after the heading.    */
  k_er_grid_cols    = 2U,   /**< Library grid column count.           */
  k_er_grid_gap     = 28U,  /**< Library grid gap.                   */
  k_er_tile_gap     = 8U,   /**< Gap between a card's stacked parts.  */
  k_er_card_label_h = 20U,  /**< Card title / author row height.      */
  k_er_card_bar_h   = 8U,   /**< Card progress-bar height.            */
  k_er_count_w      = 200U, /**< Toolbar "N books" chip width.        */
} er_layout_t;

/**
 * @enum er_progress_t
 * @brief Reading-position + percentage constants.
 */
typedef enum : uint16_t {
  k_er_page_current = 12U,  /**< Reading current page (1-based).     */
  k_er_page_total   = 248U, /**< Reading total pages.                */
  k_er_pct_full     = 100U, /**< Percent denominator.                */
} er_progress_t;

/**
 * @enum er_node_cap_t
 * @brief Box-tree node-storage capacity.
 */
typedef enum : uint16_t {
  k_er_max_nodes = 64U, /**< Upper bound on chrome box nodes.       */
} er_node_cap_t;

/**
 * @enum er_screen_t
 * @brief Screen ids for the ra_ui navigation stack.
 */
typedef enum : uint16_t {
  k_er_screen_library = 1U, /**< Library / home grid.               */
  k_er_screen_reading = 2U, /**< Reading view.                      */
} er_screen_t;

/**
 * @enum er_action_t
 * @brief Tap-target action ids (carried on box nodes as their tag).
 */
typedef enum : uint16_t {
  k_er_act_none      = 0U, /**< Not a tap target.                   */
  k_er_act_open_book = 1U, /**< Library card -> open the reading view. */
  k_er_act_nav       = 2U, /**< Bottom-nav destination (stay in lib).  */
} er_action_t;

/**
 * @enum er_touch_cfg_t
 * @brief GT911 touch-controller wiring on the EK-RA8D2 carrier.
 *
 * @details
 * The GoodIX GT911 sits on IIC_B channel 0 at its default 7-bit address;
 * polled from the loop (IRQ pin left unset). board_sim feeds --click /
 * window taps through this same ra_touch -> I2C -> GT911 path.
 */
typedef enum : uint8_t {
  k_er_touch_channel    = 0U,    /**< IIC_B channel 0.            */
  k_er_touch_addr_7b    = 0x5DU, /**< GT911 default 7-bit addr.   */
  k_er_touch_max_points = 1U,    /**< One contact = one tap.      */
} er_touch_cfg_t;

/**
 * @enum er_hit_cap_t
 * @brief Tap-target table capacity and Reading back-affordance size.
 */
typedef enum : uint16_t {
  k_er_max_targets = 32U,  /**< Max collected tap targets.          */
  k_er_back_w      = 240U, /**< Reading back-region width (px).     */
} er_hit_cap_t;

/* ===========================================================================
 * Static content (ASCII; public-domain titles)
 * =========================================================================== */

/** @brief Wordmark / status text. */
static const char k_er_wordmark[]     = "PAPYR";
static const char k_er_lib_heading[]  = "PAPYR   Library";
static const char k_er_status_right[] = "10:24   98%";
static const char k_er_search_hint[]  = "Search";
static const char k_er_count_text[]   = "6 books";
static const char k_er_book_title[]   = "The Time Machine";
static const char k_er_page_label[]   = "Page 12 of 248";
static const char k_er_chapter[]      = "I";

/** @brief Bottom-nav destinations. */
static const char* const k_er_nav_items[] = {"Library", "Store", "Notes", "Settings"};

/**
 * @enum er_nav_count_t
 * @brief Bottom-nav destination count.
 */
typedef enum : uint16_t {
  k_er_nav_count = (uint16_t)(sizeof(k_er_nav_items) / sizeof(k_er_nav_items[0])),
} er_nav_count_t;

/**
 * @struct er_book_t
 * @brief One library entry: title, author, reading progress percent.
 */
typedef struct {
  const char* title;  /**< Book title.                 */
  const char* author; /**< Author.                     */
  uint16_t    pct;    /**< Reading progress (0..100).  */
} er_book_t;

/** @brief Demo shelf (public-domain works). */
static const er_book_t k_er_books[] = {
  {"The Time Machine", "H. G. Wells", 5U},
  {"Frankenstein", "Mary Shelley", 100U},
  {"Pride and Prejudice", "Jane Austen", 42U},
  {"Moby-Dick", "Herman Melville", 12U},
  {"The Republic", "Plato", 68U},
  {"Meditations", "M. Aurelius", 30U},
};

/**
 * @enum er_book_count_t
 * @brief Number of demo books.
 */
typedef enum : uint16_t {
  k_er_book_count = (uint16_t)(sizeof(k_er_books) / sizeof(k_er_books[0])),
} er_book_count_t;

/**
 * @brief Reading-view body paragraph lines (pre-wrapped for the font).
 */
static const char* const k_er_body_lines[] = {
  "The Time Traveller (for so it will be convenient to speak of him)",
  "was expounding a recondite matter to us. His pale grey eyes shone",
  "and twinkled, and his usually pale face was flushed and animated.",
  "",
  "The fire burnt brightly, and the soft radiance of the incandescent",
  "lights in the lilies of silver caught the bubbles that flashed and",
  "passed in our glasses. Our chairs, being his patents, embraced and",
  "caressed us rather than submitted to be sat upon -- there was that",
  "luxurious after-dinner atmosphere when thought roams gracefully free",
  "of the trammels of precision.",
};

/**
 * @enum er_body_count_t
 * @brief Number of pre-wrapped body lines.
 */
typedef enum : uint16_t {
  k_er_body_line_count = (uint16_t)(sizeof(k_er_body_lines) / sizeof(k_er_body_lines[0])),
} er_body_count_t;

/* ===========================================================================
 * Static storage
 * =========================================================================== */

/**
 * @var s_framebuffer
 * @brief RGB565 framebuffer in external SDRAM, AXI-burst aligned.
 */
static uint16_t s_framebuffer[(size_t)k_er_fb_h * (size_t)k_er_fb_w]
  __attribute__((section(".sdram_data"), aligned(k_er_fb_align)));

/** @brief Display PAL config -- LCD/GLCDC backend over the SDRAM buffer. */
static const display_cfg_t k_er_display_cfg = {
  .iface             = &k_display_backend_lcd_ra_glcdc,
  .framebuffer       = s_framebuffer,
  .framebuffer_bytes = sizeof(s_framebuffer),
  .width_px          = (uint16_t)k_er_fb_w,
  .height_px         = (uint16_t)k_er_fb_h,
  .pixfmt            = k_display_pixfmt_rgb565,
  .panel_timing      = &k_ra_panel_ek_ra8d2_timing,
};

/** @brief PAL handle returned by display_init. */
static display_handle_t* s_display = nullptr;

/** @brief Mutable copy of the FB descriptor; populated at boot. */
static display_fb_t s_fb;

/** @brief Box-tree node storage for chrome layout. */
static ra_box_t s_nodes[k_er_max_nodes];

/** @brief Per-node text label (NULL if the node draws no text). */
static const char* s_label[k_er_max_nodes];

/** @brief Per-node text colour. */
static uint32_t s_label_col[k_er_max_nodes];

/** @brief Per-node progress percent, or -1 for "not a progress bar". */
static int16_t s_progress[k_er_max_nodes];

/** @brief Navigation stack (which screen is shown). */
static ra_ui_nav_t s_nav;

/** @brief Tap targets for the current screen (rect + action id). */
static ra_ui_target_t s_targets[k_er_max_targets];

/** @brief Number of tap targets currently populated. */
static uint16_t s_target_count;

/** @brief Debounce: true while a contact is held, to fire once per tap. */
static bool s_was_touching;

/* ===========================================================================
 * Boot helpers
 * =========================================================================== */

/**
 * @brief Halt forever with the red board LED on.
 *
 * @details Stuck red LED is the visual sign of a fatal init error.
 *
 * @pre None.
 * @pre None.
 * @post Function never returns; CPU parked in a WFI loop.
 * @post Red LED is on.
 *
 * @note IRQ-safe.
 * @since 0.1.0
 */
static void app_panic_halt(void)
{
  (void)ra_board_led_on(k_ra_board_led_red);
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring up clocks, MSTP, system tick, and the two board LEDs.
 *
 * @details Same boot order the other full-panel manual demos use.
 *
 * @pre Reset_Handler ran .data/.bss init.
 * @pre IRQs are still globally disabled.
 * @post Clocks, MSTP, ra_time and both board LEDs are initialised.
 * @post Global IRQs are enabled.
 *
 * @note Not thread-safe; single-shot helper.
 * @since 0.1.0
 */
static void app_bringup_clocks(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    app_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    app_panic_halt();
  }
  if (ra_mstp_init() != k_ra_ok) {
    app_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    app_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led_blue) != k_ra_ok) {
    app_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led_red) != k_ra_ok) {
    app_panic_halt();
  }
  ra_isr_globals_enable();
}

/**
 * @brief Bring up external SDRAM and the GLCDC panel, then cache the FB.
 *
 * @details
 * Settle delay, ``ra_sdramc_init`` (the framebuffer lives at 0x68000000),
 * then ``display_init`` (panel power + GLCDC routing). Caches the FB.
 *
 * @pre ``app_bringup_clocks`` has run; IRQs are enabled.
 * @pre ``s_framebuffer`` is reachable.
 * @post SDRAM is up; GLCDC scans out ``s_framebuffer``.
 * @post ``s_display`` non-NULL and ``s_fb`` mirrors the backend FB.
 *
 * @note Not thread-safe; single-shot helper.
 * @since 0.1.0
 */
static void app_bringup_panel(void)
{
  ra_delay_ms((uint32_t)k_er_settle_ms);
  if (ra_sdramc_init() != k_ra_ok) {
    app_panic_halt();
  }
  if (display_init(&k_er_display_cfg, &s_display) != k_ra_ok) {
    app_panic_halt();
  }
  if (display_get_framebuffer(s_display, &s_fb) != k_ra_ok) {
    app_panic_halt();
  }
}

/**
 * @brief Bind ra_gfx to the panel framebuffer.
 *
 * @details After this, all chrome drawing goes through ra_gfx primitives.
 *
 * @pre ``app_bringup_panel`` has run; ``s_fb.pixels`` is reachable.
 * @pre The framebuffer is RGB565.
 * @post ra_gfx draw calls operate on ``s_framebuffer``.
 * @post On failure the app panic-halts.
 *
 * @note Not thread-safe; single-shot helper.
 * @since 0.1.0
 */
static void app_bringup_gfx(void)
{
  if (ra_gfx_init(s_fb.pixels, s_fb.width_px, s_fb.height_px, k_ra_gfx_format_rgb565) != k_ra_ok) {
    app_panic_halt();
  }
}

/**
 * @brief Open the GT911 touch controller (best-effort, polled).
 *
 * @details
 * Boards / sims without the GT911 simply return an error from
 * ``ra_touch_open``; the UI still renders, just without touch input, so
 * this is non-fatal (no panic).
 *
 * @pre ``app_bringup_clocks`` has run (IIC_B clock + MSTP up).
 * @pre None.
 * @post On success the GT911 is configured for polled reads.
 * @post On failure touch input is simply unavailable.
 *
 * @note Not thread-safe; single-shot helper.
 * @since 0.1.0
 */
static void app_bringup_touch(void)
{
  const ra_touch_cfg_t cfg = {
    .i2c_channel = (uint8_t)k_er_touch_channel,
    .target_7b   = (uint8_t)k_er_touch_addr_7b,
    .irq_pin     = (uint8_t)k_ra_touch_irq_pin_unset,
    .max_points  = (uint8_t)k_er_touch_max_points,
  };
  (void)ra_touch_open(&cfg);
}

/* ===========================================================================
 * Text helpers
 * =========================================================================== */

/**
 * @brief Draw a left-anchored text run in the bundled font.
 *
 * @param[in] x     Left pixel column.
 * @param[in] y     Top pixel row.
 * @param[in] str   ASCII string (NUL-terminated).
 * @param[in] color Foreground colour (0xRRGGBB); background is paper.
 *
 * @pre ra_gfx is bound; ``str`` is non-NULL ASCII 0x20..0x7E.
 * @pre None.
 * @post The run is blitted (clipped if needed).
 * @post Glyph cells are backed with paper-white.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_text_left(int32_t x, int32_t y, const char* str, uint32_t color)
{
  (void)ra_gfx_text_out(x, y, str, &ra_gfx_font_8x16, color, (uint32_t)k_er_paper);
}

/**
 * @brief Draw a right-anchored text run ending at column @p right.
 *
 * @param[in] right Right pixel column the run ends at.
 * @param[in] y     Top pixel row.
 * @param[in] str   ASCII string (NUL-terminated).
 * @param[in] color Foreground colour (0xRRGGBB); background is paper.
 *
 * @pre ra_gfx is bound; ``str`` is non-NULL ASCII 0x20..0x7E.
 * @pre None.
 * @post The run is right-aligned to end near @p right.
 * @post Glyph cells are backed with paper-white.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_text_right(int32_t right, int32_t y, const char* str, uint32_t color)
{
  uint32_t w = 0U;
  uint32_t h = 0U;
  if (ra_gfx_text_size(str, &ra_gfx_font_8x16, &w, &h) != k_ra_ok) {
    return;
  }
  er_text_left(right - (int32_t)w, y, str, color);
}

/* ===========================================================================
 * Library screen -- built with ra_box, rendered with ra_gfx
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
 * @param[in] fill   Fill colour, or k_ra_box_no_colour.
 * @param[in] border Border colour, or k_ra_box_no_colour.
 *
 * @return A leaf ra_box_t template.
 * @retval node Configured leaf node template.
 *
 * @pre None.
 * @pre None.
 * @post Returned node has kind leaf and the requested sizing/colours.
 * @post Tree links are left for ra_box_add to set.
 *
 * @note Pure.
 * @since 0.1.0
 */
static ra_box_t er_leaf(int16_t fixed, uint16_t flex, uint32_t fill, uint32_t border)
{
  ra_box_t n  = {};
  n.kind      = (uint8_t)k_ra_box_leaf;
  n.fixed     = fixed;
  n.flex      = flex;
  n.fill      = fill;
  n.border    = border;
  n.border_w  = (border != (uint32_t)k_ra_box_no_colour) ? (int16_t)k_er_border_w : (int16_t)0;
  n.grid_cols = 1U;
  n.tag       = (int16_t)k_ra_box_none;
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
 * @return A container ra_box_t template.
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
static ra_box_t
er_container(ra_box_kind_t kind, int16_t fixed, int16_t pad, int16_t gap, uint8_t cols)
{
  ra_box_t n  = {};
  n.kind      = (uint8_t)kind;
  n.fixed     = fixed;
  n.flex      = 1U;
  n.pad       = pad;
  n.gap       = gap;
  n.grid_cols = (cols >= 1U) ? cols : 1U;
  n.tag       = (int16_t)k_ra_box_none;
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
static void er_build_toolbar(ra_box_tree_t* tree, int16_t parent)
{
  const ra_box_t tb_t = er_container(k_ra_box_stack_h,
                                     (int16_t)k_er_toolbar_h,
                                     (int16_t)k_er_pad_ui,
                                     (int16_t)k_er_pad_ui,
                                     1U);
  const int16_t  tb   = ra_box_add(tree, parent, &tb_t);

  const ra_box_t srch_t = er_leaf(0, 1U, (uint32_t)k_ra_box_no_colour, (uint32_t)k_er_ink);
  const int16_t  srch   = ra_box_add(tree, tb, &srch_t);
  s_label[srch]         = k_er_search_hint;
  s_label_col[srch]     = (uint32_t)k_er_ink_muted;

  const ra_box_t cnt_t =
    er_leaf((int16_t)k_er_count_w, 0U, (uint32_t)k_ra_box_no_colour, (uint32_t)k_er_ink);
  const int16_t cnt = ra_box_add(tree, tb, &cnt_t);
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
static void er_add_book_tile(ra_box_tree_t* tree, int16_t grid, const er_book_t* book)
{
  ra_box_t tile_t    = er_container(k_ra_box_stack_v, 0, 0, (int16_t)k_er_tile_gap, 1U);
  tile_t.tag         = (int16_t)k_er_act_open_book; /* the whole card is a tap target */
  const int16_t tile = ra_box_add(tree, grid, &tile_t);

  const ra_box_t cover_t = er_leaf(0, 1U, (uint32_t)k_er_fill, (uint32_t)k_er_ink);
  (void)ra_box_add(tree, tile, &cover_t);

  const ra_box_t lbl_t = er_leaf((int16_t)k_er_card_label_h,
                                 0U,
                                 (uint32_t)k_ra_box_no_colour,
                                 (uint32_t)k_ra_box_no_colour);
  const int16_t  title = ra_box_add(tree, tile, &lbl_t);
  s_label[title]       = book->title;
  const int16_t auth   = ra_box_add(tree, tile, &lbl_t);
  s_label[auth]        = book->author;
  s_label_col[auth]    = (uint32_t)k_er_ink_muted;

  const ra_box_t bar_t =
    er_leaf((int16_t)k_er_card_bar_h, 0U, (uint32_t)k_er_rule_soft, (uint32_t)k_ra_box_no_colour);
  const int16_t bar = ra_box_add(tree, tile, &bar_t);
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
static void er_build_nav(ra_box_tree_t* tree, int16_t parent)
{
  const ra_box_t nav_t = er_container(k_ra_box_stack_h, (int16_t)k_er_nav_h, 0, 0, 1U);
  const int16_t  nav   = ra_box_add(tree, parent, &nav_t);
  for (uint16_t i = 0U; i < (uint16_t)k_er_nav_count; ++i) {
    const ra_box_t item_t =
      er_leaf(0, 1U, (uint32_t)k_ra_box_no_colour, (uint32_t)k_ra_box_no_colour);
    const int16_t item = ra_box_add(tree, nav, &item_t);
    s_label[item]      = k_er_nav_items[i];
    s_label_col[item]  = (i == 0U) ? (uint32_t)k_er_ink : (uint32_t)k_er_ink_muted;
  }
}

/**
 * @brief Build the Library box tree (status bar, toolbar, grid, nav).
 *
 * @param[in,out] tree  Tree builder bound to s_nodes.
 * @param[in]     frame Screen rectangle to lay out within.
 *
 * @pre ra_gfx is bound; @p tree references s_nodes.
 * @pre s_book table populated.
 * @post `tree` holds the laid-out Library; side tables hold text/progress.
 * @post Every node has its rect computed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_build_library(ra_box_tree_t* tree, const ra_ui_rect_t* frame)
{
  er_reset_side_tables();
  (void)ra_box_tree_init(tree, s_nodes, (uint16_t)k_er_max_nodes);

  const ra_box_t root_t = er_container(k_ra_box_stack_v, 0, 0, 0, 1U);
  const int16_t  root   = ra_box_add(tree, (int16_t)k_ra_box_none, &root_t);

  const ra_box_t sb_t = er_leaf((int16_t)k_er_statusbar_h,
                                0U,
                                (uint32_t)k_ra_box_no_colour,
                                (uint32_t)k_ra_box_no_colour);
  const int16_t  sb   = ra_box_add(tree, root, &sb_t);
  s_label[sb]         = k_er_lib_heading;

  er_build_toolbar(tree, root);

  const ra_box_t grid_t = er_container(k_ra_box_grid,
                                       0,
                                       (int16_t)k_er_pad_ui,
                                       (int16_t)k_er_grid_gap,
                                       (uint8_t)k_er_grid_cols);
  const int16_t  grid   = ra_box_add(tree, root, &grid_t);
  for (uint16_t i = 0U; i < (uint16_t)k_er_book_count; ++i) {
    er_add_book_tile(tree, grid, &k_er_books[i]);
  }

  er_build_nav(tree, root);

  (void)ra_box_layout(tree, root, frame);
}

/**
 * @brief Render a laid-out box tree (fills, borders, progress, labels).
 *
 * @param[in] tree Laid-out tree whose nodes index the side tables.
 *
 * @pre ra_gfx is bound; @p tree laid out; side tables match its nodes.
 * @pre None.
 * @post Every node's fill/border/progress/label is drawn.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_render_boxtree(const ra_box_tree_t* tree)
{
  for (uint16_t i = 0U; i < tree->count; ++i) {
    const ra_box_t*    n = &tree->nodes[i];
    const ra_ui_rect_t r = n->rect;
    if (n->fill != (uint32_t)k_ra_box_no_colour) {
      (void)ra_gfx_rect(r.x, r.y, r.w, r.h, n->fill, true);
    }
    if ((n->border_w > 0) && (n->border != (uint32_t)k_ra_box_no_colour)) {
      (void)ra_gfx_rect(r.x, r.y, r.w, r.h, n->border, false);
    }
    if (s_progress[i] >= 0) {
      const int32_t fillw = (r.w * (int32_t)s_progress[i]) / (int32_t)k_er_pct_full;
      (void)ra_gfx_rect(r.x, r.y, fillw, r.h, (uint32_t)k_er_fill_deep, true);
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
static void er_collect_targets(const ra_box_tree_t* tree)
{
  s_target_count = 0U;
  for (uint16_t i = 0U; i < tree->count; ++i) {
    const ra_box_t* n = &tree->nodes[i];
    if ((n->tag != (int16_t)k_ra_box_none) && (s_target_count < (uint16_t)k_er_max_targets)) {
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
 * @pre ra_gfx is bound; ``s_fb`` reflects the framebuffer geometry.
 * @pre None.
 * @post The framebuffer holds the Library screen; ``s_targets`` set.
 * @post Caller flushes the panel to make it visible.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_render_library(void)
{
  (void)ra_gfx_clear((uint32_t)k_er_paper);
  ra_box_tree_t      tree;
  const ra_ui_rect_t frame = {0, 0, (int32_t)s_fb.width_px, (int32_t)s_fb.height_px};
  er_build_library(&tree, &frame);
  er_collect_targets(&tree);
  er_render_boxtree(&tree);
  /* Hairline under the status bar and above the nav, for separation. */
  (void)ra_gfx_rect(0,
                    (int32_t)k_er_statusbar_h - (int32_t)k_er_hair,
                    (int32_t)s_fb.width_px,
                    (int32_t)k_er_hair,
                    (uint32_t)k_er_rule,
                    true);
  (void)ra_gfx_rect(0,
                    (int32_t)s_fb.height_px - (int32_t)k_er_nav_h,
                    (int32_t)s_fb.width_px,
                    (int32_t)k_er_hair,
                    (uint32_t)k_er_rule,
                    true);
  er_text_right((int32_t)s_fb.width_px - (int32_t)k_er_pad_ui,
                (int32_t)k_er_text_inset_y,
                k_er_status_right,
                (uint32_t)k_er_ink_muted);
}

/* ===========================================================================
 * Reading screen -- immediate-mode ra_gfx (book reflow is a later milestone)
 * =========================================================================== */

/**
 * @brief Paint the Reading body: chapter heading then paragraph lines.
 *
 * @param[in] height Framebuffer height in pixels.
 *
 * @pre ra_gfx is bound.
 * @pre @p height matches the bound framebuffer.
 * @post Chapter heading and as many body lines as fit are drawn in ink.
 * @post Drawing stops before the footer band.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_draw_reading_body(int32_t height)
{
  const int32_t body_top = (int32_t)k_er_statusbar_h + (int32_t)k_er_body_gap;
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
}

/**
 * @brief Render the full Reading screen (status bar, body, footer).
 *
 * @pre ra_gfx is bound; ``s_fb`` reflects the framebuffer geometry.
 * @pre None.
 * @post The framebuffer holds the Reading screen.
 * @post Caller flushes the panel to make it visible.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_render_reading(void)
{
  const int32_t width  = (int32_t)s_fb.width_px;
  const int32_t height = (int32_t)s_fb.height_px;

  (void)ra_gfx_clear((uint32_t)k_er_paper);

  /* Status bar. */
  er_text_left((int32_t)k_er_pad_ui, (int32_t)k_er_text_inset_y, k_er_wordmark, (uint32_t)k_er_ink);
  const int32_t title_x =
    (int32_t)k_er_pad_ui + ((int32_t)sizeof(k_er_wordmark) * (int32_t)k_er_glyph_w);
  er_text_left(title_x, (int32_t)k_er_text_inset_y, k_er_book_title, (uint32_t)k_er_ink_muted);
  er_text_right(width - (int32_t)k_er_pad_ui,
                (int32_t)k_er_text_inset_y,
                k_er_status_right,
                (uint32_t)k_er_ink_muted);
  (void)ra_gfx_rect(0,
                    (int32_t)k_er_statusbar_h - (int32_t)k_er_hair,
                    width,
                    (int32_t)k_er_hair,
                    (uint32_t)k_er_rule,
                    true);

  er_draw_reading_body(height);

  /* Footer: rule, chapter title, page label, progress bar. */
  const int32_t band_top = height - (int32_t)k_er_footer_h;
  (void)ra_gfx_rect(0, band_top, width, (int32_t)k_er_hair, (uint32_t)k_er_rule, true);
  const int32_t text_y = band_top + (int32_t)k_er_progress_gap;
  er_text_left((int32_t)k_er_pad_ui, text_y, k_er_book_title, (uint32_t)k_er_ink_muted);
  er_text_right(width - (int32_t)k_er_pad_ui, text_y, k_er_page_label, (uint32_t)k_er_ink_muted);
  const int32_t track_x = (int32_t)k_er_pad_ui;
  const int32_t track_w = width - (2 * (int32_t)k_er_pad_ui);
  const int32_t track_y = height - (int32_t)k_er_progress_gap - (int32_t)k_er_progress_h;
  (void)ra_gfx_rect(track_x,
                    track_y,
                    track_w,
                    (int32_t)k_er_progress_h,
                    (uint32_t)k_er_rule_soft,
                    true);
  const int32_t fill_w = (track_w * (int32_t)k_er_page_current) / (int32_t)k_er_page_total;
  (void)
    ra_gfx_rect(track_x, track_y, fill_w, (int32_t)k_er_progress_h, (uint32_t)k_er_fill_deep, true);
}

/**
 * @brief Render whichever screen is on top of the navigation stack.
 *
 * @pre ra_gfx is bound; ``s_nav`` initialised.
 * @pre None.
 * @post The framebuffer holds the current screen.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_render_current(void)
{
  uint16_t top = (uint16_t)k_er_screen_library;
  (void)ra_ui_nav_top(&s_nav, &top);
  if (top == (uint16_t)k_er_screen_reading) {
    er_render_reading();
  } else {
    er_render_library();
  }
}

/**
 * @brief Route a tap to a navigation action for the current screen.
 *
 * @param[in] x Tap X (panel pixels).
 * @param[in] y Tap Y (panel pixels).
 *
 * @return true if the tap changed the screen (caller should re-render).
 * @retval true  Navigation stack changed.
 * @retval false Tap hit nothing actionable.
 *
 * @pre ra_gfx is bound; ``s_nav`` initialised; targets reflect the screen.
 * @pre None.
 * @post On a Library card tap the Reading view is pushed; on a Reading
 *       back-region tap it is popped.
 * @post On no hit the stack is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool er_handle_tap(int32_t x, int32_t y)
{
  uint16_t top = (uint16_t)k_er_screen_library;
  (void)ra_ui_nav_top(&s_nav, &top);
  if (top == (uint16_t)k_er_screen_reading) {
    const ra_ui_rect_t back = {0, 0, (int32_t)k_er_back_w, (int32_t)k_er_statusbar_h};
    if (ra_ui_rect_contains(&back, x, y)) {
      uint16_t prev = 0U;
      return (ra_ui_nav_pop(&s_nav, &prev) == k_ra_ok);
    }
    return false;
  }
  uint16_t action = (uint16_t)k_er_act_none;
  bool     hit    = false;
  (void)ra_ui_hit_test(s_targets, s_target_count, x, y, &action, &hit);
  if (hit && (action == (uint16_t)k_er_act_open_book)) {
    return (ra_ui_nav_push(&s_nav, (uint16_t)k_er_screen_reading) == k_ra_ok);
  }
  return false;
}

/**
 * @brief Poll the touch controller and dispatch a tap on a fresh press.
 *
 * @details Edge-triggered: acts once when a contact first appears, so a
 *          held finger does not repeat. Re-renders + flushes on a change.
 *
 * @pre ra_gfx bound; ``s_nav`` initialised; ``s_display`` valid.
 * @pre None.
 * @post On a press that changes the screen, the new screen is shown.
 * @post ``s_was_touching`` tracks the contact state.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_poll_touch(void)
{
  ra_touch_point_t pt  = {};
  uint8_t          got = 0U;
  if (ra_touch_read(&pt, (uint8_t)k_er_touch_max_points, &got) != k_ra_ok) {
    return;
  }
  const bool touching = (got > 0U);
  if (touching && !s_was_touching) {
    if (er_handle_tap((int32_t)pt.x, (int32_t)pt.y)) {
      er_render_current();
      (void)display_flush(s_display, display_full_rect(s_display), k_display_refresh_quality);
    }
  }
  s_was_touching = touching;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  app_bringup_clocks();
  app_bringup_panel();
  app_bringup_gfx();
  app_bringup_touch();

  (void)ra_ui_nav_init(&s_nav, (uint16_t)k_er_screen_library);
  er_render_current();
  (void)display_flush(s_display, display_full_rect(s_display), k_display_refresh_init);

  while (1) {
    er_poll_touch();
    (void)display_flush(s_display, display_full_rect(s_display), k_display_refresh_fast);
    (void)ra_board_led_toggle(k_ra_board_led_blue);
    ra_delay_ms((uint32_t)k_er_frame_ms);
  }
  return 0;
}
#pragma GCC diagnostic pop
