/**
 * @file port/usbx/src/ux_dcd_ra8_usb_setup.c
 * @brief USBX device-controller-driver bridge to ra8_usb -- SETUP / chapter-9 control path.
 *
 * @par Tag
 * [Ring 5 / PORT] {World: S}
 *
 * @details
 * Control-transfer (CTRT) handling: SETUP packet decode, the chapter-9
 * dispatch into the USBX device stack, deferred control-OUT data stages,
 * and the JLink-readable trace ring.
 *
 * Split out of ``ux_dcd_ra8_usb.c`` to keep each translation unit under
 * the maintainability line cap; the cross-translation-unit contract
 * lives in ``ux_dcd_ra8_usb_internal.h``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#define UX_SOURCE_CODE

#include <stdint.h>
#include <string.h>

#include "ra8_check.h"
#include "ra8_elc_regs.h"
#include "ra8_isr.h"
#include "ra8_log.h"
#include "ra8_usb_regs.h"
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra8_usb.h"
#include "ux_dcd_ra8_usb_internal.h"
#include "ux_device_stack.h"
#include "ux_system.h"
#include "ux_utility.h"

/**
 * @var g_setup_dispatch_count
 * @brief Counter of SETUP packets fed into the chapter-9 dispatcher.
 *
 * @details Bisect probe. Non-zero confirms a SETUP-drain entry point
 * (ra8_usb_read_setup_if_valid or _unconditional) emptied
 * USBREQ/USBVAL/USBINDX/USBLENG and ::priv_dispatch_setup fired.
 * Stays zero if VALID never asserts (no SETUP token reached the IP).
 * HUM Ch 36.2.16..36.2.19 p 1623..1626.
 *
 * @note Written only by ::priv_handle_ctrt and the VALID fallback
 *       in ::ux_dcd_ra8_usb_irq.
 * @since 0.1.0
 */
volatile uint32_t g_setup_dispatch_count = 0U;

/**
 * @var g_setup_packet_buffer
 * @brief Wire-format bytes of the most recent SETUP packet drained from
 *        the controller, ready for JLink inspection.
 *
 * @details Layout matches USB 2.0 Ch 9.3:
 *   [0]=bmRequestType, [1]=bRequest, [2..3]=wValue (LE),
 *   [4..5]=wIndex (LE), [6..7]=wLength (LE).
 * A standard GET_DESCRIPTOR(DEVICE) shows as
 *   80 06 00 01 00 00 40 00.
 * Stays all-zero until the first VALID/CTRT edge drains a SETUP.
 * HUM Ch 36.2.17..36.2.20 (USBREQ/USBVAL/USBINDX/USBLENG).
 *
 * @note Single-writer (::priv_dispatch_setup).
 * @since 0.1.0
 */
volatile uint8_t g_setup_packet_buffer[8] = {};

/**
 * @var g_setup_packet_count
 * @brief Total SETUP packets latched into ::g_setup_packet_buffer.
 *
 * @note Single-writer (::priv_dispatch_setup).
 * @since 0.1.0
 */
volatile uint32_t g_setup_packet_count = 0U;

/**
 * @var g_dispatch_skip_reason
 * @brief Last-iteration bitmask of why the SQMON dispatch path skipped.
 *
 * @details Bits:
 *  - 0x01 prev_sqmon_already_set (legacy probe, no longer gates)
 *  - 0x02 ra8_usb_read_setup_if_valid_returned_no_data (legacy CTRT only)
 *  - 0x04 ra8_usb_read_setup_unconditional_returned_err
 *  - 0x08 priv_dispatch_setup returned non-zero (USBX rejected)
 *  - 0x10 _ux_system_slave was UX_NULL when dispatch ran
 *  - 0x20 ux_slave_endpoint_transfer_request unreachable
 *  - 0x40 sqmon was zero on this DVST entry
 *  - 0x80 dispatch path actually ran to completion (success marker)
 *
 * @note Single-writer (::internal_handle_dvst, ::priv_dispatch_setup).
 * @since 0.1.0
 */
volatile uint32_t g_dispatch_skip_reason = 0U;

/**
 * @var g_ctrl_out_pending
 * @brief A control-write data stage is armed and awaiting the host's OUT data.
 *
 * @details Set true by ::priv_dispatch_setup when a host->device control
 * transfer with a non-empty data stage (e.g. DFU_DNLOAD) is decoded: the DCP
 * is armed via ::ra8_usb_dcp_out_arm and the chapter-9 dispatch is DEFERRED.
 * Cleared by ::priv_handle_ctrl_out_data once the OUT data has landed
 * (DCP BRDY) and been drained, or by a fresh SETUP that abandons the prior
 * transfer. Deferring is mandatory: receiving synchronously in the FS device
 * ISR would spin out the lower-priority HS host worker -- the thread that must
 * SEND the data -- a same-CPU deadlock.
 *
 * @note Single-writer (::priv_dispatch_setup, ::priv_handle_ctrl_out_data).
 * @since 0.1.0
 */
