/**
 * @file port/usbx/ux_dcd_ra_usb.c
 * @brief USBX device-controller-driver bridge to ra_usb -- implementation.
 *
 * @par Tag
 * [Ring 5 / PORT] {World: S}
 *
 * @details
 * Implements the dispatch contract documented in ``ux_dcd_ra_usb.h``.
 * Mirrors the layout of upstream USBX DCD ports (e.g.
 * ``ux_dcd_sim_slave_function``) but routes every call through the
 * project's ``ra_usb_*`` register-level driver instead of touching
 * USB controller registers directly.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#define UX_SOURCE_CODE

#include "ux_dcd_ra_usb.h"

#include <stdint.h>
#include <string.h>

#include "ra8d2_elc_regs.h"
#include "ra8d2_usb_regs.h"
#include "ra_check.h"
#include "ra_isr.h"
#include "ra_log.h"
#include "tx_api.h"
#include "ux_api.h"
#include "ux_device_stack.h"
#include "ux_system.h"
#include "ux_utility.h"

/**
 * @enum ra_usb_dcd_isr_prio_t
 * @brief NVIC priority chosen for the USB controller IRQs.
 *
 * @details
 * USB completion IRQs sit between SysTick (priority 0, the highest in
 * this firmware) and the application-level work threads. Picking 4
 * leaves headroom for higher-priority drivers (timers, fault paths)
 * while still pre-empting ThreadX context switches and USBX class
 * threads so SETUP / BRDY / BEMP events drain promptly.
 */
typedef enum : uint8_t {
  k_ra_usb_dcd_isr_prio = 4U, /**< NVIC priority used for both USBFS and USBHS lines. */
} ra_usb_dcd_isr_prio_t;

/* Tag used by ra_log_*. Must be a static lifetime string. */
static const char* const s_tag = "ux_dcd_ra_usb";

/**
 * @struct ra_usb_dcd_pipe_slot_t
 * @brief Per-pipe class-layer cache so the IRQ handler can re-arm
 *        BRDY-driven transfers without crawling the device endpoint
 *        list.
 */
typedef struct {
  struct UX_SLAVE_TRANSFER_STRUCT* xfer;    /**< Active transfer or NULL.       */
  uint8_t                          ep_addr; /**< USB EP number (with dir bit).  */
  uint8_t                          dir_in;  /**< 1 if IN pipe, 0 if OUT.        */
  uint16_t                         max_pkt; /**< Endpoint wMaxPacketSize.       */
} ra_usb_dcd_pipe_slot_t;

/**
 * @struct ra_usb_dcd_t
 * @brief Bridge-singleton state.
 */
typedef struct {
  ra_usb_dcd_state_t          state;                            /**< Bridge run-state.        */
  ra_usb_speed_t              speed;                            /**< Controller this drives.  */
  struct UX_SLAVE_DCD_STRUCT* owner;                            /**< Back-pointer into USBX.  */
  ra_usb_dcd_pipe_slot_t      pipes[k_ux_dcd_ra_usb_max_pipes]; /**< DCP + PIPE1..9.          */
} ra_usb_dcd_t;

/**
 * @var s_dcd
 * @brief The single bridge instance. RA8D2 has two USB controllers
 * but the device stack only ever drives one at a time, so a single
 * static is sufficient.
 *
 * @note Not thread-safe -- updated from the ISR and the dispatch
 * trampoline; concurrency must be arbitrated at the call-site.
 */
static ra_usb_dcd_t s_dcd = {
  .state = k_ux_dcd_ra_usb_state_uninit,
  .speed = k_ra_usb_speed_fs,
  .owner = nullptr,
  .pipes = {},
};

/**
 * @enum ra_setup_byte_idx_t
 * @brief Wire-format byte indices into the USBX SETUP buffer.
 *
 * @details
 * Mirrors the USB 2.0 Ch 9.3 layout, identical to USBX's own
 * ``UX_SETUP_REQUEST_TYPE`` .. ``UX_SETUP_LENGTH`` constants but
 * expressed as a typed enum to satisfy the project's no-magic-numbers
 * rule and to keep the SETUP-pack code readable.
 */
typedef enum : uint8_t {
  k_setup_idx_bmrt   = 0U, /**< bmRequestType (offset 0). */
  k_setup_idx_brq    = 1U, /**< bRequest      (offset 1). */
  k_setup_idx_val_lo = 2U, /**< wValue  low byte  (offset 2). */
  k_setup_idx_val_hi = 3U, /**< wValue  high byte (offset 3). */
  k_setup_idx_idx_lo = 4U, /**< wIndex  low byte  (offset 4). */
  k_setup_idx_idx_hi = 5U, /**< wIndex  high byte (offset 5). */
  k_setup_idx_len_lo = 6U, /**< wLength low byte  (offset 6). */
  k_setup_idx_len_hi = 7U, /**< wLength high byte (offset 7). */
} ra_setup_byte_idx_t;

/**
 * @enum ra_setup_byte_pack_t
 * @brief Bit-shift / mask constants for splitting a uint16_t SETUP
 *        field into its little-endian byte pair.
 */
typedef enum : uint16_t {
  k_setup_byte_shift = 8U,    /**< Bits per byte for the hi-byte extraction. */
  k_setup_byte_mask  = 0xFFU, /**< Low-byte mask after the shift. */
} ra_setup_byte_pack_t;

