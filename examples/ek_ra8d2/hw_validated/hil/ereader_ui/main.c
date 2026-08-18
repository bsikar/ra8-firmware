/**
 * @file examples/ek_ra8d2/hw_validated/hil/ereader_ui/main.c
 * @brief E-reader device chrome -- Library + Reading screens
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * E-reader UI chrome (issue #80). The application
 * shell is laid out by the bounded box-model engine ``libs/ra8_box``
 * (the #80 box model: stacks, a fixed-column grid, padding/gap, fixed
 * vs flex sizing), rendered into the GLCDC framebuffer through
 * ``libs/ra8_gfx`` in the flat 16-level-grayscale language of the verified
 * "PAPYR" proof-of-concept, and navigated through the ``libs/ra8_ui``
 * screen stack. The Reading body renders real reflowed book text through
 * ``libs/ra8_reflow`` when a font is present on the microSD (``FONT.OTF``);
 * with no card it falls back to the bundled ``ra8_gfx`` bitmap font (#83).
 *
 * Two screens:
 *   - Library: status bar, toolbar (search + count), a 2-column grid of
 *     book cards (cover + title + author + reading-progress bar), and a
 *     bottom navigation strip -- all laid out by ra8_box.
 *   - Reading: status bar, body text at the reading margin, footer with
 *     page label + progress bar.
 *
 * Boot: clocks/MSTP/SysTick/LEDs, then SDRAM + GLCDC (the 1024x600 RGB565
 * framebuffer lives in external SDRAM), then ra8_gfx bound to it. Layout
 * is resolution-adaptive from the framebuffer the backend reports.
 *
 * The screen rendering, in-content navigation, on-screen keyboard, battery
 * nag and input polling live in the per-aspect helper translation units under
 * ``src/`` (``ereader_ui_screens.c`` + ``ereader_ui_input.c``); this file keeps
 * boot, the app-framework screen dispatch, and the main loop. See
 * ``src/ereader_ui_steps.h`` for the shared contract.
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
#include "literata_latin1.h"
#include "ra8_app.h"
#include "ra8_batt.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_board_ek_ra8d2_touch.h"
#include "ra8_boot_entry.h"
#include "ra8_box.h"
#include "ra8_cgc.h"
#include "ra8_display_pal.h"
#include "ra8_display_pal_lcd.h"
#include "ra8_display_pal_policy.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_panel_timing.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_reflow.h"
#include "ra8_sdfont.h"
#include "ra8_sdramc.h"
#include "ra8_time.h"
#include "ra8_touch.h"
#include "ra8_ui.h"

/* ===========================================================================
 * Static content (ASCII; public-domain titles)
 *
 * Defined here (single definition site) and shared with the helper translation
 * units through the ``extern`` declarations in ereader_ui_steps.h.
 * =========================================================================== */

/** @brief Wordmark / status text. */
const char k_er_wordmark[6] = "PAPYR";
static_assert(sizeof(k_er_wordmark) == 6U, "k_er_wordmark size pins the er_read_sb_render advance");
const char k_er_lib_heading[]  = "PAPYR   Library";
const char k_er_status_right[] = "10:24   98%";
const char k_er_search_hint[]  = "Search";
const char k_er_count_text[]   = "6 books";
const char k_er_book_title[]   = "The Time Machine";
const char k_er_page_label[]   = "Page 12 of 248";
const char k_er_chapter[]      = "I";

/** @brief Bottom-nav destinations. */
const char* const k_er_nav_items[] = {"Library", "Store", "Notes", "Settings"};
static_assert((sizeof(k_er_nav_items) / sizeof(k_er_nav_items[0])) == (size_t)k_er_nav_count,
              "k_er_nav_count must match k_er_nav_items");

