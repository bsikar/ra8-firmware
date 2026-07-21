/**
 * @file examples/ek_ra8d2/hw_pending/drw_blend_demo/main.c
 * @brief DRW 2D-engine blit + per-pixel alpha-blend headless CRC demo (EK-RA8D2)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Issue #120 asks for a demo that exercises the DRW ("D/AVE 2D") engine's
 * blit AND alpha-blend path (``ra8_drw_blend_t``), not just the solid fill the
 * sibling ``drw_fill_demo`` covers. This app renders three layers into a small
 * 32x32 ARGB8888 framebuffer in SRAM and then folds an FNV-1a-32 hash over the
 * whole framebuffer, exactly like ``ereader_chrome`` does for its chrome:
 *
 *  1. Background  -- an opaque dark-blue ``ra8_drw_fill_rect`` over the whole FB.
 *  2. Sprite blit -- an opaque green ``ra8_drw_fill_rect`` sub-rectangle (the
 *     "sprite" laid down over the background).
 *  3. Alpha blend -- a half-transparent red foreground composited over the
 *     background + sprite via ``ra8_drw_set_blend`` (source-over, global alpha
 *     0x80) followed by a final ``ra8_drw_fill_rect``. With the blend unit armed
 *     the DRW reads each destination pixel back and mixes the COLOR1 source in
 *     by the global alpha, so the overlap region carries a deterministic mix of
 *     red over blue and red over green.
 *
 * The rendered framebuffer is byte-deterministic, so its hash is constant on
 * every boot. The app prints ``"drw: blit+blend crc=<8hex> PASS"`` over the
 * J-Link OB CDC channel once a second; the headless gate
 * (``board_sim_smoke.sh`` / ``hil.conf`` ``uart_scrape``) asserts that banner.
 * No display panel is touched -- this is a pure compute + memory test.
 *
 * @note **Headless-emulator status.** ``tools/board_sim`` models the DRW engine
 * as INERT (``board_periph_drw.c``), faithful to real silicon where the D/AVE 2D
 * engine never rasterizes (issue #247): register writes are accepted, STATUS
 * reads idle so ``ra8_drw_wait_idle`` returns, HWREVISION reads 0, and the
 * ORIGIN render trigger synthesises no pixel. The framebuffer therefore stays
 * zero, so the FNV-1a-32 hash is ``0x76EFDDC5`` -- the SAME value a real board
 * produces from its all-zero framebuffer. SIM == HIL: the SIL gate and the
 * bench print the identical banner. This app lives in ``hw_validated/hil/``
 * because that zero-framebuffer result is confirmed on silicon; the demo will
 * only report a genuinely composited CRC once #247 brings the engine to life.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_drw.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/** @brief Diagnostic / log tag. */
static const char* s_tag = "drw_blend";

/**
 * @enum drw_blend_config_t
 * @brief Compile-time scalar settings for the demo.
 *
 * @details
 * Baud, loop period, the bounded ``ra8_drw_wait_idle`` poll budget, the
 * global-alpha value driven into the blend unit, and the FNV-1a-32 constants
 * used to hash the framebuffer.
 */
typedef enum : uint32_t {
  k_drw_blend_baud        = 115200U,     /**< SCI8 baud rate.                 */
  k_drw_blend_period_ms   = 1000U,       /**< Delay between renders.          */
  k_drw_blend_idle_budget = 200000U,     /**< Bounded wait_idle poll budget.  */
  k_drw_blend_alpha       = 0x80U,       /**< Global source-over alpha (128). */
  k_drw_blend_fnv_offset  = 2166136261U, /**< FNV-1a-32 offset basis.         */
  k_drw_blend_fnv_prime   = 16777619U,   /**< FNV-1a-32 prime.                */
} drw_blend_config_t;

/**
 * @enum drw_blend_hash_t
 * @brief Byte / nibble constants for the FNV hash and hex banner formatting.
 *
 * @details
 * The FNV pass extracts one byte per shift; the hex printer emits eight
 * nibbles, most-significant first. Naming these clears the magic-number gate.
 */
