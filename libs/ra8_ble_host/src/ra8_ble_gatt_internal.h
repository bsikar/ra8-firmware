/**
 * @file ra8_ble_gatt_internal.h
 * @brief Test-access surface for ra8_ble_gatt internal helpers (MC/DC).
 * @ingroup grp_net
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
 * @brief Pure predicate: payload length non-zero AND attribute storage non-NULL.
 *
 * @details
 * Reusable for both the @c set_value (line 480) and @c notify
 * (line 569) memcpy guards in libs/ra8_ble_host/src/ra8_ble_gatt.c.
 *
 * @param[in] len   Caller-supplied payload length.
 * @param[in] value Attribute value buffer (NULL if not allocated).
 *
 * @return Boolean copy-needed predicate.
 * @retval true  Caller must memcpy.
 * @retval false Skip the memcpy.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the two inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * 2-condition AND; N+1 = 3 vectors:
 *  - len=0,  val!=NULL -> false (left varies vs V2)
 *  - len>0,  val!=NULL -> true
 *  - len>0,  val=NULL  -> false (right varies vs V2)
 *
 * @since 0.1.0
 */
RA8_PRIV bool ra8_ble_gatt_internal_should_copy(uint16_t len, const void* value);

/**
 * @brief Pure predicate: declaration is missing OR notify-property bit clear.
 *
 * @details
 * Promoted from the inline OR at libs/ra8_ble_host/src/ra8_ble_gatt.c
 * inside @c ra8_ble_host_gatt_notify.
 *
 * @param[in] decl_present  Non-zero if the declaration row exists.
 * @param[in] props         Properties bitmask from the declaration row.
 * @param[in] notify_mask   Notify property bit mask.
 *
 * @return Boolean reject-notify predicate.
 * @retval true  Caller must return @c k_ra8_err_invalid_arg.
 * @retval false Notify is permitted.
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
 *  - present=1, notify_bit_set     -> false
 *  - present=0, notify_bit_set     -> true (varies left)
 *  - present=1, notify_bit_clear   -> true (varies right)
 *
 * @since 0.1.0
 */
RA8_PRIV bool
ra8_ble_gatt_internal_notify_invalid(uint8_t decl_present, uint8_t props, uint8_t notify_mask);

#ifdef __cplusplus
}
#endif
