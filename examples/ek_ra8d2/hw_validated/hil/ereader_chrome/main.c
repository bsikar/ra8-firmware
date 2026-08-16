/**
 * @file examples/ek_ra8d2/hw_validated/hil/ereader_chrome/main.c
 * @brief Headless on-silicon HIL gate for the e-reader chrome render pipeline.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The e-reader chrome (#76 box model + #80 interaction) is golden-validated on
 * the ra8_emulator. This app closes the on-hardware gap: it exercises the
 * **real** `ra8_box` layout + `ra8_gfx` software render on the actual RA8D2,
 * deterministically, with no panel / SDRAM / touch / SD dependency.
 *
 * It builds a representative chrome screen as an `ra8_box` tree (a status bar
 * over a 2-column grid of "book" cells), lays it out with `ra8_box_layout`,
 * renders the boxes (fill + 1-px border) and labels (bundled
 * `ra8_gfx_font_8x16`) into a static RGB565 framebuffer in internal SRAM, then
 * folds an FNV-1a-32 hash over the whole framebuffer and prints it over the
 * SCI8 J-Link OB console:
 *
 *   `ereader-hil: chrome boxes=<N> crc=<8 hex>`
 *
 * The HIL gate (`hil.conf`, `uart_scrape`) asserts that hash equals the
 * baseline captured on-bench -- so any drift in the box-model math, the
 * software rasteriser, or the toolchain output flags a regression on real
 * silicon. Deterministic: integer layout + a fixed bitmap font + a zeroed
 * static framebuffer give the same hash every boot.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_boot_entry.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_box.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_gfx_font.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"
#include "ra8_ui.h"

/**
 * @enum chrome_hil_cfg_t
 * @brief Framebuffer + console + render geometry constants.
 */
typedef enum : uint32_t {
  k_ch_fb_w        = 320U,        /**< Framebuffer width, pixels.    */
  k_ch_fb_h        = 240U,        /**< Framebuffer height, pixels.   */
  k_ch_uart_baud   = 115200U,     /**< Console baud.                 */
  k_ch_max_nodes   = 16U,         /**< Box-tree node capacity.       */
  k_ch_cell_count  = 4U,          /**< Book cells in the grid.       */
  k_ch_status_h    = 28U,         /**< Status-bar fixed height, px.  */
  k_ch_root_pad    = 8U,          /**< Root padding, px.             */
  k_ch_grid_pad    = 6U,          /**< Grid padding, px.             */
  k_ch_grid_gap    = 8U,          /**< Grid inter-cell gap, px.      */
  k_ch_cell_pad    = 8U,          /**< Cell text inset, px.          */
  k_ch_border_w    = 1U,          /**< Cell border width, px.        */
  k_ch_fnv_offset  = 2166136261U, /**< FNV-1a-32 offset basis.       */
  k_ch_fnv_prime   = 16777619U,   /**< FNV-1a-32 prime.              */
  k_ch_hex_nibbles = 8U,          /**< Hex digits in a 32-bit value. */
  k_ch_nibble_bits = 4U,          /**< Bits per hex nibble.          */
  k_ch_nibble_mask = 0x0FU,       /**< Low-nibble mask.              */
  k_ch_dec_ten     = 10U,         /**< Hex digit / decimal split.    */
} chrome_hil_cfg_t;

/**
 * @enum chrome_hil_color_t
 * @brief Chrome palette (0xRRGGBB; ra8_gfx down-converts to RGB565).
 */
typedef enum : uint32_t {
  k_ch_col_page   = 0xF4F0E8U, /**< Page / root background.  */
  k_ch_col_bar    = 0x303860U, /**< Status-bar fill (navy).  */
  k_ch_col_bar_tx = 0xFFFFFFU, /**< Status-bar text (white). */
  k_ch_col_cell   = 0xC8D8F0U, /**< Cell fill (light blue).  */
  k_ch_col_edge   = 0x283048U, /**< Cell border (dark navy). */
  k_ch_col_ink    = 0x101010U, /**< Cell label (near-black). */
} chrome_hil_color_t;

/** @brief RGB565 render target in internal SRAM. */
static uint16_t s_framebuffer[(size_t)k_ch_fb_h * (size_t)k_ch_fb_w];

/** @brief Box-tree node storage. */
static ra8_box_t s_box_nodes[k_ch_max_nodes];

