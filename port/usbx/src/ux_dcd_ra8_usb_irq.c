/**
 * @file port/usbx/src/ux_dcd_ra8_usb_irq.c
 * @brief USBX device-controller-driver bridge to ra8_usb -- IRQ pipe-walk.
 *
 * @par Tag
 * [Ring 5 / PORT] {World: S}
 *
 * @details
 * The ISR bottom-half ``ux_dcd_ra8_usb_irq`` and its per-pipe BRDY / BEMP
 * finalisers: IN streaming, OUT draining, auto-echo, and orphan-OUT
 * capture.
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
 * @var g_ctrt_irq_count
 * @brief Counter of INTSTS0.CTRT events seen by the dispatcher.
 *
 * @details Bisect probe. Non-zero after host SETUP means the chip is
 * reporting control-stage transitions; zero means SETUPs aren't being
 * latched (HS chirp / termination problem). HUM Ch 36.2.14 p 1985.
 *
 * @note Written only by ::ux_dcd_ra8_usb_irq.
 * @since 0.1.0
 */
volatile uint32_t g_ctrt_irq_count = 0U;

/**
 * @var g_intsts0_last_dispatch
 * @brief Most-recent INTSTS0 snapshot fed to ::ux_dcd_ra8_usb_irq.
 *
 * @details Live-debug probe. Read this from JLink to see the current
 * pending interrupt mask + DVSQ/CTSQ field state without halting the
 * polled worker. HUM Ch 36.2.14 p 1985.
 *
 * @note Written only by ::ux_dcd_ra8_usb_irq.
 * @since 0.1.0
 */
volatile uint16_t g_intsts0_last_dispatch = 0U;

/**
 * @enum ra8_usb_dcd_intsts0_hist_t
 * @brief Sizing for the INTSTS0 / CTSQ history rings.
 */
typedef enum : uint8_t {
  k_ra8_usb_dcd_intsts0_hist_n = 8U, /**< Slots in g_intsts0_snapshot/g_ctsq_history. */
} ra8_usb_dcd_intsts0_hist_t;

/**
 * @var g_intsts0_valid_count
 * @brief Number of dispatch ticks where INTSTS0.VALID was observed.
 *
 * @details Bisect probe. Distinguishes "controller never latched a
 * SETUP" (count == 0) from "controller latched but our CTRT edge was
 * lost" (count > 0 while ::g_ctrt_irq_count grows). HUM Ch 36.2.14
 * INTSTS0.VALID = bit 3, p 1985.
 *
 * @note Single-writer (::ux_dcd_ra8_usb_irq).
 * @since 0.1.0
 */
volatile uint32_t g_intsts0_valid_count = 0U;

/**
 * @var g_intsts0_ctrt_count
 * @brief Number of dispatch ticks where INTSTS0.CTRT was observed.
 *
 * @details Bisect probe. Identical to ::g_ctrt_irq_count but bumped on
 * the snapshot value before the CTRT branch decides whether to handle
 * the SETUP -- useful for detecting CTRT latches that hit a snapshot
 * but failed the handler's CTSQ decode. HUM Ch 36.2.14 p 1985.
 *
 * @note Single-writer (::ux_dcd_ra8_usb_irq).
 * @since 0.1.0
 */
volatile uint32_t g_intsts0_ctrt_count = 0U;

/**
 * @var g_intsts0_snapshot
 * @brief Ring buffer of the last 8 INTSTS0 values seen by the worker
 *        on ticks where any of CTRT/VALID/DVST were set.
 *
 * @details JLink-readable trace. Index of next write slot is
 * ``g_intsts0_snapshot_count % k_ra8_usb_dcd_intsts0_hist_n``. Only
 * "interesting" ticks are recorded so a noisy idle loop doesn't
 * scroll a real SETUP edge out of the ring. HUM Ch 36.2.14 p 1985.
 *
 * @note Single-writer (::ux_dcd_ra8_usb_irq).
 * @since 0.1.0
 */
volatile uint16_t g_intsts0_snapshot[(uint32_t)k_ra8_usb_dcd_intsts0_hist_n] = {};

/**
 * @var g_intsts0_snapshot_count
 * @brief Total interesting INTSTS0 snapshots seen; modulo
 *        ::k_ra8_usb_dcd_intsts0_hist_n is the next write slot.
 *
 * @note Single-writer (::ux_dcd_ra8_usb_irq).
 * @since 0.1.0
 */
volatile uint32_t g_intsts0_snapshot_count = 0U;

