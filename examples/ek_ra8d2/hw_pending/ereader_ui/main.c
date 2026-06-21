/**
 * @file examples/ek_ra8d2/hw_pending/ereader_ui/main.c
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
 * screen stack. The Reading body renders real reflowed book text through
 * ``libs/ra_reflow`` when a font is present on the microSD (``FONT.OTF``);
 * with no card it falls back to the bundled ``ra_gfx`` bitmap font (#83).
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
#include <string.h>

#include "arnopro_latin1.h"
#include "er_pageturn.h"
#include "figure_fixture.h"
#include "ra_app.h"
#include "ra_batt.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_box.h"
#include "ra_cgc.h"
#include "ra_display_pal.h"
#include "ra_display_pal_lcd.h"
#include "ra_display_pal_policy.h"
#include "ra_err.h"
#include "ra_gfx.h"
#include "ra_gfx_font.h"
#include "ra_i3c.h"
#include "ra_isr.h"
#include "ra_keyboard.h"
#include "ra_mstp.h"
#include "ra_panel.h"
#include "ra_panel_timing.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_reflow.h"
#include "ra_reflow_image.h"
#include "ra_sdfont.h"
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
  k_er_fb_align  = 64U,  /**< 64-byte AXI-burst alignment.                  */
  k_er_settle_ms = 500U, /**< PLL / SDRAM / panel-POR settle.              */
  k_er_frame_ms  = 25U,  /**< Input poll period (ms): touch / button cadence.*/
  k_er_led_every = 16U,  /**< Toggle the heartbeat LED every Nth frame.     */
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
  k_er_screen_library  = 1U, /**< Library / home grid.               */
  k_er_screen_reading  = 2U, /**< Reading view.                      */
  k_er_screen_keyboard = 3U, /**< On-screen keyboard (search entry). */
  k_er_screen_settings = 4U, /**< Settings (optional app, #if RA_APP_SETTINGS). */
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
 * polled from the loop (IRQ pin left unset). board_sim feeds --click /
 * window taps through this same ra_touch -> I2C -> GT911 path.
 */
typedef enum : uint8_t {
  k_er_touch_channel    = 0U,    /**< IIC_B channel 0.            */
  k_er_touch_addr_7b    = 0x5DU, /**< GT911 default 7-bit addr.   */
  k_er_touch_max_points = 1U,    /**< One contact = one tap.      */
} er_touch_cfg_t;

/**
 * @enum er_fg_cfg_t
 * @brief MAX17048-class fuel gauge wiring (shares the touch IIC_B bus).
 *
 * @details The fuel gauge sits at 7-bit 0x36 on the same IIC_B channel 0 the
 * GT911 touch already brought up, so the battery is read with raw ra_i3c
 * register reads (no second bus init). board_sim models this gauge and drives
 * its SOC / CRATE from the on-screen battery slider.
 */
