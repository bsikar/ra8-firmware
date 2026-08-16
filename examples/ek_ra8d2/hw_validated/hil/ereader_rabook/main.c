/**
 * @file examples/ek_ra8d2/hw_validated/hil/ereader_rabook/main.c
 * @brief Headless HIL gate: load a compiled `.rabook` and render it (small to large).
 *
 * @details
 * The on-silicon proof of the compiled-book path. A small two-chapter book,
 * compiled by tools/epub_compile into the `ra8_book` format and baked (inflated)
 * into rabook_fixture.h, is loaded the way the device loads any book:
 *
 *   1. `ra8_book_validate()` -- accept the flat blob (magic, bounds, CRC). A real
 *      SD-loaded book is `ra8_book_open()`-ed instead (mz_uncompress + validate);
 *      see README.md. The baked fixture is already inflated so the gate needs no
 *      decompressor.
 *   2. For each chapter, `ra8_book_chapter_to_xhtml()` walks the pre-parsed DOM
 *      and bridges it to the existing renderer.
 *   3. `ra8_reflow_layout_chapter()` paginates, then every page is rendered into
 *      a 128x160 RGB565 framebuffer and folded into an FNV-1a-32.
 *   4. A second, one-image `.rabook` (rabook_gray8_fixture.h) carries a raster at
 *      full source resolution in continuous-tone gray8 -- the representation the
 *      compiler retains for zoomable content (#476), never a panel-quantised 4bpp
 *      copy. The gate confirms it is 8bpp and holds more than the 16 tones a gray4
 *      store could reproduce, then blits it 1:1 (no downscale, no re-quantise).
 *
 * The book's first chapter is short (a title + two paragraphs) and the second
 * is longer (paginates further) -- a small-to-large render in one book. The
 * fixed-metric Ahem face makes pagination + render deterministic, so the banner
 * on the SCI8 J-Link OB console is identical every boot and matches ra8_emulator:
 *
 *   `ereader-rabook-hil: chapters=<N> ch0 p=<P> crc=<8hex> ch1 p=<P> crc=<8hex> img <W>x<H> gray8 ok`
 *
 *
 * [Ring 7 / App] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "font_fixture.h"
#include "ra8_boot_entry.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_book.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_reflow.h"
#include "ra8_time.h"
#include "rabook_fixture.h"
#include "rabook_gray8_fixture.h"

/** @enum erb_consts_t @brief Framebuffer / console / hash / buffer knobs. */
typedef enum : uint32_t {
  k_erb_fb_w        = 128U,        /**< Framebuffer width, pixels.        */
  k_erb_fb_h        = 160U,        /**< Framebuffer height, pixels.       */
  k_erb_font_px     = 16U,         /**< Body font size, pixels.           */
  k_erb_ink         = 0xFF101010U, /**< Body ink colour (ARGB).           */
  k_erb_link_col    = 0xFF2A52BEU, /**< Anchor colour (ARGB).             */
  k_erb_bg          = 0xF4F0E8U,   /**< Page background (0x00RRGGBB).     */
  k_erb_xhtml_cap   = 2048U,       /**< Per-chapter serialized XHTML cap. */
  k_erb_uart_baud   = 115200U,     /**< Console baud.                     */
  k_erb_fnv_offset  = 2166136261U, /**< FNV-1a-32 offset basis.           */
  k_erb_fnv_prime   = 16777619U,   /**< FNV-1a-32 prime.                  */
  k_erb_hex_nibbles = 8U,          /**< Hex digits in a 32-bit value.     */
  k_erb_nibble_bits = 4U,          /**< Bits per hex nibble.              */
  k_erb_nibble_mask = 0x0FU,       /**< Low-nibble mask.                  */
  k_erb_dec_ten     = 10U,         /**< Hex digit / decimal split.        */
} erb_consts_t;

/** @brief RGB565 framebuffer in internal SRAM (no panel attached). */
static uint16_t s_framebuffer[(size_t)k_erb_fb_h * (size_t)k_erb_fb_w];

/** @brief Reflow engine (large -- file-scope, not on the stack). */
static ra8_reflow_t s_engine;

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
static const uint8_t k_msg_img[]  = " img ";
static const uint8_t k_msg_x[]    = "x";
static const uint8_t k_msg_g8[]   = " gray8";
static const uint8_t k_msg_ok[]   = " ok\r\n";

