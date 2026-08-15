/**
 * @file ra8_mipi_csi.c
 * @brief MIPI CSI-2 receiver HAL driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Full HUM Ch 66 ("MIPI CSI Interface" p 3935-3972) HAL driver for
 * the RA8D2 MIPI CSI-2 receiver block. Hand-authored against the
 * HUM (with FSP r_mipi_csi.c used purely as a register-sequence
 * sanity check) -- every single register documented in Ch 66 is
 * exposed, and every IRQ source has a dispatcher + callback slot.
 *
 * Pairs with the parallel ``ra8_mipi_phy`` driver (HUM Ch 64): the
 * caller brings the PHY up first, then opens the CSI receiver.
 *
 * Every register access carries a HUM Ch 66 citation immediately
 * above the access, per project policy.
 *
 * @par State Machine
 * @dot
 * digraph ra8_mipi_csi_states {
 *   bgcolor="transparent";
 *   rankdir=LR;
 *   node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
 *         fillcolor="#e8eef7", color="#5a7ca6"];
 *   edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
 *
 *   __start [shape=circle, width=0.18, label="", fillcolor="#5a7ca6", color="#5a7ca6"];
 *
 *   Gated [label="Gated"];
 *   Idle [label="Idle"];
 *   Active [label="Active"];
 *
 *   __start -> Gated [label="reset"];
 *   Gated -> Idle [label="init()"];
 *   Idle -> Active [label="start_receive()"];
 *   Active -> Idle [label="stop_receive()"];
 *   Idle -> Gated [label="deinit()"];
 * }
 * @enddot
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_mipi_csi.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hal_internal.h"
#include "ra8_log.h"
#include "ra8_mipi_csi_regs.h"
#include "ra8_mstp.h"

/**
 * @var s_tag
 * @brief Log tag prefix used by every ``ra8_log_*`` call below.
 *
 * @note Module-private; do not expose.
 */
static const char* s_tag = "MIPI_CSI";

/**
 * @enum ra8_mipi_csi_intern_t
 * @brief Internal limits used by this translation unit only.
 *
 * @details
 * ``k_ra8_mipi_csi_reset_spin_max`` bounds the busy-loop that waits
 * for RTST.VSRSTS to clear. ``k_ra8_mipi_csi_gfclr_spin_max`` bounds
 * the wait for GSST.GCD after a GSIU.GFCLR request. With vclk at
 * 125 MHz both complete in well under a microsecond; the ceilings
 * here are defensive against a stuck peripheral.
 */
typedef enum : uint16_t {
  k_ra8_mipi_csi_reset_spin_max = 1024U, /**< Max VSRSTS poll iterations. */
  k_ra8_mipi_csi_gfclr_spin_max = 1024U, /**< Max GCD poll iterations.    */
} ra8_mipi_csi_intern_t;

/* =============================================================================
 * Helpers
 * =============================================================================
 */