/**
 * @var g_ctsq_history
 * @brief Ring buffer of the last 8 INTSTS0.CTSQ[2:0] values observed
 *        with CTRT or VALID asserted.
 *
 * @details Encoding (HUM Ch 36.2.14 p 1985 / ra8_usb_ctsq_t):
 *   0=idle, 1=rdds, 2=rdss, 3=wrds, 4=wrss, 5=wrnd, 6=sqer.
 *
 * @note Single-writer (::ux_dcd_ra8_usb_irq).
 * @since 0.1.0
 */
volatile uint8_t g_ctsq_history[(uint32_t)k_ra8_usb_dcd_intsts0_hist_n] = {};

/**
 * @var g_intsts0_observed_or
 * @brief Bitwise OR of every INTSTS0 value ever fed to ::ux_dcd_ra8_usb_irq.
 *
 * @details Definitive bisect probe. Read this once via JLink and check
 * bit 3 (mask 0x0008 = VALID) -- if it's clear, the controller has
 * NEVER latched a SETUP token since power-on. Distinct from the
 * snapshot ring (which can scroll edges out of view) because OR-
 * accumulating bits is monotonic. HUM Ch 36.2.16 INTSTS0 p 1986
 * (VALID = bit 3, CTRT = bit 11, DVST = bit 12).
 *
 * @note Single-writer (::ux_dcd_ra8_usb_irq).
 * @since 0.1.0
 */
volatile uint16_t g_intsts0_observed_or = 0U;

/**
 * @var g_dvsq_history
 * @brief Per-snapshot DVSQ[2:0] field, pre-decoded for JLink readers.
 *
 * @details Companion to ::g_intsts0_snapshot. Slot ``i`` holds
 * ``(intsts0[i] >> 4) & 0x07``, i.e. 0=Powered, 1=Default, 2=Address,
 * 3=Configured, 4..7=Suspend (per HUM Ch 36.2.16 p 1986).
 * Saves the human reader from mentally decoding the raw bits.
 *
 * @note Single-writer (::ux_dcd_ra8_usb_irq).
 * @since 0.1.0
 */
volatile uint8_t g_dvsq_history[(uint32_t)k_ra8_usb_dcd_intsts0_hist_n] = {};

/* Auto-echo support: lets the bridge mirror data from one OUT pipe to one
 * IN pipe directly inside the ISR, without involving USBX worker threads.
 * Diagnostic counters expose what the path is doing; auto_echo_buf is
 * static and reused across BRDY events. Set ::g_dcd_auto_echo_enable to 1
 * via ::ux_dcd_ra8_usb_auto_echo_enable() to opt in. */
typedef enum : uint16_t {
  k_ux_dcd_ra8_usb_auto_echo_max = 512U, /**< Ux dcd RA8 USB auto echo maximum. */
} ux_dcd_ra8_usb_auto_echo_cfg_t;

volatile uint32_t g_dcd_auto_echo_drain_ok = 0U;

volatile uint32_t g_dcd_auto_echo_drain_skip = 0U;

volatile uint32_t g_dcd_auto_echo_tx_ok = 0U;

volatile uint32_t g_dcd_auto_echo_tx_err = 0U;

volatile uint16_t g_dcd_auto_echo_last_len = 0U;

static uint8_t s_dcd_auto_echo_buf[k_ux_dcd_ra8_usb_auto_echo_max];

