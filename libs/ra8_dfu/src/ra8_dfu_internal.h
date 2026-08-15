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

#ifdef __cplusplus
}
#endif