/* Spin until RTST -- see surrounding code and HUM citations. */
RA8_INTERNAL
static ra8_err_t internal_wait_reset_idle(void)
{
  for (uint16_t i = 0U; i < k_ra8_mipi_csi_reset_spin_max; ++i) { /* GCOVR_EXCL_BR_LINE */
    /* HUM Ch 66.3.6 "RTST : Reset Status Register" p 3939 */
    const uint32_t rtst = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_rtst);
    if ((rtst & k_ra8_mipi_csi_rtst_vsrsts_mask) == 0UL) { /* GCOVR_EXCL_BR_LINE */
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

/* Build the MCT0 32-bit value from a config struct -- see surrounding code and HUM citations. */
RA8_INTERNAL
static uint32_t internal_encode_mct0(const ra8_mipi_csi_config_t* cfg)
{
  uint32_t mct0 = (uint32_t)cfg->lanes & k_ra8_mipi_csi_mct0_vdln_mask;
  if (cfg->generic_rule) {
    mct0 |= k_ra8_mipi_csi_mct0_grmd_mask;
  }
  if (cfg->eccv13) {
    mct0 |= k_ra8_mipi_csi_mct0_eccv13_mask;
  }
  if (cfg->lfsren) {
    mct0 |= k_ra8_mipi_csi_mct0_lfsren_mask;
  }
  if (cfg->zlmd) {
    mct0 |= k_ra8_mipi_csi_mct0_zlmd_mask;
  }
  if (cfg->edmd) {
    mct0 |= k_ra8_mipi_csi_mct0_edmd_mask;
  }
  if (cfg->rvmd) {
    mct0 |= k_ra8_mipi_csi_mct0_rvmd_mask;
  }
  return mct0;
}

/* Refuse a write that requires RXEN = 0 if RXEN is set -- see surrounding code and HUM citations. */
RA8_INTERNAL
static ra8_err_t internal_reject_if_running(void)
{
  /* HUM Ch 66.3.4 "MCT3 : Module Control Register 3" p 3938 */
  const uint32_t cur = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mct3);
  if ((cur & k_ra8_mipi_csi_mct3_rxen_mask) != 0UL) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Validate ``cfg`` field ranges before any register writes.
 *
 * @details
 * Catches lane count, VLSIEN, EPD spacer, and short-packet threshold
 * out-of-range values described in HUM Ch 66 "MIPI Camera Serial
 * Interface" pp 3935-3958.
 *
 * @param[in] cfg Caller-provided config snapshot.
 *
 * @return ``k_ra8_ok`` on success, ``k_ra8_err_invalid_arg`` otherwise.
 * @retval k_ra8_ok All fields in range.
 * @retval k_ra8_err_invalid_arg At least one field is out of the documented range.
 *
 * @pre ``cfg`` is non-NULL (caller has already done the NULL check).
 * @post No register writes occur on failure.
 *
 * @note Internal helper, not thread-safe.
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_cfg(const ra8_mipi_csi_config_t* cfg)
{
  if ((cfg->lanes != k_ra8_mipi_csi_lanes_1) && (cfg->lanes != k_ra8_mipi_csi_lanes_2)) {
    return k_ra8_err_invalid_arg;
  }
  if ((uint8_t)cfg->vlsien > k_ra8_mipi_csi_vlsien_x4) {
    return k_ra8_err_invalid_arg;
  }
  if (((uint32_t)cfg->epd_long_spacer & ~k_ra8_mipi_csi_epct_slp_mask) != 0UL) {
    return k_ra8_err_invalid_arg;
  }
  if (((uint32_t)cfg->epd_short_spacer &
       ~(k_ra8_mipi_csi_epct_ssp_mask >> k_ra8_mipi_csi_epct_ssp_shift)) != 0UL) {
    return k_ra8_err_invalid_arg;
  }
  if ((uint32_t)cfg->short_threshold > k_ra8_mipi_csi_gsct_shth_max) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Program the MCT/EPCT/EMCT/DTEL/DTEH/GSCT receiver block.
 *
 * @details
 * HUM Ch 66.3.2-66.3.24 pp 3936-3957. Writes module-control registers,
 * EPD options, data-type enables, and the generic short-packet control.
 *
 * @param[in] cfg Caller-provided config snapshot.
 *
 * @pre Receiver was disabled (RXEN = 0) and any pending soft reset drained.
 * @post All listed registers reflect ``cfg``.
 *
 * @note Internal helper, not thread-safe.
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_program_receiver(const ra8_mipi_csi_config_t* cfg)
{
  /* HUM Ch 66.3.2 "MCT0 : Module Control Register 0" p 3936
   * VDLN + GRMD + ECCV13 + LFSREN + ZLMD + EDMD + RVMD. */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mct0) = internal_encode_mct0(cfg);

  /* HUM Ch 66.3.3 "MCT2 : Module Control Register 2" p 3937
   * Pack FRRSKW @ [24:16] | FRRCLK @ [8:0]. */
  const uint32_t mct2 =
    (((uint32_t)cfg->frrskw & (k_ra8_mipi_csi_mct2_frrskw_mask >> k_ra8_mipi_csi_mct2_frrskw_shift))
     << k_ra8_mipi_csi_mct2_frrskw_shift) |
    ((uint32_t)cfg->frrclk & k_ra8_mipi_csi_mct2_frrclk_mask);
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mct2) = mct2;

  /* HUM Ch 66.3.7 "EPCT : EPD Option Control Register" p 3939 */
  uint32_t epct = ((uint32_t)cfg->epd_long_spacer & k_ra8_mipi_csi_epct_slp_mask) |
                  (((uint32_t)cfg->epd_short_spacer &
                    (k_ra8_mipi_csi_epct_ssp_mask >> k_ra8_mipi_csi_epct_ssp_shift))
                   << k_ra8_mipi_csi_epct_ssp_shift);
  if (cfg->epd_option_2) {
    epct |= k_ra8_mipi_csi_epct_epdop_mask;
  }
  if (cfg->epd_enable) {
    epct |= k_ra8_mipi_csi_epct_epden_mask;
  }
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_epct) = epct;

  /* HUM Ch 66.3.8 "EMCT : EPD Misc Option Control Register" p 3941
   * VLSIEN[5:4] + EOTPEN[6]. */
  uint32_t emct =
    ((uint32_t)cfg->vlsien << k_ra8_mipi_csi_emct_vlsien_shift) & k_ra8_mipi_csi_emct_vlsien_mask;
  if (cfg->eotp_enable) {
    emct |= k_ra8_mipi_csi_emct_eotpen_mask;
  }
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_emct) = emct;

  /* HUM Ch 66.3.10 "DTEL : Receive Data Type Enable Low" p 3943 */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dtel) = cfg->dt_low_mask;
  /* HUM Ch 66.3.11 "DTEH : Receive Data Type Enable High" p 3944 */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dteh) = cfg->dt_high_mask;

  /* HUM Ch 66.3.24 "GSCT : Generic Short Packet Control" p 3957
   * SHTH[6:0] + GFIF[16]. */
  uint32_t gsct = ((uint32_t)cfg->short_threshold & k_ra8_mipi_csi_gsct_shth_mask);
  if (cfg->short_store_enable) {
    gsct |= k_ra8_mipi_csi_gsct_gfif_mask;
  }
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsct) = gsct;
}

