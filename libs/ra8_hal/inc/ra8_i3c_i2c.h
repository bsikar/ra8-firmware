/**
 * @file ra8_i3c_i2c.h
 * @brief IIC_B (I3C unified IP, I2C-only mode) controller driver
 * @ingroup grp_hal_comms
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Polling-mode controller driver for the RA8D2 I3C peripheral operated in
 * I2C compatibility mode (HUM Ch 40 "I3C Bus Interface (I3C)",
 * p 2445-2701). The peripheral name in FSP and in this codebase is
 * ``IIC_B`` -- it replaces the legacy IIC block that older RA parts
 * carried.
 *
 * The public surface mirrors FSP ``r_iic_b_master`` minus DTC:
 *
 * - ``ra8_i3c_i2c_init``           configure + MSTP enable
 * - ``ra8_i3c_i2c_deinit``         disable + MSTP release
 * - ``ra8_i3c_i2c_set_clock``      retune SCL without tearing down
 * - ``ra8_i3c_i2c_write``          polling write to a 7-bit target,
 *                               with optional repeated-START suppression
 *                               of the trailing STOP
 * - ``ra8_i3c_i2c_read``           polling read from a 7-bit target,
 *                               with optional repeated-START suppression
 *                               of the trailing STOP
 * - ``ra8_i3c_i2c_transfer``       combined write-then-RESTART-then-read
 *                               (the typical "address a register, read
 *                               its contents" pattern)
 * - ``ra8_i3c_i2c_abort``          cancel an in-flight transaction and
 *                               return the channel to idle
 * - ``ra8_i3c_i2c_scan``           probe a 7-bit target without payload
 * - ``ra8_i3c_i2c_attach_handler`` register completion / error callback
 * - ``ra8_i3c_i2c_get_errors`` / ``ra8_i3c_i2c_clear_errors``
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/* =============================================================================
 * Public types
 * =============================================================================
 */

/**
 * @enum ra8_i3c_i2c_speed_t
 * @brief Supported bus speeds (I2C compatibility mode).
 *
 * @details
 * Per HUM Ch 40.1.1 Table 40.1 "I2C specifications", p 2445.
 */
typedef enum : uint32_t {
  k_ra8_i3c_i2c_speed_standard  = 100000U,  /**< 100 kHz Sm. */
  k_ra8_i3c_i2c_speed_fast      = 400000U,  /**< 400 kHz Fm. */
  k_ra8_i3c_i2c_speed_fast_plus = 1000000U, /**< 1 MHz Fm+.  */
} ra8_i3c_i2c_speed_t;

/**
 * @struct ra8_i3c_i2c_cfg_t
 * @brief Configuration descriptor for ``ra8_i3c_i2c_init``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra8_i3c_i2c_init`` in
 * ``libs/ra8_hal/src/ra8_i3c_i2c.c``.
 */
typedef struct {
  uint32_t bus_hz;   /**< Target I2C clock rate in Hz.                     */
  uint32_t pclka_hz; /**< Current PCLKA / I3CCLK frequency for STDBR calc. */
} ra8_i3c_i2c_cfg_t;

/**
 * @enum ra8_i3c_i2c_err_mask_t
 * @brief Error-mask bits returned by ``ra8_i3c_i2c_get_errors``.
 */
typedef enum : uint8_t {
  k_ra8_i3c_i2c_err_none     = 0x00U, /**< No latched error. */
  k_ra8_i3c_i2c_err_arb_lost = 0x01U, /**< BST.ALF set.      */
  k_ra8_i3c_i2c_err_nack     = 0x02U, /**< BST.NACKDF set.   */
  k_ra8_i3c_i2c_err_timeout  = 0x04U, /**< BST.TODF set.     */
} ra8_i3c_i2c_err_mask_t;

/**
 * @typedef ra8_i3c_i2c_complete_fn_t
 * @brief Transfer-complete / error callback signature.
 *
 * @param[in] ctx      Caller-supplied context.
 * @param[in] err_mask OR of ``k_ra8_i3c_i2c_err_*`` bits; zero on success.
 */
