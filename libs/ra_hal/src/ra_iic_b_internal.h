/**
 * @file ra_iic_b_internal.h
 * @brief Test-access surface for ra_iic_b internal helpers (MC/DC).
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
 * @brief Pure predicate: non-zero length AND a NULL buffer pointer.
 *
 * @details
 * Reusable for both the @c (tx_len != 0 && tx == NULL) and
 * @c (rx_len != 0 && rx == NULL) checks at libs/ra_hal/src/ra_iic_b.c
 * lines 973 and 976 inside @c ra_iic_b_read.
 *
 * @param[in] len Length in bytes (zero means "not used").
 * @param[in] buf Buffer pointer (NULL means "not provided").
 *
 * @return Boolean reject predicate.
 * @retval true  Caller must return @c k_ra_err_null_ptr.
 * @retval false Combination is valid.
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
 *  - len=0,  buf=NULL   -> false
 *  - len>0,  buf=NULL   -> true (varies left)
 *  - len>0,  buf!=NULL  -> false (varies right)
 *
 * @since 0.1.0
 */
bool ra_iic_b_internal_len_buf_invalid(uint32_t len, const void* buf);

/**
 * @brief Pure predicate: non-zero error mask AND a non-NULL callback.
 *
 * @details
 * Promoted from the inline AND at libs/ra_hal/src/ra_iic_b.c
 * inside @c ra_iic_b_dispatch_eri.
 *
 * @param[in] mask Bitmask of pending error sources.
 * @param[in] cb   Callback pointer (NULL when none registered).
 *
 * @return Boolean dispatch-callback predicate.
 * @retval true  Caller must invoke @p cb.
 * @retval false Skip the callback.
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
 *  - mask=0,  cb=NULL    -> false
 *  - mask!=0, cb=NULL    -> false (varies right -> still false)
 *  - mask!=0, cb!=NULL   -> true  (both true)
 *  Reorder for MC/DC isolation:
 *  - V1: mask!=0, cb!=NULL -> true
 *  - V2: mask=0,  cb!=NULL -> false (varies mask)
 *  - V3: mask!=0, cb=NULL  -> false (varies cb)
 *
 * @since 0.1.0
 */
bool ra_iic_b_internal_should_dispatch(uint8_t mask, const void* cb);

#ifdef __cplusplus
}
#endif
