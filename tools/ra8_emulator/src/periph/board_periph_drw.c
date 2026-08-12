/**
 * @file board_periph_drw.c
 * @brief Bench-validated DRW ("D/AVE 2D") rasterizer model for ra8_emulator
 *
 * @details
 * Models the RA8D2 DRW 2D graphics engine (ra8_drw_regs.h, ra8_drw.c) at
 * @c 0x40444000. ra8_emulator's governing invariant is EIL == HIL: an app must
 * produce the IDENTICAL result in the emulator and on hardware, hardware
 * defects included. Every rule encoded here was measured on an EK-RA8D2 over
 * J-Link, not inferred from the datasheet ideal.
 *
 * @par Why the engine looked dead, then looked wrong (issues #247, #170)
 * The DRW never rasterized on the bench because it was never POWERED: it sits
 * in the graphics power domain, which @c PDCTRGD gates OFF at reset (HUM
 * Ch 11.2.14 p 452, reset value @c 0x81; domain contents -- MIPI DSI, MIPI CSI,
 * VIN, DRW, GLCDC -- in HUM Ch 11.5.1 Table 11.7 p 480). With the domain on,
 * an EK-RA8D2 reads @c HWREVISION = @c 0x0FBE0107 and the engine rasterizes.
 * The render was then still wrong in two ways -- a 16x16 request painted 8x8,
 * and the alpha byte never reached the framebuffer -- because the driver was
 * feeding the spatial limiters absolute pixel coordinates and leaving
 * CONTROL2.WRITEALPHA at its reset value. Both are fixed in ra8_drw.c; the
 * model below reproduces the CORRECTED behaviour.
 *
 * @par The geometry rule (bench-measured)
 * HUM Ch 62.6.2 p 3716: "the 2D Drawing Engine scans the whole bounding box".
 * The bounding box is @c SIZE (width in [15:0], height in [31:16], HUM
 * Ch 62.2.29 p 3704) anchored at the pixel @c ORIGIN addresses (HUM Ch 62.2.31
 * p 3705), stepping @c PITCH pixels per row. Writing @c ORIGIN is also the
 * render TRIGGER. So an axis-aligned solid rectangle needs NO spatial limiter:
 * ORIGIN positions it and SIZE sizes it. Confirmed on the bench -- with all
 * limiters disabled, @c SIZE=16x16 at @c ORIGIN=fb+px(8,8) paints exactly
 * pixels (8,8)..(23,23), and the same SIZE at @c ORIGIN=fb paints (0,0)..(15,15).
 *
 * @par The colour rule (bench-measured)
 * Colour goes through the blend unit, HUM Ch 62.6.5.1 p 3733:
 * @c dst = src*fS + dst*fD with fS/fD selected by CONTROL2.{BSF,BSI,BDF,BDI}
 * per Table 62.9. Note the RESET factors are fS = fD = 1, i.e. dst = src + dst:
 * a fill ADDS unless BDI is set. Bench: filling @c 0xFF00FF00 over a
 * @c 0x00000010 framebuffer gives @c 0xFF00FF10 without BDI and @c 0xFF00FF00
 * with it.
 *
 * @par The alpha rule (bench-measured)
 * With CONTROL2.USEACB = 0 the framebuffer alpha byte comes from the
 * @c WRITEALPHA[1:0] mux (CONTROL2 bits 23:22, HUM Ch 62.2.2 p 3694 and
 * Figure 62.23 p 3734), NOT from the colour blend. Its reset value 00 means
 * "use alpha from COLOR2", and COLOR2 is 0 for a plain fill -- which is why an
 * opaque @c 0xFF00FF00 fill read back @c 0x0000FF00 on the bench. Measured
 * behaviour of all four codes, with COLOR1 = @c 0xFF00FF00 unless noted:
 *  - 00 -> COLOR2's alpha byte (COLOR2 = @c 0x80000000 gave @c 0x8000FF00,
 *          and COLOR2's RGB did NOT leak into the result)
 *  - 01 -> source alpha, i.e. COLOR1's alpha scaled by coverage (COLOR1
 *          @c 0x8000FF00 gave @c 0x8000FF00; opaque gave @c 0xFF)
 *  - 10 -> forced 0
 *  - 11 -> the framebuffer's existing alpha
 * With USEACB = 1 the alpha channel is blended by the same formula shape using
 * CONTROL2.{BSFA,BSIA,BDFA,BDIA} (HUM Ch 62.6.5.2 p 3734).
 *
 * @par Rounding
 * The bench composite is reproduced bit-exactly by
 * @c out = (src*fS_num + dst*fD_num + 127) / 255 with the factors expressed in
 * 0..255. Checked against all four channels of @c drw_blend_demo's source-over
 * layer: source @c 0x80E04040 over @c 0xFF202060 gives @c 0xBF803050 on
 * silicon and from this formula.
 *
 * @par What is deliberately NOT modelled
 * Two configurations are left un-rasterized rather than guessed, because
 * inventing pixels the bench does not produce is the divergence this file
 * exists to prevent. Both fail SAFE -- the emulator renders nothing, so an app
 * relying on them goes visibly blank here instead of reporting a false PASS:
 *  - **Spatial limiters enabled.** The limiter half-plane semantics are only
 *    partly characterised. Bench sweeps of a single limiter show the selected
 *    half-plane boundary sitting at @c 16 - START/16 with @c XADD = +16, and
 *    the resulting coverage arriving as alpha @c 0x01-@c 0x02 rather than
 *    @c 0xFF, so a limiter-driven fill does not composite the way the linear
 *    setup in HUM Ch 62.6.2.1 predicts. No in-tree primitive needs them: the
 *    corrected rect fill and textured blit both use the bounding box alone.
 *  - **Framebuffer cache enabled** (@c CACHECTL.CENABLEFX). On silicon the
 *    rendered pixels then sit in the DRW cache and are invisible to the CPU
 *    until a @c CFLUSHFX; bench-confirmed both ways. The cache geometry and
 *    its eviction write-back are undocumented, so a partial model would be a
 *    guess. The corrected driver leaves the framebuffer cache exactly as
 *    ::ra8_drw_init configured it and no in-tree app enables it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_periph_block.h"
#include "board_periph_pdctr_internal.h"

/** @brief DRW block geometry (ra8_drw_regs.h, HUM Ch 62). */
typedef enum : uint64_t {
  k_drw_base = 0x40444000UL, /**< DRW Secure base (HUM Ch 62). */
  k_drw_span = 0x104UL,      /**< Register window (260 bytes). */
} drw_geom_t;

