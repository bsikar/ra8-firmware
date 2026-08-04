/**
 * @file port/usbx/src/ux_hcd_ra8_usb.c
 * @brief USBX host-controller-driver bridge to ra8_usb -- implementation.
 *
 * @par Tag
 * [Ring 5 / PORT] {World: S}
 *
 * @details
 * Counterpart to ``ux_dcd_ra8_usb.c``: routes USBX host-stack
 * dispatch into the ``ra8_usb_host_*`` register-level driver.
 * Mirrors the surface of upstream USBX HCD ports
 * (e.g. ``ux_hcd_ehci_entry``).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#define UX_SOURCE_CODE

#include "ux_hcd_ra8_usb.h"

#include <stdint.h>
#include <string.h>

#include "ra8_check.h"
#include "ra8_log.h"
#include "ux_api.h"

static const char* const s_tag = "ux_hcd_ra8_usb";

/**
 * @struct ra8_usb_hcd_t
 * @brief Bridge-singleton state.
 */
typedef struct {
  ra8_usb_hcd_state_t   state; /**< Bridge run-state.       */
  ra8_usb_speed_t       speed; /**< Controller this drives. */
  struct UX_HCD_STRUCT* owner; /**< Back-pointer into USBX. */
} ra8_usb_hcd_t;

/**
 * @var s_hcd
 * @brief Bridge instance. RA8 has two USB controllers but USBX
 * only ever drives one root-hub-style host at a time in this app.
 */
static ra8_usb_hcd_t s_hcd = {
  .state = k_ux_hcd_ra8_usb_state_uninit,
  .speed = k_ra8_usb_speed_fs,
  .owner = nullptr,
};

/**
 * @enum ra8_usb_hcd_ep_addr_field_t
 * @brief Bit fields of a USB endpoint address (bEndpointAddress).
 *
 * @details USB 2.0 sec 9.6.6: bit 7 is the direction bit (1 = IN) and
 * bits 3:0 carry the endpoint number. The same direction bit also
 * appears as bit 7 of bmRequestType in a SETUP packet (USB 2.0 sec 9.3).
 */
typedef enum : uint8_t {
  k_ra8_usb_hcd_ep_addr_dir_in_bit = 0x80U, /**< Direction bit set => IN.          */
  k_ra8_usb_hcd_ep_addr_num_mask   = 0x0FU, /**< Endpoint-number field (bits 3:0). */
} ra8_usb_hcd_ep_addr_field_t;

/**
 * @enum ra8_usb_hcd_pipe_t
 * @brief Pipe-index bounds for the host bridge.
 *
 * @details Hardware exposes the DCP (pipe 0) plus PIPE1..PIPE9, so a
 * valid pipe index is 0..9; the value 10 is both the count of valid
 * pipes and the out-of-range sentinel returned by ``internal_ep_to_pipe``.
 */
typedef enum : uint8_t {
  k_ra8_usb_hcd_pipe_count = 10U, /**< Valid pipes 0..9; 10 == out-of-range. */
} ra8_usb_hcd_pipe_t;

/**
 * @enum ra8_usb_hcd_ctrl_id_t
 * @brief Private USBX controller-type id reported by this HCD bridge.
 */
typedef enum : uint8_t {
  k_ra8_usb_hcd_controller_id = 99U, /**< RA-USB private controller id. */
} ra8_usb_hcd_ctrl_id_t;

/* -------------------------------------------------------------------------- */
/* Internal helpers */
/* -------------------------------------------------------------------------- */

