/**
 * @file ra_mipi_csi.c
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
 * Pairs with the parallel ``ra_mipi_phy`` driver (HUM Ch 64): the
 * caller brings the PHY up first, then opens the CSI receiver.
 *
 * Every register access carries a HUM Ch 66 citation immediately
 * above the access, per project policy.
 *
 * @par State Machine
 * @startuml
 * [*] --> Gated : reset
 * Gated  --> Idle    : init()
 * Idle   --> Active  : start_receive()
 * Active --> Idle    : stop_receive()
 * Idle   --> Gated   : deinit()
 * @enduml
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_mipi_csi.h"

#include <stdint.h>

#include "ra8d2_mipi_csi_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

/**
 * @var s_tag
 * @brief Log tag prefix used by every ``ra_log_*`` call below.
 *
 * @note Module-private; do not expose.
 */
static const char* s_tag = "MIPI_CSI";

/**
 * @enum ra_mipi_csi_intern_t
 * @brief Internal limits used by this translation unit only.
 *
 * @details
 * ``k_ra_mipi_csi_reset_spin_max`` bounds the busy-loop that waits
 * for RTST.VSRSTS to clear. ``k_ra_mipi_csi_gfclr_spin_max`` bounds
 * the wait for GSST.GCD after a GSIU.GFCLR request. With vclk at
 * 125 MHz both complete in well under a microsecond; the ceilings
 * here are defensive against a stuck peripheral.
 */
typedef enum : uint16_t {
  k_ra_mipi_csi_reset_spin_max = 1024U, /**< Max VSRSTS poll iterations.   */
  k_ra_mipi_csi_gfclr_spin_max = 1024U, /**< Max GCD poll iterations.      */
  k_ra_mipi_csi_vc_invalid_idx = 0xFFU, /**< Sentinel for "generic" event. */
} ra_mipi_csi_intern_t;

/**
 * @var s_mipi_csi_fn
 * @brief Currently-attached receive event callback.
 * @note File-scope; updated only by ``ra_mipi_csi_attach_handler``.
 */
static ra_mipi_csi_event_fn_t s_mipi_csi_fn;

/**
 * @var s_mipi_csi_ctx
 * @brief Caller context forwarded to the receive event callback.
 * @note File-scope; updated only by ``ra_mipi_csi_attach_handler``.
 */
static void* s_mipi_csi_ctx;

/**
 * @var s_mipi_csi_dl_fn
 * @brief Currently-attached data-lane callback.
 * @note File-scope; updated only by ``ra_mipi_csi_attach_dl_handler``.
 */
static ra_mipi_csi_dl_event_fn_t s_mipi_csi_dl_fn;

/**
 * @var s_mipi_csi_dl_ctx
 * @brief Caller context forwarded to the data-lane callback.
 * @note File-scope; updated only by ``ra_mipi_csi_attach_dl_handler``.
 */
static void* s_mipi_csi_dl_ctx;

/**
 * @var s_mipi_csi_vc_fn
 * @brief Currently-attached per-VC callback.
 * @note File-scope; updated only by ``ra_mipi_csi_attach_vc_handler``.
 */
static ra_mipi_csi_vc_event_fn_t s_mipi_csi_vc_fn;

/**
 * @var s_mipi_csi_vc_ctx
 * @brief Caller context forwarded to the per-VC callback.
 * @note File-scope; updated only by ``ra_mipi_csi_attach_vc_handler``.
 */
static void* s_mipi_csi_vc_ctx;

/**
 * @var s_mipi_csi_pm_fn
 * @brief Currently-attached power-management callback.
 * @note File-scope; updated only by ``ra_mipi_csi_attach_pm_handler``.
 */
static ra_mipi_csi_pm_event_fn_t s_mipi_csi_pm_fn;

/**
 * @var s_mipi_csi_pm_ctx
 * @brief Caller context forwarded to the PM callback.
 * @note File-scope; updated only by ``ra_mipi_csi_attach_pm_handler``.
 */
static void* s_mipi_csi_pm_ctx;

/**
 * @var s_mipi_csi_gst_fn
 * @brief Currently-attached short-packet FIFO callback.
 * @note File-scope; updated only by
 *       ``ra_mipi_csi_attach_short_packet_handler``.
 */
static ra_mipi_csi_short_event_fn_t s_mipi_csi_gst_fn;

/**
 * @var s_mipi_csi_gst_ctx
 * @brief Caller context forwarded to the short-packet callback.
 * @note File-scope; updated only by
 *       ``ra_mipi_csi_attach_short_packet_handler``.
 */
static void* s_mipi_csi_gst_ctx;

/* =============================================================================
 * Helpers
 * =============================================================================
 */

/**
 * @brief Spin until RTST.VSRSTS reads 0 or the budget expires.
 *
 * @return ``k_ra_ok`` if the bit cleared in time,
 *         ``k_ra_err_hw_timeout`` otherwise.
 *
 * @pre Peripheral is ungated.
 * @pre Caller has just stopped the receiver.
 * @post Peripheral reset has finished or timeout returned.
 * @post No callback has been invoked.
 */
