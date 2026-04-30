/**
 * @file ra_can.c
 * @brief Classical CAN driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Polling driver mirroring FSP ``r_can.c``. The RA8D2 group has no
 * legacy CAN block, so the register window is a placeholder inside
 * the simulator peripheral region. The flow is:
 *
 *  1. Open: drive CTLR -> reset, wait STR.RESET_STATE, clear MIER /
 *     MKIVLR, drive CTLR -> operation, wait until both reset and halt
 *     state bits clear.
 *  2. Send: validate DLC, build the ID word (with IDE / RTR), poke
 *     MB[mb].ID / DLC / DATA[], assert MCTL[mb] = TRMREQ.
 *  3. Receive: check MCTL[mb].NEWDATA, copy out, clear NEWDATA.
 *  4. Status: snapshot STR, RECR, TECR.
 *  5. Mode transition: write CTLR.SLPM/RESET/HALT and poll STR.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_can.h"

#include <string.h>

#include "ra8d2_can_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "CAN";

/**
 * @enum ra_can_internal_t
 * @brief Internal tunables.
 */
typedef enum : uint32_t {
  k_ra_can_spin_budget = 50000U, /**< Bounded poll iterations.        */
} ra_can_internal_t;

/**
 * @brief Bounded wait for an STR bit to set or clear.
 */
