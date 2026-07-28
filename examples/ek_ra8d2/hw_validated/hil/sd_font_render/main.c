/**
 * @file examples/ek_ra8d2/hw_validated/hil/sd_font_render/main.c
 * @brief Load a TTF/OTF font off an SD card and render text with ra8_reflow.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Proves the e-reader font-storage path end to end on real firmware: a font
 * file lives on an SD card (FAT volume), the genuine @ref ra8_sdmmc_spi driver
 * reads it over SPI, @ref ra8_fs serves it as a file, and @ref ra8_reflow
 * rasterises a paragraph of XHTML into the GLCDC framebuffer through
 * @ref ra8_gfx. The same pipeline is covered on host by
 * @c tests/test_ra8_sdmmc_card_reflow.c; this app promotes it to a
 * ra8_emulator-runnable binary:
 *
 *   @code
 *   tools/mkfontimg/build/mkfontimg libs/ra8_fonts/Literata-Regular.ttf /tmp/font.img
 *   make -C examples/ek_ra8d2/hw_validated/hil/sd_font_render
 *   tools/ra8_emulator/build/ra8_emulator \
 *     examples/ek_ra8d2/hw_validated/hil/sd_font_render/build/sd_font_render.elf \
 *     --sd /tmp/font.img --ppm /tmp/out.ppm
 *   @endcode
 *
 * Flow:
 *   1. CGC + MSTP + SysTick + LEDs.
 *   2. External SDRAM + GLCDC panel (framebuffer at 0x68000000), ra8_gfx bound.
 *   3. @ref ra8_sdfont_load brings up the Pmod2 SD card (J25 / SCI0 Simple-SPI),
 *      mounts the FAT volume, and reads @c FONT.OTF into an SDRAM buffer --
 *      self-provisioning the file from a baked Latin-1 font when the card is blank.
 *   4. ra8_reflow_init / layout_chapter / render_page -> framebuffer.
 *   5. Idle; GLCDC scans out the rendered page (ra8_emulator snapshots it).
 *
 * Because the font self-provisions, any FAT-formatted microSD works -- the card
 * need not be pre-loaded with @c FONT.OTF. On the physical board this needs a
 * Digilent PMOD MicroSD in J25; in ra8_emulator the @c --sd image stands in.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "literata_latin1.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_display_pal.h"
#include "ra8_display_pal_lcd.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_panel.h"
#include "ra8_panel_timing.h"
#include "ra8_reflow.h"
#include "ra8_sdfont.h"
#include "ra8_sdramc.h"
#include "ra8_time.h"

/* ===========================================================================
 * Compile-time configuration -- typed enums per the no-magic-number rule.
 * =========================================================================== */

/** @brief Framebuffer dimensions, sourced from the BSP panel descriptor. */
typedef enum : uint16_t {
  k_sfr_fb_w = k_panel_width_px,  /**< Framebuffer width  (pixels). */
  k_sfr_fb_h = k_panel_height_px, /**< Framebuffer height (pixels). */
} sfr_fb_dim_t;

/** @brief Framebuffer + pacing byte-math constants. */
typedef enum : uint16_t {
  k_sfr_fb_align  = 64U,  /**< 64-byte AXI-burst alignment.    */
  k_sfr_settle_ms = 500U, /**< PLL / SDRAM / panel-POR settle. */
  k_sfr_frame_ms  = 100U, /**< Idle heartbeat pacing (ms).     */
} sfr_fb_misc_t;

/** @brief Reflow + font-load tunables. */
typedef enum : uint32_t {
  k_sfr_font_cap     = 512U * 1024U, /**< Max font we will read off the card.   */
  k_sfr_font_px      = 18U,          /**< Body font size in pixels.             */
  k_sfr_spi_chan     = 0U,           /**< Pmod2 / J25 = SCI0 Simple-SPI.        */
  k_sfr_spi_idle     = 0xFFU,        /**< Idle byte clocked on read-only xfer.  */
  k_sfr_paper_argb   = 0xFFFFFFFFU,  /**< Page background the FB is cleared to. */
  k_sfr_ink_argb     = 0xFF101010U,  /**< Body text colour (near-black ink).    */
  k_sfr_link_argb    = 0xFF2A52BEU,  /**< Anchor text colour (cerulean).        */
  k_sfr_paper_rgb565 = 0xFFFFU,      /**< White paper in RGB565 (all bits set). */
} sfr_reflow_cfg_t;

