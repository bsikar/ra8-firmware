/**
 * @file ra_kint.c
 * @brief Key-matrix interrupt (KINT) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Tiny FSP `r_kint`-shaped driver that programmes KRCTL for the
 * column-scan engine, KRM for per-row IRQ masking, and exposes
 * `ra_kint_read_matrix` to snapshot KRSTR (live row state).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_kint.h"

#include <stdint.h>

#include "ra8d2_kint_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "KINT";

typedef struct {
  bool opened; /**< True after `ra_kint_open` succeeded. */
} ra_kint_state_t;

static ra_kint_state_t s_state = {};

ra_err_t ra_kint_open(const ra_kint_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  if ((uint8_t)cfg->edge >= (uint8_t)k_ra_kint_edge_max) {
    return k_ra_err_invalid_arg;
  }

  volatile r_kint_regs_t* reg = ra_kint();

  /* Edge selector + KRE enable (KRCTL bit 7). */
  const uint8_t kreg = (uint8_t)((uint8_t)cfg->edge << k_ra_krctl_bit_kreg);
  const uint8_t kre  = (uint8_t)(1U << k_ra_krctl_bit_kre);
  reg->KRF           = 0U;
  reg->KRM           = cfg->row_mask;
  reg->KRCTL         = (uint8_t)(kreg | kre);

  s_state.opened = true;
  ra_log_info_val(s_tag, "kint open mask", (uint32_t)cfg->row_mask);
  return k_ra_ok;
}

ra_err_t ra_kint_close(void)
{
  if (!s_state.opened) {
    return k_ra_err_invalid_state;
  }
  volatile r_kint_regs_t* reg = ra_kint();
  reg->KRCTL                  = 0U;
  reg->KRM                    = 0U;
  reg->KRF                    = 0U;
  s_state.opened              = false;
  return k_ra_ok;
}

ra_err_t ra_kint_read_matrix(uint8_t* state)
{
  RA_CHECK_NULL_PTR(state, s_tag, "state must not be nullptr");
  if (!s_state.opened) {
    return k_ra_err_invalid_state;
  }
  *state = ra_kint()->KRSTR;
  return k_ra_ok;
}
