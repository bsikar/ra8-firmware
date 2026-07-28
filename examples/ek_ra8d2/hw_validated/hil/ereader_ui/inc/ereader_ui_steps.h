/**
 * @file examples/ek_ra8d2/hw_validated/hil/ereader_ui/inc/ereader_ui_steps.h
 * @brief E-reader UI chrome -- shared contract between main.c and the
 *        per-aspect helper translation units (screens + input).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * ``ereader_ui/main.c`` was split into cohesive translation units to keep every
 * source file under the repository's 1000-line cap. This header is the seam
 * between them: it owns the compile-time typed-enum configuration, the small
 * shared structs, the ``extern`` declarations of the mutable file-scope state
 * and read-only data tables that main.c defines (single definition site), and
 * the prototypes of the helper functions moved out of main.c. The split is a
 * pure code move; the composited chrome is byte-identical.
 *
 * Translation units sharing this contract:
 *   - ``main.c``               boot, app framework, screen dispatch, main loop.
 *   - ``ereader_ui_screens.c`` Library + Reading rendering + in-content nav.
 *   - ``ereader_ui_input.c``   keyboard rendering, battery nag, tap + button
 *                              polling.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_app.h"
#include "ra8_batt.h"
#include "ra8_box.h"
#include "ra8_display_pal.h"
#include "ra8_display_pal_policy.h"
#include "ra8_keyboard.h"
#include "ra8_panel_timing.h"
#include "ra8_port_constants.h"
#include "ra8_reflow.h"
#include "ra8_ui.h"
#include "ra8_widget.h"

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
  k_er_fb_align  = 64U,  /**< 64-byte AXI-burst alignment.                    */
  k_er_settle_ms = 500U, /**< PLL / SDRAM / panel-POR settle.                 */
  k_er_frame_ms  = 25U,  /**< Input poll period (ms): touch / button cadence. */
  k_er_led_every = 16U,  /**< Toggle the heartbeat LED every Nth frame.       */
} er_fb_misc_t;

/**
 * @enum er_color_t
 * @brief PAPYR 16-level grayscale ramp, semantic roles (0xRRGGBB).
 */
typedef enum : uint32_t {
  k_er_paper     = 0xFFFFFFU, /**< Page background (g15).            */
  k_er_ink       = 0x000000U, /**< Primary text / glyphs (g0).       */
  k_er_ink_muted = 0x555555U, /**< Secondary text / metadata (g5).   */
  k_er_rule      = 0xAAAAAAU, /**< Hairline dividers (g10).          */
  k_er_rule_soft = 0xCCCCCCU, /**< Faintest hairlines / track (g12). */
  k_er_fill      = 0xDDDDDDU, /**< Flat fills / covers (g13).        */
  k_er_fill_deep = 0x888888U, /**< Heavier fill / progress (g8).     */
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
  k_er_margin_x     = 64U,  /**< Reading left/right margin.          */
  k_er_pad_ui       = 32U,  /**< Side padding for bands / grid.      */
  k_er_body_gap     = 24U,  /**< Gap between band and body text.     */
  k_er_line_h       = 26U,  /**< Body line advance.                  */
  k_er_glyph_w      = 8U,   /**< Bundled font cell width.            */
  k_er_glyph_h      = 16U,  /**< Bundled font cell height.           */
  k_er_text_inset_y = 24U,  /**< 16 px text centred in a 64 px band. */
  k_er_text_pad     = 6U,   /**< Inset for text inside a box.        */
  k_er_hair         = 1U,   /**< Hairline weight.                    */
  k_er_border_w     = 2U,   /**< Card / control border weight.       */
  k_er_progress_h   = 6U,   /**< Reading-progress bar height.        */
  k_er_progress_gap = 12U,  /**< Gap above the reading progress bar. */
  k_er_blank_lines  = 2U,   /**< Blank advance after the heading.    */
  k_er_grid_cols    = 2U,   /**< Library grid column count.          */
  k_er_grid_gap     = 28U,  /**< Library grid gap.                   */
  k_er_tile_gap     = 8U,   /**< Gap between a card's stacked parts. */
  k_er_card_label_h = 20U,  /**< Card title / author row height.     */
  k_er_card_bar_h   = 8U,   /**< Card progress-bar height.           */
  k_er_count_w      = 200U, /**< Toolbar "N books" chip width.       */
} er_layout_t;

