/**
 * @file board_usb.c
 * @brief USBFS controller model + virtual USB host (chapter-9 enumeration)
 *
 * @details
 * Implements the model declared in board_usb.h. The RA8D2 USBFS controller
 * (base 0x40250000, HUM Ch 36) is modelled register-by-register with real
 * staging buffers for the control (DCP) and bulk pipe FIFOs, and a virtual USB
 * host walks the standard chapter-9 enumeration against the real device-side
 * firmware (port/usbx/ux_dcd_ra8_usb -> ra8_usb*.c). The host delivers each SETUP
 * by latching USBREQ..USBLENG and raising the controller's CTRT interrupt
 * through board_periph's ICU -> NVIC path, then drains the descriptor bytes the
 * device pushes into the CFIFO and advances the device state machine
 * (DVSQ powered -> default -> address -> configured) until USBX activates its
 * CDC-ACM class.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "board_usb.h"

#include <stdio.h>
#include <string.h>

#include "board_console.h"
#include "board_usb_internal.h"
#include "ra8_elc_regs.h"
#include "ra8_usb_regs.h"

usb_state_t            s_usb;
bool                   s_trace;
board_usb_irq_raiser_t s_raise;

/* Set true when a real-firmware USB host (the emulated USBHS controller in
 * board_periph_usbhs_host.c) drives THIS device model through the bridge below;
 * the built-in virtual chapter-9 host then stands down (board_usb_tick returns
 * early) so exactly one host drives the device -- the chip-internal self-loop. */
bool s_external_host;

/* Self-loop role polarity (see the role-routing section of board_usb.h).
 * false: USBFS window = this device model, USBHS window = host model (Config
 * A). true: swapped (Config B) -- the firmware declared the FS controller the
 * host (SYSCFG.DCFM=1 written to the FS window) or the HS controller the
 * device (SYSCFG.DPRPU=1 written to the HS window), so the FS window routes to
 * the host model and the HS window routes here. Sticky until board_usb_init. */
bool s_roles_swapped;

/* ICU event the device model pends for its interrupts: USBFS_INT while the
 * device model sits behind the USBFS window (Config A), USBHS_USB_INT_RESUME
 * once the roles swap and the DCD drives the USBHS controller (Config B). */
uint16_t s_dev_irq_event = (uint16_t)k_ra8_elc_event_usbfs_int;

/* DCP control-OUT "wire" holding buffer (bridge path). The polled host delivers
 * a control-write data stage (e.g. a DFU_DNLOAD block) before the device -- off
 * its own IRQ on the shared core -- has armed its DCP to receive it, and the
 * device's arm-time CFIFO BCLR would discard data landed too early. So the
 * bytes are held here as "in flight on the wire" and handed to the device DCP
 * only once it has armed (PID=BUF + DCP BRDY enabled), exactly as the SIE would
 * deliver an OUT packet the device is finally ready to ACK. */
uint8_t  s_dcp_hold[k_usb_in_cap]; /**< Held control-OUT wire bytes.     */
uint16_t s_dcp_hold_len;           /**< Held byte count.                 */
bool     s_dcp_hold_pending;       /**< Held bytes await the device arm. */

/* Virtual-host bookkeeping. */
uint8_t  s_host_phase;    /**< Current host state-machine phase.   */
uint8_t  s_host_step;     /**< Index into the enumeration script.  */
uint8_t  s_host_substate; /**< Sub-state within one SETUP step.    */
uint32_t s_host_wait;     /**< Ticks spent waiting in a sub-state. */
bool     s_configured;    /**< Device reached CONFIGURED.          */
uint32_t s_usb_irqs;      /**< USB interrupts the host raised.     */

/* Bulk-echo (secondary CDC check) bookkeeping. */
uint8_t  s_echo_out[k_usb_echo_cap]; /**< Host bulk-OUT payload.     */
uint32_t s_echo_out_len;             /**< Bytes queued by --usb-in.  */
uint32_t s_echo_out_sent;            /**< Bytes delivered to device. */
uint32_t s_echo_in_got;              /**< Echoed bytes read back.    */

