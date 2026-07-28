/**
 * @file board_periph_glcdc.c
 * @brief Graphics LCD Controller (GLCDC) framebuffer-descriptor model for ra8_emulator
 *
 * @details
 * Models the RA8D2 GLCDC display controller (ra8_glcdc_regs.h, ra8_glcdc.c) at
 * @c 0x40342000 so the display / e-reader example family can be inspected
 * headlessly: the model snoops the graphics-layer register writes the driver
 * emits and recovers WHERE the scanned-out framebuffer lives in emulated memory
 * and HOW to read it (base, width, height, line stride, pixel format). That
 * descriptor is published through ::board_periph_glcdc_get_framebuffer so a
 * smoke harness can pull the rendered pixels out of modelled RAM -- on-chip SRAM
 * or external SDRAM at @c 0x68000000 -- and checksum them, and the end-of-run
 * report folds an FNV-1a-32 over the framebuffer itself so the rendered image
 * has a deterministic, greppable witness.
 *
 * @par Observe-only, not owning.
 * Unlike every other board_periph block, the GLCDC block is registered with
 * @c observe = true (see board_periph_block.h). main.c's panel compositor
 * (@c build_frame, behind @c --ppm / @c --view) already reads the GLCDC GR1
 * registers back from the emulator's sparse MMIO shadow to build the display
 * frame. An OWNING block would divert the firmware's GLCDC register writes away
 * from that shadow and the compositor would blank, regressing the @c --ppm
 * render gate. So this block SNOOPS instead: the core still routes each GLCDC
 * write here (so the descriptor decode sees the full stream) but reports the
 * access "not handled", leaving the sparse model to serve reads and record
 * writes exactly as before. The block's @c read handler is therefore never
 * called and is a stub.
 *
 * @par What is decoded (HUM Ch 63 "Graphics LCD Controller (GLCDC)").
 * The driver programs each graphics layer GR1 (offset @c 0x1100) / GR2
 * (@c 0x1200) with FLM2.BASE (framebuffer address), FLM3.LNOFF (line stride in
 * bytes), FLM5.LNNUM (lines - 1), FLM6.FORMAT (pixel format), and flips
 * FLMRD.RENB to start fetching; the background plane BG (@c 0x1000) carries the
 * BG_EN output-stage enable and BG_BGC. The model shadows that subset
 * and presents the active layer's framebuffer: GR1 when its FLMRD is set
 * (the upper layer the e-reader and @c glcdc_render draw into), otherwise GR2.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_periph.h"
#include "board_periph_block.h"

/** @brief GLCDC register-window geometry (ra8_glcdc_regs.h). */
typedef enum : uint64_t {
  k_glcdc_base = 0x40342000ULL, /**< GLCDC block base (HUM Ch 63).          */
  k_glcdc_span = 0x1500ULL,     /**< Covers CLUTs..SYSCNT_PANEL_CLK 0x1450. */
} glcdc_geom_t;

/**
 * @brief GLCDC register byte offsets this model snoops (ra8_glcdc_regs.h).
 *
 * @details
 * The subset of the GR1 / GR2 graphics-layer and BG background-plane registers
 * the framebuffer-descriptor decode needs. Names mirror FSP's @c R_GLCDC_Type
 * field names so a reader can cross-reference HUM Ch 63 directly.
 */
typedef enum : uint64_t {
  k_glcdc_off_bg_en = 0x1000ULL, /**< BG.EN.EN: output stage enable. */

  k_glcdc_off_gr1_flmrd = 0x1104ULL, /**< GR[0].FLMRD.RENB: framebuffer read.  */
  k_glcdc_off_gr1_saddr = 0x110CULL, /**< GR[0].FLM2.BASE: framebuffer addr.   */
  k_glcdc_off_gr1_flm3  = 0x1110ULL, /**< GR[0].FLM3.LNOFF: line stride bytes. */
  k_glcdc_off_gr1_line  = 0x1118ULL, /**< GR[0].FLM5: LNNUM (lines - 1).       */
  k_glcdc_off_gr1_fmt   = 0x111CULL, /**< GR[0].FLM6.FORMAT: pixel format.     */

  k_glcdc_off_gr2_flmrd = 0x1204ULL, /**< GR[1].FLMRD.RENB.  */
  k_glcdc_off_gr2_saddr = 0x120CULL, /**< GR[1].FLM2.BASE.   */
  k_glcdc_off_gr2_flm3  = 0x1210ULL, /**< GR[1].FLM3.LNOFF.  */
  k_glcdc_off_gr2_line  = 0x1218ULL, /**< GR[1].FLM5.        */
  k_glcdc_off_gr2_fmt   = 0x121CULL, /**< GR[1].FLM6.FORMAT. */
} glcdc_off_t;

