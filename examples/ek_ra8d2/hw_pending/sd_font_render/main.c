/**
 * @file examples/ek_ra8d2/hw_pending/sd_font_render/main.c
 * @brief Load a TTF/OTF font off an SD card and render text with ra_reflow.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Proves the e-reader font-storage path end to end on real firmware: a font
 * file lives on an SD card (FAT volume), the genuine @ref ra_sdmmc_spi driver
 * reads it over SPI, @ref ra_fs serves it as a file, and @ref ra_reflow
 * rasterises a paragraph of XHTML into the GLCDC framebuffer through
 * @ref ra_gfx. The same pipeline is covered on host by
 * @c tests/test_ra_sdmmc_card_reflow.c; this app promotes it to a
 * board_sim-runnable binary:
 *
 *   @code
 *   tools/mkfontimg/build/mkfontimg libs/fonts/ArnoPro-Regular.otf /tmp/font.img
 *   make -C examples/ek_ra8d2/hw_pending/sd_font_render
 *   tools/board_sim/build/board_sim \
 *     examples/ek_ra8d2/hw_pending/sd_font_render/build/sd_font_render.elf \
 *     --sd /tmp/font.img --ppm /tmp/out.ppm
 *   @endcode
 *
 * Flow:
 *   1. CGC + MSTP + SysTick + LEDs.
 *   2. External SDRAM + GLCDC panel (framebuffer at 0x68000000), ra_gfx bound.
 *   3. Pmod2 SPI (J25 / SPI channel 1) routed; chip-select claimed as a GPIO
 *      output (SD SPI-mode holds CS across the whole frame).
 *   4. ra_sdmmc_spi card bring-up, then ra_fs mount of the FAT volume.
 *   5. Read @c FONT.OTF off the card into an SDRAM buffer.
 *   6. ra_reflow_init / layout_chapter / render_page -> framebuffer.
 *   7. Idle; GLCDC scans out the rendered page (board_sim snapshots it).
 *
 * On the physical board this needs a Digilent PMOD MicroSD in J25 with a
 * FAT-formatted card carrying @c FONT.OTF; in board_sim the @c --sd image
 * stands in for the card.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_display_pal.h"
#include "ra_display_pal_lcd.h"
#include "ra_err.h"
#include "ra_fs.h"
#include "ra_gfx.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_panel.h"
#include "ra_panel_timing.h"
#include "ra_port_utils.h"
#include "ra_reflow.h"
#include "ra_sdmmc_spi.h"
#include "ra_sdramc.h"
#include "ra_spi.h"
#include "ra_time.h"

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
  k_sfr_fb_align  = 64U,  /**< 64-byte AXI-burst alignment.      */
  k_sfr_settle_ms = 500U, /**< PLL / SDRAM / panel-POR settle.   */
  k_sfr_frame_ms  = 100U, /**< Idle heartbeat pacing (ms).       */
} sfr_fb_misc_t;

/** @brief Reflow + font-load tunables. */
typedef enum : uint32_t {
  k_sfr_font_cap     = 512U * 1024U, /**< Max font we will read off the card. */
  k_sfr_font_px      = 18U,          /**< Body font size in pixels.           */
  k_sfr_spi_chan     = 1U,           /**< Pmod2 / J25 = SPI channel 1.        */
  k_sfr_spi_idle     = 0xFFU,        /**< Idle byte clocked on read-only xfer.*/
  k_sfr_paper_argb   = 0xFFFFFFFFU,  /**< Page background the FB is cleared to.*/
  k_sfr_ink_argb     = 0xFF101010U,  /**< Body text colour (near-black ink).  */
  k_sfr_link_argb    = 0xFF2A52BEU,  /**< Anchor text colour (cerulean).      */
  k_sfr_paper_rgb565 = 0xFFFFU,      /**< White paper in RGB565 (all bits set).*/
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
  k_sfr_stage_boot        = 0U,    /**< Pre-init.                       */
  k_sfr_stage_panel_ok    = 1U,    /**< SDRAM + GLCDC + ra_gfx up.      */
  k_sfr_stage_card_ok     = 2U,    /**< ra_sdmmc_spi init succeeded.    */
  k_sfr_stage_mount_ok    = 3U,    /**< ra_fs mounted the FAT volume.   */
  k_sfr_stage_font_ok     = 4U,    /**< Font read off the card.         */
  k_sfr_stage_layout_ok   = 5U,    /**< ra_reflow laid out the chapter. */
  k_sfr_stage_render_ok   = 6U,    /**< Page rasterised to the FB.      */
  k_sfr_stage_card_fail   = 0x80U, /**< SD bring-up failed.          */
  k_sfr_stage_mount_fail  = 0x81U, /**< FAT mount failed.            */
  k_sfr_stage_font_fail   = 0x82U, /**< Font open/read failed.       */
  k_sfr_stage_reflow_fail = 0x83U, /**< Layout/render failed.       */
} sfr_stage_t;

