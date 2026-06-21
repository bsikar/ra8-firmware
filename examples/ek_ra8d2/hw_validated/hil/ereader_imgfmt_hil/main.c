/**
 * @file examples/ek_ra8d2/hw_validated/hil/ereader_imgfmt_hil/main.c
 * @brief On-silicon HIL: BMP + GIF decode format-coverage gate (#143).
 *
 * @details
 * Closes the stb_image format-coverage gap: the firmware links four decoders
 * (`STBI_ONLY_JPEG/PNG/GIF/BMP` in `stb_image_impl.c`), but only PNG
 * (`ereader_image_hil`, #106) and JPEG (`ereader_jpeg_hil`) had a gate. This
 * app exercises the remaining two -- **BMP** and **GIF** -- end to end through
 * the same zero-heap `ra_img_decode_blit` pipeline, so any drift in those
 * decoders trips a gate.
 *
 *   1. `ra_gfx_init` -- bind a 160x120 RGB565 framebuffer.
 *   2. For each format: clear the framebuffer, `ra_img_decode_blit` the baked
 *      image (decode + aspect-preserving scale-to-fit + blit, allocating only
 *      from a fixed 128 KiB SRAM bump arena -- no `malloc`, NASA Rule 3), then
 *      FNV-1a-32 hash the framebuffer.
 *
 * The console banner on success is:
 *
 *   `ereader-imgfmt-hil: bmp=<8hex> gif=<8hex> PASS`
 *
 * Deterministic (fixed blobs through deterministic decoders + scaler), so the
 * banner is identical on host, board_sim, and silicon. A decode failure prints
 * a FAIL banner and halts on a BKPT before the PASS line, so the gate is exact.
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

#include "imgfmt_fixtures.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gfx.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_reflow_image.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @enum ef_consts_t @brief Console / render knobs (no magic numbers). */
typedef enum : uint32_t {
  k_ef_uart_chan   = 8U,           /**< SCI8 J-Link OB console.            */
  k_ef_uart_baud   = 115200U,      /**< Console baud.                      */
  k_ef_fb_w        = 160U,         /**< Framebuffer width, pixels.         */
  k_ef_fb_h        = 120U,         /**< Framebuffer height, pixels.        */
  k_ef_arena_bytes = 128U * 1024U, /**< Image decode scratch, bytes.       */
  k_ef_col_bg      = 0x202028U,    /**< Framebuffer clear colour.          */
  k_ef_fnv_offset  = 0x811C9DC5U,  /**< FNV-1a 32-bit offset basis.        */
  k_ef_fnv_prime   = 0x01000193U,  /**< FNV-1a 32-bit prime.               */
  k_ef_hex_nibbles = 8U,           /**< Hex digits in a 32-bit value.      */
  k_ef_nibble_bits = 4U,           /**< Bits per hex nibble.               */
  k_ef_nibble_mask = 0x0FU,        /**< Low-nibble mask.                   */
  k_ef_dec_ten     = 10U,          /**< Hex digit / decimal split.         */
} ef_consts_t;

/** @brief SCI8 console TXD = PD02. */
static const ra_port_pin_t k_ef_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
/** @brief SCI8 console RXD = PD03. */
static const ra_port_pin_t k_ef_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief RGB565 framebuffer each image is rendered into. */
static uint16_t s_framebuffer[(size_t)k_ef_fb_h * (size_t)k_ef_fb_w];
/** @brief Fixed bump arena backing the heap-free image decode. */
static uint8_t s_img_arena[k_ef_arena_bytes];

static const uint8_t k_msg_boot[] = "ereader-imgfmt-hil: boot\r\n";
static const uint8_t k_msg_fail[] = "ereader-imgfmt-hil: FAIL init\r\n";
static const uint8_t k_msg_dbmp[] = "ereader-imgfmt-hil: FAIL bmp\r\n";
static const uint8_t k_msg_dgif[] = "ereader-imgfmt-hil: FAIL gif\r\n";
static const uint8_t k_msg_pre[]  = "ereader-imgfmt-hil: bmp=";
static const uint8_t k_msg_gif[]  = " gif=";
static const uint8_t k_msg_ok[]   = " PASS\r\n";

/** @brief Emit a byte run on the SCI8 console. */
static void ef_print(const uint8_t* msg, uint32_t len)
{
  (void)ra_sci_write_polling((uint8_t)k_ef_uart_chan, msg, len);
}

