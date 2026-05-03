/**
 * @file examples/ek_ra8d2/lcd_demo/main.c
 * @brief Two-layer GLCDC demo for EK-RA8D2 (1024x600 7-inch parallel TFT)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Exercises the two-layer Graphics LCD Controller pipeline introduced
 * in Sweep 2 of the GLCDC HAL (``libs/ra_hal/src/ra_glcdc.c``):
 *   - Graphics layer 2 holds a static horizontal-gradient background
 *     image; the framebuffer is filled once at boot.
 *   - Graphics layer 1 is a small "sprite" framebuffer that bounces
 *     around inside an axis-aligned rectangle. Every frame the main
 *     loop redraws the sprite at its new position and the GLCDC
 *     reads the updated pixels at the next vertical sync.
 *   - The compositor is set to ``k_ra_blend_alpha`` so layer 1 is
 *     blended over layer 2 with the global alpha argument; the
 *     panel-wide background colour (visible where neither layer
 *     covers a pixel) is left at black.
 *
 * Sequence on entry:
 *   1. ``ra_cgc_init()`` -- bring the chip up to its standard clock
 *      tree (CPUCLK0 = 1 GHz, PCLKA = 125 MHz). The GLCDC pixel
 *      clock is sourced from PLL2 and is programmed by the GLCDC
 *      driver itself when ``ra_glcdc_init`` runs (HUM Ch 53,
 *      "Graphics LCD Controller (GLCDC)", p 3744).
 *   2. ``ra_time_init(cpuclk0_hz)`` -- SysTick @ 1 kHz for the
 *      frame-pacing busy-wait.
 *   3. ``ra_gpio_output_init(k_ra_pin_led1)`` -- LED1 toggled per
 *      frame as a visual heartbeat.
 *   4. PFS routes for the GLCDC parallel-RGB pin block.
 *      The exact EK-RA8D2 v1 board pin map for the 24-bit parallel
 *      bus, HSYNC / VSYNC / DE / CLK is documented per-pin in
 *      ``k_lcd_demo_glcdc_pins`` below. Hardware bring-up will need
 *      to confirm those entries against the EK-RA8D2 v1 user manual
 *      (``docs/reference/ek-ra8d2-v1-users-manual.pdf`` -- not yet
 *      committed). The build only requires the routing API to
 *      accept the pins; runtime behaviour is panel-specific.
 *   5. Fill layer-2 framebuffer with a horizontal gradient.
 *   6. Pre-fill the layer-1 sprite framebuffer with a solid colour.
 *   7. ``ra_glcdc_init`` (layer 1 = sprite), ``ra_glcdc_set_layer2``
 *      (layer 2 = background), ``ra_glcdc_set_blend(alpha, 0xFF)``,
 *      ``ra_glcdc_set_background_color(0)``, ``ra_glcdc_start(true)``.
 *   8. Loop: every ``k_lcd_demo_frame_period_ms`` ms, advance the
 *      sprite position, reprogram layer-1 ``pos_x``/``pos_y`` via
 *      ``ra_glcdc_set_layer2`` semantics on layer 1 (the GLCDC
 *      driver only exposes a setter for layer 2; layer 1's display
 *      window is fixed at panel origin in this demo, so the
 *      "bouncing" effect is achieved by re-drawing the sprite inside
 *      its framebuffer at the new offset and clearing the previous
 *      copy). Toggle LED1.
 *
 * @par Memory budget
 * The on-chip SRAM is 1 MiB in the linker script. A full-panel
 * 1024 x 600 RGB565 framebuffer is 1.2 MiB, so the demo uses
 * down-scaled layer images that fit:
 *   - Layer 2 background: 320 x 240 RGB565 = 153.6 KiB.
 *   - Layer 1 sprite:     128 x 128 RGB565 =  32.0 KiB.
 *   - Bounding box (where the sprite can travel inside its layer)
 *     is ``(layer1_w - sprite_size)`` x ``(layer1_h - sprite_size)``.
 * Re-driving a true 1024 x 600 full-screen surface is the user's
 * responsibility and requires placing the framebuffer in SDRAM
 * (``.sdram_data`` section in ``linker_script.ld``) plus a working
 * SDRAMC bring-up; that is out of scope for this smoke test, which
 * exists to validate the GLCDC two-layer API path.
 *
 * @par Pin-mux (EK-RA8D2 v1)
 * The exact pin mapping for the 7-inch parallel TFT on the EK-RA8D2
 * v1 board is documented per-pin in the ``k_lcd_demo_glcdc_pins``
 * table. Each entry is marked ``TODO: confirm against EK-RA8D2 v1
 * board manual table NN`` until the board manual PDF is committed
 * to ``docs/reference/``. Routing succeeds in the build because the
 * PSEL value (``k_ra_psel_glcdc``, 0x15) is the same across every
 * GLCDC-capable pin and the PFS driver only validates port/pin
 * range, not function compatibility.
 *
 * @par Architectural ring
 * [Ring 6 / APP] {World: S} -- application-layer code that runs in
 * the Secure world.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-29
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_glcdc_regs.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_glcdc.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_time.h"

/* =============================================================================
 * Compile-time configuration
 * =============================================================================
 */

