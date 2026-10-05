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

/* =============================================================================
 * Handler attach and dispatch
 *
 * attach_handler, the per-class dispatch trampolines and the ISR fan-out
 * live in Zig (mipi_dsi_dispatch_abi.zig, RA8FW-658).
 * =============================================================================
 */

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