/**
 * @brief Ep to pipe.
 *
 * @details See implementation for details.
 *
 * @param[in,out] ep_addr See function signature for type and usage.
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
static uint8_t internal_ep_to_pipe(uint8_t ep_addr)
{
  const uint8_t ep = ep_addr & (uint8_t)k_ra8_usb_hcd_ep_addr_num_mask;
  if (ep == 0U) {
    return 0U;
  }
  if (ep < (uint8_t)k_ra8_usb_hcd_pipe_count) {
    return ep;
  }
  return (uint8_t)k_ra8_usb_hcd_pipe_count;
}

/**
 * @brief Endpoint create.
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
static unsigned int internal_endpoint_create(struct UX_ENDPOINT_STRUCT* ep)
{
  if (ep == nullptr) {
    return UX_ERROR;
  }
  const uint8_t ep_addr = (uint8_t)ep->ux_endpoint_descriptor.bEndpointAddress;
  const uint8_t pipe    = internal_ep_to_pipe(ep_addr);
  if (pipe == 0U) {
    /* DCP is configured during host_init. */
    return UX_SUCCESS;
  }
  if (pipe >= (uint8_t)k_ra8_usb_hcd_pipe_count) {
    return UX_SUCCESS;
  }

  ra8_usb_ep_dir_t dir = ((ep_addr & (uint8_t)k_ra8_usb_hcd_ep_addr_dir_in_bit) != 0U)
                           ? k_ra8_usb_ep_dir_in
                           : k_ra8_usb_ep_dir_out;

  ra8_usb_ep_type_t type;
  switch ((uint8_t)ep->ux_endpoint_descriptor.bmAttributes & 0x03U) {
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

  return (ra8_usb_configure_endpoint(s_hcd.speed,
                                     pipe,
                                     (uint8_t)(ep_addr & (uint8_t)k_ra8_usb_hcd_ep_addr_num_mask),
                                     dir,
                                     type,
                                     (uint16_t)ep->ux_endpoint_descriptor.wMaxPacketSize) ==
          k_ra8_ok)
           ? UX_SUCCESS
           : UX_ERROR;
}

/**
 * @brief Drive the EP0 control transfer SETUP + optional data stage.
 *
 * @details Builds an ``ra8_usb_setup_t`` from the USBX transfer request,
 * fires the SETUP token via ``ra8_usb_host_setup_request``, then for the
 * optional data stage routes IN (D2H) through ``ra8_usb_queue_out`` (chip
 * reads from device, fills caller buffer) and OUT (H2D) through
 * ``ra8_usb_queue_in`` (chip writes caller buffer onto wire).
 *
 * @param[in,out] tr USBX EP0 control transfer request.
 *
 * @return UX_SUCCESS on accept, UX_TRANSFER_ERROR if the SIE rejected
 *         the token or the data-stage queue failed.
 * @retval UX_SUCCESS Token accepted; data stage (if any) queued.
 * @retval UX_TRANSFER_ERROR ``ra8_usb_host_setup_request`` or queue failed.
 *
 * @pre ``tr`` is non-null and points to an EP0 transfer.
 * @pre HCD bridge is past ``ux_hcd_ra8_usb_initialize``.
 * @post On success ``ux_transfer_request_actual_length`` reflects bytes
 *       queued for the data stage.
 * @post On failure the wire state is left to the controller's recovery
 *       (status-stage NAK / STALL).
 *
 * @note Runs on the USBX host task context.
 * @since 0.1.0
 */
static unsigned int internal_control_xfer(struct UX_TRANSFER_STRUCT* tr)
{
  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)tr->ux_transfer_request_function,
    .b_request       = (uint8_t)tr->ux_transfer_request_type,
    .w_value         = (uint16_t)tr->ux_transfer_request_value,
    .w_index         = (uint16_t)tr->ux_transfer_request_index,
    .w_length        = (uint16_t)tr->ux_transfer_request_requested_length,
  };
  if (ra8_usb_host_setup_request(s_hcd.speed, &setup) != k_ra8_ok) {
    return UX_TRANSFER_ERROR;
  }
  if (tr->ux_transfer_request_requested_length != 0U &&
      tr->ux_transfer_request_data_pointer != nullptr) {
    uint16_t len = (uint16_t)tr->ux_transfer_request_requested_length;
    if ((setup.bm_request_type & (uint8_t)k_ra8_usb_hcd_ep_addr_dir_in_bit) != 0U) {
      if (ra8_usb_queue_out(s_hcd.speed, 0U, tr->ux_transfer_request_data_pointer, &len, true) !=
          k_ra8_ok) {
        return UX_TRANSFER_ERROR;
      }
      tr->ux_transfer_request_actual_length = len;
    } else {
      if (ra8_usb_queue_in(s_hcd.speed, 0U, tr->ux_transfer_request_data_pointer, len) !=
          k_ra8_ok) {
        return UX_TRANSFER_ERROR;
      }
      tr->ux_transfer_request_actual_length = len;
    }
  }
  return UX_SUCCESS;
}