/* Enumeration-step log (host SETUP -> device stage), shown in the report. */
static char     s_log[k_usb_log_cap][k_usb_log_width];
static uint32_t s_log_n;

/* =============================================================================
 * Small register / logging helpers.
 * =============================================================================
 */

/** @brief Append one already-formatted line to the enumeration-step log. */
void usb_log_line(const char* msg)
{
  /* Mirror this completed USB event (one SETUP step / state change / bulk
   * buffer -- already coalesced by the caller, never per byte) onto the board
   * view's USB console tab. This is an in-memory store only and does not touch
   * stdout, so the smoke / golden output stays byte-identical. Pushed outside
   * the s_log cap below so the live tab keeps flowing past the report cap. */
  board_console_push(k_board_console_ch_usb, msg);
  if (s_log_n < (uint32_t)k_usb_log_cap) {
    (void)snprintf(s_log[s_log_n], sizeof(s_log[0]), "%s", msg);
    if (s_trace) {
      (void)fprintf(stderr, "  [usb] %s\n", s_log[s_log_n]);
    }
    s_log_n++;
  }
}

/** @brief Append a "<label>: <n> byte(s)"-style line built from one count. */
void usb_log_count(const char* label, unsigned n)
{
  char line[k_usb_log_width];
  (void)snprintf(line, sizeof(line), "%s: %u byte(s)", label, n);
  usb_log_line(line);
}

void board_usb_tick(uc_engine* uc)
{
  /* Latch the CONFIGURED marker as soon as the device state reaches it -- the
   * host sets DVSQ, but record it here so a single source of truth drives both
   * the success assertion and the report marker. */
  if (s_usb.dvsq == (uint16_t)k_ra8_dvsq_configured) {
    s_configured = true;
  }
  /* A real-firmware USB host is driving the device through the bridge (the
   * chip-internal self-loop): the built-in virtual host must not also drive it,
   * or two hosts would race the device's control endpoint. Stand down -- but
   * still pump the device's level-triggered DCP-OUT receive so a control-write
   * data stage the host delivered before the device armed still lands. */
  if (s_external_host) {
    bridge_pump_device(uc);
    return;
  }
  /* Self-loop bench (register-level USBHS host model, board_usb_host.c): the
   * firmware's own host drives this device over the loop cable
   * (board_usb_loop_*), so the built-in virtual host must not compete for the
   * control pipe or drain the bulk-IN data. */
  if (s_loop_latched) {
    return;
  }
  switch ((usb_host_phase_t)s_host_phase) {
    case k_phase_idle:
      host_run_idle_phase(uc);
      break;
    case k_phase_reset:
      host_run_reset_phase(uc);
      break;
    case k_phase_setup:
      host_run_setup_phase(uc);
      break;
    case k_phase_configured:
      host_run_configured_phase(uc);
      break;
    case k_phase_done:
    default:
      host_echo_read_in(uc); /* keep draining any late echo. */
      break;
  }
}

/* =============================================================================
 * Public lifecycle / accessors / report.
 * =============================================================================
 */

void board_usb_init(bool trace)
{
  s_usb              = (usb_state_t){};
  s_trace            = trace;
  s_host_phase       = (uint8_t)k_phase_idle;
  s_host_step        = 0U;
  s_host_substate    = (uint8_t)k_sub_deliver;
  s_host_wait        = 0U;
  s_configured       = false;
  s_usb_irqs         = 0U;
  s_echo_out_len     = 0U;
  s_echo_out_sent    = 0U;
  s_echo_in_got      = 0U;
  s_log_n            = 0U;
  s_dcp_hold_len     = 0U;
  s_dcp_hold_pending = false;
  s_roles_swapped    = false;
  s_dev_irq_event    = (uint16_t)k_ra8_elc_event_usbfs_int;
  s_usb.dvsq         = (uint16_t)k_ra8_dvsq_powered;
}

