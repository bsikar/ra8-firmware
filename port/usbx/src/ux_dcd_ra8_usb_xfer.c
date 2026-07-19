/**
 * @file port/usbx/src/ux_dcd_ra8_usb_xfer.c
 * @brief USBX device-controller-driver bridge to ra8_usb -- transfer dispatch.
 *
 * @par Tag
 * [Ring 5 / PORT] {World: S}
 *
 * @details
 * Task-context transfer submission: EP0 control transfers, bulk/interrupt
 * IN/OUT staging, the orphan-OUT consume path, and the CFIFO critical
 * section helpers.
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

#include "ra8_elc_regs.h"
#include "ra8_usb_regs.h"
#include "ra8_check.h"
#include "ra8_isr.h"
#include "ra8_log.h"
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra8_usb.h"
#include "ux_dcd_ra8_usb_internal.h"
#include "ux_device_stack.h"
#include "ux_system.h"
#include "ux_utility.h"

uint8_t s_orphan_buf[k_ra8_usb_orphan_bytes]; /**< One held OUT packet. */

uint16_t s_orphan_len = 0U; /**< Held byte count; 0 = empty. */

uint8_t s_orphan_pipe = 0U; /**< Pipe the held packet is on. */

/* -------------------------------------------------------------------------- */
/* Internal helpers */
/* -------------------------------------------------------------------------- */

/**
 * @brief Map a USB EP number (1..9) into our PIPE table index.
 *
 * @param[in] ep_addr Endpoint address (with dir bit in 0x80).
 *
 * @return Pipe index 0..9, or k_ux_dcd_ra8_usb_max_pipes on overflow.
 *
 * @details See implementation for details.
 * @retval 0 Success or default value.
 * @pre Module has been initialized.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
uint8_t internal_ep_to_pipe(uint8_t ep_addr)
{
  const uint8_t ep = ep_addr & (uint8_t)k_ra8_usb_ep_addr_num_mask;
  if (ep == 0U) {
    return 0U;
  }
  if (ep < (uint8_t)k_ux_dcd_ra8_usb_max_pipes) {
    return ep;
  }
  return (uint8_t)k_ux_dcd_ra8_usb_max_pipes;
}

/**
 * @brief Drive an EP0 / DCP control transfer (data or status stage).
 *
 * @details Split the IN-data and zero-length status paths apart so the
 * outer dispatcher's MC/DC inventory stays small.
 *
 *  - With payload (``in_transfer_length != 0``): push bytes via
 *    ``ra8_usb_dcp_in_data`` (PID=BUF, no CCPL). CCPL is asserted later
 *    on the CTSQ status-stage edge by ``internal_handle_ctrt``.
 *  - Zero-length (SET_ADDRESS, SET_CONFIGURATION...): call
 *    ``ra8_usb_control_response(true)`` which sets PID=BUF and pulses
 *    CCPL, completing the status stage.
 *
 * @param[in,out] tr USBX EP0 transfer request.
 *
 * @return UX_SUCCESS or UX_TRANSFER_ERROR.
 * @retval UX_SUCCESS Bytes / CCPL accepted by ``ra8_usb``.
 * @retval UX_TRANSFER_ERROR Underlying DCP call failed.
 *
 * @pre ``tr`` non-null and bound to the EP0 endpoint.
 * @pre Bridge is past ``ux_dcd_ra8_usb_initialize``.
 * @post ``actual_length`` reflects bytes pushed on the data path.
 * @post No wire-side state mutated on error.
 *
 * @note Runs on the USBX device task context.
 * @since 0.1.0
 */
