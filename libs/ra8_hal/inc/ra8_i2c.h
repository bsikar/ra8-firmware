/**
 * @file ra8_i2c.h
 * @brief I2C Bus Interface (IIC) controller driver -- polling mode
 * @ingroup grp_hal_comms
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Polling-mode controller driver for the RA8D2 RIIC peripheral
 * (channels IIC0/IIC1/IIC2 at ``0x4025_E000 + 0x0100 * n``). This is
 * the classic Renesas RIIC block described in HUM Ch 39 "I2C Bus
 * Interface (IIC)" p 2367-2470, distinct from the I3C unified IP that
 * ``ra8_i3c`` / ``ra8_i3c_i2c`` drive. The board's Grove / Pmod /
 * mikroBUS / Arduino I2C bus and the U15 configuration-switch port
 * expander live on IIC1 (P511/P512).
 *
 * The driver supports both bus roles. The controller (initiator) surface
 * mirrors FSP ``r_iic_master`` minus DTC / interrupt fast paths:
 *
 * - ``ra8_i2c_init``         configure + MSTP enable + bit-rate program
 * - ``ra8_i2c_deinit``       disable + MSTP release
 * - ``ra8_i2c_set_clock``    retune SCL without tearing down
 * - ``ra8_i2c_write``        polling write to a 7-bit peripheral, with
 *                           optional repeated-START suppression of the
 *                           trailing STOP
 * - ``ra8_i2c_read``         polling read from a 7-bit peripheral
 * - ``ra8_i2c_transfer``     combined write-then-RESTART-then-read
 * - ``ra8_i2c_scan``         probe a 7-bit address without payload
 * - ``ra8_i2c_get_errors`` / ``ra8_i2c_clear_errors``
 *
 * The target (peripheral) surface lets
 * the RA8D2 answer a remote controller once one of its own-address slots
 * matches. It mirrors FSP ``r_iic_slave`` minus the interrupt path
 * (HUM Ch 39.3.5 "Peripheral Transmit Operation" p 2405 and Ch 39.3.6
 * "Peripheral Receive Operation" p 2408):
 *
 * - ``ra8_i2c_peripheral_init``     programme an own-address slot (SARLy/SARUy)
 *                              and arm matching in ICSER, optional general
 *                              call and clock stretching (ICMR3.WAIT)
 * - ``ra8_i2c_peripheral_deinit``   disarm own-address matching
 * - ``ra8_i2c_peripheral_poll``     wait for an address match and report whether
 *                              the controller is reading or writing
 * - ``ra8_i2c_peripheral_receive``  drain a controller write into a buffer
 * - ``ra8_i2c_peripheral_transmit`` answer a controller read from a buffer
 * - ``ra8_i2c_peripheral_attach_handler`` register an address-match callback
 * - ``ra8_i2c_peripheral_dispatch`` classify the latched own-address / STOP
 *                              event and fire the attached callback -- callable
 *                              from the RIIC RXI / TXI / STPI ISR or a poll loop
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

#include "ra8_err.h"

/* =============================================================================
 * Public types
 * =============================================================================
 */

/**
 * @enum ra8_i2c_speed_t
 * @brief Supported bus speeds.
 *
 * @details
 * Per HUM Ch 39.1 Table 39.1 "IIC specifications" p 2367: Fast-mode
 * Plus is supported up to 1 Mbps.
 */
typedef enum : uint32_t {
  k_ra8_i2c_speed_standard  = 100000U,  /**< 100 kHz Standard mode (Sm). */
  k_ra8_i2c_speed_fast      = 400000U,  /**< 400 kHz Fast mode (Fm).     */
  k_ra8_i2c_speed_fast_plus = 1000000U, /**< 1 MHz Fast-mode Plus (Fm+). */
} ra8_i2c_speed_t;