/** @brief Register offsets this model tracks (HUM Ch 62.2, ra8_drw_regs.h). */
typedef enum : uint64_t {
  k_drw_off_control    = 0x000UL, /**< W CONTROL / R STATUS.             */
  k_drw_off_control2   = 0x004UL, /**< W CONTROL2 / R HWREVISION.        */
  k_drw_off_color1     = 0x064UL, /**< W COLOR1: base colour.            */
  k_drw_off_color2     = 0x068UL, /**< W COLOR2: secondary colour.       */
  k_drw_off_size       = 0x078UL, /**< W SIZE: bounding box W/H.         */
  k_drw_off_pitch      = 0x07CUL, /**< W PITCH: framebuffer pitch.       */
  k_drw_off_origin     = 0x080UL, /**< W ORIGIN: bbox anchor + trigger.  */
  k_drw_off_cachectl   = 0x0C4UL, /**< W CACHECTL: cache enable / flush. */
  k_drw_off_dliststart = 0x0C8UL, /**< W DLISTSTART: kick display list.  */
} drw_off_t;

/**
 * @brief Display-list encoding constants (HUM Ch 62.6; TES D/AVE format).
 *
 * @details
 * The DLR fetches 32-bit words. This model executes exactly the encoding the
 * ra8_drw display-list builder emits and bench-verified on the EK-RA8D2 for
 * issue #247: single-register entries -- a tag ``0x80808000 | index`` (byte 1
 * bit 7 marks a one-index entry) followed by one value word -- and end-of-list
 * words whose low byte is 0xFF with an argument in byte 1 (2 = wait for the
 * pipeline+cache then continue, the only wait the builder emits; anything else
 * terminates). Register index == byte offset / 4. Any other tag encoding
 * (multi-index packing) is deliberately NOT modelled: it was never observed on
 * the bench, so the model STOPS rather than guess, and an app that relied on it
 * under-renders visibly here instead of a false PASS -- the same fail-safe
 * stance as the limiter and FB-cache paths above.
 */
