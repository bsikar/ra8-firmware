/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file board_usb_vhost.c
 * @brief Built-in virtual chapter-9 USB host (see board_usb_internal.h)
 *
 * @details
 * The device-class detection, the chapter-9 + CDC enumeration script, the
 * per-step SETUP state machine, the CDC bulk echo and the MSC BOT/SCSI
 * driver -- moved verbatim out of board_usb.c.
 *
 *
 * @since 0.1.0
 */

#include <stdio.h>
#include <string.h>

#include "board_usb_internal.h"
#include "ra8_elc_regs.h"
#include "ra8_usb_regs.h"

/* =============================================================================
 * Device-class awareness -- the host detects HID / MSC / CDC from the enumerated
 * interface descriptor and surfaces the class-specific traffic after CONFIGURED,
 * so usb_hid_device / usb_msc_device exercise their data path instead of being
 * mislabelled "CDC-ACM active".
 * =============================================================================
 */

uint8_t  s_dev_class;   /**< Detected ::usb_dev_class_t.          */
uint32_t s_hid_reports; /**< HID input reports the host has read. */
int32_t  s_hid_cx;      /**< Accumulated HID boot-mouse X.        */
int32_t  s_hid_cy;      /**< Accumulated HID boot-mouse Y.        */
uint8_t  s_hid_buttons; /**< Last HID button bitmap.              */

/** @brief Human-readable active-class suffix once the device is configured. */
const char* usb_class_active_str(void)
{
  switch ((usb_dev_class_t)s_dev_class) {
    case k_usb_class_hid:
      return "HID active";
    case k_usb_class_msc:
      return "MSC active";
    case k_usb_class_cdc:
      return "CDC-ACM active";
    case k_usb_class_printer:
      return "Printer active";
    case k_usb_class_vendor:
      return "Vendor active";
    case k_usb_class_unknown:
    default:
      return "configured";
  }
}

/**
 * @brief Detect the device class from a returned descriptor, if it is config.
 *
 * @details Walks the standard descriptor chain (each entry: bLength,
 * bDescriptorType, ...). The configuration descriptor carries the interface
 * descriptor(s); the first interface's bInterfaceClass (offset 5) names the
 * device class. Called for every control-IN response -- only the configuration
 * descriptor contains an interface descriptor, so the first hit wins.
 *
 * @param[in] d   Descriptor bytes the device returned.
 * @param[in] len Number of valid bytes in @p d.
 */
void usb_detect_class(const uint8_t* d, uint16_t len)
{
  if (s_dev_class != (uint8_t)k_usb_class_unknown) {
    return;
  }
  uint16_t i = 0U;
  while (((uint32_t)i + 2U) <= (uint32_t)len) {
    const uint8_t blen  = d[i];
    const uint8_t btype = d[i + 1U];
    if (blen == 0U) {
      break;
    }
    if ((btype == (uint8_t)k_usb_dt_interface) && (((uint32_t)i + 6U) <= (uint32_t)len)) {
      const uint8_t icls = d[i + k_iface_class_off];
      if (icls == (uint8_t)k_usb_iclass_hid) {
        s_dev_class = (uint8_t)k_usb_class_hid;
        return;
      }
      if (icls == (uint8_t)k_usb_iclass_msc) {
        s_dev_class = (uint8_t)k_usb_class_msc;
        return;
      }
      if ((icls == (uint8_t)k_usb_iclass_cdc_comm) || (icls == (uint8_t)k_usb_iclass_cdc_data)) {
        s_dev_class = (uint8_t)k_usb_class_cdc;
        return;
      }
      if (icls == (uint8_t)k_usb_iclass_printer) {
        s_dev_class = (uint8_t)k_usb_class_printer;
        return;
      }
      if (icls == (uint8_t)k_usb_iclass_vendor) {
        s_dev_class = (uint8_t)k_usb_class_vendor;
        return;
      }
    }
    i = (uint16_t)((uint32_t)i + (uint32_t)blen);
  }
}

/**
 * @brief Decode one HID boot-protocol mouse input report from the device.
 *
 * @details The report is { buttons, dx, dy } (USB HID 1.11 sec E.10): byte 0 is
 * the button bitmap, bytes 1-2 are signed X/Y deltas. The host accumulates the
 * deltas into a virtual cursor so the report shows real motion (the demo walks
 * the cursor in a square) -- exactly what a real host driver would render.
 *
 * @param[in] d   Report bytes.
 * @param[in] len Report length (>= 3 for a boot mouse).
 */
