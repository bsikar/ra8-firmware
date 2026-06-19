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
#include "figure_fixture.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_box.h"
#include "ra_cgc.h"
#include "ra_display_pal.h"
#include "ra_display_pal_lcd.h"
#include "ra_err.h"
#include "ra_gfx.h"
#include "ra_gfx_font.h"
#include "ra_isr.h"
#include "ra_keyboard.h"
#include "ra_mstp.h"
#include "ra_panel.h"
#include "ra_panel_timing.h"
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
  k_er_screen_library  = 1U, /**< Library / home grid.               */
  k_er_screen_reading  = 2U, /**< Reading view.                      */
  k_er_screen_keyboard = 3U, /**< On-screen keyboard (search entry). */
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
 * @brief Reading-view chapter as XHTML, reflowed by ra_reflow when an SD
 *        font is present (the same prose as ::k_er_body_lines, but laid out
 *        live at the proportional type scale instead of pre-wrapped bitmap
 *        lines). Kept short so the software glyph rasteriser stays quick
 *        under the board_sim CPU emulator.
 */
static const char k_er_chapter_xhtml[] =
  "<html><head><style>h1 { text-align: center; }"
  ".byline { text-align: right; text-decoration: underline; }</style></head>"
  "<body><h1>The Time Machine</h1>"
  "<p class=\"byline\">by H. G. Wells</p>"
  "<img src=\"fig.png\"/>"
  "<p>The Time Traveller (for so it will be convenient to speak of him) was "
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
static ra_err_t er_image_loader(void*           ctx,
                                const char*     href,
                                uint32_t        href_len,
                                const uint8_t** out_bytes,
                                size_t*         out_len)
{
  (void)ctx;
  (void)href;
  (void)href_len;
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
    const ra_box_t item_t =
      er_leaf(0, 1U, (uint32_t)k_ra_box_no_colour, (uint32_t)k_ra_box_no_colour);
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
 * Reading screen -- ra_reflow body text (SD font) with a bitmap fallback
 * =========================================================================== */

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
  /* Publish the page count (>= 1) for the footer + page-turn taps, and clamp the
   * current page in case the chapter now paginates shorter than before. */
  s_reading_pages = pages;
  if (s_reading_page >= pages) {
    s_reading_page = pages - 1U;
  }
  (void)
    ra_reflow_render_page_at(&s_reflow_engine, s_reading_page, (int32_t)k_er_margin_x, body_top);
  (void)ra_reflow_close(&s_reflow_engine);
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

  const bool reflowed = er_draw_reading_body(height);

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
  /* In the reflow path the bar tracks the live page; the bitmap fallback keeps
   * the static placeholder ratio. */
  const int32_t pg_cur = reflowed ? (int32_t)(s_reading_page + 1U) : (int32_t)k_er_page_current;
  const int32_t pg_tot = reflowed ? (int32_t)s_reading_pages : (int32_t)k_er_page_total;
  const int32_t fill_w = (track_w * pg_cur) / pg_tot;
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

/**
 * @brief Render the on-screen keyboard screen + collect its key targets.
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
  const int32_t qy = (int32_t)k_er_statusbar_h + (int32_t)k_er_text_pad;
  er_text_left((int32_t)k_er_pad_ui, qy, "Search:", (uint32_t)k_er_ink_muted);
  const char* shown = (s_query.len > 0U) ? s_query.buf : k_er_search_hint;
  er_text_left((int32_t)k_er_pad_ui + (int32_t)k_er_kbd_qlabel, qy, shown, (uint32_t)k_er_ink);

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

static void er_render_current(void)
{
  uint16_t top = (uint16_t)k_er_screen_library;
  (void)ra_ui_nav_top(&s_nav, &top);
  if (top == (uint16_t)k_er_screen_reading) {
    er_render_reading();
  } else if (top == (uint16_t)k_er_screen_keyboard) {
    er_render_keyboard();
  } else {
    er_render_library();
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
  /* Page-turn: a tap in the right half of the body advances a page, the left
   * half goes back. No-ops at the ends and in the bitmap fallback (where
   * s_reading_pages stays 1), so a tap there leaves the screen unchanged. */
  const int32_t mid  = (int32_t)s_fb.width_px / 2;
  uint32_t      want = s_reading_page;
  if (x >= mid) {
    if ((s_reading_page + 1U) < s_reading_pages) {
      want = s_reading_page + 1U;
    }
  } else if (s_reading_page > 0U) {
    want = s_reading_page - 1U;
  }
  if (want != s_reading_page) {
    s_reading_page = want;
    return true; /* re-render at the new page */
  }
  return false;
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
  uint16_t top = (uint16_t)k_er_screen_library;
  (void)ra_ui_nav_top(&s_nav, &top);
  if (top == (uint16_t)k_er_screen_reading) {
    return er_handle_reading_tap(x, y);
  }
  if (top == (uint16_t)k_er_screen_keyboard) {
    return er_handle_keyboard_tap(x, y);
  }
  uint16_t action = (uint16_t)k_er_act_none;
  bool     hit    = false;
  (void)ra_ui_hit_test(s_targets, s_target_count, x, y, &action, &hit);
  if (hit && (action == (uint16_t)k_er_act_open_book)) {
    s_reading_page   = 0U; /* always open a book at its first page */
    s_chapter_idx    = 0U; /* and its first chapter */
    s_loc_back_count = 0U; /* with a fresh navigation back-stack */
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
  er_try_load_font(); /* Best-effort SD font for the ra_reflow Reading body. */

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