static unsigned int internal_ep0_transfer(struct UX_SLAVE_TRANSFER_STRUCT* tr)
{
  if (tr->ux_slave_transfer_request_in_transfer_length != 0U &&
      tr->ux_slave_transfer_request_data_pointer != nullptr) {
    /* USBX only routes the EP0 *IN* data stage (descriptors, class GET_*)
     * through the DCD transfer function via ``in_transfer_length``. The
     * host->device data stage (e.g. DFU_DNLOAD) is delivered into the
     * control buffer BEFORE the class entry runs -- see the deferred
     * ::ra8_usb_dcp_out_arm / ::ra8_usb_dcp_out_read path driven from
     * ::internal_dispatch_setup and ::internal_handle_ctrl_out_data. */
    const uint16_t len = (uint16_t)tr->ux_slave_transfer_request_in_transfer_length;
    if (ra8_usb_dcp_in_data(s_dcd.speed, tr->ux_slave_transfer_request_data_pointer, len) !=
        k_ra8_ok) {
      return UX_TRANSFER_ERROR;
    }
    tr->ux_slave_transfer_request_actual_length = len;
    return UX_SUCCESS;
  }
  if (ra8_usb_control_response(s_dcd.speed, true) != k_ra8_ok) {
    return UX_TRANSFER_ERROR;
  }
  return UX_SUCCESS;
}

/**
 * @brief Block on the USBX transfer semaphore until the IRQ posts completion.
 *
 * @details Mirrors the sync contract of ``ux_dcd_sim_slave_transfer_request``:
 * for non-EP0 transfers the DCD must wait on
 * ``ux_slave_transfer_request_semaphore`` until the IRQ path sets
 * ``completion_code`` and ``tx_semaphore_put``s the semaphore. Returning
 * UX_SUCCESS immediately would let class drivers observe ``actual_length=0``
 * and treat it as a short-packet completion -- loopback consumers then
 * busy-spin on ``(n == 0)``.
 *
 * @param[in,out] tr USBX transfer request being awaited.
 * @param[in] pipe Pipe index used for ``s_diag`` accounting (2 == loopback).
 *
 * @return The completion code posted by the IRQ, or UX_TRANSFER_ERROR
 *         on semaphore failure.
 * @retval UX_SUCCESS IRQ posted a successful completion.
 * @retval UX_TRANSFER_ERROR Timeout / abort / IRQ-side error.
 *
 * @pre ``tr`` is the stashed transfer at ``s_dcd.pipes[pipe].xfer``.
 * @pre Caller is on the USBX task context (not in IRQ).
 * @post ``s_dcd.pipes[pipe].xfer`` is nullptr if the wait failed.
 * @post ``completion_code`` reflects the IRQ-posted result.
 *
 * @note Compiled out under ``UX_DEVICE_STANDALONE``; not used in that build.
 * @since 0.1.0
 */
#ifndef UX_DEVICE_STANDALONE
/**
 * @brief Block on the USBX transfer semaphore until the IRQ posts completion.
 *
 * @details See the canonical block immediately above this ``#ifndef``
 * for the full contract. Mirrored here so the doxy auditor can find a
 * block immediately preceding the function definition (no preprocessor
 * directive between the block and the signature).
 *
 * @param[in,out] tr USBX transfer request being awaited.
 * @param[in] pipe Pipe index used for ``s_diag`` accounting (2 == loopback).
 *
 * @return The completion code posted by the IRQ, or UX_TRANSFER_ERROR on
 *         semaphore failure.
 * @retval UX_SUCCESS IRQ posted a successful completion.
 * @retval UX_TRANSFER_ERROR Timeout / abort / IRQ-side error.
 *
 * @pre ``tr`` is the stashed transfer at ``s_dcd.pipes[pipe].xfer``.
 * @pre Caller is on the USBX task context (not in IRQ).
 * @post ``s_dcd.pipes[pipe].xfer`` is nullptr if the wait failed.
 * @post ``completion_code`` reflects the IRQ-posted result.
 *
 * @note Compiled out under ``UX_DEVICE_STANDALONE``; not used in that build.
 * @since 0.1.0
 */
