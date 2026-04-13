/**
 * @file ra_dac_b.c
 * @brief 12-bit DAC_B driver implementation
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_dac_b.h"

#include <stdint.h>

#include "ra8d2_dac_b_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "DAC_B";

/**
 * @enum ra_dac_b_channel_t
 * @brief Named channel indices.
 */
typedef enum : uint8_t {
  k_ra_dac_b_channel_0 = 0U,
  k_ra_dac_b_channel_1 = 1U,
} ra_dac_b_channel_t;

/**
 * @brief Clamp a raw value to 12-bit range.
 *
 * @param[in] value Raw input value.
 * @return Value saturated to `[0, k_ra_dac_b_max_value]`.
 */
static inline uint16_t internal_ra_dac_b_clamp(uint16_t value)
{
  if ((uint32_t)value > (uint32_t)k_ra_dac_b_max_value) {
    return (uint16_t)k_ra_dac_b_max_value;
  }
  return value;
}

[[nodiscard]] ra_err_t ra_dac_b_init(void)
{
  /* DAC12 has two independent MSTP bits (D19/D20). The driver
   * activates both since both channel registers are programmed
   * by ra_dac_b_init.
   * HUM Ch 11.2.9 "MSTPCRD : Module Stop Control Register D", p 448 */
  ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_dac12_0);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "dac_b_init: mstp dac0");
  mst_err = ra_mstp_enable(k_ra_mstp_dac12_1);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "dac_b_init: mstp dac1");

  volatile r_dac_b_regs_t* reg = ra_dac_b();
  reg->DACR                    = 0U;
  reg->DADR0                   = 0U;
  reg->DADR1                   = 0U;
  reg->DADPR                   = 0U;
  reg->DAADSCR                 = 0U;
  reg->DAVREFCR                = 0U;
  ra_log_info(s_tag, "dac_b_init");
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_dac_b_write(uint8_t channel, uint16_t value)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_dac_b_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_dac_b_regs_t* reg     = ra_dac_b();
  const uint16_t           clamped = internal_ra_dac_b_clamp(value);

  if (channel == (uint8_t)k_ra_dac_b_channel_0) {
    reg->DADR0 = clamped;
    reg->DACR =
      (uint8_t)(reg->DACR | (uint8_t)k_ra_dac_b_mask_dae | (uint8_t)k_ra_dac_b_mask_daoe0);
  } else {
    reg->DADR1 = clamped;
    reg->DACR =
      (uint8_t)(reg->DACR | (uint8_t)k_ra_dac_b_mask_dae | (uint8_t)k_ra_dac_b_mask_daoe1);
  }
  ra_log_info_val(s_tag, "dac_b_write value", (uint32_t)clamped);
  return k_ra_ok;
}