/**
 * @enum er_progress_t
 * @brief Reading-position + percentage constants.
 */
typedef enum : uint16_t {
  k_er_page_current = 12U,  /**< Reading current page (1-based). */
  k_er_page_total   = 248U, /**< Reading total pages.            */
  k_er_pct_full     = 100U, /**< Percent denominator.            */
} er_progress_t;

/**
 * @enum er_node_cap_t
 * @brief Box-tree node-storage capacity.
 */
typedef enum : uint16_t {
  k_er_max_nodes = 64U, /**< Upper bound on chrome box nodes. */
} er_node_cap_t;

/**
 * @enum er_screen_t
 * @brief Screen ids for the ra8_ui navigation stack.
 */
typedef enum : uint16_t {
  k_er_screen_library  = 1U, /**< Library / home grid.                           */
  k_er_screen_reading  = 2U, /**< Reading view.                                  */
  k_er_screen_keyboard = 3U, /**< On-screen keyboard (search entry).             */
  k_er_screen_settings = 4U, /**< Settings (optional app, #if RA8_APP_SETTINGS). */
} er_screen_t;

/**
 * @enum er_action_t
 * @brief Tap-target action ids (carried on box nodes as their tag).
 *
 * @details
 * Keyboard key taps use the contiguous block ``k_er_act_key_base ..
 * k_er_act_key_base + 28`` so the dispatcher recovers the key index by
 * subtraction; nothing else may occupy that range.
 */
typedef enum : uint16_t {
  k_er_act_none      = 0U,   /**< Not a tap target.                      */
  k_er_act_open_book = 1U,   /**< Library card -> open the reading view. */
  k_er_act_nav       = 2U,   /**< Bottom-nav destination (stay in lib).  */
  k_er_act_search    = 3U,   /**< Toolbar Search -> open the keyboard.   */
  k_er_act_settings  = 4U,   /**< Bottom-nav Settings -> Settings app.   */
  k_er_act_key_base  = 100U, /**< Keyboard key i -> base + i.            */
} er_action_t;

/**
 * @enum er_touch_cfg_t
 * @brief GT911 touch-controller wiring on the EK-RA8D2 carrier.
 *
 * @details
 * The GoodIX GT911 sits on IIC_B channel 0 at its default 7-bit address;
 * polled from the loop (IRQ pin left unset). ra8_emulator feeds --click /
 * window taps through this same ra8_touch -> I2C -> GT911 path.
 */
typedef enum : uint8_t {
  k_er_touch_channel    = 0U,    /**< IIC_B channel 0.          */
  k_er_touch_addr_7b    = 0x5DU, /**< GT911 default 7-bit addr. */
  k_er_touch_max_points = 1U,    /**< One contact = one tap.    */
} er_touch_cfg_t;

/**
 * @enum er_touch_bus_cfg_t
 * @brief Clocking for the app-owned touch/fuel-gauge I2C bus.
 *
 * @details
 * The app brings the IIC_B peripheral up itself (fast-mode, PCLKA
 * source) and hands the bound bus to the touch driver through its
 * injected seam; the fuel-gauge reads share the same bring-up.
 */
typedef enum : uint32_t {
  k_er_touch_bus_hz   = 400000U,   /**< Fast-mode I2C clock.     */
  k_er_touch_pclka_hz = 60000000U, /**< IIC_B clock-source rate. */
} er_touch_bus_cfg_t;

/**
 * @enum er_fg_cfg_t
 * @brief MAX17048-class fuel gauge wiring (shares the touch IIC_B bus).
 *
 * @details The fuel gauge sits at 7-bit 0x36 on the same IIC_B channel 0 the
 * GT911 touch already brought up, so the battery is read with raw ra8_i3c
 * register reads (no second bus init). ra8_emulator models this gauge and drives
 * its SOC / CRATE from the on-screen battery slider.
 */
typedef enum : uint8_t {
  k_er_fg_addr      = 0x36U, /**< Fuel gauge 7-bit I2C address.         */
  k_er_fg_reg_soc   = 0x04U, /**< SOC register (high byte = percent).   */
  k_er_fg_reg_crate = 0x16U, /**< CRATE register (sign = charge dir).   */
  k_er_fg_sign      = 0x80U, /**< CRATE high-byte sign bit (discharge). */
} er_fg_cfg_t;

/**
 * @enum er_nag_cfg_t
 * @brief Low-battery nag-banner geometry.
 */