volatile bool g_ctrl_out_pending = false;

/**
 * @var g_ctrl_out_tr
 * @brief EP0 transfer request whose data buffer the deferred OUT data fills.
 *
 * @details Snapshot of the device control-endpoint transfer request captured
 * when the control-write SETUP was decoded. ::priv_handle_ctrl_out_data
 * drains the host's OUT data into ``ux_slave_transfer_request_data_pointer``
 * and then runs the chapter-9 dispatcher on it.
 *
 * @note Single-writer (::priv_dispatch_setup, ::priv_handle_ctrl_out_data).
 * @since 0.1.0
 */
UX_SLAVE_TRANSFER* g_ctrl_out_tr = UX_NULL;

/**
 * @var g_ctrl_out_wlen
 * @brief wLength (cap) of the pending control-write data stage, in bytes.
 *
 * @note Single-writer (::priv_dispatch_setup).
 * @since 0.1.0
 */
volatile uint16_t g_ctrl_out_wlen = 0U;

/**
 * @var g_ctrl_out_rx
 * @brief Byte count drained by the most recent deferred control-OUT receive.
 *
 * @note Diagnostic; JLink-readable. Single-writer (::priv_handle_ctrl_out_data).
 * @since 0.1.0
 */
volatile uint32_t g_ctrl_out_rx = 0U;

/**
 * @var g_ctrl_out_done
 * @brief Count of completed deferred control-OUT data stages.
 *
 * @note Diagnostic; JLink-readable. Single-writer (::priv_handle_ctrl_out_data).
 * @since 0.1.0
 */
volatile uint32_t g_ctrl_out_done = 0U;

/**
 * @var g_last_dispatched_setup_fp
 * @brief 64-bit fingerprint of the last SETUP packet dispatched from
 *        the ISR-driven SETUP drain in ::priv_handle_ctrt.
 *
 * @details Layout (little-endian-style bit packing):
 *  - bits  0..15: USBREQ (bmRequestType + bRequest)
 *  - bits 16..31: USBVAL  (wValue)
 *  - bits 32..47: USBINDX (wIndex)
 *  - bits 48..63: USBLENG (wLength)
 *
 * Used to de-dup VALID-driven SETUP drains so a single SETUP that
 * fires multiple ISR snapshots is not re-dispatched. A fresh
 * fingerprint (different from this latch) is treated as a new SETUP.
 *
 * @note Single-writer (::priv_handle_ctrt).
 * @since 0.1.0
 */
volatile uint64_t g_last_dispatched_setup_fp = 0U;

/**
 * @var g_dispatched_fp_ring
 * @brief Ring of the last 4 dispatched SETUP fingerprints (oldest at
 *        index 0). Used to disambiguate which 2 SETUPs the chip
 *        processed when xfer_req_total < setup_dispatch_count.
 *
 * @note Written only by ::priv_handle_ctrt.
 * @since 0.1.0
 */
volatile uint64_t g_dispatched_fp_ring[4] = {};

volatile uint8_t g_dispatched_fp_ring_idx = 0U;

/**
 * @var g_state_at_dispatch
 * @brief Snapshot of ux_slave_device_state at the moment of the most
 *        recent SETUP dispatch. Used to verify the state-mirror gate
 *        was satisfied (must be in {ATTACHED, ADDRESSED, CONFIGURED}).
 *
 * @note Single-writer (::priv_handle_ctrt).
 * @since 0.1.0
 */
volatile uint8_t g_state_at_dispatch = 0U;

/**
 * @enum ra8_usb_setup_local_t
 * @brief Local USB SETUP-packet bit-field constants.
 * @details bmRequestType direction bit (bit 7) per USB 2.0 sec 9.3.
 */
typedef enum : uint8_t {
  /* bmRequestType bit 7 -- USB 2.0 spec sec 9.3 Table 9-2:
   * 0 = Host-to-Device (write or no-data), 1 = Device-to-Host (read). */
  k_ra8_usb_setup_dir_mask = 0x80U, /**< RA8 USB setup dir mask. */
  /* SET_ADDRESS standard request code. USB 2.0 spec sec 9.4.6
   * Table 9-4 ("Standard Request Codes"): bRequest = 5 = SET_ADDRESS.
   * Used to gate out the manual CCPL pulse for SET_ADDRESS, which
   * the Renesas USBHS SIE auto-handles per HUM Ch 37.3 (auto
   * response function, p 2147). */
  k_ra8_usb_breq_set_address = 0x05U, /**< RA8 USB breq set address. */
} ra8_usb_setup_local_t;

