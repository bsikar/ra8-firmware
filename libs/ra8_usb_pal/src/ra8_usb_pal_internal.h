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

/**
 * @brief Pure translation: INTSTS0 snapshot -> PAL event bit set.
 *
 * @details
 * Promoted from @c internal_translate in
 * libs/ra8_usb_pal/src/ra8_usb_pal.c so the mapping can be driven by
 * host vectors without MMIO. ``ra8_usb_dispatch``
 * (libs/ra8_hal/src/ra8_usb_irq.c) hands the PAL handler the raw
 * INTSTS0 word, which carries both the edge bits and the CTSQ /
 * VALID / DVSQ / VBSTS sub-state fields, so every bit the taxonomy
 * can name is derivable from this one input.
 *
 * Mapping, one arm per source bit (HUM Ch 36.2.14):
 *  - SOFR  -> @c k_ra8_usb_pal_event_sof
 *  - RSME  -> @c k_ra8_usb_pal_event_resume
 *  - VBSE  -> @c _attach when VBSTS is set, else @c _detach
 *  - DVST  -> @c _suspend / @c _reset per DVSQ; Address, Configured
 *             and Powered have no taxonomy bit and yield nothing
 *  - CTRT  -> @c _setup while VALID is latched; @c _error on CTSQ=SQER
 *  - BEMP  -> @c k_ra8_usb_pal_event_ep_in
 *  - BRDY  -> @c k_ra8_usb_pal_event_ep_out
 *  - NRDY  -> @c k_ra8_usb_pal_event_error
 *
 * BRDY is the one coarse arm: it asserts both for an OUT pipe holding
 * data and for an IN pipe whose buffer is free again, and telling the
 * two apart needs BRDYSTS, which the PAL never receives. A consumer
 * that needs per-pipe truth must read BRDYSTS itself.
 *
 * @param[in] intsts0 Unmodified INTSTS0 snapshot from ``ra8_usb``.
 *
 * @return OR of @c k_ra8_usb_pal_event_* bits.
 * @retval k_ra8_usb_pal_event_none No bit in @p intsts0 maps to a
 *         named event (including a zero snapshot).
 *
 * @pre None.
 * @pre No global state is read.
 * @post No state mutated.
 * @post Return depends solely on @p intsts0.
 *
 * @note Test-access only. Pure function; safe from ISR context.
 * @since 0.1.0
 */
RA8_PRIV
uint16_t priv_usb_pal_translate_event(uint16_t intsts0);

#ifdef __cplusplus
}
#endif
