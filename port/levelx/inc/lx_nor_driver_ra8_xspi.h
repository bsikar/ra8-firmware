/**
 * @file port/levelx/inc/lx_nor_driver_ra8_xspi.h
 * @brief LevelX NOR-flash driver shim that bridges LevelX onto the RA8
 *        ``ra8_xspi`` HAL (EK-RA8D2 ISSI IS25LX512M octal-SPI flash)
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Declares the single LevelX NOR driver-initialise entry point
 * ``lx_nor_driver_ra8_xspi_initialize`` that
 * ``lx_nor_flash_open()`` / ``lx_nor_flash_format()`` calls back into
 * during NOR-flash bring-up. The function:
 *
 *   - Programmes ``LX_NOR_FLASH::lx_nor_flash_total_blocks`` and
 *     ``lx_nor_flash_words_per_block`` based on the on-board IS25LX512M
 *     geometry (4 KiB sectors, 64 MiB total -> 16384 LevelX blocks).
 *   - Hands LevelX a 512-byte sector RAM scratch buffer pointed to by
 *     ``lx_nor_flash_sector_buffer``.
 *   - Registers four read / write / erase / erase-verify callbacks that
 *     translate ``ULONG``-word LevelX requests into matching
 *     ``ra8_xspi_flash_*`` byte-oriented HAL calls.
 *
 * Sector geometry:
 *
 *   | LevelX concept                  | IS25LX512M mapping                    |
 *   |---------------------------------|--------------------------------------|
 *   | block (smallest erase unit)     | 4 KiB sector (opcode 0x20)           |
 *   | physical sector                 | 512 bytes (LX_NOR_SECTOR_SIZE words) |
 *   | physical sectors per block      | 7 usable + 1 metadata = 8            |
 *   | words per block                 | 4096 / sizeof(ULONG) = 1024 words    |
 *
 * The shim hard-wires xSPI instance 0, which is the channel routed to
 * the EK-RA8D2 octal-SPI flash device. ``lx_nor_flash_base_address``
 * is set to a non-zero "fake" base; LevelX uses it only as an opaque
 * cookie when computing flash addresses for the read / write / erase
 * callbacks below, where we re-derive the byte offset by subtracting
 * this base.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* lx_api.h drags in the LX_NOR_FLASH struct definition the function
 * signature below references. Including it here keeps consumers from
 * having to remember the include order. */
#include "lx_api.h"

/**
 * @brief LevelX NOR driver-initialise entry point bridging onto ``ra8_xspi``.
 *
 * @details
 * Pass this function as the ``nor_driver_initialize`` argument to
 * ``lx_nor_flash_open()`` or ``lx_nor_flash_format()``. LevelX invokes
 * it exactly once per ``open`` / ``format`` call to discover the
 * geometry of the flash and to populate the four ``lx_nor_flash_driver_*``
 * function pointers in the supplied control block.
 *
 * Driver mapping:
 *
 * | LevelX callback                         | Action                                       |
 * |-----------------------------------------|----------------------------------------------|
 * | ``lx_nor_flash_driver_read``            | ``ra8_xspi_flash_read(0, off, buf, words*4)`` |
 * | ``lx_nor_flash_driver_write``           | ``ra8_xspi_flash_program(0, off, buf, w*4)``  |
 * | ``lx_nor_flash_driver_block_erase``     | ``ra8_xspi_flash_erase_sector(0, off)``       |
 * | ``lx_nor_flash_driver_block_erased_verify`` | read sector and check 0xFFFFFFFF         |
 *
 * @param[in,out] nor_flash LevelX NOR-flash control block. The driver
 *   writes ``lx_nor_flash_base_address``, ``lx_nor_flash_total_blocks``,
 *   ``lx_nor_flash_words_per_block``, ``lx_nor_flash_sector_buffer``
 *   and the four ``lx_nor_flash_driver_*`` function pointers.
 *
 * @return ``LX_SUCCESS`` on success.
 * @retval LX_SUCCESS Driver registered, geometry programmed.
 *
 * @pre ``nor_flash != LX_NULL``.
 * @pre ``ra8_xspi_init(0, k_ra8_xspi_lio_1s1s1s)`` has been called.
 *
 * @post All four ``lx_nor_flash_driver_*`` pointers are non-NULL.
 * @post ``nor_flash->lx_nor_flash_total_blocks > 0``.
 *
 * @note Not thread-safe; intended for single-threaded bring-up. LevelX
 *       serialises the call through its own state machine, so the
 *       driver itself does not need additional locking.
 *
 * @since 0.1.0
 */
UINT lx_nor_driver_ra8_xspi_initialize(LX_NOR_FLASH* nor_flash);

#ifdef __cplusplus
}
#endif