/**
 * @brief Program the receiver interrupt-enable mask block.
 *
 * @details
 * HUM Ch 66.3.14-66.3.27 pp 3946-3958. Writes RXIE, DLIE0/1, VCIE(M),
 * PMIE, and GSIE in one pass.
 *
 * @param[in] cfg Caller-provided config snapshot.
 *
 * @pre Receiver was disabled (RXEN = 0).
 * @post All interrupt-enable registers reflect ``cfg``.
 *
 * @note Internal helper, not thread-safe.
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_program_irq_masks(const ra8_mipi_csi_config_t* cfg)
{
  /* HUM Ch 66.3.14 "RXIE : Receive Interrupt Enable Register" p 3946 */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_rxie) = cfg->rx_irq_mask;

  /* HUM Ch 66.3.17 "DLIE0/1 : Data Lane N Interrupt Enable" p 3948 */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dlie0) = cfg->dl_irq_mask[0];
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dlie1) = cfg->dl_irq_mask[1];

  /* HUM Ch 66.3.20 "VCIE(M) : Virtual Channel M Interrupt Enable" p 3952 */
  for (uint8_t vc = 0U; vc < (uint8_t)k_ra8_mipi_csi_vc_count; ++vc) {
    const ra8_mipi_csi_off_t off = ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcie0, vc);
    *ra8_mipi_csi_reg32(off)     = cfg->vc_irq_mask[vc];
  }

  /* HUM Ch 66.3.23 "PMIE : Power Management Interrupt Enable" p 3956 */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_pmie) = cfg->pm_irq_mask;

  /* HUM Ch 66.3.27 "GSIE : Generic Short Packet Interrupt Enable" p 3958 */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsie) = cfg->short_irq_mask;
}

