/**
 * @file ra8_glcdc.c
 * @brief Graphics LCD Controller driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Driver for the RA8D2 GLCDC block. Programmes the background
 * stage, both graphics layers (UI overlay over background image),
 * the alpha blender, output-stage brightness/contrast/dithering,
 * and double-buffered CLUTs. Closely mirrors the init order in
 * FSP's ``r_glcdc_graphics_layer_set`` and the shadow-CLUT swap
 * pattern in FSP's ``R_GLCDC_ClutEdit`` /
 * ``R_GLCDC_ColorPaletteUpdate``. Every register access carries a
 * HUM Ch 63 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @param[in] mode See header declaration for direction and constraints.
 * @return ``ra8_err_t`` error code (or void if the signature returns void).
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 *
 * @param[in] layer See header declaration for direction and constraints.
 * @param[in] plane See header declaration for direction and constraints.
 *
 * @param[in] argb See header declaration for direction and constraints.
 *
 * @param[in] global_alpha See header declaration for direction and constraints.
 *
 * @param[in] cfg See header declaration for direction and constraints.
 */

#include "ra8_glcdc.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_glcdc_regs.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"

static const char* s_tag = "GLCDC";

/* =============================================================================
 * Local constants
 * =============================================================================
 */

/** @brief CLUT entry count (16-bit address space). */
typedef enum : uint32_t {
  k_glcdc_clut_size = 0x10000UL, /**< GLCDC CLUT size. */
} glcdc_clut_t;

typedef enum : uint8_t {
  k_ra8_glcdc_layer_count   = 2U,  /**< Number of graphics layers. */
  k_ra8_glcdc_layer1        = 0U,  /**< Layer index for GR[0].     */
  k_ra8_glcdc_dispsel_below = 1U,  /**< AB1.DISPSEL = lower layer. */
  k_ra8_glcdc_dispsel_above = 2U,  /**< AB1.DISPSEL = upper layer. */
  k_ra8_glcdc_arcdef_shift  = 16U, /**< AB7.ARCDEF bit shift.      */
  k_ra8_glcdc_clutsel_shift = 16U, /**< CLUTINT.SEL bit position.  */
  k_ra8_glcdc_pdtha_shift   = 8U,  /**< PDTHA.SEL bit shift.       */
  k_ra8_glcdc_word_shift    = 16U, /**< 16-bit field shift.        */
  k_ra8_glcdc_g_shift       = 16U, /**< Green-channel byte shift.  */
  k_ra8_glcdc_b_shift       = 8U,  /**< Blue-channel byte shift.   */
} ra8_glcdc_priv_const_t;

typedef enum : uint16_t {
  k_ra8_glcdc_clut_entries = 256U, /**< Entries per CLUT plane. */
} ra8_glcdc_clut_const_t;

typedef enum : uint32_t {
  k_ra8_glcdc_clutsel_mask = 0x10000UL, /**< CLUTINT.SEL bit mask. */
  k_ra8_glcdc_arcon_mask   = 0x1000UL,  /**< AB1.ARCON bit mask.   */
} ra8_glcdc_priv_mask_t;

/* =============================================================================
 * Initialisation
 * =============================================================================
 */

/* =============================================================================
 * RA8D2-specific power + clock bring-up that FSP's R_GLCDC_Open expects to
 * have been done by the BSP before it touches any GLCDC register.  Without
 * these two steps the entire GLCDC register window at 0x40342000 silently
 * drops reads and writes (the chip's Graphics power domain is off and
 * LCDCLK is parked on the default MOCO).
 *
 *   PDCTRGD.PDDE = 0   power on the Graphics power domain
 *   LCDCKCR = PLL2P    LCDCLK = 240 MHz on PLL2P
 *
 * Both writes are PRCR-protected (LPM and CGC groups respectively).
 * =============================================================================
 */

/**
 * @brief Power on the GLCDC graphics power domain and route LCDCLK.
 *
 * @details
 * Unlocks PRCR (PRC0 for CGC, PRC1 for LPM), clears PDCTRGD to power
 * on the Graphics domain, waits for PDPGSF status, then switches
 * LCDCKCR to PLL1R via the SREQ / SRDY handshake and enables HOCO.
 * Required once at boot before any GLCDC register is touched -- the
 * peripheral bus returns garbage until PDCTRGD clears.
 *
 * @pre PRCR is currently locked (default reset state).
 * @pre PLL1 has been brought up by ra8_cgc_init so PLL1R is available
 *      as a source for LCDCKCR.
 * @post PDCTRGD = 0 (Graphics domain powered).
 * @post LCDCKCR is sourcing LCDCLK from PLL1R / 4.
 *
 * @note Single-threaded init context; not safe to call from IRQ.
 * @since 0.1.0
 */
/* HUM Ch 11.2.1 "PRCR : Protect Register" p 440 -- PRC0 (bit 0)
 * unlocks CGC; PRC1 (bit 1) unlocks LPM. Key = 0xA5xx. */