static void usb_hid_decode_report(const uint8_t* d, uint16_t len)
{
  if (len < 3U) {
    return;
  }
  s_hid_buttons = d[0];
  s_hid_cx += (int32_t)(int8_t)d[1];
  s_hid_cy += (int32_t)(int8_t)d[2];
  s_hid_reports++;
  if (s_trace) {
    (void)fprintf(stderr,
                  "  [usb] HID mouse report: btn=0x%02X dx=%+d dy=%+d -> cursor (%d,%d)\n",
                  (unsigned)s_hid_buttons,
                  (int)(int8_t)d[1],
                  (int)(int8_t)d[2],
                  (int)s_hid_cx,
                  (int)s_hid_cy);
  }
}

/* =============================================================================
 * Virtual USB host -- chapter-9 enumeration script + step machine.
 * =============================================================================
 */

/**
 * @brief The chapter-9 + CDC SETUP sequence the virtual host drives.
 *
 * @details Mirrors the request order a real host (macOS / Linux) issues for a
 * CDC-ACM device: probe the device descriptor, assign an address, re-read the
 * full device + configuration + string descriptors, select the configuration
 * (which fires USBX's CDC-ACM activate), then the two CDC line requests.
 */
static const usb_setup_step_t k_enum_script[] = {
  {k_usb_dir_device_to_host,
   k_usb_req_get_descriptor,
   (uint16_t)(k_usb_dt_device << 8),
   0U,
   (uint16_t)k_usb_desc8_len,
   "GET_DESCRIPTOR(device,8)"},
  {k_usb_dir_device_to_host,
   k_usb_req_get_descriptor,
   (uint16_t)(k_usb_dt_device << 8),
   0U,
   (uint16_t)k_usb_desc_dev_len,
   "GET_DESCRIPTOR(device,full)"},
  {k_usb_dir_host_to_device,
   k_usb_req_set_address,
   (uint16_t)k_usb_addr_assigned,
   0U,
   0U,
   "SET_ADDRESS(7)"},
  {k_usb_dir_device_to_host,
   k_usb_req_get_descriptor,
   (uint16_t)(k_usb_dt_device << 8),
   0U,
   (uint16_t)k_usb_desc_dev_len,
   "GET_DESCRIPTOR(device,full)"},
  {k_usb_dir_device_to_host,
   k_usb_req_get_descriptor,
   (uint16_t)(k_usb_dt_config << 8),
   0U,
   (uint16_t)k_usb_cfg_probe_len,
   "GET_DESCRIPTOR(config,9)"},
  {k_usb_dir_device_to_host,
   k_usb_req_get_descriptor,
   (uint16_t)(k_usb_dt_config << 8),
   0U,
   (uint16_t)k_usb_cfg_full_cap,
   "GET_DESCRIPTOR(config,full)"},
  {k_usb_dir_device_to_host,
   k_usb_req_get_descriptor,
   (uint16_t)(k_usb_dt_string << 8),
   0U,
   (uint16_t)k_usb_str_len,
   "GET_DESCRIPTOR(string,0)"},
  {k_usb_dir_host_to_device,
   k_usb_req_set_config,
   (uint16_t)k_usb_config_value,
   0U,
   0U,
   "SET_CONFIGURATION(1)"},
  {(uint8_t)k_usb_dir_host_to_device | (uint8_t)k_usb_req_class_iface,
   k_cdc_req_set_line_coding,
   0U,
   0U,
   k_cdc_line_coding_len,
   "SET_LINE_CODING"},
  {(uint8_t)k_usb_dir_host_to_device | (uint8_t)k_usb_req_class_iface,
   k_cdc_req_set_control_line_state,
   0x0003U,
   0U,
   0U,
   "SET_CONTROL_LINE_STATE"},
};

/** @brief True when SYSCFG.DPRPU is set: the device has attached its pull-up. */
bool host_device_attached(void)
{
  const uint16_t syscfg = s_usb.reg[usb_word((uint64_t)k_ra8_usb_off_syscfg)];
  return (syscfg & (uint16_t)(1U << k_ra8_syscfg_bit_dprpu)) != 0U;
}

/** @brief Latch a SETUP packet into USBREQ..USBLENG (host -> device). */
static void host_latch_setup(const usb_setup_step_t* s)
{
  s_usb.reg[usb_word((uint64_t)k_ra8_usb_off_usbreq)] =
    (uint16_t)(s->bm_request_type | (uint16_t)((uint16_t)s->b_request << k_usb_byte_bits));
  s_usb.reg[usb_word((uint64_t)k_ra8_usb_off_usbval)]  = s->w_value;
  s_usb.reg[usb_word((uint64_t)k_ra8_usb_off_usbindx)] = s->w_index;
  s_usb.reg[usb_word((uint64_t)k_ra8_usb_off_usbleng)] = s->w_length;
}

