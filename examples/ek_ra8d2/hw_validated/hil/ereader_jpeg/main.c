/**
 * @file examples/ek_ra8d2/hw_validated/hil/ereader_jpeg/main.c
 * @brief Headless on-silicon HIL gate for the raster image decode pipeline (#143).
 *
 * @details
 * Closes the *real-hardware* gap for the zero-heap `<img>` / cover-image
 * pipeline (ra8_reflow_image): decode -> nearest-neighbour scale-to-fit -> blit,
 * with no panel / SDRAM / touch / SD dependency. The app:
 *
 *   1. Decodes a baked 120x90 JPEG cover (jpeg_fixture.h) through
 *      ra8_img_decode_blit(), allocating only from a fixed SRAM bump arena
 *      (so the decode reaches no `malloc`).
 *   2. Scales it to fit a fixed 160x120 RGB565 framebuffer in internal SRAM.
 *   3. Folds an FNV-1a-32 hash over the whole framebuffer and prints it on the
 *      SCI8 J-Link OB console:
 *
 *        `ereader-jpeg-hil: img <W>x<H> crc=<8 hex>`
 *
 * The render is deterministic (integer nearest-neighbour + fixed RGB565 pack +
 * a zeroed static framebuffer), so the hash is identical every boot and -- by
 * construction -- the same value the identical host / ra8_emulator render
 * produces. Any drift in the decoder, the scale math, or the toolchain output
 * changes the hash and trips the `uart_scrape` gate.
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

#include "jpeg_fixture.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_reflow_image.h"
#include "ra8_time.h"

/* ===========================================================================
 * Sizing + protocol constants (no magic numbers)
 * ===========================================================================
 */

/** @enum iv_consts_t @brief Framebuffer / console / hash knobs. */
typedef enum : uint32_t {
  k_iv_fb_w        = 160U,         /**< Framebuffer width, pixels.    */
  k_iv_fb_h        = 120U,         /**< Framebuffer height, pixels.   */
  k_iv_uart_baud   = 115200U,      /**< Console baud.                 */
  k_iv_arena_bytes = 128U * 1024U, /**< JPEG decode scratch, bytes.   */
  k_iv_fnv_offset  = 2166136261U,  /**< FNV-1a-32 offset basis.       */
  k_iv_fnv_prime   = 16777619U,    /**< FNV-1a-32 prime.              */
  k_iv_hex_nibbles = 8U,           /**< Hex digits in a 32-bit value. */
  k_iv_nibble_bits = 4U,           /**< Bits per hex nibble.          */
  k_iv_nibble_mask = 0x0FU,        /**< Low-nibble mask.              */
  k_iv_dec_ten     = 10U,          /**< Hex digit / decimal split.    */
} iv_consts_t;

/** @enum iv_colors_t @brief 0x00RRGGBB palette. */
typedef enum : uint32_t {
  k_iv_col_bg = 0x202028U, /**< Framebuffer clear colour (dark slate). */
} iv_colors_t;

/** @brief Fixed RGB565 framebuffer in internal SRAM (no panel attached). */
static uint16_t s_framebuffer[(size_t)k_iv_fb_h * (size_t)k_iv_fb_w];

/** @brief Fixed bump arena backing the heap-free JPEG decode. */
static uint8_t s_img_arena[k_iv_arena_bytes];

static const uint8_t k_msg_boot[] = "ereader-jpeg-hil: boot\r\n";
static const uint8_t k_msg_fail[] = "ereader-jpeg-hil: FAIL init\r\n";
static const uint8_t k_msg_derr[] = "ereader-jpeg-hil: FAIL decode\r\n";
static const uint8_t k_msg_pre[]  = "ereader-jpeg-hil: img ";
static const uint8_t k_msg_x[]    = "x";
static const uint8_t k_msg_crc[]  = " crc=";
static const uint8_t k_msg_eol[]  = "\r\n";

/* ===========================================================================
 * Console helpers
 * ===========================================================================
 */