/**
 * @struct ra8_i2c_cfg_t
 * @brief Configuration descriptor for ``ra8_i2c_init``.
 *
 * @details
 * The RIIC internal reference clock is ``IICphi = PCLKB / 2^CKS`` per
 * HUM Ch 39.2.3 "ICMR1" p 2374, so the bit-rate divider needs the
 * current PCLKB frequency. cppcheck cannot see ``tests/`` so it flags
 * every field as unused; both members are read in ``ra8_i2c_init``.
 *
 * @invariant ``bus_hz`` is non-zero and ``pclkb_hz`` is non-zero.
 */
typedef struct {
  uint32_t bus_hz;   /**< Target I2C clock rate in Hz.                     */
  uint32_t pclkb_hz; /**< Current PCLKB frequency in Hz for bit-rate calc. */
} ra8_i2c_cfg_t;

/**
 * @enum ra8_i2c_err_mask_t
 * @brief Error-mask bits returned by ``ra8_i2c_get_errors``.
 *
 * @details
 * Decoded from ICSR2 (HUM Ch 39.2.10 p 2384). The values double as a
 * bitmask so a single transfer can report multiple latched faults.
 */
typedef enum : uint8_t {
  k_ra8_i2c_err_none     = 0x00U, /**< No latched error.                */
  k_ra8_i2c_err_arb_lost = 0x01U, /**< ICSR2.AL set (arbitration lost). */
  k_ra8_i2c_err_nack     = 0x02U, /**< ICSR2.NACKF set (NACK received). */
  k_ra8_i2c_err_timeout  = 0x04U, /**< ICSR2.TMOF set (bus timeout).    */
} ra8_i2c_err_mask_t;

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
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              Channel initialized, ICCR1.ICE = 1.
 * @retval k_ra8_err_null_ptr    ``cfg`` is NULL.
 * @retval k_ra8_err_invalid_arg ``channel`` out of range or
 *                              ``cfg->bus_hz`` / ``cfg->pclkb_hz`` zero.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_mstp_init`` has been called.
 * @post On success ICCR1.ICE is set and the channel is ready to service
 *       ``ra8_i2c_write`` / ``ra8_i2c_read``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i2c_init(uint8_t channel, const ra8_i2c_cfg_t* cfg);

/**
 * @brief Tear down an IIC channel.
 *
 * @param[in] channel Channel index.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              Channel torn down, MSTP gated.
 * @retval k_ra8_err_invalid_arg ``channel`` out of range.
 *
 * @pre Caller is not in the middle of a transfer.
 * @post ICCR1.ICE cleared and the channel MSTP bit ref-released.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i2c_deinit(uint8_t channel);

/**
 * @brief Update the bus clock without tearing the channel down.
 *
 * @param[in] channel  Channel index.
 * @param[in] bus_hz   New bus clock in Hz (non-zero).
 * @param[in] pclkb_hz Current PCLKB frequency in Hz (non-zero).
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              CKS / ICBRL / ICBRH reprogrammed.
 * @retval k_ra8_err_invalid_arg Channel / clock out of range.
 *
 * @pre Channel previously initialized.
 * @post ICMR1.CKS, ICBRL and ICBRH reflect the new divider.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i2c_set_clock(uint8_t channel, uint32_t bus_hz, uint32_t pclkb_hz);

/* =============================================================================
 * Polling transfers
 * =============================================================================
 */

