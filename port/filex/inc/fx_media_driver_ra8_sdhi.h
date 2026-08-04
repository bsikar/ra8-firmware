/**
 * @file port/filex/inc/fx_media_driver_ra8_sdhi.h
 * @brief FileX media driver shim that bridges FileX onto the RA8 SDHI HAL
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Declares the single FileX media-driver entry point
 * ``fx_media_driver_ra8_sdhi`` that ``fx_media_open`` /
 * ``fx_media_format`` calls back into. The function handles the nine
 * FileX driver requests (READ_SECTOR, WRITE_SECTOR, INIT, FLUSH, ABORT,
 * BOOT_READ, BOOT_WRITE, RELEASE_SECTORS, UNINIT) by translating each
 * request into the matching ``ra8_sdhi_*`` block-driver call exported
 * from ``libs/ra8_hal/inc/ra8_sdhi.h``.
 *
 * Sector size is fixed at 512 bytes (the SD specification block size
 * the underlying ``ra8_sdhi_read_block`` / ``ra8_sdhi_write_block``
 * primitives operate on). FileX does not need to know which SDHI
 * instance is in use -- the driver hard-wires instance 0, which is the
 * channel routed to the EK-RA8D2 micro-SD slot.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* fx_api.h drags in the FX_MEDIA struct definition the function
 * signature below references. Including it here keeps consumers from
 * having to remember the include order. */
#include "fx_api.h"

/**
 * @brief FileX media-driver entry point bridging onto ``ra8_sdhi_*``.
 *
 * @details
 * Pass this function as the ``driver`` argument to ``fx_media_open()``
 * / ``fx_media_format()``. FileX invokes it once per sector-level
 * I/O request, indicating the request kind in
 * ``media->fx_media_driver_request`` and reporting completion via
 * ``media->fx_media_driver_status`` (set to ``FX_SUCCESS`` on
 * success or ``FX_IO_ERROR`` on any underlying SDHI failure).
 *
 * Request mapping:
 *
 * | FileX request                | Action                                 |
 * |------------------------------|----------------------------------------|
 * | ``FX_DRIVER_INIT``           | ``ra8_sdhi_init(0)``                    |
 * | ``FX_DRIVER_UNINIT``         | ``ra8_sdhi_deinit(0)``                  |
 * | ``FX_DRIVER_READ``           | ``ra8_sdhi_read_block(0, lba, buf, n)`` |
 * | ``FX_DRIVER_WRITE``          | ``ra8_sdhi_write_block(0, lba, buf, n)``|
 * | ``FX_DRIVER_BOOT_READ``      | ``ra8_sdhi_read_block(0, 0, buf, 1)``   |
 * | ``FX_DRIVER_BOOT_WRITE``     | ``ra8_sdhi_write_block(0, 0, buf, 1)``  |
 * | ``FX_DRIVER_FLUSH``          | no-op (PIO writes are synchronous)     |
 * | ``FX_DRIVER_ABORT``          | no-op                                  |
 * | ``FX_DRIVER_RELEASE_SECTORS``| no-op (no TRIM support)                |
 *
 * @param[in,out] media FileX media control block. The driver reads
 *   ``fx_media_driver_request``, ``fx_media_driver_logical_sector``,
 *   ``fx_media_driver_sectors`` and ``fx_media_driver_buffer``, and
 *   writes ``fx_media_driver_status`` (and on init, sector geometry).
 *
 * @pre ``media != NULL``.
 * @pre Card is electrically present in the SD slot; the SDHI MSTP
 *      gate is reachable.
 *
 * @post On success ``media->fx_media_driver_status == FX_SUCCESS``.
 * @post On failure ``media->fx_media_driver_status == FX_IO_ERROR``.
 *
 * @note Blocking, polled implementation. Must not be called from an
 *       ISR. FileX serialises calls via ``fx_media_protect`` so the
 *       driver itself does not need additional locking.
 *
 * @since 0.1.0
 */
void fx_media_driver_ra8_sdhi(FX_MEDIA* media);

#ifdef __cplusplus
}
#endif