ra8_err_t ra8_mipi_csi_init(const ra8_mipi_csi_config_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  const ra8_err_t cfg_err = internal_validate_cfg(cfg);
  RA8_RETURN_ON_ERROR(cfg_err, s_tag, "mipi_csi_init: cfg out of range"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_mipi_csi);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "mipi_csi_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 66.3.4 "MCT3 : Module Control Register 3" p 3938
   * Ensure RXEN = 0 before re-configuring the receiver. */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mct3) = 0UL;

  /* HUM Ch 66.3.6 "RTST : Reset Status Register" p 3939
   * Wait for any in-flight software reset to drain. */
  const ra8_err_t rst_err = internal_wait_reset_idle();
  RA8_RETURN_ON_ERROR(rst_err, s_tag, "mipi_csi_init: vsrsts spin"); /* GCOVR_EXCL_BR_LINE */

  internal_program_receiver(cfg);
  internal_program_irq_masks(cfg);

  ra8_log_info_val(s_tag, "mipi_csi_init lanes", (uint32_t)cfg->lanes);
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_deinit(void)
{
  /* HUM Ch 66.3.4 "MCT3 : Module Control Register 3" p 3938 */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mct3) = 0UL;

  /* HUM Ch 66.3.5 "RTCT : Reset Control Register" p 3938
   * Pulse the video-pixel software reset to drain any residual state. */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_rtct) = k_ra8_mipi_csi_rtct_vsrst_mask;

  /* HUM Ch 66.3.14 "RXIE : Receive Interrupt Enable Register" p 3946 */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_rxie) = 0UL;

  /* HUM Ch 66.3.17 "DLIE0/1 : Data Lane N Interrupt Enable" p 3948 */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dlie0) = 0UL;
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dlie1) = 0UL;

  /* HUM Ch 66.3.20 "VCIE(M) : Virtual Channel M Interrupt Enable" p 3952 */
  for (uint8_t vc = 0U; vc < (uint8_t)k_ra8_mipi_csi_vc_count; ++vc) {
    const ra8_mipi_csi_off_t off = ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcie0, vc);
    *ra8_mipi_csi_reg32(off)     = 0UL;
  }

  /* HUM Ch 66.3.23 "PMIE : Power Management Interrupt Enable" p 3956 */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_pmie) = 0UL;

  /* HUM Ch 66.3.27 "GSIE : Generic Short Packet Interrupt Enable" p 3958 */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsie) = 0UL;

  priv_ra8_mipi_csi_detach_all_handlers();

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  return ra8_mstp_disable(k_ra8_mstp_mipi_csi);
}

ra8_err_t ra8_mipi_csi_reset(void)
{
  /* HUM Ch 66.3.5 "RTCT : Reset Control Register" p 3938 */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_rtct) = k_ra8_mipi_csi_rtct_vsrst_mask;
  /* HUM Ch 66.3.6 "RTST : Reset Status Register" p 3939 */
  return internal_wait_reset_idle();
}

/* =============================================================================
 * Reception control
 * =============================================================================
 */

ra8_err_t ra8_mipi_csi_start_receive(void)
{
  const ra8_err_t state_err = internal_reject_if_running();
  RA8_RETURN_ON_ERROR(state_err, s_tag, "mipi_csi start: already running"); /* GCOVR_EXCL_BR_LINE */
  /* HUM Ch 66.3.4 "MCT3 : Module Control Register 3" p 3938 */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mct3) = k_ra8_mipi_csi_mct3_rxen_mask;
  ra8_log_info(s_tag, "mipi_csi start_receive");
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_stop_receive(void)
{
  /* HUM Ch 66.3.4 "MCT3 : Module Control Register 3" p 3938 */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mct3) = 0UL;
  /* HUM Ch 66.3.5 "RTCT : Reset Control Register" p 3938 */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_rtct) = k_ra8_mipi_csi_rtct_vsrst_mask;
  ra8_log_info(s_tag, "mipi_csi stop_receive");
  return k_ra8_ok;
}

/* =============================================================================
 * Module-level status
 * =============================================================================
 */

ra8_err_t ra8_mipi_csi_get_status(uint32_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 66.3.12 "RXST : Receive Status Register" p 3944 */
  *out_mask = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_rxst);
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_clear_status(uint32_t mask)
{
  /* HUM Ch 66.3.13 "RXSC : Receive Status Clear Register" p 3945
   * Only RACTDETC (bit 17) is W1C; hardware drops other bits. */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_rxsc) = mask;
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_set_rx_irq_enable(uint32_t mask)
{
  /* HUM Ch 66.3.14 "RXIE : Receive Interrupt Enable Register" p 3946 */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_rxie) = mask;
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_get_module_irq_status(uint32_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 66.3.9 "MIST : Module Interrupt Status" p 3941 */
  *out_mask = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mist);
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_get_module_info(ra8_mipi_csi_module_info_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 66.3.1 "MCG : Module Configuration Register" p 3936 */
  const uint32_t mcg = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mcg);
  out->raw           = mcg;
  out->version       = (uint8_t)(mcg & k_ra8_mipi_csi_mcg_ver_mask);
  out->lanes_max = (uint8_t)((mcg & k_ra8_mipi_csi_mcg_sdln_mask) >> k_ra8_mipi_csi_mcg_sdln_shift);
  out->fifo_stages =
    (uint8_t)((mcg & k_ra8_mipi_csi_mcg_gsnm_mask) >> k_ra8_mipi_csi_mcg_gsnm_shift);
  return k_ra8_ok;
}