/** @brief Constants for the stage-coded gray panic fill (diagnostic only). */
typedef enum : uint32_t {
  k_sfr_diag_argb_opaque = 0xFF000000U, /**< Opaque-alpha bits of an ARGB fill. */
  k_sfr_diag_lvl_base    = 0x10U,       /**< Darkest panic gray level.          */
  k_sfr_diag_lvl_step    = 0x20U,       /**< Gray step per stage code.          */
  k_sfr_diag_lvl_mask    = 0x0FU,       /**< Low-nibble stage selector.         */
  k_sfr_diag_shift_r     = 16U,         /**< Red byte position in ARGB.         */
  k_sfr_diag_shift_g     = 8U,          /**< Green byte position in ARGB.       */
} sfr_diag_fill_t;

/** @brief Diagnostic stage codes stamped to ::g_sfr_stage (J-Link / sim). */
typedef enum : uint32_t {
  k_sfr_stage_boot        = 0U,    /**< Pre-init.                        */
  k_sfr_stage_panel_ok    = 1U,    /**< SDRAM + GLCDC + ra8_gfx up.      */
  k_sfr_stage_card_ok     = 2U,    /**< ra8_sdmmc_spi init succeeded.    */
  k_sfr_stage_mount_ok    = 3U,    /**< ra8_fs mounted the FAT volume.   */
  k_sfr_stage_font_ok     = 4U,    /**< Font read off the card.          */
  k_sfr_stage_layout_ok   = 5U,    /**< ra8_reflow laid out the chapter. */
  k_sfr_stage_render_ok   = 6U,    /**< Page rasterised to the FB.       */
  k_sfr_stage_card_fail   = 0x80U, /**< SD bring-up failed.              */
  k_sfr_stage_mount_fail  = 0x81U, /**< FAT mount failed.                */
  k_sfr_stage_font_fail   = 0x82U, /**< Font open/read failed.           */
  k_sfr_stage_reflow_fail = 0x83U, /**< Layout/render failed.            */
} sfr_stage_t;

/** @brief Pmod2 SPI pins (J25) -- SCI0 Simple-SPI per HUM Table 20.13. */
static const ra8_port_pin_t k_sfr_pin_sck  = (ra8_port_pin_t)k_ra8_board_pmod2_spi_sck;
static const ra8_port_pin_t k_sfr_pin_cipo = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cipo;
static const ra8_port_pin_t k_sfr_pin_copi = (ra8_port_pin_t)k_ra8_board_pmod2_spi_copi;
static const ra8_port_pin_t k_sfr_pin_cs   = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cs;

/** @brief XHTML body the app reflows with the SD-loaded font.
 *
 * @details Deliberately short: every glyph is rasterised by stb_truetype in
 * software, which is fast on the panel but slow under the ra8_emulator CPU
 * emulator, so a compact line keeps the sim render time practical while still
 * exercising the full SD-font -> ra8_fs -> ra8_reflow -> framebuffer path. */
static const char k_sfr_body[] = "<html><body><h1>SD font OK</h1>"
                                 "<p>Read off the card.</p></body></html>";

/* ===========================================================================
 * Static storage
 * =========================================================================== */

/** @brief RGB565 framebuffer in external SDRAM, AXI-burst aligned. */
[[gnu::section(".sdram_data"),
  gnu::aligned(
    k_sfr_fb_align)]] static uint16_t s_framebuffer[(size_t)k_sfr_fb_h * (size_t)k_sfr_fb_w];

/** @brief Font blob read off the card -- lives in SDRAM (hundreds of KiB). */
[[gnu::section(".sdram_data")]] static uint8_t s_font_buf[k_sfr_font_cap];

/** @brief Reflow engine (page / glyph / token pools live inside). */
static ra8_reflow_t s_engine;

/** @brief Display PAL config -- LCD/GLCDC backend over the SDRAM buffer. */
static const display_cfg_t k_sfr_display_cfg = {
  .iface             = &k_display_backend_lcd_ra8_glcdc,
  .framebuffer       = s_framebuffer,
  .framebuffer_bytes = sizeof(s_framebuffer),
  .width_px          = (uint16_t)k_sfr_fb_w,
  .height_px         = (uint16_t)k_sfr_fb_h,
  .pixfmt            = k_display_pixfmt_rgb565,
  .panel_timing      = &k_ra8_panel_ek_ra8d2_timing,
};