/** @brief GLCDC register field shifts / masks the decode applies (HUM Ch 63). */
typedef enum : uint32_t {
  k_glcdc_bg_en_bit      = (1U << 0U), /**< BG_EN.EN: background operation on.   */
  k_glcdc_flmrd_renb_bit = (1U << 0U), /**< FLMRD.RENB: framebuffer read enable. */
  k_glcdc_fmt_shift      = 28U,        /**< FLM6.FORMAT[30:28].                  */
  k_glcdc_fmt_mask       = 0x7U,       /**< FORMAT field width (3 bits).         */
  k_glcdc_stride_shift   = 16U,        /**< FLM3.LNOFF lives in bits [31:16].    */
  k_glcdc_stride_mask    = 0xFFFFU,    /**< FLM3.LNOFF is 16 bits.               */
  k_glcdc_lnnum_shift    = 16U,        /**< FLM5.LNNUM lives in bits [26:16].    */
  k_glcdc_lnnum_mask     = 0x7FFU,     /**< FLM5.LNNUM is 11 bits.               */
} glcdc_field_t;

/** @brief FLM6.FORMAT codes (ra8_glcdc_pixel_fmt_t; HUM Ch 63 Table 63.11). */
typedef enum : uint8_t {
  k_glcdc_fmt_argb8888 = 0x0U, /**< 32-bit ARGB.     */
  k_glcdc_fmt_rgb888   = 0x1U, /**< 24-bit RGB.      */
  k_glcdc_fmt_rgb565   = 0x2U, /**< 16-bit RGB565.   */
  k_glcdc_fmt_argb1555 = 0x3U, /**< 16-bit ARGB1555. */
  k_glcdc_fmt_argb4444 = 0x4U, /**< 16-bit ARGB4444. */
  k_glcdc_fmt_clut8    = 0x5U, /**< 8-bit CLUT.      */
  k_glcdc_fmt_clut4    = 0x6U, /**< 4-bit CLUT.      */
  k_glcdc_fmt_clut1    = 0x7U, /**< 1-bit CLUT.      */
} glcdc_fmt_t;

/** @brief Bytes-per-pixel per FLM6.FORMAT code (CLUT modes round up to 1). */
typedef enum : uint8_t {
  k_glcdc_bpp_32 = 4U, /**< ARGB8888 / RGB888 fetch width.   */
  k_glcdc_bpp_16 = 2U, /**< RGB565 / ARGB1555 / ARGB4444.    */
  k_glcdc_bpp_8  = 1U, /**< CLUT8 / CLUT4 / CLUT1 (rounded). */
} glcdc_bpp_t;

/** @brief Emulated RAM windows a GLCDC framebuffer may legally live in. */
typedef enum : uint32_t {
  k_glcdc_dtcm_base  = 0x20000000U, /**< Data TCM start.       */
  k_glcdc_dtcm_end   = 0x20010000U, /**< Data TCM end.         */
  k_glcdc_sram_base  = 0x22000000U, /**< On-chip SRAM start.   */
  k_glcdc_sram_end   = 0x22200000U, /**< On-chip SRAM end.     */
  k_glcdc_sdram_base = 0x68000000U, /**< External SDRAM start. */
  k_glcdc_sdram_end  = 0x6C000000U, /**< External SDRAM end.   */
} glcdc_ram_window_t;

/** @brief FNV-1a-32 constants (match glcdc_render's framebuffer hash). */
typedef enum : uint32_t {
  k_glcdc_fnv_offset = 2166136261U, /**< FNV-1a-32 offset basis. */
  k_glcdc_fnv_prime  = 16777619U,   /**< FNV-1a-32 prime.        */
} glcdc_fnv_t;

/** @brief Bounds for the report-time framebuffer read (NASA Rule 2). */
typedef enum : uint32_t {
  k_glcdc_read_chunk   = 4096U,     /**< uc_mem_read batch size, bytes.      */
  k_glcdc_max_fb_bytes = 0x800000U, /**< 8 MiB cap on a hashed framebuffer.  */
  k_glcdc_max_dim      = 4096U,     /**< Sanity cap on decoded width/height. */
} glcdc_bound_t;

/** @brief One graphics layer's snooped descriptor registers. */
typedef struct {
  uint32_t saddr; /**< FLM2.BASE framebuffer base address.   */
  uint32_t flm3;  /**< FLM3.LNOFF (stride in bits [31:16]).  */
  uint32_t flm5;  /**< FLM5 (LNNUM lines-1 in bits [26:16]). */
  uint32_t fmt;   /**< FLM6.FORMAT (code in bits [30:28]).   */
  bool     flmrd; /**< FLMRD.RENB: layer is fetching pixels. */
} glcdc_layer_t;

