/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/usbx/inc/ux_dcd_ra8_usb_internal.h
 * @brief USBX device-controller-driver bridge to ra8_usb -- per-module
 *        cross-translation-unit contract.
 *
 * @par Tag
 * [Ring 5 / PORT] {World: S}
 *
 * @details
 * The DCD bridge implementation is split across ux_dcd_ra8_usb.c and
 * its per-aspect siblings (ux_dcd_ra8_usb_{ep,xfer,isr,setup,dvst,
 * dvst_default,irq}.c). This header carries every symbol that more
 * than one of those translation units references: the shared typed-enum
 * and struct definitions, the bridge singleton and diagnostic blocks
 * (extern), and the handful of formerly-static helpers that are
 * called across the split. Single-translation-unit symbols stay private
 * to their owning .c; only genuinely shared symbols live here.
 *
 * This header is included by every translation unit of the bridge, after
 * the public ux_dcd_ra8_usb.h.
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_isr.h"
#include "ra8_usb.h"
#include "ux_dcd_ra8_usb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Shared typed enums and structs */
/* -------------------------------------------------------------------------- */

/**
 * @enum ra8_usb_dcd_intsts0_field_shift_t
 * @brief Bit-position shifts used to extract INTSTS0 multi-bit fields.
 *
 * @details HUM Ch 36.2.16 INTSTS0 p 1986: DVSQ occupies bits 6:4.
 */
typedef enum : uint8_t {
  k_ra8_int0_dvsq_shift = 4U, /**< DVSQ[2:0] field starts at bit 4. */
} ra8_usb_dcd_intsts0_field_shift_t;

/**
 * @struct ra8_usb_dcd_pipe_slot_t
 * @brief Per-pipe class-layer cache so the IRQ handler can re-arm
 *        BRDY-driven transfers without crawling the device endpoint
 *        list.
 */
typedef struct {
  struct UX_SLAVE_TRANSFER_STRUCT* xfer;    /**< Active transfer or NULL.      */
  uint8_t                          ep_addr; /**< USB EP number (with dir bit). */
  uint8_t                          dir_in;  /**< 1 if IN pipe, 0 if OUT.       */
  uint16_t                         max_pkt; /**< Endpoint wMaxPacketSize.      */
} ra8_usb_dcd_pipe_slot_t;

/**
 * @struct ra8_usb_dcd_t
 * @brief Bridge-singleton state.
 */
typedef struct {
  ra8_usb_dcd_state_t         state;                             /**< Bridge run-state.       */
  ra8_usb_speed_t             speed;                             /**< Controller this drives. */
  struct UX_SLAVE_DCD_STRUCT* owner;                             /**< Back-pointer into USBX. */
  ra8_usb_dcd_pipe_slot_t     pipes[k_ux_dcd_ra8_usb_max_pipes]; /**< DCP + PIPE1..9.         */
} ra8_usb_dcd_t;

/* Orphan bulk-OUT holding buffer. A host OUT packet can land in the
 * controller bank during the brief PID=BUF window between one transfer
 * completing and the next being armed -- the host pipelines a CBW's
 * data phase straight after the CBW. With no USBX waiter stashed the
 * IRQ walk cannot drain it, BRDYSTS stays latched, and the USBFS IRQ
 * storms at the full bus rate: BRDY is a real event so the event-less
 * storm guard never masks it and RTOS thread mode starves (GitHub
 * issue #6). internal_irq_drain_orphan_out pulls that packet into
 * s_orphan_buf -- which W0C-clears BRDYSTS and parks the pipe -- and
 * internal_submit_pipe hands it to the next bulk-OUT transfer. The
 * bulk-OUT wire is strictly serial, so a held packet always belongs to
 * the next OUT transfer USBX submits. */
typedef enum : uint16_t {
  k_ra8_usb_orphan_bytes = 64U, /**< One full-speed bulk MPS packet. */
} ux_dcd_ra8_usb_orphan_cfg_t;

