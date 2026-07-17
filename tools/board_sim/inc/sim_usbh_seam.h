/**
 * @file sim_usbh_seam.h
 * @brief Virtual USB host-mode devices behind the ra8_usb_host_* / hmsc seams
 *
 * @details
 * board_usb.c models the USBFS controller in DEVICE mode (a virtual host
 * drives the firmware's device stack). The inverse case -- the firmware
 * acting as USB HOST -- drives the USBHS controller through the first-party
 * `ra8_usb_host_*` primitives, and with no peer the control transfer wedges.
 * Rather than model a second controller register-by-register, those
 * primitives are seamed to a virtual peer: a HID boot keyboard that answers
 * chapter-9 enumeration and streams interrupt-IN reports, or -- when the
 * firmware links the `ra8_usb_hmsc_*` class API -- a FAT16 MSC disk whose
 * one file MRAM.BIN is the live 1 MiB MRAM code window (read-only for the
 * browse app, writable with a small sector overlay for the file-ops app).
 *
 * Split out of the board_sim main translation unit; behaviour unchanged.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>
#include <unicorn/unicorn.h>

#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Install the virtual USB host-mode device seam if the host stack is linked.
 *
 * @details Picks the virtual device class from the firmware's linked host
 * stack: an MSC host (links `ra8_usb_hmsc_read10`) gets the FAT16 disk seamed
 * at the class API; otherwise a USB-host-capable firmware (links
 * `ra8_usb_host_control_xfer`) gets the HID boot keyboard seamed at the
 * primitives. Device-mode apps link neither call path, so the hooks are inert
 * there and board_usb.c's device-mode virtual host is untouched.
 *
 * @param[in,out] uc  Active Unicorn engine.
 * @param[in]     elf Loaded ELF image (symbol resolution).
 * @param[in]     len ELF image length in bytes.
 * @return true when a seam family was installed (the register-level USBHS
 *         host model must then stay dormant -- see board_usb_host.h).
 * @retval true  hmsc- or primitive-level seams now shadow the host API.
 * @retval false No usb-host seams; the register path is the real one.
 * @pre @p uc is initialised and @p elf holds @p len bytes.
 * @pre Not running under --usbhs-loop (the caller gates that mode).
 * @post On a host app, the linked host API answers a virtual device.
 * @note No effect on device-mode apps (the hooked symbols are never called).
 * @since 0.1.0
 */
RA8_PRIV bool usbh_seam_install(uc_engine* uc, const uint8_t* elf, long len);

/**
 * @brief Whether the virtual host-mode device served its final request.
 *
 * @details True once the virtual keyboard streamed its report quota, or the
 * read-only MSC disk saw the host's WRITE(10) probe (the step right before
 * usb_host_msc_browse's PASS) -- the BOARD_SIM_USBH_STOP early-stop
 * condition.
 *
 * @return true when the host-mode ladder has completed.
 * @retval false The virtual device is still being driven (or unused).
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV bool sim_usbh_done(void);

#ifdef __cplusplus
}
#endif