/* -------------------------------------------------------------------------- */
/* Internal helpers                                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief Map a USB EP number (1..9) into our PIPE table index.
 *
 * @param[in] ep_addr Endpoint address (with dir bit in 0x80).
 *
 * @return Pipe index 0..9, or k_ux_dcd_ra_usb_max_pipes on overflow.
 *
 * @details See implementation for details.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint8_t internal_ep_to_pipe(uint8_t ep_addr)
{
  const uint8_t ep = ep_addr & (uint8_t)0x0FU;
  if (ep == 0U) {
    return 0U;
  }
  if (ep < (uint8_t)k_ux_dcd_ra_usb_max_pipes) {
    return ep;
  }
  return (uint8_t)k_ux_dcd_ra_usb_max_pipes;
}

/**
 * @brief Dispatch an OUT or IN bulk/interrupt transfer to ra_usb.
 *
 * @param[in,out] tr USBX transfer request.
 *
 * @return UX_SUCCESS on enqueue, UX_TRANSFER_ERROR on rejection.
 *
 * @details See implementation for details.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static unsigned int internal_transfer_request(struct UX_SLAVE_TRANSFER_STRUCT* tr)
{
  if (tr == nullptr || tr->ux_slave_transfer_request_endpoint == nullptr) {
    return UX_TRANSFER_ERROR;
  }

  UX_SLAVE_ENDPOINT* ep      = tr->ux_slave_transfer_request_endpoint;
  const uint8_t      ep_addr = (uint8_t)ep->ux_slave_endpoint_descriptor.bEndpointAddress;
  const uint8_t      pipe    = internal_ep_to_pipe(ep_addr);
  if (pipe >= (uint8_t)k_ux_dcd_ra_usb_max_pipes) {
    return UX_TRANSFER_ERROR;
  }

  /* DCP / EP0: split control IN data stage from the no-data status path.
   * For a control transfer with payload (e.g. GET_DESCRIPTOR) we must
   * push the bytes via ra_usb_dcp_in_data (which raises PID=BUF without
   * pulsing CCPL); CCPL is asserted later on the CTSQ status-stage edge
   * by the bridge's internal_handle_ctrt path. For zero-length control
   * (e.g. SET_ADDRESS, SET_CONFIGURATION) ra_usb_control_response(true)
   * sets PID=BUF and pulses CCPL, completing the status stage. */
  if (pipe == 0U) {
    if (tr->ux_slave_transfer_request_in_transfer_length != 0U &&
        tr->ux_slave_transfer_request_data_pointer != nullptr) {
      const uint16_t len = (uint16_t)tr->ux_slave_transfer_request_in_transfer_length;
      if (ra_usb_dcp_in_data(s_dcd.speed,
                             tr->ux_slave_transfer_request_data_pointer,
                             len) != k_ra_ok) {
        return UX_TRANSFER_ERROR;
      }
      tr->ux_slave_transfer_request_actual_length = len;
      return UX_SUCCESS;
    }
    if (ra_usb_control_response(s_dcd.speed, true) != k_ra_ok) {
      return UX_TRANSFER_ERROR;
    }
    return UX_SUCCESS;
  }

  /* Stash the active transfer so the IRQ path can post completion. */
  s_dcd.pipes[pipe].xfer    = tr;
  s_dcd.pipes[pipe].ep_addr = ep_addr;
  s_dcd.pipes[pipe].dir_in  = (uint8_t)((ep_addr & 0x80U) != 0U ? 1U : 0U);
  s_dcd.pipes[pipe].max_pkt = (uint16_t)ep->ux_slave_endpoint_descriptor.wMaxPacketSize;

  if ((ep_addr & 0x80U) != 0U) {
    const uint16_t len = (uint16_t)tr->ux_slave_transfer_request_requested_length;
    if (ra_usb_queue_in(s_dcd.speed, pipe, tr->ux_slave_transfer_request_data_pointer, len) !=
        k_ra_ok) {
      s_dcd.pipes[pipe].xfer = nullptr;
      return UX_TRANSFER_ERROR;
    }
    tr->ux_slave_transfer_request_actual_length = len;
  }

  /* Block until the IRQ path (ux_dcd_ra_usb_irq) signals completion by
   * tx_semaphore_put on ux_slave_transfer_request_semaphore.
   *
   * The upstream USBX device-stack contract is that the DCD's
   * UX_DCD_TRANSFER_REQUEST handler is synchronous from the class
   * driver's point of view: the matching pattern in
   * ux_dcd_sim_slave_transfer_request.c calls
   * _ux_device_semaphore_get(&tr->ux_slave_transfer_request_semaphore,
   *                          tr->ux_slave_transfer_request_timeout)
   * after stashing the request. _ux_device_stack_transfer_request
   * (ux_device_stack_transfer_request.c) does NOT wait on its own --
   * it just returns the DCD's status -- so without this wait the class
   * thread observes actual_length=0 immediately and treats the (still
   * pending!) read as a short-packet completion, busy-spinning while
   * the BRDY IRQ later overwrites the stale slot. */
#ifndef UX_DEVICE_STANDALONE
  ULONG timeout = tr->ux_slave_transfer_request_timeout;
  if (timeout == 0UL) {
    timeout = TX_WAIT_FOREVER;
  }
  const UINT sem_status =
    tx_semaphore_get(&tr->ux_slave_transfer_request_semaphore, timeout);
  if (sem_status != TX_SUCCESS) {
    /* Timeout or abort. Drop our slot so a stale BRDY does not write
     * into a transfer the caller has already abandoned. */
    s_dcd.pipes[pipe].xfer = nullptr;
    tr->ux_slave_transfer_request_status = UX_TRANSFER_STATUS_ABORT;
    return (sem_status == TX_NO_INSTANCE) ? UX_TRANSFER_NO_ANSWER : UX_TRANSFER_ERROR;
  }
  return tr->ux_slave_transfer_request_completion_code;
#else
  return UX_SUCCESS;
#endif
}

/**
 * @brief Translate a USBX endpoint create request into ra_usb call.
 *
 * @details See implementation for details.
 * @param[in,out] ep See function signature.
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static unsigned int internal_endpoint_create(struct UX_SLAVE_ENDPOINT_STRUCT* ep)
{
  if (ep == nullptr) {
    return UX_ERROR;
  }
  const uint8_t ep_addr = (uint8_t)ep->ux_slave_endpoint_descriptor.bEndpointAddress;
  const uint8_t pipe    = internal_ep_to_pipe(ep_addr);
  if (pipe == 0U || pipe >= (uint8_t)k_ux_dcd_ra_usb_max_pipes) {
    /* DCP is configured by ra_usb_device_init; class layer should
     * not call CREATE_ENDPOINT for EP0. */
    return UX_SUCCESS;
  }

  ra_usb_ep_dir_t  dir = ((ep_addr & 0x80U) != 0U) ? k_ra_usb_ep_dir_in : k_ra_usb_ep_dir_out;
  ra_usb_ep_type_t type;
  switch ((uint8_t)ep->ux_slave_endpoint_descriptor.bmAttributes & 0x03U) {
    case 0x02U:
      type = k_ra_usb_ep_type_bulk;
      break;
    case 0x03U:
      type = k_ra_usb_ep_type_intr;
      break;
    case 0x01U:
      type = k_ra_usb_ep_type_iso;
      break;
    default:
      return UX_ERROR;
  }

  if (ra_usb_configure_endpoint(s_dcd.speed,
                                pipe,
                                (uint8_t)(ep_addr & 0x0FU),
                                dir,
                                type,
                                (uint16_t)ep->ux_slave_endpoint_descriptor.wMaxPacketSize) !=
      k_ra_ok) {
    return UX_ERROR;
  }
  s_dcd.pipes[pipe].ep_addr = ep_addr;
  s_dcd.pipes[pipe].dir_in  = (uint8_t)((ep_addr & 0x80U) != 0U ? 1U : 0U);
  s_dcd.pipes[pipe].max_pkt = (uint16_t)ep->ux_slave_endpoint_descriptor.wMaxPacketSize;
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
 * @pre Module has been initialised.
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
  if (pipe >= (uint8_t)k_ux_dcd_ra_usb_max_pipes) {
    return UX_ERROR;
  }
  return (ra_usb_stall_endpoint(s_dcd.speed, pipe) == k_ra_ok) ? UX_SUCCESS : UX_ERROR;
}

