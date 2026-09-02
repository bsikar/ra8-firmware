/**
 * @file lx_nor_fake_ram.h
 * @brief RAM-backed LevelX NOR driver for host tests (the ra8_cache_store DIP seam).
 *
 * @details
 * A host-only LevelX NOR driver-initialise callback whose "flash" is a static
 * RAM array. It substitutes for `lx_nor_driver_ra8_xspi_initialize` (which drives
 * real Octo-SPI hardware) so `ra8_cache_store` -- and LevelX standalone
 * underneath it -- can be exercised on the x86_64 test host.
 *
 * The backing RAM is a file-global that persists across `lx_nor_flash_close` /
 * `lx_nor_flash_open`, which is exactly the "reboot: control state lost, flash
 * survives" model the crash-recovery tests need: open a fresh (zeroed)
 * `LX_NOR_FLASH` control block over the same backing to simulate a power cycle.
 *
 * @note Single global instance; single-threaded host tests only.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 *
 * @par Tag
 * [Ring 4 / Storage] {World: NS}
 *
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Forward declaration mirroring LevelX's `LX_NOR_FLASH` control block.
 * @details Keeps this header free of `lx_api.h`; the `.c` includes the real one.
 */
struct LX_NOR_FLASH_STRUCT;

/**
 * @brief LevelX NOR driver-initialise callback over the RAM backing.
 *
 * @details Signature-compatible with `ra8_cache_store_nor_init_fn`: programs the
 *          control block's geometry, the read/write/erase/verify callbacks, and
 *          the sector buffer against the static backing array.
 *
 * @param[in,out] nor_flash LevelX control block to populate.
 * @return `0` (`LX_SUCCESS`) on success; non-zero on a NULL argument.
 * @retval 0 Control block programmed.
 * @retval 1 `nor_flash` was NULL.
 *
 * @pre `nor_flash` points at caller-owned LevelX control-block storage.
 * @pre The host backing array is available (always; static storage).
 * @post On success all four driver callbacks and the geometry are set.
 * @post The backing RAM is left as-is (open uses it; format erases it).
 * @note The callback owns no storage inside @p nor_flash beyond assigned pointers.
 * @since 0.1.0
 */
unsigned int lx_nor_fake_ram_init(struct LX_NOR_FLASH_STRUCT* nor_flash);

/**
 * @brief Erase the entire RAM backing to the LevelX "erased" pattern.
 *
 * @details Resets the fake flash between independent test cases so a fresh
 *          format starts from a blank device. Not needed to simulate a crash
 *          (leave the backing intact and reopen a new control block).
 *
 * @return Nothing.
 * @pre The backing array is static host storage (always available).
 * @pre No LevelX operation is mid-flight against the backing.
 * @post Every backing word reads as the erased pattern.
 * @post A subsequent open sees a blank (unformatted) device.
 * @note This is a test-isolation operation, not a simulated power cycle.
 * @since 0.1.0
 */
void lx_nor_fake_ram_wipe(void);

/**
 * @brief Force the next @p count write callbacks to fail (fault injection).
 *
 * @details Lets a test drive the store's LevelX-write error paths. A value of 0
 *          disables injection. Reset it after the test.
 *
 * @param[in] count Number of upcoming write callbacks to fail (0 disables).
 * @return Nothing.
 * @pre The backing array is static host storage (always available).
 * @pre The caller resets injection when the fault window is done.
 * @post The next @p count writes return an error, then writes succeed again.
 * @post No backing word is changed by a failed write.
 * @note Replacing a nonzero countdown discards its unconsumed remainder.
 * @since 0.1.0
 */
void lx_nor_fake_ram_fail_writes(unsigned int count);

#ifdef __cplusplus
}
#endif