/** @brief PAL handle returned by display_init. */
static display_handle_t* s_display = nullptr;

/** @brief Mutable copy of the FB descriptor; populated at boot. */
static display_fb_t s_fb;

/** @brief Cached PCLKA rate (Hz) for the SPI clock shim. */
static uint32_t s_pclka_hz = 0U;

/* ---- J-Link / ra8_emulator readable diagnostics ----------------------------- */

/** @brief Last stage reached (::sfr_stage_t); read externally over SWD. */
volatile uint32_t g_sfr_stage = k_sfr_stage_boot;
/** @brief Bytes of font read off the card. */
volatile uint32_t g_sfr_font_len = 0U;
/** @brief Pages produced by ra8_reflow_layout_chapter. */
volatile uint32_t g_sfr_pages = 0U;
/** @brief Inked (non-paper) pixel count after render -- liveness proof. */
volatile uint32_t g_sfr_ink = 0U;
/** @brief Font provenance (::ra8_sdfont_source_t): 0 = on-card, 1 = provisioned. */
volatile uint32_t g_sfr_source = 0U;
/** @brief Last ::ra8_err_t from ::ra8_sdfont_load (0 = k_ra8_ok), for SWD triage. */
volatile uint32_t g_sfr_err = 0U;
/** @brief Idle-loop heartbeat; advances only after a clean render (HIL gate). */
volatile uint32_t g_sfr_heartbeat = 0U;

/* ===========================================================================
 * Boot helpers
 * =========================================================================== */

/** @brief Halt forever with the red board LED on (fatal init error marker).
 *
 * @details Also floods the framebuffer a stage-coded gray so a ra8_emulator
 * @c --ppm snapshot (or a glance at the panel) reveals which bring-up stage
 * failed without a debugger: low nibble of the stage code picks the level. */
static void sfr_panic_halt(uint32_t stage)
{
  g_sfr_stage = stage;
  (void)ra8_board_led_on(k_ra8_board_led_red);
  const uint8_t lvl =
    (uint8_t)((uint32_t)k_sfr_diag_lvl_base +
              ((stage & (uint32_t)k_sfr_diag_lvl_mask) * (uint32_t)k_sfr_diag_lvl_step));
  const uint32_t argb = (uint32_t)k_sfr_diag_argb_opaque |
                        ((uint32_t)lvl << (uint32_t)k_sfr_diag_shift_r) |
                        ((uint32_t)lvl << (uint32_t)k_sfr_diag_shift_g) | (uint32_t)lvl;
  (void)ra8_gfx_clear(argb);
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Clocks, MSTP, SysTick, LEDs, then enable IRQs. */
static void sfr_bringup_clocks(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    sfr_panic_halt(k_sfr_stage_boot);
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    sfr_panic_halt(k_sfr_stage_boot);
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &s_pclka_hz) != k_ra8_ok) {
    sfr_panic_halt(k_sfr_stage_boot);
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    sfr_panic_halt(k_sfr_stage_boot);
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    sfr_panic_halt(k_sfr_stage_boot);
  }
  if (ra8_board_led_init(k_ra8_board_led_blue) != k_ra8_ok) {
    sfr_panic_halt(k_sfr_stage_boot);
  }
  if (ra8_board_led_init(k_ra8_board_led_red) != k_ra8_ok) {
    sfr_panic_halt(k_sfr_stage_boot);
  }
  ra8_isr_globals_enable();
}

/** @brief External SDRAM + GLCDC panel + ra8_gfx binding. */
static void sfr_bringup_panel(void)
{
  ra8_delay_ms((uint32_t)k_sfr_settle_ms);
  if (ra8_sdramc_init() != k_ra8_ok) {
    sfr_panic_halt(k_sfr_stage_boot);
  }
  if (display_init(&k_sfr_display_cfg, &s_display) != k_ra8_ok) {
    sfr_panic_halt(k_sfr_stage_boot);
  }
  if (display_get_framebuffer(s_display, &s_fb) != k_ra8_ok) {
    sfr_panic_halt(k_sfr_stage_boot);
  }
  if (ra8_gfx_init(s_fb.pixels, s_fb.width_px, s_fb.height_px, k_ra8_gfx_format_rgb565) !=
      k_ra8_ok) {
    sfr_panic_halt(k_sfr_stage_boot);
  }
  (void)ra8_gfx_clear((uint32_t)k_sfr_paper_argb);
  g_sfr_stage = k_sfr_stage_panel_ok;
}

