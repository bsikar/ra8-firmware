/**
 * @file ra8_boot_region.h
 * @brief Startup zero-fill for linker regions the reset handler cannot reach
 * @ingroup grp_core
 *
 * @details
 * `Reset_Handler` (libs/ra8_board_ek_ra8d2/src/boot/vector_table.c) copies
 * `.data` from MRAM and zeroes `.bss`. Both live in SRAM, which answers from
 * the first instruction after reset. `.sdram_data` does not: every linker
 * script places it `> SDRAM` as `NOLOAD`, and the external SDRAM window at
 * 0x68000000 stays dark until `ra8_sdramc_init()` has run the controller
 * bring-up sequence, so the reset handler cannot touch it.
 *
 * Objects placed there with `[[gnu::section(".sdram_data")]]` still have
 * static storage duration, so C requires them to read as all-bits-zero before
 * `main()` observes them. This module supplies that fill, and the SDRAM
 * bring-up path calls it at the first moment the window answers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @brief Zero every byte in the half-open span `[start, end)`.
 *
 * @param[out] start First byte to clear. Must not be nullptr.
 * @param[in]  end   One past the last byte to clear. Must not be nullptr and
 *                   must not precede @p start.
 *
 * @return `k_ra8_ok` on success (including an empty span, where
 *         `start == end` and nothing is written), `k_ra8_err_null_ptr` if
 *         either pointer is nullptr, or `k_ra8_err_invalid_arg` if @p end
 *         precedes @p start.
 *
 * @pre The whole span is mapped and writable.
 * @post Every byte in `[start, end)` reads as zero.
 *
 * @note Byte-wise, so an unaligned or odd-length span needs no tail special
 *       case. Startup runs it once; it is not on any hot path.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_boot_zero_region(void* start, const void* end);

/**
 * @brief Zero the `.sdram_data` section, honouring C static zero-init there.
 *
 * @details
 * Bounds come from the linker symbols `g_ra8_ls_ssdram` and `g_ra8_ls_esdram`,
 * which every linker script in the tree defines around the `.sdram_data`
 * output section. An image that places nothing in SDRAM links the two symbols
 * to the same address, so this is a no-op with no measurable cost.
 *
 * @return `k_ra8_ok` on success, or an error propagated from
 *         ::ra8_boot_zero_region.
 *
 * @pre The SDRAM controller is initialised and the window is readable and
 *      writable; on the EK-RA8D2 that means `ra8_sdramc_init()` reached its
 *      final `SDCCR` enable. Calling this earlier faults or drops the writes.
 * @post Every object placed in `.sdram_data` reads as zero.
 *
 * @warning Call exactly once, during bring-up. A later call wipes live state.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_boot_zero_sdram_bss(void);

#ifdef RA8_OFF_TARGET
/**
 * @brief Host-test hook: the stand-in `.sdram_data` window.
 *
 * @details
 * The host build has no linker script and therefore no `g_ra8_ls_*sdram`
 * symbols, so ::ra8_boot_zero_sdram_bss operates on a file-static buffer
 * instead. This hook hands the test that buffer so it can dirty it and then
 * assert the fill, the same arrangement `ra8_crashlog_test_record()` uses for
 * the `.noinit` record. Not built on target.
 *
 * @param[out] out_bytes Receives the window size in bytes. Must not be
 *                       nullptr.
 *
 * @return Pointer to the first byte of the stand-in window.
 *
 * @since 0.1.0
 */
uint8_t* ra8_boot_test_sdram_window(size_t* out_bytes);
#endif /* RA8_OFF_TARGET */

#ifdef __cplusplus
}
#endif
