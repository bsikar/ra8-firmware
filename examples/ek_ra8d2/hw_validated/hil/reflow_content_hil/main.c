/**
 * @file examples/ek_ra8d2/hw_validated/hil/reflow_content_hil/main.c
 * @brief Headless on-silicon HIL gate for reflow content render + pagination (#115).
 *
 * @details
 * Closes the *real-hardware* gap for the book-content render path: paginate a
 * multi-page chapter, render every page into a framebuffer, and exercise a
 * font-size re-flow -- no panel / SD / touch needed. The app lays out a baked
 * multi-paragraph chapter through `ra_reflow` (fixed-metric Ahem face) into a
 * 160x192 RGB565 framebuffer, renders each page and folds an FNV-1a-32 over the
 * framebuffer, then calls ra_reflow_set_font_size() to re-flow at a larger size
 * and renders again. It prints a banner on the SCI8 J-Link OB console:
 *
 *   `reflow-content-hil: pages=<N> crc=<8 hex> rpages=<M> crc=<8 hex>`
 *
 * Ahem's fixed metrics make pagination + render deterministic; the banner is
 * identical every boot (stable across resets) and matches the host / board_sim
 * run, so any drift in the layout, pagination, or render changes the hash.
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

/** @enum rc_consts_t @brief Framebuffer / console / hash / type-size knobs. */
typedef enum : uint32_t {
  k_rc_fb_w        = 160U,        /**< Framebuffer width, pixels.        */
  k_rc_fb_h        = 192U,        /**< Framebuffer height, pixels.       */
  k_rc_font_px     = 16U,         /**< Body font size, pixels.           */
  k_rc_reflow_px   = 24U,         /**< Re-flow font size, pixels.        */
  k_rc_ink         = 0xFF101010U, /**< Body ink colour (ARGB).           */
  k_rc_link_col    = 0xFF2A52BEU, /**< Anchor colour (ARGB).             */
  k_rc_bg          = 0xF4F0E8U,   /**< Page background (0x00RRGGBB).      */
  k_rc_uart_chan   = 8U,          /**< SCI8 J-Link OB console.           */
  k_rc_uart_baud   = 115200U,     /**< Console baud.                     */
  k_rc_fnv_offset  = 2166136261U, /**< FNV-1a-32 offset basis.           */
  k_rc_fnv_prime   = 16777619U,   /**< FNV-1a-32 prime.                  */
  k_rc_hex_nibbles = 8U,          /**< Hex digits in a 32-bit value.     */
  k_rc_nibble_bits = 4U,          /**< Bits per hex nibble.              */
  k_rc_nibble_mask = 0x0FU,       /**< Low-nibble mask.                  */
  k_rc_dec_ten     = 10U,         /**< Hex digit / decimal split.        */
} rc_consts_t;

/** @brief SCI8 console TXD = PD02. */
static const ra_port_pin_t k_rc_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
/** @brief SCI8 console RXD = PD03. */
static const ra_port_pin_t k_rc_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief RGB565 framebuffer in internal SRAM (no panel attached). */
static uint16_t s_framebuffer[(size_t)k_rc_fb_h * (size_t)k_rc_fb_w];

/** @brief Reflow engine (large -- file-scope, not on the stack). */
static ra_reflow_t s_engine;

/** @brief Baked multi-paragraph chapter (paginates to several pages). */
static const char k_rc_chapter[] =
  "<html><body><h1>The Machine</h1>"
  "<p>The Time Traveller was expounding a recondite matter to us. His pale grey "
  "eyes shone and twinkled, and his usually pale face was flushed and animated.</p>"
  "<p>The fire burnt brightly, and the soft radiance of the incandescent lights "
  "caught the bubbles that flashed and passed in our glasses.</p>"
  "<p>Our chairs, being his patents, embraced and caressed us rather than "
  "submitted to be sat upon, and there was that luxurious after-dinner "
  "atmosphere when thought runs gracefully free of the trammels of precision.</p>"
  "<p>And he put it to us in this way, marking the points with a lean forefinger, "
  "as we sat and lazily admired his earnestness over this new paradox.</p>"
  "</body></html>";

static const uint8_t k_msg_boot[]   = "reflow-content-hil: boot\r\n";
static const uint8_t k_msg_fail[]   = "reflow-content-hil: FAIL init\r\n";
static const uint8_t k_msg_lerr[]   = "reflow-content-hil: FAIL layout\r\n";
static const uint8_t k_msg_pre[]    = "reflow-content-hil: pages=";
static const uint8_t k_msg_crc[]    = " crc=";
static const uint8_t k_msg_rpages[] = " rpages=";
static const uint8_t k_msg_eol[]    = "\r\n";

/** @brief Emit a byte run on the SCI8 console. */
static void rc_print(const uint8_t* msg, uint32_t len)
{
  (void)ra_sci_write_polling((uint8_t)k_rc_uart_chan, msg, len);
}

/** @brief Print the fail banner and trap (board_sim halts on the BKPT). */
static void rc_panic_halt(const uint8_t* msg, uint32_t len)
{
  rc_print(msg, len);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Route the SCI8 console pins to async mode. */
[[nodiscard]] static ra_err_t rc_console_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_rc_pin_txd, k_ra_psel_sci_async, "rc.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_rc_pin_rxd, k_ra_psel_sci_async, "rc.rxd8");
}

