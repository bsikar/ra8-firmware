/**
 * @file ra_acmphs.c
 * @brief High-Speed Analog Comparator (ACMPHS) driver implementation
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_acmphs.h"

#include <stdint.h>

#include "ra8d2_acmphs_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"
#include "ra_port_constants.h"

static const char* s_tag = "ACMPHS";

/**
 * @enum ra_acmphs_mstp_limit_t
 * @brief Number of ACMPHS channels that have dedicated MSTPD bits.
 *
 * @details
 * HUM Ch 11.2.9 p 449 only defines MSTPD25..MSTPD28 covering
 * ACMPHS3..ACMPHS0. Higher-numbered channels on the RA8D2
 * (``k_ra_acmphs_channel_count`` = 6) share the same MSTPD28 bit
 * as channel 0; in practice channels 4 and 5 are enabled as a
 * side effect of ACMPHS0 so the driver does not need to request
 * a separate MSTP id for them.
 */
typedef enum : uint8_t {
  k_ra_acmphs_mstp_id_count = 4U,
} ra_acmphs_mstp_limit_t;

/**
 * @var s_acmphs_mstp_table
 * @brief Channel-index -> MSTP id lookup for the first 4 channels.
 */
static const ra_mstp_t s_acmphs_mstp_table[k_ra_acmphs_mstp_id_count] = {
  k_ra_mstp_acmphs0,
  k_ra_mstp_acmphs1,
  k_ra_mstp_acmphs2,
  k_ra_mstp_acmphs3,
};

/**
 * @brief Reset one ACMPHS channel's control + selector registers.
 *
 * @param[in] ch Channel index already validated against
 *               ``k_ra_acmphs_channel_count``.
 * @return ``k_ra_ok`` or the first error from ra_mstp / NULL mapping.
 */
static ra_err_t internal_reset_channel(uint8_t ch)
{
  volatile r_acmphs_regs_t* reg = ra_acmphs(ch);
  if (reg == nullptr) {
    return k_ra_err_hw_init_failed;
  }
  if (ch < (uint8_t)k_ra_acmphs_mstp_id_count) {
    /* HUM Ch 11.2.9 "MSTPCRD : Module Stop Control Register D", p 449 */
    const ra_err_t mst_err = ra_mstp_enable(s_acmphs_mstp_table[ch]);
    RA_RETURN_ON_ERROR(mst_err, s_tag, "acmphs_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */
  }
  reg->CMPCTL  = 0U;
  reg->CMPSEL0 = 0U;
  reg->CMPSEL1 = 0U;
  reg->CPIOC   = 0U;
  reg->CMPFIR  = 0U;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_acmphs_init(void)
{
  for (uint8_t ch = 0U; ch < (uint8_t)k_ra_acmphs_channel_count; ++ch) {
    const ra_err_t err = internal_reset_channel(ch);
    RA_RETURN_ON_ERROR(err, s_tag, "acmphs_init channel reset");
  }
  ra_log_info(s_tag, "acmphs_init");
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_acmphs_channel_enable(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_acmphs_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_acmphs_regs_t* reg = ra_acmphs(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  const uint8_t current = reg->CMPCTL;
  reg->CMPCTL           = (uint8_t)(current | (uint8_t)k_ra_acmphs_mask_hcen);
  ra_log_info_val(s_tag, "enable channel", (uint32_t)channel);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_acmphs_read_output(uint8_t channel, ra_level_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if ((uint16_t)channel >= (uint16_t)k_ra_acmphs_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_acmphs_regs_t* reg = ra_acmphs(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  const uint8_t monitor = reg->CMPMON;
  if ((monitor & (uint8_t)k_ra_acmphs_mask_hcmon) != 0U) {
    *out = k_ra_level_high;
  } else {
    *out = k_ra_level_low;
  }
  return k_ra_ok;
}