enum : uintptr_t {
  k_addr_prcr       = 0x4001E3FAUL, /**< Address prcr.       */
  k_addr_pdctrgd    = 0x4001E110UL, /**< Address pdctrgd.    */
  k_addr_lcdckdivcr = 0x4001E05EUL, /**< Address lcdckdivcr. */
  k_addr_lcdckcr    = 0x4001E05FUL, /**< Address lcdckcr.    */
  k_addr_hococr     = 0x4001E036UL, /**< Address hococr.     */
};
enum : uint16_t {
  k_prcr_unlock_lpm = 0xA502U, /**< Prcr unlock lpm. */
  k_prcr_unlock_cgc = 0xA501U, /**< Prcr unlock cgc. */
  k_prcr_relock     = 0xA500U, /**< Prcr relock.     */
};
enum : uint8_t {
  k_pdctrgd_on      = 0x00U, /**< Pdctrgd on.                        */
  k_pdctrgd_pdcsf   = 0x40U, /**< bit 6: control-in-progress status. */
  k_pdctrgd_pdpgsf  = 0x80U, /**< bit 7: gated status.               */
  k_pdctrgd_status  = 0xC0U, /**< both status bits.                  */
  k_lcdck_sreq      = 0x40U, /**< Lcdck sreq.                        */
  k_lcdck_srdy_mask = 0x80U, /**< Lcdck srdy mask.                   */
  k_lcdck_sel_pll1r = 0x08U, /**< HUM 9.2.58 LCDCKSEL[3:0] = 1000b.  */
  k_lcdckdivcr_div4 = 0x02U, /**< LCDCLK = PLL1R / 4 = 100 MHz.      */
  k_hococr_run      = 0x00U, /**< Hococr run.                        */
};

/**
 * @brief Enable HOCO via PRC0-protected HOCOCR.
 *
 * @details
 * HUM Ch 9.2.34 HOCOCR.HCSTP = 0 starts the HOCO. FSP bsp_clock_init
 * keeps HOCO enabled even on PLL1-sourced systems because the
 * graphics power domain's analogue trim logic still requires it.
 *
 * @pre PRCR is locked (default reset state).
 * @pre Caller is in single-threaded init context.
 * @post HOCOCR.HCSTP = 0 (HOCO running).
 * @post PRCR is relocked.
 *
 * @note Not thread-safe; touches CGC MMIO under PRC0 unlock.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_hoco_enable(void)
{
  volatile uint16_t* const prcr   = (volatile uint16_t*)k_addr_prcr;
  volatile uint8_t* const  hococr = (volatile uint8_t*)k_addr_hococr;
  *prcr                           = (uint16_t)k_prcr_unlock_cgc;
  *hococr                         = (uint8_t)k_hococr_run;
  *prcr                           = (uint16_t)k_prcr_relock;
}

/**
 * @brief Power on the Graphics power domain via PRC1-protected PDCTRGD.
 *
 * @details
 * HUM Ch 11.2.16 PDCTRGD power-on sequence (mirrors FSP bsp_clock_init):
 *   1. wait PDCTRGD status == PDPGSF (fully gated, idle)
 *   2. write PDCTRGD = 0 (PDDE = 0 -> request power on)
 *   3. wait PDCTRGD status == 0 (fully on, transition complete)
 * Skipping the second wait races the power-up transition and makes
 * downstream MMIO writes vanish silently.
 *
 * @pre PRCR is currently relocked.
 * @pre HOCO has been re-enabled by internal_hoco_enable().
 * @post Graphics power domain is fully on.
 * @post PRCR is relocked.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_graphics_domain_power_on(void)
{
  volatile uint16_t* const prcr    = (volatile uint16_t*)k_addr_prcr;
  volatile uint8_t* const  pdctrgd = (volatile uint8_t*)k_addr_pdctrgd;
  *prcr                            = (uint16_t)k_prcr_unlock_lpm;

  /* Wait for "fully gated" state before issuing the power-on request. */
  for (uint32_t i = 0U; i < k_glcdc_clut_size; i++) {
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    /* Host MMIO fault seam: tests drive the retry / exhaustion legs. */
    if (ra8_fake_mmio_wait_eval(
          pdctrgd,
          i,
          ((*pdctrgd & (uint8_t)k_pdctrgd_status) == (uint8_t)k_pdctrgd_pdpgsf))) {
      break;
    }
#else
    if ((*pdctrgd & (uint8_t)k_pdctrgd_status) == (uint8_t)k_pdctrgd_pdpgsf) {
      break;
    }
#endif
  }
  *pdctrgd = (uint8_t)k_pdctrgd_on;
  /* Wait for "fully on" state -- both PDCSF and PDPGSF must clear. */
  for (uint32_t i = 0U; i < k_glcdc_clut_size; i++) {
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    if (ra8_fake_mmio_wait_eval(pdctrgd, i, ((*pdctrgd & (uint8_t)k_pdctrgd_status) == 0U))) {
      break;
    }
#else
    if ((*pdctrgd & (uint8_t)k_pdctrgd_status) == 0U) {
      break;
    }
#endif
  }
  *prcr = (uint16_t)k_prcr_relock;
}