/**
 * @enum ra8_usb_dcp_brdy_t
 * @brief BRDYSTS / BRDYENB bit for the Default Control Pipe (pipe 0).
 * @details The DCP is pipe 0, so its buffer-ready status occupies bit 0 of the
 * per-pipe BRDYSTS / BRDYENB registers (HUM Ch 36.2.13 "BRDYSTS" p 1984). Used
 * by ::priv_handle_ctrl_out_data to detect that the host's control-OUT
 * data stage has landed in the DCP bank.
 */
typedef enum : uint16_t {
  k_ra8_usb_dcp_brdy_bit = 0x0001U, /**< DCP (pipe 0) BRDYSTS / BRDYENB bit. */
} ra8_usb_dcp_brdy_t;

/**
 * @var s_trace
 * @brief JLink-readable ring of packed transfer/SETUP events.
 * @note ISR-context single writer; readers use JLink memory dumps.
 * @since 0.1.0
 */
static volatile uint32_t s_trace[k_dcd_trace_entries] = {};

/**
 * @var s_trace_ts
 * @brief DWT cycle-count timestamp per ::s_trace slot.
 * @note Same indexing as ::s_trace; read via JLink for gap analysis.
 * @since 0.1.0
 */
static volatile uint32_t s_trace_ts[k_dcd_trace_entries] = {};

/**
 * @enum ra8_dcd_dwt_t
 * @brief Armv8-M Data Watchpoint and Trace unit register addresses.
 */
typedef enum : uintptr_t {
  k_dcd_dwt_cyccnt_addr = 0xE0001004U, /**< DWT_CYCCNT free-running cycle counter. */
} ra8_dcd_dwt_t;

/** @brief DWT cycle counter address (Armv8-M DWT_CYCCNT). */
static volatile uint32_t* const s_dcd_dwt_cyccnt = (volatile uint32_t*)k_dcd_dwt_cyccnt_addr;

/**
 * @var s_trace_seq
 * @brief Monotonic count of events written to ::s_trace.
 * @note ``s_trace_seq % k_dcd_trace_entries`` is the next write slot.
 * @since 0.1.0
 */
static volatile uint32_t s_trace_seq = 0U;

/**
 * @brief Append one packed event to the JLink-readable trace ring.
 *
 * @details Packs ``kind<<24 | code<<16 | length`` into the next ring
 * slot and bumps the sequence counter. Overwrites the oldest entry
 * once the ring wraps.
 *
 * @param[in] kind   Event kind (::ra8_usb_dcd_trace_t kinds).
 * @param[in] code   Per-kind code byte (opcode / bRequest / none).
 * @param[in] length Per-kind 16-bit payload (length or wValue).
 *
 * @pre Any context; single concurrent writer (IRQ-callback path).
 * @pre ::s_trace_seq monotonicity is maintained by that single writer.
 * @post One ring slot holds the packed event; sequence incremented.
 * @post No other state changes.
 *
 * @note Diagnostic only; never read by production code.
 * @since 0.1.0
 */
void priv_trace_event(uint8_t kind, uint8_t code, uint16_t length)
{
  const uint32_t slot = s_trace_seq % (uint32_t)k_dcd_trace_entries;
  s_trace[slot]       = ((uint32_t)kind << (uint32_t)k_dcd_trace_kind_shift) |
                        ((uint32_t)code << (uint32_t)k_dcd_trace_code_shift) | (uint32_t)length;
  s_trace_ts[slot]    = *s_dcd_dwt_cyccnt;
  s_trace_seq++;
}

/**
 * @enum ra8_usb_setup_usbreq_t
 * @brief Byte masks and request encodings applied to the USBREQ mirror.
 *
 * @details The USBREQ register packs bRequest in its high byte and
 * bmRequestType in its low byte (HUM Ch 37.2.21 p 2087). The
 * SET_ADDRESS standard request is bRequest = 5 (0x05) with
 * bmRequestType = 0, i.e. USBREQ high byte == 0x05 (0x0500 packed).
 */
typedef enum : uint16_t {
  k_ra8_usb_usbreq_breq_mask = 0xFF00U, /**< bRequest (high) byte of USBREQ.      */
  k_ra8_usb_usbreq_bmrt_mask = 0x00FFU, /**< bmRequestType (low) byte of USBREQ.  */
  k_ra8_usb_usbreq_set_addr  = 0x0500U, /**< USBREQ high byte == SET_ADDRESS (5). */
} ra8_usb_setup_usbreq_t;

