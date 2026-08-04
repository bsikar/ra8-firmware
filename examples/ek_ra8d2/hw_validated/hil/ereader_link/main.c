/**
 * @file examples/ek_ra8d2/hw_validated/hil/ereader_link/main.c
 * @brief Headless on-silicon HIL gate for in-content hyperlink navigation (#110).
 *
 * @details
 * Closes the *real-hardware* gap for the `<a href>` link pipeline: tokenize the
 * href + element `id`, lay out the chapter, build tappable link rectangles +
 * anchor positions, then drive the navigation API the way a touch tap would --
 * no panel / SD / touch needed. The app:
 *
 *   1. Lays out a baked chapter (two `<a href>` links + a `<p id="foot">`
 *      anchor) through `ra8_reflow` with the bundled Ahem face.
 *   2. Synthesises a "tap" at the centre of each laid-out link rectangle and
 *      resolves it with ra8_reflow_hit_test_link() + ra8_reflow_href_split():
 *      one link classifies as a cross-chapter target, one as a same-chapter
 *      `#fragment`.
 *   3. Resolves the fragment to its page with ra8_reflow_find_anchor().
 *   4. Folds an FNV-1a-32 hash over the laid-out link-rectangle geometry and
 *      prints a banner on the SCI8 J-Link OB console:
 *
 *        `ereader-link-hil: links=2 cross=Y frag=Y apage=<P> geom=<8 hex>`
 *
 * Ahem is a fixed-metric face, so the layout + link geometry are deterministic;
 * the banner is identical every boot and matches the host / ra8_emulator run.
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
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_reflow.h"
#include "ra8_time.h"

/** @enum lk_consts_t @brief Viewport / console / hash knobs (no magic numbers). */
typedef enum : uint32_t {
  k_lk_view_w      = 280U,        /**< Layout viewport width, pixels.  */
  k_lk_view_h      = 360U,        /**< Layout viewport height, pixels. */
  k_lk_font_px     = 16U,         /**< Body font size, pixels.         */
  k_lk_ink         = 0xFF101010U, /**< Body ink colour (ARGB).         */
  k_lk_link_col    = 0xFF2A52BEU, /**< Anchor colour (ARGB).           */
  k_lk_uart_baud   = 115200U,     /**< Console baud.                   */
  k_lk_fnv_offset  = 2166136261U, /**< FNV-1a-32 offset basis.         */
  k_lk_fnv_prime   = 16777619U,   /**< FNV-1a-32 prime.                */
  k_lk_hex_nibbles = 8U,          /**< Hex digits in a 32-bit value.   */
  k_lk_nibble_bits = 4U,          /**< Bits per hex nibble.            */
  k_lk_nibble_mask = 0x0FU,       /**< Low-nibble mask.                */
  k_lk_dec_ten     = 10U,         /**< Hex digit / decimal split.      */
} lk_consts_t;

/** @brief Reflow engine (large -- file-scope, not on the stack). */
static ra8_reflow_t s_engine;

/** @brief Baked chapter: two links (cross-chapter + fragment) and an anchor. */
static const char k_lk_chapter[] =
  "<html><body><h1>Chapter One</h1>"
  "<p>Jump to the <a href=\"ch2.xhtml\">next chapter</a> or skip to the "
  "<a href=\"#foot\">footnote</a> below for the citation.</p>"
  "<p>Body paragraph with enough words to occupy a couple of lines so the "
  "layout has real content to paginate around the links.</p>"
  "<p id=\"foot\">Footnote: this is the cited reference text.</p></body></html>";

static const uint8_t k_msg_boot[]  = "ereader-link-hil: boot\r\n";
static const uint8_t k_msg_fail[]  = "ereader-link-hil: FAIL init\r\n";
static const uint8_t k_msg_lerr[]  = "ereader-link-hil: FAIL layout\r\n";
static const uint8_t k_msg_pre[]   = "ereader-link-hil: links=";
static const uint8_t k_msg_cross[] = " cross=";
static const uint8_t k_msg_frag[]  = " frag=";
static const uint8_t k_msg_apage[] = " apage=";
static const uint8_t k_msg_geom[]  = " geom=";
static const uint8_t k_msg_eol[]   = "\r\n";
static const uint8_t k_msg_yes[]   = "Y";
static const uint8_t k_msg_no[]    = "N";

/** @brief Emit a byte run on the SCI8 console. */
static void lk_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Print the fail banner and trap (ra8_emulator halts on the BKPT). */
static void lk_panic_halt(const uint8_t* msg, uint32_t len)
{
  lk_print(msg, len);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Print a 32-bit value as 8 upper-case hex digits. */
static void lk_print_hex(uint32_t value)
{
  uint8_t buf[k_lk_hex_nibbles];
  for (uint32_t i = 0U; i < (uint32_t)k_lk_hex_nibbles; i++) {
    const uint32_t shift = ((uint32_t)k_lk_hex_nibbles - 1U - i) * (uint32_t)k_lk_nibble_bits;
    const uint32_t nib   = (value >> shift) & (uint32_t)k_lk_nibble_mask;
    buf[i] = (uint8_t)((nib < (uint32_t)k_lk_dec_ten) ? ('0' + nib) : ('A' + (nib - k_lk_dec_ten)));
  }
  lk_print(buf, (uint32_t)k_lk_hex_nibbles);
}

/** @brief Print a small unsigned integer in decimal. */
static void lk_print_uint(uint32_t value)
{
  uint8_t  buf[k_lk_dec_ten];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_lk_dec_ten)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_lk_dec_ten));
    n++;
    value /= (uint32_t)k_lk_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    lk_print(&buf[n - 1U - i], 1U);
  }
}