static unsigned int internal_wait_completion(struct UX_SLAVE_TRANSFER_STRUCT* tr, uint8_t pipe)
{
  ULONG timeout = tr->ux_slave_transfer_request_timeout;
  if (timeout == 0U) {
    timeout = TX_WAIT_FOREVER;
  }
  if (pipe == 2U) {
    s_diag.xfer_req_pipe2_block++;
  }
  UINT sem_status = tx_semaphore_get(&tr->ux_slave_transfer_request_semaphore, timeout);
  if (pipe == 2U) {
    s_diag.xfer_req_pipe2_woken++;
  }
  if (sem_status != TX_SUCCESS) {
    /* Timeout / aborted: drop the pipe slot so a stale stash cannot
     * cause the IRQ path to complete a now-defunct request. */
    s_dcd.pipes[pipe].xfer                        = nullptr;
    tr->ux_slave_transfer_request_completion_code = UX_TRANSFER_ERROR;
    return UX_TRANSFER_ERROR;
  }
  return tr->ux_slave_transfer_request_completion_code;
}

#endif

/**
 * @brief Deliver a held orphan OUT packet to a freshly submitted transfer.
 *
 * @details Called by ::internal_submit_pipe when ::s_orphan_len is
 * non-zero for the target pipe. Copies the held packet (captured by
 * ::internal_irq_drain_orphan_out) into the transfer buffer as packet
 * one. If the held packet fills the request, or is itself a short
 * (< MPS) packet, it ends the transfer -- completed synchronously here,
 * with the semaphore posted so the task-side wait returns at once.
 * Otherwise the pipe is armed to BUF and ::internal_irq_complete_out
 * streams the remainder from the held byte offset.
 *
 * @param[in,out] tr   USBX transfer request, already stashed on the pipe.
 * @param[in]     pipe Pipe index (1..max_pipes-1); equals ::s_orphan_pipe.
 *
 * @return USBX result code.
 * @retval UX_SUCCESS        Held packet delivered (transfer complete or armed).
 * @retval UX_TRANSFER_ERROR Re-arm of the OUT pipe failed.
 *
 * @pre ``s_orphan_len != 0`` and ``s_orphan_pipe == pipe``.
 * @pre Task context; the caller stashed ``tr`` at ``s_dcd.pipes[pipe].xfer``.
 * @post ``s_orphan_len == 0`` -- the held packet was consumed.
 * @post On a completed transfer the semaphore is posted and the stash cleared.
 *
 * @note Task-context only; not ISR-safe.
 * @since 0.1.0
 */
static unsigned int internal_submit_consume_orphan(struct UX_SLAVE_TRANSFER_STRUCT* tr,
                                                   uint8_t                          pipe)
{
  const uint16_t req  = (uint16_t)tr->ux_slave_transfer_request_requested_length;
  const uint16_t mps  = s_dcd.pipes[pipe].max_pkt;
  const uint16_t held = s_orphan_len;
  const uint16_t n    = (held < req) ? held : req;
  internal_trace_event((uint8_t)k_dcd_trace_kind_ouse, s_orphan_buf[0], held);
  (void)memcpy(tr->ux_slave_transfer_request_data_pointer, s_orphan_buf, (size_t)n);
  s_orphan_len                                = 0U;
  tr->ux_slave_transfer_request_actual_length = n;

  bool done = (n >= req);
  if (held < mps) {
    done = true; /* short held packet terminates the OUT data phase */
  }
  if (done) {
    tr->ux_slave_transfer_request_completion_code = UX_SUCCESS;
    s_dcd.pipes[pipe].xfer                        = nullptr;
#ifndef UX_DEVICE_STANDALONE
    (void)tx_semaphore_put(&tr->ux_slave_transfer_request_semaphore);
#endif
    return UX_SUCCESS;
  }
  /* Partial: more packets expected -- arm BUF for the streaming tail. */
  if (ra8_usb_rearm_out_pipe(s_dcd.speed, pipe) != k_ra8_ok) {
    s_dcd.pipes[pipe].xfer = nullptr;
    return UX_TRANSFER_ERROR;
  }
  return UX_SUCCESS;
}