/**
 * @struct ra8_usb_dcd_diag_t
 * @brief Externally-readable diagnostic counters for OUT-pipe stall debug.
 * @details Read these via JLink to localise where bulk-OUT data flow stops.
 *          Layout in declaration order, 4 bytes each. Address resolved
 *          via `arm-none-eabi-nm | grep s_diag`. Increment-only; never
 *          cleared at runtime so a JLink dump after a single host write
 *          tells the full story.
 */
typedef struct {
  volatile uint32_t xfer_req_total;          /**< +0x00 internal_transfer_request entered. */
  volatile uint32_t xfer_req_null_arg;       /**< +0x04 tr or ep was NULL.                 */
  volatile uint32_t xfer_req_bad_pipe;       /**< +0x08 pipe index >= max.                 */
  volatile uint32_t xfer_req_pipe2_in;       /**< +0x0C pipe == 2.                         */
  volatile uint32_t xfer_req_pipe2_out_dir;  /**< +0x10 pipe == 2 + OUT direction.         */
  volatile uint32_t xfer_req_pipe2_stashed;  /**< +0x14 pipe2 OUT slot populated.          */
  volatile uint32_t xfer_req_pipe2_block;    /**< +0x18 about to tx_semaphore_get.         */
  volatile uint32_t xfer_req_pipe2_woken;    /**< +0x1C tx_semaphore_get returned.         */
  volatile uint32_t irq_walk_total;          /**< +0x20 IRQ pipe-walk loop iterations.     */
  volatile uint32_t irq_walk_pipe2_seen;     /**< +0x24 pipe2 walked with non-NULL xfer.   */
  volatile uint32_t irq_walk_pipe2_complete; /**< +0x28 queue_out drained pipe2.           */
  volatile uint32_t irq_walk_pipe2_no_data;  /**< +0x2C queue_out returned no_data.        */
  volatile uint32_t in_rearm_nak;            /**< +0x30 stashed IN at PID=NAK with a loaded
                                              *   bank; PID forced back to BUF.            */
  volatile uint32_t in_restage_nak;          /**< +0x34 stashed IN at PID=NAK, empty bank,
                                              *   no BEMP; current chunk re-staged.        */
  volatile uint32_t in_stage_fail;           /**< +0x38 queue_in failed while staging an IN
                                              *   chunk from the IRQ walk.                 */
  volatile uint32_t ep_create_calls;         /**< +0x3C endpoint-create invocations.    */
  volatile uint32_t ep_create_fail;          /**< +0x40 endpoint-create failures.       */
  volatile uint32_t chg_state_attached;      /**< +0x44 CHANGE_STATE(ATTACHED) calls.   */
  volatile uint32_t chg_state_configured;    /**< +0x48 CHANGE_STATE(CONFIGURED) calls. */
} ra8_usb_dcd_diag_t;

/**
 * @enum ra8_usb_dcd_trace_t
 * @brief Sizing/packing constants for the JLink-readable event ring.
 *
 * @details Each ring entry is one uint32 packed as
 * ``kind<<24 | code<<16 | length``: kind 1 = bulk-OUT transfer
 * completed (code = SCSI opcode when the transfer is a 31-byte CBW),
 * kind 2 = bulk-IN transfer completed (code unused), kind 3 = EP0
 * SETUP received (code = bRequest, length = wValue). Read
 * ``s_trace_seq`` then ``s_trace[]`` via JLink to reconstruct the
 * device-side BOT conversation after a host probe.
 */
