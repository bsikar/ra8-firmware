/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/usbx/src/ux_dcd_ra8_usb_ep.c
 * @brief USBX device-controller-driver bridge to ra8_usb -- endpoint dispatch.
 *
 * @par Tag
 * [Ring 5 / PORT] {World: S}
 *
 * @details
 * USBX DCD function trampoline plus the endpoint create / destroy /
 * stall helpers and the CHANGE_STATE accounting.
 *
 * Split out of ``ux_dcd_ra8_usb.c`` to keep each translation unit under
 * the maintainability line cap; the cross-translation-unit contract
 * lives in ``ux_dcd_ra8_usb_internal.h``.
 *
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
 * @brief Set the power-on PID of a freshly created non-control pipe.
 *
 * @details Called once from ::internal_endpoint_create after
 * ::ra8_usb_configure_endpoint. An IN pipe is left as configured --
 * the queue_in path and ::internal_submit_pipe own its PID. An OUT
 * pipe is armed to PID=BUF only when the in-ISR auto-echo path drains
 * it (CDC loopback); every other OUT pipe is parked at PID=NAK so a
 * host OUT token arriving before ::internal_submit_pipe arms a real
 * receiver is NAK-flow-controlled rather than ACKed into a FIFO with
 * no waiter -- the latter latches BRDYSTS and storms the ISR (GitHub
 * issue #6). Nested ifs keep the auto-echo test out of the MC/DC
 * inventory.
 *
 * @param[in] pipe    Pipe index (1..max_pipes-1).
 * @param[in] ep_addr Endpoint address (bit 7 = direction).
 *
 * @pre ::ra8_usb_configure_endpoint has run for this pipe.
 * @pre Bridge speed ``s_dcd.speed`` is valid.
 * @post OUT pipe PID is BUF (auto-echo pipe) or NAK (all others).
 * @post IN pipe PID is left unchanged.
 *
 * @note Single-threaded create-path use; not ISR-safe.
 * @since 0.1.0
 */
static void internal_endpoint_arm_out_pid(uint8_t pipe, uint8_t ep_addr)
{
  if ((ep_addr & (uint8_t)k_ra8_usb_ep_addr_dir_in_bit) != 0U) {
    return; /* IN pipe -- queue_in / internal_submit_pipe own the PID */
  }
  /* Fresh OUT-pipe config: drop any orphan packet held from a prior
   * configuration so it cannot be misdelivered to a new transfer. */
  s_orphan_len        = 0U;
  bool auto_echo_pipe = false;
  if (s_dcd_auto_echo_enable != 0U) {
    if (pipe == s_dcd_auto_echo_out_pipe) {
      auto_echo_pipe = true;
    }
  }
  /* HUM Ch 36.2.27 "PIPEnCTR : PIPE n Control Register" p 2005 */
  if (auto_echo_pipe) {
    (void)ra8_usb_rearm_out_pipe(s_dcd.speed, pipe);
  } else {
    (void)ra8_usb_park_out_pipe(s_dcd.speed, pipe);
  }
}

/**
 * @brief Translate a USBX endpoint create request into ra8_usb call.
 *
 * @details See implementation for details.
 * @param[in,out] ep See function signature.
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 * @pre Module has been initialized.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static unsigned int internal_endpoint_create(struct UX_SLAVE_ENDPOINT_STRUCT* ep)
{
  s_diag.ep_create_calls++;
  if (ep == nullptr) {
    return UX_ERROR;
  }
  const uint8_t ep_addr = (uint8_t)ep->ux_slave_endpoint_descriptor.bEndpointAddress;
  const uint8_t pipe    = internal_ep_to_pipe(ep_addr);
  if (pipe == 0U || pipe >= (uint8_t)k_ux_dcd_ra8_usb_max_pipes) {
    /* DCP is configured by ra8_usb_device_init; class layer should
     * not call CREATE_ENDPOINT for EP0. */
    return UX_SUCCESS;
  }

  ra8_usb_ep_dir_t  dir = ((ep_addr & (uint8_t)k_ra8_usb_ep_addr_dir_in_bit) != 0U)
                            ? k_ra8_usb_ep_dir_in
                            : k_ra8_usb_ep_dir_out;
  ra8_usb_ep_type_t type;
  switch ((uint8_t)ep->ux_slave_endpoint_descriptor.bmAttributes & 0x03U) {
    case 0x02U:
      type = k_ra8_usb_ep_type_bulk;
      break;
    case 0x03U:
      type = k_ra8_usb_ep_type_intr;
      break;
    case 0x01U:
      type = k_ra8_usb_ep_type_iso;
      break;
    default:
      return UX_ERROR;
  }

  if (ra8_usb_configure_endpoint(s_dcd.speed,
                                 pipe,
                                 (uint8_t)(ep_addr & (uint8_t)k_ra8_usb_ep_addr_num_mask),
                                 dir,
                                 type,
                                 (uint16_t)ep->ux_slave_endpoint_descriptor.wMaxPacketSize) !=
      k_ra8_ok) {
    s_diag.ep_create_fail++;
    return UX_ERROR;
  }
  s_dcd.pipes[pipe].ep_addr = ep_addr;
  s_dcd.pipes[pipe].dir_in =
    (uint8_t)((ep_addr & (uint8_t)k_ra8_usb_ep_addr_dir_in_bit) != 0U ? 1U : 0U);
  s_dcd.pipes[pipe].max_pkt = (uint16_t)ep->ux_slave_endpoint_descriptor.wMaxPacketSize;
  internal_endpoint_arm_out_pid(pipe, ep_addr);
  return UX_SUCCESS;
}

