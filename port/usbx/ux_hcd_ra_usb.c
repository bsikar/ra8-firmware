/**
 * @file port/usbx/ux_hcd_ra_usb.c
 * @brief USBX host-controller-driver bridge to ra_usb -- implementation.
 *
 * @par Tag
 * [Ring 5 / PORT] {World: S}
 *
 * @details
 * Counterpart to ``ux_dcd_ra_usb.c``: routes USBX host-stack
 * dispatch into the ``ra_usb_host_*`` register-level driver.
 * Mirrors the surface of upstream USBX HCD ports
 * (e.g. ``ux_hcd_ehci_entry``).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#define UX_SOURCE_CODE

#include "ux_hcd_ra_usb.h"

#include <stdint.h>
#include <string.h>

#include "ra_check.h"
#include "ra_log.h"
#include "ux_api.h"

static const char* const s_tag = "ux_hcd_ra_usb";

/**
 * @struct ra_usb_hcd_t
 * @brief Bridge-singleton state.
 */
typedef struct {
  ra_usb_hcd_state_t    state; /**< Bridge run-state.       */
  ra_usb_speed_t        speed; /**< Controller this drives. */
  struct UX_HCD_STRUCT* owner; /**< Back-pointer into USBX. */
} ra_usb_hcd_t;

/**
 * @var s_hcd
 * @brief Bridge instance. RA8D2 has two USB controllers but USBX
 * only ever drives one root-hub-style host at a time in this app.
 */
static ra_usb_hcd_t s_hcd = {
  .state = k_ux_hcd_ra_usb_state_uninit,
  .speed = k_ra_usb_speed_fs,
  .owner = nullptr,
};

/* -------------------------------------------------------------------------- */
/* Internal helpers                                                           */
/* -------------------------------------------------------------------------- */

static uint8_t internal_ep_to_pipe(uint8_t ep_addr)
{
  const uint8_t ep = ep_addr & (uint8_t)0x0FU;
  if (ep == 0U) {
    return 0U;
  }
  if (ep < 10U) {
    return ep;
  }
  return 10U;
}

