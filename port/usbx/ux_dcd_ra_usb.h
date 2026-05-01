/**
 * @file port/usbx/ux_dcd_ra_usb.h
 * @brief USBX device-controller-driver (DCD) bridge to ra_usb.
 *
 * @par Tag
 * [Ring 5 / PORT] {World: S}
 *
 * @details
 * Plumbs Eclipse USBX's device stack onto the project's hand-written
 * ``ra_usb_*`` register-level driver (libs/ra_hal/inc/ra_usb.h). The
 * shim implements the eight-function dispatch contract that USBX
 * expects to find behind ``UX_SLAVE_DCD::ux_slave_dcd_function`` and
 * is what ``_ux_device_stack_initialize`` ultimately reaches when the
 * application calls ``ux_device_class_cdc_acm_initialize`` and brings
 * a class up.
 *
 * ## Surface
 *
 * - ``ux_dcd_ra_usb_initialize`` -- pull MSTP gate, program SYSCFG /
 *   DCP, install ``_ux_dcd_ra_usb_function`` as the dispatch trampoline.
 * - ``ux_dcd_ra_usb_uninitialize`` -- drop the controller.
 * - ``ux_dcd_ra_usb_irq`` -- bottom-half hook for the USB ISR; reads
 *   ``INTSTS0``, decodes BRDY / BEMP / CTRT / DVST and posts the
 *   matching ``UX_SLAVE_TRANSFER`` completion semaphore.
 *
 * The dispatch table ``_ux_dcd_ra_usb_function`` switches on the
 * ``UX_DCD_*`` command codes:
 *
 *   - ``UX_DCD_CREATE_ENDPOINT``     -> ``ra_usb_configure_endpoint``
 *   - ``UX_DCD_DESTROY_ENDPOINT``    -> NAK pipe + clear PIPECFG
 *   - ``UX_DCD_RESET_ENDPOINT``      -> clear PID + toggle bit
 *   - ``UX_DCD_STALL_ENDPOINT``      -> ``ra_usb_stall_endpoint``
 *   - ``UX_DCD_SET_DEVICE_ADDRESS``  -> ``ra_usb_set_address``
 *   - ``UX_DCD_TRANSFER_REQUEST``    -> ``ra_usb_queue_in`` /
 *                                      ``ra_usb_queue_out`` /
 *                                      ``ra_usb_control_response``
 *   - ``UX_DCD_TRANSFER_ABORT``      -> NAK pipe
 *   - ``UX_DCD_GET_FRAME_NUMBER``    -> read FRMNUM
 *   - ``UX_DCD_CHANGE_STATE``        -> bookkeeping (no HW)
 *
 * The bridge is intentionally thin: it never allocates, never owns
 * any host-stack state, and never blocks. Class-layer threads (the
 * CDC bulk-in / bulk-out threads in particular) supply the
 * ``UX_SLAVE_TRANSFER`` semaphore that ``ux_dcd_ra_usb_irq`` posts on
 * BRDY completion.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra_usb.h"

/* USBX pulls in tx_api.h via ux_port.h; we forward-declare what we
 * touch so this header stays cheap to include from non-USBX TUs. */
struct UX_SLAVE_DCD_STRUCT;
struct UX_SLAVE_TRANSFER_STRUCT;
struct UX_SLAVE_ENDPOINT_STRUCT;

/**
 * @enum ra_usb_dcd_state_t
 * @brief Run-state of the DCD bridge.
 */
typedef enum : uint8_t {
  k_ux_dcd_ra_usb_state_uninit = 0U, /**< Bridge not yet initialised.   */
  k_ux_dcd_ra_usb_state_ready  = 1U, /**< Bridge installed; bus idle.   */
  k_ux_dcd_ra_usb_state_active = 2U, /**< Class-layer transfers active. */
} ra_usb_dcd_state_t;

