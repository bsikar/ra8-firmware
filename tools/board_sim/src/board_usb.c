/**
 * @file board_usb.c
 * @brief USBFS controller model + virtual USB host (chapter-9 enumeration)
 *
 * @details
 * Implements the model declared in board_usb.h. The RA8D2 USBFS controller
 * (base 0x40250000, HUM Ch 36) is modelled register-by-register with real
 * staging buffers for the control (DCP) and bulk pipe FIFOs, and a virtual USB
 * host walks the standard chapter-9 enumeration against the real device-side
 * firmware (port/usbx/ux_dcd_ra_usb -> ra_usb*.c). The host delivers each SETUP
 * by latching USBREQ..USBLENG and raising the controller's CTRT interrupt
 * through board_periph's ICU -> NVIC path, then drains the descriptor bytes the
 * device pushes into the CFIFO and advances the device state machine
 * (DVSQ powered -> default -> address -> configured) until USBX activates its
 * CDC-ACM class.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "board_usb.h"

#include <stdio.h>
#include <string.h>

#include "board_console.h"
#include "ra8d2_elc_regs.h"
#include "ra8d2_usb_regs.h"

/** @brief USB class-request, SCSI command, and CBW-layout constants. */
typedef enum : uint32_t {
  k_iface_class_off     = 5U,    /**< bInterfaceClass offset in the iface descriptor. */
  k_usb_req_class_iface = 0x21U, /**< bmRequestType: class, interface, host->device.  */
  k_cdc_line_coding_len = 7U,    /**< SET_LINE_CODING wLength.                        */
  k_scsi_inquiry        = 0x12U, /**< SCSI INQUIRY opcode.                            */
  k_inquiry_len         = 36U,   /**< INQUIRY allocation length.                      */
  k_scsi_read_capacity  = 0x25U, /**< SCSI READ CAPACITY(10) opcode.                  */
  k_scsi_read10         = 0x28U, /**< SCSI READ(10) opcode.                           */
  k_cdb10_len           = 10U,   /**< 10-byte CDB length.                             */
  k_sector_bytes        = 512U,  /**< Logical block size.                             */
  k_usb_shift24         = 24U,   /**< Byte-3 position in a 32-bit word.               */
  k_cbw_lun_off         = 13U,   /**< CBW bCBWLUN offset.                             */
  k_cbw_cdblen_off      = 14U,   /**< CBW bCBWCBLength offset.                        */
  k_cbw_cdb_off         = 15U,   /**< CBW CBWCB (command block) offset.               */
} usb_lit_t;

/* =============================================================================
 * Window geometry + model sizing.
 * =============================================================================
 */

/** @brief USBFS register-window geometry and the staging-buffer caps. */
typedef enum : uint64_t {
  k_usb_base       = 0x40250000UL, /**< USBFS base (HUM Ch 36, p 1965).       */
  k_usb_span       = 0x100UL,      /**< Modelled window (PIPECTR end < 0x90). */
  k_usb_reg_words  = 0x100UL / 2U, /**< 16-bit register shadow word count.    */
  k_usb_dcp_mps    = 64UL,         /**< EP0 default control-pipe max packet.  */
  k_usb_pipe_mps   = 64UL,         /**< Bulk-FS data pipe max packet.         */
  k_usb_pipe_count = 10UL,         /**< DCP (0) + PIPE1..PIPE9.               */
  k_usb_echo_cap   = 256UL,        /**< Bulk echo staging capacity.           */
  k_usb_in_cap     = 512UL,        /**< DCP/bulk IN staging cap (multi-MPS).  */
} usb_geom_t;

/* =============================================================================
 * Standard USB chapter-9 request constants (USB 2.0 sec 9.3 / 9.4).
 * =============================================================================
 */

/** @brief bRequest values the virtual host issues (USB 2.0 sec 9.4). */
typedef enum : uint8_t {
  k_usb_req_get_descriptor = 0x06U, /**< GET_DESCRIPTOR.    */
  k_usb_req_set_address    = 0x05U, /**< SET_ADDRESS.       */
  k_usb_req_set_config     = 0x09U, /**< SET_CONFIGURATION. */
} usb_breq_t;

/** @brief CDC class bRequest values issued after configuration (CDC 1.20). */
typedef enum : uint8_t {
  k_cdc_req_set_line_coding        = 0x20U, /**< SET_LINE_CODING (OUT data). */
  k_cdc_req_set_control_line_state = 0x22U, /**< SET_CONTROL_LINE_STATE.     */
} cdc_breq_t;

/** @brief bDescriptorType values used in GET_DESCRIPTOR wValue high byte. */
typedef enum : uint8_t {
  k_usb_dt_device = 0x01U, /**< DEVICE descriptor.        */
  k_usb_dt_config = 0x02U, /**< CONFIGURATION descriptor. */
  k_usb_dt_string = 0x03U, /**< STRING descriptor.        */
} usb_dt_t;

/** @brief bmRequestType direction bit (USB 2.0 sec 9.3 Table 9-2). */
typedef enum : uint8_t {
  k_usb_dir_device_to_host = 0x80U, /**< IN: device -> host (control read).   */
  k_usb_dir_host_to_device = 0x00U, /**< OUT: host -> device (control write). */
} usb_dir_t;

/** @brief DVSTCTR0.RHST reset-handshake field values (HUM Ch 36.2.5 "DVSTCTR0", p 1971). */
typedef enum : uint16_t {
  k_usb_rhst_fs = 0x0002U, /**< Link settled at full speed after the bus reset. */
} usb_rhst_t;

/** @brief Misc small constants used across the model (avoid bare literals). */
typedef enum : uint32_t {
  k_usb_byte_bits      = 8U,    /**< Bits per byte. */
  k_usb_byte_mask      = 0xFFU, /**< One-byte mask. */
  k_usb_cfifo_h        = 2U,    /**< CFIFOH alias offset from CFIFO (+0x2).
                                    HS-instance 16-bit residual port.      */
  k_usb_cfifo_hh       = 3U,    /**< CFIFOHH alias offset from CFIFO (+0x3).
                                    HS-instance 8-bit residual port.       */
  k_usb_addr_assigned  = 7U,    /**< Address the host assigns the device.       */
  k_usb_config_value   = 1U,    /**< bConfigurationValue the host sets.         */
  k_usb_desc8_len      = 8U,    /**< First GET_DESCRIPTOR(device) length.       */
  k_usb_desc_dev_len   = 18U,   /**< Full device-descriptor length.             */
  k_usb_cfg_probe_len  = 9U,    /**< Config-descriptor header probe len.        */
  k_usb_cfg_full_cap   = 255U,  /**< Config-descriptor full request cap.        */
  k_usb_str_len        = 255U,  /**< String-descriptor request length.          */
  k_usb_step_timeout   = 64U,   /**< Host per-step wait budget (ticks).         */
  k_usb_reset_settle   = 4U,    /**< Ticks held in bus reset before SETUP.      */
  k_usb_post_cfg_idle  = 8U,    /**< Settle ticks after CONFIGURED.             */
  k_usb_log_cap        = 48U,   /**< Enumeration-step log capacity.             */
  k_usb_log_width      = 96U,   /**< Bytes per enumeration-step log line.       */
  k_usb_trace_dump_max = 24U,   /**< Control-IN payload bytes shown in --trace. */
  k_usb_bulk_in_pipe   = 1U,    /**< CDC bulk IN pipe (EP1 IN -> pipe 1).       */
  k_usb_bulk_out_pipe  = 2U,    /**< CDC bulk OUT pipe (EP2 OUT -> pipe 2).     */
  k_usb_dcp_pipe_bit   = 1U,    /**< DCP (pipe 0) BRDYSTS/BRDYENB bit mask.     */
} usb_const_t;

/* =============================================================================
 * Controller register state + FIFO staging.
 * =============================================================================
 */

/**
 * @brief One pipe's host-visible OUT staging buffer (host -> device payload).
 *
 * @details For the DCP (pipe 0) this carries control-write data (e.g.
 * SET_LINE_CODING); for a bulk pipe it carries the host's bulk-OUT bytes. The
 * device drains it through CFIFO and the model reports the remaining count in
 * CFIFOCTR.DTLN.
 */
