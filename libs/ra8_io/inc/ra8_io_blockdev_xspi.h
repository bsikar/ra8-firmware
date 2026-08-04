/**
 * @file ra8_io_blockdev_xspi.h
 * @brief ra8_io block-device backend over OSPI NOR flash via ra8_xspi.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The xSPI backend exposes the 64 MiB external Octo-SPI NOR flash as an array
 * of 512-byte logical blocks on top of the `ra8_xspi` HAL driver. NOR semantics
 * are surfaced through the capability query: a program only clears bits
 * (1 -> 0), and a 4 KiB sector must be erased back to all-ones before it can be
 * re-programmed. So an arbitrary block write performs a whole-sector
 * read-modify-write internally -- read the 4 KiB sector, overlay the touched
 * 512-byte blocks, erase the sector, and program the 4 KiB back. Reads and
 * erases are bounds-checked; erases must be sector-aligned (multiples of eight
 * logical blocks). The backend touches no MMIO directly; every flash access
 * goes through `ra8_xspi_flash_read`/`_program`/`_erase_sector`, which carry the
 * Hardware User's Manual citations.
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
 * @struct ra8_io_blockdev_xspi_state_t
 * @brief Caller-owned private state for an OSPI NOR block device.
 *
 * @details
 * Zero-initialise and pass to ::ra8_io_blockdev_xspi_init, which fills it and
 * binds it into a ::ra8_io_blockdev_t. Treat the fields as private. The state
 * must out-live every call made through the device. The window described by
 * `base_off` and `block_count` must lie within the physical flash and be
 * whole-sector aligned (see ::ra8_io_blockdev_xspi_init).
 *
 * @invariant `base_off` is a multiple of 4096 (one erase sector).
 * @invariant `block_count` is a non-zero multiple of 8 (whole sectors).
 * @invariant `instance` is a valid xSPI instance (< 2).
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t  instance;    /**< xSPI HAL instance index, 0 or 1 (private).        */
  uint32_t base_off;    /**< Flash byte offset of logical block 0 (private).   */
  uint32_t block_count; /**< Number of 512-byte logical blocks (private).      */
  bool     read_only;   /**< true => writes and erases are rejected (private). */
} ra8_io_blockdev_xspi_state_t;

/**
 * @brief Bind an OSPI NOR block-device backend into a caller-owned handle.
 *
 * @details
 * Records `instance`/`base_off`/`block_count` in `state`, marks it read-only
 * per `read_only`, and points `bd` at the xSPI vtable with `state` as its
 * context. No allocation occurs; the caller owns both `bd` and `state`. The
 * window must be whole-sector aligned: `base_off` a multiple of 4096 and
 * `block_count` a non-zero multiple of 8 (4096 / 512). The application must
 * have brought up the controller with `ra8_xspi_init(instance, mode)` before any
 * call is dispatched through the bound device.
 *
 * @param[out] bd          Handle to bind (zero-initialised by the caller).
 * @param[out] state       Caller-owned backend state to populate.
 * @param[in]  instance    xSPI HAL instance index (0 or 1).
 * @param[in]  base_off    Flash byte offset of logical block 0 (% 4096 == 0).
 * @param[in]  block_count Number of 512-byte logical blocks (non-zero, % 8 == 0).
 * @param[in]  read_only   true to reject writes and erases on this device.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Backend bound; `bd` is usable.
 * @retval k_ra8_err_null_ptr     `bd` or `state` was NULL.
 * @retval k_ra8_err_invalid_arg  `instance` >= 2, `base_off` not sector-aligned,
 *                               or `block_count` not a non-zero multiple of 8.
 *
 * @pre `ra8_xspi_init(instance, mode)` has already succeeded.
 * @pre `bd` and `state` out-live every call made through the device.
 * @post On success `bd` dispatches to the xSPI backend over the flash window.
 * @post On any non-ok return `bd` and `state` are left unbound/untouched.
 *
 * @note Not thread-safe with respect to the same device.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_blockdev_xspi_init(ra8_io_blockdev_t*            bd,
                                                  ra8_io_blockdev_xspi_state_t* state,
                                                  uint8_t                       instance,
                                                  uint32_t                      base_off,
                                                  uint32_t                      block_count,
                                                  bool                          read_only);

#ifdef __cplusplus
}
#endif