typedef enum : uint32_t {
  k_drw_dlr_index_mask     = 0xFFU,       /**< Register index in a tag byte.    */
  k_drw_dlr_b1_boundary    = 0x00008000U, /**< Byte 1 bit 7 -> one-index entry. */
  k_drw_dlr_eol_low        = 0xFFU,       /**< Low byte of an end-of-list word. */
  k_drw_dlr_eol_arg_pos    = 8U,          /**< End-of-list argument position.   */
  k_drw_dlr_arg_wait       = 2U,          /**< Wait pipe+cache, keep reading.   */
  k_drw_dlr_idx_dliststart = 50U,         /**< DLISTSTART index (jump / stop).  */
  k_drw_dlr_bytes_per_word = 4U,          /**< Register index -> byte offset.   */
  k_drw_dlr_max_words      = 4096U,       /**< NASA Rule 2 fetch bound.         */
} drw_dlr_t;

/** @brief CONTROL bit-field constants (HUM Ch 62.2.1 p 3689). */
typedef enum : uint32_t {
  k_drw_control_limiter_mask = 0x3FU, /**< LIM1..LIM6 enable bits [5:0]. */
} drw_control_t;

/** @brief CONTROL2 bit-field constants (HUM Ch 62.2.2 pp 3691-3694). */
typedef enum : uint32_t {
  k_drw_c2_patternenable = (1U << 0U),  /**< Pattern source.           */
  k_drw_c2_textureenable = (1U << 1U),  /**< Texture source.           */
  k_drw_c2_useacb        = (1U << 3U),  /**< Alpha channel blending.   */
  k_drw_c2_bsfa          = (1U << 6U),  /**< Alpha src factor is a.    */
  k_drw_c2_bdfa          = (1U << 7U),  /**< Alpha dst factor is a.    */
  k_drw_c2_wfmt_hi_bit   = (1U << 8U),  /**< WRITEFORMAT[2].           */
  k_drw_c2_bsf           = (1U << 9U),  /**< Colour src factor is a.   */
  k_drw_c2_bdf           = (1U << 10U), /**< Colour dst factor is a.   */
  k_drw_c2_bsi           = (1U << 11U), /**< Colour src factor invert. */
  k_drw_c2_bdi           = (1U << 12U), /**< Colour dst factor invert. */
  k_drw_c2_wfmt_lo_pos   = 20U,         /**< WRITEFORMAT[1:0] @ 20.    */
  k_drw_c2_wfmt_lo_mask  = 0x3U,        /**< WRITEFORMAT[1:0] mask.    */
  k_drw_c2_walpha_pos    = 22U,         /**< WRITEALPHA[1:0] @ 22.     */
  k_drw_c2_walpha_mask   = 0x3U,        /**< WRITEALPHA[1:0] mask.     */
  k_drw_c2_bsia          = (1U << 28U), /**< Alpha src factor invert.  */
  k_drw_c2_bdia          = (1U << 29U), /**< Alpha dst factor invert.  */
} drw_c2_t;

/** @brief CACHECTL bit-field constants (HUM Ch 62.2.4 p 3694). */
typedef enum : uint32_t {
  k_drw_cachectl_cenablefx = (1U << 0U), /**< Framebuffer cache enable. */
} drw_cachectl_t;

/** @brief WRITEALPHA[1:0] codes (HUM Ch 62.2.2 p 3694, USEACB = 0). */
typedef enum : uint32_t {
  k_drw_walpha_color2    = 0U, /**< Alpha from COLOR2.       */
  k_drw_walpha_pixel_cov = 1U, /**< Source alpha (coverage). */
  k_drw_walpha_zero      = 2U, /**< Forced 0.0.              */
  k_drw_walpha_fb        = 3U, /**< Alpha from framebuffer.  */
} drw_walpha_t;

/** @brief WRITEFORMAT[2:0] codes (HUM Ch 62.2.2 p 3692). */
typedef enum : uint32_t {
  k_drw_wfmt_a8       = 0U, /**< 8 bpp A8.        */
  k_drw_wfmt_rgb565   = 1U, /**< 16 bpp RGB565.   */
  k_drw_wfmt_argb8888 = 2U, /**< 32 bpp ARGB8888. */
  k_drw_wfmt_argb4444 = 3U, /**< 16 bpp ARGB4444. */
} drw_wfmt_t;