typedef struct {
  uint8_t  data[k_usb_pipe_mps]; /**< Staged bytes the device will read. */
  uint16_t len;                  /**< Total staged length.               */
  uint16_t rd;                   /**< Read cursor as the device drains.  */
  bool     ready;                /**< Data present (BRDY-equivalent).    */
} usb_out_buf_t;

/**
 * @brief One pipe's device-visible IN staging buffer (device -> host payload).
 *
 * @details The device fills this through CFIFO (descriptor response on the DCP,
 * echoed bytes on a bulk IN pipe) and pulses CFIFOCTR.BVAL; the host then
 * drains it.
 */
typedef struct {
  uint8_t  data[k_usb_in_cap]; /**< Bytes the device queued for the host. */
  uint16_t len;                /**< Filled length (may span several MPS). */
  bool     valid;              /**< BVAL pulsed: ready for the host.      */
} usb_in_buf_t;

/**
 * @brief Aggregate USBFS controller + virtual-host model state.
 */
typedef struct {
  uint16_t      reg[k_usb_reg_words];       /**< 16-bit reflect-on-read shadow.       */
  uint16_t      ctsq;                       /**< INTSTS0.CTSQ control-stage value.    */
  uint16_t      dvsq;                       /**< INTSTS0.DVSQ device-state value.     */
  bool          setup_valid;                /**< INTSTS0.VALID: a SETUP is latched.   */
  usb_in_buf_t  dcp_in;                     /**< DCP IN buffer (descriptor response). */
  usb_out_buf_t dcp_out;                    /**< DCP OUT buffer (control-write data). */
  usb_in_buf_t  pipe_in[k_usb_pipe_count];  /**< Bulk IN per pipe.                    */
  usb_out_buf_t pipe_out[k_usb_pipe_count]; /**< Bulk OUT per pipe.                   */
} usb_state_t;

static usb_state_t            s_usb;
static bool                   s_trace;
static board_usb_irq_raiser_t s_raise;

/* Set true when a real-firmware USB host (the emulated USBHS controller in
 * board_periph_usbhs_host.c) drives THIS device model through the bridge below;
 * the built-in virtual chapter-9 host then stands down (board_usb_tick returns
 * early) so exactly one host drives the device -- the chip-internal self-loop. */
static bool s_external_host;

/* Self-loop role polarity (see the role-routing section of board_usb.h).
 * false: USBFS window = this device model, USBHS window = host model (Config
 * A). true: swapped (Config B) -- the firmware declared the FS controller the
 * host (SYSCFG.DCFM=1 written to the FS window) or the HS controller the
 * device (SYSCFG.DPRPU=1 written to the HS window), so the FS window routes to
 * the host model and the HS window routes here. Sticky until board_usb_init. */
static bool s_roles_swapped;

/* ICU event the device model pends for its interrupts: USBFS_INT while the
 * device model sits behind the USBFS window (Config A), USBHS_USB_INT_RESUME
 * once the roles swap and the DCD drives the USBHS controller (Config B). */
static uint16_t s_dev_irq_event = (uint16_t)k_ra_elc_event_usbfs_int;

/* DCP control-OUT "wire" holding buffer (bridge path). The polled host delivers
 * a control-write data stage (e.g. a DFU_DNLOAD block) before the device -- off
 * its own IRQ on the shared core -- has armed its DCP to receive it, and the
 * device's arm-time CFIFO BCLR would discard data landed too early. So the
 * bytes are held here as "in flight on the wire" and handed to the device DCP
 * only once it has armed (PID=BUF + DCP BRDY enabled), exactly as the SIE would
 * deliver an OUT packet the device is finally ready to ACK. */
static uint8_t  s_dcp_hold[k_usb_in_cap]; /**< Held control-OUT wire bytes.     */
static uint16_t s_dcp_hold_len;           /**< Held byte count.                 */
static bool     s_dcp_hold_pending;       /**< Held bytes await the device arm. */

/* Virtual-host bookkeeping. */
static uint8_t  s_host_phase;    /**< Current host state-machine phase.   */
static uint8_t  s_host_step;     /**< Index into the enumeration script.  */
static uint8_t  s_host_substate; /**< Sub-state within one SETUP step.    */
static uint32_t s_host_wait;     /**< Ticks spent waiting in a sub-state. */
static bool     s_configured;    /**< Device reached CONFIGURED.          */
static uint32_t s_usb_irqs;      /**< USB interrupts the host raised.     */

/* Bulk-echo (secondary CDC check) bookkeeping. */
static uint8_t  s_echo_out[k_usb_echo_cap]; /**< Host bulk-OUT payload.     */
static uint32_t s_echo_out_len;             /**< Bytes queued by --usb-in.  */
static uint32_t s_echo_out_sent;            /**< Bytes delivered to device. */
static uint32_t s_echo_in_got;              /**< Echoed bytes read back.    */

/* Enumeration-step log (host SETUP -> device stage), shown in the report. */
static char     s_log[k_usb_log_cap][k_usb_log_width];
static uint32_t s_log_n;

/* =============================================================================
 * Small register / logging helpers.
 * =============================================================================
 */

/** @brief Word index into the 16-bit register shadow for a window offset. */
static uint32_t usb_word(uint64_t off)
{
  return (uint32_t)((off & ~(uint64_t)1U) / 2U);
}

/** @brief Append one already-formatted line to the enumeration-step log. */
static void usb_log_line(const char* msg)
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
static void usb_log_count(const char* label, unsigned n)
{
  char line[k_usb_log_width];
  (void)snprintf(line, sizeof(line), "%s: %u byte(s)", label, n);
  usb_log_line(line);
}

/* =============================================================================
 * Device-class awareness -- the host detects HID / MSC / CDC from the enumerated
 * interface descriptor and surfaces the class-specific traffic after CONFIGURED,
 * so usb_hid_device / usb_msc_device exercise their data path instead of being
 * mislabelled "CDC-ACM active".
 * =============================================================================
 */

/** @brief USB device class the host detected from the interface descriptor. */
typedef enum : uint8_t {
  k_usb_class_unknown = 0U, /**< Not yet detected.            */
  k_usb_class_cdc     = 1U, /**< CDC-ACM (virtual serial).    */
  k_usb_class_hid     = 2U, /**< HID (boot mouse / keyboard). */
  k_usb_class_msc     = 3U, /**< Mass storage (BOT/SCSI).     */
} usb_dev_class_t;

/** @brief bInterfaceClass codes + the INTERFACE descriptor type (USB 2.0). */
typedef enum : uint8_t {
  k_usb_iclass_cdc_comm = 0x02U, /**< Communications (CDC control). */
  k_usb_iclass_hid      = 0x03U, /**< Human Interface Device.       */
  k_usb_iclass_msc      = 0x08U, /**< Mass Storage.                 */
  k_usb_iclass_cdc_data = 0x0AU, /**< CDC data.                     */
  k_usb_dt_interface    = 0x04U, /**< INTERFACE descriptor type.    */
} usb_iclass_t;

static uint8_t  s_dev_class;   /**< Detected ::usb_dev_class_t.          */
static uint32_t s_hid_reports; /**< HID input reports the host has read. */
static int32_t  s_hid_cx;      /**< Accumulated HID boot-mouse X.        */
static int32_t  s_hid_cy;      /**< Accumulated HID boot-mouse Y.        */
static uint8_t  s_hid_buttons; /**< Last HID button bitmap.              */