/** @brief Per-node label (NULL = no text), indexed by box-tree node. */
static const char* s_label[k_ch_max_nodes];

/** @brief Demo shelf titles (public-domain). */
static const char* const k_ch_titles[k_ch_cell_count] = {"Frankenstein",
                                                         "Moby Dick",
                                                         "Walden",
                                                         "The Odyssey"};

static const uint8_t k_msg_boot[] = "ereader-hil: boot\r\n";
static const uint8_t k_msg_fail[] = "ereader-hil: FAIL init\r\n";
static const uint8_t k_msg_pre[]  = "ereader-hil: chrome boxes=";
static const uint8_t k_msg_crc[]  = " crc=";
static const uint8_t k_msg_eol[]  = "\r\n";

/* ===========================================================================
 * Console
 * ===========================================================================
 */

/** @brief Emit a byte run on the SCI8 console. */
static void ch_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Park forever in WFI after a fatal init error. */
static void ch_panic_halt(void)
{
  ch_print(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Print @p value as 8 uppercase hex digits + a trailing EOL. */
static void ch_print_hex(uint32_t value)
{
  uint8_t buf[k_ch_hex_nibbles];
  for (uint32_t i = 0U; i < (uint32_t)k_ch_hex_nibbles; i++) {
    const uint32_t shift = ((uint32_t)k_ch_hex_nibbles - 1U - i) * (uint32_t)k_ch_nibble_bits;
    const uint32_t nib   = (value >> shift) & (uint32_t)k_ch_nibble_mask;
    buf[i] = (uint8_t)((nib < (uint32_t)k_ch_dec_ten) ? ('0' + nib) : ('A' + (nib - k_ch_dec_ten)));
  }
  ch_print(buf, (uint32_t)k_ch_hex_nibbles);
}

/** @brief Print a small unsigned integer in decimal. */
static void ch_print_uint(uint32_t value)
{
  uint8_t  buf[k_ch_dec_ten];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_ch_dec_ten)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_ch_dec_ten));
    n++;
    value /= (uint32_t)k_ch_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    ch_print(&buf[n - 1U - i], 1U);
  }
}

/* ===========================================================================
 * Chrome build + render
 * ===========================================================================
 */

/** @brief Build the status-bar-over-grid chrome tree; return the root index. */
static int16_t ch_build_chrome(ra8_box_tree_t* tree)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ch_max_nodes; i++) {
    s_label[i] = nullptr;
  }
  (void)ra8_box_tree_init(tree, s_box_nodes, (uint16_t)k_ch_max_nodes);
  const ra8_box_t root_n = {.kind = (uint8_t)k_ra8_box_stack_v,
                            .pad  = (int16_t)k_ch_root_pad,
                            .gap  = (int16_t)k_ch_root_pad,
                            .flex = 1U,
                            .fill = (uint32_t)k_ch_col_page};
  const int16_t   root   = ra8_box_add(tree, (int16_t)k_ra8_box_none, &root_n);

  const ra8_box_t bar_n = {.kind  = (uint8_t)k_ra8_box_leaf,
                           .fixed = (int16_t)k_ch_status_h,
                           .fill  = (uint32_t)k_ch_col_bar};
  const int16_t   bar   = ra8_box_add(tree, root, &bar_n);
  s_label[bar]          = "Library";

  const ra8_box_t grid_n = {.kind      = (uint8_t)k_ra8_box_grid,
                            .grid_cols = 2U,
                            .flex      = 1U,
                            .pad       = (int16_t)k_ch_grid_pad,
                            .gap       = (int16_t)k_ch_grid_gap};
  const int16_t   grid   = ra8_box_add(tree, root, &grid_n);

  for (uint32_t i = 0U; i < (uint32_t)k_ch_cell_count; i++) {
    const ra8_box_t cell_n = {.kind     = (uint8_t)k_ra8_box_leaf,
                              .flex     = 1U,
                              .pad      = (int16_t)k_ch_cell_pad,
                              .fill     = (uint32_t)k_ch_col_cell,
                              .border   = (uint32_t)k_ch_col_edge,
                              .border_w = (int16_t)k_ch_border_w,
                              .tag      = (int16_t)i};
    const int16_t   cell   = ra8_box_add(tree, grid, &cell_n);
    s_label[cell]          = k_ch_titles[i];
  }
  return root;
}