/** @brief Demo shelf (public-domain works). */
const er_book_t k_er_books[] = {
  {"The Time Machine", "H. G. Wells", 5U},
  {"Frankenstein", "Mary Shelley", 100U},
  {"Pride and Prejudice", "Jane Austen", 42U},
  {"Moby-Dick", "Herman Melville", 12U},
  {"The Republic", "Plato", 68U},
  {"Meditations", "M. Aurelius", 30U},
};
static_assert((sizeof(k_er_books) / sizeof(k_er_books[0])) == (size_t)k_er_book_count,
              "k_er_book_count must match k_er_books");

/**
 * @brief Reading-view body paragraph lines (pre-wrapped for the font).
 */
const char* const k_er_body_lines[] = {
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
static_assert((sizeof(k_er_body_lines) / sizeof(k_er_body_lines[0])) ==
                (size_t)k_er_body_line_count,
              "k_er_body_line_count must match k_er_body_lines");

/**
 * @brief Reading-view chapter as XHTML, reflowed by ra8_reflow when an SD
 *        font is present (the same prose as ::k_er_body_lines, but laid out
 *        live at the proportional type scale instead of pre-wrapped bitmap
 *        lines). Kept short so the software glyph rasteriser stays quick
 *        under the ra8_emulator CPU emulator.
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

/** @brief Two-chapter mock spine for in-content cross-chapter navigation (#110). */
const er_chapter_t k_er_spine[] = {
  {k_er_chapter_xhtml,
   (uint32_t)(sizeof(k_er_chapter_xhtml) - 1U),
   "ch1.xhtml",
   (uint32_t)(sizeof("ch1.xhtml") - 1U)},
  {k_er_chapter2_xhtml,
   (uint32_t)(sizeof(k_er_chapter2_xhtml) - 1U),
   "ch2.xhtml",
   (uint32_t)(sizeof("ch2.xhtml") - 1U)},
};
static_assert((sizeof(k_er_spine) / sizeof(k_er_spine[0])) == (size_t)k_er_spine_count,
              "k_er_spine_count must match k_er_spine");

/** @brief Index of the chapter currently shown in the Reading view. */
uint32_t s_chapter_idx;

/** @brief Pmod2 SPI pins (J25) -- SCI0 Simple-SPI, per sd_font_render. */
static const ra8_port_pin_t k_er_pin_sck  = (ra8_port_pin_t)k_ra8_board_pmod2_spi_sck;
static const ra8_port_pin_t k_er_pin_cipo = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cipo;
static const ra8_port_pin_t k_er_pin_copi = (ra8_port_pin_t)k_ra8_board_pmod2_spi_copi;
static const ra8_port_pin_t k_er_pin_cs   = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cs;

/* ===========================================================================
 * Static storage
 * =========================================================================== */

/**
 * @var s_framebuffer
 * @brief RGB565 framebuffer in external SDRAM, AXI-burst aligned.
 */
[[gnu::section(".sdram_data"),
  gnu::aligned(
    k_er_fb_align)]] static uint16_t s_framebuffer[(size_t)k_er_fb_h * (size_t)k_er_fb_w];

/** @brief Display PAL config -- LCD/GLCDC backend over the SDRAM buffer. */
static const display_cfg_t k_er_display_cfg = {
  .iface             = &k_display_backend_lcd_ra8_glcdc,
  .framebuffer       = s_framebuffer,
  .framebuffer_bytes = sizeof(s_framebuffer),
  .width_px          = (uint16_t)k_er_fb_w,
  .height_px         = (uint16_t)k_er_fb_h,
  .pixfmt            = k_display_pixfmt_rgb565,
  .panel_timing      = &s_ra8_panel_ek_ra8d2_timing,
};

/** @brief PAL handle returned by display_init. */
static display_handle_t* s_display = nullptr;

/** @brief Mutable copy of the FB descriptor; populated at boot. */
display_fb_t s_fb;

/** @brief Navigation stack (which screen is shown). */
ra8_ui_nav_t s_nav;

/** @brief Tap targets for the current screen (rect + action id). */
ra8_ui_target_t s_targets[k_er_max_targets];

/** @brief Number of tap targets currently populated. */
uint16_t s_target_count;

/** @brief On-screen keyboard grid (built by er_render_keyboard, read on tap). */
ra8_kbd_layout_t s_kb;

/** @brief Live search query typed on the keyboard; filters the Library shelf. */
ra8_kbd_text_t s_query;

/** @brief Reading view: current reflow page index (0-based). */
uint32_t s_reading_page;

/** @brief Reading view: total reflow pages from the last layout (>= 1). */
uint32_t s_reading_pages = 1U;

/** @brief Low-battery nag policy state (edge-triggered, see ra8_batt). */
ra8_batt_monitor_t s_batt_mon;
/** @brief Nag currently shown over the chrome (none = no banner). */
ra8_batt_nag_t s_batt_nag = k_ra8_batt_nag_none;
/** @brief Last fuel-gauge SOC percent (for the banner text). */
uint8_t s_batt_soc = 0U;
/** @brief Set by er_handle_tap when a tap only toggled the nag (incremental repaint). */
bool s_nag_region_only = false;

/** @brief Refresh-cadence policy driving e-ink page-turn waveforms (#78). */
static display_policy_t s_policy;

/**
 * @brief Pending refresh event for the next flush (set by a tap/button handler).
 * @details Defaults to a clean chapter-boundary refresh for any screen change;
 *          a same-chapter page turn lowers it to ::k_display_event_turn.
 */
display_turn_event_t s_pending_event = k_display_event_chapter;

/** @brief SWD / `--dump-sym` telemetry for the headless page-turn HIL (#78). */
volatile uint32_t g_er_cur_page;   /**< Current reading page after the last turn.       */
volatile uint32_t g_er_turns;      /**< Count of page turns applied since boot.         */
volatile uint32_t g_er_last_hint;  /**< Last `display_refresh_hint_t` flushed.          */
volatile uint32_t g_er_loop_ticks; /**< Free-running main-loop counter -- HIL liveness. */

/** @brief Default clean-refresh cadence: a GC16 every N fast turns. */
enum : uint16_t {
  k_er_clean_every = 8U, /**< Er clean every. */
};

/** @brief Reading back-stack for link round-trips (footnote + chapter) (#110). */
er_loc_t s_loc_back[k_er_page_back_cap];

/** @brief Entries used in ::s_loc_back. */
uint32_t s_loc_back_count;

/** @brief Font blob read off the SD card -- lives in SDRAM (hundreds of KiB). */
[[gnu::section(".sdram_data")]] uint8_t s_font_buf[k_er_font_cap];

/** @brief Image-decode bump arena in SDRAM (covers / figures are megabytes). */
[[gnu::section(".sdram_data")]] uint8_t s_img_arena_buf[k_er_img_arena];

/** @brief ra8_reflow engine for the Reading body (page / glyph / token pools). */
ra8_reflow_t s_reflow_engine;

/** @brief True while ::s_reflow_engine holds a laid-out chapter (cache valid). */
bool s_reflow_open = false;
/** @brief Chapter index currently laid out in ::s_reflow_engine (cache key). */
uint32_t s_reflow_chapter = (uint32_t)k_er_chapter_none;
/** @brief Body geometry the cached layout was paginated for (cache key). */
int32_t s_reflow_w = 0;
int32_t s_reflow_h = 0; /**< @see s_reflow_w */

/** @brief Bytes of font read off the card (0 if none). */
uint32_t s_font_len;

/** @brief True once an SD font is loaded; gates the ra8_reflow body render. */
bool s_have_font;

/** @brief Cached PCLKA rate (Hz) for the SD SPI clock shim. */
static uint32_t s_pclka_hz;

/** @brief Nag banner overlay widget vtable (renderer lives in the input TU). */
static const ra8_widget_vtable_t k_er_nag_vt = {.render = er_nag_render};

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
  (void)ra8_board_led_on(k_ra8_board_led_red);
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
 * @post Clocks, MSTP, ra8_time and both board LEDs are initialised.
 * @post Global IRQs are enabled.
 *
 * @note Not thread-safe; single-shot helper.
 * @since 0.1.0
 */
static void app_bringup_clocks(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    app_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    app_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    app_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    app_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led_blue) != k_ra8_ok) {
    app_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led_red) != k_ra8_ok) {
    app_panic_halt();
  }
  ra8_isr_globals_enable();
}

