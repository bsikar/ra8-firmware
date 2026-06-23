/**
 * @file board_periph_drw.c
 * @brief DRW ("D/AVE 2D") solid-fill rasterizer model for board_sim
 *
 * @details
 * Models the RA8D2 DRW 2D graphics engine (ra8d2_drw_regs.h, ra_drw.c) at
 * @c 0x40444000 so the rasterizer actually writes pixels into the emulated
 * framebuffer instead of merely shadowing control registers. Against the sparse
 * fallback the DRW control writes read back and @c ra_drw_wait_idle returned,
 * but the framebuffer was never touched, so @c drw_fill_demo's centre pixel
 * stayed clear and the banner reported @c match=N. This block makes the solid
 * fill land in memory, flipping that to @c match=Y, and serves the same path the
 * e-reader @c ra_gfx rectangle fills use.
 *
 * The engine is driven by a fixed register sequence the driver emits (see
 * @c ra_drw_fill_rect / @c ra_drw_init in ra_drw.c):
 *  - @c ORIGIN (0x080) holds the framebuffer base address.
 *  - @c PITCH (0x07C) holds the framebuffer pitch in pixels.
 *  - @c CONTROL2 (0x004) low/high bits encode WRITEFORMAT (A8 / RGB565 /
 *    ARGB8888 / ARGB4444) -> bytes-per-pixel, plus PATTERN / TEXTURE enables.
 *  - @c COLOR1 (0x064) is the ARGB8888 fill colour.
 *  - @c L1START (0x010) / @c L3START (0x018) are the rectangle's top-left
 *    (x0, y0) in 1/16-pixel sub-units; @c SIZE (0x078) carries width (low 16)
 *    and height (high 16) in whole pixels.
 *  - A write to @c CONTROL (0x000) with the limiter-enable bits set STARTS the
 *    engine. With limiters 1..4 enabled and PATTERN / TEXTURE off in CONTROL2,
 *    that is an axis-aligned solid box fill -- the only primitive this model
 *    rasterizes. Any other CONTROL trigger (triangle / line / textured blit /
 *    display list) is a no-op here (the control window still shadows so the
 *    bring-up reads back, and STATUS reports idle so the driver's wait
 *    completes; the pixels simply are not synthesised).
 *
 * Completion is synchronous: the fill is performed inside the CONTROL write, so
 * STATUS always reads back with the busy bits clear and @c ra_drw_wait_idle
 * succeeds on its first poll. HWREVISION returns a non-zero stamp so
 * @c ra_drw_get_hwrevision reports the engine as present.
 *
 * Colour conversion from the COLOR1 ARGB8888 source to the framebuffer format
 * matches the engine's documented packing: RGB565 takes the high bits of each
 * channel, ARGB4444 the high nibbles, A8 the alpha byte.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_periph_block.h"

/** @brief DRW block geometry (ra8d2_drw_regs.h). */
typedef enum : uint64_t {
  k_drw_base = 0x40444000UL, /**< DRW Secure base (HUM Ch 62).      */
  k_drw_span = 0x104UL,      /**< Register window (260 bytes).      */
} drw_geom_t;

/** @brief DRW register offsets this model interprets (ra8d2_drw_regs.h). */
typedef enum : uint64_t {
  k_drw_off_control  = 0x000UL, /**< W CONTROL (geometry) / R STATUS.    */
  k_drw_off_control2 = 0x004UL, /**< W CONTROL2 / R HWREVISION.          */
  k_drw_off_l1start  = 0x010UL, /**< W L1START: x0 in sub-pixels.        */
  k_drw_off_l3start  = 0x018UL, /**< W L3START: y0 in sub-pixels.        */
  k_drw_off_color1   = 0x064UL, /**< W COLOR1: ARGB8888 fill colour.     */
  k_drw_off_size     = 0x078UL, /**< W SIZE: width[15:0] height[31:16].  */
  k_drw_off_pitch    = 0x07CUL, /**< W PITCH: framebuffer pitch (px).    */
  k_drw_off_origin   = 0x080UL, /**< W ORIGIN: framebuffer base address. */
} drw_off_t;