typedef enum : uint32_t {
  k_dcd_trace_entries    = 64U,   /**< Ring slots (power of two).          */
  k_dcd_trace_kind_out   = 1U,    /**< Bulk-OUT transfer completed.        */
  k_dcd_trace_kind_in    = 2U,    /**< Bulk-IN transfer completed.         */
  k_dcd_trace_kind_setup = 3U,    /**< EP0 SETUP received.                 */
  k_dcd_trace_kind_shift = 24U,   /**< Kind field bit offset.              */
  k_dcd_trace_code_shift = 16U,   /**< Code field bit offset.              */
  k_dcd_trace_no_code    = 0xFFU, /**< Code when none applies.             */
  k_dcd_trace_cbw_len    = 31U,   /**< BOT CBW wire length.                */
  k_dcd_trace_cbw_op_off = 15U,   /**< CDB opcode offset inside a CBW.     */
  k_dcd_trace_byte_shift = 8U,    /**< CDB byte-pair packing shift.        */
  k_dcd_trace_op_read10  = 0x28U, /**< SCSI READ(10) opcode.               */
  k_dcd_trace_op_write10 = 0x2AU, /**< SCSI WRITE(10) opcode.              */
  k_dcd_trace_cdb_lba_hi = 4U,    /**< CDB byte: LBA bits 15..8.           */
  k_dcd_trace_cdb_lba_lo = 5U,    /**< CDB byte: LBA bits 7..0.            */
  k_dcd_trace_kind_ocap  = 5U,    /**< Orphan OUT packet captured.         */
  k_dcd_trace_kind_ouse  = 6U,    /**< Orphan packet fed to a transfer.    */
  k_dcd_trace_kind_ccpl  = 7U,    /**< EP0 H2D status stage driven.        */
  k_dcd_trace_kind_dvst  = 9U,    /**< DVST event (code = dvsq|state).     */
  k_dcd_trace_nibble     = 0x0FU, /**< Low-nibble mask for packed bytes.   */
  k_dcd_trace_nib_shift  = 4U,    /**< High-nibble shift.                  */
  k_dcd_out_drain_max    = 4U,    /**< Max OUT banks drained per ISR pass. */
} ra8_usb_dcd_trace_t;

/**
 * @enum ra8_setup_byte_idx_t
 * @brief Wire-format byte indices into the USBX SETUP buffer.
 *
 * @details
 * Mirrors the USB 2.0 Ch 9.3 layout, identical to USBX's own
 * ``UX_SETUP_REQUEST_TYPE`` .. ``UX_SETUP_LENGTH`` constants but
 * expressed as a typed enum to satisfy the project's no-magic-numbers
 * rule and to keep the SETUP-pack code readable.
 */
typedef enum : uint8_t {
  k_setup_idx_bmrt   = 0U, /**< bmRequestType (offset 0).     */
  k_setup_idx_brq    = 1U, /**< bRequest      (offset 1).     */
  k_setup_idx_val_lo = 2U, /**< wValue  low byte  (offset 2). */
  k_setup_idx_val_hi = 3U, /**< wValue  high byte (offset 3). */
  k_setup_idx_idx_lo = 4U, /**< wIndex  low byte  (offset 4). */
  k_setup_idx_idx_hi = 5U, /**< wIndex  high byte (offset 5). */
  k_setup_idx_len_lo = 6U, /**< wLength low byte  (offset 6). */
  k_setup_idx_len_hi = 7U, /**< wLength high byte (offset 7). */
} ra8_setup_byte_idx_t;

/**
 * @enum ra8_setup_byte_pack_t
 * @brief Bit-shift / mask constants for splitting a uint16_t SETUP
 *        field into its little-endian byte pair.
 */
typedef enum : uint16_t {
  k_setup_byte_shift = 8U,    /**< Bits per byte for the hi-byte extraction. */
  k_setup_byte_mask  = 0xFFU, /**< Low-byte mask after the shift.            */
} ra8_setup_byte_pack_t;

/**
 * @enum ra8_usb_ep_addr_field_t
 * @brief Bit fields of a USB endpoint address (bEndpointAddress).
 *
 * @details USB 2.0 sec 9.6.6: bit 7 is the direction bit (1 = IN), and
 * bits 3:0 carry the endpoint number; bits 6:4 are reserved.
 */