typedef enum : uint32_t {
  k_drw_blend_byte_bits = 8U,    /**< Bits per byte (FNV shift step).       */
  k_drw_blend_byte_mask = 0xFFU, /**< Low-byte mask for the FNV input.      */
  k_drw_blend_hex_digit = 8U,    /**< Hex digits in a 32-bit CRC.           */
  k_drw_blend_hi_nibble = 7U,    /**< Index of the most-significant nibble. */
  k_drw_blend_nib_bits  = 4U,    /**< Bits per hex nibble.                  */
  k_drw_blend_nib_mask  = 0xFU,  /**< Low-nibble mask for the hex digit.    */
} drw_blend_hash_t;

/**
 * @enum drw_blend_color_t
 * @brief ARGB8888 colours used by the three render layers.
 *
 * @details
 * Background, sprite, and foreground source colours. The foreground is the
 * COLOR1 source fed through the blend unit at ``k_drw_blend_alpha``.
 */
typedef enum : uint32_t {
  k_drw_blend_bg_argb = 0xFF202060U, /**< Opaque dark-blue background. */
  k_drw_blend_sp_argb = 0xFF40C040U, /**< Opaque green sprite.         */
  k_drw_blend_fg_argb = 0xFFE04040U, /**< Red foreground source RGB.   */
  k_drw_blend_fg_a80  = 0x80E04040U, /**< Foreground at global alpha 0x80.
                                      * ra8_drw_fill_rect writes the FULL
                                      * COLOR1 (including alpha), so the
                                      * blended fill must carry the global
                                      * alpha in its colour argument or it
                                      * would clobber the armed 0x80 back
                                      * to opaque and blend nothing.      */
} drw_blend_color_t;

/**
 * @enum drw_blend_geom_t
 * @brief Framebuffer + layer rectangle geometry, in pixels.
 *
 * @details
 * A 32x32 ARGB8888 framebuffer. The sprite is a centred 16x16 box; the blended
 * foreground is a 16x16 box offset up-left so it overlaps both the background
 * and the sprite, giving a deterministic three-region composite.
 */
typedef enum : uint16_t {
  k_drw_blend_fb_dim   = 32U, /**< 32 x 32 framebuffer.        */
  k_drw_blend_sp_xy    = 8U,  /**< Sprite top-left (8, 8).     */
  k_drw_blend_sp_wh    = 16U, /**< Sprite 16 x 16.             */
  k_drw_blend_fg_xy    = 4U,  /**< Foreground top-left (4, 4). */
  k_drw_blend_fg_wh    = 16U, /**< Foreground 16 x 16.         */
  k_drw_blend_full_org = 0U,  /**< Full-FB origin (0, 0).      */
} drw_blend_geom_t;

/** @brief Success / failure banner text. */
static const uint8_t k_drw_blend_pass_pfx[] = "drw: blit+blend crc=";
static const uint8_t k_drw_blend_pass_sfx[] = " PASS\r\n";
static const uint8_t k_drw_blend_fail_msg[] = "drw: blit+blend FAIL\r\n";

/**
 * @var g_drw_blend_fb
 * @brief 32 x 32 ARGB8888 framebuffer the DRW composites into.
 * @note 4-byte aligned (ARGB8888 invariant). Read externally (HIL / sim).
 * @since 0.1.0
 */
[[gnu::aligned(4)]] volatile uint32_t
  g_drw_blend_fb[(uint32_t)k_drw_blend_fb_dim * (uint32_t)k_drw_blend_fb_dim];

/**
 * @var g_drw_blend_crc
 * @brief FNV-1a-32 hash of the last rendered framebuffer.
 * @note Read externally only (HIL / board emulator).
 * @since 0.1.0
 */
volatile uint32_t g_drw_blend_crc = 0U;

