/**
 * @file examples/ek_ra8d2/hw_validated/hil/ereader_table/src/main.c
 * @brief Headless on-silicon HIL gate for `<table>` grid layout (#107).
 *
 * @details
 * Closes the *real-hardware* gap for the table layout: tokenize
 * table/tr/td/th, size equal columns, flow each cell, stack rows (with
 * row-level page breaks) -- no panel / SD / touch needed. The app lays out a
 * baked chapter with a 2-column table (plus a heading and trailing paragraph)
 * through `reflow` with the fixed-metric Ahem face, folds an FNV-1a-32 hash
 * over the laid-out glyph geometry (every glyph's x + y), and prints a banner
 * on the SCI8 J-Link OB console:
 *
 *   `ereader-table-hil: glyphs=<N> geom=<8 hex>`
 *
 * Ahem's fixed metrics make the grid deterministic; the banner is identical
 * every boot and matches the host / ra8_emulator run, so any drift in the column
 * sizing, cell flow, or row stacking changes the hash.
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
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"
#include "reflow.h"

/** @enum tb_consts_t @brief Viewport / console / hash knobs (no magic numbers). */
typedef enum : uint32_t {
  k_tb_view_w      = 280U,        /**< Layout viewport width, pixels.  */
  k_tb_view_h      = 360U,        /**< Layout viewport height, pixels. */
  k_tb_font_px     = 16U,         /**< Body font size, pixels.         */
  k_tb_ink         = 0xFF101010U, /**< Body ink colour (ARGB).         */
  k_tb_link_col    = 0xFF2A52BEU, /**< Anchor colour (ARGB).           */
  k_tb_uart_baud   = 115200U,     /**< Console baud.                   */
  k_tb_fnv_offset  = 2166136261U, /**< FNV-1a-32 offset basis.         */
  k_tb_fnv_prime   = 16777619U,   /**< FNV-1a-32 prime.                */
  k_tb_hex_nibbles = 8U,          /**< Hex digits in a 32-bit value.   */
  k_tb_nibble_bits = 4U,          /**< Bits per hex nibble.            */
  k_tb_nibble_mask = 0x0FU,       /**< Low-nibble mask.                */
  k_tb_dec_ten     = 10U,         /**< Hex digit / decimal split.      */
} tb_consts_t;

/** @brief Reflow engine (large -- file-scope, not on the stack). */
static reflow_t s_engine;

/** @brief Baked chapter: a heading, a 2-column table, and a trailing paragraph. */
static const char k_tb_chapter[] =
  "<html><body><h1>Schedule</h1>"
  "<table>"
  "<tr><th>Day</th><th>Activity for the morning</th></tr>"
  "<tr><td>Monday</td><td>Read the first three chapters</td></tr>"
  "<tr><td>Tuesday</td><td>Notes and a short summary</td></tr>"
  "<tr><td>Wednesday</td><td>Review and discuss</td></tr>"
  "</table>"
  "<p>Text after the table resumes the normal flow.</p></body></html>";

static const uint8_t k_msg_boot[] = "ereader-table-hil: boot\r\n";
static const uint8_t k_msg_fail[] = "ereader-table-hil: FAIL init\r\n";
static const uint8_t k_msg_lerr[] = "ereader-table-hil: FAIL layout\r\n";
static const uint8_t k_msg_pre[]  = "ereader-table-hil: glyphs=";
static const uint8_t k_msg_geom[] = " geom=";
static const uint8_t k_msg_eol[]  = "\r\n";

/** @brief Emit a byte run on the SCI8 console. */
static void tb_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Print the fail banner and trap (ra8_emulator halts on the BKPT). */
static void tb_panic_halt(const uint8_t* msg, uint32_t len)
{
  tb_print(msg, len);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Print a 32-bit value as 8 upper-case hex digits. */
static void tb_print_hex(uint32_t value)
{
  uint8_t buf[k_tb_hex_nibbles];
  for (uint32_t i = 0U; i < (uint32_t)k_tb_hex_nibbles; i++) {
    const uint32_t shift = ((uint32_t)k_tb_hex_nibbles - 1U - i) * (uint32_t)k_tb_nibble_bits;
    const uint32_t nib   = (value >> shift) & (uint32_t)k_tb_nibble_mask;
    buf[i] = (uint8_t)((nib < (uint32_t)k_tb_dec_ten) ? ('0' + nib) : ('A' + (nib - k_tb_dec_ten)));
  }
  tb_print(buf, (uint32_t)k_tb_hex_nibbles);
}

/** @brief Print a small unsigned integer in decimal. */
static void tb_print_uint(uint32_t value)
{
  uint8_t  buf[k_tb_dec_ten];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_tb_dec_ten)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_tb_dec_ten));
    n++;
    value /= (uint32_t)k_tb_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    tb_print(&buf[n - 1U - i], 1U);
  }
}

/** @brief FNV-1a-32 over every laid-out glyph's (x, y) position. */
static uint32_t tb_geom_hash(void)
{
  uint32_t     hsh    = (uint32_t)k_tb_fnv_offset;
  const size_t nbytes = sizeof(int32_t) * 2U; /* x + y per glyph */
  for (uint32_t g = 0U; g < s_engine.glyph_count; g++) {
    const int32_t  coords[2] = {s_engine.glyphs[g].x, s_engine.glyphs[g].y};
    const uint8_t* p         = (const uint8_t*)coords;
    for (size_t i = 0U; i < nbytes; i++) {
      hsh = (hsh ^ (uint32_t)p[i]) * (uint32_t)k_tb_fnv_prime;
    }
  }
  return hsh;
}

/** @brief Bring up clocks/MSTP/time + the SCI8 console; halt on failure. */
static void tb_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    tb_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    tb_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    tb_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_tb_uart_baud) != k_ra8_ok) {
    tb_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
}

/**
 * @brief App entry: lay out the table chapter, hash the geometry, print.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The geometry-hash banner is emitted; the CPU then loops in WFI.
 * @since 0.1.0
 */
void main(void)
{
  tb_setup_or_halt();
  ra8_isr_globals_enable();
  tb_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  if (reflow_init((uint16_t)k_tb_view_w,
                  (uint16_t)k_tb_view_h,
                  k_ahem_ttf,
                  (size_t)k_ahem_ttf_len,
                  (uint16_t)k_tb_font_px,
                  (uint32_t)k_tb_ink,
                  (uint32_t)k_tb_link_col,
                  &s_engine) != k_ra8_ok) {
    tb_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  uint32_t pages = 0U;
  if (reflow_layout_chapter(&s_engine,
                            (const uint8_t*)k_tb_chapter,
                            (uint32_t)(sizeof(k_tb_chapter) - 1U),
                            &pages) != k_ra8_ok) {
    tb_panic_halt(k_msg_lerr, (uint32_t)sizeof(k_msg_lerr) - 1U);
  }

  tb_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  tb_print_uint(s_engine.glyph_count);
  tb_print(k_msg_geom, (uint32_t)sizeof(k_msg_geom) - 1U);
  tb_print_hex(tb_geom_hash());
  tb_print(k_msg_eol, (uint32_t)sizeof(k_msg_eol) - 1U);

  while (1) {
    __asm__ volatile("wfi");
  }
}
