/**
 * @file examples/ek_ra8d2/hw_validated/hil/ereader_svg/src/main.c
 * @brief On-silicon HIL: SVG vector-art render-at-size + CRC gate (#143).
 *
 * @details
 * The vector half of the cover-art family: render an SVG document to a
 * fixed-size framebuffer through the `reflow` SVG rasterizer (#112/#141) and
 * CRC-gate the result -- the SVG counterpart to `ereader_image` (PNG) and
 * `ereader_jpeg` (JPEG), which cover the raster `stb_image` path.
 *
 *   1. `ra8_gfx_init` -- bind a 160x120 RGB565 framebuffer in internal SRAM.
 *   2. `ra8_svg_size` -- read the SVG's intrinsic size from its `viewBox`.
 *   3. `ra8_svg_render` -- map the `viewBox` onto the framebuffer box and draw
 *      the supported shapes (`<rect>` / `<circle>` / `<polygon>` filled) through
 *      ra8_gfx. No allocation (NASA Rule 3) -- the rasterizer's only side effect
 *      is drawing.
 *   4. FNV-1a-32 hash the rendered framebuffer.
 *
 * The console banner on success is:
 *
 *   `ereader-svg-hil: svg 100x100 crc=<8hex> PASS`
 *
 * where `100x100` is the SVG's intrinsic `viewBox` size and `<8hex>` the
 * framebuffer hash. The whole chain is deterministic (a fixed document through
 * a deterministic rasterizer), so the banner is identical on host, ra8_emulator,
 * and silicon. Any failure prints a FAIL banner and halts on a BKPT before the
 * PASS line, so the gate is exact.
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

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"
#include "reflow_svg.h"
#include "svg_fixture.h"

/** @enum es_consts_t @brief Console / render knobs (no magic numbers). */
typedef enum : uint32_t {
  k_es_uart_baud   = 115200U,     /**< Console baud.                 */
  k_es_fb_w        = 160U,        /**< Framebuffer width, pixels.    */
  k_es_fb_h        = 120U,        /**< Framebuffer height, pixels.   */
  k_es_col_bg      = 0x202028U,   /**< Framebuffer clear colour.     */
  k_es_fnv_offset  = 0x811C9DC5U, /**< FNV-1a 32-bit offset basis.   */
  k_es_fnv_prime   = 0x01000193U, /**< FNV-1a 32-bit prime.          */
  k_es_hex_nibbles = 8U,          /**< Hex digits in a 32-bit value. */
  k_es_nibble_bits = 4U,          /**< Bits per hex nibble.          */
  k_es_nibble_mask = 0x0FU,       /**< Low-nibble mask.              */
  k_es_dec_ten     = 10U,         /**< Hex digit / decimal split.    */
} es_consts_t;

/** @brief RGB565 framebuffer the SVG is rendered into. */
static uint16_t s_framebuffer[(size_t)k_es_fb_h * (size_t)k_es_fb_w];

static const uint8_t k_msg_boot[] = "ereader-svg-hil: boot\r\n";
static const uint8_t k_msg_fail[] = "ereader-svg-hil: FAIL init\r\n";
static const uint8_t k_msg_size[] = "ereader-svg-hil: FAIL size\r\n";
static const uint8_t k_msg_rerr[] = "ereader-svg-hil: FAIL render\r\n";
static const uint8_t k_msg_pre[]  = "ereader-svg-hil: svg ";
static const uint8_t k_msg_x[]    = "x";
static const uint8_t k_msg_crc[]  = " crc=";
static const uint8_t k_msg_ok[]   = " PASS\r\n";