/**
 * @brief Polling write of ``len`` bytes to a 7-bit peripheral address.
 *
 * @details
 * Mirrors HUM Ch 39.3.3 "Controller Transmit Operation" p 2396:
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
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              Transfer succeeded.
 * @retval k_ra8_err_null_ptr    ``data`` NULL with non-zero len or channel
 *                              invalid.
 * @retval k_ra8_err_busy        Bus busy at entry.
 * @retval k_ra8_err_hw_timeout  TDRE / TEND poll timed out.
 * @retval k_ra8_err_nack        Peripheral NACKed; STOP was issued.
 * @retval k_ra8_err_hw_error    Arbitration lost; STOP was issued.
 *
 * @pre Channel previously initialized.
 * @post On ``k_ra8_ok`` and ``send_stop``: STOP issued, bus free.
 * @post On ``k_ra8_ok`` and not ``send_stop``: bus held; next call must be
 *       on the same channel.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i2c_write(uint8_t        channel,
                                      uint8_t        peripheral_7b,
                                      const uint8_t* data,
                                      uint32_t       len,
                                      bool           send_stop);

/**
 * @brief Polling read of ``len`` bytes from a 7-bit peripheral.
 *
 * @details
 * Mirrors HUM Ch 39.3.4 "Controller Receive Operation" p 2400:
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
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              Transfer succeeded.
 * @retval k_ra8_err_null_ptr    ``data`` NULL or channel invalid.
 * @retval k_ra8_err_invalid_arg ``len`` is zero.
 * @retval k_ra8_err_busy        Bus busy at entry.
 * @retval k_ra8_err_hw_timeout  RDRF / TEND poll timed out.
 * @retval k_ra8_err_nack        Peripheral NACKed the address byte.
 * @retval k_ra8_err_hw_error    Arbitration lost.
 *
 * @pre Channel previously initialized.
 * @post STOP issued and the bus released.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_i2c_read(uint8_t channel, uint8_t peripheral_7b, uint8_t* data, uint32_t len);

/**
 * @brief Combined write-then-RESTART-then-read in one bus transaction.
 *
 * @details
 * Convenience wrapper for the common "address a register, read its
 * contents" pattern. Internally invokes ``ra8_i2c_write(..., send_stop =
 * false)`` followed by ``ra8_i2c_read``. If ``wr_len`` is zero the write
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
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              Transfer succeeded; STOP issued.
 * @retval k_ra8_err_null_ptr    ``wr``/``rd`` NULL with non-zero len, or
 *                              channel invalid.
 * @retval k_ra8_err_invalid_arg Both ``wr_len`` and ``rd_len`` are zero.
 * @retval k_ra8_err_busy        Bus busy at entry.
 * @retval k_ra8_err_nack        Peripheral NACKed.
 * @retval k_ra8_err_hw_timeout  Poll timed out.
 *
 * @pre Channel previously initialized.
 * @post Bus is released regardless of outcome.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i2c_transfer(uint8_t        channel,
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
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              Probe completed (ACK or NACK).
 * @retval k_ra8_err_null_ptr    ``out_acked`` NULL or channel invalid.
 * @retval k_ra8_err_busy        Bus busy at entry.
 * @retval k_ra8_err_hw_timeout  Status poll timed out.
 *
 * @pre Channel previously initialized.
 * @post Bus is released.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i2c_scan(uint8_t channel, uint8_t peripheral_7b, bool* out_acked);

/* =============================================================================
 * Status
 * =============================================================================
 */

/**
 * @brief Read latched error flags from ICSR2 (AL / NACKF / TMOF).
 *
 * @param[in]  channel  Channel index.
 * @param[out] out_mask OR of ``k_ra8_i2c_err_*`` bits.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              ``out_mask`` populated.
 * @retval k_ra8_err_null_ptr    ``out_mask`` is NULL.
 * @retval k_ra8_err_invalid_arg ``channel`` out of range.
 *
 * @pre Channel previously initialized.
 * @post ``*out_mask`` reflects the latched ICSR2 error bits.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i2c_get_errors(uint8_t channel, uint8_t* out_mask);

/**
 * @brief Clear latched error flags in ICSR2.
 *
 * @param[in] channel Channel index.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              AL / NACKF / TMOF W0C cleared.
 * @retval k_ra8_err_invalid_arg Channel out of range.
 *
 * @pre Channel previously initialized.
 * @post ICSR2.AL, ICSR2.NACKF and ICSR2.TMOF read back zero.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i2c_clear_errors(uint8_t channel);

/* =============================================================================
 * Target (peripheral) role
 * =============================================================================
 */

