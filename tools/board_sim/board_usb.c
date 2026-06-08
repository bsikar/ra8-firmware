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

#include "ra8d2_elc_regs.h"
#include "ra8d2_usb_regs.h"

/* =============================================================================
 * Window geometry + model sizing.
 * =============================================================================
 */

/** @brief USBFS register-window geometry and the staging-buffer caps. */
typedef enum : uint64_t {
  k_usb_base       = 0x40250000UL, /**< USBFS base (HUM Ch 36, p 1965).     */
  k_usb_span       = 0x100UL,      /**< Modelled window (PIPECTR end < 0x90).*/
  k_usb_reg_words  = 0x100UL / 2U, /**< 16-bit register shadow word count.  */
  k_usb_dcp_mps    = 64UL,         /**< EP0 default control-pipe max packet.*/
  k_usb_pipe_mps   = 64UL,         /**< Bulk-FS data pipe max packet.       */
  k_usb_pipe_count = 10UL,         /**< DCP (0) + PIPE1..PIPE9.             */
  k_usb_echo_cap   = 256UL,        /**< Bulk echo staging capacity.         */
  k_usb_in_cap     = 512UL,        /**< DCP/bulk IN staging cap (multi-MPS).*/
} usb_geom_t;

/* =============================================================================
 * Standard USB chapter-9 request constants (USB 2.0 sec 9.3 / 9.4).
 * =============================================================================
 */

/** @brief bRequest values the virtual host issues (USB 2.0 sec 9.4). */
typedef enum : uint8_t {
  k_usb_req_get_descriptor = 0x06U, /**< GET_DESCRIPTOR.        */
  k_usb_req_set_address    = 0x05U, /**< SET_ADDRESS.          */
  k_usb_req_set_config     = 0x09U, /**< SET_CONFIGURATION.    */
} usb_breq_t;

/** @brief CDC class bRequest values issued after configuration (CDC 1.20). */
typedef enum : uint8_t {
  k_cdc_req_set_line_coding        = 0x20U, /**< SET_LINE_CODING (OUT data). */
  k_cdc_req_set_control_line_state = 0x22U, /**< SET_CONTROL_LINE_STATE.    */
} cdc_breq_t;

/** @brief bDescriptorType values used in GET_DESCRIPTOR wValue high byte. */
typedef enum : uint8_t {
  k_usb_dt_device = 0x01U, /**< DEVICE descriptor.        */
  k_usb_dt_config = 0x02U, /**< CONFIGURATION descriptor. */
  k_usb_dt_string = 0x03U, /**< STRING descriptor.        */
} usb_dt_t;

/** @brief bmRequestType direction bit (USB 2.0 sec 9.3 Table 9-2). */
typedef enum : uint8_t {
  k_usb_dir_device_to_host = 0x80U, /**< IN: device -> host (control read).  */
  k_usb_dir_host_to_device = 0x00U, /**< OUT: host -> device (control write).*/
} usb_dir_t;

/** @brief Misc small constants used across the model (avoid bare literals). */
typedef enum : uint32_t {
  k_usb_byte_bits     = 8U,    /**< Bits per byte.                       */
  k_usb_byte_mask     = 0xFFU, /**< One-byte mask.                      */
  k_usb_addr_assigned = 7U,    /**< Address the host assigns the device. */
  k_usb_config_value  = 1U,    /**< bConfigurationValue the host sets.   */
  k_usb_desc8_len     = 8U,    /**< First GET_DESCRIPTOR(device) length.  */
  k_usb_desc_dev_len  = 18U,   /**< Full device-descriptor length.       */
  k_usb_cfg_probe_len = 9U,    /**< Config-descriptor header probe len.  */
  k_usb_cfg_full_cap  = 255U,  /**< Config-descriptor full request cap.  */
  k_usb_str_len       = 255U,  /**< String-descriptor request length.    */
  k_usb_step_timeout  = 64U,   /**< Host per-step wait budget (ticks).    */
  k_usb_reset_settle  = 4U,    /**< Ticks held in bus reset before SETUP. */
  k_usb_post_cfg_idle = 8U,    /**< Settle ticks after CONFIGURED.       */
  k_usb_log_cap       = 48U,   /**< Enumeration-step log capacity.       */
  k_usb_log_width     = 96U,   /**< Bytes per enumeration-step log line.  */
  k_usb_bulk_in_pipe  = 1U,    /**< CDC bulk IN pipe (EP1 IN -> pipe 1).  */
  k_usb_bulk_out_pipe = 2U,    /**< CDC bulk OUT pipe (EP2 OUT -> pipe 2).*/
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
  uint8_t  data[k_usb_pipe_mps]; /**< Staged bytes the device will read.  */
  uint16_t len;                  /**< Total staged length.                */
  uint16_t rd;                   /**< Read cursor as the device drains.   */
  bool     ready;                /**< Data present (BRDY-equivalent).      */
} usb_out_buf_t;

