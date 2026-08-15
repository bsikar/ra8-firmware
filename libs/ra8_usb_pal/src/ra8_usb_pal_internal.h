/**
 * @file ra8_usb_pal_internal.h
 * @brief Test-access surface for ra8_usb_pal internal helpers (MC/DC).
 * @details Declares the module-private pure predicates shared by the PAL implementation and its focused hosted qualification test.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 3 / PAL] {World: NS}
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
 * @brief Pure predicate: callback non-NULL AND mask non-zero.
 *
 * @details
 * Promoted from the inline AND at libs/ra8_usb_pal/src/ra8_usb_pal.c
 * inside @c internal_usb_event.
 *
 * @param[in] event_fn   Application callback (NULL when none).
 * @param[in] mask       Translated event-mask bits.
 * @param[in] none_value Numeric value of @c k_ra8_usb_pal_event_none.
 *
 * @return Boolean dispatch predicate.
 * @retval true  Caller must invoke @p event_fn.
 * @retval false Skip the callback.
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
 *  - cb=NULL, mask=evt -> false (left varies vs V2)
 *  - cb!=NULL, mask=evt -> true
 *  - cb!=NULL, mask=none -> false (right varies vs V2)
 *
 * @since 0.1.0
 */
RA8_PRIV
bool priv_usb_pal_should_dispatch_event(const void* event_fn, uint16_t mask, uint16_t none_value);

/**
 * @brief Pure predicate: endpoint number is zero OR exceeds max.
 *
 * @details
 * Promoted from the inline OR at libs/ra8_usb_pal/src/ra8_usb_pal.c
 * inside @c ra8_usb_pal_ep_recv.
 *
 * @param[in] ep_addr Endpoint number (after addr_mask strip).
 * @param[in] ep_max  Maximum permitted endpoint number.
 *
 * @return Boolean reject predicate.
 * @retval true  Caller must return @c k_ra8_err_invalid_arg.
 * @retval false Endpoint is in range.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the two inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * 2-condition OR; N+1 = 3 vectors:
 *  - ep=1,    ep_max=10 -> false
 *  - ep=0,    ep_max=10 -> true (varies left)
 *  - ep=11,   ep_max=10 -> true (varies right)
 *
 * @since 0.1.0
 */
RA8_PRIV
bool priv_usb_pal_ep_out_of_range(uint8_t ep_addr, uint8_t ep_max);

#ifdef __cplusplus
}
#endif