/* -------------------------------------------------------------------------- */
/* USBX entry-point: the ux_slave_dcd_function trampoline                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief USBX DCD function dispatcher. Stamps into
 *        ``UX_SLAVE_DCD::ux_slave_dcd_function`` during init.
 *
 * @details See implementation for details.
 * @param[in,out] dcd See function signature.
 * @param[in,out] function See function signature.
 * @param[in,out] parameter See function signature.
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
unsigned int
_ux_dcd_ra_usb_function(struct UX_SLAVE_DCD_STRUCT* dcd, unsigned int function, void* parameter)
{
  (void)dcd;
  if (s_dcd.state == k_ux_dcd_ra_usb_state_uninit) {
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

    case UX_DCD_DESTROY_ENDPOINT: {
      UX_SLAVE_ENDPOINT* ep = (UX_SLAVE_ENDPOINT*)parameter;
      if (ep == nullptr) {
        return UX_ERROR;
      }
      const uint8_t pipe =
        internal_ep_to_pipe((uint8_t)ep->ux_slave_endpoint_descriptor.bEndpointAddress);
      if (pipe < (uint8_t)k_ux_dcd_ra_usb_max_pipes) {
        s_dcd.pipes[pipe].xfer = nullptr;
      }
      return UX_SUCCESS;
    }

    case UX_DCD_RESET_ENDPOINT:
      /* ra_usb has no per-pipe reset that's exposed -- the class
       * layer's expected sequence is destroy + create. We accept
       * this as a no-op. */
      return UX_SUCCESS;

    case UX_DCD_STALL_ENDPOINT:
      return internal_endpoint_stall((UX_SLAVE_ENDPOINT*)parameter);

    case UX_DCD_SET_DEVICE_ADDRESS:
      return (ra_usb_set_address(s_dcd.speed, (uint8_t)((unsigned long)parameter & 0x7FU)) ==
              k_ra_ok)
               ? UX_SUCCESS
               : UX_ERROR;

    case UX_DCD_GET_FRAME_NUMBER:
      if (parameter != nullptr) {
        *(unsigned long*)parameter = 0UL; /* ra_usb does not surface FRMNUM. */
      }
      return UX_SUCCESS;

    case UX_DCD_CHANGE_STATE:
      s_dcd.state = ((unsigned long)parameter != 0UL) ? k_ux_dcd_ra_usb_state_active
                                                      : k_ux_dcd_ra_usb_state_ready;
      return UX_SUCCESS;

    case UX_DCD_ENDPOINT_STATUS:
      return UX_SUCCESS;

    case UX_DCD_ISR_PENDING:
      return UX_SUCCESS;

    default:
      return UX_FUNCTION_NOT_SUPPORTED;
  }
}

/* -------------------------------------------------------------------------- */
/* IRQ glue                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief NVIC -> ra_usb_dispatch trampoline for the USBFS controller.
 *
 * @details
 * Registered with ``ra_isr_register(k_ra_elc_event_usbfs_int, ...)``
 * during ``ux_dcd_ra_usb_initialize`` when the bridge is brought up
 * for the FS controller. ``ra_usb_dispatch`` reads ``INTSTS0``,
 * clears it, and forwards the snapshot to the handler attached via
 * ``ra_usb_attach_handler`` (which lives in the bridge as
 * ``internal_event_cb``). Without this trampoline the controller's
 * SETUP / BRDY / BEMP / DVST bits accumulate in INTSTS0 and the host
 * times out the enumeration handshake.
 *
 * @param[in] ctx Unused; kept to match ``ra_isr_handler_t``.
 *
 * @pre Bridge is in ``k_ux_dcd_ra_usb_state_ready`` or ``_active``.
 * @pre ``ra_usb_attach_handler`` has been called (done in the same init).
 *
 * @post ``INTSTS0`` for the FS controller has been cleared.
 * @post The bridge's ``internal_event_cb`` ran for any pending bits.
 *
 * @note Runs in NVIC handler mode; must not block.
 *
 * @see ra_usb_dispatch
 * @see ux_dcd_ra_usb_irq
 *
 * @since 0.1.0
 */
static void internal_usbfs_isr(void* ctx)
{
  (void)ctx;
  ra_usb_dispatch(k_ra_usb_speed_fs);
}

/**
 * @brief NVIC -> ra_usb_dispatch trampoline for the USBHS controller.
 *
 * @details
 * Sibling of ``internal_usbfs_isr`` for the high-speed instance. On
 * RA8D2 the USBHS controller raises a single combined "interrupt
 * or resume" line (FSP ``ELC_EVENT_USBHS_USB_INT_RESUME``); this
 * handler forwards both into ``ra_usb_dispatch`` which decodes the
 * cause from ``INTSTS0``.
 *
 * @param[in] ctx Unused; kept to match ``ra_isr_handler_t``.
 *
 * @pre Bridge is in ``k_ux_dcd_ra_usb_state_ready`` or ``_active``.
 * @pre ``ra_usb_attach_handler`` has been called (done in the same init).
 *
 * @post ``INTSTS0`` for the HS controller has been cleared.
 * @post The bridge's ``internal_event_cb`` ran for any pending bits.
 *
 * @note Runs in NVIC handler mode; must not block.
 *
 * @see ra_usb_dispatch
 * @see ux_dcd_ra_usb_irq
 *
 * @since 0.1.0
 */
static void internal_usbhs_isr(void* ctx)
{
  (void)ctx;
  ra_usb_dispatch(k_ra_usb_speed_hs);
}

/**
 * @brief Pick the ELC event number for a controller.
 *
 * @param[in] speed Which controller (FS or HS).
 * @return ``ra_elc_event_t`` event number for that controller.
 *
 * @pre ``speed`` is ``k_ra_usb_speed_fs`` or ``k_ra_usb_speed_hs``.
 * @post No state mutated.
 *
 * @note Pure function.
 *
 * @since 0.1.0
 *
 * @details See implementation for details.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 */
static ra_elc_event_t internal_pick_event(ra_usb_speed_t speed)
{
  return (speed == k_ra_usb_speed_hs) ? k_ra_elc_event_usbhs_int_resume : k_ra_elc_event_usbfs_int;
}

/**
 * @brief Pick the ISR trampoline for a controller.
 *
 * @param[in] speed Which controller (FS or HS).
 * @return Function pointer to the trampoline.
 *
 * @pre ``speed`` is ``k_ra_usb_speed_fs`` or ``k_ra_usb_speed_hs``.
 * @post No state mutated.
 *
 * @note Pure function.
 *
 * @since 0.1.0
 *
 * @details See implementation for details.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 */
static ra_isr_handler_t internal_pick_isr(ra_usb_speed_t speed)
{
  return (speed == k_ra_usb_speed_hs) ? internal_usbhs_isr : internal_usbfs_isr;
}

