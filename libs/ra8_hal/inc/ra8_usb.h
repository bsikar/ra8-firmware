/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_usb.h
 * @brief Native USB controller driver public API (device + host modes)
 * @ingroup grp_hal_usb
 *
 * @details
 * Hand-written, FSP-equivalent driver for the two USB controllers on
 * the Renesas RA8D2 (USBFS @ 0x40250000, USBHS @ 0x40351000). Mirrors
 * the bring-up flow of FSP's `r_usb_basic` /
 * `r_usb_pdriver.c` (device) and `r_usb_hreg_access.c` (host) but
 * compiled as part of this tree, with no Renesas FSP, CherryUSB, or
 * TinyUSB binaries pulled in. The class-layer splits live in
 * `ra8_usb_cdc.{h,c}` (device-side CDC ACM) and `ra8_usb_hcdc.{h,c}`
 * (host-side CDC ACM).
 *
 * ## Surface
 *
 * - **Device-mode lifecycle** -- `ra8_usb_device_init` / `_deinit`,
 *   plus power helpers `ra8_usb_enter_stop` / `_exit_stop`.
 * - **Host-mode lifecycle** -- `ra8_usb_host_init` / `_deinit`,
 *   `ra8_usb_host_bus_reset`, `ra8_usb_host_set_uact`,
 *   `ra8_usb_host_setup_request`.
 * - **Bus** -- `ra8_usb_device_attach` (raises D+ pull-up).
 * - **State** -- `ra8_usb_get_device_state`, `ra8_usb_set_address`.
 * - **Endpoints** -- `ra8_usb_configure_endpoint`, `ra8_usb_queue_in`,
 *   `ra8_usb_queue_out`, `ra8_usb_stall_endpoint`.
 * - **Control transfers** -- `ra8_usb_read_setup_if_valid` /
 *   `ra8_usb_read_setup_unconditional`,
 *   `ra8_usb_control_response`.
 * - **IRQ delivery** -- `ra8_usb_attach_handler` + `ra8_usb_dispatch`.
 *
 * This header is a thin umbrella: the shared public types and the
 * device-mode surface live in `ra8_usb_device.h`, and the host-mode
 * surface lives in `ra8_usb_host.h`. Consumers include `ra8_usb.h` and
 * get the complete API unchanged.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_usb_device.h"
#include "ra8_usb_host.h"

#ifdef __cplusplus
}
#endif