/**
 * @brief Switch LCDCLK source to PLL1R / 4 via the SREQ/SRDY handshake.
 *
 * @details
 * HUM Ch 9.2.58 LCDCKCR change handshake:
 *   SREQ=1 -> wait SRDY=1 -> set LCDCKDIVCR -> set LCDCKSEL with SREQ=1
 *   -> drop SREQ -> wait SRDY=0. PRC0 (CGC) gates these writes.
 *
 * @pre Graphics power domain is on.
 * @pre PLL1 is running and PLL1R is a valid LCDCLK source.
 * @post LCDCKCR.LCDCKSEL = PLL1R; LCDCKDIVCR = /4 (100 MHz).
 * @post PRCR is relocked.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_lcdclk_switch_pll1r(void)
{
  volatile uint16_t* const prcr       = (volatile uint16_t*)k_addr_prcr;
  volatile uint8_t* const  lcdckdivcr = (volatile uint8_t*)k_addr_lcdckdivcr;
  volatile uint8_t* const  lcdckcr    = (volatile uint8_t*)k_addr_lcdckcr;

  *prcr    = (uint16_t)k_prcr_unlock_cgc;
  *lcdckcr = (uint8_t)k_lcdck_sreq;
  for (uint32_t i = 0U; i < k_glcdc_clut_size; i++) {
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    /* Host MMIO fault seam: tests drive the retry / exhaustion legs. */
    if (ra8_fake_mmio_wait_eval(lcdckcr, i, ((*lcdckcr & (uint8_t)k_lcdck_srdy_mask) != 0U))) {
      break;
    }
#else
    if ((*lcdckcr & (uint8_t)k_lcdck_srdy_mask) != 0U) {
      break;
    }
#endif
  }
  *lcdckdivcr = (uint8_t)k_lcdckdivcr_div4;
  *lcdckcr    = (uint8_t)(k_lcdck_sreq | k_lcdck_sel_pll1r);
  *lcdckcr    = (uint8_t)k_lcdck_sel_pll1r;
  for (uint32_t i = 0U; i < k_glcdc_clut_size; i++) {
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    if (ra8_fake_mmio_wait_eval(lcdckcr, i, ((*lcdckcr & (uint8_t)k_lcdck_srdy_mask) == 0U))) {
      break;
    }
#else
    if ((*lcdckcr & (uint8_t)k_lcdck_srdy_mask) == 0U) {
      break;
    }
#endif
  }
  *prcr = (uint16_t)k_prcr_relock;
}

/**
 * @brief Bring up the GLCDC graphics power domain and clock tree.
 *
 * @details
 * Orchestrates the three-step sequence required before any GLCDC register
 * outside of MSTPCRC is writable on RA8D2:
 *   1. internal_hoco_enable()             -- analogue trim clock
 *   2. internal_graphics_domain_power_on()-- PDCTRGD power-on
 *   3. internal_lcdclk_switch_pll1r()     -- LCDCKCR -> PLL1R / 4
 * Runs on every build: the raw SYSC addresses fall inside the host
 * test's fake-mmap peripheral window and the status polls consult the
 * ra8_fake_mmio seam, so the real sequence executes on host too.
 *
 * @pre Caller is in single-threaded init context with IRQs masked or not
 *      yet enabled.
 * @pre PLL1 is running and PLL1R is a valid LCDCLK source (HUM 9.2.58).
 * @post Graphics power domain is fully on (PDCTRGD status == 0).
 * @post LCDCKCR.LCDCKSEL = PLL1R, LCDCKDIVCR = /4, PRCR relocked.
 *
 * @note Not thread-safe; touches CGC + LPM MMIO under PRC0/PRC1 unlocks.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_graphics_power_on(void)
{
  internal_hoco_enable();
  internal_graphics_domain_power_on();
  internal_lcdclk_switch_pll1r();
}

/* =============================================================================
 * Bit positions / shifts that FSP's r_glcdc.c uses verbatim.  Pulled from
 * the FSP private header constants ("GLCDC_PRV_*_SHIFT" / "_MASK") -- not
 * copied as code, just referenced for the register field layout.
 *
 *   OUT_SET.FORMAT[13:12]    output pixel format
 *   OUT_PDTHA.FORM[17:16]    dither output format (must match OUT_SET)
 *   FLM6.FORMAT[30:28]       framebuffer pixel format
 *   FLM3.LNOFF[31:16]        line stride in bytes
 *   FLM5.LNNUM[26:16]        lines per frame minus 1
 *   FLM5.DATANUM[15:0]       64-byte AXI bursts per line minus 1
 *   AB3.GRCHS[26:16]/W[10:0] graphics layer H position + width
 *   AB2.GRCVS[26:16]/W[10:0] graphics layer V position + height
 *   AB7.ARCDEF[23:16]        constant alpha for alpha-rect
 *   BG_HSIZE.HP[26:16]       BG active-pixel start position
 *   BG_VSIZE.VP[26:16]       BG active-line start position
 *   BG_SYNC HP / VP min = 1  (HUM 63 p 3760)
 * =============================================================================
 */