/** @brief Print the fail banner and trap (board_sim halts on the BKPT). */
static void ef_panic_halt(const uint8_t* msg, uint32_t len)
{
  ef_print(msg, len);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief FNV-1a hash over the rendered framebuffer. */
static uint32_t ef_framebuffer_hash(void)
{
  const uint8_t* p   = (const uint8_t*)s_framebuffer;
  const size_t   n   = sizeof(s_framebuffer);
  uint32_t       hsh = (uint32_t)k_ef_fnv_offset;
  for (size_t i = 0U; i < n; i++) {
    hsh = (hsh ^ (uint32_t)p[i]) * (uint32_t)k_ef_fnv_prime;
  }
  return hsh;
}

/** @brief Print a 32-bit value as 8 upper-case hex digits. */
static void ef_print_hex(uint32_t value)
{
  uint8_t buf[k_ef_hex_nibbles];
  for (uint32_t i = 0U; i < (uint32_t)k_ef_hex_nibbles; i++) {
    const uint32_t shift = ((uint32_t)k_ef_hex_nibbles - 1U - i) * (uint32_t)k_ef_nibble_bits;
    const uint32_t nib   = (value >> shift) & (uint32_t)k_ef_nibble_mask;
    buf[i] = (uint8_t)((nib < (uint32_t)k_ef_dec_ten) ? ('0' + nib) : ('A' + (nib - k_ef_dec_ten)));
  }
  ef_print(buf, (uint32_t)k_ef_hex_nibbles);
}

/** @brief Route the SCI8 console pins to async mode. */
[[nodiscard]] static ra_err_t ef_console_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_ef_pin_txd, k_ra_psel_sci_async, "imgfmt.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_ef_pin_rxd, k_ra_psel_sci_async, "imgfmt.rxd8");
}

/** @brief Bring up clocks/MSTP/time + the SCI8 console; halt on failure. */
static void ef_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if ((ra_cgc_init() != k_ra_ok) || (ra_mstp_init() != k_ra_ok)) {
    ef_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if ((ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) ||
      (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok)) {
    ef_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    ef_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ef_console_pins_init() != k_ra_ok) {
    ef_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  const ra_sci_cfg_t cfg = {.baud      = (uint32_t)k_ef_uart_baud,
                            .data_bits = k_ra_sci_data_8,
                            .parity    = k_ra_sci_parity_none,
                            .stop_bits = k_ra_sci_stop_1,
                            .pclk_hz   = pclka_hz};
  if (ra_sci_init((uint8_t)k_ef_uart_chan, &cfg) != k_ra_ok) {
    ef_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
}

/**
 * @brief Clear the framebuffer, decode+scale+blit one image, hash the result.
 *
 * @details Halts on decode failure before returning.
 *
 * @param[in] bytes    Encoded image bytes (BMP or GIF).
 * @param[in] len      Length of @p bytes.
 * @param[in] fail_msg Banner emitted (then BKPT) if the decode fails.
 * @param[in] fail_len Length of @p fail_msg.
 * @return The FNV-1a hash of the framebuffer after the blit.
 *
 * @pre ::ra_gfx_init bound ::s_framebuffer.
 * @post ::s_framebuffer holds the scaled image; the arena is reset to empty.
 * @since 0.1.0
 */
static uint32_t
ef_decode_and_hash(const uint8_t* bytes, size_t len, const uint8_t* fail_msg, uint32_t fail_len)
{
  (void)ra_gfx_clear((uint32_t)k_ef_col_bg);
  ra_img_arena_t arena = {.base   = s_img_arena,
                          .cap    = (size_t)k_ef_arena_bytes,
                          .offset = 0U,
                          .live   = 0U};
  int32_t        out_w = 0;
  int32_t        out_h = 0;
  if (ra_img_decode_blit(&arena,
                         bytes,
                         len,
                         0,
                         0,
                         (int32_t)k_ef_fb_w,
                         (int32_t)k_ef_fb_h,
                         &out_w,
                         &out_h) != k_ra_ok) {
    ef_panic_halt(fail_msg, fail_len);
  }
  return ef_framebuffer_hash();
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: decode the baked BMP + GIF on silicon, print the banner.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The bmp/gif framebuffer-CRC banner is emitted; CPU loops in WFI.
 * @since 0.1.0
 */
int32_t main(void)
{
  ef_setup_or_halt();
  ra_isr_globals_enable();
  ef_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  if (ra_gfx_init(s_framebuffer,
                  (uint16_t)k_ef_fb_w,
                  (uint16_t)k_ef_fb_h,
                  k_ra_gfx_format_rgb565) != k_ra_ok) {
    ef_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }

  const uint32_t bmp_crc =
    ef_decode_and_hash(k_bmp, (size_t)k_bmp_len, k_msg_dbmp, (uint32_t)sizeof(k_msg_dbmp) - 1U);
  const uint32_t gif_crc =
    ef_decode_and_hash(k_gif, (size_t)k_gif_len, k_msg_dgif, (uint32_t)sizeof(k_msg_dgif) - 1U);

  ef_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  ef_print_hex(bmp_crc);
  ef_print(k_msg_gif, (uint32_t)sizeof(k_msg_gif) - 1U);
  ef_print_hex(gif_crc);
  ef_print(k_msg_ok, (uint32_t)sizeof(k_msg_ok) - 1U);

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
