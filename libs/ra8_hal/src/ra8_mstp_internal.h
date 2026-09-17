/**
 * @file ra8_mstp_internal.h
 * @brief Cross-TU surface for the ra8_mstp module-stop driver (test access).
 * @ingroup grp_hal_system
 *
 * @details
 * Not part of the public API. Declares two helpers promoted from TU-private
 * statics so the host tests can exercise the TrustZone Non-secure-attribution
 * read-back masking directly: the on-silicon scenario (a Secure module-stop
 * write to a Non-secure-owned bit that never reads back the commanded value)
 * cannot be reproduced by the host register RAM, which honours every write, so
 * the mask logic is tested here against a pre-set register value instead of
 * through a live write. See CLAUDE.md "Test access to internal symbols (MC/DC
 * scope)".
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"

/**
 * @brief Poll one MSTPCR register until its cared-for bits read back @p expect.
 *
 * @details Promoted from a TU-private static so the host tests can drive the
 * Non-secure-attribution mask directly. Bounded settle poll: succeeds when
 * ``(reg & care_mask) == (expect & care_mask)``, so bits excluded from
 * @p care_mask (Non-secure-owned, not Secure-writable) are tolerated even
 * when they never take the commanded state.
 *
 * @param[in] reg       MSTPCR index (`< 5`).
 * @param[in] expect    Value the cared-for bits must read back.
 * @param[in] care_mask Bits that must match (Secure-owned bits).
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok             The cared-for bits settled to @p expect.
 * @retval k_ra8_err_hw_timeout They never settled within the spin budget.
 *
 * @pre ``reg`` < 5.
 * @pre The MSTPCR write being confirmed has already been issued.
 * @post On k_ra8_ok the cared-for bits read back at @p expect.
 * @post No register is modified by this function.
 *
 * @note The settle decision `(reg & care_mask) == (expect & care_mask)` is a
 * single (masked-equality) condition, not a compound boolean. On the host the
 * loop-exit is routed through the ra8_fake_mmio fault seam (which overrides the
 * decision to model timing), so the mask's tolerance of Non-secure-owned bits
 * is verified on silicon (the TrustZone e-reader boots through ra8_mstp_init
 * with USBFS0 + USBHS delegated Non-secure), while the host tests cover the
 * mask value (priv_ra8_mstp_ns_mask_internal) and the init integration.
 *
 * @note Not thread-safe; init-time single-threaded context only.
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t
priv_ra8_mstp_wait_reg_settle_internal(uint8_t reg, uint32_t expect, uint32_t care_mask);

/**
 * @brief Non-secure-owned bit mask for one MSTPCR register (read from PSAR).
 *
 * @details Promoted from a TU-private static for test access. Returns the
 * PSAR-attributed (Non-secure-owned) bits of MSTPCR[@p reg]: the PSAR register
 * value for MSTPCRB..E, and 0 for MSTPCRA (no attribution register) and on any
 * non-TrustZone system (every PSAR reads 0).
 *
 * @param[in] reg MSTPCR index (`< 5`).
 *
 * @return The Non-secure-owned bit mask (0 for MSTPCRA and non-TrustZone).
 *
 * @pre ``reg`` < 5.
 * @pre Runs in Secure / privileged context (R_PSCU is Secure-only).
 * @post No register is modified.
 * @post MSTPCRA (@p reg 0) always yields 0.
 *
 * @par MC/DC:
 * The `k_psar_addr[reg] == 0U` branch (MSTPCRA has no PSAR vs the B..E
 * registers that do) is covered by test_mstp_ns_mask_reads_psar.
 *
 * @note Not thread-safe; init-time single-threaded context only.
 * @since 0.1.0
 */
RA8_PRIV [[nodiscard]] uint32_t priv_ra8_mstp_ns_mask_internal(uint8_t reg);

#ifdef __cplusplus
}
#endif