typedef enum : uint32_t {
  k_glcdc_shift_high       = 16U, /**< GLCDC shift high.               */
  k_glcdc_shift_flm6_fmt   = 28U, /**< GLCDC shift flm6 fmt.           */
  k_glcdc_shift_outset_fmt = 12U, /**< GLCDC shift outset fmt.         */
  k_glcdc_shift_pdtha_fmt  = 16U, /**< GLCDC shift pdtha fmt.          */
  k_glcdc_shift_arcdef     = 16U, /**< GLCDC shift arcdef.             */
  k_glcdc_axi_burst_bytes  = 64U, /**< GLCDC axi burst bytes.          */
  k_glcdc_bpp_rgb565       = 2U,  /**< GLCDC bpp rgb565.               */
  k_glcdc_sync_pos_min     = 1U,  /**< HUM 63 BG_SYNC.HP / VP min = 1. */
} ra8_glcdc_layout_priv_t;

/* SEL[2:0] values for TCON_STXx2.SEL (HUM 63 p 3805) */
typedef enum : uint32_t {
  k_glcdc_sel_stva = 0U, /**< GLCDC sel stva. */
  k_glcdc_sel_stvb = 1U, /**< GLCDC sel stvb. */
  k_glcdc_sel_stha = 2U, /**< GLCDC sel stha. */
  k_glcdc_sel_de   = 7U, /**< GLCDC sel de.   */
} ra8_glcdc_tcon_sel_t;

/* GR.AB1.DISPSEL[1:0] (HUM 63 p 3744 + FSP enum):
 *   1 = TRANSPARENT  -- layer hidden, lower layer (BG) shows through
 *   2 = NON_TRANSPARENT -- this layer's pixels are output */
typedef enum : uint32_t {
  k_glcdc_dispsel_transparent     = 1U, /**< GLCDC dispsel transparent.     */
  k_glcdc_dispsel_non_transparent = 2U, /**< GLCDC dispsel non transparent. */
} ra8_glcdc_dispsel_priv_t;

/* AB7.ARCDEF[23:16] alpha constants. */
typedef enum : uint32_t {
  k_glcdc_alpha_opaque = 0xFFUL, /**< Fully opaque (constant alpha). */
} ra8_glcdc_alpha_priv_t;

/* Panel timing is supplied by the caller via ra8_glcdc_config_t.timing -- it is
 * accessory data, not a peripheral property, so the panel's values (e.g. the
 * EK-RA8D2 ER-TFT070-6) live in the board BSP (libs/ra8_board_ek_ra8d2), not in
 * this driver. */

/* OUT_SET.FORMAT[1:0] codes for output bus. */
typedef enum : uint32_t {
  k_glcdc_out_set_rgb888 = 0U, /**< GLCDC out set rgb888. */
  k_glcdc_out_set_rgb666 = 1U, /**< GLCDC out set rgb666. */
  k_glcdc_out_set_rgb565 = 2U, /**< GLCDC out set rgb565. */
} ra8_glcdc_out_set_fmt_t;

/* OUT_PDTHA.FORM[1:0] codes -- must match OUT_SET.FORMAT for non-serial. */
typedef enum : uint32_t {
  k_glcdc_pdtha_rgb888 = 0U, /**< GLCDC pdtha rgb888. */
  k_glcdc_pdtha_rgb666 = 1U, /**< GLCDC pdtha rgb666. */
  k_glcdc_pdtha_rgb565 = 2U, /**< GLCDC pdtha rgb565. */
} ra8_glcdc_pdtha_fmt_t;

/**
 * @brief Panel timing parameters shared by the per-stage helpers.
 *
 * @details Holds the horizontal and vertical timing derived from the
 * caller-supplied ra8_glcdc_config_t.timing.  Computed once in
 * `internal_panel_program` and threaded through the TCON, BG, GR1,
 * GR2, and OUT helpers so each stage uses the same numbers.
 */
typedef struct {
  uint32_t h_active; /**< Active pixels per line.                  */
  uint32_t h_back;   /**< Back-porch pixel count.                  */
  uint32_t h_sync;   /**< HSYNC pulse width (pixel clocks).        */
  uint32_t h_total;  /**< Line period = active + back + sync + fp. */
  uint32_t v_active; /**< Active lines per frame.                  */
  uint32_t v_back;   /**< Vertical back porch (lines).             */
  uint32_t v_sync;   /**< VSYNC pulse width (lines).               */
  uint32_t v_total;  /**< Frame period (lines).                    */
} ra8_glcdc_panel_timing_t;

