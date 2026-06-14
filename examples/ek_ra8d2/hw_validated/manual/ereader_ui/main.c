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

#include "ra_board_ek_ra8d2.h"
#include "ra_box.h"
#include "ra_cgc.h"
#include "ra_display_pal.h"
#include "ra_display_pal_lcd.h"
#include "ra_err.h"
#include "ra_fs.h"
#include "ra_gfx.h"
#include "ra_gfx_font.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_panel.h"
#include "ra_panel_timing.h"
#include "ra_port_utils.h"
#include "ra_reflow.h"
#include "ra_sci_spi.h"
#include "ra_sdmmc_spi.h"
#include "ra_sdramc.h"
#include "ra_spi.h"
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
 * @brief Reading-view chapter as XHTML, reflowed by ra_reflow when an SD
 *        font is present (the same prose as ::k_er_body_lines, but laid out
 *        live at the proportional type scale instead of pre-wrapped bitmap
 *        lines). Kept short so the software glyph rasteriser stays quick
 *        under the board_sim CPU emulator.
 */
static const char k_er_chapter_xhtml[] =
  "<html><body><h1>The Time Machine</h1>"
  "<p>The Time Traveller (for so it will be convenient to speak of him) was "
  "expounding a recondite matter to us. His pale grey eyes shone and twinkled, "
  "and his usually pale face was flushed and animated.</p>"
  "<p>The fire burnt brightly, and the soft radiance of the incandescent lights "
  "in the lilies of silver caught the bubbles that flashed and passed in our "
  "glasses. Our chairs, being his patents, embraced and caressed us rather than "
  "submitted to be sat upon, and thought roamed gracefully free of the trammels "
  "of precision.</p></body></html>";

/**
 * @enum er_reflow_cfg_t
 * @brief SD-font load + ra_reflow body-render tunables (no magic numbers).
 */
