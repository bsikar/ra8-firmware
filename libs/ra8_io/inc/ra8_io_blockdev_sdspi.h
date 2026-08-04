/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_io_blockdev_sdspi.h
 * @brief ra8_io block-device backend over the micro-SD-in-SPI-mode driver.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * This backend wraps the `ra8_sdmmc_spi` singleton driver so a micro-SD card on
 * the SPI bus appears to the fabric as a plain 512-byte logical block device.
 * Reads and writes forward straight to the driver's CMD17 / CMD25 paths; erase
 * uses the driver's verified-zero CMD32/CMD33/CMD38 sequence, and the reported
 * erase value is therefore ::k_ra8_io_erase_value_zero. Because the underlying
 * driver keeps a single global instance, this backend is stateless: the bound
 * ::ra8_io_blockdev_t carries a NULL context and the vtable dispatches to the
 * one driver singleton.
 *
 * The application must bring the card up with `ra8_sdmmc_spi_init(transport)`
 * before binding or using this device; the bind helper only wires the vtable
 * and does not touch the bus.
 *
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_err.h"
#include "ra8_io_blockdev.h"

/**
 * @brief Bind the micro-SD-over-SPI backend into a caller-owned handle.
 *
 * @details
 * Points `bd` at the SD-SPI vtable with a NULL context (the `ra8_sdmmc_spi`
 * driver is a single global instance, so no per-device state is needed). No
 * allocation and no bus traffic occur; the caller owns `bd`. The card itself
 * must already be initialised with `ra8_sdmmc_spi_init` before any I/O call is
 * made through the bound device.
 *
 * @param[out] bd Handle to bind (zero-initialised by the caller).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Backend bound; `bd` is usable once the card is up.
 * @retval k_ra8_err_null_ptr `bd` was NULL.
 *
 * @pre `bd` is non-NULL and out-lives every call made through the device.
 * @pre The application calls `ra8_sdmmc_spi_init` before issuing I/O.
 * @post On success `bd` dispatches to the SD-SPI singleton driver.
 * @post On any non-ok return `bd` is left unbound/untouched.
 *
 * @note Not thread-safe with respect to the same device or the driver singleton.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_blockdev_sdspi_init(ra8_io_blockdev_t* bd);

#ifdef __cplusplus
}
#endif
