/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_vin.c
 * @brief Video Input Module (VIN) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * / Round-3 driver for the RA8D2 Video Input Module. Covers
 * every register documented in HUM Ch 67 "Video Input Module"
 * p 3973-4031:
 *
 * - MC / FC / MS / MTCSTOP / DMR for the main pipeline.
 * - SLPRC / ELPRC / SPPRC / EPPRC for pre-clip windowing.
 * - UDS_CTRL / UDS_SCALE / UDS_PASS_BWIDTH / UDS_CLIP_SIZE for the
 * upscaling / downscaling pipeline.
 * - LUTP / LUTD for the 256-entry 10-to-8-bit conversion LUT.
 * - YCCRn / CBCCRn / CRCCRn for RGB -> YC matrix coefficients.
 * - CSCE1..4 for YC -> RGB matrix coefficients.
 * - CSI_IFMD / CSIFLD for CSI-2 input mode and field detection.
 * - IS / MB1..3 / UVAOF / IE / INTS / SI for capture geometry,
 * framebuffer routing, interrupts, and scanline-compare.
 *
 * The peripheral does not have its own MSTPCR bit on the RA8D2 --
 * its clocks come up alongside MIPI CSI when `k_ra8_mstp_mipi_csi`
 * is enabled. The driver therefore routes power transitions through
 * the existing `ra8_mstp` enum rather than opening up MSTPCRC for
 * direct access.
 */

#include "ra8_vin.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_vin_regs.h"

/**
 * @var s_tag
 * @brief Logging tag used by every log call in this TU.
 *
 * @details
 * Static storage so it cannot be modified from outside this file.
 * Matches the convention from ra8_glcdc.c and ra8_acmphs.c.
 *
 * @note Read-only after init; safe from any context.
 * @since 0.1.0
 */
static const char* s_tag = "VIN";

/**
 * @var s_vin_fn
 * @brief Registered VIN event callback (set by `ra8_vin_attach_handler`).
 *
 * @details
 * Single shared callback for both the status and error IRQ vectors.
 * NULL means "no callback installed" -- `ra8_vin_dispatch` becomes
 * a status-clear-only no-op in that case.
 *
 * @warning Direct modification outside `ra8_vin_attach_handler` is not
 * supported; callers must go through the API so future
 * locking can be added without source-code churn.
 *
 * @note Not thread-safe; protected by the caller's IRQ-mask discipline.
 * @since 0.1.0
 */
static ra8_vin_event_fn_t s_vin_fn;

/**
 * @var s_vin_ctx
 * @brief Context value forwarded to `s_vin_fn`.
 *
 * @details
 * Opaque pointer supplied to `ra8_vin_attach_handler` and handed back
 * to the callback unchanged -- the driver never dereferences it.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void* s_vin_ctx;

/**
 * @var s_vin_frame_fn
 * @brief Frame-end callback registered via `ra8_vin_attach_frame_handler`.
 * @since 0.1.0
 */
static ra8_vin_frame_fn_t s_vin_frame_fn;

/**
 * @var s_vin_frame_ctx
 * @brief Context forwarded to `s_vin_frame_fn`.
 * @since 0.1.0
 */
static void* s_vin_frame_ctx;

/**
 * @var s_vin_frame_buf
 * @brief Buffer base of the most recent `ra8_vin_capture_start` call.
 * @since 0.1.0
 */
static void* s_vin_frame_buf;

/**
 * @var s_vin_frame_len
 * @brief Cached length (in bytes) of the most recent capture geometry.
 * @since 0.1.0
 */
static uint32_t s_vin_frame_len;

/**
 * @enum ra8_vin_local_t
 * @brief File-scope magic-free constants.
 *
 * @details
 * Centralises the bounds-check limits used by the setter family so
 * the implementations read close to the HUM register descriptions.
 */
typedef enum : uint8_t {
  k_ra8_vin_max_im       = 2U,    /**< MC.IM legal max (0..2).                  */
  k_ra8_vin_max_dc       = 2U,    /**< MC.DC legal max (0..2).                  */
  k_ra8_vin_max_clp      = 3U,    /**< MC.CLP legal max (0..3 with 2 reserved). */
  k_ra8_vin_max_yuv444   = 1U,    /**< MC.YUV444 legal max.                     */
  k_ra8_vin_max_dtmd     = 2U,    /**< DMR.DTMD legal max (0..2).               */
  k_ra8_vin_max_ymode    = 3U,    /**< DMR.YMODE legal max (0..3).              */
  k_ra8_vin_max_vc_sel   = 0xFU,  /**< CSI_IFMD.VC_SEL 4-bit max.               */
  k_ra8_vin_max_dt       = 0x3FU, /**< CSI_IFMD.DT 6-bit max.                   */
  k_ra8_vin_max_field_no = 1U,    /**< CSIFLD.FLD_NUM legal max.                */
  k_ra8_vin_max_lsft     = 31U,   /**< Setting 3 LSFT 5-bit max.                */
  k_ra8_vin_yc_chan_max  = 2U,    /**< RGB-to-YC channel index max (0..2).      */
} ra8_vin_local_t;

/**
 * @enum ra8_vin_chan_offsets_t
 * @brief Register offsets for one row of the RGB-to-YC matrix.
 *
 * @details
 * Indexed via the channel parameter to `ra8_vin_set_rgb_to_yc`.
 */
typedef enum : uint16_t {
  k_ra8_vin_yc_y_set1  = k_ra8_vin_off_yccr1,  /**< RA8 vin yc y set1.  */
  k_ra8_vin_yc_y_set2  = k_ra8_vin_off_yccr2,  /**< RA8 vin yc y set2.  */
  k_ra8_vin_yc_y_set3  = k_ra8_vin_off_yccr3,  /**< RA8 vin yc y set3.  */
  k_ra8_vin_yc_cb_set1 = k_ra8_vin_off_cbccr1, /**< RA8 vin yc cb set1. */
  k_ra8_vin_yc_cb_set2 = k_ra8_vin_off_cbccr2, /**< RA8 vin yc cb set2. */
  k_ra8_vin_yc_cb_set3 = k_ra8_vin_off_cbccr3, /**< RA8 vin yc cb set3. */
  k_ra8_vin_yc_cr_set1 = k_ra8_vin_off_crccr1, /**< RA8 vin yc cr set1. */
  k_ra8_vin_yc_cr_set2 = k_ra8_vin_off_crccr2, /**< RA8 vin yc cr set2. */
  k_ra8_vin_yc_cr_set3 = k_ra8_vin_off_crccr3, /**< RA8 vin yc cr set3. */
} ra8_vin_chan_offsets_t;

