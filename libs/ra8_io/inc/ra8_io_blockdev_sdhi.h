/**
 * @file ra8_io_blockdev_sdhi.h
 * @brief ra8_io block-device backend over the native SDHI SD-card controller.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The SDHI backend exposes an SD/SDHC/SDXC card -- driven over the RA8D2
 * 4-bit SDHI controller through the `ra8_sdcard` HAL driver -- as the fabric's
 * 512-byte logical-block device. It is the high-throughput counterpart to the
 * SD-over-SPI backend: both present the identical ::ra8_io_blockdev_iface, so a
 * filesystem mounted on one mounts unchanged on the other.
 *
 * The card is a singleton owned by `ra8_sdcard`, so this backend keeps no
 * per-device state: it binds the const vtable into a caller-owned handle with a
 * NULL context. The application must have brought the card up with
 * ::ra8_sdcard_init before any I/O flows through the bound handle. The card has
 * no host-visible erase primitive, so the vtable's `erase` slot is NULL and the
 * fabric maps an erase request to ::k_ra8_err_not_supported.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_err.h"
#include "ra8_io_blockdev.h"

/**
 * @brief Bind the native-SDHI SD-card backend into a caller-owned handle.
 *
 * @details
 * Points `bd` at the SDHI vtable with a NULL context (the card is a singleton
 * owned by `ra8_sdcard`). No allocation occurs; the caller owns `bd`. The card
 * itself must already have been initialised with ::ra8_sdcard_init -- this
 * helper only wires the dispatch table and does not touch the controller.
 *
 * @param[out] bd Handle to bind (zero-initialised by the caller).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Backend bound; `bd` is usable.
 * @retval k_ra8_err_null_ptr `bd` was NULL.
 *
 * @pre `bd` out-lives every call made through the device.
 * @pre ::ra8_sdcard_init has returned ::k_ra8_ok before any I/O through `bd`.
 * @post On success `bd` dispatches to the native-SDHI backend.
 * @post On any non-ok return `bd` is left untouched.
 *
 * @note Not thread-safe with respect to the same device.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_blockdev_sdhi_init(ra8_io_blockdev_t* bd);

#ifdef __cplusplus
}
#endif