/** @brief Fill a rectangle by drawing @p h horizontal spans. */
static void ch_fill(const ra8_ui_rect_t* r, uint32_t color)
{
  for (int32_t row = 0; row < r->h; row++) {
    (void)ra8_gfx_line(r->x, r->y + row, r->x + r->w - 1, r->y + row, color);
  }
}

/** @brief Draw a 1-pixel border around a rectangle. */
static void ch_border(const ra8_ui_rect_t* r, uint32_t color)
{
  const int32_t x1 = r->x + r->w - 1;
  const int32_t y1 = r->y + r->h - 1;
  (void)ra8_gfx_line(r->x, r->y, x1, r->y, color);
  (void)ra8_gfx_line(r->x, y1, x1, y1, color);
  (void)ra8_gfx_line(r->x, r->y, r->x, y1, color);
  (void)ra8_gfx_line(x1, r->y, x1, y1, color);
}

/** @brief Render the laid-out tree: per node, fill + border + label. */
static void ch_render(const ra8_box_tree_t* tree)
{
  (void)ra8_gfx_clear((uint32_t)k_ch_col_page);
  for (uint16_t i = 0U; i < tree->count; i++) {
    const ra8_box_t* b = &tree->nodes[i];
    if (b->fill != (uint32_t)k_ra8_box_no_colour) {
      ch_fill(&b->rect, b->fill);
    }
    if ((b->border != (uint32_t)k_ra8_box_no_colour) && (b->border_w > 0)) {
      ch_border(&b->rect, b->border);
    }
    if (s_label[i] != nullptr) {
      const uint32_t fg =
        (b->fill == (uint32_t)k_ch_col_bar) ? (uint32_t)k_ch_col_bar_tx : (uint32_t)k_ch_col_ink;
      const uint32_t bg =
        (b->fill != (uint32_t)k_ra8_box_no_colour) ? b->fill : (uint32_t)k_ch_col_page;
      (void)ra8_gfx_text_out(b->rect.x + (int16_t)k_ch_cell_pad,
                             b->rect.y + (int16_t)k_ch_cell_pad,
                             s_label[i],
                             &ra8_gfx_font_8x16,
                             fg,
                             bg);
    }
  }
}

/** @brief FNV-1a-32 over the rendered framebuffer bytes. */
static uint32_t ch_framebuffer_hash(void)
{
  const uint8_t* p   = (const uint8_t*)s_framebuffer;
  const size_t   n   = sizeof(s_framebuffer);
  uint32_t       hsh = (uint32_t)k_ch_fnv_offset;
  for (size_t i = 0U; i < n; i++) {
    hsh = (hsh ^ (uint32_t)p[i]) * (uint32_t)k_ch_fnv_prime;
  }
  return hsh;
}

/* ===========================================================================
 * Boot + main
 * ===========================================================================
 */

/** @brief Bring up clocks/MSTP/time + the SCI8 console; halt on failure. */
static void ch_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    ch_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    ch_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    ch_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_ch_uart_baud) != k_ra8_ok) {
    ch_panic_halt();
  }
}

/**
 * @brief App entry: render the chrome to SRAM, hash it, print over UART.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The framebuffer hash banner is emitted; the CPU then loops in WFI.
 * @since 0.1.0
 */
void main(void)
{
  ch_setup_or_halt();
  ra8_isr_globals_enable();
  ch_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  if (ra8_gfx_init(s_framebuffer,
                   (uint16_t)k_ch_fb_w,
                   (uint16_t)k_ch_fb_h,
                   k_ra8_gfx_format_rgb565) != k_ra8_ok) {
    ch_panic_halt();
  }
  ra8_box_tree_t      tree  = {};
  const int16_t       root  = ch_build_chrome(&tree);
  const ra8_ui_rect_t frame = {.x = 0, .y = 0, .w = (int32_t)k_ch_fb_w, .h = (int32_t)k_ch_fb_h};
  (void)ra8_box_layout(&tree, root, &frame);
  ch_render(&tree);

  ch_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  ch_print_uint((uint32_t)tree.count);
  ch_print(k_msg_crc, (uint32_t)sizeof(k_msg_crc) - 1U);
  ch_print_hex(ch_framebuffer_hash());
  ch_print(k_msg_eol, (uint32_t)sizeof(k_msg_eol) - 1U);

  while (1) {
    __asm__ volatile("wfi");
  }
}
