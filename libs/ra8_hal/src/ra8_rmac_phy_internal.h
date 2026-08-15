/**
 * @file ra8_rmac_phy_internal.h
 * @brief Test-access surface for ra8_rmac_phy internal helpers (MC/DC).
 *
 * @details Declares module-private PHY decode predicates shared by the RMAC driver and focused host tests.
 * @ingroup grp_hal_net
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
 * @brief Pure predicate: PHY register read OK AND mask bit set.
 *
 * @details
 * Reusable for both the 1000FULL and 1000HALF speed-decode tests at
 * libs/ra8_hal/src/ra8_rmac_phy.c lines 352 and 356 inside
 * @c ra8_rmac_phy_link_status_get.
 *
 * @param[in] err       Result of the prior MIIM read.
 * @param[in] reg_value Register value just read.
 * @param[in] mask      Speed-bit mask to test.
 *
 * @return Boolean speed-detected predicate.
 * @retval true  Caller may set the corresponding speed enum.
 * @retval false Either the read failed or the bit is clear.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the three inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * 2-condition AND; N+1 = 3 vectors:
 *  - err=k_ra8_ok, mask&val=0          -> false
 *  - err=k_ra8_ok, mask&val=mask       -> true (varies right)
 *  - err!=k_ra8_ok, mask&val=mask      -> false (varies left)
 *
 * @since 0.1.0
 */
RA8_PRIV bool priv_ra8_rmac_phy_internal_speed_ok(ra8_err_t err, uint16_t reg_value, uint16_t mask);

#ifdef __cplusplus
}
#endif