/* =============================================================================
 * Data-type filter
 * =============================================================================
 */

ra8_err_t ra8_mipi_csi_set_data_type_filter(uint32_t low_mask, uint32_t high_mask)
{
  /* HUM Ch 66.3.10 "DTEL : Receive Data Type Enable Low" p 3943 */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dtel) = low_mask;
  /* HUM Ch 66.3.11 "DTEH : Receive Data Type Enable High" p 3944 */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dteh) = high_mask;
  return k_ra8_ok;
}

/* =============================================================================
 * ECC / scrambling / frame-error knobs
 * =============================================================================
 */

ra8_err_t ra8_mipi_csi_set_ecc_mode(bool eccv13, bool lfsren)
{
  const ra8_err_t state_err = internal_reject_if_running();
  RA8_RETURN_ON_ERROR(state_err, s_tag, "set_ecc_mode: rxen set"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 66.3.2 "MCT0 : Module Control Register 0" p 3936 */
  uint32_t mct0 = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mct0);
  mct0 &= ~(k_ra8_mipi_csi_mct0_eccv13_mask | k_ra8_mipi_csi_mct0_lfsren_mask);
  if (eccv13) {
    mct0 |= k_ra8_mipi_csi_mct0_eccv13_mask;
  }
  if (lfsren) {
    mct0 |= k_ra8_mipi_csi_mct0_lfsren_mask;
  }
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mct0) = mct0;
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_set_frame_error_mode(bool zlmd, bool edmd, bool rvmd)
{
  const ra8_err_t state_err = internal_reject_if_running();
  RA8_RETURN_ON_ERROR(state_err, s_tag, "set_frame_error_mode: rxen set"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 66.3.2 "MCT0 : Module Control Register 0" p 3936 */
  uint32_t mct0 = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mct0);
  mct0 &= ~(k_ra8_mipi_csi_mct0_zlmd_mask | k_ra8_mipi_csi_mct0_edmd_mask |
            k_ra8_mipi_csi_mct0_rvmd_mask);
  if (zlmd) {
    mct0 |= k_ra8_mipi_csi_mct0_zlmd_mask;
  }
  if (edmd) {
    mct0 |= k_ra8_mipi_csi_mct0_edmd_mask;
  }
  if (rvmd) {
    mct0 |= k_ra8_mipi_csi_mct0_rvmd_mask;
  }
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mct0) = mct0;
  return k_ra8_ok;
}

/* =============================================================================
 * EPD / LRTE tuning
 * =============================================================================
 */

ra8_err_t
ra8_mipi_csi_set_epd(bool enable, bool option_2, uint16_t long_spacer, uint16_t short_spacer)
{
  if (((uint32_t)long_spacer & ~k_ra8_mipi_csi_epct_slp_mask) != 0UL) {
    return k_ra8_err_invalid_arg;
  }
  if (((uint32_t)short_spacer & ~(k_ra8_mipi_csi_epct_ssp_mask >> k_ra8_mipi_csi_epct_ssp_shift)) !=
      0UL) {
    return k_ra8_err_invalid_arg;
  }

  const ra8_err_t state_err = internal_reject_if_running();
  RA8_RETURN_ON_ERROR(state_err, s_tag, "set_epd: rxen set"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 66.3.7 "EPCT : EPD Option Control Register" p 3939 */
  uint32_t epct =
    ((uint32_t)long_spacer & k_ra8_mipi_csi_epct_slp_mask) |
    (((uint32_t)short_spacer & (k_ra8_mipi_csi_epct_ssp_mask >> k_ra8_mipi_csi_epct_ssp_shift))
     << k_ra8_mipi_csi_epct_ssp_shift);
  if (option_2) {
    epct |= k_ra8_mipi_csi_epct_epdop_mask;
  }
  if (enable) {
    epct |= k_ra8_mipi_csi_epct_epden_mask;
  }
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_epct) = epct;
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_set_lrte(ra8_mipi_csi_vlsien_t vlsien, bool eotp_enable)
{
  if ((uint8_t)vlsien > k_ra8_mipi_csi_vlsien_x4) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t state_err = internal_reject_if_running();
  RA8_RETURN_ON_ERROR(state_err, s_tag, "set_lrte: rxen set"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 66.3.8 "EMCT : EPD Misc Option Control Register" p 3941 */
  uint32_t emct = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_emct);
  emct &= ~(k_ra8_mipi_csi_emct_vlsien_mask | k_ra8_mipi_csi_emct_eotpen_mask);
  emct |= ((uint32_t)vlsien << k_ra8_mipi_csi_emct_vlsien_shift) & k_ra8_mipi_csi_emct_vlsien_mask;
  if (eotp_enable) {
    emct |= k_ra8_mipi_csi_emct_eotpen_mask;
  }
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_emct) = emct;
  return k_ra8_ok;
}