/** @brief CTSQ control-stage value the host advertises for a SETUP. */
static uint16_t host_setup_ctsq(const usb_setup_step_t* s)
{
  if ((s->bm_request_type & (uint8_t)k_usb_dir_device_to_host) != 0U) {
    return (uint16_t)k_ra8_ctsq_rdds; /* control read data stage. */
  }
  if (s->w_length != 0U) {
    return (uint16_t)k_ra8_ctsq_wrds; /* control write data stage. */
  }
  return (uint16_t)k_ra8_ctsq_wrnd; /* no-data control. */
}

/** @brief Deliver the current script step's SETUP and raise the CTRT IRQ. */
void host_deliver_setup(uc_engine* uc, const usb_setup_step_t* s)
{
  /* Clear any stale DCP state from the previous transfer. */
  s_usb.dcp_in.len    = 0U;
  s_usb.dcp_in.valid  = false;
  s_usb.dcp_out.len   = 0U;
  s_usb.dcp_out.rd    = 0U;
  s_usb.dcp_out.ready = false;
  /* For an OUT-data control transfer, stage placeholder bytes the device can
   * drain through the DCP if its class handler reads them. Skipped under the
   * external-host bridge: there the REAL host payload arrives via
   * board_usb_bridge_dcp_out(), and a placeholder would let the device drain
   * zeros before it lands. */
  if (!s_external_host && ((s->bm_request_type & (uint8_t)k_usb_dir_device_to_host) == 0U) &&
      (s->w_length != 0U)) {
    const uint16_t n =
      (s->w_length < (uint16_t)k_usb_dcp_mps) ? s->w_length : (uint16_t)k_usb_dcp_mps;
    s_usb.dcp_out.len   = n;
    s_usb.dcp_out.rd    = 0U;
    s_usb.dcp_out.ready = true;
    (void)memset(s_usb.dcp_out.data, 0, sizeof(s_usb.dcp_out.data));
  }
  host_latch_setup(s);
  s_usb.setup_valid = true;
  s_usb.ctsq        = host_setup_ctsq(s);
  usb_intsts0_set((uint8_t)k_ra8_int0_bit_ctrt);
  char line[k_usb_log_width];
  (void)snprintf(line, sizeof(line), "SETUP[%u] %s -> CTRT raised", (unsigned)s_host_step, s->name);
  usb_log_line(line);
  usb_raise_irq(uc);
}

/** @brief Drain the device's queued control-IN data as the host's read. */
static void host_drain_in(void)
{
  usb_log_count("control-IN: device returned", (unsigned)s_usb.dcp_in.len);
  usb_detect_class(s_usb.dcp_in.data, s_usb.dcp_in.len);
  s_usb.dcp_in.len   = 0U;
  s_usb.dcp_in.valid = false;
}

/** @brief DCPCTR.PID == BUF: the device has armed an IN response. */
bool host_dcp_pid_buf(void)
{
  const uint16_t dcpctr = s_usb.reg[usb_word((uint64_t)k_ra8_usb_off_dcpctr)];
  return (dcpctr & (uint16_t)k_ra8_pid_mask) == (uint16_t)k_ra8_pid_buf;
}

/** @brief Observe (and clear) DCPCTR.CCPL: the device ended a control transfer. */
bool host_take_ccpl(void)
{
  const uint32_t w    = usb_word((uint64_t)k_ra8_usb_off_dcpctr);
  const uint16_t ccpl = (uint16_t)(1U << k_ra8_dcpctr_bit_ccpl);
  const bool     seen = (s_usb.reg[w] & ccpl) != 0U;
  if (seen) {
    s_usb.reg[w] = (uint16_t)(s_usb.reg[w] & (uint16_t)~ccpl); /* SIE clears CCPL. */
  }
  return seen;
}

/** @brief Apply the SIE-owned side effects of a no-data control request. */
void host_apply_no_data(uc_engine* uc, const usb_setup_step_t* s)
{
  if (s->b_request == (uint8_t)k_usb_req_set_address) {
    /* The SIE latches USBADDR and owns the IN-ZLP status stage itself; mirror
     * that and advance the device state to Address (HUM Ch 36.3). */
    s_usb.reg[usb_word((uint64_t)k_ra8_usb_off_usbaddr)] =
      (uint16_t)(s->w_value & (uint16_t)k_ra8_usbaddr_addr_mask);
    s_usb.dvsq = (uint16_t)k_ra8_dvsq_address;
    usb_intsts0_set((uint8_t)k_ra8_int0_bit_dvst);
    char line[k_usb_log_width];
    (void)
      snprintf(line, sizeof(line), "SET_ADDRESS: DVSQ -> Address (addr=%u)", (unsigned)s->w_value);
    usb_log_line(line);
    usb_raise_irq(uc);
  }
}