/**
 * @brief Bring up external SDRAM and the GLCDC panel, then cache the FB.
 *
 * @details
 * Settle delay, ``ra8_sdramc_init`` (the framebuffer lives at 0x68000000),
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
  ra8_delay_ms((uint32_t)k_er_settle_ms);
  if (ra8_sdramc_init() != k_ra8_ok) {
    app_panic_halt();
  }
  if (display_init(&k_er_display_cfg, &s_display) != k_ra8_ok) {
    app_panic_halt();
  }
  if (display_get_framebuffer(s_display, &s_fb) != k_ra8_ok) {
    app_panic_halt();
  }
}

/**
 * @brief Bind ra8_gfx to the panel framebuffer.
 *
 * @details After this, all chrome drawing goes through ra8_gfx primitives.
 *
 * @pre ``app_bringup_panel`` has run; ``s_fb.pixels`` is reachable.
 * @pre The framebuffer is RGB565.
 * @post ra8_gfx draw calls operate on ``s_framebuffer``.
 * @post On failure the app panic-halts.
 *
 * @note Not thread-safe; single-shot helper.
 * @since 0.1.0
 */
static void app_bringup_gfx(void)
{
  if (ra8_gfx_init(s_fb.pixels, s_fb.width_px, s_fb.height_px, k_ra8_gfx_format_rgb565) !=
      k_ra8_ok) {
    app_panic_halt();
  }
}