/* =============================================================================
 * Per-data-lane status / IRQ
 * =============================================================================
 */

ra8_err_t ra8_mipi_csi_dl_get_status(uint8_t lane, uint32_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  if (lane > (uint8_t)k_ra8_mipi_csi_dl_max) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 66.3.15 "DLST(N) : Data Lane N Status" p 3946 */
  const ra8_mipi_csi_off_t off = ra8_mipi_csi_dl_off(k_ra8_mipi_csi_off_dlst0, lane);
  *out_mask                    = *ra8_mipi_csi_reg32(off);
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_dl_clear_status(uint8_t lane, uint32_t mask)
{
  if (lane > (uint8_t)k_ra8_mipi_csi_dl_max) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 66.3.16 "DLSC(N) : Data Lane N Status Clear" p 3947 */
  const ra8_mipi_csi_off_t off = ra8_mipi_csi_dl_off(k_ra8_mipi_csi_off_dlsc0, lane);
  *ra8_mipi_csi_reg32(off)     = mask & k_ra8_mipi_csi_dlsc_all_mask;
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_dl_set_irq_enable(uint8_t lane, uint32_t mask)
{
  if (lane > (uint8_t)k_ra8_mipi_csi_dl_max) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 66.3.17 "DLIE(N) : Data Lane N Interrupt Enable" p 3948 */
  const ra8_mipi_csi_off_t off = ra8_mipi_csi_dl_off(k_ra8_mipi_csi_off_dlie0, lane);
  *ra8_mipi_csi_reg32(off)     = mask;
  return k_ra8_ok;
}

/* =============================================================================
 * Per-VC status / IRQ
 * =============================================================================
 */

ra8_err_t ra8_mipi_csi_vc_get_status(uint8_t vc, uint32_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  if (vc > (uint8_t)k_ra8_mipi_csi_vc_max) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 66.3.18 "VCST(M) : Virtual Channel M Status" p 3949 */
  const ra8_mipi_csi_off_t off = ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcst0, vc);
  *out_mask                    = *ra8_mipi_csi_reg32(off);
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_vc_clear_status(uint8_t vc, uint32_t mask)
{
  if (vc > (uint8_t)k_ra8_mipi_csi_vc_max) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 66.3.19 "VCSC(M) : Virtual Channel M Status Clear" p 3951 */
  const ra8_mipi_csi_off_t off = ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcsc0, vc);
  *ra8_mipi_csi_reg32(off)     = mask;
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_vc_set_irq_enable(uint8_t vc, uint32_t mask)
{
  if (vc > (uint8_t)k_ra8_mipi_csi_vc_max) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 66.3.20 "VCIE(M) : Virtual Channel M Interrupt Enable" p 3952 */
  const ra8_mipi_csi_off_t off = ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcie0, vc);
  *ra8_mipi_csi_reg32(off)     = mask;
  return k_ra8_ok;
}

/* =============================================================================
 * Power-management status / IRQ
 * =============================================================================
 */

ra8_err_t ra8_mipi_csi_pm_get_status(uint32_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 66.3.21 "PMST : Power Management Status" p 3954 */
  *out_mask = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_pmst);
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_pm_clear_status(uint32_t mask)
{
  /* HUM Ch 66.3.22 "PMSC : Power Management Status Clear" p 3955
   * Only [7:0] are W1C; hardware drops other bits. */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_pmsc) = mask & k_ra8_mipi_csi_pmsc_all_mask;
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_pm_set_irq_enable(uint32_t mask)
{
  /* HUM Ch 66.3.23 "PMIE : Power Management Interrupt Enable" p 3956 */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_pmie) = mask & k_ra8_mipi_csi_pmie_all_mask;
  return k_ra8_ok;
}

/* =============================================================================
 * Generic short-packet FIFO
 * =============================================================================
 */