/** @brief Sub-state k_sub_deliver: latch + raise CTRT, then pick the next sub. */
static void host_step_deliver(uc_engine* uc, const usb_setup_step_t* s)
{
  host_deliver_setup(uc, s);
  s_host_wait = 0U;
  if ((s->bm_request_type & (uint8_t)k_usb_dir_device_to_host) != 0U) {
    s_host_substate = (uint8_t)k_sub_wait_in; /* control read: wait for data. */
  } else if (s->b_request == (uint8_t)k_usb_req_set_address) {
    host_apply_no_data(uc, s);
    s_host_substate = (uint8_t)k_sub_next; /* SIE-handled; no device CCPL. */
  } else {
    s_host_substate = (uint8_t)k_sub_wait_ack; /* no/OUT-data: wait for CCPL. */
  }
}

/** @brief Sub-state k_sub_wait_in: await the device's control-IN response. */
static void host_step_wait_in(void)
{
  if (s_usb.dcp_in.valid && host_dcp_pid_buf()) {
    host_drain_in();
    s_host_substate = (uint8_t)k_sub_status;
    s_host_wait     = 0U;
    return;
  }
  s_host_wait++;
  if (s_host_wait > (uint32_t)k_usb_step_timeout) {
    usb_log_count("STALLED waiting for control-IN data at step", (unsigned)s_host_step);
    s_host_substate = (uint8_t)k_sub_status; /* push on; report the stall. */
    s_host_wait     = 0U;
  }
}

/** @brief Sub-state k_sub_status: deliver the control-read status stage. */
static void host_step_status(uc_engine* uc)
{
  s_usb.ctsq        = (uint16_t)k_ra8_ctsq_rdss; /* read status stage. */
  s_usb.setup_valid = false;
  usb_intsts0_set((uint8_t)k_ra8_int0_bit_ctrt);
  usb_raise_irq(uc);
  s_host_substate = (uint8_t)k_sub_wait_ack;
  s_host_wait     = 0U;
}

/** @brief Sub-state k_sub_wait_ack: await the device's CCPL (transfer end). */
static void host_step_wait_ack(void)
{
  if (host_take_ccpl()) {
    s_host_substate = (uint8_t)k_sub_next;
    s_host_wait     = 0U;
    return;
  }
  s_host_wait++;
  if (s_host_wait > (uint32_t)k_usb_step_timeout) {
    s_host_substate = (uint8_t)k_sub_next; /* device may auto-ack; move on. */
    s_host_wait     = 0U;
  }
}

/** @brief Number of steps in the enumeration script. */
static uint32_t host_script_len(void)
{
  return (uint32_t)(sizeof(k_enum_script) / sizeof(k_enum_script[0]));
}

/** @brief Mark the device CONFIGURED: advance DVSQ and raise DVST. */
void host_mark_configured(uc_engine* uc)
{
  s_usb.dvsq = (uint16_t)k_ra8_dvsq_configured;
  usb_intsts0_set((uint8_t)k_ra8_int0_bit_dvst);
  usb_raise_irq(uc);
  if (!s_configured) {
    usb_log_line("SET_CONFIGURATION done: DVSQ -> Configured");
  }
}

/** @brief Sub-state k_sub_next: record any post-step effect, advance the step. */
static void host_step_next(uc_engine* uc, const usb_setup_step_t* s)
{
  if (s->b_request == (uint8_t)k_usb_req_set_config) {
    host_mark_configured(uc);
  }
  s_host_step++;
  s_host_substate = (uint8_t)k_sub_deliver;
  s_host_wait     = 0U;
  if (s_host_step >= host_script_len()) {
    s_host_phase = (uint8_t)k_phase_configured;
    s_host_wait  = 0U;
  }
}

/** @brief Run one micro-step of the active enumeration script entry. */
void host_run_setup_phase(uc_engine* uc)
{
  if (s_host_step >= host_script_len()) {
    s_host_phase = (uint8_t)k_phase_configured;
    return;
  }
  const usb_setup_step_t* s = &k_enum_script[s_host_step];
  switch ((usb_host_sub_t)s_host_substate) {
    case k_sub_deliver:
      host_step_deliver(uc, s);
      break;
    case k_sub_wait_in:
      host_step_wait_in();
      break;
    case k_sub_status:
      host_step_status(uc);
      break;
    case k_sub_wait_ack:
      host_step_wait_ack();
      break;
    case k_sub_next:
    default:
      host_step_next(uc, s);
      break;
  }
}

