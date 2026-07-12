/**
 * @file examples/ek_ra8d2/hw_pending/ereader_comic/main.c
 * @brief Headless gate for the comic-archive reader: open a CBZ + a CBR, decode
 *        page 0 of each, and print a framebuffer CRC (board_sim / sim-only).
 *
 * @details
 * The end-to-end proof of `ra8_comic` on the real cross-compiled target build:
 * open a baked `.cbz` (ZIP of images) and a baked `.cbr` (RAR of images) through
 * `ra8_comic_open` over a resident-buffer seek+read callback, extract page 0's
 * encoded image on demand, and decode + scale-blit it into a 160x120 RGB565
 * framebuffer via the zero-heap `ra8_img_decode_blit` pipeline. An FNV-1a-32 over
 * the framebuffer is printed on the SCI8 J-Link OB console:
 *
 *   `ereader-comic-hil: cbz pages=<N> <W>x<H> crc=<8hex>`
 *   ` cbr pages=<M> <W>x<H> crc=<8hex> ok`
 *
 * Both fixtures carry the same first page image, so the two CRCs are identical --
 * proving the CBZ (miniz ZIP) and CBR (clean-room RAR) backends extract byte-for-
 * byte the same page and the reader renders both containers interchangeably. The
 * render is deterministic (integer nearest-neighbour + fixed RGB565 pack + zeroed
 * framebuffer), so the banner is identical on host, board_sim, and silicon.
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
#include <string.h>

#include "comic_hil_fixture.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_comic.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_reflow_image.h"
#include "ra8_time.h"

/** @enum cm_consts_t @brief Framebuffer / console / reader / hash knobs. */
typedef enum : uint32_t {
  k_cm_fb_w        = 160U,         /**< Framebuffer width, pixels.    */
  k_cm_fb_h        = 120U,         /**< Framebuffer height, pixels.   */
  k_cm_uart_baud   = 115200U,      /**< Console baud.                 */
  k_cm_arena_bytes = 128U * 1024U, /**< PNG decode scratch, bytes.    */
  k_cm_page_cap    = 8U,           /**< Comic page-index capacity.    */
  k_cm_name_cap    = 256U,         /**< Comic name-arena bytes.       */
  k_cm_pagebuf     = 16U * 1024U,  /**< Encoded page-image scratch.   */
  k_cm_fnv_offset  = 2166136261U,  /**< FNV-1a-32 offset basis.       */
  k_cm_fnv_prime   = 16777619U,    /**< FNV-1a-32 prime.              */
  k_cm_hex_nibbles = 8U,           /**< Hex digits in a 32-bit value. */
  k_cm_nibble_bits = 4U,           /**< Bits per hex nibble.          */
  k_cm_nibble_mask = 0x0FU,        /**< Low-nibble mask.              */
  k_cm_dec_ten     = 10U,          /**< Hex digit / decimal split.    */
} cm_consts_t;

/** @enum cm_colors_t @brief 0x00RRGGBB palette. */
typedef enum : uint32_t {
  k_cm_col_bg = 0x202028U, /**< Framebuffer clear colour (dark slate). */
} cm_colors_t;

/** @brief Fixed RGB565 framebuffer in internal SRAM (no panel attached). */
static uint16_t s_framebuffer[(size_t)k_cm_fb_h * (size_t)k_cm_fb_w];
/** @brief Fixed bump arena backing the heap-free page decode. */
static uint8_t s_img_arena[k_cm_arena_bytes];
/** @brief The open comic reader (large -- file-scope, not on the stack). */
static ra8_comic_t s_comic;
/** @brief Caller page index for the reader. */
static ra8_comic_page_t s_pages[k_cm_page_cap];
/** @brief Caller name arena for the reader. */
static char s_names[k_cm_name_cap];
/** @brief One page's extracted encoded image bytes. */
static uint8_t s_pagebuf[k_cm_pagebuf];