/** @brief Human-readable active-class suffix once the device is configured. */
static const char* usb_class_active_str(void)
{
  switch ((usb_dev_class_t)s_dev_class) {
    case k_usb_class_hid:
      return "HID active";
    case k_usb_class_msc:
      return "MSC active";
    case k_usb_class_cdc:
      return "CDC-ACM active";
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
static void usb_detect_class(const uint8_t* d, uint16_t len)
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

/** @brief INTSTS0 value the device reads: event bits OR computed fields. */
static uint16_t usb_intsts0(void)
{
  uint16_t v = s_usb.reg[usb_word((uint64_t)k_ra_usb_off_intsts0)];
  v &= (uint16_t)((1U << k_ra_int0_bit_brdy) | (1U << k_ra_int0_bit_nrdy) |
                  (1U << k_ra_int0_bit_bemp) | (1U << k_ra_int0_bit_ctrt) |
                  (1U << k_ra_int0_bit_dvst) | (1U << k_ra_int0_bit_vbse));
  v = (uint16_t)(v | (s_usb.ctsq & (uint16_t)k_ra_intsts0_mask_ctsq));
  v = (uint16_t)(v | (s_usb.dvsq & (uint16_t)k_ra_intsts0_mask_dvsq));
  v = (uint16_t)(v | (uint16_t)k_ra_intsts0_mask_vbsts); /* VBUS always present. */
  if (s_usb.setup_valid) {
    v = (uint16_t)(v | (uint16_t)k_ra_intsts0_mask_valid);
  }
  return v;
}

/** @brief Set an INTSTS0 event bit in the shadow (host asserts it). */
static void usb_intsts0_set(uint8_t bit)
{
  const uint32_t w = usb_word((uint64_t)k_ra_usb_off_intsts0);
  s_usb.reg[w]     = (uint16_t)(s_usb.reg[w] | (uint16_t)(1U << bit));
}

/** @brief Raise the device controller interrupt (USBFS_INT, or USBHS after a role swap). */
static void usb_raise_irq(uc_engine* uc)
{
  if (s_raise != nullptr) {
    s_raise(uc, s_dev_irq_event);
    s_usb_irqs++;
  }
}

/* =============================================================================
 * CFIFO routing -- map CFIFOSEL (CURPIPE + ISEL) to the active staging buffer.
 * =============================================================================
 */

/** @brief Currently-selected CFIFO pipe number (CFIFOSEL.CURPIPE[3:0]). */
static uint8_t cfifo_pipe(void)
{
  const uint16_t sel = s_usb.reg[usb_word((uint64_t)k_ra_usb_off_cfifosel)];
  return (uint8_t)(sel & (uint16_t)k_ra_fifosel_curpipe);
}

/** @brief True when CFIFOSEL selects the IN direction (device writes / fills). */
static bool cfifo_is_in(void)
{
  const uint16_t sel = s_usb.reg[usb_word((uint64_t)k_ra_usb_off_cfifosel)];
  return (sel & (uint16_t)k_ra_fifosel_isel) != 0U;
}

/** @brief The device-IN staging buffer CFIFOSEL currently points at. */
static usb_in_buf_t* cfifo_in_buf(void)
{
  const uint8_t p = cfifo_pipe();
  return (p == 0U) ? &s_usb.dcp_in : &s_usb.pipe_in[p % k_usb_pipe_count];
}

/** @brief The device-OUT staging buffer CFIFOSEL currently points at. */
static usb_out_buf_t* cfifo_out_buf(void)
{
  const uint8_t p = cfifo_pipe();
  return (p == 0U) ? &s_usb.dcp_out : &s_usb.pipe_out[p % k_usb_pipe_count];
}

/** @brief Bytes still available for the device to read on the selected OUT buf. */
static uint16_t cfifo_dtln(void)
{
  if (cfifo_is_in()) {
    return 0U;
  }
  const usb_out_buf_t* b = cfifo_out_buf();
  return (uint16_t)(b->len - b->rd);
}

/** @brief Service a CFIFO data-port read: drain one unit (1..4 bytes) from the OUT buffer. */
static uint32_t cfifo_read_port(unsigned size)
{
  usb_out_buf_t* b = cfifo_out_buf();
  uint32_t       v = 0U;
  for (unsigned i = 0U; (i < size) && (b->rd < b->len); i++) {
    v |= (uint32_t)((uint32_t)b->data[b->rd] << (i * k_usb_byte_bits));
    b->rd++;
  }
  return v;
}

/** @brief Service a CFIFO data-port write: append one unit (1..4 bytes) to the IN buffer. */
static void cfifo_write_port(uint32_t value, unsigned size)
{
  usb_in_buf_t* b = cfifo_in_buf();
  for (unsigned i = 0U; (i < size) && (b->len < (uint16_t)sizeof(b->data)); i++) {
    b->data[b->len] = (uint8_t)((value >> (i * k_usb_byte_bits)) & k_usb_byte_mask);
    b->len++;
  }
}

/** @brief Apply a CFIFOCTR write: BCLR clears the buffer, BVAL commits an IN. */
static void cfifoctr_write(uint16_t value)
{
  if ((value & (uint16_t)k_ra_fifoctr_bclr) != 0U) {
    if (cfifo_is_in()) {
      usb_in_buf_t* b = cfifo_in_buf();
      b->len          = 0U;
      b->valid        = false;
    } else {
      usb_out_buf_t* b = cfifo_out_buf();
      b->rd            = b->len; /* drop the remainder of the OUT buffer. */
    }
  }
  if ((value & (uint16_t)k_ra_fifoctr_bval) != 0U) {
    if (cfifo_is_in()) {
      cfifo_in_buf()->valid = true; /* IN buffer committed; ready for the host. */
    }
  }
}

/* =============================================================================
 * Register read / write dispatch (USBFS window at 0x40250000).
 * =============================================================================
 */

/** @brief CFIFOCTR read value: FRDY always ready, DTLN = OUT bytes available. */
static uint16_t cfifoctr_read(void)
{
  return (uint16_t)((uint16_t)k_ra_fifoctr_frdy | (cfifo_dtln() & (uint16_t)k_ra_fifoctr_dtln));
}

/** @brief Read one device-controller register; @p off is the byte offset into the window. */
static uint32_t usb_reg_read(uint64_t off, unsigned size)
{
  switch ((uint16_t)off) {
    case (uint16_t)k_ra_usb_off_intsts0:
      return usb_intsts0();
    case (uint16_t)k_ra_usb_off_cfifoctr:
      return cfifoctr_read();
    case (uint16_t)k_ra_usb_off_cfifo:
    case (uint16_t)((uint16_t)k_ra_usb_off_cfifo + (uint16_t)k_usb_cfifo_h):  /* CFIFOH tail.  */
    case (uint16_t)((uint16_t)k_ra_usb_off_cfifo + (uint16_t)k_usb_cfifo_hh): /* CFIFOHH tail. */
      /* Full access width: the HS instance drains its CFIFO 32 bits at a time
       * (MBW=32) with 16/8-bit residual reads through the CFIFOH / CFIFOHH
       * aliases (HUM Ch 37.2.7 "CFIFO, DnFIFO : FIFO Port Register" p 2069-2070); the FS
       * instance uses 16-bit accesses at the base port. */
      return cfifo_read_port(size);
    case (uint16_t)k_ra_usb_off_syssts0:
      return (uint16_t)0x0003U; /* LNST = J-state: device pull-up seen. */
    case (uint16_t)k_ra_usb_off_dvstctr0: {
      /* HUM Ch 36.2.5 "DVSTCTR0 : Device State Control Register 0", p 1971:
       * RHST[2:0] reports the settled link speed once the host's bus reset
       * completes; the device firmware (internal_dvst_track_speed in the DCD)
       * re-aims the USBX framework at it. The modelled loop is a full-speed
       * link (the FS controller's ceiling caps it in either polarity), so
       * report FS from the moment the device has left Powered. */
      uint16_t v = s_usb.reg[usb_word(off)];
      if (s_usb.dvsq != (uint16_t)k_ra_dvsq_powered) {
        v = (uint16_t)(v | (uint16_t)k_usb_rhst_fs);
      }
      return v;
    }
    case (uint16_t)k_ra_usb_off_frmnum:
      return s_usb.reg[usb_word((uint64_t)k_ra_usb_off_frmnum)];
    default:
      return s_usb.reg[usb_word(off)];
  }
}

/** @brief Write one device-controller register; @p off is the byte offset into the window. */
static void usb_reg_write(uint64_t off, uint32_t value, unsigned size)
{
  const uint16_t v16 = (uint16_t)value;
  switch ((uint16_t)off) {
    case (uint16_t)k_ra_usb_off_cfifo:
    case (uint16_t)((uint16_t)k_ra_usb_off_cfifo + (uint16_t)k_usb_cfifo_h):  /* CFIFOH tail.  */
    case (uint16_t)((uint16_t)k_ra_usb_off_cfifo + (uint16_t)k_usb_cfifo_hh): /* CFIFOHH tail. */
      /* Full access width: the HS instance fills its CFIFO 32 bits at a time
       * (MBW=32) with 16/8-bit residual writes through the CFIFOH / CFIFOHH
       * aliases (HUM Ch 37.2.7 "CFIFO, DnFIFO : FIFO Port Register" p 2069-2070); truncating to
       * 16 bits here zeroed bytes 2-3 of every descriptor word in Config B. */
      cfifo_write_port(value, size);
      return;
    case (uint16_t)k_ra_usb_off_cfifoctr:
      cfifoctr_write(v16);
      return;
    case (uint16_t)k_ra_usb_off_intsts0:
      /* W0C on the event bits: a written 0 clears, a 1 preserves. */
      s_usb.reg[usb_word(off)] &= v16;
      if ((v16 & (uint16_t)k_ra_intsts0_mask_valid) == 0U) {
        s_usb.setup_valid = false;
      }
      return;
    default:
      s_usb.reg[usb_word(off)] = v16;
      return;
  }
}

uint64_t board_usb_read(uc_engine* uc, uint64_t addr, unsigned size, bool* handled)
{
  (void)uc;
  if ((addr < (uint64_t)k_usb_base) || (addr >= ((uint64_t)k_usb_base + (uint64_t)k_usb_span))) {
    *handled = false;
    return 0U;
  }
  *handled = true;
  if (s_roles_swapped) {
    /* Config B: the polled ra_usb_host_* driver owns the USBFS controller. */
    return board_usbhs_host_reg_read(uc, addr - (uint64_t)k_usb_base, size);
  }
  return (uint64_t)usb_reg_read(addr - (uint64_t)k_usb_base, size);
}

void board_usb_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value, bool* handled)
{
  if ((addr < (uint64_t)k_usb_base) || (addr >= ((uint64_t)k_usb_base + (uint64_t)k_usb_span))) {
    *handled = false;
    return;
  }
  *handled           = true;
  const uint64_t off = addr - (uint64_t)k_usb_base;
  /* Role declaration (HUM Ch 36.2.1 SYSCFG.DCFM, p 1966): the firmware writes
   * DCFM=1 to select controller (host) function on THIS instance. Under the
   * self-loop bridge that pins the FS window to the host model -- Config B. */
  if (s_external_host && !s_roles_swapped && (off == (uint64_t)k_ra_usb_off_syscfg) &&
      (((uint16_t)value & (uint16_t)(1U << k_ra_syscfg_bit_dcfm)) != 0U)) {
    board_usb_roles_swap(uc);
  }
  if (s_roles_swapped) {
    board_usbhs_host_reg_write(uc, off, size, value);
    return;
  }
  usb_reg_write(off, (uint32_t)value, size);
}

/* =============================================================================
 * Virtual USB host -- chapter-9 enumeration script + step machine.
 * =============================================================================
 */

/** @brief One SETUP step in the host enumeration script (USB 2.0 sec 9.4). */
typedef struct {
  uint8_t     bm_request_type; /**< Direction + type + recipient.        */
  uint8_t     b_request;       /**< bRequest.                            */
  uint16_t    w_value;         /**< wValue.                              */
  uint16_t    w_index;         /**< wIndex.                              */
  uint16_t    w_length;        /**< wLength (host's expected data size). */
  const char* name;            /**< Human label for the step log.        */
} usb_setup_step_t;

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
  {k_usb_dir_host_to_device | k_usb_req_class_iface,
   k_cdc_req_set_line_coding,
   0U,
   0U,
   k_cdc_line_coding_len,
   "SET_LINE_CODING"},
  {k_usb_dir_host_to_device | k_usb_req_class_iface,
   k_cdc_req_set_control_line_state,
   0x0003U,
   0U,
   0U,
   "SET_CONTROL_LINE_STATE"},
};

