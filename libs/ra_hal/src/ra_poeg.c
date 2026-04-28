/**
 * @file ra_poeg.c
 * @brief Port Output Enable for GPT (POEG) driver implementation
 *
 * @details
 * new driver. See ``libs/ra_hal/inc/ra_poeg.h`` for the
 * public surface and HUM Ch 21 "Port Output Enable for GPT
 * (POEG)" (p 871..877) for register semantics.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_poeg.h"

#include <stdint.h>

#include "ra8d2_mstp_regs.h"
#include "ra8d2_poeg_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "POEG";

/**
 * @var s_poeg_mstp_table
 * @brief Per-group MSTP id lookup. Group 0 == POEGA, ... 3 == POEGD.
 */
static const ra_mstp_t s_poeg_mstp_table[k_ra_poeg_group_count] = {
  k_ra_mstp_poeg_a,
  k_ra_mstp_poeg_b,
  k_ra_mstp_poeg_c,
  k_ra_mstp_poeg_d,
};

/**
 * @enum ra_poeg_bits_t
 * @brief POEGG status/control bit positions and combined masks.
 */
typedef enum : uint32_t {
  k_ra_poeg_status_all = (uint32_t)k_ra_poeg_status_pidf | (uint32_t)k_ra_poeg_status_iocf |
                         (uint32_t)k_ra_poeg_status_ovrf | (uint32_t)k_ra_poeg_status_ssf |
                         (uint32_t)k_ra_poeg_status_st,
} ra_poeg_bits_t;

/**
 * @struct ra_poeg_state_t
 * @brief Per-group runtime state.
 */
typedef struct {
  ra_poeg_event_fn_t fn;
  void*              ctx;
} ra_poeg_state_t;

static ra_poeg_state_t s_poeg_state[k_ra_poeg_group_count];

static uint32_t internal_cfg_to_poegg(const ra_poeg_cfg_t* cfg)
{
  uint32_t v = 0U;
  if (cfg->enable_pin) {
    v |= (uint32_t)k_ra_poeg_en_pide;
  }
  if (cfg->enable_ioc) {
    v |= (uint32_t)k_ra_poeg_en_iocen;
  }
  if (cfg->enable_osc_stop) {
    v |= (uint32_t)k_ra_poeg_en_osten;
  }
  if (cfg->invert_input) {
    v |= (uint32_t)k_ra_poeg_en_inv;
  }
  return v;
}

ra_err_t ra_poeg_init(uint8_t group, const ra_poeg_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "cfg must not be nullptr");
  volatile r_poeg_regs_t* reg = ra_poeg(group);
  RA_CHECK_NULL_PTR(reg, s_tag, "group out of range");

  /* HUM Ch 11.2.9 "MSTPCRD : Module Stop Control Register D", p 448 */
  const ra_err_t mst_err = ra_mstp_enable(s_poeg_mstp_table[group]);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "poeg_init: mstp enable");

  /* HUM Ch 21.2.1 "POEGG : POEG Group n Setting Register", p 872 */
  reg->POEGG = internal_cfg_to_poegg(cfg);
  ra_log_info_val(s_tag, "init group", (uint32_t)group);
  return k_ra_ok;
}

ra_err_t ra_poeg_deinit(uint8_t group)
{
  volatile r_poeg_regs_t* reg = ra_poeg(group);
  RA_CHECK_NULL_PTR(reg, s_tag, "group out of range");

  reg->POEGG              = 0U;
  s_poeg_state[group].fn  = nullptr;
  s_poeg_state[group].ctx = nullptr;
  (void)ra_mstp_disable(s_poeg_mstp_table[group]);
  return k_ra_ok;
}

ra_err_t ra_poeg_trigger_stop(uint8_t group)
{
  volatile r_poeg_regs_t* reg = ra_poeg(group);
  RA_CHECK_NULL_PTR(reg, s_tag, "group out of range");

  reg->POEGG |= (uint32_t)k_ra_poeg_status_ssf;
  return k_ra_ok;
}

ra_err_t ra_poeg_get_status(uint8_t group, uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  volatile r_poeg_regs_t* reg = ra_poeg(group);
  RA_CHECK_NULL_PTR(reg, s_tag, "group out of range");

  *out_mask = reg->POEGG & (uint32_t)k_ra_poeg_status_all;
  return k_ra_ok;
}

ra_err_t ra_poeg_clear_status(uint8_t group, uint32_t mask)
{
  volatile r_poeg_regs_t* reg = ra_poeg(group);
  RA_CHECK_NULL_PTR(reg, s_tag, "group out of range");

  const uint32_t current = reg->POEGG;
  reg->POEGG             = current & ~(mask & (uint32_t)k_ra_poeg_status_all);
  return k_ra_ok;
}

ra_err_t ra_poeg_attach_handler(uint8_t group, ra_poeg_event_fn_t fn, void* ctx)
{
  if (group >= (uint8_t)k_ra_poeg_group_count) {
    return k_ra_err_invalid_arg;
  }
  s_poeg_state[group].fn  = fn;
  s_poeg_state[group].ctx = ctx;
  return k_ra_ok;
}

ra_err_t ra_poeg_enter_stop(uint8_t group)
{
  if (group >= (uint8_t)k_ra_poeg_group_count) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_disable(s_poeg_mstp_table[group]);
}

ra_err_t ra_poeg_exit_stop(uint8_t group)
{
  if (group >= (uint8_t)k_ra_poeg_group_count) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_enable(s_poeg_mstp_table[group]);
}

void ra_poeg_dispatch(uint8_t group)
{
  if (group >= (uint8_t)k_ra_poeg_group_count) {
    return;
  }
  volatile r_poeg_regs_t* reg  = ra_poeg(group);
  uint32_t                mask = 0U;
  if (reg != nullptr) {
    mask = reg->POEGG & (uint32_t)k_ra_poeg_status_all;
  }
  const ra_poeg_event_fn_t fn  = s_poeg_state[group].fn;
  void* const              ctx = s_poeg_state[group].ctx;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}