static unsigned int internal_endpoint_create(struct UX_ENDPOINT_STRUCT* ep)
{
  if (ep == nullptr) {
    return UX_ERROR;
  }
  const uint8_t ep_addr = (uint8_t)ep->ux_endpoint_descriptor.bEndpointAddress;
  const uint8_t pipe    = internal_ep_to_pipe(ep_addr);
  if (pipe == 0U || pipe >= 10U) {
    /* DCP is configured during host_init. */
    return UX_SUCCESS;
  }

  ra_usb_ep_dir_t dir = ((ep_addr & 0x80U) != 0U) ? k_ra_usb_ep_dir_in : k_ra_usb_ep_dir_out;

  ra_usb_ep_type_t type;
  switch ((uint8_t)ep->ux_endpoint_descriptor.bmAttributes & 0x03U) {
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

  return (ra_usb_configure_endpoint(s_hcd.speed,
                                    pipe,
                                    (uint8_t)(ep_addr & 0x0FU),
                                    dir,
                                    type,
                                    (uint16_t)ep->ux_endpoint_descriptor.wMaxPacketSize) == k_ra_ok)
           ? UX_SUCCESS
           : UX_ERROR;
}

static unsigned int internal_transfer_request(struct UX_TRANSFER_STRUCT* tr)
{
  if (tr == nullptr || tr->ux_transfer_request_endpoint == nullptr) {
    return UX_TRANSFER_ERROR;
  }
  UX_ENDPOINT*  ep      = tr->ux_transfer_request_endpoint;
  const uint8_t ep_addr = (uint8_t)ep->ux_endpoint_descriptor.bEndpointAddress;
  const uint8_t pipe    = internal_ep_to_pipe(ep_addr);

  /* Control transfer on EP0 -- emit SETUP via ra_usb_host_setup_request,
   * then drive the optional data + status stages. */
  if (pipe == 0U) {
    ra_usb_setup_t setup = {
      .bm_request_type = (uint8_t)tr->ux_transfer_request_function,
      .b_request       = (uint8_t)tr->ux_transfer_request_type,
      .w_value         = (uint16_t)tr->ux_transfer_request_value,
      .w_index         = (uint16_t)tr->ux_transfer_request_index,
      .w_length        = (uint16_t)tr->ux_transfer_request_requested_length,
    };
    if (ra_usb_host_setup_request(s_hcd.speed, &setup) != k_ra_ok) {
      return UX_TRANSFER_ERROR;
    }
    if (tr->ux_transfer_request_requested_length != 0U &&
        tr->ux_transfer_request_data_pointer != nullptr) {
      uint16_t len = (uint16_t)tr->ux_transfer_request_requested_length;
      if ((setup.bm_request_type & 0x80U) != 0U) {
        if (ra_usb_queue_out(s_hcd.speed, 0U, tr->ux_transfer_request_data_pointer, &len) !=
            k_ra_ok) {
          return UX_TRANSFER_ERROR;
        }
        tr->ux_transfer_request_actual_length = len;
      } else {
        if (ra_usb_queue_in(s_hcd.speed, 0U, tr->ux_transfer_request_data_pointer, len) !=
            k_ra_ok) {
          return UX_TRANSFER_ERROR;
        }
        tr->ux_transfer_request_actual_length = len;
      }
    }
    return UX_SUCCESS;
  }

  if (pipe >= 10U) {
    return UX_TRANSFER_ERROR;
  }

  /* Bulk / interrupt: IN reads from device -> fill caller buffer.
   * OUT writes from caller buffer -> device. */
  if ((ep_addr & 0x80U) != 0U) {
    uint16_t len = (uint16_t)tr->ux_transfer_request_requested_length;
    if (ra_usb_queue_out(s_hcd.speed, pipe, tr->ux_transfer_request_data_pointer, &len) !=
        k_ra_ok) {
      return UX_TRANSFER_ERROR;
    }
    tr->ux_transfer_request_actual_length = len;
  } else {
    const uint16_t len = (uint16_t)tr->ux_transfer_request_requested_length;
    if (ra_usb_queue_in(s_hcd.speed, pipe, tr->ux_transfer_request_data_pointer, len) != k_ra_ok) {
      return UX_TRANSFER_ERROR;
    }
    tr->ux_transfer_request_actual_length = len;
  }
  return UX_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Dispatcher                                                                 */
/* -------------------------------------------------------------------------- */

unsigned int
_ux_hcd_ra_usb_function(struct UX_HCD_STRUCT* hcd, unsigned int function, void* parameter)
{
  (void)hcd;
  if (s_hcd.state == k_ux_hcd_ra_usb_state_uninit) {
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
      if (ra_usb_host_bus_reset(s_hcd.speed, true) != k_ra_ok) {
        return UX_ERROR;
      }
      /* USBX leaves the spec-required 10 ms hold to the caller. */
      if (ra_usb_host_bus_reset(s_hcd.speed, false) != k_ra_ok) {
        return UX_ERROR;
      }
      return UX_SUCCESS;

    case UX_HCD_ENABLE_PORT:
      if (ra_usb_host_set_uact(s_hcd.speed, true) != k_ra_ok) {
        return UX_ERROR;
      }
      s_hcd.state = k_ux_hcd_ra_usb_state_active;
      return UX_SUCCESS;

    case UX_HCD_DISABLE_PORT:
    case UX_HCD_POWER_DOWN_PORT:
      if (ra_usb_host_set_uact(s_hcd.speed, false) != k_ra_ok) {
        return UX_ERROR;
      }
      s_hcd.state = k_ux_hcd_ra_usb_state_ready;
      return UX_SUCCESS;

    case UX_HCD_POWER_ON_PORT:
      return UX_SUCCESS;

    case UX_HCD_GET_PORT_STATUS:
      if (parameter != nullptr) {
        *(unsigned long*)parameter = 0UL;
      }
      return UX_SUCCESS;

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
/* Lifecycle                                                                  */
/* -------------------------------------------------------------------------- */

ra_err_t ux_hcd_ra_usb_initialize(ra_usb_speed_t speed)
{
  if ((uint8_t)speed > (uint8_t)k_ra_usb_speed_hs) {
    return k_ra_err_invalid_arg;
  }
  RA_RETURN_ON_ERROR(ra_usb_host_init(speed), s_tag, "ra_usb_host_init");

  if (_ux_system_host == UX_NULL || _ux_system_host->ux_system_host_hcd_array == UX_NULL) {
    return k_ra_err_invalid_state;
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
  slot->ux_hcd_controller_type     = 99U;
  slot->ux_hcd_nb_root_hubs        = 1U;
  slot->ux_hcd_entry_function      = _ux_hcd_ra_usb_function;
  slot->ux_hcd_controller_hardware = (void*)&s_hcd;

  s_hcd.owner = slot;
  s_hcd.speed = speed;
  s_hcd.state = k_ux_hcd_ra_usb_state_ready;

  ra_log_info(s_tag, "HCD bridge installed");
  return k_ra_ok;
}

ra_err_t ux_hcd_ra_usb_uninitialize(void)
{
  if (s_hcd.state == k_ux_hcd_ra_usb_state_uninit) {
    return k_ra_err_invalid_state;
  }
  (void)ra_usb_host_set_uact(s_hcd.speed, false);
  (void)ra_usb_host_deinit(s_hcd.speed);
  if (s_hcd.owner != nullptr) {
    s_hcd.owner->ux_hcd_status         = UX_HCD_STATUS_HALTED;
    s_hcd.owner->ux_hcd_entry_function = nullptr;
  }
  s_hcd.state = k_ux_hcd_ra_usb_state_uninit;
  s_hcd.owner = nullptr;
  return k_ra_ok;
}

ra_usb_hcd_state_t ux_hcd_ra_usb_state(void)
{
  return s_hcd.state;
}