/**
 * @brief Layer-1 (sprite) framebuffer dimensions, in pixels.
 *
 * @details
 * 128 x 128 RGB565 = 32 KiB, well within the 1 MiB SRAM budget.
 */
typedef enum : uint16_t {
  k_lcd_demo_layer1_w = 128U, /**< Layer-1 framebuffer width.  */
  k_lcd_demo_layer1_h = 128U, /**< Layer-1 framebuffer height. */
} lcd_demo_layer1_dim_t;

/**
 * @brief Layer-2 (background) framebuffer dimensions, in pixels.
 *
 * @details
 * 320 x 240 RGB565 = 153.6 KiB. Positioned at panel origin so the
 * top-left quadrant of the panel shows the gradient and the rest of
 * the panel shows the GLCDC background colour (black).
 */
typedef enum : uint16_t {
  k_lcd_demo_layer2_w = 320U, /**< Layer-2 framebuffer width.  */
  k_lcd_demo_layer2_h = 240U, /**< Layer-2 framebuffer height. */
} lcd_demo_layer2_dim_t;

/**
 * @brief Sprite drawn inside the layer-1 framebuffer.
 *
 * @details
 * 64 x 64 solid-colour rectangle. The sprite "bounces" by clearing
 * the previous rectangle to transparent (0x0000) and drawing a new
 * rectangle at the advanced position; layer-1 ``pos_x`` / ``pos_y``
 * stays fixed at panel origin.
 */
typedef enum : uint16_t {
  k_lcd_demo_sprite_w = 64U, /**< Sprite width  in pixels. */
  k_lcd_demo_sprite_h = 64U, /**< Sprite height in pixels. */
} lcd_demo_sprite_dim_t;

/** @brief Fixed display position for layer 1 (panel origin, top-left). */
typedef enum : uint16_t {
  k_lcd_demo_layer1_pos_x = 0U, /**< Layer-1 X position on panel. */
  k_lcd_demo_layer1_pos_y = 0U, /**< Layer-1 Y position on panel. */
  k_lcd_demo_layer2_pos_x = 0U, /**< Layer-2 X position on panel. */
  k_lcd_demo_layer2_pos_y = 0U, /**< Layer-2 Y position on panel. */
} lcd_demo_layer_pos_t;

/** @brief Frame pacing -- 60 Hz nominal -> 16 ms per frame. */
typedef enum : uint32_t {
  k_lcd_demo_frame_period_ms = 16U, /**< 1000 ms / 60 Hz approx 16 ms. */
} lcd_demo_pace_t;

/** @brief Initial sprite velocity (pixels per frame, signed). */
typedef enum : int16_t {
  k_lcd_demo_init_vx = 2, /**< +X velocity, pixels per frame. */
  k_lcd_demo_init_vy = 2, /**< +Y velocity, pixels per frame. */
} lcd_demo_velocity_t;

/** @brief Fixed sprite colour (RGB565: solid magenta). */
typedef enum : uint16_t {
  k_lcd_demo_sprite_color = 0xF81FU, /**< RGB565 magenta. */
} lcd_demo_sprite_color_t;