/**
 * @brief Open the GT911 touch controller (best-effort, polled).
 *
 * @details
 * Brings the app-owned IIC_B peripheral up in I2C-compat mode, binds it
 * through the ra8_io facade into the driver's injected seam, then opens
 * the driver. The fuel-gauge reads in ``ereader_ui_input.c`` share this
 * bus bring-up. Boards / emulator runs without the GT911 simply return an error
 * from ``ra8_touch_open``; the UI still renders, just without touch
 * input, so this is non-fatal (no panic).
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
  const ra8_board_touch_cfg_t cfg = {
    .max_points = (uint8_t)k_er_touch_max_points,
    .irq_pin    = (uint8_t)k_ra8_touch_irq_pin_unset,
  };
  (void)ra8_board_touch_open(&cfg);
}

/* ===========================================================================
 * Optional SD-loaded font for the ra8_reflow Reading body
 * =========================================================================== */

/**
 * @brief Best-effort: load FONT.OTF off the Pmod2 SD card into s_font_buf.
 *
 * @details
 * Delegates the Pmod2 SPI bring-up, FAT mount, and font read to
 * @ref ra8_sdfont_load. Entirely non-fatal: any failure (no Pmod, no card, no
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
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &s_pclka_hz) != k_ra8_ok) {
    return;
  }
  const ra8_sdfont_cfg_t cfg = {
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
  if (ra8_sdfont_load(&cfg, s_font_buf, (uint32_t)k_er_font_cap, &got, nullptr) != k_ra8_ok) {
    return;
  }
  s_font_len  = got;
  s_have_font = true;
}

#ifdef RA8_APP_SETTINGS
/**
 * @brief Minimal Settings screen for the optional Settings app.
 *
 * @details Only compiled when ``RA8_APP_SETTINGS`` is defined -- the default
 * build excludes the Settings app entirely (it is never registered, so it is
 * absent from the launcher / nav). A real settings panel would compose its own
 * widget tree; this stub draws a paper page with an ink title band so the
 * enabled build is complete and runnable.
 *
 * @pre ::ra8_gfx is bound to ::s_framebuffer.
 * @post The framebuffer holds the Settings page; no tap targets are armed.
 * @since 0.1.0
 */