/** @brief The modelled GLCDC: snooped graphics-layer + background state. */
typedef struct {
  glcdc_layer_t gr1;   /**< Graphics layer 1 (upper).             */
  glcdc_layer_t gr2;   /**< Graphics layer 2 (lower).             */
  bool          bg_on; /**< BG_EN.EN: output stage enabled.       */
  uc_engine*    uc;    /**< Engine handle stashed for the report. */
} glcdc_state_t;

static glcdc_state_t s_glcdc;

/** @brief Bytes per pixel for an FLM6.FORMAT code (HUM Ch 63 Table 63.11). */
static uint32_t glcdc_bpp_for_format(uint8_t fmt)
{
  switch (fmt) {
    case (uint8_t)k_glcdc_fmt_argb8888:
    case (uint8_t)k_glcdc_fmt_rgb888:
      return (uint32_t)k_glcdc_bpp_32;
    case (uint8_t)k_glcdc_fmt_rgb565:
    case (uint8_t)k_glcdc_fmt_argb1555:
    case (uint8_t)k_glcdc_fmt_argb4444:
      return (uint32_t)k_glcdc_bpp_16;
    default:
      return (uint32_t)k_glcdc_bpp_8; /* CLUT8 / CLUT4 / CLUT1. */
  }
}

/** @brief True iff @p addr is inside an emulated RAM window a framebuffer uses. */
static bool glcdc_addr_is_ram(uint32_t addr)
{
  const bool in_dtcm = (addr >= (uint32_t)k_glcdc_dtcm_base) && (addr < (uint32_t)k_glcdc_dtcm_end);
  const bool in_sram = (addr >= (uint32_t)k_glcdc_sram_base) && (addr < (uint32_t)k_glcdc_sram_end);
  const bool in_sdram =
    (addr >= (uint32_t)k_glcdc_sdram_base) && (addr < (uint32_t)k_glcdc_sdram_end);
  return in_dtcm || in_sram || in_sdram;
}

/**
 * @brief Decode a layer's snooped registers into a framebuffer descriptor.
 *
 * @details
 * Recovers the descriptor from the FSP-documented field layout: width is the
 * line stride divided by the format's bytes-per-pixel, height is FLM5.LNNUM + 1,
 * the format is FLM6.FORMAT[30:28]. Reports the layer "programmed" only when its
 * FLMRD is set, its base points into a modelled RAM window, and the stride /
 * dimensions are sane -- the same gate ::board_periph_glcdc_get_framebuffer and
 * the report apply.
 *
 * @param[in]  layer One graphics layer's snooped registers.
 * @param[in]  num   Layer number to stamp into @p out->layer (1 or 2).
 * @param[out] out   Receives the descriptor when the layer is programmed.
 * @return true when @p layer carries a usable framebuffer (and @p out written).
 * @retval true  Layer programmed; @p out holds base / dims / stride / format.
 * @retval false Layer idle or its registers are not framebuffer-sane.
 * @pre @p out is non-NULL.
 * @pre @p num is 1 or 2.
 * @post On true, @p out->base is in a modelled RAM window.
 * @post On false, @p out is left unchanged.
 * @note Pure decode; touches no emulated memory.
 * @since 0.1.0
 */
static bool glcdc_layer_decode(const glcdc_layer_t* layer, uint8_t num, board_glcdc_fb_t* out)
{
  if (!layer->flmrd || !glcdc_addr_is_ram(layer->saddr)) {
    return false;
  }
  const uint32_t stride =
    (layer->flm3 >> (uint32_t)k_glcdc_stride_shift) & (uint32_t)k_glcdc_stride_mask;
  const uint32_t lnnum =
    (layer->flm5 >> (uint32_t)k_glcdc_lnnum_shift) & (uint32_t)k_glcdc_lnnum_mask;
  const uint8_t fmt =
    (uint8_t)((layer->fmt >> (uint32_t)k_glcdc_fmt_shift) & (uint32_t)k_glcdc_fmt_mask);
  const uint32_t bpp    = glcdc_bpp_for_format(fmt);
  const uint32_t height = lnnum + 1U;
  const uint32_t width  = stride / bpp;
  if ((stride == 0U) || (width == 0U) || (width > (uint32_t)k_glcdc_max_dim) ||
      (height > (uint32_t)k_glcdc_max_dim)) {
    return false;
  }
  out->base    = layer->saddr;
  out->width   = width;
  out->height  = height;
  out->stride  = stride;
  out->format  = fmt;
  out->layer   = num;
  out->enabled = s_glcdc.bg_on;
  return true;
}