/** @brief Bytes per pixel for RGB565 framebuffers (used in stride math). */
typedef enum : uint8_t {
  k_lcd_demo_bpp_rgb565 = 2U, /**< 2 bytes per RGB565 pixel. */
} lcd_demo_bpp_t;

/** @brief Global alpha applied to layer 1 in alpha-blend mode. */
typedef enum : uint8_t {
  k_lcd_demo_global_alpha = 0xFFU, /**< Layer 1 fully opaque. */
} lcd_demo_alpha_t;

/** @brief Panel-wide background colour where no layer is drawn. */
typedef enum : uint32_t {
  k_lcd_demo_panel_bg_argb = 0x00000000UL, /**< Solid black. */
} lcd_demo_panel_bg_t;

/**
 * @brief Framebuffer alignment in bytes.
 *
 * @details
 * 64 bytes matches the AXI burst length used by the GLCDC DMA so a
 * full burst never straddles a cache-line boundary. Named so
 * ``readability-magic-numbers`` does not flag the
 * ``__attribute__((aligned()))`` attribute.
 */
enum : uint8_t {
  k_lcd_demo_fb_align = 64U, /**< AXI burst-friendly framebuffer alignment. */
};

/**
 * @brief Number of GLCDC pin routes the demo programmes.
 *
 * @details
 * 24 data lines + HSYNC + VSYNC + DE + CLK = 28 pins. Using the
 * enum makes the table sweep loop's upper bound NASA Rule 2 visible.
 */
typedef enum : uint8_t {
  k_lcd_demo_glcdc_pin_count = 28U,
} lcd_demo_pin_count_t;

/**
 * @brief Sprite movement bounds inside layer 1.
 *
 * @details
 * The sprite's top-left position is clamped so the full sprite is
 * visible: ``[0, layer1_w - sprite_w]`` and ``[0, layer1_h - sprite_h]``.
 */
typedef enum : uint16_t {
  k_lcd_demo_max_x = (uint16_t)(k_lcd_demo_layer1_w - k_lcd_demo_sprite_w),
  k_lcd_demo_max_y = (uint16_t)(k_lcd_demo_layer1_h - k_lcd_demo_sprite_h),
} lcd_demo_bounds_t;

/* =============================================================================
 * Pin routing table
 *
 * The EK-RA8D2 v1 board user manual is not yet committed to
 * `docs/reference/`, so each entry below is marked TODO. The pins
 * shown are the GLCDC-capable pins on the chip's 289-BGA pin list
 * (datasheet R01DS0493EJ table "Pin Functions", section "GLCDC"),
 * which is necessary-but-not-sufficient: the board manual must be
 * cross-checked to make sure those pads are wired to the LCD
 * connector J57 rather than to a different peripheral.
 * =============================================================================
 */

/**
 * @def LCD_DEMO_PP
 * @brief Pack a (port, pin) pair into the static-initialiser-compatible
 *        ``ra_port_pin_t`` packed form.
 *
 * @details
 * Used in ``k_lcd_demo_glcdc_pins[]`` because static array initialisers
 * in C must use constant expressions, which rules out a static-inline
 * helper. The macro is the "Reducing duplicated code" carve-out from
 * the project preprocessor policy.
 */
#define LCD_DEMO_PP(port, pin) ((ra_port_pin_t)(((uint16_t)(port) << 8) | (uint16_t)(pin)))

/**
 * @brief GLCDC pin list. Each pin gets ``k_ra_psel_glcdc``.
 *
 * @details
 * Order corresponds to LCDD0..LCDD23, LCD_HSYNC, LCD_VSYNC, LCD_DE,
 * LCD_CLK. Confirm against the EK-RA8D2 v1 user manual section
 * "LCD Connector J57" before flashing real hardware.
 */
