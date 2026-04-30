/**
 * @file ra_flash_lp.c
 * @brief Low-power flash variant (FLASH_LP) driver -- placeholder
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Host-testable stand-in for the FSP `r_flash_lp` driver. The RA8D2
 * silicon ships MRAM as code memory, so the FLASH_LP block is not
 * present. This translation unit tracks lifecycle in TU statics,
 * validates arguments, and returns `k_ra_err_not_supported` for
 * destructive operations so misuse fails loudly.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_flash_lp.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "FLASH_LP";

typedef struct {
  bool opened;
  bool busy;
} ra_flash_lp_state_t;

static ra_flash_lp_state_t s_state = {};

ra_err_t ra_flash_lp_open(const ra_flash_lp_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  if (cfg->fclk_hz == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (s_state.opened) {
    return k_ra_err_exists;
  }
  s_state.opened = true;
  s_state.busy   = false;
  ra_log_info_val(s_tag, "flash_lp open fclk_hz", cfg->fclk_hz);
  return k_ra_ok;
}

ra_err_t ra_flash_lp_write(uint32_t flash_addr, const uint8_t* src, uint32_t len)
{
  (void)flash_addr;
  RA_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  if (len == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  return k_ra_err_not_supported;
}

ra_err_t ra_flash_lp_erase(uint32_t flash_addr, uint32_t block_count)
{
  (void)flash_addr;
  if (block_count == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  return k_ra_err_not_supported;
}

ra_err_t ra_flash_lp_blank_check(uint32_t flash_addr, uint32_t len, uint8_t* out_blank)
{
  (void)flash_addr;
  RA_CHECK_NULL_PTR(out_blank, s_tag, "out_blank must not be nullptr");
  if (len == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  *out_blank = 0U;
  return k_ra_ok;
}

ra_err_t ra_flash_lp_status_get(uint8_t* out_open, uint8_t* out_busy)
{
  RA_CHECK_NULL_PTR(out_open, s_tag, "out_open must not be nullptr");
  RA_CHECK_NULL_PTR(out_busy, s_tag, "out_busy must not be nullptr");
  *out_open = (uint8_t)(s_state.opened ? 1U : 0U);
  *out_busy = (uint8_t)(s_state.busy ? 1U : 0U);
  return k_ra_ok;
}

ra_err_t ra_flash_lp_close(void)
{
  if (!s_state.opened) {
    return k_ra_err_invalid_state;
  }
  s_state.opened = false;
  s_state.busy   = false;
  return k_ra_ok;
}