typedef enum : uint8_t {
  k_ra8_usb_ep_addr_dir_in_bit = 0x80U, /**< Direction bit set => IN endpoint. */
  k_ra8_usb_ep_addr_num_mask   = 0x0FU, /**< Endpoint-number field (bits 3:0). */
} ra8_usb_ep_addr_field_t;

/**
 * @enum ra8_usb_dcd_field_mask_t
 * @brief Small field masks and sentinels used across the DCD bridge.
 */
typedef enum : uint16_t {
  k_ra8_usb_dvsq_field_mask = 0x07U,  /**< 3-bit DVSQ/RHST field after shift. */
  k_ra8_usb_state_unknown   = 0xFFUL, /**< Device-state snapshot sentinel.    */
} ra8_usb_dcd_field_mask_t;

/**
 * @enum ra8_usb_dispatch_skip_bit_t
 * @brief Bits ORed into ``s_dispatch_skip_reason`` to record why a
 *        SETUP dispatch was skipped or how it completed.
 */
typedef enum : uint8_t {
  k_ra8_usb_skip_usbreq_unchanged = 0x40U, /**< USBREQ unchanged since last dispatch. */
  k_ra8_usb_skip_process_ok       = 0x80U, /**< control_request_process returned OK.  */
} ra8_usb_dispatch_skip_bit_t;

/* -------------------------------------------------------------------------- */
/* Shared mutable state (defined once; externed here) */
/* -------------------------------------------------------------------------- */

/**
 * @var s_dcd
 * @brief The single bridge instance, defined in ux_dcd_ra8_usb.c.
 * @details RA8 has two USB controllers but the device stack only ever
 * drives one at a time, so a single instance is sufficient. Updated from
 * the ISR and the dispatch trampoline; concurrency is arbitrated at the
 * call-site.
 * @note Not thread-safe.
 * @since 0.1.0
 */
extern ra8_usb_dcd_t s_dcd;

/**
 * @var s_diag
 * @brief Bridge diagnostic counter block, defined in ux_dcd_ra8_usb.c.
 * @note Single-writer per counter; JLink-readable.
 * @since 0.1.0
 */
extern ra8_usb_dcd_diag_t s_diag;

/**
 * @var s_dcd_auto_echo_enable
 * @brief Non-zero once ISR-side auto-echo is armed. Defined in
 *        ux_dcd_ra8_usb.c.
 * @since 0.1.0
 */
extern volatile uint32_t s_dcd_auto_echo_enable;

/**
 * @var s_dcd_auto_echo_out_pipe
 * @brief Pipe drained by the in-ISR auto-echo path. Defined in
 *        ux_dcd_ra8_usb.c.
 * @since 0.1.0
 */
extern uint8_t s_dcd_auto_echo_out_pipe;

/**
 * @var s_dcd_auto_echo_in_pipe
 * @brief Pipe the auto-echo path re-queues onto. Defined in
 *        ux_dcd_ra8_usb.c.
 * @since 0.1.0
 */
extern uint8_t s_dcd_auto_echo_in_pipe;

/**
 * @var s_isr_spurious_run
 * @brief Consecutive event-less ISR entries since the last SysTick
 *        re-enable. Defined in ux_dcd_ra8_usb.c.
 * @since 0.1.0
 */
extern volatile uint32_t s_isr_spurious_run;

/**
 * @var s_orphan_buf
 * @brief One held no-receiver bulk-OUT packet. Defined in
 *        ux_dcd_ra8_usb_xfer.c.
 * @since 0.1.0
 */
extern uint8_t s_orphan_buf[k_ra8_usb_orphan_bytes];

/**
 * @var s_orphan_len
 * @brief Held orphan byte count; 0 = empty. Defined in
 *        ux_dcd_ra8_usb_xfer.c.
 * @since 0.1.0
 */
extern uint16_t s_orphan_len;

/**
 * @var s_orphan_pipe
 * @brief Pipe the held orphan packet is on. Defined in
 *        ux_dcd_ra8_usb_xfer.c.
 * @since 0.1.0
 */