/** @brief ARGB8888 channel layout + fixed-point blend constants. */
typedef enum : uint32_t {
  k_drw_byte_mask      = 0xFFU,   /**< One colour channel.           */
  k_drw_a_shift        = 24U,     /**< ARGB8888 alpha byte position. */
  k_drw_r_shift        = 16U,     /**< ARGB8888 red byte position.   */
  k_drw_g_shift        = 8U,      /**< ARGB8888 green byte position. */
  k_drw_alpha_full     = 255U,    /**< Blend factor 1.0.             */
  k_drw_alpha_round    = 127U,    /**< Round-to-nearest bias.        */
  k_drw_size_w_mask    = 0xFFFFU, /**< SIZE width / height field.    */
  k_drw_size_h_pos     = 16U,     /**< SIZE height in [31:16].       */
  k_drw_rgb565_r_pos   = 11U,     /**< RGB565 red position.          */
  k_drw_rgb565_g_pos   = 5U,      /**< RGB565 green position.        */
  k_drw_argb4444_a_pos = 12U,     /**< ARGB4444 alpha position.      */
  k_drw_nibble_shift   = 4U,      /**< 8 bpc -> 4 bpc.               */
  k_drw_r565_shift     = 3U,      /**< 8 bpc -> 5 bpc.               */
  k_drw_g565_shift     = 2U,      /**< 8 bpc -> 6 bpc.               */
} drw_layout_t;

/** @brief Bytes per framebuffer pixel (HUM Ch 62.3.1.1 p 3707). */
typedef enum : uint32_t {
  k_drw_bpp_8  = 1U, /**< A8.                */
  k_drw_bpp_16 = 2U, /**< RGB565 / ARGB4444. */
  k_drw_bpp_32 = 4U, /**< ARGB8888.          */
} drw_bpp_t;

/**
 * @brief Hardware revision stamp read back from real EK-RA8D2 silicon.
 *
 * @details Captured over J-Link with the graphics power domain enabled
 * (HUM Ch 62.2.6 "HWREVISION" p 3696). Reads 0 while the domain is gated.
 */
typedef enum : uint32_t {
  k_drw_hwrevision_value = 0x0FBE0107U, /**< Bench-observed HWREVISION. */
} drw_hwrev_t;

/** @brief Per-tick order slot for the DRW block (relative order only). */
typedef enum : uint32_t {
  k_drw_block_order = 160U, /**< After the DOC block; report order. */
} drw_order_t;

/**
 * @struct drw_state_t
 * @brief Shadow of the write-only DRW register file plus render statistics.
 *
 * @details
 * Every DRW register except STATUS, HWREVISION and the performance counters
 * is write-only on silicon (HUM Ch 62.2, R/W column "W"), so the model keeps
 * its own copies to rasterize from.
 *
 * @invariant @c pitch and @c size are zero until firmware programs them, and
 * a render with either zero is a no-op.
 *
 * @see drw_render
 */
typedef struct {
  uint32_t control;  /**< CONTROL: limiter enables.         */
  uint32_t control2; /**< CONTROL2: format / blend / alpha. */
  uint32_t color1;   /**< COLOR1: source colour.            */
  uint32_t color2;   /**< COLOR2: secondary colour.         */
  uint32_t size;     /**< SIZE: packed bounding box W/H.    */
  uint32_t pitch;    /**< PITCH: framebuffer pitch, pixels. */
  uint32_t origin;   /**< ORIGIN: bounding box anchor.      */
  uint32_t cachectl; /**< CACHECTL: cache enables.          */
  uint32_t renders;  /**< Bounding boxes rasterized.        */
  uint32_t skipped;  /**< Renders declined as unmodelled.   */
  uint32_t last_w;   /**< Width of the last render.         */
  uint32_t last_h;   /**< Height of the last render.        */
} drw_state_t;

/**
 * @var s_drw
 * @brief Module-private DRW model state.
 * @note Reset by ::drw_reset on every emulated system reset.
 * @warning Not thread-safe; ra8_emulator drives one CPU thread.
 */
static drw_state_t s_drw;

/** @brief Extract one 8-bit channel of an ARGB8888 word. */
static uint32_t drw_chan(uint32_t argb, uint32_t shift)
{
  return (argb >> shift) & (uint32_t)k_drw_byte_mask;
}

/** @brief Framebuffer bytes per pixel for the programmed WRITEFORMAT. */
static uint32_t drw_bpp(void)
{
  const uint32_t lo =
    (s_drw.control2 >> (uint32_t)k_drw_c2_wfmt_lo_pos) & (uint32_t)k_drw_c2_wfmt_lo_mask;
  const uint32_t hi = ((s_drw.control2 & (uint32_t)k_drw_c2_wfmt_hi_bit) != 0U) ? 0x4U : 0x0U;
  switch (lo | hi) {
    case (uint32_t)k_drw_wfmt_a8:
      return (uint32_t)k_drw_bpp_8;
    case (uint32_t)k_drw_wfmt_rgb565:
    case (uint32_t)k_drw_wfmt_argb4444:
      return (uint32_t)k_drw_bpp_16;
    default:
      /* ARGB8888 and the prohibited codes both take the 32-bit width. */
      return (uint32_t)k_drw_bpp_32;
  }
}