/**
 * @brief Drive a bulk / interrupt transfer on a non-EP0 pipe.
 *
 * @details IN endpoints (``ep_addr & 0x80U``) read from the device and
 * fill the caller buffer via ``ra8_usb_queue_out`` (the host-side OUT
 * direction is "data flowing out of the chip into caller memory"). OUT
 * endpoints write caller bytes to the device via ``ra8_usb_queue_in``.
 *
 * @param[in,out] tr USBX transfer request.
 * @param[in] ep_addr Endpoint address (bit 7 = direction).
 * @param[in] pipe Pipe index in 1..9.
 *
 * @return UX_SUCCESS or UX_TRANSFER_ERROR.
 * @retval UX_SUCCESS Bytes queued; ``actual_length`` updated.
 * @retval UX_TRANSFER_ERROR Underlying queue call failed.
 *
 * @pre ``tr`` non-null with a valid data pointer.
 * @pre ``pipe`` in 1..9 (caller already filtered EP0 / out-of-range).
 * @post ``actual_length`` reflects bytes queued on success.
 * @post No state change on failure.
 *
 * @note Runs on the USBX host task context.
 * @since 0.1.0
 */
static unsigned int internal_bulk_xfer(struct UX_TRANSFER_STRUCT* tr, uint8_t ep_addr, uint8_t pipe)
{
  if ((ep_addr & (uint8_t)k_ra8_usb_hcd_ep_addr_dir_in_bit) != 0U) {
    uint16_t len = (uint16_t)tr->ux_transfer_request_requested_length;
    if (ra8_usb_queue_out(s_hcd.speed, pipe, tr->ux_transfer_request_data_pointer, &len, true) !=
        k_ra8_ok) {
      return UX_TRANSFER_ERROR;
    }
    tr->ux_transfer_request_actual_length = len;
  } else {
    const uint16_t len = (uint16_t)tr->ux_transfer_request_requested_length;
    if (ra8_usb_queue_in(s_hcd.speed, pipe, tr->ux_transfer_request_data_pointer, len) !=
        k_ra8_ok) {
      return UX_TRANSFER_ERROR;
    }
    tr->ux_transfer_request_actual_length = len;
  }
  return UX_SUCCESS;
}

/**
 * @brief Transfer request.
 *
 * @details See implementation for details.
 *
 * @param[in,out] tr See function signature for type and usage.
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
static unsigned int internal_transfer_request(struct UX_TRANSFER_STRUCT* tr)
{
  if (tr == nullptr || tr->ux_transfer_request_endpoint == nullptr) {
    return UX_TRANSFER_ERROR;
  }
  UX_ENDPOINT*  ep      = tr->ux_transfer_request_endpoint;
  const uint8_t ep_addr = (uint8_t)ep->ux_endpoint_descriptor.bEndpointAddress;
  const uint8_t pipe    = internal_ep_to_pipe(ep_addr);

  if (pipe == 0U) {
    return internal_control_xfer(tr);
  }
  if (pipe >= (uint8_t)k_ra8_usb_hcd_pipe_count) {
    return UX_TRANSFER_ERROR;
  }
  return internal_bulk_xfer(tr, ep_addr, pipe);
}

/* -------------------------------------------------------------------------- */
/* Dispatcher */
/* -------------------------------------------------------------------------- */

/**
 * @brief Pulse the root-hub port reset (assert then deassert).
 *
 * @details USBX leaves the spec-required 10 ms hold between the assert
 * and deassert edges to the caller (it issues two separate
 * ``UX_HCD_RESET_PORT`` calls with the hold in between for some hosts,
 * or expects the HCD to short-circuit it for others). Here we just
 * trampoline both edges into ``ra8_usb_host_bus_reset``.
 *
 * @return UX_SUCCESS or UX_ERROR.
 * @retval UX_SUCCESS Both edges accepted.
 * @retval UX_ERROR Either edge rejected by the controller.
 *
 * @pre HCD bridge is past ``ux_hcd_ra8_usb_initialize``.
 * @pre Caller observes the USB 2.0 timing requirements.
 * @post Bus reset signalling has completed on the wire.
 * @post No bridge-side state mutated.
 *
 * @note Runs on the USBX host task context.
 * @since 0.1.0
 */