static ra_err_t internal_wait_reset_idle(void)
{
  for (uint16_t i = 0U; i < (uint16_t)k_ra_mipi_csi_reset_spin_max; ++i) {
    /* HUM Ch 66.3.6 "RTST : Reset Status Register" p 3939 */
    const uint32_t rtst = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_rtst);
    if ((rtst & (uint32_t)k_ra_mipi_csi_rtst_vsrsts_mask) == 0UL) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Build the MCT0 32-bit value from a config struct.
 *
 * @param[in] cfg Caller-supplied config (already validated).
 * @return Encoded MCT0 value (VDLN + GRMD + ECC + frame-error bits).
 *
 * @pre cfg is non-NULL and validated.
 * @pre cfg->lanes is one of {1, 2}.
 * @post Returned value masks only documented MCT0 bits.
 * @post Reserved bits remain 0.
 */
static uint32_t internal_encode_mct0(const ra_mipi_csi_config_t* cfg)
{
  uint32_t mct0 = (uint32_t)cfg->lanes & (uint32_t)k_ra_mipi_csi_mct0_vdln_mask;
  if (cfg->generic_rule) {
    mct0 |= (uint32_t)k_ra_mipi_csi_mct0_grmd_mask;
  }
  if (cfg->eccv13) {
    mct0 |= (uint32_t)k_ra_mipi_csi_mct0_eccv13_mask;
  }
  if (cfg->lfsren) {
    mct0 |= (uint32_t)k_ra_mipi_csi_mct0_lfsren_mask;
  }
  if (cfg->zlmd) {
    mct0 |= (uint32_t)k_ra_mipi_csi_mct0_zlmd_mask;
  }
  if (cfg->edmd) {
    mct0 |= (uint32_t)k_ra_mipi_csi_mct0_edmd_mask;
  }
  if (cfg->rvmd) {
    mct0 |= (uint32_t)k_ra_mipi_csi_mct0_rvmd_mask;
  }
  return mct0;
}

/**
 * @brief Refuse a write that requires RXEN = 0 if RXEN is set.
 *
 * @return ``k_ra_ok`` if RXEN is 0, ``k_ra_err_invalid_state`` if 1.
 *
 * @pre Peripheral is ungated.
 * @pre Register window is mapped.
 * @post No hardware state is modified.
 * @post Returned value reflects current MCT3.RXEN bit.
 */
static ra_err_t internal_reject_if_running(void)
{
  /* HUM Ch 66.3.4 "MCT3 : Module Control Register 3" p 3938 */
  const uint32_t cur = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mct3);
  if ((cur & (uint32_t)k_ra_mipi_csi_mct3_rxen_mask) != 0UL) {
    return k_ra_err_invalid_state;
  }
  return k_ra_ok;
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
 * @return ``k_ra_ok`` on success, ``k_ra_err_invalid_arg`` otherwise.
 * @retval k_ra_ok All fields in range.
 * @retval k_ra_err_invalid_arg At least one field is out of the documented range.
 *
 * @pre ``cfg`` is non-NULL (caller has already done the NULL check).
 * @post No register writes occur on failure.
 *
 * @note Internal helper, not thread-safe.
 */
static ra_err_t internal_validate_cfg(const ra_mipi_csi_config_t* cfg)
{
  if ((cfg->lanes != k_ra_mipi_csi_lanes_1) && (cfg->lanes != k_ra_mipi_csi_lanes_2)) {
    return k_ra_err_invalid_arg;
  }
  if ((uint8_t)cfg->vlsien > (uint8_t)k_ra_mipi_csi_vlsien_x4) {
    return k_ra_err_invalid_arg;
  }
  if (((uint32_t)cfg->epd_long_spacer & ~(uint32_t)k_ra_mipi_csi_epct_slp_mask) != 0UL) {
    return k_ra_err_invalid_arg;
  }
  if (((uint32_t)cfg->epd_short_spacer &
       ~((uint32_t)k_ra_mipi_csi_epct_ssp_mask >> (uint32_t)k_ra_mipi_csi_epct_ssp_shift)) != 0UL) {
    return k_ra_err_invalid_arg;
  }
  if ((uint32_t)cfg->short_threshold > (uint32_t)k_ra_mipi_csi_gsct_shth_max) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
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
 */
static void internal_program_receiver(const ra_mipi_csi_config_t* cfg)
{
  /* HUM Ch 66.3.2 "MCT0 : Module Control Register 0" p 3936
   * VDLN + GRMD + ECCV13 + LFSREN + ZLMD + EDMD + RVMD. */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mct0) = internal_encode_mct0(cfg);

  /* HUM Ch 66.3.3 "MCT2 : Module Control Register 2" p 3937
   * Pack FRRSKW @ [24:16] | FRRCLK @ [8:0]. */
  const uint32_t mct2 = (((uint32_t)cfg->frrskw & ((uint32_t)k_ra_mipi_csi_mct2_frrskw_mask >>
                                                   (uint32_t)k_ra_mipi_csi_mct2_frrskw_shift))
                         << (uint32_t)k_ra_mipi_csi_mct2_frrskw_shift) |
                        ((uint32_t)cfg->frrclk & (uint32_t)k_ra_mipi_csi_mct2_frrclk_mask);
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mct2) = mct2;

  /* HUM Ch 66.3.7 "EPCT : EPD Option Control Register" p 3939 */
  uint32_t epct = ((uint32_t)cfg->epd_long_spacer & (uint32_t)k_ra_mipi_csi_epct_slp_mask) |
                  (((uint32_t)cfg->epd_short_spacer & ((uint32_t)k_ra_mipi_csi_epct_ssp_mask >>
                                                       (uint32_t)k_ra_mipi_csi_epct_ssp_shift))
                   << (uint32_t)k_ra_mipi_csi_epct_ssp_shift);
  if (cfg->epd_option_2) {
    epct |= (uint32_t)k_ra_mipi_csi_epct_epdop_mask;
  }
  if (cfg->epd_enable) {
    epct |= (uint32_t)k_ra_mipi_csi_epct_epden_mask;
  }
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_epct) = epct;

  /* HUM Ch 66.3.8 "EMCT : EPD Misc Option Control Register" p 3941
   * VLSIEN[5:4] + EOTPEN[6]. */
  uint32_t emct = ((uint32_t)cfg->vlsien << (uint32_t)k_ra_mipi_csi_emct_vlsien_shift) &
                  (uint32_t)k_ra_mipi_csi_emct_vlsien_mask;
  if (cfg->eotp_enable) {
    emct |= (uint32_t)k_ra_mipi_csi_emct_eotpen_mask;
  }
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_emct) = emct;

  /* HUM Ch 66.3.10 "DTEL : Receive Data Type Enable Low" p 3943 */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_dtel) = cfg->dt_low_mask;
  /* HUM Ch 66.3.11 "DTEH : Receive Data Type Enable High" p 3944 */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_dteh) = cfg->dt_high_mask;

  /* HUM Ch 66.3.24 "GSCT : Generic Short Packet Control" p 3957
   * SHTH[6:0] + GFIF[16]. */
  uint32_t gsct = ((uint32_t)cfg->short_threshold & (uint32_t)k_ra_mipi_csi_gsct_shth_mask);
  if (cfg->short_store_enable) {
    gsct |= (uint32_t)k_ra_mipi_csi_gsct_gfif_mask;
  }
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsct) = gsct;
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
 */