static ra_err_t internal_wait_str(volatile r_can_t* reg, uint16_t mask, uint8_t expected)
{
  for (uint32_t i = 0U; i < k_ra_can_spin_budget; ++i) {
    const uint16_t value = (uint16_t)(reg->STR & mask);
    if ((expected != 0U) && (value == mask)) {
      return k_ra_ok;
    }
    if ((expected == 0U) && (value == 0U)) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

ra_err_t ra_can_open(uint8_t channel)
{
  volatile r_can_t* reg = ra_can(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "ra_can_open: channel out of range");

  /* Drive into reset mode, wait for STR.RESET_STATE. */
  reg->CTLR = k_ra_can_ctlr_reset;
  RA_RETURN_ON_ERROR(internal_wait_str(reg, k_ra_can_str_reset_state, 1U),
                     s_tag,
                     "ra_can_open: reset-state ack timeout");

  /* Disable all mailbox interrupts and mask-invalids. */
  reg->MIER   = 0U;
  reg->MKIVLR = 0U;

  /* Clear all mailbox control bytes. */
  for (uint8_t mb = 0U; mb < k_ra_can_mailbox_count; ++mb) {
    reg->MCTL[mb] = 0U;
  }

  /* Leave reset -> operation. The simulator (and silicon) clears the
   * RESET_STATE acknowledgement once CTLR drops; clear our tracking
   * mirror explicitly so the polling helper observes it. */
  reg->STR  = 0U;
  reg->CTLR = 0U;
  RA_RETURN_ON_ERROR(internal_wait_str(reg, k_ra_can_str_reset_state, 0U),
                     s_tag,
                     "ra_can_open: leave-reset timeout");
  ra_log_info_val(s_tag, "ra_can_open ch", (uint32_t)channel);
  return k_ra_ok;
}

ra_err_t ra_can_close(uint8_t channel)
{
  volatile r_can_t* reg = ra_can(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "ra_can_close: channel out of range");
  reg->CTLR = k_ra_can_ctlr_reset;
  return k_ra_ok;
}

/**
 * @brief Validate a frame descriptor before transmit.
 */
static ra_err_t internal_validate_send(uint8_t mailbox, const ra_can_frame_t* frame)
{
  if (mailbox >= k_ra_can_mailbox_count) {
    ra_log_error(s_tag, "ra_can_send: mailbox out of range");
    return k_ra_err_invalid_arg;
  }
  if (frame->dlc > k_ra_can_dlc_max) {
    ra_log_error(s_tag, "ra_can_send: dlc out of range");
    return k_ra_err_invalid_arg;
  }
  if ((frame->is_extended == 0U) && (frame->id > k_ra_can_id_std_mask)) {
    ra_log_error(s_tag, "ra_can_send: 11-bit id overflow");
    return k_ra_err_invalid_arg;
  }
  if ((frame->is_extended != 0U) && (frame->id > k_ra_can_id_ext_mask)) {
    ra_log_error(s_tag, "ra_can_send: 29-bit id overflow");
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/**
 * @brief Build the mailbox-ID word from a frame descriptor.
 */
static uint32_t internal_build_id(const ra_can_frame_t* frame)
{
  uint32_t id_word = (frame->is_extended != 0U) ? (frame->id & k_ra_can_id_ext_mask)
                                                : (frame->id & k_ra_can_id_std_mask);
  if (frame->is_extended != 0U) {
    id_word |= k_ra_can_id_ide_msk;
  }
  if (frame->is_remote != 0U) {
    id_word |= k_ra_can_id_rtr_msk;
  }
  return id_word;
}

ra_err_t ra_can_send(uint8_t channel, uint8_t mailbox, const ra_can_frame_t* frame)
{
  volatile r_can_t* reg = ra_can(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "ra_can_send: channel out of range");
  RA_CHECK_NULL_PTR(frame, s_tag, "ra_can_send: frame must not be null");
  RA_RETURN_ON_ERROR(internal_validate_send(mailbox, frame),
                     s_tag,
                     "ra_can_send: frame validation failed");

  reg->MB[mailbox].ID  = internal_build_id(frame);
  reg->MB[mailbox].DLC = (uint16_t)frame->dlc;
  for (uint8_t i = 0U; i < k_ra_can_data_bytes; ++i) {
    reg->MB[mailbox].DATA[i] = (i < frame->dlc) ? frame->data[i] : 0U;
  }
  reg->MCTL[mailbox] = k_ra_can_mctl_trmreq;
  return k_ra_ok;
}

ra_err_t ra_can_receive(uint8_t channel, uint8_t mailbox, ra_can_frame_t* out_frame)
{
  volatile r_can_t* reg = ra_can(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "ra_can_receive: channel out of range");
  RA_CHECK_NULL_PTR(out_frame, s_tag, "ra_can_receive: out_frame must not be null");
  if (mailbox >= k_ra_can_mailbox_count) {
    ra_log_error(s_tag, "ra_can_receive: mailbox out of range");
    return k_ra_err_invalid_arg;
  }
  const uint8_t mctl = reg->MCTL[mailbox];
  if ((mctl & k_ra_can_mctl_newdata) == 0U) {
    return k_ra_err_no_data;
  }

  const uint32_t id_word = reg->MB[mailbox].ID;
  out_frame->is_extended = ((id_word & k_ra_can_id_ide_msk) != 0U) ? 1U : 0U;
  out_frame->is_remote   = ((id_word & k_ra_can_id_rtr_msk) != 0U) ? 1U : 0U;
  out_frame->id          = (out_frame->is_extended != 0U) ? (id_word & k_ra_can_id_ext_mask)
                                                          : (id_word & k_ra_can_id_std_mask);
  uint8_t dlc            = (uint8_t)(reg->MB[mailbox].DLC & k_ra_can_dlc_field_mask);
  if (dlc > k_ra_can_dlc_max) {
    dlc = k_ra_can_dlc_max;
  }
  out_frame->dlc = dlc;
  for (uint8_t i = 0U; i < k_ra_can_data_bytes; ++i) {
    out_frame->data[i] = reg->MB[mailbox].DATA[i];
  }
  reg->MCTL[mailbox] = (uint8_t)(mctl & (uint8_t)~k_ra_can_mctl_newdata);
  return k_ra_ok;
}

ra_err_t ra_can_get_status(uint8_t channel, ra_can_status_t* out_status)
{
  volatile r_can_t* reg = ra_can(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "ra_can_get_status: channel out of range");
  RA_CHECK_NULL_PTR(out_status, s_tag, "ra_can_get_status: out_status null");
  const uint16_t str      = reg->STR;
  out_status->str         = str;
  out_status->tx_err_cnt  = reg->TECR;
  out_status->rx_err_cnt  = reg->RECR;
  out_status->bus_off     = ((str & k_ra_can_str_bus_off) != 0U) ? 1U : 0U;
  out_status->err_passive = ((str & k_ra_can_str_error_passive) != 0U) ? 1U : 0U;
  return k_ra_ok;
}

ra_err_t ra_can_mode_transition(uint8_t channel, ra_can_mode_t mode)
{
  volatile r_can_t* reg = ra_can(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "ra_can_mode_transition: channel out of range");

  uint16_t ctlr   = 0U;
  uint16_t expect = 0U;
  switch (mode) {
    case k_ra_can_mode_operation:
      ctlr   = 0U;
      expect = 0U;
      break;
    case k_ra_can_mode_halt:
      ctlr   = k_ra_can_ctlr_halt;
      expect = k_ra_can_str_halt_state;
      break;
    case k_ra_can_mode_reset:
      ctlr   = k_ra_can_ctlr_reset;
      expect = k_ra_can_str_reset_state;
      break;
    case k_ra_can_mode_sleep:
      ctlr   = k_ra_can_ctlr_sleep;
      expect = 0U;
      break;
    default:
      ra_log_error(s_tag, "ra_can_mode_transition: bad mode");
      return k_ra_err_invalid_arg;
  }

  reg->CTLR = ctlr;
  if (expect != 0U) {
    return internal_wait_str(reg, expect, 1U);
  }
  /* Operation/sleep: just wait for reset+halt to clear. */
  return internal_wait_str(reg, (uint16_t)(k_ra_can_str_reset_state | k_ra_can_str_halt_state), 0U);
}
