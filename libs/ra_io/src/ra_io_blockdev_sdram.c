/**
 * @file ra_io_blockdev_sdram.c
 * @brief SDRAM block-device backend -- the 64 MiB SDRAM window as a ramdisk.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Once `ra_sdramc_init` has brought the SDRAM controller up, the external
 * 64 MiB window is plain memory, so this backend reuses the RAM backend's
 * vtable and state. ::ra_io_blockdev_sdram_init validates the requested block
 * count against the window, starts the controller, and delegates the binding
 * (and thereby every read/write/erase/caps callback) to
 * ::ra_io_blockdev_ram_init. Contents are volatile across a power cycle.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_io_blockdev_sdram.h"

#include <stdint.h>

#include "ra8d2_sdramc_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_io_blockdev_ram.h"
#include "ra_sdramc.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra_io_blockdev_sdram";

/**
 * @enum ra_io_sdram_const_t
 * @brief SDRAM-backend layout constants.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_io_sdram_min_blocks = 1, /**< Smallest legal device size, in blocks. */
} ra_io_sdram_const_t;

/**
 * @brief Maximum block count the SDRAM window can hold.
 *
 * @details
 * The 64 MiB window divided by the 512-byte logical block size, computed once
 * so ::ra_io_blockdev_sdram_init expresses no magic ratio inline.
 *
 * @return uint32_t Number of 512-byte logical blocks the window spans.
 * @retval 131072 The 64 MiB window divided by 512 bytes.
 *
 * @pre `k_ra_sdram_size_bytes` is a multiple of `k_ra_io_block_size_bytes`.
 * @pre The result fits in `uint32_t` (64 MiB / 512 == 131072).
 * @post No state is mutated.
 * @post The return reflects only the window/block-size ratio.
 *
 * @note Thread-safe (pure constant computation).
 *
 * @since 0.1.0
 */
static uint32_t sdram_max_blocks(void)
{
  return (uint32_t)k_ra_sdram_size_bytes / (uint32_t)k_ra_io_block_size_bytes;
}

ra_err_t ra_io_blockdev_sdram_init(ra_io_blockdev_t*           bd,
                                   ra_io_blockdev_ram_state_t* state,
                                   uint32_t                    block_count)
{
  RA_CHECK_NULL_PTR(bd, s_tag, "bd must not be nullptr");
  RA_CHECK_NULL_PTR(state, s_tag, "state must not be nullptr");
  if (block_count < (uint32_t)k_ra_io_sdram_min_blocks) {
    return k_ra_err_invalid_size;
  }
  if (block_count > sdram_max_blocks()) {
    return k_ra_err_invalid_size;
  }
  RA_RETURN_ON_ERROR(ra_sdramc_init(), s_tag, "sdram bring-up");
  return ra_io_blockdev_ram_init(bd,
                                 state,
                                 (uint8_t*)(uintptr_t)k_ra_sdram_base_addr,
                                 block_count,
                                 false);
}
