/**
 * @file ra_canfd.h
 * @brief CANFD Lite driver (bit-timing, TX, RX, error state)
 *
 * @details
 * Minimal driver surface over the RA8D2 CANFD Lite controller --
 * enough to bring a channel up at a chosen nominal bit rate, queue a
 * CAN 2.0B / CAN-FD frame into the TX message buffer, poll the RX
 * FIFO for inbound frames, and read the TX/RX error counters.
 *
 * DMA hooks, interrupt delivery, and the full acceptance-filter bank
 * are deferred until there is a real consumer that needs them.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_canfd_regs.h"
#include "ra_err.h"

/**
 * @struct ra_canfd_frame_t
 * @brief A single CAN / CAN-FD frame as seen by the driver public API.
 *
 * @details
 * `id` holds an 11-bit standard or 29-bit extended identifier (masked
 * to `k_ra_canfd_id_std_mask` / `k_ra_canfd_id_ext_mask` on send).
 * `dlc` is the raw DLC nibble (0..15) -- callers passing a length
 * longer than 8 bytes must also set `is_fd`.
 *
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra_canfd_transmit`` and
 * ``ra_canfd_receive`` in ``libs/ra_hal/src/ra_canfd.c``.
 *
 * @invariant `dlc <= 15`
 * @invariant If `is_extended == 0`, `id` fits in 11 bits.
 * @invariant If `is_brs`, the channel must have been initialised with
 *            a data bit rate > nominal bit rate.
 *
 * @note The `data` array is sized for the worst case (64-byte CAN-FD
 *       payload). Classic CAN frames only touch the first 8 bytes.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint32_t id;                              /**< Arbitration-ID field.                   */
  uint8_t  dlc;                             /**< DLC code (0..15). See CAN-FD DLC table. */
  uint8_t  is_extended;                     /**< 0 = 11-bit ID, 1 = 29-bit ID.           */
  uint8_t  is_fd;                           /**< 0 = classic CAN, 1 = CAN-FD.            */
  uint8_t  is_brs;                          /**< 1 = bit-rate switch in data phase.      */
  uint8_t  data[k_ra_canfd_data_bytes_max]; /**< Payload bytes.                          */
} ra_canfd_frame_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Take a CANFD channel out of reset into operation mode.
 *
 * @param[in] channel Channel index (0..1).
 * @return `k_ra_ok` on success, `k_ra_err_null_ptr` if `channel` out of range.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_canfd_init(uint8_t channel);

/**
 * @brief Put the CANFD channel back into reset.
 *
 * @param[in] channel Channel index (0..1).
 * @return `k_ra_ok` on success, `k_ra_err_null_ptr` if `channel` out of range.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_canfd_deinit(uint8_t channel);

/**
 * @brief Programme nominal (and optional data) bit-timing registers.
 *
 * @details
 * Uses PCLKA as the CAN source clock (HUM default). Targets a 75%
 * sample point (TSEG1=15, TSEG2=4, SJW=min(4,TSEG2)) and rejects any
 * bit rate that does not resolve to an integer prescaler in 1..256.
 *
 * @param[in] channel          Channel index (0..1).
 * @param[in] bitrate_bps      Nominal-phase bit rate in bits per second.
 * @param[in] data_bitrate_bps Data-phase bit rate (for CAN-FD). Set to
 *                             `bitrate_bps` (or 0) for classic-only.
 *
 * @return `k_ra_err_null_ptr` if `channel` out of range.
 * @return `k_ra_err_invalid_arg` if either bit rate is zero or does
 *         not resolve to a valid prescaler / timing triple.
 * @return `k_ra_ok` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_canfd_set_bitrate(uint8_t channel, uint32_t bitrate_bps, uint32_t data_bitrate_bps);

/**
 * @brief Queue a frame into the TX message buffer and trigger transmission.
 *
 * @details
 * Fire-and-forget: the routine validates every field, writes the ID /
 * DLC / FD-status / payload into the TX message-buffer registers,
 * then asserts `TMTR` in `CFDTMC`. No completion callback is raised.
 *
 * @param[in] channel Channel index (0..1).
 * @param[in] frame   Frame descriptor. Must not be NULL.
 *
 * @return `k_ra_err_null_ptr`   if `channel` out of range or `frame == NULL`.
 * @return `k_ra_err_invalid_arg` on bad DLC, oversized ID, or inconsistent flags.
 * @return `k_ra_ok` on success.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_canfd_transmit(uint8_t channel, const ra_canfd_frame_t* frame);

/**
 * @brief Poll the RX FIFO for the next available frame.
 *
 * @details
 * Non-blocking: on an empty FIFO the call returns `k_ra_err_no_data`
 * immediately. On success the frame is popped (by writing the pointer
 * control register) before returning.
 *
 * @param[in]  channel   Channel index (0..1).
 * @param[out] out_frame Destination frame. Must not be NULL.
 *
 * @return `k_ra_err_null_ptr`  if `channel` out of range or `out_frame == NULL`.
 * @return `k_ra_err_no_data`   if the RX FIFO is empty.
 * @return `k_ra_ok`            on success.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_canfd_receive(uint8_t channel, ra_canfd_frame_t* out_frame);

/**
 * @brief Read the TX and RX error counters.
 *
 * @param[in]  channel Channel index (0..1).
 * @param[out] tx_err  TEC counter value. Must not be NULL.
 * @param[out] rx_err  REC counter value. Must not be NULL.
 *
 * @return `k_ra_err_null_ptr` if any pointer is NULL or channel out of range.
 * @return `k_ra_ok`           on success.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_canfd_get_error_state(uint8_t channel, uint8_t* tx_err, uint8_t* rx_err);

/**
 * @typedef ra_canfd_event_fn_t
 * @brief CANFD event callback.
 */
typedef void (*ra_canfd_event_fn_t)(void* ctx, uint8_t channel, uint32_t status_mask);

/**
 * @brief Read the channel status register (CFDCnSTS).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_canfd_get_status(uint8_t channel, uint32_t* out_mask);

/**
 * @brief Clear error flags in CFDCnERFL.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_canfd_clear_status(uint8_t channel, uint32_t mask);

/**
 * @brief Attach a callback for CANFD events (shared across channels).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_canfd_attach_handler(ra_canfd_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch a CANFD event for a channel -- snapshot + fire callback.
 * @since 0.1.0
 */
void ra_canfd_dispatch(uint8_t channel);

/**
 * @brief Put the CANFD channel into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_canfd_enter_stop(uint8_t channel);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_canfd_exit_stop(uint8_t channel);

#ifdef __cplusplus
}
#endif