bool board_periph_glcdc_get_framebuffer(board_glcdc_fb_t* out)
{
  if (out == nullptr) {
    return false;
  }
  /* GR1 is the upper layer the e-reader and glcdc_render draw into; prefer it,
   * then fall back to GR2 when only the lower layer is fetching. */
  if (glcdc_layer_decode(&s_glcdc.gr1, 1U, out)) {
    return true;
  }
  return glcdc_layer_decode(&s_glcdc.gr2, 2U, out);
}

/** @brief Snoop one register write into the matching layer / BG shadow field. */
static void glcdc_snoop(uint64_t off, uint32_t value)
{
  switch (off) {
    /* HUM Ch 63 "BG_EN" p 3759 */ /* background-plane operation enable. */
    case (uint64_t)k_glcdc_off_bg_en:
      s_glcdc.bg_on = ((value & (uint32_t)k_glcdc_bg_en_bit) != 0U);
      break;

    /* HUM Ch 63 "GR1_FLM2.BASE" p 3744 */ /* layer-1 framebuffer base address. */
    case (uint64_t)k_glcdc_off_gr1_saddr:
      s_glcdc.gr1.saddr = value;
      break;
    /* HUM Ch 63 "GR1_FLM3.LNOFF" p 3744 */ /* layer-1 line stride (bytes). */
    case (uint64_t)k_glcdc_off_gr1_flm3:
      s_glcdc.gr1.flm3 = value;
      break;
    /* HUM Ch 63 "GR1_FLM5.LNNUM" p 3744 */ /* layer-1 line count minus one. */
    case (uint64_t)k_glcdc_off_gr1_line:
      s_glcdc.gr1.flm5 = value;
      break;
    /* HUM Ch 63 "GR1_FLM6.FORMAT" p 3744 */ /* layer-1 pixel format code. */
    case (uint64_t)k_glcdc_off_gr1_fmt:
      s_glcdc.gr1.fmt = value;
      break;
    /* HUM Ch 63 "GR1_FLMRD.RENB" p 3744 */ /* layer-1 framebuffer read enable. */
    case (uint64_t)k_glcdc_off_gr1_flmrd:
      s_glcdc.gr1.flmrd = ((value & (uint32_t)k_glcdc_flmrd_renb_bit) != 0U);
      break;

    /* HUM Ch 63 "GR2_FLM2.BASE" p 3744 */ /* layer-2 framebuffer base address. */
    case (uint64_t)k_glcdc_off_gr2_saddr:
      s_glcdc.gr2.saddr = value;
      break;
    /* HUM Ch 63 "GR2_FLM3.LNOFF" p 3744 */ /* layer-2 line stride (bytes). */
    case (uint64_t)k_glcdc_off_gr2_flm3:
      s_glcdc.gr2.flm3 = value;
      break;
    /* HUM Ch 63 "GR2_FLM5.LNNUM" p 3744 */ /* layer-2 line count minus one. */
    case (uint64_t)k_glcdc_off_gr2_line:
      s_glcdc.gr2.flm5 = value;
      break;
    /* HUM Ch 63 "GR2_FLM6.FORMAT" p 3744 */ /* layer-2 pixel format code. */
    case (uint64_t)k_glcdc_off_gr2_fmt:
      s_glcdc.gr2.fmt = value;
      break;
    /* HUM Ch 63 "GR2_FLMRD.RENB" p 3744 */ /* layer-2 framebuffer read enable. */
    case (uint64_t)k_glcdc_off_gr2_flmrd:
      s_glcdc.gr2.flmrd = ((value & (uint32_t)k_glcdc_flmrd_renb_bit) != 0U);
      break;

    default:
      break; /* Other GLCDC registers: shadowed by the caller's sparse model. */
  }
}

/** @brief MMIO write snoop inside the GLCDC window (observe-only, never owns). */
static void glcdc_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)size;
  s_glcdc.uc = uc; /* stash for the report's framebuffer read */
  glcdc_snoop(addr - (uint64_t)k_glcdc_base, (uint32_t)value);
}

/** @brief Read stub: an observe block never owns reads (the sparse model does). */
static uint64_t glcdc_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)addr;
  (void)size;
  return 0U;
}

/** @brief Stash the engine each chunk so the report can read framebuffer RAM. */
static void glcdc_tick(uc_engine* uc)
{
  s_glcdc.uc = uc;
}

/** @brief Clear the GLCDC model on reset / process start. */
static void glcdc_reset(void)
{
  s_glcdc = (glcdc_state_t){};
}