static unsigned int internal_port_reset(void)
{
  if (ra8_usb_host_bus_reset(s_hcd.speed, true) != k_ra8_ok) {
    return UX_ERROR;
  }
  if (ra8_usb_host_bus_reset(s_hcd.speed, false) != k_ra8_ok) {
    return UX_ERROR;
  }
  return UX_SUCCESS;
}

/**
 * @brief Enable or disable the root-hub port via DVSTCTR0.UACT.
 *
 * @details Wraps ``ra8_usb_host_set_uact`` so the dispatcher's
 * enable/disable cases collapse to a one-line call. On enable the
 * bridge moves into ``active``; on disable it returns to ``ready``.
 *
 * @param[in] enable True to assert UACT (SOF generation, downstream
 *                   bus activity), false to deassert.
 *
 * @return UX_SUCCESS or UX_ERROR.
 * @retval UX_SUCCESS UACT toggled and state updated.
 * @retval UX_ERROR ``ra8_usb_host_set_uact`` rejected the change.
 *
 * @pre HCD bridge is past ``ux_hcd_ra8_usb_initialize``.
 * @pre Caller serializes port-state mutations.
 * @post ``s_hcd.state`` reflects the new enable bit on success.
 * @post Wire is gated by UACT after this returns.
 *
 * @note Runs on the USBX host task context.
 * @since 0.1.0
 */
static unsigned int internal_port_set_enabled(bool enable)
{
  if (ra8_usb_host_set_uact(s_hcd.speed, enable) != k_ra8_ok) {
    return UX_ERROR;
  }
  s_hcd.state = enable ? k_ux_hcd_ra8_usb_state_active : k_ux_hcd_ra8_usb_state_ready;
  return UX_SUCCESS;
}

/**
 * @brief USBX host-side HCD function dispatcher.
 *
 * @details Stamped into ``UX_HCD::ux_hcd_entry_function`` during
 * ``ux_hcd_ra8_usb_initialize``. The USBX host stack calls this with a
 * ``UX_HCD_*`` selector to request endpoint create/destroy/reset,
 * transfer request, transfer abort, and root-hub port management
 * (reset / enable / disable). The trampoline routes each selector to
 * the matching ``internal_*`` helper.
 *
 * @param[in,out] hcd USBX HCD ownership block (currently unused; the
 *                    bridge keeps its own static state in ``s_hcd``).
 * @param[in] function USBX ``UX_HCD_*`` selector
 *                     (e.g. ``UX_HCD_TRANSFER_REQUEST``,
 *                     ``UX_HCD_RESET_PORT``).
 * @param[in,out] parameter Selector-dependent argument
 *                          (``UX_TRANSFER*`` / ``UX_ENDPOINT*`` /
 *                          opaque).
 *
 * @return USBX result code from the dispatched helper.
 * @retval UX_SUCCESS Function handled.
 * @retval UX_CONTROLLER_UNKNOWN Bridge has not been initialized.
 * @retval UX_ERROR Selector unsupported, or helper rejected the call.
 *
 * @pre Bridge is past ``ux_hcd_ra8_usb_initialize`` (or call returns
 *      ``UX_CONTROLLER_UNKNOWN``).
 * @pre Caller is the USBX host stack.
 * @post ``s_hcd`` updated per the dispatched selector.
 * @post Wire-side state may have been mutated (port reset / UACT toggle).
 *
 * @note Runs on the USBX host task context; not ISR-safe.
 * @since 0.1.0
 */