/** @brief Host state-machine phases. */
typedef enum : uint8_t {
  k_phase_idle       = 0U, /**< Waiting for the device pull-up (DPRPU).    */
  k_phase_reset      = 1U, /**< Holding bus reset; device re-arms its DCP. */
  k_phase_setup      = 2U, /**< Walking the enumeration script.            */
  k_phase_configured = 3U, /**< Device configured; optional bulk echo.     */
  k_phase_done       = 4U, /**< Terminal idle.                             */
} usb_host_phase_t;

/** @brief Per-SETUP-step sub-states. */
typedef enum : uint8_t {
  k_sub_deliver  = 0U, /**< Latch the SETUP + raise CTRT.                 */
  k_sub_wait_in  = 1U, /**< Wait for the device's IN data (control read). */
  k_sub_status   = 2U, /**< Deliver the status stage + raise CTRT.        */
  k_sub_wait_ack = 3U, /**< Wait for the device's CCPL (transfer end).    */
  k_sub_next     = 4U, /**< Advance to the next script step.              */
} usb_host_sub_t;

/** @brief True when SYSCFG.DPRPU is set: the device has attached its pull-up. */
static bool host_device_attached(void)
{
  const uint16_t syscfg = s_usb.reg[usb_word((uint64_t)k_ra_usb_off_syscfg)];
  return (syscfg & (uint16_t)(1U << k_ra_syscfg_bit_dprpu)) != 0U;
}

/** @brief Latch a SETUP packet into USBREQ..USBLENG (host -> device). */
static void host_latch_setup(const usb_setup_step_t* s)
{
  s_usb.reg[usb_word((uint64_t)k_ra_usb_off_usbreq)] =
    (uint16_t)(s->bm_request_type | (uint16_t)((uint16_t)s->b_request << k_usb_byte_bits));
  s_usb.reg[usb_word((uint64_t)k_ra_usb_off_usbval)]  = s->w_value;
  s_usb.reg[usb_word((uint64_t)k_ra_usb_off_usbindx)] = s->w_index;
  s_usb.reg[usb_word((uint64_t)k_ra_usb_off_usbleng)] = s->w_length;
}

/** @brief CTSQ control-stage value the host advertises for a SETUP. */
static uint16_t host_setup_ctsq(const usb_setup_step_t* s)
{
  if ((s->bm_request_type & (uint8_t)k_usb_dir_device_to_host) != 0U) {
    return (uint16_t)k_ra_ctsq_rdds; /* control read data stage. */
  }
  if (s->w_length != 0U) {
    return (uint16_t)k_ra_ctsq_wrds; /* control write data stage. */
  }
  return (uint16_t)k_ra_ctsq_wrnd; /* no-data control. */
}

/** @brief Deliver the current script step's SETUP and raise the CTRT IRQ. */
static void host_deliver_setup(uc_engine* uc, const usb_setup_step_t* s)
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
  usb_intsts0_set((uint8_t)k_ra_int0_bit_ctrt);
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
static bool host_dcp_pid_buf(void)
{
  const uint16_t dcpctr = s_usb.reg[usb_word((uint64_t)k_ra_usb_off_dcpctr)];
  return (dcpctr & (uint16_t)k_ra_pid_mask) == (uint16_t)k_ra_pid_buf;
}

/** @brief Observe (and clear) DCPCTR.CCPL: the device ended a control transfer. */
static bool host_take_ccpl(void)
{
  const uint32_t w    = usb_word((uint64_t)k_ra_usb_off_dcpctr);
  const uint16_t ccpl = (uint16_t)(1U << k_ra_dcpctr_bit_ccpl);
  const bool     seen = (s_usb.reg[w] & ccpl) != 0U;
  if (seen) {
    s_usb.reg[w] = (uint16_t)(s_usb.reg[w] & (uint16_t)~ccpl); /* SIE clears CCPL. */
  }
  return seen;
}