/** @brief Pmod2 SPI pins (J25) -- RSPI bus B per UM Table 19 p 27. */
static const ra_port_pin_t k_sfr_pin_sck  = (ra_port_pin_t)k_ra_board_pmod2_spi_sck;
static const ra_port_pin_t k_sfr_pin_cipo = (ra_port_pin_t)k_ra_board_pmod2_spi_cipo;
static const ra_port_pin_t k_sfr_pin_copi = (ra_port_pin_t)k_ra_board_pmod2_spi_copi;
static const ra_port_pin_t k_sfr_pin_cs   = (ra_port_pin_t)k_ra_board_pmod2_spi_cs;

/** @brief XHTML body the app reflows with the SD-loaded font.
 *
 * @details Deliberately short: every glyph is rasterised by stb_truetype in
 * software, which is fast on the panel but slow under the board_sim CPU
 * emulator, so a compact line keeps the sim render time practical while still
 * exercising the full SD-font -> ra_fs -> ra_reflow -> framebuffer path. */
static const char k_sfr_body[] = "<html><body><h1>SD font OK</h1>"
                                 "<p>Read off the card.</p></body></html>";

/* ===========================================================================
 * Static storage
 * =========================================================================== */

/** @brief RGB565 framebuffer in external SDRAM, AXI-burst aligned. */
static uint16_t s_framebuffer[(size_t)k_sfr_fb_h * (size_t)k_sfr_fb_w]
  __attribute__((section(".sdram_data"), aligned(k_sfr_fb_align)));

/** @brief Font blob read off the card -- lives in SDRAM (hundreds of KiB). */
static uint8_t s_font_buf[k_sfr_font_cap] __attribute__((section(".sdram_data")));

/** @brief Reflow engine (page / glyph / token pools live inside). */
static ra_reflow_t s_engine;

/** @brief Display PAL config -- LCD/GLCDC backend over the SDRAM buffer. */
static const display_cfg_t k_sfr_display_cfg = {
  .iface             = &k_display_backend_lcd_ra_glcdc,
  .framebuffer       = s_framebuffer,
  .framebuffer_bytes = sizeof(s_framebuffer),
  .width_px          = (uint16_t)k_sfr_fb_w,
  .height_px         = (uint16_t)k_sfr_fb_h,
  .pixfmt            = k_display_pixfmt_rgb565,
  .panel_timing      = &k_ra_panel_ek_ra8d2_timing,
};

/** @brief PAL handle returned by display_init. */
static display_handle_t* s_display = nullptr;

/** @brief Mutable copy of the FB descriptor; populated at boot. */
static display_fb_t s_fb;

/** @brief Cached PCLKA rate (Hz) for the SPI clock shim. */
static uint32_t s_pclka_hz = 0U;

/* ---- J-Link / board_sim readable diagnostics ----------------------------- */

/** @brief Last stage reached (::sfr_stage_t); read externally over SWD. */
volatile uint32_t g_sfr_stage = k_sfr_stage_boot;
/** @brief Bytes of font read off the card. */
volatile uint32_t g_sfr_font_len = 0U;
/** @brief Pages produced by ra_reflow_layout_chapter. */
volatile uint32_t g_sfr_pages = 0U;
/** @brief Inked (non-paper) pixel count after render -- liveness proof. */
volatile uint32_t g_sfr_ink = 0U;

/* ===========================================================================
 * SPI -> ra_sdmmc_spi transport adapter
 * =========================================================================== */

/* cppcheck-suppress constParameterCallback
 * Reason: bound to ra_sdmmc_spi_transport_t::set_clock, whose signature is
 * `ra_err_t (*)(void*, uint32_t)`; const-qualifying ctx would break the
 * function-pointer type shared by every transport binding. */
static ra_err_t sfr_spi_set_clock(void* ctx, uint32_t hz)
{
  const uint32_t pclka_hz = *(const uint32_t*)ctx;
  return ra_spi_set_clock((uint8_t)k_sfr_spi_chan, hz, pclka_hz);
}

static ra_err_t sfr_spi_cs(void* ctx, bool asserted)
{
  (void)ctx;
  return ra_gpio_write(k_sfr_pin_cs, asserted ? k_ra_level_low : k_ra_level_high);
}