/** @brief CONTROL / CONTROL2 / SIZE field constants (ra8d2_drw_regs.h). */
typedef enum : uint32_t {
  k_drw_ctrl_quad_box    = 0x0FU,       /**< Limiters 1..4 enabled (box).   */
  k_drw_c2_patternenable = (1U << 0U),  /**< CONTROL2.PATTERNENABLE.        */
  k_drw_c2_textureenable = (1U << 1U),  /**< CONTROL2.TEXTUREENABLE.        */
  k_drw_c2_wfmt_lo_pos   = 20U,         /**< CONTROL2.WRITEFORMAT[1:0] @20. */
  k_drw_c2_wfmt_lo_mask  = 0x3U,        /**< Two low WRITEFORMAT bits.      */
  k_drw_c2_wfmt_hi_bit   = (1U << 8U),  /**< CONTROL2.WRITEFORMAT2 (bit 2). */
  k_drw_size_w_mask      = 0xFFFFU,     /**< SIZE width (low half-word).    */
  k_drw_size_h_pos       = 16U,         /**< SIZE height (high half-word).  */
  k_drw_subpixel_shift   = 4U,          /**< 16 sub-pixels per whole pixel. */
  k_drw_hwrevision_stamp = 0x00000909U, /**< Non-zero HWREVISION (present). */
} drw_field_t;

/** @brief COLOR1 ARGB8888 channel-extraction and pixel-pack constants. */
typedef enum : uint32_t {
  k_drw_argb_a_shift   = 24U,   /**< COLOR1 alpha byte position [31:24]. */
  k_drw_argb_r_shift   = 16U,   /**< COLOR1 red byte position [23:16].   */
  k_drw_argb_g_shift   = 8U,    /**< COLOR1 green byte position [15:8].   */
  k_drw_argb_byte_mask = 0xFFU, /**< Single 8-bit channel mask.          */
  k_drw_argb4444_a_pos = 12U,   /**< ARGB4444 alpha nibble position.     */
  k_drw_rgb565_r_pos   = 11U,   /**< RGB565 red field position [15:11].  */
  k_drw_rgb565_g_pos   = 5U,    /**< RGB565 green field position [10:5]. */
} drw_pixel_pack_t;

/** @brief WRITEFORMAT codes (ra_drw_writeformat_t). */
typedef enum : uint32_t {
  k_drw_wfmt_a8       = 0U, /**< 8 bpp A(8).      */
  k_drw_wfmt_rgb565   = 1U, /**< 16 bpp RGB565.   */
  k_drw_wfmt_argb8888 = 2U, /**< 32 bpp ARGB8888. */
  k_drw_wfmt_argb4444 = 3U, /**< 16 bpp ARGB4444. */
} drw_wfmt_t;

/** @brief Per-tick order slot for the DRW block (relative order only). */
typedef enum : uint32_t {
  k_drw_block_order = 160U, /**< After the DOC block; report order. */
} drw_order_t;

/** @brief DRW register state (the subset the solid-fill path consumes). */
typedef struct {
  uint32_t origin;   /**< ORIGIN: framebuffer base address.     */
  uint32_t pitch;    /**< PITCH: framebuffer pitch in pixels.   */
  uint32_t control2; /**< CONTROL2: write-format + source bits. */
  uint32_t color1;   /**< COLOR1: ARGB8888 fill colour.         */
  uint32_t l1start;  /**< L1START: x0 in sub-pixels.            */
  uint32_t l3start;  /**< L3START: y0 in sub-pixels.            */
  uint32_t size;     /**< SIZE: packed width / height.          */
  uint32_t fills;    /**< Solid box fills rasterized.           */
  uint32_t last_w;   /**< Last fill width (px), for the report. */
  uint32_t last_h;   /**< Last fill height (px), for the report.*/
} drw_state_t;