typedef enum : uint8_t {
  k_er_fg_addr      = 0x36U, /**< Fuel gauge 7-bit I2C address.        */
  k_er_fg_reg_soc   = 0x04U, /**< SOC register (high byte = percent).  */
  k_er_fg_reg_crate = 0x16U, /**< CRATE register (sign = charge dir).  */
  k_er_fg_sign      = 0x80U, /**< CRATE high-byte sign bit (discharge).*/
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
  k_er_max_targets   = 32U,  /**< Max collected tap targets.          */
  k_er_back_w        = 240U, /**< Reading back-region width (px).     */
  k_er_page_back_cap = 16U,  /**< Footnote-jump back-stack depth.     */
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
  k_er_nav_count       = (uint16_t)(sizeof(k_er_nav_items) / sizeof(k_er_nav_items[0])),
  k_er_nav_idx_setting = 3U, /**< Index of the "Settings" nav item. */
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
 * @brief Reading-view chapter as XHTML, reflowed by ra_reflow when an SD
 *        font is present (the same prose as ::k_er_body_lines, but laid out
 *        live at the proportional type scale instead of pre-wrapped bitmap
 *        lines). Kept short so the software glyph rasteriser stays quick
 *        under the board_sim CPU emulator.
 */
static const char k_er_chapter_xhtml[] =
  "<html><head><style>h1 { text-align: center; color: maroon; }"
  ".byline { text-align: right; text-decoration: underline; color: #808080; font-size: 14px; }"
  ".lead { font-size: 130%; } .draft { display: none; }"
  " p.lead { color: navy; }</style></head>"
  "<body><h1>The Time Machine</h1>"
  "<p class=\"draft\">INTERNAL DRAFT -- NOT FOR DISTRIBUTION</p>"
  "<p class=\"byline\">by H. G. Wells</p>"
  "<img src=\"logo.svg\"/>"
  "<p class=\"lead\">The Time Traveller (for so it will be convenient to speak of him) was "
  "expounding a recondite matter to us. See the "
  "<a href=\"#note\">editor's note</a> for context. His pale grey eyes shone "
  "and twinkled, and his usually pale face was flushed and animated.</p>"
  "<p>The fire burnt brightly, and the soft radiance of the incandescent lights "
  "in the lilies of silver caught the bubbles that flashed and passed in our "
  "glasses. Our chairs, being his patents, embraced and caressed us rather than "
  "submitted to be sat upon, and thought roamed gracefully free of the trammels "
  "of precision.</p>"
  "<p id=\"note\">Editor's note: this passage introduces the frame narrator "
  "before the Traveller's account begins. <a href=\"ch2.xhtml\">Continue to "
  "Chapter II</a>.</p></body></html>";

/** @brief Reading-view chapter two (cross-chapter `<a href>` link target). */
static const char k_er_chapter2_xhtml[] =
  "<html><body><h1>Chapter II</h1>"
  "<p>The Machine, said the Time Traveller, holding the lamp aloft, is the thing "
  "I have been at work upon these many years; and now at last it is built.</p>"
  "<p>He took one of the small octahedral things from the table and handed it to "
  "us. <a href=\"ch1.xhtml\">Back to Chapter I</a>.</p></body></html>";

/**
 * @struct er_chapter_t
 * @brief One mock-spine chapter: its XHTML, length, and resolving href.
 */
typedef struct {
  const char* xhtml;    /**< Chapter XHTML body.                  */
  uint32_t    len;      /**< Length of @ref xhtml (excl. NUL).    */
  const char* href;     /**< Manifest href that links to it.      */
  uint32_t    href_len; /**< Length of @ref href (excl. NUL).     */
} er_chapter_t;

/** @brief Two-chapter mock spine for in-content cross-chapter navigation (#110). */
static const er_chapter_t k_er_spine[] = {
  {k_er_chapter_xhtml,
   (uint32_t)(sizeof(k_er_chapter_xhtml) - 1U),
   "ch1.xhtml",
   (uint32_t)(sizeof("ch1.xhtml") - 1U)},
  {k_er_chapter2_xhtml,
   (uint32_t)(sizeof(k_er_chapter2_xhtml) - 1U),
   "ch2.xhtml",
   (uint32_t)(sizeof("ch2.xhtml") - 1U)},
};

/** @brief Index of the chapter currently shown in the Reading view. */
static uint32_t s_chapter_idx;

/**
 * @enum er_reflow_cfg_t
 * @brief SD-font load + ra_reflow body-render tunables (no magic numbers).
 */
typedef enum : uint32_t {
  k_er_font_cap    = 512U * 1024U,       /**< Max font read off the card (bytes).  */
  k_er_font_min    = 16U,                /**< Smallest plausible font blob (bytes).*/
  k_er_reflow_px   = 22U,                /**< Body type size (pixels).             */
  k_er_spi_chan    = 0U,                 /**< Pmod2 / J25 = SCI0 Simple-SPI.       */
  k_er_reflow_ink  = 0xFF101010U,        /**< Body text colour (near-black ARGB).  */
  k_er_reflow_link = 0xFF2A52BEU,        /**< Anchor colour (cerulean ARGB).       */
  k_er_img_arena   = 2U * 1024U * 1024U, /**< SDRAM image-decode scratch.    */
} er_reflow_cfg_t;

/** @brief Pmod2 SPI pins (J25) -- SCI0 Simple-SPI, per sd_font_render. */
static const ra_port_pin_t k_er_pin_sck  = (ra_port_pin_t)k_ra_board_pmod2_spi_sck;
static const ra_port_pin_t k_er_pin_cipo = (ra_port_pin_t)k_ra_board_pmod2_spi_cipo;
static const ra_port_pin_t k_er_pin_copi = (ra_port_pin_t)k_ra_board_pmod2_spi_copi;
static const ra_port_pin_t k_er_pin_cs   = (ra_port_pin_t)k_ra_board_pmod2_spi_cs;

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

/** @brief Library box-node count (set by er_render_library for the band widgets). */
static uint16_t s_lib_node_count;

/** @brief First Library body box-node (root=0, status bar=1, body from here). */
static uint16_t s_lib_body_first = 2U;

/** @brief On-screen keyboard grid (built by er_render_keyboard, read on tap). */
static ra_kbd_layout_t s_kb;

/** @brief Live search query typed on the keyboard; filters the Library shelf. */
static ra_kbd_text_t s_query;

/** @brief Reading view: current reflow page index (0-based). */
static uint32_t s_reading_page;

/** @brief Reading view: total reflow pages from the last layout (>= 1). */
static uint32_t s_reading_pages = 1U;

/** @brief Debounce: true while a contact is held, to fire once per tap. */
static bool s_was_touching;

/** @brief Debounce: true while SW1/SW2 are held, to fire once per press. */
static bool s_was_sw1;
static bool s_was_sw2; /**< @see s_was_sw1 */

/** @brief Low-battery nag policy state (edge-triggered, see ra_batt). */
static ra_batt_monitor_t s_batt_mon;
/** @brief Nag currently shown over the chrome (none = no banner). */
static ra_batt_nag_t s_batt_nag = k_ra_batt_nag_none;
/** @brief Last fuel-gauge SOC percent (for the banner text). */
static uint8_t s_batt_soc = 0U;

/** @brief Nag banner messages (the SOC percent + dismiss hint follow at render). */
static const char k_er_nag_low_msg[]  = "Low battery ";
static const char k_er_nag_crit_msg[] = "Battery critical ";
/** @brief Trailing hint so the banner says how to close itself. */
static const char k_er_nag_hint[] = "  (tap to dismiss)";

/** @brief Refresh-cadence policy driving e-ink page-turn waveforms (#78). */
static display_policy_t s_policy;

/**
 * @brief Pending refresh event for the next flush (set by a tap/button handler).
 * @details Defaults to a clean chapter-boundary refresh for any screen change;
 *          a same-chapter page turn lowers it to ::k_display_event_turn.
 */
static display_turn_event_t s_pending_event = k_display_event_chapter;

/** @brief SWD / `--dump-sym` telemetry for the headless page-turn HIL (#78). */
volatile uint32_t g_er_cur_page;  /**< Current reading page after the last turn.   */
volatile uint32_t g_er_turns;     /**< Count of page turns applied since boot.     */
volatile uint32_t g_er_last_hint; /**< Last `display_refresh_hint_t` flushed.       */

/** @brief SW1 (P009) = previous page; active-low, internal pull-up. */
static const ra_port_pin_t k_er_pin_sw1 =
  (ra_port_pin_t)(((uint16_t)k_ra_port_0 << 8) | (uint16_t)k_ra_pin_9);
/** @brief SW2 (P008) = next page; active-low, internal pull-up. */
static const ra_port_pin_t k_er_pin_sw2 =
  (ra_port_pin_t)(((uint16_t)k_ra_port_0 << 8) | (uint16_t)k_ra_pin_8);

/** @brief Default clean-refresh cadence: a GC16 every N fast turns. */
enum : uint16_t {
  k_er_clean_every = 8U,
};

/**
 * @struct er_loc_t
 * @brief A reading location (chapter + page) for the navigation back-stack.
 */
typedef struct {
  uint32_t chapter; /**< Spine chapter index. */
  uint32_t page;    /**< Page within chapter. */
} er_loc_t;

/** @brief Reading back-stack for link round-trips (footnote + chapter) (#110). */
static er_loc_t s_loc_back[k_er_page_back_cap];

/** @brief Entries used in ::s_loc_back. */
static uint32_t s_loc_back_count;

/** @brief Font blob read off the SD card -- lives in SDRAM (hundreds of KiB). */
static uint8_t s_font_buf[k_er_font_cap] __attribute__((section(".sdram_data")));

/** @brief Image-decode bump arena in SDRAM (covers / figures are megabytes). */
static uint8_t s_img_arena_buf[k_er_img_arena] __attribute__((section(".sdram_data")));

/** @brief ra_reflow engine for the Reading body (page / glyph / token pools). */
static ra_reflow_t s_reflow_engine;

/** @enum er_reflow_sentinel_t @brief "No chapter cached" sentinel. */
typedef enum : uint32_t {
  k_er_chapter_none = 0xFFFFFFFFU, /**< s_reflow_chapter when nothing is laid out. */
} er_reflow_sentinel_t;

/** @brief True while ::s_reflow_engine holds a laid-out chapter (cache valid). */
static bool s_reflow_open = false;
/** @brief Chapter index currently laid out in ::s_reflow_engine (cache key). */
static uint32_t s_reflow_chapter = (uint32_t)k_er_chapter_none;
/** @brief Body geometry the cached layout was paginated for (cache key). */
static int32_t s_reflow_w = 0;
static int32_t s_reflow_h = 0; /**< @see s_reflow_w */

/** @brief Bytes of font read off the card (0 if none). */
static uint32_t s_font_len;

/** @brief True once an SD font is loaded; gates the ra_reflow body render. */
static bool s_have_font;

/**
 * @brief ra_reflow image loader: resolve any `<img src>` to the baked figure.
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
 * @return k_ra_ok always (the figure is always available).
 * @retval k_ra_ok The bundled figure bytes were returned.
 * @pre @p out_bytes and @p out_len are non-null.
 * @pre The bundled figure is a valid encoded image.
 * @post `*out_bytes` / `*out_len` describe ::k_er_figure_png.
 * @post No state mutated.
 * @note Not thread-safe (single-threaded UI loop).
 * @since 0.1.0
 */
/**
 * @brief In-content SVG (#112): a navy field, a gold disc, and a crimson block.
 *
 * @details Returned by ::er_image_loader for any `*.svg` `<img src>`; rendered as
 * vector `<rect>`/`<circle>` shapes by ra_svg_render (no raster decode).
 */
static const char k_er_logo_svg[] =
  "<svg viewBox=\"0 0 120 80\">"
  "<rect x=\"0\" y=\"0\" width=\"120\" height=\"80\" fill=\"#283C82\"/>"
  "<circle cx=\"34\" cy=\"40\" r=\"24\" fill=\"#E6C84B\"/>"
  "<path d=\"M66 62 L112 62 L89 16 Z\" fill=\"#C03A37\"/>"
  "</svg>";

/** @brief SVG fixture size + the `.svg` extension length for href routing. */
typedef enum : uint32_t {
  k_er_logo_svg_len = (uint32_t)(sizeof(k_er_logo_svg) - 1U), /**< SVG byte count.      */
  k_er_ext_len      = 4U,                                     /**< Length of ".svg".    */
} er_svg_len_t;

static ra_err_t er_image_loader(void*           ctx,
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
    return k_ra_ok;
  }
  *out_bytes = k_er_figure_png;
  *out_len   = (size_t)k_er_figure_png_len;
  return k_ra_ok;
}

/** @brief Cached PCLKA rate (Hz) for the SD SPI clock shim. */
static uint32_t s_pclka_hz;

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
 * Optional SD-loaded font for the ra_reflow Reading body
 * =========================================================================== */

/**
 * @brief Best-effort: load FONT.OTF off the Pmod2 SD card into s_font_buf.
 *
 * @details
 * Delegates the Pmod2 SPI bring-up, FAT mount, and font read to
 * @ref ra_sdfont_load. Entirely non-fatal: any failure (no Pmod, no card, no
 * FONT.OTF) leaves ::s_have_font false and the Reading view falls back to the
 * baked Latin-1 font (or the bundled bitmap), so the chrome is never broken by
 * the absence of an SD card. Provisioning is left disabled (NULL blob): the
 * e-reader reads whatever the user's card already holds and never writes to it
 * -- unlike `sd_font_render`, which self-provisions a blank card.
 *
 * @pre app_bringup_clocks has run.
 * @pre ::s_font_buf is reachable (SDRAM).
 * @post On success ::s_have_font is true and ::s_font_len > 0.
 * @post On any failure ::s_have_font stays false (no panic, no card write).
 * @note Not thread-safe; single-shot helper.
 * @since 0.1.0
 */
static void er_try_load_font(void)
{
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &s_pclka_hz) != k_ra_ok) {
    return;
  }
  const ra_sdfont_cfg_t cfg = {
    .spi_channel    = (uint8_t)k_er_spi_chan,
    .sck            = k_er_pin_sck,
    .cipo           = k_er_pin_cipo,
    .copi           = k_er_pin_copi,
    .cs             = k_er_pin_cs,
    .pclka_hz       = s_pclka_hz,
    .filename       = "FONT.OTF",
    .provision_blob = nullptr, /* read-only: never write to the user's card */
    .provision_len  = 0U,
  };
  uint32_t got = 0U;
  if (ra_sdfont_load(&cfg, s_font_buf, (uint32_t)k_er_font_cap, &got, nullptr) != k_ra_ok) {
    return;
  }
  s_font_len  = got;
  s_have_font = true;
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

  ra_box_t srch_t    = er_leaf(0, 1U, (uint32_t)k_ra_box_no_colour, (uint32_t)k_er_ink);
  srch_t.tag         = (int16_t)k_er_act_search; /* tap the Search field -> keyboard */
  const int16_t srch = ra_box_add(tree, tb, &srch_t);
  s_label[srch]      = k_er_search_hint;
  s_label_col[srch]  = (uint32_t)k_er_ink_muted;

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
    ra_box_t item_t = er_leaf(0, 1U, (uint32_t)k_ra_box_no_colour, (uint32_t)k_ra_box_no_colour);