static ra_err_t sfr_spi_xfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
{
  (void)ctx;
  if ((tx != nullptr) && (rx != nullptr)) {
    return ra_spi_write_read((uint8_t)k_sfr_spi_chan, tx, rx, len, k_ra_spi_width_8);
  }
  for (uint32_t i = 0U; i < len; i++) {
    const uint8_t  tx_byte = (tx != nullptr) ? tx[i] : (uint8_t)k_sfr_spi_idle;
    uint8_t        rx_byte = 0U;
    const ra_err_t err     = ra_spi_xfer8((uint8_t)k_sfr_spi_chan, tx_byte, &rx_byte);
    if (err != k_ra_ok) {
      return err;
    }
    if (rx != nullptr) {
      rx[i] = rx_byte;
    }
  }
  return k_ra_ok;
}

/* ===========================================================================
 * Boot helpers
 * =========================================================================== */

/** @brief Halt forever with the red board LED on (fatal init error marker).
 *
 * @details Also floods the framebuffer a stage-coded gray so a board_sim
 * @c --ppm snapshot (or a glance at the panel) reveals which bring-up stage
 * failed without a debugger: low nibble of the stage code picks the level. */
static void sfr_panic_halt(uint32_t stage)
{
  g_sfr_stage = stage;
  (void)ra_board_led_on(k_ra_board_led_red);
  const uint8_t lvl =
    (uint8_t)((uint32_t)k_sfr_diag_lvl_base +
              ((stage & (uint32_t)k_sfr_diag_lvl_mask) * (uint32_t)k_sfr_diag_lvl_step));
  const uint32_t argb = (uint32_t)k_sfr_diag_argb_opaque |
                        ((uint32_t)lvl << (uint32_t)k_sfr_diag_shift_r) |
                        ((uint32_t)lvl << (uint32_t)k_sfr_diag_shift_g) | (uint32_t)lvl;
  (void)ra_gfx_clear(argb);
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Clocks, MSTP, SysTick, LEDs, then enable IRQs. */
static void sfr_bringup_clocks(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    sfr_panic_halt(k_sfr_stage_boot);
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    sfr_panic_halt(k_sfr_stage_boot);
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &s_pclka_hz) != k_ra_ok) {
    sfr_panic_halt(k_sfr_stage_boot);
  }
  if (ra_mstp_init() != k_ra_ok) {
    sfr_panic_halt(k_sfr_stage_boot);
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    sfr_panic_halt(k_sfr_stage_boot);
  }
  if (ra_board_led_init(k_ra_board_led_blue) != k_ra_ok) {
    sfr_panic_halt(k_sfr_stage_boot);
  }
  if (ra_board_led_init(k_ra_board_led_red) != k_ra_ok) {
    sfr_panic_halt(k_sfr_stage_boot);
  }
  ra_isr_globals_enable();
}

/** @brief External SDRAM + GLCDC panel + ra_gfx binding. */
static void sfr_bringup_panel(void)
{
  ra_delay_ms((uint32_t)k_sfr_settle_ms);
  if (ra_sdramc_init() != k_ra_ok) {
    sfr_panic_halt(k_sfr_stage_boot);
  }
  if (display_init(&k_sfr_display_cfg, &s_display) != k_ra_ok) {
    sfr_panic_halt(k_sfr_stage_boot);
  }
  if (display_get_framebuffer(s_display, &s_fb) != k_ra_ok) {
    sfr_panic_halt(k_sfr_stage_boot);
  }
  if (ra_gfx_init(s_fb.pixels, s_fb.width_px, s_fb.height_px, k_ra_gfx_format_rgb565) != k_ra_ok) {
    sfr_panic_halt(k_sfr_stage_boot);
  }
  (void)ra_gfx_clear((uint32_t)k_sfr_paper_argb);
  g_sfr_stage = k_sfr_stage_panel_ok;
}

