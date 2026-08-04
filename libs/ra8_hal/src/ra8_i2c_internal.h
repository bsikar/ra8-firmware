/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_i2c_internal.h
 * @brief Test-access surface for ra8_i2c internal helpers (MC/DC).
 * @ingroup grp_hal_comms
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_i2c.h"
#include "ra8_i2c_regs.h"

/**
 * @brief Pure predicate: either clock argument is zero.
 *
 * @details
 * Promoted from the OR decision in ``internal_i2c_bitrate`` (also used
 * by ``ra8_i2c_init``) so the two conditions can be exercised with
 * independent influence under MC/DC.
 *
 * @param[in] bus_hz   Target bus clock in Hz.
 * @param[in] pclkb_hz Reference PCLKB clock in Hz.
 *
 * @return Boolean reject predicate.
 * @retval true  At least one clock is zero (invalid).
 * @retval false Both clocks are non-zero.
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
 *  - V1: bus!=0, pclkb!=0 -> false
 *  - V2: bus=0,  pclkb!=0 -> true  (varies left)
 *  - V3: bus!=0, pclkb=0  -> true  (varies right)
 *
 * @since 0.1.0
 */
RA8_PRIV
bool ra8_i2c_internal_clk_invalid(uint32_t bus_hz, uint32_t pclkb_hz);

/**
 * @var s_i2c_tag
 * @brief Log tag shared by the I2C transfer and configuration planes.
 *
 * @details
 * Defined once in ``ra8_i2c.c`` (the data-transfer translation unit) and
 * consumed by both ``ra8_i2c.c`` and ``ra8_i2c_config.c`` so the two halves
 * of the split driver log under the same "I2C" tag.
 *
 * @note Read-only string pointer; not mutated after static init.
 *
 * @since 0.1.0
 */
extern const char* const s_i2c_tag;

/**
 * @struct ra8_i2c_state_t
 * @brief Per-channel driver state shared by both I2C translation units.
 *
 * @details
 * Tracks initialization and bus-held ownership for each RIIC channel. The
 * data-transfer plane sets ``bus_held`` after a ``send_stop = false`` write
 * and the configuration plane clears both fields on ``ra8_i2c_init`` /
 * ``ra8_i2c_deinit``. The target plane owns ``peripheral_active`` and the
 * attached address-match callback (``peripheral_handler`` / ``peripheral_ctx``)
 * that ``ra8_i2c_peripheral_dispatch`` fires.
 *
 * @invariant ``bus_held`` is only true between a held write and its chained
 *            transfer; cleared on every STOP path and on (de)init.
 * @invariant ``peripheral_handler`` is NULL unless a caller attached one.
 *
 * @see s_i2c_state
 *
 * @since 0.1.0
 */
typedef struct {
  bool                          initialized; /**< Tracks ``ra8_i2c_init`` / ``ra8_i2c_deinit``. */
  bool                          bus_held;    /**< True when the previous write returned with
                         ``send_stop=false`` so the next call must inject a
                         repeated-START instead of a fresh START.            */
  bool                          peripheral_active; /**< True between ``ra8_i2c_peripheral_init`` and
                         ``ra8_i2c_peripheral_deinit`` -- own-address matching is
                         armed and the target transfer calls may run.        */
  ra8_i2c_peripheral_event_fn_t peripheral_handler; /**< Address-match callback
                         fired by ``ra8_i2c_peripheral_dispatch``; NULL when no
                         handler is attached.                                */
  void* peripheral_ctx; /**< Opaque context passed to ``peripheral_handler``. */
} ra8_i2c_state_t;

/**
 * @var s_i2c_state
 * @brief Per-channel state table indexed by channel.
 *
 * @details
 * Defined once in ``ra8_i2c.c`` and shared with ``ra8_i2c_config.c`` so the
 * bring-up and transfer planes observe the same bus-ownership state.
 *
 * @warning Mutated only by the driver under the not-thread-safe contract.
 *
 * @see ra8_i2c_state_t
 *
 * @since 0.1.0
 */
extern ra8_i2c_state_t s_i2c_state[k_ra8_i2c_channel_count];

/* =============================================================================
 * Target (peripheral) role -- pure decision predicates promoted to TU-external
 * linkage so their compound decisions can be exercised under MC/DC. Production
 * callers live in ``ra8_i2c_peripheral.c``; the only out-of-TU consumers are the
 * host tests under ``tests/``.
 * =============================================================================
 */

/**
 * @brief Poll-exit predicate: an own-address match or a STOP was observed.
 *
 * @details
 * Drives the bounded wait in ``ra8_i2c_peripheral_poll``. The match flags come
 * from ICSR1 (AAS0/1/2, GCA); STOP comes from ICSR2. Promoted so the OR can
 * be exercised with independent influence under MC/DC.
 *
 * @param[in] icsr1 Snapshot of ICSR1 (own-address detection flags).
 * @param[in] icsr2 Snapshot of ICSR2 (STOP detection flag).
 *
 * @return Whether the poll loop should stop spinning.
 * @retval true  An own-address match or a STOP condition is latched.
 * @retval false Neither -- keep spinning.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the two inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * Decision: ``(icsr1 & match) != 0 || (icsr2 & stop) != 0`` (2 conditions);
 * N+1 = 3 vectors:
 *  - V1: match=0, stop=0 -> false (control: both false)
 *  - V2: match=1, stop=0 -> true  (varies left)
 *  - V3: match=0, stop=1 -> true  (varies right)
 *
 * @since 0.1.0
 */
RA8_PRIV
bool ra8_i2c_internal_peripheral_poll_done(uint8_t icsr1, uint8_t icsr2);

