/**
 * @file ra_xspi.h
 * @brief xSPI / Octo-SPI driver (flash read/program/erase + ID/status)
 *
 * @details
 * Driver surface for the RA8D2 xSPI controller in direct-command
 * mode. Supports initialisation at a given link-layer IO width, raw
 * command-buffer writes, and a minimal set of SPI NOR flash
 * operations (read, page program, sector erase, status/ID read).
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
 * @brief Initialise an xSPI instance in the chosen link-layer IO mode.
 *
 * @param[in] instance xSPI instance (0 or 1).
 * @param[in] mode     Desired LIOCFG mode (1S/2S/4S/8S/8D).
 *
 * @return `k_ra_ok` on success.
 * @return `k_ra_err_null_ptr` if `instance` is out of range.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_xspi_init(uint8_t instance, ra_xspi_lio_mode_t mode);

/**
 * @brief Issue a raw command via the direct-command registers.
 *
 * @param[in] instance xSPI channel (0 or 1).
 * @param[in] cmd_buf  Up to 16 bytes of command / address / data.
 * @param[in] len      Number of bytes in `cmd_buf`.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_xspi_direct_command(uint8_t instance, const uint8_t* cmd_buf, uint8_t len);

/**
 * @brief Read `len` bytes from external flash into `buf`.
 *
 * @details
 * Builds a standard 0x03 (1S-1S-1S) read command sequence, programmes
 * the xSPI direct-command registers, polls COMSTT for completion, and
 * copies the RDBUF shadow into `buf`. In `RA_SIMULATOR_MODE` the body
 * is backed by an on-host 4 KiB fake-flash buffer so unit tests can
 * exercise the function end-to-end.
 *
 * @param[in]  instance   xSPI instance (0 or 1).
 * @param[in]  flash_addr Flash offset to read from.
 * @param[out] buf        Destination buffer. Must not be NULL.
 * @param[in]  len        Number of bytes to read.
 *
 * @return `k_ra_ok` on success.
 * @return `k_ra_err_null_ptr`    if buf is NULL or instance out of range.
 * @return `k_ra_err_invalid_arg` if len == 0 or > `k_ra_xspi_max_xfer`.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_xspi_flash_read(uint8_t instance, uint32_t flash_addr, uint8_t* buf, uint32_t len);

/**
 * @brief Program (write) `len` bytes at `flash_addr`.
 *
 * @details
 * Sequence is: Write Enable -> Page Program -> Poll WIP. On a
 * simulator build the fake-flash buffer is updated byte-wise. On
 * target the xSPI direct-command registers are driven and COMSTT is
 * polled for completion.
 *
 * @param[in] instance   xSPI instance.
 * @param[in] flash_addr Flash offset.
 * @param[in] data       Source buffer.
 * @param[in] len        Number of bytes to program.
 *
 * @return `k_ra_ok` on success.
 * @return `k_ra_err_null_ptr`    if data or instance is invalid.
 * @return `k_ra_err_invalid_arg` if len == 0 or > `k_ra_xspi_max_xfer`.
 * @return `k_ra_err_timeout`     if the WIP bit never clears.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_xspi_flash_program(uint8_t instance, uint32_t flash_addr, const uint8_t* data, uint32_t len);

/**
 * @brief Erase the 4 KiB sector containing `flash_addr`.
 *
 * @param[in] instance   xSPI instance.
 * @param[in] flash_addr Address within the target sector.
 *
 * @return `k_ra_ok` on success.
 * @return `k_ra_err_null_ptr` if `instance` is out of range.
 * @return `k_ra_err_timeout`  if WIP does not clear.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_xspi_flash_erase_sector(uint8_t instance, uint32_t flash_addr);

/**
 * @brief Read the flash Status Register (opcode 0x05).
 *
 * @param[in]  instance   xSPI instance.
 * @param[out] out_status Status-register value (WIP is bit 0).
 *
 * @return `k_ra_ok` on success.
 * @return `k_ra_err_null_ptr` if pointer is NULL or instance out of range.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_xspi_flash_read_status(uint8_t instance, uint8_t* out_status);

/**
 * @brief Read the JEDEC ID (opcode 0x9F), returning 24 bits in a uint32_t.
 *
 * @details
 * The returned word packs the three JEDEC bytes as:
 * `(manufacturer << 16) | (mem_type << 8) | capacity`. Compatible
 * with Macronix MX25, Winbond W25, Renesas AT25, and ISSI IS25 parts.
 *
 * @param[in]  instance xSPI instance.
 * @param[out] out_id   24-bit JEDEC ID in a 32-bit word.
 *
 * @return `k_ra_ok` on success.
 * @return `k_ra_err_null_ptr` if pointer is NULL or instance out of range.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_xspi_flash_read_id(uint8_t instance, uint32_t* out_id);

/**
 * @typedef ra_xspi_event_fn_t
 * @brief xSPI event callback.
 */
typedef void (*ra_xspi_event_fn_t)(void* ctx, uint32_t status_mask);

/**
 * @brief Tear down the xSPI instance (disable + MSTP release).
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_xspi_deinit(uint8_t instance);

/**
 * @brief Read the xSPI COMSTT busy/error mask.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_xspi_get_status(uint8_t instance, uint32_t* out_mask);

/**
 * @brief Clear transfer-complete / error bits in INTS via INTC.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_xspi_clear_status(uint8_t instance, uint32_t mask);

/**
 * @brief Attach a completion callback.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_xspi_attach_handler(uint8_t instance, ra_xspi_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch an xSPI event -- snapshot INTS + fire callback.
 * @since 0.2.0
 */
void ra_xspi_dispatch(uint8_t instance);

/**
 * @brief Put an xSPI instance into MSTP-gated stop.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_xspi_enter_stop(uint8_t instance);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.2.0
 */
[[nodiscard]] ra_err_t ra_xspi_exit_stop(uint8_t instance);

#ifdef __cplusplus
}
#endif