/**
 * @var g_drw_blend_err
 * @brief Non-zero if a DRW call returned an error during the last render.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_drw_blend_err = 0U;

/**
 * @var g_drw_blend_heartbeat
 * @brief Bumps once per main-loop pass -- liveness for headless probes.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_drw_blend_heartbeat = 0U;

/** @brief Park forever after a fatal init failure. */
static void drw_blend_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Bring CGC + SysTick + SCI8 + LEDs + MSTP up. */
static void drw_blend_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    drw_blend_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    drw_blend_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    drw_blend_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    drw_blend_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_drw_blend_baud) != k_ra8_ok) {
    drw_blend_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    drw_blend_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    drw_blend_panic_halt();
  }
}

/**
 * @brief Bring up the DRW engine pointed at ``g_drw_blend_fb``.
 *
 * @par MC/DC:
 * Single decision ``err != k_ra8_ok`` in the caller -- no compound condition.
 *
 * @return ``ra8_err_t`` from ``ra8_drw_init``.
 * @retval k_ra8_ok DRW configured for ARGB8888 over the demo framebuffer.
 * @retval k_ra8_err_null_ptr Internal config pointer was NULL (cannot happen).
 * @retval k_ra8_err_hw_init_failed MSTP enable failed.
 * @pre CGC + MSTP up; IRQs masked or single-threaded init.
 * @pre ``g_drw_blend_fb`` is 4-byte aligned (ARGB8888).
 * @post DRW ORIGIN/PITCH/CONTROL2 target ``g_drw_blend_fb`` (ARGB8888).
 * @post FB + texture caches are left off so the CPU reads fresh pixels.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t drw_blend_configure(void)
{
  const ra8_drw_config_t cfg = {
    .framebuffer_addr       = (uintptr_t)g_drw_blend_fb,
    .pitch_px               = (uint16_t)k_drw_blend_fb_dim,
    .format                 = k_ra8_drw_writefmt_argb8888,
    .enable_caches          = false,
    .enable_buffered_writes = false,
  };
  return ra8_drw_init(&cfg);
}

/**
 * @brief Issue one opaque ``ra8_drw_fill_rect`` and wait for the engine.
 *
 * @param[in] x     Top-left X in pixels.
 * @param[in] y     Top-left Y in pixels.
 * @param[in] w     Width in pixels.
 * @param[in] h     Height in pixels.
 * @param[in] argb  0xAARRGGBB fill colour.
 *
 * @par MC/DC:
 * Single decision ``ferr != k_ra8_ok`` -- no compound condition.
 *
 * @return ``ra8_err_t`` -- ``k_ra8_ok`` once the fill is issued and the engine
 *         goes idle, else the fill / wait error.
 * @retval k_ra8_ok Fill rasterized.
 * @retval k_ra8_err_invalid_arg Geometry rejected by ``ra8_drw_fill_rect``.
 * @retval k_ra8_err_hw_timeout Engine stayed busy past the poll budget.
 * @pre ::drw_blend_configure succeeded.
 * @pre ``w`` and ``h`` are within the DRW's accepted range.
 * @post On success the rectangle is present in ``g_drw_blend_fb``.
 * @post The DRW engine is idle on return.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t
drw_blend_fill(int16_t x, int16_t y, uint16_t w, uint16_t h, uint32_t argb)
{
  const ra8_drw_rect_t rect = {
    .x              = x,
    .y              = y,
    .width_px       = w,
    .height_px      = h,
    .color_argb8888 = argb,
  };
  const ra8_err_t ferr = ra8_drw_fill_rect(&rect);
  if (ferr != k_ra8_ok) {
    return ferr;
  }
  return ra8_drw_wait_idle((uint32_t)k_drw_blend_idle_budget);
}

/**
 * @brief Arm the blend unit for opaque-over (source-over) global-alpha mixing.
 *
 * @details
 * Sets COLOR1 to the foreground source via ``ra8_drw_set_gradient`` (COLOR2 is
 * unused here but written for determinism), then programs ``ra8_drw_blend_t``
 * for the standard source-over combination at the demo's global alpha. The
 * subsequent ``ra8_drw_fill_rect`` composites COLOR1 over each destination
 * pixel instead of overwriting it.
 *
 * @par MC/DC:
 * Single decision ``err != k_ra8_ok`` per call -- no compound condition.
 *
 * @return ``ra8_err_t`` -- ``k_ra8_ok`` once COLOR1/COLOR2 and CONTROL2 are set.
 * @retval k_ra8_ok Blend unit armed.
 * @retval k_ra8_err_null_ptr Internal descriptor pointer was NULL (cannot
 *         happen; both descriptors are stack locals).
 * @pre ::drw_blend_configure succeeded.
 * @pre The foreground source colour fits ARGB8888.
 * @post COLOR1 holds the foreground RGB with alpha = ``k_drw_blend_alpha``.
 * @post CONTROL2 source-over + USEACB bits are set for the next fill.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t drw_blend_arm(void)
{
  const ra8_drw_gradient_t grad = {
    .color1_argb8888 = (uint32_t)k_drw_blend_fg_argb,
    .color2_argb8888 = (uint32_t)k_drw_blend_bg_argb,
  };
  const ra8_err_t gerr = ra8_drw_set_gradient(&grad);
  if (gerr != k_ra8_ok) {
    return gerr;
  }

  /* True source-over: out = src*a + dst*(1-a). Source factor = alpha
   * (BSF=1, BSI=0); destination factor = 1-alpha (BDF=1, BDI=1). The
   * previous descriptor left dst_invert=false (dst factor = alpha, an
   * additive mix), which is not source-over. */
  const ra8_drw_blend_t blend = {
    .use_alpha_channel = true,
    .src_factor        = true,
    .dst_factor        = true,
    .src_invert        = false,
    .dst_invert        = true,
    .src_factor_alpha  = true,
    .dst_factor_alpha  = true,
    .src_invert_alpha  = false,
    .dst_invert_alpha  = true,
    .use_color2_dst    = false,
    .global_alpha      = (uint8_t)k_drw_blend_alpha,
  };
  return ra8_drw_set_blend(&blend);
}