/** @brief Phase k_phase_idle: wait for the device pull-up, then bus-reset. */
void host_run_idle_phase(uc_engine* uc)
{
  if (!host_device_attached()) {
    return;
  }
  usb_log_line("device pull-up detected (SYSCFG.DPRPU) -> bus reset");
  s_usb.dvsq = (uint16_t)k_ra8_dvsq_default;
  usb_intsts0_set((uint8_t)k_ra8_int0_bit_dvst);
  usb_raise_irq(uc);
  s_host_phase = (uint8_t)k_phase_reset;
  s_host_wait  = 0U;
}

/** @brief Phase k_phase_reset: hold reset a few ticks so the DCP re-arms. */
void host_run_reset_phase(uc_engine* uc)
{
  /* Keep DVSQ at Default and re-raise DVST so the bridge's busreset_rearm runs
   * (it re-defaults DCPCFG/DCPMAXP/PIPECTR/INTENB0 -- HUM Ch 36.3). */
  s_usb.dvsq = (uint16_t)k_ra8_dvsq_default;
  if (s_host_wait == 0U) {
    usb_intsts0_set((uint8_t)k_ra8_int0_bit_dvst);
    usb_raise_irq(uc);
  }
  s_host_wait++;
  if (s_host_wait >= (uint32_t)k_usb_reset_settle) {
    s_host_phase    = (uint8_t)k_phase_setup;
    s_host_step     = 0U;
    s_host_substate = (uint8_t)k_sub_deliver;
    s_host_wait     = 0U;
  }
}

/** @brief Deliver one bulk-OUT packet to the CDC data OUT pipe and signal BRDY. */
static void host_echo_send_out(uc_engine* uc)
{
  const uint32_t remaining = s_echo_out_len - s_echo_out_sent;
  const uint32_t chunk     = (remaining < (uint32_t)k_usb_pipe_mps) ? remaining : k_usb_pipe_mps;
  usb_out_buf_t* b         = &s_usb.pipe_out[k_usb_bulk_out_pipe];
  (void)memcpy(b->data, &s_echo_out[s_echo_out_sent], chunk);
  b->len   = (uint16_t)chunk;
  b->rd    = 0U;
  b->ready = true;
  /* BRDYSTS bit for the OUT pipe: an OUT packet has landed (HUM Ch 36.2.12). */
  const uint32_t w = usb_word((uint64_t)k_ra8_usb_off_brdysts);
  s_usb.reg[w]     = (uint16_t)(s_usb.reg[w] | (uint16_t)(1U << k_usb_bulk_out_pipe));
  usb_intsts0_set((uint8_t)k_ra8_int0_bit_brdy);
  s_echo_out_sent += chunk;
  usb_log_count("bulk OUT: delivered to data pipe", (unsigned)chunk);
  usb_raise_irq(uc);
}

/**
 * @brief Drain bytes the device queued on the IN pipe; ack the IN transfer.
 *
 * @details After consuming the buffer the host raises BEMP for the pipe -- the
 * "transmit buffer empty" interrupt the RA dcd uses to complete an IN transfer.
 * Without it the device-side USBX blocks after one packet, so a HID device that
 * streams reports (the boot mouse) freezes after the first; with it the reports
 * keep flowing and the cursor walks its square.
 *
 * @param[in,out] uc Unicorn engine (to pend the USB interrupt).
 */
void host_echo_read_in(uc_engine* uc)
{
  usb_in_buf_t* b = &s_usb.pipe_in[k_usb_bulk_in_pipe];
  if (b->valid && (b->len > 0U)) {
    if (s_dev_class == (uint8_t)k_usb_class_hid) {
      usb_hid_decode_report(b->data, b->len); /* interrupt-IN mouse report. */
    } else {
      s_echo_in_got += b->len;
      usb_log_count("bulk IN: read echoed bytes from data pipe", (unsigned)b->len);
    }
    b->len   = 0U;
    b->valid = false;
    /* Acknowledge the IN transfer (BEMP) so the device can queue the next. */
    const uint32_t w = usb_word((uint64_t)k_ra8_usb_off_bempsts);
    s_usb.reg[w]     = (uint16_t)(s_usb.reg[w] | (uint16_t)(1U << k_usb_bulk_in_pipe));
    usb_intsts0_set((uint8_t)k_ra8_int0_bit_bemp);
    usb_raise_irq(uc);
  } else if (b->valid) {
    b->valid = false; /* drained ZLP terminator. */
  }
}

/* =============================================================================
 * MSC Bulk-Only Transport host -- drives SCSI against the device's RAM disk so
 * usb_msc_device actually serves storage (CBW -> data -> CSW per command).
 * =============================================================================
 */