static void internal_program_irq_masks(const ra_mipi_csi_config_t* cfg)
{
  /* HUM Ch 66.3.14 "RXIE : Receive Interrupt Enable Register" p 3946 */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_rxie) = cfg->rx_irq_mask;

  /* HUM Ch 66.3.17 "DLIE0/1 : Data Lane N Interrupt Enable" p 3948 */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_dlie0) = cfg->dl_irq_mask[0];
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_dlie1) = cfg->dl_irq_mask[1];

  /* HUM Ch 66.3.20 "VCIE(M) : Virtual Channel M Interrupt Enable" p 3952 */
  for (uint8_t vc = 0U; vc < (uint8_t)k_ra_mipi_csi_vc_count; ++vc) {
    const ra_mipi_csi_off_t off = ra_mipi_csi_vc_off(k_ra_mipi_csi_off_vcie0, vc);
    *ra_mipi_csi_reg32(off)     = cfg->vc_irq_mask[vc];
  }

  /* HUM Ch 66.3.23 "PMIE : Power Management Interrupt Enable" p 3956 */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_pmie) = cfg->pm_irq_mask;

  /* HUM Ch 66.3.27 "GSIE : Generic Short Packet Interrupt Enable" p 3958 */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsie) = cfg->short_irq_mask;
}

ra_err_t ra_mipi_csi_init(const ra_mipi_csi_config_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  const ra_err_t cfg_err = internal_validate_cfg(cfg);
  RA_RETURN_ON_ERROR(cfg_err, s_tag, "mipi_csi_init: cfg out of range");

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_mipi_csi);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "mipi_csi_init: mstp enable");

  /* HUM Ch 66.3.4 "MCT3 : Module Control Register 3" p 3938
   * Ensure RXEN = 0 before re-configuring the receiver. */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mct3) = 0UL;

  /* HUM Ch 66.3.6 "RTST : Reset Status Register" p 3939
   * Wait for any in-flight software reset to drain. */
  const ra_err_t rst_err = internal_wait_reset_idle();
  RA_RETURN_ON_ERROR(rst_err, s_tag, "mipi_csi_init: vsrsts spin");

  internal_program_receiver(cfg);
  internal_program_irq_masks(cfg);

  ra_log_info_val(s_tag, "mipi_csi_init lanes", (uint32_t)cfg->lanes);
  return k_ra_ok;
}