/**
 * @brief Resolve one blend factor to its 0..255 numerator.
 *
 * @details
 * HUM Ch 62.6.5.1 p 3733: the factor is 1 when the "is alpha" flag and the
 * invert flag are both clear, alpha when "is alpha" is set alone, 0 when
 * invert is set alone, and 1 - alpha when both are set. Shared by the colour
 * and the alpha channel because HUM Ch 62.6.5.2 p 3734 uses the identical
 * formula shape.
 *
 * @param[in] is_alpha CONTROL2 BSF / BDF / BSFA / BDFA for this factor.
 * @param[in] invert   CONTROL2 BSI / BDI / BSIA / BDIA for this factor.
 * @param[in] alpha    Source alpha in 0..255.
 * @return Factor numerator in 0..255 (denominator 255).
 *
 * @par MC/DC:
 * Two independent single-condition branches, four reachable outcomes, all
 * four exercised by the bench vectors in the file header: reset factors
 * (both clear -> 1), SRC_ONE (invert alone -> 0), and source-over
 * (is_alpha alone -> alpha; both -> 1 - alpha).
 */
static uint32_t drw_factor(bool is_alpha, bool invert, uint32_t alpha)
{
  if (is_alpha) {
    return invert ? ((uint32_t)k_drw_alpha_full - alpha) : alpha;
  }
  return invert ? 0U : (uint32_t)k_drw_alpha_full;
}

/** @brief Apply one channel of `src*fS + dst*fD`, rounded and saturated. */
static uint32_t drw_mix(uint32_t src, uint32_t dst, uint32_t fs, uint32_t fd)
{
  const uint32_t num = (src * fs) + (dst * fd) + (uint32_t)k_drw_alpha_round;
  const uint32_t out = num / (uint32_t)k_drw_alpha_full;
  return (out > (uint32_t)k_drw_byte_mask) ? (uint32_t)k_drw_byte_mask : out;
}

/** @brief Framebuffer alpha byte per the WRITEALPHA mux or the alpha blend. */
static uint32_t drw_out_alpha(uint32_t src_a, uint32_t dst_a)
{
  if ((s_drw.control2 & (uint32_t)k_drw_c2_useacb) != 0U) {
    /* HUM Ch 62.6.5.2 "Alpha Channel Blending" p 3734 */
    const uint32_t fsa = drw_factor((s_drw.control2 & (uint32_t)k_drw_c2_bsfa) != 0U,
                                    (s_drw.control2 & (uint32_t)k_drw_c2_bsia) != 0U,
                                    src_a);
    const uint32_t fda = drw_factor((s_drw.control2 & (uint32_t)k_drw_c2_bdfa) != 0U,
                                    (s_drw.control2 & (uint32_t)k_drw_c2_bdia) != 0U,
                                    src_a);
    return drw_mix(src_a, dst_a, fsa, fda);
  }
  /* HUM Ch 62.2.2 "WRITEALPHA[1:0]" p 3694 -- Figure 62.23 p 3734 */
  const uint32_t code =
    (s_drw.control2 >> (uint32_t)k_drw_c2_walpha_pos) & (uint32_t)k_drw_c2_walpha_mask;
  switch (code) {
    case (uint32_t)k_drw_walpha_color2:
      return drw_chan(s_drw.color2, (uint32_t)k_drw_a_shift);
    case (uint32_t)k_drw_walpha_pixel_cov:
      return src_a;
    case (uint32_t)k_drw_walpha_zero:
      return 0U;
    default:
      return dst_a;
  }
}

