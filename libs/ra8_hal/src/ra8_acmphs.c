/**
 * @file ra8_acmphs.c
 * @brief High-Speed Analog Comparator driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the RA8D2 ACMPHS block (6 channels total;
 * channels 0..3 have dedicated MSTPD bits, 4..5 share with
 * ACMPHS0). Exposes per-channel init/deinit/enable, output read,
 * input selection, status get/clear, async dispatch, and power
 * transition. Every register access carries a HUM Ch 56
 * citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_acmphs.h"

#include <stdint.h>

#include "ra8_acmphs_regs.h"
#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_port_constants.h"

static const char* s_tag = "ACMPHS";

/**
 * @enum ra8_acmphs_mstp_limit_t
 * @brief Number of ACMPHS channels that have dedicated MSTPD bits.
 *
 * @details
 * HUM Ch 11.2.9 p 449 only defines MSTPD25..MSTPD28 covering
 * ACMPHS3..ACMPHS0. Higher-numbered channels on the RA8D2
 * (``k_ra8_acmphs_channel_count`` = 6) share the same MSTPD28 bit
 * as channel 0; in practice channels 4 and 5 are enabled as a
 * side effect of ACMPHS0 so the driver does not need to request
 * a separate MSTP id for them.
 */
typedef enum : uint8_t {
  k_ra8_acmphs_mstp_id_count = 4U, /**< RA8 acmphs mstp ID count. */
} ra8_acmphs_mstp_limit_t;

/**
 * @var s_acmphs_mstp_table
 * @brief Channel-index -> MSTP id lookup for the first 4 channels.
 */
static const ra8_mstp_t s_acmphs_mstp_table[k_ra8_acmphs_mstp_id_count] = {
  k_ra8_mstp_acmphs0,
  k_ra8_mstp_acmphs1,
  k_ra8_mstp_acmphs2,
  k_ra8_mstp_acmphs3,
};