static void er_render_settings(void)
{
  (void)ra8_gfx_clear((uint32_t)k_er_paper);
  (void)ra8_gfx_rect(0, 0, (int32_t)k_er_fb_w, (int32_t)k_er_statusbar_h, (uint32_t)k_er_ink, true);
  s_target_count = 0U;
}
#endif

/* ===========================================================================
 * App framework integration (#146)
 *
 * Each screen is re-expressed as an ``ra8_app``: the screen-id stack (``s_nav``)
 * remains the source of truth for *which* screen is active, and that id is the
 * active app's id, so ::er_render_current focuses the matching app and renders
 * its tree. The render callbacks are the existing ``er_render_*`` functions, so
 * the composited chrome is byte-identical (``make ereader-golden`` stays green)
 * while the control flow is now an app composition rather than a hardcoded
 * if/else. ``RA8_APP_SETTINGS`` shows the build-time "uninstallable core" knob: a
 * Settings app is only registered when the macro is defined.
 * ===========================================================================
 */

/** @brief Library / Reading / Keyboard (+ optional Settings) app instances. */
static ra8_app_t s_app_library;
static ra8_app_t s_app_reader;
static ra8_app_t s_app_keyboard;
#ifdef RA8_APP_SETTINGS
static ra8_app_t s_app_settings;
#endif

/** @brief App registry storage (4 slots: 3 core + an optional Settings). */
static ra8_app_t*         s_app_slots[4];
static ra8_app_registry_t s_app_reg;

/** @brief App render trampoline: dispatch to the screen render by app id. */
static void er_app_render(const ra8_app_t* a)
{
  if (a->id == (uint16_t)k_er_screen_reading) {
    er_render_reading();
  } else if (a->id == (uint16_t)k_er_screen_keyboard) {
    er_render_keyboard();
#ifdef RA8_APP_SETTINGS
  } else if (a->id == (uint16_t)k_er_screen_settings) {
    er_render_settings();
#endif
  } else {
    er_render_library();
  }
}

/** @brief Vtable shared by every screen-app (render-only; nav owns transitions). */
static const ra8_app_vtable_t k_er_app_vt = {
  .render = er_app_render,
};

/** @brief Register the e-reader screen-apps into ::s_app_reg. */
static void er_apps_init(void)
{
  s_app_library  = (ra8_app_t){.vt        = &k_er_app_vt,
                               .id        = (uint16_t)k_er_screen_library,
                               .name      = "Library",
                               .removable = false};
  s_app_reader   = (ra8_app_t){.vt        = &k_er_app_vt,
                               .id        = (uint16_t)k_er_screen_reading,
                               .name      = "Reader",
                               .removable = false};
  s_app_keyboard = (ra8_app_t){.vt        = &k_er_app_vt,
                               .id        = (uint16_t)k_er_screen_keyboard,
                               .name      = "Search",
                               .removable = false};
  (void)ra8_app_registry_init(&s_app_reg,
                              s_app_slots,
                              (uint16_t)(sizeof(s_app_slots) / sizeof(s_app_slots[0])));
  (void)ra8_app_register(&s_app_reg, &s_app_library);
  (void)ra8_app_register(&s_app_reg, &s_app_reader);
  (void)ra8_app_register(&s_app_reg, &s_app_keyboard);
#ifdef RA8_APP_SETTINGS
  s_app_settings = (ra8_app_t){.vt        = &k_er_app_vt,
                               .id        = (uint16_t)k_er_screen_settings,
                               .name      = "Settings",
                               .removable = true};
  (void)ra8_app_register(&s_app_reg, &s_app_settings);
#endif
}

void er_render_current(void)
{
  uint16_t top = (uint16_t)k_er_screen_library;
  (void)ra8_ui_nav_top(&s_nav, &top);
  /* The nav top is the active screen-app's id: focus it (idempotent if already
   * focused, so no flicker) and render its widget tree. */
  (void)ra8_app_launch(&s_app_reg, top);
  (void)ra8_app_render(&s_app_reg);
  /* Low-battery nag (#145/#146): an app-framework-level overlay widget drawn
   * over whichever screen-app is active. A no-op at a healthy SOC, so the chrome
   * golden (rendered at the default battery) stays byte-identical. */
  if (s_batt_nag != k_ra8_batt_nag_none) {
    ra8_widget_t nag = {.vt = &k_er_nag_vt, .visible = true, .dirty = true};
    (void)ra8_widget_render_dirty(&nag, 1U);
  }
}