static const uint8_t k_msg_boot[] = "ereader-comic-hil: boot\r\n";
static const uint8_t k_msg_fail[] = "ereader-comic-hil: FAIL init\r\n";
static const uint8_t k_msg_cerr[] = "ereader-comic-hil: FAIL comic\r\n";
static const uint8_t k_msg_pre[]  = "ereader-comic-hil:";
static const uint8_t k_msg_cbz[]  = " cbz pages=";
static const uint8_t k_msg_cbr[]  = " cbr pages=";
static const uint8_t k_msg_x[]    = "x";
static const uint8_t k_msg_dim[]  = " ";
static const uint8_t k_msg_crc[]  = " crc=";
static const uint8_t k_msg_ok[]   = " ok\r\n";

/**
 * @struct cm_src_t
 * @brief Resident-buffer backing for the reader's seek+read callback.
 */
typedef struct {
  const uint8_t* d; /**< Archive bytes.  */
  size_t         n; /**< Archive length. */
} cm_src_t;

/** @brief Emit a byte run on the SCI8 console. */
static void cm_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Print the fail banner and trap (board_sim halts on the BKPT). */
static void cm_panic_halt(const uint8_t* msg, uint32_t len)
{
  cm_print(msg, len);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Print a 32-bit value as 8 upper-case hex digits. */
static void cm_print_hex(uint32_t value)
{
  uint8_t buf[k_cm_hex_nibbles];
  for (uint32_t i = 0U; i < (uint32_t)k_cm_hex_nibbles; i++) {
    const uint32_t shift = ((uint32_t)k_cm_hex_nibbles - 1U - i) * (uint32_t)k_cm_nibble_bits;
    const uint32_t nib   = (value >> shift) & (uint32_t)k_cm_nibble_mask;
    buf[i] = (uint8_t)((nib < (uint32_t)k_cm_dec_ten) ? ('0' + nib) : ('A' + (nib - k_cm_dec_ten)));
  }
  cm_print(buf, (uint32_t)k_cm_hex_nibbles);
}

/** @brief Print a small unsigned integer in decimal. */
static void cm_print_uint(uint32_t value)
{
  uint8_t  buf[k_cm_dec_ten];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_cm_dec_ten)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_cm_dec_ten));
    n++;
    value /= (uint32_t)k_cm_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    cm_print(&buf[n - 1U - i], 1U);
  }
}

/** @brief Read callback over a resident archive buffer. */
static size_t cm_read(void* ctx, uint64_t off, void* buf, size_t len)
{
  const cm_src_t* s = (const cm_src_t*)ctx;
  if (off >= (uint64_t)s->n) {
    return 0U;
  }
  const uint64_t avail = (uint64_t)s->n - off;
  const size_t   k     = (len > (size_t)avail) ? (size_t)avail : len;
  (void)memcpy(buf, &s->d[off], k);
  return k;
}

/** @brief FNV-1a-32 over the rendered framebuffer bytes. */
static uint32_t cm_framebuffer_hash(void)
{
  const uint8_t* p   = (const uint8_t*)s_framebuffer;
  const size_t   n   = sizeof(s_framebuffer);
  uint32_t       hsh = (uint32_t)k_cm_fnv_offset;
  for (size_t i = 0U; i < n; i++) {
    hsh = (hsh ^ (uint32_t)p[i]) * (uint32_t)k_cm_fnv_prime;
  }
  return hsh;
}

