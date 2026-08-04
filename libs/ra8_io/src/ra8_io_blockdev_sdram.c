/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_io_blockdev_sdram.c
 * @brief SDRAM block-device backend -- the 64 MiB SDRAM window as a ramdisk.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Once `ra8_sdramc_init` has brought the SDRAM controller up, the external
 * 64 MiB window is plain memory, so this backend reuses the RAM backend's
 * vtable and state. ::ra8_io_blockdev_sdram_init validates the requested block
 * count against the window, starts the controller, and delegates the binding
 * (and thereby every read/write/erase/caps callback) to
 * ::ra8_io_blockdev_ram_init. Contents are volatile across a power cycle.
 */

#include "ra8_io_blockdev_sdram.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_sdramc.h"
#include "ra8_sdramc_regs.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_blockdev_sdram";

/**
 * @enum ra8_io_sdram_const_t
 * @brief SDRAM-backend layout constants.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_io_sdram_min_blocks = 1, /**< Smallest legal device size, in blocks. */
} ra8_io_sdram_const_t;

/**
 * @brief Maximum block count the SDRAM window can hold.
 *
 * @details
 * The 64 MiB window divided by the 512-byte logical block size, computed once
 * so ::ra8_io_blockdev_sdram_init expresses no magic ratio inline.
 *
 * @return uint32_t Number of 512-byte logical blocks the window spans.
 * @retval 131072 The 64 MiB window divided by 512 bytes.
 *
 * @pre `k_ra8_sdram_size_bytes` is a multiple of `k_ra8_io_block_size_bytes`.
 * @pre The result fits in `uint32_t` (64 MiB / 512 == 131072).
 * @post No state is mutated.
 * @post The return reflects only the window/block-size ratio.
 *
 * @note Thread-safe (pure constant computation).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t sdram_max_blocks(void)
{
  return (uint32_t)k_ra8_sdram_size_bytes / (uint32_t)k_ra8_io_block_size_bytes;
}

ra8_err_t ra8_io_blockdev_sdram_init(ra8_io_blockdev_t*           bd,
                                     ra8_io_blockdev_ram_state_t* state,
                                     uint32_t                     block_count)
{
  RA8_CHECK_NULL_PTR(bd, s_tag, "bd must not be nullptr");
  RA8_CHECK_NULL_PTR(state, s_tag, "state must not be nullptr");
  if (block_count < (uint32_t)k_ra8_io_sdram_min_blocks) {
    return k_ra8_err_invalid_size;
  }
  if (block_count > sdram_max_blocks()) {
    return k_ra8_err_invalid_size;
  }
  RA8_RETURN_ON_ERROR(ra8_sdramc_init(), s_tag, "sdram bring-up");
  return ra8_io_blockdev_ram_init(bd,
                                  state,
                                  (uint8_t*)(uintptr_t)k_ra8_sdram_base_addr,
                                  block_count,
                                  false);
}