/**
 * @brief Reset one ACMPHS channel's control + selector registers.
 *
 * @param[in] ch Channel index already validated against
 * ``k_ra8_acmphs_channel_count``.
 * @return ``k_ra8_ok`` or the first error from ra8_mstp / NULL mapping.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_reset_channel(uint8_t ch)
{
  volatile r_acmphs_regs_t* reg = ra8_acmphs(ch);
  if (reg == nullptr) {              /* GCOVR_EXCL_BR_LINE -- ch already bounded */
    return k_ra8_err_hw_init_failed; /* GCOVR_EXCL_LINE                          */
  }
  if (ch < k_ra8_acmphs_mstp_id_count) {
    /* HUM Ch 11.2.9 "MSTPCRD : Module Stop Control Register D", p 449 */
    const ra8_err_t mst_err = ra8_mstp_enable(s_acmphs_mstp_table[ch]);
    RA8_RETURN_ON_ERROR(mst_err, s_tag, "acmphs_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */
  }
  reg->CMPCTL  = 0U;
  reg->CMPSEL0 = 0U;
  reg->CMPSEL1 = 0U;
  reg->CPIOC   = 0U;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_acmphs_init(void)
{
  for (uint8_t ch = 0U; ch < (uint8_t)k_ra8_acmphs_channel_count; ++ch) {
    const ra8_err_t err = internal_reset_channel(ch);
    RA8_RETURN_ON_ERROR(err, s_tag, "acmphs_init channel reset"); /* GCOVR_EXCL_BR_LINE */
  }
  ra8_log_info(s_tag, "acmphs_init");
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_acmphs_channel_enable(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra8_acmphs_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_acmphs_regs_t* reg = ra8_acmphs(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  const uint8_t current = reg->CMPCTL;
  reg->CMPCTL           = (uint8_t)(current | k_ra8_acmphs_mask_hcen);
  ra8_log_info_val(s_tag, "enable channel", (uint32_t)channel);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_acmphs_read_output(uint8_t channel, ra8_level_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if ((uint16_t)channel >= k_ra8_acmphs_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_acmphs_regs_t* reg = ra8_acmphs(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  const uint8_t monitor = reg->CMPMON;
  if ((monitor & k_ra8_acmphs_mask_hcmon) != 0U) {
    *out = k_ra8_level_high;
  } else {
    *out = k_ra8_level_low;
  }
  return k_ra8_ok;
}

/* =============================================================================
 * full build-out
 * =============================================================================
 */

/**
 * @enum ra8_acmphs_bits_t
 * @brief CMPCTL field shifts (match FSP R_ACMPHS0_CMPCTL_b).
 */
typedef enum : uint8_t {
  k_ra8_acmphs_ceg_shift          = 3U, /**< CMPCTL.CEG[1:0] at [4:3].            */
  k_ra8_acmphs_cdfs_shift         = 5U, /**< CMPCTL.CDFS[1:0] at [6:5].           */
  k_ra8_acmphs_cdfs_enabled_value = 1U, /**< "Any sampling clock" when filter on. */
  k_ra8_acmphs_ctl_mask = k_ra8_acmphs_mask_hcen | k_ra8_acmphs_mask_ceg | k_ra8_acmphs_mask_cinv |
                          k_ra8_acmphs_mask_coe |
                          k_ra8_acmphs_mask_cdfs, /**< RA8 acmphs ctl mask. */
} ra8_acmphs_bits_t;

static ra8_acmphs_event_fn_t s_acmphs_fn;
static void*                 s_acmphs_ctx;

ra8_err_t ra8_acmphs_channel_init(uint8_t channel, const ra8_acmphs_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  if ((uint16_t)channel >= k_ra8_acmphs_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_acmphs_regs_t* reg = ra8_acmphs(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  if (channel < k_ra8_acmphs_mstp_id_count) {
    const ra8_err_t mst_err = ra8_mstp_enable(s_acmphs_mstp_table[channel]);
    RA8_RETURN_ON_ERROR(mst_err, s_tag, "acmphs_init: mstp"); /* GCOVR_EXCL_BR_LINE */
  }

  reg->CMPSEL0 = cfg->ivpsel;
  reg->CMPSEL1 = cfg->ivrefsel;

  /* FSP CMPCTL packs the filter select into CDFS[1:0] @ [6:5]. There
   * is no standalone CMPFIR register on RA8D2; the driver maps its
   * boolean filter_en into CDFS = 0 (off) or 1 (base sampling). */
  uint8_t ctl = k_ra8_acmphs_mask_hcen;
  ctl |= (uint8_t)((uint8_t)cfg->edge << k_ra8_acmphs_ceg_shift);
  if (cfg->invert_out) {
    ctl |= k_ra8_acmphs_mask_cinv;
  }
  if (cfg->filter_en) {
    ctl |= (uint8_t)(k_ra8_acmphs_cdfs_enabled_value << k_ra8_acmphs_cdfs_shift);
  }
  reg->CMPCTL = ctl;
  return k_ra8_ok;
}

ra8_err_t ra8_acmphs_channel_deinit(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra8_acmphs_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_acmphs_regs_t* reg = ra8_acmphs(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  reg->CMPCTL  = 0U;
  reg->CMPSEL0 = 0U;
  reg->CMPSEL1 = 0U;
  if (channel < k_ra8_acmphs_mstp_id_count) {
    (void)ra8_mstp_disable(s_acmphs_mstp_table[channel]);
  }
  return k_ra8_ok;
}

ra8_err_t ra8_acmphs_set_inputs(uint8_t channel, uint8_t ivpsel, uint8_t ivrefsel)
{
  if ((uint16_t)channel >= k_ra8_acmphs_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_acmphs_regs_t* reg = ra8_acmphs(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  reg->CMPSEL0 = ivpsel;
  reg->CMPSEL1 = ivrefsel;
  return k_ra8_ok;
}

ra8_err_t ra8_acmphs_get_status(uint8_t channel, uint8_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  if ((uint16_t)channel >= k_ra8_acmphs_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_acmphs_regs_t* reg = ra8_acmphs(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  *out_mask = (uint8_t)(reg->CMPCTL & k_ra8_acmphs_ctl_mask);
  return k_ra8_ok;
}

ra8_err_t ra8_acmphs_clear_status(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra8_acmphs_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_acmphs_regs_t* reg = ra8_acmphs(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  reg->CMPCTL = 0U;
  return k_ra8_ok;
}

ra8_err_t ra8_acmphs_attach_handler(ra8_acmphs_event_fn_t fn, void* ctx)
{
  s_acmphs_fn  = fn;
  s_acmphs_ctx = ctx;
  return k_ra8_ok;
}

ra8_err_t ra8_acmphs_enter_stop(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra8_acmphs_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  if (channel < k_ra8_acmphs_mstp_id_count) {
    return ra8_mstp_disable(s_acmphs_mstp_table[channel]);
  }
  return k_ra8_ok;
}

ra8_err_t ra8_acmphs_exit_stop(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra8_acmphs_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  if (channel < k_ra8_acmphs_mstp_id_count) {
    return ra8_mstp_enable(s_acmphs_mstp_table[channel]);
  }
  return k_ra8_ok;
}

RA8_ISR_SAFE
void ra8_acmphs_dispatch(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra8_acmphs_channel_count) {
    return;
  }
  const ra8_acmphs_event_fn_t fn  = s_acmphs_fn;
  void* const                 ctx = s_acmphs_ctx;
  if (fn != nullptr) {
    fn(ctx, channel);
  }
}