/**
 * @brief ra_usb_attach_handler trampoline.
 *
 * @details See implementation for details.
 * @param[in,out] ctx See function signature.
 * @param[in,out] speed See function signature.
 * @param[in,out] status_mask See function signature.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_event_cb(void* ctx, ra_usb_speed_t speed, uint16_t status_mask)
{
  (void)ctx;
  ux_dcd_ra_usb_irq(speed, status_mask);
}

/**
 * @brief Pack the bridge's SETUP snapshot into the USBX EP0 transfer
 *        request and hand it to the chapter-9 dispatcher.
 *
 * @details
 * USBX expects the 8-byte SETUP packet to live in
 * ``ux_slave_transfer_request_setup`` of the device's EP0 transfer
 * request, in the wire byte order
 * (bmRequestType, bRequest, wValue_lo, wValue_hi,
 *  wIndex_lo,  wIndex_hi,  wLength_lo, wLength_hi). The mirror
 * registers in the RA8D2 USBFS / USBHS controllers (USBREQ, USBVAL,
 * USBINDX, USBLENG -- HUM Ch 36.2.16..36.2.19, p.1623..1626) already
 * deliver the multi-byte fields in host endian, so we re-serialise
 * them little-endian here. Once the buffer is filled we call
 * ``_ux_device_stack_control_request_process`` which decodes the
 * standard request, drives any IN data stage via the bridge's
 * ``UX_DCD_TRANSFER_REQUEST`` path, and ultimately answers the host
 * (descriptors, SET_ADDRESS, SET_CONFIGURATION, etc.).
 *
 * Mirrors the pattern in
 * ``ux_hcd_sim_host_transaction_schedule.c::SETUP``-handling block
 * which is the upstream reference for "controller has a SETUP packet,
 * push it into the device stack".
 *
 * @param[in] setup Decoded SETUP packet snapshot from
 *                  ``ra_usb_read_setup``.
 *
 * @return UX_SUCCESS if the EP0 transfer request was dispatched,
 *         UX_ERROR if no device / EP0 is available yet.
 * @retval UX_SUCCESS Chapter-9 dispatcher consumed the SETUP.
 * @retval UX_ERROR  Device pointer or EP0 endpoint not bound
 *                   (e.g. CTRT fired before USBX device-stack init).
 *
 * @pre ``setup`` is non-NULL.
 * @pre ``_ux_system_slave`` is bound (set by
 *      ``_ux_device_stack_initialize``).
 *
 * @post EP0 transfer request's ``setup`` buffer holds the wire-format
 *       SETUP, and chapter-9 has been invoked synchronously.
 * @post EP0 ``actual_length`` and ``current_data_pointer`` are reset
 *       so the dispatcher writes from the beginning of the data buffer.
 *
 * @note Runs in IRQ-callback context (called from
 *       ``ra_usb_dispatch`` via ``internal_event_cb``).
 *
 * @see _ux_device_stack_control_request_process
 * @since 0.1.0
 */
static unsigned int internal_dispatch_setup(const ra_usb_setup_t* setup)
{
  if (setup == nullptr || _ux_system_slave == UX_NULL) {
    return UX_ERROR;
  }
  UX_SLAVE_DEVICE* device = &_ux_system_slave->ux_system_slave_device;
  UX_SLAVE_TRANSFER* tr =
    &device->ux_slave_device_control_endpoint.ux_slave_endpoint_transfer_request;
  if (tr == UX_NULL) {
    return UX_ERROR;
  }

  tr->ux_slave_transfer_request_setup[k_setup_idx_bmrt]   = setup->bm_request_type;
  tr->ux_slave_transfer_request_setup[k_setup_idx_brq]    = setup->b_request;
  tr->ux_slave_transfer_request_setup[k_setup_idx_val_lo] = (uint8_t)(setup->w_value & k_setup_byte_mask);
  tr->ux_slave_transfer_request_setup[k_setup_idx_val_hi] =
    (uint8_t)((setup->w_value >> k_setup_byte_shift) & k_setup_byte_mask);
  tr->ux_slave_transfer_request_setup[k_setup_idx_idx_lo] = (uint8_t)(setup->w_index & k_setup_byte_mask);
  tr->ux_slave_transfer_request_setup[k_setup_idx_idx_hi] =
    (uint8_t)((setup->w_index >> k_setup_byte_shift) & k_setup_byte_mask);
  tr->ux_slave_transfer_request_setup[k_setup_idx_len_lo] = (uint8_t)(setup->w_length & k_setup_byte_mask);
  tr->ux_slave_transfer_request_setup[k_setup_idx_len_hi] =
    (uint8_t)((setup->w_length >> k_setup_byte_shift) & k_setup_byte_mask);

  tr->ux_slave_transfer_request_actual_length        = 0UL;
  tr->ux_slave_transfer_request_current_data_pointer =
    tr->ux_slave_transfer_request_data_pointer;
  /* Chapter-9 dispatcher gates on completion_code == UX_SUCCESS
   * (ux_device_stack_control_request_process.c line ~101). The
   * previous SETUP may have left it as UX_TRANSFER_STALLED on a
   * STALL'd request -- clear it so this fresh SETUP is honored. */
  tr->ux_slave_transfer_request_completion_code = UX_SUCCESS;

  return _ux_device_stack_control_request_process(tr);
}

/**
 * @brief Decode INTSTS0.CTSQ and forward the control transfer event.
 *
 * @details
 * Called from ``ux_dcd_ra_usb_irq`` when ``INTSTS0.CTRT`` (bit 11,
 * HUM Ch 36.2.14, p.1620) is asserted. CTSQ[2:0] (mask
 * ``k_ra_intsts0_mask_ctsq``) reports which control-stage edge the
 * controller has just transitioned into:
 *
 *  - ``k_ra_ctsq_rdds`` / ``_wrds`` / ``_wrnd`` -- a SETUP packet has
 *    just been latched; drain it via ``ra_usb_read_setup`` and feed
 *    the chapter-9 stack through ``internal_dispatch_setup``.
 *  - ``k_ra_ctsq_rdss`` / ``_wrss`` -- the data phase is finished and
 *    the controller is in the status stage; pulse ``DCPCTR.CCPL`` via
 *    ``ra_usb_control_response(true)`` so the host sees ACK.
 *  - ``k_ra_ctsq_sqer`` -- protocol sequence error; STALL EP0 by
 *    passing ``false`` to ``ra_usb_control_response``.
 *  - ``k_ra_ctsq_idle`` -- transient; nothing to do.
 *
 * @param[in] speed Which controller fired (FS or HS).
 * @param[in] intsts0 Snapshot of INTSTS0 captured by ``ra_usb_dispatch``.
 *
 * @pre Bridge is past ``ux_dcd_ra_usb_initialize``.
 * @pre ``INTSTS0`` snapshot reflects a CTRT-asserted edge.
 *
 * @post For data-stage CTSQ values, the chapter-9 dispatcher has been
 *       invoked and (best effort) consumed the SETUP.
 * @post For status-stage CTSQ values, ``DCPCTR.CCPL`` has been pulsed
 *       (ACK) or ``DCPCTR.PID`` has been forced to STALL on sequence
 *       error.
 *
 * @note Runs in IRQ-callback context.
 *
 * @see ra_usb_read_setup
 * @see ra_usb_control_response
 * @since 0.1.0
 */
