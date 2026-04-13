/**
 * @file ra_acmphs.c
 * @brief High-Speed Analog Comparator driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Wave 4 driver for the RA8D2 ACMPHS block (6 channels total;
 * channels 0..3 have dedicated MSTPD bits, 4..5 share with
 * ACMPHS0). Exposes per-channel init/deinit/enable, output read,
 * input selection, status get/clear, async dispatch, and power
 * transition. Every register access carries a HUM Ch 56
 * citation.
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
  if (reg == nullptr) {             /* GCOVR_EXCL_BR_LINE -- ch already bounded */
    return k_ra_err_hw_init_failed; /* GCOVR_EXCL_LINE */
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

/* =============================================================================
 * Wave 4.2 -- full build-out
 * =============================================================================
 */

/**
 * @enum ra_acmphs_bits_t
 * @brief CMPCTL field shifts.
 */
typedef enum : uint8_t {
  k_ra_acmphs_ceg_shift = 1U, /**< CMPCTL.CEG[1:0].        */
  k_ra_acmphs_cinv_bit  = 3U, /**< CMPCTL.CINV.            */
  k_ra_acmphs_coe_bit   = 6U, /**< CMPCTL.COE enable out.  */
  k_ra_acmphs_cif_bit   = 0U, /**< CMPFIR.CFS[1:0] shift.  */
  k_ra_acmphs_cen_bit   = 7U, /**< CMPFIR.CFEN enable.     */
  k_ra_acmphs_ctl_mask  = (uint8_t)k_ra_acmphs_mask_hcen | (uint8_t)(3U << 1U) |
                          (uint8_t)(1U << 3U) | (uint8_t)(1U << 6U),
} ra_acmphs_bits_t;

static ra_acmphs_event_fn_t s_acmphs_fn;
static void*                s_acmphs_ctx;

ra_err_t ra_acmphs_channel_init(uint8_t channel, const ra_acmphs_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "cfg must not be nullptr");
  if ((uint16_t)channel >= (uint16_t)k_ra_acmphs_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_acmphs_regs_t* reg = ra_acmphs(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  if (channel < (uint8_t)k_ra_acmphs_mstp_id_count) {
    const ra_err_t mst_err = ra_mstp_enable(s_acmphs_mstp_table[channel]);
    RA_RETURN_ON_ERROR(mst_err, s_tag, "acmphs_init: mstp");
  }

  reg->CMPSEL0 = cfg->ivpsel;
  reg->CMPSEL1 = cfg->ivrefsel;
  if (cfg->filter_en) {
    reg->CMPFIR = (uint8_t)(1U << k_ra_acmphs_cen_bit);
  } else {
    reg->CMPFIR = 0U;
  }

  uint8_t ctl = (uint8_t)k_ra_acmphs_mask_hcen;
  ctl |= (uint8_t)((uint8_t)cfg->edge << k_ra_acmphs_ceg_shift);
  if (cfg->invert_out) {
    ctl |= (uint8_t)(1U << k_ra_acmphs_cinv_bit);
  }
  reg->CMPCTL = ctl;
  return k_ra_ok;
}

ra_err_t ra_acmphs_channel_deinit(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_acmphs_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_acmphs_regs_t* reg = ra_acmphs(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  reg->CMPCTL  = 0U;
  reg->CMPSEL0 = 0U;
  reg->CMPSEL1 = 0U;
  reg->CMPFIR  = 0U;
  if (channel < (uint8_t)k_ra_acmphs_mstp_id_count) {
    (void)ra_mstp_disable(s_acmphs_mstp_table[channel]);
  }
  return k_ra_ok;
}

ra_err_t ra_acmphs_set_inputs(uint8_t channel, uint8_t ivpsel, uint8_t ivrefsel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_acmphs_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_acmphs_regs_t* reg = ra_acmphs(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  reg->CMPSEL0 = ivpsel;
  reg->CMPSEL1 = ivrefsel;
  return k_ra_ok;
}

ra_err_t ra_acmphs_get_status(uint8_t channel, uint8_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  if ((uint16_t)channel >= (uint16_t)k_ra_acmphs_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_acmphs_regs_t* reg = ra_acmphs(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  *out_mask = (uint8_t)(reg->CMPCTL & (uint8_t)k_ra_acmphs_ctl_mask);
  return k_ra_ok;
}

ra_err_t ra_acmphs_clear_status(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_acmphs_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_acmphs_regs_t* reg = ra_acmphs(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  reg->CMPCTL = 0U;
  return k_ra_ok;
}

ra_err_t ra_acmphs_attach_handler(ra_acmphs_event_fn_t fn, void* ctx)
{
  s_acmphs_fn  = fn;
  s_acmphs_ctx = ctx;
  return k_ra_ok;
}

ra_err_t ra_acmphs_enter_stop(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_acmphs_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if (channel < (uint8_t)k_ra_acmphs_mstp_id_count) {
    return ra_mstp_disable(s_acmphs_mstp_table[channel]);
  }
  return k_ra_ok;
}

ra_err_t ra_acmphs_exit_stop(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_acmphs_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if (channel < (uint8_t)k_ra_acmphs_mstp_id_count) {
    return ra_mstp_enable(s_acmphs_mstp_table[channel]);
  }
  return k_ra_ok;
}

void ra_acmphs_dispatch(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_acmphs_channel_count) {
    return;
  }
  const ra_acmphs_event_fn_t fn  = s_acmphs_fn;
  void* const                ctx = s_acmphs_ctx;
  if (fn != nullptr) {
    fn(ctx, channel);
  }
}
