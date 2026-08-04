/**
 * @file port/levelx/inc/lx_filex_adapter.h
 * @brief FileX <-> LevelX adapter so FAT can ride a wear-levelled NOR partition
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Declares ``fx_media_driver_ra8_levelx`` -- a FileX media-driver entry
 * point that translates FileX block-device requests
 * (``FX_DRIVER_INIT`` / ``FX_DRIVER_READ`` / ``FX_DRIVER_WRITE`` /
 * ``FX_DRIVER_BOOT_READ`` / ``FX_DRIVER_BOOT_WRITE`` /
 * ``FX_DRIVER_FLUSH`` / ``FX_DRIVER_ABORT`` / ``FX_DRIVER_UNINIT`` /
 * ``FX_DRIVER_RELEASE_SECTORS``) into ``lx_nor_flash_sector_read`` /
 * ``lx_nor_flash_sector_write`` calls against a LevelX NOR-flash
 * control block.
 *
 * The shim lets a FAT volume (``fx_media_open()``) live on top of the
 * LevelX wear-levelling layer that already sits on top of
 * ``ra8_xspi_flash_*`` against the on-board EK-RA8D2 ISSI IS25LX512M
 * octal-SPI NOR flash. The user-visible result is "FAT-on-OSPI" with
 * dynamic block remapping -- exactly what the e-reader's local
 * library wants when no SD card is present.
 *
 * Stack layout:
 *
 * @code
 *   FileX (FAT, fx_media_open)
 *       |
 *       v
 *   fx_media_driver_ra8_levelx (this file)
 *       |
 *       v
 *   LevelX (lx_nor_flash_sector_read / write)
 *       |
 *       v
 *   lx_nor_driver_ra8_xspi (port/levelx/src/lx_nor_driver_ra8_xspi.c)
 *       |
 *       v
 *   ra8_xspi_flash_read / program / erase_sector
 *       |
 *       v
 *   IS25LX512M octal-SPI NOR (EK-RA8D2 on-board chip)
 * @endcode
 *
 * Sector geometry:
 *
 * - LevelX exposes 512-byte logical sectors
 *   (``LX_NOR_SECTOR_SIZE * sizeof(ULONG)`` = 512 bytes), which lines
 *   up exactly with the FAT12/16/32 sector size FileX uses by default.
 *   The two layers share the same ``ULONG``-aligned scratch buffer
 *   contract, so each FileX request maps 1:1 onto one LevelX call --
 *   no copy, no chunking.
 * - The LevelX partition reserves the topmost physical sectors for
 *   metadata; usable FAT space is therefore strictly less than the
 *   raw LevelX-managed window. Call
 *   ``lx_filex_adapter_get_total_sectors()`` to learn the safe total
 *   to pass to ``fx_media_format()``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "fx_api.h"
#include "lx_api.h"

/**
 * @brief Bind the FileX adapter to a LevelX NOR-flash control block.
 *
 * @details
 * Stores ``nor_flash`` as the module-private back-end pointer. All
 * subsequent ``fx_media_driver_ra8_levelx`` calls dispatch their
 * ``FX_DRIVER_READ`` / ``FX_DRIVER_WRITE`` requests against this
 * pointer. The caller retains ownership of the storage; the adapter
 * does not free it.
 *
 * Must be called before ``fx_media_open()`` / ``fx_media_format()``.
 *
 * @param[in] nor_flash LevelX NOR-flash control block already opened
 *   via ``lx_nor_flash_open``. Must not be NULL.
 *
 * @return ``FX_SUCCESS`` on success.
 * @retval FX_PTR_ERROR  ``nor_flash`` was NULL.
 * @retval FX_SUCCESS    Adapter is wired up.
 *
 * @pre ``nor_flash`` was successfully opened with ``lx_nor_flash_open``.
 * @pre The adapter is not currently servicing a FileX request.
 *
 * @post ``fx_media_driver_ra8_levelx`` may be passed to ``fx_media_open``.
 * @post Subsequent media driver calls translate to LevelX I/O.
 *
 * @note Not thread-safe; intended for single-threaded init.
 *
 * @since 0.1.0
 */
UINT lx_filex_adapter_bind(LX_NOR_FLASH* nor_flash);

/**
 * @brief Return the number of FAT-usable 512-byte sectors in the LevelX partition.
 *
 * @details
 * LevelX reserves one physical sector per block for its mapping
 * metadata. Out of every 8 physical sectors in a block, only 7 are
 * available for user data. This helper computes
 * ``total_blocks * (physical_sectors_per_block - 1)`` against the
 * currently-bound ``LX_NOR_FLASH`` and returns it for the caller to
 * pass to ``fx_media_format()``.
 *
 * @return Number of 512-byte sectors that FAT may safely use.
 * @retval 0 Adapter has not been bound, or bound flash is not open.
 *
 * @pre ``lx_filex_adapter_bind`` was called with a successfully open
 *      LevelX flash.
 *
 * @post The returned value is <= the bound flash's logical sector count.
 *
 * @note Pure helper, no side effects.
 *
 * @since 0.1.0
 *
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 */
ULONG lx_filex_adapter_get_total_sectors(void);

/**
 * @brief FileX media-driver entry point bridging onto LevelX.
 *
 * @details
 * Pass this function as the ``driver`` argument to ``fx_media_open()``
 * / ``fx_media_format()``. FileX invokes it once per sector-level I/O
 * request, indicating the request kind in
 * ``media->fx_media_driver_request`` and reporting completion via
 * ``media->fx_media_driver_status`` (set to ``FX_SUCCESS`` on success
 * or ``FX_IO_ERROR`` on any underlying LevelX failure).
 *
 * Request mapping:
 *
 * | FileX request                | Action                                                |
 * |------------------------------|-------------------------------------------------------|
 * | ``FX_DRIVER_INIT``           | confirm an LX_NOR_FLASH is bound, advertise geometry  |
 * | ``FX_DRIVER_UNINIT``         | drop the binding (no side-effects on the flash)       |
 * | ``FX_DRIVER_READ``           | one ``lx_nor_flash_sector_read`` per sector requested |
 * | ``FX_DRIVER_WRITE``          | one ``lx_nor_flash_sector_write`` per sector requested|
 * | ``FX_DRIVER_BOOT_READ``      | ``lx_nor_flash_sector_read(0, ...)``                  |
 * | ``FX_DRIVER_BOOT_WRITE``     | ``lx_nor_flash_sector_write(0, ...)``                 |
 * | ``FX_DRIVER_FLUSH``          | no-op (LevelX writes are synchronous)                 |
 * | ``FX_DRIVER_ABORT``          | no-op                                                 |
 * | ``FX_DRIVER_RELEASE_SECTORS``| no-op (no TRIM support)                               |
 *
 * @param[in,out] media FileX media control block. The driver reads
 *   ``fx_media_driver_request``, ``fx_media_driver_logical_sector``,
 *   ``fx_media_driver_sectors`` and ``fx_media_driver_buffer``, and
 *   writes ``fx_media_driver_status``.
 *
 * @pre ``media != NULL``.
 * @pre ``lx_filex_adapter_bind()`` has been called with an open
 *      ``LX_NOR_FLASH``.
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
void fx_media_driver_ra8_levelx(FX_MEDIA* media);

#ifdef __cplusplus
}
#endif