ra_err_t ra_mipi_csi_deinit(void)
{
  /* HUM Ch 66.3.4 "MCT3 : Module Control Register 3" p 3938 */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mct3) = 0UL;

  /* HUM Ch 66.3.5 "RTCT : Reset Control Register" p 3938
   * Pulse the video-pixel software reset to drain any residual state. */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_rtct) = (uint32_t)k_ra_mipi_csi_rtct_vsrst_mask;

  /* HUM Ch 66.3.14 "RXIE : Receive Interrupt Enable Register" p 3946 */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_rxie) = 0UL;

  /* HUM Ch 66.3.17 "DLIE0/1 : Data Lane N Interrupt Enable" p 3948 */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_dlie0) = 0UL;
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_dlie1) = 0UL;

  /* HUM Ch 66.3.20 "VCIE(M) : Virtual Channel M Interrupt Enable" p 3952 */
  for (uint8_t vc = 0U; vc < (uint8_t)k_ra_mipi_csi_vc_count; ++vc) {
    const ra_mipi_csi_off_t off = ra_mipi_csi_vc_off(k_ra_mipi_csi_off_vcie0, vc);
    *ra_mipi_csi_reg32(off)     = 0UL;
  }

  /* HUM Ch 66.3.23 "PMIE : Power Management Interrupt Enable" p 3956 */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_pmie) = 0UL;

  /* HUM Ch 66.3.27 "GSIE : Generic Short Packet Interrupt Enable" p 3958 */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsie) = 0UL;

  s_mipi_csi_fn      = nullptr;
  s_mipi_csi_ctx     = nullptr;
  s_mipi_csi_dl_fn   = nullptr;
  s_mipi_csi_dl_ctx  = nullptr;
  s_mipi_csi_vc_fn   = nullptr;
  s_mipi_csi_vc_ctx  = nullptr;
  s_mipi_csi_pm_fn   = nullptr;
  s_mipi_csi_pm_ctx  = nullptr;
  s_mipi_csi_gst_fn  = nullptr;
  s_mipi_csi_gst_ctx = nullptr;

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  return ra_mstp_disable(k_ra_mstp_mipi_csi);
}

ra_err_t ra_mipi_csi_reset(void)
{
  /* HUM Ch 66.3.5 "RTCT : Reset Control Register" p 3938 */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_rtct) = (uint32_t)k_ra_mipi_csi_rtct_vsrst_mask;
  /* HUM Ch 66.3.6 "RTST : Reset Status Register" p 3939 */
  return internal_wait_reset_idle();
}

/* =============================================================================
 * Reception control
 * =============================================================================
 */

ra_err_t ra_mipi_csi_start_receive(void)
{
  const ra_err_t state_err = internal_reject_if_running();
  RA_RETURN_ON_ERROR(state_err, s_tag, "mipi_csi start: already running");
  /* HUM Ch 66.3.4 "MCT3 : Module Control Register 3" p 3938 */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mct3) = (uint32_t)k_ra_mipi_csi_mct3_rxen_mask;
  ra_log_info(s_tag, "mipi_csi start_receive");
  return k_ra_ok;
}

ra_err_t ra_mipi_csi_stop_receive(void)
{
  /* HUM Ch 66.3.4 "MCT3 : Module Control Register 3" p 3938 */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mct3) = 0UL;
  /* HUM Ch 66.3.5 "RTCT : Reset Control Register" p 3938 */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_rtct) = (uint32_t)k_ra_mipi_csi_rtct_vsrst_mask;
  ra_log_info(s_tag, "mipi_csi stop_receive");
  return k_ra_ok;
}

/* =============================================================================
 * Module-level status
 * =============================================================================
 */

ra_err_t ra_mipi_csi_get_status(uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 66.3.12 "RXST : Receive Status Register" p 3944 */
  *out_mask = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_rxst);
  return k_ra_ok;
}

ra_err_t ra_mipi_csi_clear_status(uint32_t mask)
{
  /* HUM Ch 66.3.13 "RXSC : Receive Status Clear Register" p 3945
   * Only RACTDETC (bit 17) is W1C; hardware drops other bits. */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_rxsc) = mask;
  return k_ra_ok;
}

ra_err_t ra_mipi_csi_set_rx_irq_enable(uint32_t mask)
{
  /* HUM Ch 66.3.14 "RXIE : Receive Interrupt Enable Register" p 3946 */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_rxie) = mask;
  return k_ra_ok;
}

ra_err_t ra_mipi_csi_get_module_irq_status(uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 66.3.9 "MIST : Module Interrupt Status" p 3941 */
  *out_mask = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mist);
  return k_ra_ok;
}

ra_err_t ra_mipi_csi_get_module_info(ra_mipi_csi_module_info_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 66.3.1 "MCG : Module Configuration Register" p 3936 */
  const uint32_t mcg = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mcg);
  out->raw           = mcg;
  out->version       = (uint8_t)(mcg & (uint32_t)k_ra_mipi_csi_mcg_ver_mask);
  out->lanes_max     = (uint8_t)((mcg & (uint32_t)k_ra_mipi_csi_mcg_sdln_mask) >>
                                 (uint32_t)k_ra_mipi_csi_mcg_sdln_shift);
  out->fifo_stages   = (uint8_t)((mcg & (uint32_t)k_ra_mipi_csi_mcg_gsnm_mask) >>
                                 (uint32_t)k_ra_mipi_csi_mcg_gsnm_shift);
  return k_ra_ok;
}

/* =============================================================================
 * Data-type filter
 * =============================================================================
 */

ra_err_t ra_mipi_csi_set_data_type_filter(uint32_t low_mask, uint32_t high_mask)
{
  /* HUM Ch 66.3.10 "DTEL : Receive Data Type Enable Low" p 3943 */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_dtel) = low_mask;
  /* HUM Ch 66.3.11 "DTEH : Receive Data Type Enable High" p 3944 */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_dteh) = high_mask;
  return k_ra_ok;
}

/* =============================================================================
 * ECC / scrambling / frame-error knobs
 * =============================================================================
 */

