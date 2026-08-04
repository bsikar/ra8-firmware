/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/usbx/inc/ux_dcd_ra8_usb.h
 * @brief USBX device-controller-driver (DCD) bridge to ra8_usb.
 *
 * @par Tag
 * [Ring 5 / PORT] {World: S}
 *
 * @details
 * Plumbs Eclipse USBX's device stack onto the project's hand-written
 * ``ra8_usb_*`` register-level driver (libs/ra8_hal/inc/ra8_usb.h). The
 * shim implements the eight-function dispatch contract that USBX
 * expects to find behind ``UX_SLAVE_DCD::ux_slave_dcd_function`` and
 * is what ``_ux_device_stack_initialize`` ultimately reaches when the
 * application calls ``ux_device_class_cdc_acm_initialize`` and brings
 * a class up.
 *
 * ## Surface
 *
 * - ``ux_dcd_ra8_usb_initialize`` -- pull MSTP gate, program SYSCFG /
 *   DCP, install ``_ux_dcd_ra8_usb_function`` as the dispatch trampoline.
 * - ``ux_dcd_ra8_usb_uninitialize`` -- drop the controller.
 * - ``ux_dcd_ra8_usb_irq`` -- bottom-half hook for the USB ISR; reads
 *   ``INTSTS0``, decodes BRDY / BEMP / CTRT / DVST and posts the
 *   matching ``UX_SLAVE_TRANSFER`` completion semaphore.
 *
 * The dispatch table ``_ux_dcd_ra8_usb_function`` switches on the
 * ``UX_DCD_*`` command codes:
 *
 *   - ``UX_DCD_CREATE_ENDPOINT``     -> ``ra8_usb_configure_endpoint``
 *   - ``UX_DCD_DESTROY_ENDPOINT``    -> NAK pipe + clear PIPECFG
 *   - ``UX_DCD_RESET_ENDPOINT``      -> clear PID + toggle bit
 *   - ``UX_DCD_STALL_ENDPOINT``      -> ``ra8_usb_stall_endpoint``
 *   - ``UX_DCD_SET_DEVICE_ADDRESS``  -> ``ra8_usb_set_address``
 *   - ``UX_DCD_TRANSFER_REQUEST``    -> ``ra8_usb_queue_in`` /
 *                                      ``ra8_usb_queue_out`` /
 *                                      ``ra8_usb_control_response``
 *   - ``UX_DCD_TRANSFER_ABORT``      -> NAK pipe
 *   - ``UX_DCD_GET_FRAME_NUMBER``    -> read FRMNUM
 *   - ``UX_DCD_CHANGE_STATE``        -> bookkeeping (no HW)
 *
 * The bridge is intentionally thin: it never allocates, never owns
 * any host-stack state, and never blocks. Class-layer threads (the
 * CDC bulk-in / bulk-out threads in particular) supply the
 * ``UX_SLAVE_TRANSFER`` semaphore that ``ux_dcd_ra8_usb_irq`` posts on
 * BRDY completion.
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_usb.h"

/* USBX pulls in tx_api.h via ux_port.h; we forward-declare what we
 * touch so this header stays cheap to include from non-USBX TUs. */
struct UX_SLAVE_DCD_STRUCT;
struct UX_SLAVE_TRANSFER_STRUCT;
struct UX_SLAVE_ENDPOINT_STRUCT;

/**
 * @enum ra8_usb_dcd_state_t
 * @brief Run-state of the DCD bridge.
 */
typedef enum : uint8_t {
  k_ux_dcd_ra8_usb_state_uninit = 0U, /**< Bridge not yet initialized.   */
  k_ux_dcd_ra8_usb_state_ready  = 1U, /**< Bridge installed; bus idle.   */
  k_ux_dcd_ra8_usb_state_active = 2U, /**< Class-layer transfers active. */
} ra8_usb_dcd_state_t;

/**
 * @enum ra8_usb_dcd_limits_t
 * @brief Compile-time sizing of the bridge.
 *
 * @details Independent of ``UX_MAX_ED`` so we can statically pin the
 * pipe table without dragging in ``ux_api.h`` from this header.
 */
typedef enum : uint8_t {
  k_ux_dcd_ra8_usb_max_pipes = 10U, /**< DCP + 9 PIPE entries (HUM 36.1). */
} ra8_usb_dcd_limits_t;

