/**
 * @file ra8_io_blockdev_sdram.h
 * @brief ra8_io block-device backend over the external 64 MiB SDRAM window.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The SDRAM backend is a volatile ramdisk: once the SDRAM controller is brought
 * up, the external 64 MiB window at `k_ra8_sdram_base_addr` is plain memory, so
 * this backend reuses the RAM backend's vtable and state
 * (::ra8_io_blockdev_ram_state_t) verbatim. It treats the SDRAM window as an
 * array of 512-byte logical blocks, erases to zero, and needs no
 * erase-before-write. Contents do not survive a power cycle.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_ram.h"

/**
 * @brief Bring up the SDRAM controller and bind it as a volatile ramdisk.
 *
 * @details
 * Validates `block_count` against the 64 MiB SDRAM window, initialises the
 * SDRAM controller via `ra8_sdramc_init`, then delegates to
 * ::ra8_io_blockdev_ram_init using the SDRAM base address as the backing buffer
 * (writable, never read-only). The reused ::ra8_io_blockdev_ram_state_t is
 * caller-owned and must out-live every call made through the device.
 *
 * @param[out] bd          Handle to bind (zero-initialised by the caller).
 * @param[out] state       Caller-owned RAM-backend state to populate.
 * @param[in]  block_count Number of 512-byte logical blocks (>= 1, <= window).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Controller up and backend bound; `bd` is usable.
 * @retval k_ra8_err_null_ptr     `bd` or `state` was NULL.
 * @retval k_ra8_err_invalid_size `block_count` was zero or exceeds the window.
 *
 * @pre `bd` and `state` out-live every call made through the device.
 * @pre The SDRAM pins/clocks are configured for `ra8_sdramc_init` to succeed.
 * @post On success `bd` dispatches to the RAM backend over the SDRAM window.
 * @post On any non-ok return `bd` and `state` are left unbound/untouched.
 *
 * @note Not thread-safe with respect to the same device.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_blockdev_sdram_init(ra8_io_blockdev_t*           bd,
                                                   ra8_io_blockdev_ram_state_t* state,
                                                   uint32_t                     block_count);

#ifdef __cplusplus
}
#endif