/**
 * @brief Program TCON sync-signal registers for parallel-RGB output.
 *
 * @details Sets STVA/STHA pulse widths and routes STVA -> TCON0,
 * STHA -> TCON1, DE -> TCON2 with the polarity inversions that the
 * panel expects.  Mirrors FSP `r_glcdc_sync_signal_set`.
 *
 * @param[in] t Panel timing parameters; only `h_*` and `v_*` widths
 *              and the back-porch values are read.
 *
 * @pre internal_graphics_power_on has been called.
 * @pre GLCDC is held in SWRST so writes go to shadow registers.
 * @post TCON shadow registers contain the panel's sync timing.
 * @post TCON.DE polarity is set to active-high.
 *
 * @note Single-threaded init context only.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_program_tcon(const ra8_glcdc_panel_timing_t* t)
{
  *ra8_glcdc_reg32(k_ra8_glcdc_off_out_clkphase) = 0U; /* all rising edge */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_tcon_tim)     = 0U;

  /* STVA -> LCD_TCON0 (VSYNC, active-low: INV=1). */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_tcon_stva1) = t->v_sync;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_tcon_stva2) = (uint32_t)k_glcdc_sel_stva | (1UL << 4);

  /* STHA pulse width, DE -> LCD_TCON2 (active-high: INV=0). */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_tcon_stha1) = t->h_sync;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_tcon_stha2) = (uint32_t)k_glcdc_sel_de;

  /* STVB -> LCD_TCON1 (HSYNC routed via STVB2.SEL = STHA, active-low). */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_tcon_stvb1) =
    ((uint32_t)(t->v_sync + t->v_back) << k_glcdc_shift_high) | t->v_active;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_tcon_stvb2) = (uint32_t)k_glcdc_sel_stha | (1UL << 4);

  /* STHB encodes horizontal DE strobe; TCON3 unused on this board. */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_tcon_sthb1) =
    ((uint32_t)(t->h_sync + t->h_back) << k_glcdc_shift_high) | t->h_active;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_tcon_sthb2) = 0U;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_tcon_de)    = 0U;
}

/**
 * @brief Program the GLCDC background-plane registers.
 *
 * @details Sets BG period, sync position, and active area so the
 * BG plane drives a full-frame solid colour from BG_BGC.  Mirrors
 * FSP `r_glcdc_background_screen_set`.
 *
 * @param[in] t Panel timing parameters used to compute the BG area
 *              and frame period.
 *
 * @pre internal_program_tcon has been called.
 * @pre GLCDC is held in SWRST so writes go to shadow registers.
 * @post BG plane is programmed for `t->h_active x t->v_active`.
 * @post BG_BGC is cleared to zero (callers set it via
 *       `ra8_glcdc_set_background_color`).
 *
 * @note Single-threaded init context only.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_program_bg(const ra8_glcdc_panel_timing_t* t)
{
  const uint32_t bg_h_pos                  = t->h_back + k_glcdc_sync_pos_min; /* FSP: back+1 */
  const uint32_t bg_v_pos                  = t->v_back + k_glcdc_sync_pos_min;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_bg_per) = (t->v_total << k_glcdc_shift_high) | t->h_total;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_bg_sync) =
    ((uint32_t)k_glcdc_sync_pos_min << k_glcdc_shift_high) | (uint32_t)k_glcdc_sync_pos_min;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_bg_hsize) = (bg_h_pos << k_glcdc_shift_high) | t->h_active;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_bg_vsize) = (bg_v_pos << k_glcdc_shift_high) | t->v_active;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_bg_bgc)   = 0U;
}

/**
 * @brief Program graphics layer 1 (top blend layer).
 *
 * @details Configures GR1's framebuffer pointer, format, AXI burst
 * count, panel position/size, and alpha.  DISPSEL=transparent and
 * FLMRD=0 so by default GR1 contributes nothing to the output --
 * callers that want layer-1 pixels fill the framebuffer themselves
 * and flip FLMRD=1.
 *
 * @param[in] t          Panel timing.
 * @param[in] fb_addr    GR1 framebuffer base address.
 * @param[in] fb_format  Pixel format code for FLM6.FORMAT[30:28].
 * @param[in] fb_w       Framebuffer width in pixels (and layer width on the panel).
 * @param[in] fb_h       Framebuffer height in pixels (and layer height on the panel).
 *
 * @pre internal_program_bg has been called.
 * @pre `fb_addr` points to allocated framebuffer memory (only
 *      accessed once FLMRD is flipped to 1 by the caller).
 * @post GR1 is hidden (DISPSEL=transparent, FLMRD=0, alpha=0).
 * @post All other GR1 registers are programmed for full-panel
 *       layer dimensions.
 *
 * @note Single-threaded init context only.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_program_gr1(const ra8_glcdc_panel_timing_t* t,
                                 uintptr_t                       fb_addr,
                                 uint16_t                        fb_format,
                                 uint16_t                        fb_w,
                                 uint16_t                        fb_h)
{
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_fmt)   = ((uint32_t)fb_format << k_glcdc_shift_flm6_fmt);
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_saddr) = (uint32_t)fb_addr;

  /* FLM3.LNOFF (line stride) + FLM5 (active line / AXI-burst count)
   * track the FRAMEBUFFER dimensions, not the panel.  This lets a
   * small SRAM framebuffer occupy only a portion of the panel
   * (rest is filled by the BG plane via BG_BGC). */
  const uint32_t line_bytes                  = (uint32_t)fb_w * (uint32_t)k_glcdc_bpp_rgb565;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_flm3) = (line_bytes << k_glcdc_shift_high);

  const uint32_t datanum = (line_bytes / (uint32_t)k_glcdc_axi_burst_bytes) - 1U;
  const uint32_t lnnum   = (uint32_t)fb_h - 1U;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_line) = (lnnum << k_glcdc_shift_high) | datanum;

  /* Layer position on the panel: start at the back-porch corner
   * (top-left of the active area) so a small framebuffer renders
   * at the panel's origin and the rest stays BG-plane. */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_size) = (t->h_back << k_glcdc_shift_high) | (uint32_t)fb_w;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_ab2)  = (t->v_back << k_glcdc_shift_high) | (uint32_t)fb_h;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_ab4)  = (t->v_back << k_glcdc_shift_high) | (uint32_t)fb_h;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_ab5)  = (t->h_back << k_glcdc_shift_high) | (uint32_t)fb_w;

  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_ab1)   = (uint32_t)k_glcdc_dispsel_transparent;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_flmrd) = 0U;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_ab7)   = 0UL;
}

