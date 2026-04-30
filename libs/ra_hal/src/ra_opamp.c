/**
 * @file ra_opamp.c
 * @brief Op-amp (AMP) peripheral driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * FSP `r_opamp`-shaped lifecycle (open / close / configure). Each
 * call programmes a single AMP channel: gain nibble in AMPGAIN,
 * input-pin nibble in AMPINSEL, and the per-channel enable bit in
 * AMPC. Reference: FSP `r_opamp` driver shape and the AMP register
 * window described in `ra8d2_opamp_regs.h`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_opamp.h"

#include <stdint.h>

#include "ra8d2_opamp_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "OPAMP";

typedef struct {
  bool opened; /**< True after `ra_opamp_open` succeeded. */
} ra_opamp_state_t;

static ra_opamp_state_t s_state = {};

/**
 * @brief Compute a 16-bit nibble update preserving other channels.
 */
static uint16_t internal_set_nibble(uint16_t reg, ra_opamp_channel_t channel, uint8_t value)
{
  const uint8_t  shift = (uint8_t)((uint8_t)channel * k_ra_opamp_field_width);
  const uint16_t mask  = (uint16_t)(k_ra_opamp_field_mask << shift);
  const uint16_t bits  = (uint16_t)(((uint16_t)value & k_ra_opamp_field_mask) << shift);
  return (uint16_t)((reg & (uint16_t)~mask) | bits);
}

ra_err_t ra_opamp_open(void)
{
  volatile r_opamp_regs_t* reg = ra_opamp();

  reg->AMPMC    = 0U;
  reg->AMPC     = 0U;
  reg->AMPMS    = 0U;
  reg->AMPTRS   = 0U;
  reg->AMPGAIN  = 0U;
  reg->AMPINSEL = 0U;

  s_state.opened = true;
  ra_log_info(s_tag, "opamp open");
  return k_ra_ok;
}

ra_err_t ra_opamp_close(void)
{
  if (!s_state.opened) {
    return k_ra_err_invalid_state;
  }
  volatile r_opamp_regs_t* reg = ra_opamp();
  reg->AMPC                    = 0U;
  reg->AMPGAIN                 = 0U;
  reg->AMPINSEL                = 0U;
  s_state.opened               = false;
  return k_ra_ok;
}

ra_err_t ra_opamp_configure(const ra_opamp_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  if (!s_state.opened) {
    return k_ra_err_invalid_state;
  }
  if ((uint8_t)cfg->channel >= (uint8_t)k_ra_opamp_ch_max) {
    return k_ra_err_invalid_arg;
  }
  if ((uint8_t)cfg->gain >= (uint8_t)k_ra_opamp_gain_max) {
    return k_ra_err_invalid_arg;
  }
  if ((uint8_t)cfg->input_pin >= (uint8_t)k_ra_opamp_input_pin_max) {
    return k_ra_err_invalid_arg;
  }
  if ((uint8_t)cfg->output_pin >= (uint8_t)k_ra_opamp_output_pin_max) {
    return k_ra_err_invalid_arg;
  }

  volatile r_opamp_regs_t* reg = ra_opamp();
  reg->AMPGAIN  = internal_set_nibble(reg->AMPGAIN, cfg->channel, (uint8_t)cfg->gain);
  reg->AMPINSEL = internal_set_nibble(reg->AMPINSEL, cfg->channel, (uint8_t)cfg->input_pin);
  reg->AMPTRS   = (uint8_t)((reg->AMPTRS & (uint8_t)~(uint8_t)k_ra_opamp_field_mask) |
                            ((uint8_t)cfg->output_pin & (uint8_t)k_ra_opamp_field_mask));
  reg->AMPC     = (uint8_t)(reg->AMPC | (uint8_t)(1U << (uint8_t)cfg->channel));

  ra_log_info_val(s_tag, "opamp ch armed", (uint32_t)cfg->channel);
  return k_ra_ok;
}