ra_err_t ra_mipi_csi_set_ecc_mode(bool eccv13, bool lfsren)
{
  const ra_err_t state_err = internal_reject_if_running();
  RA_RETURN_ON_ERROR(state_err, s_tag, "set_ecc_mode: rxen set");

  /* HUM Ch 66.3.2 "MCT0 : Module Control Register 0" p 3936 */
  uint32_t mct0 = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mct0);
  mct0 &= ~((uint32_t)k_ra_mipi_csi_mct0_eccv13_mask | (uint32_t)k_ra_mipi_csi_mct0_lfsren_mask);
  if (eccv13) {
    mct0 |= (uint32_t)k_ra_mipi_csi_mct0_eccv13_mask;
  }
  if (lfsren) {
    mct0 |= (uint32_t)k_ra_mipi_csi_mct0_lfsren_mask;
  }
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mct0) = mct0;
  return k_ra_ok;
}

ra_err_t ra_mipi_csi_set_frame_error_mode(bool zlmd, bool edmd, bool rvmd)
{
  const ra_err_t state_err = internal_reject_if_running();
  RA_RETURN_ON_ERROR(state_err, s_tag, "set_frame_error_mode: rxen set");

  /* HUM Ch 66.3.2 "MCT0 : Module Control Register 0" p 3936 */
  uint32_t mct0 = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mct0);
  mct0 &= ~((uint32_t)k_ra_mipi_csi_mct0_zlmd_mask | (uint32_t)k_ra_mipi_csi_mct0_edmd_mask |
            (uint32_t)k_ra_mipi_csi_mct0_rvmd_mask);
  if (zlmd) {
    mct0 |= (uint32_t)k_ra_mipi_csi_mct0_zlmd_mask;
  }
  if (edmd) {
    mct0 |= (uint32_t)k_ra_mipi_csi_mct0_edmd_mask;
  }
  if (rvmd) {
    mct0 |= (uint32_t)k_ra_mipi_csi_mct0_rvmd_mask;
  }
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mct0) = mct0;
  return k_ra_ok;
}

/* =============================================================================
 * EPD / LRTE tuning
 * =============================================================================
 */

ra_err_t
ra_mipi_csi_set_epd(bool enable, bool option_2, uint16_t long_spacer, uint16_t short_spacer)
{
  if (((uint32_t)long_spacer & ~(uint32_t)k_ra_mipi_csi_epct_slp_mask) != 0UL) {
    return k_ra_err_invalid_arg;
  }
  if (((uint32_t)short_spacer &
       ~((uint32_t)k_ra_mipi_csi_epct_ssp_mask >> (uint32_t)k_ra_mipi_csi_epct_ssp_shift)) != 0UL) {
    return k_ra_err_invalid_arg;
  }

  const ra_err_t state_err = internal_reject_if_running();
  RA_RETURN_ON_ERROR(state_err, s_tag, "set_epd: rxen set");

  /* HUM Ch 66.3.7 "EPCT : EPD Option Control Register" p 3939 */
  uint32_t epct = ((uint32_t)long_spacer & (uint32_t)k_ra_mipi_csi_epct_slp_mask) |
                  (((uint32_t)short_spacer & ((uint32_t)k_ra_mipi_csi_epct_ssp_mask >>
                                              (uint32_t)k_ra_mipi_csi_epct_ssp_shift))
                   << (uint32_t)k_ra_mipi_csi_epct_ssp_shift);
  if (option_2) {
    epct |= (uint32_t)k_ra_mipi_csi_epct_epdop_mask;
  }
  if (enable) {
    epct |= (uint32_t)k_ra_mipi_csi_epct_epden_mask;
  }
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_epct) = epct;
  return k_ra_ok;
}

ra_err_t ra_mipi_csi_set_lrte(ra_mipi_csi_vlsien_t vlsien, bool eotp_enable)
{
  if ((uint8_t)vlsien > (uint8_t)k_ra_mipi_csi_vlsien_x4) {
    return k_ra_err_invalid_arg;
  }
  const ra_err_t state_err = internal_reject_if_running();
  RA_RETURN_ON_ERROR(state_err, s_tag, "set_lrte: rxen set");

  /* HUM Ch 66.3.8 "EMCT : EPD Misc Option Control Register" p 3941 */
  uint32_t emct = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_emct);
  emct &= ~((uint32_t)k_ra_mipi_csi_emct_vlsien_mask | (uint32_t)k_ra_mipi_csi_emct_eotpen_mask);
  emct |= ((uint32_t)vlsien << (uint32_t)k_ra_mipi_csi_emct_vlsien_shift) &
          (uint32_t)k_ra_mipi_csi_emct_vlsien_mask;
  if (eotp_enable) {
    emct |= (uint32_t)k_ra_mipi_csi_emct_eotpen_mask;
  }
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_emct) = emct;
  return k_ra_ok;
}

/* =============================================================================
 * Per-data-lane status / IRQ
 * =============================================================================
 */

ra_err_t ra_mipi_csi_dl_get_status(uint8_t lane, uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  if (lane > (uint8_t)k_ra_mipi_csi_dl_max) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 66.3.15 "DLST(N) : Data Lane N Status" p 3946 */
  const ra_mipi_csi_off_t off = ra_mipi_csi_dl_off(k_ra_mipi_csi_off_dlst0, lane);
  *out_mask                   = *ra_mipi_csi_reg32(off);
  return k_ra_ok;
}