/**
 * @brief Record per-bit IRQ counters and the JLink-visible event ring.
 *
 * @details Bumps ``g_intsts0_valid_count`` / ``g_intsts0_ctrt_count`` /
 * ``g_setup_token_observed`` then, when any event bit (CTRT / VALID /
 * DVST) is set, snapshots the wire-format INTSTS0 along with the
 * decoded CTSQ and DVSQ fields into the round-robin history ring (HUM
 * Ch 36.2.14 / Ch 36.2.16). Folded out of ``ux_dcd_ra8_usb_irq`` so the
 * outer ISR-side trampoline stays under the NASA P10 Rule 4 size cap.
 *
 * @param[in] intsts0 INTSTS0 snapshot forwarded by ``ra8_usb_dispatch``.
 *
 * @pre Bridge is past ``ux_dcd_ra8_usb_initialize``.
 * @pre Caller is on the ISR callback path.
 * @post Counters / history ring updated; no INTSTS0 W0C write performed.
 * @post No wire-side state mutated.
 *
 * @note ISR-only; must not block.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_irq_record_snapshot(uint16_t intsts0)
{
  const uint16_t ctrt_bit  = (uint16_t)(1U << (uint8_t)k_ra8_int0_bit_ctrt);
  const uint16_t dvst_bit  = (uint16_t)(1U << (uint8_t)k_ra8_int0_bit_dvst);
  const uint16_t valid_bit = (uint16_t)k_ra8_intsts0_mask_valid;

  if ((intsts0 & valid_bit) != 0U) {
    g_intsts0_valid_count++;
    /* VALID = "USB Request Reception Flag" (HUM Ch 37.2.18 p 2081);
     * fold into the canonical SETUP-arrival counter. */
    g_setup_token_observed++;
  }
  if ((intsts0 & ctrt_bit) != 0U) {
    g_intsts0_ctrt_count++;
  }
  const uint16_t interesting_mask = (uint16_t)(ctrt_bit | valid_bit | dvst_bit);
  if ((intsts0 & interesting_mask) != 0U) {
    const uint8_t slot =
      (uint8_t)(g_intsts0_snapshot_count % (uint32_t)k_ra8_usb_dcd_intsts0_hist_n);
    g_intsts0_snapshot[slot] = intsts0;
    g_ctsq_history[slot]     = (uint8_t)(intsts0 & (uint16_t)k_ra8_intsts0_mask_ctsq);
    /* DVSQ field is bits 6:4 (HUM Ch 36.2.16 p 1986). */
    g_dvsq_history[slot] =
      (uint8_t)((intsts0 & (uint16_t)k_ra8_intsts0_mask_dvsq) >> (uint8_t)k_ra8_int0_dvsq_shift);
    g_intsts0_snapshot_count++;
  }
}

/**
 * @brief No-waiter auto-echo drain on the configured OUT pipe.
 *
 * @details When the USBX-side waiter is absent (typically because the
 * worker thread has not yet been dispatched) and the operator has
 * enabled the in-ISR auto-echo path, drain the OUT pipe locally and
 * re-queue the bytes back on the IN pipe. Workaround for ThreadX
 * worker scheduling failure -- keeps echo functional without any
 * thread-mode involvement.
 *
 * THROUGHPUT NOTE.
 * On hardware this echo loop sustains ~2.66 MB/s one-way at HS and
 * ~360 KB/s at FS (measured via scripts/hil/usb/stream_bench.py against
 * Linux cdc_acm). That is ~66 % of the per-microframe scheduling
 * ceiling for HS (4 MB/s at 1 packet/125 us microframe) and ~35 % of
 * FS bulk wire. The bottleneck is NOT this code path -- it is the
 * host's cdc_acm URB-completion pipeline (MPS-sized read URBs, one
 * completion per packet, callback latency limits URB recycling). To
 * push past this ceiling needs either a libusb async client, a kernel
 * cdc_acm patch with bigger URBs, or a vendor-specific interface.
 *
 * @param[in] i Pipe index that just signalled BRDY/BEMP with no waiter.
 *
 * @pre Called from ISR context only.
 * @pre Caller already verified ``g_dcd.pipes[i].xfer == nullptr``.
 * @post On a successful drain, the IN pipe holds the echoed payload
 *       (with a trailing ZLP when ``len % MPS == 0``).
 * @post Counters under ``s_dcd_auto_echo_*`` are updated.
 *
 * @note ISR-only; must not block.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_irq_auto_echo(uint8_t i)
{
  if (g_dcd_auto_echo_enable != 0U && i == g_dcd_auto_echo_out_pipe &&
      g_dcd.pipes[i].dir_in == 0U) {
    uint16_t        len = (uint16_t)k_ux_dcd_ra8_usb_auto_echo_max;
    const ra8_err_t qo =
      ra8_usb_queue_out(g_dcd.speed, i, s_dcd_auto_echo_buf, &len, /* rearm */ true);
    if (qo == k_ra8_ok && len > 0U) {
      g_dcd_auto_echo_drain_ok++;
      g_dcd_auto_echo_last_len = len;
      const uint16_t out_mps   = g_dcd.pipes[i].max_pkt;
      if (ra8_usb_queue_in(g_dcd.speed, g_dcd_auto_echo_in_pipe, s_dcd_auto_echo_buf, len) ==
          k_ra8_ok) {
        g_dcd_auto_echo_tx_ok++;
        /* If the data packet was exactly MPS, follow with a ZLP so the
         * host URB completes; otherwise the host waits for a short
         * packet that never arrives. */
        if (out_mps != 0U && (len % out_mps) == 0U) {
          (void)ra8_usb_queue_in(g_dcd.speed, g_dcd_auto_echo_in_pipe, s_dcd_auto_echo_buf, 0U);
        }
      } else {
        g_dcd_auto_echo_tx_err++;
      }
    } else {
      g_dcd_auto_echo_drain_skip++;
      (void)ra8_usb_rearm_out_pipe(g_dcd.speed, i);
    }
  }
}