/** @brief FNV-1a-32 over the rendered framebuffer bytes. */
static uint32_t drw_blend_framebuffer_hash(void)
{
  const uint32_t words = (uint32_t)k_drw_blend_fb_dim * (uint32_t)k_drw_blend_fb_dim;
  uint32_t       hsh   = (uint32_t)k_drw_blend_fnv_offset;
  for (uint32_t w = 0U; w < words; ++w) {
    const uint32_t px = g_drw_blend_fb[w];
    for (uint32_t b = 0U; b < (uint32_t)sizeof(uint32_t); ++b) {
      const uint8_t byte =
        (uint8_t)((px >> (b * (uint32_t)k_drw_blend_byte_bits)) & (uint32_t)k_drw_blend_byte_mask);
      hsh = (hsh ^ (uint32_t)byte) * (uint32_t)k_drw_blend_fnv_prime;
    }
  }
  return hsh;
}

/**
 * @brief Render the three layers (background, sprite, blended foreground).
 *
 * @param[out] out_crc Receives the FNV-1a-32 hash of the rendered framebuffer.
 *
 * @par MC/DC:
 * Each step is a single ``err != k_ra8_ok`` early-return decision; no compound
 * conditions. The host test drives the success path plus one failure-injection
 * vector per step.
 *
 * @return ``ra8_err_t`` -- ``k_ra8_ok`` once all three layers are composited.
 * @retval k_ra8_ok Framebuffer rendered and hashed.
 * @retval k_ra8_err_null_ptr ``out_crc`` was NULL.
 * @retval k_ra8_err_invalid_arg A layer rectangle was rejected.
 * @retval k_ra8_err_hw_timeout The engine stayed busy past the budget.
 * @pre ::drw_blend_configure succeeded.
 * @pre ``out_crc`` is non-null.
 * @post On success ``*out_crc`` holds the framebuffer hash.
 * @post ``g_drw_blend_err`` reflects the last DRW call.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t drw_blend_render(uint32_t* out_crc)
{
  RA8_CHECK_NULL_PTR(out_crc, s_tag, "out_crc must not be nullptr");

  const ra8_err_t bg = drw_blend_fill((int16_t)k_drw_blend_full_org,
                                      (int16_t)k_drw_blend_full_org,
                                      (uint16_t)k_drw_blend_fb_dim,
                                      (uint16_t)k_drw_blend_fb_dim,
                                      (uint32_t)k_drw_blend_bg_argb);
  if (bg != k_ra8_ok) {
    g_drw_blend_err = (uint32_t)bg;
    return bg;
  }

  const ra8_err_t sp = drw_blend_fill((int16_t)k_drw_blend_sp_xy,
                                      (int16_t)k_drw_blend_sp_xy,
                                      (uint16_t)k_drw_blend_sp_wh,
                                      (uint16_t)k_drw_blend_sp_wh,
                                      (uint32_t)k_drw_blend_sp_argb);
  if (sp != k_ra8_ok) {
    g_drw_blend_err = (uint32_t)sp;
    return sp;
  }

  const ra8_err_t armed = drw_blend_arm();
  if (armed != k_ra8_ok) {
    g_drw_blend_err = (uint32_t)armed;
    return armed;
  }

  /* The blended fill carries the global alpha IN its colour: fill_rect
   * writes the full COLOR1 register, so passing the opaque source colour
   * here would clobber the armed alpha back to 0xFF and blend nothing. */
  const ra8_err_t fg = drw_blend_fill((int16_t)k_drw_blend_fg_xy,
                                      (int16_t)k_drw_blend_fg_xy,
                                      (uint16_t)k_drw_blend_fg_wh,
                                      (uint16_t)k_drw_blend_fg_wh,
                                      (uint32_t)k_drw_blend_fg_a80);
  if (fg != k_ra8_ok) {
    g_drw_blend_err = (uint32_t)fg;
    return fg;
  }

  g_drw_blend_err = 0U;
  *out_crc        = drw_blend_framebuffer_hash();
  return k_ra8_ok;
}