/**
 * @enum ra8_i2c_peripheral_slot_t
 * @brief Which own-address register pair (SARLy/SARUy) holds the address.
 *
 * @details
 * The RIIC block exposes three independent own-address comparators per
 * channel. A target may answer on any one of them; this driver programmes a
 * single slot per ``ra8_i2c_peripheral_init`` call. Each addresses one of the
 * RIIC own-address comparators (the ``SARLy`` / ``SARUy`` register pairs),
 * exposed here as an inclusive "own-address slot".
 *
 * @see ra8_i2c_peripheral_cfg_t
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_i2c_peripheral_slot_0 = 0U, /**< Own-address slot 0 (SARL0/SARU0). */
  k_ra8_i2c_peripheral_slot_1 = 1U, /**< Own-address slot 1 (SARL1/SARU1). */
  k_ra8_i2c_peripheral_slot_2 = 2U, /**< Own-address slot 2 (SARL2/SARU2). */
} ra8_i2c_peripheral_slot_t;

/**
 * @enum ra8_i2c_peripheral_event_t
 * @brief Outcome of ``ra8_i2c_peripheral_poll`` / ``ra8_i2c_peripheral_dispatch``.
 *
 * @details
 * Reported from the perspective of the remote controller's R/W# bit, decoded
 * from the own-address detection flags (ICSR1) and the transmit/receive mode
 * bit (ICCR2.TRS) that the hardware latches on the ninth SCL clock. ``poll``
 * only ever returns ``none`` / ``write`` / ``read``; ``dispatch`` additionally
 * reports ``stop`` when a bus STOP ended the transaction with no fresh match.
 *
 * @see ra8_i2c_peripheral_poll
 * @see ra8_i2c_peripheral_dispatch
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_i2c_peripheral_event_none  = 0U, /**< No own-address match observed.       */
  k_ra8_i2c_peripheral_event_write = 1U, /**< Controller WRITE: call receive next. */
  k_ra8_i2c_peripheral_event_read  = 2U, /**< Controller READ: call transmit next. */
  k_ra8_i2c_peripheral_event_stop  = 3U, /**< STOP ended the transaction (idle).   */
} ra8_i2c_peripheral_event_t;

/**
 * @typedef ra8_i2c_peripheral_event_fn_t
 * @brief Address-match callback fired by ``ra8_i2c_peripheral_dispatch``.
 *
 * @details
 * Invoked once per ``ra8_i2c_peripheral_dispatch`` call when the latched RIIC
 * status decodes to a target event: a controller addressed this device for a
 * write (``k_ra8_i2c_peripheral_event_write``) or a read
 * (``k_ra8_i2c_peripheral_event_read``), or a STOP ended the transaction
 * (``k_ra8_i2c_peripheral_event_stop``). A write handler typically answers with
 * ``ra8_i2c_peripheral_receive`` and a read handler with
 * ``ra8_i2c_peripheral_transmit``. Runs in the dispatcher's context -- a RIIC
 * ISR or a poll loop -- so keep it short and non-blocking when wired to an ISR.
 *
 * @param[in] ctx   Opaque context registered with
 *                  ``ra8_i2c_peripheral_attach_handler``.
 * @param[in] event Decoded target event; never
 *                  ``k_ra8_i2c_peripheral_event_none`` (dispatch drops that).
 *
 * @note Thread safety: runs in the dispatcher's context; not re-entrant.
 * @see ra8_i2c_peripheral_attach_handler
 * @see ra8_i2c_peripheral_dispatch
 * @since 0.1.0
 */
typedef void (*ra8_i2c_peripheral_event_fn_t)(void* ctx, ra8_i2c_peripheral_event_t event);