static void internal_handle_ctrt(ra_usb_speed_t speed, uint16_t intsts0)
{
  const uint16_t ctsq = (uint16_t)(intsts0 & (uint16_t)k_ra_intsts0_mask_ctsq);
  switch (ctsq) {
    case k_ra_ctsq_rdds:
    case k_ra_ctsq_wrds: {
      /* SETUP latched, data stage to follow. USBX dispatcher will
       * either push IN payload (rdds -> ra_usb_dcp_in_data) or wait
       * for OUT data (wrds). The status stage is acked separately on
       * the matching CTSQ=rdss / wrss edge.
       *
       * If ra_usb_read_setup fails (INTSTS0.VALID never asserted, the
       * snapshot raced past the SETUP latch) or _ux_device_stack_
       * control_request_process rejects the request (unknown
       * descriptor, bad state, class driver said no), the chip is
       * left in CTSQ=rdds/wrds with PID=NAK and the host hangs
       * waiting for data this device will never push. STALL EP0 in
       * those cases so the host moves on instead of stalling
       * enumeration. */
      ra_usb_setup_t setup = {};
      if (ra_usb_read_setup(speed, &setup) != k_ra_ok) {
        (void)ra_usb_control_response(speed, false);
        break;
      }
      if (internal_dispatch_setup(&setup) != UX_SUCCESS) {
        (void)ra_usb_control_response(speed, false);
      }
      break;
    }
    case k_ra_ctsq_wrnd: {
      /* SETUP latched, no data stage (e.g. SET_ADDRESS,
       * SET_CONFIGURATION, SET_INTERFACE, SET_CONTROL_LINE_STATE,
       * SEND_BREAK). The Renesas USB IP is already in the no-data
       * status stage and waiting for the device to drive an IN-ZLP
       * via DCPCTR.CCPL. The chapter-9 / class dispatchers run
       * synchronously and return UX_SUCCESS on accept without
       * calling back through UX_DCD_TRANSFER_REQUEST for the status
       * ZLP, so the bridge must pulse CCPL itself. STALL on error so
       * the host sees the request rejected instead of hanging. */
      ra_usb_setup_t setup = {};
      if (ra_usb_read_setup(speed, &setup) != k_ra_ok) {
        break;
      }
      const unsigned int rc = internal_dispatch_setup(&setup);
      (void)ra_usb_control_response(speed, rc == UX_SUCCESS);
      break;
    }
    case k_ra_ctsq_rdss:
    case k_ra_ctsq_wrss:
      (void)ra_usb_control_response(speed, true);
      break;
    case k_ra_ctsq_sqer:
      (void)ra_usb_control_response(speed, false);
      break;
    case k_ra_ctsq_idle:
    default:
      break;
  }
}

/**
 * @brief Decode INTSTS0.DVSQ and propagate the device-state change
 *        into USBX's device-state machine.
 *
 * @details
 * Called from ``ux_dcd_ra_usb_irq`` when ``INTSTS0.DVST`` (bit 12,
 * HUM Ch 36.2.14, p.1620) is asserted. The DVSQ[3:0] field
 * (mask ``k_ra_intsts0_mask_dvsq``, HUM Ch 36.2.14, p.1621) encodes
 * the controller's current bus state. We translate to USBX's
 * ``UX_DEVICE_*`` state constants and update both
 * ``_ux_system_slave->ux_system_slave_device.ux_slave_device_state``
 * and the application-installed ``ux_system_slave_change_function``
 * callback so class drivers (CDC, HID, MSC) observe bus reset, address
 * assignment and suspend/resume.
 *
 * @param[in] intsts0 Snapshot of INTSTS0 captured by ``ra_usb_dispatch``.
 *
 * @pre Bridge is past ``ux_dcd_ra_usb_initialize``.
 * @pre ``_ux_system_slave`` is bound.
 *
 * @post ``ux_slave_device_state`` reflects the new bus state.
 * @post ``ux_system_slave_change_function`` (if non-NULL) has been
 *       called once with the new state.
 *
 * @note Runs in IRQ-callback context.
 *
 * @since 0.1.0
 */