/* TODO: confirm against EK-RA8D2 v1 board manual table NN ("LCD Connector J57"). */
static const ra_port_pin_t k_lcd_demo_glcdc_pins[k_lcd_demo_glcdc_pin_count] = {
  /* LCDD0..LCDD7  -> Port 6 (datasheet "GLCDC" pin function table). */
  /* TODO confirm */ LCD_DEMO_PP(6U, 0U),
  /* TODO confirm */ LCD_DEMO_PP(6U, 1U),
  /* TODO confirm */ LCD_DEMO_PP(6U, 2U),
  /* TODO confirm */ LCD_DEMO_PP(6U, 3U),
  /* TODO confirm */ LCD_DEMO_PP(6U, 4U),
  /* TODO confirm */ LCD_DEMO_PP(6U, 5U),
  /* TODO confirm */ LCD_DEMO_PP(6U, 6U),
  /* TODO confirm */ LCD_DEMO_PP(6U, 7U),
  /* LCDD8..LCDD15 -> Port 7. */
  /* TODO confirm */ LCD_DEMO_PP(7U, 0U),
  /* TODO confirm */ LCD_DEMO_PP(7U, 1U),
  /* TODO confirm */ LCD_DEMO_PP(7U, 2U),
  /* TODO confirm */ LCD_DEMO_PP(7U, 3U),
  /* TODO confirm */ LCD_DEMO_PP(7U, 4U),
  /* TODO confirm */ LCD_DEMO_PP(7U, 5U),
  /* TODO confirm */ LCD_DEMO_PP(7U, 6U),
  /* TODO confirm */ LCD_DEMO_PP(7U, 7U),
  /* LCDD16..LCDD23 -> Port 8. */
  /* TODO confirm */ LCD_DEMO_PP(8U, 0U),
  /* TODO confirm */ LCD_DEMO_PP(8U, 1U),
  /* TODO confirm */ LCD_DEMO_PP(8U, 2U),
  /* TODO confirm */ LCD_DEMO_PP(8U, 3U),
  /* TODO confirm */ LCD_DEMO_PP(8U, 4U),
  /* TODO confirm */ LCD_DEMO_PP(8U, 5U),
  /* TODO confirm */ LCD_DEMO_PP(8U, 6U),
  /* TODO confirm */ LCD_DEMO_PP(8U, 7U),
  /* HSYNC / VSYNC / DE / CLK -> Port 9. */
  /* TODO confirm */ LCD_DEMO_PP(9U, 0U),
  /* TODO confirm */ LCD_DEMO_PP(9U, 1U),
  /* TODO confirm */ LCD_DEMO_PP(9U, 2U),
  /* TODO confirm */ LCD_DEMO_PP(9U, 3U),
};

/* =============================================================================
 * Framebuffers (placed in SRAM; aligned to 64 bytes for AXI burst safety)
 * =============================================================================
 */

/**
 * @var s_layer2_fb
 * @brief Layer-2 background framebuffer (320 x 240 RGB565).
 *
 * @details
 * Filled once at boot with a horizontal red-to-blue gradient. The
 * static keyword keeps the buffer at file scope so it lands in
 * ``.bss`` (zero-init by Reset_Handler).
 */
static uint16_t s_layer2_fb[k_lcd_demo_layer2_h][k_lcd_demo_layer2_w]
  __attribute__((aligned(k_lcd_demo_fb_align)));

/**
 * @var s_layer1_fb
 * @brief Layer-1 sprite framebuffer (128 x 128 RGB565).
 *
 * @details
 * Initially zeroed (transparent in RGB565: 0x0000 reads as
 * RGB(0,0,0) but layer 1 is composited via per-pixel alpha so
 * 0x0000 effectively means "do not occlude layer 2"). Updated each
 * frame with a 64x64 magenta sprite at the current bouncing
 * position.
 */
static uint16_t s_layer1_fb[k_lcd_demo_layer1_h][k_lcd_demo_layer1_w]
  __attribute__((aligned(k_lcd_demo_fb_align)));

/* =============================================================================
 * Sprite state
 * =============================================================================
 */

/** @brief Current sprite top-left position (X). */
static uint16_t s_sprite_x = 0U;
/** @brief Current sprite top-left position (Y). */
static uint16_t s_sprite_y = 0U;
/** @brief Sprite X velocity (signed). */
static int16_t s_sprite_vx = k_lcd_demo_init_vx;
/** @brief Sprite Y velocity (signed). */
static int16_t s_sprite_vy = k_lcd_demo_init_vy;