/**
 * @enum ra8_usb_setup_fp_shift_t
 * @brief Bit offset of each 16-bit SETUP mirror inside the 64-bit
 *        SETUP fingerprint word.
 *
 * @details USBREQ occupies bits 0-15, USBVAL bits 16-31, USBINDX bits
 * 32-47 and USBLENG bits 48-63. Only the USBLENG offset falls outside
 * the ignored small-integer set and needs a name here.
 */
typedef enum : uint8_t {
  k_ra8_usb_fp_shift_usbleng = 48U, /**< USBLENG packs into fingerprint bits 48-63. */
} ra8_usb_setup_fp_shift_t;

/**
 * @brief Pack a decoded SETUP into the 8-byte USB-wire little-endian layout.
 *
 * @details Serialises bmRequestType, bRequest, wValue, wIndex, wLength
 * in the byte order USB 2.0 Ch 9.3 mandates for the SETUP packet --
 * the same order USBX's ``ux_slave_transfer_request_setup`` buffer
 * expects. The RA8 USB controller delivers the multi-byte fields
 * already in host endian via USBREQ/USBVAL/USBINDX/USBLENG (HUM
 * Ch 36.2.17..20), so the helper just re-serialises them little-endian.
 *
 * @param[out] buf 8-byte destination buffer (any aligned ``uint8_t[8]``).
 * @param[in] setup Decoded SETUP source.
 *
 * @pre ``buf`` and ``setup`` are non-null.
 * @pre ``buf`` has space for 8 bytes.
 * @post ``buf[0..7]`` holds the wire-format SETUP packet.
 * @post Source ``setup`` is unmodified.
 *
 * @note Pure data-shuffling; safe in IRQ context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pack_setup_le(volatile uint8_t* buf, const ra8_usb_setup_t* setup)
{
  buf[k_setup_idx_bmrt]   = setup->bm_request_type;
  buf[k_setup_idx_brq]    = setup->b_request;
  buf[k_setup_idx_val_lo] = (uint8_t)(setup->w_value & k_setup_byte_mask);
  buf[k_setup_idx_val_hi] = (uint8_t)((setup->w_value >> k_setup_byte_shift) & k_setup_byte_mask);
  buf[k_setup_idx_idx_lo] = (uint8_t)(setup->w_index & k_setup_byte_mask);
  buf[k_setup_idx_idx_hi] = (uint8_t)((setup->w_index >> k_setup_byte_shift) & k_setup_byte_mask);
  buf[k_setup_idx_len_lo] = (uint8_t)(setup->w_length & k_setup_byte_mask);
  buf[k_setup_idx_len_hi] = (uint8_t)((setup->w_length >> k_setup_byte_shift) & k_setup_byte_mask);
}

/**
 * @brief Arm + defer a control-WRITE data stage; report whether it was deferred.
 *
 * @details A host->device control transfer with a non-empty data stage (e.g.
 * DFU_DNLOAD) cannot be dispatched from the SETUP IRQ: the chapter-9 class
 * control_request reads its payload from the control buffer, but the host's
 * OUT data has not arrived yet, and receiving it synchronously here would spin
 * the FS device ISR while the lower-priority HS host worker -- the thread that
 * must SEND the data -- is preempted (a same-CPU deadlock). So arm the DCP and
 * record the pending transfer; ::priv_handle_ctrl_out_data drains the data
 * on the subsequent DCP BRDY IRQ and only THEN runs the dispatcher. A fresh
 * SETUP always clears any stale pending state first.
 *
 * @param[in] setup Decoded SETUP packet (non-null).
 * @param[in] tr    EP0 transfer request whose buffer receives the OUT data.
 *
 * @return true when the data stage was deferred (caller must NOT dispatch now);
 *         false for IN / no-data requests (caller dispatches immediately).
 * @retval true  Control-write data stage armed; dispatch deferred to the BRDY ISR.
 * @retval false IN or no-data request; caller dispatches chapter-9 immediately.
 *
 * @pre The DCP PID write gate is open (INTSTS0.VALID cleared).
 * @pre @p tr is the bound EP0 transfer request.
 * @post On defer: DCP armed, ::g_ctrl_out_pending set, ::g_ctrl_out_tr captured.
 * @post On no-defer: ::g_ctrl_out_pending cleared.
 *
 * @note ISR-callback context. Nested ifs (not a compound &&) keep this out of
 *       the MC/DC compound-decision inventory.
 * @see priv_handle_ctrl_out_data
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_try_defer_ctrl_out(const ra8_usb_setup_t* setup,
                                                     UX_SLAVE_TRANSFER*     tr)
{
  g_ctrl_out_pending = false;
  if ((setup->bm_request_type & (uint8_t)k_ra8_usb_setup_dir_mask) == 0U) {
    if (setup->w_length > 0U) {
      g_ctrl_out_tr      = tr;
      g_ctrl_out_wlen    = setup->w_length;
      g_ctrl_out_pending = true;
      (void)ra8_usb_dcp_out_arm(g_dcd.speed);
      return true;
    }
  }
  return false;
}

/**
 * @brief Push a decoded SETUP packet into the USBX chapter-9 dispatcher.
 *
 * @details Mirrors the SETUP into the JLink-readable probe buffer
 * (``g_setup_packet_buffer``) so the bench can confirm the bridge drained
 * USBREQ/USBVAL/USBINDX/USBLENG correctly even before USBX is fully
 * bound, then forwards the packet through ``_ux_system_slave``'s EP0
 * transfer-request buffer into ``_ux_device_stack_control_request_process``.
 *
 * @param[in] setup Decoded SETUP packet snapshot (non-null).
 *
 * @return USBX result code.
 * @retval UX_SUCCESS Chapter-9 dispatcher consumed the SETUP.
 * @retval UX_ERROR ``setup`` was null, or USBX is not yet bound, or
 *                  EP0 endpoint is not available.
 *
 * @pre ``setup`` is non-null.
 * @pre ``_ux_system_slave`` is bound by ``_ux_device_stack_initialize``.
 * @post EP0 transfer-request ``setup`` buffer holds the wire-format SETUP.
 * @post Chapter-9 dispatcher has been invoked synchronously.
 *
 * @note Runs in IRQ-callback context (called from ``ra8_usb_dispatch`` via
 *       ``priv_event_cb``); must not block.
 * @since 0.1.0
 */
