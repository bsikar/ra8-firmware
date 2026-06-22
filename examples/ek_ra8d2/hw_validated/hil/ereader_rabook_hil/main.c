/**
 * @file examples/ek_ra8d2/hw_validated/hil/ereader_rabook_hil/main.c
 * @brief Headless HIL gate: load a compiled `.rabook` and render it (small to large).
 *
 * @details
 * The on-silicon proof of the compiled-book path. A small two-chapter book,
 * compiled by tools/epub_compile into the `ra_book` format and baked (inflated)
 * into rabook_fixture.h, is loaded the way the device loads any book:
 *
 *   1. `ra_book_validate()` -- accept the flat blob (magic, bounds, CRC). A real
 *      SD-loaded book is `ra_book_open()`-ed instead (mz_uncompress + validate);
 *      see README.md. The baked fixture is already inflated so the gate needs no
 *      decompressor.
 *   2. For each chapter, `ra_book_chapter_to_xhtml()` walks the pre-parsed DOM
 *      and bridges it to the existing renderer.
 *   3. `ra_reflow_layout_chapter()` paginates, then every page is rendered into
 *      a 128x160 RGB565 framebuffer and folded into an FNV-1a-32.
 *
 * The book's first chapter is short (a title + two paragraphs) and the second
 * is longer (paginates further) -- a small-to-large render in one book. The
 * fixed-metric Ahem face makes pagination + render deterministic, so the banner
 * on the SCI8 J-Link OB console is identical every boot and matches board_sim:
 *
 *   `ereader-rabook-hil: chapters=<N> ch0 p=<P> crc=<8hex> ch1 p=<P> crc=<8hex> ok`
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 7 / App] {World: NS}
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "font_fixture.h"
#include "ra_book.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gfx.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_reflow.h"
#include "ra_sci.h"
#include "ra_time.h"
#include "rabook_fixture.h"

/** @enum erb_consts_t @brief Framebuffer / console / hash / buffer knobs. */
typedef enum : uint32_t {
  k_erb_fb_w        = 128U,        /**< Framebuffer width, pixels.        */
  k_erb_fb_h        = 160U,        /**< Framebuffer height, pixels.       */
  k_erb_font_px     = 16U,         /**< Body font size, pixels.           */
  k_erb_ink         = 0xFF101010U, /**< Body ink colour (ARGB).           */
  k_erb_link_col    = 0xFF2A52BEU, /**< Anchor colour (ARGB).             */
  k_erb_bg          = 0xF4F0E8U,   /**< Page background (0x00RRGGBB).      */
  k_erb_xhtml_cap   = 2048U,       /**< Per-chapter serialized XHTML cap.  */
  k_erb_uart_chan   = 8U,          /**< SCI8 J-Link OB console.           */
  k_erb_uart_baud   = 115200U,     /**< Console baud.                     */
  k_erb_fnv_offset  = 2166136261U, /**< FNV-1a-32 offset basis.           */
  k_erb_fnv_prime   = 16777619U,   /**< FNV-1a-32 prime.                  */
  k_erb_hex_nibbles = 8U,          /**< Hex digits in a 32-bit value.     */
  k_erb_nibble_bits = 4U,          /**< Bits per hex nibble.              */
  k_erb_nibble_mask = 0x0FU,       /**< Low-nibble mask.                  */
  k_erb_dec_ten     = 10U,         /**< Hex digit / decimal split.        */
} erb_consts_t;

/** @brief SCI8 console TXD = PD02. */
static const ra_port_pin_t k_erb_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
/** @brief SCI8 console RXD = PD03. */
static const ra_port_pin_t k_erb_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief RGB565 framebuffer in internal SRAM (no panel attached). */
static uint16_t s_framebuffer[(size_t)k_erb_fb_h * (size_t)k_erb_fb_w];

/** @brief Reflow engine (large -- file-scope, not on the stack). */
static ra_reflow_t s_engine;

/** @brief Scratch for one chapter's serialized XHTML (DOM -> renderer bridge). */
static char s_xhtml[k_erb_xhtml_cap];

static const uint8_t k_msg_boot[] = "ereader-rabook-hil: boot\r\n";
static const uint8_t k_msg_fail[] = "ereader-rabook-hil: FAIL init\r\n";
static const uint8_t k_msg_berr[] = "ereader-rabook-hil: FAIL book\r\n";
static const uint8_t k_msg_lerr[] = "ereader-rabook-hil: FAIL layout\r\n";
static const uint8_t k_msg_pre[]  = "ereader-rabook-hil: chapters=";
static const uint8_t k_msg_ch[]   = " ch";
static const uint8_t k_msg_p[]    = " p=";
static const uint8_t k_msg_crc[]  = " crc=";
static const uint8_t k_msg_ok[]   = " ok\r\n";

/** @brief Emit a byte run on the SCI8 console. */
static void erb_print(const uint8_t* msg, uint32_t len)
{
  (void)ra_sci_write_polling((uint8_t)k_erb_uart_chan, msg, len);
}

/** @brief Print the fail banner and trap (board_sim halts on the BKPT). */
static void erb_panic_halt(const uint8_t* msg, uint32_t len)
{
  erb_print(msg, len);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Route the SCI8 console pins to async mode. */
[[nodiscard]] static ra_err_t erb_console_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_erb_pin_txd, k_ra_psel_sci_async, "erb.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_erb_pin_rxd, k_ra_psel_sci_async, "erb.rxd8");
}