typedef void (*ra8_i3c_i2c_complete_fn_t)(void* ctx, uint8_t err_mask);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise the IIC_B channel and bring the bus up.
 *
 * @param[in] channel Channel index (only ``0`` is valid on RA8D2).
 * @param[in] cfg     Configuration descriptor.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              Channel initialized, BCTL.BUSE = 1.
 * @retval k_ra8_err_null_ptr    ``cfg`` is NULL.
 * @retval k_ra8_err_invalid_arg ``channel`` out of range or
 *                              ``cfg->bus_hz`` zero.
 * @retval k_ra8_err_hw_timeout  RSTCTL.RI3CRST didn't self-clear.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_mstp_init`` has been called.
 * @post On success ``BCTL.BUSE`` is set and the channel is ready to
 *       service ``ra8_i3c_i2c_write`` / ``ra8_i3c_i2c_read``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_i2c_init(uint8_t channel, const ra8_i3c_i2c_cfg_t* cfg);

/**
 * @brief Tear down the IIC_B channel.
 *
 * @param[in] channel Channel index.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              Channel torn down, MSTP gated.
 * @retval k_ra8_err_invalid_arg ``channel`` out of range.
 *
 * @pre Caller is not in the middle of a transfer.
 * @post BCTL.BUSE cleared and I3C MSTP bit ref-released.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_i2c_deinit(uint8_t channel);

/**
 * @brief Update the bus clock without tearing the channel down.
 *
 * @param[in] channel  Channel index.
 * @param[in] bus_hz   New bus clock in Hz (non-zero).
 * @param[in] pclka_hz Current PCLKA frequency in Hz.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              STDBR programmed.
 * @retval k_ra8_err_invalid_arg Channel / clock out of range.
 *
 * @pre Channel previously initialized.
 * @post STDBR.SBRLO / SBRHO reflect the new divider.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_i2c_set_clock(uint8_t channel, uint32_t bus_hz, uint32_t pclka_hz);

/* =============================================================================
 * Polling transfers
 * =============================================================================
 */