extern uint8_t s_orphan_pipe;

/**
 * @var s_setup_dispatch_count
 * @brief Count of SETUP packets fed into the chapter-9 dispatcher.
 *        Defined in ux_dcd_ra8_usb_setup.c.
 * @since 0.1.0
 */
extern volatile uint32_t s_setup_dispatch_count;

/**
 * @var s_setup_packet_buffer
 * @brief Wire-format bytes of the most recent SETUP packet. Defined in
 *        ux_dcd_ra8_usb_setup.c.
 * @since 0.1.0
 */
extern volatile uint8_t s_setup_packet_buffer[8];

/**
 * @var s_setup_packet_count
 * @brief Total SETUP packets latched into ::s_setup_packet_buffer.
 *        Defined in ux_dcd_ra8_usb_setup.c.
 * @since 0.1.0
 */
extern volatile uint32_t s_setup_packet_count;

/**
 * @var s_dispatch_skip_reason
 * @brief Last-iteration bitmask of why a SETUP dispatch was skipped or
 *        completed. Defined in ux_dcd_ra8_usb_setup.c.
 * @since 0.1.0
 */
extern volatile uint32_t s_dispatch_skip_reason;

/**
 * @var s_last_dispatched_setup_fp
 * @brief 64-bit fingerprint of the last dispatched SETUP packet. Defined
 *        in ux_dcd_ra8_usb_setup.c.
 * @since 0.1.0
 */
extern volatile uint64_t s_last_dispatched_setup_fp;

/**
 * @var s_setup_token_observed
 * @brief Counter of proven device-side SETUP-token latches. Defined in
 *        ux_dcd_ra8_usb_dvst_default.c.
 * @since 0.1.0
 */
extern volatile uint32_t s_setup_token_observed;

/* -------------------------------------------------------------------------- */
/* Shared helpers (formerly static; called across translation units) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Map a USB EP number (1..9) into the PIPE table index.
 * @details Defined in ``ux_dcd_ra8_usb_xfer.c``.
 * @param[in] ep_addr Endpoint address (with dir bit in 0x80).
 * @return Pipe index 0..9, or ::k_ux_dcd_ra8_usb_max_pipes on overflow.
 * @retval 0 The masked endpoint number is 0 (control endpoint EP0).
 * @retval 1..9 The masked endpoint number, used directly as the pipe index.
 * @retval ::k_ux_dcd_ra8_usb_max_pipes The masked endpoint number is >= the
 *         pipe count (out-of-range overflow sentinel).
 * @pre ::s_dcd is past ``ux_dcd_ra8_usb_initialize``.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV uint8_t internal_ep_to_pipe(uint8_t ep_addr);

/**
 * @brief USBX ``UX_DCD_TRANSFER_REQUEST`` dispatcher entry point.
 * @details Defined in ``ux_dcd_ra8_usb_xfer.c``; validates the incoming
 * transfer request, maps the endpoint to a pipe, and dispatches to the
 * EP0 control path or the IN/OUT data-transfer path.
 * @param[in,out] tr USBX transfer request to submit.
 * @return USBX result code.
 * @retval UX_SUCCESS Transfer submitted (and completed by the IRQ path).
 * @retval UX_TRANSFER_ERROR Validation or bridge-layer rejection.
 * @pre ::s_dcd is past ``ux_dcd_ra8_usb_initialize``.
 * @pre Caller is the USBX device-stack dispatcher (task context).
 * @post For non-EP0 IN transfers, ``s_dcd.pipes[pipe]`` holds the stash.
 * @post ``s_diag`` counters reflect the dispatch.
 * @note Not ISR-safe; runs on the USBX device task context.
 * @since 0.1.0
 */
RA8_PRIV unsigned int internal_transfer_request(struct UX_SLAVE_TRANSFER_STRUCT* tr);