/**
 * @brief Endpoint stall.
 *
 * @details See implementation for details.
 *
 * @param[in,out] ep See function signature for type and usage.
 *
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
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
static unsigned int internal_endpoint_stall(struct UX_SLAVE_ENDPOINT_STRUCT* ep)
{
  if (ep == nullptr) {
    return UX_ERROR;
  }
  const uint8_t pipe =
    internal_ep_to_pipe((uint8_t)ep->ux_slave_endpoint_descriptor.bEndpointAddress);
  if (pipe >= (uint8_t)k_ux_dcd_ra8_usb_max_pipes) {
    return UX_ERROR;
  }
  return (ra8_usb_stall_endpoint(s_dcd.speed, pipe) == k_ra8_ok) ? UX_SUCCESS : UX_ERROR;
}

/* -------------------------------------------------------------------------- */
/* USBX entry-point: the ux_slave_dcd_function trampoline */
/* -------------------------------------------------------------------------- */

/**
 * @brief Drop the bridge's per-pipe transfer stash for a destroyed endpoint.
 *
 * @details USBX calls ``UX_DCD_DESTROY_ENDPOINT`` when a class driver
 * tears down an interface (e.g. SET_CONFIGURATION(0) or device detach).
 * The ra8_usb register layer has no per-pipe destroy primitive, so the
 * cleanup we owe is forgetting any cached ``UX_SLAVE_TRANSFER*`` so the
 * IRQ path cannot post a completion against a defunct request.
 *
 * @param[in] ep USBX endpoint pointer (the parameter from the dispatch).
 *
 * @return UX_SUCCESS, or UX_ERROR if ``ep`` is null.
 * @retval UX_SUCCESS Pipe stash cleared (or pipe was out of range).
 * @retval UX_ERROR ``ep`` was null.
 *
 * @pre Bridge is past ``ux_dcd_ra8_usb_initialize``.
 * @pre Caller is the USBX device-stack dispatcher.
 * @post ``s_dcd.pipes[pipe].xfer`` is nullptr for the named pipe.
 * @post No wire-side state mutated.
 *
 * @note Runs on the USBX device task context.
 * @since 0.1.0
 */
static unsigned int internal_endpoint_destroy(struct UX_SLAVE_ENDPOINT_STRUCT* ep)
{
  if (ep == nullptr) {
    return UX_ERROR;
  }
  const uint8_t pipe =
    internal_ep_to_pipe((uint8_t)ep->ux_slave_endpoint_descriptor.bEndpointAddress);
  if (pipe < (uint8_t)k_ux_dcd_ra8_usb_max_pipes) {
    s_dcd.pipes[pipe].xfer = nullptr;
  }
  return UX_SUCCESS;
}

/**
 * @brief Count UX_DCD_CHANGE_STATE notifications by target state.
 *
 * @details Diagnostic brackets around the chapter-9 configuration
 * walk: the stack notifies ATTACHED during teardown and CONFIGURED at
 * the end, so the counter pair shows how far configuration processing
 * ran (read via JLink alongside ::s_diag).
 *
 * @param[in] state The UX device state being announced.
 *
 * @pre Called from the DCD function dispatcher only.
 * @pre ::s_diag is single-writer per counter.
 * @post The matching counter is incremented (others untouched).
 * @post No other state changes.
 *
 * @note Diagnostic only; never read by production code.
 * @since 0.1.0
 */