#ifdef RA_APP_SETTINGS
    /* The "Settings" nav item is tappable only when the Settings app is installed
     * (RA_APP_SETTINGS): tagging affects target collection, not pixels, so the
     * default (uninstalled) build's chrome is byte-identical. */
    if (i == (uint16_t)k_er_nav_idx_setting) {
      item_t.tag = (int16_t)k_er_act_settings;
    }
#endif
    const int16_t item = ra_box_add(tree, nav, &item_t);
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
    while ((j < (uint32_t)k_ra_kbd_text_max) && (needle[j] != '\0') && (hay[i + j] != '\0') &&
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
  /* The status bar is one node; everything after it is the body band (the band
   * widgets in er_render_library split the render at this index). */
  s_lib_body_first = (uint16_t)(sb + 1);

  er_build_toolbar(tree, root);

  const ra_box_t grid_t = er_container(k_ra_box_grid,
                                       0,
                                       (int16_t)k_er_pad_ui,
                                       (int16_t)k_er_grid_gap,
                                       (uint8_t)k_er_grid_cols);
  const int16_t  grid   = ra_box_add(tree, root, &grid_t);
  for (uint16_t i = 0U; i < (uint16_t)k_er_book_count; ++i) {
    if (!er_book_matches(&k_er_books[i])) {
      continue; /* hidden by the committed search query */
    }
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
/** @brief Render box nodes ``[from, to)`` of ::s_nodes (fill/border/bar/label). */
static void er_render_boxnodes(uint16_t from, uint16_t to)
{
  for (uint16_t i = from; i < to; ++i) {
    const ra_box_t*    n = &s_nodes[i];
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
/** @brief Status-bar band widget: heading node + the under-bar hairline + clock. */
static void er_lib_sb_render(ra_widget_t* w)
{
  (void)w;
  er_render_boxnodes(0U, s_lib_body_first); /* root (no-op) + the status-bar node */
  (void)ra_gfx_rect(0,
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
static void er_lib_body_render(ra_widget_t* w)
{
  (void)w;
  er_render_boxnodes(s_lib_body_first, s_lib_node_count); /* toolbar + grid + nav */
  (void)ra_gfx_rect(0,
                    (int32_t)s_fb.height_px - (int32_t)k_er_nav_h,
                    (int32_t)s_fb.width_px,
                    (int32_t)k_er_hair,
                    (uint32_t)k_er_rule,
                    true);
}

/** @brief Vtables for the Library status-bar / body band widgets. */
static const ra_widget_vtable_t k_er_lib_sb_vt   = {.render = er_lib_sb_render};
static const ra_widget_vtable_t k_er_lib_body_vt = {.render = er_lib_body_render};

static void er_render_library(void)
{
  (void)ra_gfx_clear((uint32_t)k_er_paper);
  ra_box_tree_t      tree;
  const ra_ui_rect_t frame = {0, 0, (int32_t)s_fb.width_px, (int32_t)s_fb.height_px};
  er_build_library(&tree, &frame);
  er_collect_targets(&tree);
  s_lib_node_count = tree.count;

  /* Compose the screen from two band widgets (#145): a status bar over the body
   * (toolbar + grid + nav). The bands occupy disjoint y-ranges, so the widget
   * composition renders byte-identically to the monolithic box tree -- the
   * status bar can now be invalidated + partial-flushed on its own. */
  ra_widget_t bands[2] = {};
  bands[0].vt          = &k_er_lib_sb_vt;
  bands[0].fixed       = (int16_t)k_er_statusbar_h;
  bands[0].visible     = true;
  bands[1].vt          = &k_er_lib_body_vt;
  bands[1].flex        = 1U;
  bands[1].visible     = true;
  ra_box_t band_scratch[3];
  (void)ra_widget_layout_stack(bands, 2U, &frame, k_ra_widget_axis_col, 0, 0, band_scratch, 3U);
  (void)ra_widget_invalidate(&bands[0], k_ra_widget_refresh_quality);
  (void)ra_widget_invalidate(&bands[1], k_ra_widget_refresh_quality);
  (void)ra_widget_render_dirty(bands, 2U);
}

/* ===========================================================================
 * Reading screen -- ra_reflow body text (SD font) with a bitmap fallback
 * =========================================================================== */

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
    (void)ra_reflow_close(&s_reflow_engine);
    s_reflow_open = false;
  }
  if (ra_reflow_init((uint16_t)body_w,
                     (uint16_t)body_h,
                     font_data,
                     font_len,
                     (uint16_t)k_er_reflow_px,
                     (uint32_t)k_er_reflow_ink,
                     (uint32_t)k_er_reflow_link,
                     &s_reflow_engine) != k_ra_ok) {
    return false;
  }
  /* Bind the image loader + SDRAM decode arena so the chapter's <img> renders
   * (decode -> scale -> blit). Without this the engine reserves a placeholder. */
  static ra_img_arena_t s_reflow_img_arena;
  s_reflow_img_arena = (ra_img_arena_t){.base   = s_img_arena_buf,
                                        .cap    = (size_t)k_er_img_arena,
                                        .offset = 0U,
                                        .live   = 0U};
  (void)ra_reflow_set_image_loader(&s_reflow_engine, er_image_loader, nullptr, &s_reflow_img_arena);
  uint32_t            pages = 0U;
  const er_chapter_t* chap  = &k_er_spine[s_chapter_idx];
  if (ra_reflow_layout_chapter(&s_reflow_engine, (const uint8_t*)chap->xhtml, chap->len, &pages) !=
      k_ra_ok) {
    (void)ra_reflow_close(&s_reflow_engine);
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
 * @brief Render the Reading body through ra_reflow when an SD font is loaded.
 *
 * @details Lays the chapter XHTML out against the body rectangle (inset
 *          below the status bar, above the footer) and paints the current
 *          page (::s_reading_page) there via ra_reflow_render_page_at.
 *          er_render_reading has already cleared the framebuffer to paper
 *          and the body colour is dark ink, so the text shows (the
 *          ra_reflow_init colour args are the text colours, not the
 *          background). Publishes the layout's page count to ::s_reading_pages
 *          and clamps ::s_reading_page into range.
 *
 * @param[in] body_top Top y of the body band (pixels).
 * @param[in] height   Framebuffer height (pixels).
 * @return true if reflowed text was painted; false to use the bitmap fallback.
 * @retval true  ra_reflow rendered the body.
 * @retval false No font / init / layout failure -- caller draws the bitmap body.
 * @pre ra_gfx is bound and the body region is cleared to paper.
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
  const uint8_t* font_data = s_have_font ? s_font_buf : g_ra_font_arnopro_latin1;
  const uint32_t font_len  = s_have_font ? s_font_len : g_ra_font_arnopro_latin1_len;
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
  (void)
    ra_reflow_render_page_at(&s_reflow_engine, s_reading_page, (int32_t)k_er_margin_x, body_top);
  return true;
}

/**
 * @brief Paint the Reading body: reflowed text (SD or baked font), else bitmap.
 *
 * @param[in] height Framebuffer height in pixels.
 *
 * @return true if the body was reflowed (paginated); false on the bitmap fallback.
 * @retval true  ra_reflow painted the current page (SD font or the baked font).
 * @retval false The bundled bitmap lines were drawn (reflow-engine failure).
 * @pre ra_gfx is bound.
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
static void er_read_sb_render(ra_widget_t* w)
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
  (void)ra_gfx_rect(0,
                    (int32_t)k_er_statusbar_h - (int32_t)k_er_hair,
                    width,
                    (int32_t)k_er_hair,
                    (uint32_t)k_er_rule,
                    true);
}

/** @brief Body band widget: the reflowed (or bitmap-fallback) chapter text. */
static void er_read_body_render(ra_widget_t* w)
{
  (void)w;
  s_read_reflowed = er_draw_reading_body((int32_t)s_fb.height_px);
}

/** @brief Footer band widget: rule + chapter title + page label + progress bar. */
static void er_read_footer_render(ra_widget_t* w)
{
  (void)w;
  const int32_t width    = (int32_t)s_fb.width_px;
  const int32_t height   = (int32_t)s_fb.height_px;
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
  /* In the reflow path the bar tracks the live page; the bitmap fallback keeps
   * the static placeholder ratio. */
  const int32_t pg_cur =
    s_read_reflowed ? (int32_t)(s_reading_page + 1U) : (int32_t)k_er_page_current;
  const int32_t pg_tot = s_read_reflowed ? (int32_t)s_reading_pages : (int32_t)k_er_page_total;
  const int32_t fill_w = (track_w * pg_cur) / pg_tot;
  (void)
    ra_gfx_rect(track_x, track_y, fill_w, (int32_t)k_er_progress_h, (uint32_t)k_er_fill_deep, true);
}

/** @brief Vtables for the Reading status-bar / body / footer band widgets. */
static const ra_widget_vtable_t k_er_read_sb_vt     = {.render = er_read_sb_render};
static const ra_widget_vtable_t k_er_read_body_vt   = {.render = er_read_body_render};
static const ra_widget_vtable_t k_er_read_footer_vt = {.render = er_read_footer_render};

/**
 * @brief Render the full Reading screen (status bar, body, footer).
 *
 * @details Composes three disjoint-y band widgets (#145) -- status bar, body,
 * footer -- each drawing its existing absolute-coordinate region. The bands
 * occupy non-overlapping y-ranges and render in array order (status bar, then
 * body, then footer), so the composite is byte-identical to the former
 * monolithic render while each band can now be invalidated + partial-flushed on
 * its own (a page turn dirties only body + footer, leaving the status bar).
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
  (void)ra_gfx_clear((uint32_t)k_er_paper);
  const ra_ui_rect_t frame    = {0, 0, (int32_t)s_fb.width_px, (int32_t)s_fb.height_px};
  ra_widget_t        bands[3] = {};
  bands[0].vt                 = &k_er_read_sb_vt;
  bands[0].fixed              = (int16_t)k_er_statusbar_h;
  bands[0].visible            = true;
  bands[1].vt                 = &k_er_read_body_vt;
  bands[1].flex               = 1U;
  bands[1].visible            = true;
  bands[2].vt                 = &k_er_read_footer_vt;
  bands[2].fixed              = (int16_t)k_er_footer_h;
  bands[2].visible            = true;
  ra_box_t band_scratch[4];
  (void)ra_widget_layout_stack(bands, 3U, &frame, k_ra_widget_axis_col, 0, 0, band_scratch, 4U);
  for (uint16_t i = 0U; i < 3U; ++i) {
    (void)ra_widget_invalidate(&bands[i], k_ra_widget_refresh_quality);
  }
  (void)ra_widget_render_dirty(bands, 3U);
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
/** @enum er_kbd_render_t @brief Keyboard chrome metrics (8x16 glyphs + keys). */
typedef enum : int32_t {
  k_er_kbd_glyph_w   = 8,  /**< ra_gfx_font_8x16 glyph width.        */
  k_er_kbd_glyph_h   = 16, /**< ra_gfx_font_8x16 glyph height.       */
  k_er_kbd_qlabel    = 72, /**< "Search:" label column width.        */
  k_er_kbd_gap       = 6,  /**< Inset gap around each key.           */
  k_er_kbd_radius    = 10, /**< Key corner radius (rounded, iOS).    */
  k_er_kbd_shadow_dy = 3,  /**< Key drop-shadow offset (down).       */
  k_er_kbd_lab_max   = 8,  /**< Key-label scratch size.              */
  /* Glyph-icon geometry (px from the key centre) for SHIFT / DEL / RETURN. */
  k_er_ico_aw   = 11, /**< Shift arrowhead half-width.        */
  k_er_ico_atop = 12, /**< Shift arrowhead apex, above centre.*/
  k_er_ico_abot = 2,  /**< Shift arrowhead base, above centre.*/
  k_er_ico_sw   = 3,  /**< Shift stem half-width.             */
  k_er_ico_sbot = 11, /**< Shift stem, below centre.          */
  k_er_ico_dw   = 14, /**< Delete glyph half-width.           */
  k_er_ico_dh   = 9,  /**< Delete glyph half-height.          */
  k_er_ico_dx   = 5,  /**< Delete X half-extent.              */
  k_er_ico_dxo  = 4,  /**< Delete X right-of-centre offset.   */
  k_er_ico_rw   = 11, /**< Return shaft half-width.           */
  k_er_ico_rh   = 10, /**< Return tail height.                */
  k_er_ico_rah  = 6,  /**< Return arrowhead leg.              */
} er_kbd_render_t;

/** @enum er_kbd_color_t @brief Apple-style grayscale keyboard palette. */
typedef enum : uint32_t {
  k_er_kbd_bg    = 0xCCCCCCU, /**< Keyboard background.        */
  k_er_kbd_keylt = 0xFFFFFFU, /**< Light (letter/space) key.   */
  k_er_kbd_keydk = 0xAAAAAAU, /**< Dark (special) key.         */
  k_er_kbd_keysh = 0x909090U, /**< Key drop shadow.            */
} er_kbd_color_t;

/** @brief Text label for key @p idx (SPACE / 123-ABC, or live-case char in @p s).
 *
 * @note SHIFT / BACKSPACE / RETURN are drawn as glyph icons, not text, so they
 *       never reach this helper.
 */
static const char* er_kbd_label(uint8_t idx, char* s)
{
  switch (s_kb.keys[idx].kind) {
    case k_ra_kbd_key_space:
      return "space";
    case k_ra_kbd_key_layer:
      if (s_kb.keys[idx].aux == (uint8_t)k_ra_kbd_layer_numbers) {
        return "123";
      }
      if (s_kb.keys[idx].aux == (uint8_t)k_ra_kbd_layer_symbols) {
        return "#+=";
      }
      return "ABC";
    default:
      s[0] = ra_kbd_key_glyph(&s_kb, idx);
      s[1] = '\0';
      return s;
  }
}

/** @brief Filled rounded rect (rect cross + four corner discs). */
static void er_round_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c)
{
  const int32_t r = (int32_t)k_er_kbd_radius;
  (void)ra_gfx_rect(x + r, y, w - (2 * r), h, c, true);
  (void)ra_gfx_rect(x, y + r, w, h - (2 * r), c, true);
  (void)ra_gfx_circle(x + r, y + r, r, c, true);
  (void)ra_gfx_circle((x + w - r) - 1, y + r, r, c, true);
  (void)ra_gfx_circle(x + r, (y + h - r) - 1, r, c, true);
  (void)ra_gfx_circle((x + w - r) - 1, (y + h - r) - 1, r, c, true);
}

/** @brief Filled upward triangle from apex row @p ay to base row @p by (scanlines). */
static void er_tri_up(int32_t cx, int32_t ay, int32_t by, int32_t hw, uint32_t c)
{
  const int32_t span = by - ay;
  for (int32_t y = ay; y <= by; ++y) {
    const int32_t half = (span > 0) ? ((hw * (y - ay)) / span) : hw;
    (void)ra_gfx_line(cx - half, y, cx + half, y, c);
  }
}

/** @brief SHIFT glyph: an up-arrow (filled arrowhead over a narrow stem). */
static void er_icon_shift(int32_t cx, int32_t cy, uint32_t c)
{
  er_tri_up(cx, cy - (int32_t)k_er_ico_atop, cy - (int32_t)k_er_ico_abot, (int32_t)k_er_ico_aw, c);
  (void)ra_gfx_rect(cx - (int32_t)k_er_ico_sw,
                    cy - (int32_t)k_er_ico_abot,
                    2 * (int32_t)k_er_ico_sw,
                    (int32_t)k_er_ico_abot + (int32_t)k_er_ico_sbot,
                    c,
                    true);
}

/** @brief BACKSPACE glyph: a left-pointing pentagon outline with an X inside. */
static void er_icon_delete(int32_t cx, int32_t cy, uint32_t c)
{
  const int32_t w  = (int32_t)k_er_ico_dw;
  const int32_t h  = (int32_t)k_er_ico_dh;
  const int32_t tx = (cx - w) + h; /* where the tip opens into the body */
  (void)ra_gfx_line(cx - w, cy, tx, cy - h, c);
  (void)ra_gfx_line(tx, cy - h, cx + w, cy - h, c);
  (void)ra_gfx_line(cx + w, cy - h, cx + w, cy + h, c);
  (void)ra_gfx_line(cx + w, cy + h, tx, cy + h, c);
  (void)ra_gfx_line(tx, cy + h, cx - w, cy, c);
  const int32_t xo = (int32_t)k_er_ico_dxo;
  const int32_t xe = (int32_t)k_er_ico_dx;
  (void)ra_gfx_line((cx + xo) - xe, cy - xe, cx + xo + xe, cy + xe, c);
  (void)ra_gfx_line(cx + xo + xe, cy - xe, (cx + xo) - xe, cy + xe, c);
}

/** @brief RETURN glyph: a carriage-return arrow (tail down, shaft left, arrowhead). */
static void er_icon_return(int32_t cx, int32_t cy, uint32_t c)
{
  const int32_t w = (int32_t)k_er_ico_rw;
  const int32_t a = (int32_t)k_er_ico_rah;
  (void)ra_gfx_line(cx + w, cy - (int32_t)k_er_ico_rh, cx + w, cy, c);
  (void)ra_gfx_line(cx + w, cy, cx - w, cy, c);
  (void)ra_gfx_line(cx - w, cy, (cx - w) + a, cy - a, c);
  (void)ra_gfx_line(cx - w, cy, (cx - w) + a, cy + a, c);
}

/** @brief True for keys drawn dark (SHIFT / BACKSPACE / 123-ABC / RETURN). */
static bool er_key_is_special(ra_kbd_key_kind_t kind)
{
  return (kind != k_ra_kbd_key_char) && (kind != k_ra_kbd_key_space);
}

/** @brief Draw the text label of key @p idx centred in the key body. */
static void
er_draw_key_label(uint8_t idx, int32_t kx, int32_t ky, int32_t kw, int32_t kh, uint32_t fill)
{
  char          scratch[k_er_kbd_lab_max] = {};
  const char*   lab                       = er_kbd_label(idx, scratch);
  const int32_t lw                        = (int32_t)strlen(lab) * (int32_t)k_er_kbd_glyph_w;
  const int32_t lx                        = kx + ((kw - lw) / 2);
  const int32_t ly                        = ky + ((kh - (int32_t)k_er_kbd_glyph_h) / 2);
  (void)ra_gfx_text_out(lx, ly, lab, &ra_gfx_font_8x16, (uint32_t)k_er_ink, fill);
}

/** @brief Draw key @p idx Apple-style: a shadowed rounded key + glyph icon / label.
 *
 * @details SHIFT / BACKSPACE / RETURN render as vector glyph icons; every other
 * key (letters, digits, symbols, SPACE, 123/#+=/ABC) renders its text label. An
 * armed one-shot SHIFT inverts the SHIFT key (white body) like iOS.
 */
static void er_draw_key(uint8_t idx)
{
  const ra_ui_rect_t      r       = s_kb.keys[idx].rect;
  const int32_t           g       = (int32_t)k_er_kbd_gap;
  const int32_t           kx      = r.x + g;
  const int32_t           ky      = r.y + g;
  const int32_t           kw      = r.w - (2 * g);
  const int32_t           kh      = r.h - (2 * g);
  const ra_kbd_key_kind_t kind    = s_kb.keys[idx].kind;
  const bool              special = er_key_is_special(kind);
  const bool              sh_on   = (kind == k_ra_kbd_key_shift) && s_kb.shift;
  const uint32_t fill = (special && !sh_on) ? (uint32_t)k_er_kbd_keydk : (uint32_t)k_er_kbd_keylt;
  er_round_rect(kx, ky + (int32_t)k_er_kbd_shadow_dy, kw, kh, (uint32_t)k_er_kbd_keysh);
  er_round_rect(kx, ky, kw, kh, fill);

  const int32_t cx = kx + (kw / 2);
  const int32_t cy = ky + (kh / 2);
  switch (kind) {
    case k_ra_kbd_key_shift:
      er_icon_shift(cx, cy, (uint32_t)k_er_ink);
      break;
    case k_ra_kbd_key_backspace:
      er_icon_delete(cx, cy, (uint32_t)k_er_ink);
      break;
    case k_ra_kbd_key_enter:
      er_icon_return(cx, cy, (uint32_t)k_er_ink);
      break;
    default:
      er_draw_key_label(idx, kx, ky, kw, kh, fill);
      break;
  }
}

/** @brief Search-field band widget: the "Search:" label + the query / hint. */
static void er_kbd_search_render(ra_widget_t* w)
{
  (void)w;
  const int32_t qy = (int32_t)k_er_statusbar_h + (int32_t)k_er_text_pad;
  er_text_left((int32_t)k_er_pad_ui, qy, "Search:", (uint32_t)k_er_ink_muted);
  const char* shown = (s_query.len > 0U) ? s_query.buf : k_er_search_hint;
  er_text_left((int32_t)k_er_pad_ui + (int32_t)k_er_kbd_qlabel, qy, shown, (uint32_t)k_er_ink);
}

/** @brief Keyboard band widget: gray backdrop + the laid-out keys + tap targets. */
static void er_kbd_keys_render(ra_widget_t* w)
{
  (void)w;
  /* Fill the keyboard band with the iOS-style gray backdrop. */
  const int32_t band_y = (int32_t)k_er_statusbar_h + (int32_t)k_er_toolbar_h;
  (void)ra_gfx_rect(0,
                    band_y,
                    (int32_t)s_fb.width_px,
                    (int32_t)s_fb.height_px - band_y,
                    (uint32_t)k_er_kbd_bg,
                    true);

  const int32_t      ky    = band_y + (int32_t)k_er_pad_ui;
  const ra_ui_rect_t frame = {.x = (int32_t)k_er_pad_ui,
                              .y = ky,
                              .w = (int32_t)s_fb.width_px - (2 * (int32_t)k_er_pad_ui),
                              .h = (int32_t)s_fb.height_px - ky - (int32_t)k_er_pad_ui};
  (void)ra_kbd_layout_init(&s_kb, &frame);

  s_target_count = 0U;
  for (uint8_t i = 0U; i < s_kb.count; i++) {
    er_draw_key(i);
    if (s_target_count < (uint16_t)k_er_max_targets) {
      s_targets[s_target_count].rect      = s_kb.keys[i].rect;
      s_targets[s_target_count].action_id = (uint16_t)((uint16_t)k_er_act_key_base + (uint16_t)i);
      s_targets[s_target_count].reserved  = 0U;
      s_target_count++;
    }
  }
}

/** @brief Vtables for the keyboard search-field / keys band widgets. */
static const ra_widget_vtable_t k_er_kbd_search_vt = {.render = er_kbd_search_render};
static const ra_widget_vtable_t k_er_kbd_keys_vt   = {.render = er_kbd_keys_render};

/**
 * @brief Render the on-screen keyboard screen + collect its key targets.
 *
 * @details Composes two disjoint-y band widgets (#145): the search field over
 * the keyboard. The search band draws the "Search:" label + query in the top
 * strip [0, statusbar+toolbar); the keys band fills the gray backdrop and lays
 * out + draws the keys below it, collecting their tap targets. The bands occupy
 * non-overlapping y-ranges and render in array order, so the composite is
 * byte-identical to the former monolithic render while the search field can be
 * partial-flushed on its own as the query is typed.
 *
 * @pre ra_gfx is bound; ``s_fb`` reflects the framebuffer geometry.
 * @pre None.
 * @post The framebuffer holds the keyboard; ``s_kb`` + ``s_targets`` set.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_render_keyboard(void)
{
  (void)ra_gfx_clear((uint32_t)k_er_paper);
  const ra_ui_rect_t frame    = {0, 0, (int32_t)s_fb.width_px, (int32_t)s_fb.height_px};
  ra_widget_t        bands[2] = {};
  bands[0].vt                 = &k_er_kbd_search_vt;
  bands[0].fixed              = (int16_t)((int32_t)k_er_statusbar_h + (int32_t)k_er_toolbar_h);
  bands[0].visible            = true;
  bands[1].vt                 = &k_er_kbd_keys_vt;
  bands[1].flex               = 1U;
  bands[1].visible            = true;
  ra_box_t band_scratch[3];
  (void)ra_widget_layout_stack(bands, 2U, &frame, k_ra_widget_axis_col, 0, 0, band_scratch, 3U);
  for (uint16_t i = 0U; i < 2U; ++i) {
    (void)ra_widget_invalidate(&bands[i], k_ra_widget_refresh_quality);
  }
  (void)ra_widget_render_dirty(bands, 2U);
}

#ifdef RA_APP_SETTINGS
/**
 * @brief Minimal Settings screen for the optional Settings app.
 *
 * @details Only compiled when ``RA_APP_SETTINGS`` is defined -- the default
 * build excludes the Settings app entirely (it is never registered, so it is
 * absent from the launcher / nav). A real settings panel would compose its own
 * widget tree; this stub draws a paper page with an ink title band so the
 * enabled build is complete and runnable.
 *
 * @pre ::ra_gfx is bound to ::s_framebuffer.
 * @post The framebuffer holds the Settings page; no tap targets are armed.
 * @since 0.1.0
 */
static void er_render_settings(void)
{
  (void)ra_gfx_clear((uint32_t)k_er_paper);
  (void)ra_gfx_rect(0, 0, (int32_t)k_er_fb_w, (int32_t)k_er_statusbar_h, (uint32_t)k_er_ink, true);
  s_target_count = 0U;
}
#endif

/* ===========================================================================
 * App framework integration (#146)
 *
 * Each screen is re-expressed as an ``ra_app``: the screen-id stack (``s_nav``)
 * remains the source of truth for *which* screen is active, and that id is the
 * active app's id, so ::er_render_current focuses the matching app and renders
 * its tree. The render callbacks are the existing ``er_render_*`` functions, so
 * the composited chrome is byte-identical (``make ereader-golden`` stays green)
 * while the control flow is now an app composition rather than a hardcoded
 * if/else. ``RA_APP_SETTINGS`` shows the build-time "uninstallable core" knob: a
 * Settings app is only registered when the macro is defined.
 * ===========================================================================
 */

/** @brief Library / Reading / Keyboard (+ optional Settings) app instances. */
static ra_app_t s_app_library;
static ra_app_t s_app_reader;
static ra_app_t s_app_keyboard;
#ifdef RA_APP_SETTINGS
static ra_app_t s_app_settings;
#endif

/** @brief App registry storage (4 slots: 3 core + an optional Settings). */
static ra_app_t*         s_app_slots[4];
static ra_app_registry_t s_app_reg;

/** @brief App render trampoline: dispatch to the screen render by app id. */
static void er_app_render(ra_app_t* a)
{
  if (a->id == (uint16_t)k_er_screen_reading) {
    er_render_reading();
  } else if (a->id == (uint16_t)k_er_screen_keyboard) {
    er_render_keyboard();
#ifdef RA_APP_SETTINGS
  } else if (a->id == (uint16_t)k_er_screen_settings) {
    er_render_settings();
#endif
  } else {
    er_render_library();
  }
}

/** @brief Vtable shared by every screen-app (render-only; nav owns transitions). */
static const ra_app_vtable_t k_er_app_vt = {
  .render = er_app_render,
};

/** @brief Register the e-reader screen-apps into ::s_app_reg. */
static void er_apps_init(void)
{
  s_app_library  = (ra_app_t){.vt        = &k_er_app_vt,
                              .id        = (uint16_t)k_er_screen_library,
                              .name      = "Library",
                              .removable = false};
  s_app_reader   = (ra_app_t){.vt        = &k_er_app_vt,
                              .id        = (uint16_t)k_er_screen_reading,
                              .name      = "Reader",
                              .removable = false};
  s_app_keyboard = (ra_app_t){.vt        = &k_er_app_vt,
                              .id        = (uint16_t)k_er_screen_keyboard,
                              .name      = "Search",
                              .removable = false};
  (void)ra_app_registry_init(&s_app_reg,
                             s_app_slots,
                             (uint16_t)(sizeof(s_app_slots) / sizeof(s_app_slots[0])));
  (void)ra_app_register(&s_app_reg, &s_app_library);
  (void)ra_app_register(&s_app_reg, &s_app_reader);
  (void)ra_app_register(&s_app_reg, &s_app_keyboard);
#ifdef RA_APP_SETTINGS
  s_app_settings = (ra_app_t){.vt        = &k_er_app_vt,
                              .id        = (uint16_t)k_er_screen_settings,
                              .name      = "Settings",
                              .removable = true};
  (void)ra_app_register(&s_app_reg, &s_app_settings);
#endif
}

/** @brief Best-effort read of one fuel-gauge register on the touch IIC_B bus. */
static bool er_fg_read(uint8_t reg, uint8_t* out)
{
  if (out == nullptr) {
    return false;
  }
  if (ra_i3c_write((uint8_t)k_er_touch_channel, (uint8_t)k_er_fg_addr, &reg, 1U, true) != k_ra_ok) {
    return false;
  }
  return (ra_i3c_read((uint8_t)k_er_touch_channel, (uint8_t)k_er_fg_addr, out, 1U, false) ==
          k_ra_ok);
}

/** @brief Read SOC + charge direction from the fuel gauge; false if absent (NAK). */
static bool er_read_battery(uint8_t* soc, bool* charging)
{
  if ((soc == nullptr) || (charging == nullptr)) {
    return false;
  }
  uint8_t pct      = 0U;
  uint8_t crate_hi = 0U;
  if (!er_fg_read((uint8_t)k_er_fg_reg_soc, &pct)) {
    return false;
  }
  if (!er_fg_read((uint8_t)k_er_fg_reg_crate, &crate_hi)) {
    return false;
  }
  *soc      = pct;
  *charging = ((crate_hi & (uint8_t)k_er_fg_sign) == 0U);
  return true;
}

/** @brief Append "<v>%" (v in 0..255) to @p buf at *pos, bounded + NUL-terminated. */
static void er_append_pct(char* buf, uint32_t cap, uint32_t* pos, uint8_t v)
{
  char     tmp[k_er_nag_dec_max];
  uint32_t n = 0U;
  if (v == 0U) {
    tmp[n] = '0';
    n++;
  }
  while ((v > 0U) && (n < (uint32_t)k_er_nag_dec_max)) {
    tmp[n] = (char)('0' + (v % (uint8_t)k_er_nag_dec_ten));
    n++;
    v = (uint8_t)(v / (uint8_t)k_er_nag_dec_ten);
  }
  for (uint32_t i = 0U; (i < n) && (*pos < (cap - 1U)); i++) {
    buf[*pos] = tmp[n - 1U - i];
    (*pos)++;
  }
  if (*pos < (cap - 1U)) {
    buf[*pos] = '%';
    (*pos)++;
  }
  buf[*pos] = '\0';
}

/** @brief Append NUL-terminated @p s to @p buf at *pos, bounded + NUL-terminated. */
static void er_append_str(char* buf, uint32_t cap, uint32_t* pos, const char* s)
{
  /* Bounded by the buffer cap (NASA Rule 2); the NUL is the early exit. */
  for (uint32_t i = 0U; (i < (uint32_t)k_er_nag_line_max) && (s[i] != '\0') && (*pos < (cap - 1U));
       i++) {
    buf[*pos] = s[i];
    (*pos)++;
  }
  buf[*pos] = '\0';
}

/** @brief True iff (x,y) lands on the low-battery banner rect. */
static bool er_nag_hit(int32_t x, int32_t y)
{
  const int32_t bx = (int32_t)k_er_nag_margin;
  const int32_t bw = (int32_t)s_fb.width_px - (2 * (int32_t)k_er_nag_margin);
  return (x >= bx) && (x < (bx + bw)) && (y >= (int32_t)k_er_nag_top) &&
         (y < ((int32_t)k_er_nag_top + (int32_t)k_er_nag_h));
}

/** @brief Nag banner overlay widget: a centered low-battery toast over the screen. */
static void er_nag_render(ra_widget_t* w)
{
  (void)w;
  if (s_batt_nag == k_ra_batt_nag_none) {
    return;
  }
  const bool     crit = (s_batt_nag == k_ra_batt_nag_critical);
  const uint32_t bg   = crit ? (uint32_t)k_er_nag_crit_bg : (uint32_t)k_er_nag_low_bg;
  const int32_t  x    = (int32_t)k_er_nag_margin;
  const int32_t  bw   = (int32_t)s_fb.width_px - (2 * (int32_t)k_er_nag_margin);
  (void)ra_gfx_rect(x, (int32_t)k_er_nag_top, bw, (int32_t)k_er_nag_h, bg, true);
  (void)ra_gfx_rect(x, (int32_t)k_er_nag_top, bw, (int32_t)k_er_nag_h, (uint32_t)k_er_ink, false);
  char     line[k_er_nag_line_max];
  uint32_t pos = 0U;
  er_append_str(line,
                (uint32_t)k_er_nag_line_max,
                &pos,
                crit ? k_er_nag_crit_msg : k_er_nag_low_msg);
  er_append_pct(line, (uint32_t)k_er_nag_line_max, &pos, s_batt_soc);
  er_append_str(line, (uint32_t)k_er_nag_line_max, &pos, k_er_nag_hint);
  (void)ra_gfx_text_out(x + (int32_t)k_er_nag_text_dx,
                        (int32_t)k_er_nag_top + (int32_t)k_er_nag_text_dy,
                        line,
                        &ra_gfx_font_8x16,
                        (uint32_t)k_er_nag_text,
                        bg);
}

/** @brief Vtable for the low-battery nag banner overlay widget. */
static const ra_widget_vtable_t k_er_nag_vt = {.render = er_nag_render};

static void er_render_current(void)
{
  uint16_t top = (uint16_t)k_er_screen_library;
  (void)ra_ui_nav_top(&s_nav, &top);
  /* The nav top is the active screen-app's id: focus it (idempotent if already
   * focused, so no flicker) and render its widget tree. */
  (void)ra_app_launch(&s_app_reg, top);
  (void)ra_app_render(&s_app_reg);
  /* Low-battery nag (#145/#146): an app-framework-level overlay widget drawn
   * over whichever screen-app is active. A no-op at a healthy SOC, so the chrome
   * golden (rendered at the default battery) stays byte-identical. */
  if (s_batt_nag != k_ra_batt_nag_none) {
    ra_widget_t nag = {.vt = &k_er_nag_vt, .visible = true, .dirty = true};
    (void)ra_widget_render_dirty(&nag, 1U);
  }
}

/** @brief Push the current (chapter, page) on the back-stack, capacity permitting. */
static void er_push_loc(void)
{
  if (s_loc_back_count < (uint32_t)k_er_page_back_cap) {
    s_loc_back[s_loc_back_count].chapter = s_chapter_idx;
    s_loc_back[s_loc_back_count].page    = s_reading_page;
    s_loc_back_count++;
  }
}

/** @brief Number of chapters in the (mock) spine. */
static uint32_t er_spine_count(void)
{
  return (uint32_t)(sizeof(k_er_spine) / sizeof(k_er_spine[0]));
}

/**
 * @brief Flush the panel for a refresh event through the cadence policy (#78).
 *
 * @details Runs the pluggable ::s_policy for @p event to pick the waveform hint
 * + extent, then issues one ``display_flush`` over the page rect. On a continuous
 * LCD backend the hint is inert; on e-ink it selects A2 / GC16 / INIT. Refreshes
 * the SWD telemetry so the headless HIL can observe the cadence.
 *
 * @param[in] event The page-transition that occurred (open / turn / chapter).
 * @pre ::s_policy was initialised and ::s_display is valid.
 * @pre ::s_fb describes the bound framebuffer.
 * @post One panel flush is issued; ::g_er_last_hint / ::g_er_cur_page are current.
 * @note Not thread-safe; the reader UI is single-threaded.
 * @since 0.1.0
 */
static void er_flush_event(display_turn_event_t event)
{
  display_policy_decision_t dec = {};
  if (display_policy_decide(&s_policy, event, &dec) != k_ra_ok) {
    dec.hint = k_display_refresh_quality; /* safe fallback: a clean full update */
  }
  display_rect_t rect = {};
  if (display_policy_full_rect(s_fb.width_px, s_fb.height_px, &rect) != k_ra_ok) {
    rect = display_full_rect(s_display);
  }
  (void)display_flush(s_display, rect, dec.hint);
  g_er_last_hint = (uint32_t)dec.hint;
  g_er_cur_page  = s_reading_page;
}

/**
 * @brief Apply a page turn to the reading location (touch + button shared path).
 *
 * @details Delegates the pure decision to ::er_pageturn_step (next/prev, chapter
 * crossing, book-end clamp), writes the new (chapter, page) back to the file-
 * scope state, and records the matching refresh event in ::s_pending_event
 * (a same-chapter turn -> ::k_display_event_turn, a chapter crossing ->
 * ::k_display_event_chapter). A PREV crossing lands on ::k_er_page_last, which
 * ::er_render_reading clamps to the real last page after it lays the new chapter
 * out. Counts the turn for the HIL telemetry.
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
static bool er_apply_pageturn(er_dir_t dir)
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
  if (ra_reflow_find_anchor(&s_reflow_engine,
                            (const char*)&s_reflow_engine.text_pool[off + frag_off],
                            frag_len,
                            &page) != k_ra_ok) {
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
  const uint32_t count = (uint32_t)(sizeof(k_er_spine) / sizeof(k_er_spine[0]));
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

/**
 * @brief Follow an in-content `<a>` tap in the Reading body (#110).
 *
 * @details Hit-tests the laid-out reflow links at the panel tap (mapped to the
 * body's page-local space) via ra_reflow_hit_test_link(), classifies the href
 * with ra_reflow_href_split(), and routes a same-chapter `#fragment` to
 * er_nav_fragment() or a cross-chapter target to er_nav_chapter(). The prior
 * location is pushed on ::s_loc_back so Back can return.
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
static bool er_reading_link_tap(int32_t x, int32_t y)
{
  const int32_t body_top = (int32_t)k_er_statusbar_h + (int32_t)k_er_body_gap;
  uint32_t      off      = 0U;
  uint32_t      len      = 0U;
  if (ra_reflow_hit_test_link(&s_reflow_engine,
                              s_reading_page,
                              x - (int32_t)k_er_margin_x,
                              y - body_top,
                              &off,
                              &len) != k_ra_ok) {
    return false;
  }
  ra_reflow_href_kind_t kind     = k_ra_reflow_href_empty;
  uint32_t              path_len = 0U;
  uint32_t              frag_off = 0U;
  uint32_t              frag_len = 0U;
  if (ra_reflow_href_split((const char*)&s_reflow_engine.text_pool[off],
                           len,
                           &kind,
                           &path_len,
                           &frag_off,
                           &frag_len) != k_ra_ok) {
    return false;
  }
  if (kind == k_ra_reflow_href_fragment) {
    return er_nav_fragment(off, frag_off, frag_len);
  }
  if ((kind == k_ra_reflow_href_chapter) || (kind == k_ra_reflow_href_chapter_fragment)) {
    return er_nav_chapter(off, path_len);
  }
  return false;
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
/**
 * @brief Handle a tap while the Reading view is on top.
 *
 * @details In priority order: the back-region (return from a link jump, else
 * pop to the Library), an in-content `<a>` link (er_reading_link_tap), then a
 * left/right page-turn. Split out of er_handle_tap() to keep each within the
 * cognitive-complexity budget.
 *
 * @param[in] x Tap X (panel pixels).
 * @param[in] y Tap Y (panel pixels).
 * @return true iff the tap changed the reading location / screen.
 * @retval true  Re-render required.
 * @retval false Tap hit nothing actionable.
 * @pre The Reading view is on top of ::s_nav.
 * @pre ra_gfx is bound and the chapter is laid out.
 * @post The navigation / page state may change.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool er_handle_reading_tap(int32_t x, int32_t y)
{
  const ra_ui_rect_t back = {0, 0, (int32_t)k_er_back_w, (int32_t)k_er_statusbar_h};
  if (ra_ui_rect_contains(&back, x, y)) {
    /* Back returns from a link jump (footnote or chapter) first, then pops the
     * screen back to the Library (#110). */
    if (s_loc_back_count > 0U) {
      s_loc_back_count--;
      s_chapter_idx  = s_loc_back[s_loc_back_count].chapter;
      s_reading_page = s_loc_back[s_loc_back_count].page;
      return true;
    }
    uint16_t prev = 0U;
    return (ra_ui_nav_pop(&s_nav, &prev) == k_ra_ok);
  }
  /* Follow an in-content `<a>` link before falling back to page-turn. */
  if (er_reading_link_tap(x, y)) {
    return true;
  }
  /* Page-turn: a tap in the right third of the screen advances, the left third
   * goes back, the middle third is neutral. Crosses chapter boundaries and
   * clamps at the book ends (er_apply_pageturn). A no-op leaves the screen
   * unchanged (e.g. in the bitmap fallback where s_reading_pages stays 1). */
  return er_apply_pageturn(er_tap_to_dir(x, (int32_t)s_fb.width_px));
}

/**
 * @brief Route a tap on the keyboard screen: type a key, or commit on ENTER.
 *
 * @param[in] x Tap X (panel pixels).
 * @param[in] y Tap Y (panel pixels).
 * @return true if the screen must re-render (a key changed the query, or the
 *         committed query popped back to the filtered Library).
 */
static bool er_handle_keyboard_tap(int32_t x, int32_t y)
{
  uint16_t action = (uint16_t)k_er_act_none;
  bool     hit    = false;
  (void)ra_ui_hit_test(s_targets, s_target_count, x, y, &action, &hit);
  if (!hit || (action < (uint16_t)k_er_act_key_base)) {
    return false;
  }
  const uint8_t idx = (uint8_t)(action - (uint16_t)k_er_act_key_base);
  (void)ra_kbd_apply(&s_query, &s_kb, idx);
  if (s_query.committed) {
    uint16_t prev = (uint16_t)k_er_screen_library;
    return (ra_ui_nav_pop(&s_nav, &prev) == k_ra_ok); /* ENTER -> filtered Library */
  }
  return true; /* re-render to show the updated query */
}

static bool er_handle_tap(int32_t x, int32_t y)
{
  /* Default any screen/location change to a clean full refresh; a same-chapter
   * page turn lowers this to a fast partial inside er_apply_pageturn. */
  s_pending_event = k_display_event_chapter;
  /* A tap on the low-battery banner dismisses it (acknowledge). ra_batt will not
   * re-raise the same band until the battery recovers, so it stays dismissed
   * until the next descent into a worse band, or a recovery then re-drain. */
  if ((s_batt_nag != k_ra_batt_nag_none) && er_nag_hit(x, y)) {
    s_batt_nag = k_ra_batt_nag_none;
    return true;
  }
  uint16_t top = (uint16_t)k_er_screen_library;
  (void)ra_ui_nav_top(&s_nav, &top);
  if (top == (uint16_t)k_er_screen_reading) {
    return er_handle_reading_tap(x, y);
  }
  if (top == (uint16_t)k_er_screen_keyboard) {
    return er_handle_keyboard_tap(x, y);
  }
#ifdef RA_APP_SETTINGS
  if (top == (uint16_t)k_er_screen_settings) {
    /* Settings has no tap targets; any tap returns to the Library. */
    return (ra_ui_nav_pop(&s_nav, &top) == k_ra_ok);
  }
#endif
  uint16_t action = (uint16_t)k_er_act_none;
  bool     hit    = false;
  (void)ra_ui_hit_test(s_targets, s_target_count, x, y, &action, &hit);
#ifdef RA_APP_SETTINGS
  if (hit && (action == (uint16_t)k_er_act_settings)) {
    return (ra_ui_nav_push(&s_nav, (uint16_t)k_er_screen_settings) == k_ra_ok);
  }
#endif
  if (hit && (action == (uint16_t)k_er_act_open_book)) {
    s_reading_page   = 0U;                   /* always open a book at its first page */
    s_chapter_idx    = 0U;                   /* and its first chapter */
    s_loc_back_count = 0U;                   /* with a fresh navigation back-stack */
    s_pending_event  = k_display_event_open; /* fresh book -> clean INIT */
    return (ra_ui_nav_push(&s_nav, (uint16_t)k_er_screen_reading) == k_ra_ok);
  }
  if (hit && (action == (uint16_t)k_er_act_search)) {
    (void)ra_kbd_text_init(&s_query); /* fresh query each time Search opens */
    return (ra_ui_nav_push(&s_nav, (uint16_t)k_er_screen_keyboard) == k_ra_ok);
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
      er_flush_event(s_pending_event);
    }
  }
  s_was_touching = touching;
}

/** @brief Read a user switch (active-low); true == pressed. */
static bool er_sw_pressed(ra_port_pin_t pin)
{
  ra_level_t level = k_ra_level_high;
  if (ra_gpio_read(pin, &level) != k_ra_ok) {
    return false; /* unreadable switch -> treat as released (touch still works) */
  }
  return (level == k_ra_level_low);
}

/**
 * @brief Poll SW1/SW2 and turn a page on a fresh press (#78).
 *
 * @details Edge-triggered per button (a held switch fires once): SW1 = previous
 * page, SW2 = next. A turn re-renders the Reading view and flushes through the
 * cadence policy. Inert outside the Reading view (::er_apply_pageturn no-ops when
 * there is nothing to turn). board_sim drives the switches via ``--button``.
 *
 * @pre The switch pins were configured as inputs (::app_bringup_buttons).
 * @pre ::s_display / ::s_fb are valid.
 * @post On a fresh press that turns a page, the new page is shown and flushed.
 * @post ::s_was_sw1 / ::s_was_sw2 track the held state.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_poll_buttons(void)
{
  const bool sw1    = er_sw_pressed(k_er_pin_sw1);
  const bool sw2    = er_sw_pressed(k_er_pin_sw2);
  const bool fresh1 = sw1 && !s_was_sw1;
  const bool fresh2 = sw2 && !s_was_sw2;
  s_was_sw1         = sw1;
  s_was_sw2         = sw2;
  /* Page-turn buttons act only in the Reading view (a press elsewhere is a
   * no-op, so it cannot disturb the reading location from another screen). */
  uint16_t top = (uint16_t)k_er_screen_library;
  (void)ra_ui_nav_top(&s_nav, &top);
  if ((top != (uint16_t)k_er_screen_reading) || !(fresh1 || fresh2)) {
    return;
  }
  if (er_apply_pageturn(er_buttons_to_dir(fresh1, fresh2))) {
    er_render_current();
    er_flush_event(s_pending_event);
  }
}