/**
 * @brief Mask the USB IRQ at the NVIC to break an interrupt storm.
 * @details Defined in ux_dcd_ra8_usb.c; called from the ISR
 * trampolines once a sustained run of event-less entries proves a storm.
 * @pre Called from the active controller's ISR context.
 * @pre ::s_usb_irq_slot has been resolved at init.
 * @post USB IRQ line disabled at the NVIC until the next SysTick re-enable.
 * @post Pending USB events stay latched in INTSTS0 / BRDYSTS (level state).
 * @note ISR-safe.
 * @since 0.1.0
 */
RA8_PRIV void internal_usbfs_irq_mask(void);

/**
 * @brief Append one packed event to the JLink-readable trace ring.
 * @details Defined in ux_dcd_ra8_usb_setup.c; packs
 * kind<<24 | code<<16 | length into the next ring slot.
 * @param[in] kind   Event kind (::ra8_usb_dcd_trace_t kinds).
 * @param[in] code   Per-kind code byte (opcode / bRequest / none).
 * @param[in] length Per-kind 16-bit payload (length or wValue).
 * @pre Any context; single concurrent writer (IRQ-callback path).
 * @pre ::s_dcd is past ux_dcd_ra8_usb_initialize.
 * @post One ring slot holds the packed event; sequence incremented.
 * @post No other state changes.
 * @note Diagnostic only; never read by production code.
 * @since 0.1.0
 */
RA8_PRIV void internal_trace_event(uint8_t kind, uint8_t code, uint16_t length);

/**
 * @brief Pick the ELC event number for a controller.
 * @details Defined in ux_dcd_ra8_usb_isr.c.
 * @param[in] speed Which controller (FS or HS).
 * @return ra8_elc_event_t event number for that controller.
 * @retval k_ra8_elc_event_usbhs_int_resume speed is k_ra8_usb_speed_hs.
 * @retval k_ra8_elc_event_usbfs_int speed is k_ra8_usb_speed_fs (any non-HS value).
 * @pre speed is k_ra8_usb_speed_fs or k_ra8_usb_speed_hs.
 * @pre ::s_dcd is past ux_dcd_ra8_usb_initialize.
 * @post No state mutated.
 * @post Pure function.
 * @note Pure function.
 * @since 0.1.0
 */
RA8_PRIV ra8_elc_event_t internal_pick_event(ra8_usb_speed_t speed);

/**
 * @brief Pick the ISR trampoline for a controller.
 * @details Defined in ux_dcd_ra8_usb_isr.c.
 * @param[in] speed Which controller (FS or HS).
 * @return Function pointer to the trampoline.
 * @retval internal_usbhs_isr speed is k_ra8_usb_speed_hs.
 * @retval internal_usbfs_isr speed is k_ra8_usb_speed_fs (any non-HS value).
 * @pre speed is k_ra8_usb_speed_fs or k_ra8_usb_speed_hs.
 * @pre ::s_dcd is past ux_dcd_ra8_usb_initialize.
 * @post No state mutated.
 * @post Pure function.
 * @note Pure function.
 * @since 0.1.0
 */
RA8_PRIV ra8_isr_handler_t internal_pick_isr(ra8_usb_speed_t speed);

/**
 * @brief ra8_usb_attach_handler trampoline into ::ux_dcd_ra8_usb_irq.
 * @details Defined in ux_dcd_ra8_usb_isr.c.
 * @param[in,out] ctx Unused ra8_usb callback context.
 * @param[in] speed Which controller fired.
 * @param[in] status_mask INTSTS0 snapshot forwarded from ra8_usb_dispatch.
 * @pre ::s_dcd is past ux_dcd_ra8_usb_initialize.
 * @pre Caller is ra8_usb_dispatch.
 * @post ::ux_dcd_ra8_usb_irq has run for the snapshot.
 * @post No other state changes here.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV void internal_event_cb(void* ctx, ra8_usb_speed_t speed, uint16_t status_mask);

/**
 * @brief Push a decoded SETUP packet into the USBX chapter-9 dispatcher.
 * @details Defined in ux_dcd_ra8_usb_setup.c.
 * @param[in] setup Decoded SETUP packet snapshot (non-null).
 * @return USBX result code.
 * @retval UX_SUCCESS Chapter-9 dispatcher consumed the SETUP.
 * @retval UX_ERROR setup null, USBX not bound, or EP0 unavailable.
 * @pre setup is non-null.
 * @pre _ux_system_slave is bound by _ux_device_stack_initialize.
 * @post EP0 transfer-request setup buffer holds the wire-format SETUP.
 * @post Chapter-9 dispatcher has been invoked synchronously.
 * @note Runs in IRQ-callback context; must not block.
 * @since 0.1.0
 */