/**
 * @brief One pipe's device-visible IN staging buffer (device -> host payload).
 *
 * @details The device fills this through CFIFO (descriptor response on the DCP,
 * echoed bytes on a bulk IN pipe) and pulses CFIFOCTR.BVAL; the host then
 * drains it.
 */
typedef struct {
  uint8_t  data[k_usb_in_cap]; /**< Bytes the device queued for the host.  */
  uint16_t len;                /**< Filled length (may span several MPS).  */
  bool     valid;              /**< BVAL pulsed: ready for the host.       */
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
  usb_in_buf_t  pipe_in[k_usb_pipe_count];  /**< Bulk IN per pipe.        */
  usb_out_buf_t pipe_out[k_usb_pipe_count]; /**< Bulk OUT per pipe.       */
} usb_state_t;

static usb_state_t            s_usb;
static bool                   s_trace;
static board_usb_irq_raiser_t s_raise;

/* Virtual-host bookkeeping. */
static uint8_t  s_host_phase;    /**< Current host state-machine phase.    */
static uint8_t  s_host_step;     /**< Index into the enumeration script.   */
static uint8_t  s_host_substate; /**< Sub-state within one SETUP step.     */
static uint32_t s_host_wait;     /**< Ticks spent waiting in a sub-state.  */
static bool     s_configured;    /**< Device reached CONFIGURED.           */
static uint32_t s_usb_irqs;      /**< USB interrupts the host raised.      */

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