typedef enum : int32_t {
  k_er_nag_h        = 72, /**< Banner height (px).                     */
  k_er_nag_margin   = 24, /**< Banner inset from the screen sides.     */
  k_er_nag_top      = 96, /**< Banner top y (clear of the status bar). */
  k_er_nag_text_dx  = 28, /**< Text inset from the banner left.        */
  k_er_nag_text_dy  = 28, /**< Text inset from the banner top.         */
  k_er_nag_dec_max  = 4,  /**< Scratch for a "NNN%" decimal tail.      */
  k_er_nag_dec_ten  = 10, /**< Decimal base.                           */
  k_er_nag_line_max = 48, /**< Banner text buffer size (bounds copy).  */
  k_er_batt_every   = 40, /**< Poll the gauge every Nth frame (~1/s).  */
} er_nag_cfg_t;

/** @enum er_nag_color_t @brief Nag-banner palette (grayscale e-ink). */
typedef enum : uint32_t {
  k_er_nag_low_bg  = 0x555555U, /**< Low-battery banner fill (g5).    */
  k_er_nag_crit_bg = 0x000000U, /**< Critical banner fill (g0 ink).   */
  k_er_nag_text    = 0xFFFFFFU, /**< Banner text (paper on the fill). */
} er_nag_color_t;

/**
 * @enum er_hit_cap_t
 * @brief Tap-target table capacity and Reading back-affordance size.
 */
typedef enum : uint16_t {
  k_er_max_targets   = 32U,  /**< Max collected tap targets.      */
  k_er_back_w        = 240U, /**< Reading back-region width (px). */
  k_er_page_back_cap = 16U,  /**< Footnote-jump back-stack depth. */
} er_hit_cap_t;

/**
 * @struct er_book_t
 * @brief One library entry: title, author, reading progress percent.
 */
typedef struct {
  const char* title;  /**< Book title.                */
  const char* author; /**< Author.                    */
  uint16_t    pct;    /**< Reading progress (0..100). */
} er_book_t;

/**
 * @struct er_chapter_t
 * @brief One mock-spine chapter: its XHTML, length, and resolving href.
 */
typedef struct {
  const char* xhtml;    /**< Chapter XHTML body.               */
  uint32_t    len;      /**< Length of @ref xhtml (excl. NUL). */
  const char* href;     /**< Manifest href that links to it.   */
  uint32_t    href_len; /**< Length of @ref href (excl. NUL).  */
} er_chapter_t;

/**
 * @struct er_loc_t
 * @brief A reading location (chapter + page) for the navigation back-stack.
 */
typedef struct {
  uint32_t chapter; /**< Spine chapter index. */
  uint32_t page;    /**< Page within chapter. */
} er_loc_t;

/**
 * @enum er_reflow_cfg_t
 * @brief SD-font load + ra8_reflow body-render tunables (no magic numbers).
 */
typedef enum : uint32_t {
  k_er_font_cap    = 512U * 1024U,       /**< Max font read off the card (bytes).   */
  k_er_font_min    = 16U,                /**< Smallest plausible font blob (bytes). */
  k_er_reflow_px   = 22U,                /**< Body type size (pixels).              */
  k_er_spi_chan    = 0U,                 /**< Pmod2 / J25 = SCI0 Simple-SPI.        */
  k_er_reflow_ink  = 0xFF101010U,        /**< Body text colour (near-black ARGB).   */
  k_er_reflow_link = 0xFF2A52BEU,        /**< Anchor colour (cerulean ARGB).        */
  k_er_img_arena   = 2U * 1024U * 1024U, /**< SDRAM image-decode scratch.           */

  /* Layer-3 glyph atlas (#164): cache rasterised body glyphs in SDRAM so page
   * turns reuse them instead of re-running stb_truetype every frame. */
  k_er_glyph_cell_px    = 64U,                                     /**< Cell edge, px (>= body). */
  k_er_glyph_cell_bytes = k_er_glyph_cell_px * k_er_glyph_cell_px, /**< Bytes per cell.          */
  k_er_glyph_cells      = 256U,                                    /**< Glyph-cache budget.      */
  k_er_glyph_buckets    = 512U,                                    /**< Glyph-cache hash buckets. */
} er_reflow_cfg_t;

