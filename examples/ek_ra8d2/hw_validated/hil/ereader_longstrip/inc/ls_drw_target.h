/**
 * @file ls_drw_target.h
 * @brief DRW + Helium MVE longstrip compositor target binding for ereader_longstrip.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "longstrip.h"
#include "ra8_err.h"

/**
 * @brief Initialize DRW and bind the @ref longstrip_target_t hardware compositor.
 *
 * @param[in,out] fb565      RGB565 framebuffer base (width @p view_w, height @p view_h).
 * @param[in]     view_w     Viewport width in pixels.
 * @param[in]     view_h     Viewport height in pixels.
 * @param[in,out] stage_565  32-byte aligned RGB565 texture staging buffer.
 * @param[in]     stage_cap  Capacity of @p stage_565 in pixels.
 *
 * @return ``ra8_err_t`` status code.
 * @retval k_ra8_ok           DRW initialized and compositor target ready.
 * @retval k_ra8_err_null_ptr @p fb565 or @p stage_565 was ``nullptr``.
 *
 * @pre Graphics domain and display controller have been brought up cleanly.
 * @pre @p stage_565 spans at least @p view_w pixels in DMA-reachable RAM.
 * @post Hardware DRW fills and MVE+DRW band blits dispatch through ::ls_drw_target().
 * @post Internal target state binds @p fb565 without dynamic allocation.
 *
 * @note Not thread-safe; drives shared DRW hardware.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ls_drw_target_init(uint16_t* fb565,
                                           uint16_t  view_w,
                                           uint16_t  view_h,
                                           uint16_t* stage_565,
                                           uint32_t  stage_cap);

/**
 * @brief Return the initialized @ref longstrip_target_t for longstrip compositing.
 *
 * @return Mutable pointer to the app-owned @ref longstrip_target_t.
 *
 * @pre ::ls_drw_target_init() completed with ``k_ra8_ok``.
 * @pre Caller holds single-threaded render ownership of the panel framebuffer.
 * @post Returned target routes blits and solid fills through DRW + MVE.
 * @post No hardware state is altered by this accessor.
 *
 * @note Not thread-safe across concurrent render callers.
 * @since 0.1.0
 */
[[nodiscard]] longstrip_target_t* ls_drw_target(void);

#ifdef __cplusplus
}
#endif