/**
 * @brief Polling write of ``len`` bytes to a 7-bit target address.
 *
 * @details
 * Mirrors FSP's ``R_IIC_B_MASTER_Write`` flow without the DTC fast
 * path:
 *
 * 1. Reject the call if the bus is busy (BCST.BFREF == 0).
 * 2. Issue a START condition.
 * 3. Send ``(target_7b << 1) | 0`` as the address byte.
 * 4. Push each payload byte into NTDTBP0 once NTST.TDBEF0 sets.
 * 5. If ``restart == false`` issue a STOP; otherwise leave the bus
 *    held and return -- the caller is expected to follow this call
 *    immediately with another ``ra8_i3c_i2c_write`` / ``ra8_i3c_i2c_read``
 *    that will inject a repeated-START rather than a fresh START.
 *
 * State machine:
 * ``IDLE -> ADDR_TX -> DATA_TX -> { STOP | hold for RESTART } -> IDLE``.
 *
 * On NACK or arbitration loss the transaction is aborted (STOP issued
 * unconditionally) and the matching error code is returned.
 *
 * @param[in] channel   Channel index.
 * @param[in] target_7b 7-bit peripheral address.
 * @param[in] data      Buffer to send (must be non-NULL even when
 *                      ``len`` is zero).
 * @param[in] len       Byte count.
 * @param[in] restart   When ``true``, suppress the trailing STOP and
 *                      keep the bus held so the next call (typically a
 *                      read) issues a repeated-START. When ``false``,
 *                      STOP is issued and the bus is released.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              Transfer succeeded.
 * @retval k_ra8_err_null_ptr    ``data`` is NULL or channel invalid.
 * @retval k_ra8_err_busy        Bus busy at entry (BCST.BFREF clear).
 * @retval k_ra8_err_hw_timeout  TDBEF0 / TENDF poll timed out.
 * @retval k_ra8_err_nack        Peripheral NACKed; STOP was issued.
 * @retval k_ra8_err_hw_error    Arbitration lost; STOP was issued.
 *
 * @pre Channel previously initialized.
 * @post On ``k_ra8_ok`` and ``restart == false``: STOP issued, bus free.
 * @post On ``k_ra8_ok`` and ``restart == true``: bus held by controller;
 *       next call must be on the same channel/target.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_i2c_write(uint8_t        channel,
                                          uint8_t        target_7b,
                                          const uint8_t* data,
                                          uint32_t       len,
                                          bool           restart);

/**
 * @brief Polling read of ``len`` bytes from a 7-bit target.
 *
 * @details
 * Mirrors FSP's ``R_IIC_B_MASTER_Read`` minus DTC:
 *
 * 1. Reject the call if the bus is busy (BCST.BFREF == 0) AND no
 *    repeated-START is in progress (i.e. caller did not hold the bus
 *    via a prior ``ra8_i3c_i2c_write(..., restart=true)``).
 * 2. Issue a START (or RESTART when the bus is already held).
 * 3. Send ``(target_7b << 1) | 1`` as the address byte.
 * 4. Drain ``len`` bytes from NTDTBP0; ACK every byte except the
 *    last, which is NACKed via ACKCTL.ACKT (paired with ACKTWP).
 * 5. Issue a STOP unless ``restart == true``.
 *
 * State machine:
 * ``IDLE -> ADDR_TX -> DATA_RX -> { STOP | hold for RESTART } -> IDLE``.
 *
 * @param[in]  channel   Channel index.
 * @param[in]  target_7b 7-bit peripheral address.
 * @param[out] buf       Destination buffer (non-NULL).
 * @param[in]  len       Byte count (non-zero).
 * @param[in]  restart   When ``true``, suppress the trailing STOP.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              Transfer succeeded.
 * @retval k_ra8_err_null_ptr    ``buf`` is NULL or channel invalid.
 * @retval k_ra8_err_invalid_arg ``len`` is zero.
 * @retval k_ra8_err_busy        Bus busy at entry.
 * @retval k_ra8_err_hw_timeout  RDBFF0 / TENDF poll timed out.
 * @retval k_ra8_err_nack        Peripheral NACKed the address byte.
 * @retval k_ra8_err_hw_error    Arbitration lost.
 *
 * @pre Channel previously initialized.
 * @post BST status flags cleared.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_i3c_i2c_read(uint8_t channel, uint8_t target_7b, uint8_t* buf, uint32_t len, bool restart);

/**
 * @brief Combined write-then-RESTART-then-read in a single bus
 *        transaction.
 *
 * @details
 * Convenience wrapper for the most common I2C pattern: write a
 * register address, then read its contents back from the same target.
 * Internally invokes ``ra8_i3c_i2c_write(..., restart=true)`` followed by
 * ``ra8_i3c_i2c_read(..., restart=false)``. State machine:
 *
 * ``IDLE -> ADDR_TX -> DATA_TX -> RESTART -> ADDR_TX(read) -> DATA_RX -> STOP -> IDLE``
 *
 * If either ``tx_len`` or ``rx_len`` is zero the corresponding phase
 * is skipped (e.g. ``tx_len = 0`` degenerates to a plain read).
 *
 * @param[in]  channel   Channel index.
 * @param[in]  target_7b 7-bit peripheral address.
 * @param[in]  tx        Bytes to send first (e.g. register address).
 *                       May be NULL only when ``tx_len == 0``.
 * @param[in]  tx_len    Number of bytes to send.
 * @param[out] rx        Destination buffer for the read phase.
 *                       May be NULL only when ``rx_len == 0``.
 * @param[in]  rx_len    Number of bytes to read.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              Transfer succeeded; STOP issued.
 * @retval k_ra8_err_null_ptr    ``tx``/``rx`` NULL with non-zero len, or
 *                              channel invalid.
 * @retval k_ra8_err_invalid_arg Both ``tx_len`` and ``rx_len`` are zero.
 * @retval k_ra8_err_busy        Bus busy at entry.
 * @retval k_ra8_err_nack        Peripheral NACKed.
 * @retval k_ra8_err_hw_timeout  Poll timed out.
 *
 * @pre Channel previously initialized.
 * @post Bus is released regardless of outcome.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_i2c_transfer(uint8_t        channel,
                                             uint8_t        target_7b,
                                             const uint8_t* tx,
                                             uint32_t       tx_len,
                                             uint8_t*       rx,
                                             uint32_t       rx_len);

/**
 * @brief Cancel any in-flight transaction and return the channel to
 *        idle.
 *
 * @details
 * Mirrors FSP's ``R_IIC_B_MASTER_Abort`` controller abort-sequence helper
 * for the polling driver. Steps:
 *
 * 1. Mask BIE / NTIE so a pending interrupt cannot fire mid-tear-down.
 * 2. Issue STOP (unconditional). The hardware will eventually drive
 *    SPCNDDF; the polling helpers don't gate on it because no further
 *    bus traffic is expected from this channel until the next
 *    ``ra8_i3c_i2c_write`` / ``_read`` clears BST again.
 * 3. Clear all latched bus-status flags.
 *
 * @param[in] channel Channel index.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              Abort issued.
 * @retval k_ra8_err_invalid_arg Channel out of range.
 *
 * @pre Channel previously initialized.
 * @post Channel is idle; BST flags cleared.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_i2c_abort(uint8_t channel);

/**
 * @brief Probe whether a 7-bit address ACKs.
 *
 * @details
 * Issues START, writes the address byte, waits for the ACK / NACK
 * status, and issues STOP. Equivalent to a single ``i2cdetect``
 * sweep entry.
 *
 * @param[in]  channel   Channel index.
 * @param[in]  target_7b 7-bit peripheral address.
 * @param[out] out_acked Set to ``true`` when the peripheral ACKs, ``false``
 *                       on NACK.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              Probe completed (ACK or NACK).
 * @retval k_ra8_err_null_ptr    ``out_acked`` is NULL or channel invalid.
 * @retval k_ra8_err_hw_timeout  Status poll timed out.
 *
 * @pre Channel previously initialized.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_i2c_scan(uint8_t channel, uint8_t target_7b, bool* out_acked);

/* =============================================================================
 * Status
 * =============================================================================
 */