unsigned int
_ux_hcd_ra8_usb_function(struct UX_HCD_STRUCT* hcd, unsigned int function, void* parameter)
{
  (void)hcd;
  if (s_hcd.state == k_ux_hcd_ra8_usb_state_uninit) {
    return UX_CONTROLLER_UNKNOWN;
  }

  switch (function) {
    case UX_HCD_CREATE_ENDPOINT:
      return internal_endpoint_create((UX_ENDPOINT*)parameter);

    case UX_HCD_DESTROY_ENDPOINT:
    case UX_HCD_RESET_ENDPOINT:
      return UX_SUCCESS;

    case UX_HCD_TRANSFER_REQUEST:
      return internal_transfer_request((UX_TRANSFER*)parameter);

    case UX_HCD_TRANSFER_ABORT:
      return UX_SUCCESS;

    case UX_HCD_RESET_PORT:
      return internal_port_reset();

    case UX_HCD_ENABLE_PORT:
      return internal_port_set_enabled(true);

    case UX_HCD_DISABLE_PORT:
    case UX_HCD_POWER_DOWN_PORT:
      return internal_port_set_enabled(false);

    case UX_HCD_POWER_ON_PORT:
      return UX_SUCCESS;

    case UX_HCD_GET_PORT_STATUS:
    case UX_HCD_GET_FRAME_NUMBER:
      if (parameter != nullptr) {
        *(unsigned long*)parameter = 0UL;
      }
      return UX_SUCCESS;

    case UX_HCD_PROCESS_DONE_QUEUE:
    case UX_HCD_DISABLE_CONTROLLER:
    case UX_HCD_UNINITIALIZE:
      return UX_SUCCESS;

    default:
      return UX_FUNCTION_NOT_SUPPORTED;
  }
}

/* -------------------------------------------------------------------------- */
/* Lifecycle */
/* -------------------------------------------------------------------------- */

/**
 * @brief Ux hcd ra usb initialize.
 *
 * @details See implementation for details.
 *
 * @param[in,out] speed See function signature for type and usage.
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
ra8_err_t ux_hcd_ra8_usb_initialize(ra8_usb_speed_t speed)
{
  if ((uint8_t)speed > (uint8_t)k_ra8_usb_speed_hs) {
    return k_ra8_err_invalid_arg;
  }
  RA8_RETURN_ON_ERROR(ra8_usb_host_init(speed), s_tag, "ra8_usb_host_init");

  if (_ux_system_host == UX_NULL || _ux_system_host->ux_system_host_hcd_array == UX_NULL) {
    return k_ra8_err_invalid_state;
  }

  /* Pick the first free HCD slot. UX_MAX_HCD defaults to 1 in
   * ux_port.h; we walk up to UX_SYSTEM_HOST_MAX_HCD_GET() entries. */
  UX_HCD*    slot     = _ux_system_host->ux_system_host_hcd_array;
  const UINT max_hcds = UX_SYSTEM_HOST_MAX_HCD_GET();
  for (UINT i = 0; i < max_hcds; i++) {
    if (slot->ux_hcd_status == UX_HCD_STATUS_UNUSED) {
      break;
    }
    slot++;
  }
  slot->ux_hcd_status              = UX_HCD_STATUS_OPERATIONAL;
  slot->ux_hcd_controller_type     = (UINT)k_ra8_usb_hcd_controller_id;
  slot->ux_hcd_nb_root_hubs        = 1U;
  slot->ux_hcd_entry_function      = _ux_hcd_ra8_usb_function;
  slot->ux_hcd_controller_hardware = (void*)&s_hcd;

  s_hcd.owner = slot;
  s_hcd.speed = speed;
  s_hcd.state = k_ux_hcd_ra8_usb_state_ready;

  ra8_log_info(s_tag, "HCD bridge installed");
  return k_ra8_ok;
}

/**
 * @brief Ux hcd ra usb uninitialize.
 *
 * @details See implementation for details.
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
ra8_err_t ux_hcd_ra8_usb_uninitialize(void)
{
  if (s_hcd.state == k_ux_hcd_ra8_usb_state_uninit) {
    return k_ra8_err_invalid_state;
  }
  (void)ra8_usb_host_set_uact(s_hcd.speed, false);
  (void)ra8_usb_host_deinit(s_hcd.speed);
  if (s_hcd.owner != nullptr) {
    s_hcd.owner->ux_hcd_status         = UX_HCD_STATUS_HALTED;
    s_hcd.owner->ux_hcd_entry_function = nullptr;
  }
  s_hcd.state = k_ux_hcd_ra8_usb_state_uninit;
  s_hcd.owner = nullptr;
  return k_ra8_ok;
}

/**
 * @brief Ux hcd ra usb state.
 *
 * @details See implementation for details.
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
ra8_usb_hcd_state_t ux_hcd_ra8_usb_state(void)
{
  return s_hcd.state;
}
