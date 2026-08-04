/**
 * @file board_usb_internal.h
 * @brief Module-private state + interfaces shared by the board_usb TUs
 *
 * @details
 * The USBFS device model is one logical module split across five translation
 * units (core lifecycle/tick/report, the register+CFIFO model, the built-in
 * virtual chapter-9 host, the USBHS<->USBFS bridge, and the self-loop
 * transport). They share one controller state instance and a set of
 * bookkeeping counters; this header carries the shared types, the state
 * declarations and the cross-TU helpers. Nothing here is part of the
 * emulator-facing API in inc/board_usb.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>
#include <unicorn/unicorn.h>

#include "board_usb.h"
#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

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
  k_cbw_tag_off         = 4U,    /**< CBW dCBWTag offset (LE u32).                    */
  k_cbw_dtl_off         = 8U,    /**< CBW dCBWDataTransferLength offset (LE u32).     */
  k_cbw_flags_off       = 12U,   /**< CBW bmCBWFlags offset.                          */
  k_cbw_lun_off         = 13U,   /**< CBW bCBWLUN offset.                             */
  k_cbw_cdblen_off      = 14U,   /**< CBW bCBWCBLength offset.                        */
  k_cbw_cdb_off         = 15U,   /**< CBW CBWCB (command block) offset.               */
} usb_lit_t;

/**
 * @enum usb_le_lane_t
 * @brief Byte-lane offsets within a little-endian 32-bit protocol field.
 *
 * @details
 * The USB Mass Storage CBW stores dCBWTag and dCBWDataTransferLength
 * little-endian, so each is written as four byte lanes relative to the
 * field's own offset. Naming the lanes keeps `k_cbw_tag_off + k_le_lane_b1`
 * readable as "byte 1 of the tag" rather than as the bare index 5.
 */
typedef enum : uint8_t {
  k_le_lane_b0 = 0U, /**< Least-significant byte (bits 7:0).  */
  k_le_lane_b1 = 1U, /**< Bits 15:8.                          */
  k_le_lane_b2 = 2U, /**< Bits 23:16.                         */
  k_le_lane_b3 = 3U, /**< Most-significant byte (bits 31:24). */
} usb_le_lane_t;

/**
 * @enum scsi_cap10_off_t
 * @brief Field offsets in a SCSI READ CAPACITY(10) parameter block.
 *
 * @details
 * Both fields are big-endian u32 (SCSI byte order), so lane 0 is the
 * most-significant byte -- the opposite of ::usb_le_lane_t.
 */
typedef enum : uint8_t {
  k_cap10_last_lba_off = 0U, /**< Returned logical block address (BE u32). */
  k_cap10_blocklen_off = 4U, /**< Block length in bytes (BE u32).          */
} scsi_cap10_off_t;

/** @brief Byte-lane offsets within a big-endian 32-bit SCSI field. */
typedef enum : uint8_t {
  k_be_lane_b3 = 0U, /**< Most-significant byte (bits 31:24). */
  k_be_lane_b2 = 1U, /**< Bits 23:16.                         */
  k_be_lane_b1 = 2U, /**< Bits 15:8.                          */
  k_be_lane_b0 = 3U, /**< Least-significant byte (bits 7:0).  */
} usb_be_lane_t;

/** @brief USB device class the host detected from the interface descriptor. */
typedef enum : uint8_t {
  k_usb_class_unknown = 0U, /**< Not yet detected.            */
  k_usb_class_cdc     = 1U, /**< CDC-ACM (virtual serial).    */
  k_usb_class_hid     = 2U, /**< HID (boot mouse / keyboard). */
  k_usb_class_msc     = 3U, /**< Mass storage (BOT/SCSI).     */
  k_usb_class_printer = 4U, /**< Printer (7/1/x), issue #265. */
  k_usb_class_vendor  = 5U, /**< Vendor specific (0xFF).      */
} usb_dev_class_t;

