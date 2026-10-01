/**
 * @file ra8_board_ek_ra8d2_gpt_profile.h
 * @brief EK-RA8D2 timer and PWM profiles: how this board splits its GPT
 *        channels between the two ports, in dense board numbering.
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * The GPT timer adapter (`fw_if_timer_ra8.h`) and the PWM adapter
 * (`fw_if_pwm_ra8.h`) both answer in chip channels 0..9, and one channel can
 * be open through only one of them. This file is the board's answer to which
 * channel is which, and it makes the two sets disjoint so a board timer and a
 * board PWM output can never collide.
 *
 * PWM outputs are what the board *wires*: the GTIOCnA pins that reach the
 * Arduino header (UM Table 20, p 28), restricted to channels the adapter
 * offers. That is three:
 *   - PWM 0: Arduino D6,  P105, GTIOC1A;
 *   - PWM 1: Arduino D10, P103, GTIOC2A;
 *   - PWM 2: Arduino D11, P101, GTIOC8A.
 * D4 (GTIOC10A) is left out because channel 10's width is not established
 * in-tree; the B-pin routings (D0, D3, D5, D9, D12, D13) are left out because
 * the adapter offers pin A only.
 *
 * Timers need no pin, so they get every remaining channel the adapter offers:
 * GPT 0, 3, 4, 5, 6, 7 and 9, as timers 0..6.
 *
 * Opening a PWM output routes its pin to the GPT function and claims it with
 * the pin validator under "board.pwm.dN", so a pin some other driver holds
 * fails the open with that driver's conflict code. Closing releases the
 * claim. The Arduino header shares pins with Octo-SPI and is only connected
 * with SW4-4 ON; this profile does not flip SW4-4, bring-up does.
 *
 * Not proven on the bench: the PSEL value is the one the camera's GPT12 XCLK
 * uses (::k_ra8_psel_gpt0) and has not been scoped on these three pins, and
 * the pin is left in peripheral mode after close.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "fw_if_pwm.h"
#include "fw_if_timer.h"
#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The PWM outputs this board wires, by board index.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_board_pwm_arduino_d6  = 0U, /**< D6,  GTIOC1A. */
  k_ra8_board_pwm_arduino_d10 = 1U, /**< D10, GTIOC2A. */
  k_ra8_board_pwm_arduino_d11 = 2U, /**< D11, GTIOC8A. */
} ra8_board_pwm_index_t;

/** @brief How many PWM outputs and timers this board offers. */
typedef enum : uint8_t {
  k_ra8_board_pwm_count   = 3U, /**< Wired GTIOCnA outputs.       */
  k_ra8_board_timer_count = 7U, /**< GPT channels left to timers. */
} ra8_board_gpt_counts_t;

/**
 * @brief Chip GPT channel behind board timer @p index.
 *
 * @param[in]  index    Board timer, zero-based.
 * @param[out] out_chip Chip channel.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for a NULL @p out_chip, or
 *         ::k_ra8_err_not_found past ::k_ra8_board_timer_count.
 * @note Pure.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_timer_to_chip(uint8_t index, uint8_t* out_chip);

/**
 * @brief Chip GPT channel behind board PWM output @p index.
 *
 * @param[in]  index    Board output, zero-based.
 * @param[out] out_chip Chip channel; the output is that channel's pin A.
 * @return As ::ra8_board_timer_to_chip, against ::k_ra8_board_pwm_count.
 * @note Pure.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_pwm_to_chip(uint8_t index, uint8_t* out_chip);

/**
 * @brief Bind @p tmr to the board timer profile.
 * @param[out] tmr Handle to bind.
 * @return As ::fw_timer_bind.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_timer_profile_bind(fw_timer_t* tmr);

/**
 * @brief Bind @p pwm to the board PWM profile.
 * @param[out] pwm Handle to bind.
 * @return As ::fw_pwm_bind.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_pwm_profile_bind(fw_pwm_t* pwm);

/**
 * @brief The board's timer handle, bound on first acquisition.
 * @details Same shape and caveats as `ra8_board_clock()`: one handle for the
 *          board, never NULL, acquire it once during bring-up.
 * @return Bound handle.
 * @since 0.1.0
 */
[[nodiscard]] const fw_timer_t* ra8_board_timer(void);

/**
 * @brief The board's PWM handle, bound on first acquisition.
 * @return Bound handle, never NULL.
 * @since 0.1.0
 */
[[nodiscard]] const fw_pwm_t* ra8_board_pwm(void);

#ifdef __cplusplus
}
#endif
