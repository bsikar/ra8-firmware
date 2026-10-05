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
static const char* const s_tag = "MIPI_DSI";

/* =============================================================================
 * Video mode
 *
 * video_configure, video_start, video_stop and set_video_timing live in Zig
 * (mipi_dsi_video_abi.zig, RA8FW-652).
 * =============================================================================
 */

/* =============================================================================
 * Status / IRQ
 *
 * get_status, link_status_get, clear_status, ack_error_get, rx_result_get,
 * rx_payload_read, te_event_pending/clear and irq_enable live in Zig
 * (mipi_dsi_status_abi.zig, RA8FW-648).
 * =============================================================================
 */

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