/**
 * @brief Enter a CFIFO critical section: mask interrupts.
 *
 * @details The CFIFO port (CFIFOSEL.CURPIPE + the FIFO window) is a
 * single shared resource. The class thread stages the first chunk of
 * every bulk-IN transfer through it while the USB ISR walk drains
 * bulk-OUT packets and stages IN continuations through the same port.
 * An ISR preempting the thread mid-FIFO-write retargets CURPIPE under
 * it and the remaining bytes land in the wrong pipe -- framing stays
 * intact, the payload is garbage (observed against macOS as FAT
 * sectors reading back zeroed during sustained sequential reads).
 * Masking interrupts for the short stage (FRDY wait on an empty bank
 * plus a <= MPS FIFO write, microseconds) closes the race.
 *
 * @return The PRIMASK value to pass to ::internal_fifo_unlock.
 * @retval 0 Interrupts were enabled on entry.
 * @pre Thread (non-ISR) context.
 * @pre The section being protected is bounded (no unbounded spins).
 * @post Interrupts are masked.
 * @post No other state changes.
 * @note Pair every call with ::internal_fifo_unlock.
 * @since 0.1.0
 */
static uint32_t internal_fifo_lock(void)
{
  uint32_t primask = 0U;
  __asm__ volatile("mrs %0, primask" : "=r"(primask));
  __asm__ volatile("cpsid i" ::: "memory");
  return primask;
}

/**
 * @brief Leave a CFIFO critical section: restore the interrupt mask.
 *
 * @details Re-enables interrupts only when they were enabled at the
 * matching ::internal_fifo_lock, so nesting inside an already-masked
 * region stays masked.
 *
 * @param[in] primask PRIMASK snapshot from ::internal_fifo_lock.
 *
 * @pre @p primask came from the matching ::internal_fifo_lock call.
 * @pre The protected FIFO operation has completed.
 * @post Interrupts are enabled again when they were on entry.
 * @post No other state changes.
 *
 * @note Pair of ::internal_fifo_lock.
 * @since 0.1.0
 */
static void internal_fifo_unlock(uint32_t primask)
{
  if ((primask & 1U) == 0U) {
    __asm__ volatile("cpsie i" ::: "memory");
  }
}

/**
 * @brief Stage the first chunk of an IN transfer (plus single-packet ZLP).
 *
 * @details IN endpoint path of ::internal_submit_pipe: USBX hands the
 * DCD the whole transfer, but ``ra8_usb_queue_in`` moves at most one
 * max-packet bank -- so a transfer longer than MPS is streamed: this
 * pushes the first packet (atomically against the ISR walk's shared
 * CFIFO use, see ::internal_fifo_lock) and
 * ::internal_irq_complete_in pushes each subsequent packet as the
 * host drains the previous one (BEMP). A single-packet MPS-exact
 * transfer gets its trailing ZLP staged into the second bank.
 *
 * @param[in,out] tr   USBX transfer request, already stashed.
 * @param[in]     pipe Pipe index the transfer is bound to.
 *
 * @return ``UX_SUCCESS`` or ``UX_TRANSFER_ERROR``.
 * @retval UX_SUCCESS        First chunk (and any ZLP) queued.
 * @retval UX_TRANSFER_ERROR Bridge rejected the queue; stash cleared.
 *
 * @pre ``s_dcd.pipes[pipe].xfer == tr``.
 * @pre Task (non-ISR) context.
 * @post On success ``actual_length`` holds the queued byte count.
 * @post On error ``s_dcd.pipes[pipe].xfer`` is cleared.
 *
 * @note Task-context only; masks interrupts for the FIFO stage.
 * @since 0.1.0
 */