/** @enum er_reflow_sentinel_t @brief "No chapter cached" sentinel. */
typedef enum : uint32_t {
  k_er_chapter_none = 0xFFFFFFFFU, /**< s_reflow_chapter when nothing is laid out. */
} er_reflow_sentinel_t;

/* ===========================================================================
 * Shared read-only content tables (defined in main.c -- single site).
 * =========================================================================== */

/**
 * @brief Wordmark / status text. Complete bound so ::er_read_sb_render can take
 *        ``sizeof`` to advance past it (a static_assert in main.c pins the size).
 */
extern const char k_er_wordmark[6];
extern const char k_er_lib_heading[];  /**< Library heading text.    */
extern const char k_er_status_right[]; /**< Status-bar right text.   */
extern const char k_er_search_hint[];  /**< Search placeholder.      */
extern const char k_er_count_text[];   /**< Book-count chip text.    */
extern const char k_er_book_title[];   /**< Reading title text.      */
extern const char k_er_page_label[];   /**< Reading page label.      */
extern const char k_er_chapter[];      /**< Bitmap-fallback heading. */

/** @brief Bottom-nav destinations (Library / Store / Notes / Settings). */
extern const char* const k_er_nav_items[];
/**
 * @enum er_nav_count_t
 * @brief Bottom-nav destination count.
 */
typedef enum : uint16_t {
  k_er_nav_count       = 4U, /**< Number of bottom-nav destinations. */
  k_er_nav_idx_setting = 3U, /**< Index of the "Settings" nav item.  */
} er_nav_count_t;

/** @brief Demo shelf (public-domain works). */
extern const er_book_t k_er_books[];
/**
 * @enum er_book_count_t
 * @brief Number of demo books.
 */
typedef enum : uint16_t {
  k_er_book_count = 6U, /**< Number of demo books. */
} er_book_count_t;

/** @brief Reading-view body paragraph lines (pre-wrapped for the font). */
extern const char* const k_er_body_lines[];
/**
 * @enum er_body_count_t
 * @brief Number of pre-wrapped body lines.
 */
typedef enum : uint16_t {
  k_er_body_line_count = 10U, /**< Number of pre-wrapped body lines. */
} er_body_count_t;

/** @brief Two-chapter mock spine for in-content cross-chapter navigation (#110). */
extern const er_chapter_t k_er_spine[];
/**
 * @enum er_spine_count_t
 * @brief Number of chapters in the mock spine.
 */
typedef enum : uint16_t {
  k_er_spine_count = 2U, /**< Number of chapters in k_er_spine. */
} er_spine_count_t;

/* ===========================================================================
 * Shared mutable file-scope state (defined in main.c -- single site).
 * =========================================================================== */

extern display_fb_t         s_fb;              /**< Mutable copy of the FB descriptor.    */
extern ra8_ui_target_t      s_targets[];       /**< Tap targets for the current screen.   */
extern uint16_t             s_target_count;    /**< Tap targets currently populated.      */
extern ra8_kbd_layout_t     s_kb;              /**< On-screen keyboard grid.              */
extern ra8_kbd_text_t       s_query;           /**< Live search query.                    */
extern uint32_t             s_chapter_idx;     /**< Reading chapter index.                */
extern uint32_t             s_reading_page;    /**< Reading current reflow page.          */
extern uint32_t             s_reading_pages;   /**< Reading total reflow pages (>= 1).    */
extern ra8_reflow_t         s_reflow_engine;   /**< ra8_reflow engine for the body.       */
extern bool                 s_reflow_open;     /**< True while the engine holds a layout. */
extern uint32_t             s_reflow_chapter;  /**< Chapter laid out (cache key).         */
extern int32_t              s_reflow_w;        /**< Body width the layout used.           */
extern int32_t              s_reflow_h;        /**< Body height the layout used.          */
extern uint8_t              s_font_buf[];      /**< Font blob read off the SD card.       */
extern uint32_t             s_font_len;        /**< Bytes of font read off the card.      */
extern bool                 s_have_font;       /**< True once an SD font is loaded.       */
extern uint8_t              s_img_arena_buf[]; /**< Image-decode bump arena in SDRAM.     */
extern er_loc_t             s_loc_back[];      /**< Reading back-stack for link jumps.    */
extern uint32_t             s_loc_back_count;  /**< Entries used in s_loc_back.           */
extern ra8_ui_nav_t         s_nav;             /**< Navigation stack (active screen).     */
extern ra8_batt_nag_t       s_batt_nag;        /**< Nag currently shown over the chrome.  */
extern uint8_t              s_batt_soc;        /**< Last fuel-gauge SOC percent.          */
extern ra8_batt_monitor_t   s_batt_mon;        /**< Low-battery nag policy state.         */
extern bool                 s_nag_region_only; /**< True when a tap only toggled the nag. */
extern display_turn_event_t s_pending_event;   /**< Pending refresh event for next flush. */

