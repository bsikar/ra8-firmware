/**
 * @file fw_if_gpt_ra8_claim.c
 * @brief One owner per GPT channel across the timer and PWM adapters.
 * @ingroup grp_fw_timer
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details See fw_if_gpt_ra8_claim.h for why this exists. Sized by the
 *          timer adapter's channel count, which the PWM adapter shares.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "fw_if_gpt_ra8_claim.h"

#include <stdbool.h>
#include <stdint.h>

#include "fw_if_timer_ra8.h"
#include "ra8_err.h"

/** @brief Owner of each channel the adapters expose. */
static fw_gpt_ra8_owner_t s_owner[k_fw_timer_ra8_channel_count];

ra8_err_t fw_gpt_ra8_claim(uint8_t channel, fw_gpt_ra8_owner_t owner)
{
  if ((channel >= k_fw_timer_ra8_channel_count) || (owner == k_fw_gpt_ra8_owner_none)) {
    return k_ra8_err_invalid_arg;
  }
  if (s_owner[channel] != k_fw_gpt_ra8_owner_none) {
    return k_ra8_err_busy;
  }
  s_owner[channel] = owner;
  return k_ra8_ok;
}

bool fw_gpt_ra8_owned_by(uint8_t channel, fw_gpt_ra8_owner_t owner)
{
  return (channel < k_fw_timer_ra8_channel_count) && (owner != k_fw_gpt_ra8_owner_none) &&
         (s_owner[channel] == owner);
}

void fw_gpt_ra8_release(uint8_t channel, fw_gpt_ra8_owner_t owner)
{
  if (fw_gpt_ra8_owned_by(channel, owner)) {
    s_owner[channel] = k_fw_gpt_ra8_owner_none;
  }
}
