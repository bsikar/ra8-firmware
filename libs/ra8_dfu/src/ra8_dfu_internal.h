/**
 * @file ra8_dfu_internal.h
 * @brief TU-shared surface for the DFU MRAM program/verify implementation.
 * @ingroup grp_security
 *
 * @par Tag
 * [Ring 4 / Service] {World: S}
 *
 * @details
 * Not part of the public DFU API. Carries the handful of ``ra8_dfu_program.c``
 * helpers that are promoted from @c static to external linkage so their
 * compound decisions can be exercised with independent influence (MC/DC) from
 * the host unit tests. Production callers keep using the public
 * ``ra8_dfu.h`` surface; the only consumers of these symbols outside the
 * defining TU are the tests under @c tests/ (see CLAUDE.md, "Test access to
 * internal symbols").
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"

/**
 * @brief Program @p len bytes at @p addr through the SECURE MRAM gate.
 *
 * @details
 * The DFU core runs in the secure world and its slot MRAM carries secure
 * attribution, so it must be programmed via ``MRCPC1``
 * (``k_ra8_flash_world_s``). This helper drives ``ra8_flash_write_block``
 * directly, one 32-byte page at a time, erasing each page to the all-ones
 * baseline first, with IRQs masked across each page program so no ISR
 * fetches code-MRAM while the array is busy. The soft write-window installed
 * by ::ra8_dfu_program_prepare is still enforced inside
 * ``ra8_flash_write_block``, so an out-of-slot destination is rejected.
 *
 * Promoted from TU-private @c static linkage so the argument guard can be
 * exercised for MC/DC directly (both production callers validate their
 * pointer/length before dispatching here, so neither guard condition is
 * presentable on a public-API path); defined in @c ra8_dfu_program.c.
 *
 * @param[in] addr 32-byte aligned MRAM destination.
 * @param[in] src  Non-NULL source buffer of at least @p len bytes.
 * @param[in] len  Non-zero multiple of the 32-byte page size.
 *
 * @return ``ra8_err_t`` from the first failing page, else ``k_ra8_ok``.
 * @retval k_ra8_ok Every page committed.
 * @retval k_ra8_err_invalid_arg @p src was NULL or @p len was zero.
 *
 * @pre @p src non-null and @p len a non-zero multiple of the page size.
 * @pre This TU is linked into ``.sram_text`` (SRAM-resident program loop).
 * @post On success every page in [addr, addr+len) holds @p src.
 * @post The secure program gate is re-locked on every exit path.
 * @note Thread-safe: no; masks IRQs across each page program.
 * @note Test-access only outside the defining TU.
 *
 * @par MC/DC:
 * Decision: ``(src == nullptr) || (len == 0U)`` (2 conditions, ``||``); the
 * argument guard. N+1 = 3 vectors:
 *  - V1: src!=NULL, len!=0 -> F,F -> false (control: proceeds to program)
 *  - V2: src==NULL         -> T,. -> true  (varies left; returns invalid_arg)
 *  - V3: src!=NULL, len==0 -> F,T -> true  (varies right; returns invalid_arg)
 *
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_dfu_write_secure(uintptr_t addr, const uint8_t* src, uint32_t len);

#ifdef RA8_OFF_TARGET
/**
 * @brief Host-test seam: overwrite raw bytes of ping-pong copy @p copy_idx.
 *
 * @details Simulates a torn erase (`0xFF` bytes), torn page write, or bit-rot
 * directly inside the off-target dual-copy RAM shadow without weakening the
 * production public API.
 *
 * @param[in] copy_idx Ping-pong copy slot index (`0` or `1`).
 * @param[in] raw32    Non-NULL 32-byte replacement image for that copy.
 * @return ``ra8_err_t`` outcome.
 * @retval k_ra8_ok              Copy overwritten.
 * @retval k_ra8_err_invalid_arg @p copy_idx >= 2.
 * @retval k_ra8_err_null_ptr    @p raw32 is NULL.
 *
 * @pre Off-target host test build only (`RA8_OFF_TARGET`).
 * @post Ping-pong shadow @p copy_idx holds @p raw32.
 * @note Test-access only. @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_dfu_boot_state_corrupt_copy(uint8_t copy_idx, const uint8_t* raw32);

/**
 * @brief Host-test seam: reset both off-target ping-pong copies to erased `0xFF`.
 *
 * @pre Off-target host test build only (`RA8_OFF_TARGET`).
 * @post Both off-target boot-state copies read all `0xFF`.
 * @note Test-access only. @since 0.1.0
 */
RA8_PRIV void priv_dfu_boot_state_reset_store(void);
#endif /* RA8_OFF_TARGET */

#ifdef __cplusplus
}
#endif