/** @brief MSC BOT per-command phase. */
typedef enum : uint8_t {
  k_msc_send = 0U, /**< Push the next command's CBW.    */
  k_msc_data = 1U, /**< Read the data-phase bytes.      */
  k_msc_csw  = 2U, /**< Read the 13-byte CSW.           */
  k_msc_done = 3U, /**< All scripted commands finished. */
} msc_phase_t;

/** @brief Index of each command in the scripted SCSI sequence. */
typedef enum : uint8_t {
  k_msc_cmd_inquiry       = 0U, /**< INQUIRY: standard data, 36-byte allocation. */
  k_msc_cmd_read_capacity = 1U, /**< READ CAPACITY (10): last LBA + block size.  */
  k_msc_cmd_read10        = 2U, /**< READ (10): LBA 0, one block.                */
} msc_script_cmd_t;

/** @brief BOT / SCSI sizing the host uses (USB Mass Storage BBB 1.0). */
typedef enum : uint32_t {
  k_msc_cbw_len   = 31U,   /**< Command Block Wrapper length.  */
  k_msc_csw_len   = 13U,   /**< Command Status Wrapper length. */
  k_msc_flag_in   = 0x80U, /**< bmCBWFlags: device-to-host.    */
  k_msc_settle    = 4U,    /**< Ticks to wait for a phase.     */
  k_msc_cmd_count = 3U,    /**< Scripted commands (below).     */
} msc_const_t;

static uint8_t  s_msc_phase;      /**< ::msc_phase_t for the active command.  */
static uint8_t  s_msc_cmd;        /**< Index into the SCSI command script.    */
static uint32_t s_msc_tag;        /**< Running dCBWTag.                       */
static uint32_t s_msc_data_len;   /**< Expected data-phase length.            */
static uint32_t s_msc_data_got;   /**< Data-phase bytes read so far.          */
static uint32_t s_msc_wait;       /**< Phase pacing.                          */
uint32_t        s_msc_blocks;     /**< Capacity in blocks (READ CAPACITY).    */
uint32_t        s_msc_block_len;  /**< Block size in bytes.                   */
uint32_t        s_msc_read_ok;    /**< Sector-read data-phase bytes captured. */
bool            s_msc_inquiry_ok; /**< INQUIRY data phase completed.          */

/**
 * @brief Build the SCSI CDB for the scripted command @p cmd.
 *
 * @param[in]  cmd      Script index (0=INQUIRY, 1=READ CAPACITY(10), 2=READ(10)).
 * @param[out] cdb      16-byte command block to fill (pre-zeroed).
 * @param[out] cdb_len  Length of the CDB.
 * @param[out] data_len Expected data-phase byte count.
 */
static void host_msc_build_cdb(uint8_t cmd, uint8_t* cdb, uint8_t* cdb_len, uint32_t* data_len)
{
  for (uint32_t i = 0U; i < 16U; i++) {
    cdb[i] = 0U;
  }
  switch ((msc_script_cmd_t)cmd) {
    case k_msc_cmd_inquiry: /* INQUIRY: standard data, 36-byte allocation. */
      cdb[0]    = (uint8_t)k_scsi_inquiry;
      cdb[4]    = (uint8_t)k_inquiry_len;
      *cdb_len  = 6U;
      *data_len = (uint32_t)k_inquiry_len;
      break;
    case k_msc_cmd_read_capacity: /* READ CAPACITY (10): last LBA + block size. */
      cdb[0]    = (uint8_t)k_scsi_read_capacity;
      *cdb_len  = (uint8_t)k_cdb10_len;
      *data_len = 8U;
      break;
    case k_msc_cmd_read10: /* READ (10): LBA 0, one block (512 bytes). */
    default:
      cdb[0]    = (uint8_t)k_scsi_read10;
      cdb[8]    = 1U; /* transfer length = 1 block (big-endian low byte). */
      *cdb_len  = (uint8_t)k_cdb10_len;
      *data_len = (uint32_t)k_sector_bytes;
      break;
  }
}