ra8_err_t ra8_mipi_csi_short_packet_configure(uint8_t threshold, bool store_enable)
{
  if ((uint32_t)threshold > k_ra8_mipi_csi_gsct_shth_max) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 66.3.24 "GSCT : Generic Short Packet Control" p 3957 */
  uint32_t gsct = ((uint32_t)threshold & k_ra8_mipi_csi_gsct_shth_mask);
  if (store_enable) {
    gsct |= k_ra8_mipi_csi_gsct_gfif_mask;
  }
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsct) = gsct;
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_short_packet_set_irq_enable(uint32_t mask)
{
  /* HUM Ch 66.3.27 "GSIE : Generic Short Packet Interrupt Enable" p 3958 */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsie) = mask & k_ra8_mipi_csi_gsie_all_mask;
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_short_packet_get_status(uint32_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 66.3.25 "GSST : Generic Short Packet Status" p 3957 */
  *out_mask = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsst);
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_short_packet_clear_status(uint32_t mask)
{
  /* HUM Ch 66.3.26 "GSSC : Generic Short Packet Status Clear" p 3958
   * Only GOVC (bit 4) is W1C; hardware drops other bits. */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gssc) = mask & k_ra8_mipi_csi_gssc_govc_mask;
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_read_short_packet(ra8_mipi_csi_short_packet_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 66.3.25 "GSST : Generic Short Packet Status" p 3957
   * PNUM[15:8] reports queued packet count. */
  const uint32_t gsst = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsst);
  const uint32_t pnum = (gsst & k_ra8_mipi_csi_gsst_pnum_mask) >> k_ra8_mipi_csi_gsst_pnum_shift;
  if (pnum == 0UL) {
    return k_ra8_err_empty;
  }
  /* HUM Ch 66.3.29 "GSIU : Generic Short Packet Information Update" p 3960
   * FINC advances the read pointer before we sample GSHT. */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsiu) = k_ra8_mipi_csi_gsiu_finc_mask;

  /* HUM Ch 66.3.28 "GSHT : Generic Short Packet Header" p 3959 */
  const uint32_t gsht = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsht);
  out->raw            = gsht;
  out->payload =
    (uint16_t)((gsht & k_ra8_mipi_csi_gsht_spdt_mask) >> k_ra8_mipi_csi_gsht_spdt_shift);
  out->data_type =
    (uint8_t)((gsht & k_ra8_mipi_csi_gsht_dtyp_mask) >> k_ra8_mipi_csi_gsht_dtyp_shift);
  out->vc = (uint8_t)((gsht & k_ra8_mipi_csi_gsht_spvc_mask) >> k_ra8_mipi_csi_gsht_spvc_shift);
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_short_packet_clear_fifo(void)
{
  /* HUM Ch 66.3.29 "GSIU : Generic Short Packet Information Update" p 3960
   * Drive GFCLR = 1 to request the clear, wait for GSST.GCD = 1, then
   * release GFCLR by writing 0. */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsiu) = k_ra8_mipi_csi_gsiu_gfclr_mask;

  ra8_err_t err = k_ra8_err_hw_timeout;
  for (uint16_t i = 0U; i < k_ra8_mipi_csi_gfclr_spin_max; ++i) { /* GCOVR_EXCL_BR_LINE */
    /* HUM Ch 66.3.25 "GSST : Generic Short Packet Status" p 3957 */
    const uint32_t gsst = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsst);
    if ((gsst & k_ra8_mipi_csi_gsst_gcd_mask) != 0UL) { /* GCOVR_EXCL_BR_LINE */
      err = k_ra8_ok;
      break;
    }
  }

  /* Release the request line regardless of outcome. */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsiu) = 0UL;
  return err;
}

ra8_err_t ra8_mipi_csi_short_packet_re_enable_store(void)
{
  /* HUM Ch 66.3.29 "GSIU : Generic Short Packet Information Update" p 3960 */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsiu) = k_ra8_mipi_csi_gsiu_gfen_mask;
  return k_ra8_ok;
}

/* =============================================================================
 * Power transition
 * =============================================================================
 */

ra8_err_t ra8_mipi_csi_enter_stop(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  return ra8_mstp_disable(k_ra8_mstp_mipi_csi);
}

ra8_err_t ra8_mipi_csi_exit_stop(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  return ra8_mstp_enable(k_ra8_mstp_mipi_csi);
}