/** @brief bInterfaceClass codes + the INTERFACE descriptor type (USB 2.0). */
typedef enum : uint8_t {
  k_usb_iclass_cdc_comm = 0x02U, /**< Communications (CDC control). */
  k_usb_iclass_hid      = 0x03U, /**< Human Interface Device.       */
  k_usb_iclass_printer  = 0x07U, /**< Printer (issue #265).         */
  k_usb_iclass_msc      = 0x08U, /**< Mass Storage.                 */
  k_usb_iclass_cdc_data = 0x0AU, /**< CDC data.                     */
  k_usb_iclass_vendor   = 0xFFU, /**< Vendor specific (issue #265). */
  k_usb_dt_interface    = 0x04U, /**< INTERFACE descriptor type.    */
} usb_iclass_t;

/** @brief One SETUP step in the host enumeration script (USB 2.0 sec 9.4). */
typedef struct {
  uint8_t     bm_request_type; /**< Direction + type + recipient.        */
  uint8_t     b_request;       /**< bRequest.                            */
  uint16_t    w_value;         /**< wValue.                              */
  uint16_t    w_index;         /**< wIndex.                              */
  uint16_t    w_length;        /**< wLength (host's expected data size). */
  const char* name;            /**< Human label for the step log.        */
} usb_setup_step_t;

/** @brief Word index into the 16-bit register shadow for a window offset. */
static inline uint32_t usb_word(uint64_t off)
{
  return (uint32_t)((off & ~(uint64_t)1U) / 2U);
}

/* Shared model state -- one logical module across the board_usb_* TUs; the
 * defining TU is noted per declaration. */
extern usb_state_t            s_usb;                      /**< Controller + FIFO state (core).    */
extern bool                   s_trace;                    /**< --trace verbose logging (core).    */
extern board_usb_irq_raiser_t s_raise;                    /**< ICU pend callback (core).          */
extern bool                   s_external_host;            /**< Bridge host owns the bus (core).   */
extern bool                   s_roles_swapped;            /**< Self-loop role polarity (core).    */
extern uint16_t               s_dev_irq_event;            /**< Device ICU event number (core).    */
extern uint8_t                s_dcp_hold[k_usb_in_cap];   /**< Held control-OUT bytes (core).     */
extern uint16_t               s_dcp_hold_len;             /**< Held byte count (core).            */
extern bool                   s_dcp_hold_pending;         /**< Held bytes await the arm (core).   */
extern uint8_t                s_host_phase;               /**< Virtual-host phase (core).         */
extern uint8_t                s_host_step;                /**< Enumeration-script index (core).   */
extern uint8_t                s_host_substate;            /**< Per-step sub-state (core).         */
extern uint32_t               s_host_wait;                /**< Sub-state wait ticks (core).       */
extern bool                   s_configured;               /**< Device reached CONFIGURED (core).  */
extern uint32_t               s_usb_irqs;                 /**< USB interrupts raised (core).      */
extern uint8_t                s_echo_out[k_usb_echo_cap]; /**< Host bulk-OUT payload (core).      */
extern uint32_t               s_echo_out_len;             /**< Bytes queued by --usb-in (core).   */
extern uint32_t               s_echo_out_sent;            /**< Bytes delivered to device (core).  */
extern uint32_t               s_echo_in_got;              /**< Echoed bytes read back (core).     */
extern uint8_t                s_dev_class;                /**< Detected device class (vhost).     */
extern uint32_t               s_hid_reports;              /**< HID reports read (vhost).          */
extern int32_t                s_hid_cx;                   /**< Accumulated HID mouse X (vhost).   */
extern int32_t                s_hid_cy;                   /**< Accumulated HID mouse Y (vhost).   */
extern uint8_t                s_hid_buttons;              /**< Last HID button bitmap (vhost).    */
extern uint32_t               s_msc_blocks;               /**< MSC capacity in blocks (vhost).    */
extern uint32_t               s_msc_block_len;            /**< MSC block size (vhost).            */
extern uint32_t               s_msc_read_ok;              /**< MSC read data bytes (vhost).       */
extern bool                   s_msc_inquiry_ok;           /**< MSC INQUIRY completed (vhost).     */
extern bool                   s_loop_latched;             /**< Firmware host owns the bus (loop). */
extern uint32_t               s_loop_setups;              /**< SETUPs from the fw host (loop).    */
extern uint32_t               s_loop_bulk_out_pkts;       /**< Bulk-OUT packets (loop).           */
extern uint32_t               s_loop_bulk_in_pkts;        /**< Bulk-IN packets (loop).            */