/**
 * @brief Receive-loop predicate: keep draining while no STOP and room remains.
 *
 * @details
 * Drives the per-byte loop in ``ra8_i2c_peripheral_receive``. Promoted so the AND
 * can be exercised with independent influence under MC/DC.
 *
 * @param[in] icsr2    Snapshot of ICSR2 (STOP detection flag).
 * @param[in] received Bytes stored so far.
 * @param[in] capacity Destination buffer size.
 *
 * @return Whether another byte may be received into the buffer.
 * @retval true  No STOP latched and ``received`` < ``capacity``.
 * @retval false A STOP is latched or the buffer is full.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * Decision: ``(icsr2 & stop) == 0 && received < capacity`` (2 conditions);
 * N+1 = 3 vectors:
 *  - V1: stop=0, received<capacity -> true  (control: both true)
 *  - V2: stop=1, received<capacity -> false (varies left)
 *  - V3: stop=0, received==capacity -> false (varies right)
 *
 * @since 0.1.0
 */
RA8_PRIV
bool ra8_i2c_internal_peripheral_rx_continue(uint8_t icsr2, uint32_t received, uint32_t capacity);

/**
 * @brief Drain controller-write data bytes from ICDRR into ``buf``.
 *
 * @details
 * The per-byte receive loop extracted from ``ra8_i2c_peripheral_receive`` so the
 * public entry stays within the NASA Rule 4 statement budget. Each iteration
 * waits for ICSR2.RDRF or STOP, then -- while ``ra8_i2c_internal_peripheral_rx_continue``
 * holds (no STOP and room remains) -- copies one ICDRR byte. On STOP or a full
 * buffer it drains a final pending byte (only when RDRF is set and room is
 * left) and stops. The loop is bounded by ``capacity + 1`` (NASA P10 Rule 2).
 * Promoted to TU-external linkage so the final-byte drain guard can be
 * exercised with independent influence: the ra8_fake MMIO window is
 * side-effect-free, so the public ``ra8_i2c_peripheral_receive`` path cannot
 * present RDRF set at the address-phase wait and clear at this guard.
 *
 * @param[in]  reg       Channel register block.
 * @param[out] buf       Destination buffer.
 * @param[in]  capacity  Buffer size in bytes (non-zero).
 * @param[out] out_count Number of bytes stored.
 *
 * @return ``ra8_err_t`` outcome of the last status poll.
 * @retval k_ra8_ok             Loop ended on STOP or a full buffer.
 * @retval k_ra8_err_hw_timeout A status poll exhausted its spin budget.
 *
 * @pre reg, buf and out_count are non-NULL.
 * @pre The address-phase dummy read already consumed the matched address.
 * @post ``*out_count`` <= ``capacity``.
 * @post Bytes ``[0, *out_count)`` of ``buf`` hold the received payload.
 * @note Thread safety: not thread-safe (drives a single channel).
 * @note Test-access only outside the defining TU.
 *
 * @par MC/DC:
 * Decision: ``((icsr2 & rdrf) != 0) && (count < capacity)`` (2 conditions, the
 * final-pending-byte drain guard); N+1 = 3 vectors:
 *  - V1: rdrf set, count<capacity  -> true  (control: drains one trailing byte)
 *  - V2: rdrf clear                -> false (varies left)
 *  - V3: rdrf set, count==capacity -> false (varies right)
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t ra8_i2c_internal_target_drain_rx(volatile r_i2c_regs_t* reg,
                                           uint8_t*               buf,
                                           uint32_t               capacity,
                                           uint32_t*              out_count);

/**
 * @brief Transmit-completion predicate: the controller ended the read.
 *
 * @details
 * Used by ``ra8_i2c_peripheral_transmit`` after the data loop to confirm the frame
 * closed cleanly. Promoted so the OR can be exercised under MC/DC.
 *
 * @param[in] icsr2 Snapshot of ICSR2 (NACKF and TEND flags).
 *
 * @return Whether the controller has ended the read transaction.
 * @retval true  NACKF or TEND is latched.
 * @retval false Neither -- transmission is still in progress.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the input snapshot.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * Decision: ``(icsr2 & nackf) != 0 || (icsr2 & tend) != 0`` (2 conditions);
 * N+1 = 3 vectors:
 *  - V1: nackf=0, tend=0 -> false (control: both false)
 *  - V2: nackf=1, tend=0 -> true  (varies left)
 *  - V3: nackf=0, tend=1 -> true  (varies right)
 *
 * @since 0.1.0
 */
RA8_PRIV
bool ra8_i2c_internal_peripheral_tx_done(uint8_t icsr2);

/**
 * @brief Transmit-loop predicate: keep sending while no NACK and data remains.
 *
 * @details
 * Drives the per-byte loop in ``ra8_i2c_peripheral_transmit``. Promoted so the AND
 * can be exercised with independent influence under MC/DC.
 *
 * @param[in] icsr2 Snapshot of ICSR2 (NACKF detection flag).
 * @param[in] sent  Bytes accepted by the controller so far.
 * @param[in] len   Number of bytes available to send.
 *
 * @return Whether another byte should be queued to ICDRT.
 * @retval true  No NACK latched and ``sent`` < ``len``.
 * @retval false The controller NACKed or all bytes are queued.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * Decision: ``(icsr2 & nackf) == 0 && sent < len`` (2 conditions);
 * N+1 = 3 vectors:
 *  - V1: nackf=0, sent<len -> true  (control: both true)
 *  - V2: nackf=1, sent<len -> false (varies left)
 *  - V3: nackf=0, sent==len -> false (varies right)
 *
 * @since 0.1.0
 */
RA8_PRIV
bool ra8_i2c_internal_peripheral_tx_continue(uint8_t icsr2, uint32_t sent, uint32_t len);

#ifdef __cplusplus
}
#endif
