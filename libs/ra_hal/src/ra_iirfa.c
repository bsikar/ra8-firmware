/**
 * @file ra_iirfa.c
 * @brief IIR Filter Accelerator (IIRFA) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Software-only placeholder for the FSP ``r_iirfa`` peripheral
 * driver. RA8D2 may not ship the IIRFA block; this file computes the
 * cascaded-biquad output in software so example projects targeting
 * the FSP API still produce correct filter results in unit tests.
 *
 * @warning RA8D2 silicon may not include this peripheral; verify
 *          against the BSP feature header before flashing.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_iirfa.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "IIRFA";

/**
 * @enum ra_iirfa_status_t
 * @brief Status-bit constants.
 */
typedef enum : uint8_t {
  k_ra_iirfa_status_open = 0x1U,
} ra_iirfa_status_t;

/**
 * @struct ra_iirfa_state_t
 * @brief Driver-wide placeholder state.
 */
typedef struct {
  bool              open;
  uint8_t           stage_count;
  uint8_t           shift;
  ra_iirfa_biquad_t stages[k_ra_iirfa_max_stages];
  int32_t           x1[k_ra_iirfa_max_stages];
  int32_t           x2[k_ra_iirfa_max_stages];
  int32_t           y1[k_ra_iirfa_max_stages];
  int32_t           y2[k_ra_iirfa_max_stages];
} ra_iirfa_state_t;

static ra_iirfa_state_t s_iirfa;

ra_err_t ra_iirfa_open(const ra_iirfa_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "cfg must not be nullptr");
  if ((cfg->stage_count == 0U) || ((uint16_t)cfg->stage_count > k_ra_iirfa_max_stages)) {
    return k_ra_err_invalid_arg;
  }
  s_iirfa.open        = true;
  s_iirfa.stage_count = cfg->stage_count;
  s_iirfa.shift       = cfg->shift;
  for (uint8_t i = 0U; i < k_ra_iirfa_max_stages; ++i) {
    s_iirfa.stages[i] = cfg->stages[i];
    s_iirfa.x1[i]     = 0;
    s_iirfa.x2[i]     = 0;
    s_iirfa.y1[i]     = 0;
    s_iirfa.y2[i]     = 0;
  }
  ra_log_info_val(s_tag, "open stages", (uint32_t)cfg->stage_count);
  return k_ra_ok;
}

ra_err_t ra_iirfa_close(void)
{
  s_iirfa.open = false;
  return k_ra_ok;
}

ra_err_t ra_iirfa_reset(void)
{
  for (uint8_t i = 0U; i < k_ra_iirfa_max_stages; ++i) {
    s_iirfa.x1[i] = 0;
    s_iirfa.x2[i] = 0;
    s_iirfa.y1[i] = 0;
    s_iirfa.y2[i] = 0;
  }
  return k_ra_ok;
}

ra_err_t ra_iirfa_step(int32_t in, int32_t* out_sample)
{
  RA_CHECK_NULL_PTR(out_sample, s_tag, "out_sample must not be nullptr");
  if (!s_iirfa.open) {
    return k_ra_err_invalid_state;
  }
  int32_t x = in;
  for (uint8_t i = 0U; i < s_iirfa.stage_count; ++i) {
    const ra_iirfa_biquad_t* st  = &s_iirfa.stages[i];
    int64_t                  acc = 0;
    acc += (int64_t)st->b0 * (int64_t)x;
    acc += (int64_t)st->b1 * (int64_t)s_iirfa.x1[i];
    acc += (int64_t)st->b2 * (int64_t)s_iirfa.x2[i];
    acc -= (int64_t)st->a1 * (int64_t)s_iirfa.y1[i];
    acc -= (int64_t)st->a2 * (int64_t)s_iirfa.y2[i];
    const int32_t y = (int32_t)(acc >> s_iirfa.shift);
    s_iirfa.x2[i]   = s_iirfa.x1[i];
    s_iirfa.x1[i]   = x;
    s_iirfa.y2[i]   = s_iirfa.y1[i];
    s_iirfa.y1[i]   = y;
    x               = y;
  }
  *out_sample = x;
  return k_ra_ok;
}

ra_err_t ra_iirfa_get_status(uint8_t* out_status)
{
  RA_CHECK_NULL_PTR(out_status, s_tag, "out_status must not be nullptr");
  uint8_t mask = 0U;
  if (s_iirfa.open) {
    mask |= k_ra_iirfa_status_open;
  }
  *out_status = mask;
  return k_ra_ok;
}
