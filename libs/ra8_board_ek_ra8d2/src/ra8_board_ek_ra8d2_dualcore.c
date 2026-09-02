/**
 * @file ra8_board_ek_ra8d2_dualcore.c
 * @brief EK-RA8D2 BSP -- the runtime view of the CPU0 <-> CPU1 shared window
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * One function, projecting the compile-time map in
 * ``ra8_board_ek_ra8d2_dualcore.h`` into the descriptor an application can
 * carve from. It exists so a consumer can ask the board rather than copy a
 * literal; the address itself is the header's, and this unit adds no second
 * copy of it.
 *
 * Touches no MCU registers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_board_ek_ra8d2_dualcore.h"

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_board.dualcore";

/**
 * @var s_shared_ram_is_non_cacheable
 * @brief Whether the boot MPU marks the shared window Normal non-cacheable.
 *
 * @details
 * ``src/boot/system_init.c`` programmes MPU region 4 over the shared window only
 * under ``RA8_BOOT_ENABLE_CACHE_MPU``, which also gates enabling the M85
 * caches. Reporting the flag rather than a constant `true` keeps the
 * descriptor honest: in a build without it nothing is stale (there is no cache
 * to be stale) but the memory map makes no promise, and an application that
 * later enables the D-cache by another route must know that.
 *
 * @note Compile-time constant; no runtime state.
 * @warning Do not hardcode `true` here -- the whole point of the field is that
 *          it tracks the build that is actually running.
 * @since 0.1.0
 */
#ifdef RA8_BOOT_ENABLE_CACHE_MPU
static const bool s_shared_ram_is_non_cacheable = true;
#else
static const bool s_shared_ram_is_non_cacheable = false;
#endif

ra8_err_t ra8_board_shared_ram(ra8_board_shared_ram_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  *out = (ra8_board_shared_ram_t){
    .base          = (void*)(uintptr_t)k_ra8_board_shared_ram_base,
    .size_bytes    = (uint32_t)k_ra8_board_shared_ram_size_bytes,
    .non_cacheable = s_shared_ram_is_non_cacheable,
  };
  return k_ra8_ok;
}