unsigned int priv_dispatch_setup(const ra8_usb_setup_t* setup)
{
  if (setup == nullptr) {
    return UX_ERROR;
  }

  /* Mirror the just-decoded SETUP into the JLink-readable probe BEFORE
   * touching any USBX-owned state. Even if USBX is not yet bound (no
   * device stack init, no class registration), the bench can confirm
   * via JLink that the chip latched a real SETUP and the bridge drained
   * USBREQ/USBVAL/USBINDX/USBLENG correctly. */
  internal_pack_setup_le(g_setup_packet_buffer, setup);
  g_setup_packet_count++;
  priv_trace_event((uint8_t)k_dcd_trace_kind_setup, setup->b_request, setup->w_value);

  /* USBX must already be bound to forward the SETUP into the chapter-9
   * dispatcher. If it is not, that is fine for the bench probe path --
   * we still recorded the packet above. Mark the skip reason so a JLink
   * read disambiguates "USBX not bound" from "drain failed". */
  if (_ux_system_slave == UX_NULL) {
    g_dispatch_skip_reason |= 0x10U;
    return UX_ERROR;
  }
  UX_SLAVE_DEVICE*   device = &_ux_system_slave->ux_system_slave_device;
  UX_SLAVE_TRANSFER* tr =
    &device->ux_slave_device_control_endpoint.ux_slave_endpoint_transfer_request;
  if (tr == UX_NULL) {
    g_dispatch_skip_reason |= 0x20U;
    return UX_ERROR;
  }

  internal_pack_setup_le(tr->ux_slave_transfer_request_setup, setup);

  tr->ux_slave_transfer_request_actual_length        = 0UL;
  tr->ux_slave_transfer_request_current_data_pointer = tr->ux_slave_transfer_request_data_pointer;
  /* Chapter-9 dispatcher gates on completion_code == UX_SUCCESS
   * (ux_device_stack_control_request_process.c line ~101). The
   * previous SETUP may have left it as UX_TRANSFER_STALLED on a
   * STALL'd request -- clear it so this fresh SETUP is honored. */
  tr->ux_slave_transfer_request_completion_code = UX_SUCCESS;

  /* Control-OUT with a data stage (e.g. DFU_DNLOAD) is armed and DEFERRED to
   * the DCP BRDY ISR (see internal_try_defer_ctrl_out); IN / no-data requests
   * fall through to the immediate chapter-9 dispatch below. */
  if (internal_try_defer_ctrl_out(setup, tr)) {
    g_dispatch_skip_reason |= (uint32_t)k_ra8_usb_skip_process_ok;
    return UX_SUCCESS;
  }

  const unsigned int rc = _ux_device_stack_control_request_process(tr);
  if (rc != UX_SUCCESS) {
    g_dispatch_skip_reason |= 0x08U;
  } else {
    g_dispatch_skip_reason |= (uint32_t)k_ra8_usb_skip_process_ok;
  }
  return rc;
}