ra_err_t ra_mipi_csi_dl_clear_status(uint8_t lane, uint32_t mask)
{
  if (lane > (uint8_t)k_ra_mipi_csi_dl_max) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 66.3.16 "DLSC(N) : Data Lane N Status Clear" p 3947 */
  const ra_mipi_csi_off_t off = ra_mipi_csi_dl_off(k_ra_mipi_csi_off_dlsc0, lane);
  *ra_mipi_csi_reg32(off)     = mask & (uint32_t)k_ra_mipi_csi_dlsc_all_mask;
  return k_ra_ok;
}

ra_err_t ra_mipi_csi_dl_set_irq_enable(uint8_t lane, uint32_t mask)
{
  if (lane > (uint8_t)k_ra_mipi_csi_dl_max) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 66.3.17 "DLIE(N) : Data Lane N Interrupt Enable" p 3948 */
  const ra_mipi_csi_off_t off = ra_mipi_csi_dl_off(k_ra_mipi_csi_off_dlie0, lane);
  *ra_mipi_csi_reg32(off)     = mask;
  return k_ra_ok;
}

/* =============================================================================
 * Per-VC status / IRQ
 * =============================================================================
 */

ra_err_t ra_mipi_csi_vc_get_status(uint8_t vc, uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  if (vc > (uint8_t)k_ra_mipi_csi_vc_max) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 66.3.18 "VCST(M) : Virtual Channel M Status" p 3949 */
  const ra_mipi_csi_off_t off = ra_mipi_csi_vc_off(k_ra_mipi_csi_off_vcst0, vc);
  *out_mask                   = *ra_mipi_csi_reg32(off);
  return k_ra_ok;
}

ra_err_t ra_mipi_csi_vc_clear_status(uint8_t vc, uint32_t mask)
{
  if (vc > (uint8_t)k_ra_mipi_csi_vc_max) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 66.3.19 "VCSC(M) : Virtual Channel M Status Clear" p 3951 */
  const ra_mipi_csi_off_t off = ra_mipi_csi_vc_off(k_ra_mipi_csi_off_vcsc0, vc);
  *ra_mipi_csi_reg32(off)     = mask;
  return k_ra_ok;
}

ra_err_t ra_mipi_csi_vc_set_irq_enable(uint8_t vc, uint32_t mask)
{
  if (vc > (uint8_t)k_ra_mipi_csi_vc_max) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 66.3.20 "VCIE(M) : Virtual Channel M Interrupt Enable" p 3952 */
  const ra_mipi_csi_off_t off = ra_mipi_csi_vc_off(k_ra_mipi_csi_off_vcie0, vc);
  *ra_mipi_csi_reg32(off)     = mask;
  return k_ra_ok;
}

/* =============================================================================
 * Power-management status / IRQ
 * =============================================================================
 */

ra_err_t ra_mipi_csi_pm_get_status(uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 66.3.21 "PMST : Power Management Status" p 3954 */
  *out_mask = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_pmst);
  return k_ra_ok;
}

ra_err_t ra_mipi_csi_pm_clear_status(uint32_t mask)
{
  /* HUM Ch 66.3.22 "PMSC : Power Management Status Clear" p 3955
   * Only [7:0] are W1C; hardware drops other bits. */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_pmsc) = mask & (uint32_t)k_ra_mipi_csi_pmsc_all_mask;
  return k_ra_ok;
}

ra_err_t ra_mipi_csi_pm_set_irq_enable(uint32_t mask)
{
  /* HUM Ch 66.3.23 "PMIE : Power Management Interrupt Enable" p 3956 */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_pmie) = mask & (uint32_t)k_ra_mipi_csi_pmie_all_mask;
  return k_ra_ok;
}

/* =============================================================================
 * Generic short-packet FIFO
 * =============================================================================
 */

ra_err_t ra_mipi_csi_short_packet_configure(uint8_t threshold, bool store_enable)
{
  if ((uint32_t)threshold > (uint32_t)k_ra_mipi_csi_gsct_shth_max) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 66.3.24 "GSCT : Generic Short Packet Control" p 3957 */
  uint32_t gsct = ((uint32_t)threshold & (uint32_t)k_ra_mipi_csi_gsct_shth_mask);
  if (store_enable) {
    gsct |= (uint32_t)k_ra_mipi_csi_gsct_gfif_mask;
  }
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsct) = gsct;
  return k_ra_ok;
}

ra_err_t ra_mipi_csi_short_packet_set_irq_enable(uint32_t mask)
{
  /* HUM Ch 66.3.27 "GSIE : Generic Short Packet Interrupt Enable" p 3958 */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsie) = mask & (uint32_t)k_ra_mipi_csi_gsie_all_mask;
  return k_ra_ok;
}

ra_err_t ra_mipi_csi_short_packet_get_status(uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 66.3.25 "GSST : Generic Short Packet Status" p 3957 */
  *out_mask = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsst);
  return k_ra_ok;
}

ra_err_t ra_mipi_csi_short_packet_clear_status(uint32_t mask)
{
  /* HUM Ch 66.3.26 "GSSC : Generic Short Packet Status Clear" p 3958
   * Only GOVC (bit 4) is W1C; hardware drops other bits. */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gssc) = mask & (uint32_t)k_ra_mipi_csi_gssc_govc_mask;
  return k_ra_ok;
}