/**
 * @brief Register the ra8_usb bridge as the active USBX DCD.
 *
 * @details
 * Wraps ``ra8_usb_device_init`` and stamps the global
 * ``_ux_system_slave -> ux_system_slave_dcd`` so subsequent
 * ``ux_device_class_*_initialize`` calls land on this bridge. The D+
 * pull-up is left off; the caller raises it via ``ra8_usb_device_attach``
 * / ``ra8_nsc_usb_attach`` once descriptors are wired up.
 *
 * @param[in] speed Which controller (FS or HS).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 DCD installed; ready for class init.
 * @retval k_ra8_err_invalid_arg    ``speed`` out of range.
 * @retval k_ra8_err_hw_init_failed ``ra8_usb_device_init`` failed.
 *
 * @pre ``_ux_system_initialize`` has run.
 * @pre Single-threaded init context.
 *
 * @post ``_ux_system_slave -> ux_system_slave_dcd.ux_slave_dcd_function``
 *       points at the bridge dispatcher.
 * @post ``ra8_usb_*`` controller is clocked, IRQs unmasked.
 *
 * @note Not thread-safe.
 * @see ux_dcd_ra8_usb_uninitialize
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ux_dcd_ra8_usb_initialize(ra8_usb_speed_t speed);

/**
 * @brief Tear down the ra8_usb DCD bridge.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Bridge released.
 * @retval k_ra8_err_invalid_state  Bridge was never initialized.
 *
 * @pre ``ux_dcd_ra8_usb_initialize`` previously succeeded.
 *
 * @post Controller MSTP-gated; subsequent USBX calls fail.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ux_dcd_ra8_usb_uninitialize(void);

/**
 * @brief ISR-context bottom-half. Decodes ``INTSTS0`` and posts the
 *        relevant USBX transfer semaphore.
 *
 * @details
 * Wired into the project's USB ISR via ``ra8_usb_attach_handler``
 * during ``ux_dcd_ra8_usb_initialize``. The handler walks the
 * BRDY / BEMP / CTRT bits, looks up the active
 * ``UX_SLAVE_TRANSFER``, copies bytes in / out of the pipe FIFO via
 * ``ra8_usb_queue_in`` / ``ra8_usb_queue_out``, and posts the request
 * semaphore on completion.
 *
 * @param[in] speed     Which controller fired (passed through from
 *                      ``ra8_usb_dispatch``).
 * @param[in] intsts0   Snapshot of ``INTSTS0`` at the time of the IRQ.
 *
 * @pre Bridge is in ``k_ux_dcd_ra8_usb_state_ready`` or ``_active``.
 *
 * @post ``INTSTS0`` event bits the bridge handles are cleared.
 *
 * @note Re-entrant only across speed instances (FS vs HS).
 * @since 0.1.0
 *
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 */
void ux_dcd_ra8_usb_irq(ra8_usb_speed_t speed, uint16_t intsts0);

/**
 * @brief Read the bridge run-state. Test-only; not part of the USBX
 *        contract.
 *
 * @return Current ``ra8_usb_dcd_state_t``.
 *
 * @pre None.
 *
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
ra8_usb_dcd_state_t ux_dcd_ra8_usb_state(void);

/**
 * @brief Enable ISR-side auto-echo from a bulk OUT pipe to a bulk IN pipe.
 *
 * @details
 * Workaround for a ThreadX scheduling failure on this silicon where the
 * application worker thread blocks indefinitely on tx_semaphore_get after
 * the CDC class activates. With auto-echo on, the bridge's
 * ::internal_irq_walk_pipe drains the OUT pipe in-place and re-queues the
 * data on the IN pipe entirely inside the USB IRQ -- no thread-mode
 * dispatch required.
 *
 * @param[in] out_pipe Pipe index of the bulk OUT endpoint (e.g. 2 for CDC).
 * @param[in] in_pipe  Pipe index of the bulk IN endpoint (e.g. 1 for CDC).
 *
 * @pre Both pipes have been configured via the USBX endpoint-create path
 *      (i.e. host has issued SET_CONFIGURATION).
 * @pre Called from task / startup context, not from inside an ISR.
 *
 * @post Subsequent BRDY events on ``out_pipe`` with no USBX waiter
 *       drive the auto-echo body and re-queue on ``in_pipe``.
 * @post Auto-echo counters under ``s_dcd_auto_echo_*`` start tracking.
 *
 * @note Bypasses USBX for the data path; do not combine with
 *       _ux_device_class_cdc_acm_read / _write on the same pipes.
 * @since 0.1.0
 */
void ux_dcd_ra8_usb_auto_echo_enable(uint8_t out_pipe, uint8_t in_pipe);

/**
 * @brief Re-enable the USB NVIC IRQ at the controller level.
 *
 * @details Call from a periodic context (SysTick, watchdog) to recover
 * after the ISR's spurious-entry path masked the line to stop a
 * USBR-driven IRQ storm. Writes ``1`` to NVIC ISER[0] bit 0.
 *
 * @pre ``ra8_isr`` mapped USBHS to NVIC IRQ 0 (project-wide invariant).
 * @pre Caller is the periodic re-arm context, not the storm-affected ISR.
 *
 * @post NVIC routes the next USBHS event to ::internal_usbhs_isr.
 * @post No other CPU / module state is changed.
 *
 * @note Idempotent; safe to call from polling context.
 * @since 0.1.0
 */
void ux_dcd_ra8_usb_irq_reenable(void);

/* Internal dispatcher exposed for direct invocation from
 * ``_ux_dcd_ra8_usb_initialize`` (in the .c file) and for unit tests. */
/**
 * @brief  ux dcd ra usb function.
 *
 * @details See implementation for details.
 *
 * @param[in,out] dcd See function signature for type and usage.
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
_ux_dcd_ra8_usb_function(struct UX_SLAVE_DCD_STRUCT* dcd, unsigned int function, void* parameter);

#ifdef __cplusplus
}
#endif