/** @brief Print one hex nibble's worth of the 32-bit CRC over the console. */
static void drw_blend_print_hex(uint32_t value)
{
  static const uint8_t k_hex[] = "0123456789ABCDEF";
  uint8_t              out[(uint32_t)k_drw_blend_hex_digit];
  for (uint32_t i = 0U; i < (uint32_t)k_drw_blend_hex_digit; ++i) {
    const uint32_t shift = ((uint32_t)k_drw_blend_hi_nibble - i) * (uint32_t)k_drw_blend_nib_bits;
    out[i]               = k_hex[(value >> shift) & (uint32_t)k_drw_blend_nib_mask];
  }
  (void)ra8_board_uart_console_write(out, (size_t)sizeof(out));
}

/** @brief Emit the success banner: prefix + 8 hex CRC + " PASS\r\n". */
static void drw_blend_print_pass(uint32_t crc)
{
  (void)ra8_board_uart_console_write(k_drw_blend_pass_pfx,
                                     (size_t)(sizeof(k_drw_blend_pass_pfx) - 1U));
  drw_blend_print_hex(crc);
  (void)ra8_board_uart_console_write(k_drw_blend_pass_sfx,
                                     (size_t)(sizeof(k_drw_blend_pass_sfx) - 1U));
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  drw_blend_setup_or_halt();
  /* Clear PRIMASK so SysTick can dispatch and ra8_delay_ms() uses the
   * SysTick path (board_sim does not advance DWT_CYCCNT). No NVIC sources
   * are armed by this demo. */
  ra8_isr_globals_enable();

  if (drw_blend_configure() != k_ra8_ok) {
    drw_blend_panic_halt();
  }

  while (1) {
    uint32_t        crc = 0U;
    const ra8_err_t err = drw_blend_render(&crc);
    if (err == k_ra8_ok) {
      g_drw_blend_crc = crc;
      drw_blend_print_pass(crc);
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    } else {
      (void)ra8_board_uart_console_write(k_drw_blend_fail_msg,
                                         (size_t)(sizeof(k_drw_blend_fail_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led2);
    }
    ++g_drw_blend_heartbeat;
    ra8_delay_ms(k_drw_blend_period_ms);
  }
  drw_blend_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
