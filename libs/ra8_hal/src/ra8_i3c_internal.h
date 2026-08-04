/**
 * @file ra8_i3c_internal.h
 * @brief Test-access surface for ra8_i3c internal helpers (MC/DC).
 * @ingroup grp_hal_comms
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

/**
 * @brief Pure predicate: target_addr out of range OR max_len is zero.
 *
 * @details
 * Promoted from the inline OR at libs/ra8_hal/src/ra8_i3c.c in
 * @c ra8_i3c_recv_ccc.
 *
 * @param[in] addr_mask  Maximum valid 7-bit address (mask).
 * @param[in] target     Candidate target address.
 * @param[in] max_len    Caller-supplied max read length.
 *
 * @return Boolean reject predicate.
 * @retval true  Caller must return @c k_ra8_err_invalid_arg.
 * @retval false Inputs valid.
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
 *  - target<=mask, max_len>0 -> false
 *  - target>mask,  max_len>0 -> true (varies left)
 *  - target<=mask, max_len=0 -> true (varies right)
 *
 * @since 0.1.0
 */
RA8_PRIV
bool ra8_i3c_internal_recv_ccc_invalid(uint8_t addr_mask, uint8_t target, uint8_t max_len);

/**
 * @brief Pure predicate: HDR mode value is not one of {SDR, DDR, TS}.
 *
 * @details
 * Promoted from the inline triple AND at libs/ra8_hal/src/ra8_i3c.c
 * in @c ra8_i3c_set_hdr_mode.
 *
 * @param[in] sdr_val  Numeric value of @c k_ra8_i3c_hdr_mode_sdr.
 * @param[in] ddr_val  Numeric value of @c k_ra8_i3c_hdr_mode_ddr.
 * @param[in] ts_val   Numeric value of @c k_ra8_i3c_hdr_mode_ts.
 * @param[in] mode     Candidate mode value.
 *
 * @return Boolean reject predicate.
 * @retval true  Mode is not one of the three legal values.
 * @retval false Mode is legal.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the four inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * 3-condition AND of inequalities (== compound NOT-OR); N+1 = 4 vectors:
 *  - mode=SDR -> false (SDR check varies)
 *  - mode=DDR -> false (DDR check varies)
 *  - mode=TS  -> false (TS check varies)
 *  - mode=99  -> true  (all inequalities true)
 *
 * @since 0.1.0
 */
RA8_PRIV
bool ra8_i3c_internal_hdr_mode_invalid(uint32_t sdr_val,
                                       uint32_t ddr_val,
                                       uint32_t ts_val,
                                       uint32_t mode);

#ifdef __cplusplus
}
#endif