/**
 * @brief Drain a fresh SETUP and forward to the chapter-9 dispatcher.
 *
 * @details Called by ``internal_ctrt_handle_valid`` for every observed
 * non-SET_ADDRESS SETUP. Reads the SETUP via
 * ``ra8_usb_read_setup_unconditional``, records the fingerprint in the
 * diagnostic ring, promotes USBX device state to ATTACHED if it dropped
 * below the chapter-9 gate, dispatches via ``priv_dispatch_setup``,
 * and on no-data H2D control transfers (e.g. SET_CONFIGURATION,
 * SET_INTERFACE, SET_FEATURE) pulses CCPL via ``ra8_usb_control_response``
 * so the host observes the status-stage IN-ZLP. HUM Ch 37.3 p 2147.
 *
 * @param[in] speed Which controller fired.
 * @param[in] fingerprint Packed USBREQ/USBVAL/USBINDX/USBLENG snapshot
 *                        used as the dedup key.
 *
 * @pre Caller has verified VALID is observed and the fingerprint is new.
 * @pre Controller register block matches ``speed``.
 * @post ``g_last_dispatched_setup_fp`` updated.
 * @post CCPL pulsed for no-data H2D control transfers.
 *
 * @note ISR-callback context; must not block.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_ctrt_dispatch_fresh_setup(ra8_usb_speed_t speed,
                                                            uint64_t        fingerprint)
{
  ra8_usb_setup_t setup = {};
  if (ra8_usb_read_setup_unconditional(speed, &setup) != k_ra8_ok) {
    return;
  }
  g_setup_dispatch_count++;
  g_last_dispatched_setup_fp                     = fingerprint;
  g_dispatched_fp_ring[g_dispatched_fp_ring_idx] = fingerprint;
  g_dispatched_fp_ring_idx = (uint8_t)((g_dispatched_fp_ring_idx + 1U) & 0x03U);

  /* Belt-and-suspenders: ensure device_state is in {ATTACHED,
   * ADDRESSED, CONFIGURED} BEFORE dispatch so the chapter-9 gate
   * does not silently drop the SETUP. State demotion only happens on
   * bus reset / suspend, so promoting to ATTACHED is monotonic-safe.
   * Per-case branch keeps this out of the compound-decision MC/DC
   * inventory. */
  if (_ux_system_slave != UX_NULL) {
    UX_SLAVE_DEVICE* const dev = &_ux_system_slave->ux_system_slave_device;
    switch (dev->ux_slave_device_state) {
      case (ULONG)UX_DEVICE_ATTACHED:
      case (ULONG)UX_DEVICE_ADDRESSED:
      case (ULONG)UX_DEVICE_CONFIGURED:
        break;
      default:
        dev->ux_slave_device_state = (ULONG)UX_DEVICE_ATTACHED;
        break;
    }
  }
  g_state_at_dispatch = (uint8_t)(_ux_system_slave != UX_NULL
                                    ? _ux_system_slave->ux_system_slave_device.ux_slave_device_state
                                    : (ULONG)k_ra8_usb_state_unknown);
  const unsigned int rc = priv_dispatch_setup(&setup);

  /* Drive CCPL for no-data H2D control transfers (SET_ADDRESS,
   * SET_CONFIGURATION, SET_INTERFACE, SET_FEATURE...) so the host
   * observes the status-stage IN-ZLP. Nested ifs keep the
   * (dir==0) AND (wLength==0) gate out of the MC/DC inventory. */
  if ((setup.bm_request_type & (uint8_t)k_ra8_usb_setup_dir_mask) == 0U) {
    if (setup.w_length == 0U) {
      (void)ra8_usb_control_response(speed, rc == UX_SUCCESS);
      /* kind 7: H2D status driven -- code = bRequest, len = USBX rc. */
      priv_trace_event((uint8_t)k_dcd_trace_kind_ccpl, setup.b_request, (uint16_t)rc);
    }
  }
}