/**
 * @enum ra_usb_dcd_limits_t
 * @brief Compile-time sizing of the bridge.
 *
 * @details Independent of ``UX_MAX_ED`` so we can statically pin the
 * pipe table without dragging in ``ux_api.h`` from this header.
 */
typedef enum : uint8_t {
  k_ux_dcd_ra_usb_max_pipes = 10U, /**< DCP + 9 PIPE entries (HUM 36.1).  */
} ra_usb_dcd_limits_t;

/**
 * @brief Register the ra_usb bridge as the active USBX DCD.
 *
 * @details
 * Wraps ``ra_usb_device_init`` and stamps the global
 * ``_ux_system_slave -> ux_system_slave_dcd`` so subsequent
 * ``ux_device_class_*_initialize`` calls land on this bridge. The D+
 * pull-up is left off; the caller raises it via ``ra_usb_device_attach``
 * / ``ra_nsc_usb_attach`` once descriptors are wired up.
 *
 * @param[in] speed Which controller (FS or HS).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 DCD installed; ready for class init.
 * @retval k_ra_err_invalid_arg    ``speed`` out of range.
 * @retval k_ra_err_hw_init_failed ``ra_usb_device_init`` failed.
 *
 * @pre ``_ux_system_initialize`` has run.
 * @pre Single-threaded init context.
 *
 * @post ``_ux_system_slave -> ux_system_slave_dcd.ux_slave_dcd_function``
 *       points at the bridge dispatcher.
 * @post ``ra_usb_*`` controller is clocked, IRQs unmasked.
 *
 * @note Not thread-safe.
 * @see ux_dcd_ra_usb_uninitialize
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ux_dcd_ra_usb_initialize(ra_usb_speed_t speed);

/**
 * @brief Tear down the ra_usb DCD bridge.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Bridge released.
 * @retval k_ra_err_invalid_state  Bridge was never initialised.
 *
 * @pre ``ux_dcd_ra_usb_initialize`` previously succeeded.
 *
 * @post Controller MSTP-gated; subsequent USBX calls fail.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ux_dcd_ra_usb_uninitialize(void);

/**
 * @brief ISR-context bottom-half. Decodes ``INTSTS0`` and posts the
 *        relevant USBX transfer semaphore.
 *
 * @details
 * Wired into the project's USB ISR via ``ra_usb_attach_handler``
 * during ``ux_dcd_ra_usb_initialize``. The handler walks the
 * BRDY / BEMP / CTRT bits, looks up the active
 * ``UX_SLAVE_TRANSFER``, copies bytes in / out of the pipe FIFO via
 * ``ra_usb_queue_in`` / ``ra_usb_queue_out``, and posts the request
 * semaphore on completion.
 *
 * @param[in] speed     Which controller fired (passed through from
 *                      ``ra_usb_dispatch``).
 * @param[in] intsts0   Snapshot of ``INTSTS0`` at the time of the IRQ.
 *
 * @pre Bridge is in ``k_ux_dcd_ra_usb_state_ready`` or ``_active``.
 *
 * @post ``INTSTS0`` event bits the bridge handles are cleared.
 *
 * @note Re-entrant only across speed instances (FS vs HS).
 * @since 0.1.0
 */
void ux_dcd_ra_usb_irq(ra_usb_speed_t speed, uint16_t intsts0);

/**
 * @brief Read the bridge run-state. Test-only; not part of the USBX
 *        contract.
 *
 * @return Current ``ra_usb_dcd_state_t``.
 *
 * @pre None.
 *
 * @post No state mutated.
 *
 * @note Thread-safe (single 8-bit load).
 * @since 0.1.0
 */
ra_usb_dcd_state_t ux_dcd_ra_usb_state(void);

/* Internal dispatcher exposed for direct invocation from
 * ``_ux_dcd_ra_usb_initialize`` (in the .c file) and for unit tests. */
unsigned int
_ux_dcd_ra_usb_function(struct UX_SLAVE_DCD_STRUCT* dcd, unsigned int function, void* parameter);

#ifdef __cplusplus
}
#endif