/** @brief Push the current command's CBW onto the bulk-OUT pipe (raise BRDY). */
static void host_msc_send_cbw(uc_engine* uc)
{
  uint8_t  cdb[16];
  uint8_t  cdb_len  = 0U;
  uint32_t data_len = 0U;
  host_msc_build_cdb(s_msc_cmd, cdb, &cdb_len, &data_len);
  s_msc_data_len = data_len;
  s_msc_data_got = 0U;
  s_msc_tag++;

  usb_out_buf_t* b = &s_usb.pipe_out[k_usb_bulk_out_pipe];
  uint8_t*       d = b->data;
  d[0]             = (uint8_t)'U'; /* dCBWSignature 'USBC' (LE 0x43425355). */
  d[1]             = (uint8_t)'S';
  d[2]             = (uint8_t)'B';
  d[3]             = (uint8_t)'C';
  /* dCBWTag / dCBWDataTransferLength are little-endian u32 (USB MSC BOT 5.1). */
  d[k_cbw_tag_off + k_le_lane_b0] = (uint8_t)(s_msc_tag & (uint32_t)k_usb_byte_mask);
  d[k_cbw_tag_off + k_le_lane_b1] = (uint8_t)((s_msc_tag >> 8) & (uint32_t)k_usb_byte_mask);
  d[k_cbw_tag_off + k_le_lane_b2] = (uint8_t)((s_msc_tag >> 16) & (uint32_t)k_usb_byte_mask);
  d[k_cbw_tag_off + k_le_lane_b3] =
    (uint8_t)((s_msc_tag >> (uint32_t)k_usb_shift24) & (uint32_t)k_usb_byte_mask);
  d[k_cbw_dtl_off + k_le_lane_b0] = (uint8_t)(data_len & (uint32_t)k_usb_byte_mask);
  d[k_cbw_dtl_off + k_le_lane_b1] = (uint8_t)((data_len >> 8) & (uint32_t)k_usb_byte_mask);
  d[k_cbw_dtl_off + k_le_lane_b2] = (uint8_t)((data_len >> 16) & (uint32_t)k_usb_byte_mask);
  d[k_cbw_dtl_off + k_le_lane_b3] =
    (uint8_t)((data_len >> (uint32_t)k_usb_shift24) & (uint32_t)k_usb_byte_mask);
  d[k_cbw_flags_off]  = (uint8_t)k_msc_flag_in; /* all scripted commands read. */
  d[k_cbw_lun_off]    = 0U;                     /* LUN 0.                      */
  d[k_cbw_cdblen_off] = cdb_len;
  for (uint32_t i = 0U; i < 16U; i++) {
    d[k_cbw_cdb_off + i] = cdb[i];
  }
  b->len           = (uint16_t)k_msc_cbw_len;
  b->rd            = 0U;
  b->ready         = true;
  const uint32_t w = usb_word((uint64_t)k_ra8_usb_off_brdysts);
  s_usb.reg[w]     = (uint16_t)(s_usb.reg[w] | (uint16_t)(1U << k_usb_bulk_out_pipe));
  usb_intsts0_set((uint8_t)k_ra8_int0_bit_brdy);
  usb_raise_irq(uc);
}

/** @brief Consume one IN buffer (data or CSW) and acknowledge it with BEMP. */
static uint16_t host_msc_take_in(uc_engine* uc, uint8_t* out, uint16_t cap)
{
  usb_in_buf_t* b = &s_usb.pipe_in[k_usb_bulk_in_pipe];
  if (!b->valid) {
    return 0U;
  }
  const uint16_t n = (b->len < cap) ? b->len : cap;
  for (uint16_t i = 0U; i < n; i++) {
    out[i] = b->data[i];
  }
  b->len           = 0U;
  b->valid         = false;
  const uint32_t w = usb_word((uint64_t)k_ra8_usb_off_bempsts);
  s_usb.reg[w]     = (uint16_t)(s_usb.reg[w] | (uint16_t)(1U << k_usb_bulk_in_pipe));
  usb_intsts0_set((uint8_t)k_ra8_int0_bit_bemp);
  usb_raise_irq(uc);
  return n;
}

/** @brief Parse a READ CAPACITY (10) response into block count + size. */
static void host_msc_parse_capacity(const uint8_t* d, uint16_t n)
{
  if (n < 8U) {
    return;
  }
  /* Both parameter-block fields are big-endian u32 (SCSI READ CAPACITY(10)). */
  const uint32_t last_lba =
    ((uint32_t)d[k_cap10_last_lba_off + k_be_lane_b3] << (uint32_t)k_usb_shift24) |
    ((uint32_t)d[k_cap10_last_lba_off + k_be_lane_b2] << 16) |
    ((uint32_t)d[k_cap10_last_lba_off + k_be_lane_b1] << 8) |
    (uint32_t)d[k_cap10_last_lba_off + k_be_lane_b0];
  s_msc_block_len = ((uint32_t)d[k_cap10_blocklen_off + k_be_lane_b3] << (uint32_t)k_usb_shift24) |
                    ((uint32_t)d[k_cap10_blocklen_off + k_be_lane_b2] << 16) |
                    ((uint32_t)d[k_cap10_blocklen_off + k_be_lane_b1] << 8) |
                    (uint32_t)d[k_cap10_blocklen_off + k_be_lane_b0];
  s_msc_blocks    = last_lba + 1U;
}