/**
 * @struct ra8_i2c_peripheral_cfg_t
 * @brief Configuration descriptor for ``ra8_i2c_peripheral_init``.
 *
 * @details
 * Selects the 7-bit own address, the comparator slot that holds it, whether
 * to also answer the I2C general-call address, whether to arm ICMR3.WAIT-based
 * clock stretching so the servicing loop has time to handle each byte, and
 * whether to arm the ICIER target interrupts. cppcheck cannot see ``tests/``
 * so it flags the members as unused; all five are read in
 * ``ra8_i2c_peripheral_init``.
 *
 * @invariant ``own_addr_7b`` is a 7-bit value (<= 0x7F) and ``slot`` is one of
 *            the three ``k_ra8_i2c_peripheral_slot_*`` values.
 *
 * @see ra8_i2c_peripheral_init
 * @since 0.1.0
 */
typedef struct {
  uint8_t                   own_addr_7b;   /**< 7-bit own address to answer on.    */
  ra8_i2c_peripheral_slot_t slot;          /**< Own-address comparator slot.       */
  bool                      general_call;  /**< Also answer the general-call addr. */
  bool                      clock_stretch; /**< Arm ICMR3.WAIT clock stretching.   */
  bool                      irq_enable;    /**< Arm ICIER RIE / TIE / SPIE.        */
} ra8_i2c_peripheral_cfg_t;

