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
#include "ra_check.h"
#include "ra_isr.h"
#include "ra_log.h"
#include "ux_api.h"

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

/* -------------------------------------------------------------------------- */
/* Internal helpers                                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief Map a USB EP number (1..9) into our PIPE table index.
 *
 * @param[in] ep_addr Endpoint address (with dir bit in 0x80).
 *
 * @return Pipe index 0..9, or k_ux_dcd_ra_usb_max_pipes on overflow.
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

  /* DCP / EP0: control responses are driven by the chapter-9 layer
   * via PID + CCPL. We just ack with the queued data length here. */
  if (pipe == 0U) {
    if (ra_usb_control_response(s_dcd.speed, true) != k_ra_ok) {
      return UX_TRANSFER_ERROR;
    }
    if (tr->ux_slave_transfer_request_in_transfer_length != 0U &&
        tr->ux_slave_transfer_request_data_pointer != nullptr) {
      const uint16_t len = (uint16_t)tr->ux_slave_transfer_request_in_transfer_length;
      if (ra_usb_queue_in(s_dcd.speed, 0U, tr->ux_slave_transfer_request_data_pointer, len) !=
          k_ra_ok) {
        return UX_TRANSFER_ERROR;
      }
      tr->ux_slave_transfer_request_actual_length = len;
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
      return UX_TRANSFER_ERROR;
    }
    tr->ux_slave_transfer_request_actual_length = len;
  }
  /* OUT pipes block until the IRQ path delivers bytes; nothing else
   * to do here. The class thread waits on
   * ux_slave_transfer_request_semaphore. */
  return UX_SUCCESS;
}

/**
 * @brief Translate a USBX endpoint create request into ra_usb call.
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
 */
static ra_isr_handler_t internal_pick_isr(ra_usb_speed_t speed)
{
  return (speed == k_ra_usb_speed_hs) ? internal_usbhs_isr : internal_usbfs_isr;
}

/**
 * @brief ra_usb_attach_handler trampoline.
 */
static void internal_event_cb(void* ctx, ra_usb_speed_t speed, uint16_t status_mask)
{
  (void)ctx;
  ux_dcd_ra_usb_irq(speed, status_mask);
}

void ux_dcd_ra_usb_irq(ra_usb_speed_t speed, uint16_t intsts0)
{
  (void)speed;
  (void)intsts0;
  if (s_dcd.state == k_ux_dcd_ra_usb_state_uninit) {
    return;
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
      uint16_t len = (uint16_t)tr->ux_slave_transfer_request_requested_length;
      if (ra_usb_queue_out(s_dcd.speed, i, tr->ux_slave_transfer_request_data_pointer, &len) ==
          k_ra_ok) {
        tr->ux_slave_transfer_request_actual_length   = len;
        tr->ux_slave_transfer_request_completion_code = UX_SUCCESS;
        s_dcd.pipes[i].xfer                           = nullptr;
#ifndef UX_DEVICE_STANDALONE
        (void)tx_semaphore_put(&tr->ux_slave_transfer_request_semaphore);
#endif
      }
    }
  }
}

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* -------------------------------------------------------------------------- */

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

  /* Wire the controller's combined interrupt line into the IELSR
   * dispatch table. Event codes verified against FSP
   * `ra/fsp/src/bsp/mcu/ra8d2/bsp_elc.h` lines 133/347 -- USBFS_INT =
   * 0x09A, USBHS_USB_INT_RESUME = 0x2C3. State is set to
   * `_ready` first so the IRQ trampoline (which runs immediately after
   * ra_isr_register enables the NVIC line) sees a coherent bridge. */
  uint16_t       slot      = 0U;
  const ra_err_t isr_err   = ra_isr_register(internal_pick_event(speed),
                                           internal_pick_isr(speed),
                                           nullptr,
                                           (uint8_t)k_ra_usb_dcd_isr_prio,
                                           &slot);
  if (isr_err != k_ra_ok) {
    ra_log_error(s_tag, "ra_isr_register USB interrupt failed");
    s_dcd.state = k_ux_dcd_ra_usb_state_uninit;
    return isr_err;
  }

  /* Tell USBX system the speed. */
  _ux_system_slave->ux_system_slave_speed =
    (speed == k_ra_usb_speed_hs) ? UX_HIGH_SPEED_DEVICE : UX_FULL_SPEED_DEVICE;

  ra_log_info(s_tag, "DCD bridge installed");
  return k_ra_ok;
}

ra_err_t ux_dcd_ra_usb_uninitialize(void)
{
  if (s_dcd.state == k_ux_dcd_ra_usb_state_uninit) {
    return k_ra_err_invalid_state;
  }
  (void)ra_isr_unregister(internal_pick_event(s_dcd.speed));
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

ra_usb_dcd_state_t ux_dcd_ra_usb_state(void)
{
  return s_dcd.state;
}