/**
 * @brief Stage the next IN chunk of a stashed transfer onto the pipe.
 *
 * @details Pushes the next MPS-bounded chunk at the running
 * ``actual_length`` offset via ::ra8_usb_queue_in and advances the
 * offset on success. A failure (typically an FRDY timeout while the
 * SIE holds the FIFO) is counted in ``g_diag.in_stage_fail`` and
 * leaves ``actual_length`` unchanged so the caller can retry the same
 * chunk on a later walk.
 *
 * @param[in,out] tr Stashed transfer being streamed.
 * @param[in]     i  Pipe index.
 * @return true when the chunk was queued.
 * @retval false ``ra8_usb_queue_in`` failed; nothing was consumed.
 *
 * @pre Called from ISR context only.
 * @pre ``tr->ux_slave_transfer_request_actual_length`` is < requested.
 * @post On success ``actual_length`` advanced by the staged chunk.
 * @post On failure ``g_diag.in_stage_fail`` is incremented.
 *
 * @note ISR-only; must not block.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_irq_stage_next_in(UX_SLAVE_TRANSFER* tr, uint8_t i)
{
  const uint16_t total = (uint16_t)tr->ux_slave_transfer_request_requested_length;
  const uint16_t sent  = (uint16_t)tr->ux_slave_transfer_request_actual_length;
  const uint16_t mps   = g_dcd.pipes[i].max_pkt;
  const uint16_t rem   = (uint16_t)(total - sent);
  const uint16_t chunk = (rem > mps) ? mps : rem;
  if (ra8_usb_queue_in(g_dcd.speed, i, &tr->ux_slave_transfer_request_data_pointer[sent], chunk) !=
      k_ra8_ok) {
    g_diag.in_stage_fail++;
    return false;
  }
  tr->ux_slave_transfer_request_actual_length = (ULONG)((uint32_t)sent + (uint32_t)chunk);
  return true;
}

/**
 * @brief Recover a stashed IN transfer whose pipe fell to PID=NAK.
 *
 * @details Strand recovery (seen against macOS hosts): a stashed IN
 * transfer must never sit with PIPECTR.PID = NAK -- the SIE then NAKs
 * every IN token, the host eventually suspends the bus, and the class
 * thread blocks on the semaphore forever. Two recoverable shapes:
 *  - a loaded bank (INBUFM set) whose PID=BUF arm was lost: re-arm it;
 *  - an empty bank with no BEMP pending mid-stream: the previous
 *    staging attempt failed; re-stage the current chunk.
 * HUM Ch 36.2.27 PIPEnCTR (PID / INBUFM).
 *
 * @param[in,out] tr  Stashed transfer being streamed.
 * @param[in]     i   Pipe index.
 * @param[in]     reg Controller register window.
 * @return true when a recovery action was taken (caller returns).
 * @retval false The pipe is armed normally; continue the walk.
 *
 * @pre Called from ISR context only.
 * @pre ``g_dcd.pipes[i].xfer == tr``.
 * @post On recovery the pipe is re-armed or the chunk re-staged.
 * @post ``g_diag`` counters record which recovery fired.
 *
 * @note ISR-only; must not block.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_irq_recover_in_nak(UX_SLAVE_TRANSFER* tr, uint8_t i, volatile r_usb_regs_t* reg)
{
  const uint16_t pipe_bit = (uint16_t)(1U << i);
  const uint16_t total    = (uint16_t)tr->ux_slave_transfer_request_requested_length;
  const uint16_t sent     = (uint16_t)tr->ux_slave_transfer_request_actual_length;
  const uint16_t ctr      = reg->PIPECTR[i - 1U];
  if ((ctr & (uint16_t)k_ra8_pipectr_pid_mask) != (uint16_t)k_ra8_pid_nak) {
    return false;
  }
  if ((ctr & (uint16_t)k_ra8_pipectr_inbufm) != 0U) {
    g_diag.in_rearm_nak++;
    (void)ra8_usb_rearm_out_pipe(g_dcd.speed, i);
    return true; /* BEMP follows once the re-armed bank drains */
  }
  if ((reg->BEMPSTS & pipe_bit) == 0U) {
    if (sent < total) {
      g_diag.in_restage_nak++;
      (void)internal_irq_stage_next_in(tr, i);
      return true;
    }
  }
  return false;
}

