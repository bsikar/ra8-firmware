/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_i3c_i2c_internal.h
 * @brief Test-access surface for ra8_i3c_i2c internal helpers (MC/DC).
 * @ingroup grp_hal_comms
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_i3c_i2c.h"
#include "ra8_i3c_i2c_regs.h"

/**
 * @struct ra8_i3c_i2c_state_t
 * @brief Per-channel dispatch state shared across the driver's TUs.
 *
 * @details
 * Tracks the registered completion callback, its context, the
 * initialization flag, and the held-bus flag used to inject a RESTART
 * instead of a fresh START on a chained transaction. Defined as the
 * @c s_iic_b_state table in @c ra8_i3c_i2c.c.
 *
 * @invariant @c bus_held is true only between a @c restart=true transaction
 *            and the next chained call on the same channel.
 *
 * @see s_iic_b_state
 * @since 0.1.0
 */
typedef struct {
  ra8_i3c_i2c_complete_fn_t cb;          /**< Callback or NULL. */
  void*                     ctx;         /**< Callback context. */
  bool                      initialized; /**< Tracks ``internal_i3c_i2c_init`` /
                                         ``internal_i3c_i2c_deinit``.           */
  bool                      bus_held;    /**< True when the previous
                                         transaction returned with
                                         ``restart=true`` and the next
                                         call must inject a RESTART
                                         instead of a fresh START.      */
} ra8_i3c_i2c_state_t;

/**
 * @var s_iic_b_state
 * @brief Per-channel state table indexed by channel.
 *
 * @details
 * Defined in @c ra8_i3c_i2c.c; the control-plane TU (@c abort / @c scan /
 * @c attach_handler / @c dispatch_eri) reads and updates the held-bus and
 * callback fields through this @c extern declaration.
 *
 * @note Not thread-safe; serialise channel access at a higher layer.
 * @warning Owned by @c ra8_i3c_i2c.c; do not define elsewhere.
 * @since 0.1.0
 */
extern ra8_i3c_i2c_state_t s_iic_b_state[k_ra8_i3c_i2c_channel_count];

/**
 * @brief Issue a START condition on the given channel registers.
 *
 * @details
 * Shared START helper writing @c CNDCTL.STCND. Promoted to TU-external
 * linkage so the control-plane TU (@c internal_i3c_i2c_scan) and the
 * data-path TU can both issue it.
 *
 * @param[in] reg Channel register block (non-NULL).
 *
 * @pre @p reg points to a valid IIC_B register block.
 * @pre The channel has been brought up by @c internal_i3c_i2c_init.
 * @post @c CNDCTL.STCND is set, requesting a START on the bus.
 * @post No other register is modified.
 *
 * @note Not thread-safe; serialise per channel.
 * @since 0.1.0
 */
RA8_PRIV
void internal_i3c_i2c_start(volatile r_i3c_i2c_regs_t* reg);

/**
 * @brief Issue a STOP condition and clear the prior STOP-detect flag.
 *
 * @details
 * Shared STOP helper writing @c CNDCTL.SPCND after a W0C clear of
 * @c BST.SPCNDDF. Promoted to TU-external linkage so the control-plane TU
 * (@c abort / @c scan) and the data-path TU can both issue it.
 *
 * @param[in] reg Channel register block (non-NULL).
 *
 * @pre @p reg points to a valid IIC_B register block.
 * @pre The channel has been brought up by @c internal_i3c_i2c_init.
 * @post @c CNDCTL.SPCND is set, requesting a STOP on the bus.
 * @post The prior @c BST.SPCNDDF flag is cleared.
 *
 * @note Not thread-safe; serialise per channel.
 * @since 0.1.0
 */
RA8_PRIV
void internal_i3c_i2c_stop(volatile r_i3c_i2c_regs_t* reg);

/**
 * @brief Clear all latched bus-status flags ahead of a new transaction.
 *
 * @details
 * Shared BST scrub used before every transaction. Promoted to TU-external
 * linkage so the control-plane TU (@c abort / @c scan) and the data-path TU
 * can both clear the latched flags.
 *
 * @param[in] reg Channel register block (non-NULL).
 *
 * @pre @p reg points to a valid IIC_B register block.
 * @pre The channel has been brought up by @c internal_i3c_i2c_init.
 * @post The transaction-related @c BST flags are cleared (W0C).
 * @post No other register is modified.
 *
 * @note Not thread-safe; serialise per channel.
 * @since 0.1.0
 */
RA8_PRIV
void internal_i3c_i2c_clear_bst(volatile r_i3c_i2c_regs_t* reg);

/**
 * @brief Send a single address byte and wait for TDBEF0 readiness.
 *
 * @details
 * Shared address-phase helper. Promoted to TU-external linkage so the
 * control-plane TU (@c internal_i3c_i2c_scan) and the data-path TU can both
 * push the pre-shifted address byte and forward timeout / NACK detection.
 *
 * @param[in] reg          Channel registers (non-NULL).
 * @param[in] address_byte Pre-shifted 7-bit address with the R/W bit set.
 *
 * @return ::ra8_err_t outcome of the address phase.
 * @retval k_ra8_ok            Address byte written, TDBEF0 ready.
 * @retval k_ra8_err_hw_timeout TDBEF0 never set within the poll budget.
 *
 * @pre @p reg points to a valid IIC_B register block.
 * @pre A START / RESTART has already been requested.
 * @post On success the address byte is in @c NTDTBP0.
 * @post No global state is modified on the timeout path.
 *
 * @note Not thread-safe; serialise per channel.
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t internal_i3c_i2c_send_address(volatile r_i3c_i2c_regs_t* reg, uint8_t address_byte);

/**
 * @brief Pure predicate: non-zero length AND a NULL buffer pointer.
 *
 * @details
 * Reusable for both the @c (tx_len != 0 && tx == NULL) and
 * @c (rx_len != 0 && rx == NULL) checks at libs/ra8_hal/src/ra8_i3c_i2c.c
 * lines 973 and 976 inside @c internal_i3c_i2c_read.
 *
 * @param[in] len Length in bytes (zero means "not used").
 * @param[in] buf Buffer pointer (NULL means "not provided").
 *
 * @return Boolean reject predicate.
 * @retval true  Caller must return @c k_ra8_err_null_ptr.
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
RA8_PRIV
bool internal_i3c_i2c_len_buf_invalid(uint32_t len, const void* buf);

/**
 * @brief Pure predicate: non-zero error mask AND a non-NULL callback.
 *
 * @details
 * Promoted from the inline AND at libs/ra8_hal/src/ra8_i3c_i2c.c
 * inside @c internal_i3c_i2c_dispatch_eri.
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
RA8_PRIV
bool internal_i3c_i2c_should_dispatch(uint8_t mask, const void* cb);

#ifdef __cplusplus
}
#endif
