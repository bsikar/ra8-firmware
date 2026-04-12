/**
 * @file ra_xspi.h
 * @brief xSPI / Octo-SPI driver (framework)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_ospi_regs.h"
#include "ra_err.h"

/**
 * @brief Initialise an xSPI instance in single-bit SPI mode.
 */
[[nodiscard]] ra_err_t ra_xspi_init(uint8_t instance, ra_xspi_lio_mode_t mode);

/**
 * @brief Issue a raw command via the direct-command registers.
 *
 * @param[in] instance xSPI channel (0 or 1).
 * @param[in] cmd_buf  Up to 16 bytes of command / address / data.
 * @param[in] len      Number of bytes in `cmd_buf`.
 */
[[nodiscard]] ra_err_t
ra_xspi_direct_command(uint8_t instance, const uint8_t* cmd_buf, uint8_t len);

#ifdef __cplusplus
}
#endif