/**
 * @brief Complete a stashed IN-pipe transfer.
 *
 * @details IN: data was already pushed in TRANSFER_REQUEST. Mark
 * complete on the BEMP that follows, and W0C-ack the per-pipe BEMPSTS
 * bit so the global INTSTS0.BEMP flag can de-assert -- without this
 * BEMPSTS stays set forever, the global BEMP re-asserts after every
 * dispatch ack, and the ISR storm prevents PendSV from delivering
 * completions to the worker. HUM Ch 36.2.15 BEMPSTS (W0C, write
 * ``~pipe_bit`` to clear only this pipe).
 *
 * @param[in,out] tr Stashed transfer to complete.
 * @param[in]     i  Pipe index.
 *
 * @pre Called from ISR context only.
 * @pre ``g_dcd.pipes[i].xfer == tr``.
 * @post ``g_dcd.pipes[i].xfer == nullptr``.
 * @post Per-pipe BEMPSTS bit is cleared and the waiter is woken (non-standalone).
 *
 * @note ISR-only; must not block.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_irq_complete_in(UX_SLAVE_TRANSFER* tr, uint8_t i)
{
  volatile r_usb_regs_t* reg = (g_dcd.speed == k_ra8_usb_speed_hs) ? ra8_usb_hs() : ra8_usb_fs();
  if (reg == nullptr) { /* GCOVR_EXCL_BR_LINE -- speed always maps   */
    return;             /* GCOVR_EXCL_LINE -- valid speed proves reg */
  }
  const uint16_t pipe_bit = (uint16_t)(1U << i);
  const uint16_t total    = (uint16_t)tr->ux_slave_transfer_request_requested_length;
  const uint16_t sent     = (uint16_t)tr->ux_slave_transfer_request_actual_length;

  if (internal_irq_recover_in_nak(tr, i, reg)) {
    return;
  }

  /* Wait for BEMPSTS: it confirms the host has drained the bank just
   * transmitted. Acting before that would let the next BOT stage (or
   * the next data packet) be pushed over data the host has not read --
   * a race that corrupts the transport and surfaces host-side as
   * DID_ERROR. If BEMP has not fired, leave the transfer stashed and
   * retry on the next IRQ walk. HUM Ch 36.2.15 BEMPSTS (W0C). */
  if ((reg->BEMPSTS & pipe_bit) == 0U) {
    return;
  }

  /* Multi-packet streaming. ra8_usb_queue_in moves at most one MPS
   * bank, so a transfer longer than MPS is sent one bank per host
   * drain: push the next bank until the whole requested length is
   * out, only then post completion. ``actual_length`` is the running
   * queued-byte count seeded by internal_submit_pipe. The BEMPSTS ack
   * is transactional: it lands only after the next chunk is staged, so
   * a staging failure keeps the event pending and retries on the next
   * walk instead of stranding the transfer. */
  if (sent < total) {
    if (internal_irq_stage_next_in(tr, i)) {
      reg->BEMPSTS = (uint16_t)(~pipe_bit);
    }
    return;
  }
  reg->BEMPSTS = (uint16_t)(~pipe_bit);

  /* Multi-packet trailing ZLP: stage it only when USBX's force_zlp
   * flag asks for one (host expects more than the MPS-aligned
   * payload), and complete the transfer on the ZLP's own BEMP. BOT
   * data phases never set the flag (the class sends exactly the
   * host-requested length), so the CSW always follows the data. */
  if (tr->ux_slave_transfer_request_force_zlp == (ULONG)UX_TRUE) {
    tr->ux_slave_transfer_request_force_zlp = (ULONG)UX_FALSE;
    (void)ra8_usb_queue_in(g_dcd.speed, i, tr->ux_slave_transfer_request_data_pointer, 0U);
    return;
  }
  priv_trace_event((uint8_t)k_dcd_trace_kind_in, (uint8_t)k_dcd_trace_no_code, total);
  tr->ux_slave_transfer_request_completion_code = UX_SUCCESS;
  g_dcd.pipes[i].xfer                           = nullptr;
#ifndef UX_DEVICE_STANDALONE
  (void)tx_semaphore_put(&tr->ux_slave_transfer_request_semaphore);
#endif
}

