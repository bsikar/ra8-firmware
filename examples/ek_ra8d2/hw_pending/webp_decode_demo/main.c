/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_pending/webp_decode_demo/main.c
 * @brief On-target WebP decode smoke test (ra8_webp over vendored libwebp).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Stand-alone EK-RA8D2 application that proves the vendored libwebp decoder
 * (``libs/third_party/libwebp``) and the ``ra8_webp`` facade cross-compile,
 * link, and run on the Cortex-M85 -- decoding with zero heap through the
 * ``ra8_webp`` bump arena (``_sbrk`` traps after init).
 *
 * A tiny 8x8 lossless (VP8L) WebP is baked into the image (::k_webp_lossless,
 * the same fixture as ``tests/fixtures/webp/fixture_lossless.webp``). Because it
 * is lossless with ``-exact``, its RGB is preserved bit-exact, so a known pixel
 * can be checked after decode.
 *
 * Flow:
 *   1. ``ra8_cgc_init`` / ``ra8_mstp_init`` / ``ra8_time_init`` bring the clocks
 *      and SysTick up.
 *   2. ``ra8_board_uart_console_init`` opens the J-Link OB VCOM console (SCI8).
 *   3. Bind a static SRAM arena, decode the baked WebP into a static RGBA8888
 *      framebuffer, and verify pixel (1,1) == (32, 32, 32, 255).
 *   4. Print ``"webp: PASS 8x8\r\n"`` on success (banner line for the HIL
 *      scrape) or ``"webp: FAIL\r\n"`` on any mismatch, then park.
 *
 * NOTE(#289): this decodes a whole WebP into one RGBA buffer. The band-tile
 * normalisation + ra8_reflow raster dispatch land with #289.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_time.h"
#include "ra8_webp.h"
#include "ra8_webp_arena.h"

/**
 * @enum webp_demo_consts_t
 * @brief Fixed geometry + buffer sizes for the baked 8x8 fixture.
 */
typedef enum : uint32_t {
  k_webp_demo_baud     = 115200U,                              /**< Console baud rate.         */
  k_webp_demo_dim      = 8U,                                   /**< Fixture width and height.  */
  k_webp_demo_bpp      = 4U,                                   /**< RGBA8888 bytes per pixel.  */
  k_webp_demo_stride   = k_webp_demo_dim * k_webp_demo_bpp,    /**< Row stride.                */
  k_webp_demo_fb_bytes = k_webp_demo_dim * k_webp_demo_stride, /**< 8x8 RGBA.                  */
  k_webp_demo_scratch  = 256U * 1024U,                         /**< Decode arena, bytes.       */
  k_webp_demo_probe_r  = 32U,                                  /**< Expected R at pixel (1,1). */
  k_webp_demo_probe_a  = 255U,                                 /**< Expected A at pixel (1,1). */
} webp_demo_consts_t;

/** 8x8 VP8L (lossless, -exact) fixture -- see tests/fixtures/webp/README.md. */
static const uint8_t k_webp_lossless[] = {
  0x52, 0x49, 0x46, 0x46, 0x2C, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56,
  0x50, 0x38, 0x4C, 0x1F, 0x00, 0x00, 0x00, 0x2F, 0x07, 0xC0, 0x01, 0x00, 0xCD,
  0x65, 0x44, 0xFF, 0x63, 0x17, 0x85, 0x28, 0x78, 0xFF, 0x03, 0x42, 0x02, 0xC2,
  0x14, 0xFF, 0x77, 0x6A, 0x0E, 0x0C, 0x48, 0xC4, 0x04, 0x80, 0xAD, 0x0D, 0x00,
};

/** PASS banner (HIL scrape target). */
static const uint8_t k_webp_demo_pass[] = "webp: PASS 8x8\r\n";
/** FAIL banner (HIL scrape target). */
static const uint8_t k_webp_demo_fail[] = "webp: FAIL\r\n";

/** Decode arena backing store (static SRAM, NASA P10 Rule 3). */
alignas(16) static uint8_t s_webp_demo_scratch[k_webp_demo_scratch];
/** Decoded RGBA8888 output framebuffer. */
alignas(16) static uint8_t s_webp_demo_fb[k_webp_demo_fb_bytes];

/**
 * @brief Park the CPU forever after a fatal init failure.
 * @pre Called only after an unrecoverable setup error.
 * @post CPU is parked; only a debugger or reset wakes it.
 * @since 0.1.0
 */
static void webp_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC + module-stop + SysTick + console up, or panic-halt.
 * @pre Reset default clock tree; MSTP gating cleared for nothing yet.
 * @post CGC live, SysTick running, and the console is ready for writes.
 * @since 0.1.0
 */
static void webp_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    webp_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    webp_demo_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    webp_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    webp_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_webp_demo_baud) != k_ra8_ok) {
    webp_demo_panic_halt();
  }
}

/**
 * @brief Decode the baked fixture and confirm a known pixel round-trips.
 * @return true if the decode succeeded and pixel (1,1) matches the source.
 * @pre The console + clocks are up (webp_demo_setup_or_halt ran).
 * @post @c s_webp_demo_fb holds the decoded canvas on success.
 * @since 0.1.0
 */
static bool webp_demo_decode_ok(void)
{
  ra8_webp_arena_t arena = {.base   = s_webp_demo_scratch,
                            .cap    = sizeof s_webp_demo_scratch,
                            .offset = 0U,
                            .live   = 0U};
  uint32_t         w     = 0U;
  uint32_t         h     = 0U;
  const ra8_err_t  e     = ra8_webp_decode_rgba(k_webp_lossless,
                                                sizeof k_webp_lossless,
                                                &arena,
                                                s_webp_demo_fb,
                                                (size_t)k_webp_demo_stride,
                                                sizeof s_webp_demo_fb,
                                                &w,
                                                &h);
  if ((e != k_ra8_ok) || (w != k_webp_demo_dim) || (h != k_webp_demo_dim)) {
    return false;
  }
  /* Pixel (1,1) of the source pattern is (r,g,b,a) = (32, 32, 32, 255). */
  const uint8_t* const p = &s_webp_demo_fb[k_webp_demo_stride + k_webp_demo_bpp];
  return (p[0] == (uint8_t)k_webp_demo_probe_r) && (p[1] == (uint8_t)k_webp_demo_probe_r) &&
         (p[2] == (uint8_t)k_webp_demo_probe_r) && (p[3] == (uint8_t)k_webp_demo_probe_a);
}

/**
 * @brief Application entry point.
 * @return Never returns (parks in a WFI loop after emitting the banner).
 * @since 0.1.0
 */
int main(void)
{
  webp_demo_setup_or_halt();

  const bool           ok     = webp_demo_decode_ok();
  const uint8_t* const banner = ok ? k_webp_demo_pass : k_webp_demo_fail;
  const size_t ban_len = ok ? (sizeof k_webp_demo_pass - 1U) : (sizeof k_webp_demo_fail - 1U);
  (void)ra8_board_uart_console_write(banner, ban_len);

  webp_demo_panic_halt();
  return 0;
}