/** @brief SWD / `--dump-sym` telemetry for the headless page-turn HIL (#78). */
extern volatile uint32_t g_er_cur_page;  /**< Current reading page after the last turn. */
extern volatile uint32_t g_er_turns;     /**< Count of page turns applied since boot.   */
extern volatile uint32_t g_er_last_hint; /**< Last `display_refresh_hint_t` flushed.    */

/* ===========================================================================
 * Text helpers (ereader_ui_screens.c) -- shared by every band renderer.
 * =========================================================================== */

/**
 * @brief Draw a left-anchored text run in the bundled font.
 *
 * @param[in] x     Left pixel column.
 * @param[in] y     Top pixel row.
 * @param[in] str   ASCII string (NUL-terminated).
 * @param[in] color Foreground colour (0xRRGGBB); background is paper.
 *
 * @pre ra8_gfx is bound; ``str`` is non-NULL ASCII 0x20..0x7E.
 * @pre None.
 * @post The run is blitted (clipped if needed).
 * @post Glyph cells are backed with paper-white.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
void er_text_left(int32_t x, int32_t y, const char* str, uint32_t color);

/**
 * @brief Draw a right-anchored text run ending at column @p right.
 *
 * @param[in] right Right pixel column the run ends at.
 * @param[in] y     Top pixel row.
 * @param[in] str   ASCII string (NUL-terminated).
 * @param[in] color Foreground colour (0xRRGGBB); background is paper.
 *
 * @pre ra8_gfx is bound; ``str`` is non-NULL ASCII 0x20..0x7E.
 * @pre None.
 * @post The run is right-aligned to end near @p right.
 * @post Glyph cells are backed with paper-white.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
void er_text_right(int32_t right, int32_t y, const char* str, uint32_t color);

/* ===========================================================================
 * Screen rendering + in-content navigation (ereader_ui_screens.c).
 * =========================================================================== */

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
void er_render_library(void);

/**
 * @brief Render the full Reading screen (status bar, body, footer).
 *
 * @pre ra8_gfx is bound; ``s_fb`` reflects the framebuffer geometry.
 * @pre None.
 * @post The framebuffer holds the Reading screen.
 * @post Caller flushes the panel to make it visible.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
void er_render_reading(void);

/**
 * @brief Apply a page turn to the reading location (touch + button shared path).
 *
 * @param[in] dir Direction (::k_er_dir_prev / ::k_er_dir_next / ::k_er_dir_none).
 * @return true iff the reading location changed (caller must re-render).
 * @retval true  A page turn was applied.
 * @retval false No turn (book end, or ::k_er_dir_none).
 * @pre The Reading view is active and a chapter is laid out (::s_reading_pages).
 * @pre ::s_chapter_idx / ::s_reading_page are the visible location.
 * @post On true the location advanced and ::s_pending_event reflects the kind.
 * @post On false no state changed.
 * @note Not thread-safe.
 * @since 0.1.0
 */
bool er_apply_pageturn(er_dir_t dir);

/**
 * @brief Follow an in-content `<a>` tap in the Reading body (#110).
 *
 * @param[in] x Tap X (panel pixels).
 * @param[in] y Tap Y (panel pixels).
 * @return true iff the tap followed a link and changed the reading location.
 * @retval true  A link was followed; chapter/page updated.
 * @retval false No link hit, or an unresolved / external target.
 * @pre The Reading body was laid out (link rects reflect the chapter).
 * @pre ::s_reading_page is the visible page.
 * @post On true the reading location changed and the old one is on ::s_loc_back.
 * @note Not thread-safe.
 * @since 0.1.0
 */
bool er_reading_link_tap(int32_t x, int32_t y);

/**
 * @brief Push the current (chapter, page) on the back-stack, capacity permitting.
 *
 * @pre ::s_chapter_idx / ::s_reading_page are the visible location.
 * @pre None.
 * @post On free capacity the location is appended to ::s_loc_back.
 * @post ::s_loc_back_count is bounded by ::k_er_page_back_cap.
 * @note Not thread-safe.
 * @since 0.1.0
 */