/** @brief Raise the USBFS controller interrupt (USBFS_INT == 0x09A). */
static void usb_raise_irq(uc_engine* uc)
{
  if (s_raise != nullptr) {
    s_raise(uc, (uint16_t)k_ra_elc_event_usbfs_int);
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

/** @brief Service a CFIFO data-port read: drain one unit from the OUT buffer. */
static uint16_t cfifo_read_port(unsigned size)
{
  usb_out_buf_t* b = cfifo_out_buf();
  uint16_t       v = 0U;
  for (unsigned i = 0U; (i < size) && (b->rd < b->len); i++) {
    v = (uint16_t)(v | ((uint16_t)b->data[b->rd] << (i * k_usb_byte_bits)));
    b->rd++;
  }
  return v;
}

/** @brief Service a CFIFO data-port write: append one unit to the IN buffer. */
static void cfifo_write_port(uint16_t value, unsigned size)
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

/** @brief Read one USBFS register; @p off is the byte offset into the window. */
static uint16_t usb_reg_read(uint64_t off, unsigned size)
{
  switch ((uint16_t)off) {
    case (uint16_t)k_ra_usb_off_intsts0:
      return usb_intsts0();
    case (uint16_t)k_ra_usb_off_cfifoctr:
      return cfifoctr_read();
    case (uint16_t)k_ra_usb_off_cfifo:
      return cfifo_read_port(size);
    case (uint16_t)k_ra_usb_off_syssts0:
      return (uint16_t)0x0003U; /* LNST = J-state: device pull-up seen.       */
    case (uint16_t)k_ra_usb_off_frmnum:
      return s_usb.reg[usb_word((uint64_t)k_ra_usb_off_frmnum)];
    default:
      return s_usb.reg[usb_word(off)];
  }
}

/** @brief Write one USBFS register; @p off is the byte offset into the window. */
static void usb_reg_write(uint64_t off, uint16_t value, unsigned size)
{
  switch ((uint16_t)off) {
    case (uint16_t)k_ra_usb_off_cfifo:
      cfifo_write_port(value, size);
      return;
    case (uint16_t)k_ra_usb_off_cfifoctr:
      cfifoctr_write(value);
      return;
    case (uint16_t)k_ra_usb_off_intsts0:
      /* W0C on the event bits: a written 0 clears, a 1 preserves. */
      s_usb.reg[usb_word(off)] &= value;
      if ((value & (uint16_t)k_ra_intsts0_mask_valid) == 0U) {
        s_usb.setup_valid = false;
      }
      return;
    default:
      s_usb.reg[usb_word(off)] = value;
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
  return (uint64_t)usb_reg_read(addr - (uint64_t)k_usb_base, size);
}

void board_usb_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value, bool* handled)
{
  (void)uc;
  if ((addr < (uint64_t)k_usb_base) || (addr >= ((uint64_t)k_usb_base + (uint64_t)k_usb_span))) {
    *handled = false;
    return;
  }
  *handled = true;
  usb_reg_write(addr - (uint64_t)k_usb_base, (uint16_t)value, size);
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
  uint16_t    w_length;        /**< wLength (host's expected data size).  */
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
  {k_usb_dir_host_to_device | 0x21U, k_cdc_req_set_line_coding, 0U, 0U, 7U, "SET_LINE_CODING"},
  {k_usb_dir_host_to_device | 0x21U,
   k_cdc_req_set_control_line_state,
   0x0003U,
   0U,
   0U,
   "SET_CONTROL_LINE_STATE"},
};

/** @brief Host state-machine phases. */
typedef enum : uint8_t {
  k_phase_idle       = 0U, /**< Waiting for the device pull-up (DPRPU).     */
  k_phase_reset      = 1U, /**< Holding bus reset; device re-arms its DCP.  */
  k_phase_setup      = 2U, /**< Walking the enumeration script.            */
  k_phase_configured = 3U, /**< Device configured; optional bulk echo.     */
  k_phase_done       = 4U, /**< Terminal idle.                            */
} usb_host_phase_t;

/** @brief Per-SETUP-step sub-states. */
typedef enum : uint8_t {
  k_sub_deliver  = 0U, /**< Latch the SETUP + raise CTRT.                  */
  k_sub_wait_in  = 1U, /**< Wait for the device's IN data (control read).  */
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
   * drain through the DCP if its class handler reads them. */
  if (((s->bm_request_type & (uint8_t)k_usb_dir_device_to_host) == 0U) && (s->w_length != 0U)) {
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

/** @brief Drain any bytes the device echoed onto the bulk IN pipe. */
static void host_echo_read_in(void)
{
  usb_in_buf_t* b = &s_usb.pipe_in[k_usb_bulk_in_pipe];
  if (b->valid && (b->len > 0U)) {
    s_echo_in_got += b->len;
    usb_log_count("bulk IN: read echoed bytes from data pipe", (unsigned)b->len);
    b->len   = 0U;
    b->valid = false;
  } else if (b->valid) {
    b->valid = false; /* drained ZLP terminator. */
  }
}

/** @brief Phase k_phase_configured: optionally drive the CDC bulk echo. */
static void host_run_configured_phase(uc_engine* uc)
{
  host_echo_read_in();
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

void board_usb_tick(uc_engine* uc)
{
  /* Latch the CONFIGURED marker as soon as the device state reaches it -- the
   * host sets DVSQ, but record it here so a single source of truth drives both
   * the success assertion and the report marker. */
  if (s_usb.dvsq == (uint16_t)k_ra_dvsq_configured) {
    s_configured = true;
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
      host_echo_read_in(); /* keep draining any late echo. */
      break;
  }
}

/* =============================================================================
 * Public lifecycle / accessors / report.
 * =============================================================================
 */

void board_usb_init(bool trace)
{
  s_usb           = (usb_state_t){};
  s_trace         = trace;
  s_host_phase    = (uint8_t)k_phase_idle;
  s_host_step     = 0U;
  s_host_substate = (uint8_t)k_sub_deliver;
  s_host_wait     = 0U;
  s_configured    = false;
  s_usb_irqs      = 0U;
  s_echo_out_len  = 0U;
  s_echo_out_sent = 0U;
  s_echo_in_got   = 0U;
  s_log_n         = 0U;
  s_usb.dvsq      = (uint16_t)k_ra_dvsq_powered;
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
  (void)fprintf(stderr,
                "  USB: %s\n",
                s_configured ? "device CONFIGURED (CDC-ACM active)"
                             : "enumeration INCOMPLETE (device did not reach CONFIGURED)");
  if (s_echo_out_len > 0U) {
    (void)fprintf(stderr,
                  "  USB CDC echo  : sent %u byte(s) OUT, read %u byte(s) IN\n",
                  s_echo_out_sent,
                  s_echo_in_got);
  }
}