static unsigned int internal_submit_in_pipe(struct UX_SLAVE_TRANSFER_STRUCT* tr, uint8_t pipe)
{
  const uint16_t total = (uint16_t)tr->ux_slave_transfer_request_requested_length;
  const uint16_t mps   = s_dcd.pipes[pipe].max_pkt;
  const uint16_t chunk = (total > mps) ? mps : total;
  /* The ISR walk shares the CFIFO port; stage atomically (see
   * internal_fifo_lock). */
  const uint32_t primask = internal_fifo_lock();
  if (ra8_usb_queue_in(s_dcd.speed, pipe, tr->ux_slave_transfer_request_data_pointer, chunk) !=
      k_ra8_ok) {
    internal_fifo_unlock(primask);
    s_dcd.pipes[pipe].xfer = nullptr;
    return UX_TRANSFER_ERROR;
  }
  tr->ux_slave_transfer_request_actual_length = chunk;
  /* Trailing ZLP policy: honor USBX's per-transfer force_zlp flag
   * (set by ux_device_stack_transfer_request only when the host asked
   * for MORE than the payload and the payload is MPS-aligned). An
   * unconditional MPS-exact ZLP corrupts BOT: a 512-byte READ(10)
   * data phase on a 512-MPS HS bulk pipe is single-packet MPS-exact,
   * and the spurious ZLP lands where the host expects the CSW
   * (observed against macOS at HS as a read-LBA0-then-reset loop).
   * The ZLP itself is staged by internal_irq_complete_in on the data
   * packet's BEMP -- bulk IN pipes run single-banked, so there is no
   * second bank to pre-stage it into here. */
  internal_fifo_unlock(primask);
  return UX_SUCCESS;
}

/**
 * @brief Submit the stashed IN/OUT transfer on a non-EP0 pipe.
 *
 * @details Extracted from ::internal_transfer_request to keep the
 * dispatcher under the NASA P10 Rule 4 60-line cap. For IN endpoints
 * (bit-7 set in ``ep_addr``) we push the buffer via ``ra8_usb_queue_in``
 * and record the actual length; for OUT endpoints we move PIPECTR.PID
 * from NAK to BUF via ``ra8_usb_rearm_out_pipe`` so the controller ACKs
 * the first host OUT token. HUM Ch 36.2.27 PIPECTR.PID.
 *
 * @param[in,out] tr      USBX transfer request, already validated and stashed.
 * @param[in]     pipe    Pipe index the transfer is bound to (1..max_pipes-1).
 * @param[in]     ep_addr Endpoint address byte (bit-7 = direction).
 *
 * @return ``UX_SUCCESS`` on submission; ``UX_TRANSFER_ERROR`` if the
 *         bridge layer rejected the request.
 * @retval UX_SUCCESS Buffer queued (IN) or PIPECTR.PID == BUF (OUT).
 * @retval UX_TRANSFER_ERROR Bridge layer rejected the submission;
 *                           ``s_dcd.pipes[pipe].xfer`` has been cleared.
 *
 * @pre ``s_dcd.pipes[pipe].xfer == tr`` (caller stashed the request).
 * @pre ``pipe != 0`` (EP0 handled separately).
 *
 * @post On error, ``s_dcd.pipes[pipe].xfer`` is cleared.
 * @post On success, ``tr->ux_slave_transfer_request_actual_length`` is
 *       set for IN transfers; OUT pipes have PIPECTR.PID == BUF.
 *
 * @note Task-context only; not ISR-safe.
 * @since 0.1.0
 */
static unsigned int
internal_submit_pipe(struct UX_SLAVE_TRANSFER_STRUCT* tr, uint8_t pipe, uint8_t ep_addr)
{
  if ((ep_addr & (uint8_t)k_ra8_usb_ep_addr_dir_in_bit) != 0U) {
    return internal_submit_in_pipe(tr, pipe);
  }
  /* OUT pipe. A host packet may already have landed in the controller
   * bank during the PID=BUF window after the previous transfer (the
   * host pipelines a CBW's data phase). internal_irq_drain_orphan_out
   * stashes such a packet; the bulk-OUT wire is serial so it belongs to
   * THIS transfer -- deliver it as packet one. Nested ifs keep the test
   * out of the MC/DC inventory. */
  if (s_orphan_len != 0U) {
    if (s_orphan_pipe == pipe) {
      return internal_submit_consume_orphan(tr, pipe);
    }
  }
  /* No held packet: move PID from NAK to BUF so the controller ACKs the
   * host's first OUT token. internal_irq_complete_out parks the pipe
   * back at NAK once the transfer drains. HUM Ch 36.2.27 PIPECTR.PID. */
  if (ra8_usb_rearm_out_pipe(s_dcd.speed, pipe) != k_ra8_ok) {
    s_dcd.pipes[pipe].xfer = nullptr;
    return UX_TRANSFER_ERROR;
  }
  return UX_SUCCESS;
}