/** @brief Emit a byte run on the SCI8 console. */
static void es_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Print the fail banner and trap (ra8_emulator halts on the BKPT). */
static void es_panic_halt(const uint8_t* msg, uint32_t len)
{
  es_print(msg, len);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief FNV-1a hash over the rendered framebuffer. */
static uint32_t es_framebuffer_hash(void)
{
  const uint8_t* p   = (const uint8_t*)s_framebuffer;
  const size_t   n   = sizeof(s_framebuffer);
  uint32_t       hsh = (uint32_t)k_es_fnv_offset;
  for (size_t i = 0U; i < n; i++) {
    hsh = (hsh ^ (uint32_t)p[i]) * (uint32_t)k_es_fnv_prime;
  }
  return hsh;
}

/** @brief Print a 32-bit value as 8 upper-case hex digits. */
static void es_print_hex(uint32_t value)
{
  uint8_t buf[k_es_hex_nibbles];
  for (uint32_t i = 0U; i < (uint32_t)k_es_hex_nibbles; i++) {
    const uint32_t shift = ((uint32_t)k_es_hex_nibbles - 1U - i) * (uint32_t)k_es_nibble_bits;
    const uint32_t nib   = (value >> shift) & (uint32_t)k_es_nibble_mask;
    buf[i] = (uint8_t)((nib < (uint32_t)k_es_dec_ten) ? ('0' + nib) : ('A' + (nib - k_es_dec_ten)));
  }
  es_print(buf, (uint32_t)k_es_hex_nibbles);
}

/** @brief Print a small unsigned integer in decimal. */
static void es_print_uint(uint32_t value)
{
  uint8_t  buf[k_es_dec_ten];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_es_dec_ten)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_es_dec_ten));
    n++;
    value /= (uint32_t)k_es_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    es_print(&buf[n - 1U - i], 1U);
  }
}

/** @brief Bring up clocks/MSTP/time + the SCI8 console; halt on failure. */
static void es_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    es_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    es_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    es_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_es_uart_baud) != k_ra8_ok) {
    es_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
}

/**
 * @brief Read the SVG intrinsic size and render it into the framebuffer.
 *
 * @details Halts on any failure before returning. On success the SVG has been
 * rasterized into ::s_framebuffer and its intrinsic dimensions returned.
 *
 * @param[out] out_w Receives the SVG `viewBox` width.
 * @param[out] out_h Receives the SVG `viewBox` height.
 *
 * @pre ::es_setup_or_halt ran and ::ra8_gfx_init bound ::s_framebuffer.
 * @post On return ::s_framebuffer holds the rendered SVG; @p out_w/@p out_h set.
 * @since 0.1.0
 */
static void es_render_svg_or_halt(int32_t* out_w, int32_t* out_h)
{
  const size_t svg_len = sizeof(k_svg_fixture) - 1U;
  if (ra8_svg_size((const uint8_t*)k_svg_fixture, svg_len, out_w, out_h) != k_ra8_ok) {
    es_panic_halt(k_msg_size, (uint32_t)sizeof(k_msg_size) - 1U);
  }
  if (ra8_svg_render((const uint8_t*)k_svg_fixture,
                     svg_len,
                     0,
                     0,
                     (int32_t)k_es_fb_w,
                     (int32_t)k_es_fb_h) != k_ra8_ok) {
    es_panic_halt(k_msg_rerr, (uint32_t)sizeof(k_msg_rerr) - 1U);
  }
}

/**
 * @brief App entry: render the baked SVG on silicon, print the banner.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The svg-size / framebuffer-CRC banner is emitted; CPU loops in WFI.
 * @since 0.1.0
 */
void main(void)
{
  es_setup_or_halt();
  ra8_isr_globals_enable();
  es_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  if (ra8_gfx_init(s_framebuffer,
                   (uint16_t)k_es_fb_w,
                   (uint16_t)k_es_fb_h,
                   k_ra8_gfx_format_rgb565) != k_ra8_ok) {
    es_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  (void)ra8_gfx_clear((uint32_t)k_es_col_bg);

  int32_t svg_w = 0;
  int32_t svg_h = 0;
  es_render_svg_or_halt(&svg_w, &svg_h);

  es_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  es_print_uint((uint32_t)svg_w);
  es_print(k_msg_x, (uint32_t)sizeof(k_msg_x) - 1U);
  es_print_uint((uint32_t)svg_h);
  es_print(k_msg_crc, (uint32_t)sizeof(k_msg_crc) - 1U);
  es_print_hex(es_framebuffer_hash());
  es_print(k_msg_ok, (uint32_t)sizeof(k_msg_ok) - 1U);

  while (1) {
    __asm__ volatile("wfi");
  }
}