/**
 * @brief Read latched error flags from BST (AL / NACKDF / TODF).
 *
 * @param[in]  channel  Channel index.
 * @param[out] out_mask OR of ``k_ra8_i3c_i2c_err_*`` bits.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              ``out_mask`` populated.
 * @retval k_ra8_err_null_ptr    ``out_mask`` is NULL.
 * @retval k_ra8_err_invalid_arg ``channel`` out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_i2c_get_errors(uint8_t channel, uint8_t* out_mask);

/**
 * @brief Clear latched error flags in BST.
 *
 * @param[in] channel Channel index.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              Error bits W0C cleared.
 * @retval k_ra8_err_invalid_arg Channel out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i3c_i2c_clear_errors(uint8_t channel);

/* =============================================================================
 * Interrupt path
 * =============================================================================
 */

/**
 * @brief Attach a completion / error callback for the channel.
 *
 * @param[in] channel Channel index.
 * @param[in] fn      Callback fired from the dispatch helpers, or
 *                    NULL to detach.
 * @param[in] ctx     Context pointer passed to the callback.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              Callback registered.
 * @retval k_ra8_err_invalid_arg Channel out of range.
 *
 * @pre Channel previously initialized.
 * @post BIE / NTIE bits track ``fn`` non-null status.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_i3c_i2c_attach_handler(uint8_t channel, ra8_i3c_i2c_complete_fn_t fn, void* ctx);

/**
 * @brief Dispatch the bus-error IRQ source.
 *
 * @details
 * Test-callable shim that mirrors the ERI handler in FSP's
 * ``r_iic_b_master``: it samples BST, masks the latched error bits
 * back to ``k_ra8_i3c_i2c_err_*``, clears them, and fires the registered
 * callback with that mask if it is non-zero.
 *
 * @param[in] channel Channel index.
 *
 * @since 0.1.0
 *
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 */
void ra8_i3c_i2c_dispatch_eri(uint8_t channel);

#ifdef __cplusplus
}
#endif