/** @brief Composite COLOR1 over one destination pixel, in ARGB8888 space. */
static uint32_t drw_shade(uint32_t dst)
{
  const uint32_t src_a = drw_chan(s_drw.color1, (uint32_t)k_drw_a_shift);
  const uint32_t fs    = drw_factor((s_drw.control2 & (uint32_t)k_drw_c2_bsf) != 0U,
                                    (s_drw.control2 & (uint32_t)k_drw_c2_bsi) != 0U,
                                    src_a);
  const uint32_t fd    = drw_factor((s_drw.control2 & (uint32_t)k_drw_c2_bdf) != 0U,
                                    (s_drw.control2 & (uint32_t)k_drw_c2_bdi) != 0U,
                                    src_a);

  const uint32_t r = drw_mix(drw_chan(s_drw.color1, (uint32_t)k_drw_r_shift),
                             drw_chan(dst, (uint32_t)k_drw_r_shift),
                             fs,
                             fd);
  const uint32_t g = drw_mix(drw_chan(s_drw.color1, (uint32_t)k_drw_g_shift),
                             drw_chan(dst, (uint32_t)k_drw_g_shift),
                             fs,
                             fd);
  const uint32_t b = drw_mix(drw_chan(s_drw.color1, 0U), drw_chan(dst, 0U), fs, fd);
  const uint32_t a = drw_out_alpha(src_a, drw_chan(dst, (uint32_t)k_drw_a_shift));

  return (a << (uint32_t)k_drw_a_shift) | (r << (uint32_t)k_drw_r_shift) |
         (g << (uint32_t)k_drw_g_shift) | b;
}

/** @brief Narrow an ARGB8888 result to the programmed framebuffer format. */
static uint32_t drw_pack(uint32_t argb, uint32_t bpp)
{
  if (bpp == (uint32_t)k_drw_bpp_32) {
    return argb;
  }
  const uint32_t a = drw_chan(argb, (uint32_t)k_drw_a_shift);
  if (bpp == (uint32_t)k_drw_bpp_8) {
    return a;
  }
  const uint32_t r = drw_chan(argb, (uint32_t)k_drw_r_shift);
  const uint32_t g = drw_chan(argb, (uint32_t)k_drw_g_shift);
  const uint32_t b = drw_chan(argb, 0U);
  const uint32_t lo =
    (s_drw.control2 >> (uint32_t)k_drw_c2_wfmt_lo_pos) & (uint32_t)k_drw_c2_wfmt_lo_mask;
  if (lo == (uint32_t)k_drw_wfmt_argb4444) {
    return ((a >> (uint32_t)k_drw_nibble_shift) << (uint32_t)k_drw_argb4444_a_pos) |
           ((r >> (uint32_t)k_drw_nibble_shift) << (uint32_t)k_drw_g_shift) |
           ((g >> (uint32_t)k_drw_nibble_shift) << (uint32_t)k_drw_nibble_shift) |
           (b >> (uint32_t)k_drw_nibble_shift);
  }
  return ((r >> (uint32_t)k_drw_r565_shift) << (uint32_t)k_drw_rgb565_r_pos) |
         ((g >> (uint32_t)k_drw_g565_shift) << (uint32_t)k_drw_rgb565_g_pos) |
         (b >> (uint32_t)k_drw_r565_shift);
}

/**
 * @brief Decide whether this render is one the model reproduces faithfully.
 *
 * @param[in] w Bounding box width in pixels, from SIZE.
 * @param[in] h Bounding box height in pixels, from SIZE.
 * @return @c true when the render may proceed.
 * @retval false Domain dark, geometry unprogrammed, or an unmodelled path.
 *
 * @par MC/DC:
 * A chain of independent single-condition rejects. Vectors: powered +
 * geometry programmed + no limiter + no FB cache + solid source -> true
 * (control); each reject condition flipped alone -> false, proving each
 * independently determines the outcome.
 */
static bool drw_render_modelled(uint32_t w, uint32_t h)
{
  if (!board_pdctr_graphics_powered()) {
    return false; /* Dark domain: writes are lost on silicon. */
  }
  if ((w == 0U) || (h == 0U) || (s_drw.pitch == 0U) || (s_drw.origin == 0U)) {
    return false; /* Nothing programmed yet -- e.g. the init ORIGIN write. */
  }
  if ((s_drw.control & (uint32_t)k_drw_control_limiter_mask) != 0U) {
    return false; /* Limiter semantics uncharacterised -- see file header. */
  }
  if ((s_drw.cachectl & (uint32_t)k_drw_cachectl_cenablefx) != 0U) {
    return false; /* FB cache holds the pixels -- see file header. */
  }
  const uint32_t sources = (uint32_t)k_drw_c2_patternenable | (uint32_t)k_drw_c2_textureenable;
  return (s_drw.control2 & sources) == 0U;
}