/** @brief Emit a byte run on the SCI8 console. */
static void iv_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Print the init-fail banner and spin (ra8_emulator halts on the BKPT). */
static void iv_panic_halt(void)
{
  iv_print(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Print a 32-bit value as 8 upper-case hex digits. */
static void iv_print_hex(uint32_t value)
{
  uint8_t buf[k_iv_hex_nibbles];
  for (uint32_t i = 0U; i < (uint32_t)k_iv_hex_nibbles; i++) {
    const uint32_t shift = ((uint32_t)k_iv_hex_nibbles - 1U - i) * (uint32_t)k_iv_nibble_bits;
    const uint32_t nib   = (value >> shift) & (uint32_t)k_iv_nibble_mask;
    buf[i] = (uint8_t)((nib < (uint32_t)k_iv_dec_ten) ? ('0' + nib) : ('A' + (nib - k_iv_dec_ten)));
  }
  iv_print(buf, (uint32_t)k_iv_hex_nibbles);
}

/** @brief Print a small unsigned integer in decimal. */
static void iv_print_uint(uint32_t value)
{
  uint8_t  buf[k_iv_dec_ten];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_iv_dec_ten)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_iv_dec_ten));
    n++;
    value /= (uint32_t)k_iv_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    iv_print(&buf[n - 1U - i], 1U);
  }
}

/* ===========================================================================
 * Render + hash
 * ===========================================================================
 */

/** @brief FNV-1a-32 over the rendered framebuffer bytes. */
static uint32_t iv_framebuffer_hash(void)
{
  const uint8_t* p   = (const uint8_t*)s_framebuffer;
  const size_t   n   = sizeof(s_framebuffer);
  uint32_t       hsh = (uint32_t)k_iv_fnv_offset;
  for (size_t i = 0U; i < n; i++) {
    hsh = (hsh ^ (uint32_t)p[i]) * (uint32_t)k_iv_fnv_prime;
  }
  return hsh;
}

/* ===========================================================================
 * Boot + main
 * ===========================================================================
 */

/** @brief Bring up clocks/MSTP/time + the SCI8 console; halt on failure. */
static void iv_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    iv_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    iv_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    iv_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_iv_uart_baud) != k_ra8_ok) {
    iv_panic_halt();
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: decode + scale + blit the cover to SRAM, hash it, print.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The framebuffer hash banner is emitted; the CPU then loops in WFI.
 * @since 0.1.0
 */
int32_t main(void)
{
  iv_setup_or_halt();
  ra8_isr_globals_enable();
  iv_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  if (ra8_gfx_init(s_framebuffer,
                   (uint16_t)k_iv_fb_w,
                   (uint16_t)k_iv_fb_h,
                   k_ra8_gfx_format_rgb565) != k_ra8_ok) {
    iv_panic_halt();
  }
  (void)ra8_gfx_clear((uint32_t)k_iv_col_bg);

  ra8_img_arena_t arena = {.base   = s_img_arena,
                           .cap    = (size_t)k_iv_arena_bytes,
                           .offset = 0U,
                           .live   = 0U};
  int32_t         out_w = 0;
  int32_t         out_h = 0;
  const ra8_err_t err   = ra8_img_decode_blit(&arena,
                                              k_cover_jpg,
                                              (size_t)k_cover_jpg_len,
                                              0,
                                              0,
                                              (int32_t)k_iv_fb_w,
                                              (int32_t)k_iv_fb_h,
                                              &out_w,
                                              &out_h);
  if (err != k_ra8_ok) {
    iv_print(k_msg_derr, (uint32_t)sizeof(k_msg_derr) - 1U);
    iv_panic_halt();
  }

  iv_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  iv_print_uint((uint32_t)out_w);
  iv_print(k_msg_x, (uint32_t)sizeof(k_msg_x) - 1U);
  iv_print_uint((uint32_t)out_h);
  iv_print(k_msg_crc, (uint32_t)sizeof(k_msg_crc) - 1U);
  iv_print_hex(iv_framebuffer_hash());
  iv_print(k_msg_eol, (uint32_t)sizeof(k_msg_eol) - 1U);

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
