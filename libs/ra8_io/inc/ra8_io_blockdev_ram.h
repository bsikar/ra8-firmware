/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_io_blockdev_ram.h
 * @brief ra8_io block-device backend over a caller-owned RAM buffer.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The RAM backend treats a flat caller-owned byte buffer as an array of
 * 512-byte logical blocks. It is the reference backend (the simplest correct
 * implementation of ::ra8_io_blockdev_iface), the substrate for host unit tests,
 * and a usable scratch/ramdisk volume on target (e.g. a small staging area in
 * SRAM, or -- via the SDRAM backend's identical model -- the 64 MiB external
 * SDRAM window). It erases to zero and needs no erase-before-write.
 *
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

/**
 * @struct ra8_io_blockdev_ram_state_t
 * @brief Caller-owned private state for a RAM block device.
 *
 * @details
 * Zero-initialise and pass to ::ra8_io_blockdev_ram_init, which fills it and
 * binds it into a ::ra8_io_blockdev_t. Treat the fields as private. The `storage`
 * buffer and this state must out-live every call made through the device.
 *
 * @invariant `storage` covers `block_count * k_ra8_io_block_size_bytes` bytes.
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t* storage;     /**< Caller-owned backing buffer (private).            */
  uint32_t block_count; /**< Number of 512-byte logical blocks (private).      */
  bool     read_only;   /**< true => writes and erases are rejected (private). */
} ra8_io_blockdev_ram_state_t;

/**
 * @brief Bind a RAM block-device backend into a caller-owned handle.
 *
 * @details
 * Records `storage`/`block_count` in `state`, marks it read-only per
 * `read_only`, and points `bd` at the RAM vtable with `state` as its context.
 * No allocation occurs; the caller owns both `bd` and `state`.
 *
 * @param[out] bd          Handle to bind (zero-initialised by the caller).
 * @param[out] state       Caller-owned backend state to populate.
 * @param[in]  storage     Backing buffer of `block_count * 512` bytes.
 * @param[in]  block_count Number of 512-byte logical blocks (>= 1).
 * @param[in]  read_only   true to reject writes and erases on this device.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Backend bound; `bd` is usable.
 * @retval k_ra8_err_null_ptr     `bd`, `state`, or `storage` was NULL.
 * @retval k_ra8_err_invalid_size `block_count` was zero.
 *
 * @pre `storage` covers at least `block_count * 512` bytes.
 * @pre `bd` and `state` out-live every call made through the device.
 * @post On success `bd` dispatches to the RAM backend over `storage`.
 * @post On any non-ok return `bd` and `state` are left unbound/untouched.
 *
 * @note Not thread-safe with respect to the same device.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_blockdev_ram_init(ra8_io_blockdev_t*           bd,
                                                 ra8_io_blockdev_ram_state_t* state,
                                                 uint8_t*                     storage,
                                                 uint32_t                     block_count,
                                                 bool                         read_only);

#ifdef __cplusplus
}
#endif
