/**
 * @file ra_i2c.h
 * @brief I2C Bus Interface (IIC) controller driver -- polling mode
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Polling-mode controller driver for the RA8D2 RIIC peripheral
 * (channels IIC0/IIC1/IIC2 at ``0x4025_E000 + 0x0100 * n``). This is
 * the classic Renesas RIIC block described in HUM Ch 39 "I2C Bus
 * Interface (IIC)" p 2367-2470, distinct from the I3C unified IP that
 * ``ra_i3c`` / ``ra_i3c_i2c`` drive. The board's Grove / Pmod /
 * mikroBUS / Arduino I2C bus and the U15 configuration-switch port
 * expander live on IIC1 (P511/P512).
 *
 * The driver operates only as an I2C controller. The transfer surface
 * mirrors FSP ``r_iic_master`` minus DTC / interrupt fast paths:
 *
 * - ``ra_i2c_init``         configure + MSTP enable + bit-rate program
 * - ``ra_i2c_deinit``       disable + MSTP release
 * - ``ra_i2c_set_clock``    retune SCL without tearing down
 * - ``ra_i2c_write``        polling write to a 7-bit peripheral, with
 *                           optional repeated-START suppression of the
 *                           trailing STOP
 * - ``ra_i2c_read``         polling read from a 7-bit peripheral
 * - ``ra_i2c_transfer``     combined write-then-RESTART-then-read
 * - ``ra_i2c_scan``         probe a 7-bit address without payload
 * - ``ra_i2c_get_errors`` / ``ra_i2c_clear_errors``
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/* =============================================================================
 * Public types
 * =============================================================================
 */

/**
 * @enum ra_i2c_speed_t
 * @brief Supported bus speeds.
 *
 * @details
 * Per HUM Ch 39.1 Table 39.1 "IIC specifications" p 2367: Fast-mode
 * Plus is supported up to 1 Mbps.
 */
typedef enum : uint32_t {
  k_ra_i2c_speed_standard  = 100000U,  /**< 100 kHz Standard mode (Sm). */
  k_ra_i2c_speed_fast      = 400000U,  /**< 400 kHz Fast mode (Fm). */
  k_ra_i2c_speed_fast_plus = 1000000U, /**< 1 MHz Fast-mode Plus (Fm+). */
} ra_i2c_speed_t;

/**
 * @struct ra_i2c_cfg_t
 * @brief Configuration descriptor for ``ra_i2c_init``.
 *
 * @details
 * The RIIC internal reference clock is ``IICphi = PCLKB / 2^CKS`` per
 * HUM Ch 39.2.3 "ICMR1" p 2374, so the bit-rate divider needs the
 * current PCLKB frequency. cppcheck cannot see ``tests/`` so it flags
 * every field as unused; both members are read in ``ra_i2c_init``.
 *
 * @invariant ``bus_hz`` is non-zero and ``pclkb_hz`` is non-zero.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint32_t bus_hz;   /**< Target I2C clock rate in Hz. */
  uint32_t pclkb_hz; /**< Current PCLKB frequency in Hz for bit-rate calc. */
} ra_i2c_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @enum ra_i2c_err_mask_t
 * @brief Error-mask bits returned by ``ra_i2c_get_errors``.
 *
 * @details
 * Decoded from ICSR2 (HUM Ch 39.2.10 p 2384). The values double as a
 * bitmask so a single transfer can report multiple latched faults.
 */
typedef enum : uint8_t {
  k_ra_i2c_err_none     = 0x00U, /**< No latched error. */
  k_ra_i2c_err_arb_lost = 0x01U, /**< ICSR2.AL set (arbitration lost). */
  k_ra_i2c_err_nack     = 0x02U, /**< ICSR2.NACKF set (NACK received). */
  k_ra_i2c_err_timeout  = 0x04U, /**< ICSR2.TMOF set (bus timeout). */
} ra_i2c_err_mask_t;

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise an IIC channel as a controller and bring the bus up.
 *
 * @details
 * Mirrors the FSP ``r_iic_master`` open + HUM Ch 39.3.2 "Initial
 * Settings" p 2395 bring-up: ungate the channel MSTP gate, hold the
 * IIC reset (ICCR1.IICRST), program the bit rate (CKS / ICBRL / ICBRH),
 * enable the function bits in ICFER (MALE / NACKE / SCLE plus FMPE for
 * 1 MHz), then release the reset and set ICCR1.ICE.
 *
 * @param[in] channel Channel index (0, 1 or 2).
 * @param[in] cfg     Configuration descriptor (non-NULL).
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok              Channel initialized, ICCR1.ICE = 1.
 * @retval k_ra_err_null_ptr    ``cfg`` is NULL.
 * @retval k_ra_err_invalid_arg ``channel`` out of range or
 *                              ``cfg->bus_hz`` / ``cfg->pclkb_hz`` zero.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra_mstp_init`` has been called.
 * @post On success ICCR1.ICE is set and the channel is ready to service
 *       ``ra_i2c_write`` / ``ra_i2c_read``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i2c_init(uint8_t channel, const ra_i2c_cfg_t* cfg);