RA8_PRIV unsigned int internal_dispatch_setup(const ra8_usb_setup_t* setup);

/**
 * @brief Handle the CTRT (control-transfer-stage) interrupt branch.
 * @details Defined in ux_dcd_ra8_usb_setup.c.
 * @param[in] speed Which controller fired.
 * @param[in] intsts0 INTSTS0 snapshot captured at the top of the ISR.
 * @pre Caller has masked intsts0 against the event mask.
 * @pre ::s_dcd is past ux_dcd_ra8_usb_initialize.
 * @post For SET_ADDRESS, INTSTS0.VALID is W0C-cleared.
 * @post For other fresh SETUPs the chapter-9 dispatcher has run.
 * @note ISR-callback context; must not block.
 * @since 0.1.0
 */
RA8_PRIV void internal_handle_ctrt(ra8_usb_speed_t speed, uint16_t intsts0);

/**
 * @brief Drain a deferred control-OUT data stage and run chapter-9.
 * @details Defined in ux_dcd_ra8_usb_setup.c.
 * @param[in] speed Which controller fired (FS or HS).
 * @pre ::s_dcd is past ux_dcd_ra8_usb_initialize.
 * @pre Runs ahead of the CTRT status handling within the same IRQ.
 * @post On a drained packet, the pending flag is cleared and chapter-9 ran.
 * @post On a not-yet-landed bank, state is unchanged (retried next IRQ).
 * @note ISR-callback context; must not block past the bounded CFIFO wait.
 * @since 0.1.0
 */
RA8_PRIV void internal_handle_ctrl_out_data(ra8_usb_speed_t speed);

/**
 * @brief Process the DVSQ=Default branch of the DVST handler.
 * @details Defined in ux_dcd_ra8_usb_dvst_default.c; drains the
 * persistent SETUP mirrors, dispatches a fresh SETUP if USBREQ changed,
 * then re-arms the DCP via ra8_usb_device_busreset_rearm.
 * @param[in] speed Which controller fired.
 * @pre Caller confirmed dvsq == k_ra8_dvsq_default.
 * @pre ::s_dcd is past ux_dcd_ra8_usb_initialize.
 * @post DCP re-armed via ra8_usb_device_busreset_rearm.
 * @post ::s_busreset_rearm_count incremented.
 * @note ISR-callback context; must not block.
 * @since 0.1.0
 */
RA8_PRIV void internal_dvst_default_state(ra8_usb_speed_t speed);

/**
 * @brief Capture DVSTCTR0 / RHST history, mirror speed, dispatch DVST.
 * @details Defined in ux_dcd_ra8_usb_dvst.c.
 * @param[in] speed Which controller fired (FS or HS).
 * @param[in] intsts0 INTSTS0 snapshot (forwarded to internal_handle_dvst).
 * @pre Caller has already verified the DVST bit is set in intsts0.
 * @pre ::s_dcd is past ux_dcd_ra8_usb_initialize.
 * @post ::s_dvst_irq_count and the RHST history are updated.
 * @post Speed mirror updated and the DVST state-machine update has run.
 * @note ISR-only; must not block.
 * @since 0.1.0
 */
RA8_PRIV void internal_irq_dvst_prelude(ra8_usb_speed_t speed, uint16_t intsts0);

#ifdef __cplusplus
}
#endif