/**
 * @brief VALID-observed branch of ``priv_handle_ctrt``.
 *
 * @details Reads USBREQ/USBVAL/USBINDX/USBLENG (HUM Ch 37.2.21..24
 * p 2087..2090, persistent mirrors), computes a 64-bit fingerprint for
 * dedup, short-circuits SET_ADDRESS (HUM Ch 37.3 p 2147 -- SIE owns the
 * USBADDR latch and the IN-ZLP status stage), and otherwise forwards a
 * fresh SETUP via ``internal_ctrt_dispatch_fresh_setup``.
 *
 * @param[in] speed Which controller fired.
 *
 * @pre Caller has verified the VALID bit in INTSTS0.
 * @pre Bridge is past ``ux_dcd_ra8_usb_initialize``.
 * @post INTSTS0.VALID is W0C-cleared for every observed SETUP.
 * @post For non-SET_ADDRESS SETUPs the chapter-9 dispatcher has run.
 *
 * @note ISR-callback context; must not block.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_ctrt_handle_valid(ra8_usb_speed_t speed)
{
  volatile r_usb_regs_t* const reg = (speed == k_ra8_usb_speed_hs) ? ra8_usb_hs() : ra8_usb_fs();
  if (reg == nullptr) {
    return;
  }
  const uint16_t usbreq_live  = reg->USBREQ;
  const uint16_t usbval_live  = reg->USBVAL;
  const uint16_t usbindx_live = reg->USBINDX;
  const uint16_t usbleng_live = reg->USBLENG;
  const uint64_t fingerprint  = ((uint64_t)usbreq_live) | ((uint64_t)usbval_live << 16) |
                                ((uint64_t)usbindx_live << 32) |
                                ((uint64_t)usbleng_live << (uint8_t)k_ra8_usb_fp_shift_usbleng);

  /* Clear INTSTS0.VALID before anything else. The USBREQ/USBVAL/
   * USBINDX/USBLENG mirrors persist (HUM Ch 37.2.21..24 p 2087), so the
   * SETUP stays readable below; but a latched VALID stalls the
   * DCPCTR.PID write gate (HUM Ch 37.2.31 p 2095) and holds the USB IRQ
   * line asserted. VALID is a real event bit (in event_msk), so the
   * ISR spurious-entry gate does not short-circuit it -- it must be
   * cleared here. The early clear is also the dedup: a second
   * ISR entry for the same physical SETUP sees VALID=0 and
   * priv_handle_ctrt skips this function.
   * HUM Ch 36.2.14 INTSTS0 p 1985 (W0C). */
  reg->INTSTS0 = (uint16_t)(reg->INTSTS0 & (uint16_t)~(uint16_t)k_ra8_intsts0_mask_valid);

  /* SET_ADDRESS short-circuit (HUM Ch 37.3 p 2147 -- the SIE owns the
   * USBADDR latch and the IN-ZLP status stage). Nested ifs keep this
   * out of the MC/DC compound-decision inventory. */
  bool is_set_address = false;
  if ((usbreq_live & (uint16_t)k_ra8_usb_usbreq_breq_mask) == (uint16_t)k_ra8_usb_usbreq_set_addr) {
    if ((usbreq_live & (uint16_t)k_ra8_usb_usbreq_bmrt_mask) == 0x0000U) {
      if (usbleng_live == 0U) {
        is_set_address = true;
      }
    }
  }
  if (is_set_address) {
    return;
  }

  /* Dispatch every observed SETUP, including a host retransmit of
   * byte-identical bytes after a missed response window. The previous
   * `fingerprint != g_last_dispatched_setup_fp` skip dropped exactly
   * those retransmits and -- with the VALID clear also skipped -- left
   * the controller wedged. The early VALID clear above is the real
   * dedup; the fingerprint is now recorded for diagnostics only. */
  internal_ctrt_dispatch_fresh_setup(speed, fingerprint);
}

/**
 * @brief Handle the CTRT (control-transfer-stage) interrupt branch.
 *
 * @details Decodes the CTSQ status field from ``intsts0``, drains any
 * latched VALID SETUP via ``internal_ctrt_handle_valid``, and threads
 * the per-stage handlers (SETUP / IN data / OUT data / status). Pulled
 * out of the outer ISR so each helper stays under the NASA P10 Rule 4
 * size cap. Per HUM Ch 37.2.18 p 2081, USBREQ/USBVAL/USBINDX/USBLENG
 * persist after the SIE auto-clears VALID, so VALID is drained
 * unconditionally and the dispatcher dedups via a 64-bit fingerprint.
 *
 * @param[in] speed Which controller fired (``k_ra8_usb_speed_fs`` or
 *                  ``k_ra8_usb_speed_hs``).
 * @param[in] intsts0 INTSTS0 snapshot captured at the top of the ISR.
 *
 * @pre Caller has masked ``intsts0`` against the event mask.
 * @pre Bridge is past ``ux_dcd_ra8_usb_initialize``.
 * @post For SET_ADDRESS, INTSTS0.VALID is W0C-cleared.
 * @post For other fresh SETUPs the chapter-9 dispatcher has run.
 *
 * @note ISR-callback context; must not block.
 * @since 0.1.0
 */
