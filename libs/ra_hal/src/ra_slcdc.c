/**
 * @file ra_slcdc.c
 * @brief Segment LCD Controller (SLCDC) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Software-only placeholder for the FSP ``r_slcdc`` peripheral
 * driver. RA8D2 has GLCDC instead; this file exposes the FSP-shaped
 * surface so example projects can build cleanly.
 *
 * @warning RA8D2 silicon may not include this peripheral; verify
 *          against the BSP feature header before flashing.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_slcdc.h"

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "SLCDC";

/**
 * @enum ra_slcdc_status_t
 * @brief Status-bit constants.
 */
typedef enum : uint8_t {
  k_ra_slcdc_status_open = 0x1U,
  k_ra_slcdc_status_on   = 0x2U,
} ra_slcdc_status_t;

/**
 * @struct ra_slcdc_state_t
 * @brief Driver-wide placeholder state.
 */
typedef struct {
  bool                      open;
  bool                      display_on;
  ra_slcdc_bias_t           bias;
  ra_slcdc_drive_volt_gen_t drive_volt;
  uint8_t                   time_slice;
  uint16_t                  frame_rate;
  uint8_t                   contrast;
  uint8_t                   buffer[k_ra_slcdc_segment_bytes];
} ra_slcdc_state_t;

static ra_slcdc_state_t s_slcdc;

ra_err_t ra_slcdc_open(const ra_slcdc_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "cfg must not be nullptr");
  if (cfg->time_slice == 0U) {
    return k_ra_err_invalid_arg;
  }
  s_slcdc.open       = true;
  s_slcdc.display_on = false;
  s_slcdc.bias       = cfg->bias;
  s_slcdc.drive_volt = cfg->drive_volt;
  s_slcdc.time_slice = cfg->time_slice;
  s_slcdc.frame_rate = cfg->frame_rate;
  s_slcdc.contrast   = cfg->contrast;
  for (uint8_t i = 0U; i < k_ra_slcdc_segment_bytes; ++i) {
    s_slcdc.buffer[i] = 0U;
  }
  ra_log_info_val(s_tag, "open ts", (uint32_t)cfg->time_slice);
  return k_ra_ok;
}

ra_err_t ra_slcdc_close(void)
{
  s_slcdc.open       = false;
  s_slcdc.display_on = false;
  return k_ra_ok;
}

ra_err_t ra_slcdc_on(void)
{
  if (!s_slcdc.open) {
    return k_ra_err_invalid_state;
  }
  s_slcdc.display_on = true;
  return k_ra_ok;
}

ra_err_t ra_slcdc_off(void)
{
  s_slcdc.display_on = false;
  return k_ra_ok;
}

ra_err_t ra_slcdc_write(uint8_t index, uint8_t value)
{
  if ((uint16_t)index >= k_ra_slcdc_segment_bytes) {
    return k_ra_err_invalid_arg;
  }
  s_slcdc.buffer[index] = value;
  return k_ra_ok;
}

ra_err_t ra_slcdc_get_status(uint8_t* out_status)
{
  RA_CHECK_NULL_PTR(out_status, s_tag, "out_status must not be nullptr");
  uint8_t mask = 0U;
  if (s_slcdc.open) {
    mask |= k_ra_slcdc_status_open;
  }
  if (s_slcdc.display_on) {
    mask |= k_ra_slcdc_status_on;
  }
  *out_status = mask;
  return k_ra_ok;
}