/** @brief Print a 32-bit value as 8 upper-case hex digits. */
static void rc_print_hex(uint32_t value)
{
  uint8_t buf[k_rc_hex_nibbles];
  for (uint32_t i = 0U; i < (uint32_t)k_rc_hex_nibbles; i++) {
    const uint32_t shift = ((uint32_t)k_rc_hex_nibbles - 1U - i) * (uint32_t)k_rc_nibble_bits;
    const uint32_t nib   = (value >> shift) & (uint32_t)k_rc_nibble_mask;
    buf[i] = (uint8_t)((nib < (uint32_t)k_rc_dec_ten) ? ('0' + nib) : ('A' + (nib - k_rc_dec_ten)));
  }
  rc_print(buf, (uint32_t)k_rc_hex_nibbles);
}

/** @brief Print a small unsigned integer in decimal. */
static void rc_print_uint(uint32_t value)
{
  uint8_t  buf[k_rc_dec_ten];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_rc_dec_ten)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_rc_dec_ten));
    n++;
    value /= (uint32_t)k_rc_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    rc_print(&buf[n - 1U - i], 1U);
  }
}

/**
 * @brief Render every page of the laid-out chapter; fold an FNV over the output.
 *
 * @param[out] out_hash Receives the FNV-1a-32 over every page's framebuffer.
 * @return The page count.
 */
static uint32_t rc_render_all(uint32_t* out_hash)
{
  uint32_t pages = 0U;
  (void)ra_reflow_get_page_count(&s_engine, &pages);
  uint32_t     hsh    = (uint32_t)k_rc_fnv_offset;
  const size_t nbytes = (size_t)k_rc_fb_w * (size_t)k_rc_fb_h * sizeof(uint16_t);
  for (uint32_t p = 0U; p < pages; p++) {
    (void)ra_gfx_clear((uint32_t)k_rc_bg);
    (void)ra_reflow_render_page(&s_engine, p, s_framebuffer);
    const uint8_t* fb = (const uint8_t*)s_framebuffer;
    for (size_t i = 0U; i < nbytes; i++) {
      hsh = (hsh ^ (uint32_t)fb[i]) * (uint32_t)k_rc_fnv_prime;
    }
  }
  *out_hash = hsh;
  return pages;
}

/** @brief Bring up clocks/MSTP/time + the SCI8 console; halt on failure. */
static void rc_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if ((ra_cgc_init() != k_ra_ok) || (ra_mstp_init() != k_ra_ok)) {
    rc_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if ((ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) ||
      (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok)) {
    rc_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    rc_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (rc_console_pins_init() != k_ra_ok) {
    rc_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  const ra_sci_cfg_t cfg = {.baud      = (uint32_t)k_rc_uart_baud,
                            .data_bits = k_ra_sci_data_8,
                            .parity    = k_ra_sci_parity_none,
                            .stop_bits = k_ra_sci_stop_1,
                            .pclk_hz   = pclka_hz};
  if (ra_sci_init((uint8_t)k_rc_uart_chan, &cfg) != k_ra_ok) {
    rc_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: paginate + render the chapter, re-flow bigger, print.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The page-count + render-hash banner is emitted; the CPU loops in WFI.
 * @since 0.1.0
 */
int32_t main(void)
{
  rc_setup_or_halt();
  ra_isr_globals_enable();
  rc_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  if (ra_gfx_init(s_framebuffer,
                  (uint16_t)k_rc_fb_w,
                  (uint16_t)k_rc_fb_h,
                  k_ra_gfx_format_rgb565) != k_ra_ok) {
    rc_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra_reflow_init((uint16_t)k_rc_fb_w,
                     (uint16_t)k_rc_fb_h,
                     k_ahem_ttf,
                     (size_t)k_ahem_ttf_len,
                     (uint16_t)k_rc_font_px,
                     (uint32_t)k_rc_ink,
                     (uint32_t)k_rc_link_col,
                     &s_engine) != k_ra_ok) {
    rc_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  uint32_t pages = 0U;
  if (ra_reflow_layout_chapter(&s_engine,
                               (const uint8_t*)k_rc_chapter,
                               (uint32_t)(sizeof(k_rc_chapter) - 1U),
                               &pages) != k_ra_ok) {
    rc_panic_halt(k_msg_lerr, (uint32_t)sizeof(k_msg_lerr) - 1U);
  }

  uint32_t       crc1   = 0U;
  const uint32_t pages1 = rc_render_all(&crc1);

  /* Re-flow at a larger size on the cached chapter; render again. */
  if (ra_reflow_set_font_size(&s_engine, (uint16_t)k_rc_reflow_px) != k_ra_ok) {
    rc_panic_halt(k_msg_lerr, (uint32_t)sizeof(k_msg_lerr) - 1U);
  }
  uint32_t       crc2   = 0U;
  const uint32_t pages2 = rc_render_all(&crc2);

  rc_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  rc_print_uint(pages1);
  rc_print(k_msg_crc, (uint32_t)sizeof(k_msg_crc) - 1U);
  rc_print_hex(crc1);
  rc_print(k_msg_rpages, (uint32_t)sizeof(k_msg_rpages) - 1U);
  rc_print_uint(pages2);
  rc_print(k_msg_crc, (uint32_t)sizeof(k_msg_crc) - 1U);
  rc_print_hex(crc2);
  rc_print(k_msg_eol, (uint32_t)sizeof(k_msg_eol) - 1U);

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