/**
 * @brief Tear down an IIC channel.
 *
 * @param[in] channel Channel index.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok              Channel torn down, MSTP gated.
 * @retval k_ra_err_invalid_arg ``channel`` out of range.
 *
 * @pre Caller is not in the middle of a transfer.
 * @post ICCR1.ICE cleared and the channel MSTP bit ref-released.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i2c_deinit(uint8_t channel);

/**
 * @brief Update the bus clock without tearing the channel down.
 *
 * @param[in] channel  Channel index.
 * @param[in] bus_hz   New bus clock in Hz (non-zero).
 * @param[in] pclkb_hz Current PCLKB frequency in Hz (non-zero).
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok              CKS / ICBRL / ICBRH reprogrammed.
 * @retval k_ra_err_invalid_arg Channel / clock out of range.
 *
 * @pre Channel previously initialized.
 * @post ICMR1.CKS, ICBRL and ICBRH reflect the new divider.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i2c_set_clock(uint8_t channel, uint32_t bus_hz, uint32_t pclkb_hz);

/* =============================================================================
 * Polling transfers
 * =============================================================================
 */

/**
 * @brief Polling write of ``len`` bytes to a 7-bit peripheral address.
 *
 * @details
 * Mirrors HUM Ch 39.3.3 "Master Transmit Operation" p 2396:
 *
 * 1. Reject the call if the bus is busy (ICCR2.BBSY == 1) and no
 *    repeated-START is in progress.
 * 2. Issue a START (or RESTART when the bus is already held).
 * 3. Write ``(peripheral_7b << 1) | 0`` to ICDRT (controller transmit).
 * 4. Push each payload byte into ICDRT once ICSR2.TDRE sets, bailing on
 *    NACK (ICSR2.NACKF).
 * 5. Wait for ICSR2.TEND, then either issue STOP (``send_stop``) or hold
 *    the bus for a chained read / write.
 *
 * State machine:
 * ``IDLE -> ADDR_TX -> DATA_TX -> { STOP | hold for RESTART } -> IDLE``.
 *
 * @param[in] channel       Channel index.
 * @param[in] peripheral_7b 7-bit peripheral address.
 * @param[in] data          Buffer to send (non-NULL when ``len`` > 0).
 * @param[in] len           Byte count.
 * @param[in] send_stop     When ``true``, issue STOP and release the bus;
 *                          when ``false``, hold the bus so the next call
 *                          injects a repeated-START.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok              Transfer succeeded.
 * @retval k_ra_err_null_ptr    ``data`` NULL with non-zero len or channel
 *                              invalid.
 * @retval k_ra_err_busy        Bus busy at entry.
 * @retval k_ra_err_hw_timeout  TDRE / TEND poll timed out.
 * @retval k_ra_err_nack        Peripheral NACKed; STOP was issued.
 * @retval k_ra_err_hw_error    Arbitration lost; STOP was issued.
 *
 * @pre Channel previously initialized.
 * @post On ``k_ra_ok`` and ``send_stop``: STOP issued, bus free.
 * @post On ``k_ra_ok`` and not ``send_stop``: bus held; next call must be
 *       on the same channel.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i2c_write(uint8_t        channel,
                                    uint8_t        peripheral_7b,
                                    const uint8_t* data,
                                    uint32_t       len,
                                    bool           send_stop);

/**
 * @brief Polling read of ``len`` bytes from a 7-bit peripheral.
 *
 * @details
 * Mirrors HUM Ch 39.3.4 "Master Receive Operation" p 2400:
 *
 * 1. Reject the call if the bus is busy and no repeated-START is held.
 * 2. Issue a START (or RESTART when the bus is already held).
 * 3. Write ``(peripheral_7b << 1) | 1`` to ICDRT, then drop to receive
 *    mode once the address byte is acknowledged.
 * 4. Dummy-read ICDRR to begin clocking, then drain ``len`` bytes from
 *    ICDRR; the second-to-last byte arms ICMR3.WAIT and the last byte is
 *    NACKed via ICMR3.ACKBT (paired with ACKWP).
 * 5. Issue STOP and release the bus.
 *
 * State machine:
 * ``IDLE -> ADDR_TX -> DATA_RX -> STOP -> IDLE``.
 *
 * @param[in]  channel       Channel index.
 * @param[in]  peripheral_7b 7-bit peripheral address.
 * @param[out] data          Destination buffer (non-NULL).
 * @param[in]  len           Byte count (non-zero).
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok              Transfer succeeded.
 * @retval k_ra_err_null_ptr    ``data`` NULL or channel invalid.
 * @retval k_ra_err_invalid_arg ``len`` is zero.
 * @retval k_ra_err_busy        Bus busy at entry.
 * @retval k_ra_err_hw_timeout  RDRF / TEND poll timed out.
 * @retval k_ra_err_nack        Peripheral NACKed the address byte.
 * @retval k_ra_err_hw_error    Arbitration lost.
 *
 * @pre Channel previously initialized.
 * @post STOP issued and the bus released.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_i2c_read(uint8_t channel, uint8_t peripheral_7b, uint8_t* data, uint32_t len);

/**
 * @brief Combined write-then-RESTART-then-read in one bus transaction.
 *
 * @details
 * Convenience wrapper for the common "address a register, read its
 * contents" pattern. Internally invokes ``ra_i2c_write(..., send_stop =
 * false)`` followed by ``ra_i2c_read``. If ``wr_len`` is zero the write
 * phase is skipped (degenerates to a plain read); if ``rd_len`` is zero
 * the read phase is skipped (degenerates to a plain write with STOP).
 *
 * State machine:
 * ``IDLE -> ADDR_TX -> DATA_TX -> RESTART -> ADDR_TX(read) -> DATA_RX -> STOP``.
 *
 * @param[in]  channel       Channel index.
 * @param[in]  peripheral_7b 7-bit peripheral address.
 * @param[in]  wr            Bytes to send first (NULL only when wr_len 0).
 * @param[in]  wr_len        Number of bytes to send.
 * @param[out] rd            Destination buffer (NULL only when rd_len 0).
 * @param[in]  rd_len        Number of bytes to read.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok              Transfer succeeded; STOP issued.
 * @retval k_ra_err_null_ptr    ``wr``/``rd`` NULL with non-zero len, or
 *                              channel invalid.
 * @retval k_ra_err_invalid_arg Both ``wr_len`` and ``rd_len`` are zero.
 * @retval k_ra_err_busy        Bus busy at entry.
 * @retval k_ra_err_nack        Peripheral NACKed.
 * @retval k_ra_err_hw_timeout  Poll timed out.
 *
 * @pre Channel previously initialized.
 * @post Bus is released regardless of outcome.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i2c_transfer(uint8_t        channel,
                                       uint8_t        peripheral_7b,
                                       const uint8_t* wr,
                                       uint32_t       wr_len,
                                       uint8_t*       rd,
                                       uint32_t       rd_len);

/**
 * @brief Probe whether a 7-bit peripheral address ACKs.
 *
 * @details
 * Issues START, writes the address byte, waits for the ACK / NACK
 * status (ICSR2.TEND / NACKF), and issues STOP. Equivalent to a single
 * ``i2cdetect`` sweep entry.
 *
 * @param[in]  channel       Channel index.
 * @param[in]  peripheral_7b 7-bit peripheral address.
 * @param[out] out_acked     Set to ``true`` on ACK, ``false`` on NACK.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok              Probe completed (ACK or NACK).
 * @retval k_ra_err_null_ptr    ``out_acked`` NULL or channel invalid.
 * @retval k_ra_err_busy        Bus busy at entry.
 * @retval k_ra_err_hw_timeout  Status poll timed out.
 *
 * @pre Channel previously initialized.
 * @post Bus is released.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i2c_scan(uint8_t channel, uint8_t peripheral_7b, bool* out_acked);

/* =============================================================================
 * Status
 * =============================================================================
 */

/**
 * @brief Read latched error flags from ICSR2 (AL / NACKF / TMOF).
 *
 * @param[in]  channel  Channel index.
 * @param[out] out_mask OR of ``k_ra_i2c_err_*`` bits.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok              ``out_mask`` populated.
 * @retval k_ra_err_null_ptr    ``out_mask`` is NULL.
 * @retval k_ra_err_invalid_arg ``channel`` out of range.
 *
 * @pre Channel previously initialized.
 * @post ``*out_mask`` reflects the latched ICSR2 error bits.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i2c_get_errors(uint8_t channel, uint8_t* out_mask);

/**
 * @brief Clear latched error flags in ICSR2.
 *
 * @param[in] channel Channel index.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok              AL / NACKF / TMOF W0C cleared.
 * @retval k_ra_err_invalid_arg Channel out of range.
 *
 * @pre Channel previously initialized.
 * @post ICSR2.AL, ICSR2.NACKF and ICSR2.TMOF read back zero.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i2c_clear_errors(uint8_t channel);

#ifdef __cplusplus
}
#endif