/**
 * @brief Arm an IIC channel to answer as an I2C target (peripheral).
 *
 * @details
 * Programmes one own-address slot (``SARLy`` = ``own_addr_7b`` << 1 in 7-bit
 * format, ``SARUy`` = 0) and enables the matching ``ICSER.SARyE`` bit, plus
 * ``ICSER.GCAE`` when ``general_call`` is set, so the hardware raises
 * ``ICSR1.AASy`` / ``ICSR1.GCA`` on a matching address (HUM Ch 39.2.7 p 2380,
 * Ch 39.2.9 p 2382, Ch 39.2.13 p 2390). When ``clock_stretch`` is set,
 * ICMR3.WAIT is armed so SCL is held low between the ninth and first clocks
 * until the byte is serviced. When ``irq_enable`` is set, ICIER's RIE / TIE /
 * SPIE bits are armed so the RIIC RXI / TXI / STPI interrupts fire on an
 * own-address match and a STOP; wire those vectors to
 * ``ra8_i2c_peripheral_dispatch``. The channel stays in peripheral mode
 * (ICCR2.MST = 0) until a matching address arrives.
 *
 * @param[in] channel Channel index (0, 1 or 2).
 * @param[in] cfg     Configuration descriptor (non-NULL).
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              Own-address slot armed.
 * @retval k_ra8_err_null_ptr    ``cfg`` is NULL or ``channel`` invalid.
 * @retval k_ra8_err_invalid_arg ``own_addr_7b`` > 0x7F or ``slot`` out of range.
 *
 * @pre ``ra8_i2c_init`` has already brought the channel up (ICE = 1).
 * @pre IRQs masked or single-threaded init context.
 * @post On success the channel answers ``cfg->own_addr_7b`` and is ready for
 *       ``ra8_i2c_peripheral_poll`` / ``ra8_i2c_peripheral_dispatch``.
 * @post ``ICSER`` reflects the armed own-address / general-call enables.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_i2c_peripheral_dispatch
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i2c_peripheral_init(uint8_t                         channel,
                                                const ra8_i2c_peripheral_cfg_t* cfg);

/**
 * @brief Disarm own-address matching, returning the channel to controller use.
 *
 * @param[in] channel Channel index.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok                  ICSER cleared, WAIT released.
 * @retval k_ra8_err_invalid_arg     ``channel`` out of range.
 * @retval k_ra8_err_not_initialized Target role was not armed for this channel.
 *
 * @pre Channel previously armed with ``ra8_i2c_peripheral_init``.
 * @pre IRQs masked or single-threaded context.
 * @post ``ICSER`` reads 0 and ICMR3.WAIT is clear.
 * @post The channel answers no own address; controller-role transfers resume.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i2c_peripheral_deinit(uint8_t channel);

/**
 * @brief Wait (bounded) for a controller to address this target.
 *
 * @details
 * Spins on ICSR1 / ICSR2 until an own-address match or a STOP is observed,
 * then classifies the transaction from ICSR1 (match flags) and ICCR2.TRS:
 * ``write`` when the controller will send data (target receives), ``read``
 * when the controller will clock data out (target transmits). Returns
 * ``none`` if the bounded spin elapses without a match.
 *
 * State machine:
 * ``IDLE --match,R/W=0--> (write) --match,R/W=1--> (read)``.
 *
 * @param[in]  channel   Channel index.
 * @param[out] out_event Decoded event (non-NULL).
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok                  ``*out_event`` populated (may be ``none``).
 * @retval k_ra8_err_null_ptr        ``out_event`` NULL or channel invalid.
 * @retval k_ra8_err_not_initialized Target role not armed for this channel.
 *
 * @pre Channel armed with ``ra8_i2c_peripheral_init``.
 * @pre ``out_event`` points to writable storage.
 * @post ``*out_event`` reflects the latched ICSR1 / ICCR2 state.
 * @post No bus state is mutated (read-only poll).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i2c_peripheral_poll(uint8_t                     channel,
                                                ra8_i2c_peripheral_event_t* out_event);

/**
 * @brief Drain a controller-write transaction into ``buf`` (target receive).
 *
 * @details
 * Mirrors HUM Ch 39.3.6 "Peripheral Receive Operation" p 2408: dummy-read ICDRR to
 * discard the matched address byte, then read each data byte from ICDRR as
 * ICSR2.RDRF sets, stopping when ICSR2.STOP is detected or ``buf`` fills.
 * Clears ICSR2.STOP before returning. Clock stretching (if armed) gives the
 * loop time to read each byte before the controller clocks the next.
 *
 * State machine:
 * ``RX_ACTIVE --RDRF--> store byte ... --STOP|full--> IDLE``.
 *
 * @param[in]  channel      Channel index.
 * @param[out] buf          Destination buffer (non-NULL).
 * @param[in]  capacity     Buffer size in bytes (non-zero).
 * @param[out] out_received Number of bytes stored (non-NULL).
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok                  Bytes received (possibly zero) up to STOP.
 * @retval k_ra8_err_null_ptr        ``buf`` / ``out_received`` NULL or channel
 *                                  invalid.
 * @retval k_ra8_err_invalid_arg     ``capacity`` is zero.
 * @retval k_ra8_err_not_initialized Target role not armed.
 * @retval k_ra8_err_hw_timeout      No byte arrived within the spin budget.
 *
 * @pre Channel armed and ``ra8_i2c_peripheral_poll`` returned ``write``.
 * @pre ``buf`` has room for ``capacity`` bytes and ``out_received`` is writable.
 * @post ``*out_received`` <= ``capacity`` and ICSR2.STOP is cleared.
 * @post Bytes ``[0, *out_received)`` of ``buf`` hold the received payload.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_i2c_peripheral_receive(uint8_t   channel,
                                                   uint8_t*  buf,
                                                   uint32_t  capacity,
                                                   uint32_t* out_received);

/**
 * @brief Answer a controller-read transaction from ``data`` (target transmit).
 *
 * @details
 * Mirrors HUM Ch 39.3.5 "Peripheral Transmit Operation" p 2405: push each byte to
 * ICDRT as ICSR2.TDRE sets, stopping when the controller NACKs the last byte
 * it wants (ICSR2.NACKF) or the buffer is exhausted, then waits for TEND /
 * NACKF, dummy-reads ICDRR to release SCL, and clears NACKF / STOP.
 *
 * State machine:
 * ``TX_ACTIVE --TDRE--> push byte ... --NACK|sent==len--> IDLE``.
 *
 * @param[in]  channel  Channel index.
 * @param[in]  data     Source buffer (non-NULL).
 * @param[in]  len      Bytes available to send (non-zero).
 * @param[out] out_sent Number of bytes the controller accepted (non-NULL).
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok                  Transmit completed (controller NACK is a
 *                                  normal end-of-read, not an error).
 * @retval k_ra8_err_null_ptr        ``data`` / ``out_sent`` NULL or channel
 *                                  invalid.
 * @retval k_ra8_err_invalid_arg     ``len`` is zero.
 * @retval k_ra8_err_not_initialized Target role not armed.
 * @retval k_ra8_err_hw_timeout      TDRE never armed before any byte was sent.
 *
 * @pre Channel armed and ``ra8_i2c_peripheral_poll`` returned ``read``.
 * @pre ``data`` holds ``len`` bytes and ``out_sent`` is writable.
 * @post ``*out_sent`` <= ``len`` and ICSR2.NACKF / STOP are cleared.
 * @post SCL is released (ICDRR dummy-read) so the bus is free for the next
 *       transaction.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_i2c_peripheral_transmit(uint8_t channel, const uint8_t* data, uint32_t len, uint32_t* out_sent);

/**
 * @brief Attach (or detach) the address-match callback for a channel.
 *
 * @details
 * Stores ``fn`` and ``ctx`` in the channel's driver state so a later
 * ``ra8_i2c_peripheral_dispatch`` can fire the callback when a controller
 * addresses this target or a STOP ends a transaction. Pass ``fn == NULL`` to
 * detach; dispatch then becomes a no-op for the channel. The handler is
 * independent of ``ra8_i2c_peripheral_poll``, which never consults it.
 *
 * @param[in] channel Channel index (0, 1 or 2).
 * @param[in] fn      Callback to fire on a decoded event, or NULL to detach.
 * @param[in] ctx     Opaque context passed back to ``fn`` (may be NULL).
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              Handler stored for the channel.
 * @retval k_ra8_err_invalid_arg ``channel`` out of range.
 *
 * @pre IRQs masked or single-threaded with respect to dispatch.
 * @pre The channel index is one this driver owns.
 * @post The channel's handler equals ``fn`` and its context equals ``ctx``.
 * @post No hardware register is touched.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_i2c_peripheral_dispatch
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_i2c_peripheral_attach_handler(uint8_t channel, ra8_i2c_peripheral_event_fn_t fn, void* ctx);

/**
 * @brief Snapshot the latched target status and fire the attached callback.
 *
 * @details
 * Reads ICSR1 (own-address match flags), ICSR2 (STOP) and ICCR2 (TRS) once,
 * classifies them into an ``ra8_i2c_peripheral_event_t`` (an own-address match
 * decodes to ``write`` / ``read`` from TRS; otherwise a latched STOP decodes to
 * ``stop``), and -- when the event is not ``none`` and a handler is attached --
 * invokes the handler with the registered context. Read-only with respect to
 * the bus: it clears no flags and queues no data, leaving the data-phase flag
 * handling to ``ra8_i2c_peripheral_receive`` / ``ra8_i2c_peripheral_transmit`` the
 * handler calls. Safe to call from the RIIC RXI / TXI / STPI ISR or a poll
 * loop; a no-op when the channel is invalid, not armed, or has no handler.
 *
 * @param[in] channel Channel index (0, 1 or 2).
 *
 * @pre The channel was armed with ``ra8_i2c_peripheral_init``.
 * @pre A handler may or may not be attached.
 * @post At most one handler invocation occurs per call.
 * @post No RIIC register is written (read-only snapshot).
 *
 * @note Thread safety: runs in the caller's context (ISR or poll); not
 *       re-entrant on one channel.
 * @see ra8_i2c_peripheral_attach_handler
 * @since 0.1.0
 */
void ra8_i2c_peripheral_dispatch(uint8_t channel);

#ifdef __cplusplus
}
#endif
