/**
 * @file fw_if_pwm_ra8.h
 * @brief RA8 GPT chip adapter for the neutral `fw_if_pwm` port.
 * @ingroup grp_fw_pwm
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Fills the seven `fw_pwm_iface_t` ops over `ra8_gpt`. The second half of the
 * timer/PWM split in `docs/PORTS.md`, and the PWM twin of `fw_if_timer_ra8.h`.
 *
 * @par What "index" means here
 * Output N is the A pin (GTIOCnA) of chip channel N. A GPT channel has one
 * counter feeding both its A and B pins, so offering B as a second output
 * would hand out two outputs that cannot have different periods while the
 * port promises each its own. B stays unexposed until a board needs it, and
 * then it comes with its own coupling rule. A board binding maps its dense
 * header numbering onto these channels.
 *
 * @par Same ten channels as the timer adapter
 * GPT320..GPT329, for the same reason: only those widths are established
 * in-tree. A channel is held by one adapter at a time, so a PWM output and a
 * timer on the same channel cannot both be open; the second gets
 * ::k_ra8_err_busy.
 *
 * @par Period and duty
 * The period goes to GTPR as the wrap point, so one cycle is period + 1
 * counts. The Q16 duty becomes a compare value of
 * `duty * (period + 1) / K_FW_PWM_DUTY_FULL`, which makes full duty
 * period + 1: a compare value the counter never reaches, so the output never
 * falls. That needs period + 1 to fit in 32 bits, hence
 * ::k_fw_pwm_ra8_period_max is one short of the counter's range.
 *
 * While the channel is counting a new duty goes to the GTCCRC buffer and
 * lands at the next cycle end, so a cycle is never cut short. While it is
 * stopped it is also written to GTCCRA directly, so the first cycle after
 * start already has it. A period change keeps the ratio by recomputing the
 * compare value the same way.
 *
 * @par Polarity and the stopped level
 * Active-high and active-low both work (GTIOR pattern 0x9 or 0x6). The level
 * held while stopped is the inactive one for the chosen polarity, set through
 * GTIOR.OADFLT at open, which is what the port means by "stop driving".
 *
 * @par Not proven here
 * Host vectors check register values, not waveforms. Whether 0% duty leaves
 * a one-count sliver at cycle start, what the first cycle after start looks
 * like, and what the pin does once the module is stopped after close are
 * questions for the bench.
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

#include "fw_if_pwm.h"
#include "ra8_err.h"

/**
 * @enum fw_pwm_ra8_limits_t
 * @brief What this adapter reports through `get_caps`.
 */
typedef enum : uint32_t {
  k_fw_pwm_ra8_channel_count = 10U,             /**< GTIOC0A..GTIOC9A.            */
  k_fw_pwm_ra8_counter_bits  = 32U,             /**< GPT32 counter width.         */
  k_fw_pwm_ra8_period_max    = UINT32_MAX - 1U, /**< period + 1 must fit 32 bits. */
} fw_pwm_ra8_limits_t;

/**
 * @brief The ops struct this adapter fills.
 *
 * @details Static, context-free, like the timer adapter: bind with a NULL
 *          `ctx`.
 *
 * @return Borrowed ops struct, never NULL.
 * @since 0.1.0
 */
const fw_pwm_iface_t* fw_pwm_ra8_iface(void);

/**
 * @brief Bind a handle to this chip adapter.
 *
 * @param[out] pwm Caller-owned handle, contents ignored on entry.
 * @return As ::fw_pwm_bind.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fw_pwm_ra8_bind(fw_pwm_t* pwm);

#ifdef __cplusplus
}
#endif