void er_render_nag_region(void)
{
  const int32_t nx = (int32_t)k_er_nag_margin;
  const int32_t ny = (int32_t)k_er_nag_top;
  const int32_t nw = (int32_t)s_fb.width_px - (2 * (int32_t)k_er_nag_margin);
  const int32_t nh = (int32_t)k_er_nag_h;
  if (ra8_gfx_set_clip(nx, ny, nw, nh) != k_ra8_ok) {
    er_render_current(); /* clip unavailable -> safe full repaint. */
    return;
  }
  uint16_t top = (uint16_t)k_er_screen_library;
  (void)ra8_ui_nav_top(&s_nav, &top);
  (void)ra8_app_launch(&s_app_reg, top);
  (void)ra8_app_render(&s_app_reg);
  if (s_batt_nag != k_ra8_batt_nag_none) {
    ra8_widget_t nag = {.vt = &k_er_nag_vt, .visible = true, .dirty = true};
    (void)ra8_widget_render_dirty(&nag, 1U);
  }
  (void)ra8_gfx_reset_clip();
}

void er_flush_event(display_turn_event_t event)
{
  display_policy_decision_t dec = {};
  if (display_policy_decide(&s_policy, event, &dec) != k_ra8_ok) {
    dec.hint = k_display_refresh_quality; /* safe fallback: a clean full update */
  }
  display_rect_t rect = {};
  if (display_policy_full_rect(s_fb.width_px, s_fb.height_px, &rect) != k_ra8_ok) {
    rect = display_full_rect(s_display);
  }
  (void)display_flush(s_display, rect, dec.hint);
  g_er_last_hint = (uint32_t)dec.hint;
  g_er_cur_page  = s_reading_page;
}

void main(void)
{
  app_bringup_clocks();
  app_bringup_panel();
  app_bringup_gfx();
  app_bringup_touch();
  (void)ra8_batt_monitor_init(&s_batt_mon); /* low-battery nag policy (#145/#146) */
  /* User switches SW1/SW2 as inputs with internal pull-ups (active-low). Best-
   * effort: a config failure just leaves page-turn on the touch path. */
  (void)ra8_board_sw_init(k_ra8_board_sw1);
  (void)ra8_board_sw_init(k_ra8_board_sw2);
  er_try_load_font(); /* Best-effort SD font for the ra8_reflow Reading body. */

  /* Default refresh cadence: fast A2 turns with a periodic GC16 clean (#78). */
  (void)display_policy_init(&s_policy, k_display_policy_fast_clean, (uint16_t)k_er_clean_every);
  (void)ra8_ui_nav_init(&s_nav, (uint16_t)k_er_screen_library);
  er_apps_init(); /* re-express the screens as ra8_app instances (#146) */
  er_render_current();
  er_flush_event(k_display_event_open); /* boot -> a clean INIT panel update */

  while (1) {
    er_poll_touch();   /* fast cadence so taps feel responsive   */
    er_poll_buttons(); /* page-turn switches                     */
    er_poll_battery(); /* self-throttled (~1 Hz) low-battery nag */
    /* Heartbeat LED toggles on a slow sub-cadence so it blinks (~1 Hz) instead
     * of strobing at the input-poll rate. */
    if ((g_er_loop_ticks % (uint32_t)k_er_led_every) == 0U) {
      (void)ra8_board_led_toggle(k_ra8_board_led_blue);
    }
    /* Free-running liveness counter: the jlink_memprobe HIL reads it twice across
     * a window and asserts the render/poll loop advanced (it never WFIs). */
    g_er_loop_ticks++;
    ra8_delay_ms((uint32_t)k_er_frame_ms);
  }
}