/** @brief Rasterize the programmed bounding box into emulated memory. */
static void drw_render(uc_engine* uc)
{
  const uint32_t w = s_drw.size & (uint32_t)k_drw_size_w_mask;
  const uint32_t h = (s_drw.size >> (uint32_t)k_drw_size_h_pos) & (uint32_t)k_drw_size_w_mask;

  if (!drw_render_modelled(w, h)) {
    if ((w != 0U) && (h != 0U) && (s_drw.origin != 0U)) {
      s_drw.skipped++;
    }
    return;
  }

  const uint32_t bpp = drw_bpp();
  for (uint32_t row = 0U; row < h; ++row) {
    const uint64_t line =
      (uint64_t)s_drw.origin + ((uint64_t)row * (uint64_t)s_drw.pitch * (uint64_t)bpp);
    for (uint32_t col = 0U; col < w; ++col) {
      const uint64_t addr = line + ((uint64_t)col * (uint64_t)bpp);
      uint32_t       dst  = 0U;
      (void)uc_mem_read(uc, addr, &dst, (size_t)bpp);
      const uint32_t out = drw_pack(drw_shade(dst), bpp);
      (void)uc_mem_write(uc, addr, &out, (size_t)bpp);
    }
  }
  s_drw.renders++;
  s_drw.last_w = w;
  s_drw.last_h = h;
}

/** @brief MMIO read inside the DRW window. */
static uint64_t drw_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_drw_base;
  switch (off) {
    case (uint64_t)k_drw_off_control:
      /* HUM Ch 62.2.5 "STATUS: Status Control Register" p 3695. The model
       * rasterizes synchronously on the ORIGIN write, so the engine is never
       * observably busy and latches no IRQ or bus error: STATUS reads
       * all-zero and ra8_drw_wait_idle succeeds on its first poll.
       *
       * This MUST return explicitly rather than fall through to HWREVISION:
       * the two registers are read aliases at different offsets carrying
       * different values, and sharing an arm made ra8_drw_wait_idle poll
       * STATUS, see the revision stamp, never observe idle, and time out. */
      return 0U;
    case (uint64_t)k_drw_off_control2:
      /* HUM Ch 62.2.6 "HWREVISION: Hardware Revision Register" p 3696.
       * Reads 0 while the graphics power domain is gated (HUM Ch 11.2.14
       * p 452) and 0x0FBE0107 once it is powered -- both bench-captured, so
       * firmware that skips the power-domain bring-up sees the dead register
       * here exactly as it does on the bench. */
      if (!board_pdctr_graphics_powered()) {
        return 0U;
      }
      return (uint64_t)k_drw_hwrevision_value;
    default:
      /* Every other DRW register is write-only on silicon (HUM Ch 62.2) and
       * the driver shadows what it needs, so a deterministic zero is both
       * harmless and faithful. */
      return 0U;
  }
}

/** @brief Latch one shadowed DRW register; returns false for unknown offsets. */
static bool drw_latch(uint64_t off, uint32_t val)
{
  switch (off) {
    case (uint64_t)k_drw_off_control:
      s_drw.control = val;
      return true;
    case (uint64_t)k_drw_off_control2:
      s_drw.control2 = val;
      return true;
    case (uint64_t)k_drw_off_color1:
      s_drw.color1 = val;
      return true;
    case (uint64_t)k_drw_off_color2:
      s_drw.color2 = val;
      return true;
    case (uint64_t)k_drw_off_size:
      s_drw.size = val;
      return true;
    case (uint64_t)k_drw_off_pitch:
      s_drw.pitch = val;
      return true;
    case (uint64_t)k_drw_off_cachectl:
      s_drw.cachectl = val;
      return true;
    default:
      /* Limiter, texture, CLUT and IRQ registers are accepted and discarded:
       * the modelled render path does not consume them. */
      return false;
  }
}

/** @brief Apply one display-list register write; ORIGIN triggers a render. */
static void drw_dlr_exec_reg(uc_engine* uc, uint32_t index, uint32_t val)
{
  const uint64_t off = (uint64_t)index * (uint64_t)k_drw_dlr_bytes_per_word;
  if (off == (uint64_t)k_drw_off_origin) {
    /* Same trigger as a CPU ORIGIN write: anchor the box and rasterize. */
    s_drw.origin = val;
    drw_render(uc);
    return;
  }
  (void)drw_latch(off, val);
}

