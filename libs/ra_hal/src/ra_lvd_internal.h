/**
 * @file ra_lvd_internal.h
 * @brief Test-access surface for ra_lvd internal helpers (MC/DC).
 *
 * @details
 * Not part of the public API. Tests under tests/ MAY include this
 * header to drive compound boolean decisions that sit in TU-private
 * helpers behind the public ra_lvd facade.
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
 * @brief Pure predicate for "RN=1 prohibited when RHSEL=1" rejection.
 *
 * @details
 * Returns true iff @p hysteresis equals the HVD code AND @p negate
 * equals the after-assert code.  Promoted from the inline compound
 * AND at libs/ra_hal/src/ra_lvd.c.
 *
 * @param[in] hvd_val           Numeric value of @c k_ra_lvd_hysteresis_hvd.
 * @param[in] after_assert_val  Numeric value of @c k_ra_lvd_negate_after_assert.
 * @param[in] hysteresis        Candidate hysteresis value.
 * @param[in] negate            Candidate negate value.
 *
 * @return Boolean reject-config predicate.
 * @retval true  Caller must return @c k_ra_err_invalid_arg.
 * @retval false Combination is allowed.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the four inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * 2-condition AND; N+1 = 3 vectors:
 *  - hyst!=hvd, neg=after  -> false
 *  - hyst=hvd,  neg=after  -> true  (varies hyst)
 *  - hyst=hvd,  neg!=after -> false (varies neg)
 *
 * @since 0.1.0
 */
bool ra_lvd_internal_reject_hvd_after(uint32_t hvd_val,
                                      uint32_t after_assert_val,
                                      uint32_t hysteresis,
                                      uint32_t negate);

/**
 * @brief Pure predicate for the CR0 "set RI bit" decision.
 *
 * @details
 * Returns true iff @p response equals the reset code OR equals the
 * reset-on-rise code.  Promoted from the inline compound OR at
 * libs/ra_hal/src/ra_lvd.c inside @c internal_compose_cr0.
 *
 * @param[in] reset_val          Numeric value of @c k_ra_lvd_response_reset.
 * @param[in] reset_on_rise_val  Numeric value of @c k_ra_lvd_response_reset_on_rise.
 * @param[in] response           Candidate response value.
 *
 * @return Boolean set-RI predicate.
 * @retval true  Caller must OR @c k_ra_lvd_cr0_mask_ri into CR0.
 * @retval false RI bit stays clear.
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
 *  - resp=interrupt    -> false
 *  - resp=reset        -> true (varies left)
 *  - resp=reset_on_rise-> true (varies right)
 *
 * @since 0.1.0
 */
bool ra_lvd_internal_set_ri_bit(uint32_t reset_val, uint32_t reset_on_rise_val, uint32_t response);

#ifdef __cplusplus
}
#endif
