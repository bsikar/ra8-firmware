/**
 * @file port/levelx/inc/lx_fs_backend.h
 * @brief ra8_fs block-device backend over a LevelX wear-levelled NOR partition
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Declares ``lx_fs_backend_bind()`` -- the adapter that exposes an open
 * ``LX_NOR_FLASH`` as an ::ra8_fs_backend_t, so the first-party ``ra8_fs``
 * filesystem can format and mount a FAT volume on top of the LevelX
 * wear-levelling layer that already sits on top of ``ra8_xspi_flash_*``
 * against the on-board EK-RA8D2 ISSI IS25LX512M octal-SPI NOR flash.
 *
 * Stack layout:
 *
 * @code
 *   ra8_fs (FAT, ra8_fs_mount)
 *       |
 *       v
 *   lx_fs_backend (this file: read_block / write_block / get_capacity)
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
 * - LevelX exposes 512-byte logical sectors (``LX_NOR_SECTOR_SIZE *
 *   sizeof(ULONG)`` = 512 bytes), which is also the smallest sector size
 *   ``ra8_fs`` supports, so each backend block maps 1:1 onto one LevelX call.
 * - The capacity this backend reports is ``total_blocks *
 *   (physical_sectors_per_block - 1)``: LevelX's own per-block sector count
 *   (which already excludes its one metadata sector per block) minus one more
 *   sector per block as wear-levelling headroom -- the same margin the
 *   retired FileX adapter advertised to ``fx_media_format``.
 * - A logical sector LevelX has never mapped is erased space, so a read of it
 *   fills the caller's buffer with the NOR erased value ``0xFF`` and succeeds
 *   rather than failing -- exactly what a raw block device would return.
 * - No ``erase_blocks`` callback is offered: ``ra8_fs`` only uses an erase
 *   that guarantees a ZERO read-back, and NOR (via LevelX) erases to ones.
 *   The formatter falls back to writing zeros, which is correct here.
 *
 * The adapter is stateless apart from one static sector bounce buffer used to
 * satisfy LevelX's ``ULONG``-aligned buffer contract for arbitrary caller
 * pointers; ``ra8_fs`` serialises every backend call (see
 * ``ra8_fs_set_lock()``), so the buffer is never shared between two calls.
 *
 * @author Brighton Sikarskie
 * @date 2026-08-10
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lx_api.h"
#include "ra8_err.h"
#include "ra8_fs.h"

/**
 * @brief Expose an open LevelX NOR flash as an ``ra8_fs`` block-device backend.
 *
 * @details
 * Fills @p out with trampolines that forward ``ra8_fs``'s ``read_block`` /
 * ``write_block`` / ``get_capacity`` calls into ``lx_nor_flash_sector_read`` /
 * ``lx_nor_flash_sector_write`` against @p nor_flash, with ``out->ctx``
 * pointing at @p nor_flash. ``erase_blocks`` is left NULL (NOR erases to ones,
 * and ``ra8_fs`` only consumes a zero-guaranteeing erase), so the formatter
 * writes zeros instead. After this call every ``ra8_fs`` API
 * (``ra8_fs_format``, ``ra8_fs_mount``, ...) works on the LevelX partition.
 *
 * The caller retains ownership of @p nor_flash; it must stay open for the
 * lifetime of every filesystem call made through @p out.
 *
 * @param[in]  nor_flash LevelX NOR-flash control block already opened via
 *                       ``lx_nor_flash_open``. Must not be NULL.
 * @param[out] out       Backend to populate. Must not be NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  @p out is wired to @p nor_flash.
 * @retval k_ra8_err_null_ptr        @p nor_flash or @p out was NULL.
 * @retval k_ra8_err_not_initialized @p nor_flash reports zero usable sectors
 *                                   (not opened, or geometry not programmed).
 *
 * @pre @p nor_flash was successfully opened with ``lx_nor_flash_open``.
 * @pre @p out is writable and outlives every filesystem call.
 * @post On ::k_ra8_ok, @p out's callbacks and ``ctx`` reference @p nor_flash.
 * @post On any non-ok return @p out is left unmodified.
 *
 * @note Not thread-safe; intended for single-threaded init, exactly like
 *       ``lx_nor_flash_open`` itself.
 *
 * @par Example:
 * @code
 * ra8_fs_backend_t be = {};
 * if (lx_fs_backend_bind(&s_nor_flash, &be) == k_ra8_ok) {
 *   ra8_fs_mount_t* mnt = nullptr;
 *   (void)ra8_fs_mount(&be, &mnt);   // FAT now runs on wear-levelled NOR
 * }
 * @endcode
 *
 * @see ra8_fs_backend_t  The seam this fills.
 * @see ra8_fs_set_lock() RTOS-world serialisation for the calls above.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t lx_fs_backend_bind(LX_NOR_FLASH* nor_flash, ra8_fs_backend_t* out);

#ifdef __cplusplus
}
#endif