void board_usb_set_irq_raiser(board_usb_irq_raiser_t raise)
{
  s_raise = raise;
}

bool board_usb_configured(void)
{
  return s_configured;
}

void board_usb_feed_bulk_in(const uint8_t* data, uint32_t len)
{
  if ((data == nullptr) || (len == 0U)) {
    return;
  }
  const uint32_t n = (len > (uint32_t)k_usb_echo_cap) ? (uint32_t)k_usb_echo_cap : len;
  (void)memcpy(s_echo_out, data, n);
  s_echo_out_len = n;
}

uint32_t board_usb_echo_received(void)
{
  return s_echo_in_got;
}

/** @brief Print the device-state name for the report line. */
static const char* usb_dvsq_name(uint16_t dvsq)
{
  switch (dvsq & (uint16_t)k_ra8_intsts0_mask_dvsq) {
    case (uint16_t)k_ra8_dvsq_powered:
      return "Powered";
    case (uint16_t)k_ra8_dvsq_default:
      return "Default";
    case (uint16_t)k_ra8_dvsq_address:
      return "Address";
    case (uint16_t)k_ra8_dvsq_configured:
      return "Configured";
    default:
      return "Suspended";
  }
}

const char* board_usb_state_string(void)
{
  if (s_configured) {
    switch ((usb_dev_class_t)s_dev_class) {
      case k_usb_class_hid:
        return "CONFIGURED (HID active)";
      case k_usb_class_msc:
        return "CONFIGURED (MSC active)";
      case k_usb_class_cdc:
        return "CONFIGURED (CDC-ACM active)";
      case k_usb_class_printer:
        return "CONFIGURED (Printer active)";
      case k_usb_class_vendor:
        return "CONFIGURED (Vendor active)";
      case k_usb_class_unknown:
      default:
        return "CONFIGURED";
    }
  }
  return usb_dvsq_name(s_usb.dvsq);
}

void board_usb_report(void)
{
  if ((s_usb_irqs == 0U) && (s_log_n == 0U)) {
    return; /* USB never came up in this run; stay silent. */
  }
  (void)fprintf(stderr,
                "  USB-FS        : %u IRQ(s), device state %s\n",
                s_usb_irqs,
                usb_dvsq_name(s_usb.dvsq));
  for (uint32_t i = 0U; i < s_log_n; i++) {
    (void)fprintf(stderr, "    usb: %s\n", s_log[i]);
  }
  if (s_configured) {
    (void)fprintf(stderr, "  USB: device CONFIGURED (%s)\n", usb_class_active_str());
  } else {
    (void)fprintf(stderr, "  USB: enumeration INCOMPLETE (device did not reach CONFIGURED)\n");
  }
  if (s_hid_reports > 0U) {
    (void)fprintf(stderr,
                  "  USB HID       : %u mouse report(s), cursor (%d,%d) buttons 0x%02X\n",
                  s_hid_reports,
                  (int)s_hid_cx,
                  (int)s_hid_cy,
                  (unsigned)s_hid_buttons);
  }
  if ((s_msc_blocks > 0U) || (s_msc_read_ok > 0U)) {
    (void)fprintf(
      stderr,
      "  USB MSC       : capacity %u blocks x %uB, INQUIRY %s, sector read %u byte(s)\n",
      s_msc_blocks,
      s_msc_block_len,
      s_msc_inquiry_ok ? "ok" : "--",
      s_msc_read_ok);
  }
  if (s_echo_out_len > 0U) {
    (void)fprintf(stderr,
                  "  USB CDC echo  : sent %u byte(s) OUT, read %u byte(s) IN\n",
                  s_echo_out_sent,
                  s_echo_in_got);
  }
  if (s_loop_latched) {
    (void)fprintf(stderr,
                  "  USB self-loop : fw host drove %u SETUP(s), %u bulk-OUT, %u bulk-IN pkt(s)\n",
                  s_loop_setups,
                  s_loop_bulk_out_pkts,
                  s_loop_bulk_in_pkts);
  }
}