/** @brief Phase ::k_msc_send: push the next CBW, or finish the script. */
static void host_msc_phase_send(uc_engine* uc)
{
  if (s_msc_cmd >= (uint8_t)k_msc_cmd_count) {
    s_msc_phase = (uint8_t)k_msc_done;
    return;
  }
  host_msc_send_cbw(uc);
  s_msc_phase = (uint8_t)k_msc_data;
  s_msc_wait  = 0U;
}

/**
 * @brief Record one data-phase burst against the command that is in flight.
 *
 * @details
 * Each scripted command captures its data phase differently: READ CAPACITY is
 * parsed into the geometry globals, INQUIRY only needs to be seen at all, and
 * READ (10) accumulates its byte count. Anything else is counted but not
 * interpreted.
 *
 * @param[in] d Data-phase bytes just taken off the bulk-IN pipe.
 * @param[in] n Number of valid bytes in @p d, always non-zero.
 * @pre @p d is non-null.
 * @pre @p n is greater than zero (the caller checked).
 * @post The globals for the in-flight command reflect this burst.
 * @post @p d is unmodified.
 * @note Not thread-safe; mutates file-scope MSC state.
 */
static void host_msc_record_data(const uint8_t* d, uint16_t n)
{
  switch ((msc_script_cmd_t)s_msc_cmd) {
    case k_msc_cmd_read_capacity:
      host_msc_parse_capacity(d, n);
      break;
    case k_msc_cmd_inquiry:
      s_msc_inquiry_ok = true;
      break;
    case k_msc_cmd_read10:
      s_msc_read_ok += n;
      break;
    default:
      break;
  }
}

/** @brief Phase ::k_msc_data: drain the data phase until it is done or stalls. */
static void host_msc_phase_data(uc_engine* uc)
{
  uint8_t        buf[k_usb_in_cap];
  const uint16_t n = host_msc_take_in(uc, buf, (uint16_t)sizeof(buf));
  if (n == 0U) {
    s_msc_wait++;
  } else {
    host_msc_record_data(buf, n);
    s_msc_data_got += n;
    s_msc_wait = 0U;
  }
  if ((s_msc_data_got >= s_msc_data_len) || (s_msc_wait > (uint32_t)k_usb_step_timeout)) {
    s_msc_phase = (uint8_t)k_msc_csw;
    s_msc_wait  = 0U;
  }
}

/** @brief Phase ::k_msc_csw: take the CSW, then advance to the next command. */
static void host_msc_phase_csw(uc_engine* uc)
{
  uint8_t        buf[k_usb_in_cap];
  const uint16_t n = host_msc_take_in(uc, buf, (uint16_t)sizeof(buf));
  if ((n >= (uint16_t)k_msc_csw_len) || (s_msc_wait > (uint32_t)k_usb_step_timeout)) {
    s_msc_cmd++;
    s_msc_phase = (uint8_t)k_msc_send;
    s_msc_wait  = 0U;
  } else {
    s_msc_wait++;
  }
}

/** @brief Drive the MSC BOT state machine one tick while CONFIGURED. */
static void host_msc_drive(uc_engine* uc)
{
  switch ((msc_phase_t)s_msc_phase) {
    case k_msc_send:
      host_msc_phase_send(uc);
      break;
    case k_msc_data:
      host_msc_phase_data(uc);
      break;
    case k_msc_csw:
      host_msc_phase_csw(uc);
      break;
    case k_msc_done:
    default:
      break;
  }
}

/** @brief Phase k_phase_configured: optionally drive the CDC bulk echo. */
void host_run_configured_phase(uc_engine* uc)
{
  if (s_dev_class == (uint8_t)k_usb_class_msc) {
    host_msc_drive(uc); /* run the BOT/SCSI script against the RAM disk. */
    return;
  }
  host_echo_read_in(uc);
  if (s_dev_class == (uint8_t)k_usb_class_hid) {
    return; /* keep polling the HID interrupt-IN pipe; reports keep flowing. */
  }
  if (s_echo_out_len == 0U) {
    s_host_phase = (uint8_t)k_phase_done;
    return;
  }
  /* Pace one OUT packet, then let the device echo it back before the next. */
  if (s_echo_out_sent < s_echo_out_len) {
    if (s_host_wait == 0U) {
      host_echo_send_out(uc);
    }
    s_host_wait++;
    if (s_host_wait >= (uint32_t)k_usb_reset_settle) {
      s_host_wait = 0U;
    }
    return;
  }
  /* All OUT bytes sent; drain the tail echo for a settle window, then finish. */
  s_host_wait++;
  if (s_host_wait >= (uint32_t)k_usb_post_cfg_idle) {
    s_host_phase = (uint8_t)k_phase_done;
  }
}
