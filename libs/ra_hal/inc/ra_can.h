/**
 * @file ra_can.h
 * @brief Classical CAN (CAN 2.0B) driver -- mirrors FSP ``r_can``
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Polling-mode driver over the classical CAN block. The RA8D2 group
 * does not actually carry the legacy CAN IP (CAN-FD covers the same
 * role and lives behind ``ra_canfd``); this driver provides the
 * matching public surface so applications that previously targeted
 * RX or RA4/RA6 silicon keep their compile shape and so the FSP
 * reference flow stays exercised in tests.
 *
 * Public surface (mirrors FSP ``R_CAN_*``):
 *   - ``ra_can_open``  / ``ra_can_close``
 *   - ``ra_can_send`` (mailbox transmit)
 *   - ``ra_can_receive`` (mailbox receive poll)
 *   - ``ra_can_get_status`` (STR register snapshot + error counters)
 *   - ``ra_can_mode_transition`` (operation / halt / reset / sleep)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_can_regs.h"
#include "ra_err.h"

/**
 * @enum ra_can_mode_t
 * @brief Channel operating mode (matches FSP ``can_operation_mode_t``).
 */
typedef enum : uint8_t {
  k_ra_can_mode_operation = 0U, /**< Normal operation.   */
  k_ra_can_mode_halt      = 1U, /**< Halt request.       */
  k_ra_can_mode_reset     = 2U, /**< Reset request.      */
  k_ra_can_mode_sleep     = 3U, /**< Sleep request.      */
} ra_can_mode_t;

/**
 * @struct ra_can_frame_t
 * @brief One classical-CAN frame.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused; each
 * member is read in ``ra_can_send`` and ``ra_can_receive`` in
 * ``libs/ra_hal/src/ra_can.c``.
 *
 * @invariant ``dlc <= k_ra_can_dlc_max``.
 * @invariant If ``is_extended == 0``, ``id`` fits in 11 bits.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint32_t id;                        /**< Arbitration ID.        */
  uint8_t  dlc;                       /**< 0..8 byte count.       */
  uint8_t  is_extended;               /**< 0 = 11-bit, 1 = 29-bit.*/
  uint8_t  is_remote;                 /**< Remote-frame request.  */
  uint8_t  data[k_ra_can_data_bytes]; /**< Payload bytes.         */
} ra_can_frame_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_can_status_t
 * @brief Snapshot returned by ``ra_can_get_status``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint16_t str;         /**< Raw STR snapshot.                 */
  uint8_t  tx_err_cnt;  /**< TECR counter.                     */
  uint8_t  rx_err_cnt;  /**< RECR counter.                     */
  uint8_t  bus_off;     /**< Non-zero when BUSOFF latched.     */
  uint8_t  err_passive; /**< Non-zero when error-passive.      */
} ra_can_status_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Open a classical-CAN channel.
 *
 * @param[in] channel Channel index (must be 0).
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok               Channel opened, CTLR=operation.
 * @retval k_ra_err_invalid_arg  Channel out of range.
 * @retval k_ra_err_hw_timeout   Reset/halt mode-ack never arrived.
 *
 * @pre Caller is single-threaded init context.
 * @post On ``k_ra_ok`` the channel is in operation mode.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_can_open(uint8_t channel);

/**
 * @brief Close the CAN channel and put it back into reset mode.
 *
 * @param[in] channel Channel index (must be 0).
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok              Channel closed.
 * @retval k_ra_err_invalid_arg Channel out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_can_close(uint8_t channel);

/**
 * @brief Queue a frame into one of the channel's mailboxes and trigger TX.
 *
 * @param[in] channel  Channel index.
 * @param[in] mailbox  Mailbox index (0..31).
 * @param[in] frame    Frame to send (non-NULL).
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok               Frame queued, TRMREQ set.
 * @retval k_ra_err_null_ptr     ``frame`` is NULL.
 * @retval k_ra_err_invalid_arg  channel/mailbox/dlc/id out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_can_send(uint8_t channel, uint8_t mailbox, const ra_can_frame_t* frame);

/**
 * @brief Poll a mailbox for a received frame.
 *
 * @param[in]  channel   Channel index.
 * @param[in]  mailbox   Mailbox index (0..31).
 * @param[out] out_frame Destination frame (non-NULL).
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok               Frame copied out and NEWDATA cleared.
 * @retval k_ra_err_null_ptr     ``out_frame`` is NULL.
 * @retval k_ra_err_invalid_arg  channel/mailbox out of range.
 * @retval k_ra_err_no_data      No new data in the mailbox.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_can_receive(uint8_t channel, uint8_t mailbox, ra_can_frame_t* out_frame);

/**
 * @brief Read STR + error counters into ``out_status``.
 *
 * @param[in]  channel    Channel index.
 * @param[out] out_status Destination snapshot (non-NULL).
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok               Snapshot populated.
 * @retval k_ra_err_null_ptr     ``out_status`` is NULL.
 * @retval k_ra_err_invalid_arg  Channel out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_can_get_status(uint8_t channel, ra_can_status_t* out_status);

/**
 * @brief Drive a mode transition (operation / halt / reset / sleep).
 *
 * @param[in] channel Channel index.
 * @param[in] mode    Desired mode.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok               Mode latched.
 * @retval k_ra_err_invalid_arg  Channel/mode out of range.
 * @retval k_ra_err_hw_timeout   Mode-ack never arrived in STR.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_can_mode_transition(uint8_t channel, ra_can_mode_t mode);

#ifdef __cplusplus
}
#endif
