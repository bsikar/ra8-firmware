/**
 * @file ra8_io_blockdev_usbmsc.h
 * @brief ra8_io block-device backend over a hosted USB mass-storage device.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The USB-MSC backend exposes a mass-storage device the board is hosting --
 * driven over Bulk-Only Transport by the `ra8_usb_hmsc` class layer -- as the
 * fabric's 512-byte logical-block device. A thumb drive therefore substitutes
 * for an SD card, OSPI NOR or MRAM behind the same ::ra8_io_blockdev_t, and a
 * filesystem mounted on one mounts unchanged on the others.
 *
 * The class layer owns a single attached device, but that device may expose
 * several logical units, so the caller-owned state carries the LUN this handle
 * addresses. Bind one handle per LUN to reach two units of the same device.
 *
 * Two transport limits live here rather than in every consumer. SCSI READ(10)
 * and WRITE(10) carry a 16-bit TRANSFER LENGTH, so a request longer than
 * ::k_ra8_io_usbmsc_max_transfer_blocks is refused rather than truncated.
 * READ CAPACITY(10) may report a native block size other than 512, which the
 * fabric's fixed logical block cannot represent, so ::ra8_io_blockdev_get_caps
 * refuses such a device instead of reporting a capacity in the wrong unit. The
 * 32-bit LBA those commands carry needs no clamp here: the fabric addresses
 * 32-bit LBAs too, and the widening to the 64-bit `ra8_fs` seam happens once
 * in ::ra8_io_blockdev_as_fs_backend.
 *
 * The application must have brought the class layer up with
 * ::ra8_usb_hmsc_init and seen the attach callback fire before any I/O flows
 * through the bound handle; the bind helper only wires the dispatch table and
 * issues no bus traffic. USB mass storage exposes no host erase primitive, so
 * the vtable's `erase` slot is NULL and the fabric maps an erase request to
 * ::k_ra8_err_not_supported.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_io_blockdev.h"

/**
 * @enum ra8_io_blockdev_usbmsc_limit_t
 * @brief Largest block run one SCSI command can carry on this transport.
 *
 * @details
 * The bound comes from the SCSI command definitions the `ra8_usb_hmsc` class
 * layer issues rather than from the fabric: READ(10) and WRITE(10) (SBC-4
 * sections 5.11 and 5.31) carry a 16-bit TRANSFER LENGTH field. A consumer
 * that wants to move more than this in one call must split the run itself.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_io_usbmsc_max_transfer_blocks = 65535, /**< READ(10)/WRITE(10) block ceiling. */
} ra8_io_blockdev_usbmsc_limit_t;

/**
 * @struct ra8_io_blockdev_usbmsc_state_t
 * @brief Caller-owned private state for a USB mass-storage block device.
 *
 * @details
 * Zero-initialise and pass to ::ra8_io_blockdev_usbmsc_init, which fills it and
 * binds it into a ::ra8_io_blockdev_t. Treat the field as private. The state
 * must out-live every call made through the device. The attached device itself
 * is owned by the `ra8_usb_hmsc` class layer, so nothing but the LUN selection
 * lives here.
 *
 * @invariant `lun` is within the class layer's `k_ra8_hmsc_max_lun` ceiling.
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t lun; /**< SCSI logical unit this handle addresses (private). */
} ra8_io_blockdev_usbmsc_state_t;

/**
 * @brief Bind a hosted USB mass-storage backend into a caller-owned handle.
 *
 * @details
 * Records `lun` in `state` and points `bd` at the USB-MSC vtable with `state`
 * as its context. No allocation and no bus traffic occur; the caller owns both
 * `bd` and `state`. The device itself must already be enumerated -- the
 * application calls ::ra8_usb_hmsc_init and waits for the attach callback --
 * before any I/O is issued through the bound handle.
 *
 * @param[out] bd    Handle to bind (zero-initialised by the caller).
 * @param[out] state Caller-owned backend state to populate.
 * @param[in]  lun   Logical unit to address, within `k_ra8_hmsc_max_lun`.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Backend bound; `bd` is usable once attached.
 * @retval k_ra8_err_null_ptr     `bd` or `state` was NULL.
 * @retval k_ra8_err_out_of_range `lun` exceeds the class layer's LUN ceiling.
 *
 * @pre `bd` and `state` out-live every call made through the device.
 * @pre The application enumerates the device before issuing I/O through `bd`.
 * @post On success `bd` dispatches to the USB-MSC backend for `lun`.
 * @post On any non-ok return `bd` and `state` are left unbound/untouched.
 *
 * @note Not thread-safe with respect to the same device or the class layer.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_blockdev_usbmsc_init(ra8_io_blockdev_t*              bd,
                                                    ra8_io_blockdev_usbmsc_state_t* state,
                                                    uint8_t                         lun);

#ifdef __cplusplus
}
#endif