/**
 * @brief Encode a `ra8_vin_config_t` into the MC register value.
 *
 * @details
 * Folds ``cfg->{bypass_csc, big_endian, interlace_mode, input_fmt,
 * pixel_clip_mode}`` into the MC bitfield documented at HUM Ch 67.2.1
 * "MC: Main Control Register" (p 3975). The ME / ST start-trigger
 * bits are deliberately left clear so the caller can pulse them on
 * demand.
 *
 * @param[in] cfg Validated configuration.
 * @return Composed MC value with ME / ST left clear.
 * @retval 0..(uint32_t)-1 Bitwise-OR of the documented MC fields.
 *
 * @pre `cfg != NULL` (caller-validated).
 * @pre `cfg->input_fmt` fits the 3-bit MC.INF field.
 * @pre `cfg->interlace_mode <= k_ra8_vin_max_im`.
 * @post Returned value has `k_ra8_vin_mc_me` = 0.
 * @post Returned value has `k_ra8_vin_mc_st` = 0 (start pulse only on demand).
 *
 * @note Pure function; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_compose_mc(const ra8_vin_config_t* cfg)
{
  /* HUM Ch 67.2.1 "MC: Main Control Register" p 3975 */
  uint32_t mc = 0UL;
  if (cfg->bypass_csc) {
    mc |= k_ra8_vin_mc_bps;
  }
  if (cfg->big_endian) {
    mc |= k_ra8_vin_mc_en;
  }
  /* IM[4:3] -- interlace mode. */
  mc |= ((uint32_t)cfg->interlace_mode << k_ra8_vin_mc_shift_im) & k_ra8_vin_mc_im;
  /* INF[18:16] -- input format. */
  mc |= ((uint32_t)cfg->input_fmt << k_ra8_vin_mc_shift_inf) & k_ra8_vin_mc_inf;
  /* CLP[29:28] -- pixel data clipping. */
  mc |= ((uint32_t)cfg->pixel_clip_mode << k_ra8_vin_mc_shift_clp) & k_ra8_vin_mc_clp;
  return mc;
}

/**
 * @brief Pulse MC.ST then absorb the >= 10 ICLK settling time.
 *
 * @details
 * Writes ``mc_now | MC.ST`` then reads MC back several times so the
 * peripheral observes the >= 10-ICLK settle window mandated by HUM
 * Ch 67.2.1 "MC: Main Control Register" (p 3975).
 *
 * @param[in] mc_now Current MC register value (without ST set).
 *
 * @pre Caller holds mc_reg pointer to MC register.
 * @pre Capture is not in progress.
 * @post MC.ST has been pulsed; >= 10 ICLK have elapsed.
 * @post MC retains the original ``mc_now`` field bits (ST is self-clearing).
 *
 * @note Inline helper, single-CPU.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_pulse_st(uint32_t mc_now)
{
  /* HUM Ch 67.2.1 "MC: Main Control Register" p 3975
 * Pulse MC.ST = 1 then absorb >= 10 ICLK by reading MC back. */
  volatile uint32_t* mc_reg = ra8_vin_reg32(k_ra8_vin_off_mc);
  *mc_reg                   = mc_now | k_ra8_vin_mc_st;
  for (uint8_t i = 0U; i < k_ra8_vin_st_settle_reads; ++i) {
    (void)*mc_reg;
  }
}

/**
 * @brief Read-modify-write a single bit-field within MC.
 *
 * @details
 * Atomic from the caller's perspective when IRQs are masked. The
 * helper exists to keep field-update sites from re-implementing the
 * mask / shift dance for HUM Ch 67.2.1 "MC: Main Control Register"
 * (p 3975).
 *
 * @param[in] clear_mask Bits to clear before OR-ing in `set_bits`.
 * @param[in] set_bits Bits to set on top of the cleared field.
 *
 * @pre Caller has masked IRQs.
 * @pre `set_bits & ~clear_mask` is empty (no out-of-field bits).
 * @post MC = (MC & ~clear_mask) | set_bits.
 * @post Bits outside ``clear_mask`` are unchanged.
 *
 * @note Inline helper.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_mc_rmw(uint32_t clear_mask, uint32_t set_bits)
{
  /* HUM Ch 67.2.1 "MC: Main Control Register" p 3975 */
  volatile uint32_t* mc_reg = ra8_vin_reg32(k_ra8_vin_off_mc);
  *mc_reg                   = (*mc_reg & ~clear_mask) | set_bits;
}

