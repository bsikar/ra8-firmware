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

#include "ra_drw.h"

/**
 * @enum ra_drw_internal_const_t
 * @brief Driver-internal numeric constants -- avoid bare literals.
 *
 * @details
 * Shared between @c ra_drw.c and @c ra_drw_draw.c so the surface-setup
 * TU and the geometry-primitive TU draw on the same named constants.
 *
 * @invariant Every member fits in @c uint16_t.
 *
 * @see ra_drw_internal_mask32_t
 */
typedef enum : uint16_t {
  k_ra_drw_internal_byte_mask   = 0x00FFU, /**< 8-bit channel mask. */
  k_ra_drw_internal_one_subpx   = 16U,     /**< 1 px == 16 sub-pixels. */
  k_ra_drw_internal_clut_max    = 256U,    /**< CLUT entries (HUM 62.5.4). */
  k_ra_drw_internal_dlist_align = 4U,      /**< 4-byte alignment of DLIST. */
  k_ra_drw_internal_perfev_max  = 0x001FU, /**< 0x1F is "every clock". */
} ra_drw_internal_const_t;

/**
 * @enum ra_drw_internal_mask32_t
 * @brief 32-bit driver-internal masks.
 *
 * @details
 * Shared between @c ra_drw.c and @c ra_drw_draw.c.
 *
 * @invariant Every member fits in @c uint32_t.
 *
 * @see ra_drw_internal_const_t
 */
typedef enum : uint32_t {
  k_ra_drw_internal_color_alpha_mask = 0x00FFFFFFUL, /**< 0x00RRGGBB low bits. */
  k_ra_drw_internal_align_mask       = 0x00000003UL, /**< 4-byte alignment. */
} ra_drw_internal_mask32_t;

/**
 * @brief Convert a signed pixel coordinate to the DRW Q12.4 sub-pixel grid.
 *
 * @details
 * Shifts @p px left by the sub-pixel shift so 1 px maps to 16 sub-pixels.
 * Shared by both DRW TUs (surface setup and geometry primitives).
 *
 * @param[in] px Pixel coordinate (signed, in [-32768, 32767] range
 * -- the framebuffer never grows beyond 1024x1024).
 * @return Sub-pixel value (px << 4), wrapped to uint32_t.
 *
 * @pre Caller has range-checked @p px upstream.
 * @pre The shift cannot overflow 32 bits for the supported FB size.
 * @post Returned value preserves bit width and is suitable for
 * LnSTART / LnXADD / LnYADD writes.
 * @post No side effects.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static inline uint32_t internal_to_subpixel(int32_t px)
{
  return (uint32_t)(px << k_ra_drw_subpixel_shift);
}

/**
 * @brief Compute integer absolute value with no library calls.
 *
 * @details
 * Branch-on-sign absolute value, used by the line and triangle
 * primitives. Shared between DRW TUs.
 *
 * @param[in] v Signed value (range [-32768, 32767]).
 * @return Non-negative absolute value cast to int32_t.
 *
 * @pre @p v may be any int32_t except INT32_MIN.
 * @pre Result fits in int32_t.
 * @post Returned value is >= 0.
 * @post No side effects.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static inline int32_t internal_iabs(int32_t v)
{
  return (v < 0) ? -v : v;
}

/**
 * @brief Programme the four edge limiters that bound an axis-aligned rect.
 *
 * @details
 * Writes SIZE plus L1..L4 START/XADD/YADD to describe a filled
 * axis-aligned quad (HUM Ch 62.2.10-62.2.12, 62.2.29). Shared between
 * @c ra_drw.c and @c ra_drw_draw.c so both the solid-fill and the
 * textured-blit primitive reuse the same limiter encoding.
 *
 * @param[in] rect Rectangle in pixel space (validated by caller).
 *
 * @pre @p rect dimensions in [1..1024].
 * @pre Driver initialized.
 * @post L1..L4 START / XADD / YADD describe a filled axis-aligned quad.
 * @post SIZE = (height << 16) | width.
 *
 * @note Not thread-safe; writes MMIO.
 * @since 0.1.0
 */
void internal_program_rect_limiters(const ra_drw_rect_t* rect);

/**
 * @brief Pure predicate for the "rect is below min dim" rejection.
 *
 * @details
 * Returns true iff @p width or @p height is below @p min_dim.
 * Promoted from the inline compound OR at libs/ra_hal/src/ra_drw.c
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
 * libs/ra_hal/src/ra_drw.c inside @c ra_drw_blit_textured_rect.
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