/**
 * @brief Execute a display list the DLR fetches from emulated memory.
 *
 * @details
 * Mirrors the DRW display-list reader for the encoding the ra8_drw builder
 * emits: single-register entries and end-of-list words (see ::drw_dlr_t). Each
 * ORIGIN write inside the list rasterizes exactly as a CPU ORIGIN write does,
 * so a clear+fill list reproduces the byte-identical framebuffer the bench
 * produced for issue #247. The fetch loop is bounded by ::k_drw_dlr_max_words
 * (NASA Rule 2). Any tag encoding the builder never emits stops the list --
 * unmodelled rather than guessed.
 *
 * @param[in] uc        Emulator engine (framebuffer + display-list memory).
 * @param[in] dlist_addr Display-list base address (DLISTSTART value).
 */
static void drw_run_dlist(uc_engine* uc, uint32_t dlist_addr)
{
  uint32_t addr = dlist_addr;
  for (uint32_t fetched = 0U; fetched < (uint32_t)k_drw_dlr_max_words; ++fetched) {
    uint32_t tag = 0U;
    (void)uc_mem_read(uc, (uint64_t)addr, &tag, sizeof(tag));
    addr += (uint32_t)k_drw_dlr_bytes_per_word;

    if ((tag & (uint32_t)k_drw_dlr_b1_boundary) != 0U) {
      /* One-index entry: byte 0 is a register, one value word follows. */
      const uint32_t index = tag & (uint32_t)k_drw_dlr_index_mask;
      if (index == (uint32_t)k_drw_dlr_idx_dliststart) {
        return; /* display-list jump / stop */
      }
      uint32_t val = 0U;
      (void)uc_mem_read(uc, (uint64_t)addr, &val, sizeof(val));
      addr += (uint32_t)k_drw_dlr_bytes_per_word;
      drw_dlr_exec_reg(uc, index, val);
      continue;
    }
    if ((tag & (uint32_t)k_drw_dlr_index_mask) == (uint32_t)k_drw_dlr_eol_low) {
      /* End-of-list word: byte 1 is the argument. The builder emits only the
       * pipeline+cache wait (2, a synchronous no-op here); every other value
       * terminates. */
      const uint32_t arg =
        (tag >> (uint32_t)k_drw_dlr_eol_arg_pos) & (uint32_t)k_drw_dlr_index_mask;
      if (arg == (uint32_t)k_drw_dlr_arg_wait) {
        continue;
      }
      return; /* terminate */
    }
    return; /* unmodelled multi-index packing: stop, do not guess */
  }
}

/** @brief MMIO write inside the DRW window; ORIGIN or DLISTSTART renders. */
static void drw_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)size;
  const uint64_t off = addr - (uint64_t)k_drw_base;
  const uint32_t val = (uint32_t)value;

  /* While the graphics power domain is gated the block is unpowered and every
   * write is LOST on silicon (HUM Ch 11.2.14 p 452), so the shadow must not
   * latch it either -- otherwise firmware that skips the bring-up would
   * render here and not on the bench. */
  if (!board_pdctr_graphics_powered()) {
    return;
  }

  if (off == (uint64_t)k_drw_off_origin) {
    /* HUM Ch 62.2.31 p 3705: writing ORIGIN both anchors the bounding box and
     * TRIGGERS the render. */
    s_drw.origin = val;
    drw_render(uc);
    return;
  }
  if (off == (uint64_t)k_drw_off_dliststart) {
    /* HUM Ch 62.2.32 p 3705: writing DLISTSTART kicks the display-list reader,
     * which executes the list from RAM (issue #247 loop-stable clear+fill). */
    drw_run_dlist(uc, val);
    return;
  }
  (void)drw_latch(off, val);
}

/** @brief Reset the DRW model to power-on state. */
static void drw_reset(void)
{
  s_drw = (drw_state_t){};
}

/** @brief End-of-run summary line. */
static void drw_report(void)
{
  if ((s_drw.renders == 0U) && (s_drw.skipped == 0U)) {
    return;
  }
  if (s_drw.skipped != 0U) {
    /* Loud: an app reached a DRW configuration this model declines to
     * reproduce, so the emulated framebuffer is NOT what the bench shows. */
    (void)fprintf(stderr,
                  "  DRW           : %u boxes rasterized (last %ux%u), "
                  "%u DECLINED (limiter / FB-cache / textured path unmodelled)\n",
                  s_drw.renders,
                  s_drw.last_w,
                  s_drw.last_h,
                  s_drw.skipped);
    return;
  }
  (void)fprintf(stderr,
                "  DRW           : %u boxes rasterized (last %ux%u)\n",
                s_drw.renders,
                s_drw.last_w,
                s_drw.last_h);
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
[[gnu::constructor]] static void drw_block_register(void)
{
  board_periph_register_block(&k_drw_block);
}