/**
 * @brief Program graphics layer 2 (lower blend layer).
 *
 * @details CRITICAL bring-up finding (2026-05-10): the RA8D2 GLCDC
 * output stage composes its final pixel from `BG x GR2 x GR1`.  If
 * GR2 is left in reset state (all zeros, FLMRD=0, AB1.DISPSEL=0),
 * the pipeline emits solid black even when BG_BGC is non-zero and
 * GR1.DISPSEL=transparent -- the unconfigured lower layer silently
 * masks the BG plane.  This helper configures GR2 to the same
 * dimensions as the panel with DISPSEL=transparent, alpha=0, and
 * FLMRD=0, so it occupies its pipeline slot without contributing
 * pixels.  Mirrors FSP `r_glcdc_open` which configures both layers
 * in a `for (layer = 0..1)` loop.
 *
 * @param[in] t Panel timing parameters used for GR2 dimensions.
 *
 * @pre internal_program_gr1 has been called.
 * @pre GLCDC is held in SWRST so writes go to shadow registers.
 * @post GR2 is hidden (DISPSEL=transparent, FLMRD=0, alpha=0) but
 *       its pipeline slot is configured.
 * @post `ra8_glcdc_start` must still assert `GR2.VEN` to commit.
 *
 * @note Single-threaded init context only.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_program_gr2(const ra8_glcdc_panel_timing_t* t)
{
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_size)  = (t->h_back << k_glcdc_shift_high) | t->h_active;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab2)   = (t->v_back << k_glcdc_shift_high) | t->v_active;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab4)   = (t->v_back << k_glcdc_shift_high) | t->v_active;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab5)   = (t->h_back << k_glcdc_shift_high) | t->h_active;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab7)   = 0UL;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_ab1)   = (uint32_t)k_glcdc_dispsel_transparent;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_flmrd) = 0U;
}

/**
 * @brief Program the OUT stage (output format, brightness, contrast).
 *
 * @details Selects RGB888 output (drives all 24 LCD_DATAn pins; the
 * chip auto-expands RGB565 framebuffer pixels internally) and
 * programs identity brightness/contrast.  Mirrors FSP
 * `r_glcdc_output_block_set` + the default brightness/contrast path
 * in `r_glcdc_brightness_correction`.
 *
 * @pre internal_program_gr2 has been called.
 * @pre GLCDC is held in SWRST so writes go to shadow registers.
 * @post Output bus format is RGB888 to match the parallel-RGB panel.
 * @post Brightness/contrast are configured for identity transform.
 *
 * @note Single-threaded init context only.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_program_out(void)
{
  enum : uint32_t {
    k_bright_mid   = 512UL, /**< Bright mid.   */
    k_contrast_mid = 128UL, /**< Contrast mid. */
  };
  *ra8_glcdc_reg32(k_ra8_glcdc_off_out_set) =
    ((uint32_t)k_glcdc_out_set_rgb888 << k_glcdc_shift_outset_fmt);
  *ra8_glcdc_reg32(k_ra8_glcdc_off_panel_dtha) =
    ((uint32_t)k_glcdc_pdtha_rgb888 << k_glcdc_shift_pdtha_fmt);
  *ra8_glcdc_reg32(k_ra8_glcdc_off_out_bright1) = k_bright_mid;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_out_bright2) = (k_bright_mid << 16) | k_bright_mid;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_out_contrast) =
    (k_contrast_mid << 16) | (k_contrast_mid << 8) | k_contrast_mid;
}