/**
 * @brief USBX ``UX_DCD_TRANSFER_REQUEST`` dispatcher entry point.
 *
 * @details Validates the incoming transfer request, maps the endpoint
 * address to a pipe index, and dispatches to either ``internal_ep0_transfer``
 * (EP0 control) or stashes the request and submits an IN/OUT data
 * transfer via ::internal_submit_pipe through the ``ra8_usb_*`` register
 * layer. Under non-standalone builds the request is then awaited via
 * ``internal_wait_completion`` so USBX class drivers see synchronous
 * completion with non-zero ``actual_length``.
 *
 * @param[in,out] tr USBX transfer request to submit. ``ux_slave_transfer_request_endpoint``
 *                   must point at a configured endpoint; data pointer
 *                   and requested length describe the buffer.
 *
 * @return USBX result code.
 * @retval UX_SUCCESS Transfer submitted (and, in non-standalone builds,
 *                    completed by the IRQ path).
 * @retval UX_TRANSFER_ERROR ``tr`` was null, endpoint was null, pipe
 *                           was out of range, or the bridge layer
 *                           rejected the submission.
 *
 * @pre Bridge is past ``ux_dcd_ra8_usb_initialize``.
 * @pre Caller is the USBX device-stack dispatcher (task context).
 * @post For non-EP0 IN transfers, ``s_dcd.pipes[pipe]`` holds the active stash.
 * @post ``s_diag`` counters reflect the dispatch.
 *
 * @note Not ISR-safe; runs on the USBX device task context.
 * @since 0.1.0
 */
unsigned int internal_transfer_request(struct UX_SLAVE_TRANSFER_STRUCT* tr)
{
  s_diag.xfer_req_total++;
  if (tr == nullptr) {
    s_diag.xfer_req_null_arg++;
    return UX_TRANSFER_ERROR;
  }
  if (tr->ux_slave_transfer_request_endpoint == nullptr) {
    s_diag.xfer_req_null_arg++;
    return UX_TRANSFER_ERROR;
  }

  UX_SLAVE_ENDPOINT* ep      = tr->ux_slave_transfer_request_endpoint;
  const uint8_t      ep_addr = (uint8_t)ep->ux_slave_endpoint_descriptor.bEndpointAddress;
  const uint8_t      pipe    = internal_ep_to_pipe(ep_addr);
  if (pipe >= (uint8_t)k_ux_dcd_ra8_usb_max_pipes) {
    s_diag.xfer_req_bad_pipe++;
    return UX_TRANSFER_ERROR;
  }
  if (pipe == 2U) {
    s_diag.xfer_req_pipe2_in++;
    if ((ep_addr & (uint8_t)k_ra8_usb_ep_addr_dir_in_bit) == 0U) {
      s_diag.xfer_req_pipe2_out_dir++;
    }
  }

  if (pipe == 0U) {
    return internal_ep0_transfer(tr);
  }

  /* Stash the active transfer so the IRQ path can post completion. */
  s_dcd.pipes[pipe].xfer    = tr;
  s_dcd.pipes[pipe].ep_addr = ep_addr;
  s_dcd.pipes[pipe].dir_in =
    (uint8_t)((ep_addr & (uint8_t)k_ra8_usb_ep_addr_dir_in_bit) != 0U ? 1U : 0U);
  s_dcd.pipes[pipe].max_pkt = (uint16_t)ep->ux_slave_endpoint_descriptor.wMaxPacketSize;
  if (pipe == 2U) {
    s_diag.xfer_req_pipe2_stashed++;
  }

  const unsigned int submit = internal_submit_pipe(tr, pipe, ep_addr);
  if (submit != UX_SUCCESS) {
    return submit;
  }

#ifndef UX_DEVICE_STANDALONE
  return internal_wait_completion(tr, pipe);
#else
  return UX_SUCCESS;
#endif
}