/** @brief Append one already-formatted line to the enumeration-step log (core). */
RA8_PRIV void usb_log_line(const char* msg);

/** @brief Log a labelled count once (label + n) into the step log (core). */
RA8_PRIV void usb_log_count(const char* label, unsigned n);

/** @brief INTSTS0 value the device reads: event bits OR computed fields (dev). */
RA8_PRIV uint16_t usb_intsts0(void);

/** @brief Set one INTSTS0 event bit in the register shadow (dev). */
RA8_PRIV void usb_intsts0_set(uint8_t bit);

/** @brief Pend the device's ICU interrupt via the registered raiser (dev). */
RA8_PRIV void usb_raise_irq(uc_engine* uc);

/** @brief USBFS window register read dispatch (dev). */
RA8_PRIV uint32_t usb_reg_read(uint64_t off, unsigned size);

/** @brief USBFS window register write dispatch (dev). */
RA8_PRIV void usb_reg_write(uint64_t off, uint32_t value, unsigned size);

/** @brief Detect the device class from the enumerated config descriptor (vhost). */
RA8_PRIV void usb_detect_class(const uint8_t* d, uint16_t len);

/** @brief Human label of the detected class + live traffic totals (vhost). */
RA8_PRIV const char* usb_class_active_str(void);

/** @brief Virtual-host phase driver: waiting for the device pull-up (vhost). */
RA8_PRIV void host_run_idle_phase(uc_engine* uc);

/** @brief Virtual-host phase driver: holding bus reset (vhost). */
RA8_PRIV void host_run_reset_phase(uc_engine* uc);

/** @brief Virtual-host phase driver: walking the enumeration script (vhost). */
RA8_PRIV void host_run_setup_phase(uc_engine* uc);

/** @brief Virtual-host phase driver: post-CONFIGURED class traffic (vhost). */
RA8_PRIV void host_run_configured_phase(uc_engine* uc);

/** @brief Drain any device bulk-IN data the virtual host is owed (vhost). */
RA8_PRIV void host_echo_read_in(uc_engine* uc);

/** @brief Pump the device's level-triggered DCP-OUT receive (bridge). */
RA8_PRIV void bridge_pump_device(uc_engine* uc);

/** @brief True when SYSCFG.DPRPU is set: the device attached its pull-up (vhost). */
RA8_PRIV bool host_device_attached(void);

/** @brief Latch a SETUP packet into USBREQ..USBLENG + raise CTRT (vhost). */
RA8_PRIV void host_deliver_setup(uc_engine* uc, const usb_setup_step_t* s);

/** @brief Apply a no-data control write's device-state side effects (vhost). */
RA8_PRIV void host_apply_no_data(uc_engine* uc, const usb_setup_step_t* s);

/** @brief True when the device armed its DCP to BUF (ready for OUT) (vhost). */
RA8_PRIV bool host_dcp_pid_buf(void);

/** @brief Consume the device's CCPL (control transfer complete) (vhost). */
RA8_PRIV bool host_take_ccpl(void);

/** @brief Advance the device state to CONFIGURED (vhost). */
RA8_PRIV void host_mark_configured(uc_engine* uc);

#ifdef __cplusplus
}
#endif