/**
 * @brief Program every GLCDC register required to drive the panel.
 *
 * @details Thin orchestrator that builds the panel-timing struct from
 * the caller-supplied @p tm and calls the five per-stage helpers in
 * the order FSP's `r_glcdc_open` uses: TCON, BG, GR1, GR2, OUT.
 *
 * @param[in] fb_addr    Framebuffer base address for GR1.
 * @param[in] fb_format  GR1 framebuffer pixel format.
 * @param[in] fb_w       GR1 framebuffer width (pixels).
 * @param[in] fb_h       GR1 framebuffer height (pixels).
 * @param[in] tm         Panel RGB timing (from the board BSP).
 *
 * @pre internal_graphics_power_on has been called.
 * @pre GLCDC peripheral clock is active (MSTPCRD enabled).
 * @post All GLCDC stage shadow registers are programmed.
 * @post Pipeline is configured for BG-only output until a caller
 *       flips GR1.FLMRD=1 with a populated framebuffer.
 *
 * @note Single-threaded init context only.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_panel_program(uintptr_t                 fb_addr,
                                   uint16_t                  fb_format,
                                   uint16_t                  fb_w,
                                   uint16_t                  fb_h,
                                   const ra8_glcdc_timing_t* tm)
{
  const ra8_glcdc_panel_timing_t t = {
    .h_active = (uint32_t)tm->h_active,
    .h_back   = (uint32_t)tm->h_back,
    .h_sync   = (uint32_t)tm->h_sync,
    .h_total =
      (uint32_t)tm->h_active + (uint32_t)tm->h_front + (uint32_t)tm->h_back + (uint32_t)tm->h_sync,
    .v_active = (uint32_t)tm->v_active,
    .v_back   = (uint32_t)tm->v_back,
    .v_sync   = (uint32_t)tm->v_sync,
    .v_total =
      (uint32_t)tm->v_active + (uint32_t)tm->v_front + (uint32_t)tm->v_back + (uint32_t)tm->v_sync,
  };
  internal_program_tcon(&t);
  internal_program_bg(&t);
  internal_program_gr1(&t, fb_addr, fb_format, fb_w, fb_h);
  internal_program_gr2(&t);
  internal_program_out();
}

/* HUM Ch 63 "Graphics LCD Controller (GLCDC)" p 3744 */
ra8_err_t ra8_glcdc_init(const ra8_glcdc_config_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");

  /* HUM Ch 11.2.14 "PDCTRGD" p 452 + HUM Ch 9.2.58 "LCDCKCR" p 373.
   * Power on the Graphics domain and route LCDCLK to PLL2P.  Without
   * this the GLCDC peripheral bus is unreachable. */
  internal_graphics_power_on();

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_glcdc);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "glcdc_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 63 "BG_EN.SWRST" p 3759 + FSP r_glcdc_open step 2.  Single
   * write of SWRST=1.  FSP uses a bit-field write (read-modify-write
   * leaving other bits at their reset values, which are all 0) -- a
   * plain `*reg = SWRST_MASK` is equivalent on a register that was
   * just out of reset. */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_bg_en) = (uint32_t)(1UL << 16);

  /* HUM Ch 63 "SYSCNT_PANEL_CLK" p 3813.  CLKSEL=LCDCLK, CLKEN=1,
   * DCDR=/5 -> 240 MHz / 5 = 48 MHz pixel clock. */
  enum : uint32_t {
    k_panel_clk_clksel_lcdclk = 1U << 8, /**< Panel clk clksel lcdclk. */
    k_panel_clk_clken         = 1U << 6, /**< Panel clk clken.         */
    k_panel_clk_dcdr_div2     = 0x02U,   /**< Panel clk dcdr div2.     */
  };
  /* LCDCLK = 100 MHz (PLL1R / 4), DCDR /2 -> pixel clock = 50 MHz.
   * Matches the LVGL EK-RA8D2 reference for the ER-TFT070-6 panel. */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_panel_clk) = (uint32_t)k_panel_clk_clksel_lcdclk |
                                                (uint32_t)k_panel_clk_clken |
                                                (uint32_t)k_panel_clk_dcdr_div2;

  /* Programme every other panel-side register in the FSP-documented order. */
  internal_panel_program(cfg->framebuffer_addr,
                         (uint16_t)cfg->format,
                         cfg->width_px,
                         cfg->height_px,
                         &cfg->timing);

  ra8_log_info_val(s_tag, "glcdc_init fb", cfg->framebuffer_addr);
  return k_ra8_ok;
}

ra8_err_t ra8_glcdc_start(bool enable)
{
  enum : uint32_t {
    k_bit_en    = 1U << 0,  /**< Bit en.    */
    k_bit_ven   = 1U << 8,  /**< Bit ven.   */
    k_bit_swrst = 1U << 16, /**< Bit swrst. */
  };

  if (!enable) {
    /* Drop everything to disabled but keep SWRST high so register
     * reads keep working (writing SWRST=0 re-enters reset). */
    *ra8_glcdc_reg32(k_ra8_glcdc_off_bg_en)   = (uint32_t)k_bit_swrst;
    *ra8_glcdc_reg32(k_ra8_glcdc_off_sys_cfg) = 0U;
    return k_ra8_ok;
  }

  /* Match FSP's R_GLCDC_Start exactly: bit-field-style sequential writes
   * so SWRST stays high while we add VEN then EN.  Using read-modify-
   * write rather than full-word writes avoids momentarily clearing
   * SWRST or putting the controller in an undefined transition. */
  uint32_t bg_en = *ra8_glcdc_reg32(k_ra8_glcdc_off_bg_en);

  /* Step 1: VEN = 1 -- enable register value reflection on next VS. */
  bg_en |= (uint32_t)k_bit_ven;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_bg_en) = bg_en;

  /* Step 2: EN = 1 -- background plane operation enable. */
  bg_en |= (uint32_t)k_bit_en;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_bg_en) = bg_en;

  /* Step 3: SYSCNT.DTCTEN.VPOSDTC = 1 -- line-detect IRQ source. */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_sys_cfg) = 1U;

  /* Step 4: OUT_VLATCH.VEN = 1 -- commit the OUT_SET / OUT_PDTHA /
   * brightness / contrast shadow registers to the output stage's
   * working registers.  Without this the output-bus format we wrote
   * in init never reaches the pixel pipeline. */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_out_vlatch) = 1U;

  /* Step 5: GR1.VEN + GR2.VEN -- commit graphics-layer shadow regs.
   *
   * Both layers MUST be VEN-asserted, even if only one carries a
   * framebuffer.  See bring-up finding in internal_panel_program:
   * without GR2's shadow registers committed, the output pipeline's
   * lower-layer slot stays in reset and the panel sees solid black.
   * FSP r_glcdc_open writes PVEN on every layer it configured. */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_en) = 1U;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr2_en) = 1U;

  return k_ra8_ok;
}