/**
 * @brief Finish a completed OUT transfer: park, trace, wake the waiter.
 *
 * @details Parks the pipe at PID=NAK (a host OUT token landing before
 * the class layer arms the next receiver is then NAK'd rather than
 * ACK'd into a FIFO with no waiter -- the latter latches BRDYSTS and
 * storms the ISR, GitHub issue #6), records the transfer in the trace
 * ring (with the SCSI opcode + CDB bytes 1..2 when it is a 31-byte
 * CBW), posts UX_SUCCESS, and wakes the waiter.
 *
 * @param[in,out] tr  Stashed transfer that just completed.
 * @param[in]     i   Pipe index.
 * @param[in]     now Total bytes received for this transfer.
 *
 * @pre Called from ISR context only.
 * @pre ``g_dcd.pipes[i].xfer == tr``.
 * @post ``g_dcd.pipes[i].xfer == nullptr`` and the waiter is woken.
 * @post The pipe is parked at PID=NAK.
 *
 * @note ISR-only; must not block.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_irq_finish_out(UX_SLAVE_TRANSFER* tr, uint8_t i, uint16_t now)
{
  (void)ra8_usb_park_out_pipe(g_dcd.speed, i);
  uint8_t  trace_op  = (uint8_t)k_dcd_trace_no_code;
  uint16_t trace_len = now;
  if (now == (uint16_t)k_dcd_trace_cbw_len) {
    const uint8_t* cdb = &tr->ux_slave_transfer_request_data_pointer[k_dcd_trace_cbw_op_off];
    trace_op           = cdb[0];
    /* For CBWs, carry CDB bytes 1..2 instead of the length so VPD
     * INQUIRY requests (EVPD flag + page code) are visible. For
     * READ(10)/WRITE(10) carry the low 16 LBA bits instead. */
    uint8_t hi = cdb[1];
    uint8_t lo = cdb[2];
    if (cdb[0] == (uint8_t)k_dcd_trace_op_read10) {
      hi = cdb[k_dcd_trace_cdb_lba_hi];
      lo = cdb[k_dcd_trace_cdb_lba_lo];
    }
    if (cdb[0] == (uint8_t)k_dcd_trace_op_write10) {
      hi = cdb[k_dcd_trace_cdb_lba_hi];
      lo = cdb[k_dcd_trace_cdb_lba_lo];
    }
    trace_len = (uint16_t)(((uint16_t)hi << (uint16_t)k_dcd_trace_byte_shift) | (uint16_t)lo);
  }
  priv_trace_event((uint8_t)k_dcd_trace_kind_out, trace_op, trace_len);
  tr->ux_slave_transfer_request_completion_code = UX_SUCCESS;
  g_dcd.pipes[i].xfer                           = nullptr;
#ifndef UX_DEVICE_STANDALONE
  (void)tx_semaphore_put(&tr->ux_slave_transfer_request_semaphore);
#endif
}

/**
 * @brief Complete a stashed OUT-pipe transfer (or rearm on no-data).
 *
 * @details Drain the OUT pipe into the stashed buffer. On success post
 * UX_SUCCESS and wake the waiter; on ``k_ra8_err_no_data`` proactively
 * W0C-ack NRDYSTS for this pipe and force PIPECTR.PID = BUF so the
 * next host OUT is ACKed. HUM Ch 36.2.13 NRDYSTS + Ch 36.2.27 PIPECTR.
 *
 * @param[in,out] tr Stashed transfer to complete.
 * @param[in]     i  Pipe index.
 *
 * @pre Called from ISR context only.
 * @pre ``g_dcd.pipes[i].xfer == tr``.
 * @post On success ``g_dcd.pipes[i].xfer == nullptr`` and the waiter is woken.
 * @post On no-data the OUT pipe is rearmed; no semaphore wake.
 *
 * @note ISR-only; must not block.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_irq_complete_out(UX_SLAVE_TRANSFER* tr, uint8_t i)
{
  /* Multi-packet streaming. ra8_usb_queue_out drains at most one MPS
   * bank, so an OUT transfer longer than MPS (e.g. the 512-byte SCSI
   * WRITE data phase) is received one bank per host packet:
   * accumulate into the caller buffer at the running offset, re-arm
   * the pipe for the next bank, and complete only once the whole
   * requested length is in -- or the host ends early with a short
   * (< MPS) packet, which terminates the OUT data phase (this is also
   * how a 31-byte CBW into a 64-byte request completes). */
  const uint16_t total = (uint16_t)tr->ux_slave_transfer_request_requested_length;
  const uint16_t mps   = g_dcd.pipes[i].max_pkt;
  /* Drain every OUT bank the controller currently holds (DBLB presents
   * up to two back-to-back) before re-arming, so a single BRDY services
   * as many banks as are ready. One MPS bank per ra8_usb_queue_out call;
   * stop on completion, a short packet, or when no bank is ready. */
  for (uint32_t pass = 0U; pass < (uint32_t)k_dcd_out_drain_max; pass++) {
    const uint16_t got  = (uint16_t)tr->ux_slave_transfer_request_actual_length;
    uint16_t       want = (uint16_t)(total - got);
    if (want > mps) {
      want = mps;
    }
    uint16_t        len    = want;
    const ra8_err_t qo_err = ra8_usb_queue_out(g_dcd.speed,
                                               i,
                                               &tr->ux_slave_transfer_request_data_pointer[got],
                                               &len,
                                               /* rearm */ false);
    if (qo_err != k_ra8_ok) {
      if (i == 2U) {
        g_diag.irq_walk_pipe2_no_data++;
      }
      break;
    }
    if (i == 2U) {
      g_diag.irq_walk_pipe2_complete++;
    }
    const uint16_t now                          = (uint16_t)(got + len);
    tr->ux_slave_transfer_request_actual_length = (ULONG)now;
    if (now >= total) {
      internal_irq_finish_out(tr, i, now);
      return;
    }
    if (len < mps) {
      internal_irq_finish_out(tr, i, now); /* short packet ends the phase */
      return;
    }
  }
  /* More packets expected -- re-arm for the next host OUT token(s). */
  (void)ra8_usb_rearm_out_pipe(g_dcd.speed, i);
}