/** @enum erb_img_const_t @brief Full-resolution gray8 figure check bounds (#476). */
typedef enum : uint32_t {
  k_erb_gray4_levels = 16U,       /**< Distinct tones a 4bpp store can ever show. */
  k_erb_level_count  = 256U,      /**< gray8 value space (presence bitmap size).  */
  k_erb_img_max_px   = 96U * 48U, /**< Fixture image pixel cap (bounds the scan). */
} erb_img_const_t;

/** @brief Emit a byte run on the SCI8 console. */
static void erb_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Print the fail banner and trap (ra8_emulator halts on the BKPT). */
static void erb_panic_halt(const uint8_t* msg, uint32_t len)
{
  erb_print(msg, len);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
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
 * @brief Fold the whole RGB565 framebuffer into a running FNV-1a-32.
 * @param[in] seed Running hash to extend (start from ::k_erb_fnv_offset).
 * @return The framebuffer-folded hash.
 */
static uint32_t erb_hash_fb(uint32_t seed)
{
  const size_t   nbytes = (size_t)k_erb_fb_w * (size_t)k_erb_fb_h * sizeof(uint16_t);
  const uint8_t* fb     = (const uint8_t*)s_framebuffer;
  uint32_t       hsh    = seed;
  for (size_t i = 0U; i < nbytes; i++) {
    hsh = (hsh ^ (uint32_t)fb[i]) * (uint32_t)k_erb_fnv_prime;
  }
  return hsh;
}

/**
 * @brief Render every laid-out page of the current chapter; fold an FNV over it.
 * @param[out] out_hash Receives the FNV-1a-32 over every page's framebuffer.
 * @return The page count.
 */
static uint32_t erb_render_all(uint32_t* out_hash)
{
  uint32_t pages = 0U;
  (void)ra8_reflow_get_page_count(&s_engine, &pages);
  uint32_t hsh = (uint32_t)k_erb_fnv_offset;
  for (uint32_t p = 0U; p < pages; p++) {
    (void)ra8_gfx_clear((uint32_t)k_erb_bg);
    (void)ra8_reflow_render_page(&s_engine, p, s_framebuffer);
    hsh = erb_hash_fb(hsh);
  }
  *out_hash = hsh;
  return pages;
}

/** @brief Bring up clocks/MSTP/time + the SCI8 console; halt on failure. */
static void erb_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    erb_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    erb_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    erb_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_erb_uart_baud) != k_ra8_ok) {
    erb_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
}

