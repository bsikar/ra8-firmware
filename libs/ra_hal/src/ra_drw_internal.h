/**
 * @file ra_drw_internal.h
 * @brief Test-access surface for ra_drw internal helpers (MC/DC).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @brief Pure predicate for the "rect is below min dim" rejection.
 *
 * @details
 * Returns true iff @p width or @p height is below @p min_dim.
 * Promoted from the inline compound OR at libs/ra_hal/src/ra_drw.c:776
 * inside @c ra_drw_blit_textured_rect.
 *
 * @param[in] min_dim Minimum permitted dimension (1 px).
 * @param[in] width   Width in pixels.
 * @param[in] height  Height in pixels.
 *
 * @return Boolean reject predicate.
 * @retval true  Caller must return @c k_ra_err_invalid_arg.
 * @retval false Dimensions meet the lower bound.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the three inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * 2-condition OR; N+1 = 3 vectors:
 *  - w>=min, h>=min -> false
 *  - w<min,  h>=min -> true (varies left)
 *  - w>=min, h<min  -> true (varies right)
 *
 * @since 0.1.0
 */
bool ra_drw_internal_rect_below_min(uint16_t min_dim, uint16_t width, uint16_t height);

/**
 * @brief Pure predicate for the "rect exceeds max dim" rejection.
 *
 * @details
 * Returns true iff @p width exceeds @p max_w or @p height exceeds
 * @p max_h.  Promoted from the inline compound OR at
 * libs/ra_hal/src/ra_drw.c:780 inside @c ra_drw_blit_textured_rect.
 *
 * @param[in] max_w  Maximum permitted width  (1024 px).
 * @param[in] max_h  Maximum permitted height (1024 px).
 * @param[in] width  Width in pixels.
 * @param[in] height Height in pixels.
 *
 * @return Boolean reject predicate.
 * @retval true  Caller must return @c k_ra_err_invalid_arg.
 * @retval false Dimensions are within both upper bounds.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the four inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * 2-condition OR; N+1 = 3 vectors:
 *  - w<=max_w, h<=max_h -> false
 *  - w>max_w,  h<=max_h -> true (varies left)
 *  - w<=max_w, h>max_h  -> true (varies right)
 *
 * @since 0.1.0
 */
bool ra_drw_internal_rect_above_max(uint16_t max_w,
                                    uint16_t max_h,
                                    uint16_t width,
                                    uint16_t height);

#ifdef __cplusplus
}
#endif