/**
 * @brief Capture a no-receiver bulk-OUT packet into the orphan buffer.
 *
 * @details Run from the IRQ pipe-walk for an OUT pipe that has no USBX
 * transfer stashed. If a host packet has landed in the controller bank
 * (BRDYSTS set), draining it via ::ra8_usb_queue_out both W0C-clears
 * BRDYSTS -- so the no-receiver BRDY interrupt cannot storm the CPU and
 * starve thread mode (GitHub issue #6) -- and preserves the packet in
 * ::g_orphan_buf for ::internal_submit_pipe to hand to the next
 * bulk-OUT transfer. The pipe is then parked at PID=NAK so no second
 * packet can land. Skipped for IN pipes, unconfigured pipes, the CDC
 * auto-echo pipe, and while the one-deep buffer is already occupied.
 *
 * @param[in] i Pipe index (1..max_pipes-1).
 *
 * @pre Caller is on the ISR callback path; ``g_dcd.pipes[i].xfer`` is NULL.
 * @pre Bridge is past ``ux_dcd_ra8_usb_initialize``.
 * @post On a captured packet ::g_orphan_len / ::g_orphan_pipe hold it,
 *       BRDYSTS for pipe ``i`` is cleared, and the pipe is parked at NAK.
 * @post Otherwise no state changes (fast BRDYSTS-clear early return).
 *
 * @note ISR-only; must not block.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_irq_drain_orphan_out(uint8_t i)
{
  if (g_dcd.pipes[i].dir_in != 0U) {
    return; /* IN pipe -- the host does not write to it */
  }
  if (g_dcd.pipes[i].ep_addr == 0U) {
    return; /* pipe not configured by the class layer */
  }
  if (g_dcd_auto_echo_enable != 0U) {
    if (i == g_dcd_auto_echo_out_pipe) {
      return; /* CDC auto-echo drains this pipe itself */
    }
  }
  if (g_orphan_len != 0U) {
    return; /* one-deep holding buffer already occupied */
  }
  uint16_t len = (uint16_t)k_ra8_usb_orphan_bytes;
  if (ra8_usb_queue_out(g_dcd.speed, i, g_orphan_buf, &len, /* rearm */ false) == k_ra8_ok) {
    if (len != 0U) {
      g_orphan_pipe = i;
      g_orphan_len  = len;
      priv_trace_event((uint8_t)k_dcd_trace_kind_ocap, g_orphan_buf[0], len);
      /* queue_out W0C-cleared BRDYSTS and re-armed PID=BUF; pull it
       * back to NAK so a second packet cannot land before the next
       * transfer is submitted. A full-speed bulk packet takes ~45 us
       * on the wire -- far longer than this queue_out -> park gap --
       * so the controller cannot accept one in between. */
      (void)ra8_usb_park_out_pipe(g_dcd.speed, i);
    }
  }
}

