/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/tz_nsc_cgc_usb/ns_usb_internal.h
 * @brief Private split-seam shared between ns_usb.c and ns_usb_host.c (#96).
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * The Non-Secure USB self-loop image is split across two translation units for
 * the 1000-line file-size cap: ``ns_usb.c`` holds the USBFS CDC-ACM device
 * (descriptors, callbacks, USBX bring-up, the device worker, and the single
 * ``tx_application_define``), while ``ns_usb_host.c`` holds the USBHS polled
 * host ladder (enumerate + bulk echo). ``tx_application_define`` lives in
 * ``ns_usb.c`` but spawns the host worker defined in ``ns_usb_host.c``, so this
 * header forwards the host worker entry, its ThreadX thread storage, and the
 * thread tunables both TUs need. Everything else stays file-local in its own
 * TU.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "tx_api.h"

/**
 * @enum ns_host_cfg_t
 * @brief HS host worker thread storage geometry + the shared ThreadX time-slice.
 * @details Shared by both TUs: ``ns_usb_host.c`` sizes ::s_ns_host_stack with
 *          ::k_ns_host_stack_bytes, and ``tx_application_define`` (in
 *          ``ns_usb.c``) passes both values to ``tx_thread_create`` for the host
 *          worker. The same ::k_ns_time_slice is also applied to the device
 *          worker so the two round-robin each tick.
 * @invariant ::k_ns_host_stack_bytes is a multiple of 8 (ThreadX stack align).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ns_host_stack_bytes = 8192U, /**< HS host worker stack (bytes).          */
  k_ns_time_slice       = 1U,    /**< ThreadX time-slice (ticks) per worker. */
} ns_host_cfg_t;

/**
 * @brief Non-Secure ``ra8_delay_ms`` -- sleep ``ms`` ThreadX ticks.
 * @details Defined in ns_usb.c (the NS image deliberately drops ra8_time.c). The
 *          full contract lives on the ra8_time.h declaration; this seam exists so
 *          the host ladder in ns_usb_host.c can reach the ThreadX-backed delay.
 * @param[in] ms Milliseconds to block (0 is rounded up to one tick).
 * @return void.
 * @pre Called from ThreadX thread context (the USB / host worker), not an ISR.
 * @pre The ThreadX scheduler is running (1 ms tick live).
 * @post The caller blocked for at least ``ms`` ticks.
 * @post No SysTick reconfiguration occurs.
 * @note Not callable from interrupt context.
 * @see ra8_time_ms()
 * @since 0.1.0
 */
void ra8_delay_ms(uint32_t ms);

/**
 * @brief Non-Secure ``ra8_time_ms`` -- monotonic millisecond clock from ThreadX.
 * @details Defined in ns_usb.c. The full contract lives on the ra8_time.h
 *          declaration; this seam exists so the host ladder in ns_usb_host.c can
 *          read the millisecond clock for its attach timeout.
 * @return Milliseconds since the ThreadX scheduler started.
 * @retval 0 Immediately after the kernel starts.
 * @pre The ThreadX scheduler is running.
 * @pre Called from thread context.
 * @post No state changes (pure read of the kernel tick).
 * @post The return value is monotonic between wraps (~49 days).
 * @note Thread-safe (single-word kernel read).
 * @see ra8_delay_ms()
 * @since 0.1.0
 */
uint32_t ra8_time_ms(void);

/**
 * @var s_ns_host_thread
 * @brief ThreadX TCB for the HS host worker (defined in ns_usb_host.c).
 * @details Spawned by ``tx_application_define`` in ns_usb.c; the host worker
 *          itself runs from ns_usb_host.c. Declared here so the spawner TU can
 *          reference the storage.
 * @note Single-writer (ThreadX in the NS image).
 * @warning Do not modify outside ThreadX; J-Link / spawner reference only.
 * @since 0.1.0
 */
extern TX_THREAD s_ns_host_thread;

/**
 * @var s_ns_host_stack
 * @brief Stack backing storage for ::s_ns_host_thread (defined in ns_usb_host.c).
 * @details Sized by ::k_ns_host_stack_bytes; passed verbatim by the spawner.
 * @note Owned by ThreadX once the worker is created.
 * @warning Do not write outside the ThreadX stack frame.
 * @since 0.1.0
 */
extern UCHAR s_ns_host_stack[k_ns_host_stack_bytes];

/**
 * @var s_ns_host_thread_name
 * @brief HS host worker thread name (defined in ns_usb_host.c; ThreadX CHAR*).
 * @details Writable NS data because ThreadX's ``name_ptr`` field is ``CHAR*``.
 * @note Read by ThreadX / J-Link only.
 * @warning Do not mutate after thread creation.
 * @since 0.1.0
 */
extern CHAR s_ns_host_thread_name[];

/**
 * @brief HS host worker -- enumerate the looped FS device, then echo forever.
 * @details Defined in ns_usb_host.c; forwarded here so the single
 *          ``tx_application_define`` in ns_usb.c can pass it to
 *          ``tx_thread_create``. Enumerates the FS CDC device over the J7 loop
 *          cable (GET_DESCRIPTOR / SET_ADDRESS / SET_CONFIGURATION), opens the
 *          bulk pipes, then bulk round-trips a deterministic pattern forever,
 *          advancing ``g_tz_usb_host_rounds_ok`` (the HIL gate).
 * @param[in] arg Unused (ThreadX entry signature).
 * @return Never returns.
 * @pre ``tx_application_define`` auto-started this thread.
 * @pre The Secure boot delegated USBHS (PSARB12), set host mode + J7 VBUS, and
 *      enabled the UTMI PLL; the device worker is bringing the FS device up.
 * @post On success ``g_tz_usb_host_rounds_ok`` advances forever (HIL gate).
 * @post Each failed enumeration pass retries after a pause; the first error code
 *       is latched in ``g_tz_usb_host_err``.
 * @note Polled host (no IRQ); shares a time-sliced priority with the device.
 * @since 0.1.0
 */
RA8_PRIV VOID ns_host_worker(ULONG arg);