/**
 * @brief Poll the fuel gauge, fold it into the nag policy, and toggle the banner.
 *
 * @details Best-effort: a fuel-gauge NAK (none fitted, e.g. a stock EVM) leaves
 * the chrome untouched. ::ra_batt edge-triggers a LOW (<=20%) / CRITICAL (<=10%)
 * nag, which raises (or upgrades) the persistent banner; the banner clears once
 * the battery recovers above the low band's hysteresis margin or starts
 * charging. The chrome is re-rendered only when the banner state changes, so a
 * steady battery costs nothing. The gauge is polled only every ::k_er_batt_every
 * frames (~1 Hz): each read is two I2C transactions, so polling per frame
 * starved the touch loop. On the sim the board_sim battery slider drives the
 * gauge, so dragging below 20% / 10% raises the banner and back up clears it;
 * tapping the banner dismisses it.
 *
 * @pre ::app_bringup_touch brought up IIC_B channel 0 (shared with the gauge).
 * @pre ::s_batt_mon was initialised by ::ra_batt_monitor_init.
 * @post On a banner state change the chrome is re-rendered + flushed.
 * @post ::s_batt_nag reflects the banner shown.
 * @note Not thread-safe.
 *
 * @par MC/DC:
 * Decision: `(s_batt_nag != k_ra_batt_nag_none) && (chg || (soc > recover))`
 * (3 conditions: banner-active, charging, soc-recovered). The ra_batt policy's
 * own edge / re-arm decisions are MC/DC-tested in tests/test_ra_batt.c; this
 * clear-the-banner glue is exercised by the board_sim battery-slider gate
 * (dragging below 20/10 percent raises the banner; back up or toggling charge
 * clears it) rather than a host unit test, per the hw_pending board-demo
 * convention (no host test, like smbus_demo).
 *
 * @since 0.1.0
 */