/**
 * @brief Open one comic, decode page 0 to the framebuffer, report geometry + hash.
 * @param[in]  data     Archive bytes.
 * @param[in]  len      Archive length.
 * @param[out] pages    Receives the page count.
 * @param[out] out_w    Receives the blitted width.
 * @param[out] out_h    Receives the blitted height.
 * @param[out] out_hash Receives the framebuffer FNV hash.
 * @return Whether the open + decode succeeded.
 * @retval true  Page 0 decoded and hashed.
 * @retval false Open, extract, or decode failed.
 * @pre @p data holds @p len readable bytes.
 * @pre The framebuffer was bound with `ra8_gfx_init`.
 * @post On true every output is populated and the framebuffer holds page 0.
 * @post The reader is closed on every path.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool cm_render(const uint8_t* data,
                      size_t         len,
                      uint32_t*      pages,
                      int32_t*       out_w,
                      int32_t*       out_h,
                      uint32_t*      out_hash)
{
  cm_src_t        src  = {.d = data, .n = len};
  const ra8_err_t oerr = ra8_comic_open(&s_comic,
                                        cm_read,
                                        &src,
                                        (uint64_t)len,
                                        s_pages,
                                        (uint32_t)k_cm_page_cap,
                                        s_names,
                                        (uint32_t)sizeof(s_names));
  if (oerr != k_ra8_ok) {
    return false;
  }
  *pages               = ra8_comic_page_count(&s_comic);
  size_t          got  = 0U;
  const ra8_err_t rerr = ra8_comic_page_read(&s_comic, 0U, s_pagebuf, sizeof(s_pagebuf), &got);
  if (rerr != k_ra8_ok) {
    (void)ra8_comic_close(&s_comic);
    return false;
  }
  (void)ra8_gfx_clear((uint32_t)k_cm_col_bg);
  ra8_img_arena_t arena = {.base   = s_img_arena,
                           .cap    = (size_t)k_cm_arena_bytes,
                           .offset = 0U,
                           .live   = 0U};
  const ra8_err_t derr  = ra8_img_decode_blit(&arena,
                                              s_pagebuf,
                                              got,
                                              0,
                                              0,
                                              (int32_t)k_cm_fb_w,
                                              (int32_t)k_cm_fb_h,
                                              out_w,
                                              out_h);
  (void)ra8_comic_close(&s_comic);
  if (derr != k_ra8_ok) {
    return false;
  }
  *out_hash = cm_framebuffer_hash();
  return true;
}

/** @brief Print one container's `pages=<N> <W>x<H> crc=<hash>` fragment. */
static void
cm_report(const uint8_t* tag, uint32_t tag_len, uint32_t pages, int32_t w, int32_t h, uint32_t hash)
{
  cm_print(tag, tag_len);
  cm_print_uint(pages);
  cm_print(k_msg_dim, (uint32_t)sizeof(k_msg_dim) - 1U);
  cm_print_uint((uint32_t)w);
  cm_print(k_msg_x, (uint32_t)sizeof(k_msg_x) - 1U);
  cm_print_uint((uint32_t)h);
  cm_print(k_msg_crc, (uint32_t)sizeof(k_msg_crc) - 1U);
  cm_print_hex(hash);
}

/** @brief Bring up clocks/MSTP/time + the SCI8 console; halt on failure. */
static void cm_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    cm_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    cm_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    cm_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_cm_uart_baud) != k_ra8_ok) {
    cm_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: open + render page 0 of the baked CBZ and CBR, print CRCs.
 * @return Never returns.
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The per-container page-count + render-hash banner is emitted; loops in WFI.
 * @since 0.1.0
 */
int32_t main(void)
{
  cm_setup_or_halt();
  ra8_isr_globals_enable();
  cm_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  if (ra8_gfx_init(s_framebuffer,
                   (uint16_t)k_cm_fb_w,
                   (uint16_t)k_cm_fb_h,
                   k_ra8_gfx_format_rgb565) != k_ra8_ok) {
    cm_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }

  uint32_t zp   = 0U;
  int32_t  zw   = 0;
  int32_t  zh   = 0;
  uint32_t zh32 = 0U;
  if (!cm_render(k_comic_cbz, (size_t)k_comic_cbz_len, &zp, &zw, &zh, &zh32)) {
    cm_panic_halt(k_msg_cerr, (uint32_t)sizeof(k_msg_cerr) - 1U);
  }
  uint32_t rp   = 0U;
  int32_t  rw   = 0;
  int32_t  rh   = 0;
  uint32_t rh32 = 0U;
  if (!cm_render(k_comic_cbr, (size_t)k_comic_cbr_len, &rp, &rw, &rh, &rh32)) {
    cm_panic_halt(k_msg_cerr, (uint32_t)sizeof(k_msg_cerr) - 1U);
  }

  cm_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  cm_report(k_msg_cbz, (uint32_t)sizeof(k_msg_cbz) - 1U, zp, zw, zh, zh32);
  cm_report(k_msg_cbr, (uint32_t)sizeof(k_msg_cbr) - 1U, rp, rw, rh, rh32);
  cm_print(k_msg_ok, (uint32_t)sizeof(k_msg_ok) - 1U);

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
