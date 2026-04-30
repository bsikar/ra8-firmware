/**
 * @file ra_dsmif.c
 * @brief Delta-Sigma Demodulator Interface (DSMIF) driver -- placeholder
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Host-testable stand-in for the FSP `r_dsmif` driver. The RA8D2
 * silicon does not include a DSMIF block, but downstream code that
 * targets RA8M motor variants needs the API surface to compile and
 * link. This translation unit tracks lifecycle / scan state in TU
 * statics, validates arguments, and zero-fills the output samples.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_dsmif.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "DSMIF";

/**
 * @struct ra_dsmif_state_t
 * @brief Lifecycle bookkeeping for the placeholder driver.
 */
typedef struct {
  bool    opened;       /**< True after a successful `_open`. */
  bool    scanning;     /**< True between `_scan_start` / `_scan_stop`. */
  uint8_t channel_mask; /**< Cached enabled-channel bitmap. */
} ra_dsmif_state_t;

static ra_dsmif_state_t s_state = {};

ra_err_t ra_dsmif_open(const ra_dsmif_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  if (cfg->channel_mask == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (s_state.opened) {
    return k_ra_err_exists;
  }
  s_state.opened       = true;
  s_state.scanning     = false;
  s_state.channel_mask = cfg->channel_mask;
  ra_log_info_val(s_tag, "dsmif open mask", (uint32_t)cfg->channel_mask);
  return k_ra_ok;
}

ra_err_t ra_dsmif_scan_start(void)
{
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  s_state.scanning = true;
  return k_ra_ok;
}

ra_err_t ra_dsmif_scan_stop(void)
{
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  s_state.scanning = false;
  return k_ra_ok;
}

ra_err_t ra_dsmif_read(ra_dsmif_channel_t ch, uint16_t* out_data)
{
  RA_CHECK_NULL_PTR(out_data, s_tag, "out_data must not be nullptr");
  if (ch >= k_ra_dsmif_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  *out_data = 0U;
  return k_ra_ok;
}

ra_err_t ra_dsmif_read32(ra_dsmif_channel_t ch, uint32_t* out_data)
{
  RA_CHECK_NULL_PTR(out_data, s_tag, "out_data must not be nullptr");
  if (ch >= k_ra_dsmif_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if (!s_state.opened) {
    return k_ra_err_not_initialized;
  }
  *out_data = 0U;
  return k_ra_ok;
}

ra_err_t ra_dsmif_status_get(uint8_t* out_open, uint8_t* out_scanning)
{
  RA_CHECK_NULL_PTR(out_open, s_tag, "out_open must not be nullptr");
  RA_CHECK_NULL_PTR(out_scanning, s_tag, "out_scanning must not be nullptr");
  *out_open     = (uint8_t)(s_state.opened ? 1U : 0U);
  *out_scanning = (uint8_t)(s_state.scanning ? 1U : 0U);
  return k_ra_ok;
}

ra_err_t ra_dsmif_close(void)
{
  if (!s_state.opened) {
    return k_ra_err_invalid_state;
  }
  s_state.opened       = false;
  s_state.scanning     = false;
  s_state.channel_mask = 0U;
  return k_ra_ok;
}