typedef enum : uint32_t {
  k_er_font_cap    = 512U * 1024U, /**< Max font read off the card (bytes).  */
  k_er_font_min    = 16U,          /**< Smallest plausible font blob (bytes).*/
  k_er_reflow_px   = 22U,          /**< Body type size (pixels).             */
  k_er_spi_chan    = 0U,           /**< Pmod2 / J25 = SCI0 Simple-SPI.       */
  k_er_reflow_ink  = 0xFF101010U,  /**< Body text colour (near-black ARGB).  */
  k_er_reflow_link = 0xFF2A52BEU,  /**< Anchor colour (cerulean ARGB).       */
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

/** @brief Reading view: current reflow page index (0-based). */
static uint32_t s_reading_page;

/** @brief Reading view: total reflow pages from the last layout (>= 1). */
static uint32_t s_reading_pages = 1U;

/** @brief Debounce: true while a contact is held, to fire once per tap. */
static bool s_was_touching;

/** @brief Font blob read off the SD card -- lives in SDRAM (hundreds of KiB). */
static uint8_t s_font_buf[k_er_font_cap] __attribute__((section(".sdram_data")));

/** @brief ra_reflow engine for the Reading body (page / glyph / token pools). */
static ra_reflow_t s_reflow_engine;

/** @brief Bytes of font read off the card (0 if none). */
static uint32_t s_font_len;

/** @brief True once an SD font is loaded; gates the ra_reflow body render. */
static bool s_have_font;

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
 * @brief ra_sdmmc_spi set-clock shim onto ra_sci_spi.
 *
 * @param[in] ctx Pointer to the cached PCLKA rate (Hz).
 * @param[in] hz  Requested SPI clock (Hz).
 * @return ra_err_t from ra_sci_spi_set_clock.
 * @retval k_ra_ok Clock applied.
 * @pre ra_sci_spi_init has run for the channel.
 * @pre @p ctx points at a valid PCLKA rate.
 * @post The SCI0 Simple-SPI bit rate matches @p hz as closely as the divisors allow.
 * @post No other channel state changes.
 * @note Not thread-safe.
 * @since 0.1.0
 */
/* cppcheck-suppress constParameterCallback */
static ra_err_t er_spi_set_clock(void* ctx, uint32_t hz)
{
  const uint32_t pclka_hz = *(const uint32_t*)ctx;
  return ra_sci_spi_set_clock((uint8_t)k_er_spi_chan, hz, pclka_hz);
}

/**
 * @brief ra_sdmmc_spi chip-select shim (active-low, GPIO-held).
 *
 * @param[in] ctx      Unused.
 * @param[in] asserted true selects the card (CS low); false deselects (CS high).
 * @return ra_err_t from ra_gpio_write.
 * @retval k_ra_ok CS level driven.
 * @pre k_er_pin_cs is a GPIO output.
 * @pre None.
 * @post The CS line reflects @p asserted.
 * @post No other pin changes.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t er_spi_cs(void* ctx, bool asserted)
{
  (void)ctx;
  return ra_gpio_write(k_er_pin_cs, asserted ? k_ra_level_low : k_ra_level_high);
}

/**
 * @brief ra_sdmmc_spi full-duplex transfer shim onto ra_sci_spi.
 *
 * @param[in]  ctx Unused.
 * @param[in]  tx  Bytes to clock out (may be NULL for read-only).
 * @param[out] rx  Bytes clocked in (may be NULL for write-only).
 * @param[in]  len Transfer length in bytes.
 * @return ra_err_t from ra_sci_spi_xfer.
 * @retval k_ra_ok Transfer complete.
 * @pre ra_sci_spi_init has run for the channel.
 * @pre @p tx / @p rx hold @p len bytes when non-NULL.
 * @post @p rx holds the bytes shifted in when non-NULL.
 * @post The bus is left idle (CS unchanged by this call).
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t er_spi_xfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
{
  (void)ctx;
  return ra_sci_spi_xfer((uint8_t)k_er_spi_chan, tx, rx, len);
}

/**
 * @brief Route Pmod2 to SCI0 Simple-SPI and bring the channel up.
 *
 * @details Mirrors sd_font_render: caches PCLKA, routes SCK/CIPO/COPI to
 *          SCI0, claims CS as a GPIO output held high, then inits the
 *          channel at the SD power-on clock.
 *
 * @return ra_err_t -- the first failing step, or k_ra_ok.
 * @retval k_ra_ok SPI ready for ra_sdmmc_spi.
 * @pre app_bringup_clocks has run (PCLKA + MSTP up).
 * @pre The Pmod2 pins are free (no other peripheral routed).
 * @post On success SCI0 Simple-SPI is initialised and CS idles high.
 * @post On failure the channel is left unconfigured; caller skips the SD font.
 * @note Not thread-safe; single-shot helper.
 * @since 0.1.0
 */
static ra_err_t er_setup_sd_spi(void)
{
  ra_err_t err = ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &s_pclka_hz);
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_pfs_route_peripheral(k_er_pin_sck, k_ra_psel_sci_async, "er.sck");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_pfs_route_peripheral(k_er_pin_cipo, k_ra_psel_sci_async, "er.cipo");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_pfs_route_peripheral(k_er_pin_copi, k_ra_psel_sci_async, "er.copi");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_gpio_output_init(k_er_pin_cs, k_ra_level_high);
  if (err != k_ra_ok) {
    return err;
  }
  const ra_sci_spi_cfg_t cfg = {
    .baud_hz   = (uint32_t)k_ra_sdmmc_spi_clock_init_hz,
    .pclk_hz   = s_pclka_hz,
    .mode      = k_ra_spi_mode_0,
    .lsb_first = false,
  };
  return ra_sci_spi_init((uint8_t)k_er_spi_chan, &cfg);
}

/**
 * @brief Best-effort: bring up the SD card and read FONT.OTF into s_font_buf.
 *
 * @details
 * Entirely non-fatal: any failure (no Pmod, no card, no FONT.OTF) leaves
 * ::s_have_font false and the Reading view falls back to the bundled bitmap
 * font, so the chrome is never broken by the absence of an SD card. On
 * success ::s_font_buf / ::s_font_len hold the face for ra_reflow.
 *
 * @pre app_bringup_clocks has run.
 * @pre ::s_font_buf is reachable (SDRAM).
 * @post On success ::s_have_font is true and ::s_font_len > 0.
 * @post On any failure ::s_have_font stays false (no panic).
 * @note Not thread-safe; single-shot helper.
 * @since 0.1.0
 */