static void internal_count_change_state(unsigned long state)
{
  if (state == (unsigned long)UX_DEVICE_ATTACHED) {
    s_diag.chg_state_attached++;
  }
  if (state == (unsigned long)UX_DEVICE_CONFIGURED) {
    s_diag.chg_state_configured++;
  }
}

unsigned int
/**
 * @brief USBX device-side DCD function dispatcher.
 *
 * @details Stamped into ``UX_SLAVE_DCD::ux_slave_dcd_function`` during
 * ``ux_dcd_ra8_usb_initialize``. The USBX device stack calls this with a
 * ``UX_DCD_*`` selector to request endpoint create/destroy, transfer
 * request, transfer abort, stall, and similar primitives; the trampoline
 * routes each to the matching ``internal_*`` helper.
 *
 * @param[in,out] dcd USBX DCD ownership block (currently unused; the
 *                    bridge keeps its own static state in ``s_dcd``).
 * @param[in] function USBX ``UX_DCD_*`` selector (e.g.
 *                     ``UX_DCD_TRANSFER_REQUEST``).
 * @param[in,out] parameter Selector-dependent argument
 *                          (``UX_SLAVE_TRANSFER*`` /
 *                          ``UX_SLAVE_ENDPOINT*`` / opaque).
 *
 * @return USBX result code from the dispatched helper.
 * @retval UX_SUCCESS Function handled.
 * @retval UX_CONTROLLER_UNKNOWN Bridge has not been initialized.
 * @retval UX_ERROR Selector unsupported, or helper rejected the call.
 * @retval UX_TRANSFER_ERROR Transfer-request helper failed.
 *
 * @pre Bridge is past ``ux_dcd_ra8_usb_initialize`` (or the call returns
 *      ``UX_CONTROLLER_UNKNOWN``).
 * @pre Caller is the USBX device stack.
 * @post ``s_dcd`` updated per the dispatched selector.
 * @post Wire-side state may have been mutated (CREATE / STALL).
 *
 * @note Runs on the USBX device task context; not ISR-safe.
 * @since 0.1.0
 */
_ux_dcd_ra8_usb_function(struct UX_SLAVE_DCD_STRUCT* dcd, unsigned int function, void* parameter)
{
  (void)dcd;
  if (s_dcd.state == k_ux_dcd_ra8_usb_state_uninit) {
    return UX_CONTROLLER_UNKNOWN;
  }

  switch (function) {
    case UX_DCD_TRANSFER_REQUEST:
      return internal_transfer_request((UX_SLAVE_TRANSFER*)parameter);

    case UX_DCD_TRANSFER_ABORT:
      /* Best effort: NAK the pipe by re-running endpoint configure. */
      return UX_SUCCESS;

    case UX_DCD_CREATE_ENDPOINT:
      return internal_endpoint_create((UX_SLAVE_ENDPOINT*)parameter);

    case UX_DCD_DESTROY_ENDPOINT:
      return internal_endpoint_destroy((UX_SLAVE_ENDPOINT*)parameter);

    case UX_DCD_RESET_ENDPOINT:
      /* ra8_usb has no per-pipe reset that's exposed -- the class
       * layer's expected sequence is destroy + create. We accept
       * this as a no-op. */
      return UX_SUCCESS;

    case UX_DCD_STALL_ENDPOINT:
      return internal_endpoint_stall((UX_SLAVE_ENDPOINT*)parameter);

    case UX_DCD_SET_DEVICE_ADDRESS:
      /* No-op: the SIE auto-responds to SET_ADDRESS (HUM Ch 37.3 p 2147)
       * -- writing USBADDR from firmware fights the auto-latch and
       * stalls enumeration. */
      return UX_SUCCESS;

    case UX_DCD_GET_FRAME_NUMBER:
      if (parameter != nullptr) {
        *(unsigned long*)parameter = 0UL; /* ra8_usb does not surface FRMNUM. */
      }
      return UX_SUCCESS;

    case UX_DCD_CHANGE_STATE:
      internal_count_change_state((unsigned long)parameter);
      s_dcd.state = ((unsigned long)parameter != 0UL) ? k_ux_dcd_ra8_usb_state_active
                                                      : k_ux_dcd_ra8_usb_state_ready;
      return UX_SUCCESS;

    case UX_DCD_ENDPOINT_STATUS:
      return UX_SUCCESS;

    case UX_DCD_ISR_PENDING:
      return UX_SUCCESS;

    default:
      return UX_FUNCTION_NOT_SUPPORTED;
  }
}
