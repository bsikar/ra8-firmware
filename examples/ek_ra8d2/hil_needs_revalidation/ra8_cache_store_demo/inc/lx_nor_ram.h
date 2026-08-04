/**
 * @file lx_nor_ram.h
 * @brief RAM-backed standalone-LevelX NOR driver (the ra8_cache_store DIP seam).
 *
 * @details
 * A first-party LevelX NOR driver-initialise callback whose "flash" is a static
 * SRAM array. It is the injected @ref ra8_cache_store_nor_init_fn for the
 * ra8_cache_store_demo example, substituting for the production Octo-SPI driver
 * (`lx_nor_driver_ra8_xspi_initialize`) so the entire persistent-cache path --
 * LevelX standalone plus ra8_cache_store on top of it -- runs in SRAM with no
 * MMIO. Because nothing touches a peripheral register, the emulated
 * (ra8_emulator) run and the on-silicon run execute byte-identical instructions:
 * EIL equals HIL by construction.
 *
 * The backing SRAM persists across `lx_nor_flash_close` / `lx_nor_flash_open`
 * within one boot, which models the "reboot: control state lost, on-media
 * content survives" behaviour the persistence-across-remount demonstration
 * needs: re-open a fresh (zeroed) `LX_NOR_FLASH` control block over the same
 * backing to simulate a power cycle. (A real Octo-SPI part persists across an
 * actual power cycle; this RAM model persists across the in-process remount.)
 *
 * @note Single global instance; the store serialises access (one reader core).
 * @since 0.1.0
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Forward declaration mirroring LevelX's `LX_NOR_FLASH` control block.
 * @details Keeps this header free of `lx_api.h`; only the `.c` includes it.
 */
struct LX_NOR_FLASH_STRUCT;

/**
 * @brief LevelX NOR driver-initialise callback over the static SRAM backing.
 *
 * @details Signature-compatible with `ra8_cache_store_nor_init_fn`: programs the
 *          control block's geometry, the read/write/erase/verify callbacks, and
 *          the LevelX per-open sector buffer against the static backing array.
 *          Passed straight to `lx_nor_flash_open()` / `lx_nor_flash_format()`.
 *
 * @param[in,out] nor_flash LevelX control block to populate.
 * @return `0` (`LX_SUCCESS`) on success; non-zero on a NULL argument.
 * @retval 0 Control block programmed.
 * @retval 1 @p nor_flash was NULL.
 *
 * @pre @p nor_flash points at caller-owned LevelX control-block storage.
 * @pre The static backing array is available (always; static storage).
 * @post On success all four driver callbacks and the geometry are set.
 * @post The backing SRAM is left as-is (open uses it; format erases it).
 * @note Not thread-safe; the store serialises access.
 * @since 0.1.0
 */
unsigned int lx_nor_ram_init(struct LX_NOR_FLASH_STRUCT* nor_flash);

/**
 * @brief Erase the entire SRAM backing to the LevelX "erased" pattern.
 *
 * @details Resets the fake NOR media to a blank (unformatted) device.
 *          Firmware never needs this -- reset zeroes the BSS backing and the
 *          first mount formats it -- but the host test calls it between
 *          independent scenarios that share the one static backing.
 *
 * @return Nothing.
 * @pre The backing array is static storage (always available).
 * @pre No LevelX operation is mid-flight against the backing.
 * @post Every backing word reads as the erased pattern.
 * @post A subsequent open sees a blank (unformatted) device.
 * @note Not thread-safe; the store serialises access.
 * @since 0.1.0
 */
void lx_nor_ram_wipe(void);

#ifdef __cplusplus
}
#endif