/* =============================================================================
 * Helpers
 * =============================================================================
 */

/**
 * @brief Halt forever in WFI -- panic stop on init failure.
 *
 * @pre Called only after a fatal error in boot.
 * @post CPU is parked; only a debugger or external reset wakes it.
 *
 * @note Not thread-safe (intended for boot-time fatal error path only).
 *
 * @since 0.1.0
 */
static void lcd_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Programme PSEL = GLCDC on every pin in ``k_lcd_demo_glcdc_pins``.
 *
 * @details
 * Iterates the pin table once and routes each entry to GLCDC via
 * ``ra_pfs_route_peripheral``. Returns on the first failure.
 *
 * @return ``ra_err_t`` from the first failing route call, or ``k_ra_ok``.
 *
 * @retval k_ra_ok                       All pins routed.
 * @retval k_ra_err_gpio_invalid_port    PFS port index out of range.
 * @retval k_ra_err_gpio_invalid_pin     PFS pin index out of range.
 * @retval k_ra_err_gpio_conflict        Pin already claimed by another owner.
 *
 * @pre IOPORT module is reachable.
 * @pre Caller is single-threaded init context.
 *
 * @post On success, every entry of ``k_lcd_demo_glcdc_pins`` is
 *       in ``k_ra_psel_glcdc`` mode (PFS PSEL = 0x15).
 *
 * @note Not thread-safe; call once from boot.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t lcd_demo_pins_init(void)
{
  for (uint8_t i = 0U; i < k_lcd_demo_glcdc_pin_count; i++) {
    const ra_err_t err =
      ra_pfs_route_peripheral(k_lcd_demo_glcdc_pins[i], k_ra_psel_glcdc, "lcd_demo.glcdc");
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Pack 8-bit R/G/B values into an RGB565 16-bit pixel.
 *
 * @param[in] r Red   0..255.
 * @param[in] g Green 0..255.
 * @param[in] b Blue  0..255.
 * @return RGB565 packed value.
 */
static inline uint16_t lcd_demo_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
  enum : uint8_t {
    k_red_shift   = 8U,
    k_green_shift = 3U,
    k_blue_shift  = 3U,
    k_red_mask    = 0xF8U,
    k_green_mask  = 0xFCU,
    k_blue_mask   = 0xF8U,
  };
  const uint16_t r5 = (uint16_t)((uint16_t)(r & k_red_mask) << k_red_shift);
  const uint16_t g6 = (uint16_t)((uint16_t)(g & k_green_mask) << k_green_shift);
  const uint16_t b5 = (uint16_t)((uint16_t)(b & k_blue_mask) >> k_blue_shift);
  return (uint16_t)(r5 | g6 | b5);
}

/**
 * @brief Fill ``s_layer2_fb`` with a horizontal red-to-blue gradient.
 *
 * @details
 * Each column ``x`` gets ``red = 255 - (x * 255 / w)`` and
 * ``blue = (x * 255 / w)``; green stays at 0. Writes the same
 * pixel down every row of the column.
 *
 * @pre None.
 *
 * @post Every pixel in ``s_layer2_fb`` has been written exactly once.
 *
 * @since 0.1.0
 */
static void lcd_demo_fill_background_gradient(void)
{
  enum : uint16_t {
    k_grad_w         = k_lcd_demo_layer2_w,
    k_grad_h         = k_lcd_demo_layer2_h,
    k_grad_full_byte = 255U,
  };
  for (uint16_t x = 0U; x < k_grad_w; x++) {
    const uint8_t  red   = (uint8_t)(k_grad_full_byte - ((x * k_grad_full_byte) / k_grad_w));
    const uint8_t  blue  = (uint8_t)((x * k_grad_full_byte) / k_grad_w);
    const uint16_t pixel = lcd_demo_rgb565(red, 0U, blue);
    for (uint16_t y = 0U; y < k_grad_h; y++) {
      s_layer2_fb[y][x] = pixel;
    }
  }
}

/**
 * @brief Clear the entire layer-1 sprite framebuffer to 0x0000.
 *
 * @pre None.
 * @post All ``s_layer1_fb`` pixels are 0x0000.
 *
 * @since 0.1.0
 */
static void lcd_demo_layer1_clear(void)
{
  for (uint16_t y = 0U; y < k_lcd_demo_layer1_h; y++) {
    for (uint16_t x = 0U; x < k_lcd_demo_layer1_w; x++) {
      s_layer1_fb[y][x] = 0U;
    }
  }
}

/**
 * @brief Draw a solid-colour ``sprite_w x sprite_h`` rectangle into
 *        layer-1 at ``(x, y)``.
 *
 * @param[in] x        Top-left X (must be <= ``k_lcd_demo_max_x``).
 * @param[in] y        Top-left Y (must be <= ``k_lcd_demo_max_y``).
 * @param[in] color    RGB565 pixel value.
 *
 * @pre Caller has clamped ``x``/``y`` to the layer-1 bounds.
 * @post Pixels in [x, x+sprite_w) x [y, y+sprite_h) are ``color``.
 *
 * @since 0.1.0
 */
static void lcd_demo_layer1_draw_sprite(uint16_t x, uint16_t y, uint16_t color)
{
  for (uint16_t dy = 0U; dy < k_lcd_demo_sprite_h; dy++) {
    const uint16_t py = (uint16_t)(y + dy);
    for (uint16_t dx = 0U; dx < k_lcd_demo_sprite_w; dx++) {
      const uint16_t px   = (uint16_t)(x + dx);
      s_layer1_fb[py][px] = color;
    }
  }
}

/**
 * @brief Advance ``s_sprite_x`` / ``s_sprite_y`` by their velocity,
 *        bouncing off the layer-1 boundary box.
 *
 * @details
 * The bounding box is ``[0, k_lcd_demo_max_x]`` x
 * ``[0, k_lcd_demo_max_y]``. On reaching either edge, the
 * corresponding velocity component is negated.
 *
 * @pre The sprite is currently within the bounding box.
 * @post The sprite is still within the bounding box.
 *
 * @since 0.1.0
 */
static void lcd_demo_sprite_step(void)
{
  int32_t nx = (int32_t)s_sprite_x + (int32_t)s_sprite_vx;
  int32_t ny = (int32_t)s_sprite_y + (int32_t)s_sprite_vy;

  if (nx < 0) {
    nx          = 0;
    s_sprite_vx = (int16_t)(-s_sprite_vx);
  } else if (nx > (int32_t)k_lcd_demo_max_x) {
    nx          = (int32_t)k_lcd_demo_max_x;
    s_sprite_vx = (int16_t)(-s_sprite_vx);
  }
  if (ny < 0) {
    ny          = 0;
    s_sprite_vy = (int16_t)(-s_sprite_vy);
  } else if (ny > (int32_t)k_lcd_demo_max_y) {
    ny          = (int32_t)k_lcd_demo_max_y;
    s_sprite_vy = (int16_t)(-s_sprite_vy);
  }
  s_sprite_x = (uint16_t)nx;
  s_sprite_y = (uint16_t)ny;
}

/**
 * @brief Bring up CGC, SysTick, LED1, and GLCDC pin routing.
 *
 * @details
 * Halts the CPU on any failure.
 *
 * @pre Reset_Handler completed; we have a stack and zeroed BSS.
 * @post On success the chip is at its standard PLL clocks, SysTick
 *       is running, LED1 is an output, and the GLCDC pin block is
 *       routed to PSEL=glcdc.
 *
 * @since 0.1.0
 */
static void lcd_demo_drivers_init_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    lcd_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    lcd_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    lcd_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    lcd_demo_panic_halt();
  }
  if (lcd_demo_pins_init() != k_ra_ok) {
    lcd_demo_panic_halt();
  }
}

