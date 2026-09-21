/**
 * @file ra8_mipi_dsi_dispatch.c
 * @brief MIPI DSI-2 host driver -- video mode, status, IRQ dispatch, and
 *        convenience surfaces.
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Second translation unit of the hand-written HAL for the RA8D2 MIPI
 * DSI Host module (HUM Ch 65, p 3839-3934). The configuration, link,
 * HS-clock, sequence-channel command, and ULPS paths live in the sibling
 * ``ra8_mipi_dsi.c``; this file carries:
 *
 *  - video-mode configure / start / stop;
 *  - status getters (ISR, LINKSR, ack/error, receive-result, payload);
 *  - tearing-effect query / clear;
 *  - per-class interrupt enable + callback attach;
 *  - the per-class IRQ dispatch routines and the top-level dispatcher;
 *  - the "sweep 6" convenience surfaces (video timing, command send,
 *    ULPS shortcuts, link-status alias).
 *
 * The mutable state shared with the command-submission path (the
 * registered callback + the pending receive buffer) and the bounded
 * register-poll helper used by video mode are declared in
 * ``ra8_mipi_dsi_internal.h``.
 *
 * Every register access carries a HUM Ch 65 citation in the form
 * required by `scripts/checks/cite_check.py`:
 *
 *   /\* HUM Ch 65.X "name", p NNNN *\/
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_mipi_dsi.h"
#include "ra8_mipi_dsi_internal.h"
#include "ra8_mipi_dsi_regs.h"
#include "ra8_mstp.h"

/**
 * @var s_tag
 * @brief Component tag used by the `ra8_log_*` family.
 *
 * @details
 * Static so the linker keeps it confined to this TU. Same convention
 * as every other ra8_hal driver (see `ra8_glcdc.c`, `ra8_doc.c`). The
 * sibling ``ra8_mipi_dsi.c`` keeps its own identical copy -- read-only
 * constants are not shared across TUs.
 *
 * @note Read-only after assignment. Not modified at runtime.
 * @warning Never modify directly -- declared `const` to enforce.
 * @since 0.1.0
 */
static const char* s_tag = "MIPI_DSI";