/** @brief Route Pmod2 SPI pins to RSPI bus B and init SPI channel 1. */
static void sfr_bringup_spi(void)
{
  if (ra_pfs_route_peripheral(k_sfr_pin_sck, k_ra_psel_spi, "sfr.sck") != k_ra_ok) {
    sfr_panic_halt(k_sfr_stage_card_fail);
  }
  if (ra_pfs_route_peripheral(k_sfr_pin_cipo, k_ra_psel_spi, "sfr.cipo") != k_ra_ok) {
    sfr_panic_halt(k_sfr_stage_card_fail);
  }
  if (ra_pfs_route_peripheral(k_sfr_pin_copi, k_ra_psel_spi, "sfr.copi") != k_ra_ok) {
    sfr_panic_halt(k_sfr_stage_card_fail);
  }
  /* CS as a GPIO output, idle high (deasserted): SD SPI-mode framing needs
   * CS held across the whole 6-byte command + data trailer, which the RSPI
   * hardware CS controller cannot do (it pulses per word). */
  if (ra_gpio_output_init(k_sfr_pin_cs, k_ra_level_high) != k_ra_ok) {
    sfr_panic_halt(k_sfr_stage_card_fail);
  }
  const ra_spi_cfg_t spi_cfg = {
    .baud_hz   = (uint32_t)k_ra_sdmmc_spi_clock_init_hz,
    .pclka_hz  = s_pclka_hz,
    .mode      = k_ra_spi_mode_0,
    .lsb_first = false,
  };
  if (ra_spi_init((uint8_t)k_sfr_spi_chan, &spi_cfg) != k_ra_ok) {
    sfr_panic_halt(k_sfr_stage_card_fail);
  }
}

/** @brief Bring up the SD card, mount FAT, read FONT.OTF into ::s_font_buf. */
static ra_fs_mount_t* sfr_load_font_or_halt(void)
{
  const ra_sdmmc_spi_transport_t transport = {
    .set_clock = sfr_spi_set_clock,
    .cs        = sfr_spi_cs,
    .xfer      = sfr_spi_xfer,
    .ctx       = &s_pclka_hz,
  };
  if (ra_sdmmc_spi_init(&transport) != k_ra_ok) {
    sfr_panic_halt(k_sfr_stage_card_fail);
  }
  g_sfr_stage = k_sfr_stage_card_ok;

  ra_fs_backend_t backend = {};
  if (ra_sdmmc_spi_bind_fs_backend(&backend) != k_ra_ok) {
    sfr_panic_halt(k_sfr_stage_mount_fail);
  }
  ra_fs_mount_t* mount = nullptr;
  if (ra_fs_mount(&backend, &mount) != k_ra_ok) {
    sfr_panic_halt(k_sfr_stage_mount_fail);
  }
  g_sfr_stage = k_sfr_stage_mount_ok;

  ra_fs_file_t* f = nullptr;
  if (ra_fs_open(mount, "FONT.OTF", k_ra_fs_mode_read, &f) != k_ra_ok) {
    sfr_panic_halt(k_sfr_stage_font_fail);
  }
  uint32_t got = 0U;
  if (ra_fs_read(f, s_font_buf, (uint32_t)k_sfr_font_cap, &got) != k_ra_ok) {
    (void)ra_fs_close(f);
    sfr_panic_halt(k_sfr_stage_font_fail);
  }
  (void)ra_fs_close(f);
  if (got < 16U) {
    sfr_panic_halt(k_sfr_stage_font_fail);
  }
  g_sfr_font_len = got;
  g_sfr_stage    = k_sfr_stage_font_ok;
  return mount;
}

/** @brief Reflow the body XHTML with the SD font into the framebuffer. */
static void sfr_render_or_halt(void)
{
  if (ra_reflow_init((uint16_t)k_sfr_fb_w,
                     (uint16_t)k_sfr_fb_h,
                     s_font_buf,
                     g_sfr_font_len,
                     (uint16_t)k_sfr_font_px,
                     (uint32_t)k_sfr_ink_argb,
                     (uint32_t)k_sfr_link_argb,
                     &s_engine) != k_ra_ok) {
    sfr_panic_halt(k_sfr_stage_reflow_fail);
  }
  uint32_t pages = 0U;
  if (ra_reflow_layout_chapter(&s_engine,
                               (const uint8_t*)k_sfr_body,
                               (uint32_t)(sizeof(k_sfr_body) - 1U),
                               &pages) != k_ra_ok) {
    sfr_panic_halt(k_sfr_stage_reflow_fail);
  }
  g_sfr_pages = pages;
  g_sfr_stage = k_sfr_stage_layout_ok;
  if (ra_reflow_render_page(&s_engine, 0U, nullptr) != k_ra_ok) {
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
  (void)ra_board_led_on(k_ra_board_led_blue);
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
  sfr_bringup_spi();
  (void)sfr_load_font_or_halt();
  sfr_render_or_halt();

  /* The page is rendered; GLCDC scans out the framebuffer continuously.
   * board_sim snapshots it via --ppm. Idle with a heartbeat. */
  while (1) {
    ra_delay_ms((uint32_t)k_sfr_frame_ms);
  }
}
#pragma GCC diagnostic pop
