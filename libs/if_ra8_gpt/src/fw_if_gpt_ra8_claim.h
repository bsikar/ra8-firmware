/**
 * @file fw_if_gpt_ra8_claim.h
 * @brief Which port adapter owns each GPT channel. Private to if_ra8_gpt.
 * @ingroup grp_fw_timer
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * The timer and PWM adapters both sit on the same GPT channels. Each open
 * calls `ra8_gpt_init`, which takes a module-stop reference and reprograms
 * the whole channel, so a timer and a PWM output on one channel would
 * silently clobber each other. One owner per channel, recorded here, is what
 * makes the second open fail with ::k_ra8_err_busy instead.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum fw_gpt_ra8_owner_t
 * @brief Who holds a channel.
 */
typedef enum : uint8_t {
  k_fw_gpt_ra8_owner_none  = 0U, /**< Free.                       */
  k_fw_gpt_ra8_owner_timer = 1U, /**< Opened through fw_if_timer. */
  k_fw_gpt_ra8_owner_pwm   = 2U, /**< Opened through fw_if_pwm.   */
} fw_gpt_ra8_owner_t;

/**
 * @brief Take @p channel for @p owner.
 *
 * @param[in] channel Chip channel, already range-checked by a port facade.
 * @param[in] owner   Claiming adapter; not ::k_fw_gpt_ra8_owner_none.
 * @return ::k_ra8_ok, or ::k_ra8_err_busy when anyone already holds it.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fw_gpt_ra8_claim(uint8_t channel, fw_gpt_ra8_owner_t owner);

/**
 * @brief Whether @p owner holds @p channel.
 *
 * @param[in] channel Chip channel.
 * @param[in] owner   Adapter asking.
 * @return True only for the adapter that claimed it.
 * @since 0.1.0
 */
bool fw_gpt_ra8_owned_by(uint8_t channel, fw_gpt_ra8_owner_t owner);

/**
 * @brief Give @p channel back. A no-op unless @p owner holds it.
 *
 * @param[in] channel Chip channel.
 * @param[in] owner   Releasing adapter.
 * @since 0.1.0
 */
void fw_gpt_ra8_release(uint8_t channel, fw_gpt_ra8_owner_t owner);