/**
 * @brief Decode an RXRSS slot register into a struct.
 *
 * @param[in]  raw       Raw 32-bit RXRSS?R word.
 * @param[out] out_result Populated by the caller.
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_ra8_mipi_dsi_decode_rx(uint32_t raw, ra8_mipi_dsi_rx_result_t* out_result)
{
  out_result->data[0] = (uint8_t)(raw & k_ra8_mipi_dsi_rxrss_data0_mask);
  out_result->data[1] =
    (uint8_t)((raw & k_ra8_mipi_dsi_rxrss_data1_mask) >> (uint32_t)k_rxrss_data1_shift);
  out_result->cmd_id =
    (ra8_mipi_dsi_dt_t)((raw & k_ra8_mipi_dsi_rxrss_dt_mask) >> (uint32_t)k_rxrss_dt_shift);
  out_result->virtual_channel =
    (ra8_mipi_dsi_vc_t)((raw & k_ra8_mipi_dsi_rxrss_vc_mask) >> (uint32_t)k_rxrss_vc_shift);
  out_result->long_packet          = ((raw & k_ra8_mipi_dsi_rxrss_fmt) != 0U);
  out_result->rx_success           = ((raw & k_ra8_mipi_dsi_rxrss_rxsuc) != 0U);
  out_result->rx_fatal_error       = ((raw & k_ra8_mipi_dsi_rxrss_rxferr) != 0U);
  out_result->rx_fail              = ((raw & k_ra8_mipi_dsi_rxrss_rxfail) != 0U);
  out_result->rx_packet_data_fail  = ((raw & k_ra8_mipi_dsi_rxrss_rxpfail) != 0U);
  out_result->rx_correctable_error = ((raw & k_ra8_mipi_dsi_rxrss_rxcerr) != 0U);
  out_result->rx_ack_and_error     = ((raw & k_ra8_mipi_dsi_rxrss_rxake) != 0U);
  out_result->info_overwritten     = ((raw & k_ra8_mipi_dsi_rxrss_infoow) != 0U);
}

/* =============================================================================
 * Video mode
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_mipi_dsi_video_configure(const ra8_mipi_dsi_video_cfg_t* vcfg)
{
  RA8_CHECK_NULL_PTR(vcfg, s_tag, "vcfg must not be nullptr");
  if ((uint32_t)vcfg->virtual_channel > (uint32_t)k_ra8_mipi_dsi_vc3) {
    return k_ra8_err_invalid_arg;
  }

  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();

  /* HUM Ch 65.2 "VMSET1R : Video Mode Setting 1", p 3892 */
  reg->VMSET1R = (((uint32_t)vcfg->video_mode_delay) << k_ra8_mipi_dsi_vmset1_dly_shift) &
                 k_ra8_mipi_dsi_vmset1_dly_mask;

  /* HUM Ch 65.2 "VMPPSETR : Video Mode Pixel Packet Setting", p 3896 */
  uint32_t vmpp =
    (((uint32_t)vcfg->pixel_format) << k_ra8_mipi_dsi_vmpp_dt_shift) & k_ra8_mipi_dsi_vmpp_dt_mask;
  vmpp |= (((uint32_t)vcfg->virtual_channel) << k_ra8_mipi_dsi_vmpp_vc_shift) &
          k_ra8_mipi_dsi_vmpp_vc_mask;
  if (vcfg->sync_pulse) {
    vmpp |= k_ra8_mipi_dsi_vmpp_txesync;
  }
  reg->VMPPSETR = vmpp;

  /* HUM Ch 65.2 "VMVSSETR : Video Mode Vertical Sync Setting", p 3897 */
  uint32_t vmvs = (((uint32_t)vcfg->vertical_sync_lines) & k_ra8_mipi_dsi_vmvs_vsa_mask);
  vmvs |= (((uint32_t)vcfg->vertical_active_lines) << k_ra8_mipi_dsi_vmvs_vact_shift) &
          k_ra8_mipi_dsi_vmvs_vact_mask;
  if (vcfg->vsync_active_high) {
    vmvs |= k_ra8_mipi_dsi_vmvs_vspol;
  }
  reg->VMVSSETR = vmvs;

  /* HUM Ch 65.2 "VMVPSETR : Video Mode Vertical Porch Setting", p 3898 */
  uint32_t vmvp = ((uint32_t)vcfg->vertical_back_porch) & k_ra8_mipi_dsi_vmvp_vbp_mask;
  vmvp |= (((uint32_t)vcfg->vertical_front_porch) << k_ra8_mipi_dsi_vmvp_vfp_shift) &
          k_ra8_mipi_dsi_vmvp_vfp_mask;
  reg->VMVPSETR = vmvp;

  /* HUM Ch 65.2 "VMHSSETR : Video Mode Horizontal Sync Setting", p 3899 */
  uint32_t vmhs = ((uint32_t)vcfg->horizontal_sync_lines) & k_ra8_mipi_dsi_vmhs_hsa_mask;
  vmhs |= (((uint32_t)vcfg->horizontal_active_pixels) << k_ra8_mipi_dsi_vmhs_hact_shift) &
          k_ra8_mipi_dsi_vmhs_hact_mask;
  if (vcfg->hsync_active_high) {
    vmhs |= k_ra8_mipi_dsi_vmhs_hspol;
  }
  reg->VMHSSETR = vmhs;

  /* HUM Ch 65.2 "VMHPSETR : Video Mode Horizontal Porch Setting", p 3899 */
  uint32_t vmhp = ((uint32_t)vcfg->horizontal_back_porch) & k_ra8_mipi_dsi_vmhp_hbp_mask;
  vmhp |= (((uint32_t)vcfg->horizontal_front_porch) << k_ra8_mipi_dsi_vmhp_hfp_shift) &
          k_ra8_mipi_dsi_vmhp_hfp_mask;
  reg->VMHPSETR = vmhp;

  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_video_start(const ra8_mipi_dsi_video_cfg_t* vcfg)
{
  RA8_CHECK_NULL_PTR(vcfg, s_tag, "vcfg must not be nullptr");
  volatile r_mipi_dsi_regs_t* reg   = ra8_mipi_dsi();
  uint32_t                    vmset = k_ra8_mipi_dsi_vmset0_vstart;
  if (vcfg->hsa_no_lp) {
    vmset |= k_ra8_mipi_dsi_vmset0_hsanolp;
  }
  if (vcfg->hbp_no_lp) {
    vmset |= k_ra8_mipi_dsi_vmset0_hbpnolp;
  }
  if (vcfg->hfp_no_lp) {
    vmset |= k_ra8_mipi_dsi_vmset0_hfpnolp;
  }
  /* HUM Ch 65.2 "VMSET0R : Video Mode Setting 0", p 3891 */
  reg->VMSET0R = vmset;
  /* HUM Ch 65.2 "VMSR : Video Mode Status Register", p 3893 */
  return priv_ra8_mipi_dsi_internal_wait_eq(&reg->VMSR,
                                            k_ra8_mipi_dsi_vmsr_virdy,
                                            k_ra8_mipi_dsi_vmsr_virdy);
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_video_stop(void)
{
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  /* HUM Ch 65.2 "VMSET0R : Video Mode Setting 0", p 3891 */
  reg->VMSET0R = k_ra8_mipi_dsi_vmset0_vstop;
  /* HUM Ch 65.2 "VMSR : Video Mode Status Register", p 3893 */
  const ra8_err_t err = priv_ra8_mipi_dsi_internal_wait_eq(&reg->VMSR,
                                                           k_ra8_mipi_dsi_vmsr_stop,
                                                           k_ra8_mipi_dsi_vmsr_stop);
  if (err == k_ra8_ok) {
    /* HUM Ch 65.2 "VMSCR : Video Mode Status Clear", p 3894 */
    reg->VMSCR = k_ra8_mipi_dsi_vmsr_clear_all;
  }
  return err;
}

/* =============================================================================
 * Status / IRQ
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_mipi_dsi_get_status(uint32_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 65.2 "ISR : Interrupt Status Register", p 3840 */
  *out_mask = ra8_mipi_dsi()->ISR;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_link_status_get(ra8_mipi_dsi_link_status_t* out_status)
{
  RA8_CHECK_NULL_PTR(out_status, s_tag, "out_status must not be nullptr");
  /* HUM Ch 65.2 "LINKSR : Link Status Register", p 3842 */
  const uint32_t v                 = ra8_mipi_dsi()->LINKSR;
  out_status->sequence_ch0_running = ((v & k_ra8_mipi_dsi_link_sq0run) != 0U);
  out_status->sequence_ch1_running = ((v & k_ra8_mipi_dsi_link_sq1run) != 0U);
  out_status->video_running        = ((v & k_ra8_mipi_dsi_link_vrun) != 0U);
  out_status->hs_busy              = ((v & k_ra8_mipi_dsi_link_hsbusy) != 0U);
  out_status->lp_busy              = ((v & k_ra8_mipi_dsi_link_lpbusy) != 0U);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_clear_status(uint32_t mask)
{
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  if ((mask & k_ra8_mipi_dsi_isr_sq0) != 0U) {
    /* HUM Ch 65.2 "SQCH0SCR : Sequence Channel 0 Status Clear", p 3902 */
    reg->SQCH0SCR = k_ra8_mipi_dsi_sqch_clear_all;
  }
  if ((mask & k_ra8_mipi_dsi_isr_sq1) != 0U) {
    /* HUM Ch 65.2 "SQCH1SCR : Sequence Channel 1 Status Clear", p 3907 */
    reg->SQCH1SCR = k_ra8_mipi_dsi_sqch_clear_all;
  }
  if ((mask & k_ra8_mipi_dsi_isr_vm) != 0U) {
    /* HUM Ch 65.2 "VMSCR : Video Mode Status Clear", p 3894 */
    reg->VMSCR = k_ra8_mipi_dsi_vmsr_clear_all;
  }
  if ((mask & k_ra8_mipi_dsi_isr_rcv) != 0U) {
    /* HUM Ch 65.2 "RXSCR : Receive Status Clear", p 3855 */
    reg->RXSCR = k_ra8_mipi_dsi_rxsr_clear_all;
  }
  if ((mask & k_ra8_mipi_dsi_isr_ferr) != 0U) {
    /* HUM Ch 65.2 "FERRSCR : Fatal Error Status Clear", p 3878 */
    reg->FERRSCR = k_ra8_mipi_dsi_ferrsr_clear_all;
  }
  if ((mask & k_ra8_mipi_dsi_isr_ppi) != 0U) {
    /* HUM Ch 65.2 "PLSCR : PHY Lane Status Clear", p 3887 */
    reg->PLSCR = k_ra8_mipi_dsi_plsr_clear_all;
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_ack_error_get(ra8_mipi_dsi_ack_error_t* out_err)
{
  RA8_CHECK_NULL_PTR(out_err, s_tag, "out_err must not be nullptr");
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  /* HUM Ch 65.2 "AKEPACMSR : Ack/Error Report Accumulated Status", p 3864 */
  const uint32_t v      = reg->AKEPACMSR;
  out_err->error_report = (uint16_t)(v & k_ra8_mipi_dsi_akep_erep_mask);
  out_err->virtual_channel =
    (ra8_mipi_dsi_vc_t)(((v & k_ra8_mipi_dsi_akep_vc_mask) >> (uint32_t)k_akep_vc_shift) &
                        k_ra8_mipi_dsi_vc_mask);
  /* HUM Ch 65.2 "AKEPSCR : Ack/Error Report Status Clear", p 3865 */
  reg->AKEPSCR = v;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_rx_result_get(uint8_t                   slot,
                                                   ra8_mipi_dsi_rx_result_t* out_result)
{
  RA8_CHECK_NULL_PTR(out_result, s_tag, "out_result must not be nullptr");
  if (slot >= (uint8_t)k_ra8_mipi_dsi_rx_slots) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  /* HUM Ch 65.2 "RXRSSR : Receive Result Save Status Register", p 3866 */
  const uint32_t valid_bit = (1U << slot);
  if ((reg->RXRSSR & valid_bit) == 0U) {
    return k_ra8_err_no_data;
  }
  uint32_t raw = 0U;
  /* HUM Ch 65.2 "RXRSS0R..RXRSS3R : Receive Result Save 0..3", p 3879 */
  switch (slot) {
    case 0U:
      raw = reg->RXRSS0R;
      break;
    case 1U:
      raw = reg->RXRSS1R;
      break;
    case 2U:
      raw = reg->RXRSS2R;
      break;
    case 3U:
    default:
      raw = reg->RXRSS3R;
      break;
  }
  internal_ra8_mipi_dsi_decode_rx(raw, out_result);
  /* HUM Ch 65.2 "RXRSSCR : Receive Result Save Status Clear", p 3867 */
  reg->RXRSSCR = valid_bit;
  /* HUM Ch 65.2 "RXRINFOOWSCR : Receive Result Info-Overwrite Clear", p 3869 */
  reg->RXRINFOOWSCR = valid_bit;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t
ra8_mipi_dsi_rx_payload_read(uint8_t* dest, uint16_t max_len, uint16_t* out_len)
{
  RA8_CHECK_NULL_PTR(dest, s_tag, "dest must not be nullptr");
  RA8_CHECK_NULL_PTR(out_len, s_tag, "out_len must not be nullptr");
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  /* HUM Ch 65.2 "RXPPD0R..RXPPD3R : Receive Packet Payload 0..3", p 3879 */
  const uint32_t words[4] = {reg->RXPPD0R, reg->RXPPD1R, reg->RXPPD2R, reg->RXPPD3R};
  const uint16_t cap      = k_ra8_mipi_dsi_payload_max;
  const uint16_t eff      = (max_len < cap) ? max_len : cap;
  for (uint16_t i = 0U; i < eff; ++i) {
    const uint16_t word_idx = (uint16_t)(i / 4U);
    const uint16_t byte_idx = (uint16_t)(i % 4U);
    dest[i] =
      (uint8_t)((words[word_idx] >> (byte_idx * (uint32_t)k_shift_8)) & k_ra8_mipi_dsi_byte_mask);
  }
  *out_len = eff;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_te_event_pending(bool* out_pending)
{
  RA8_CHECK_NULL_PTR(out_pending, s_tag, "out_pending must not be nullptr");
  /* HUM Ch 65.2 "RXSR : Receive Status Register", p 3852 */
  const uint32_t v       = ra8_mipi_dsi()->RXSR;
  const uint32_t te_mask = k_ra8_mipi_dsi_rxsr_rxte | k_ra8_mipi_dsi_rxsr_extedet;
  *out_pending           = ((v & te_mask) != 0U);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_te_event_clear(void)
{
  /* HUM Ch 65.2 "RXSCR : Receive Status Clear", p 3855 */
  ra8_mipi_dsi()->RXSCR = k_ra8_mipi_dsi_rxsr_rxte | k_ra8_mipi_dsi_rxsr_extedet;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t
ra8_mipi_dsi_irq_enable(ra8_mipi_dsi_event_t event, uint32_t mask, bool enable)
{
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  volatile uint32_t*          ier = nullptr;
  switch (event) {
    case k_ra8_mipi_dsi_event_seq0:
      /* HUM Ch 65.2 "SQCH0IER : Sequence Channel 0 Interrupt Enable", p 3903 */
      ier = &reg->SQCH0IER;
      break;
    case k_ra8_mipi_dsi_event_seq1:
      /* HUM Ch 65.2 "SQCH1IER : Sequence Channel 1 Interrupt Enable", p 3909 */
      ier = &reg->SQCH1IER;
      break;
    case k_ra8_mipi_dsi_event_video:
      /* HUM Ch 65.2 "VMIER : Video Mode Interrupt Enable", p 3895 */
      ier = &reg->VMIER;
      break;
    case k_ra8_mipi_dsi_event_receive:
      /* HUM Ch 65.2 "RXIER : Receive Interrupt Enable", p 3858 */
      ier = &reg->RXIER;
      break;
    case k_ra8_mipi_dsi_event_fatal:
      /* HUM Ch 65.2 "FERRIER : Fatal Error Interrupt Enable", p 3879 */
      ier = &reg->FERRIER;
      break;
    case k_ra8_mipi_dsi_event_phy:
      /* HUM Ch 65.2 "PLIER : PHY Lane Interrupt Enable", p 3889 */
      ier = &reg->PLIER;
      break;
    default:
      return k_ra8_err_invalid_arg;
  }
  if (enable) {
    *ier |= mask;
  } else {
    *ier &= ~mask;
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_attach_handler(ra8_mipi_dsi_event_fn_t fn, void* ctx)
{
  s_mipi_dsi_event_fn  = fn;
  s_mipi_dsi_event_ctx = ctx;
  return k_ra8_ok;
}

/* =============================================================================
 * Per-class dispatch
 * =============================================================================
 */

/**
 * @brief Common helper that fires the registered callback.
 *
 * @param[in] event Class enum.
 * @param[in] mask  Status bits captured before clearing.
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_ra8_mipi_dsi_call_user(ra8_mipi_dsi_event_t event, uint32_t mask)
{
  const ra8_mipi_dsi_event_fn_t fn  = s_mipi_dsi_event_fn;
  void* const                   ctx = s_mipi_dsi_event_ctx;
  if (fn != nullptr) {
    fn(ctx, event, mask);
  }
}

RA8_ISR_SAFE
void ra8_mipi_dsi_dispatch_seq0(void)
{
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  /* HUM Ch 65.2 "SQCH0SR : Sequence Channel 0 Status Register", p 3900 */
  const uint32_t bits = reg->SQCH0SR;
  /* HUM Ch 65.2 "SQCH0SCR : Sequence Channel 0 Status Clear", p 3902 */
  reg->SQCH0SCR = bits & k_ra8_mipi_dsi_sqch_clear_all;
  internal_ra8_mipi_dsi_call_user(k_ra8_mipi_dsi_event_seq0, bits);
}

RA8_ISR_SAFE
void ra8_mipi_dsi_dispatch_seq1(void)
{
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  /* HUM Ch 65.2 "SQCH1SR : Sequence Channel 1 Status Register", p 3905 */
  const uint32_t bits = reg->SQCH1SR;
  /* HUM Ch 65.2 "SQCH1SCR : Sequence Channel 1 Status Clear", p 3907 */
  reg->SQCH1SCR = bits & k_ra8_mipi_dsi_sqch_clear_all;
  internal_ra8_mipi_dsi_call_user(k_ra8_mipi_dsi_event_seq1, bits);
}

RA8_ISR_SAFE
void ra8_mipi_dsi_dispatch_video(void)
{
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  /* HUM Ch 65.2 "VMSR : Video Mode Status Register", p 3893 */
  const uint32_t bits = reg->VMSR;
  /* HUM Ch 65.2 "VMSCR : Video Mode Status Clear", p 3894 */
  reg->VMSCR = bits & k_ra8_mipi_dsi_vmsr_clear_all;
  /* If buffer over/underflow, FSP recommends a soft reset; mirror that. */
  if ((bits & (k_ra8_mipi_dsi_vmsr_vbufovf | k_ra8_mipi_dsi_vmsr_vbufudf)) != 0U) {
    /* HUM Ch 65.2 "RSTCR : Reset Control Register", p 3845 */
    reg->RSTCR = k_ra8_mipi_dsi_rstcr_swrst;
    /* HUM Ch 65.2 "RSTCR : Reset Control Register", p 3845 */
    reg->RSTCR = 0U;
  }
  internal_ra8_mipi_dsi_call_user(k_ra8_mipi_dsi_event_video, bits);
}

RA8_ISR_SAFE
void ra8_mipi_dsi_dispatch_receive(void)
{
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  /* HUM Ch 65.2 "RXSR : Receive Status Register", p 3852 */
  const uint32_t bits = reg->RXSR;
  /* HUM Ch 65.2 "RXSCR : Receive Status Clear", p 3855 */
  reg->RXSCR = bits & k_ra8_mipi_dsi_rxsr_clear_all;
  /* If a response packet arrived, copy RXPPD into the pending buffer. */
  if ((bits & k_ra8_mipi_dsi_rxsr_rxresp) != 0U) {
    // mcdc-deactivated: ra8_mipi_dsi_dispatch_receive pending-RX gate; s_mipi_dsi_pending_rx_buffer and s_mipi_dsi_pending_rx_len are written together (atomic pair) by ra8_mipi_dsi_rx_payload_register; the buffer is never set without a non-zero length and vice-versa, so the conditions are co-dependent on any reachable path.
    if ((s_mipi_dsi_pending_rx_buffer != nullptr) && (s_mipi_dsi_pending_rx_len > 0U)) {
      uint16_t got = 0U;
      (void)ra8_mipi_dsi_rx_payload_read(s_mipi_dsi_pending_rx_buffer,
                                         s_mipi_dsi_pending_rx_len,
                                         &got);
      s_mipi_dsi_pending_rx_buffer = nullptr;
      s_mipi_dsi_pending_rx_len    = 0U;
    }
  }
  /* HUM Ch 65.2 "RXRINFOOWSCR : Receive Result Info-Overwrite Clear", p 3869 */
  reg->RXRINFOOWSCR = k_ra8_mipi_dsi_rxrinfoow_sl0;
  internal_ra8_mipi_dsi_call_user(k_ra8_mipi_dsi_event_receive, bits);
}

RA8_ISR_SAFE
void ra8_mipi_dsi_dispatch_fatal(void)
{
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  /* HUM Ch 65.2 "FERRSR : Fatal Error Status Register", p 3876 */
  const uint32_t bits = reg->FERRSR;
  /* HUM Ch 65.2 "FERRSCR : Fatal Error Status Clear", p 3878 */
  reg->FERRSCR = bits & k_ra8_mipi_dsi_ferrsr_clear_all;
  internal_ra8_mipi_dsi_call_user(k_ra8_mipi_dsi_event_fatal, bits);
}

RA8_ISR_SAFE
void ra8_mipi_dsi_dispatch_phy(void)
{
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  /* HUM Ch 65.2 "PLSR : PHY Lane Status Register", p 3884 */
  const uint32_t bits = reg->PLSR;
  /* HUM Ch 65.2 "PLSCR : PHY Lane Status Clear", p 3887 */
  reg->PLSCR = bits & k_ra8_mipi_dsi_plsr_clear_all;
  internal_ra8_mipi_dsi_call_user(k_ra8_mipi_dsi_event_phy, bits);
}

RA8_ISR_SAFE
void ra8_mipi_dsi_dispatch(void)
{
  /* HUM Ch 65.2 "ISR : Interrupt Status Register", p 3840 */
  const uint32_t snapshot = ra8_mipi_dsi()->ISR;
  if ((snapshot & k_ra8_mipi_dsi_isr_sq0) != 0U) {
    ra8_mipi_dsi_dispatch_seq0();
  }
  if ((snapshot & k_ra8_mipi_dsi_isr_sq1) != 0U) {
    ra8_mipi_dsi_dispatch_seq1();
  }
  if ((snapshot & k_ra8_mipi_dsi_isr_vm) != 0U) {
    ra8_mipi_dsi_dispatch_video();
  }
  if ((snapshot & k_ra8_mipi_dsi_isr_rcv) != 0U) {
    ra8_mipi_dsi_dispatch_receive();
  }
  if ((snapshot & k_ra8_mipi_dsi_isr_ferr) != 0U) {
    ra8_mipi_dsi_dispatch_fatal();
  }
  if ((snapshot & k_ra8_mipi_dsi_isr_ppi) != 0U) {
    ra8_mipi_dsi_dispatch_phy();
  }
  /* If nothing was set, still call user with mask=0 so the legacy
   * "always invoke" contract from the previous revision is preserved. */
  if ((snapshot & k_ra8_mipi_dsi_isr_all) == 0U) {
    internal_ra8_mipi_dsi_call_user(k_ra8_mipi_dsi_event_phy, 0U);
  }
}

/* =============================================================================
 * Sweep 6 convenience surfaces
 * =============================================================================
 */

/**
 * @enum ra8_mipi_dsi_timing_limits_t
 * @brief Per-field bit-width caps for ::ra8_mipi_dsi_video_timing_t.
 *
 * @details
 * HUM Ch 65.2 register fields impose these maxima:
 * VSA/HSA = 12 bits, V/HBP + V/HFP = 13 bits, V/HACT = 15 bits.
 * Used by ::ra8_mipi_dsi_set_video_timing to range-check inputs.
 */
typedef enum : uint16_t {
  k_ra8_mipi_dsi_timing_max_sync   = 0x0FFFU, /**< 12-bit field. */
  k_ra8_mipi_dsi_timing_max_porch  = 0x1FFFU, /**< 13-bit field. */
  k_ra8_mipi_dsi_timing_max_active = 0x7FFFU, /**< 15-bit field. */
} ra8_mipi_dsi_timing_limits_t;

[[nodiscard]] ra8_err_t ra8_mipi_dsi_set_video_timing(const ra8_mipi_dsi_video_timing_t* timing)
{
  RA8_CHECK_NULL_PTR(timing, s_tag, "timing must not be nullptr");

  /* Range-check every field against its register-width limit. */
  if ((timing->horizontal_sync > k_ra8_mipi_dsi_timing_max_sync) ||
      (timing->vertical_sync > k_ra8_mipi_dsi_timing_max_sync)) {
    return k_ra8_err_invalid_arg;
  }
  if ((timing->horizontal_back_porch > k_ra8_mipi_dsi_timing_max_porch) ||
      (timing->horizontal_front_porch > k_ra8_mipi_dsi_timing_max_porch) ||
      (timing->vertical_back_porch > k_ra8_mipi_dsi_timing_max_porch) ||
      (timing->vertical_front_porch > k_ra8_mipi_dsi_timing_max_porch)) {
    return k_ra8_err_invalid_arg;
  }
  if ((timing->horizontal_active > k_ra8_mipi_dsi_timing_max_active) ||
      (timing->vertical_active > k_ra8_mipi_dsi_timing_max_active)) {
    return k_ra8_err_invalid_arg;
  }

  /* Build a full video config with sensible defaults: RGB888 on VC0,
   * sync-pulse off, blanking stays HS so the panel does not drop the
   * link between lines. */
  const ra8_mipi_dsi_video_cfg_t v = {
    .pixel_format             = k_ra8_mipi_dsi_dt_pixel_rgb888,
    .virtual_channel          = k_ra8_mipi_dsi_vc0,
    .sync_pulse               = false,
    .hsa_no_lp                = true,
    .hbp_no_lp                = true,
    .hfp_no_lp                = true,
    .vsync_active_high        = true,
    .hsync_active_high        = true,
    .vertical_sync_lines      = timing->vertical_sync,
    .vertical_active_lines    = timing->vertical_active,
    .vertical_back_porch      = timing->vertical_back_porch,
    .vertical_front_porch     = timing->vertical_front_porch,
    .horizontal_sync_lines    = timing->horizontal_sync,
    .horizontal_active_pixels = timing->horizontal_active,
    .horizontal_back_porch    = timing->horizontal_back_porch,
    .horizontal_front_porch   = timing->horizontal_front_porch,
    .video_mode_delay         = 0U,
  };
  return ra8_mipi_dsi_video_configure(&v);
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_send_command_short(ra8_mipi_dsi_dt_t dt,
                                                        const uint8_t     params[2])
{
  RA8_CHECK_NULL_PTR(params, s_tag, "params must not be nullptr");
  return ra8_mipi_dsi_send_short_packet(dt, k_ra8_mipi_dsi_vc0, params[0], params[1]);
}

[[nodiscard]] ra8_err_t
ra8_mipi_dsi_send_command_long(ra8_mipi_dsi_dt_t dt, const uint8_t* payload, uint16_t len)
{
  if ((len > 0U) && (payload == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  /* HS path on VC0 -- command-mode panels always want this routing. */
  return ra8_mipi_dsi_send_long_packet(dt, k_ra8_mipi_dsi_vc0, payload, len, false);
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_send_command_payload(ra8_mipi_dsi_dt_t packet_type,
                                                          const uint8_t*    payload,
                                                          uint16_t          len)
{
  if ((len > 0U) && (payload == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  /* HUM Ch 65 "Command-mode packet TX" pp 3839-3934 -- short writes
   * pack the payload into the 2-parameter header; long writes stage
   * via TXPPD0..3R. */
  enum : uint16_t {
    k_ra8_mipi_dsi_short_payload_max = 2U, /**< RA8 mipi dsi short payload maximum. */
  };
  if (len <= k_ra8_mipi_dsi_short_payload_max) {
    const uint8_t p0 = (len > 0U) ? payload[0] : 0U;
    const uint8_t p1 = (len > 1U) ? payload[1] : 0U;
    return ra8_mipi_dsi_send_short_packet(packet_type, k_ra8_mipi_dsi_vc0, p0, p1);
  }
  /* Long packet through LP escape (low_power = true). */
  return ra8_mipi_dsi_send_long_packet(packet_type, k_ra8_mipi_dsi_vc0, payload, len, true);
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_enter_ulps(void)
{
  return ra8_mipi_dsi_ulps_enter(k_ra8_mipi_dsi_lane_all);
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_exit_ulps(void)
{
  return ra8_mipi_dsi_ulps_exit(k_ra8_mipi_dsi_lane_all);
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_get_link_status(ra8_mipi_dsi_link_status_t* out)
{
  return ra8_mipi_dsi_link_status_get(out);
}