/** @brief FNV-1a-32 over the active framebuffer, read out of emulated RAM. */
static bool glcdc_hash_framebuffer(const board_glcdc_fb_t* fb, uint32_t* out_crc)
{
  if (s_glcdc.uc == nullptr) {
    return false;
  }
  const uint32_t bpp        = glcdc_bpp_for_format(fb->format);
  const uint64_t line_bytes = (uint64_t)fb->width * (uint64_t)bpp;
  const uint64_t total      = line_bytes * (uint64_t)fb->height;
  if ((total == 0U) || (total > (uint64_t)k_glcdc_max_fb_bytes)) {
    return false;
  }
  uint32_t hsh = (uint32_t)k_glcdc_fnv_offset;
  uint8_t  buf[k_glcdc_read_chunk];
  /* Hash the visible pixels row by row (skipping any inter-line stride padding)
   * so the witness matches a contiguous width*bpp*height framebuffer hash. */
  for (uint32_t row = 0U; row < fb->height; ++row) {
    const uint64_t row_base = (uint64_t)fb->base + ((uint64_t)row * (uint64_t)fb->stride);
    uint64_t       done     = 0U;
    while (done < line_bytes) {
      uint64_t want = line_bytes - done;
      if (want > (uint64_t)k_glcdc_read_chunk) {
        want = (uint64_t)k_glcdc_read_chunk;
      }
      if (uc_mem_read(s_glcdc.uc, row_base + done, buf, (size_t)want) != UC_ERR_OK) {
        return false;
      }
      for (uint64_t i = 0U; i < want; ++i) {
        hsh = (hsh ^ (uint32_t)buf[i]) * (uint32_t)k_glcdc_fnv_prime;
      }
      done += want;
    }
  }
  *out_crc = hsh;
  return true;
}

/** @brief Human label for an FLM6.FORMAT code. */
static const char* glcdc_format_name(uint8_t fmt)
{
  switch (fmt) {
    case (uint8_t)k_glcdc_fmt_argb8888:
      return "argb8888";
    case (uint8_t)k_glcdc_fmt_rgb888:
      return "rgb888";
    case (uint8_t)k_glcdc_fmt_rgb565:
      return "rgb565";
    case (uint8_t)k_glcdc_fmt_argb1555:
      return "argb1555";
    case (uint8_t)k_glcdc_fmt_argb4444:
      return "argb4444";
    default:
      return "clut";
  }
}

/** @brief End-of-run GLCDC section: the active framebuffer + its content hash. */
static void glcdc_report(void)
{
  board_glcdc_fb_t fb = {};
  if (!board_periph_glcdc_get_framebuffer(&fb)) {
    return; /* No graphics layer programmed: stay quiet. */
  }
  uint32_t    crc    = 0U;
  const bool  hashed = glcdc_hash_framebuffer(&fb, &crc);
  const char* enab   = fb.enabled ? "scanning" : "idle";
  if (hashed) {
    (void)fprintf(stderr,
                  "  GLCDC         : layer%u fb=0x%08X %ux%u %s stride=%u crc=%08X (%s)\n",
                  (unsigned)fb.layer,
                  fb.base,
                  fb.width,
                  fb.height,
                  glcdc_format_name(fb.format),
                  fb.stride,
                  crc,
                  enab);
  } else {
    (void)fprintf(stderr,
                  "  GLCDC         : layer%u fb=0x%08X %ux%u %s stride=%u (%s)\n",
                  (unsigned)fb.layer,
                  fb.base,
                  fb.width,
                  fb.height,
                  glcdc_format_name(fb.format),
                  fb.stride,
                  enab);
  }
}

/** @brief Per-tick / report order slot for the GLCDC block (after DRW). */
typedef enum : uint32_t {
  k_glcdc_block_order = 165U, /**< After DRW (160), before SRAM-ECC (176). */
} glcdc_order_t;

/** @brief GLCDC block descriptor: observe-only snoop over the register window. */
static const board_periph_block_t k_glcdc_block = {
  .base    = (uint64_t)k_glcdc_base,
  .span    = (uint64_t)k_glcdc_span,
  .order   = (uint32_t)k_glcdc_block_order,
  .read    = glcdc_read,
  .write   = glcdc_write,
  .tick    = glcdc_tick,
  .reset   = glcdc_reset,
  .report  = glcdc_report,
  .name    = "GLCDC",
  .observe = true,
};

/** @brief Register the GLCDC block before main (host constructor). */
[[gnu::constructor]] static void glcdc_block_register(void)
{
  board_periph_register_block(&k_glcdc_block);
}