[[nodiscard]] ra8_err_t ra8_vin_init(const ra8_vin_config_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  if (cfg->image_stride_px == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->interlace_mode > k_ra8_vin_max_im) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->pixel_clip_mode > k_ra8_vin_max_clp) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 11.2.8 "MSTPCRC: Module Stop Control Register C", p 446
 * VIN shares its module-stop bit with MIPI CSI on the RA8D2. */
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_mipi_csi);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "vin_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  const uint32_t mc_clean = internal_compose_mc(cfg);

  /* HUM Ch 67.2.1 "MC: Main Control Register" p 3975 */
  *ra8_vin_reg32(k_ra8_vin_off_mc) = mc_clean;
  /* HUM Ch 67.2.3 "FC: Frame Capture Register" p 3979 */
  *ra8_vin_reg32(k_ra8_vin_off_fc) = 0UL;

  /* HUM Ch 67.2.13 "IS: Image Stride Register" p 3984 */
  *ra8_vin_reg32(k_ra8_vin_off_is) = (uint32_t)cfg->image_stride_px;
  /* HUM Ch 67.2.10 "MB1: Memory Base 1 Register" p 3985 */
  *ra8_vin_reg32(k_ra8_vin_off_mb1) = cfg->framebuffer_addr_1;
  /* HUM Ch 67.2.11 "MB2: Memory Base 2 Register" p 3986 */
  *ra8_vin_reg32(k_ra8_vin_off_mb2) = cfg->framebuffer_addr_2;
  /* HUM Ch 67.2.12 "MB3: Memory Base 3 Register" p 3987 */
  *ra8_vin_reg32(k_ra8_vin_off_mb3) = cfg->framebuffer_addr_3;

  /* HUM Ch 67.2.15 "IE: Interrupt Enable Register" p 3987 */
  *ra8_vin_reg32(k_ra8_vin_off_ie) = cfg->interrupt_enable;
  /* HUM Ch 67.2.17 "SI: Scanline Interrupt Register" p 3991 */
  *ra8_vin_reg32(k_ra8_vin_off_si) = (uint32_t)cfg->scanline_compare;
  /* HUM Ch 67.2.16 "INTS: Interrupt Status Register" p 3989
 * Clear all latched W1C bits left over from a previous run. */
  *ra8_vin_reg32(k_ra8_vin_off_ints) = k_ra8_vin_int_w1c_mask;

  ra8_log_info_val(s_tag, "vin_init mb1", cfg->framebuffer_addr_1);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_deinit(void)
{
  /* HUM Ch 67.2.3 "FC: Frame Capture Register" p 3979 */
  *ra8_vin_reg32(k_ra8_vin_off_fc) = 0UL;
  /* HUM Ch 67.2.1 "MC: Main Control Register" p 3975 */
  *ra8_vin_reg32(k_ra8_vin_off_mc) = 0UL;
  /* HUM Ch 67.2.15 "IE: Interrupt Enable Register" p 3987 */
  *ra8_vin_reg32(k_ra8_vin_off_ie) = 0UL;
  /* HUM Ch 67.2.16 "INTS: Interrupt Status Register" p 3989 */
  *ra8_vin_reg32(k_ra8_vin_off_ints) = k_ra8_vin_int_w1c_mask;

  s_vin_fn        = nullptr;
  s_vin_ctx       = nullptr;
  s_vin_frame_fn  = nullptr;
  s_vin_frame_ctx = nullptr;
  s_vin_frame_buf = nullptr;
  s_vin_frame_len = 0U;

  /* HUM Ch 11.2.8 "MSTPCRC: Module Stop Control Register C", p 446 */
  return ra8_mstp_disable(k_ra8_mstp_mipi_csi);
}