ra_err_t ra_mipi_csi_read_short_packet(ra_mipi_csi_short_packet_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 66.3.25 "GSST : Generic Short Packet Status" p 3957
   * PNUM[15:8] reports queued packet count. */
  const uint32_t gsst = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsst);
  const uint32_t pnum =
    (gsst & (uint32_t)k_ra_mipi_csi_gsst_pnum_mask) >> (uint32_t)k_ra_mipi_csi_gsst_pnum_shift;
  if (pnum == 0UL) {
    return k_ra_err_empty;
  }
  /* HUM Ch 66.3.29 "GSIU : Generic Short Packet Information Update" p 3960
   * FINC advances the read pointer before we sample GSHT. */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsiu) = (uint32_t)k_ra_mipi_csi_gsiu_finc_mask;

  /* HUM Ch 66.3.28 "GSHT : Generic Short Packet Header" p 3959 */
  const uint32_t gsht = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsht);
  out->raw            = gsht;
  out->payload        = (uint16_t)((gsht & (uint32_t)k_ra_mipi_csi_gsht_spdt_mask) >>
                                   (uint32_t)k_ra_mipi_csi_gsht_spdt_shift);
  out->data_type      = (uint8_t)((gsht & (uint32_t)k_ra_mipi_csi_gsht_dtyp_mask) >>
                                  (uint32_t)k_ra_mipi_csi_gsht_dtyp_shift);
  out->vc             = (uint8_t)((gsht & (uint32_t)k_ra_mipi_csi_gsht_spvc_mask) >>
                                  (uint32_t)k_ra_mipi_csi_gsht_spvc_shift);
  return k_ra_ok;
}

ra_err_t ra_mipi_csi_short_packet_clear_fifo(void)
{
  /* HUM Ch 66.3.29 "GSIU : Generic Short Packet Information Update" p 3960
   * Drive GFCLR = 1 to request the clear, wait for GSST.GCD = 1, then
   * release GFCLR by writing 0. */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsiu) = (uint32_t)k_ra_mipi_csi_gsiu_gfclr_mask;

  ra_err_t err = k_ra_err_hw_timeout;
  for (uint16_t i = 0U; i < (uint16_t)k_ra_mipi_csi_gfclr_spin_max; ++i) {
    /* HUM Ch 66.3.25 "GSST : Generic Short Packet Status" p 3957 */
    const uint32_t gsst = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsst);
    if ((gsst & (uint32_t)k_ra_mipi_csi_gsst_gcd_mask) != 0UL) {
      err = k_ra_ok;
      break;
    }
  }

  /* Release the request line regardless of outcome. */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsiu) = 0UL;
  return err;
}

ra_err_t ra_mipi_csi_short_packet_re_enable_store(void)
{
  /* HUM Ch 66.3.29 "GSIU : Generic Short Packet Information Update" p 3960 */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsiu) = (uint32_t)k_ra_mipi_csi_gsiu_gfen_mask;
  return k_ra_ok;
}

/* =============================================================================
 * Interrupt path (handlers + dispatchers)
 * =============================================================================
 */

ra_err_t ra_mipi_csi_attach_handler(ra_mipi_csi_event_fn_t fn, void* ctx)
{
  s_mipi_csi_fn  = fn;
  s_mipi_csi_ctx = ctx;
  return k_ra_ok;
}

ra_err_t ra_mipi_csi_attach_dl_handler(ra_mipi_csi_dl_event_fn_t fn, void* ctx)
{
  s_mipi_csi_dl_fn  = fn;
  s_mipi_csi_dl_ctx = ctx;
  return k_ra_ok;
}

ra_err_t ra_mipi_csi_attach_vc_handler(ra_mipi_csi_vc_event_fn_t fn, void* ctx)
{
  s_mipi_csi_vc_fn  = fn;
  s_mipi_csi_vc_ctx = ctx;
  return k_ra_ok;
}

ra_err_t ra_mipi_csi_attach_pm_handler(ra_mipi_csi_pm_event_fn_t fn, void* ctx)
{
  s_mipi_csi_pm_fn  = fn;
  s_mipi_csi_pm_ctx = ctx;
  return k_ra_ok;
}

ra_err_t ra_mipi_csi_attach_short_packet_handler(ra_mipi_csi_short_event_fn_t fn, void* ctx)
{
  s_mipi_csi_gst_fn  = fn;
  s_mipi_csi_gst_ctx = ctx;
  return k_ra_ok;
}

void ra_mipi_csi_dispatch(void)
{
  /* HUM Ch 66.3.12 "RXST : Receive Status Register" p 3944 */
  const uint32_t rxst = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_rxst);
  /* HUM Ch 66.3.13 "RXSC : Receive Status Clear Register" p 3945
   * Clear sticky RACTDET so the next edge can be observed. */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_rxsc) = rxst & (uint32_t)k_ra_mipi_csi_rxsc_ractdetc_mask;

  const ra_mipi_csi_event_fn_t fn  = s_mipi_csi_fn;
  void* const                  ctx = s_mipi_csi_ctx;
  if (fn != nullptr) {
    fn(ctx, rxst);
  }
}