/**
 * @brief Per-pipe finaliser for the BRDY/BEMP walk in ::ux_dcd_ra8_usb_irq.
 *
 * @details If no USBX-side waiter is stashed, dispatch to the in-ISR
 * auto-echo path (workaround for ThreadX worker scheduling failure)
 * and the no-receiver orphan-OUT capture. Otherwise dispatch to
 * ::internal_irq_complete_in or ::internal_irq_complete_out depending
 * on the pipe direction.
 *
 * @param[in] i Pipe index (1..max_pipes-1).
 *
 * @pre Bridge is past ``ux_dcd_ra8_usb_initialize``.
 * @pre Caller is on the ISR callback path.
 *
 * @post One of: a transfer was completed and the semaphore was put;
 *       the OUT pipe was rearmed on no-data; or the auto-echo loop
 *       drained the OUT pipe and re-queued on the IN pipe.
 * @post ``g_diag.irq_walk_total`` is incremented.
 *
 * @note ISR-only; must not block.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_irq_walk_pipe(uint8_t i)
{
  g_diag.irq_walk_total++;
  UX_SLAVE_TRANSFER* tr = g_dcd.pipes[i].xfer;
  if (tr == nullptr) {
    internal_irq_auto_echo(i);
    internal_irq_drain_orphan_out(i);
    return;
  }
  if (i == 2U) {
    g_diag.irq_walk_pipe2_seen++;
  }
  if (g_dcd.pipes[i].dir_in != 0U) {
    internal_irq_complete_in(tr, i);
    return;
  }
  internal_irq_complete_out(tr, i);
}

/**
 * @brief Ux dcd ra usb irq.
 *
 * @details See implementation for details.
 *
 * @param[in,out] speed See function signature for type and usage.
 * @param[in,out] intsts0 See function signature for type and usage.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
void ux_dcd_ra8_usb_irq(ra8_usb_speed_t speed, uint16_t intsts0)
{
  if (g_dcd.state == k_ux_dcd_ra8_usb_state_uninit) {
    return;
  }

  g_intsts0_last_dispatch = intsts0;
  g_intsts0_observed_or   = (uint16_t)(g_intsts0_observed_or | intsts0);

  internal_irq_record_snapshot(intsts0);

  const uint16_t ctrt_bit   = (uint16_t)(1U << (uint8_t)k_ra8_int0_bit_ctrt);
  const uint16_t dvst_bit   = (uint16_t)(1U << (uint8_t)k_ra8_int0_bit_dvst);
  const uint16_t valid_bit  = (uint16_t)k_ra8_intsts0_mask_valid;
  const bool     have_ctrt  = (intsts0 & ctrt_bit) != 0U;
  const bool     have_valid = (intsts0 & valid_bit) != 0U;
  const bool     have_dvst  = (intsts0 & dvst_bit) != 0U;

  /* DVST FIRST -- bus reset / suspend / resume must be processed
   * BEFORE the SETUP drain. internal_handle_dvst calls
   * ra8_usb_device_busreset_rearm on Default-state which RESETS
   * DCPCFG/DCPMAXP/DCPCTR. Draining first would have the response
   * wiped, leaving the chip NAK'ing host IN tokens. */
  if (have_dvst) {
    priv_irq_dvst_prelude(speed, intsts0);
  }

  /* Deferred control-OUT data stage (e.g. DFU_DNLOAD) BEFORE the CTRT block:
   * if the host's OUT data has landed in the DCP bank, drain it and run
   * chapter-9 now so the payload is consumed before the CTSQ=wrss edge in
   * priv_handle_ctrt drives the status-stage CCPL. */
  priv_handle_ctrl_out_data(speed);

  /* SETUP / chapter-9 path runs AFTER DVST so the busreset_rearm
   * has already restored DCP defaults. */
  if (have_ctrt) {
    g_ctrt_irq_count++;
    priv_handle_ctrt(speed, intsts0);
  }
  /* VALID-without-CTRT fallback (HUM Ch 36.2.14 p 1985): the polled
   * worker can race the controller between the VALID and CTRT edges;
   * ra8_usb_dispatch preserves VALID on W0C ack so we can still drain. */
  else if (have_valid) {
    priv_handle_ctrt(speed, intsts0);
  }

  /* Walk every pipe with a queued OUT transfer. ra8_usb_queue_out
   * returns k_ra8_err_no_data if BRDY hasn't fired for that pipe;
   * we just retry on the next IRQ in that case. */
  for (uint8_t i = 1U; i < (uint8_t)k_ux_dcd_ra8_usb_max_pipes; i++) {
    internal_irq_walk_pipe(i);
  }
}