static drw_state_t s_drw;

/** @brief Bytes per framebuffer pixel for the CONTROL2 write-format. */
static uint32_t drw_bpp(void)
{
  const uint32_t lo =
    (s_drw.control2 >> (uint32_t)k_drw_c2_wfmt_lo_pos) & (uint32_t)k_drw_c2_wfmt_lo_mask;
  const uint32_t hi = ((s_drw.control2 & (uint32_t)k_drw_c2_wfmt_hi_bit) != 0U) ? 0x4U : 0x0U;
  switch (lo | hi) {
    case (uint32_t)k_drw_wfmt_a8:
      return 1U;
    case (uint32_t)k_drw_wfmt_argb8888:
      return 4U;
    case (uint32_t)k_drw_wfmt_rgb565:
    case (uint32_t)k_drw_wfmt_argb4444:
      return 2U;
    default:
      return 4U; /* Prohibited codes: default to ARGB8888 width. */
  }
}

/** @brief Pack the COLOR1 ARGB8888 source into the framebuffer pixel format. */
static uint32_t drw_pack_pixel(uint32_t bpp)
{
  const uint32_t a =
    (s_drw.color1 >> (uint32_t)k_drw_argb_a_shift) & (uint32_t)k_drw_argb_byte_mask;
  const uint32_t r =
    (s_drw.color1 >> (uint32_t)k_drw_argb_r_shift) & (uint32_t)k_drw_argb_byte_mask;
  const uint32_t g =
    (s_drw.color1 >> (uint32_t)k_drw_argb_g_shift) & (uint32_t)k_drw_argb_byte_mask;
  const uint32_t b = s_drw.color1 & (uint32_t)k_drw_argb_byte_mask;
  if (bpp == 4U) {
    return s_drw.color1;
  }
  if (bpp == 1U) {
    return a;
  }
  /* 16 bpp: distinguish RGB565 from ARGB4444 via the write-format code. */
  const uint32_t lo =
    (s_drw.control2 >> (uint32_t)k_drw_c2_wfmt_lo_pos) & (uint32_t)k_drw_c2_wfmt_lo_mask;
  if (lo == (uint32_t)k_drw_wfmt_argb4444) {
    return ((a >> 4U) << (uint32_t)k_drw_argb4444_a_pos) | ((r >> 4U) << 8U) | ((g >> 4U) << 4U) |
           (b >> 4U);
  }
  return ((r >> 3U) << (uint32_t)k_drw_rgb565_r_pos) | ((g >> 2U) << (uint32_t)k_drw_rgb565_g_pos) |
         (b >> 3U);
}

/** @brief Rasterize the programmed solid box into emulated framebuffer memory. */
static void drw_fill_box(uc_engine* uc)
{
  /* PATTERN / TEXTURE sources are not solid fills -- leave those to silicon. */
  if ((s_drw.control2 & ((uint32_t)k_drw_c2_patternenable | (uint32_t)k_drw_c2_textureenable)) !=
      0U) {
    return;
  }
  const uint32_t w = s_drw.size & (uint32_t)k_drw_size_w_mask;
  const uint32_t h = (s_drw.size >> (uint32_t)k_drw_size_h_pos) & (uint32_t)k_drw_size_w_mask;
  if ((w == 0U) || (h == 0U) || (s_drw.pitch == 0U) || (s_drw.origin == 0U)) {
    return;
  }
  const uint32_t x0  = s_drw.l1start >> (uint32_t)k_drw_subpixel_shift;
  const uint32_t y0  = s_drw.l3start >> (uint32_t)k_drw_subpixel_shift;
  const uint32_t bpp = drw_bpp();
  const uint32_t px  = drw_pack_pixel(bpp);

  for (uint32_t row = 0U; row < h; ++row) {
    const uint64_t line_base =
      (uint64_t)s_drw.origin +
      (((uint64_t)(y0 + row) * (uint64_t)s_drw.pitch + (uint64_t)x0) * (uint64_t)bpp);
    for (uint32_t col = 0U; col < w; ++col) {
      const uint64_t addr = line_base + ((uint64_t)col * (uint64_t)bpp);
      (void)uc_mem_write(uc, addr, &px, (size_t)bpp);
    }
  }
  s_drw.fills++;
  s_drw.last_w = w;
  s_drw.last_h = h;
}