/** @brief Apply the SIE-owned side effects of a no-data control request. */
static void host_apply_no_data(uc_engine* uc, const usb_setup_step_t* s)
{
  if (s->b_request == (uint8_t)k_usb_req_set_address) {
    /* The SIE latches USBADDR and owns the IN-ZLP status stage itself; mirror
     * that and advance the device state to Address (HUM Ch 36.3). */
    s_usb.reg[usb_word((uint64_t)k_ra_usb_off_usbaddr)] =
      (uint16_t)(s->w_value & (uint16_t)k_ra_usbaddr_addr_mask);
    s_usb.dvsq = (uint16_t)k_ra_dvsq_address;
    usb_intsts0_set((uint8_t)k_ra_int0_bit_dvst);
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
  s_usb.ctsq        = (uint16_t)k_ra_ctsq_rdss; /* read status stage. */
  s_usb.setup_valid = false;
  usb_intsts0_set((uint8_t)k_ra_int0_bit_ctrt);
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
static void host_mark_configured(uc_engine* uc)
{
  s_usb.dvsq = (uint16_t)k_ra_dvsq_configured;
  usb_intsts0_set((uint8_t)k_ra_int0_bit_dvst);
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
static void host_run_setup_phase(uc_engine* uc)
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
static void host_run_idle_phase(uc_engine* uc)
{
  if (!host_device_attached()) {
    return;
  }
  usb_log_line("device pull-up detected (SYSCFG.DPRPU) -> bus reset");
  s_usb.dvsq = (uint16_t)k_ra_dvsq_default;
  usb_intsts0_set((uint8_t)k_ra_int0_bit_dvst);
  usb_raise_irq(uc);
  s_host_phase = (uint8_t)k_phase_reset;
  s_host_wait  = 0U;
}

/** @brief Phase k_phase_reset: hold reset a few ticks so the DCP re-arms. */
static void host_run_reset_phase(uc_engine* uc)
{
  /* Keep DVSQ at Default and re-raise DVST so the bridge's busreset_rearm runs
   * (it re-defaults DCPCFG/DCPMAXP/PIPECTR/INTENB0 -- HUM Ch 36.3). */
  s_usb.dvsq = (uint16_t)k_ra_dvsq_default;
  if (s_host_wait == 0U) {
    usb_intsts0_set((uint8_t)k_ra_int0_bit_dvst);
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
  const uint32_t w = usb_word((uint64_t)k_ra_usb_off_brdysts);
  s_usb.reg[w]     = (uint16_t)(s_usb.reg[w] | (uint16_t)(1U << k_usb_bulk_out_pipe));
  usb_intsts0_set((uint8_t)k_ra_int0_bit_brdy);
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
static void host_echo_read_in(uc_engine* uc)
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
    const uint32_t w = usb_word((uint64_t)k_ra_usb_off_bempsts);
    s_usb.reg[w]     = (uint16_t)(s_usb.reg[w] | (uint16_t)(1U << k_usb_bulk_in_pipe));
    usb_intsts0_set((uint8_t)k_ra_int0_bit_bemp);
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
static uint32_t s_msc_blocks;     /**< Capacity in blocks (READ CAPACITY).    */
static uint32_t s_msc_block_len;  /**< Block size in bytes.                   */
static uint32_t s_msc_read_ok;    /**< Sector-read data-phase bytes captured. */
static bool     s_msc_inquiry_ok; /**< INQUIRY data phase completed.          */

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
  switch (cmd) {
    case 0U: /* INQUIRY: standard data, 36-byte allocation. */
      cdb[0]    = (uint8_t)k_scsi_inquiry;
      cdb[4]    = (uint8_t)k_inquiry_len;
      *cdb_len  = 6U;
      *data_len = (uint32_t)k_inquiry_len;
      break;
    case 1U: /* READ CAPACITY (10): 8-byte response (last LBA + block size). */
      cdb[0]    = (uint8_t)k_scsi_read_capacity;
      *cdb_len  = (uint8_t)k_cdb10_len;
      *data_len = 8U;
      break;
    case 2U: /* READ (10): LBA 0, one block (512 bytes). */
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
  d[4]             = (uint8_t)(s_msc_tag & (uint32_t)k_usb_byte_mask);
  d[5]             = (uint8_t)((s_msc_tag >> 8) & (uint32_t)k_usb_byte_mask);
  d[6]             = (uint8_t)((s_msc_tag >> 16) & (uint32_t)k_usb_byte_mask);
  d[7]             = (uint8_t)((s_msc_tag >> (uint32_t)k_usb_shift24) & (uint32_t)k_usb_byte_mask);
  d[8]             = (uint8_t)(data_len & (uint32_t)k_usb_byte_mask);
  d[9]             = (uint8_t)((data_len >> 8) & (uint32_t)k_usb_byte_mask);
  d[10]            = (uint8_t)((data_len >> 16) & (uint32_t)k_usb_byte_mask);
  d[11]            = (uint8_t)((data_len >> (uint32_t)k_usb_shift24) & (uint32_t)k_usb_byte_mask);
  d[12]            = (uint8_t)k_msc_flag_in; /* all scripted commands read. */
  d[k_cbw_lun_off] = 0U;                     /* LUN 0.                      */
  d[k_cbw_cdblen_off] = cdb_len;
  for (uint32_t i = 0U; i < 16U; i++) {
    d[k_cbw_cdb_off + i] = cdb[i];
  }
  b->len           = (uint16_t)k_msc_cbw_len;
  b->rd            = 0U;
  b->ready         = true;
  const uint32_t w = usb_word((uint64_t)k_ra_usb_off_brdysts);
  s_usb.reg[w]     = (uint16_t)(s_usb.reg[w] | (uint16_t)(1U << k_usb_bulk_out_pipe));
  usb_intsts0_set((uint8_t)k_ra_int0_bit_brdy);
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
  const uint32_t w = usb_word((uint64_t)k_ra_usb_off_bempsts);
  s_usb.reg[w]     = (uint16_t)(s_usb.reg[w] | (uint16_t)(1U << k_usb_bulk_in_pipe));
  usb_intsts0_set((uint8_t)k_ra_int0_bit_bemp);
  usb_raise_irq(uc);
  return n;
}

/** @brief Parse a READ CAPACITY (10) response into block count + size. */
static void host_msc_parse_capacity(const uint8_t* d, uint16_t n)
{
  if (n < 8U) {
    return;
  }
  const uint32_t last_lba = ((uint32_t)d[0] << (uint32_t)k_usb_shift24) | ((uint32_t)d[1] << 16) |
                            ((uint32_t)d[2] << 8) | (uint32_t)d[3];
  s_msc_block_len         = ((uint32_t)d[4] << (uint32_t)k_usb_shift24) | ((uint32_t)d[5] << 16) |
                            ((uint32_t)d[6] << 8) | (uint32_t)d[7];
  s_msc_blocks            = last_lba + 1U;
}

/** @brief Drive the MSC BOT state machine one tick while CONFIGURED. */
static void host_msc_drive(uc_engine* uc)
{
  uint8_t buf[k_usb_in_cap];
  switch ((msc_phase_t)s_msc_phase) {
    case k_msc_send:
      if (s_msc_cmd >= (uint8_t)k_msc_cmd_count) {
        s_msc_phase = (uint8_t)k_msc_done;
        return;
      }
      host_msc_send_cbw(uc);
      s_msc_phase = (uint8_t)k_msc_data;
      s_msc_wait  = 0U;
      break;
    case k_msc_data: {
      const uint16_t n = host_msc_take_in(uc, buf, (uint16_t)sizeof(buf));
      if (n > 0U) {
        if (s_msc_cmd == 1U) {
          host_msc_parse_capacity(buf, n);
        } else if (s_msc_cmd == 0U) {
          s_msc_inquiry_ok = true;
        } else if (s_msc_cmd == 2U) {
          s_msc_read_ok += n;
        }
        s_msc_data_got += n;
        s_msc_wait = 0U;
      } else {
        s_msc_wait++;
      }
      if ((s_msc_data_got >= s_msc_data_len) || (s_msc_wait > (uint32_t)k_usb_step_timeout)) {
        s_msc_phase = (uint8_t)k_msc_csw;
        s_msc_wait  = 0U;
      }
      break;
    }
    case k_msc_csw: {
      const uint16_t n = host_msc_take_in(uc, buf, (uint16_t)sizeof(buf));
      if ((n >= (uint16_t)k_msc_csw_len) || (s_msc_wait > (uint32_t)k_usb_step_timeout)) {
        s_msc_cmd++;
        s_msc_phase = (uint8_t)k_msc_send;
        s_msc_wait  = 0U;
      } else {
        s_msc_wait++;
      }
      break;
    }
    case k_msc_done:
    default:
      break;
  }
}

/** @brief Phase k_phase_configured: optionally drive the CDC bulk echo. */
static void host_run_configured_phase(uc_engine* uc)
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

/* =============================================================================
 * Chip-internal USBHS-host <-> USBFS-device bridge (see board_usb.h).
 *
 * The emulated USBHS host controller (board_periph_usbhs_host.c) calls these to
 * drive THIS device model from the real firmware's host-side register accesses,
 * reusing the same device-poking primitives the virtual host uses. This closes
 * the RA8D2's on-chip HS-host -> FS-device loop entirely in-process.
 * =============================================================================
 */

void board_usb_set_external_host(bool present)
{
  s_external_host = present;
}

bool board_usb_roles_swapped(void)
{
  return s_roles_swapped;
}

void board_usb_roles_swap(uc_engine* uc)
{
  (void)uc;
  if (s_roles_swapped) {
    return; /* one-shot: both triggers (FS DCFM=1, HS DPRPU=1) fire in Config B. */
  }
  /* The device firmware's PHY + module-init writes landed in the HS window's
   * shadow while its role was still unknowable (the HS host bring-up drives the
   * very same device-polarity PHY sequence first). Adopt that accumulated state
   * as the device model's register file -- the models swap windows, the
   * firmware-visible register contents must not change -- and reset the host
   * model for its fresh life behind the FS window (which is still virgin: the
   * FS host driver's first window write is the SYSCFG.DCFM=1 that triggers
   * this swap, and the HS DPRPU=1 trigger fires before any FS host activity). */
  (void)board_usbhs_host_shadow_handoff(s_usb.reg, (uint32_t)k_usb_reg_words);
  s_roles_swapped = true;
  s_dev_irq_event = (uint16_t)k_ra_elc_event_usbhs_int_resume;
  usb_log_line("role swap: FS window = host model, HS window = device model (Config B)");
}

uint64_t board_usb_dev_reg_read(uc_engine* uc, uint64_t off, unsigned size)
{
  (void)uc;
  return (uint64_t)usb_reg_read(off, size);
}

void board_usb_dev_reg_write(uc_engine* uc, uint64_t off, unsigned size, uint64_t value)
{
  (void)uc;
  usb_reg_write(off, (uint32_t)value, size);
}

bool board_usb_dev_attached(void)
{
  return host_device_attached();
}

void board_usb_bridge_bus_reset(uc_engine* uc)
{
  s_usb.dvsq = (uint16_t)k_ra_dvsq_default;
  usb_intsts0_set((uint8_t)k_ra_int0_bit_dvst);
  usb_log_line("bridge: HS host bus reset -> device Default");
  usb_raise_irq(uc);
}

void board_usb_bridge_deliver_setup(uc_engine* uc, const uint8_t* setup)
{
  if (setup == nullptr) {
    return;
  }
  const usb_setup_step_t s = {
    .bm_request_type = setup[0],
    .b_request       = setup[1],
    .w_value         = (uint16_t)((uint16_t)setup[2] | (uint16_t)((uint16_t)setup[3] << 8)),
    .w_index         = (uint16_t)((uint16_t)setup[4] | (uint16_t)((uint16_t)setup[5] << 8)),
    .w_length        = (uint16_t)((uint16_t)setup[6] | (uint16_t)((uint16_t)setup[7] << 8)),
    .name            = "bridge SETUP",
  };
  host_deliver_setup(uc, &s);
  /* SET_ADDRESS is latched by the SIE on hardware; mirror that so the device
   * advances to the Address state (host_apply_no_data self-guards on bRequest). */
  host_apply_no_data(uc, &s);
}

bool board_usb_bridge_dcp_in_ready(void)
{
  return s_usb.dcp_in.valid && host_dcp_pid_buf();
}

uint16_t board_usb_bridge_dcp_in_take(uint8_t* buf, uint16_t cap)
{
  if (buf == nullptr) {
    return 0U;
  }
  uint16_t n = s_usb.dcp_in.len;
  if (n > cap) {
    n = cap;
  }
  (void)memcpy(buf, s_usb.dcp_in.data, n);
  usb_detect_class(s_usb.dcp_in.data, s_usb.dcp_in.len);
  usb_log_count("bridge control-IN: device returned", (unsigned)s_usb.dcp_in.len);
  if (s_trace) {
    /* Payload head, --trace only: enough to identify a descriptor / status. */
    char           line[k_usb_log_width];
    int            w = 0;
    const uint16_t limit =
      (n < (uint16_t)k_usb_trace_dump_max) ? n : (uint16_t)k_usb_trace_dump_max;
    for (uint16_t i = 0U; (i < limit) && (w >= 0) && ((size_t)w < sizeof(line)); i++) {
      w += snprintf(&line[w], sizeof(line) - (size_t)w, "%02X ", (unsigned)s_usb.dcp_in.data[i]);
    }
    (void)fprintf(stderr, "  [usb] bridge control-IN bytes: %s\n", line);
  }
  s_usb.dcp_in.len   = 0U;
  s_usb.dcp_in.valid = false;
  return n;
}

void board_usb_bridge_ctrl_status(uc_engine* uc)
{
  s_usb.ctsq        = (uint16_t)k_ra_ctsq_rdss;
  s_usb.setup_valid = false;
  usb_intsts0_set((uint8_t)k_ra_int0_bit_ctrt);
  usb_raise_irq(uc);
}

bool board_usb_bridge_dev_took_ccpl(void)
{
  return host_take_ccpl();
}

void board_usb_bridge_mark_configured(uc_engine* uc)
{
  host_mark_configured(uc);
}

void board_usb_bridge_bulk_out(uc_engine* uc, uint8_t dev_pipe, const uint8_t* data, uint16_t len)
{
  if (data == nullptr) {
    return;
  }
  usb_out_buf_t* b = &s_usb.pipe_out[dev_pipe % k_usb_pipe_count];
  uint16_t       n = len;
  if (n > (uint16_t)sizeof(b->data)) {
    n = (uint16_t)sizeof(b->data);
  }
  (void)memcpy(b->data, data, n);
  b->len           = n;
  b->rd            = 0U;
  b->ready         = true;
  const uint32_t w = usb_word((uint64_t)k_ra_usb_off_brdysts);
  s_usb.reg[w]     = (uint16_t)(s_usb.reg[w] | (uint16_t)(1U << dev_pipe));
  usb_intsts0_set((uint8_t)k_ra_int0_bit_brdy);
  usb_raise_irq(uc);
}

bool board_usb_bridge_bulk_out_consumed(uint8_t dev_pipe)
{
  const usb_out_buf_t* b = &s_usb.pipe_out[dev_pipe % k_usb_pipe_count];
  /* The device firmware drains the staged OUT packet through its CFIFO, which
   * advances @c rd to @c len. Until then the packet is still in flight, so the
   * host must not push the next one (it would overwrite this bank -- the exact
   * failure that wedged a multi-packet WRITE(10) data stage). */
  return b->ready && (b->rd >= b->len);
}

bool board_usb_bridge_bulk_in_ready(uint8_t dev_pipe)
{
  const usb_in_buf_t* b = &s_usb.pipe_in[dev_pipe % k_usb_pipe_count];
  return b->valid && (b->len > 0U);
}

uint16_t board_usb_bridge_bulk_in_take(uc_engine* uc, uint8_t dev_pipe, uint8_t* buf, uint16_t cap)
{
  if (buf == nullptr) {
    return 0U;
  }
  usb_in_buf_t* b = &s_usb.pipe_in[dev_pipe % k_usb_pipe_count];
  uint16_t      n = b->len;
  if (n > cap) {
    n = cap;
  }
  (void)memcpy(buf, b->data, n);
  b->len           = 0U;
  b->valid         = false;
  const uint32_t w = usb_word((uint64_t)k_ra_usb_off_bempsts);
  s_usb.reg[w]     = (uint16_t)(s_usb.reg[w] | (uint16_t)(1U << dev_pipe));
  usb_intsts0_set((uint8_t)k_ra_int0_bit_bemp);
  usb_raise_irq(uc);
  return n;
}

void board_usb_bridge_dcp_out(uc_engine* uc, const uint8_t* data, uint16_t len)
{
  (void)uc;
  if (data == nullptr) {
    return;
  }
  uint16_t n = len;
  if (n > (uint16_t)sizeof(s_dcp_hold)) {
    n = (uint16_t)sizeof(s_dcp_hold);
  }
  /* Hold the bytes as "in flight on the wire": the device has almost certainly
   * not armed its DCP for OUT yet, and its arm-time BCLR would discard them.
   * bridge_pump_device() lands them once the device is ready. */
  (void)memcpy(s_dcp_hold, data, n);
  s_dcp_hold_len     = n;
  s_dcp_hold_pending = true;
}

bool board_usb_bridge_dcp_out_consumed(void)
{
  /* Still on the wire (not yet handed to the device): not consumed. */
  if (s_dcp_hold_pending) {
    return false;
  }
  return s_usb.dcp_out.ready && (s_usb.dcp_out.rd >= s_usb.dcp_out.len);
}

/**
 * @brief Re-assert a device level-triggered condition the polled host awaits.
 *
 * @details In the chip-internal self-loop the polled host issues a control-write
 * data stage (e.g. a DFU_DNLOAD block) on the DCP before the device firmware --
 * running on the same core off its own IRQ -- has armed the DCP to receive it.
 * On real silicon the SIE keeps re-issuing the OUT token until the device ACKs;
 * here the one-shot BRDY the delivery pended is cleared by the device's own
 * ``ra_usb_dcp_out_arm`` (W0C) and never re-raised, so the receive stalls. This
 * pump, run each emulation chunk while the external host drives the device,
 * re-raises the device DCP BRDY whenever the device has armed the DCP for OUT
 * (PID=BUF and BRDYENB.PIPE0) and undrained OUT data is still staged -- exactly
 * the SIE's OUT-retry, so the device reads the block on its next BRDY ISR.
 *
 * @param[in,out] uc Unicorn engine (to pend the device USB interrupt).
 */
static void bridge_pump_device(uc_engine* uc)
{
  if (!s_dcp_hold_pending) {
    return;
  }
  const uint16_t dcpctr  = s_usb.reg[usb_word((uint64_t)k_ra_usb_off_dcpctr)];
  const uint16_t brdyenb = s_usb.reg[usb_word((uint64_t)k_ra_usb_off_brdyenb)];
  const bool     armed   = ((dcpctr & (uint16_t)k_ra_pid_mask) == (uint16_t)k_ra_pid_buf) &&
                           ((brdyenb & (uint16_t)k_usb_dcp_pipe_bit) != 0U);
  if (!armed) {
    return; /* device has not armed its DCP for OUT yet; keep the bytes on the wire. */
  }
  /* The device is ready (its arm-time BCLR already ran on an empty bank): land
   * the held control-OUT packet into the DCP and raise its BRDY so the device's
   * receive ISR drains it, exactly as an OUT token the device finally ACKs. */
  (void)memcpy(s_usb.dcp_out.data, s_dcp_hold, s_dcp_hold_len);
  s_usb.dcp_out.len   = s_dcp_hold_len;
  s_usb.dcp_out.rd    = 0U;
  s_usb.dcp_out.ready = true;
  s_dcp_hold_pending  = false;
  const uint32_t w    = usb_word((uint64_t)k_ra_usb_off_brdysts);
  s_usb.reg[w]        = (uint16_t)(s_usb.reg[w] | (uint16_t)k_usb_dcp_pipe_bit);
  usb_intsts0_set((uint8_t)k_ra_int0_bit_brdy);
  usb_raise_irq(uc);
}

/* Loop-cable (self-loop) state -- see the board_usb_loop_* block below. */
static bool     s_loop_latched;                   /**< Firmware host owns the bus.   */
static bool     s_loop_pending_cfg;               /**< SET_CONFIGURATION in flight.  */
static uint16_t s_loop_dcp_rd;                    /**< Host cursor into dcp_in.      */
static uint16_t s_loop_pipe_rd[k_usb_pipe_count]; /**< Host cursors into pipe_in.    */
static uint32_t s_loop_setups;                    /**< SETUPs the fw host delivered. */
static uint32_t s_loop_bulk_out_pkts;             /**< Bulk-OUT packets delivered.   */
static uint32_t s_loop_bulk_in_pkts;              /**< Bulk-IN packets drained.      */

void board_usb_tick(uc_engine* uc)
{
  /* Latch the CONFIGURED marker as soon as the device state reaches it -- the
   * host sets DVSQ, but record it here so a single source of truth drives both
   * the success assertion and the report marker. */
  if (s_usb.dvsq == (uint16_t)k_ra_dvsq_configured) {
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
  s_dev_irq_event    = (uint16_t)k_ra_elc_event_usbfs_int;
  s_usb.dvsq         = (uint16_t)k_ra_dvsq_powered;
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
  switch (dvsq & (uint16_t)k_ra_intsts0_mask_dvsq) {
    case (uint16_t)k_ra_dvsq_powered:
      return "Powered";
    case (uint16_t)k_ra_dvsq_default:
      return "Default";
    case (uint16_t)k_ra_dvsq_address:
      return "Address";
    case (uint16_t)k_ra_dvsq_configured:
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

/* =============================================================================
 * Loop-cable transport -- the USBHS HOST-mode model (board_usb_host.c) drives
 * this device through these calls when the firmware itself is the bus host
 * (self-loop bench: USBHS J7 host cabled to USBFS J11 device). Each helper
 * mirrors the corresponding built-in virtual-host step so the device-side
 * firmware sees identical register semantics either way.
 * =============================================================================
 */

/** @brief SETUP-word decode helpers + the DCP status bit for the loop path. */
typedef enum : uint16_t {
  k_loop_req_bm_mask    = 0x00FFU, /**< bmRequestType byte of USBREQ.    */
  k_ra_usb_dcp_brdy_bit = 0x0001U, /**< BRDYSTS bit 0: the DCP (pipe 0). */
} loop_req_mask_t;

/** @brief Reset one pipe's IN/OUT staging plus the host-side read cursor. */
static void loop_reset_pipe(uint8_t pipe)
{
  s_usb.pipe_in[pipe].len    = 0U;
  s_usb.pipe_in[pipe].valid  = false;
  s_usb.pipe_out[pipe].len   = 0U;
  s_usb.pipe_out[pipe].rd    = 0U;
  s_usb.pipe_out[pipe].ready = false;
  s_loop_pipe_rd[pipe]       = 0U;
}

/** @brief Reset the DCP staging (both directions) plus the host read cursor. */
static void loop_reset_dcp(void)
{
  s_usb.dcp_in.len    = 0U;
  s_usb.dcp_in.valid  = false;
  s_usb.dcp_out.len   = 0U;
  s_usb.dcp_out.rd    = 0U;
  s_usb.dcp_out.ready = false;
  s_loop_dcp_rd       = 0U;
}

void board_usb_loop_latch(void)
{
  if (s_loop_latched) {
    return;
  }
  s_loop_latched = true;
  usb_log_line("self-loop latched: firmware host owns the bus (virtual host parked)");
}

bool board_usb_loop_attached(void)
{
  return host_device_attached();
}

void board_usb_loop_bus_reset(uc_engine* uc)
{
  loop_reset_dcp();
  for (uint32_t p = 0U; p < (uint32_t)k_usb_pipe_count; p++) {
    loop_reset_pipe((uint8_t)p);
  }
  s_usb.setup_valid = false;
  s_usb.ctsq        = (uint16_t)k_ra_ctsq_idle;
  s_usb.dvsq        = (uint16_t)k_ra_dvsq_default;
  usb_intsts0_set((uint8_t)k_ra_int0_bit_dvst);
  usb_log_line("loop: bus reset -> DVSQ Default");
  usb_raise_irq(uc);
}

bool board_usb_loop_setup(uc_engine* uc, uint16_t req, uint16_t val, uint16_t indx, uint16_t leng)
{
  const uint8_t bm   = (uint8_t)(req & (uint16_t)k_loop_req_bm_mask);
  const uint8_t breq = (uint8_t)(req >> k_usb_byte_bits);
  /* A new SETUP supersedes whatever control transfer preceded it: drop stale
   * DCP staging and consume a stale CCPL (the fw host is strictly serial, so
   * any completion still latched here belongs to the finished transfer). */
  loop_reset_dcp();
  (void)host_take_ccpl();
  s_loop_pending_cfg                                  = false;
  s_usb.reg[usb_word((uint64_t)k_ra_usb_off_usbreq)]  = req;
  s_usb.reg[usb_word((uint64_t)k_ra_usb_off_usbval)]  = val;
  s_usb.reg[usb_word((uint64_t)k_ra_usb_off_usbindx)] = indx;
  s_usb.reg[usb_word((uint64_t)k_ra_usb_off_usbleng)] = leng;
  s_loop_setups++;
  char line[k_usb_log_width];
  (void)snprintf(line,
                 sizeof(line),
                 "loop: SETUP bm=0x%02X bReq=0x%02X wVal=0x%04X wLen=%u",
                 (unsigned)bm,
                 (unsigned)breq,
                 (unsigned)val,
                 (unsigned)leng);
  usb_log_line(line);
  if ((bm == (uint8_t)k_usb_dir_host_to_device) && (breq == (uint8_t)k_usb_req_set_address)) {
    /* The device SIE latches USBADDR and runs the whole status stage itself
     * (mirrors ::host_apply_no_data): the host needs no device CCPL. */
    s_usb.reg[usb_word((uint64_t)k_ra_usb_off_usbaddr)] =
      (uint16_t)(val & (uint16_t)k_ra_usbaddr_addr_mask);
    s_usb.dvsq = (uint16_t)k_ra_dvsq_address;
    usb_intsts0_set((uint8_t)k_ra_int0_bit_dvst);
    usb_raise_irq(uc);
    return true;
  }
  if ((bm == (uint8_t)k_usb_dir_host_to_device) && (breq == (uint8_t)k_usb_req_set_config)) {
    s_loop_pending_cfg = true; /* configured-state advance rides the CCPL. */
  }
  /* For an OUT-data request the payload arrives via board_usb_loop_ctrl_out. */
  s_usb.setup_valid = true;
  if ((bm & (uint8_t)k_usb_dir_device_to_host) != 0U) {
    s_usb.ctsq = (uint16_t)k_ra_ctsq_rdds;
  } else if (leng != 0U) {
    s_usb.ctsq = (uint16_t)k_ra_ctsq_wrds;
  } else {
    s_usb.ctsq = (uint16_t)k_ra_ctsq_wrnd;
  }
  usb_intsts0_set((uint8_t)k_ra_int0_bit_ctrt);
  usb_raise_irq(uc);
  return false;
}

bool board_usb_loop_take_ccpl(uc_engine* uc)
{
  if (!host_take_ccpl()) {
    return false;
  }
  if (s_loop_pending_cfg) {
    s_loop_pending_cfg = false;
    host_mark_configured(uc);
  }
  return true;
}

uint16_t board_usb_loop_ctrl_in_avail(void)
{
  if (!s_usb.dcp_in.valid) {
    return 0U;
  }
  return (uint16_t)(s_usb.dcp_in.len - s_loop_dcp_rd);
}

uint16_t board_usb_loop_ctrl_in_read(uint8_t* dst, uint16_t cap)
{
  const uint16_t avail = board_usb_loop_ctrl_in_avail();
  const uint16_t n     = (avail < cap) ? avail : cap;
  for (uint16_t i = 0U; i < n; i++) {
    dst[i] = s_usb.dcp_in.data[s_loop_dcp_rd + i];
  }
  s_loop_dcp_rd = (uint16_t)(s_loop_dcp_rd + n);
  if ((n > 0U) && (s_loop_dcp_rd >= s_usb.dcp_in.len)) {
    usb_detect_class(s_usb.dcp_in.data, s_usb.dcp_in.len);
    s_usb.dcp_in.len   = 0U;
    s_usb.dcp_in.valid = false;
    s_loop_dcp_rd      = 0U;
  }
  return n;
}

void board_usb_loop_ctrl_in_flush(void)
{
  s_usb.dcp_in.len   = 0U;
  s_usb.dcp_in.valid = false;
  s_loop_dcp_rd      = 0U;
}

void board_usb_loop_ctrl_out(uc_engine* uc, const uint8_t* data, uint16_t len)
{
  uint16_t n = len;
  if (n > (uint16_t)sizeof(s_usb.dcp_out.data)) {
    n = (uint16_t)sizeof(s_usb.dcp_out.data);
  }
  (void)memcpy(s_usb.dcp_out.data, data, n);
  s_usb.dcp_out.len   = n;
  s_usb.dcp_out.rd    = 0U;
  s_usb.dcp_out.ready = true;
  const uint32_t w    = usb_word((uint64_t)k_ra_usb_off_brdysts);
  s_usb.reg[w]        = (uint16_t)(s_usb.reg[w] | (uint16_t)k_ra_usb_dcp_brdy_bit);
  usb_intsts0_set((uint8_t)k_ra_int0_bit_brdy);
  usb_raise_irq(uc);
}

void board_usb_loop_status_out_zlp(uc_engine* uc)
{
  s_usb.ctsq        = (uint16_t)k_ra_ctsq_rdss;
  s_usb.setup_valid = false;
  usb_intsts0_set((uint8_t)k_ra_int0_bit_ctrt);
  usb_raise_irq(uc);
}

void board_usb_loop_bulk_out(uc_engine* uc, uint8_t ep, const uint8_t* data, uint16_t len)
{
  usb_out_buf_t* b = &s_usb.pipe_out[ep % k_usb_pipe_count];
  uint16_t       n = len;
  if (n > (uint16_t)sizeof(b->data)) {
    n = (uint16_t)sizeof(b->data);
  }
  (void)memcpy(b->data, data, n);
  b->len           = n;
  b->rd            = 0U;
  b->ready         = true;
  const uint32_t w = usb_word((uint64_t)k_ra_usb_off_brdysts);
  s_usb.reg[w]     = (uint16_t)(s_usb.reg[w] | (uint16_t)(1U << (ep % k_usb_pipe_count)));
  usb_intsts0_set((uint8_t)k_ra_int0_bit_brdy);
  s_loop_bulk_out_pkts++;
  usb_raise_irq(uc);
}

uint16_t board_usb_loop_bulk_in_avail(uint8_t ep)
{
  const usb_in_buf_t* b = &s_usb.pipe_in[ep % k_usb_pipe_count];
  if (!b->valid) {
    return 0U;
  }
  return (uint16_t)(b->len - s_loop_pipe_rd[ep % k_usb_pipe_count]);
}

uint16_t board_usb_loop_bulk_in_read(uc_engine* uc, uint8_t ep, uint8_t* dst, uint16_t cap)
{
  const uint8_t  pipe  = (uint8_t)(ep % k_usb_pipe_count);
  usb_in_buf_t*  b     = &s_usb.pipe_in[pipe];
  const uint16_t avail = board_usb_loop_bulk_in_avail(ep);
  const uint16_t n     = (avail < cap) ? avail : cap;
  for (uint16_t i = 0U; i < n; i++) {
    dst[i] = b->data[s_loop_pipe_rd[pipe] + i];
  }
  s_loop_pipe_rd[pipe] = (uint16_t)(s_loop_pipe_rd[pipe] + n);
  if ((n > 0U) && (s_loop_pipe_rd[pipe] >= b->len)) {
    b->len               = 0U;
    b->valid             = false;
    s_loop_pipe_rd[pipe] = 0U;
    s_loop_bulk_in_pkts++;
    /* Transmit buffer drained: raise the device's BEMP for this pipe so its
     * firmware can queue the next packet (mirrors ::host_echo_read_in). */
    const uint32_t w = usb_word((uint64_t)k_ra_usb_off_bempsts);
    s_usb.reg[w]     = (uint16_t)(s_usb.reg[w] | (uint16_t)(1U << pipe));
    usb_intsts0_set((uint8_t)k_ra_int0_bit_bemp);
    usb_raise_irq(uc);
  }
  return n;
}

void board_usb_loop_bulk_in_flush(uint8_t ep)
{
  const uint8_t pipe        = (uint8_t)(ep % k_usb_pipe_count);
  s_usb.pipe_in[pipe].len   = 0U;
  s_usb.pipe_in[pipe].valid = false;
  s_loop_pipe_rd[pipe]      = 0U;
}
