/**
 * @file port/usbx/inc/ux_hcd_ra8_usb.h
 * @brief USBX host-controller-driver (HCD) bridge to ra8_usb.
 *
 * @par Tag
 * [Ring 5 / PORT] {World: S}
 *
 * @details
 * Plumbs Eclipse USBX's host stack onto the project's hand-written
 * ``ra8_usb_*`` register-level driver in host mode (libs/ra8_hal/inc/ra8_usb.h
 * -- ``ra8_usb_host_init`` / ``ra8_usb_host_setup_request`` / etc.). The
 * shim implements the dispatch contract that USBX expects to find
 * behind ``UX_HCD::ux_hcd_entry_function``.
 *
 * ## Surface
 *
 * - ``ux_hcd_ra8_usb_initialize`` -- bring up the host controller via
 *   ``ra8_usb_host_init``, install ``_ux_hcd_ra8_usb_function`` as the
 *   dispatch trampoline, register a single root-hub port.
 * - ``ux_hcd_ra8_usb_uninitialize`` -- drop the controller.
 *
 * The dispatch table ``_ux_hcd_ra8_usb_function`` switches on the
 * ``UX_HCD_*`` command codes:
 *
 *   - ``UX_HCD_CREATE_ENDPOINT``    -> ra8_usb_configure_endpoint
 *   - ``UX_HCD_DESTROY_ENDPOINT``   -> NAK pipe
 *   - ``UX_HCD_TRANSFER_REQUEST``   -> ra8_usb_host_setup_request /
 *                                      ra8_usb_queue_in /
 *                                      ra8_usb_queue_out
 *   - ``UX_HCD_TRANSFER_ABORT``     -> NAK pipe
 *   - ``UX_HCD_RESET_PORT``         -> ra8_usb_host_bus_reset
 *   - ``UX_HCD_ENABLE_PORT``        -> ra8_usb_host_set_uact(true)
 *   - ``UX_HCD_DISABLE_PORT``       -> ra8_usb_host_set_uact(false)
 *   - ``UX_HCD_GET_PORT_STATUS``    -> read SYSSTS0
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_usb.h"

struct UX_HCD_STRUCT;
struct UX_ENDPOINT_STRUCT;
struct UX_TRANSFER_STRUCT;

/**
 * @enum ra8_usb_hcd_state_t
 * @brief Run-state of the HCD bridge.
 */
typedef enum : uint8_t {
  k_ux_hcd_ra8_usb_state_uninit = 0U, /**< Bridge not yet initialized. */
  k_ux_hcd_ra8_usb_state_ready  = 1U, /**< Bridge installed; bus idle. */
  k_ux_hcd_ra8_usb_state_active = 2U, /**< SOF generation enabled.     */
} ra8_usb_hcd_state_t;

/**
 * @brief Register the ra8_usb host bridge as the active USBX HCD.
 *
 * @details
 * Wraps ``ra8_usb_host_init`` and registers a UX_HCD entry into the
 * USBX host system table so subsequent ``ux_host_class_*_register``
 * calls find this bridge.
 *
 * @param[in] speed Which controller (FS or HS).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 HCD installed; bus idle (UACT=0).
 * @retval k_ra8_err_invalid_arg    ``speed`` out of range.
 * @retval k_ra8_err_hw_init_failed ``ra8_usb_host_init`` failed.
 *
 * @pre ``_ux_system_initialize`` and ``_ux_host_stack_initialize`` ran.
 *
 * @post UACT=0; caller raises it via the dispatch table once a
 *       device is detected on SYSSTS0.LNST.
 *
 * @note Not thread-safe.
 * @see ux_hcd_ra8_usb_uninitialize
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ux_hcd_ra8_usb_initialize(ra8_usb_speed_t speed);

/**
 * @brief Tear down the ra8_usb HCD bridge.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Bridge released.
 * @retval k_ra8_err_invalid_state  Bridge was never initialized.
 *
 * @pre ``ux_hcd_ra8_usb_initialize`` previously succeeded.
 *
 * @post Controller MSTP-gated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ux_hcd_ra8_usb_uninitialize(void);

/**
 * @brief Read the bridge run-state. Test-only.
 *
 * @return Current ``ra8_usb_hcd_state_t``.
 *
 * @pre None.
 * @post No state mutated.
 *
 * @note Thread-safe (single 8-bit load).
 * @since 0.1.0
 *
 * @details See implementation for details.
 * @retval 0 Success or default value.
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 */
ra8_usb_hcd_state_t ux_hcd_ra8_usb_state(void);

/* Dispatcher exposed for direct invocation and unit tests. */
/**
 * @brief  ux hcd ra usb function.
 *
 * @details See implementation for details.
 *
 * @param[in,out] hcd See function signature for type and usage.
 * @param[in,out] function See function signature for type and usage.
 * @param[in,out] parameter See function signature for type and usage.
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
unsigned int
_ux_hcd_ra8_usb_function(struct UX_HCD_STRUCT* hcd, unsigned int function, void* parameter);

#ifdef __cplusplus
}
#endif