/** @brief Reset the DRW model to power-on state. */
static void drw_reset(void)
{
  s_drw = (drw_state_t){};
}

/** @brief MMIO read inside the DRW window. */
static uint64_t drw_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_drw_base;
  if (off == (uint64_t)k_drw_off_control) {
    return 0U; /* STATUS alias: busy bits clear -> engine idle. */
  }
  if (off == (uint64_t)k_drw_off_control2) {
    return (uint64_t)k_drw_hwrevision_stamp; /* HWREVISION alias. */
  }
  if (off == (uint64_t)k_drw_off_origin) {
    return s_drw.origin;
  }
  if (off == (uint64_t)k_drw_off_pitch) {
    return s_drw.pitch;
  }
  if (off == (uint64_t)k_drw_off_color1) {
    return s_drw.color1;
  }
  if (off == (uint64_t)k_drw_off_size) {
    return s_drw.size;
  }
  return 0U;
}

/** @brief MMIO write inside the DRW window. */
static void drw_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)size;
  const uint64_t off = addr - (uint64_t)k_drw_base;
  switch (off) {
    case (uint64_t)k_drw_off_origin:
      s_drw.origin = (uint32_t)value;
      break;
    case (uint64_t)k_drw_off_pitch:
      s_drw.pitch = (uint32_t)value;
      break;
    case (uint64_t)k_drw_off_control2:
      s_drw.control2 = (uint32_t)value;
      break;
    case (uint64_t)k_drw_off_color1:
      s_drw.color1 = (uint32_t)value;
      break;
    case (uint64_t)k_drw_off_l1start:
      s_drw.l1start = (uint32_t)value;
      break;
    case (uint64_t)k_drw_off_l3start:
      s_drw.l3start = (uint32_t)value;
      break;
    case (uint64_t)k_drw_off_size:
      s_drw.size = (uint32_t)value;
      break;
    case (uint64_t)k_drw_off_control:
      /* CONTROL write starts the engine; the box-fill encoding is limiters
       * 1..4 enabled with no pattern / texture source. */
      if (((uint32_t)value & (uint32_t)k_drw_ctrl_quad_box) == (uint32_t)k_drw_ctrl_quad_box) {
        drw_fill_box(uc);
      }
      break;
    default:
      break; /* Other geometry registers: shadowed by the sparse fallback. */
  }
}

/** @brief End-of-run DRW section: fills performed and last box dimensions. */
static void drw_report(void)
{
  if (s_drw.fills == 0U) {
    return; /* Untouched: stay quiet. */
  }
  (void)fprintf(stderr,
                "  DRW           : box fills=%u last=%ux%u color1=0x%08X\n",
                s_drw.fills,
                s_drw.last_w,
                s_drw.last_h,
                s_drw.color1);
}

/** @brief DRW block descriptor (self-registered with the core). */
static const board_periph_block_t k_drw_block = {
  .base   = (uint64_t)k_drw_base,
  .span   = (uint64_t)k_drw_span,
  .order  = (uint32_t)k_drw_block_order,
  .read   = drw_read,
  .write  = drw_write,
  .tick   = nullptr,
  .reset  = drw_reset,
  .report = drw_report,
  .name   = "DRW",
};

/** @brief Register the DRW block before main (host constructor). */
__attribute__((constructor)) static void drw_block_register(void)
{
  board_periph_register_block(&k_drw_block);
}