/** @brief Print a 32-bit value as 8 upper-case hex digits. */
static void erb_print_hex(uint32_t value)
{
  uint8_t buf[k_erb_hex_nibbles];
  for (uint32_t i = 0U; i < (uint32_t)k_erb_hex_nibbles; i++) {
    const uint32_t shift = ((uint32_t)k_erb_hex_nibbles - 1U - i) * (uint32_t)k_erb_nibble_bits;
    const uint32_t nib   = (value >> shift) & (uint32_t)k_erb_nibble_mask;
    buf[i] =
      (uint8_t)((nib < (uint32_t)k_erb_dec_ten) ? ('0' + nib) : ('A' + (nib - k_erb_dec_ten)));
  }
  erb_print(buf, (uint32_t)k_erb_hex_nibbles);
}

/** @brief Print a small unsigned integer in decimal. */
static void erb_print_uint(uint32_t value)
{
  uint8_t  buf[k_erb_dec_ten];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_erb_dec_ten)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_erb_dec_ten));
    n++;
    value /= (uint32_t)k_erb_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    erb_print(&buf[n - 1U - i], 1U);
  }
}

/**
 * @brief Render every laid-out page of the current chapter; fold an FNV over it.
 * @param[out] out_hash Receives the FNV-1a-32 over every page's framebuffer.
 * @return The page count.
 */
static uint32_t erb_render_all(uint32_t* out_hash)
{
  uint32_t pages = 0U;
  (void)ra_reflow_get_page_count(&s_engine, &pages);
  uint32_t     hsh    = (uint32_t)k_erb_fnv_offset;
  const size_t nbytes = (size_t)k_erb_fb_w * (size_t)k_erb_fb_h * sizeof(uint16_t);
  for (uint32_t p = 0U; p < pages; p++) {
    (void)ra_gfx_clear((uint32_t)k_erb_bg);
    (void)ra_reflow_render_page(&s_engine, p, s_framebuffer);
    const uint8_t* fb = (const uint8_t*)s_framebuffer;
    for (size_t i = 0U; i < nbytes; i++) {
      hsh = (hsh ^ (uint32_t)fb[i]) * (uint32_t)k_erb_fnv_prime;
    }
  }
  *out_hash = hsh;
  return pages;
}

/** @brief Bring up clocks/MSTP/time + the SCI8 console; halt on failure. */
static void erb_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if ((ra_cgc_init() != k_ra_ok) || (ra_mstp_init() != k_ra_ok)) {
    erb_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if ((ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) ||
      (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok)) {
    erb_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    erb_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (erb_console_pins_init() != k_ra_ok) {
    erb_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  const ra_sci_cfg_t cfg = {.baud      = (uint32_t)k_erb_uart_baud,
                            .data_bits = k_ra_sci_data_8,
                            .parity    = k_ra_sci_parity_none,
                            .stop_bits = k_ra_sci_stop_1,
                            .pclk_hz   = pclka_hz};
  if (ra_sci_init((uint8_t)k_erb_uart_chan, &cfg) != k_ra_ok) {
    erb_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
}

/** @brief Lay out + render one chapter; emit `chN p=<pages> crc=<hash>`. */
static void erb_render_chapter(const void* book, uint32_t chapter_idx)
{
  size_t xlen = 0U;
  if (ra_book_chapter_to_xhtml(book, chapter_idx, s_xhtml, (size_t)k_erb_xhtml_cap, &xlen) !=
      k_ra_ok) {
    erb_panic_halt(k_msg_berr, (uint32_t)sizeof(k_msg_berr) - 1U);
  }
  uint32_t pages = 0U;
  if (ra_reflow_layout_chapter(&s_engine, (const uint8_t*)s_xhtml, (uint32_t)xlen, &pages) !=
      k_ra_ok) {
    erb_panic_halt(k_msg_lerr, (uint32_t)sizeof(k_msg_lerr) - 1U);
  }
  uint32_t       hash     = 0U;
  const uint32_t rendered = erb_render_all(&hash);

  erb_print(k_msg_ch, (uint32_t)sizeof(k_msg_ch) - 1U);
  erb_print_uint(chapter_idx);
  erb_print(k_msg_p, (uint32_t)sizeof(k_msg_p) - 1U);
  erb_print_uint(rendered);
  erb_print(k_msg_crc, (uint32_t)sizeof(k_msg_crc) - 1U);
  erb_print_hex(hash);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: validate the baked book, render each chapter, print.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The per-chapter page-count + render-hash banner is emitted; loops in WFI.
 * @since 0.1.0
 */
int32_t main(void)
{
  erb_setup_or_halt();
  ra_isr_globals_enable();
  erb_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  const void* book = (const void*)k_rabook_fixture;
  if (ra_book_validate(book, (size_t)k_rabook_fixture_len) != k_ra_ok) {
    erb_panic_halt(k_msg_berr, (uint32_t)sizeof(k_msg_berr) - 1U);
  }
  const uint32_t chapters = ra_book_header(book)->chapter_count;

  if (ra_gfx_init(s_framebuffer,
                  (uint16_t)k_erb_fb_w,
                  (uint16_t)k_erb_fb_h,
                  k_ra_gfx_format_rgb565) != k_ra_ok) {
    erb_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra_reflow_init((uint16_t)k_erb_fb_w,
                     (uint16_t)k_erb_fb_h,
                     k_ahem_ttf,
                     (size_t)k_ahem_ttf_len,
                     (uint16_t)k_erb_font_px,
                     (uint32_t)k_erb_ink,
                     (uint32_t)k_erb_link_col,
                     &s_engine) != k_ra_ok) {
    erb_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }

  erb_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  erb_print_uint(chapters);
  for (uint32_t ci = 0U; ci < chapters; ci++) {
    erb_render_chapter(book, ci);
  }
  erb_print(k_msg_ok, (uint32_t)sizeof(k_msg_ok) - 1U);

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