static void er_try_load_font(void)
{
  if (er_setup_sd_spi() != k_ra_ok) {
    return;
  }
  const ra_sdmmc_spi_transport_t transport = {
    .set_clock = er_spi_set_clock,
    .cs        = er_spi_cs,
    .xfer      = er_spi_xfer,
    .ctx       = &s_pclka_hz,
  };
  if (ra_sdmmc_spi_init(&transport) != k_ra_ok) {
    return;
  }
  ra_fs_backend_t backend = {};
  if (ra_sdmmc_spi_bind_fs_backend(&backend) != k_ra_ok) {
    return;
  }
  ra_fs_mount_t* mount = nullptr;
  if (ra_fs_mount(&backend, &mount) != k_ra_ok) {
    return;
  }
  ra_fs_file_t* file = nullptr;
  if (ra_fs_open(mount, "FONT.OTF", k_ra_fs_mode_read, &file) != k_ra_ok) {
    return;
  }
  uint32_t       got = 0U;
  const ra_err_t err = ra_fs_read(file, s_font_buf, (uint32_t)k_er_font_cap, &got);
  (void)ra_fs_close(file);
  if (err != k_ra_ok) {
    return;
  }
  if (got >= (uint32_t)k_er_font_min) {
    s_font_len  = got;
    s_have_font = true;
  }
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
  if (!s_have_font) {
    return false;
  }
  const int32_t body_w = (int32_t)s_fb.width_px - ((int32_t)k_er_margin_x * 2);
  const int32_t body_h =
    height - (int32_t)k_er_statusbar_h - (int32_t)k_er_footer_h - ((int32_t)k_er_body_gap * 2);
  if (body_w <= 0) {
    return false;
  }
  if (body_h <= 0) {
    return false;
  }
  if (ra_reflow_init((uint16_t)body_w,
                     (uint16_t)body_h,
                     s_font_buf,
                     s_font_len,
                     (uint16_t)k_er_reflow_px,
                     (uint32_t)k_er_reflow_ink,
                     (uint32_t)k_er_reflow_link,
                     &s_reflow_engine) != k_ra_ok) {
    return false;
  }
  uint32_t pages = 0U;
  if (ra_reflow_layout_chapter(&s_reflow_engine,
                               (const uint8_t*)k_er_chapter_xhtml,
                               (uint32_t)(sizeof(k_er_chapter_xhtml) - 1U),
                               &pages) != k_ra_ok) {
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
 * @brief Paint the Reading body: reflowed SD-font text, else bitmap lines.
 *
 * @param[in] height Framebuffer height in pixels.
 *
 * @pre ra_gfx is bound.
 * @pre @p height matches the bound framebuffer.
 * @post The body band holds reflowed text (SD font) or the bitmap fallback.
 * @post Drawing stops before the footer band.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void er_draw_reading_body(int32_t height)
{
  const int32_t body_top = (int32_t)k_er_statusbar_h + (int32_t)k_er_body_gap;
  if (er_draw_reading_body_reflow(body_top, height)) {
    return; /* Live proportional text from the SD-loaded font. */
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
  /* In the reflow path the bar tracks the live page; the bitmap fallback keeps
   * the static placeholder ratio so the no-card golden image is unchanged. */
  const int32_t pg_cur = s_have_font ? (int32_t)(s_reading_page + 1U) : (int32_t)k_er_page_current;
  const int32_t pg_tot = s_have_font ? (int32_t)s_reading_pages : (int32_t)k_er_page_total;
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
  uint16_t action = (uint16_t)k_er_act_none;
  bool     hit    = false;
  (void)ra_ui_hit_test(s_targets, s_target_count, x, y, &action, &hit);
  if (hit && (action == (uint16_t)k_er_act_open_book)) {
    s_reading_page = 0U; /* always open a book at its first page */
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