/** @brief Print "Y" or "N". */
static void lk_print_bool(bool yes)
{
  if (yes) {
    lk_print(k_msg_yes, (uint32_t)sizeof(k_msg_yes) - 1U);
  } else {
    lk_print(k_msg_no, (uint32_t)sizeof(k_msg_no) - 1U);
  }
}

/** @brief FNV-1a-32 over the laid-out link-rectangle geometry. */
static uint32_t lk_geom_hash(void)
{
  const uint8_t* p   = (const uint8_t*)s_engine.link_rects;
  const size_t   n   = (size_t)s_engine.link_rect_count * sizeof(s_engine.link_rects[0]);
  uint32_t       hsh = (uint32_t)k_lk_fnv_offset;
  for (size_t i = 0U; i < n; i++) {
    hsh = (hsh ^ (uint32_t)p[i]) * (uint32_t)k_lk_fnv_prime;
  }
  return hsh;
}

/**
 * @brief Classify the href at a link rect's centre via the public nav API.
 *
 * @param[in]  idx       Link-rect index to probe.
 * @param[out] out_cross Set true when the href is a cross-chapter target.
 * @param[out] out_frag  Set true when the href is a same-chapter fragment.
 * @return None.
 */
static void lk_probe_rect(uint32_t idx, bool* out_cross, bool* out_frag)
{
  const ra8_reflow_link_rect_t* rect = &s_engine.link_rects[idx];
  const int32_t                 cx   = rect->x + (rect->w / 2);
  const int32_t                 cy   = rect->y + (rect->h / 2);
  uint32_t                      off  = 0U;
  uint32_t                      len  = 0U;
  if (ra8_reflow_hit_test_link(&s_engine, rect->page_index, cx, cy, &off, &len) != k_ra8_ok) {
    return;
  }
  ra8_reflow_href_kind_t kind = k_ra8_reflow_href_empty;
  uint32_t               pl   = 0U;
  uint32_t               fo   = 0U;
  uint32_t               fl   = 0U;
  if (ra8_reflow_href_split((const char*)&s_engine.text_pool[off], len, &kind, &pl, &fo, &fl) !=
      k_ra8_ok) {
    return;
  }
  if ((kind == k_ra8_reflow_href_chapter) || (kind == k_ra8_reflow_href_chapter_fragment)) {
    *out_cross = true;
  }
  if (kind == k_ra8_reflow_href_fragment) {
    *out_frag = true;
  }
}

/** @brief Bring up clocks/MSTP/time + the SCI8 console; halt on failure. */
static void lk_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    lk_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    lk_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    lk_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_lk_uart_baud) != k_ra8_ok) {
    lk_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: lay out the link chapter, drive the nav API, print results.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The nav-result banner is emitted; the CPU then loops in WFI.
 * @since 0.1.0
 */
int32_t main(void)
{
  lk_setup_or_halt();
  ra8_isr_globals_enable();
  lk_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  if (ra8_reflow_init((uint16_t)k_lk_view_w,
                      (uint16_t)k_lk_view_h,
                      k_ahem_ttf,
                      (size_t)k_ahem_ttf_len,
                      (uint16_t)k_lk_font_px,
                      (uint32_t)k_lk_ink,
                      (uint32_t)k_lk_link_col,
                      &s_engine) != k_ra8_ok) {
    lk_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  uint32_t pages = 0U;
  if (ra8_reflow_layout_chapter(&s_engine,
                                (const uint8_t*)k_lk_chapter,
                                (uint32_t)(sizeof(k_lk_chapter) - 1U),
                                &pages) != k_ra8_ok) {
    lk_panic_halt(k_msg_lerr, (uint32_t)sizeof(k_msg_lerr) - 1U);
  }

  /* Synthesise a tap at the centre of every link rect; classify each. */
  bool cross = false;
  bool frag  = false;
  for (uint32_t i = 0U; i < s_engine.link_rect_count; ++i) {
    lk_probe_rect(i, &cross, &frag);
  }

  /* Resolve the same-chapter "#foot" fragment to its page. */
  uint32_t apage = 0U;
  if (ra8_reflow_find_anchor(&s_engine, "foot", (uint32_t)(sizeof("foot") - 1U), &apage) !=
      k_ra8_ok) {
    apage = (uint32_t)k_lk_view_h; /* sentinel: not found */
  }

  lk_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  lk_print_uint(s_engine.link_target_count);
  lk_print(k_msg_cross, (uint32_t)sizeof(k_msg_cross) - 1U);
  lk_print_bool(cross);
  lk_print(k_msg_frag, (uint32_t)sizeof(k_msg_frag) - 1U);
  lk_print_bool(frag);
  lk_print(k_msg_apage, (uint32_t)sizeof(k_msg_apage) - 1U);
  lk_print_uint(apage);
  lk_print(k_msg_geom, (uint32_t)sizeof(k_msg_geom) - 1U);
  lk_print_hex(lk_geom_hash());
  lk_print(k_msg_eol, (uint32_t)sizeof(k_msg_eol) - 1U);

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