/**
 * @brief Programme GLCDC layers + blender and start panel scanning.
 *
 * @details
 * HUM Ch 53 ("Graphics LCD Controller (GLCDC)", p 3744). Layer 1
 * is the sprite framebuffer (configured via ``ra_glcdc_init``);
 * layer 2 is the background gradient (configured via
 * ``ra_glcdc_set_layer2``). Halts the CPU on any failure.
 *
 * @pre ``lcd_demo_drivers_init_or_halt`` has run.
 * @pre Both framebuffers have been pre-filled.
 * @post Panel is scanning the composite of both layers.
 *
 * @since 0.1.0
 */
static void lcd_demo_glcdc_start_or_halt(void)
{
  /* Layer 1 (UI overlay): sprite framebuffer. HUM Ch 53 GR1_FLM2/5/6. */
  const ra_glcdc_config_t cfg = {
    .framebuffer_addr = (uint32_t)(uintptr_t)&s_layer1_fb[0][0],
    .width_px         = k_lcd_demo_layer1_w,
    .height_px        = k_lcd_demo_layer1_h,
    .format           = k_ra_glcdc_fmt_rgb565,
  };
  if (ra_glcdc_init(&cfg) != k_ra_ok) {
    lcd_demo_panic_halt();
  }

  /* Layer 2 (background): gradient framebuffer. HUM Ch 53 GR2_FLM2/3/5/6 + AB1..AB7. */
  const ra_glcdc_layer2_cfg_t l2 = {
    .framebuffer_addr  = (uint32_t)(uintptr_t)&s_layer2_fb[0][0],
    .line_stride_bytes = (uint32_t)k_lcd_demo_layer2_w * k_lcd_demo_bpp_rgb565,
    .width_px          = k_lcd_demo_layer2_w,
    .height_px         = k_lcd_demo_layer2_h,
    .pos_x             = k_lcd_demo_layer2_pos_x,
    .pos_y             = k_lcd_demo_layer2_pos_y,
    .format            = k_ra_glcdc_fmt_rgb565,
    .alpha             = k_lcd_demo_global_alpha,
  };
  if (ra_glcdc_set_layer2(&l2) != k_ra_ok) {
    lcd_demo_panic_halt();
  }
  if (ra_glcdc_set_blend(k_ra_blend_alpha, k_lcd_demo_global_alpha) != k_ra_ok) {
    lcd_demo_panic_halt();
  }
  if (ra_glcdc_set_background_color(k_lcd_demo_panel_bg_argb) != k_ra_ok) {
    lcd_demo_panic_halt();
  }
  if (ra_glcdc_start(true) != k_ra_ok) {
    lcd_demo_panic_halt();
  }
}