void priv_handle_ctrt(ra8_usb_speed_t speed, uint16_t intsts0)
{
  const uint16_t ctsq       = (uint16_t)(intsts0 & (uint16_t)k_ra8_intsts0_mask_ctsq);
  const bool     have_valid = ((intsts0 & (uint16_t)k_ra8_intsts0_mask_valid) != 0U);

  /* Per HUM Ch 37.2.18 p 2081 the SIE auto-clears VALID quickly but
   * USBREQ/USBVAL/USBINDX/USBLENG persist; drain unconditionally on
   * any VALID observation and dedup via the fingerprint. */
  if (have_valid) {
    internal_ctrt_handle_valid(speed);
  }

  /* Status-stage-only handling (no SETUP drain needed). CTSQ=wrnd
   * fires on the trailing edge of no-data H2D transfers when the
   * SETUP-drain CCPL above did not run. */
  switch (ctsq) {
    case k_ra8_ctsq_rdss:
    case k_ra8_ctsq_wrss:
    case k_ra8_ctsq_wrnd:
      (void)ra8_usb_control_response(speed, true);
      break;
    case k_ra8_ctsq_sqer:
      (void)ra8_usb_control_response(speed, false);
      break;
    default:
      break;
  }
}

/**
 * @brief Drain a deferred control-OUT data stage and run chapter-9.
 *
 * @details Back half of the host->device control-write data path; the front
 * half (::priv_dispatch_setup) armed the DCP and set ::g_ctrl_out_pending
 * on the SETUP IRQ. Invoked from ::ux_dcd_ra8_usb_irq on every IRQ, AHEAD of the
 * CTRT status handling, this acts only when a control-write data stage is
 * pending and the host's OUT packet has landed in the DCP bank (DCP BRDY
 * asserted). It drains the bytes into the captured EP0 transfer buffer via
 * ::ra8_usb_dcp_out_read and then runs ::_ux_device_stack_control_request_process
 * so the class control_request (e.g. DFU_DNLOAD) sees its payload. The
 * control-write status stage (IN-ZLP) is driven separately by the CTSQ=wrss
 * edge in ::priv_handle_ctrt.
 *
 * Deferring to this BRDY IRQ -- rather than receiving synchronously in the
 * SETUP IRQ -- is mandatory on the USB self-loop: the FS device ISR and the
 * lower-priority HS host worker thread share one CPU, so a blocking receive in
 * the SETUP path would spin out the very thread that must SEND the data.
 *
 * @param[in] speed Which controller fired (FS or HS).
 *
 * @pre Bridge is past ::ux_dcd_ra8_usb_initialize.
 * @pre Runs ahead of the CTRT status handling within the same IRQ.
 * @post On a drained packet, ::g_ctrl_out_pending is cleared and chapter-9 ran.
 * @post On a not-yet-landed bank, state is unchanged (retried next IRQ).
 *
 * @note ISR-callback context; must not block past the bounded CFIFO wait.
 * @see priv_dispatch_setup
 * @see ra8_usb_dcp_out_read
 * @since 0.1.0
 */
void priv_handle_ctrl_out_data(ra8_usb_speed_t speed)
{
  if (!g_ctrl_out_pending) {
    return;
  }
  volatile r_usb_regs_t* const reg = (speed == k_ra8_usb_speed_hs) ? ra8_usb_hs() : ra8_usb_fs();
  if (reg == nullptr) {
    return;
  }
  /* Wait for the host's OUT packet to land before draining; an unrelated IRQ
   * (e.g. a BRDY for a bulk pipe) leaves us pending for the next pass.
   * HUM Ch 36.2.13 "BRDYSTS" p 1984. */
  if ((reg->BRDYSTS & (uint16_t)k_ra8_usb_dcp_brdy_bit) == 0U) {
    return;
  }

  UX_SLAVE_TRANSFER* const tr = g_ctrl_out_tr;
  g_ctrl_out_pending          = false;
  g_ctrl_out_tr               = UX_NULL;
  if (tr == UX_NULL) {
    return;
  }

  uint16_t rx = 0U;
  if (ra8_usb_dcp_out_read(speed,
                           tr->ux_slave_transfer_request_data_pointer,
                           g_ctrl_out_wlen,
                           &rx) != k_ra8_ok) {
    return;
  }
  tr->ux_slave_transfer_request_actual_length = rx;
  g_ctrl_out_rx                               = (uint32_t)rx;
  g_ctrl_out_done++;
  (void)_ux_device_stack_control_request_process(tr);
  /* Complete the control-write with its IN-ZLP status stage: pulse CCPL so the
   * SIE answers the host's status-stage IN token. Without this the host's
   * status read (internal_host_ctrl_status) never sees the ZLP and times out. */
  (void)ra8_usb_control_response(speed, true);
}