void ra_mipi_csi_dispatch_dl(void)
{
  /* HUM Ch 66.3.9 "MIST : Module Interrupt Status" p 3941 */
  const uint32_t mist = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mist);

  /* HUM Ch 66.3.15 "DLST(N) : Data Lane N Status" p 3946 */
  const uint32_t dlst0 = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_dlst0);
  const uint32_t dlst1 = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_dlst1);

  /* HUM Ch 66.3.16 "DLSC(N) : Data Lane N Status Clear" p 3947
   * ULP bit (24) is RO; mask it out before W1C. */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_dlsc0) = dlst0 & (uint32_t)k_ra_mipi_csi_dlsc_all_mask;
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_dlsc1) = dlst1 & (uint32_t)k_ra_mipi_csi_dlsc_all_mask;

  const ra_mipi_csi_dl_event_fn_t fn  = s_mipi_csi_dl_fn;
  void* const                     ctx = s_mipi_csi_dl_ctx;
  if (fn == nullptr) {
    return;
  }

  if ((mist & (uint32_t)k_ra_mipi_csi_mist_dl0s_mask) != 0UL) {
    fn(ctx, 0U, dlst0);
  }
  if ((mist & (uint32_t)k_ra_mipi_csi_mist_dl1s_mask) != 0UL) {
    fn(ctx, 1U, dlst1);
  }
}

void ra_mipi_csi_dispatch_vc(void)
{
  /* HUM Ch 66.3.9 "MIST : Module Interrupt Status" p 3941 */
  const uint32_t mist = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_mist);
  const uint32_t vc_flags =
    (mist & (uint32_t)k_ra_mipi_csi_mist_vc_mask) >> (uint32_t)k_ra_mipi_csi_mist_vc_shift;

  const ra_mipi_csi_vc_event_fn_t fn  = s_mipi_csi_vc_fn;
  void* const                     ctx = s_mipi_csi_vc_ctx;

  uint32_t generic_errors = 0UL;
  for (uint8_t vc = 0U; vc < (uint8_t)k_ra_mipi_csi_vc_count; ++vc) {
    if ((vc_flags & ((uint32_t)1U << vc)) == 0UL) {
      continue;
    }
    /* HUM Ch 66.3.18 "VCST(M) : Virtual Channel M Status" p 3949 */
    const ra_mipi_csi_off_t st_off  = ra_mipi_csi_vc_off(k_ra_mipi_csi_off_vcst0, vc);
    const ra_mipi_csi_off_t clr_off = ra_mipi_csi_vc_off(k_ra_mipi_csi_off_vcsc0, vc);
    const uint32_t          vcst    = *ra_mipi_csi_reg32(st_off);

    /* HUM Ch 66.3.19 "VCSC(M) : Virtual Channel M Status Clear" p 3951 */
    *ra_mipi_csi_reg32(clr_off) = vcst & (uint32_t)k_ra_mipi_csi_vcsc_per_vc_mask;

    /* MLF + ECD apply to every VC because the offending packet's VC
     * cannot be determined. Surface them once at the end. */
    generic_errors |= vcst & (uint32_t)k_ra_mipi_csi_vcst_generic_err_mask;
    const uint32_t vc_specific = vcst & ~(uint32_t)k_ra_mipi_csi_vcst_generic_err_mask;

    if (fn != nullptr) {
      fn(ctx, vc, vc_specific);
    }
  }

  if (fn != nullptr) {
    fn(ctx, (uint8_t)k_ra_mipi_csi_vc_invalid_idx, generic_errors);
  }
}

void ra_mipi_csi_dispatch_pm(void)
{
  /* HUM Ch 66.3.21 "PMST : Power Management Status" p 3954 */
  const uint32_t pmst = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_pmst);
  /* HUM Ch 66.3.22 "PMSC : Power Management Status Clear" p 3955
   * Only [7:0] are W1C; the upper state bits are RO. */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_pmsc) = pmst & (uint32_t)k_ra_mipi_csi_pmst_w1c_mask;

  const ra_mipi_csi_pm_event_fn_t fn  = s_mipi_csi_pm_fn;
  void* const                     ctx = s_mipi_csi_pm_ctx;
  if (fn != nullptr) {
    fn(ctx, pmst);
  }
}

void ra_mipi_csi_dispatch_short_packet(void)
{
  /* HUM Ch 66.3.25 "GSST : Generic Short Packet Status" p 3957 */
  const uint32_t gsst = *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gsst);
  /* HUM Ch 66.3.26 "GSSC : Generic Short Packet Status Clear" p 3958
   * Clear GOV so the next overflow can be observed. */
  *ra_mipi_csi_reg32(k_ra_mipi_csi_off_gssc) = gsst & (uint32_t)k_ra_mipi_csi_gssc_govc_mask;

  const ra_mipi_csi_short_event_fn_t fn  = s_mipi_csi_gst_fn;
  void* const                        ctx = s_mipi_csi_gst_ctx;
  if (fn != nullptr) {
    fn(ctx, gsst);
  }
}

/* =============================================================================
 * Power transition
 * =============================================================================
 */

ra_err_t ra_mipi_csi_enter_stop(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  return ra_mstp_disable(k_ra_mstp_mipi_csi);
}

ra_err_t ra_mipi_csi_exit_stop(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  return ra_mstp_enable(k_ra_mstp_mipi_csi);
}