/** @brief Lay out + render one chapter; emit `chN p=<pages> crc=<hash>`. */
static void erb_render_chapter(const void* book, uint32_t chapter_idx)
{
  size_t xlen = 0U;
  if (ra8_book_chapter_to_xhtml(book, chapter_idx, s_xhtml, (size_t)k_erb_xhtml_cap, &xlen) !=
      k_ra8_ok) {
    erb_panic_halt(k_msg_berr, (uint32_t)sizeof(k_msg_berr) - 1U);
  }
  uint32_t pages = 0U;
  if (ra8_reflow_layout_chapter(&s_engine, (const uint8_t*)s_xhtml, (uint32_t)xlen, &pages) !=
      k_ra8_ok) {
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

/**
 * @brief Count the distinct gray8 tone levels in @p px[0..n) (bounded scan).
 * @param[in] px Gray8 pixel bytes (non-NULL).
 * @param[in] n  Pixel count (`<= k_erb_img_max_px`, the scan bound).
 * @return The number of distinct byte values seen (`0..256`).
 */
static uint32_t erb_distinct_levels(const uint8_t* px, uint32_t n)
{
  bool           seen[k_erb_level_count] = {};
  uint32_t       distinct                = 0U;
  const uint32_t lim = (n < (uint32_t)k_erb_img_max_px) ? n : (uint32_t)k_erb_img_max_px;
  for (uint32_t i = 0U; i < lim; i++) {
    if (!seen[px[i]]) {
      seen[px[i]] = true;
      distinct++;
    }
  }
  return distinct;
}

/**
 * @brief Render the retained full-resolution gray8 figure and check it (#476).
 *
 * @details The compiled-book path retains zoomable rasters at full source
 *          resolution in continuous-tone gray8, never a panel-quantised 4bpp copy.
 *          This is the on-silicon proof: a one-image `.rabook`
 *          (rabook_gray8_fixture.h) is validated, and its image is confirmed to be
 *          an 8bpp raster (`raw_size == width * height`) carrying MORE than the 16
 *          distinct tones a gray4 store could ever hold -- so a gray4 import would
 *          have destroyed it. The pixels are then blitted 1:1 (no downscale, no
 *          re-quantise) to prove they render at full resolution. Any failure halts
 *          on the FAIL banner; success emits the deterministic ` img <W>x<H> gray8`.
 *
 * @pre ra8_gfx_init() has been called (the framebuffer is bound).
 * @post The `img` banner field is emitted, or the gate halts on a bad fixture.
 */
static void erb_render_image(void)
{
  const void* img_book = (const void*)k_rabook_gray8_fixture;
  if (ra8_book_validate(img_book, (size_t)k_rabook_gray8_fixture_len) != k_ra8_ok) {
    erb_panic_halt(k_msg_berr, (uint32_t)sizeof(k_msg_berr) - 1U);
  }
  const ra8_book_header_t* hdr = ra8_book_header(img_book);
  if (hdr->image_count == 0U) {
    erb_panic_halt(k_msg_berr, (uint32_t)sizeof(k_msg_berr) - 1U);
  }
  const ra8_book_image_t* im  = &ra8_book_images(img_book)[0];
  const uint32_t          npx = (uint32_t)im->width * (uint32_t)im->height;
  /* Must be a gray8 raster (8bpp: one byte per pixel), within the scan bound. */
  if ((im->format != (uint8_t)k_ra8_book_image_gray4) ||
      (ra8_book_image_pixfmt(im) != k_ra8_book_pixfmt_gray8) || (im->raw_size != npx) ||
      (npx == 0U) || (npx > (uint32_t)k_erb_img_max_px)) {
    erb_panic_halt(k_msg_berr, (uint32_t)sizeof(k_msg_berr) - 1U);
  }
  /* Continuous tone survived: more than 16 distinct levels is impossible for a
   * 4bpp store, so this proves the source was kept at full depth, not quantised. */
  const uint8_t* px = ra8_book_image_data(img_book, im);
  if (erb_distinct_levels(px, npx) <= (uint32_t)k_erb_gray4_levels) {
    erb_panic_halt(k_msg_berr, (uint32_t)sizeof(k_msg_berr) - 1U);
  }

  /* Render the retained pixels straight (1 byte/pixel, no unpack) at full res. */
  (void)ra8_gfx_clear((uint32_t)k_erb_bg);
  (void)ra8_gfx_blit_gray8(px, (int32_t)im->width, (int32_t)im->height, 0, 0);

  erb_print(k_msg_img, (uint32_t)sizeof(k_msg_img) - 1U);
  erb_print_uint((uint32_t)im->width);
  erb_print(k_msg_x, (uint32_t)sizeof(k_msg_x) - 1U);
  erb_print_uint((uint32_t)im->height);
  erb_print(k_msg_g8, (uint32_t)sizeof(k_msg_g8) - 1U);
}

/**
 * @brief App entry: validate the baked book, render each chapter, print.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The per-chapter page-count + render-hash banner is emitted; loops in WFI.
 * @since 0.1.0
 */
void main(void)
{
  erb_setup_or_halt();
  ra8_isr_globals_enable();
  erb_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  const void* book = (const void*)k_rabook_fixture;
  if (ra8_book_validate(book, (size_t)k_rabook_fixture_len) != k_ra8_ok) {
    erb_panic_halt(k_msg_berr, (uint32_t)sizeof(k_msg_berr) - 1U);
  }
  const uint32_t chapters = ra8_book_header(book)->chapter_count;

  if (ra8_gfx_init(s_framebuffer,
                   (uint16_t)k_erb_fb_w,
                   (uint16_t)k_erb_fb_h,
                   k_ra8_gfx_format_rgb565) != k_ra8_ok) {
    erb_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_reflow_init((uint16_t)k_erb_fb_w,
                      (uint16_t)k_erb_fb_h,
                      k_ahem_ttf,
                      (size_t)k_ahem_ttf_len,
                      (uint16_t)k_erb_font_px,
                      (uint32_t)k_erb_ink,
                      (uint32_t)k_erb_link_col,
                      &s_engine) != k_ra8_ok) {
    erb_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }

  erb_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  erb_print_uint(chapters);
  for (uint32_t ci = 0U; ci < chapters; ci++) {
    erb_render_chapter(book, ci);
  }
  erb_render_image(); /* #476: render the retained full-resolution gray8 figure */
  erb_print(k_msg_ok, (uint32_t)sizeof(k_msg_ok) - 1U);

  while (1) {
    __asm__ volatile("wfi");
  }
}
