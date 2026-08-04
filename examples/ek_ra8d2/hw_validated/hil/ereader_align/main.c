/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/ereader_align/main.c
 * @brief Headless on-silicon HIL gate for text alignment + justification (#108).
 *
 * @details
 * Closes the *real-hardware* gap for the alignment pipeline: parse
 * `style="text-align:..."`, lay out centre / right / justified paragraphs, and
 * shift / space the glyphs accordingly -- no panel / SD / touch needed. The app
 * lays out a baked chapter (one paragraph each of right, centre, justify, and
 * the default left) through `ra8_reflow` with the fixed-metric Ahem face, folds
 * an FNV-1a-32 hash over the laid-out glyph geometry (every glyph's x + y), and
 * prints a banner on the SCI8 J-Link OB console:
 *
 *   `ereader-align-hil: glyphs=<N> geom=<8 hex>`
 *
 * Ahem's fixed metrics make the layout deterministic; the banner is identical
 * every boot and matches the host / ra8_emulator run, so any drift in the
 * alignment offsets or the justification slack changes the hash.
 *
 *
 * [Ring 7 / App] {World: NS}
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "font_fixture.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_reflow.h"
#include "ra8_time.h"

/** @enum al_consts_t @brief Viewport / console / hash knobs (no magic numbers). */
typedef enum : uint32_t {
  k_al_view_w      = 280U,        /**< Layout viewport width, pixels.  */
  k_al_view_h      = 360U,        /**< Layout viewport height, pixels. */
  k_al_font_px     = 16U,         /**< Body font size, pixels.         */
  k_al_ink         = 0xFF101010U, /**< Body ink colour (ARGB).         */
  k_al_link_col    = 0xFF2A52BEU, /**< Anchor colour (ARGB).           */
  k_al_uart_baud   = 115200U,     /**< Console baud.                   */
  k_al_fnv_offset  = 2166136261U, /**< FNV-1a-32 offset basis.         */
  k_al_fnv_prime   = 16777619U,   /**< FNV-1a-32 prime.                */
  k_al_hex_nibbles = 8U,          /**< Hex digits in a 32-bit value.   */
  k_al_nibble_bits = 4U,          /**< Bits per hex nibble.            */
  k_al_nibble_mask = 0x0FU,       /**< Low-nibble mask.                */
  k_al_dec_ten     = 10U,         /**< Hex digit / decimal split.      */
} al_consts_t;

/** @brief Reflow engine (large -- file-scope, not on the stack). */
static ra8_reflow_t s_engine;

/** @brief Baked chapter: right, centre, justified, and default-left paragraphs. */
static const char k_al_chapter[] =
  "<html><body><h1 style=\"text-align:center\">Aligned</h1>"
  "<p style=\"text-align:right\">Right aligned paragraph flush to the margin.</p>"
  "<p style=\"text-align:center\">Centred paragraph line in the column.</p>"
  "<p style=\"text-align:justify\">aa bb cc dd ee ff gg hh ii jj kk ll mm nn oo "
  "pp qq rr ss tt uu vv ww xx yy zz</p>"
  "<p>Plain left paragraph as the default baseline.</p></body></html>";

static const uint8_t k_msg_boot[] = "ereader-align-hil: boot\r\n";
static const uint8_t k_msg_fail[] = "ereader-align-hil: FAIL init\r\n";
static const uint8_t k_msg_lerr[] = "ereader-align-hil: FAIL layout\r\n";
static const uint8_t k_msg_pre[]  = "ereader-align-hil: glyphs=";
static const uint8_t k_msg_geom[] = " geom=";
static const uint8_t k_msg_eol[]  = "\r\n";

/** @brief Emit a byte run on the SCI8 console. */
static void al_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Print the fail banner and trap (ra8_emulator halts on the BKPT). */
static void al_panic_halt(const uint8_t* msg, uint32_t len)
{
  al_print(msg, len);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Print a 32-bit value as 8 upper-case hex digits. */
static void al_print_hex(uint32_t value)
{
  uint8_t buf[k_al_hex_nibbles];
  for (uint32_t i = 0U; i < (uint32_t)k_al_hex_nibbles; i++) {
    const uint32_t shift = ((uint32_t)k_al_hex_nibbles - 1U - i) * (uint32_t)k_al_nibble_bits;
    const uint32_t nib   = (value >> shift) & (uint32_t)k_al_nibble_mask;
    buf[i] = (uint8_t)((nib < (uint32_t)k_al_dec_ten) ? ('0' + nib) : ('A' + (nib - k_al_dec_ten)));
  }
  al_print(buf, (uint32_t)k_al_hex_nibbles);
}

/** @brief Print a small unsigned integer in decimal. */
static void al_print_uint(uint32_t value)
{
  uint8_t  buf[k_al_dec_ten];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_al_dec_ten)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_al_dec_ten));
    n++;
    value /= (uint32_t)k_al_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    al_print(&buf[n - 1U - i], 1U);
  }
}

/** @brief FNV-1a-32 over every laid-out glyph's (x, y) position. */
static uint32_t al_geom_hash(void)
{
  uint32_t     hsh    = (uint32_t)k_al_fnv_offset;
  const size_t nbytes = sizeof(int32_t) * 2U; /* x + y per glyph */
  for (uint32_t g = 0U; g < s_engine.glyph_count; g++) {
    const int32_t  coords[2] = {s_engine.glyphs[g].x, s_engine.glyphs[g].y};
    const uint8_t* p         = (const uint8_t*)coords;
    for (size_t i = 0U; i < nbytes; i++) {
      hsh = (hsh ^ (uint32_t)p[i]) * (uint32_t)k_al_fnv_prime;
    }
  }
  return hsh;
}

/** @brief Bring up clocks/MSTP/time + the SCI8 console; halt on failure. */
static void al_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    al_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    al_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    al_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_al_uart_baud) != k_ra8_ok) {
    al_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: lay out the aligned chapter, hash the geometry, print.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The geometry-hash banner is emitted; the CPU then loops in WFI.
 * @since 0.1.0
 */
int32_t main(void)
{
  al_setup_or_halt();
  ra8_isr_globals_enable();
  al_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  if (ra8_reflow_init((uint16_t)k_al_view_w,
                      (uint16_t)k_al_view_h,
                      k_ahem_ttf,
                      (size_t)k_ahem_ttf_len,
                      (uint16_t)k_al_font_px,
                      (uint32_t)k_al_ink,
                      (uint32_t)k_al_link_col,
                      &s_engine) != k_ra8_ok) {
    al_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  uint32_t pages = 0U;
  if (ra8_reflow_layout_chapter(&s_engine,
                                (const uint8_t*)k_al_chapter,
                                (uint32_t)(sizeof(k_al_chapter) - 1U),
                                &pages) != k_ra8_ok) {
    al_panic_halt(k_msg_lerr, (uint32_t)sizeof(k_msg_lerr) - 1U);
  }

  al_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  al_print_uint(s_engine.glyph_count);
  al_print(k_msg_geom, (uint32_t)sizeof(k_msg_geom) - 1U);
  al_print_hex(al_geom_hash());
  al_print(k_msg_eol, (uint32_t)sizeof(k_msg_eol) - 1U);

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