/* =============================================================================
 * Output stage: dithering / brightness / contrast
 * =============================================================================
 */

ra8_err_t ra8_glcdc_set_dithering(ra8_glcdc_dither_mode_t mode)
{
  uint32_t sel;
  switch (mode) {
    case k_ra8_dither_off: {
      sel = 0U;
      break;
    }
    case k_ra8_dither_truncate: {
      sel = 2U;
      break;
    }
    case k_ra8_dither_2x2: {
      sel = 3U;
      break;
    }
    default: {
      return k_ra8_err_invalid_arg;
    }
  }
  /* HUM Ch 63 "OUT_PDTHA.SEL" p 3744 */ /* panel dither selector. */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_panel_dtha) = (sel << k_ra8_glcdc_pdtha_shift);
  return k_ra8_ok;
}

ra8_err_t ra8_glcdc_set_brightness(uint8_t r, uint8_t g, uint8_t b)
{
  /* HUM Ch 63 "OUT_BRIGHT1.BRTG" p 3744 */ /* green offset. */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_out_bright1) = (uint32_t)g;

  /* HUM Ch 63 "OUT_BRIGHT2.BRTB/BRTR" p 3744 */ /* blue + red offsets. */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_out_bright2) =
    ((uint32_t)b << k_ra8_glcdc_word_shift) | (uint32_t)r;
  return k_ra8_ok;
}

ra8_err_t ra8_glcdc_set_contrast(uint8_t r, uint8_t g, uint8_t b)
{
  /* HUM Ch 63 "OUT_CONTRAST.CONTG/CONTB/CONTR" p 3744 */
  *ra8_glcdc_reg32(k_ra8_glcdc_off_out_contrast) =
    ((uint32_t)g << k_ra8_glcdc_g_shift) | ((uint32_t)b << k_ra8_glcdc_b_shift) | (uint32_t)r;
  return k_ra8_ok;
}

/* =============================================================================
 * lifecycle + IRQ + power transition
 * =============================================================================
 */

static ra8_glcdc_event_fn_t s_glcdc_fn;
static void*                s_glcdc_ctx;

ra8_err_t ra8_glcdc_deinit(void)
{
  *ra8_glcdc_reg32(k_ra8_glcdc_off_sys_cfg) = 0U;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_bg_en)   = 0U;
  *ra8_glcdc_reg32(k_ra8_glcdc_off_gr1_en)  = 0U;
  s_glcdc_fn                                = nullptr;
  s_glcdc_ctx                               = nullptr;
  return ra8_mstp_disable(k_ra8_mstp_glcdc);
}

ra8_err_t ra8_glcdc_get_status(uint32_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  *out_mask = *ra8_glcdc_reg32(k_ra8_glcdc_off_sys_stat);
  return k_ra8_ok;
}

ra8_err_t ra8_glcdc_clear_status(uint32_t mask)
{
  volatile uint32_t* reg = ra8_glcdc_reg32(k_ra8_glcdc_off_sys_stat);
  *reg                   = *reg & ~mask;
  return k_ra8_ok;
}

ra8_err_t ra8_glcdc_attach_handler(ra8_glcdc_event_fn_t fn, void* ctx)
{
  s_glcdc_fn  = fn;
  s_glcdc_ctx = ctx;
  return k_ra8_ok;
}

RA8_ISR_SAFE
void ra8_glcdc_dispatch(void)
{
  volatile uint32_t*         reg  = ra8_glcdc_reg32(k_ra8_glcdc_off_sys_stat);
  const uint32_t             mask = *reg;
  const ra8_glcdc_event_fn_t fn   = s_glcdc_fn;
  void* const                ctx  = s_glcdc_ctx;
  *reg                            = 0U;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

ra8_err_t ra8_glcdc_enter_stop(void)
{
  *ra8_glcdc_reg32(k_ra8_glcdc_off_sys_cfg) = 0U;
  return ra8_mstp_disable(k_ra8_mstp_glcdc);
}

ra8_err_t ra8_glcdc_exit_stop(void)
{
  return ra8_mstp_enable(k_ra8_mstp_glcdc);
}