/** @brief Load FONT.OTF off the Pmod2 card (self-provisioning) into ::s_font_buf.
 *
 * @details Delegates the Pmod2 SPI bring-up, FAT mount, and font read to
 * @ref ra8_sdfont_load, which writes the baked Latin-1 font to the card and reads
 * it back when @c FONT.OTF is absent -- so any FAT-formatted card just works. The
 * raw ::ra8_err_t is stashed in ::g_sfr_err for SWD triage; on failure the panel
 * is flooded a stage-coded gray and the app halts. */
static void sfr_load_font_or_halt(void)
{
  const ra8_sdfont_cfg_t cfg = {
    .spi_channel    = (uint8_t)k_sfr_spi_chan,
    .sck            = k_sfr_pin_sck,
    .cipo           = k_sfr_pin_cipo,
    .copi           = k_sfr_pin_copi,
    .cs             = k_sfr_pin_cs,
    .pclka_hz       = s_pclka_hz,
    .filename       = "FONT.OTF",
    .provision_blob = g_ra8_font_literata_latin1,
    .provision_len  = g_ra8_font_literata_latin1_len,
  };
  uint32_t            got = 0U;
  ra8_sdfont_source_t src = k_ra8_sdfont_source_card;
  const ra8_err_t     err = ra8_sdfont_load(&cfg, s_font_buf, (uint32_t)k_sfr_font_cap, &got, &src);
  g_sfr_err               = (uint32_t)err;
  if (err != k_ra8_ok) {
    sfr_panic_halt(k_sfr_stage_card_fail);
  }
  g_sfr_font_len = got;
  g_sfr_source   = (uint32_t)src;
  g_sfr_stage    = k_sfr_stage_font_ok;
}

/** @brief Reflow the body XHTML with the SD font into the framebuffer. */
static void sfr_render_or_halt(void)
{
  if (ra8_reflow_init((uint16_t)k_sfr_fb_w,
                      (uint16_t)k_sfr_fb_h,
                      s_font_buf,
                      g_sfr_font_len,
                      (uint16_t)k_sfr_font_px,
                      (uint32_t)k_sfr_ink_argb,
                      (uint32_t)k_sfr_link_argb,
                      &s_engine) != k_ra8_ok) {
    sfr_panic_halt(k_sfr_stage_reflow_fail);
  }
  uint32_t pages = 0U;
  if (ra8_reflow_layout_chapter(&s_engine,
                                (const uint8_t*)k_sfr_body,
                                (uint32_t)(sizeof(k_sfr_body) - 1U),
                                &pages) != k_ra8_ok) {
    sfr_panic_halt(k_sfr_stage_reflow_fail);
  }
  g_sfr_pages = pages;
  g_sfr_stage = k_sfr_stage_layout_ok;
  if (ra8_reflow_render_page(&s_engine, 0U, nullptr) != k_ra8_ok) {
    sfr_panic_halt(k_sfr_stage_reflow_fail);
  }

  /* Count inked pixels so a HIL probe / sim can prove text actually landed. */
  uint32_t       ink      = 0U;
  const uint16_t paper565 = (uint16_t)k_sfr_paper_rgb565;
  for (size_t i = 0U; i < (size_t)k_sfr_fb_w * (size_t)k_sfr_fb_h; i++) {
    if (s_framebuffer[i] != paper565) {
      ink++;
    }
  }
  g_sfr_ink   = ink;
  g_sfr_stage = k_sfr_stage_render_ok;
  (void)ra8_board_led_on(k_ra8_board_led_blue);
}

/* ===========================================================================
 * Main
 * =========================================================================== */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  sfr_bringup_clocks();
  sfr_bringup_panel();
  sfr_load_font_or_halt();
  sfr_render_or_halt();

  /* The page is rendered; GLCDC scans out the framebuffer continuously.
   * ra8_emulator snapshots it via --ppm. Idle with a heartbeat that advances only
   * on this success path -- the panic-halt loop does not bump it -- so a J-Link
   * memprobe gate proves the full SD-font render pipeline ran end to end. */
  while (1) {
    ra8_delay_ms((uint32_t)k_sfr_frame_ms);
    g_sfr_heartbeat++;
  }
}
#pragma GCC diagnostic pop