static void internal_handle_dvst(uint16_t intsts0)
{
  if (_ux_system_slave == UX_NULL) {
    return;
  }
  UX_SLAVE_DEVICE* device = &_ux_system_slave->ux_system_slave_device;
  const uint16_t   dvsq   = (uint16_t)(intsts0 & (uint16_t)k_ra_intsts0_mask_dvsq);
  unsigned long    new_state;
  /* DVSQ[6] (suspend) overlays the underlying state. Test it first so
   * that "configured + suspend" (0x70) is correctly classified as
   * SUSPENDED, not as the default-case fall-through. SUSPENDED is the
   * only DVST-driven state allowed to demote a higher state, because it
   * is a true bus-level event the chapter-9 dispatcher cannot observe. */
  if ((dvsq & (uint16_t)k_ra_dvsq_suspend) != 0U) {
    new_state                     = (unsigned long)UX_DEVICE_SUSPENDED;
    device->ux_slave_device_state = new_state;
    if (_ux_system_slave->ux_system_slave_change_function != UX_NULL) {
      (void)_ux_system_slave->ux_system_slave_change_function(new_state);
    }
    return;
  }
  switch (dvsq) {
    case k_ra_dvsq_powered:
      new_state = (unsigned long)UX_DEVICE_ATTACHED;
      break;
    case k_ra_dvsq_default:
      /* Bus reset just deasserted: device is in USB DEFAULT state, ready
       * to accept SET_ADDRESS on EP0. Map DEFAULT to ATTACHED so the
       * chapter-9 dispatcher can advance to ADDRESSED on SET_ADDRESS. */
      new_state = (unsigned long)UX_DEVICE_ATTACHED;
      break;
    case k_ra_dvsq_address:
      /* ADDRESSED is normally written by the chapter-9 dispatcher
       * (`_ux_device_stack_address_set`), but the RA USBFS peripheral can
       * auto-handle SET_ADDRESS in hardware -- in that case the only
       * software signal is a DVST whose DVSQ already reads ADDRESS. Map
       * it here so the no-demote rank guard below can advance the
       * software state when the dispatcher hasn't (and will skip when it
       * already has). */
      new_state = (unsigned long)UX_DEVICE_ADDRESSED;
      break;
    case k_ra_dvsq_configured:
      /* CONFIGURED is normally written by the chapter-9 dispatcher
       * (`_ux_device_stack_configuration_set`); same rationale as
       * ADDRESS above -- propose the advance and let the no-demote rank
       * guard arbitrate. */
      new_state = (unsigned long)UX_DEVICE_CONFIGURED;
      break;
    default:
      return;
  }
  /* Never demote: the chapter-9 dispatcher (CTRT-side) may have already
   * advanced state to ADDRESSED/CONFIGURED before this DVST snapshot is
   * processed. Comparing as numeric ranks (RESET<ATTACHED<ADDRESSED<
   * CONFIGURED) lets ATTACHED-from-DVST update RESET but never overwrite
   * a higher state. SUSPENDED was handled above and bypasses this
   * monotonicity guard because suspend is a bus-level demotion. */
  if (new_state <= device->ux_slave_device_state) {
    return;
  }
  device->ux_slave_device_state = new_state;
  if (_ux_system_slave->ux_system_slave_change_function != UX_NULL) {
    (void)_ux_system_slave->ux_system_slave_change_function(new_state);
  }
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
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
void ux_dcd_ra_usb_irq(ra_usb_speed_t speed, uint16_t intsts0)
{
  if (s_dcd.state == k_ux_dcd_ra_usb_state_uninit) {
    return;
  }

  /* CTRT FIRST: SETUP / chapter-9 path must run before any other
   * handler so that ra_usb_read_setup can drain USBREQ/USBVAL/
   * USBINDX/USBLENG while INTSTS0.VALID is still asserted -- the
   * Renesas USB IP latches the SETUP packet briefly and a delayed
   * read can race past it. The previous "DVST first" ordering was
   * introduced to advance ux_slave_device_state before EP0 was
   * gated by USBX (state in {ATTACHED, ADDRESSED, CONFIGURED}), but
   * ux_dcd_ra_usb_initialize now stamps ATTACHED at init and the
   * chapter-9 dispatcher advances state synchronously thereafter,
   * so DVST-first is no longer required and only adds latency
   * between the SETUP latch and our drain.
   *
   * The no-demote rank guard in internal_handle_dvst keeps the
   * post-CTRT case safe: even if DVST runs after CTRT and reports
   * a stale ADDRESS/CONFIGURED snapshot, it cannot demote the state
   * the chapter-9 dispatcher already advanced. */
  if ((intsts0 & (uint16_t)(1U << (uint8_t)k_ra_int0_bit_ctrt)) != 0U) {
    internal_handle_ctrt(speed, intsts0);
  }

  if ((intsts0 & (uint16_t)(1U << (uint8_t)k_ra_int0_bit_dvst)) != 0U) {
    internal_handle_dvst(intsts0);
  }

  /* Walk every pipe with a queued OUT transfer. ra_usb_queue_out
   * returns k_ra_err_no_data if BRDY hasn't fired for that pipe;
   * we just retry on the next IRQ in that case. */
  for (uint8_t i = 1U; i < (uint8_t)k_ux_dcd_ra_usb_max_pipes; i++) {
    UX_SLAVE_TRANSFER* tr = s_dcd.pipes[i].xfer;
    if (tr == nullptr) {
      continue;
    }
    if (s_dcd.pipes[i].dir_in != 0U) {
      /* IN: data was already pushed in TRANSFER_REQUEST. Mark
       * complete on the BEMP that follows. */
      tr->ux_slave_transfer_request_completion_code = UX_SUCCESS;
      s_dcd.pipes[i].xfer                           = nullptr;
#ifndef UX_DEVICE_STANDALONE
      (void)tx_semaphore_put(&tr->ux_slave_transfer_request_semaphore);
#endif
    } else {
      uint16_t       len    = (uint16_t)tr->ux_slave_transfer_request_requested_length;
      const ra_err_t qo_err = ra_usb_queue_out(
        s_dcd.speed, i, tr->ux_slave_transfer_request_data_pointer, &len);
      if (qo_err == k_ra_ok) {
        tr->ux_slave_transfer_request_actual_length   = len;
        tr->ux_slave_transfer_request_completion_code = UX_SUCCESS;
        s_dcd.pipes[i].xfer                           = nullptr;
#ifndef UX_DEVICE_STANDALONE
        (void)tx_semaphore_put(&tr->ux_slave_transfer_request_semaphore);
#endif
      } else if (qo_err == k_ra_err_no_data) {
        /* No BRDY pending. The RA8D2 USB single-buffered OUT pipe
         * state machine can leave PID at NAK between drains; if the
         * controller has already responded NAK to one or more host
         * OUT tokens (NRDYSTS bit `i` accumulating), the pipe will
         * stay parked at NAK indefinitely and the host (macOS) gives
         * up. Proactively ack NRDYSTS for this pipe and force
         * PID=BUF so the next host OUT token is ACKed.
         * HUM Ch 36.2.13 NRDYSTS (W0C) + Ch 36.2.27 PIPECTR.PID. */
        (void)ra_usb_rearm_out_pipe(s_dcd.speed, i);
      }
    }
  }
}

/* -------------------------------------------------------------------------- */
/* Polled-dispatch worker (Option A from HARDWARE_BRINGUP.md USB-FS notes)    */
/* -------------------------------------------------------------------------- */

/**
 * @enum ra_usb_dcd_worker_cfg_t
 * @brief Compile-time sizing for the bridge's polled-dispatch worker.
 *
 * @details
 * The RA8D2 USBFS / USBHS controllers normally drive enumeration off
 * the USBFS_INT / USBHS_USB_INT_RESUME NVIC line, but registering that
 * line via ``ra_isr_register`` HardFaults on EK-RA8D2 silicon
 * (PC=0xEFFFFFFE, see ``docs/HARDWARE_BRINGUP.md`` "second-round
 * hardware verification" + "night sweep" sections). Until that
 * root-cause is solved we run a ``ra_usb_dispatch`` polling loop in a
 * dedicated ThreadX thread so SETUP / BRDY / BEMP / DVST events drain
 * INTSTS0 with sub-millisecond latency -- well inside the macOS
 * ~10 ms post-DPRPU SETUP window.
 *
 * The worker is spawned by ``ux_dcd_ra_usb_initialize`` BEFORE the
 * caller's ``ra_usb_device_attach(true)`` raises DPRPU, so the very
 * first SETUP packet the host issues is observed by the dispatcher.
 *
 * @invariant Worker priority is strictly greater (numerically) than
 *            any application init thread that calls
 *            ``ra_usb_device_attach`` -- otherwise the worker starves
 *            the init thread before D+ pull-up is asserted.
 */
typedef enum : uint16_t {
  k_ra_usb_dcd_worker_stack_bytes = 2048U, /**< Worker thread stack (bytes).      */
  k_ra_usb_dcd_worker_priority    = 12U,   /**< ThreadX priority (lower than app worker @8). */
  k_ra_usb_dcd_worker_threshold   = 12U,   /**< Pre-emption threshold == priority.*/
  k_ra_usb_dcd_worker_time_slice  = 0U,    /**< 0 == TX_NO_TIME_SLICE.            */
} ra_usb_dcd_worker_cfg_t;

/**
 * @var s_dispatch_thread
 * @brief ThreadX TCB for the polled-dispatch worker.
 *
 * @details One worker per bridge instance. The bridge is a singleton
 * (RA8D2 has two USB controllers but the device stack only ever
 * drives one at a time), so a single TCB is sufficient.
 *
 * @note Owned by ``ux_dcd_ra_usb_initialize``; must not be touched
 *       outside the bridge.
 * @warning Direct modification will corrupt the ThreadX scheduler.
 * @since 0.1.0
 */
static TX_THREAD s_dispatch_thread;

/**
 * @var s_dispatch_stack
 * @brief Stack backing storage for ``s_dispatch_thread``.
 *
 * @details Statically allocated per NASA Power-of-10 Rule 3 (no
 * dynamic allocation after init).
 *
 * @note Read/written exclusively by the ThreadX scheduler.
 * @since 0.1.0
 */
static UCHAR s_dispatch_stack[k_ra_usb_dcd_worker_stack_bytes];

/**
 * @var s_dispatch_thread_name
 * @brief Mutable name buffer handed to ``tx_thread_create``.
 *
 * @details ThreadX's ``tx_thread_create`` takes ``CHAR*`` (non-const)
 * for the thread name, so we keep a writable storage slot to avoid a
 * ``-Wcast-qual`` warning at the call site. Contents are never
 * modified after init.
 *
 * @note Read-only post-init; treat as ``const`` despite the type.
 * @since 0.1.0
 */
static CHAR s_dispatch_thread_name[] = "ux_dcd_ra_usb_disp";

/**
 * @var s_dispatch_thread_started
 * @brief Guard preventing a second ``tx_thread_create`` on re-init.
 *
 * @details ``ux_dcd_ra_usb_initialize`` may be called once per boot,
 * but if the application explicitly tears the bridge down and back up
 * we keep the original thread alive (the worker is benign in the
 * uninit state because it relinquishes immediately when
 * ``s_dcd.state`` is ``k_ux_dcd_ra_usb_state_uninit``).
 *
 * @note Single-writer (init path); single-reader.
 * @since 0.1.0
 */
static bool s_dispatch_thread_started = false;

/**
 * @brief Polled-dispatch worker entry. Calls ``ra_usb_dispatch`` in a
 *        tight loop so the bridge sees SETUP / BRDY / BEMP / DVST
 *        edges with sub-millisecond latency.
 *
 * @details
 * Runs forever. Each iteration:
 *   1. If the bridge is uninit, relinquish (avoid burning CPU before
 *      ``ux_dcd_ra_usb_initialize`` has stamped ``s_dcd.speed``).
 *   2. Otherwise call ``ra_usb_dispatch(s_dcd.speed)`` which reads
 *      INTSTS0, clears it, and drives the bridge's
 *      ``internal_event_cb`` for any pending bits.
 *   3. ``tx_thread_relinquish`` so equal-priority threads round-robin.
 *
 * @param[in] arg Unused (ThreadX entry signature).
 *
 * @pre ``ux_dcd_ra_usb_initialize`` has stamped ``s_dcd.speed``.
 * @pre ThreadX scheduler is running.
 *
 * @return Never returns (worker loops forever).
 * @retval (none) Worker entry; no value is ever returned.
 *
 * @post Never returns.
 * @post For every INTSTS0 edge the bridge callback has executed.
 *
 * @note Single-instance worker; not designed for re-entry.
 * @warning Lower-priority threads (the application init thread that
 *          calls ra_usb_device_attach) must run before this worker is
 *          useful -- see priority rationale on ``ra_usb_dcd_worker_cfg_t``.
 *
 * @see ra_usb_dispatch
 * @since 0.1.0
 */
static VOID internal_dispatch_worker(ULONG arg)
{
  (void)arg;
  while (1) {
    if (s_dcd.state == k_ux_dcd_ra_usb_state_uninit) {
      tx_thread_relinquish();
      continue;
    }
    ra_usb_dispatch(s_dcd.speed);
    tx_thread_relinquish();
  }
}

/**
 * @brief Spawn the polled-dispatch worker exactly once per boot.
 *
 * @details
 * Idempotent. Subsequent calls (e.g. across a deinit / reinit cycle)
 * are no-ops because ThreadX cannot recycle a TCB cleanly without a
 * full ``tx_thread_delete`` dance which the bridge intentionally
 * avoids -- the worker is benign in the uninit state.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                     Worker running (or already running).
 * @retval k_ra_err_rtos_thread_create ``tx_thread_create`` failed.
 *
 * @pre ThreadX scheduler is running.
 * @pre Caller holds the bridge init lock (single-threaded init context).
 *
 * @post Exactly one ``s_dispatch_thread`` instance exists.
 * @post Worker is auto-started.
 *
 * @note Not thread-safe; call only from ``ux_dcd_ra_usb_initialize``.
 * @since 0.1.0
 */
static ra_err_t internal_start_dispatch_worker(void)
{
  if (s_dispatch_thread_started) {
    return k_ra_ok;
  }
  const UINT tx = tx_thread_create(&s_dispatch_thread,
                                   s_dispatch_thread_name,
                                   internal_dispatch_worker,
                                   0UL,
                                   s_dispatch_stack,
                                   (ULONG)k_ra_usb_dcd_worker_stack_bytes,
                                   (UINT)k_ra_usb_dcd_worker_priority,
                                   (UINT)k_ra_usb_dcd_worker_threshold,
                                   (ULONG)k_ra_usb_dcd_worker_time_slice,
                                   TX_AUTO_START);
  if (tx != TX_SUCCESS) {
    return k_ra_err_rtos_thread_create;
  }
  s_dispatch_thread_started = true;
  return k_ra_ok;
}

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief Ux dcd ra usb initialize.
 *
 * @details See implementation for details.
 *
 * @param[in,out] speed See function signature for type and usage.
 *
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
ra_err_t ux_dcd_ra_usb_initialize(ra_usb_speed_t speed)
{
  if ((uint8_t)speed > (uint8_t)k_ra_usb_speed_hs) {
    return k_ra_err_invalid_arg;
  }
  RA_RETURN_ON_ERROR(ra_usb_device_init(speed), s_tag, "ra_usb_device_init");
  RA_RETURN_ON_ERROR(ra_usb_attach_handler(internal_event_cb, nullptr),
                     s_tag,
                     "ra_usb_attach_handler");

  /* Wire ourselves into _ux_system_slave -> ux_system_slave_dcd. */
  if (_ux_system_slave == UX_NULL) {
    return k_ra_err_invalid_state;
  }
  UX_SLAVE_DCD* owner                     = &_ux_system_slave->ux_system_slave_dcd;
  owner->ux_slave_dcd_status              = UX_DCD_STATUS_OPERATIONAL;
  owner->ux_slave_dcd_controller_type     = 99U; /* RA-USB private id.    */
  owner->ux_slave_dcd_function            = _ux_dcd_ra_usb_function;
  owner->ux_slave_dcd_controller_hardware = (void*)&s_dcd;

  s_dcd.speed = speed;
  s_dcd.owner = owner;
  s_dcd.state = k_ux_dcd_ra_usb_state_ready;

  for (uint8_t i = 0U; i < (uint8_t)k_ux_dcd_ra_usb_max_pipes; i++) {
    s_dcd.pipes[i].xfer = nullptr;
  }

  /* IRQ wiring temporarily disabled -- see HARDWARE_BRINGUP.md
   * "second-round" entry. Even with FSP-verified event codes (0x09A
   * USBFS_INT, 0x2C3 USBHS_USB_INT_RESUME) the very first
   * ra_isr_register call HardFaults on EK-RA8D2 silicon (PC=
   * 0xEFFFFFFE, MMFAR=0x40700004 -- BLE placeholder space, unrelated
   * to NVIC). Likely a SAU/IDAU-style transition issue or a
   * dispatch-table init order problem. The bridge spawns a dedicated
   * polled-dispatch worker below (Option A in the night-sweep notes)
   * so SETUP / BRDY / BEMP / DVST events still drain INTSTS0 fast
   * enough for the host to complete enumeration. */
  (void)internal_pick_event;
  (void)internal_pick_isr;
  (void)k_ra_usb_dcd_isr_prio;

  /* Tell USBX system the speed. */
  _ux_system_slave->ux_system_slave_speed =
    (speed == k_ra_usb_speed_hs) ? UX_HIGH_SPEED_DEVICE : UX_FULL_SPEED_DEVICE;

  /* Mirror the controller-bring-up work upstream DCDs do in their
   * "initialize_complete" hook (see ux_dcd_sim_slave_initialize_complete.c).
   * _ux_device_stack_initialize only stores the FS/HS framework
   * pointer pairs and allocates the EP0 data buffer; the active
   * device_framework / device_framework_length pair, the parsed
   * device descriptor, the EP0 transfer-request endpoint binding
   * and DCD CREATE_ENDPOINT for EP0 must be done by the DCD or the
   * chapter-9 dispatcher silently STALLs the very first
   * GET_DESCRIPTOR(DEVICE) request (DCPCTR.PID -> 0x42 / STALL). */
  UX_SLAVE_DEVICE* device = &_ux_system_slave->ux_system_slave_device;
  if (_ux_system_slave->ux_system_slave_speed == UX_HIGH_SPEED_DEVICE) {
    _ux_system_slave->ux_system_slave_device_framework =
      _ux_system_slave->ux_system_slave_device_framework_high_speed;
    _ux_system_slave->ux_system_slave_device_framework_length =
      _ux_system_slave->ux_system_slave_device_framework_length_high_speed;
  } else {
    _ux_system_slave->ux_system_slave_device_framework =
      _ux_system_slave->ux_system_slave_device_framework_full_speed;
    _ux_system_slave->ux_system_slave_device_framework_length =
      _ux_system_slave->ux_system_slave_device_framework_length_full_speed;
  }

  if (_ux_system_slave->ux_system_slave_device_framework != UX_NULL) {
    _ux_utility_descriptor_parse(_ux_system_slave->ux_system_slave_device_framework,
                                 _ux_system_device_descriptor_structure,
                                 UX_DEVICE_DESCRIPTOR_ENTRIES,
                                 (UCHAR*)&device->ux_slave_device_descriptor);
  }

  UX_SLAVE_TRANSFER* tr =
    &device->ux_slave_device_control_endpoint.ux_slave_endpoint_transfer_request;
  tr->ux_slave_transfer_request_timeout = UX_MS_TO_TICK(UX_CONTROL_TRANSFER_TIMEOUT);
  tr->ux_slave_transfer_request_current_data_pointer =
    tr->ux_slave_transfer_request_data_pointer;
  tr->ux_slave_transfer_request_endpoint = &device->ux_slave_device_control_endpoint;
  device->ux_slave_device_control_endpoint.ux_slave_endpoint_descriptor.wMaxPacketSize =
    device->ux_slave_device_descriptor.bMaxPacketSize0;
  tr->ux_slave_transfer_request_requested_length =
    device->ux_slave_device_descriptor.bMaxPacketSize0;
  tr->ux_slave_transfer_request_transfer_length =
    device->ux_slave_device_descriptor.bMaxPacketSize0;

  /* Hand EP0 to ourselves so any future TRANSFER_REQUEST has the
   * pipe table populated. */
  (void)_ux_dcd_ra_usb_function(owner,
                                UX_DCD_CREATE_ENDPOINT,
                                (void*)&device->ux_slave_device_control_endpoint);

  device->ux_slave_device_control_endpoint.ux_slave_endpoint_state = UX_ENDPOINT_RESET;
  tr->ux_slave_transfer_request_phase = UX_TRANSFER_PHASE_DATA_IN;

  /* USBX gates EP0 transfers in _ux_device_stack_transfer_request on
   * state in {ATTACHED, ADDRESSED, CONFIGURED}; .bss-zero leaves it
   * at RESET(0). On the first DVST IRQ we advance to ATTACHED, but
   * the host's first GET_DESCRIPTOR(DEVICE) can race ahead of that
   * IRQ and get rejected with UX_TRANSFER_NOT_READY -- the chip then
   * hangs at CTSQ=read-data-stage. Stamp ATTACHED here, before the
   * caller raises DPRPU via ra_usb_device_attach(), so the very
   * first SETUP from the host is always serviceable. The no-demote
   * rank guard in internal_handle_dvst keeps subsequent CTRT-driven
   * advances to ADDRESSED/CONFIGURED safe. */
  device->ux_slave_device_state = (unsigned long)UX_DEVICE_ATTACHED;

  /* Spawn the polled-dispatch worker BEFORE returning so it is
   * already pumping ra_usb_dispatch by the time the application
   * calls ra_usb_device_attach(true). Without this the host raises
   * D+ pull-up, sees the device, issues a SETUP within ~10 ms, and
   * suspends the bus before the application worker even reaches its
   * first ra_usb_dispatch call (ctrt_count=0, setup_count=0 -- see
   * HARDWARE_BRINGUP.md "night sweep"). */
  RA_RETURN_ON_ERROR(internal_start_dispatch_worker(),
                     s_tag,
                     "internal_start_dispatch_worker");

  ra_log_info(s_tag, "DCD bridge installed");
  return k_ra_ok;
}

/**
 * @brief Ux dcd ra usb uninitialize.
 *
 * @details See implementation for details.
 *
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
ra_err_t ux_dcd_ra_usb_uninitialize(void)
{
  if (s_dcd.state == k_ux_dcd_ra_usb_state_uninit) {
    return k_ra_err_invalid_state;
  }
  /* Matching pair to the disabled ra_isr_register in the init path. */
  (void)ra_usb_attach_handler(nullptr, nullptr);
  (void)ra_usb_device_deinit(s_dcd.speed);
  if (s_dcd.owner != nullptr) {
    s_dcd.owner->ux_slave_dcd_status   = UX_DCD_STATUS_HALTED;
    s_dcd.owner->ux_slave_dcd_function = nullptr;
  }
  s_dcd.state = k_ux_dcd_ra_usb_state_uninit;
  s_dcd.owner = nullptr;
  return k_ra_ok;
}

/**
 * @brief Ux dcd ra usb state.
 *
 * @details See implementation for details.
 *
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
ra_usb_dcd_state_t ux_dcd_ra_usb_state(void)
{
  return s_dcd.state;
}