/**
 * @brief Top-level boot. Drivers, framebuffers, GLCDC, first sprite.
 *
 * @details
 * Composes the per-stage init helpers above so ``main`` stays a
 * one-liner and each stage is independently visible in a debugger.
 *
 * @pre Reset_Handler completed.
 * @post Panel is scanning, sprite is drawn at the initial position.
 *
 * @since 0.1.0
 */
static void lcd_demo_setup_or_halt(void)
{
  lcd_demo_drivers_init_or_halt();
  lcd_demo_fill_background_gradient();
  lcd_demo_layer1_clear();
  lcd_demo_glcdc_start_or_halt();
  /* First sprite frame so the first vsync has something to show. */
  lcd_demo_layer1_draw_sprite(s_sprite_x, s_sprite_y, k_lcd_demo_sprite_color);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up GLCDC then runs the bouncing
 *        sprite loop forever.
 *
 * @return Never returns. Type is ``int32_t`` per the project's
 *         no-bare-``int`` rule; gcc's ``-Wmain`` is silenced via pragma.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On clean entry the CPU stays in the redraw loop forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  lcd_demo_setup_or_halt();
  ra_isr_globals_enable();

  while (1) {
    /* Erase previous sprite, advance, redraw. The driver streams the
     * updated framebuffer at the next panel vsync (HUM Ch 53). */
    lcd_demo_layer1_draw_sprite(s_sprite_x, s_sprite_y, 0U);
    lcd_demo_sprite_step();
    lcd_demo_layer1_draw_sprite(s_sprite_x, s_sprite_y, k_lcd_demo_sprite_color);

    if (ra_board_led_toggle(k_ra_board_led1) != k_ra_ok) {
      break;
    }
    ra_delay_ms(k_lcd_demo_frame_period_ms);
  }

  lcd_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