static void er_poll_battery(void)
{
  /* Throttle: the gauge moves slowly and each read is two I2C transactions, so
   * polling it every frame dominated the loop (the GT911 touch read is the only
   * other per-frame I2C). Poll ~once a second so touch stays responsive. */
  static uint32_t s_skip = 0U;
  if (s_skip != 0U) {
    s_skip--;
    return;
  }
  s_skip = (uint32_t)k_er_batt_every - 1U;

  uint8_t soc = 0U;
  bool    chg = false;
  if (!er_read_battery(&soc, &chg)) {
    return; /* best-effort: no gauge fitted -> no banner */
  }
  s_batt_soc        = soc;
  ra_batt_nag_t nag = k_ra_batt_nag_none;
  if (ra_batt_update(&s_batt_mon, soc, chg, &nag) != k_ra_ok) {
    return;
  }
  const ra_batt_nag_t prev = s_batt_nag;
  const uint8_t recover = (uint8_t)((uint8_t)k_ra_batt_low_pct + (uint8_t)k_ra_batt_rearm_margin);
  if (nag != k_ra_batt_nag_none) {
    s_batt_nag = nag; /* new low / critical edge -> raise or upgrade the banner */
  } else if ((s_batt_nag != k_ra_batt_nag_none) && (chg || (soc > recover))) {
    s_batt_nag = k_ra_batt_nag_none; /* recovered or charging -> clear the banner */
  }
  if (s_batt_nag != prev) {
    er_render_current();
    er_flush_event(k_display_event_chapter);
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  app_bringup_clocks();
  app_bringup_panel();
  app_bringup_gfx();
  app_bringup_touch();
  (void)ra_batt_monitor_init(&s_batt_mon); /* low-battery nag policy (#145/#146) */
  /* User switches SW1/SW2 as inputs with internal pull-ups (active-low). Best-
   * effort: a config failure just leaves page-turn on the touch path. */
  (void)ra_gpio_input_init(k_er_pin_sw1, k_ra_pull_up);
  (void)ra_gpio_input_init(k_er_pin_sw2, k_ra_pull_up);
  er_try_load_font(); /* Best-effort SD font for the ra_reflow Reading body. */

  /* Default refresh cadence: fast A2 turns with a periodic GC16 clean (#78). */
  (void)display_policy_init(&s_policy, k_display_policy_fast_clean, (uint16_t)k_er_clean_every);
  (void)ra_ui_nav_init(&s_nav, (uint16_t)k_er_screen_library);
  er_apps_init(); /* re-express the screens as ra_app instances (#146) */
  er_render_current();
  er_flush_event(k_display_event_open); /* boot -> a clean INIT panel update */

  uint32_t frame = 0U;
  while (1) {
    er_poll_touch();   /* fast cadence so taps feel responsive */
    er_poll_buttons(); /* page-turn switches */
    er_poll_battery(); /* self-throttled (~1 Hz) low-battery nag */
    /* Heartbeat LED toggles on a slow sub-cadence so it blinks (~1 Hz) instead
     * of strobing at the input-poll rate. */
    if ((frame % (uint32_t)k_er_led_every) == 0U) {
      (void)ra_board_led_toggle(k_ra_board_led_blue);
    }
    frame++;
    ra_delay_ms((uint32_t)k_er_frame_ms);
  }
  return 0;
}
#pragma GCC diagnostic pop