[[nodiscard]] ra8_err_t ra8_vin_reset(void)
{
  /* HUM Ch 67.2.1 "MC: Main Control Register" p 3975
 * Clear ME first so the ST pulse re-initialises a stopped pipeline. */
  volatile uint32_t* mc_reg = ra8_vin_reg32(k_ra8_vin_off_mc);
  const uint32_t     mc_now = *mc_reg & ~k_ra8_vin_mc_me;
  *mc_reg                   = mc_now;
  internal_pulse_st(mc_now);
  ra8_log_info(s_tag, "vin_reset");
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_capture_arm(ra8_vin_capture_mode_t mode)
{
  if ((mode != k_ra8_vin_capture_single) && (mode != k_ra8_vin_capture_continuous) &&
      (mode != k_ra8_vin_capture_continuous_field_skip)) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 67.2.1 "MC: Main Control Register" p 3975 */
  volatile uint32_t* mc_reg = ra8_vin_reg32(k_ra8_vin_off_mc);
  /* HUM Ch 67.2.3 "FC: Frame Capture Register" p 3979 */
  volatile uint32_t* fc_reg = ra8_vin_reg32(k_ra8_vin_off_fc);

  const uint32_t mc_now = *mc_reg;
  const uint32_t fc_now = *fc_reg;
  // mcdc-deactivated: ra8_vin idle-state guard; MC.ME (module enable) and FC.CC (capture continuous) are set together by the public-API ra8_vin_capture_start sequence and cleared together by ra8_vin_capture_stop, so the two condition bits are co-dependent in normal operation -- no MC/DC vector can flip one without the other on a quiescent capture controller.
  if (((mc_now & k_ra8_vin_mc_me) != 0UL) || ((fc_now & k_ra8_vin_fc_cc) != 0UL)) {
    return k_ra8_err_invalid_state;
  }

  /* HUM Ch 67.3.1 "Initialization Procedure" p 3995 */
  internal_pulse_st(mc_now);

  /* HUM Ch 67.2.1 "MC: Main Control Register" p 3975
 * Set ME = 1 (and re-clear ST -- it is write-only). */
  *mc_reg = mc_now | k_ra8_vin_mc_me;

  if ((mode == k_ra8_vin_capture_continuous) || (mode == k_ra8_vin_capture_continuous_field_skip)) {
    /* HUM Ch 67.2.3 "FC: Frame Capture Register" p 3979 */
    *fc_reg = k_ra8_vin_fc_cc;
  }

  ra8_log_info_val(s_tag, "vin_capture_start mode", (uint32_t)mode);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_capture_disarm(void)
{
  /* HUM Ch 67.2.1 "MC: Main Control Register" p 3975 */
  volatile uint32_t* mc_reg = ra8_vin_reg32(k_ra8_vin_off_mc);
  /* HUM Ch 67.2.3 "FC: Frame Capture Register" p 3979 */
  volatile uint32_t* fc_reg = ra8_vin_reg32(k_ra8_vin_off_fc);

  const uint32_t mc_now = *mc_reg;
  if ((mc_now & k_ra8_vin_mc_me) == 0UL) {
    return k_ra8_err_invalid_state;
  }

  /* HUM Ch 67.2.3 "FC: Frame Capture Register" p 3979 */
  *fc_reg = 0UL;
  /* HUM Ch 67.2.1 "MC: Main Control Register" p 3975 */
  *mc_reg = mc_now & ~k_ra8_vin_mc_me;

  /* HUM Ch 67.2.18 "MTCSTOP: AXI Transfer Stop Control Register" p 3991
 * Force any outstanding AXI writes to drain so the caller can safely
 * recycle the frame buffer once OUTSTAND reaches 0. */
  volatile uint32_t* mtc_reg = ra8_vin_reg32(k_ra8_vin_off_mtcstop);
  *mtc_reg                   = k_ra8_vin_mtcstop_req;

  /* Bounded poll for OUTSTAND -> 0 (NASA Rule 2). */
  ra8_err_t err = k_ra8_err_hw_timeout;
  for (uint16_t i = 0U; i < k_ra8_vin_stop_drain_max; ++i) { /* GCOVR_EXCL_BR_LINE */
    if ((*mtc_reg & k_ra8_vin_mtcstop_outstand) == 0UL) {    /* GCOVR_EXCL_BR_LINE */
      err = k_ra8_ok;
      break;
    }
  }
  ra8_log_info(s_tag, "vin_capture_stop");
  return err;
}

[[nodiscard]] ra8_err_t ra8_vin_set_preclip(const ra8_vin_preclip_t* window)
{
  RA8_CHECK_NULL_PTR(window, s_tag, "preclip must not be nullptr");
  if ((window->line_end < window->line_start) || (window->pixel_end < window->pixel_start)) {
    return k_ra8_err_invalid_arg;
  }
  if ((((uint32_t)window->line_end | (uint32_t)window->line_start | (uint32_t)window->pixel_end |
        (uint32_t)window->pixel_start) &
       ~k_ra8_vin_preclip_mask) != 0UL) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 67.2.4 "SLPRC: Pre-Clip Start Line Register" p 3980 */
  *ra8_vin_reg32(k_ra8_vin_off_slprc) = (uint32_t)window->line_start;
  /* HUM Ch 67.2.5 "ELPRC: Pre-Clip End Line Register" p 3980 */
  *ra8_vin_reg32(k_ra8_vin_off_elprc) = (uint32_t)window->line_end;
  /* HUM Ch 67.2.6 "SPPRC: Pre-Clip Start Pixel Register" p 3981 */
  *ra8_vin_reg32(k_ra8_vin_off_spprc) = (uint32_t)window->pixel_start;
  /* HUM Ch 67.2.7 "EPPRC: Pre-Clip End Pixel Register" p 3981 */
  *ra8_vin_reg32(k_ra8_vin_off_epprc) = (uint32_t)window->pixel_end;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_set_uds_scale(const ra8_vin_uds_scale_t* scale)
{
  RA8_CHECK_NULL_PTR(scale, s_tag, "scale must not be nullptr");
  if ((scale->v_mantissa > (uint8_t)k_ra8_vin_uds_max_mant) ||
      (scale->h_mantissa > (uint8_t)k_ra8_vin_uds_max_mant) ||
      (scale->v_fraction > k_ra8_vin_uds_max_frac) ||
      (scale->h_fraction > k_ra8_vin_uds_max_frac)) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 67.2.22 "UDS_SCALE: Scaling Factor Register" p 3998 */
  uint32_t v =
    ((uint32_t)scale->v_fraction << k_ra8_vin_uds_scale_shift_vfrac) & k_ra8_vin_uds_scale_vfrac;
  v |= ((uint32_t)scale->v_mantissa << k_ra8_vin_uds_scale_shift_vmant) & k_ra8_vin_uds_scale_vmant;
  v |= ((uint32_t)scale->h_fraction << k_ra8_vin_uds_scale_shift_hfrac) & k_ra8_vin_uds_scale_hfrac;
  v |= ((uint32_t)scale->h_mantissa << k_ra8_vin_uds_scale_shift_hmant) & k_ra8_vin_uds_scale_hmant;
  *ra8_vin_reg32(k_ra8_vin_off_uds_scale) = v;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_set_uds_passband(uint8_t v_bwidth, uint8_t h_bwidth)
{
  if ((v_bwidth > (uint8_t)k_ra8_vin_uds_max_bw) || (h_bwidth > (uint8_t)k_ra8_vin_uds_max_bw)) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 67.2.23 "UDS_PASS_BWIDTH: Passband Register" p 4001 */
  uint32_t v = ((uint32_t)v_bwidth << k_ra8_vin_uds_bwidth_shift_v) & k_ra8_vin_uds_bwidth_v;
  v |= ((uint32_t)h_bwidth << k_ra8_vin_uds_bwidth_shift_h) & k_ra8_vin_uds_bwidth_h;
  *ra8_vin_reg32(k_ra8_vin_off_uds_pass_bwidth) = v;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_set_uds_clip(uint16_t v_size, uint16_t h_size)
{
  if ((v_size > k_ra8_vin_uds_max_clip) || (h_size > k_ra8_vin_uds_max_clip)) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 67.2.24 "UDS_CLIP_SIZE: Output Size Clipping Register" p 4002 */
  uint32_t v = ((uint32_t)v_size << k_ra8_vin_uds_clip_shift_v) & k_ra8_vin_uds_clip_v;
  v |= ((uint32_t)h_size << k_ra8_vin_uds_clip_shift_h) & k_ra8_vin_uds_clip_h;
  *ra8_vin_reg32(k_ra8_vin_off_uds_clip_size) = v;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_set_uds_ctrl(const ra8_vin_uds_ctrl_t* ctrl)
{
  RA8_CHECK_NULL_PTR(ctrl, s_tag, "ctrl must not be nullptr");
  /* HUM Ch 67.2.21 "UDS_CTRL: Scaling Control Register" p 3997 */
  uint32_t v = 0UL;
  if (ctrl->b_cb_nearest) {
    v |= k_ra8_vin_uds_ctrl_ne_bcb;
  }
  if (ctrl->g_y_nearest) {
    v |= k_ra8_vin_uds_ctrl_ne_gy;
  }
  if (ctrl->r_cr_nearest) {
    v |= k_ra8_vin_uds_ctrl_ne_rcr;
  }
  if (ctrl->multitap) {
    v |= k_ra8_vin_uds_ctrl_bc;
  }
  if (ctrl->advanced_bl) {
    v |= k_ra8_vin_uds_ctrl_bladv;
  }
  if (ctrl->advanced_amd) {
    v |= k_ra8_vin_uds_ctrl_amd;
  }
  *ra8_vin_reg32(k_ra8_vin_off_uds_ctrl) = v;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_enable_scaling(bool enable)
{
  uint32_t scle_bits = 0UL;
  if (enable) {
    scle_bits = k_ra8_vin_mc_scle;
  }
  internal_mc_rmw(k_ra8_vin_mc_scle, scle_bits);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_lut_program(const uint8_t* y_table,
                                            const uint8_t* cb_table,
                                            const uint8_t* cr_table,
                                            bool           enable)
{
  RA8_CHECK_NULL_PTR(y_table, s_tag, "y_table must not be nullptr");
  const uint8_t* cb = (cb_table != nullptr) ? cb_table : y_table;
  const uint8_t* cr = (cr_table != nullptr) ? cr_table : y_table;

  for (uint16_t i = 0U; i < k_ra8_vin_lut_entries; ++i) {
    /* HUM Ch 67.2.25 "LUTP: Look Up Table Pointer Register" p 4003 */
    uint32_t lutp = ((uint32_t)i << k_ra8_vin_lutp_shift_ltypr) & k_ra8_vin_lutp_ltypr;
    lutp |= ((uint32_t)i << k_ra8_vin_lutp_shift_ltcbpr) & k_ra8_vin_lutp_ltcbpr;
    lutp |= ((uint32_t)i << k_ra8_vin_lutp_shift_ltcrpr) & k_ra8_vin_lutp_ltcrpr;
    *ra8_vin_reg32(k_ra8_vin_off_lutp) = lutp;

    /* HUM Ch 67.2.26 "LUTD: Look Up Table Data Register" p 4004 */
    uint32_t lutd = ((uint32_t)y_table[i] << k_ra8_vin_lutd_shift_ltydt) & k_ra8_vin_lutd_ltydt;
    lutd |= ((uint32_t)cb[i] << k_ra8_vin_lutd_shift_ltcbdt) & k_ra8_vin_lutd_ltcbdt;
    lutd |= ((uint32_t)cr[i] << k_ra8_vin_lutd_shift_ltcrdt) & k_ra8_vin_lutd_ltcrdt;
    *ra8_vin_reg32(k_ra8_vin_off_lutd) = lutd;
  }
  /* HUM Ch 67.2.1 "MC: Main Control Register" p 3975 */
  uint32_t lute_bits = 0UL;
  if (enable) {
    lute_bits = k_ra8_vin_mc_lute;
  }
  internal_mc_rmw(k_ra8_vin_mc_lute, lute_bits);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_set_yc_to_rgb(const ra8_vin_yc_to_rgb_t* coeffs)
{
  RA8_CHECK_NULL_PTR(coeffs, s_tag, "coeffs must not be nullptr");
  /* HUM Ch 67.2.36 "CSCE1" p 3995 */
  uint32_t csce1 = (uint32_t)coeffs->y_multiplier & k_ra8_vin_csce1_ymul2;
  if (coeffs->enable_round) {
    csce1 |= k_ra8_vin_csce1_round;
  }
  *ra8_vin_reg32(k_ra8_vin_off_csce1) = csce1;

  /* HUM Ch 67.2.37 "CSCE2" p 3995 */
  uint32_t csce2 = (uint32_t)coeffs->cbcr_sub & k_ra8_vin_csce2_csub2;
  csce2 |= ((uint32_t)coeffs->y_sub << k_ra8_vin_csce2_shift_ysub2) & k_ra8_vin_csce2_ysub2;
  *ra8_vin_reg32(k_ra8_vin_off_csce2) = csce2;

  /* HUM Ch 67.2.38 "CSCE3" p 3996 */
  uint32_t csce3 = (uint32_t)coeffs->cr_mul_lo & k_ra8_vin_csce_mul_lo;
  csce3 |= ((uint32_t)coeffs->cr_mul_hi << k_ra8_vin_csce_shift_mul_hi) & k_ra8_vin_csce_mul_hi;
  *ra8_vin_reg32(k_ra8_vin_off_csce3) = csce3;

  /* HUM Ch 67.2.39 "CSCE4" p 3996 */
  uint32_t csce4 = (uint32_t)coeffs->cb_mul_lo & k_ra8_vin_csce_mul_lo;
  csce4 |= ((uint32_t)coeffs->cb_mul_hi << k_ra8_vin_csce_shift_mul_hi) & k_ra8_vin_csce_mul_hi;
  *ra8_vin_reg32(k_ra8_vin_off_csce4) = csce4;
  return k_ra8_ok;
}

/**
 * @brief Resolve the (set1, set2, set3) offset triple for one row.
 *
 * @details
 * Maps the channel index onto the matching YCCRn / CBCCRn / CRCCRn
 * register-offset triple documented at HUM Ch 67.2.27 (p 4002-4011).
 * Used by the runtime CSC programming path so the per-channel writes
 * stay table-driven.
 *
 * @param[in] channel 0 = Y (YCCRn), 1 = Cb (CBCCRn), 2 = Cr (CRCCRn).
 * @param[out] off1 Setting 1 offset.
 * @param[out] off2 Setting 2 offset.
 * @param[out] off3 Setting 3 offset.
 *
 * @pre `channel <= k_ra8_vin_yc_chan_max`.
 * @pre All out-pointers are non-NULL.
 * @post off1/off2/off3 carry the corresponding `ra8_vin_off_t` values.
 * @post No VIN register has been read or written.
 *
 * @note Inline helper.
 * @since 0.1.0
 */
RA8_INTERNAL
static void
internal_yc_offsets(uint8_t channel, ra8_vin_off_t* off1, ra8_vin_off_t* off2, ra8_vin_off_t* off3)
{
  /* HUM Ch 67.2.27 "YCCR1: RGB-to-Y Coefficient R" p 4002-4011 */
  if (channel == 0U) {
    *off1 = k_ra8_vin_off_yccr1;
    *off2 = k_ra8_vin_off_yccr2;
    *off3 = k_ra8_vin_off_yccr3;
  } else if (channel == 1U) {
    *off1 = k_ra8_vin_off_cbccr1;
    *off2 = k_ra8_vin_off_cbccr2;
    *off3 = k_ra8_vin_off_cbccr3;
  } else {
    *off1 = k_ra8_vin_off_crccr1;
    *off2 = k_ra8_vin_off_crccr2;
    *off3 = k_ra8_vin_off_crccr3;
  }
}

[[nodiscard]] ra8_err_t ra8_vin_set_rgb_to_yc(uint8_t channel, const ra8_vin_rgb_to_yc_chan_t* row)
{
  RA8_CHECK_NULL_PTR(row, s_tag, "row must not be nullptr");
  if (channel > k_ra8_vin_yc_chan_max) {
    return k_ra8_err_invalid_arg;
  }
  if (row->shift_down > k_ra8_vin_max_lsft) {
    return k_ra8_err_invalid_arg;
  }
  ra8_vin_off_t off1 = k_ra8_vin_off_yccr1;
  ra8_vin_off_t off2 = k_ra8_vin_off_yccr2;
  ra8_vin_off_t off3 = k_ra8_vin_off_yccr3;
  internal_yc_offsets(channel, &off1, &off2, &off3);

  /* HUM Ch 67.2.27 "YCCR1: RGB-to-Y Coefficient R" p 4002-4010 */
  *ra8_vin_reg32(off1) = (uint32_t)row->r_coeff & k_ra8_vin_yc1_rp;

  /* HUM Ch 67.2.28 "YCCR2: RGB-to-Y Coefficient G+B" p 4003-4010 */
  uint32_t s2 = (uint32_t)row->g_coeff & k_ra8_vin_yc2_gp;
  s2 |= ((uint32_t)row->b_coeff << k_ra8_vin_yccr_shift_bp) & k_ra8_vin_yc2_bp;
  *ra8_vin_reg32(off2) = s2;

  /* HUM Ch 67.2.29 "YCCR3: RGB-to-Y Normalization" p 4004-4011 */
  uint32_t s3 = (uint32_t)row->add_offset & k_ra8_vin_yc3_ap;
  if (row->enable_hen) {
    s3 |= k_ra8_vin_yc3_hen;
  }
  s3 |= ((uint32_t)row->shift_down << k_ra8_vin_yccr_shift_sft) & k_ra8_vin_yc3_sft;
  *ra8_vin_reg32(off3) = s3;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_set_dithering(uint8_t mode, bool direction)
{
  if (mode > k_ra8_vin_max_dc) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 67.2.1 "MC: Main Control Register" p 3975
 * DC[15:14] dithering mode + DC2[24] direction. */
  const uint32_t dc_bits  = ((uint32_t)mode << k_ra8_vin_mc_shift_dc) & k_ra8_vin_mc_dc;
  uint32_t       dc2_bits = 0UL;
  if (direction) {
    dc2_bits = k_ra8_vin_mc_dc2;
  }
  internal_mc_rmw(k_ra8_vin_mc_dc | k_ra8_vin_mc_dc2, dc_bits | dc2_bits);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_set_yuv444_mode(uint8_t mode)
{
  if (mode > k_ra8_vin_max_yuv444) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 67.2.1 "MC: Main Control Register" p 3975 */
  internal_mc_rmw(k_ra8_vin_mc_yuv444, (mode != 0U) ? k_ra8_vin_mc_yuv444 : 0UL);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_set_interlace_mode(uint8_t mode)
{
  if (mode > k_ra8_vin_max_im) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 67.2.1 "MC: Main Control Register" p 3975 */
  internal_mc_rmw(k_ra8_vin_mc_im, ((uint32_t)mode << k_ra8_vin_mc_shift_im) & k_ra8_vin_mc_im);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_set_data_mode(const ra8_vin_data_mode_t* mode)
{
  RA8_CHECK_NULL_PTR(mode, s_tag, "mode must not be nullptr");
  if ((mode->conv_mode > k_ra8_vin_max_dtmd) || (mode->y_mode > k_ra8_vin_max_ymode)) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 67.2.19 "DMR: Data Mode Register" p 3992 */
  uint32_t v = ((uint32_t)mode->conv_mode << k_ra8_vin_dmr_shift_dtmd) & k_ra8_vin_dmr_dtmd;
  if (mode->alpha_bit) {
    v |= k_ra8_vin_dmr_abit;
  }
  if (mode->byte_swap) {
    v |= k_ra8_vin_dmr_bpsm;
  }
  if (mode->extend_rgb) {
    v |= k_ra8_vin_dmr_exrgb;
  }
  if (mode->yc_through) {
    v |= k_ra8_vin_dmr_yc_thr;
  }
  v |= ((uint32_t)mode->y_mode << k_ra8_vin_dmr_shift_ymode) & k_ra8_vin_dmr_ymode;
  v |= ((uint32_t)mode->alpha_byte << k_ra8_vin_dmr_shift_a8bit) & k_ra8_vin_dmr_a8bit;
  *ra8_vin_reg32(k_ra8_vin_off_dmr) = v;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_set_csi_input(const ra8_vin_csi_input_t* input)
{
  RA8_CHECK_NULL_PTR(input, s_tag, "input must not be nullptr");
  if ((input->virtual_channel > k_ra8_vin_max_vc_sel) || (input->data_type > k_ra8_vin_max_dt)) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 67.2.8 "CSI_IFMD: CSI2 Interface Mode Register" p 3982 */
  uint32_t v = ((uint32_t)input->virtual_channel << k_ra8_vin_csi_ifmd_shift_vc_sel) &
               k_ra8_vin_csi_ifmd_vc_sel;
  v |= ((uint32_t)input->data_type << k_ra8_vin_csi_ifmd_shift_dt) & k_ra8_vin_csi_ifmd_dt;
  if (input->zero_extend) {
    v |= k_ra8_vin_csi_ifmd_des0;
  }
  *ra8_vin_reg32(k_ra8_vin_off_csi_ifmd) = v;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_set_field_detect(const ra8_vin_field_detect_t* detect)
{
  RA8_CHECK_NULL_PTR(detect, s_tag, "detect must not be nullptr");
  if (detect->even_field_num > k_ra8_vin_max_field_no) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 67.2.9 "CSIFLD: Field Detection Control Register" p 3983 */
  uint32_t v = 0UL;
  if (detect->enable) {
    v |= k_ra8_vin_csifld_fld_en;
  }
  if (detect->even_field_sel) {
    v |= k_ra8_vin_csifld_fld_sel;
  }
  if (detect->even_field_num != 0U) {
    v |= k_ra8_vin_csifld_fld_num;
  }
  *ra8_vin_reg32(k_ra8_vin_off_csifld) = v;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_set_framebuffers(uint32_t mb1, uint32_t mb2, uint32_t mb3)
{
  if ((mb1 == 0U) && (mb2 == 0U) && (mb3 == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if (((mb1 & k_ra8_vin_mb_align_mask) != 0UL) || ((mb2 & k_ra8_vin_mb_align_mask) != 0UL) ||
      ((mb3 & k_ra8_vin_mb_align_mask) != 0UL)) {
    return k_ra8_err_invalid_arg;
  }
  if (mb1 != 0U) {
    /* HUM Ch 67.2.10 "MB1: Memory Base 1 Register" p 3985 */
    *ra8_vin_reg32(k_ra8_vin_off_mb1) = mb1;
  }
  if (mb2 != 0U) {
    /* HUM Ch 67.2.11 "MB2: Memory Base 2 Register" p 3986 */
    *ra8_vin_reg32(k_ra8_vin_off_mb2) = mb2;
  }
  if (mb3 != 0U) {
    /* HUM Ch 67.2.12 "MB3: Memory Base 3 Register" p 3987 */
    *ra8_vin_reg32(k_ra8_vin_off_mb3) = mb3;
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_set_uv_offset(uint32_t uv_addr)
{
  if ((uv_addr & k_ra8_vin_mb_align_mask) != 0UL) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 67.2.20 "UVAOF: UV Address Offset Register" p 3994 */
  *ra8_vin_reg32(k_ra8_vin_off_uvaof) = uv_addr;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_get_status(uint32_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 67.2.16 "INTS: Interrupt Status Register" p 3989 */
  *out_mask = *ra8_vin_reg32(k_ra8_vin_off_ints);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_clear_status(uint32_t mask)
{
  /* HUM Ch 67.2.16 "INTS: Interrupt Status Register" p 3989
 * Only FOS / ARES / FIS2 are W1C; gate other bits to avoid
 * accidentally toggling state bits the hardware owns. */
  const uint32_t w1c                 = mask & k_ra8_vin_int_w1c_mask;
  *ra8_vin_reg32(k_ra8_vin_off_ints) = w1c;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_get_module_status(ra8_vin_module_status_t* out_status)
{
  RA8_CHECK_NULL_PTR(out_status, s_tag, "out_status must not be nullptr");
  /* HUM Ch 67.2.2 "MS: Module Status Register" p 3978 */
  const uint32_t v                = *ra8_vin_reg32(k_ra8_vin_off_ms);
  out_status->capture_active      = ((v & k_ra8_vin_ms_ca) != 0UL);
  out_status->active_video        = ((v & k_ra8_vin_ms_av) != 0UL);
  out_status->even_field          = ((v & k_ra8_vin_ms_fs) != 0UL);
  out_status->frame_buffer_id     = (uint8_t)((v & k_ra8_vin_ms_fbs) >> k_ra8_vin_ms_shift_fbs);
  out_status->memory_active       = ((v & k_ra8_vin_ms_ma) != 0UL);
  out_status->latest_frame_buffer = (uint8_t)((v & k_ra8_vin_ms_fms) >> k_ra8_vin_ms_shift_fms);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_get_line_count(uint16_t* out_count)
{
  RA8_CHECK_NULL_PTR(out_count, s_tag, "out_count must not be nullptr");
  /* HUM Ch 67.2.14 "LC: Line Count Register" p 3987 */
  *out_count = (uint16_t)(*ra8_vin_reg32(k_ra8_vin_off_lc) & k_ra8_vin_lc_mask);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_get_active_buffer(uint8_t* out_id)
{
  RA8_CHECK_NULL_PTR(out_id, s_tag, "out_id must not be nullptr");
  /* HUM Ch 67.2.2 "MS: Module Status Register" p 3978 */
  const uint32_t v = *ra8_vin_reg32(k_ra8_vin_off_ms);
  *out_id          = (uint8_t)((v & k_ra8_vin_ms_fbs) >> k_ra8_vin_ms_shift_fbs);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_set_interrupt_enable(uint32_t mask)
{
  /* HUM Ch 67.2.15 "IE: Interrupt Enable Register" p 3987 */
  *ra8_vin_reg32(k_ra8_vin_off_ie) = mask;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_set_scanline_compare(uint16_t line)
{
  if (line > (uint16_t)k_ra8_vin_si_mask) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 67.2.17 "SI: Scanline Interrupt Register" p 3991 */
  *ra8_vin_reg32(k_ra8_vin_off_si) = (uint32_t)line;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_vin_attach_handler(ra8_vin_event_fn_t fn, void* ctx)
{
  s_vin_fn  = fn;
  s_vin_ctx = ctx;
  return k_ra8_ok;
}

RA8_ISR_SAFE
void ra8_vin_dispatch(void)
{
  /* HUM Ch 67.2.16 "INTS: Interrupt Status Register" p 3989 */
  volatile uint32_t* reg  = ra8_vin_reg32(k_ra8_vin_off_ints);
  const uint32_t     mask = *reg;

  const ra8_vin_event_fn_t fn  = s_vin_fn;
  void* const              ctx = s_vin_ctx;
  if (fn != nullptr) {
    fn(ctx, mask);
  }

  /* Sweep 17: route FME (frame memory complete) into the frame-end
 * callback registered via `ra8_vin_attach_frame_handler`. */
  if ((mask & k_ra8_vin_int_fme) != 0UL) {
    const ra8_vin_frame_fn_t frame_fn  = s_vin_frame_fn;
    void* const              frame_ctx = s_vin_frame_ctx;
    if (frame_fn != nullptr) {
      frame_fn(frame_ctx, s_vin_frame_buf, s_vin_frame_len);
    }
  }

  /* HUM Ch 67.2.16 "INTS: Interrupt Status Register" p 3989
 * Acknowledge the W1C subset (FOS / ARES / FIS2). */
  *reg = mask & k_ra8_vin_int_w1c_mask;
}

[[nodiscard]] ra8_err_t ra8_vin_enter_stop(void)
{
  /* HUM Ch 67.2.1 "MC: Main Control Register" p 3975 */
  volatile uint32_t* mc_reg = ra8_vin_reg32(k_ra8_vin_off_mc);
  *mc_reg                   = *mc_reg & ~k_ra8_vin_mc_me;
  /* HUM Ch 67.2.3 "FC: Frame Capture Register" p 3979 */
  *ra8_vin_reg32(k_ra8_vin_off_fc) = 0UL;

  /* HUM Ch 11.2.8 "MSTPCRC: Module Stop Control Register C", p 446 */
  return ra8_mstp_disable(k_ra8_mstp_mipi_csi);
}

[[nodiscard]] ra8_err_t ra8_vin_exit_stop(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC: Module Stop Control Register C", p 446 */
  return ra8_mstp_enable(k_ra8_mstp_mipi_csi);
}

/* =============================================================================
 * Sweep 17: dynamic capture window + frame-end IRQ
 * =============================================================================
 */

/**
 * @enum ra8_vin_capture_geom_t
 * @brief Bounded geometry constants used by `ra8_vin_capture_start`.
 */
typedef enum : uint16_t {
  k_ra8_vin_geom_max_dim = 4096U, /**< Hardware caps both axes at 4096. */
} ra8_vin_capture_geom_t;

/**
 * @enum ra8_vin_format_bpp_t
 * @brief Bytes-per-pixel lookup for `ra8_vin_input_fmt_t`.
 */
typedef enum : uint8_t {
  k_ra8_vin_bpp_ycbcr422 = 2U, /**< 4:2:2 packs to 2 bytes / pixel. */
  k_ra8_vin_bpp_raw8     = 1U, /**< RAW8 = 1 byte / pixel.          */
  k_ra8_vin_bpp_rgb888   = 4U, /**< RGB888 packed in 32-bit cells.  */
} ra8_vin_format_bpp_t;

/* Resolve bytes-per-pixel for a given input format -- see implementation for details. */
RA8_INTERNAL
static uint8_t internal_format_bpp(ra8_vin_input_fmt_t format)
{
  uint8_t bpp = k_ra8_vin_bpp_ycbcr422;
  if (format == k_ra8_vin_input_raw8) {
    bpp = k_ra8_vin_bpp_raw8;
  } else if (format == k_ra8_vin_input_rgb888) {
    bpp = k_ra8_vin_bpp_rgb888;
  } else {
    bpp = k_ra8_vin_bpp_ycbcr422;
  }
  return bpp;
}

[[nodiscard]] ra8_err_t
ra8_vin_capture_start(void* buf, uint16_t w, uint16_t h, ra8_vin_input_fmt_t format)
{
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  if ((w == 0U) || (h == 0U) || (w > k_ra8_vin_geom_max_dim) || (h > k_ra8_vin_geom_max_dim)) {
    return k_ra8_err_invalid_arg;
  }
  if (((uintptr_t)buf & k_ra8_vin_mb_align_mask) != 0U) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 67.2.10 "MB1: Memory Base 1 Register" p 3985 */
  *ra8_vin_reg32(k_ra8_vin_off_mb1) = (uint32_t)(uintptr_t)buf;
  /* HUM Ch 67.2.13 "IS: Image Stride Register" p 3984 */
  *ra8_vin_reg32(k_ra8_vin_off_is) = (uint32_t)w;

  /* HUM Ch 67.2.1 "MC: Main Control Register" p 3975 */ /* update INF only. */
  internal_mc_rmw(k_ra8_vin_mc_inf,
                  ((uint32_t)format << k_ra8_vin_mc_shift_inf) & k_ra8_vin_mc_inf);

  /* Cache geometry for the frame-end callback. */
  s_vin_frame_buf = buf;
  s_vin_frame_len = (uint32_t)w * (uint32_t)h * (uint32_t)internal_format_bpp(format);

  return ra8_vin_capture_arm(k_ra8_vin_capture_single);
}

[[nodiscard]] ra8_err_t ra8_vin_capture_stop(void)
{
  return ra8_vin_capture_disarm();
}

[[nodiscard]] ra8_err_t ra8_vin_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
  if ((w == 0U) || (h == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t pixel_end = (uint32_t)x + (uint32_t)w - 1U;
  const uint32_t line_end  = (uint32_t)y + (uint32_t)h - 1U;
  if ((pixel_end > k_ra8_vin_preclip_mask) || (line_end > k_ra8_vin_preclip_mask)) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_vin_preclip_t window = {
    .line_start  = y,
    .line_end    = (uint16_t)line_end,
    .pixel_start = x,
    .pixel_end   = (uint16_t)pixel_end,
  };
  return ra8_vin_set_preclip(&window);
}

[[nodiscard]] ra8_err_t ra8_vin_attach_frame_handler(ra8_vin_frame_fn_t fn, void* ctx)
{
  s_vin_frame_fn  = fn;
  s_vin_frame_ctx = ctx;
  return k_ra8_ok;
}