void er_push_loc(void);

/**
 * @brief Number of chapters in the (mock) spine.
 *
 * @return Chapter count of ::k_er_spine.
 * @retval count The number of spine chapters.
 * @pre ::k_er_spine is a valid table.
 * @pre None.
 * @post No state mutated.
 * @post Returned value is >= 1.
 * @note Pure.
 * @since 0.1.0
 */
uint32_t er_spine_count(void);

/* ===========================================================================
 * Keyboard rendering (ereader_ui_input.c).
 * =========================================================================== */

/**
 * @brief Render the on-screen keyboard screen + collect its key targets.
 *
 * @pre ra8_gfx is bound; ``s_fb`` reflects the framebuffer geometry.
 * @pre None.
 * @post The framebuffer holds the keyboard; ``s_kb`` + ``s_targets`` set.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
void er_render_keyboard(void);

/* ===========================================================================
 * Battery nag + input polling (ereader_ui_input.c).
 * =========================================================================== */

/**
 * @brief Nag banner overlay widget: a centered low-battery toast over the screen.
 *
 * @param[in,out] w Widget instance (unused; the banner reads file-scope state).
 *
 * @pre ra8_gfx is bound; ``s_fb`` reflects the framebuffer geometry.
 * @pre None.
 * @post On an active nag the banner rect + text is drawn; else nothing.
 * @post No navigation state mutated.
 * @note Not thread-safe.
 * @since 0.1.0
 */
void er_nag_render(ra8_widget_t* w);

/**
 * @brief Poll the touch controller and dispatch a tap on a fresh press.
 *
 * @pre ra8_gfx bound; ``s_nav`` initialised; ``s_display`` valid.
 * @pre None.
 * @post On a press that changes the screen, the new screen is shown.
 * @post ``s_was_touching`` tracks the contact state.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
void er_poll_touch(void);

/**
 * @brief Poll SW1/SW2 and turn a page on a fresh press (#78).
 *
 * @pre The switch pins were configured as inputs (::app_bringup_buttons).
 * @pre ::s_display / ::s_fb are valid.
 * @post On a fresh press that turns a page, the new page is shown and flushed.
 * @post ::s_was_sw1 / ::s_was_sw2 track the held state.
 * @note Not thread-safe.
 * @since 0.1.0
 */
void er_poll_buttons(void);

/**
 * @brief Poll the fuel gauge, fold it into the nag policy, and toggle the banner.
 *
 * @pre ::app_bringup_touch brought up IIC_B channel 0 (shared with the gauge).
 * @pre ::s_batt_mon was initialised by ::ra8_batt_monitor_init.
 * @post On a banner state change the chrome is re-rendered + flushed.
 * @post ::s_batt_nag reflects the banner shown.
 * @note Not thread-safe.
 * @since 0.1.0
 */
void er_poll_battery(void);

/* ===========================================================================
 * Screen dispatch + panel flush (main.c) -- called from the input helpers.
 * =========================================================================== */

/**
 * @brief Render whichever screen is on top of the navigation stack.
 *
 * @pre ra8_gfx is bound; ``s_nav`` initialised.
 * @pre None.
 * @post The framebuffer holds the current screen.
 * @post No navigation state mutated beyond focusing the active app.
 * @note Not thread-safe.
 * @since 0.1.0
 */
void er_render_current(void);

/**
 * @brief Repaint only the low-battery banner rect (dirty-region update).
 *
 * @pre ra8_gfx is bound; ``s_nav`` initialised.
 * @pre None.
 * @post Only the banner rect is repainted (full repaint if clip unavailable).
 * @post No navigation state mutated.
 * @note Not thread-safe.
 * @since 0.1.0
 */
void er_render_nag_region(void);

/**
 * @brief Flush the panel for a refresh event through the cadence policy (#78).
 *
 * @param[in] event The page-transition that occurred (open / turn / chapter).
 * @pre ::s_policy was initialised and ::s_display is valid.
 * @pre ::s_fb describes the bound framebuffer.
 * @post One panel flush is issued; ::g_er_last_hint / ::g_er_cur_page are current.
 * @note Not thread-safe; the reader UI is single-threaded.
 * @since 0.1.0
 */
void er_flush_event(display_turn_event_t event);
