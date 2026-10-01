/**
 * @file examples/ek_ra8d2/hw_pending/cache_store_paged_demo/src/csp_nor.h
 * @brief RAM-backed LevelX NOR model for the cache_store paged demo.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * ra8_cache_store binds its physical-flash driver through an injected
 * `nor_driver_init` callback. Production binds the Octo-SPI driver; this app
 * binds the model behind ::csp_nor_init instead, so the whole store path runs
 * in SRAM with no MMIO and an emulated run is byte-identical to an on-silicon
 * one. The backing array is static storage, so it survives a LevelX
 * close/open inside one boot: that is exactly the "control state lost, media
 * survives" shape the demo's crash-replay leg needs.
 *
 * Split out of `main.c` so neither translation unit carries both the media
 * model and the demo legs.
 *
 * @author Brighton Sikarskie
 * @date 2026-09-29
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#ifndef CSP_NOR_H
#define CSP_NOR_H

#include "lx_api.h"

/**
 * @brief Bind the RAM-backed NOR model into a LevelX control block.
 * @details Fills @p nor_flash with the model's geometry, its sector scratch
 *          buffer and its four driver callbacks. Bound as the store's
 *          `nor_driver_init`; LevelX calls it from `lx_nor_flash_open`.
 * @param[in,out] nor_flash LevelX control block to populate.
 * @return `LX_SUCCESS`, or `LX_ERROR` on a NULL argument.
 * @retval 0 Geometry and all four callbacks are set.
 * @retval 1 @p nor_flash was NULL.
 * @pre @p nor_flash is a zeroed or reusable LevelX control block.
 * @pre The backing is static storage (always available).
 * @post On success the geometry and all four callbacks are set.
 * @post The backing bytes are left as they were (format erases, open does not).
 * @note Not thread-safe; the store serialises access.
 * @since 0.1.0
 */
unsigned int csp_nor_init(struct LX_NOR_FLASH_STRUCT* nor_flash);

/**
 * @brief Blank the whole SRAM backing to the erased pattern.
 * @details Puts the model in the state a never-written NOR part would be in,
 *          so the first mount sees a blank device regardless of BSS content.
 * @return Nothing.
 * @pre The backing is static storage (always available).
 * @pre No LevelX operation is mid-flight against the backing.
 * @post Every backing word reads as the erased pattern.
 * @post A subsequent open sees a blank (unformatted) device.
 * @note Not thread-safe; called once before the first mount.
 * @since 0.1.0
 */
void csp_nor_wipe(void);

#endif /* CSP_NOR_H */
